#include "directory_scan.h"
#include "events.h"
#include "myvava.h"
#include "storage_path.h"
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

#define DISK_SIZE (MYVAVA_EVENT_SIZE+4)
struct event_entry { uint8_t data[MYVAVA_EVENT_SIZE]; };
struct myvava_events {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool stop, head_acked;
    int directory, lock;
    char *path,*mount;
    uint8_t origin[16];
    uint64_t sequence, gap_reported;
    struct myvava_event_status status;
    struct event_entry pending[MYVAVA_EVENT_QUEUE], *saved;
    unsigned head;
    myvava_event_durable_fn durable;void *durable_arg;
};
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t c=UINT32_MAX;
    while (n--) { c^=*p++; for (unsigned b=0;b<8;++b) c=(c>>1)^(0xedb88320u & (0u-(c&1))); }
    return ~c;
}
static void filename(const uint8_t *p, char name[53]) {
    myvava_hex(p+4,24,name); memcpy(name+48,".evt",5);
}
static bool file_name(const char *name, bool temporary) {
    size_t n=temporary?56:52; uint8_t bytes[24]; char hex[49];
    if (strlen(name)!=n || strcmp(name+48,temporary?".evt.tmp":".evt")) return false;
    memcpy(hex,name,48); hex[48]=0; return myvava_unhex(hex,bytes,24);
}
static bool private_regular(int fd) {
    struct stat st;
    return !fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&0077) && st.st_nlink==1;
}
static bool read_entry(int dir, const char *name, uint8_t *out) {
    int fd=openat(dir,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return false;
    uint8_t bytes[DISK_SIZE+1]; ssize_t n=private_regular(fd)?read(fd,bytes,sizeof(bytes)):-1;
    close(fd);
    if (n!=DISK_SIZE) return false;
    uint32_t sum=crc(bytes,MYVAVA_EVENT_SIZE);
    for (unsigned i=0;i<4;++i) if (bytes[MYVAVA_EVENT_SIZE+i]!=(uint8_t)(sum>>(24-8*i))) return false;
    struct myvava_event e; char expected[53]; filename(bytes,expected);
    if (strcmp(name,expected) || !myvava_event_decode(bytes,MYVAVA_EVENT_SIZE,&e)) return false;
    memcpy(out,bytes,MYVAVA_EVENT_SIZE); return true;
}
static int compare_entry(const void *a, const void *b) { return memcmp((const uint8_t *)a+4,(const uint8_t *)b+4,24); }
static const char *load(struct myvava_events *s) {
    s->directory=storage_path_open(s->path,s->mount,true);
    struct stat st;
    if (s->directory<0 || fstat(s->directory,&st) || !S_ISDIR(st.st_mode) ||
        st.st_uid!=geteuid() || (st.st_mode&0077)) return "event_directory_unavailable_or_unsafe";
    s->lock=openat(s->directory,".lock",O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    if (s->lock<0 || !private_regular(s->lock) || fcntl(s->lock,F_SETLK,&lock)) return "event_directory_locked_or_unsafe";
    DIR *dir=directory_scan_open(s->directory);
    if (!dir) return "event_directory_read_failed";
    const char *error=NULL; unsigned count=0; struct dirent *entry;
    errno=0;
    while ((entry=readdir(dir))) {
        const char *name=entry->d_name;
        if (!strcmp(name,".") || !strcmp(name,"..") || !strcmp(name,".lock")) continue;
        if (file_name(name,true)) {
            int fd=openat(s->directory,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
            bool safe=fd>=0 && private_regular(fd); if (fd>=0) close(fd);
            if (!safe || unlinkat(s->directory,name,0)) { error="event_temporary_file_unsafe"; break; }
        } else if (!file_name(name,false) || count==s->status.limit ||
                   !read_entry(s->directory,name,s->saved[count].data)) {
            error="event_spool_corrupt_or_over_limit"; break;
        } else ++count;
        errno=0;
    }
    if (!error && errno) error="event_directory_read_failed";
    closedir(dir);
    if (!error && fsync(s->directory)) error="event_directory_sync_failed";
    if (!error) {
        qsort(s->saved,count,sizeof(*s->saved),compare_entry);
        pthread_mutex_lock(&s->mutex); s->status.retained=count; s->status.ready=true; pthread_mutex_unlock(&s->mutex);
    }
    return error;
}
static bool write_entry(struct myvava_events *s, const uint8_t *p) {
    char name[53],temp[57]; filename(p,name); snprintf(temp,sizeof(temp),"%s.tmp",name);
    /* A previous attempt may have renamed successfully but failed directory
     * fsync. Validate and finish that exact record, never overwrite it. */
    struct stat st;
    if (!fstatat(s->directory,name,&st,AT_SYMLINK_NOFOLLOW)) {
        uint8_t old[MYVAVA_EVENT_SIZE];
        return read_entry(s->directory,name,old) && !memcmp(old,p,sizeof(old)) && !fsync(s->directory);
    }
    if (errno!=ENOENT) return false;
    uint8_t bytes[DISK_SIZE]; memcpy(bytes,p,MYVAVA_EVENT_SIZE);
    uint32_t sum=crc(p,MYVAVA_EVENT_SIZE);
    for (unsigned i=0;i<4;++i) bytes[MYVAVA_EVENT_SIZE+i]=(uint8_t)(sum>>(24-8*i));
    int fd=openat(s->directory,temp,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) return false;
    size_t at=0; bool ok=true;
    while (at<sizeof(bytes)) {
        ssize_t n=write(fd,bytes+at,sizeof(bytes)-at);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { ok=false; break; }
        at+=(size_t)n;
    }
    if (ok && fsync(fd)) ok=false;
    if (close(fd)) ok=false;
    if (ok && renameat(s->directory,temp,s->directory,name)) ok=false;
    if (!ok) { (void)unlinkat(s->directory,temp,0); return false; }
    return !fsync(s->directory);
}
static void queue_locked(struct myvava_events *s, const struct myvava_event *input) {
    struct myvava_event e=*input; memcpy(e.origin,s->origin,16);
    e.sequence=++s->sequence; e.dropped=s->status.dropped;
    unsigned at=(s->head+s->status.queued)%MYVAVA_EVENT_QUEUE;
    myvava_event_encode(s->pending[at].data,&e); ++s->status.queued;
}
static void *worker(void *arg) {
    struct myvava_events *s=arg; const char *error="event_storage_initializing";uint64_t next_check=0;
    pthread_mutex_lock(&s->mutex);
    if (error) strcpy(s->status.error,error);
    for (;;) {
        if(!s->stop&&myvava_millis()>=next_check){
            next_check=myvava_millis()+1000;
            pthread_mutex_unlock(&s->mutex);
            bool same=!error&&storage_path_same(s->directory,s->path,s->mount);
            if(!same){
                pthread_mutex_lock(&s->mutex);s->status.ready=false;s->status.retained=0;s->head_acked=false;pthread_mutex_unlock(&s->mutex);
                if(s->lock>=0)close(s->lock);if(s->directory>=0)close(s->directory);s->lock=s->directory=-1;
                error=load(s);
            }
            pthread_mutex_lock(&s->mutex);snprintf(s->status.error,sizeof(s->status.error),"%s",error?error:"");
        }
        if (s->stop && (error || !s->status.queued || s->status.retained==s->status.limit || *s->status.error)) break;
        if (!error && s->gap_reported<s->status.dropped && s->status.queued<MYVAVA_EVENT_QUEUE && s->sequence<INT64_MAX) {
            struct myvava_event e={.slot=255,.kind=MVE_GAP,.monotonic_ms=myvava_millis(),.unix_ms=(uint64_t)time(NULL)*1000};
            strcpy(e.detail,"queue_overflow"); queue_locked(s,&e); s->gap_reported=s->status.dropped;
        }
        bool acked=!error && s->head_acked && s->status.retained;
        bool writing=!error && !acked && s->status.queued && s->status.retained<s->status.limit;
        if (acked || writing) {
            uint8_t data[MYVAVA_EVENT_SIZE];
            memcpy(data,acked?s->saved[0].data:s->pending[s->head].data,sizeof(data));
            pthread_mutex_unlock(&s->mutex);
            bool ok;
            if (acked) {
                char name[53]; filename(data,name);
                ok=(!unlinkat(s->directory,name,0) || errno==ENOENT) && !fsync(s->directory);
            } else ok=write_entry(s,data);
            pthread_mutex_lock(&s->mutex);
            if (ok) {
                s->status.error[0]=0;
                if (acked) {
                    --s->status.retained;
                    memmove(s->saved,s->saved+1,s->status.retained*sizeof(*s->saved));
                    s->head_acked=false; ++s->status.acknowledged;
                } else {
                    memcpy(s->saved[s->status.retained++].data,data,sizeof(data));
                    s->head=(s->head+1)%MYVAVA_EVENT_QUEUE; --s->status.queued; ++s->status.committed;
                    myvava_event_durable_fn callback=s->durable;void *callback_arg=s->durable_arg;
                    if(callback){struct myvava_event event;bool valid=myvava_event_decode(data,sizeof(data),&event);
                        pthread_mutex_unlock(&s->mutex);if(valid)callback(&event,callback_arg);pthread_mutex_lock(&s->mutex);}
                }
                continue;
            }
            error=acked?"event_ack_cleanup_failed":"event_write_failed";
            strcpy(s->status.error,error);s->status.ready=false;
        }
        if (s->stop) break;
        struct timespec until; clock_gettime(CLOCK_REALTIME,&until); ++until.tv_sec;
        pthread_cond_timedwait(&s->changed,&s->mutex,&until);
    }
    pthread_mutex_unlock(&s->mutex);
    if (s->lock>=0) close(s->lock);
    if (s->directory>=0) close(s->directory);
    return NULL;
}
struct myvava_events *myvava_events_start_guarded(const char *path,unsigned limit,const char *mount) {
    if (!storage_path_valid(path) || limit<16 || limit>4096 || (mount&&*mount&&(!storage_path_valid(mount)||strncmp(path,mount,strlen(mount))||path[strlen(mount)]!='/'))) return NULL;
    struct myvava_events *s=calloc(1,sizeof(*s)); if (!s) return NULL;
    s->directory=s->lock=-1; s->status.enabled=true; s->status.limit=limit;
    s->path=strdup(path);s->mount=strdup(mount?mount:"");s->saved=calloc(limit,sizeof(*s->saved));
    if (!s->path || !s->mount || !s->saved || !myvava_random(s->origin,16)) goto failed;
    if (pthread_mutex_init(&s->mutex,NULL)) goto failed;
    if (pthread_cond_init(&s->changed,NULL)) { pthread_mutex_destroy(&s->mutex); goto failed; }
    if (pthread_create(&s->thread,NULL,worker,s)) {
        pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->mutex); goto failed;
    }
    return s;
failed:
    free(s->path);free(s->mount);free(s->saved); free(s); return NULL;
}
struct myvava_events *myvava_events_start(const char *path,unsigned limit){return myvava_events_start_guarded(path,limit,NULL);}
bool myvava_events_emit(struct myvava_events *s, const struct myvava_event *e) {
    if (!s) return false;
    pthread_mutex_lock(&s->mutex);
    bool ok=!s->stop && s->sequence<INT64_MAX;
    if (ok && s->status.queued==MYVAVA_EVENT_QUEUE) {
        ++s->sequence; ++s->status.dropped; ok=false;
    } else if (ok) queue_locked(s,e);
    pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->mutex); return ok;
}
void myvava_events_on_durable(struct myvava_events *s,myvava_event_durable_fn fn,void *arg){if(!s)return;pthread_mutex_lock(&s->mutex);s->durable=fn;s->durable_arg=arg;pthread_mutex_unlock(&s->mutex);}
bool myvava_events_peek(struct myvava_events *s, uint8_t out[MYVAVA_EVENT_SIZE]) {
    if (!s) return false;
    pthread_mutex_lock(&s->mutex);
    bool ok=s->status.ready && s->status.retained && !s->head_acked;
    if (ok) memcpy(out,s->saved[0].data,MYVAVA_EVENT_SIZE);
    pthread_mutex_unlock(&s->mutex); return ok;
}
void myvava_events_ack(struct myvava_events *s, const uint8_t origin[16], uint64_t seq) {
    if (!s) return;
    pthread_mutex_lock(&s->mutex);
    struct myvava_event e;
    if (s->status.ready && s->status.retained && myvava_event_decode(s->saved[0].data,MYVAVA_EVENT_SIZE,&e) &&
        e.sequence==seq && !memcmp(e.origin,origin,16)) {
        s->head_acked=true; pthread_cond_signal(&s->changed);
    }
    pthread_mutex_unlock(&s->mutex);
}
struct myvava_event_status myvava_events_status(struct myvava_events *s) {
    struct myvava_event_status result={0}; if (!s) return result;
    pthread_mutex_lock(&s->mutex); result=s->status; pthread_mutex_unlock(&s->mutex); return result;
}
void myvava_events_stop(struct myvava_events *s) {
    if (!s) return;
    pthread_mutex_lock(&s->mutex); s->stop=true; pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->mutex);
    pthread_join(s->thread,NULL); pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->mutex);
    free(s->path);free(s->mount);free(s->saved); free(s);
}
