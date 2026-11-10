#include "media_store.h"
#include "ts_mux.h"
#include "protocol.h"
#include "h264.h"
#include <pthread.h>
#include <openssl/sha.h>
#include <openssl/crypto.h>
#include <stdlib.h>
#include <string.h>
#define LIVE_LIMIT 4
#define RING_SIZE (512u*1024u)
struct live {
    uint64_t id,consumer,received,generation,sequence,frame_at,timestamp;char device[64],serial[32];uint8_t boot[16],owner[32];unsigned slot;
    bool talk,failed,audio,key,need_key,raw,negotiated;uint32_t epoch,assembling_epoch;
    struct h264_config codec;struct mvm_sample *samples[16];unsigned sample_head,sample_count;size_t sample_bytes;struct ts_mux mux;
    uint32_t total,assembled;uint8_t bits[(MVM_FRAME_MAX/MVM_FRAGMENT+8)/8],frame[MVM_FRAME_MAX];
    uint8_t ring[RING_SIZE];size_t head,used;
    uint8_t speech[MVM_TALK_MAX];size_t speech_size;uint32_t speech_sequence;uint64_t speech_at;
};
struct mvm_store {pthread_mutex_t lock;struct {char name[64];uint8_t boot[16];uint64_t seen;} devices[16];struct live live[LIVE_LIMIT];};
struct mvm_store *mvm_store_new(void){struct mvm_store *s=calloc(1,sizeof(*s));if(s&&pthread_mutex_init(&s->lock,NULL)){free(s);s=NULL;}return s;}
static void clear_samples(struct live *l){while(l->sample_count){struct mvm_sample *p=l->samples[l->sample_head];l->sample_bytes-=p->size;free(p);l->sample_head=(l->sample_head+1)%16;--l->sample_count;}}
void mvm_store_free(struct mvm_store *s){if(s){for(unsigned i=0;i<LIVE_LIMIT;++i)clear_samples(&s->live[i]);pthread_mutex_destroy(&s->lock);free(s);}}
static bool valid(struct live *l,uint64_t now){return l->id&&!l->failed&&now-l->consumer<3000&&now-l->received<15000;}
void mvm_store_device(struct mvm_store *s,const char *device,const uint8_t boot[16]){
    if(!s||!boot||!myvava_id_valid(device))return;
    uint8_t any=0;for(unsigned i=0;i<16;++i)any|=boot[i];
    pthread_mutex_lock(&s->lock);
    unsigned entry=16,empty=16;
    for(unsigned i=0;i<16;++i){
        if(!strcmp(s->devices[i].name,device)){entry=i;break;}
        if(!s->devices[i].name[0]&&empty==16)empty=i;
    }
    if(entry==16&&any)entry=empty;
    if(entry<16){
        if(!any)memset(&s->devices[entry],0,sizeof(s->devices[entry]));
        else{strcpy(s->devices[entry].name,device);memcpy(s->devices[entry].boot,boot,16);s->devices[entry].seen=myvava_millis();}
    }
    for(unsigned i=0;i<LIVE_LIMIT;++i)if(s->live[i].id&&!strcmp(s->live[i].device,device)&&(!any||memcmp(s->live[i].boot,boot,16)))s->live[i].failed=true;
    pthread_mutex_unlock(&s->lock);
}
static uint64_t open_live(struct mvm_store *s,const char *device,const uint8_t boot[16],unsigned slot,const char *serial,bool talk,const char *owner,bool raw){if(!s||!owner||strlen(owner)!=64||slot>3||!myvava_id_valid(device)||!myvava_id_valid(serial)||strlen(serial)>31)return 0;uint64_t id=0,now=myvava_millis();pthread_mutex_lock(&s->lock);bool online=false;for(unsigned i=0;i<16;++i)if(!strcmp(s->devices[i].name,device)&&!memcmp(s->devices[i].boot,boot,16)&&now-s->devices[i].seen<10000)online=true;
    if(online){for(unsigned i=0;i<LIVE_LIMIT;++i)if(valid(&s->live[i],now)&&!strcmp(s->live[i].device,device)&&s->live[i].slot==slot)online=false;}
    if(online)for(unsigned i=0;i<LIVE_LIMIT;++i)if(!s->live[i].id){struct live *l=&s->live[i];if(myvava_random((uint8_t *)&id,8)&&id){memset(l,0,sizeof(*l));l->id=id;l->received=l->consumer=now;l->slot=slot;l->talk=talk;l->raw=raw;SHA256((const unsigned char *)owner,strlen(owner),l->owner);l->need_key=true;strcpy(l->device,device);strcpy(l->serial,serial);memcpy(l->boot,boot,16);}break;}
    pthread_mutex_unlock(&s->lock);return id;
}
uint64_t mvm_store_open(struct mvm_store *s,const char *device,const uint8_t boot[16],unsigned slot,const char *serial,bool talk,const char *owner){return open_live(s,device,boot,slot,serial,talk,owner,false);}
uint64_t mvm_store_open_frames(struct mvm_store *s,const char *device,const uint8_t boot[16],unsigned slot,const char *serial,bool talk,const char *owner){return open_live(s,device,boot,slot,serial,talk,owner,true);}
int mvm_store_owner(struct mvm_store *s,const char *device,uint64_t id,const char *owner,unsigned *slot,char serial[32]){
    if(!s||!owner)return 403;uint8_t hash[32];SHA256((const unsigned char *)owner,strlen(owner),hash);int result=404;
    pthread_mutex_lock(&s->lock);for(unsigned i=0;i<LIVE_LIMIT;++i){struct live *l=&s->live[i];if(l->id!=id||strcmp(l->device,device)||!valid(l,myvava_millis()))continue;
        result=CRYPTO_memcmp(hash,l->owner,32)?403:!l->raw||!l->negotiated?409:200;if(result==200){*slot=l->slot;memcpy(serial,l->serial,32);}break;}pthread_mutex_unlock(&s->lock);return result;
}
int mvm_store_read_sample(struct mvm_store *s,uint64_t id,struct mvm_sample *out,size_t cap){
    if(!s||!out)return -1;int result=-1;pthread_mutex_lock(&s->lock);for(unsigned i=0;i<LIVE_LIMIT;++i){struct live *l=&s->live[i];if(l->id!=id)continue;
        if(valid(l,myvava_millis())&&l->raw){l->consumer=myvava_millis();result=0;if(l->sample_count){struct mvm_sample *p=l->samples[l->sample_head];if(p->size>cap){l->failed=true;result=-1;}else{memcpy(out,p,sizeof(*p)+p->size);l->sample_bytes-=p->size;free(p);l->sample_head=(l->sample_head+1)%16;--l->sample_count;result=1;}}}break;}
    pthread_mutex_unlock(&s->lock);return result;
}
void mvm_store_close(struct mvm_store *s,uint64_t id){if(!s||!id)return;pthread_mutex_lock(&s->lock);for(unsigned i=0;i<LIVE_LIMIT;++i)if(s->live[i].id==id){clear_samples(&s->live[i]);memset(&s->live[i],0,sizeof(s->live[i]));}pthread_mutex_unlock(&s->lock);}
int mvm_store_read(struct mvm_store *s,uint64_t id,void *out,size_t size){if(!s||!id)return -1;int n=-1;pthread_mutex_lock(&s->lock);uint64_t now=myvava_millis();for(unsigned i=0;i<LIVE_LIMIT;++i){struct live *l=&s->live[i];if(l->id!=id)continue;if(valid(l,now)){l->consumer=now;size_t k=l->used;if(k>size)k=size;for(size_t j=0;j<k;++j)((uint8_t *)out)[j]=l->ring[(l->head+j)%RING_SIZE];l->head=(l->head+k)%RING_SIZE;l->used-=k;n=(int)k;}break;}pthread_mutex_unlock(&s->lock);return n;}
int mvm_store_talk(struct mvm_store *s,const char *device,uint64_t id,const char *owner,const uint8_t *p,size_t n){if(!s||!id||!owner||n>MVM_TALK_MAX||!vava_talk_aac(p,n))return 400;uint8_t hash[32];SHA256((const unsigned char *)owner,strlen(owner),hash);int code=404;uint64_t now=myvava_millis();pthread_mutex_lock(&s->lock);for(unsigned i=0;i<LIVE_LIMIT;++i){struct live *l=&s->live[i];if(l->id!=id||strcmp(l->device,device)||!valid(l,now))continue;if(!l->talk||CRYPTO_memcmp(hash,l->owner,32)){code=403;break;}if((l->speech_size&&now-l->speech_at<500)||now-l->speech_at<100){code=429;break;}if(l->speech_sequence==UINT32_MAX){code=409;break;}memcpy(l->speech,p,n);l->speech_size=n;l->speech_at=now;++l->speech_sequence;code=200;break;}pthread_mutex_unlock(&s->lock);return code;}
size_t mvm_store_control(struct mvm_store *s,const char *device,const struct myvava_message *m,uint8_t *out){return mvm_store_control_version(s,device,m,out,false);}
size_t mvm_store_control_version(struct mvm_store *s,const char *device,const struct myvava_message *m,uint8_t *out,bool v2){memset(out,0,MYVAVA_CHUNK);if(!s)return MVM_CONTROL;size_t n=MVM_CONTROL;uint64_t now=myvava_millis();pthread_mutex_lock(&s->lock);
    for(unsigned i=0;i<LIVE_LIMIT;++i){struct live *l=&s->live[i];if(strcmp(l->device,device)||memcmp(l->boot,m->boot,16)||!valid(l,now))continue;uint8_t *p=out+l->slot*48;mvm_p64(p,l->id);memcpy(p+8,l->serial,32);p[40]=l->talk;if(l->raw&&v2){p[41]=2;l->negotiated=true;}
        if(l->speech_size&&(now-l->speech_at>=500||(m->id==l->id&&m->offset>=l->speech_sequence)))l->speech_size=0;
        if(l->speech_size&&n==MVM_CONTROL){p=out+MVM_CONTROL;mvm_p64(p,l->id);p[8]=(uint8_t)(l->speech_sequence>>24);p[9]=(uint8_t)(l->speech_sequence>>16);p[10]=(uint8_t)(l->speech_sequence>>8);p[11]=(uint8_t)l->speech_sequence;p[12]=(uint8_t)l->slot;p[14]=(uint8_t)(l->speech_size>>8);p[15]=(uint8_t)l->speech_size;memcpy(p+16,l->speech,l->speech_size);n+=16+l->speech_size;}
    }pthread_mutex_unlock(&s->lock);return n;
}
static bool write_ts(void *arg,const uint8_t *p,size_t n){struct live *l=arg;if(n>RING_SIZE-l->used)return false;for(size_t i=0;i<n;++i)l->ring[(l->head+l->used+i)%RING_SIZE]=p[i];l->used+=n;return true;}
bool mvm_store_frame(struct mvm_store *s,const char *device,const struct myvava_message *m){
    if(!s||m->length<=32||m->total>MVM_FRAME_MAX||!m->total||m->offset>=m->total||m->offset%MVM_FRAGMENT||m->length-32>m->total-m->offset||m->length-32!=(m->total-m->offset>MVM_FRAGMENT?MVM_FRAGMENT:m->total-m->offset))return false;
    const uint8_t *p=m->data;uint64_t seq=mvm_u64(p),timestamp=mvm_u64(p+8),generation=mvm_u64(p+16);unsigned slot=p[24];if(!seq||!generation||slot>3||p[25]>1||p[26]>1)return false;uint32_t epoch=(uint32_t)p[27]<<24|(uint32_t)p[28]<<16|(uint32_t)p[29]<<8|p[30];if(p[31]!=0&&p[31]!=2)return false;if((p[31]==0&&epoch)||(p[31]==2&&!epoch))return false;
    bool ok=false;uint64_t now=myvava_millis();pthread_mutex_lock(&s->lock);
    for(unsigned i=0;i<LIVE_LIMIT;++i){struct live *l=&s->live[i];if(l->id!=m->id||l->slot!=slot||strcmp(l->device,device)||memcmp(l->boot,m->boot,16)||!valid(l,now))continue;
        if(l->raw!=(p[31]==2)|| (l->raw&&!l->negotiated)){l->failed=true;break;}
        if(!p[25]&&l->raw&&epoch<l->epoch)break;
        if(l->generation&&l->generation!=generation){l->failed=true;break;}l->generation=generation;
        if(seq<l->sequence)break;
        if(seq!=l->sequence){if(l->sequence&&(l->assembled!=l->total||seq!=l->sequence+1))l->need_key=true;l->sequence=seq;l->assembled=0;l->total=m->total;l->audio=p[25];l->key=p[26];l->timestamp=timestamp;l->assembling_epoch=epoch;l->frame_at=now;memset(l->bits,0,sizeof(l->bits));}
        if(l->total!=m->total||l->audio!=(bool)p[25]||l->key!=(bool)p[26]||l->timestamp!=timestamp||l->assembling_epoch!=epoch||now-l->frame_at>500){l->need_key=true;break;}
        unsigned bit=m->offset/MVM_FRAGMENT;size_t size=m->length-32;
        if(l->bits[bit/8]&(1u<<(bit%8))){ok=!memcmp(l->frame+m->offset,p+32,size);if(!ok)l->failed=true;break;}
        memcpy(l->frame+m->offset,p+32,size);l->bits[bit/8]|=(uint8_t)(1u<<(bit%8));l->assembled+=(uint32_t)size;ok=true;
        if(l->assembled==l->total){l->received=now;if(l->need_key&&((l->audio&&(!l->raw||!l->epoch))||(!l->audio&&!l->key)))break;
            if(l->raw){
                if(!l->audio){bool key,changed;int accepted=h264_config_frame(&l->codec,l->frame,l->total,&key,&changed);
                    if(accepted<0){l->need_key=true;break;}if(!accepted)break;
                    if(key!=l->key||(epoch!=l->epoch&&!key)||(epoch==l->epoch&&changed)){l->failed=true;break;}
                    l->epoch=epoch;if(key)l->need_key=false;
                }
                if(l->sample_count==16||l->total>RING_SIZE-l->sample_bytes){l->failed=true;break;}
                struct mvm_sample *sample=malloc(sizeof(*sample)+l->total);if(!sample){l->failed=true;break;}
                *sample=(struct mvm_sample){.generation=generation,.sequence=seq,.timestamp=timestamp,.epoch=epoch,.audio=l->audio,.key=l->key,.size=l->total};memcpy(sample->data,l->frame,l->total);
                l->samples[(l->sample_head+l->sample_count)%16]=sample;++l->sample_count;l->sample_bytes+=l->total;break;
            }
            /* Preserve the common camera clock and TS continuity through losses;
             * restart only before the first emitted keyframe. */
            if(!l->mux.started&&!l->audio&&l->key)l->mux=(struct ts_mux){.continuous=true};
            if(!ts_mux_frame(&l->mux,l->audio,l->key,l->timestamp,l->frame,l->total,write_ts,l))l->failed=true;else if(!l->audio&&l->key)l->need_key=false;
        }break;
    }pthread_mutex_unlock(&s->lock);return ok;
}
