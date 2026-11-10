#ifndef VAVA_DIRECTORY_SCAN_H
#define VAVA_DIRECTORY_SCAN_H
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <unistd.h>

/* dup() shares the directory offset. Give each scan its own open description,
 * and retain ownership of that descriptor if fdopendir() cannot allocate. */
static inline DIR *directory_scan_open(int directory){
    int fd=openat(directory,".",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(fd<0)return NULL;
    DIR *scan=fdopendir(fd);
    if(!scan){int error=errno;close(fd);errno=error;}
    return scan;
}
/* An I/O error must not turn a partially scanned archive into a valid catalog,
 * or make a nonempty mountpoint appear safe to mount over. */
static inline struct dirent *directory_scan_next(DIR *scan,bool *ok){
    if(!*ok)return NULL;
    errno=0;struct dirent *entry=readdir(scan);
    if(!entry&&errno)*ok=false;
    return entry;
}
#endif
