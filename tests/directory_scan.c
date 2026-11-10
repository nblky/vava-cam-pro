#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#undef NDEBUG
#include <assert.h>

static bool fail_open,fail_read;
static int scanned_fd=-1;
static DIR *test_fdopendir(int fd){
    scanned_fd=fd;
    if(fail_open){errno=ENOMEM;return NULL;}
    return fdopendir(fd);
}
static struct dirent *test_readdir(DIR *dir){
    if(fail_read){errno=EIO;return NULL;}
    return readdir(dir);
}
#define fdopendir test_fdopendir
#define readdir test_readdir
#include "directory_scan.h"
#undef fdopendir
#undef readdir

static unsigned count(DIR *scan){
    bool ok=true;unsigned n=0;struct dirent *e;
    while((e=directory_scan_next(scan,&ok)))if(e->d_name[0]!='.')++n;
    assert(ok);return n;
}
int main(void){
    char path[]="/tmp/vava-scan-XXXXXX";assert(mkdtemp(path));
    int dir=open(path,O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(dir>=0);
    int file=openat(dir,"clip",O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(file>=0);close(file);
    DIR *a=directory_scan_open(dir),*b=directory_scan_open(dir);assert(a&&b);
    assert(count(a)==1&&count(b)==1);closedir(a);closedir(b);
    a=directory_scan_open(dir);assert(a&&count(a)==1);closedir(a);
    fail_open=true;assert(!directory_scan_open(dir)&&errno==ENOMEM);
    assert(fcntl(scanned_fd,F_GETFD)==-1&&errno==EBADF);
    fail_open=false;a=directory_scan_open(dir);assert(a);
    fail_read=true;bool ok=true;assert(!directory_scan_next(a,&ok)&&!ok&&errno==EIO);closedir(a);
    assert(!directory_scan_open(-1));
    assert(!unlinkat(dir,"clip",0));close(dir);assert(!rmdir(path));
    puts("directory offsets, allocation cleanup and read errors passed");return 0;
}
