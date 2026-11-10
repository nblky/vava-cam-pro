#include "directory_scan.h"
#include "nas.h"
#include "nas_control.h"
#include "mp4_archive.h"
#include "protocol.h"
#include "storage_path.h"
#include <sys/statvfs.h>
#include "sealed_file.h"
#include "recorder.h"
#include "files.h"
#include "transfer_journal.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <mntent.h>
#include <sys/vfs.h>
#endif
extern char **environ;
enum { NAS_QUEUED,NAS_COPYING,NAS_DONE,NAS_RETRY,NAS_DEST,NAS_FAILED,NAS_CANCELLED,NAS_EXPIRED };
struct nas_row { struct record_info r; bool seen; unsigned state; uint64_t retry; int error; };
struct nas {
    pthread_t worker; pthread_mutex_t lock,policy_lock; pthread_cond_t wake; bool stop;
    struct nas_control *control;uint8_t target[32];
    struct { char serial[32]; bool enabled; } policy[4];
    struct nas_row rows[RECORD_LIMIT];
    int receipts; pid_t child;struct transfer_journal *journal;uint64_t storage_generation;
};
bool nas_config_valid(const struct nas_config *c) {
    if (!c->host || !*c->host || c->host[0]=='-' || c->host[0]=='.' || strlen(c->host)>253 || !c->export_path || c->export_path[0]!='/' ||
        strlen(c->export_path)>511 || !storage_path_valid(c->mount_dir) || strlen(c->mount_dir)>511 ||
        !c->station_id || !myvava_id_valid(c->station_id) || !c->executable || c->executable[0]!='/') return false;
    for (const char *p=c->host;*p;++p) if (!((*p>='a'&&*p<='z') || (*p>='A'&&*p<='Z') ||
        (*p>='0'&&*p<='9') || *p=='.' || *p=='-' || *p==':')) return false;
    for (const char *p=c->export_path;*p;++p) if ((unsigned char)*p<=32 || (unsigned char)*p>126 || *p=='\\' || *p=='\"' || *p==',') return false;
    return !strstr(c->export_path,"/../") && strcmp(c->export_path,"/..") && !strstr(c->export_path,"/./") && strcmp(c->export_path,"/.");
}
static int private_file(int dir,const char *name,int flags,bool local_owner) {
    int fd=openat(dir,name,flags|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600); struct stat st;
    if (fd<0) return -1;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || (local_owner && st.st_uid!=geteuid()) || (st.st_mode&0077) || st.st_nlink!=1) {
        close(fd); errno=EINVAL; return -1;
    }
    return fd;
}
static bool write_all(int fd,const uint8_t *p,size_t n) {
    while (n) { ssize_t k=write(fd,p,n); if (k<0 && errno==EINTR) continue;
        if (k<=0) return false; p+=k; n-=(size_t)k; } return true;
}
static bool exact(int fd,const void *data,size_t n) {
    uint8_t bytes[161]; if (n>160) return false;
    ssize_t k=pread(fd,bytes,n+1,0); return k==(ssize_t)n && !memcmp(bytes,data,n);
}
static bool atomic_meta(int dir,const char *name,const void *data,size_t n,bool local_owner) {
    char tmp[128]; if (snprintf(tmp,sizeof(tmp),"%s.tmp",name)>=(int)sizeof(tmp)) return false;
    int fd=private_file(dir,tmp,O_WRONLY|O_CREAT,local_owner); if (fd<0) return false;
    bool ok=!ftruncate(fd,0) && write_all(fd,data,n) && !fsync(fd); if (close(fd)) ok=false;
    if (ok) ok=!renameat(dir,tmp,dir,name) && !fsync(dir);
    return ok;
}
static bool descriptor(const struct record_info *r,uint8_t data[128]) {
    if (!r->transport_stream || r->bytes>MYVAVA_FILE_MAX || r->frames>UINT32_MAX || r->audio_frames>UINT32_MAX) return false;
    struct myvava_file f={.id=r->id,.size=(uint32_t)r->bytes,.unix_seconds=r->unix_seconds,
        .first_ms=r->first_ms,.last_ms=r->last_ms,.frames=(uint32_t)r->frames,.audio_frames=(uint32_t)r->audio_frames,
        .sample_rate=r->sample_rate,.channels=(uint8_t)r->channels,.slot=(uint8_t)r->slot,.stream=(uint8_t)r->stream,
        .pir=r->pir,.clock_valid=r->clock_valid};
    memcpy(f.sha256,r->sha256,32); memcpy(f.serial,r->serial,32); return myvava_file_encode(data,&f);
}
static bool receipt(struct nas *n,const struct record_info *r,const uint8_t *desc,bool save) {
    char file[40]; snprintf(file,sizeof(file),"%016" PRIx64 ".done",r->id);
    uint8_t data[160]; memcpy(data,n->target,32); memcpy(data+32,desc,128);
    if (save) return atomic_meta(n->receipts,file,data,sizeof(data),true);
    int fd=private_file(n->receipts,file,O_RDONLY,true); if (fd<0) return false;
    bool ok=exact(fd,data,sizeof(data)); close(fd); return ok;
}
static bool enabled(struct nas *n,const struct record_info *r) {
    uint8_t target[32];if(!n->journal||n->storage_generation!=recorder_storage_generation()||!nas_control_target(n->control,target))return false;
    for(unsigned i=0;i<RECORD_LIMIT;++i)if(n->rows[i].seen&&n->rows[i].r.id==r->id&&(n->rows[i].state==NAS_CANCELLED||n->rows[i].state==NAS_EXPIRED))return false;
    pthread_mutex_lock(&n->policy_lock);bool ok=n->policy[r->slot].enabled&&!strcmp(n->policy[r->slot].serial,r->serial);pthread_mutex_unlock(&n->policy_lock);return ok;
}
static const char *error_name(int);
static int error_code(const char *name){for(int i=0;i<=29;++i)if(!strcmp(name,error_name(i)))return i;return 13;}
static bool job_save(struct nas *n,const struct nas_row *r){uint8_t desc[128];struct transfer_job j={.state=r->state,.offset=r->state==NAS_DONE?(uint32_t)r->r.bytes:0,.retry_at=r->state==NAS_RETRY?(uint64_t)time(NULL)+(r->error==21?1:30):0};if(!descriptor(&r->r,desc)||!myvava_file_decode(desc,128,&j.file))return false;memcpy(j.target,r->r.nas_target,32);snprintf(j.error,sizeof(j.error),"%s",error_name(r->error));return transfer_journal_put(n->journal,&j);}
static bool job_restore(struct nas *n,struct nas_row *r){struct transfer_job j;int found=transfer_journal_get(n->journal,r->r.id,&j);if(found<0)return false;if(!found)return job_save(n,r);if(j.state>NAS_EXPIRED||j.file.size!=r->r.bytes||memcmp(j.file.sha256,r->r.sha256,32)||memcmp(j.target,r->r.nas_target,32))return false;r->state=j.state;r->error=error_code(j.error);if(r->state==NAS_COPYING)r->state=NAS_QUEUED;if(r->state==NAS_QUEUED||r->state==NAS_RETRY){if(memcmp(j.target,n->target,32))r->state=NAS_DEST;uint64_t now=(uint64_t)time(NULL);r->retry=myvava_millis()+(j.retry_at>now?j.retry_at-now:0)*1000;}return true;}
static void set_state(struct nas *n,unsigned index,unsigned state,int error) {
    pthread_mutex_lock(&n->lock);struct nas_row *r=&n->rows[index];
    if(r->state!=NAS_CANCELLED&&r->state!=NAS_EXPIRED){r->state=state;r->error=error;/* Mount health probes run every 30 s. A lease denied while a probe
         * is running must not retry at the same phase forever. This check is
         * memory-only; actual failed copies retain their 30 s backoff. */
        r->retry=myvava_millis()+(error==21?1000:30000);
        if((state==NAS_DONE||state==NAS_RETRY||state==NAS_FAILED||state==NAS_EXPIRED)&&!job_save(n,r)){r->state=NAS_FAILED;r->error=27;}}
    pthread_mutex_unlock(&n->lock);
}
static void copy_record(struct nas *n,unsigned index) {
    struct record_info r=n->rows[index].r; uint8_t desc[128];
    if (!descriptor(&r,desc)) { set_state(n,index,NAS_FAILED,10); return; }
    if (receipt(n,&r,desc,false)) { set_state(n,index,NAS_DONE,0); return; }
    struct record_reader reader={.fd=-1};
    if (!recorder_reader_open(r.slot,r.serial,r.id,&reader)) { set_state(n,index,recorder_available()?NAS_EXPIRED:NAS_RETRY,recorder_available()?28:11); return; }
    if (reader.info.bytes!=r.bytes || memcmp(reader.info.sha256,r.sha256,32)) {
        recorder_reader_close(&reader); set_state(n,index,NAS_FAILED,11); return;
    }
    struct nas_lease lease;
    if(!nas_control_acquire(n->control,&lease)){recorder_reader_close(&reader);set_state(n,index,NAS_RETRY,21);return;}
    char desc_hex[257]; myvava_hex(desc,128,desc_hex);
    char *args[]={(char *)lease.config.executable,"--nas-copy-worker",(char *)lease.config.host,(char *)lease.config.export_path,
        (char *)lease.config.mount_dir,(char *)lease.config.station_id,lease.config.lab_local?"lab-local":"nfs",desc_hex,reader.info.encrypted?(char *)recorder_keyring_path():"-",NULL};
    uint64_t started=myvava_millis(); bool cancelled=false; int status=0;
    unsigned phases=lease.config.lab_local?1:2;
    for (unsigned phase=0;phase<phases;++phase) {
        args[6]=lease.config.lab_local?"lab-local":phase==0?"nfs-prepare":"nfs";
        posix_spawn_file_actions_t actions;
        if (posix_spawn_file_actions_init(&actions)) { status=12<<8; break; }
        posix_spawnattr_t attrs; int rc=posix_spawnattr_init(&attrs); bool attrs_ok=rc==0;
        if (!rc) rc=posix_spawnattr_setflags(&attrs,POSIX_SPAWN_SETPGROUP);
        if (!rc) rc=posix_spawnattr_setpgroup(&attrs,0);
        if (!rc) rc=posix_spawn_file_actions_adddup2(&actions,reader.fd,3);
        pid_t child=0;
        if (!rc) rc=posix_spawn(&child,lease.config.executable,&actions,&attrs,args,environ);
        pthread_mutex_lock(&n->lock);n->child=child;pthread_mutex_unlock(&n->lock);
        if (attrs_ok) posix_spawnattr_destroy(&attrs);
        posix_spawn_file_actions_destroy(&actions);
        if (rc) { status=12<<8; break; }
        set_state(n,index,NAS_COPYING,0);
        for (;;) {
            pid_t p=waitpid(child,&status,WNOHANG);
            if (p==child) { pthread_mutex_lock(&n->lock);n->child=0;pthread_mutex_unlock(&n->lock);break; }
            if (p<0 && errno!=EINTR) { pthread_mutex_lock(&n->lock);n->child=0;pthread_mutex_unlock(&n->lock);status=13<<8;break; }
            pthread_mutex_lock(&n->lock); bool stop=n->stop, pause=!enabled(n,&r)||!nas_control_valid(n->control,&lease); pthread_mutex_unlock(&n->lock);
            if (stop || pause || myvava_millis()-started>45000) {
                if (!cancelled) { (void)kill(-child,SIGKILL); cancelled=true; set_state(n,index,NAS_RETRY,stop||pause?14:15); }
                /* The mount helper replaces the prepare child, so a blocked
                 * mount retains this tracked PID and cannot accumulate. */
                if (stop) { recorder_reader_close(&reader);nas_control_release(n->control);return; }
            }
            struct timespec delay={.tv_nsec=20000000}; nanosleep(&delay,NULL);
        }
        if (cancelled || !WIFEXITED(status) || WEXITSTATUS(status)) break;
        pthread_mutex_lock(&n->lock); bool stop=n->stop, pause=!enabled(n,&r)||!nas_control_valid(n->control,&lease); pthread_mutex_unlock(&n->lock);
        if (stop || pause || myvava_millis()-started>45000) { cancelled=true; break; }
    }
    recorder_reader_close(&reader);nas_control_release(n->control);
    if (!cancelled && WIFEXITED(status) && WEXITSTATUS(status)==0) {
        bool saved=receipt(n,&r,desc,true); set_state(n,index,saved?NAS_DONE:NAS_RETRY,saved?0:24);
    } else set_state(n,index,NAS_RETRY,cancelled?15:WIFEXITED(status)?WEXITSTATUS(status):13);
}

