#include "directory_scan.h"
#include "recorder.h"
#include "record_index.h"
#include "record_archive.h"
#include "protocol.h"
#include "ts_mux.h"
#include "storage_path.h"
#if MYVAVA_SDK
#include <openssl/evp.h>
#include "sealed_file.h"
#include "files.h"
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum job_kind { OPEN, DATA, FINISH };
struct job { struct job *next; enum job_kind kind; unsigned slot; size_t n; bool frame, audio, keyframe; uint64_t timestamp; uint8_t *data; };
static pthread_t worker;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
/* Index/file I/O never owns the short frame/session mutex. Lock order for
 * compound operations is catalog_lock, then lock. */
static pthread_mutex_t catalog_lock=PTHREAD_MUTEX_INITIALIZER;
static unsigned catalog_records;static bool maintenance;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static struct job *head,*tail;
/* Completion must remain possible after allocation/queue failure. */
static struct job finish_jobs[4];
static struct record_info sessions[4];
static int files[4]={-1,-1,-1,-1};
static bool owned[4];
static struct ts_mux muxes[4];
#if MYVAVA_SDK
static EVP_MD_CTX *digests[4];
static struct seal_writer *sealed_writers[4];
static struct seal_keys *storage_keys;
static char keyring_path[768];
#endif
bool recorder_keys(const char *dir) {
#if MYVAVA_SDK
    if(!dir || strlen(dir)>=sizeof(keyring_path) || storage_keys)return false;
    storage_keys=seal_keys_load(dir);if(!storage_keys)return false;strcpy(keyring_path,dir);return true;
#else
    (void)dir;return false;
#endif
}
const char *recorder_keyring_path(void) {
#if MYVAVA_SDK
    return storage_keys?keyring_path:NULL;
#else
    return NULL;
#endif
}
bool recorder_encrypted(void) {return recorder_keyring_path()!=NULL;}
const char *recorder_key_id(void) {
#if MYVAVA_SDK
    return storage_keys?seal_keys_active(storage_keys):"";
#else
    return "";
#endif
}
static uint64_t storage_size(const struct record_info *r) {
#if MYVAVA_SDK
    if(r->encrypted)return seal_size(r->bytes);
#endif
    return r->bytes;
}
static record_complete_fn complete;
static void *complete_arg;
void recorder_on_complete(record_complete_fn fn, void *arg) { complete=fn; complete_arg=arg; }
const char *recorder_format(const struct record_info *r) { return r->transport_stream?"mpegts-h264-aac":"h264-annex-b"; }
static const char *extension(const struct record_info *r) { return r->encrypted?"sealed":r->transport_stream?"ts":"h264"; }
static char directory[768];
static int directory_fd=-1;
static bool catalog_healthy;
static bool removable;
static char mount_guard[768],storage_error[48];
static uint64_t storage_generation,storage_check;
/* A queued external operation may only touch the medium it was admitted on.
 * Catalog mutation checks this while holding the lifecycle/catalog lock. */
static _Thread_local uint64_t expected_generation;
void recorder_expect_generation(uint64_t generation){expected_generation=generation;}
static bool generation_matches(void){return !expected_generation||expected_generation==storage_generation;}
static void storage_tick(void);
static size_t queued_bytes;
static uint64_t disk_bytes, quota_bytes, last_id;
static bool initialized, stopping;
static struct { uint64_t token, id; } readers[4];
static uint64_t reader_sequence;
static uint64_t recovery_before,recovery_next;
static struct {bool enabled,healthy,clock;unsigned revision,days,pending_hours;uint64_t deleted,bytes,protected,next,after;} retention={.healthy=true};
static int retention_dir=-1;
static void retention_tick(void);
static uint64_t monotonic_ms(void) {
    struct timespec ts; if (clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
struct storage_summary recorder_summary(void) {
    pthread_mutex_lock(&lock);
    struct storage_summary s={.bytes=catalog_healthy?disk_bytes:0,.quota=quota_bytes,.queued=queued_bytes,.records=catalog_healthy?catalog_records:0,
        .available=catalog_healthy&&!stopping&&generation_matches(),.generation=storage_generation};
    strcpy(s.error,storage_error);
    for (unsigned i=0;i<4;++i) s.active+=sessions[i].active;
    pthread_mutex_unlock(&lock);
    return s;
}
unsigned recorder_snapshot_generation(uint64_t before,struct record_info *out,unsigned capacity,uint64_t *generation) {
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);bool available=catalog_healthy&&!stopping&&generation_matches();*generation=available?storage_generation:0;pthread_mutex_unlock(&lock);
    unsigned n=available?record_index_scan(before,out,capacity):0;
    if(n==UINT32_MAX){pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"catalog_read_failed");pthread_mutex_unlock(&lock);*generation=0;n=0;}
    pthread_mutex_unlock(&catalog_lock);return n;
}
unsigned recorder_snapshot_before(uint64_t before,struct record_info *out,unsigned capacity){uint64_t generation;return recorder_snapshot_generation(before,out,capacity,&generation);}
unsigned recorder_snapshot(struct record_info *out,unsigned capacity){return recorder_snapshot_before(0,out,capacity);}
bool recorder_info(unsigned slot,const char *serial,uint64_t id,struct record_info *out) {
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);bool available=catalog_healthy&&!stopping&&generation_matches();pthread_mutex_unlock(&lock);
    bool found=available&&record_index_get(slot,serial,id,out);pthread_mutex_unlock(&catalog_lock);return found;
}
bool recorder_available(void){pthread_mutex_lock(&lock);bool available=initialized&&catalog_healthy&&!stopping&&generation_matches();pthread_mutex_unlock(&lock);return available;}
uint64_t recorder_storage_generation(void){pthread_mutex_lock(&lock);uint64_t value=initialized&&catalog_healthy&&!stopping&&generation_matches()?storage_generation:0;pthread_mutex_unlock(&lock);return value;}
static bool path(char *out, size_t size, uint64_t id, const char *ext) {
    int n=snprintf(out,size,"%s/vrec-%016" PRIx64 ".%s",directory,id,ext);
    return n>=0 && (size_t)n<size;
}
/* All store I/O is relative to the open medium. A removed mount must never
 * redirect a pending writer into the underlying internal-flash mountpoint. */
