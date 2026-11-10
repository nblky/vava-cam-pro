#include "directory_scan.h"
#include "upload.h"
#include "recorder.h"
#include "protocol.h"
#include "transfer_journal.h"
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum upload_state { U_QUEUED,U_UPLOADING,U_COMMITTED,U_RETRY,U_FAILED,U_DESTINATION,U_CANCELLED,U_EXPIRED };
#define FULL_EXPORT_LIMIT 16
#define EXPORT_LIMIT 20
struct upload_row { bool present;uint64_t source_bytes;uint8_t source_sha256[32]; struct record_info info; enum upload_state state; uint32_t offset; uint64_t retry, export_id, expires; char error[40]; };
struct uploader {
    pthread_t thread; pthread_mutex_t lock,policy_lock,reply_lock; pthread_cond_t wake;
    bool stop, ready, inflight, answered, reset;
    int dir;struct transfer_journal *journal;uint64_t storage_generation;
    uint8_t target[32];
    struct { char serial[32]; bool enabled; } policy[4];
    struct upload_row rows[RECORD_LIMIT],exports[EXPORT_LIMIT];
    unsigned rate_kib; uint64_t send_after;
    struct record_info snapshot[RECORD_LIMIT];
    struct myvava_message request, answer, mailbox;
    uint8_t mailbox_data[33];bool mailbox_ready,mailbox_reset;
    uint8_t payload[MYVAVA_FILE_PAYLOAD_MAX], ack[33];
    bool file_batch;
    struct myvava_file_channel channel;
};
static bool allowed(struct uploader *u,const struct record_info *r) {
    pthread_mutex_lock(&u->policy_lock);bool ok=u->policy[r->slot].enabled&&!strcmp(u->policy[r->slot].serial,r->serial);pthread_mutex_unlock(&u->policy_lock);return ok;
}
static struct upload_row *row_at(struct uploader *u,unsigned i) { return i<RECORD_LIMIT?&u->rows[i]:&u->exports[i-RECORD_LIMIT]; }
static bool row_allowed(struct uploader *u,unsigned i) {
    if(!u->journal||recorder_storage_generation()!=u->storage_generation)return false;
    struct upload_row *r=row_at(u,i);
    if(r->state==U_CANCELLED||r->state==U_EXPIRED||r->state==U_DESTINATION)return false;
    if(!r->export_id)return allowed(u,&r->info);
    /* Explicit TF retrieval is authorised by the immutable clip identity.
     * Historical clips remain retrievable after unpairing or slot reuse;
     * automatic cloud uploads still follow the current camera policy above. */
    return r->expires>(uint64_t)time(NULL);
}
static bool export_save(struct uploader *,const struct upload_row *);
static bool job_save(struct uploader *,const struct upload_row *);
static void status(struct uploader *u,unsigned i,enum upload_state s,uint32_t offset,const char *error) {
    pthread_mutex_lock(&u->lock);struct upload_row *r=row_at(u,i);
    if(r->state!=U_CANCELLED&&r->state!=U_EXPIRED){
        r->state=s;r->offset=offset;snprintf(r->error,sizeof(r->error),"%s",error?error:"");
        if(s==U_RETRY)r->retry=myvava_millis()+30000;
        if(s==U_FAILED||s==U_COMMITTED||s==U_EXPIRED||s==U_RETRY){
            bool saved=r->export_id?export_save(u,r):job_save(u,r);
            if(!saved){r->state=U_FAILED;strcpy(r->error,"transfer_journal_failed");}
        }
    }
    pthread_mutex_unlock(&u->lock);
}
static bool file_description(const struct record_info *r,uint8_t *descriptor) {
    if (!r->transport_stream || r->bytes>MYVAVA_FILE_MAX || r->frames>UINT32_MAX || r->audio_frames>UINT32_MAX) return false;
    struct myvava_file f={.id=r->id,.size=(uint32_t)r->bytes,.unix_seconds=r->unix_seconds,
        .first_ms=r->first_ms,.last_ms=r->last_ms,.frames=(uint32_t)r->frames,.audio_frames=(uint32_t)r->audio_frames,
        .sample_rate=r->sample_rate,.channels=(uint8_t)r->channels,.slot=(uint8_t)r->slot,.stream=(uint8_t)r->stream,
        .pir=r->pir,.clock_valid=r->clock_valid,.file_type=r->file_type,
        .detection_flags=(r->detection_reported?1:0)|(r->detection_reported&&r->detection_result==1?2:0)|(r->preview?4:0),.detection_type=r->detection_type};
    memcpy(f.sha256,r->sha256,32); memcpy(f.serial,r->serial,32); return myvava_file_encode(descriptor,&f);
}
static bool job_save(struct uploader *u,const struct upload_row *r){
    uint8_t b[128];struct transfer_job job={.state=r->state,.offset=r->offset,.retry_at=r->state==U_RETRY?(uint64_t)time(NULL)+30:0};
    if(!file_description(&r->info,b)||!myvava_file_decode(b,128,&job.file))return false;
    memcpy(job.target,r->info.upload_target,32);strcpy(job.error,r->error);return transfer_journal_put(u->journal,&job);
}
static bool job_restore(struct uploader *u,struct upload_row *r){
    struct transfer_job job;int found=transfer_journal_get(u->journal,r->info.id,&job);if(found<0)return false;if(!found)return job_save(u,r);
    if(memcmp(job.file.sha256,r->info.sha256,32)||job.file.size!=r->info.bytes||memcmp(job.target,r->info.upload_target,32)||job.state>U_EXPIRED)return false;
    r->state=job.state;r->offset=job.offset;strcpy(r->error,job.error);
    if(r->state==U_UPLOADING)r->state=U_QUEUED;
    if(r->state==U_QUEUED||r->state==U_RETRY){if(memcmp(job.target,u->target,32))r->state=U_DESTINATION;uint64_t now=(uint64_t)time(NULL);r->retry=myvava_millis()+(job.retry_at>now?job.retry_at-now:0)*1000;}
    return true;
}
static void put64(uint8_t *b,uint64_t v){for(int i=7;i>=0;--i){b[i]=(uint8_t)v;v>>=8;}}
static uint64_t get64(const uint8_t *b){uint64_t n=0;for(unsigned i=0;i<8;++i)n=n*256+b[i];return n;}
/* A request is durable before admission. Progress is recovered from the server;
 * terminal outcomes survive source deletion and process restart. */
