#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#undef NDEBUG
#include <assert.h>

static unsigned closes,spawn_failures;
static bool pending_open;
static int test_spawn(pid_t *pid,const char *path,const posix_spawn_file_actions_t *actions,
                      const posix_spawnattr_t *attr,char *const argv[],char *const env[]){
    (void)path;(void)actions;(void)attr;(void)env;pending_open=!strcmp(argv[1],"open");
    if(!pending_open){++closes;if(spawn_failures){--spawn_failures;return EAGAIN;}}
    *pid=1234;return 0;
}
static pid_t test_waitpid(pid_t pid,int *status,int options){
    (void)options;*status=(!pending_open&&closes<=3)?1<<8:0;return pid;
}
static ssize_t test_write(int fd,const void *p,size_t n){(void)fd;(void)p;return (ssize_t)n;}
#define posix_spawn test_spawn
#define waitpid test_waitpid
#define write test_write
#include "../vava-station/src/platform.c"
#undef posix_spawn
#undef waitpid
#undef write

int main(void){
    char path[]="/tmp/vava-ap-test-XXXXXX";int fd=mkstemp(path);assert(fd>=0);assert(!fchmod(fd,0700));close(fd);
    assert(platform_init(path));uint64_t now=1000;
    assert(platform_pair_open("pair","password",120,now));platform_tick(++now);assert(!strcmp(platform_state(),"open"));
    platform_pair_close(now);assert(closes==1);
    const unsigned delay[]={1000,2000,4000};
    for(unsigned i=0;i<3;++i){
        platform_tick(++now);assert(!strcmp(platform_state(),"close_failed"));
        assert(!platform_pair_until());assert(!platform_pair_open("pair","password",120,now));
        platform_tick(now+delay[i]-1);assert(closes==i+1);
        now+=delay[i];platform_tick(now);assert(closes==i+2);
    }
    platform_tick(++now);assert(!strcmp(platform_state(),"closed"));
    /* Spawn failures also continue beyond three attempts, with a 30 s cap. */
    spawn_failures=7;unsigned before=closes;platform_pair_close(now);assert(closes==before+1);
    const unsigned spawn_delay[]={1000,2000,4000,8000,16000,30000,30000};
    for(unsigned i=0;i<7;++i){
        platform_tick(now+spawn_delay[i]-1);assert(closes==before+i+1);
        now+=spawn_delay[i];platform_tick(now);assert(closes==before+i+2);
    }
    platform_tick(++now);assert(!strcmp(platform_state(),"closed"));
    assert(platform_pair_open("pair","password",1,now));platform_tick(++now);
    now=platform_pair_until();assert(now);platform_tick(now);assert(!strcmp(platform_state(),"closing"));
    platform_tick(++now);assert(!strcmp(platform_state(),"closed"));
    assert(!unlink(path));puts("pairing AP retries survive helper and spawn failures with bounded backoff");return 0;
}