static const char *leaf(const char *name){const char *p=strrchr(name,'/');return p?p+1:name;}
static const char *relative(const char *name){size_t n=strlen(directory);return !strncmp(name,directory,n)&&name[n]=='/'?name+n+1:name;}
static int store_parent(const char *name){return record_archive_parent(directory_fd,relative(name),false);}
static int store_open(const char *name,int flags){int dir=store_parent(name);if(dir<0)return -1;int fd=openat(dir,leaf(name),flags|O_NOFOLLOW,0600);int error=errno;close(dir);errno=error;return fd;}
static int store_stat(const char *name,struct stat *st){int dir=store_parent(name);if(dir<0)return -1;int rc=fstatat(dir,leaf(name),st,AT_SYMLINK_NOFOLLOW);int error=errno;close(dir);errno=error;return rc;}
static int store_unlink(const char *name){int dir=store_parent(name);if(dir<0)return -1;int rc=unlinkat(dir,leaf(name),0);int error=errno;close(dir);errno=error;return rc;}
static int store_rename(const char *from,const char *to){int a=store_parent(from),b=store_parent(to);int rc=a>=0&&b>=0?renameat(a,leaf(from),b,leaf(to)):-1;int error=errno;if(a>=0)close(a);if(b>=0)close(b);errno=error;return rc;}
static bool sync_name(const char *name){int dir=store_parent(name);if(dir<0)return false;bool ok=!fsync(dir);int error=ok?0:errno;close(dir);errno=error;return ok;}
static bool blob_path(char *out,size_t n,const struct record_info *r){if(!*r->archive)return path(out,n,r->id,extension(r));int k=snprintf(out,n,"%s/%s",directory,r->archive);return k>=0&&(size_t)k<n;}
int recorder_directory_fd(void){pthread_mutex_lock(&lock);int fd=initialized&&catalog_healthy&&!stopping&&generation_matches()?dup(directory_fd):-1;if(fd>=0)(void)fcntl(fd,F_SETFD,FD_CLOEXEC);pthread_mutex_unlock(&lock);return fd;}
static bool write_count(int fd, const void *data, size_t n, size_t *written) {
    const uint8_t *p=data;
    while (n) {
        ssize_t k=write(fd,p,n);
        if (k<0 && errno==EINTR) continue;
        if (k<=0) return false;
        n-=(size_t)k; p+=k; *written+=(size_t)k;
    }
    return true;
}
static bool write_all(int fd, const void *data, size_t n) { size_t written=0; return write_count(fd,data,n,&written); }
static uint64_t remove_blob(const char *p) {
    struct stat st;
    if (store_stat(p,&st) || !S_ISREG(st.st_mode) || store_unlink(p)) return 0;
    return (uint64_t)st.st_size;
}
static void put64(uint8_t *b, uint64_t n) { vava_put32(b,(uint32_t)n); vava_put32(b+4,(uint32_t)(n>>32)); }
static uint64_t get64(const uint8_t *b) { return vava_u32(b)|((uint64_t)vava_u32(b+4)<<32); }
static void event_marker(const struct record_info *r,uint8_t b[64]){memset(b,0,64);memcpy(b,"MVREVT01",8);put64(b+8,r->id);put64(b+16,r->generation);memcpy(b+24,r->sha256,32);vava_put32(b+60,vava_crc32(b,60));}
static bool event_recorded(const struct record_info *r){char name[1024];if(!path(name,sizeof(name),r->id,"evtmark"))return false;int fd=store_open(name,O_RDONLY|O_CLOEXEC|O_NONBLOCK);if(fd<0)return false;uint8_t expected[64],actual[65];event_marker(r,expected);struct stat st;bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&read(fd,actual,sizeof(actual))==64&&!memcmp(actual,expected,64);close(fd);return ok;}
void recorder_event_durable(unsigned slot,const char *serial,uint64_t id,uint64_t generation){
    struct record_info r;pthread_mutex_lock(&catalog_lock);bool found=recorder_available()&&record_index_get(slot,serial,id,&r)&&r.generation==generation;
    int dir=found?recorder_directory_fd():-1;pthread_mutex_unlock(&catalog_lock);
    if(dir<0)return;(void)fcntl(dir,F_SETFD,FD_CLOEXEC);uint8_t b[64];event_marker(&r,b);char temp[64],final[64];snprintf(temp,sizeof(temp),"vrec-%016" PRIx64 ".evtmark.tmp",id);snprintf(final,sizeof(final),"vrec-%016" PRIx64 ".evtmark",id);
    int fd=openat(dir,temp,O_WRONLY|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);struct stat st;
    if(fd>=0){bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&!ftruncate(fd,0)&&write_all(fd,b,sizeof(b))&&!fsync(fd);if(close(fd))ok=false;if(ok&&!renameat(dir,temp,dir,final))(void)fsync(dir);}
    close(dir);
}
static void recover_events(void){
    if(!complete||monotonic_ms()<recovery_next)return;
    struct record_info rows[16];unsigned n=recorder_snapshot_before(recovery_before,rows,16);
    for(unsigned i=0;i<n;++i){
        if(!event_recorded(&rows[i])&&!complete(&rows[i],true,complete_arg)){recovery_next=monotonic_ms()+1000;return;}
        recovery_before=rows[i].id;
    }
    if(n<16){recovery_before=0;recovery_next=monotonic_ms()+60000;}else recovery_next=monotonic_ms()+100;
}
static bool metadata(const struct record_info *r) {
    uint8_t b[512]={0}; char temp[1024], final[1024]; size_t size=r->transport_stream?512:128;
    memcpy(b,r->transport_stream?"VAVAREC6":"VAVAREC1",8); put64(b+16,r->id); put64(b+24,r->generation); put64(b+32,r->unix_seconds);
    put64(b+40,r->bytes); put64(b+48,r->frames); b[56]=(uint8_t)r->slot; b[57]=(uint8_t)r->stream;
    b[58]=r->pir; memcpy(b+64,r->serial,32); memcpy(b+96,r->error,31);
    if (r->transport_stream) {
        b[59]=1; b[60]=r->clock_valid; b[61]=r->encrypted;
        put64(b+128,r->audio_frames); put64(b+136,r->first_ms); put64(b+144,r->last_ms);
        vava_put32(b+152,r->sample_rate); vava_put32(b+156,r->channels); memcpy(b+160,r->sha256,32); memcpy(b+192,r->upload_target,32); memcpy(b+224,r->nas_target,32);
    }
    if(*r->archive)memcpy(b+256,r->archive,80);
    if(r->transport_stream){b[336]=r->file_type;b[337]=r->detection_reported;vava_put32(b+340,r->detection_result);vava_put32(b+344,r->detection_type);}
    vava_put32(b+8,vava_crc32(b+16,size-16));
    if (!path(temp,sizeof(temp),r->id,*r->archive?"archive-part":"meta-part") || !path(final,sizeof(final),r->id,"meta")) return false;
    if(*r->archive){struct stat st;if(!store_stat(temp,&st)){if(!S_ISREG(st.st_mode)||st.st_nlink!=1||store_unlink(temp))return false;}else if(errno!=ENOENT)return false;}
    int fd=store_open(temp,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC); if (fd<0) return false;
    bool ok=write_all(fd,b,size) && fsync(fd)==0;
    if (close(fd)) ok=false;
    if (ok && store_rename(temp,final)) ok=false;
    if (!ok) store_unlink(temp);
    return ok;
}
/* Persist the destination before moving a committed blob. If power is lost,
 * load_entry completes this exact move; timestamps/config changes cannot rename it. */
