#include "file_store.h"
#include "sealed_file.h"
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void sleep_ms(unsigned milliseconds) {
    struct timespec delay={.tv_sec=(time_t)(milliseconds/1000),
                           .tv_nsec=(long)(milliseconds%1000)*1000000L};
    while (nanosleep(&delay,&delay)!=0) CHECK(errno==EINTR);
}
static uint64_t seq;
static struct myvava_message send_request(struct file_store *s,const char *device,uint8_t type,
                                         struct myvava_file *f,uint32_t offset,const uint8_t *data,size_t length,uint8_t ack[33]) {
    struct myvava_message m={.type=type,.id=f->id,.total=f->size,.offset=offset,.data=data,.length=length};
    uint64_t cookie=++seq, returned=0; CHECK(file_store_submit(s,cookie,device,&m));
    uint64_t end=myvava_millis()+5000;
    while (!file_store_poll(s,&returned,&m,ack)) { CHECK(myvava_millis()<end); struct timespec t={.tv_nsec=1000000}; nanosleep(&t,NULL); }
    CHECK(returned==cookie && m.type==MYVAVA_FILE_ACK && m.id==f->id && m.total==f->size && m.length==33);
    CHECK(!memcmp(ack,f->sha256,32)); return m;
}
static unsigned offer(struct file_store *s,const char *device,struct myvava_file *f,uint32_t *offset) {
    uint8_t d[128],ack[33]; CHECK(myvava_file_encode(d,f));
    struct myvava_message m=send_request(s,device,MYVAVA_FILE_OFFER,f,0,d,sizeof(d),ack); *offset=m.offset; return ack[32];
}
static void commit_small(struct file_store *s,uint64_t id,const char *serial,bool imported) {
    const uint8_t bytes[188]={0x47};uint8_t ack[33],payload[220],descriptor[128];unsigned n;uint32_t off;
    struct myvava_file f={.id=id,.size=188,.frames=1,.first_ms=1000,.last_ms=1000,.pir=1,.clock_valid=1,.unix_seconds=id==1?1600000100:1600000000};
    strcpy(f.serial,serial);CHECK(EVP_Digest(bytes,188,f.sha256,&n,EVP_sha256(),NULL)==1);
    CHECK(offer(s,"BASE_A",&f,&off)==MVF_PARTIAL);memcpy(payload,f.sha256,32);memcpy(payload+32,bytes,188);
    send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,0,payload,220,ack);CHECK(ack[32]==MVF_PARTIAL);
    send_request(s,"BASE_A",MYVAVA_FILE_COMMIT,&f,188,f.sha256,32,ack);CHECK(ack[32]==MVF_COMMITTED);
    if(imported){CHECK(myvava_file_encode(descriptor,&f));CHECK(file_store_mark_imported(s,"BASE_A",descriptor));}
}
static void storage_sources(void) {
    char dir[]="/tmp/myvava-sources-XXXXXX",out[16384],name[256];CHECK(mkdtemp(dir));
    struct file_store *s=file_store_start(dir,100000,64);CHECK(s);
    uint64_t cache=UINT64_C(0xa000000000000001),import=UINT64_C(0xb000000000000001);
    commit_small(s,cache,"CAM_A",false);commit_small(s,import,"CAM_A",true);
    for(unsigned i=1;i<=20;++i)commit_small(s,i,i<=18?"CAM_A":"CAM_B",false);
    char first[16384],second[16384];
    CHECK(file_store_list_filtered(s,"BASE_A",0,"cloud","CAM_A",first,sizeof(first)));
    CHECK(strstr(first,"\"total\":18"));CHECK(!strstr(first,"a000000000000001"));CHECK(!strstr(first,"CAM_B"));
    // A newer timestamp beats a larger ID; equal timestamps break ties by ID.
    const char *newest=strstr(first,"\"id\":\"0000000000000001\""),*tie=strstr(first,"\"id\":\"0000000000000012\"");
    CHECK(newest&&tie&&newest<tie);CHECK(strstr(first,"\"id\":\"0000000000000004\""));CHECK(!strstr(first,"\"id\":\"0000000000000003\""));
    CHECK(file_store_list_filtered(s,"BASE_A",1,"cloud","CAM_A",second,sizeof(second)));
    const char *three=strstr(second,"\"id\":\"0000000000000003\""),*two=strstr(second,"\"id\":\"0000000000000002\"");CHECK(three&&two&&three<two);
    file_store_stop(s);s=file_store_start(dir,100000,64);CHECK(s);
    CHECK(file_store_list_filtered(s,"BASE_A",0,"cloud","CAM_A",out,sizeof(out)));CHECK(!strcmp(first,out));
    CHECK(file_store_list_filtered(s,"BASE_A",1,"cloud","CAM_A",out,sizeof(out)));CHECK(!strcmp(second,out));
    CHECK(file_store_list_filtered(s,"BASE_A",0,"tf_export",NULL,out,sizeof(out)));CHECK(strstr(out,"\"total\":1"));CHECK(strstr(out,"a000000000000001"));
    CHECK(file_store_list_filtered(s,"BASE_A",0,"legacy_import",NULL,out,sizeof(out)));CHECK(strstr(out,"\"total\":1"));CHECK(strstr(out,"b000000000000001"));
    CHECK(file_store_list_filtered(s,"BASE_B",0,"cloud",NULL,out,sizeof(out)));CHECK(strstr(out,"\"total\":0"));
    CHECK(!file_store_list_filtered(s,"BASE_A",0,"invalid",NULL,out,sizeof(out)));
    struct myvava_file f;const char *kind=NULL;CHECK(file_store_info_storage(s,"BASE_A",import,&f,&kind));CHECK(!strcmp(kind,"legacy_import"));
    // Cloud retention never deletes a fresh TF cache or an imported old video.
    file_store_enable_retention(s);CHECK(file_store_policy_set(s,1,true,1)==200);
    uint64_t end=myvava_millis()+10000;do {CHECK(file_store_list_filtered(s,"BASE_A",0,"cloud",NULL,out,sizeof(out)));if(strstr(out,"\"total\":0"))break;CHECK(myvava_millis()<end);sleep_ms(10);}while(1);
    CHECK(file_store_info(s,"BASE_A",cache,&f));CHECK(file_store_info(s,"BASE_A",import,&f));CHECK(file_store_policy_set(s,2,false,1)==200);
    file_store_stop(s);snprintf(name,sizeof(name),"%s/BASE_A.a000000000000001.meta",dir);
    struct timespec old[2]={{.tv_sec=time(NULL)-3700},{.tv_sec=time(NULL)-3700}};CHECK(!utimensat(AT_FDCWD,name,old,0));
    s=file_store_start(dir,100000,64);CHECK(s);int lease=file_store_acquire(s,"BASE_A",cache,&f);CHECK(lease>=0);file_store_enable_retention(s);
    sleep_ms(1200);CHECK(file_store_info(s,"BASE_A",cache,&f));file_store_release(s,"BASE_A",cache,lease);
    end=myvava_millis()+5000;while(file_store_info(s,"BASE_A",cache,&f)){CHECK(myvava_millis()<end);sleep_ms(10);}
    CHECK(file_store_info_storage(s,"BASE_A",import,&f,&kind));CHECK(!strcmp(kind,"legacy_import"));
    file_store_stop(s);DIR *d=opendir(dir);CHECK(d);struct dirent *entry;while((entry=readdir(d))){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;CHECK(!unlinkat(dirfd(d),entry->d_name,0));}closedir(d);CHECK(!rmdir(dir));
    puts("storage sources: stable newest-first pages across restart, filter before paging, import identity, cloud-only retention, TF cache expiry and reader protection passed");
}
static void encrypted_batches(void) {
    char dir[]="/tmp/myvava-batch-XXXXXX",keydir[]="/tmp/myvava-batchkeys-XXXXXX",kid[33],path[256];
    CHECK(mkdtemp(dir)&&mkdtemp(keydir));CHECK(seal_keys_generate(keydir,false,kid));
    uint8_t bytes[188*512],payload[MYVAVA_FILE_PAYLOAD_MAX],ack[33];unsigned digest_size;
    for(size_t i=0;i<sizeof(bytes);++i)bytes[i]=(uint8_t)(i*13u);
    struct myvava_file f={.id=7,.size=sizeof(bytes),.frames=512,.first_ms=1000,.last_ms=2000};strcpy(f.serial,"BATCH_CAM");
    CHECK(EVP_Digest(bytes,sizeof(bytes),f.sha256,&digest_size,EVP_sha256(),NULL)==1);memcpy(payload,f.sha256,32);
    struct file_store *s=file_store_start_encrypted(dir,1024*1024,2,keydir);CHECK(s);uint32_t off=0;
    CHECK(offer(s,"BASE_A",&f,&off)==MVF_PARTIAL&&off==0);
    /* A legacy partial is resumed with batches; disk capsules stay identical. */
    memcpy(payload+32,bytes,MYVAVA_FILE_DATA_MAX);
    struct myvava_message m=send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,0,payload,32+MYVAVA_FILE_DATA_MAX,ack);
    CHECK(ack[32]==MVF_PARTIAL&&m.offset==MYVAVA_FILE_DATA_MAX);off=m.offset;
    memcpy(payload+32,bytes+off,MYVAVA_FILE_BATCH_DATA_MAX);
    m=send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,off,payload,sizeof(payload),ack);
    CHECK(ack[32]==MVF_PARTIAL&&m.offset==off+MYVAVA_FILE_BATCH_DATA_MAX);
    uint32_t durable=m.offset;
    m=send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,off,payload,sizeof(payload),ack);CHECK(ack[32]==MVF_PARTIAL&&m.offset==durable);
    payload[40]^=1;send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,off,payload,sizeof(payload),ack);CHECK(ack[32]==MVF_CONFLICT);payload[40]^=1;
    file_store_stop(s);
    snprintf(path,sizeof(path),"%s/BASE_A.0000000000000007.sealed-part",dir);
    int fd=open(path,O_WRONLY|O_APPEND);CHECK(fd>=0);CHECK(write(fd,"MVSEAL01broken",14)==14);CHECK(!close(fd));
    s=file_store_start_encrypted(dir,1024*1024,2,keydir);CHECK(s);
    CHECK(offer(s,"BASE_A",&f,&off)==MVF_PARTIAL&&off==durable);
    while(off<f.size){size_t n=f.size-off;if(n>MYVAVA_FILE_BATCH_DATA_MAX)n=MYVAVA_FILE_BATCH_DATA_MAX;
        memcpy(payload+32,bytes+off,n);m=send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,off,payload,n+32,ack);CHECK(ack[32]==MVF_PARTIAL&&m.offset==off+n);off=m.offset;}
    send_request(s,"BASE_A",MYVAVA_FILE_COMMIT,&f,off,f.sha256,32,ack);CHECK(ack[32]==MVF_COMMITTED);
    struct myvava_file info;fd=file_store_acquire(s,"BASE_A",f.id,&info);CHECK(fd>=0);
    uint8_t clear[MYVAVA_FILE_BATCH_DATA_MAX];
    for(off=0;off<f.size;){size_t n=f.size-off;if(n>sizeof(clear))n=sizeof(clear);CHECK(file_store_read(s,fd,&info,off,clear,n)==(int)n);CHECK(!memcmp(clear,bytes+off,n));off+=(uint32_t)n;}
    file_store_release(s,"BASE_A",f.id,fd);file_store_stop(s);
    const char *dirs[]={dir,keydir};for(unsigned i=0;i<2;++i){DIR *d=opendir(dirs[i]);CHECK(d);struct dirent *entry;while((entry=readdir(d))){if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;CHECK(!unlinkat(dirfd(d),entry->d_name,0));}closedir(d);CHECK(!rmdir(dirs[i]));}
    puts("encrypted batches: legacy resume, durable offsets, exact duplicate/conflict, interrupted capsule repair and complete digest passed");
}
int main(void) {
    umask(0077); char dir[]="/tmp/myvava-store-XXXXXX"; CHECK(mkdtemp(dir));
    uint8_t bytes[1504],d[128],ack[33],payload[768]; memset(bytes,0x47,sizeof(bytes));
    struct myvava_file f={.id=1,.size=sizeof(bytes),.frames=8,.first_ms=1000,.last_ms=2000,.slot=0,.pir=1};
    strcpy(f.serial,"CAMERA_A"); unsigned n=0;
    CHECK(EVP_Digest(bytes,sizeof(bytes),f.sha256,&n,EVP_sha256(),NULL)==1 && n==32);
    CHECK(myvava_file_encode(d,&f));
    struct file_store *s=file_store_start(dir,2*sizeof(bytes),2); CHECK(s);
    uint32_t off=0; CHECK(offer(s,"BASE_A",&f,&off)==MVF_PARTIAL && off==0);
    memcpy(payload,f.sha256,32); memcpy(payload+32,bytes,736);
    struct myvava_message m=send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,0,payload,sizeof(payload),ack);
    CHECK(ack[32]==MVF_PARTIAL && m.offset==736);
    m=send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,0,payload,sizeof(payload),ack);
    CHECK(ack[32]==MVF_PARTIAL && m.offset==736); /* exact retransmission */
    payload[40]^=1; send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,0,payload,sizeof(payload),ack); CHECK(ack[32]==MVF_CONFLICT); payload[40]^=1;
    file_store_stop(s); s=file_store_start(dir,2*sizeof(bytes),2); CHECK(s);
    CHECK(offer(s,"BASE_A",&f,&off)==MVF_PARTIAL && off==736);
    for (;off<f.size;) {
        size_t len=f.size-off; if (len>736) len=736; memcpy(payload+32,bytes+off,len);
        m=send_request(s,"BASE_A",MYVAVA_FILE_DATA,&f,off,payload,len+32,ack); CHECK(ack[32]==MVF_PARTIAL); off=m.offset;
    }
    /* A failed metadata publication must not acknowledge completion. */
    char obstacle[256]; snprintf(obstacle,sizeof(obstacle),"%s/BASE_A.0000000000000001.meta-tmp",dir); CHECK(!mkdir(obstacle,0700));
    send_request(s,"BASE_A",MYVAVA_FILE_COMMIT,&f,f.size,f.sha256,32,ack); CHECK(ack[32]==MVF_IO);
    file_store_stop(s); CHECK(!rmdir(obstacle));
    s=file_store_start(dir,2*sizeof(bytes),2); CHECK(s);
    CHECK(offer(s,"BASE_A",&f,&off)==MVF_COMMITTED && off==f.size); /* final-name / marker gap */
    strcpy(f.serial,"CAMERA_B"); CHECK(offer(s,"BASE_A",&f,&off)==MVF_CONFLICT);
    strcpy(f.serial,"CAMERA_A");
    CHECK(offer(s,"BASE_B",&f,&off)==MVF_PARTIAL); /* authenticated identity namespace */
    f.id=2; CHECK(offer(s,"BASE_A",&f,&off)==MVF_QUOTA); f.id=1;
    char listing[4096]; CHECK(file_store_list(s,"BASE_A",0,listing,sizeof(listing))); CHECK(strstr(listing,"\"state\":\"committed\""));
    CHECK(file_store_list(s,"BASE_B",0,listing,sizeof(listing))); CHECK(strstr(listing,"\"state\":\"receiving\""));
    /* Corruption is never promoted to a final file. */
    memcpy(payload,f.sha256,32); memset(payload+32,0,736);
    for (off=0;off<f.size;) {
        size_t len=f.size-off; if (len>736) len=736;
        m=send_request(s,"BASE_B",MYVAVA_FILE_DATA,&f,off,payload,len+32,ack); CHECK(ack[32]==MVF_PARTIAL); off=m.offset;
    }
    send_request(s,"BASE_B",MYVAVA_FILE_COMMIT,&f,f.size,f.sha256,32,ack); CHECK(ack[32]==MVF_DIGEST);
    struct myvava_file read_info; int reader=file_store_acquire(s,"BASE_A",f.id,&read_info);CHECK(reader>=0&&read_info.size==f.size);
    CHECK(file_store_acquire(s,"BASE_B",f.id,&read_info)<0); /* no reads of unfinished objects */
    CHECK(file_store_delete(s,"BASE_A",f.id)==409); /* active reader protects the file */
    file_store_enable_retention(s);
    CHECK(file_store_policy_set(s,1,true,0)==200);
    CHECK(file_store_policy_set(s,1,true,0)==409);
    uint64_t protected_until=myvava_millis()+1200;
    while(myvava_millis()<protected_until){struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);}
    CHECK(file_store_list(s,"BASE_A",0,listing,sizeof(listing)));CHECK(strstr(listing,"\"total\":1"));
    CHECK(file_store_list(s,"BASE_B",0,listing,sizeof(listing)));CHECK(strstr(listing,"\"state\":\"receiving\""));
    file_store_release(s,"BASE_A",f.id,reader);
    uint64_t cleanup_until=myvava_millis()+5000;
    do {
        CHECK(file_store_list(s,"BASE_A",0,listing,sizeof(listing)));
        if(strstr(listing,"\"total\":0"))break;
        CHECK(myvava_millis()<cleanup_until);struct timespec pause={.tv_nsec=10000000};nanosleep(&pause,NULL);
    } while(1);
    CHECK(file_store_delete(s,"BASE_A",f.id)==200);
    CHECK(file_store_delete(s,"BASE_A",f.id)==200); /* idempotent tombstone */
    CHECK(file_store_acquire(s,"BASE_A",f.id,&read_info)<0);
    CHECK(offer(s,"BASE_A",&f,&off)==MVF_DELETED); /* never resurrect a deleted recording */
    file_store_stop(s);s=file_store_start(dir,2*sizeof(bytes),2);CHECK(s);
    CHECK(offer(s,"BASE_A",&f,&off)==MVF_DELETED);
    CHECK(file_store_list(s,"BASE_A",0,listing,sizeof(listing)));CHECK(strstr(listing,"\"total\":0"));
    CHECK(myvava_file_encode(d,&f)); struct myvava_message pending={.type=MYVAVA_FILE_OFFER,.id=f.id,.total=f.size,.length=128,.data=d};
    for (unsigned i=0;i<FILE_STORE_JOBS;++i) CHECK(file_store_submit(s,++seq,"BASE_A",&pending));
    CHECK(!file_store_submit(s,++seq,"BASE_A",&pending)); /* unconsumed work/results are bounded */
    file_store_stop(s);
    /* A damaged policy preserves archives and refuses automatic deletion or
     * overwrite, while keeping the archive service readable. */
    char policy[256];snprintf(policy,sizeof(policy),"%s/.archive-policy",dir);
    int corrupt=open(policy,O_WRONLY);CHECK(corrupt>=0);CHECK(write(corrupt,"X",1)==1);CHECK(!close(corrupt));
    s=file_store_start(dir,2*sizeof(bytes),2);CHECK(s);file_store_enable_retention(s);
    CHECK(file_store_policy_json(s,listing,sizeof(listing)));CHECK(strstr(listing,"\"healthy\":false"));
    CHECK(strstr(listing,"\"enabled\":false"));CHECK(file_store_policy_set(s,1,false,30)==503);
    CHECK(file_store_list(s,"BASE_B",0,listing,sizeof(listing)));CHECK(strstr(listing,"\"total\":1"));file_store_stop(s);
    char unknown[256]; snprintf(unknown,sizeof(unknown),"%s/ORPHAN.0000000000000001.part",dir);
    int fd=open(unknown,O_WRONLY|O_CREAT|O_EXCL,0600); CHECK(fd>=0); CHECK(write(fd,"x",1)==1); close(fd);
    CHECK(!file_store_start(dir,2*sizeof(bytes),2)); /* preserve and reject uncharged orphan */
    puts("file store: resume, conflict, quota, commit-gap, digest, bounded jobs, retention leases and corrupt policy passed");
    /* The harness owns this temporary directory only. */
    DIR *cleanup=opendir(dir); CHECK(cleanup); struct dirent *entry;
    while ((entry=readdir(cleanup))) {
        if (!strcmp(entry->d_name,".") || !strcmp(entry->d_name,"..")) continue;
        CHECK(!unlinkat(dirfd(cleanup),entry->d_name,0));
    }
    closedir(cleanup); CHECK(!rmdir(dir));
    storage_sources();
    encrypted_batches();
    return 0;
}