static bool export_save(struct uploader *u,const struct upload_row *r){
    uint8_t data[320]={0};size_t length=r->info.preview?320:256;memcpy(data,r->info.preview?"MVEXP002":"MVEXPORT",8);put64(data+16,r->export_id);put64(data+24,r->expires);
    memcpy(data+32,u->target,32);if(!file_description(&r->info,data+64))return false;
    data[192]=(uint8_t)r->state;memcpy(data+196,r->error,40);vava_put32(data+236,r->offset);
    if(r->info.preview){put64(data+240,r->source_bytes);memcpy(data+248,r->source_sha256,32);}
    vava_put32(data+8,vava_crc32(data+16,length-16));
    char name[40],tmp[48];snprintf(name,sizeof(name),"%016" PRIx64 ".export",r->export_id);snprintf(tmp,sizeof(tmp),"%s.tmp",name);
    int fd=openat(u->dir,tmp,O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0)return false;
    struct stat st;bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&!ftruncate(fd,0);
    size_t off=0;while(ok&&off<length){ssize_t n=write(fd,data+off,length-off);if(n<0&&errno==EINTR)continue;if(n<=0)ok=false;else off+=(size_t)n;}
    ok=ok&&!fsync(fd);if(close(fd))ok=false;if(ok)ok=!renameat(u->dir,tmp,u->dir,name)&&!fsync(u->dir);return ok;
}
static bool export_load(struct uploader *u){
    DIR *d=directory_scan_open(u->dir);if(!d)return false;struct dirent *e;unsigned count=0;bool ok=true;
    while(ok&&(e=directory_scan_next(d,&ok))){if(strlen(e->d_name)!=23||strcmp(e->d_name+16,".export"))continue;
        uint8_t data[321],id[8];char hex[17];memcpy(hex,e->d_name,16);hex[16]=0;
        int fd=openat(u->dir,e->d_name,O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);struct stat st;struct myvava_file f;ssize_t length=fd>=0?read(fd,data,sizeof(data)):-1;
        ok=fd>=0&&!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&
            ((length==256&&!memcmp(data,"MVEXPORT",8))||(length==320&&!memcmp(data,"MVEXP002",8)))&&vava_u32(data+8)==vava_crc32(data+16,(size_t)length-16)&&
            myvava_unhex(hex,id,8)&&get64(id)==get64(data+16)&&(id[0]&128)&&count<EXPORT_LIMIT&&
            data[192]<=U_EXPIRED&&memchr(data+196,0,40)&&myvava_file_decode(data+64,128,&f)&&((length==320)==((f.detection_flags&4)!=0));
        if(fd>=0)close(fd);if(!ok)break;
        unsigned at=(f.detection_flags&4)?FULL_EXPORT_LIMIT:0,end=(f.detection_flags&4)?EXPORT_LIMIT:FULL_EXPORT_LIMIT;while(at<end&&u->exports[at].present)++at;if(at==end){ok=false;break;}++count;
        struct upload_row *r=&u->exports[at];*r=(struct upload_row){.present=true,.export_id=get64(data+16),.expires=get64(data+24),.state=data[192],.offset=vava_u32(data+236)};
        r->info=(struct record_info){.id=f.id,.slot=f.slot,.stream=f.stream,.bytes=f.size,.frames=f.frames,.audio_frames=f.audio_frames,
            .unix_seconds=f.unix_seconds,.first_ms=f.first_ms,.last_ms=f.last_ms,.pir=f.pir,.clock_valid=f.clock_valid,.sample_rate=f.sample_rate,.channels=f.channels,.transport_stream=true,
            .preview=(f.detection_flags&4)!=0,.file_type=f.file_type,.detection_reported=(f.detection_flags&1)!=0,.detection_result=(f.detection_flags&2)?1:0,.detection_type=f.detection_type};
        if(r->info.preview){r->source_bytes=get64(data+240);memcpy(r->source_sha256,data+248,32);if(r->source_bytes<r->info.bytes||r->info.bytes>1048476)ok=false;}
        memcpy(r->info.sha256,f.sha256,32);memcpy(r->info.serial,f.serial,32);memcpy(r->error,data+196,40);
        for(unsigned i=0;i<40&&r->error[i];++i)if(!((r->error[i]>='a'&&r->error[i]<='z')||r->error[i]=='_'))ok=false;
        if(r->state!=U_COMMITTED&&r->state!=U_FAILED&&r->state!=U_CANCELLED&&r->state!=U_EXPIRED){r->state=memcmp(data+32,u->target,32)?U_DESTINATION:U_QUEUED;if(r->expires<=(uint64_t)time(NULL))r->state=U_EXPIRED;}
    }
    closedir(d);return ok;
}
static bool receipt(struct uploader *u,const struct record_info *r,const uint8_t *desc,bool save) {
    char name[40],temp[48]; snprintf(name,sizeof(name),"%016" PRIx64 ".done",r->id); snprintf(temp,sizeof(temp),"%016" PRIx64 ".tmp",r->id);
    uint8_t data[160],old[161]; memcpy(data,u->target,32); memcpy(data+32,desc,128);
    int fd=openat(u->dir,save?temp:name,(save?O_WRONLY|O_CREAT:O_RDONLY)|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if (fd<0) return false;
    struct stat st; bool ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&0077) && st.st_nlink==1;
    if (ok && save) {
        ok=!ftruncate(fd,0); size_t off=0;
        while (ok && off<sizeof(data)) { ssize_t n=write(fd,data+off,sizeof(data)-off);
            if (n<0 && errno==EINTR) continue; if (n<=0) ok=false; else off+=(size_t)n; }
        ok=ok && !fsync(fd);
    } else if (ok) {
        ssize_t n=read(fd,old,sizeof(old)); ok=n==(ssize_t)sizeof(data) && !memcmp(data,old,sizeof(data));
    }
    if (close(fd)) ok=false;
    if (ok && save) ok=!renameat(u->dir,temp,u->dir,name) && !fsync(u->dir);
    return ok;
}
static bool peek(void *arg,struct myvava_message *m,uint8_t data[MYVAVA_FILE_PAYLOAD_MAX]) {
    struct uploader *u=arg; bool ok=false;if(pthread_mutex_trylock(&u->lock))return false;
    if (u->ready && !u->inflight && !u->reset && !u->answered) {
        *m=u->request; memcpy(data,u->payload,m->length); m->data=data; u->inflight=true; ok=true;
        /* Background cloud uploads alone use the saved rate. Pace from the
         * actual departure, letting ACK time consume this chunk's budget. */
        if(m->type==MYVAVA_FILE_DATA&&!(m->id>>63)&&u->rate_kib)
            u->send_after=myvava_millis()+(m->length*1000+u->rate_kib*1024-1)/(u->rate_kib*1024);
    }
    pthread_mutex_unlock(&u->lock); return ok;
}
static void answer(void *arg,const struct myvava_message *m) {
    struct uploader *u=arg;if(m->length!=33)return;pthread_mutex_lock(&u->reply_lock);
    u->mailbox=*m;memcpy(u->mailbox_data,m->data,33);u->mailbox_ready=true;
    pthread_mutex_unlock(&u->reply_lock);pthread_cond_signal(&u->wake);
}
static void reset(void *arg) {
    struct uploader *u=arg;pthread_mutex_lock(&u->reply_lock);u->mailbox_reset=true;pthread_mutex_unlock(&u->reply_lock);pthread_cond_signal(&u->wake);
}
static void replies(struct uploader *u,bool clear){
    pthread_mutex_lock(&u->reply_lock);
    if(!clear&&u->inflight){
        if(u->mailbox_reset)u->reset=true;
        if(u->mailbox_ready&&u->mailbox.id==u->request.id&&u->mailbox.total==u->request.total){u->answer=u->mailbox;memcpy(u->ack,u->mailbox_data,33);u->answer.data=u->ack;u->answered=true;}
    }
    u->mailbox_ready=u->mailbox_reset=false;pthread_mutex_unlock(&u->reply_lock);
}
/* Called with the upload lock held. A paced background transfer yields at a
 * settled exchange so explicit playback is not stuck behind a slow batch. */
