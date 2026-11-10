#include "nas_control.h"
#include "myvava.h"
#include "protocol.h"
#include <errno.h>
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
extern char **environ;
struct nas_control {
    pthread_t worker;pthread_mutex_t lock;pthread_cond_t wake;
    int state;bool stop,enabled,ready,retry,active,healthy;unsigned leases;
    uint32_t revision,applied;uint64_t total,available,checked,retry_at;
    char host[254],export_path[512],mount[512],station[64],executable[1024];
    bool lab;char phase[32],error[48];uint8_t target[32];
};
static bool fingerprint(struct nas_control *c){char b[1600];int n=snprintf(b,sizeof(b),"myvava-nas-v1\n%s\n%s\n%s\n%s\n%d",c->host,c->export_path,c->mount,c->station,c->lab);unsigned length=0;return n>0&&n<(int)sizeof(b)&&EVP_Digest(b,(size_t)n,c->target,&length,EVP_sha256(),NULL)==1&&length==32;}
static bool private_regular(int fd){struct stat st;return !fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1;}
static bool save(struct nas_control *c,uint32_t revision,bool enabled,const char *host,const char *export_path){
    uint8_t b[1024]={0};memcpy(b,"MVNAS001",8);vava_put32(b+8,revision);b[12]=enabled;memcpy(b+16,host,strlen(host));memcpy(b+272,export_path,strlen(export_path));vava_put32(b+1020,vava_crc32(b,1020));
    int fd=openat(c->state,"nas-config.tmp",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0)return false;
    bool ok=private_regular(fd)&&!ftruncate(fd,0);size_t at=0;while(ok&&at<sizeof(b)){ssize_t n=write(fd,b+at,sizeof(b)-at);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else at+=(size_t)n;}
    if(ok)ok=!fsync(fd);if(close(fd))ok=false;return ok&&!renameat(c->state,"nas-config.tmp",c->state,"nas-config.bin")&&!fsync(c->state);
}
static bool load(struct nas_control *c){
    int fd=openat(c->state,"nas-config.bin",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return errno==ENOENT;
    uint8_t b[1025];ssize_t n=private_regular(fd)?read(fd,b,sizeof(b)):-1;close(fd);
    if(n!=1024||memcmp(b,"MVNAS001",8)||vava_u32(b+1020)!=vava_crc32(b,1020)||b[12]>1||!memchr(b+16,0,254)||!memchr(b+272,0,512))return false;
    struct nas_config cfg={(char *)b+16,(char *)b+272,c->mount,c->station,c->executable,c->lab};
    if(!vava_u32(b+8)||!nas_config_valid(&cfg))return false;
    c->revision=vava_u32(b+8);c->enabled=b[12];strcpy(c->host,cfg.host);strcpy(c->export_path,cfg.export_path);return true;
}
/* All NFS I/O runs in one tracked child process. It can be killed on a
 * revision change; an uninterruptible child is never replaced by another. */
static int operation(struct nas_control *c,const struct nas_lease *lease,const char *action,uint64_t capacity[2]){
    int pipefd[2];if(pipe(pipefd))return 12;(void)fcntl(pipefd[0],F_SETFD,FD_CLOEXEC);(void)fcntl(pipefd[1],F_SETFD,FD_CLOEXEC);(void)fcntl(pipefd[0],F_SETFL,O_NONBLOCK);
    char *args[]={c->executable,"--nas-admin-worker",(char *)lease->config.host,(char *)lease->config.export_path,c->mount,c->station,c->lab?"lab-local":"nfs",(char *)action,NULL};
    posix_spawn_file_actions_t actions;posix_spawnattr_t attr;
    if(posix_spawn_file_actions_init(&actions)){close(pipefd[0]);close(pipefd[1]);return 12;}
    int rc=posix_spawnattr_init(&attr);bool has_attr=!rc;
    if(!rc)rc=posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);if(!rc)rc=posix_spawnattr_setpgroup(&attr,0);
    if(!rc&&pipefd[0]!=3)rc=posix_spawn_file_actions_addclose(&actions,pipefd[0]);
    if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,pipefd[1],3);
    pid_t child=0;if(!rc)rc=posix_spawn(&child,c->executable,&actions,&attr,args,environ);
    posix_spawn_file_actions_destroy(&actions);if(has_attr)posix_spawnattr_destroy(&attr);close(pipefd[1]);
    if(rc){close(pipefd[0]);return 12;}
    uint64_t start=myvava_millis();bool killed=false;int status=0;
    for(;;){pid_t got=waitpid(child,&status,WNOHANG);if(got==child)break;if(got<0&&errno!=EINTR){status=13<<8;break;}
        pthread_mutex_lock(&c->lock);bool stop=c->stop,changed=c->revision!=lease->revision;pthread_mutex_unlock(&c->lock);
        if(stop||changed||myvava_millis()-start>45000){if(!killed){(void)kill(-child,SIGKILL);killed=true;}if(stop){close(pipefd[0]);return 14;}}
        struct timespec pause={.tv_nsec=20000000};nanosleep(&pause,NULL);
    }
    int result=killed?15:WIFEXITED(status)?WEXITSTATUS(status):13;
    if(!result&&!strcmp(action,"probe")&&read(pipefd[0],capacity,2*sizeof(*capacity))!=(ssize_t)(2*sizeof(*capacity)))result=26;
    close(pipefd[0]);return result;
}
static void snapshot(struct nas_control *c,struct nas_lease *lease){strcpy(lease->host,c->host);strcpy(lease->export_path,c->export_path);lease->revision=c->revision;lease->config=(struct nas_config){lease->host,lease->export_path,c->mount,c->station,c->executable,c->lab};}
static void *worker(void *arg){struct nas_control *c=arg;pthread_mutex_lock(&c->lock);
    while(!c->stop){
        uint64_t now=myvava_millis();bool needed=c->revision!=c->applied||c->retry||((c->enabled||c->error[0])&&now>=c->retry_at);
        if(c->healthy&&needed&&!c->leases){struct nas_lease lease;snapshot(c,&lease);bool enable=c->enabled;c->active=true;c->ready=false;c->retry=false;strcpy(c->phase,enable?"mounting":"unmounting");pthread_mutex_unlock(&c->lock);
            uint64_t capacity[2]={0};int result=operation(c,&lease,enable?"prepare":"unmount",capacity);if(!result&&enable)result=operation(c,&lease,"probe",capacity);
            pthread_mutex_lock(&c->lock);c->active=false;
            if(c->revision==lease.revision){c->applied=lease.revision;c->checked=(uint64_t)time(NULL);c->retry_at=myvava_millis()+30000;c->ready=enable&&!result;c->total=capacity[0];c->available=capacity[1];
                strcpy(c->phase,result?"retry_wait":enable?"ready":"disabled");snprintf(c->error,sizeof(c->error),"%s",result==0?"":result==15?"operation_cancelled_or_timed_out":result==21?"nfs_mount_unavailable_or_mismatched":result==22?"nfs_unwritable_or_unsafe":result==26?"nfs_probe_failed":"nfs_operation_failed");
                if(result&&!enable)c->retry=true;
            }
            /* Retry failures with a deadline, not a busy respawn loop. */
            if(c->retry&&c->revision==lease.revision){c->retry=false;c->applied=lease.revision;}
        }
        struct timespec until;clock_gettime(CLOCK_REALTIME,&until);++until.tv_sec;if(!c->stop)pthread_cond_timedwait(&c->wake,&c->lock,&until);
    }pthread_mutex_unlock(&c->lock);return NULL;
}
struct nas_control *nas_control_start(const char *state_dir,const struct nas_config *cfg){
    struct nas_control *c=calloc(1,sizeof(*c));if(!c)return NULL;c->state=-1;
    struct nas_config seed=*cfg;if(!seed.host)seed.host="unconfigured";if(!seed.export_path)seed.export_path="/";
    if(!nas_config_valid(&seed)||strlen(seed.executable)>=sizeof(c->executable))goto fail;
    strcpy(c->host,seed.host);strcpy(c->export_path,seed.export_path);strcpy(c->mount,seed.mount_dir);strcpy(c->station,seed.station_id);strcpy(c->executable,seed.executable);c->lab=seed.lab_local;c->enabled=cfg->host&&cfg->export_path;c->revision=1;
    c->state=open(state_dir,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(c->state<0)goto fail;
    c->healthy=load(c)&&fingerprint(c);if(!c->healthy){c->enabled=false;strcpy(c->error,"nas_configuration_corrupt");}
    strcpy(c->phase,c->healthy?"queued":"failed");if(pthread_mutex_init(&c->lock,NULL))goto fail;
    if(pthread_cond_init(&c->wake,NULL)){pthread_mutex_destroy(&c->lock);goto fail;}
    if(pthread_create(&c->worker,NULL,worker,c)){pthread_cond_destroy(&c->wake);pthread_mutex_destroy(&c->lock);goto fail;}return c;
fail:if(c->state>=0)close(c->state);free(c);return NULL;
}
void nas_control_stop(struct nas_control *c){if(!c)return;pthread_mutex_lock(&c->lock);c->stop=true;pthread_cond_signal(&c->wake);pthread_mutex_unlock(&c->lock);pthread_join(c->worker,NULL);pthread_cond_destroy(&c->wake);pthread_mutex_destroy(&c->lock);close(c->state);free(c);}
bool nas_control_target(struct nas_control *c,uint8_t out[32]){if(!c)return false;pthread_mutex_lock(&c->lock);bool ok=c->healthy&&c->enabled;memcpy(out,c->target,32);pthread_mutex_unlock(&c->lock);return ok;}
bool nas_control_acquire(struct nas_control *c,struct nas_lease *lease){pthread_mutex_lock(&c->lock);bool ok=c->healthy&&c->ready&&c->enabled&&!c->active&&c->revision==c->applied&&!c->stop;if(ok){snapshot(c,lease);++c->leases;}pthread_mutex_unlock(&c->lock);return ok;}
bool nas_control_valid(struct nas_control *c,const struct nas_lease *lease){pthread_mutex_lock(&c->lock);bool ok=c->healthy&&c->enabled&&c->revision==lease->revision&&!c->stop;pthread_mutex_unlock(&c->lock);return ok;}
void nas_control_release(struct nas_control *c){pthread_mutex_lock(&c->lock);if(c->leases)--c->leases;pthread_cond_signal(&c->wake);pthread_mutex_unlock(&c->lock);}
const char *nas_control_configure(struct nas_control *c,uint32_t revision,bool enabled,const char *host,const char *export_path){
    struct nas_config candidate={host,export_path,c->mount,c->station,c->executable,c->lab};if(!nas_config_valid(&candidate))return "invalid_nas_configuration";
    pthread_mutex_lock(&c->lock);const char *error=NULL;bool changed=strcmp(host,c->host)||strcmp(export_path,c->export_path);
    if(!c->healthy)error="nas_configuration_corrupt";
    else if(revision!=c->revision)error="nas_revision_conflict";
    else if(changed&&(c->enabled||c->active||c->leases||strcmp(c->phase,"disabled")))error="disable_and_unmount_nas_before_target_change";
    else if(changed||enabled!=c->enabled){
        if(c->revision==UINT32_MAX)error="nas_revision_exhausted";
        else if(!save(c,c->revision+1,enabled,host,export_path)){c->healthy=false;c->ready=false;strcpy(c->phase,"failed");strcpy(c->error,"nas_configuration_write_failed");error="nas_configuration_write_failed";}
        else {++c->revision;c->enabled=enabled;c->ready=false;strcpy(c->host,host);strcpy(c->export_path,export_path);if(!fingerprint(c))error="nas_target_hash_failed";strcpy(c->phase,"queued");pthread_cond_signal(&c->wake);}
    }
    pthread_mutex_unlock(&c->lock);return error;
}
const char *nas_control_action(struct nas_control *c,uint32_t revision,const char *action){
    if(!strcmp(action,"unmount")||!strcmp(action,"cancel")){char host[254],path[512];pthread_mutex_lock(&c->lock);strcpy(host,c->host);strcpy(path,c->export_path);pthread_mutex_unlock(&c->lock);return nas_control_configure(c,revision,false,host,path);}
    if(strcmp(action,"retry")&&strcmp(action,"probe"))return "invalid_nas_action";
    pthread_mutex_lock(&c->lock);const char *error=!c->healthy?"nas_configuration_corrupt":revision!=c->revision?"nas_revision_conflict":c->active?"nas_operation_in_progress":NULL;
    if(!error){c->retry=true;pthread_cond_signal(&c->wake);}pthread_mutex_unlock(&c->lock);return error;
}
bool nas_control_json(struct nas_control *c,char *out,size_t size){if(!c)return false;pthread_mutex_lock(&c->lock);
    int n=snprintf(out,size,"{\"ok\":true,\"revision\":%u,\"applied_revision\":%u,\"enabled\":%s,\"ready\":%s,\"host\":\"%s\",\"export\":\"%s\",\"phase\":\"%s\",\"error\":\"%s\",\"total_bytes\":%" PRIu64 ",\"available_bytes\":%" PRIu64 ",\"checked_unix_seconds\":%" PRIu64 ",\"active_copies\":%u,\"format\":\"mpegts-h264-aac\",\"archive_formats\":[\"ts\",\"mp4\"]}",c->revision,c->applied,c->enabled?"true":"false",c->ready?"true":"false",!strcmp(c->host,"unconfigured")?"":c->host,c->export_path,c->phase,c->error,c->total,c->available,c->checked,c->leases);
    pthread_mutex_unlock(&c->lock);return n>=0&&(size_t)n<size;
}