static bool open_storage(struct nas *n){
    struct stat st;int parent=recorder_directory_fd();if(parent<0)return false;
    bool created=!mkdirat(parent,".nas",0700)||errno==EEXIST;
    n->receipts=created?openat(parent,".nas",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;close(parent);
    if(n->receipts<0)return false;
    if(fstat(n->receipts,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077))return false;
    n->journal=transfer_journal_open(n->receipts);if(!n->journal)return false;
    return true;
}
static bool sync_storage(struct nas *n){
    uint64_t generation=recorder_storage_generation();
    pthread_mutex_lock(&n->lock);
    if(generation&&n->journal&&generation==n->storage_generation){pthread_mutex_unlock(&n->lock);return true;}
    transfer_journal_close(n->journal);n->journal=NULL;if(n->receipts>=0)close(n->receipts);n->receipts=-1;n->storage_generation=0;memset(n->rows,0,sizeof(n->rows));
    pthread_mutex_unlock(&n->lock);if(!generation)return false;
    struct nas *stage=calloc(1,sizeof(*stage));if(!stage)return false;stage->receipts=-1;bool ok=open_storage(stage)&&generation==recorder_storage_generation();
    pthread_mutex_lock(&n->lock);if(ok){n->receipts=stage->receipts;n->journal=stage->journal;n->storage_generation=generation;}pthread_mutex_unlock(&n->lock);
    if(!ok){transfer_journal_close(stage->journal);if(stage->receipts>=0)close(stage->receipts);}free(stage);return ok;
}
static void *nas_worker(void *arg) {
    struct nas *n=arg; struct record_info records[RECORD_LIMIT]; unsigned cursor=0;uint64_t before=0,history_before=0;
    for (;;) {
        if(!sync_storage(n)){
            before=history_before=0;pthread_mutex_lock(&n->lock);bool stop=n->stop;
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);++until.tv_sec;if(!stop)pthread_cond_timedwait(&n->wake,&n->lock,&until);
            pthread_mutex_unlock(&n->lock);if(stop)break;continue;
        }
        uint64_t captured_generation;unsigned count=recorder_snapshot_generation(before,records,RECORD_LIMIT,&captured_generation);
        pthread_mutex_lock(&n->lock);
        if(captured_generation!=n->storage_generation){pthread_mutex_unlock(&n->lock);continue;}
        (void)nas_control_target(n->control,n->target); if (n->stop) { pthread_mutex_unlock(&n->lock); break; }
        for (unsigned i=0;i<RECORD_LIMIT;++i) n->rows[i].seen=false;
        for (unsigned k=0;k<count;++k) {
            struct record_info *r=&records[k]; uint8_t nonzero=0; for (unsigned j=0;j<32;++j) nonzero|=r->nas_target[j];
            if (!nonzero) continue;
            for (unsigned i=0;i<RECORD_LIMIT;++i) if (n->rows[i].r.id==r->id && !memcmp(n->rows[i].r.sha256,r->sha256,32)) n->rows[i].seen=true;
        }
        for (unsigned i=0;i<RECORD_LIMIT;++i) if (!n->rows[i].seen) memset(&n->rows[i],0,sizeof(n->rows[i]));
        for (unsigned k=0;k<count;++k) {
            struct record_info *r=&records[k]; uint8_t nonzero=0; for (unsigned j=0;j<32;++j) nonzero|=r->nas_target[j];
            if (!nonzero) continue;
            unsigned i=0; while (i<RECORD_LIMIT && n->rows[i].r.id!=r->id) ++i;
            if (i<RECORD_LIMIT) continue;
            for (i=0;i<RECORD_LIMIT && n->rows[i].seen;++i) {}
            if (i<RECORD_LIMIT){n->rows[i]=(struct nas_row){.r=*r,.seen=true,.state=memcmp(r->nas_target,n->target,32)?NAS_DEST:NAS_QUEUED};if(!job_restore(n,&n->rows[i])){n->rows[i].state=NAS_FAILED;n->rows[i].error=27;}}
        }
        for(unsigned i=0;i<RECORD_LIMIT;++i)if(n->rows[i].seen&&(n->rows[i].state==NAS_QUEUED||n->rows[i].state==NAS_RETRY)&&memcmp(n->rows[i].r.nas_target,n->target,32)){n->rows[i].state=NAS_DEST;(void)job_save(n,&n->rows[i]);}
        struct transfer_job history[16];unsigned hn=transfer_journal_scan(n->journal,history_before,history,16);
        if(hn!=UINT32_MAX){for(unsigned h=0;h<hn;++h){struct transfer_job *j=&history[h];struct record_info info;
            if((j->state==NAS_QUEUED||j->state==NAS_RETRY||j->state==NAS_COPYING||j->state==NAS_DEST)&&recorder_available()&&!recorder_info(j->file.slot,j->file.serial,j->file.id,&info)&&recorder_storage_generation()==n->storage_generation){
                j->state=NAS_EXPIRED;strcpy(j->error,"source_expired");(void)transfer_journal_put(n->journal,j);
            }
            if(transfer_journal_count(n->journal)>61440&&(j->state==2||j->state==6||j->state==7)&&recorder_available()&&!recorder_info(j->file.slot,j->file.serial,j->file.id,&info)&&recorder_storage_generation()==n->storage_generation){char receipt[40];snprintf(receipt,sizeof(receipt),"%016" PRIx64 ".done",j->file.id);if((!unlinkat(n->receipts,receipt,0)||errno==ENOENT)&&!fsync(n->receipts))(void)transfer_journal_delete(n->journal,j->file.id);}
        }history_before=hn==16?history[hn-1].file.id:0;}
        int selected=-1;
        for (unsigned k=0;k<RECORD_LIMIT;++k) {
            unsigned i=(cursor+k)%RECORD_LIMIT; struct nas_row *row=&n->rows[i];
            if (row->seen && enabled(n,&row->r) && (row->state==NAS_QUEUED || (row->state==NAS_RETRY && myvava_millis()>=row->retry))) {
                selected=(int)i; cursor=(i+1)%RECORD_LIMIT; break;
            }
        }
        pthread_mutex_unlock(&n->lock);
        if (selected>=0) copy_record(n,(unsigned)selected);
        else {
            before=count==RECORD_LIMIT?records[count-1].id:0;
            pthread_mutex_lock(&n->lock); struct timespec until; clock_gettime(CLOCK_REALTIME,&until); ++until.tv_sec;
            if (!n->stop) pthread_cond_timedwait(&n->wake,&n->lock,&until); pthread_mutex_unlock(&n->lock);
        }
    }
    return NULL;
}
struct nas *nas_start(const char *record_dir,const char *state_dir,const struct nas_config *cfg) {
    (void)record_dir;struct nas *n=calloc(1,sizeof(*n));if(!n)return NULL;n->receipts=-1;
    n->control=nas_control_start(state_dir,cfg);if(!n->control){free(n);return NULL;}
    (void)nas_control_target(n->control,n->target);
    if(pthread_mutex_init(&n->policy_lock,NULL))goto fail;
    if(pthread_mutex_init(&n->lock,NULL)){pthread_mutex_destroy(&n->policy_lock);goto fail;}
    if(pthread_cond_init(&n->wake,NULL)){pthread_mutex_destroy(&n->lock);pthread_mutex_destroy(&n->policy_lock);goto fail;}
    if(pthread_create(&n->worker,NULL,nas_worker,n)){pthread_cond_destroy(&n->wake);pthread_mutex_destroy(&n->lock);pthread_mutex_destroy(&n->policy_lock);goto fail;}
    return n;
fail:nas_control_stop(n->control);free(n);return NULL;
}
void nas_stop(struct nas *n) {
    if(!n)return;pthread_mutex_lock(&n->lock);n->stop=true;pthread_cond_signal(&n->wake);pthread_mutex_unlock(&n->lock);
    pthread_join(n->worker,NULL);nas_control_stop(n->control);pthread_cond_destroy(&n->wake);pthread_mutex_destroy(&n->lock);pthread_mutex_destroy(&n->policy_lock);transfer_journal_close(n->journal);if(n->receipts>=0)close(n->receipts);free(n);
}
bool nas_target(struct nas *n,uint8_t out[32]){return n&&nas_control_target(n->control,out);}
bool nas_storage_ready(struct nas *n){if(!n)return false;if(pthread_mutex_trylock(&n->lock))return false;bool ok=n->journal&&n->storage_generation==recorder_storage_generation();pthread_mutex_unlock(&n->lock);return ok;}
bool nas_status(struct nas *n,char *out,size_t size){return n&&nas_control_json(n->control,out,size);}
const char *nas_configure(struct nas *n,uint32_t revision,bool enabled,const char *host,const char *path){return n?nas_control_configure(n->control,revision,enabled,host,path):"nas_unavailable";}
const char *nas_control(struct nas *n,uint32_t revision,const char *action){return n?nas_control_action(n->control,revision,action):"nas_unavailable";}
void nas_policy(struct nas *n,unsigned slot,const char *serial,bool enabled_value) {
    if (!n || slot>3 || strlen(serial)>=32) return;
    pthread_mutex_lock(&n->policy_lock); strcpy(n->policy[slot].serial,serial); n->policy[slot].enabled=enabled_value;
    pthread_cond_signal(&n->wake); pthread_mutex_unlock(&n->policy_lock);
}
static const char *error_name(int code) {
    switch (code) {
    case 0: return "";
    case 10: return "unsupported_recording";
    case 11: return "source_unavailable";
    case 12: return "copy_worker_start_failed";
    case 13: return "copy_worker_interrupted";
    case 14: return "copy_cancelled";
    case 15: return "copy_deadline_or_cancelled";
    case 20: return "invalid_archive_manifest";
    case 21: return "nfs_mount_unavailable_or_mismatched";
    case 22: return "archive_directory_unsafe_or_unwritable";
    case 23: return "archive_identity_or_digest_conflict";
    case 24: return "archive_commit_failed";
    case 25: return "archive_file_open_failed";
    case 26: return "archive_io_or_digest_failed";
    case 27: return "transfer_journal_failed";
    case 28: return "source_expired";
    case 29: return "mp4_conversion_failed";
    default: return "copy_worker_failed";
    }
}
bool nas_json(struct nas *n,unsigned slot,const char *serial,unsigned page,char *out,size_t size) {
    if(!n||slot>3||page>4095)return false;const char *names[]={"queued","copying","committed","retry_wait","destination_changed","failed","cancelled","source_expired"};
    uint8_t configured_target[32];bool globally_enabled=nas_control_target(n->control,configured_target);
    pthread_mutex_lock(&n->policy_lock);bool policy_enabled=n->policy[slot].enabled&&!strcmp(n->policy[slot].serial,serial);pthread_mutex_unlock(&n->policy_lock);
    pthread_mutex_lock(&n->lock);if(!n->journal||n->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&n->lock);return false;}struct transfer_job rows[16];unsigned total=0,count=transfer_journal_page(n->journal,slot,serial,page,rows,16,&total);
    if(count==UINT32_MAX){pthread_mutex_unlock(&n->lock);return false;}
    size_t used=(size_t)snprintf(out,size,"{\"ok\":true,\"page\":%u,\"format\":\"mpegts-h264-aac\",\"archive_formats\":[\"ts\",\"mp4\"],\"records\":[",page);
    for(unsigned i=0;i<count&&used<size;++i){struct transfer_job *r=&rows[i];for(unsigned k=0;k<RECORD_LIMIT;++k)if(n->rows[k].seen&&n->rows[k].r.id==r->file.id){r->state=n->rows[k].state;snprintf(r->error,sizeof(r->error),"%s",error_name(n->rows[k].error));break;}
        if(r->state>NAS_EXPIRED){pthread_mutex_unlock(&n->lock);return false;}const char *state=names[r->state];
        if((r->state==NAS_QUEUED||r->state==NAS_COPYING||r->state==NAS_RETRY)&&(!globally_enabled||!policy_enabled))state="paused";
        used+=(size_t)snprintf(out+used,size-used,"%s{\"id\":\"%016" PRIx64 "\",\"state\":\"%s\",\"error_code\":%d,\"error\":\"%s\"}",i?",":"",r->file.id,state,error_code(r->error),r->error);
    }
    if(used<size)used+=(size_t)snprintf(out+used,size-used,"],\"total\":%u}\n",total);pthread_mutex_unlock(&n->lock);return used<size;
}

