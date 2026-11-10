#include "directory_scan.h"
#include "record_archive.h"
#include "record_index.h"
#include "protocol.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static char station[26];
static int offset;
static bool component(const char *s,size_t min,size_t max,bool hyphen){
    size_t n=strlen(s);if(n<min||n>max)return false;
    for(size_t i=0;i<n;++i)if(!((s[i]>='0'&&s[i]<='9')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')||(hyphen&&s[i]=='-')))return false;
    return true;
}
static bool date_name(const char *s){if(strlen(s)!=8)return false;for(unsigned i=0;i<8;++i)if(s[i]<'0'||s[i]>'9')return false;return true;}
static bool clip_name(const char *s){
    if(strlen(s)!=10||s[6]!='_'||s[8]!='_'||s[9]<'0'||s[9]>'3'||s[7]<'A'||s[7]>'Z')return false;
    for(unsigned i=0;i<6;++i)if(s[i]<'0'||s[i]>'9')return false;
    return s[0]<='2'&&(s[0]!='2'||s[1]<='3')&&s[2]<='5'&&s[4]<='5';
}
bool record_archive_config(const char *serial,int seconds){
    if(serial&&*serial&&!component(serial,25,25,false))return false;
    snprintf(station,sizeof(station),"%s",serial?serial:"");offset=seconds;return true;
}
bool record_archive_valid(const char *relative){
    if(!relative||strlen(relative)>=80)return false;char b[80];strcpy(b,relative);char *p=b,*part[4];
    for(unsigned i=0;i<4;++i){part[i]=p;char *slash=strchr(p,'/');if(i<3){if(!slash)return false;*slash=0;p=slash+1;}else if(slash)return false;}
    return component(part[0],25,25,false)&&date_name(part[1])&&component(part[2],1,31,true)&&clip_name(part[3]);
}
static int child(int parent,const char *name,bool create,dev_t device){
    bool made=false;if(create){if(!mkdirat(parent,name,0700))made=true;else if(errno!=EEXIST)return -1;}
    int fd=openat(parent,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd>=0&&(fstat(fd,&st)||st.st_dev!=device||st.st_uid!=geteuid()||(st.st_mode&0077)||(made&&fsync(parent)))){close(fd);errno=EACCES;return -1;}return fd;
}
int record_archive_parent(int root,const char *relative,bool create){
    if(!relative||!*relative||relative[0]=='/'||strlen(relative)>=1024){errno=EINVAL;return -1;}
    char copy[1024];strcpy(copy,relative);struct stat st;if(fstat(root,&st))return -1;int fd=dup(root);if(fd<0)return -1;
    char *p=copy,*slash;while((slash=strchr(p,'/'))){*slash=0;if(!component(p,1,31,true)){close(fd);errno=EINVAL;return -1;}
        int next=child(fd,p,create,st.st_dev);int error=errno;close(fd);if(next<0){errno=error;return -1;}fd=next;p=slash+1;}
    if(!*p||!strcmp(p,".")||!strcmp(p,"..")){close(fd);errno=EINVAL;return -1;}return fd;
}
bool record_archive_choose(int root,struct record_info *r){
    r->archive[0]=0;
    if(!*station||!r->transport_stream||!r->clock_valid||offset%3600||offset< -12*3600||offset>13*3600||!component(r->serial,1,31,true))return true;
    if(r->unix_seconds>INT64_MAX-86400)return true;
    time_t local=(time_t)((int64_t)r->unix_seconds+offset);struct tm tm;if(!gmtime_r(&local,&tm)||tm.tm_year<100||tm.tm_year>8099)return true;
    char date[9],clock[7],relative[80];if(!strftime(date,sizeof(date),"%Y%m%d",&tm)||!strftime(clock,sizeof(clock),"%H%M%S",&tm))return false;
    snprintf(relative,sizeof(relative),"%s/%s/%s/%s_%c_%u",station,date,r->serial,clock,'M'+offset/3600,r->slot);
    if(!record_archive_valid(relative))return false;int dir=record_archive_parent(root,relative,true);if(dir<0)return false;
    struct stat st;int found=fstatat(dir,strrchr(relative,'/')+1,&st,AT_SYMLINK_NOFOLLOW);int error=errno;close(dir);
    if(!found)return true; /* Never overwrite a same-second recording. */
    if(error!=ENOENT)return false;strcpy(r->archive,relative);return true;
}
struct entry {char path[80];uint16_t duration;uint8_t slot,file_type;};
static int compare(const void *a,const void *b){return strcmp(((const struct entry *)a)->path,((const struct entry *)b)->path);}
static bool write_all(int fd,const void *data,size_t n){const unsigned char *p=data;while(n){ssize_t k=write(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return false;p+=k;n-=(size_t)k;}return true;}
static int index_begin(int dir){
    /* Abandoned temporaries are disposable; never truncate an existing link. */
    if(unlinkat(dir,".vava.idx.tmp",0)&&errno!=ENOENT)return -1;
    return openat(dir,".vava.idx.tmp",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
}
static bool index_end(int dir,int fd,bool ok){if(ok)ok=!fsync(fd);if(close(fd))ok=false;if(ok)ok=!renameat(dir,".vava.idx.tmp",dir,"vava.idx")&&!fsync(dir);return ok;}
static bool header(int fd,unsigned count){uint8_t b[12]={0};vava_put32(b,0xEB0000BB);vava_put32(b+4,count);return write_all(fd,b,sizeof(b));}
static void obfuscate(uint8_t *row,unsigned start,const char *name,unsigned n){uint8_t seed=(uint8_t)(vava_crc32((const uint8_t *)name,n)%100+1);row[0]=seed+26;for(unsigned i=0;i<n;++i)row[start+i]=(uint8_t)name[i]^seed;}
static bool file_index(int dir,const struct entry *rows,unsigned count){
    int fd=index_begin(dir);if(fd<0)return false;bool ok=header(fd,count);
    for(unsigned i=0;ok&&i<count;++i){uint8_t row[16]={0};const char *name=strrchr(rows[i].path,'/')+1;
        /* Only the normalized, verified archive category belongs in the index. */
        row[1]=rows[i].file_type;row[2]=rows[i].slot;obfuscate(row,3,name,10);row[14]=(uint8_t)rows[i].duration;row[15]=(uint8_t)(rows[i].duration>>8);ok=write_all(fd,row,sizeof(row));}
    return index_end(dir,fd,ok);
}
static bool date_index(int dir,const struct entry *rows,unsigned count){
    unsigned dates=0;for(unsigned i=0;i<count;++i)if(!i||memcmp(rows[i-1].path+26,rows[i].path+26,8))++dates;
    int fd=index_begin(dir);if(fd<0)return false;bool ok=header(fd,dates);
    for(unsigned i=0;ok&&i<count;++i)if(!i||memcmp(rows[i-1].path+26,rows[i].path+26,8)){uint8_t b[10]={0};obfuscate(b,1,rows[i].path+26,8);ok=write_all(fd,b,sizeof(b));}
    return index_end(dir,fd,ok);
}
static void range(const struct entry *rows,unsigned count,const char *prefix,unsigned *begin,unsigned *end){
    size_t n=strlen(prefix);unsigned lo=0,hi=count;while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(strcmp(rows[mid].path,prefix)<0)lo=mid+1;else hi=mid;}
    *begin=lo;while(lo<count&&!strncmp(rows[lo].path,prefix,n))++lo;*end=lo;
}
static bool walk(int dir,dev_t device,unsigned level,const char *prefix,const struct entry *rows,unsigned count,bool cleanup,uint64_t *bytes,uint64_t *removed){
    DIR *d=directory_scan_open(dir);if(!d)return false;
    bool ok=true;struct dirent *e;unsigned first,last;range(rows,count,prefix,&first,&last);
    while(ok&&(e=directory_scan_next(d,&ok))){if(e->d_name[0]=='.')continue;
        if(level==3){if(!clip_name(e->d_name))continue;char path[80];int n=snprintf(path,sizeof(path),"%s%s",prefix,e->d_name);if(n<0||(size_t)n>=sizeof(path)){ok=false;break;}
            struct entry key={0};strcpy(key.path,path);bool live=bsearch(&key,rows,count,sizeof(*rows),compare)!=NULL;struct stat st;
            if(fstatat(dir,e->d_name,&st,AT_SYMLINK_NOFOLLOW)||!S_ISREG(st.st_mode)||st.st_nlink!=1||st.st_uid!=geteuid()||(st.st_mode&0077)){ok=false;break;}
            if(cleanup&&!live){if(unlinkat(dir,e->d_name,0)){ok=false;break;}*removed+=(uint64_t)st.st_size;}else *bytes+=(uint64_t)st.st_size;
        }else{
            bool valid=level==0?component(e->d_name,25,25,false):level==1?date_name(e->d_name):component(e->d_name,1,31,true);if(!valid)continue;
            int sub=child(dir,e->d_name,false,device);if(sub<0){ok=false;break;}char path[80];int n=snprintf(path,sizeof(path),"%s%s/",prefix,e->d_name);
            ok=n>0&&(size_t)n<sizeof(path)&&walk(sub,device,level+1,path,rows,count,cleanup,bytes,removed);close(sub);
            if(ok&&level>0){/* Empty camera/date folders may disappear; unknown files are preserved. */
                if(unlinkat(dir,e->d_name,AT_REMOVEDIR)&&errno!=ENOTEMPTY&&errno!=EEXIST)ok=false;
            }
        }
    }
    closedir(d);if(!ok)return false;
    if(level==3){if(first==last){if(unlinkat(dir,"vava.idx",0)&&errno!=ENOENT)return false;if(unlinkat(dir,".vava.idx.tmp",0)&&errno!=ENOENT)return false;}
        else if(!file_index(dir,rows+first,last-first))return false;}
    if(level==1&&!date_index(dir,rows+first,last-first))return false;
    return !fsync(dir);
}
static struct entry *collect(unsigned *length){
    unsigned capacity=record_index_count(),count=0;struct entry *rows=calloc(capacity?capacity:1,sizeof(*rows));if(!rows)return NULL;
    uint64_t after=0;bool ok=true;for(;;){struct record_info batch[16];unsigned n=record_index_after(after,batch,16);if(n==UINT32_MAX){ok=false;break;}
        for(unsigned i=0;i<n;++i){struct record_info *r=&batch[i];after=r->id;if(!*r->archive)continue;if(count>=capacity||!record_archive_valid(r->archive)){ok=false;break;}
            strcpy(rows[count].path,r->archive);rows[count].slot=(uint8_t)r->slot;rows[count].file_type=r->file_type;uint64_t seconds=(r->last_ms-r->first_ms)/1000;rows[count].duration=(uint16_t)(seconds>65535?65535:seconds);++count;}
        if(!ok||n<16)break;}
    qsort(rows,count,sizeof(*rows),compare);for(unsigned i=1;i<count;++i)if(!strcmp(rows[i-1].path,rows[i].path))ok=false;
    if(!ok){free(rows);return NULL;}*length=count;return rows;
}
bool record_archive_reconcile(int root,bool cleanup,uint64_t *bytes,uint64_t *removed){
    *bytes=*removed=0;unsigned count=0;struct entry *rows=collect(&count);if(!rows)return false;
    struct stat st;bool ok=!fstat(root,&st)&&walk(root,st.st_dev,0,"",rows,count,cleanup,bytes,removed);free(rows);return ok;
}
bool record_archive_update(int root,const char *relative){
    if(!*relative)return true;if(!record_archive_valid(relative))return false;
    unsigned count=0;struct entry *rows=collect(&count);if(!rows)return false;
    char prefix[80];strcpy(prefix,relative);char *slash=strrchr(prefix,'/');slash[1]=0;
    unsigned first,last;range(rows,count,prefix,&first,&last);int dir=record_archive_parent(root,relative,false);bool ok=dir>=0;
    if(ok&&first!=last)ok=file_index(dir,rows+first,last-first);
    else if(ok){ok=(!unlinkat(dir,"vava.idx",0)||errno==ENOENT)&&!fsync(dir);}
    if(dir>=0)close(dir);
    /* A last-file deletion removes empty parents only; unexpected files remain. */
    if(ok&&first==last){*slash=0;int parent=record_archive_parent(root,prefix,false);
        if(parent<0)ok=false;else{if(unlinkat(parent,strrchr(prefix,'/')+1,AT_REMOVEDIR)&&errno!=ENOTEMPTY&&errno!=EEXIST)ok=false;if(fsync(parent))ok=false;close(parent);}
        prefix[34]=0;parent=record_archive_parent(root,prefix,false);
        if(parent<0)ok=false;else{if(unlinkat(parent,prefix+26,AT_REMOVEDIR)&&errno!=ENOTEMPTY&&errno!=EEXIST)ok=false;if(fsync(parent))ok=false;close(parent);}
    }
    memcpy(prefix,relative,26);prefix[26]=0;range(rows,count,prefix,&first,&last);
    char index[40];memcpy(index,relative,25);strcpy(index+25,"/vava.idx");dir=record_archive_parent(root,index,false);
    if(ok)ok=dir>=0&&date_index(dir,rows+first,last-first);if(dir>=0)close(dir);free(rows);return ok;
}
