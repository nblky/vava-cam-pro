#include "directory_scan.h"
#include "transfer_journal.h"
#include "protocol.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define LIMIT 65536u
struct transfer_journal {int dir;uint64_t *ids;uint16_t *owners;unsigned count,capacity,owner_count;struct {unsigned slot;char serial[32];} identities[1024];};
static int cmp(const void *a,const void *b){uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return x>y?1:x<y?-1:0;}
static unsigned find(struct transfer_journal *j,uint64_t id){unsigned lo=0,hi=j->count;while(lo<hi){unsigned m=lo+(hi-lo)/2;if(j->ids[m]<id)lo=m+1;else hi=m;}return lo;}
static bool grow(struct transfer_journal *j){if(j->count<j->capacity)return true;if(j->count==LIMIT)return false;unsigned cap=j->capacity?j->capacity*2:128;void *p=realloc(j->ids,cap*sizeof(*j->ids));if(!p)return false;j->ids=p;p=realloc(j->owners,cap*sizeof(*j->owners));if(!p)return false;j->owners=p;j->capacity=cap;return true;}
static unsigned owner(struct transfer_journal *j,const struct myvava_file *f){unsigned i=0;while(i<j->owner_count&&(j->identities[i].slot!=f->slot||strcmp(j->identities[i].serial,f->serial)))++i;if(i==j->owner_count&&i<1024){j->identities[i].slot=f->slot;strcpy(j->identities[i].serial,f->serial);++j->owner_count;}return i;}
static int private_file(int dir,const char *name,int flags){int fd=openat(dir,name,flags|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);struct stat st;if(fd>=0&&(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)||st.st_nlink!=1)){close(fd);fd=-1;}return fd;}
static void encode(uint8_t b[256],const struct transfer_job *r){memset(b,0,256);memcpy(b,"MVJOB001",8);(void)myvava_file_encode(b+16,&r->file);memcpy(b+144,r->target,32);b[176]=(uint8_t)r->state;vava_put32(b+180,r->offset);for(unsigned k=0;k<8;++k)b[184+k]=(uint8_t)(r->retry_at>>(8*k));memcpy(b+192,r->error,40);vava_put32(b+8,vava_crc32(b+16,240));}
int transfer_journal_get(struct transfer_journal *j,uint64_t id,struct transfer_job *r){
    if(!j)return -1;unsigned index=find(j,id);if(index==j->count||j->ids[index]!=id)return 0;char name[32];snprintf(name,sizeof(name),"%016" PRIx64 ".job",id);
    int fd=private_file(j->dir,name,O_RDONLY);if(fd<0)return -1;uint8_t b[257];ssize_t got;do{got=read(fd,b,sizeof(b));}while(got<0&&errno==EINTR);close(fd);
    if(got!=256||memcmp(b,"MVJOB001",8)||vava_u32(b+8)!=vava_crc32(b+16,240)||!memchr(b+192,0,40)||b[176]>15)return -1;
    memset(r,0,sizeof(*r));if(!myvava_file_decode(b+16,128,&r->file)||r->file.id!=id)return -1;memcpy(r->target,b+144,32);r->state=b[176];r->offset=vava_u32(b+180);for(unsigned k=0;k<8;++k)r->retry_at|=(uint64_t)b[184+k]<<(8*k);memcpy(r->error,b+192,40);
    for(unsigned i=0;r->error[i];++i)if(!((r->error[i]>='a'&&r->error[i]<='z')||(r->error[i]>='0'&&r->error[i]<='9')||r->error[i]=='_'))return -1;
    return r->offset<=r->file.size?1:-1;
}
struct transfer_journal *transfer_journal_open(int parent){
    if(mkdirat(parent,"jobs",0700)&&errno!=EEXIST)return NULL;int fd=openat(parent,"jobs",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);struct stat st;
    if(fd<0)return NULL;if(fstat(fd,&st)||st.st_uid!=geteuid()||(st.st_mode&0077)){close(fd);return NULL;}
    struct transfer_journal *j=calloc(1,sizeof(*j));if(!j){close(fd);return NULL;}j->dir=fd;DIR *dir=directory_scan_open(fd);if(!dir){transfer_journal_close(j);return NULL;}
    bool ok=true;struct dirent *e;while(ok&&(e=directory_scan_next(dir,&ok))){if(strlen(e->d_name)!=20||strcmp(e->d_name+16,".job"))continue;char hex[17];memcpy(hex,e->d_name,16);hex[16]=0;uint8_t b[8];if(!myvava_unhex(hex,b,8)||!grow(j)){ok=false;break;}uint64_t id=0;for(unsigned k=0;k<8;++k)id=id*256+b[k];if(!id){ok=false;break;}j->ids[j->count++]=id;}
    closedir(dir);qsort(j->ids,j->count,sizeof(*j->ids),cmp);for(unsigned i=0;ok&&i<j->count;++i){struct transfer_job row;ok=transfer_journal_get(j,j->ids[i],&row)==1;if(ok){unsigned index=owner(j,&row.file);ok=index<1024;j->owners[i]=(uint16_t)index;}}
    if(!ok){transfer_journal_close(j);return NULL;}return j;
}
void transfer_journal_close(struct transfer_journal *j){if(j){close(j->dir);free(j->ids);free(j->owners);free(j);}}
unsigned transfer_journal_count(struct transfer_journal *j){return j?j->count:0;}
bool transfer_journal_put(struct transfer_journal *j,const struct transfer_job *r){
    if(!j||r->state>15||!memchr(r->error,0,40))return false;uint8_t b[256];if(!myvava_file_encode(b,&r->file))return false;
    unsigned i=find(j,r->file.id);bool exists=i<j->count&&j->ids[i]==r->file.id;if(!exists&&!grow(j))return false;
    unsigned identity=owner(j,&r->file);if(identity==1024)return false;
    encode(b,r);char name[32],tmp[40];snprintf(name,sizeof(name),"%016" PRIx64 ".job",r->file.id);snprintf(tmp,sizeof(tmp),"%s.tmp",name);
    int fd=private_file(j->dir,tmp,O_WRONLY|O_CREAT);if(fd<0)return false;bool ok=!ftruncate(fd,0);size_t off=0;
    while(ok&&off<sizeof(b)){ssize_t n=write(fd,b+off,sizeof(b)-off);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else off+=(size_t)n;}
    ok=ok&&!fsync(fd);if(close(fd))ok=false;if(ok)ok=!renameat(j->dir,tmp,j->dir,name)&&!fsync(j->dir);
    if(ok&&!exists){memmove(j->ids+i+1,j->ids+i,(j->count-i)*sizeof(*j->ids));memmove(j->owners+i+1,j->owners+i,(j->count-i)*sizeof(*j->owners));j->ids[i]=r->file.id;j->owners[i]=(uint16_t)identity;++j->count;}return ok;
}
unsigned transfer_journal_page(struct transfer_journal *j,unsigned slot,const char *serial,unsigned page,struct transfer_job *out,unsigned cap,unsigned *total){
    *total=0;if(!j)return UINT32_MAX;unsigned n=0;for(unsigned i=j->count;i>0;--i){unsigned identity=j->owners[i-1];if(j->identities[identity].slot!=slot||strcmp(j->identities[identity].serial,serial))continue;unsigned k=(*total)++;if(k<page*cap||n==cap)continue;if(transfer_journal_get(j,j->ids[i-1],&out[n])!=1)return UINT32_MAX;++n;}return n;
}
unsigned transfer_journal_scan(struct transfer_journal *j,uint64_t before,struct transfer_job *out,unsigned cap){if(!j)return UINT32_MAX;unsigned i=before?find(j,before):j->count,n=0;while(i&&n<cap){if(transfer_journal_get(j,j->ids[--i],&out[n])!=1)return UINT32_MAX;++n;}return n;}
bool transfer_journal_delete(struct transfer_journal *j,uint64_t id){if(!j)return false;unsigned i=find(j,id);if(i==j->count||j->ids[i]!=id)return false;char name[32];snprintf(name,sizeof(name),"%016" PRIx64 ".job",id);if(unlinkat(j->dir,name,0)||fsync(j->dir))return false;memmove(j->ids+i,j->ids+i+1,(j->count-i-1)*sizeof(*j->ids));memmove(j->owners+i,j->owners+i+1,(j->count-i-1)*sizeof(*j->owners));--j->count;return true;}