bool nas_action(struct nas *n,unsigned slot,const char *serial,uint64_t id,const char *action){
    if(!n)return false;pthread_mutex_lock(&n->lock);if(!n->journal||n->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&n->lock);return false;}struct transfer_job j;bool ok=transfer_journal_get(n->journal,id,&j)==1&&j.file.slot==slot&&!strcmp(j.file.serial,serial);
    unsigned index=0;while(index<RECORD_LIMIT&&!(n->rows[index].seen&&n->rows[index].r.id==id))++index;
    struct record_info info;bool present=recorder_info(slot,serial,id,&info);
    if(ok&&!strcmp(action,"forget")){
        ok=!present&&!n->child&&(j.state==NAS_DONE||j.state==NAS_CANCELLED||j.state==NAS_EXPIRED||j.state==NAS_FAILED)&&transfer_journal_delete(n->journal,id);
        if(ok){char file[40];snprintf(file,sizeof(file),"%016" PRIx64 ".done",id);(void)unlinkat(n->receipts,file,0);}
    }else if(ok){
        if(!strcmp(action,"cancel")&&j.state!=NAS_DONE){j.state=NAS_CANCELLED;strcpy(j.error,"copy_cancelled");}
        else if(!strcmp(action,"retry")&&j.state!=NAS_DONE&&j.state!=NAS_DEST&&present&&info.bytes==j.file.size&&!memcmp(info.sha256,j.file.sha256,32)&&!memcmp(j.target,n->target,32)){j.state=NAS_QUEUED;j.error[0]=0;j.retry_at=0;}
        else ok=false;
        if(ok)ok=transfer_journal_put(n->journal,&j);
        if(ok&&index<RECORD_LIMIT){n->rows[index].state=j.state;n->rows[index].retry=0;n->rows[index].error=error_code(j.error);}
    }
    pthread_cond_signal(&n->wake);pthread_mutex_unlock(&n->lock);return ok;
}

