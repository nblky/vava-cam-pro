#include "web_auth.h"
#include "myvava.h"
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define HASH_ROUNDS 600000
struct web_auth { sqlite3 *db; pthread_mutex_t lock; uint64_t next_login; };
static bool run(struct web_auth *a,const char *sql) { return sqlite3_exec(a->db,sql,NULL,NULL,NULL)==SQLITE_OK; }
static sqlite3_stmt *query(struct web_auth *a,const char *sql) { sqlite3_stmt *q=NULL;if(sqlite3_prepare_v2(a->db,sql,-1,&q,NULL)!=SQLITE_OK)return NULL;return q; }
static void bind(sqlite3_stmt *q,int i,const char *s) {sqlite3_bind_text(q,i,s,-1,SQLITE_TRANSIENT);}
static bool password_valid(const char *s) {size_t n=strlen(s);if(n<12||n>128)return false;for(size_t i=0;i<n;++i)if((unsigned char)s[i]<32||(unsigned char)s[i]>126)return false;return true;}
static bool hash(const char *s,const uint8_t salt[16],uint8_t out[32]) {return PKCS5_PBKDF2_HMAC(s,(int)strlen(s),salt,16,HASH_ROUNDS,EVP_sha256(),32,out)==1;}
static bool token_hash(const char *token,uint8_t out[32]) {uint8_t b[32];unsigned n;return myvava_unhex(token,b,32)&&EVP_Digest(b,32,out,&n,EVP_sha256(),NULL)==1&&n==32;}
static bool acl_valid(const char *s) {
    if(!strcmp(s,"*"))return true;if(!*s||strlen(s)>=1024)return false;
    char copy[1024];strcpy(copy,s);char *save=NULL;
    if(*s==','||s[strlen(s)-1]==','||strstr(s,",,"))return false;
    for(char *p=strtok_r(copy,",",&save);p;p=strtok_r(NULL,",",&save))if(!myvava_id_valid(p))return false;return true;
}
bool web_auth_allowed(const struct web_user *u,const char *device) {
    if(!myvava_id_valid(device))return false;if(!strcmp(u->devices,"*"))return true;
    const char *p=u->devices;size_t n=strlen(device);
    while(*p){const char *end=strchr(p,',');size_t m=end?(size_t)(end-p):strlen(p);if(m==n&&!memcmp(p,device,n))return true;if(!end)break;p=end+1;}return false;
}
static bool read_user(sqlite3_stmt *q,struct web_user *u) {
    const char *name=(const char *)sqlite3_column_text(q,0),*acl=(const char *)sqlite3_column_text(q,2);int role=sqlite3_column_int(q,1);
    if(!name||!myvava_id_valid(name)||!acl||!acl_valid(acl)||role<1||role>3)return false;
    strcpy(u->name,name);strcpy(u->devices,acl);u->role=(enum web_role)role;return true;
}
static int set_user_locked(struct web_auth *a,const char *name,const char *password,enum web_role role,const char *acl,bool disabled) {
    uint8_t salt[16],digest[32];bool replace=password&&*password;
    if(!myvava_id_valid(name)||role<1||role>3||!acl_valid(acl)||(role==WEB_ADMIN&&strcmp(acl,"*"))||(replace&&!password_valid(password)))return 400;
    if(replace&&(!myvava_random(salt,16)||!hash(password,salt,digest)))return 503;
    if(!run(a,"BEGIN IMMEDIATE"))return 503;
    sqlite3_stmt *q=query(a,"SELECT role,disabled FROM users WHERE name=?1");if(!q)goto fail;bind(q,1,name);int r=sqlite3_step(q);bool exists=r==SQLITE_ROW;
    bool was_admin=exists&&sqlite3_column_int(q,0)==WEB_ADMIN&&!sqlite3_column_int(q,1);sqlite3_finalize(q);
    if(r!=SQLITE_ROW&&r!=SQLITE_DONE)goto fail;
    if(!exists&&!replace){run(a,"ROLLBACK");return 400;}
    if(was_admin&&(disabled||role!=WEB_ADMIN)) {
        q=query(a,"SELECT count(*) FROM users WHERE role=3 AND disabled=0");if(!q)goto fail;
        r=sqlite3_step(q);int count=sqlite3_column_int(q,0);sqlite3_finalize(q);
        if(r!=SQLITE_ROW)goto fail;if(count<=1){run(a,"ROLLBACK");return 409;}
    }
    q=query(a,"SELECT count(*) FROM users");if(!q)goto fail;r=sqlite3_step(q);int count=sqlite3_column_int(q,0);sqlite3_finalize(q);
    if(r!=SQLITE_ROW)goto fail;if(!exists&&count>=64){run(a,"ROLLBACK");return 409;}
    q=query(a,replace?"INSERT INTO users(name,role,devices,disabled,salt,digest) VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(name) DO UPDATE SET role=excluded.role,devices=excluded.devices,disabled=excluded.disabled,salt=excluded.salt,digest=excluded.digest":"UPDATE users SET role=?2,devices=?3,disabled=?4 WHERE name=?1");
    if(!q)goto fail;bind(q,1,name);sqlite3_bind_int(q,2,role);bind(q,3,acl);sqlite3_bind_int(q,4,disabled);
    if(replace){sqlite3_bind_blob(q,5,salt,16,SQLITE_TRANSIENT);sqlite3_bind_blob(q,6,digest,32,SQLITE_TRANSIENT);}r=sqlite3_step(q);sqlite3_finalize(q);OPENSSL_cleanse(digest,sizeof(digest));if(r!=SQLITE_DONE)goto fail;
    q=query(a,"DELETE FROM sessions WHERE name=?1");if(!q)goto fail;bind(q,1,name);r=sqlite3_step(q);sqlite3_finalize(q);if(r!=SQLITE_DONE||!run(a,"COMMIT"))goto fail;return 200;
fail:run(a,"ROLLBACK");return 503;
}
int web_auth_user_set(struct web_auth *a,const char *name,const char *password,enum web_role role,const char *acl,bool disabled) {pthread_mutex_lock(&a->lock);int r=set_user_locked(a,name,password,role,acl,disabled);pthread_mutex_unlock(&a->lock);return r;}
struct web_auth *web_auth_open(const char *state,const char *bootstrap) {
    char path[2048];if(snprintf(path,sizeof(path),"%s/web.sqlite3",state)>=(int)sizeof(path))return NULL;
    int fd=open(path,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);struct stat st;
    if(fd<0)return NULL;bool safe=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1;close(fd);if(!safe)return NULL;
    struct web_auth *a=calloc(1,sizeof(*a));if(!a)return NULL;pthread_mutex_init(&a->lock,NULL);
    if(sqlite3_open_v2(path,&a->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_FULLMUTEX,NULL)!=SQLITE_OK)goto fail;
    sqlite3_busy_timeout(a->db,2000);
    if(!run(a,"PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON;"
        "CREATE TABLE IF NOT EXISTS users(name TEXT PRIMARY KEY,role INTEGER NOT NULL,devices TEXT NOT NULL,disabled INTEGER NOT NULL,salt BLOB NOT NULL,digest BLOB NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sessions(token BLOB PRIMARY KEY,name TEXT NOT NULL REFERENCES users(name),expires INTEGER NOT NULL,created INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS read_cursors(name TEXT NOT NULL REFERENCES users(name),device TEXT NOT NULL,event TEXT NOT NULL,PRIMARY KEY(name,device));"))goto fail;
    sqlite3_stmt *q=query(a,"PRAGMA quick_check");if(!q)goto fail;int r=sqlite3_step(q);bool good=r==SQLITE_ROW&&!strcmp((const char *)sqlite3_column_text(q,0),"ok");sqlite3_finalize(q);if(!good)goto fail;
    q=query(a,"SELECT count(*) FROM users WHERE role=3 AND disabled=0");if(!q)goto fail;r=sqlite3_step(q);int count=sqlite3_column_int(q,0);sqlite3_finalize(q);if(r!=SQLITE_ROW)goto fail;
    if(!count) {
        if(!bootstrap)goto fail;fd=open(bootstrap,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)goto fail;
        char pass[131]={0};safe=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&st.st_size>=12&&st.st_size<=129;
        ssize_t n=safe?read(fd,pass,sizeof(pass)-1):-1;close(fd);if(n<=0||n!=st.st_size)goto fail;if(pass[n-1]=='\n')pass[--n]=0;
        r=set_user_locked(a,"admin",pass,WEB_ADMIN,"*",false);OPENSSL_cleanse(pass,sizeof(pass));if(r!=200)goto fail;
        int dir=open(state,O_RDONLY|O_DIRECTORY|O_CLOEXEC);good=dir>=0&&!fsync(dir);if(dir>=0)close(dir);if(!good)goto fail;
    }
    return a;
fail:web_auth_close(a);return NULL;
}
void web_auth_close(struct web_auth *a) {if(!a)return;sqlite3_close(a->db);pthread_mutex_destroy(&a->lock);free(a);}
bool web_auth_session(struct web_auth *a,const char *token,struct web_user *u) {
    uint8_t digest[32];if(!token_hash(token,digest))return false;
    pthread_mutex_lock(&a->lock);sqlite3_stmt *q=query(a,"SELECT u.name,u.role,u.devices FROM sessions s JOIN users u ON s.name=u.name WHERE s.token=?1 AND s.expires>?2 AND u.disabled=0");bool ok=false;
    if(q){sqlite3_bind_blob(q,1,digest,32,SQLITE_TRANSIENT);sqlite3_bind_int64(q,2,(sqlite3_int64)time(NULL));ok=sqlite3_step(q)==SQLITE_ROW&&read_user(q,u);sqlite3_finalize(q);}pthread_mutex_unlock(&a->lock);return ok;
}
int web_auth_login(struct web_auth *a,const char *name,const char *password,char token[65],struct web_user *u) {
    if(!myvava_id_valid(name)||strlen(password)>128)return 401;
    pthread_mutex_lock(&a->lock);uint64_t now=myvava_millis();int status=503;
    if(now<a->next_login){pthread_mutex_unlock(&a->lock);return 429;}a->next_login=now+500;
    uint8_t salt[16]={0},expected[32]={0},actual[32];bool found=false;
    sqlite3_stmt *q=query(a,"SELECT name,role,devices,salt,digest,disabled FROM users WHERE name=?1");if(!q)goto done;bind(q,1,name);
    if(sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_bytes(q,3)==16&&sqlite3_column_bytes(q,4)==32&&read_user(q,u)&&!sqlite3_column_int(q,5)){memcpy(salt,sqlite3_column_blob(q,3),16);memcpy(expected,sqlite3_column_blob(q,4),32);found=true;}sqlite3_finalize(q);
    bool matches=hash(password,salt,actual)&&CRYPTO_memcmp(expected,actual,32)==0;OPENSSL_cleanse(actual,sizeof(actual));
    if(!matches||!found){status=401;goto done;}
    uint8_t raw[32],digest[32];if(!myvava_random(raw,32))goto done;myvava_hex(raw,32,token);OPENSSL_cleanse(raw,sizeof(raw));if(!token_hash(token,digest))goto done;
    if(!run(a,"BEGIN IMMEDIATE"))goto done;
    q=query(a,"DELETE FROM sessions WHERE expires<=?1 OR token IN (SELECT token FROM sessions WHERE name=?2 ORDER BY created DESC LIMIT -1 OFFSET 7)");if(!q)goto rollback;
    sqlite3_bind_int64(q,1,(sqlite3_int64)time(NULL));bind(q,2,name);int r=sqlite3_step(q);sqlite3_finalize(q);if(r!=SQLITE_DONE)goto rollback;
    q=query(a,"SELECT count(*) FROM sessions");if(!q)goto rollback;r=sqlite3_step(q);int count=sqlite3_column_int(q,0);sqlite3_finalize(q);if(r!=SQLITE_ROW||count>=256)goto rollback;
    q=query(a,"INSERT INTO sessions(token,name,expires,created) VALUES(?1,?2,?3,?4)");if(!q)goto rollback;
    sqlite3_bind_blob(q,1,digest,32,SQLITE_TRANSIENT);bind(q,2,name);sqlite3_bind_int64(q,3,(sqlite3_int64)time(NULL)+43200);sqlite3_bind_int64(q,4,(sqlite3_int64)time(NULL));r=sqlite3_step(q);sqlite3_finalize(q);if(r!=SQLITE_DONE||!run(a,"COMMIT"))goto rollback;status=200;goto done;
rollback:run(a,"ROLLBACK");
done:pthread_mutex_unlock(&a->lock);return status;
}
bool web_auth_logout(struct web_auth *a,const char *token) {
    uint8_t digest[32];if(!token_hash(token,digest))return false;pthread_mutex_lock(&a->lock);
    sqlite3_stmt *q=query(a,"DELETE FROM sessions WHERE token=?1");bool ok=false;if(q){sqlite3_bind_blob(q,1,digest,32,SQLITE_TRANSIENT);ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);}pthread_mutex_unlock(&a->lock);return ok;
}
int web_auth_password(struct web_auth *a,const struct web_user *u,const char *old,const char *password) {
    if(!password_valid(password)||strlen(old)>128)return 400;
    pthread_mutex_lock(&a->lock);int status=503;uint64_t now=myvava_millis();if(now<a->next_login){pthread_mutex_unlock(&a->lock);return 429;}a->next_login=now+500;
    sqlite3_stmt *q=query(a,"SELECT name,role,devices,salt,digest FROM users WHERE name=?1 AND disabled=0");uint8_t salt[16],digest[32],actual[32];struct web_user current;bool found=false;
    if(q){bind(q,1,u->name);if(sqlite3_step(q)==SQLITE_ROW&&sqlite3_column_bytes(q,3)==16&&sqlite3_column_bytes(q,4)==32&&read_user(q,&current)){memcpy(salt,sqlite3_column_blob(q,3),16);memcpy(digest,sqlite3_column_blob(q,4),32);found=true;}sqlite3_finalize(q);}
    if(found){status=hash(old,salt,actual)&&CRYPTO_memcmp(actual,digest,32)==0?set_user_locked(a,current.name,password,current.role,current.devices,false):401;OPENSSL_cleanse(actual,sizeof(actual));}pthread_mutex_unlock(&a->lock);return status;
}
bool web_auth_users(struct web_auth *a,char *out,size_t cap) {
    pthread_mutex_lock(&a->lock);sqlite3_stmt *q=query(a,"SELECT name,role,devices,disabled FROM users ORDER BY name");bool ok=q!=NULL;size_t n=(size_t)snprintf(out,cap,"{\"ok\":true,\"users\":[");unsigned count=0;int r=SQLITE_DONE;
    while(ok&&(r=sqlite3_step(q))==SQLITE_ROW){struct web_user u;if(!read_user(q,&u)){ok=false;break;}int k=snprintf(out+n,cap-n,"%s{\"name\":\"%s\",\"role\":%d,\"devices\":\"%s\",\"disabled\":%s}",count++?",":"",u.name,u.role,u.devices,sqlite3_column_int(q,3)?"true":"false");if(k<0||(size_t)k>=cap-n){ok=false;break;}n+=(size_t)k;}
    if(ok)ok=r==SQLITE_DONE&&n+3<cap;if(ok)memcpy(out+n,"]}",3);sqlite3_finalize(q);pthread_mutex_unlock(&a->lock);return ok;
}
bool web_auth_cursor(struct web_auth *a,const struct web_user *u,const char *device,char out[17]) {
    strcpy(out,"0000000000000000");pthread_mutex_lock(&a->lock);sqlite3_stmt *q=query(a,"SELECT event FROM read_cursors WHERE name=?1 AND device=?2");bool ok=false;
    if(q){bind(q,1,u->name);bind(q,2,device);int r=sqlite3_step(q);uint8_t raw[8];if(r==SQLITE_ROW){const char *s=(const char *)sqlite3_column_text(q,0);ok=s&&myvava_unhex(s,raw,8);if(ok)strcpy(out,s);}else ok=r==SQLITE_DONE;sqlite3_finalize(q);}pthread_mutex_unlock(&a->lock);return ok;
}
bool web_auth_mark(struct web_auth *a,const struct web_user *u,const char *device,const char *event) {
    uint8_t raw[8];if(!myvava_unhex(event,raw,8)||raw[0]>127)return false;char normalized[17];myvava_hex(raw,8,normalized);
    pthread_mutex_lock(&a->lock);sqlite3_stmt *q=query(a,"INSERT INTO read_cursors(name,device,event) VALUES(?1,?2,?3) ON CONFLICT(name,device) DO UPDATE SET event=max(event,excluded.event)");bool ok=false;
    if(q){bind(q,1,u->name);bind(q,2,device);bind(q,3,normalized);ok=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);}pthread_mutex_unlock(&a->lock);return ok;
}
