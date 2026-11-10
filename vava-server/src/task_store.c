#include "task_store.h"
#include <sqlite3.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct task_store { sqlite3 *db; unsigned limit; };
#define COLUMNS "id,epoch,device,boot,request_key,command,state,reason,expires,created_at,total,received"
static bool sql(struct task_store *s, const char *text) {
    return sqlite3_exec(s->db,text,NULL,NULL,NULL)==SQLITE_OK;
}
static sqlite3_stmt *prepare(struct task_store *s, const char *text) {
    sqlite3_stmt *q=NULL;
    if (sqlite3_prepare_v2(s->db,text,-1,&q,NULL)!=SQLITE_OK) { sqlite3_finalize(q); return NULL; }
    return q;
}
static bool finish(sqlite3_stmt *q) {
    bool ok=q && sqlite3_step(q)==SQLITE_DONE;
    return sqlite3_finalize(q)==SQLITE_OK && ok;
}
static void blob(sqlite3_stmt *q, int column, const void *p, int n) {
    sqlite3_bind_blob(q,column,p,n,SQLITE_TRANSIENT);
}
static void string(sqlite3_stmt *q, int column, const char *p) {
    sqlite3_bind_text(q,column,p,-1,SQLITE_TRANSIENT);
}
static bool scalar(struct task_store *s, const char *text, uint64_t *out) {
    sqlite3_stmt *q=prepare(s,text);
    bool ok=q && sqlite3_step(q)==SQLITE_ROW && sqlite3_column_int64(q,0)>=0;
    if (ok) *out=(uint64_t)sqlite3_column_int64(q,0);
    return sqlite3_finalize(q)==SQLITE_OK && ok;
}
static bool private_file(const char *path, bool allow_missing) {
    struct stat st;
    if (lstat(path,&st)) return allow_missing && errno==ENOENT;
    return S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&0077) && st.st_nlink==1;
}
struct task_store *task_store_open(const char *directory, unsigned limit) {
    char path[1024], side[1032];
    char *canonical=realpath(directory,NULL);
    if (!canonical) return NULL;
    int length=snprintf(path,sizeof(path),"%s/tasks.sqlite3",canonical);
    free(canonical);
    if (length<0 || length>=(int)sizeof(path)) return NULL;
    bool created=false;
    int fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd>=0) { created=true; close(fd); }
    else if (errno!=EEXIST) return NULL;
    if (!private_file(path,false)) return NULL;
    const char *suffixes[]={"-wal","-shm","-journal"};
    for (unsigned i=0;i<3;++i) {
        snprintf(side,sizeof(side),"%s%s",path,suffixes[i]);
        if (!private_file(side,true)) return NULL;
    }
    struct task_store *s=calloc(1,sizeof(*s));
    if (!s) return NULL;
    s->limit=limit;
    if (sqlite3_open_v2(path,&s->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOFOLLOW|SQLITE_OPEN_NOMUTEX,NULL)!=SQLITE_OK) goto failed;
    sqlite3_busy_timeout(s->db,100);
    uint64_t app=0,version=0;
    if (!scalar(s,"PRAGMA application_id",&app) || !scalar(s,"PRAGMA user_version",&version)) goto failed;
    if (!created && (app!=1297503281 || (version!=2 && version!=3 && version!=4))) goto failed;
    if (!sql(s,"PRAGMA trusted_schema=OFF; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;"
               "PRAGMA fullfsync=ON; PRAGMA checkpoint_fullfsync=ON;"
               "PRAGMA cache_size=-2048; PRAGMA wal_autocheckpoint=64; PRAGMA journal_size_limit=4194304;"
               "PRAGMA max_page_count=131072;")) goto failed;
    if (created && !sql(s,
        "BEGIN IMMEDIATE;"
        "CREATE TABLE tasks (id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "epoch BLOB NOT NULL CHECK(length(epoch)=16),device TEXT NOT NULL,boot BLOB NOT NULL CHECK(length(boot)=16),"
        "request_key TEXT NOT NULL CHECK(length(request_key)=32),command TEXT NOT NULL CHECK(length(command) BETWEEN 1 AND 511),"
        "state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),reason TEXT NOT NULL DEFAULT '',"
        "expires INTEGER NOT NULL,created_at INTEGER NOT NULL,"
        "total INTEGER NOT NULL DEFAULT 0 CHECK(total BETWEEN 0 AND 32767),"
        "received INTEGER NOT NULL DEFAULT 0 CHECK(received BETWEEN 0 AND total),"
        "reclaimable INTEGER NOT NULL DEFAULT 0 CHECK(reclaimable IN (0,1)),"
        "result BLOB NOT NULL DEFAULT X'' CHECK(length(result)=received),"
        "UNIQUE(device,request_key));"
        "CREATE INDEX pending ON tasks(epoch,device,state);"
        "CREATE TABLE stats (id INTEGER PRIMARY KEY CHECK(id=1),pruned INTEGER NOT NULL);"
        "INSERT INTO stats VALUES(1,0); PRAGMA application_id=1297503281; PRAGMA user_version=2; COMMIT;")) goto failed;
    if ((created || version==2) && !sql(s,
        "BEGIN IMMEDIATE;"
        "CREATE TABLE events (id INTEGER PRIMARY KEY AUTOINCREMENT,device TEXT NOT NULL,"
        "origin BLOB NOT NULL CHECK(length(origin)=16),sequence INTEGER NOT NULL CHECK(sequence>0),"
        "received_at INTEGER NOT NULL,gap INTEGER NOT NULL CHECK(gap>=0),"
        "payload BLOB NOT NULL CHECK(length(payload)=160),UNIQUE(device,origin,sequence));"
        "CREATE INDEX event_device ON events(device,id);"
        "CREATE TABLE event_streams (device TEXT NOT NULL,origin BLOB NOT NULL CHECK(length(origin)=16),"
        "sequence INTEGER NOT NULL CHECK(sequence>0),payload BLOB NOT NULL CHECK(length(payload)=160),"
        "PRIMARY KEY(device,origin));"
        "CREATE TABLE event_stats (id INTEGER PRIMARY KEY CHECK(id=1),pruned INTEGER NOT NULL);"
        "INSERT INTO event_stats VALUES(1,0); PRAGMA user_version=3; COMMIT;")) goto failed;
    if((created||version<4)&&!sql(s,
        "BEGIN IMMEDIATE;"
        "ALTER TABLE events ADD COLUMN category TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE events ADD COLUMN serial TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE events ADD COLUMN clip TEXT NOT NULL DEFAULT '';"
        "UPDATE events SET category=CASE hex(substr(payload,78,1)) WHEN '03' THEN 'detection' WHEN '04' THEN 'detection' WHEN '02' THEN 'pairing' WHEN '07' THEN 'storage' WHEN '01' THEN 'system' ELSE 'recording' END,"
        "serial=rtrim(CAST(substr(payload,81,32) AS TEXT),char(0)),clip=lower(hex(substr(payload,53,8)));"
        "CREATE INDEX event_filter ON events(device,category,serial,id);"
        "CREATE INDEX event_clip ON events(device,serial,clip,id);"
        "CREATE TABLE clip_states(device TEXT NOT NULL,serial TEXT NOT NULL,clip TEXT NOT NULL,state INTEGER NOT NULL,PRIMARY KEY(device,serial,clip));"
        "INSERT INTO clip_states SELECT device,serial,clip,max(CASE hex(substr(payload,78,1)) WHEN '05' THEN 1 WHEN '06' THEN 2 WHEN '09' THEN 3 WHEN '08' THEN 4 WHEN '0A' THEN 4 ELSE 0 END) FROM events WHERE clip!='0000000000000000' GROUP BY device,serial,clip;"
        "PRAGMA user_version=4; COMMIT;"))goto failed;
    sqlite3_stmt *q=prepare(s,"PRAGMA quick_check");
    bool valid=q && sqlite3_step(q)==SQLITE_ROW && !strcmp((const char *)sqlite3_column_text(q,0),"ok");
    sqlite3_finalize(q);
    if (!valid || !sql(s,
        "BEGIN IMMEDIATE;"
        "UPDATE tasks SET state=3,reason='server_restarted_before_dispatch' WHERE state=0;"
        "UPDATE tasks SET state=4,reason='server_restarted_after_dispatch' WHERE state=1; COMMIT;")) goto failed;
    /* Persist creation of the database name, in addition to SQLite's WAL commits. */
    fd=open(directory,O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if (fd<0) goto failed;
    int synced=fsync(fd); close(fd);
    if (synced) goto failed;
    return s;
failed:
    fprintf(stderr,"task journal error: %s\n",sqlite3_errstr(sqlite3_errcode(s->db)));
    task_store_close(s);
    return NULL; /* Never replace a damaged or unknown database with an empty one. */
}
void task_store_close(struct task_store *s) { if (s) { sqlite3_close(s->db); free(s); } }
void task_clear(struct task *t) { free(t->result); memset(t,0,sizeof(*t)); }
static bool copy_text(sqlite3_stmt *q, int i, char *out, size_t capacity) {
    const char *p=(const char *)sqlite3_column_text(q,i);
    int n=sqlite3_column_bytes(q,i);
    if (!p || n<0 || (size_t)n>=capacity || memchr(p,0,(size_t)n)) return false;
    memcpy(out,p,(size_t)n); out[n]=0; return true;
}
static bool read_task(sqlite3_stmt *q, struct task *t, bool with_result) {
    memset(t,0,sizeof(*t));
    if (sqlite3_column_int64(q,0)<=0 || sqlite3_column_bytes(q,1)!=16 || sqlite3_column_bytes(q,3)!=16) return false;
    t->id=(uint64_t)sqlite3_column_int64(q,0);
    memcpy(t->epoch,sqlite3_column_blob(q,1),16); memcpy(t->boot,sqlite3_column_blob(q,3),16);
    if (!copy_text(q,2,t->device,sizeof(t->device)) || !copy_text(q,4,t->key,sizeof(t->key)) ||
        !copy_text(q,5,t->command,sizeof(t->command)) || !copy_text(q,7,t->reason,sizeof(t->reason))) return false;
    uint8_t key[16];
    if (!myvava_id_valid(t->device) || !myvava_unhex(t->key,key,sizeof(key))) return false;
    int state=sqlite3_column_int(q,6),total=sqlite3_column_int(q,10),received=sqlite3_column_int(q,11);
    if (state<QUEUED || state>UNKNOWN || total<0 || total>=MYVAVA_RESULT_MAX || received<0 || received>total) return false;
    t->state=(enum task_state)state; t->expires=(uint64_t)sqlite3_column_int64(q,8);
    t->created_at=(uint64_t)sqlite3_column_int64(q,9); t->result_size=(uint32_t)total; t->received=(uint32_t)received;
    if (with_result) {
        if (sqlite3_column_bytes(q,12)!=received || (state==COMPLETE && (!total || received!=total))) return false;
        t->result=calloc(1,(size_t)total+1);
        if (!t->result) return false;
        if (received) memcpy(t->result,sqlite3_column_blob(q,12),(size_t)received);
    }
    return true;
}
static int fetch(sqlite3_stmt *q, struct task *t) {
    memset(t,0,sizeof(*t));
    if (!q) return -1;
    int status=sqlite3_step(q);
    int result=status==SQLITE_DONE?0:status==SQLITE_ROW && read_task(q,t,true)?1:-1;
    if (sqlite3_finalize(q)!=SQLITE_OK) result=-1;
    if (result<0) task_clear(t);
    return result;
}
int task_get(struct task_store *s, const uint8_t epoch[16], uint64_t id, struct task *t) {
    if (id>INT64_MAX) { memset(t,0,sizeof(*t)); return 0; }
    sqlite3_stmt *q=prepare(s,"SELECT " COLUMNS ",result FROM tasks WHERE epoch=? AND id=?");
    if (q) { blob(q,1,epoch,16); sqlite3_bind_int64(q,2,(sqlite3_int64)id); }
    return fetch(q,t);
}
int task_find_key(struct task_store *s, const char *device, const char *key, struct task *t) {
    sqlite3_stmt *q=prepare(s,"SELECT " COLUMNS ",result FROM tasks WHERE device=? AND request_key=?");
    if (q) { string(q,1,device); string(q,2,key); }
    return fetch(q,t);
}
int task_pending(struct task_store *s, const uint8_t epoch[16], const char *device, struct task *t) {
    sqlite3_stmt *q=prepare(s,"SELECT " COLUMNS ",result FROM tasks WHERE epoch=? AND device=? AND state IN (0,1) ORDER BY id LIMIT 1");
    if (q) { blob(q,1,epoch,16); string(q,2,device); }
    return fetch(q,t);
}
int task_insert(struct task_store *s, struct task *t, const char *prefix, const char *previous) {
    if (!sql(s,"BEGIN IMMEDIATE")) return -1;
    uint64_t count=0;
    if (!scalar(s,"SELECT count(*) FROM tasks",&count)) goto failed;
    while (count>=s->limit) {
        sqlite3_stmt *q=prepare(s,"DELETE FROM tasks WHERE id=(SELECT id FROM tasks WHERE state IN (2,3,4)"
            " AND substr(request_key,1,16) NOT IN (?,?) AND (state=3 OR epoch!=? OR reclaimable=1) ORDER BY id LIMIT 1)");
        if (!q) goto failed;
        string(q,1,prefix); string(q,2,previous); blob(q,3,t->epoch,16);
        if (!finish(q)) goto failed;
        if (!sqlite3_changes(s->db)) { sql(s,"ROLLBACK"); return 0; }
        if (!sql(s,"UPDATE stats SET pruned=pruned+1 WHERE id=1")) goto failed;
        --count;
    }
    sqlite3_stmt *q=prepare(s,"INSERT INTO tasks(epoch,device,boot,request_key,command,state,expires,created_at) VALUES(?,?,?,?,?,0,?,?)");
    if (!q) goto failed;
    blob(q,1,t->epoch,16); string(q,2,t->device); blob(q,3,t->boot,16); string(q,4,t->key); string(q,5,t->command);
    sqlite3_bind_int64(q,6,(sqlite3_int64)t->expires); sqlite3_bind_int64(q,7,(sqlite3_int64)time(NULL));
    if (!finish(q)) goto failed;
    t->id=(uint64_t)sqlite3_last_insert_rowid(s->db);
    if (!sql(s,"COMMIT")) goto failed;
    return 1;
failed:
    sql(s,"ROLLBACK"); return -1;
}
bool task_dispatch(struct task_store *s, struct task *t) {
    if (t->state==SENT) return true;
    sqlite3_stmt *q=prepare(s,"UPDATE tasks SET state=1 WHERE id=? AND state=0");
    if (!q) return false;
    sqlite3_bind_int64(q,1,(sqlite3_int64)t->id);
    bool ok=finish(q) && sqlite3_changes(s->db)==1;
    if (ok) t->state=SENT;
    return ok;
}
bool task_save_result(struct task_store *s, struct task *t) {
    sqlite3_stmt *q=prepare(s,"UPDATE tasks SET total=?,received=?,result=?,state=?,reason=? WHERE id=? AND state IN (1,2,4)");
    if (!q) return false;
    bool complete=t->received==t->result_size;
    sqlite3_bind_int(q,1,(int)t->result_size); sqlite3_bind_int(q,2,(int)t->received);
    blob(q,3,t->result,(int)t->received); sqlite3_bind_int(q,4,complete?COMPLETE:t->state);
    string(q,5,complete?"":t->reason); sqlite3_bind_int64(q,6,(sqlite3_int64)t->id);
    return finish(q) && sqlite3_changes(s->db)==1;
}
bool task_confirm_result(struct task_store *s, const uint8_t epoch[16], const char *device,
                         const uint8_t boot[16], uint64_t id, uint32_t total) {
    if (!id || id>INT64_MAX || !total) return true;
    /* An empty SYNC at the full offset proves receipt of the final ACK. It
     * also proves the station has moved beyond any earlier uncertain task. */
    sqlite3_stmt *q=prepare(s,"UPDATE tasks SET reclaimable=1 WHERE epoch=? AND device=? AND boot=? AND id<=?"
        " AND state IN (2,3,4) AND reclaimable=0 AND EXISTS(SELECT 1 FROM tasks WHERE id=? AND epoch=?"
        " AND device=? AND boot=? AND state=2 AND total=? AND received=total)");
    if (!q) return false;
    blob(q,1,epoch,16); string(q,2,device); blob(q,3,boot,16); sqlite3_bind_int64(q,4,(sqlite3_int64)id);
    sqlite3_bind_int64(q,5,(sqlite3_int64)id); blob(q,6,epoch,16); string(q,7,device); blob(q,8,boot,16);
    sqlite3_bind_int(q,9,(int)total); return finish(q);
}
bool task_expire(struct task_store *s, const uint8_t epoch[16], uint64_t now) {
    sqlite3_stmt *q=prepare(s,"UPDATE tasks SET reason=CASE state WHEN 0 THEN 'expired_before_dispatch' ELSE 'result_timeout' END,"
        "state=CASE state WHEN 0 THEN 3 ELSE 4 END WHERE epoch=? AND state IN (0,1) AND expires<=?");
    if (!q) return false;
    blob(q,1,epoch,16); sqlite3_bind_int64(q,2,(sqlite3_int64)now); return finish(q);
}
bool task_new_boot(struct task_store *s, const uint8_t epoch[16], const char *device, const uint8_t boot[16]) {
    sqlite3_stmt *q=prepare(s,"UPDATE tasks SET reason=CASE state WHEN 0 THEN 'device_restarted_before_dispatch'"
        " WHEN 1 THEN 'device_restarted_after_dispatch' ELSE reason END,"
        "state=CASE state WHEN 0 THEN 3 WHEN 1 THEN 4 ELSE state END,reclaimable=1"
        " WHERE epoch=? AND device=? AND boot!=? AND (state IN (0,1) OR reclaimable=0)");
    if (!q) return false;
    blob(q,1,epoch,16); string(q,2,device); blob(q,3,boot,16); return finish(q);
}
bool task_stats(struct task_store *s, uint64_t *count, uint64_t *pruned) {
    return scalar(s,"SELECT count(*) FROM tasks",count) && scalar(s,"SELECT pruned FROM stats WHERE id=1",pruned);
}
int task_history(struct task_store *s, const char *device, uint64_t before, struct task *out, unsigned capacity) {
    if (before>INT64_MAX) return -1;
    sqlite3_stmt *q=prepare(s,"SELECT " COLUMNS " FROM tasks WHERE device=? AND id<? ORDER BY id DESC LIMIT ?");
    if (!q) return -1;
    string(q,1,device); sqlite3_bind_int64(q,2,before?(sqlite3_int64)before:INT64_MAX); sqlite3_bind_int(q,3,(int)capacity);
    int status,count=0;
    while ((status=sqlite3_step(q))==SQLITE_ROW) {
        if ((unsigned)count>=capacity || !read_task(q,&out[count],false)) { count=-1; break; }
        ++count;
    }
    if (status!=SQLITE_DONE) count=-1;
    if (sqlite3_finalize(q)!=SQLITE_OK) count=-1;
    return count;
}