/* The child is a fresh exec of the station binary, with only an inherited
 * source descriptor. NFS syscalls, including mount, never run on the core loop. */
static bool mounted(const struct nas_config *c,bool prepare) {
    struct stat st;
    if(prepare&&!c->lab_local&&lstat(c->mount_dir,&st)&&errno==ENOENT&&mkdir(c->mount_dir,0700))return false;
    if (lstat(c->mount_dir,&st) || !S_ISDIR(st.st_mode)) return false;
    if (c->lab_local) return st.st_uid==geteuid() && !(st.st_mode&0022);
#ifdef __linux__
    char source[800]; snprintf(source,sizeof(source),strchr(c->host,':')?"[%s]:%s":"%s:%s",c->host,c->export_path);
    bool already=false,match=false; FILE *f=setmntent("/proc/self/mounts","r"); if (!f) return false;
    struct mntent *e;
    while ((e=getmntent(f))) if (!strcmp(e->mnt_dir,c->mount_dir)) {
        already=true; match=(!strcmp(e->mnt_type,"nfs") || !strcmp(e->mnt_type,"nfs4")) && !strcmp(e->mnt_fsname,source);
    }
    endmntent(f);
    if (already && !match) return false;
    if (!already) {
        if (!prepare || st.st_uid!=geteuid() || (st.st_mode&0022)) return false;
        DIR *dir=opendir(c->mount_dir); if (!dir) return false;
        bool empty=true,scanned=true; struct dirent *entry;
        while ((entry=directory_scan_next(dir,&scanned))) if (strcmp(entry->d_name,".") && strcmp(entry->d_name,"..")) { empty=false; break; }
        closedir(dir); if (!scanned||!empty) return false; /* never hide existing local data under a new mount */
        const char *mount_args[]={"mount.nfs",source,c->mount_dir,"-o",
            "vers=3,proto=tcp,nolock,hard,timeo=30,retrans=2,nodev,nosuid,noexec",NULL};
        /* No wrapper/waiter process: the parent tracks the actual foreground
         * mount syscall process. Never request mount.nfs background mode. */
        execv("/sbin/mount.nfs",(char *const *)mount_args); return false;
    }

    struct statfs fs; return !statfs(c->mount_dir,&fs) && (unsigned long)fs.f_type==0x6969;
#else
    (void)prepare; return false;
#endif
}
static int archive_root(const struct nas_config *c){
    int fd=open(c->mount_dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(fd<0)return -1;
#ifdef __linux__
    if(!c->lab_local){struct statfs fs;if(fstatfs(fd,&fs)||(unsigned long)fs.f_type!=0x6969){close(fd);return -1;}}
#endif
    return fd;
}
int nas_admin_main(int argc,char **argv){
    if(argc!=8)return 20;umask(0077);signal(SIGPIPE,SIG_IGN);(void)setpgid(0,0);
    struct nas_config c={argv[2],argv[3],argv[4],argv[5],argv[0],!strcmp(argv[6],"lab-local")};
    if(!nas_config_valid(&c)||(strcmp(argv[6],"lab-local")&&strcmp(argv[6],"nfs")))return 20;
    if(!strcmp(argv[7],"unmount")){
        if(c.lab_local)return 0;
#ifdef __linux__
        char source[800];snprintf(source,sizeof(source),strchr(c.host,':')?"[%s]:%s":"%s:%s",c.host,c.export_path);
        FILE *table=setmntent("/proc/self/mounts","r");if(!table)return 21;bool found=false,match=false;struct mntent *entry;
        while((entry=getmntent(table)))if(!strcmp(entry->mnt_dir,c.mount_dir)){found=true;match=(!strcmp(entry->mnt_type,"nfs")||!strcmp(entry->mnt_type,"nfs4"))&&!strcmp(entry->mnt_fsname,source);}
        endmntent(table);if(!found)return 0;if(!match)return 21;
        char *args[]={"umount",(char *)c.mount_dir,NULL};execv("/bin/umount",args);return 21;
#else
        return 0;
#endif
    }
    bool prepare=!strcmp(argv[7],"prepare");if(!prepare&&strcmp(argv[7],"probe"))return 20;
    if(!mounted(&c,prepare))return 21;if(prepare)return 0;
    int root=archive_root(&c);if(root<0)return 21;struct statvfs fs;
    if(fstatvfs(root,&fs)||!fs.f_frsize||fs.f_blocks>UINT64_MAX/fs.f_frsize||fs.f_bavail>UINT64_MAX/fs.f_frsize){close(root);return 26;}
    char name[96];snprintf(name,sizeof(name),".myvava-probe-%s-%ld",c.station_id,(long)getpid());
    int probe=openat(root,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);bool ok=probe>=0;
    if(ok){const uint8_t data[]="myvava-nfs-probe";ok=write_all(probe,data,sizeof(data))&&!fsync(probe);if(close(probe))ok=false;if(unlinkat(root,name,0)||fsync(root))ok=false;}
    close(root);if(!ok)return 22;
    uint64_t capacity[2]={(uint64_t)fs.f_blocks*fs.f_frsize,(uint64_t)fs.f_bavail*fs.f_frsize};
    return write_all(3,(const uint8_t *)capacity,sizeof(capacity))?0:26;
}
static int subdir(int parent,const char *name) {
    if (mkdirat(parent,name,0700) && errno!=EEXIST) return -1;
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC); if (fd<0) return -1;
    struct stat st; if (fstat(fd,&st) || (st.st_mode&0077) || fsync(parent)) { close(fd); return -1; }
    return fd;
}
static bool verify(int fd,const struct myvava_file *f) {
    struct stat st; if (fstat(fd,&st) || !S_ISREG(st.st_mode) || (uint64_t)st.st_size!=f->size) return false;
    EVP_MD_CTX *md=EVP_MD_CTX_new(); if (!md) return false;
    bool ok=EVP_DigestInit_ex(md,EVP_sha256(),NULL)==1; uint8_t buffer[65536],digest[32]; unsigned size=0;
    for (uint32_t off=0;ok && off<f->size;) {
        size_t len=f->size-off; if (len>sizeof(buffer)) len=sizeof(buffer);
        ssize_t got=pread(fd,buffer,len,off); if (got<0 && errno==EINTR) continue;
        ok=got>0 && EVP_DigestUpdate(md,buffer,(size_t)(got>0?got:0))==1; if (got>0) off+=(uint32_t)got;
    }
    ok=ok && EVP_DigestFinal_ex(md,digest,&size)==1 && size==32 && !memcmp(digest,f->sha256,32); EVP_MD_CTX_free(md); return ok;
}
/* The canonical TS and the browser/desktop-compatible MP4 are a pair. A
 * durable receipt is emitted only after both files and their manifests commit. */