static bool export_waiting(struct uploader *u) {
    uint64_t now=myvava_millis(),unix_now=(uint64_t)time(NULL);
    for(unsigned i=0;i<EXPORT_LIMIT;++i){struct upload_row *r=&u->exports[i];
        if(r->present&&r->expires>unix_now&&(r->state==U_QUEUED||(r->state==U_RETRY&&now>=r->retry)))return true;
    }
    return false;
}
/* The worker retains a single bounded request until network acknowledgment.
 * It never abandons an in-flight request on a policy change: settle its reply,
 * then pause. Reconnect resets force a fresh OFFER to recover server offset. */
static int exchange(struct uploader *u,unsigned index,const struct record_info *r,uint8_t type,uint32_t offset,
                    const uint8_t *data,size_t n,uint32_t *received) {
    pthread_mutex_lock(&u->lock);
    bool background=!row_at(u,index)->export_id;
    if (u->stop || !row_allowed(u,index) || (background&&export_waiting(u))) { pthread_mutex_unlock(&u->lock); return -2; }
    while(background&&!u->stop&&row_allowed(u,index)&&!export_waiting(u)&&myvava_millis()<u->send_after){
        uint64_t now=myvava_millis();if(now>=u->send_after)break;
        uint64_t remaining=u->send_after-now;if(remaining>1000)remaining=1000;
        struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=(long)remaining*1000000;if(until.tv_nsec>=1000000000){++until.tv_sec;until.tv_nsec-=1000000000;}
        pthread_cond_timedwait(&u->wake,&u->lock,&until);
    }
    if(u->stop||!row_allowed(u,index)||(background&&export_waiting(u))){pthread_mutex_unlock(&u->lock);return -2;}
    replies(u,true);
    u->request=(struct myvava_message){.type=type,.id=r->id,.total=(uint32_t)r->bytes,.offset=offset,.length=n};
    memcpy(u->payload,data,n); u->ready=true; u->inflight=u->answered=u->reset=false;
    uint64_t exchange_deadline=myvava_millis()+20000;
    while (!u->stop && !u->answered && !u->reset && recorder_storage_generation()==u->storage_generation && myvava_millis()<exchange_deadline) {
        replies(u,false);if(u->answered||u->reset)break;
        if (!u->inflight && !row_allowed(u,index)) break;
        struct timespec until;clock_gettime(CLOCK_REALTIME,&until);++until.tv_sec;
        pthread_cond_timedwait(&u->wake,&u->lock,&until);
    }
    replies(u,false);int result=-1;
    if (u->answered && !u->reset && !memcmp(u->ack,r->sha256,32) && u->answer.offset<=r->bytes && u->ack[32]<=MVF_DELETED) {
        result=u->ack[32]; *received=u->answer.offset;
        if(type==MYVAVA_FILE_OFFER)u->file_batch=u->answer.file_batch;
    }
    if (!row_allowed(u,index) && !u->inflight) result=-2;
    u->ready=u->inflight=u->answered=u->reset=false;
    pthread_mutex_unlock(&u->lock); return result;
}
static void transfer(struct uploader *u,unsigned index) {
    pthread_mutex_lock(&u->lock);struct upload_row saved=*row_at(u,index);pthread_mutex_unlock(&u->lock);
    struct record_info r=saved.info;uint64_t source_id=r.id;if(saved.export_id)r.id=saved.export_id; uint8_t desc[MYVAVA_FILE_DESC],data[MYVAVA_FILE_PAYLOAD_MAX];
    if (!file_description(&r,desc)) { status(u,index,U_FAILED,0,"unsupported_recording"); return; }
    if (receipt(u,&r,desc,false)) { status(u,index,U_COMMITTED,(uint32_t)r.bytes,NULL); return; }
    struct record_reader reader={.fd=-1}; uint32_t offset=saved.offset; int result;
    status(u,index,U_UPLOADING,saved.offset,NULL);
    result=exchange(u,index,&r,MYVAVA_FILE_OFFER,0,desc,sizeof(desc),&offset);
    if(result==MVF_PARTIAL)status(u,index,U_UPLOADING,offset,NULL);
    unsigned chunks=0;
    while (result==MVF_PARTIAL && offset<r.bytes) {
        if (reader.fd<0 || myvava_millis()+1000>=reader.deadline_ms) {
            recorder_reader_close(&reader);
            if (!recorder_reader_open(r.slot,r.serial,source_id,&reader)) {
                status(u,index,recorder_available()?U_EXPIRED:U_RETRY,offset,"source_unavailable"); return;
            }
            if (reader.info.bytes!=(r.preview?saved.source_bytes:r.bytes) || memcmp(reader.info.sha256,r.preview?saved.source_sha256:r.sha256,32) ||
                (!saved.export_id&&memcmp(reader.info.upload_target,r.upload_target,32))) {
                status(u,index,U_FAILED,offset,"source_identity_changed"); recorder_reader_close(&reader); return;
            }
        }
        /* Interactive TF fetches use negotiated batches; background cloud
         * keeps its small paced exchanges so it can yield promptly. */
        size_t maximum=saved.export_id&&u->file_batch?MYVAVA_FILE_BATCH_DATA_MAX:MYVAVA_FILE_DATA_MAX;
        size_t n=(size_t)(r.bytes-offset); if (n>maximum) n=maximum;
        int got=recorder_reader_read(&reader,offset,data+32,n);
        if (got!=(int)n) { status(u,index,U_RETRY,offset,"source_read_failed"); recorder_reader_close(&reader); return; }
        recorder_reader_close(&reader); /* no source lease is held while the network is offline */
        memcpy(data,r.sha256,32); uint32_t expected=offset+(uint32_t)n;
        result=exchange(u,index,&r,MYVAVA_FILE_DATA,offset,data,n+32,&offset);
        if (result==MVF_PARTIAL && offset!=expected) { result=MVF_CONFLICT; break; }
        status(u,index,U_UPLOADING,offset,NULL);
        if(result==MVF_PARTIAL&&offset<r.bytes&&++chunks==32){recorder_reader_close(&reader);status(u,index,U_QUEUED,offset,NULL);return;}
    }
    recorder_reader_close(&reader);
    if (result==MVF_PARTIAL && offset==r.bytes) result=exchange(u,index,&r,MYVAVA_FILE_COMMIT,offset,r.sha256,32,&offset);
    if (result==MVF_COMMITTED && offset==r.bytes) {
        if (receipt(u,&r,desc,true)) status(u,index,U_COMMITTED,offset,NULL);
        else status(u,index,U_RETRY,offset,"local_receipt_write_failed");
    } else if (result==-2) status(u,index,U_QUEUED,offset,NULL);
    else if (result==MVF_CONFLICT || result==MVF_DIGEST || result==MVF_INVALID || result==MVF_DELETED)
        status(u,index,U_FAILED,offset,result==MVF_DELETED?"server_deleted_file":result==MVF_DIGEST?"server_digest_mismatch":result==MVF_CONFLICT?"server_identity_conflict":"server_rejected_file");
    else status(u,index,U_RETRY,offset,result==MVF_QUOTA?"server_quota_full":result==MVF_IO?"server_storage_unavailable":"connection_interrupted");
}
static bool open_storage(struct uploader *u){
    struct stat st;int parent=recorder_directory_fd();if(parent<0)return false;
    bool created=!mkdirat(parent,".remote",0700)||errno==EEXIST;
    u->dir=created?openat(parent,".remote",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC):-1;close(parent);
    if(u->dir<0)return false;
    if(fstat(u->dir,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077))return false;
    u->journal=transfer_journal_open(u->dir);if(!u->journal||!export_load(u))return false;u->rate_kib=128;
    int rate_fd=openat(u->dir,"rate.bin",O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);
    if(rate_fd>=0){uint8_t b[17];struct stat st;bool valid=!fstat(rate_fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&read(rate_fd,b,sizeof(b))==16&&!memcmp(b,"MVRATE01",8)&&vava_u32(b+12)==vava_crc32(b,12)&&vava_u32(b+8)>=1&&vava_u32(b+8)<=4096;close(rate_fd);if(!valid)return false;u->rate_kib=vava_u32(b+8);}else if(errno!=ENOENT)return false;
    return true;
}
static bool sync_storage(struct uploader *u){
    uint64_t generation=recorder_storage_generation();
    pthread_mutex_lock(&u->lock);
    if(generation&&u->journal&&u->storage_generation==generation){pthread_mutex_unlock(&u->lock);return true;}
    transfer_journal_close(u->journal);u->journal=NULL;if(u->dir>=0)close(u->dir);u->dir=-1;u->storage_generation=0;
    memset(u->rows,0,sizeof(u->rows));memset(u->exports,0,sizeof(u->exports));u->ready=u->inflight=u->answered=u->reset=false;
    pthread_mutex_unlock(&u->lock);
    if(!generation)return false;
    /* Build the potentially large journal outside the network mutex. */
    struct uploader *stage=calloc(1,sizeof(*stage));if(!stage)return false;stage->dir=-1;memcpy(stage->target,u->target,32);
    bool ok=open_storage(stage)&&generation==recorder_storage_generation();
    pthread_mutex_lock(&u->lock);
    if(ok){u->dir=stage->dir;u->journal=stage->journal;u->rate_kib=stage->rate_kib;memcpy(u->exports,stage->exports,sizeof(u->exports));u->storage_generation=generation;}
    pthread_mutex_unlock(&u->lock);
    if(!ok){transfer_journal_close(stage->journal);if(stage->dir>=0)close(stage->dir);}free(stage);return ok;
}
static void *worker(void *arg) {
    struct uploader *u=arg; struct record_info *records=u->snapshot;
    unsigned cursor=0;uint64_t before=0,history_before=0;
    for (;;) {
        if(!sync_storage(u)){
            before=history_before=0;pthread_mutex_lock(&u->lock);bool stop=u->stop;
            struct timespec until;clock_gettime(CLOCK_REALTIME,&until);++until.tv_sec;if(!stop)pthread_cond_timedwait(&u->wake,&u->lock,&until);
            pthread_mutex_unlock(&u->lock);if(stop)break;continue;
        }
        uint64_t captured_generation;unsigned count=recorder_snapshot_generation(before,records,RECORD_LIMIT,&captured_generation);
        pthread_mutex_lock(&u->lock);
        if(captured_generation!=u->storage_generation){pthread_mutex_unlock(&u->lock);continue;}
        if (u->stop) { pthread_mutex_unlock(&u->lock); break; }
        for (unsigned i=0;i<RECORD_LIMIT;++i) u->rows[i].present=false;
        for (unsigned k=0;k<count;++k) {
            struct record_info *r=&records[k]; uint8_t v=0; for (unsigned i=0;i<32;++i) v|=r->upload_target[i];
            if (!v) continue;
            unsigned i=0; while (i<RECORD_LIMIT && !(u->rows[i].info.id==r->id && !memcmp(u->rows[i].info.sha256,r->sha256,32))) ++i;
            if (i<RECORD_LIMIT) u->rows[i].present=true;
        }
        for (unsigned i=0;i<RECORD_LIMIT;++i) if (!u->rows[i].present) memset(&u->rows[i],0,sizeof(u->rows[i]));
        for (unsigned k=0;k<count;++k) {
            struct record_info *r=&records[k]; uint8_t v=0; for (unsigned i=0;i<32;++i) v|=r->upload_target[i];
            if (!v) continue;
            unsigned i=0; while (i<RECORD_LIMIT && u->rows[i].info.id!=r->id) ++i;
            if (i<RECORD_LIMIT) continue;
            for (i=0;i<RECORD_LIMIT && u->rows[i].present;++i) {}
            if (i==RECORD_LIMIT) continue;
            u->rows[i]=(struct upload_row){.present=true,.info=*r,.state=memcmp(r->upload_target,u->target,32)?U_DESTINATION:U_QUEUED};
            if(!job_restore(u,&u->rows[i])){u->rows[i].state=U_FAILED;strcpy(u->rows[i].error,"transfer_journal_failed");}
        }
        struct transfer_job history[16];unsigned hn=transfer_journal_scan(u->journal,history_before,history,16);
        if(hn!=UINT32_MAX){for(unsigned h=0;h<hn;++h){struct transfer_job *j=&history[h];struct record_info info;
            if((j->state==U_QUEUED||j->state==U_RETRY||j->state==U_UPLOADING||j->state==U_DESTINATION)&&recorder_available()&&!recorder_info(j->file.slot,j->file.serial,j->file.id,&info)&&recorder_storage_generation()==u->storage_generation){
                j->state=U_EXPIRED;strcpy(j->error,"source_expired");(void)transfer_journal_put(u->journal,j);
            }
            if(transfer_journal_count(u->journal)>61440&&(j->state==2||j->state==6||j->state==7)&&recorder_available()&&!recorder_info(j->file.slot,j->file.serial,j->file.id,&info)&&recorder_storage_generation()==u->storage_generation){char receipt[40];snprintf(receipt,sizeof(receipt),"%016" PRIx64 ".done",j->file.id);if((!unlinkat(u->dir,receipt,0)||errno==ENOENT)&&!fsync(u->dir))(void)transfer_journal_delete(u->journal,j->file.id);}
        }history_before=hn==16?history[hn-1].file.id:0;}
        int selected=-1; uint64_t now=myvava_millis();
        for (unsigned k=0;k<RECORD_LIMIT+EXPORT_LIMIT;++k) {
            unsigned i=(cursor+k)%(RECORD_LIMIT+EXPORT_LIMIT); struct upload_row *row=row_at(u,i);
            if(row->export_id&&row->expires<=(uint64_t)time(NULL)&&(row->state==U_QUEUED||row->state==U_RETRY)){row->state=U_EXPIRED;strcpy(row->error,"export_expired");(void)export_save(u,row);}
            if (row->present && row_allowed(u,i) && (row->state==U_QUEUED || (row->state==U_RETRY && now>=row->retry))) { selected=(int)i; cursor=(i+1)%(RECORD_LIMIT+EXPORT_LIMIT); break; }
        }
        pthread_mutex_unlock(&u->lock);
        if (selected>=0) transfer(u,(unsigned)selected);
        else {
            before=count==RECORD_LIMIT?records[count-1].id:0;
            pthread_mutex_lock(&u->lock);
            struct timespec until; clock_gettime(CLOCK_REALTIME,&until); ++until.tv_sec;
            if (!u->stop) pthread_cond_timedwait(&u->wake,&u->lock,&until);
            pthread_mutex_unlock(&u->lock);
        }
    }
    return NULL;
}
struct uploader *upload_start(const char *record_dir,const struct myvava_config *cfg) {
    if (!cfg->host || !cfg->device_id || !cfg->key_file || !myvava_id_valid(cfg->device_id) || !cfg->port || cfg->port>65535) return NULL;
    uint8_t key[32]; if (!myvava_key_read(cfg->key_file,key)) return NULL;
    struct uploader *u=calloc(1,sizeof(*u)); if (!u) return NULL;
    char dest[512]; int n=snprintf(dest,sizeof(dest),"myvava-upload-v1\n%s\n%u\n%s\n",cfg->host,cfg->port,cfg->device_id);
    EVP_MD_CTX *md=EVP_MD_CTX_new(); unsigned length=0;
    bool ok=md && n>0 && n<(int)sizeof(dest) && EVP_DigestInit_ex(md,EVP_sha256(),NULL)==1 &&
        EVP_DigestUpdate(md,dest,(size_t)n)==1 && EVP_DigestUpdate(md,key,32)==1 && EVP_DigestFinal_ex(md,u->target,&length)==1 && length==32;
    EVP_MD_CTX_free(md); memset(key,0,sizeof(key)); if (!ok) { free(u); return NULL; }
    (void)record_dir;u->dir=-1;u->rate_kib=128;
    u->channel=(struct myvava_file_channel){u,peek,answer,reset};
    if(pthread_mutex_init(&u->reply_lock,NULL))goto fail;
    if(pthread_mutex_init(&u->policy_lock,NULL)){pthread_mutex_destroy(&u->reply_lock);goto fail;}
    if (pthread_mutex_init(&u->lock,NULL)) {pthread_mutex_destroy(&u->policy_lock);pthread_mutex_destroy(&u->reply_lock);goto fail;}
    if (pthread_cond_init(&u->wake,NULL)) { pthread_mutex_destroy(&u->lock);pthread_mutex_destroy(&u->policy_lock);pthread_mutex_destroy(&u->reply_lock);goto fail; }
    if (pthread_create(&u->thread,NULL,worker,u)) { pthread_cond_destroy(&u->wake); pthread_mutex_destroy(&u->lock);pthread_mutex_destroy(&u->policy_lock);pthread_mutex_destroy(&u->reply_lock);goto fail; }
    return u;
fail:
    transfer_journal_close(u->journal);close(u->dir); free(u); return NULL;
}
void upload_stop(struct uploader *u) {
    if (!u) return;
    pthread_mutex_lock(&u->lock); u->stop=true; pthread_cond_signal(&u->wake); pthread_mutex_unlock(&u->lock);
    pthread_join(u->thread,NULL); pthread_mutex_destroy(&u->lock);pthread_mutex_destroy(&u->policy_lock);pthread_mutex_destroy(&u->reply_lock); pthread_cond_destroy(&u->wake);transfer_journal_close(u->journal); close(u->dir); free(u);
}
const uint8_t *upload_target(struct uploader *u) { return u?u->target:NULL; }
bool upload_storage_ready(struct uploader *u){if(!u)return false;if(pthread_mutex_trylock(&u->lock))return false;bool ok=u->journal&&u->storage_generation==recorder_storage_generation();pthread_mutex_unlock(&u->lock);return ok;}
const struct myvava_file_channel *upload_channel(struct uploader *u) { return u?&u->channel:NULL; }
void upload_policy(struct uploader *u,unsigned slot,const char *serial,bool enabled) {
    if (!u || slot>3 || strlen(serial)>=32) return;
    pthread_mutex_lock(&u->policy_lock); strcpy(u->policy[slot].serial,serial); u->policy[slot].enabled=enabled;
    pthread_cond_signal(&u->wake); pthread_mutex_unlock(&u->policy_lock);
}
bool upload_json(struct uploader *u,unsigned slot,const char *serial,unsigned page,char *out,size_t size) {
    if(!u||slot>3||page>4095)return false;static const char *names[]={"queued","uploading","committed","retry_wait","failed","destination_changed","cancelled","source_expired"};
    pthread_mutex_lock(&u->lock);if(!u->journal||u->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&u->lock);return false;}struct transfer_job rows[16];unsigned total=0,count=transfer_journal_page(u->journal,slot,serial,page,rows,16,&total);
    if(count==UINT32_MAX){pthread_mutex_unlock(&u->lock);return false;}
    pthread_mutex_lock(&u->policy_lock);bool policy_enabled=u->policy[slot].enabled&&!strcmp(u->policy[slot].serial,serial);pthread_mutex_unlock(&u->policy_lock);
    size_t used=(size_t)snprintf(out,size,"{\"ok\":true,\"total\":%u,\"page\":%u,\"uploads\":[",total,page);
    for(unsigned i=0;i<count&&used<size;++i){struct transfer_job *r=&rows[i];for(unsigned k=0;k<RECORD_LIMIT;++k)if(u->rows[k].present&&u->rows[k].info.id==r->file.id){r->state=u->rows[k].state;r->offset=u->rows[k].offset;strcpy(r->error,u->rows[k].error);break;}
        if(r->state>U_EXPIRED){pthread_mutex_unlock(&u->lock);return false;}const char *state=names[r->state];
        if((r->state==U_QUEUED||r->state==U_RETRY||r->state==U_UPLOADING)&&!policy_enabled)state="paused";
        used+=(size_t)snprintf(out+used,size-used,"%s{\"id\":\"%016" PRIx64 "\",\"state\":\"%s\",\"bytes\":%u,\"offset\":%u,\"error\":\"%s\"}",i?",":"",r->file.id,state,r->file.size,r->offset,r->error);
    }
    if(used<size)used+=(size_t)snprintf(out+used,size-used,"]}\n");pthread_mutex_unlock(&u->lock);return used<size;
}

