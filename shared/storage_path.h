#ifndef VAVA_STORAGE_PATH_H
#define VAVA_STORAGE_PATH_H
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef __linux__
#include <mntent.h>
#endif
/* Paths are configuration, never shell fragments. Walk every component with
 * O_NOFOLLOW and create only the final directory, relative to its open parent. */
static inline bool storage_path_valid(const char *path) {
    if(!path||path[0]!='/'||!path[1]||strlen(path)>=768)return false;
    const char *p=path+1;
    while(*p){const char *end=strchr(p,'/');size_t n=end?(size_t)(end-p):strlen(p);
        if(!n||(n==1&&*p=='.')||(n==2&&!memcmp(p,"..",2)))return false;
        if(!end)return true;p=end+1;if(!*p)return false;}
    return false;
}
static inline bool storage_mount_present(const char *mount) {
    if(!mount||!*mount)return true;
#ifdef __linux__
    FILE *f=setmntent("/proc/self/mounts","r");if(!f)return false;
    bool found=false;struct mntent *m;
    while((m=getmntent(f)))if(!strcmp(m->mnt_dir,mount)){found=true;break;}
    endmntent(f);return found;
#else
    return false; /* Production mount guards require Linux's mount table. */
#endif
}
static inline int storage_path_open(const char *path,const char *mount,bool create) {
    if(!storage_path_valid(path)){errno=EINVAL;return -1;}
    /* Unguarded mode is for host development (macOS /tmp is a symlink).
     * Deployment supplies a mount guard and uses the strict walk below. */
    if(!mount||!*mount){
        if(create&&mkdir(path,0700)&&errno!=EEXIST)return -1;
        int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
        if(fd>=0&&(fstat(fd,&st)||st.st_uid!=geteuid()||(st.st_mode&0077))){close(fd);errno=EACCES;return -1;}return fd;
    }
    if(mount&&*mount){size_t n=strlen(mount);if(!storage_path_valid(mount)||strncmp(path,mount,n)||path[n]!='/'){errno=EINVAL;return -1;}
        if(!storage_mount_present(mount)){errno=ENODEV;return -1;}}
    int dir=open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC);if(dir<0)return -1;
    char copy[768];strcpy(copy,path+1);char *part=copy;dev_t medium=0;bool anchored=false;
    for(;;){char *next=strchr(part,'/');if(next)*next=0;
        if(!next&&create&&mkdirat(dir,part,0700)&&errno!=EEXIST){close(dir);return -1;}
        int child=openat(dir,part,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);close(dir);if(child<0)return -1;dir=child;
        struct stat st;if(fstat(dir,&st)){close(dir);return -1;}
        size_t prefix=(size_t)(part-copy)+strlen(part)+1;
        if(mount&&*mount&&prefix==strlen(mount)){medium=st.st_dev;anchored=true;}
        if(anchored&&st.st_dev!=medium){close(dir);errno=EXDEV;return -1;}
        if(!next){if(st.st_uid!=geteuid()||(st.st_mode&0077)){close(dir);errno=EACCES;return -1;}return dir;}
        part=next+1;
    }
}
static inline bool storage_path_same(int fd,const char *path,const char *mount) {
    if(fd<0)return false;
    int current=storage_path_open(path,mount,false);struct stat a,b;
    bool same=current>=0&&!fstat(fd,&a)&&!fstat(current,&b)&&a.st_dev==b.st_dev&&a.st_ino==b.st_ino;
    if(current>=0)close(current);return same;
}
#endif