int event_store_put(struct task_store *s, const char *device, const uint8_t data[MYVAVA_EVENT_SIZE], unsigned limit) {
    struct myvava_event e;
    if (!myvava_event_decode(data,MYVAVA_EVENT_SIZE,&e) || !sql(s,"BEGIN IMMEDIATE")) return -1;
    sqlite3_stmt *q=prepare(s,"SELECT sequence,payload FROM event_streams WHERE device=? AND origin=?");
    if (!q) goto failed;
    string(q,1,device); blob(q,2,e.origin,16);
    int rc=sqlite3_step(q); uint64_t previous=0;
    bool duplicate=false, conflict=false;
    if (rc==SQLITE_ROW) {
        previous=(uint64_t)sqlite3_column_int64(q,0);
        duplicate=e.sequence<=previous;
        conflict=e.sequence==previous && (sqlite3_column_bytes(q,1)!=MYVAVA_EVENT_SIZE ||
            memcmp(sqlite3_column_blob(q,1),data,MYVAVA_EVENT_SIZE));
    }
    bool valid=(rc==SQLITE_ROW || rc==SQLITE_DONE);
    if (sqlite3_finalize(q)!=SQLITE_OK || !valid || conflict) goto failed;
    if (duplicate) { sql(s,"ROLLBACK"); return 1; }
    if (!previous) {
        uint64_t count;
        if (!scalar(s,"SELECT count(*) FROM event_streams",&count) || count>=65536) goto failed;
    }
    uint64_t retained;
    if (!scalar(s,"SELECT count(*) FROM events",&retained)) goto failed;
    if (retained>=limit) {
        q=prepare(s,"DELETE FROM events WHERE id IN (SELECT id FROM events ORDER BY id LIMIT ?)");
        if (!q) goto failed;
        sqlite3_bind_int64(q,1,(sqlite3_int64)(retained-limit+1));
        if (!finish(q)) goto failed;
        unsigned removed=(unsigned)sqlite3_changes(s->db);
        q=prepare(s,"UPDATE event_stats SET pruned=pruned+?");
        if (!q) goto failed;
        sqlite3_bind_int(q,1,(int)removed); if (!finish(q)) goto failed;
    }
    q=prepare(s,"INSERT INTO events(device,origin,sequence,received_at,gap,payload,category,serial,clip) VALUES(?,?,?,?,?,?,?,?,?)");
    if (!q) goto failed;
    string(q,1,device); blob(q,2,e.origin,16); sqlite3_bind_int64(q,3,(sqlite3_int64)e.sequence);
    sqlite3_bind_int64(q,4,(sqlite3_int64)time(NULL)); sqlite3_bind_int64(q,5,(sqlite3_int64)(e.sequence-previous-1));
    blob(q,6,data,MYVAVA_EVENT_SIZE);char clip[17];uint8_t raw_id[8];for(unsigned i=0;i<8;++i)raw_id[7-i]=(uint8_t)(e.related_id>>(i*8));myvava_hex(raw_id,8,clip);
    string(q,7,myvava_event_category(e.kind));string(q,8,e.serial);string(q,9,clip);if(!finish(q))goto failed;
    if(e.related_id){unsigned state=e.kind==MVE_RECORD_START?1:e.kind==MVE_RECORD_STOP_REQUEST?2:e.kind==MVE_RECORD_FAILED?3:(e.kind==MVE_RECORD_COMMITTED||e.kind==MVE_RECORD_RECOVERED)?4:0;
        q=prepare(s,"INSERT INTO clip_states(device,serial,clip,state) VALUES(?,?,?,?) ON CONFLICT(device,serial,clip) DO UPDATE SET state=max(state,excluded.state)");if(!q)goto failed;
        string(q,1,device);string(q,2,e.serial);string(q,3,clip);sqlite3_bind_int(q,4,(int)state);if(!finish(q))goto failed;
    }
    if(retained>=limit&&!sql(s,"DELETE FROM clip_states WHERE NOT EXISTS(SELECT 1 FROM events WHERE events.device=clip_states.device AND events.serial=clip_states.serial AND events.clip=clip_states.clip)"))goto failed;
    q=prepare(s,"INSERT INTO event_streams(device,origin,sequence,payload) VALUES(?,?,?,?) "
        "ON CONFLICT(device,origin) DO UPDATE SET sequence=excluded.sequence,payload=excluded.payload");
    if (!q) goto failed;
    string(q,1,device); blob(q,2,e.origin,16); sqlite3_bind_int64(q,3,(sqlite3_int64)e.sequence);
    blob(q,4,data,MYVAVA_EVENT_SIZE);
    if (!finish(q) || !sql(s,"COMMIT")) goto failed;
    return 1;
failed:
    sql(s,"ROLLBACK"); return -1;
}
int event_store_history(struct task_store *s,const char *device,uint64_t before,struct stored_event *out,unsigned capacity){return event_store_filtered(s,device,before,"", "",out,capacity);}
int event_store_filtered(struct task_store *s,const char *device,uint64_t before,const char *category,const char *serial,struct stored_event *out,unsigned capacity) {
    if(before>INT64_MAX)return -1;
    sqlite3_stmt *q=prepare(s,"SELECT e.id,e.received_at,e.gap,e.payload,coalesce(c.state,0) FROM events e LEFT JOIN clip_states c ON c.device=e.device AND c.serial=e.serial AND c.clip=e.clip WHERE e.device=? AND (?=0 OR e.id<?) AND (?='' OR e.category=?) AND (?='' OR e.serial=?) ORDER BY e.id DESC LIMIT ?");
    if(!q)return -1;string(q,1,device);sqlite3_bind_int64(q,2,(sqlite3_int64)before);sqlite3_bind_int64(q,3,(sqlite3_int64)before);
    string(q,4,category);string(q,5,category);string(q,6,serial);string(q,7,serial);sqlite3_bind_int(q,8,(int)capacity);int count=0,rc;
    while((rc=sqlite3_step(q))==SQLITE_ROW){
        if((unsigned)count>=capacity||!myvava_event_decode(sqlite3_column_blob(q,3),(size_t)sqlite3_column_bytes(q,3),&out[count].event))break;
        out[count].id=(uint64_t)sqlite3_column_int64(q,0);out[count].received_at=(uint64_t)sqlite3_column_int64(q,1);out[count].gap=(uint64_t)sqlite3_column_int64(q,2);out[count].clip_state=(unsigned)sqlite3_column_int(q,4);++count;
    }
    bool ok=rc==SQLITE_DONE;if(sqlite3_finalize(q)!=SQLITE_OK)ok=false;return ok?count:-1;
}
bool event_store_stats(struct task_store *s, uint64_t *count, uint64_t *pruned, uint64_t *streams) {
    return scalar(s,"SELECT count(*) FROM events",count) && scalar(s,"SELECT pruned FROM event_stats WHERE id=1",pruned) &&
        scalar(s,"SELECT count(*) FROM event_streams",streams);
}
