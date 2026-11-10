/* Test-only syscall interposition. No fault-injection hooks in the daemon. */
#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static int delayed_fsync(int fd){
    const char *marker=getenv("MYVAVA_TEST_BLOCK_FSYNC"),*directory=getenv("MYVAVA_TEST_BLOCK_DIRECTORY");char path[1024]={0};
#ifdef __APPLE__
    (void)fcntl(fd,F_GETPATH,path);
#else
    char link[64];snprintf(link,sizeof(link),"/proc/self/fd/%d",fd);ssize_t n=readlink(link,path,sizeof(path)-1);if(n>0)path[n]=0;
#endif
    if(marker&&directory&&!strcmp(path,directory)&&!access(marker,F_OK)){
        char entered[1024];snprintf(entered,sizeof(entered),"%s.entered",marker);int signal=open(entered,O_WRONLY|O_CREAT,0600);if(signal>=0)close(signal);
        for(unsigned i=0;i<500&&!access(marker,F_OK);++i){struct timespec pause={.tv_nsec=20000000};nanosleep(&pause,NULL);}
    }
#ifdef __APPLE__
    /* dyld excludes references in the interposing image itself. RTLD_NEXT
     * may resolve the replacement again on modern macOS and recurse. */
    return fsync(fd);
#else
    int (*original)(int)=NULL;void *symbol=dlsym(RTLD_NEXT,"fsync");memcpy(&original,&symbol,sizeof(original));return original?original(fd):-1;
#endif
}
#ifdef __APPLE__
__attribute__((used)) static const struct {int (*replacement)(int);int (*original)(int);} interpose
    __attribute__((section("__DATA,__interpose")))={delayed_fsync,fsync};
#else
int fsync(int fd){return delayed_fsync(fd);}
#endif