static bool mp4_digest(int fd,uint8_t digest[32],uint64_t *length){
    struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<=0||st.st_size>MYVAVA_FILE_MAX+16u*1024*1024)return false;
    EVP_MD_CTX *md=EVP_MD_CTX_new();if(!md)return false;bool ok=EVP_DigestInit_ex(md,EVP_sha256(),NULL)==1;uint8_t b[65536];uint64_t off=0;
    while(ok&&off<(uint64_t)st.st_size){size_t want=(uint64_t)st.st_size-off;if(want>sizeof(b))want=sizeof(b);ssize_t got=pread(fd,b,want,(off_t)off);if(got<0&&errno==EINTR)continue;ok=got>0&&EVP_DigestUpdate(md,b,(size_t)(got>0?got:0))==1;if(got>0)off+=(uint64_t)got;}
    unsigned n=0;ok=ok&&EVP_DigestFinal_ex(md,digest,&n)==1&&n==32;EVP_MD_CTX_free(md);*length=(uint64_t)st.st_size;return ok;
}
static int ensure_mp4(int dir,const char *base,const char *ts,const struct myvava_file *f){
    char final[96],part[96],meta[104];snprintf(final,sizeof(final),"%s.mp4",base);snprintf(part,sizeof(part),"%s.mp4.part",base);snprintf(meta,sizeof(meta),"%s.mp4.meta",base);
    uint8_t manifest[88]={0},old[89]={0},digest[32];memcpy(manifest,"MVMP4001",8);memcpy(manifest+8,f->sha256,32);uint64_t length=0;
    int prior=private_file(dir,final,O_RDONLY,false);bool exists=prior>=0;if(prior<0&&errno!=ENOENT)return 23;
    int marker=private_file(dir,meta,O_RDONLY,false);int marker_error=errno;
    if(marker>=0){ssize_t n=read(marker,old,sizeof(old));close(marker);
        bool ok=n==88&&!memcmp(old,manifest,40)&&vava_u32(old+84)==vava_crc32(old,84)&&prior>=0&&mp4_digest(prior,digest,&length)&&!memcmp(old+40,digest,32);
        uint64_t expected=0;for(unsigned i=0;i<8;++i)expected|=(uint64_t)old[72+i]<<(8*i);if(prior>=0)close(prior);return ok&&expected==length?0:23;
    }
    if(prior>=0)close(prior);if(marker_error!=ENOENT)return 24;
    int input=private_file(dir,ts,O_RDONLY,false),output=private_file(dir,part,O_RDWR|O_CREAT,false);bool ok=input>=0&&output>=0;
    if(ok)ok=!ftruncate(output,0)&&mp4_archive(input,output,f)&&!fsync(output)&&mp4_digest(output,digest,&length);
    if(input>=0)close(input);if(output>=0&&close(output))ok=false;if(!ok)return 29;
    memcpy(manifest+40,digest,32);for(unsigned i=0;i<8;++i)manifest[72+i]=(uint8_t)(length>>(8*i));vava_put32(manifest+84,vava_crc32(manifest,84));
    if(exists){prior=private_file(dir,final,O_RDONLY,false);uint8_t prior_digest[32];uint64_t prior_size=0;ok=prior>=0&&mp4_digest(prior,prior_digest,&prior_size)&&prior_size==length&&!memcmp(digest,prior_digest,32);if(prior>=0)close(prior);if(!ok)return 23;(void)unlinkat(dir,part,0);}
    else if(renameat(dir,part,dir,final)||fsync(dir))return 24;
    return atomic_meta(dir,meta,manifest,sizeof(manifest),false)?0:24;
}
int nas_copy_main(int argc,char **argv) {
    if (argc!=8 && argc!=9) return 20;
    umask(0077); signal(SIGPIPE,SIG_IGN);
    /* The copy owns a process group so timeout also kills a mount helper. */
    (void)setpgid(0,0);
    struct nas_config c={argv[2],argv[3],argv[4],argv[5],argv[0],!strcmp(argv[6],"lab-local")};
    uint8_t desc[128]; struct myvava_file f; bool prepare=!strcmp(argv[6],"nfs-prepare");
    if ((strcmp(argv[6],"lab-local") && strcmp(argv[6],"nfs") && !prepare) || !nas_config_valid(&c) ||
        !myvava_unhex(argv[7],desc,128) || !myvava_file_decode(desc,128,&f)) return 20;
    if (!mounted(&c,prepare)) return 21;
    if (prepare) return 0;
    int root=archive_root(&c); if (root<0) return 21;
    int station=subdir(root,c.station_id); close(root); if (station<0) return 22;
    char date[16]="unsynced",time_text[16]="unsynced";
    time_t stamp=(time_t)f.unix_seconds; struct tm utc;
    if (f.clock_valid) {
        if (stamp<0 || (uint64_t)stamp!=f.unix_seconds || !gmtime_r(&stamp,&utc) ||
            !strftime(date,sizeof(date),"%Y-%m-%d",&utc) || !strftime(time_text,sizeof(time_text),"%H%M%SZ",&utc)) { close(station); return 20; }
    }
    int day=subdir(station,date); close(station); if (day<0) return 22;
    char camera_name[96]; size_t cn=0; static const char digits[]="0123456789abcdef";
    for (size_t k=0;f.serial[k];++k) {
        unsigned char c=(unsigned char)f.serial[k];
        if (c=='.' || c==':') { camera_name[cn++]='%'; camera_name[cn++]=digits[c>>4]; camera_name[cn++]=digits[c&15]; }
        else camera_name[cn++]=(char)c;
    }
    camera_name[cn]=0;
    int camera=subdir(day,camera_name); close(day); if (camera<0) return 22;
    char base[80],final[96],part[96],meta[96];
    snprintf(base,sizeof(base),"%s_%016" PRIx64 "_motion",time_text,f.id);
    snprintf(final,sizeof(final),"%s.ts",base); snprintf(part,sizeof(part),"%s.part",base); snprintf(meta,sizeof(meta),"%s.meta",base);
    int prior=private_file(camera,meta,O_RDONLY,false);
    if (prior>=0) { bool same=exact(prior,desc,128); close(prior); if (!same) { close(camera); return 23; } }
    else if (errno!=ENOENT) { close(camera); return 24; }
    int fd=private_file(camera,final,O_RDONLY,false);
    if (fd>=0) {
        bool ok=verify(fd,&f); close(fd);
        if (!ok) { close(camera); return 23; }
        int marker=private_file(camera,meta,O_RDONLY,false);
        if (marker>=0) { ok=exact(marker,desc,128); close(marker); }
        else if (errno==ENOENT) ok=atomic_meta(camera,meta,desc,128,false);
        else ok=false;
        int mp4=ok?ensure_mp4(camera,base,final,&f):24;close(camera);return mp4;
    } else if (errno!=ENOENT) { close(camera); return 23; }
    fd=private_file(camera,part,O_RDWR|O_CREAT,false); if (fd<0) { close(camera); return 25; }
    struct seal_keys *keys=NULL;struct seal_reader *sealed=NULL;
    if(argc==9&&strcmp(argv[8],"-")){keys=seal_keys_load(argv[8]);if(keys)sealed=seal_reader_open(3,keys,f.id,f.size);if(!sealed){seal_keys_free(keys);close(fd);close(camera);return 11;}}
    bool ok=!ftruncate(fd,0); uint8_t buffer[65536];
    for (uint32_t off=0;ok && off<f.size;) {
        size_t len=f.size-off; if (len>sizeof(buffer)) len=sizeof(buffer);
        ssize_t got=sealed?seal_reader_read(sealed,off,buffer,len):pread(3,buffer,len,off); if (got<0 && errno==EINTR) continue;
        ok=got>0 && write_all(fd,buffer,(size_t)(got>0?got:0)); if (got>0) off+=(uint32_t)got;
    }
    seal_reader_free(sealed);seal_keys_free(keys);
    ok=ok && !fsync(fd) && verify(fd,&f); if (close(fd)) ok=false;
    if (!ok) { close(camera); return 26; }
    if (renameat(camera,part,camera,final) || fsync(camera) || !atomic_meta(camera,meta,desc,128,false)) { close(camera); return 24; }
    int mp4=ensure_mp4(camera,base,final,&f);close(camera);return mp4;
}
