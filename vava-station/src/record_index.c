#include "record_index.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define ROW_BYTES 368u
#define CACHE_ROWS 32u
struct reference {uint64_t id;uint32_t offset;uint16_t identity,reserved;};
struct identity {unsigned slot;char serial[32];};
static struct reference *refs;
static unsigned count,capacity,identities;
static struct identity owners[1024];
static struct record_info cache[CACHE_ROWS];
static unsigned next_cache;
static int fd=-1;
static uint32_t disk_rows;
static int backing_dir=-1;
static void p64(uint8_t *b,uint64_t n){for(unsigned i=0;i<8;++i)b[i]=(uint8_t)(n>>(8*i));}
static uint64_t g64(const uint8_t *b){uint64_t n=0;for(unsigned i=0;i<8;++i)n|=(uint64_t)b[i]<<(8*i);return n;}
static void encode(uint8_t b[ROW_BYTES],const struct record_info *r){
    memset(b,0,ROW_BYTES);memcpy(b,"VRCACHE3",8);
    const uint64_t v[]={r->id,r->generation,r->unix_seconds,r->bytes,r->stored_bytes,r->frames,r->audio_frames,r->first_ms,r->last_ms};
    for(unsigned i=0;i<9;++i)p64(b+8+i*8,v[i]);
    vava_put32(b+80,r->slot);vava_put32(b+84,r->stream);vava_put32(b+88,r->sample_rate);vava_put32(b+92,r->channels);
    vava_put32(b+96,(r->pir?1u:0u)|(r->transport_stream?2u:0u)|(r->clock_valid?4u:0u)|(r->encrypted?8u:0u));
    memcpy(b+100,r->sha256,32);memcpy(b+132,r->upload_target,32);memcpy(b+164,r->nas_target,32);memcpy(b+196,r->serial,32);memcpy(b+228,r->error,40);memcpy(b+268,r->archive,80);b[348]=r->file_type;b[349]=r->detection_reported;vava_put32(b+352,r->detection_result);vava_put32(b+356,r->detection_type);vava_put32(b+364,vava_crc32(b,364));
}
static bool decode(const uint8_t b[ROW_BYTES],struct record_info *r){
    if(memcmp(b,"VRCACHE3",8)||vava_u32(b+364)!=vava_crc32(b,364)||!memchr(b+268,0,80)||!memchr(b+196,0,32)||!memchr(b+228,0,40)||vava_u32(b+80)>3||vava_u32(b+96)>15)return false;
    unsigned flags=vava_u32(b+96);*r=(struct record_info){.id=g64(b+8),.generation=g64(b+16),.unix_seconds=g64(b+24),.bytes=g64(b+32),.stored_bytes=g64(b+40),.frames=g64(b+48),.audio_frames=g64(b+56),.first_ms=g64(b+64),.last_ms=g64(b+72),.slot=vava_u32(b+80),.stream=vava_u32(b+84),.sample_rate=vava_u32(b+88),.channels=vava_u32(b+92),.pir=(flags&1)!=0,.transport_stream=(flags&2)!=0,.clock_valid=(flags&4)!=0,.encrypted=(flags&8)!=0};
    memcpy(r->sha256,b+100,32);memcpy(r->upload_target,b+132,32);memcpy(r->nas_target,b+164,32);memcpy(r->serial,b+196,32);memcpy(r->error,b+228,40);memcpy(r->archive,b+268,80);r->file_type=b[348];r->detection_reported=b[349];r->detection_result=vava_u32(b+352);r->detection_type=vava_u32(b+356);return r->id!=0&&(!r->file_type||(r->file_type>=5&&r->file_type<=8))&&b[349]<=1;
}
static bool io(bool write_mode,void *buf,size_t n,off_t off){
    size_t done=0;while(done<n){ssize_t got=write_mode?pwrite(fd,(uint8_t *)buf+done,n-done,off+(off_t)done):pread(fd,(uint8_t *)buf+done,n-done,off+(off_t)done);if(got<0&&errno==EINTR)continue;if(got<=0)return false;done+=(size_t)got;}return true;
}
bool record_index_open(const char *directory){
    int dir=open(directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)return false;bool ok=record_index_open_fd(dir);close(dir);return ok;
}
bool record_index_open_fd(int directory_fd){
    record_index_close();backing_dir=dup(directory_fd);if(backing_dir<0)return false;(void)fcntl(backing_dir,F_SETFD,FD_CLOEXEC);
    fd=openat(backing_dir,".catalog.cache",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);struct stat st;
    if(fd<0||fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)||st.st_nlink!=1||ftruncate(fd,0)){record_index_close();return false;}return true;
}
void record_index_close(void){if(fd>=0)close(fd);fd=-1;if(backing_dir>=0)close(backing_dir);backing_dir=-1;free(refs);refs=NULL;count=capacity=identities=disk_rows=next_cache=0;memset(cache,0,sizeof(cache));memset(owners,0,sizeof(owners));}
unsigned record_index_count(void){return count;}
unsigned record_index_owners(unsigned page,struct record_owner *out,unsigned cap,unsigned *total){
    unsigned counts[1024]={0};for(unsigned i=0;i<count;++i)++counts[refs[i].identity];
    unsigned n=0;*total=0;for(unsigned i=0;i<identities;++i)if(counts[i]){
        unsigned at=(*total)++;if(at<page*cap||n>=cap)continue;
        out[n]=(struct record_owner){.slot=owners[i].slot,.records=counts[i]};strcpy(out[n++].serial,owners[i].serial);
    }
    return n;
}
bool record_index_identity(unsigned slot,const char *serial){
    for(unsigned i=0;i<count;++i){const struct identity *o=&owners[refs[i].identity];if(o->slot==slot&&!strcmp(o->serial,serial))return true;}return false;
}
static bool row(unsigned i,struct record_info *r){
    if(i>=count)return false;for(unsigned k=0;k<CACHE_ROWS;++k)if(cache[k].id==refs[i].id){*r=cache[k];return true;}
    uint8_t bytes[ROW_BYTES];if(!io(false,bytes,sizeof(bytes),(off_t)refs[i].offset*ROW_BYTES)||!decode(bytes,r)||r->id!=refs[i].id)return false;
    cache[next_cache++%CACHE_ROWS]=*r;return true;
}
static int compare(const void *a,const void *b){const struct reference *x=a,*y=b;return x->id>y->id?1:x->id<y->id?-1:0;}
void record_index_sort(void){qsort(refs,count,sizeof(*refs),compare);}
static unsigned find(uint64_t id){unsigned lo=0,hi=count;while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(refs[mid].id<id)lo=mid+1;else hi=mid;}return lo<count&&refs[lo].id==id?lo:count;}
bool record_index_has(uint64_t id){return find(id)<count;}
static bool compact(void){
    int scratch=openat(backing_dir,".catalog.new",O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);struct stat st;
    if(scratch<0)return false;if(fstat(scratch,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)||st.st_nlink!=1||ftruncate(scratch,0)){close(scratch);return false;}
    bool ok=true;uint8_t b[ROW_BYTES];for(unsigned i=0;ok&&i<count;++i){struct record_info r;ok=row(i,&r);if(ok){encode(b,&r);size_t off=0;while(ok&&off<sizeof(b)){ssize_t n=write(scratch,b+off,sizeof(b)-off);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else off+=(size_t)n;}}}
    if(ok)ok=!fsync(scratch)&&!renameat(backing_dir,".catalog.new",backing_dir,".catalog.cache");
    if(!ok){close(scratch);return false;}close(fd);fd=scratch;for(unsigned i=0;i<count;++i)refs[i].offset=i;disk_rows=count;return true;
}
bool record_index_add(const struct record_info *r){
    if(fd<0||!r->id||r->slot>3||!memchr(r->serial,0,32)||count>=RECORD_CATALOG_MAX)return false;
    unsigned at=0,hi=count;while(at<hi){unsigned mid=at+(hi-at)/2;if(refs[mid].id<r->id)at=mid+1;else hi=mid;}
    if(at<count&&refs[at].id==r->id)return false;
    if(count==capacity){unsigned cap=capacity?capacity*2:128;if(cap>RECORD_CATALOG_MAX)cap=RECORD_CATALOG_MAX;void *p=realloc(refs,cap*sizeof(*refs));if(!p)return false;refs=p;capacity=cap;}
    unsigned identity=0;while(identity<identities&&(owners[identity].slot!=r->slot||strcmp(owners[identity].serial,r->serial)))++identity;
    if(identity==identities){if(identities==1024)return false;owners[identities].slot=r->slot;strcpy(owners[identities++].serial,r->serial);}
    if(disk_rows>count*2+256&&!compact())return false;if(disk_rows==UINT32_MAX)return false;
    uint8_t b[ROW_BYTES];encode(b,r);if(!io(true,b,sizeof(b),(off_t)disk_rows*ROW_BYTES))return false;
    memmove(refs+at+1,refs+at,(count-at)*sizeof(*refs));refs[at]=(struct reference){.id=r->id,.offset=disk_rows++,.identity=(uint16_t)identity};++count;cache[next_cache++%CACHE_ROWS]=*r;
    return true;
}
int record_index_lookup(unsigned slot,const char *serial,uint64_t id,struct record_info *r){unsigned i=find(id);if(i==count||owners[refs[i].identity].slot!=slot||strcmp(owners[refs[i].identity].serial,serial))return 0;return row(i,r)?1:-1;}
bool record_index_get(unsigned slot,const char *serial,uint64_t id,struct record_info *r){return record_index_lookup(slot,serial,id,r)>0;}
bool record_index_remove(uint64_t id){unsigned i=find(id);if(i==count)return false;memmove(refs+i,refs+i+1,(count-i-1)*sizeof(*refs));--count;for(unsigned k=0;k<CACHE_ROWS;++k)if(cache[k].id==id)cache[k].id=0;return true;}
unsigned record_index_page(unsigned slot,const char *serial,unsigned page,struct record_info *out,unsigned cap,unsigned *total){
    *total=0;unsigned n=0;for(unsigned i=count;i>0;--i){struct identity *identity=&owners[refs[i-1].identity];if(identity->slot!=slot||strcmp(identity->serial,serial))continue;
        unsigned index=(*total)++;if(index<page*cap||n==cap)continue;if(!row(i-1,&out[n]))return UINT32_MAX;++n;}return n;
}
unsigned record_index_scan(uint64_t before,struct record_info *out,unsigned cap){
    unsigned n=0,i=count;if(before){unsigned lo=0,hi=count;while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(refs[mid].id<before)lo=mid+1;else hi=mid;}i=lo;}
    while(i&&n<cap){if(!row(--i,&out[n]))return UINT32_MAX;++n;}return n;
}
unsigned record_index_after(uint64_t after,struct record_info *out,unsigned cap){
    unsigned lo=0,hi=count,n=0;while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(refs[mid].id<=after)lo=mid+1;else hi=mid;}
    while(lo<count&&n<cap){if(!row(lo++,&out[n]))return UINT32_MAX;++n;}return n;
}

/* Cursor is an exclusive stable ID, not an offset into a changing directory.
 * Filter before counting/pagination. Unknown clocks only match unfiltered queries. */
unsigned record_index_query(unsigned slot,const char *serial,uint64_t before,uint64_t from,uint64_t to,struct record_info *out,unsigned cap,unsigned *total,bool *more){
    unsigned n=0;*total=0;*more=false;
    for(unsigned i=count;i>0;--i){struct reference *ref=&refs[i-1];struct identity *owner=&owners[ref->identity];
        if(serial&&(owner->slot!=slot||strcmp(owner->serial,serial)))continue;
        struct record_info r;bool loaded=false;
        if(to){if(!row(i-1,&r))return UINT32_MAX;loaded=true;if(!r.clock_valid||r.unix_seconds<from||r.unix_seconds>=to)continue;}
        ++*total;if(before&&ref->id>=before)continue;
        if(n==cap){*more=true;continue;}
        if(!loaded&&!row(i-1,&r))return UINT32_MAX;out[n++]=r;
    }
    return n;
}
