#include "media.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
/* Camera TCP can deliver audio and video in one read burst. Keep a bounded
 * FIFO per camera so the network thread can drain both without destroying GOPs. */
#define FRAME_QUEUE_COUNT 16
#define FRAME_QUEUE_BYTES (512u*1024u)
struct frame {uint64_t lease,generation,sequence,timestamp;size_t size,offset;uint32_t epoch;bool audio,key;uint8_t *data;};
struct frame_queue {struct frame frames[FRAME_QUEUE_COUNT];unsigned head,count;size_t bytes;};
static void queue_pop(struct frame_queue *q){struct frame *f=&q->frames[q->head];q->bytes-=f->size;free(f->data);memset(f,0,sizeof(*f));q->head=(q->head+1)%FRAME_QUEUE_COUNT;--q->count;}
static void queue_clear(struct frame_queue *q){while(q->count)queue_pop(q);q->head=0;}
struct mvm_client {pthread_mutex_t lock;uint64_t until,sequence[4],talk_until;struct mvm_lease leases[4];struct mvm_talk talk;uint64_t ack_lease;uint32_t ack_sequence;struct frame_queue queues[4];bool needs_key[4];uint32_t epoch[4];unsigned cursor;};
uint64_t mvm_u64(const uint8_t *p){uint64_t n=0;for(unsigned i=0;i<8;++i)n=n*256+p[i];return n;}
void mvm_p64(uint8_t *p,uint64_t n){for(unsigned i=0;i<8;++i)p[7-i]=(uint8_t)(n>>(8*i));}
struct mvm_client *mvm_client_new(void){struct mvm_client *c=calloc(1,sizeof(*c));if(c&&pthread_mutex_init(&c->lock,NULL)){free(c);c=NULL;}return c;}
void mvm_client_free(struct mvm_client *c){if(c){for(unsigned i=0;i<4;++i)queue_clear(&c->queues[i]);pthread_mutex_destroy(&c->lock);free(c);}}
void mvm_client_reset(struct mvm_client *c){if(!c)return;pthread_mutex_lock(&c->lock);c->until=0;memset(c->leases,0,sizeof(c->leases));memset(&c->talk,0,sizeof(c->talk));for(unsigned i=0;i<4;++i){queue_clear(&c->queues[i]);c->needs_key[i]=true;}pthread_mutex_unlock(&c->lock);}
bool mvm_client_active(struct mvm_client *c){if(!c)return false;pthread_mutex_lock(&c->lock);bool active=false;if(myvava_millis()<c->until)for(unsigned i=0;i<4;++i)active|=c->leases[i].id!=0;pthread_mutex_unlock(&c->lock);return active;}
bool mvm_client_control(struct mvm_client *c,const uint8_t *p,size_t n){return mvm_client_control_version(c,p,n,false);}
bool mvm_client_control_version(struct mvm_client *c,const uint8_t *p,size_t n,bool v2){
    if(!c||n<MVM_CONTROL||n>MVM_CONTROL+16+MVM_TALK_MAX)return false;
    struct mvm_lease leases[4]={0};struct mvm_talk talk={0};
    for(unsigned i=0;i<4;++i){const uint8_t *s=p+i*48;leases[i].id=mvm_u64(s);memcpy(leases[i].serial,s+8,32);leases[i].talk=s[40];
        if(s[40]>1||!memchr(s+8,0,32))return false;
        leases[i].framing=s[41];if(s[41]&&(!v2||s[41]!=MVM_FRAMING_VERSION))return false;
        for(unsigned k=42;k<48;++k)if(s[k])return false;
        if(leases[i].id&&(!myvava_id_valid(leases[i].serial)||strlen(leases[i].serial)>31))return false;
    }
    if(n>MVM_CONTROL){const uint8_t *s=p+MVM_CONTROL;if(n<MVM_CONTROL+16)return false;talk.lease=mvm_u64(s);talk.sequence=(uint32_t)s[8]<<24|(uint32_t)s[9]<<16|(uint32_t)s[10]<<8|s[11];talk.slot=s[12];talk.length=(size_t)s[14]*256+s[15];
        if(s[13]||talk.slot>3||!talk.sequence||!talk.lease||talk.length<8||talk.length>MVM_TALK_MAX||n!=MVM_CONTROL+16+talk.length||leases[talk.slot].id!=talk.lease||!leases[talk.slot].talk)return false;memcpy(talk.data,s+16,talk.length);
    }
    pthread_mutex_lock(&c->lock);for(unsigned i=0;i<4;++i)if(leases[i].id!=c->leases[i].id||strcmp(leases[i].serial,c->leases[i].serial)||leases[i].framing!=c->leases[i].framing){c->epoch[i]=0;queue_clear(&c->queues[i]);c->needs_key[i]=true;}
    memcpy(c->leases,leases,sizeof(leases));c->until=myvava_millis()+MVM_LEASE_MS;
    if(talk.lease&&(talk.lease!=c->ack_lease||talk.sequence>c->ack_sequence)&&!c->talk.lease){c->talk=talk;c->talk_until=myvava_millis()+500;}
    pthread_mutex_unlock(&c->lock);return true;
}
void mvm_client_poll(struct mvm_client *c,struct myvava_message *m){pthread_mutex_lock(&c->lock);*m=(struct myvava_message){.type=MYVAVA_MEDIA_POLL,.id=c->ack_lease,.offset=c->ack_sequence};pthread_mutex_unlock(&c->lock);}
void mvm_client_snapshot(struct mvm_client *c,struct mvm_lease leases[4],struct mvm_talk *talk){memset(leases,0,4*sizeof(*leases));memset(talk,0,sizeof(*talk));if(!c)return;pthread_mutex_lock(&c->lock);
    if(myvava_millis()<c->until){memcpy(leases,c->leases,4*sizeof(*leases));if(c->talk.lease){if(myvava_millis()<c->talk_until)*talk=c->talk;c->ack_lease=c->talk.lease;c->ack_sequence=c->talk.sequence;c->talk.lease=0;}}
    pthread_mutex_unlock(&c->lock);
}
void mvm_client_frame(struct mvm_client *c,unsigned slot,uint64_t lease,uint64_t generation,bool audio,bool key,uint64_t timestamp,const uint8_t *data,size_t n,const uint8_t *sps,size_t ns,const uint8_t *pps,size_t np){
    mvm_client_frame_epoch(c,slot,lease,generation,1,audio,key,timestamp,data,n,sps,ns,pps,np);
}
void mvm_client_frame_epoch(struct mvm_client *c,unsigned slot,uint64_t lease,uint64_t generation,uint32_t epoch,bool audio,bool key,uint64_t timestamp,const uint8_t *data,size_t n,const uint8_t *sps,size_t ns,const uint8_t *pps,size_t np){
    if(!c||slot>3||!n||n>MVM_FRAME_MAX||ns>MVM_FRAME_MAX-n||np>MVM_FRAME_MAX-n-ns)return;pthread_mutex_lock(&c->lock);struct frame_queue *q=&c->queues[slot];
    if(lease&&lease==c->leases[slot].id&&myvava_millis()<c->until){
        if(c->leases[slot].framing==2&&!audio&&epoch!=c->epoch[slot]){
            if(!epoch||epoch<c->epoch[slot]||!key||!ns||!np){pthread_mutex_unlock(&c->lock);return;}
            /* Preserve queued audio and its clock; remove old video including
             * a partially transmitted frame. Sequence numbers are not reused. */
            struct frame kept[FRAME_QUEUE_COUNT];unsigned count=0;
            while(q->count){struct frame *f=&q->frames[q->head];if(f->audio){kept[count++]=*f;f->data=NULL;}queue_pop(q);}
            q->head=0;for(unsigned i=0;i<count;++i){q->frames[i]=kept[i];q->bytes+=kept[i].size;}q->count=count;
            c->epoch[slot]=epoch;c->needs_key[slot]=true;
        }
        if(q->count==FRAME_QUEUE_COUNT||n+ns+np>FRAME_QUEUE_BYTES-q->bytes){if(!audio)c->needs_key[slot]=true;}
        else if(!c->needs_key[slot]||(!audio&&key&&ns&&np)){
            struct frame *f=&q->frames[(q->head+q->count)%FRAME_QUEUE_COUNT];f->data=malloc(n+ns+np);
            if(!f->data){if(!audio)c->needs_key[slot]=true;}
            else {
                if(!audio&&key)c->needs_key[slot]=false;f->epoch=epoch;f->lease=lease;f->generation=generation;f->sequence=++c->sequence[slot];f->timestamp=timestamp;f->audio=audio;f->key=key;f->offset=0;f->size=n+ns+np;
                if(ns)memcpy(f->data,sps,ns);if(np)memcpy(f->data+ns,pps,np);memcpy(f->data+ns+np,data,n);q->bytes+=f->size;++q->count;
            }
        }
    }pthread_mutex_unlock(&c->lock);
}
bool mvm_client_peek(struct mvm_client *c,struct myvava_message *m,uint8_t data[MYVAVA_CHUNK]){if(!c)return false;bool ok=false;pthread_mutex_lock(&c->lock);
    if(myvava_millis()<c->until)for(unsigned k=0;k<4;++k){unsigned slot=(c->cursor+k)%4;struct frame_queue *q=&c->queues[slot];if(!q->count||!c->leases[slot].id)continue;struct frame *f=&q->frames[q->head];
        size_t n=f->size-f->offset;if(n>MVM_FRAGMENT)n=MVM_FRAGMENT;memset(data,0,32);mvm_p64(data,f->sequence);mvm_p64(data+8,f->timestamp);mvm_p64(data+16,f->generation);data[24]=(uint8_t)slot;data[25]=f->audio;data[26]=f->key;if(c->leases[slot].framing==2){data[27]=(uint8_t)(f->epoch>>24);data[28]=(uint8_t)(f->epoch>>16);data[29]=(uint8_t)(f->epoch>>8);data[30]=(uint8_t)f->epoch;data[31]=2;}
        memcpy(data+32,f->data+f->offset,n);*m=(struct myvava_message){.type=MYVAVA_MEDIA_FRAME,.id=f->lease,.total=(uint32_t)f->size,.offset=(uint32_t)f->offset,.length=n+32,.data=data};f->offset+=n;if(f->offset==f->size)queue_pop(q);c->cursor=(slot+1)%4;ok=true;break;}
    pthread_mutex_unlock(&c->lock);return ok;
}