static bool relocate(struct record_info *r){
    if(*r->archive)return true;
    if(!record_archive_choose(directory_fd,r))return false;
    if(!*r->archive)return true;
    char from[1024],to[1024];path(from,sizeof(from),r->id,extension(r));blob_path(to,sizeof(to),r);
    return metadata(r)&&!fsync(directory_fd)&&!store_rename(from,to)&&sync_name(to)&&!fsync(directory_fd);
}
static bool rebuild_archive(bool cleanup,uint64_t *bytes,uint64_t *removed){
    bool ok=record_archive_reconcile(directory_fd,cleanup,bytes,removed);
    if(!ok){pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"archive_index_failed");pthread_mutex_unlock(&lock);}return ok;
}
static bool update_archive(const struct record_info *r){
    bool ok=record_archive_update(directory_fd,r->archive);
    if(!ok){pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"archive_index_failed");pthread_mutex_unlock(&lock);}return ok;
}
static bool enqueue(enum job_kind kind, unsigned slot, const void *data, size_t n, bool frame) {
    if (n>VAVA_MAX_FRAME || (n && queued_bytes+n>4u*1024u*1024u)) return false;
    struct job *j=kind==FINISH ? &finish_jobs[slot] : malloc(sizeof(*j)+n); if (!j) return false;
    *j=(struct job){.kind=kind,.slot=slot,.n=n,.frame=frame,.data=(uint8_t *)(j+1)};
    if (n) memcpy(j->data,data,n);
    if (tail) tail->next=j; else head=j;
    tail=j; queued_bytes+=n; pthread_cond_signal(&ready); return true;
}
static void failed(unsigned s, const char *why) {
    sessions[s].failed=true; snprintf(sessions[s].error,sizeof(sessions[s].error),"%s",why);
}
static bool sync_directory(void) {return directory_fd>=0&&!fsync(directory_fd);}
struct output { unsigned slot; size_t written, used; uint64_t available,plain_before; bool encrypted; uint8_t buffer[16356]; };
static bool flush(struct output *o) {
    if (!o->used) return true;
    size_t before=o->written;
    bool ok;
#if MYVAVA_SDK
    if(o->encrypted){ok=seal_writer_write(sealed_writers[o->slot],o->buffer,o->used);o->written+=o->used;}
    else ok=write_count(files[o->slot],o->buffer,o->used,&o->written);
    if (digests[o->slot] && EVP_DigestUpdate(digests[o->slot],o->buffer,o->written-before)!=1) ok=false;
#else
    (void)before;ok=write_count(files[o->slot],o->buffer,o->used,&o->written);
#endif
    o->used=0; return ok;
}
static bool output(void *arg, const uint8_t *p, size_t n) {
    struct output *o=arg;
    if (n>o->available || o->written+o->used>o->available-n) return false;
#if MYVAVA_SDK
    if(o->encrypted){uint64_t next=seal_size(o->plain_before+o->written+o->used+n);if(!next||next-seal_size(o->plain_before)>o->available)return false;}
#endif
    if (n>sizeof(o->buffer)-o->used && !flush(o)) return false;
    if (n>sizeof(o->buffer)) return false;
    memcpy(o->buffer+o->used,p,n); o->used+=n; return true;
}
static void *run(void *unused) {
    (void)unused;
    for (;;) {
        if(removable)storage_tick();
        retention_tick();
        pthread_mutex_lock(&lock);
        while (!head && !stopping) {
            pthread_mutex_unlock(&lock);if(removable)storage_tick();retention_tick();if(recorder_available())recover_events();pthread_mutex_lock(&lock);
            if(!head&&!stopping){struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=100000000;if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}pthread_cond_timedwait(&ready,&lock,&until);}
        }
        if (!head) { pthread_mutex_unlock(&lock); break; }
        struct job *j=head; head=j->next; if (!head) tail=NULL;
        unsigned s=j->slot; struct record_info r=sessions[s];
        pthread_mutex_unlock(&lock);
        bool ok=true; const char *error=NULL; char a[1024],b[1024]; size_t written=0; uint64_t removed=0,opened_bytes=0;
        if (j->kind==OPEN) {
            ok=path(a,sizeof(a),r.id,"part");
            if (ok) { files[s]=store_open(a,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC); ok=files[s]>=0; owned[s]=ok; }
#if MYVAVA_SDK
            if (ok && r.transport_stream) {
                digests[s]=EVP_MD_CTX_new(); ok=digests[s] && EVP_DigestInit_ex(digests[s],EVP_sha256(),NULL)==1;
                memset(&muxes[s],0,sizeof(muxes[s]));
                if(ok&&r.encrypted){
                    pthread_mutex_lock(&lock);ok=disk_bytes<=quota_bytes&&quota_bytes-disk_bytes>=SEAL_HEADER;pthread_mutex_unlock(&lock);
                    if(ok){opened_bytes=SEAL_HEADER;sealed_writers[s]=seal_writer_open(files[s],storage_keys,r.id);ok=sealed_writers[s]!=NULL;}
                }
            }
#endif
        } else if (j->kind==DATA) {
            pthread_mutex_lock(&lock);
            ok=!sessions[s].failed && disk_bytes<quota_bytes;
            uint64_t available=ok?quota_bytes-disk_bytes:0;
            pthread_mutex_unlock(&lock);
            if (ok && r.transport_stream) {
                struct output o={.slot=s,.available=available,.plain_before=r.bytes,.encrypted=r.encrypted};
                ok=files[s]>=0 && ts_mux_frame(&muxes[s],j->audio,j->keyframe,j->timestamp,j->data,j->n,output,&o);
                if (ok) ok=flush(&o);
                written=o.written; error=muxes[s].error;
            } else if (ok) ok=j->n<=available && files[s]>=0 && write_count(files[s],j->data,j->n,&written);
        } else {
            ok=files[s]>=0 && !r.failed && r.frames>0;
#if MYVAVA_SDK
            if (ok && r.transport_stream) {
                unsigned length=0; ok=EVP_DigestFinal_ex(digests[s],r.sha256,&length)==1 && length==32;
            }
            EVP_MD_CTX_free(digests[s]); digests[s]=NULL;
            if(ok&&r.encrypted)ok=seal_writer_finish(sealed_writers[s]);
            seal_writer_free(sealed_writers[s]);sealed_writers[s]=NULL;
#endif
            if (files[s]>=0) {
                if (fsync(files[s])) ok=false;
                if (close(files[s])) ok=false;
                files[s]=-1;
            }
            (void)path(a,sizeof(a),r.id,"part"); (void)path(b,sizeof(b),r.id,extension(&r));
            bool renamed=false;
            if (ok && store_rename(a,b)) ok=false;
            else if (ok) renamed=true;
            if (ok && (!sync_directory() || !metadata(&r) || !sync_directory())) ok=false;
            if (!ok && renamed) {
                char meta[1024]; (void)path(meta,sizeof(meta),r.id,"meta");
                /* Withdraw the commit marker before removing the referenced blob. */
                if (!store_unlink(meta) || errno==ENOENT) removed+=remove_blob(b);
            }
            if (!ok && owned[s]) removed+=remove_blob(a);
            if(!ok&&r.encrypted){struct stat remaining;
                if(store_stat(a,&remaining)&&errno==ENOENT&&store_stat(b,&remaining)&&errno==ENOENT)removed=r.stored_bytes;
            }
            owned[s]=false;
            if (!ok) (void)sync_directory();
        }
        pthread_mutex_lock(&lock);
        queued_bytes-=j->n;
        if (!ok && !sessions[s].failed) failed(s,error?error:j->kind==FINISH ? "record_not_committed":"storage_write_failed_or_quota");
        if(j->kind==OPEN){sessions[s].stored_bytes+=opened_bytes;disk_bytes+=opened_bytes;}
        if (j->kind==DATA) {
            sessions[s].bytes+=written;uint64_t stored=storage_size(&sessions[s]);
            if(stored>=sessions[s].stored_bytes)disk_bytes+=stored-sessions[s].stored_bytes;
            sessions[s].stored_bytes=stored;if (ok) sessions[s].frames+=j->frame;
            if (r.transport_stream) {
                sessions[s].audio_frames=muxes[s].audio_frames;
                sessions[s].first_ms=muxes[s].first_ms; sessions[s].last_ms=muxes[s].last_ms;
                sessions[s].sample_rate=muxes[s].sample_rate; sessions[s].channels=muxes[s].channels;
            }
        }
        if (j->kind==FINISH) {
            memcpy(sessions[s].sha256,r.sha256,32);
            if (disk_bytes>=removed) disk_bytes-=removed;
        }
        struct record_info result=sessions[s];
        bool allocated=j->kind!=FINISH;
        pthread_mutex_unlock(&lock); if (allocated) free(j);
        else {
            pthread_mutex_lock(&catalog_lock);bool archived=!ok||relocate(&result);
            bool indexed=!ok||(archived&&record_index_add(&result));
            if(ok&&indexed)indexed=update_archive(&result);
            unsigned count=record_index_count();pthread_mutex_lock(&lock);
            memcpy(sessions[s].archive,result.archive,sizeof(result.archive));
            sessions[s].active=false;sessions[s].finishing=false;catalog_records=count;
            if(!indexed){catalog_healthy=false;strcpy(storage_error,"catalog_index_failed");failed(s,"catalog_index_failed");}
            result=sessions[s];pthread_mutex_unlock(&lock);pthread_mutex_unlock(&catalog_lock);
            if(complete&&!complete(&result,false,complete_arg)){recovery_before=0;recovery_next=monotonic_ms()+1000;}
        }
    }
    return NULL;
}
static bool load_entry(const char *name) {
    uint64_t id=0; int used=0;
    bool deleted=false;
    if(sscanf(name,"vrec-%16" SCNx64 ".meta%n",&id,&used)!=1||used!=26||name[used]){used=0;if(sscanf(name,"vrec-%16" SCNx64 ".deleted%n",&id,&used)!=1||used!=29||name[used])return true;deleted=true;}
    char mp[1024],vp[1024]; uint8_t b[513]; struct stat st;
    if (!path(mp,sizeof(mp),id,deleted?"deleted":"meta")) return false;
    if (store_stat(mp,&st) || !S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=geteuid()||(st.st_mode&0077)) return false;
    int meta_fd=store_open(mp,O_RDONLY|O_CLOEXEC|O_NONBLOCK);if(meta_fd<0)return false;FILE *f=fdopen(meta_fd,"rb");if(!f){close(meta_fd);return false;}
    size_t n=fread(b,1,sizeof(b),f); bool ok=!ferror(f); fclose(f);
    bool v6=n==512&&!memcmp(b,"VAVAREC6",8);
    bool v5=v6||(n==512&&!memcmp(b,"VAVAREC5",8));
    bool encrypted=(n==256&&!memcmp(b,"VAVAREC4",8))||(v5&&b[61]==1);
    bool v3=v5||(n==256 && (!memcmp(b,"VAVAREC3",8)||encrypted));
    bool ts=v5||(n==256 && (!memcmp(b,"VAVAREC2",8) || v3));
    if (!ok || (!ts && (n!=128 || memcmp(b,"VAVAREC1",8))) || vava_u32(b+8)!=vava_crc32(b+16,n-16) ||
        id!=get64(b+16) || b[56]>3 || b[57]>1 || b[58]>1 || record_index_count()==RECORD_CATALOG_MAX) return false;
    struct record_info r={.id=id,.generation=get64(b+24),.unix_seconds=get64(b+32),
        .bytes=get64(b+40),.frames=get64(b+48),.slot=b[56],.stream=b[57],.pir=b[58],.transport_stream=ts,.encrypted=encrypted};
    r.stored_bytes=storage_size(&r);
    if (ts) {
        if (b[59]!=1 || b[60]>1 || b[61]!=(encrypted?1:0) || b[62] || b[63] || !r.frames || !r.bytes || r.bytes%188) return false;
        r.clock_valid=b[60]; r.audio_frames=get64(b+128); r.first_ms=get64(b+136); r.last_ms=get64(b+144);
        r.sample_rate=vava_u32(b+152); r.channels=vava_u32(b+156); memcpy(r.sha256,b+160,32);
        if (r.last_ms<r.first_ms || r.last_ms-r.first_ms>180000 || r.channels>2 ||
            (!!r.audio_frames!=(r.sample_rate>0 && r.channels>0))) return false;
        if (v3) { memcpy(r.upload_target,b+192,32); memcpy(r.nas_target,b+224,32); }
        else for (unsigned k=192;k<256;++k) if (b[k]) return false;
    }
    if(!vava_serial(b+64,r.serial))return false;
    if(v5){if(!memchr(b+256,0,80)||(*((char *)b+256)&&!record_archive_valid((char *)b+256)))return false;strcpy(r.archive,(char *)b+256);
        if(*r.archive){
        const char *cam=r.archive+35,*end=strchr(cam,'/');if(!end||(size_t)(end-cam)!=strlen(r.serial)||strncmp(cam,r.serial,(size_t)(end-cam))||end[10]!=(char)('0'+r.slot))return false;}
        if(v6){r.file_type=b[336];r.detection_reported=b[337];r.detection_result=vava_u32(b+340);r.detection_type=vava_u32(b+344);
            if((r.file_type&&(r.file_type<5||r.file_type>8))||b[337]>1||b[338]||b[339]||(!r.detection_reported&&(r.file_type||r.detection_result||r.detection_type)))return false;}
        for(unsigned k=v6?348:336;k<512;++k)if(b[k])return false;}
    if(!blob_path(vp,sizeof(vp),&r))return false;
    if(deleted){
        if(store_unlink(vp)&&errno!=ENOENT)return false;
        /* A deletion interrupted after a migration intent can still own the flat source. */
        char flat[1024];path(flat,sizeof(flat),id,extension(&r));if(*r.archive&&store_unlink(flat)&&errno!=ENOENT)return false;
        bool synced=sync_name(vp);if(!synced&&errno!=ENOENT)return false;
        return !store_unlink(mp)&&!fsync(directory_fd);
    }
    if(*r.archive&&store_stat(vp,&st)){
        if(errno!=ENOENT)return false;char flat[1024];path(flat,sizeof(flat),id,extension(&r));
        int parent=record_archive_parent(directory_fd,r.archive,true);if(parent<0)return false;close(parent);
        if(store_stat(flat,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||(uint64_t)st.st_size!=storage_size(&r)||store_rename(flat,vp)||!sync_name(vp)||fsync(directory_fd))return false;
    }
    if(store_stat(vp,&st)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=geteuid()||(st.st_mode&0077)||(uint64_t)st.st_size!=storage_size(&r))return false;
    if(encrypted) {
#if MYVAVA_SDK
        int fd=store_open(vp,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct seal_reader *reader=fd>=0?seal_reader_open(fd,storage_keys,id,r.bytes):NULL;
        if(fd>=0)close(fd);if(!reader)return false;seal_reader_free(reader);
#else
        return false;
#endif
    }
    if (!vava_serial(b+64,r.serial)) return false;
    for (unsigned k=96;k<127 && b[k];++k)
        if (!((b[k]>='a' && b[k]<='z') || (b[k]>='0' && b[k]<='9') || b[k]=='_')) return false;
    memcpy(r.error,b+96,31);if(!relocate(&r)||!record_index_add(&r))return false;
    if (id>last_id) last_id=id;
    return true;
}
static bool attach_storage(void) {
    struct stat st;
    directory_fd=storage_path_open(directory,mount_guard,true);if(directory_fd<0)return false;
    if(!record_index_open_fd(directory_fd)){close(directory_fd);directory_fd=-1;return false;}disk_bytes=last_id=0;catalog_healthy=false;
    DIR *d=directory_scan_open(directory_fd); if (!d){record_index_close();close(directory_fd);directory_fd=-1;return false;}
    struct dirent *e; bool ok=true;
    for(unsigned pass=0;ok&&pass<3;++pass){rewinddir(d);
        while((e=directory_scan_next(d,&ok))){
            pthread_mutex_lock(&lock);bool stop=stopping;pthread_mutex_unlock(&lock);if(stop){ok=false;break;}
            const char *ext=strrchr(e->d_name,'.');
            if(pass<2){if(ext&&!strcmp(ext,pass==0?".deleted":".meta")&&!load_entry(e->d_name)){ok=false;break;}continue;}
            uint64_t id;int n=0;
            if(sscanf(e->d_name,"vrec-%16" SCNx64 ".%n",&id,&n)==1&&n==22&&
                (!strcmp(e->d_name+n,"h264")||!strcmp(e->d_name+n,"ts")||!strcmp(e->d_name+n,"sealed")||!strcmp(e->d_name+n,"part"))){
                char p[1024];if(path(p,sizeof(p),id,e->d_name+n)&&!store_stat(p,&st)&&S_ISREG(st.st_mode))disk_bytes+=(uint64_t)st.st_size;
                if(id>last_id)last_id=id;
            }
        }
    }
    uint64_t archive_bytes=0,removed=0;if(ok)ok=record_archive_reconcile(directory_fd,false,&archive_bytes,&removed);disk_bytes+=archive_bytes;
    closedir(d);
    record_index_sort();
    if (!ok){record_index_close();close(directory_fd);directory_fd=-1;disk_bytes=0;return false;}
    pthread_mutex_lock(&lock);catalog_healthy=true;catalog_records=record_index_count();++storage_generation;storage_error[0]=0;pthread_mutex_unlock(&lock);
    recovery_before=0;recovery_next=monotonic_ms()+1000;
    retention.after=0;retention.next=monotonic_ms()+10000;
    return true;
}
static void storage_tick(void){
    if(monotonic_ms()<storage_check)return;storage_check=monotonic_ms()+1000;
    pthread_mutex_lock(&lock);bool stop=stopping,healthy=catalog_healthy;pthread_mutex_unlock(&lock);if(stop)return;
    if(healthy&&storage_path_same(directory_fd,directory,mount_guard))return;
    pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"storage_unavailable_or_corrupt");
    bool active=false;
    for(unsigned s=0;s<4;++s)if(sessions[s].active){active=true;failed(s,"storage_detached");if(!sessions[s].finishing&&enqueue(FINISH,s,NULL,0,false))sessions[s].finishing=true;}
    bool pending=head!=NULL;pthread_mutex_unlock(&lock);
    if(active||pending)return;
    /* No new reader can acquire this index while unhealthy. Existing readers
     * keep their own descriptors and are invalidated by the generation check. */
    pthread_mutex_lock(&catalog_lock);record_index_close();if(directory_fd>=0)close(directory_fd);directory_fd=-1;
    (void)attach_storage();pthread_mutex_unlock(&catalog_lock);
}
static bool start_storage(const char *dir,uint64_t quota,const char *mount,bool dynamic){
    if(initialized||!storage_path_valid(dir)||!quota||(mount&&*mount&&(!storage_path_valid(mount)||strncmp(dir,mount,strlen(mount))||dir[strlen(mount)]!='/')))return false;
    strcpy(directory,dir);quota_bytes=quota;snprintf(mount_guard,sizeof(mount_guard),"%s",mount?mount:"");removable=dynamic;
    strcpy(storage_error,"storage_initializing");stopping=false;
    if(!dynamic&&!attach_storage())return false;
    initialized=true;if(pthread_create(&worker,NULL,run,NULL)){initialized=false;record_index_close();if(directory_fd>=0)close(directory_fd);directory_fd=-1;return false;}return true;
}
bool recorder_init(const char *dir,uint64_t quota){return start_storage(dir,quota,NULL,false);}
bool recorder_init_guarded(const char *dir,uint64_t quota,const char *mount){return start_storage(dir,quota,mount,true);}
bool recorder_start(unsigned s, unsigned stream, uint64_t generation, const char *serial, bool pir, bool clock_valid, const uint8_t *upload_target, const uint8_t *nas_target, uint64_t *id) {
    if (!initialized || s>=4 || stream>1 || strlen(serial)>=32) return false;
    pthread_mutex_lock(&lock);
    unsigned reserved=catalog_records; for (unsigned i=0;i<4;++i) reserved+=sessions[i].active;
    bool ok=!sessions[s].active && catalog_healthy && !maintenance && reserved<RECORD_CATALOG_MAX && disk_bytes<quota_bytes && last_id<UINT64_MAX;
    if (ok) {
        uint64_t next=(uint64_t)time(NULL)*1000000;
        if (next<=last_id) next=last_id+1;
        last_id=next;
        sessions[s]=(struct record_info){.id=next,.slot=s,.stream=stream,.generation=generation,
            .unix_seconds=(uint64_t)time(NULL),.pir=pir,.active=true,.transport_stream=MYVAVA_SDK,.clock_valid=clock_valid,.encrypted=recorder_encrypted()};
        strcpy(sessions[s].serial,serial);
        if (upload_target) memcpy(sessions[s].upload_target,upload_target,32);
        if (nas_target) memcpy(sessions[s].nas_target,nas_target,32);
        ok=enqueue(OPEN,s,NULL,0,false);
        if (ok) *id=next; else sessions[s].active=false;
    }
    pthread_mutex_unlock(&lock); return ok;
}
bool recorder_data(unsigned s, const void *data, size_t n, bool frame) {
    if (s>=4 || !initialized) return false;
    pthread_mutex_lock(&lock);
    bool ok=sessions[s].active && !sessions[s].finishing && !sessions[s].failed && !sessions[s].transport_stream;
    if (ok && !enqueue(DATA,s,data,n,frame)) { failed(s,"record_queue_full"); ok=false; }
    pthread_mutex_unlock(&lock); return ok;
}
bool recorder_frame(unsigned s, bool audio, bool keyframe, uint64_t timestamp, const void *data, size_t n,
                    const void *sps, size_t sn, const void *pps, size_t pn) {
    if (s>=4 || !initialized || !data || !n || n>VAVA_MAX_FRAME || sn>1024 || pn>1024 ||
        (sn && !sps) || (pn && !pps) || n+sn+pn>VAVA_MAX_FRAME) return false;
    pthread_mutex_lock(&lock);
    bool ok=sessions[s].active && !sessions[s].finishing && !sessions[s].failed && sessions[s].transport_stream;
    size_t total=n+sn+pn;
    if (ok) {
        struct job *j=queued_bytes+total<=4u*1024u*1024u?malloc(sizeof(*j)+total):NULL;
        if (!j) { failed(s,"record_queue_full"); ok=false; }
        else {
            *j=(struct job){.kind=DATA,.slot=s,.n=total,.frame=!audio,.audio=audio,.keyframe=keyframe,
                            .timestamp=timestamp,.data=(uint8_t *)(j+1)};
            if (sn) memcpy(j->data,sps,sn);
            if (pn) memcpy(j->data+sn,pps,pn);
            memcpy(j->data+sn+pn,data,n);
            if (tail) tail->next=j; else head=j;
            tail=j; queued_bytes+=total; pthread_cond_signal(&ready);
        }
    }
    pthread_mutex_unlock(&lock); return ok;
}
void recorder_finish(unsigned s, const char *reason) {
    if (s>=4 || !initialized) return;
    pthread_mutex_lock(&lock);
    if (sessions[s].active && !sessions[s].finishing) {
        if (!sessions[s].failed) snprintf(sessions[s].error,sizeof(sessions[s].error),"%s",reason);
        if (enqueue(FINISH,s,NULL,0,false)) sessions[s].finishing=true;
    }
    pthread_mutex_unlock(&lock);
}
void recorder_abort(unsigned s, const char *reason) {
    if (s>=4 || !initialized) return;
    pthread_mutex_lock(&lock);
    if (sessions[s].active && !sessions[s].failed) failed(s,reason);
    pthread_mutex_unlock(&lock);
    recorder_finish(s,reason);
}
void recorder_detection(unsigned s,uint64_t id,uint64_t generation,uint32_t result,uint32_t type) {
    if(s>=4||!initialized)return;
    pthread_mutex_lock(&lock);struct record_info *r=&sessions[s];
    if(r->active&&!r->finishing&&r->pir&&r->id==id&&r->generation==generation){
        r->detection_reported=true;r->detection_result=result;r->detection_type=type;
        /* This firmware's cmd 200 namespace differs from App fileType.
         * Stock consumer 0x450fe0 -> 0x448a68: raw 1 => movement; raw 5
         * is emitted by the camera IVP human path at 0x4000787c. Other
         * values remain unknown until their sender is verified. */
        if(result==1){if(type==5)r->file_type=6;else if(type==1&&!r->file_type)r->file_type=5;}
    }
    pthread_mutex_unlock(&lock);
}
struct record_info recorder_state(unsigned s) {
    struct record_info r={0}; if (s>=4 || !initialized) return r;
    pthread_mutex_lock(&lock); r=sessions[s]; pthread_mutex_unlock(&lock); return r;
}
void recorder_shutdown(void) {
    if (!initialized) return;
    for (unsigned s=0;s<4;++s) recorder_finish(s,"service_shutdown");
    pthread_mutex_lock(&lock); stopping=true; pthread_cond_signal(&ready); pthread_mutex_unlock(&lock);
    pthread_join(worker,NULL);pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);initialized=false;catalog_healthy=false;pthread_mutex_unlock(&lock);record_index_close();close(directory_fd);directory_fd=-1;pthread_mutex_unlock(&catalog_lock);if(retention_dir>=0)close(retention_dir);retention_dir=-1;
#if MYVAVA_SDK
    seal_keys_free(storage_keys);storage_keys=NULL;keyring_path[0]=0;
#endif
}
static int record_json(const struct record_info *r,char *out,size_t size){
    char hash[65]={0};
    if (r->transport_stream) for (unsigned k=0;k<32;++k) (void)snprintf(hash+k*2,3,"%02x",r->sha256[k]);
    int n=snprintf(out,size,"{\"id\":\"%016" PRIx64 "\",\"unix_seconds\":%" PRIu64 ",\"bytes\":%" PRIu64
        ",\"frames\":%" PRIu64 ",\"stream\":%u,\"trigger\":\"%s\",\"format\":\"%s\",\"end_reason\":\"%s\","
        "\"audio_frames\":%" PRIu64 ",\"sample_rate\":%u,\"channels\":%u,\"first_timestamp_ms\":%" PRIu64
        ",\"last_timestamp_ms\":%" PRIu64 ",\"clock_valid\":%s,\"sha256\":\"%s\",\"encrypted\":%s,\"stored_bytes\":%" PRIu64 ",\"archive_path\":\"%s\",\"serial\":\"%s\",\"slot\":%u,\"storage\":\"tf\",\"duration_ms\":%" PRIu64 ",\"file_type\":%u,\"detection_reported\":%s,\"detection_result_raw\":%u,\"detection_type_raw\":%u}",
        r->id,r->unix_seconds,r->bytes,r->frames,r->stream,r->pir?"pir_first_connect":"manual",
        recorder_format(r),r->error,r->audio_frames,r->sample_rate,r->channels,r->first_ms,r->last_ms,r->clock_valid?"true":"false",hash,r->encrypted?"true":"false",r->stored_bytes,r->archive,r->serial,r->slot,r->last_ms>=r->first_ms?r->last_ms-r->first_ms:0,r->file_type,r->detection_reported?"true":"false",r->detection_result,r->detection_type);
    return n;
}
bool recorder_identity(unsigned slot,const char *serial){
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);bool ready=catalog_healthy&&!stopping;pthread_mutex_unlock(&lock);
    bool found=ready&&record_index_identity(slot,serial);pthread_mutex_unlock(&catalog_lock);return found;
}
bool recorder_cameras(unsigned page,char *out,size_t size){
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);bool ok=catalog_healthy&&!stopping&&generation_matches();uint64_t gen=storage_generation;pthread_mutex_unlock(&lock);
    struct record_owner owners[16];unsigned total=0,n=ok?record_index_owners(page,owners,16,&total):0;size_t used=0;
    int k=snprintf(out,size,"{\"ok\":true,\"storage_generation\":%" PRIu64 ",\"page\":%u,\"total\":%u,\"cameras\":[",gen,page,total);
    ok=ok&&k>0&&(size_t)k<size;if(ok)used=(size_t)k;
    for(unsigned i=0;ok&&i<n;++i){k=snprintf(out+used,size-used,"%s{\"slot\":%u,\"serial\":\"%s\",\"records\":%u}",i?",":"",owners[i].slot,owners[i].serial,owners[i].records);ok=k>0&&(size_t)k<size-used;if(ok)used+=(size_t)k;}
    if(ok)ok=size-used>=4&&snprintf(out+used,size-used,"]}\n")>0;
    pthread_mutex_unlock(&catalog_lock);return ok;
}
bool recorder_list(unsigned slot, const char *serial, unsigned page, char *out, size_t size) {
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);
    int n=snprintf(out,size,"{\"ok\":true,\"storage_bytes\":%" PRIu64 ",\"quota_bytes\":%" PRIu64 ",\"records\":[",disk_bytes,quota_bytes);
    bool ok=n>=0 && (size_t)n<size && catalog_healthy && generation_matches(); size_t used=ok?(size_t)n:0; unsigned index=0, emitted=0;
    pthread_mutex_unlock(&lock);
    struct record_info batch[16];unsigned found=ok?record_index_page(slot,serial,page,batch,16,&index):0;if(found==UINT32_MAX){ok=false;found=0;pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"catalog_read_failed");pthread_mutex_unlock(&lock);}
    for (unsigned i=0;ok && i<found;++i) {
        const struct record_info *r=&batch[i];
        if(emitted){if(used+1>=size){ok=false;break;}out[used++]=',';}
        n=record_json(r,out+used,size-used);
        ok=n>=0 && (size_t)n<size-used; if (ok) used+=(size_t)n;
        ++emitted;
    }
    if (ok) { n=snprintf(out+used,size-used,"],\"page\":%u,\"total\":%u,\"storage_generation\":%" PRIu64 "}\n",page,index,storage_generation); ok=n>=0 && (size_t)n<size-used; }
    pthread_mutex_unlock(&catalog_lock); return ok;
}
static const char *query(unsigned slot,const char *serial,uint64_t before,uint64_t generation,uint64_t from,uint64_t to,char *out,size_t size){
    if((serial&&slot>=4)||(from&&!to)||(to&&to<=from)||(before&&!generation))return "invalid_record_query";
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);
    const char *error=!catalog_healthy||stopping||!generation_matches()?"storage_changed_or_unavailable":generation&&generation!=storage_generation?"storage_generation_conflict":NULL;
    uint64_t current=storage_generation;pthread_mutex_unlock(&lock);
    struct record_info batch[16];unsigned total=0,n=0;bool more=false;
    if(!error){n=record_index_query(slot,serial,before,from,to,batch,16,&total,&more);if(n==UINT32_MAX){error="storage_catalog_unavailable";pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"catalog_read_failed");pthread_mutex_unlock(&lock);}}
    size_t used=0;int k=0;
    if(!error){k=snprintf(out,size,"{\"ok\":true,\"storage\":\"tf\",\"storage_generation\":%" PRIu64 ",\"total\":%u,\"has_more\":%s,\"next_before\":\"%016" PRIx64 "\",\"records\":[",current,total,more?"true":"false",more&&n?batch[n-1].id:0);
        if(k<0||(size_t)k>=size)error="record_response_limit";else used=(size_t)k;
        for(unsigned i=0;!error&&i<n;++i){if(i){if(used+1>=size){error="record_response_limit";break;}out[used++]=',';}k=record_json(&batch[i],out+used,size-used);if(k<0||(size_t)k>=size-used)error="record_response_limit";else used+=(size_t)k;}
        if(!error&&(snprintf(out+used,size-used,"]}\n")<0||size-used<4))error="record_response_limit";
    }
    pthread_mutex_unlock(&catalog_lock);return error;
}
const char *recorder_query(unsigned slot,const char *serial,uint64_t before,uint64_t generation,uint64_t from,uint64_t to,char *out,size_t size){
    if(!serial||!*serial)return "invalid_record_query";
    return query(slot,serial,before,generation,from,to,out,size);
}
const char *recorder_query_all(uint64_t before,uint64_t generation,uint64_t from,uint64_t to,char *out,size_t size){return query(0,NULL,before,generation,from,to,out,size);}
bool recorder_info_json(unsigned slot,const char *serial,uint64_t id,char *out,size_t size){
    if(slot>=4)return false;
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);bool ok=catalog_healthy&&!stopping&&generation_matches();uint64_t generation=storage_generation;struct record_info current=sessions[slot];pthread_mutex_unlock(&lock);
    struct record_info r={0};const char *state="missing";bool present=false;
    if(ok){int found=record_index_lookup(slot,serial,id,&r);
        if(found<0){pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"catalog_read_failed");pthread_mutex_unlock(&lock);pthread_mutex_unlock(&catalog_lock);return false;}
        if(found){state="committed";present=true;}
        else if(current.id==id&&!strcmp(current.serial,serial)&&(current.active||current.failed)){r=current;present=true;state=r.failed?"failed":r.finishing?"finishing":"recording";}
        int n=snprintf(out,size,"{\"ok\":true,\"id\":\"%016" PRIx64 "\",\"state\":\"%s\",\"serial\":\"%s\",\"slot\":%u,\"storage\":\"tf\",\"storage_generation\":%" PRIu64 ",\"record\":",id,state,serial,slot,generation);
        ok=n>=0&&(size_t)n<size;size_t used=ok?(size_t)n:0;
        if(ok){n=present?record_json(&r,out+used,size-used):snprintf(out+used,size-used,"null");ok=n>=0&&(size_t)n<size-used;if(ok)used+=(size_t)n;}
        if(ok)ok=snprintf(out+used,size-used,"}\n")>=0&&size-used>=3;
    }
    pthread_mutex_unlock(&catalog_lock);return ok;
}
bool recorder_path(unsigned slot, const char *serial, uint64_t id, char *out, size_t size) {
    pthread_mutex_lock(&catalog_lock);bool ok=false;struct record_info r;
    if(recorder_available()&&record_index_get(slot,serial,id,&r))ok=blob_path(out,size,&r);
    pthread_mutex_unlock(&catalog_lock);return ok;
}
bool recorder_reader_open(unsigned slot,const char *serial,uint64_t id,struct record_reader *out){
    struct record_reader r={.fd=-1};char filename[1024];int parent=-1;
    pthread_mutex_lock(&catalog_lock);
    bool found=recorder_available()&&record_index_get(slot,serial,id,&r.info)&&blob_path(filename,sizeof(filename),&r.info);
    pthread_mutex_lock(&lock);unsigned k=0;while(k<4&&readers[k].token)++k;
    found=found&&k<4&&reader_sequence<UINT64_MAX&&catalog_healthy&&!stopping&&generation_matches();
    if(found){r.token=++reader_sequence;r.storage_generation=storage_generation;readers[k].token=r.token;readers[k].id=id;parent=dup(directory_fd);}
    pthread_mutex_unlock(&lock);pthread_mutex_unlock(&catalog_lock);if(!found)return false;
    int media_parent=parent>=0?record_archive_parent(parent,relative(filename),false):-1;
    r.fd=media_parent>=0?openat(media_parent,leaf(filename),O_RDONLY|O_NOFOLLOW|O_CLOEXEC):-1;if(media_parent>=0)close(media_parent);if(parent>=0)close(parent);struct stat st;
    if(r.fd<0||fstat(r.fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)||st.st_nlink!=1||(uint64_t)st.st_size!=storage_size(&r.info)){recorder_reader_close(&r);return false;}
#if MYVAVA_SDK
    if(r.info.encrypted){r.sealed=seal_reader_open(r.fd,storage_keys,id,r.info.bytes);if(!r.sealed){recorder_reader_close(&r);return false;}}
#endif
    r.deadline_ms=monotonic_ms()+60000;*out=r;return true;
}
int recorder_reader_read(struct record_reader *r, uint64_t offset, void *out, size_t n) {
    if (!r->token || r->fd<0 || !out || !n || n>65536 || offset>r->info.bytes ||
        offset>INT64_MAX || monotonic_ms()>=r->deadline_ms || recorder_storage_generation()!=r->storage_generation) { errno=EINVAL; return -1; }
    if (n>r->info.bytes-offset) n=(size_t)(r->info.bytes-offset);
    if (!n) return 0;
#if MYVAVA_SDK
    if(r->sealed)return seal_reader_read(r->sealed,offset,out,n);
#endif
    ssize_t got;
    do { got=pread(r->fd,out,n,(off_t)offset); } while (got<0 && errno==EINTR);
    if (got==0) { errno=EIO; return -1; }
    return (int)got;
}
void recorder_reader_close(struct record_reader *r) {
    if (!r->token) return;
#if MYVAVA_SDK
    seal_reader_free(r->sealed);r->sealed=NULL;
#endif
    if (r->fd>=0) close(r->fd);
    pthread_mutex_lock(&lock);
    for (unsigned i=0;i<4;++i) if (readers[i].token==r->token) readers[i].token=readers[i].id=0;
    pthread_mutex_unlock(&lock);
    memset(r,0,sizeof(*r)); r->fd=-1;
}
static const char *delete_record(unsigned slot, const char *serial, uint64_t id) {
    pthread_mutex_lock(&lock);bool ok=false;
    for (unsigned i=0;i<4;++i) if (readers[i].token && readers[i].id==id) { pthread_mutex_unlock(&lock);return "record_in_use"; }
    if(sessions[slot].id==id&&sessions[slot].active){pthread_mutex_unlock(&lock);return "recording_active";}
    bool available=catalog_healthy&&!stopping&&generation_matches();pthread_mutex_unlock(&lock);
    if(!available)return "storage_changed_or_unavailable";
    struct record_info r;int found=record_index_lookup(slot,serial,id,&r);
    if(found<0){pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"catalog_read_failed");pthread_mutex_unlock(&lock);return "storage_catalog_unavailable";}
    if(!found)return "record_not_found";
    {
        char a[1024],b[1024],tomb[1024]; path(a,sizeof(a),id,"meta"); blob_path(b,sizeof(b),&r);
        path(tomb,sizeof(tomb),id,"deleted");
        /* A tombstone prevents an interrupted deletion leaving a live catalog
         * entry whose video is missing. Restore metadata if video removal fails. */
        if (!store_rename(a,tomb)) {
            if(!sync_directory()){(void)store_rename(tomb,a);(void)sync_directory();return "record_delete_io_failed";}
            if (!store_unlink(b) || errno==ENOENT) {
                pthread_mutex_lock(&lock);if(disk_bytes>=r.stored_bytes)disk_bytes-=r.stored_bytes;pthread_mutex_unlock(&lock);
                (void)record_index_remove(id); bool durable=sync_name(b);if(durable)(void)store_unlink(tomb);char marker[1024];if(path(marker,sizeof(marker),id,"evtmark"))(void)store_unlink(marker);if(path(marker,sizeof(marker),id,"hold"))(void)store_unlink(marker);ok=durable&&update_archive(&r);
                if(!durable){pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"archive_delete_sync_failed");pthread_mutex_unlock(&lock);}
            } else if (store_rename(tomb,a)) {
                (void)record_index_remove(id); /* orphan is still charged to quota */
            }
            if(!sync_directory()){ok=false;pthread_mutex_lock(&lock);catalog_healthy=false;strcpy(storage_error,"archive_delete_sync_failed");pthread_mutex_unlock(&lock);}
        }
    }
    unsigned count=record_index_count();pthread_mutex_lock(&lock);catalog_records=count;bool healthy=catalog_healthy;pthread_mutex_unlock(&lock);return ok?NULL:healthy?"record_delete_io_failed":"archive_index_failed";
}
const char *recorder_delete_result(unsigned slot,const char *serial,uint64_t id){if(slot>=4)return "invalid_camera_slot";pthread_mutex_lock(&catalog_lock);const char *error=delete_record(slot,serial,id);pthread_mutex_unlock(&catalog_lock);return error;}
bool recorder_delete(unsigned slot,const char *serial,uint64_t id){return recorder_delete_result(slot,serial,id)==NULL;}
bool recorder_preview(unsigned slot,const char *serial,uint64_t id,struct record_info *prefix,struct record_info *source){
#if MYVAVA_SDK
    struct record_reader reader={.fd=-1};
    if(!prefix||!source||!recorder_reader_open(slot,serial,id,&reader))return false;
    *source=reader.info;bool ok=source->transport_stream,seen=false,done=false;
    EVP_MD_CTX *hash=EVP_MD_CTX_new();ok=ok&&hash&&EVP_DigestInit_ex(hash,EVP_sha256(),NULL)==1;
    uint8_t data[188*320];uint64_t offset=0,limit=source->bytes;
    if(limit>1048476)limit=1048476; /* <= 1 MiB, aligned to TS packets. */
    while(ok&&!done&&offset<limit){size_t n=(size_t)(limit-offset);if(n>sizeof(data))n=sizeof(data);
        ok=recorder_reader_read(&reader,offset,data,n)==(int)n;if(!ok)break;
        size_t consumed=0;
        for(;consumed<n;consumed+=188){const uint8_t *p=data+consumed;
            if(p[0]!=0x47||(p[1]&0x80)){ok=false;break;}
            unsigned pid=((p[1]&31u)<<8)|p[2];
            if(pid==0x100&&(p[1]&0x40)){
                if(seen){done=true;break;}
                if(!(p[3]&0x20)||p[4]<1||!(p[5]&0x40)){ok=false;break;}
                seen=true;
            }
        }
        if(ok&&consumed)ok=EVP_DigestUpdate(hash,data,consumed)==1;
        offset+=consumed;
    }
    ok=ok&&seen&&(done||offset==source->bytes)&&offset>0;
    if(ok){*prefix=*source;prefix->bytes=offset;prefix->frames=1;prefix->audio_frames=0;prefix->sample_rate=prefix->channels=0;
        prefix->last_ms=prefix->first_ms;prefix->preview=true;unsigned n=0;ok=EVP_DigestFinal_ex(hash,prefix->sha256,&n)==1&&n==32;}
    EVP_MD_CTX_free(hash);recorder_reader_close(&reader);return ok;
#else
    (void)slot;(void)serial;(void)id;(void)prefix;(void)source;return false;
#endif
}
bool recorder_cleanup(uint64_t *removed_bytes) {
    *removed_bytes=0;
    pthread_mutex_lock(&catalog_lock);pthread_mutex_lock(&lock);
    if(!catalog_healthy||stopping||!generation_matches()){pthread_mutex_unlock(&lock);pthread_mutex_unlock(&catalog_lock);return false;}
    for (unsigned i=0;i<4;++i) if (sessions[i].active) { pthread_mutex_unlock(&lock);pthread_mutex_unlock(&catalog_lock);return false; }
    maintenance=true;pthread_mutex_unlock(&lock);
    DIR *d=directory_fd>=0?directory_scan_open(directory_fd):NULL; bool ok=d!=NULL; struct dirent *e;if(d)rewinddir(d);
    while (d && (e=directory_scan_next(d,&ok))) {
        uint64_t id; int used=0;
        if (sscanf(e->d_name,"vrec-%16" SCNx64 ".%n",&id,&used)!=1 || used!=22) continue;
        const char *ext=e->d_name+used;
        bool blob=!strcmp(ext,"part") || !strcmp(ext,"h264") || !strcmp(ext,"ts") || !strcmp(ext,"sealed");
        if (!blob && strcmp(ext,"meta-part") && strcmp(ext,"archive-part") && strcmp(ext,"deleted") && strcmp(ext,"evtmark") && strcmp(ext,"evtmark.tmp") && strcmp(ext,"hold") && strcmp(ext,"hold.tmp")) continue;
        bool live=record_index_has(id);
        if (live) continue;
        char p[1024]; struct stat st;
        if (!path(p,sizeof(p),id,ext) || store_stat(p,&st) || !S_ISREG(st.st_mode) || store_unlink(p)) { ok=false; continue; }
        if (blob) {
            *removed_bytes+=(uint64_t)st.st_size;
            pthread_mutex_lock(&lock);disk_bytes=disk_bytes>=(uint64_t)st.st_size?disk_bytes-(uint64_t)st.st_size:0;pthread_mutex_unlock(&lock);
        }
    }
    if (d) closedir(d);
    uint64_t archive_bytes,archive_removed;if(!rebuild_archive(true,&archive_bytes,&archive_removed))ok=false;
    *removed_bytes+=archive_removed;pthread_mutex_lock(&lock);disk_bytes=disk_bytes>=archive_removed?disk_bytes-archive_removed:0;pthread_mutex_unlock(&lock);
    (void)sync_directory();
    pthread_mutex_lock(&lock);maintenance=false;pthread_mutex_unlock(&lock);pthread_mutex_unlock(&catalog_lock);return ok;
}