static bool export_request(struct uploader *u,unsigned slot,const char *serial,uint64_t source_id,uint64_t export_id,const struct record_info *prefix,const struct record_info *source){
    if(!u||slot>3||!(export_id>>63)||source_id>>63)return false;
    pthread_mutex_lock(&u->lock);if(!u->journal||u->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&u->lock);return false;}
    for(unsigned i=0;i<EXPORT_LIMIT;++i)if(u->exports[i].present&&u->exports[i].export_id==export_id){
        struct upload_row *r=&u->exports[i];
        bool same=r->info.preview==(prefix!=NULL)&&r->info.id==source_id&&r->info.slot==slot&&!strcmp(r->info.serial,serial);
        if(same&&prefix){
            same=r->info.bytes==prefix->bytes&&!memcmp(r->info.sha256,prefix->sha256,32)&&r->state!=U_COMMITTED&&strcmp(r->error,"server_deleted_file");
            if(same&&(r->state==U_FAILED||r->state==U_CANCELLED||r->state==U_EXPIRED||r->state==U_RETRY)){
                struct upload_row next=*r;next.state=U_QUEUED;next.expires=(uint64_t)time(NULL)+120;next.error[0]=0;
                same=recorder_hold(slot,serial,source_id,next.expires)&&export_save(u,&next);if(same)*r=next;
            }
        }
        pthread_cond_signal(&u->wake);pthread_mutex_unlock(&u->lock);return same;
    }
    unsigned begin=prefix?FULL_EXPORT_LIMIT:0,end=prefix?EXPORT_LIMIT:FULL_EXPORT_LIMIT;
    unsigned i=begin;while(i<end&&u->exports[i].present)++i;
    /* Expired terminal history must not permanently exhaust the bounded table
     * after viewers re-fetch an expired server cache. Never reclaim active I/O. */
    if(i==end){uint64_t now=(uint64_t)time(NULL);
        for(unsigned k=begin;k<end;++k){struct upload_row *r=&u->exports[k];
            if(now<1577836800||!r->expires||(!r->info.preview&&now<r->expires)||((u->ready||u->inflight)&&u->request.id==r->export_id))continue;
            if(r->state!=U_COMMITTED&&r->state!=U_FAILED&&r->state!=U_CANCELLED&&r->state!=U_EXPIRED)continue;
            char name[40];snprintf(name,sizeof(name),"%016" PRIx64 ".export",r->export_id);
            if(unlinkat(u->dir,name,0)||fsync(u->dir))break;
            snprintf(name,sizeof(name),"%016" PRIx64 ".done",r->export_id);(void)unlinkat(u->dir,name,0);
            memset(r,0,sizeof(*r));i=k;break;
        }
    }
    struct record_info info;bool ok=i<end&&recorder_info(slot,serial,source_id,&info)&&info.transport_stream;
    if(ok&&prefix)ok=source&&info.bytes==source->bytes&&!memcmp(info.sha256,source->sha256,32)&&prefix->preview;
    if(ok){struct upload_row r={.present=true,.info=prefix?*prefix:info,.export_id=export_id,.expires=(uint64_t)time(NULL)+(prefix?120:3600),.state=U_QUEUED};
        if(prefix){r.source_bytes=info.bytes;memcpy(r.source_sha256,info.sha256,32);}
        ok=recorder_hold(slot,serial,source_id,r.expires)&&export_save(u,&r);if(ok)u->exports[i]=r;}
    pthread_cond_signal(&u->wake);pthread_mutex_unlock(&u->lock);return ok;
}
bool upload_export(struct uploader *u,unsigned slot,const char *serial,uint64_t source_id,uint64_t export_id){return export_request(u,slot,serial,source_id,export_id,NULL,NULL);}
bool upload_preview(struct uploader *u,unsigned slot,const char *serial,uint64_t source_id,uint64_t export_id){
    struct record_info prefix,source;
    return recorder_preview(slot,serial,source_id,&prefix,&source)&&export_request(u,slot,serial,source_id,export_id,&prefix,&source);
}
bool upload_export_action(struct uploader *u,unsigned slot,const char *serial,uint64_t id,bool cancel){
    if(!u)return false;pthread_mutex_lock(&u->lock);if(!u->journal||u->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&u->lock);return false;}bool ok=false;
    for(unsigned i=0;i<EXPORT_LIMIT;++i){struct upload_row *r=&u->exports[i];if(!r->present||r->export_id!=id||r->info.slot!=slot||strcmp(r->info.serial,serial))continue;
        if(cancel&&r->state!=U_COMMITTED){struct upload_row next=*r;next.state=U_CANCELLED;strcpy(next.error,"user_cancelled");ok=export_save(u,&next);if(ok)*r=next;}
        else if(!cancel&&(r->state==U_FAILED||r->state==U_CANCELLED||r->state==U_EXPIRED||r->state==U_RETRY)&&!strcmp(r->error,"server_deleted_file")){ok=false;}
        else if(!cancel&&r->state!=U_DESTINATION&&r->state!=U_COMMITTED){struct record_info info;if(recorder_info(slot,serial,r->info.id,&info)&&info.bytes==r->info.bytes&&!memcmp(info.sha256,r->info.sha256,32)){struct upload_row next=*r;next.state=U_QUEUED;next.expires=(uint64_t)time(NULL)+3600;next.error[0]=0;ok=recorder_hold(slot,serial,next.info.id,next.expires)&&export_save(u,&next);if(ok)*r=next;}}
        break;
    }
    pthread_cond_signal(&u->wake);pthread_mutex_unlock(&u->lock);return ok;
}
bool upload_exports_json(struct uploader *u,unsigned slot,const char *serial,char *out,size_t size){
    if(!u||slot>3)return false;static const char *names[]={"queued","uploading","committed","retry_wait","failed","destination_changed","cancelled","source_expired"};
    pthread_mutex_lock(&u->lock);if(!u->journal||u->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&u->lock);return false;}size_t n=(size_t)snprintf(out,size,"{\"ok\":true,\"exports\":[");unsigned count=0;
    for(unsigned i=0;i<EXPORT_LIMIT&&n<size;++i){struct upload_row *r=&u->exports[i];if(!r->present||r->info.preview||r->info.slot!=slot||strcmp(r->info.serial,serial))continue;
        n+=(size_t)snprintf(out+n,size-n,"%s{\"id\":\"%016" PRIx64 "\",\"source_id\":\"%016" PRIx64 "\",\"state\":\"%s\",\"offset\":%u,\"bytes\":%" PRIu64 ",\"expires\":%" PRIu64 ",\"error\":\"%s\"}",count++?",":"",r->export_id,r->info.id,names[r->state],r->offset,r->info.bytes,r->expires,r->error);
    }
    if(n<size)n+=(size_t)snprintf(out+n,size-n,"],\"rate_kib\":%u,\"rate_scope\":\"cloud_upload\",\"retrieval_rate_limited\":false}\n",u->rate_kib);pthread_mutex_unlock(&u->lock);return n<size;
}
static bool rate_save(struct uploader *u,unsigned kib){
    uint8_t b[16]={0};memcpy(b,"MVRATE01",8);vava_put32(b+8,kib);vava_put32(b+12,vava_crc32(b,12));
    int fd=openat(u->dir,"rate.tmp",O_WRONLY|O_CREAT|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC,0600);struct stat st;if(fd<0)return false;
    bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&!ftruncate(fd,0)&&write(fd,b,16)==16&&!fsync(fd);if(close(fd))ok=false;
    return ok&&!renameat(u->dir,"rate.tmp",u->dir,"rate.bin")&&!fsync(u->dir);
}
bool upload_rate(struct uploader *u,unsigned kib){if(!u||kib<1||kib>4096)return false;pthread_mutex_lock(&u->lock);bool ok=u->journal&&u->storage_generation==recorder_storage_generation()&&rate_save(u,kib);if(ok)u->rate_kib=kib;pthread_cond_signal(&u->wake);pthread_mutex_unlock(&u->lock);return ok;}
unsigned upload_get_rate(struct uploader *u){if(!u)return 0;pthread_mutex_lock(&u->lock);unsigned rate=u->rate_kib;pthread_mutex_unlock(&u->lock);return rate;}
bool upload_action(struct uploader *u,unsigned slot,const char *serial,uint64_t id,const char *action){
    if(!u)return false;pthread_mutex_lock(&u->lock);if(!u->journal||u->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&u->lock);return false;}struct transfer_job j;bool ok=transfer_journal_get(u->journal,id,&j)==1&&j.file.slot==slot&&!strcmp(j.file.serial,serial);
    unsigned index=0;while(index<RECORD_LIMIT&&!(u->rows[index].present&&u->rows[index].info.id==id))++index;
    struct record_info info;bool present=recorder_info(slot,serial,id,&info);
    if(ok&&!strcmp(action,"forget")){
        ok=!present&&(j.state==U_COMMITTED||j.state==U_CANCELLED||j.state==U_EXPIRED||j.state==U_FAILED)&&!(u->ready&&u->request.id==id)&&transfer_journal_delete(u->journal,id);
        if(ok){char file[40];snprintf(file,sizeof(file),"%016" PRIx64 ".done",id);(void)unlinkat(u->dir,file,0);}
    }else if(ok){
        if(!strcmp(action,"cancel")&&j.state!=U_COMMITTED){j.state=U_CANCELLED;strcpy(j.error,"user_cancelled");}
        else if(!strcmp(action,"retry")&&j.state!=U_COMMITTED&&j.state!=U_DESTINATION&&strcmp(j.error,"server_deleted_file")&&present&&info.bytes==j.file.size&&!memcmp(info.sha256,j.file.sha256,32)&&!memcmp(j.target,u->target,32)){j.state=U_QUEUED;j.error[0]=0;j.retry_at=0;}
        else ok=false;
        if(ok)ok=transfer_journal_put(u->journal,&j);
        if(ok&&index<RECORD_LIMIT){u->rows[index].state=j.state;u->rows[index].retry=0;strcpy(u->rows[index].error,j.error);}
    }
    pthread_cond_signal(&u->wake);pthread_mutex_unlock(&u->lock);return ok;
}
bool upload_export_forget(struct uploader *u,unsigned slot,const char *serial,uint64_t id){
    if(!u)return false;pthread_mutex_lock(&u->lock);if(!u->journal||u->storage_generation!=recorder_storage_generation()){pthread_mutex_unlock(&u->lock);return false;}bool ok=false;
    for(unsigned i=0;i<EXPORT_LIMIT;++i){struct upload_row *r=&u->exports[i];if(!r->present||r->export_id!=id||r->info.slot!=slot||strcmp(r->info.serial,serial))continue;
        /* A cancellation can still be settling an in-flight network request. */
        if((u->ready||u->inflight)&&u->request.id==id)break;
        if(r->state==U_COMMITTED||r->state==U_FAILED||r->state==U_CANCELLED||r->state==U_EXPIRED){
            char name[40];snprintf(name,sizeof(name),"%016" PRIx64 ".export",id);ok=!unlinkat(u->dir,name,0)&&!fsync(u->dir);
            if(ok){memset(r,0,sizeof(*r));snprintf(name,sizeof(name),"%016" PRIx64 ".done",id);(void)unlinkat(u->dir,name,0);}
        }break;
    }
    pthread_mutex_unlock(&u->lock);return ok;
}