static bool private_regular(int fd){struct stat st;return !fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1;}
static bool retention_save(unsigned revision,bool enabled,unsigned days,unsigned pending){
    uint8_t b[64]={0};memcpy(b,"MVKEEP01",8);vava_put32(b+8,revision);b[12]=enabled;vava_put32(b+16,days);vava_put32(b+20,pending);vava_put32(b+60,vava_crc32(b,60));
    int fd=openat(retention_dir,"retention.tmp",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0)return false;
    bool ok=private_regular(fd)&&!ftruncate(fd,0)&&write_all(fd,b,sizeof(b))&&!fsync(fd);if(close(fd))ok=false;
    return ok&&!renameat(retention_dir,"retention.tmp",retention_dir,"retention.bin")&&!fsync(retention_dir);
}
bool recorder_retention_init(const char *state_dir,bool enabled){
    retention.enabled=enabled;retention.revision=1;retention.days=30;retention.pending_hours=72;
    retention_dir=open(state_dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(retention_dir<0)return false;
    int fd=openat(retention_dir,"retention.bin",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd<0){if(errno==ENOENT)return true;retention.healthy=false;return true;}
    uint8_t b[65];ssize_t n=private_regular(fd)?read(fd,b,sizeof(b)):-1;close(fd);
    retention.healthy=n==64&&!memcmp(b,"MVKEEP01",8)&&vava_crc32(b,60)==vava_u32(b+60)&&vava_u32(b+8)&&b[12]<=1&&vava_u32(b+16)<=3650&&vava_u32(b+20)>=1&&vava_u32(b+20)<=720;
    if(retention.healthy){retention.revision=vava_u32(b+8);retention.enabled=b[12];retention.days=vava_u32(b+16);retention.pending_hours=vava_u32(b+20);}else retention.enabled=false;
    return true;
}
void recorder_clock(bool valid){pthread_mutex_lock(&lock);retention.clock=valid;pthread_mutex_unlock(&lock);}
bool recorder_retention_json(char *out,size_t size){pthread_mutex_lock(&lock);
    int n=snprintf(out,size,"{\"ok\":true,\"revision\":%u,\"enabled\":%s,\"healthy\":%s,\"max_days\":%u,\"pending_hours\":%u,\"high_water_percent\":90,\"low_water_percent\":80,\"removed_since_boot\":%" PRIu64 ",\"removed_bytes_since_boot\":%" PRIu64 ",\"protected_observations\":%" PRIu64 ",\"clock_valid\":%s,\"error\":\"%s\"}\n",retention.revision,retention.enabled?"true":"false",retention.healthy?"true":"false",retention.days,retention.pending_hours,retention.deleted,retention.bytes,retention.protected,retention.clock?"true":"false",retention.healthy?"":"retention_configuration_corrupt_or_write_failed");
    pthread_mutex_unlock(&lock);return n>=0&&(size_t)n<size;
}
const char *recorder_retention_config(unsigned revision,bool enabled,unsigned days,unsigned pending){
    if(days>3650||pending<1||pending>720)return "invalid_retention_policy";
    pthread_mutex_lock(&lock);const char *error=!retention.healthy?"retention_configuration_corrupt":revision!=retention.revision?"retention_revision_conflict":revision==UINT32_MAX?"retention_revision_exhausted":NULL;pthread_mutex_unlock(&lock);if(error)return error;
    bool ok=retention_save(revision+1,enabled,days,pending);
    pthread_mutex_lock(&lock);if(ok){retention.revision=revision+1;retention.enabled=enabled;retention.days=days;retention.pending_hours=pending;}else{retention.healthy=false;retention.enabled=false;}pthread_mutex_unlock(&lock);
    return ok?NULL:"retention_configuration_write_failed";
}
/* A persistent, bounded export hold survives reboot. It is written before the
 * export intent, so failure can only overprotect a source for at most one hour. */
bool recorder_hold(unsigned slot,const char *serial,uint64_t id,uint64_t until){
    pthread_mutex_lock(&catalog_lock);struct record_info r;bool ok=recorder_available()&&record_index_get(slot,serial,id,&r);char name[1024],tmp[1024];
    if(ok){uint8_t b[64]={0};memcpy(b,"MVHOLD01",8);put64(b+8,id);put64(b+16,until);memcpy(b+24,r.sha256,32);vava_put32(b+60,vava_crc32(b,60));
        path(name,sizeof(name),id,"hold");path(tmp,sizeof(tmp),id,"hold.tmp");int fd=store_open(tmp,O_WRONLY|O_CREAT|O_CLOEXEC|O_NONBLOCK);
        ok=fd>=0&&private_regular(fd)&&!ftruncate(fd,0)&&write_all(fd,b,sizeof(b))&&!fsync(fd);if(fd>=0&&close(fd))ok=false;if(ok)ok=!store_rename(tmp,name)&&sync_directory();
    }pthread_mutex_unlock(&catalog_lock);return ok;
}
static bool held(const struct record_info *r,uint64_t now){char name[1024];path(name,sizeof(name),r->id,"hold");int fd=store_open(name,O_RDONLY|O_CLOEXEC|O_NONBLOCK);if(fd<0)return errno!=ENOENT;
    uint8_t b[65];ssize_t n=private_regular(fd)?read(fd,b,sizeof(b)):-1;close(fd);
    if(n!=64||memcmp(b,"MVHOLD01",8)||get64(b+8)!=r->id||memcmp(b+24,r->sha256,32)||vava_u32(b+60)!=vava_crc32(b,60))return true;
    return now<get64(b+16);
}
#if MYVAVA_SDK
static bool target_complete(const struct record_info *r,const char *backend,const uint8_t target[32]){
    uint8_t any=0;for(unsigned i=0;i<32;++i)any|=target[i];if(!any)return true;
    int dir=openat(directory_fd,backend,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return false;
    char name[40];snprintf(name,sizeof(name),"%016" PRIx64 ".done",r->id);int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);close(dir);if(fd<0)return false;
    uint8_t b[161];ssize_t n=private_regular(fd)?read(fd,b,sizeof(b)):-1;close(fd);struct myvava_file f;
    return n==160&&!memcmp(b,target,32)&&myvava_file_decode(b+32,128,&f)&&f.id==r->id&&f.slot==r->slot&&!strcmp(f.serial,r->serial)&&f.size==r->bytes&&!memcmp(f.sha256,r->sha256,32);
}
#endif
static void retention_tick(void){
    uint64_t ms=monotonic_ms();if(ms<retention.next)return;
    retention.next=ms+100;
    pthread_mutex_lock(&lock);bool enabled=retention.enabled&&retention.healthy&&catalog_healthy&&!stopping&&generation_matches(),clock=retention.clock;
    unsigned days=retention.days,hours=retention.pending_hours;(void)hours;uint64_t used=disk_bytes,quota=quota_bytes;unsigned count=catalog_records;pthread_mutex_unlock(&lock);
    if(!enabled)return;
    static bool pressure;pressure=used>=quota/10*9||count>=RECORD_CATALOG_MAX-16||(pressure&&used>quota/10*8);
    pthread_mutex_lock(&catalog_lock);struct record_info rows[16];unsigned n=recorder_available()?record_index_after(retention.after,rows,16):0;
    if(n==UINT32_MAX){pthread_mutex_unlock(&catalog_lock);retention.next=ms+10000;return;}
    uint64_t now=(uint64_t)time(NULL);unsigned removed=0;
    for(unsigned i=0;i<n;++i){struct record_info *r=&rows[i];retention.after=r->id;
        bool age=clock&&r->clock_valid&&now>=r->unix_seconds,expired=age&&days&&now-r->unix_seconds>=(uint64_t)days*86400;
        if(!pressure&&!expired)continue;
        bool protected=held(r,now);
#if MYVAVA_SDK
        bool pending=!target_complete(r,".remote",r->upload_target)||!target_complete(r,".nas",r->nas_target);
        if(pending&&!(age&&now-r->unix_seconds>=(uint64_t)hours*3600))protected=true;
#endif
        if(protected){pthread_mutex_lock(&lock);++retention.protected;pthread_mutex_unlock(&lock);continue;}
        if(!delete_record(r->slot,r->serial,r->id)){++removed;pthread_mutex_lock(&lock);++retention.deleted;retention.bytes+=r->stored_bytes;used=disk_bytes;count=catalog_records;pthread_mutex_unlock(&lock);
            pressure=used>quota/10*8||count>=RECORD_CATALOG_MAX-16;if(removed>=4)break;}
    }
    pthread_mutex_unlock(&catalog_lock);
    if(n<16&&removed<4){retention.after=0;retention.next=ms+1000;}
}
