#include "media.h"
#include "media_store.h"
#include "ts_mux.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char owner[]="0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
static uint8_t boot[16]={1};
static void registry_churn(void){
    struct mvm_store *s=mvm_store_new();assert(s);
    uint8_t unavailable[16]={0},out[188];char device[64];
    for(unsigned i=0;i<40;++i){
        snprintf(device,sizeof(device),"BASE_%u",i);mvm_store_device(s,device,boot);
        uint64_t id=mvm_store_open(s,device,boot,0,"CAMERA_1",false,owner);assert(id);
        /* Registry reload revokes the station before accepting a new identity. */
        mvm_store_device(s,device,unavailable);
        assert(mvm_store_read(s,id,out,sizeof(out))==-1);mvm_store_close(s,id);
        assert(!mvm_store_open(s,device,boot,0,"CAMERA_1",false,owner));
        assert(!mvm_store_open(s,device,unavailable,0,"CAMERA_1",false,owner));
    }
    /* Removing an earlier entry must not duplicate a still-registered station
     * into the vacant slot and leave a stale copy when it is later revoked. */
    mvm_store_device(s,"BASE_A",boot);mvm_store_device(s,"BASE_B",boot);
    mvm_store_device(s,"BASE_A",unavailable);mvm_store_device(s,"BASE_B",boot);
    mvm_store_device(s,"BASE_B",unavailable);
    assert(!mvm_store_open(s,"BASE_B",boot,0,"CAMERA_1",false,owner));
    mvm_store_free(s);
}
static bool discard(void *arg,const uint8_t *data,size_t n){(void)arg;assert(data[0]==0x47&&n==188);return true;}
static void continuous_timeline(void){
    static const uint8_t video[]={0,0,0,1,0x65,0x88};
    static const uint8_t audio[]={0xff,0xf1,0x6c,0x40,0x01,0x3f,0xfc,0,0};
    struct ts_mux clip={0};
    assert(ts_mux_frame(&clip,false,true,1000,video,sizeof(video),discard,NULL));
    assert(!ts_mux_frame(&clip,false,true,181001,video,sizeof(video),discard,NULL));
    assert(!strcmp(clip.error,"media_timestamp_range"));
    struct mvm_store *s=mvm_store_new();assert(s);mvm_store_device(s,"BASE_A",boot);
    uint64_t id=mvm_store_open(s,"BASE_A",boot,0,"CAMERA_1",false,owner);assert(id);
    uint8_t data[64]={0},out[2048];uint64_t seq=0;
    struct myvava_message m={.type=MYVAVA_MEDIA_FRAME,.id=id,.data=data};memcpy(m.boot,boot,16);
    /* Exercise the actual live receiver across 3 minutes and the TS PTS wrap,
     * with interleaved video/audio and no wall-clock test delay. */
    for(uint64_t ms=1000;ms<=96001000;ms+=15000){
        for(unsigned a=0;a<2;++a){
            mvm_p64(data,++seq);mvm_p64(data+8,ms);mvm_p64(data+16,1);data[25]=(uint8_t)a;data[26]=(uint8_t)!a;
            m.total=a?sizeof(audio):sizeof(video);m.length=(uint16_t)(32+m.total);memcpy(data+32,a?audio:video,m.total);
            assert(mvm_store_frame(s,"BASE_A",&m));int got=mvm_store_read(s,id,out,sizeof(out));assert(got>0);
            if(!a){
                bool found=false;
                for(int i=0;i<got;i+=188){const uint8_t *p=out+i;
                    if((((unsigned)(p[1]&31)<<8)|p[2])!=0x100)continue;
                    size_t at=4+((p[3]&0x20)?(size_t)p[4]+1:0)+9;
                    assert(at+5<=188);const uint8_t *t=p+at;
                    uint64_t pts=((uint64_t)(t[0]&14)<<29)|((uint64_t)t[1]<<22)|
                        ((uint64_t)(t[2]&254)<<14)|((uint64_t)t[3]<<7)|(t[4]>>1);
                    assert(pts==((90000+(ms-1000)*90)&((UINT64_C(1)<<33)-1)));found=true;break;
                }
                assert(found);
            }
        }
    }
    mvm_p64(data,++seq);mvm_p64(data+8,96001000+15001);
    assert(mvm_store_frame(s,"BASE_A",&m));assert(mvm_store_read(s,id,out,sizeof(out))==-1);
    mvm_store_free(s);
}
struct packet {struct myvava_message m;uint8_t bytes[MYVAVA_CHUNK];};
static unsigned frame(struct mvm_client *c,uint64_t id,bool key,struct packet *packets){
    static uint8_t video[1800]={0,0,0,1,0x65};static const uint8_t sps[]={0,0,0,1,0x67},pps[]={0,0,0,1,0x68};static uint64_t timestamp=1000;
    mvm_client_frame(c,0,id,1,false,key,timestamp,video,sizeof(video),key?sps:NULL,key?sizeof(sps):0,key?pps:NULL,key?sizeof(pps):0);timestamp+=67;
    unsigned n=0;while(n<8&&mvm_client_peek(c,&packets[n].m,packets[n].bytes)){memcpy(packets[n].m.boot,boot,16);n++;}return n;
}
static void burst_queue(void){
    struct mvm_client *c=mvm_client_new();assert(c);uint8_t control[MVM_CONTROL]={0},bytes[MYVAVA_CHUNK];
    mvm_p64(control,1);memcpy(control+8,"CAMERA_1",9);assert(mvm_client_control(c,control,sizeof(control)));
    static const uint8_t video[1800]={0,0,0,1,0x65},audio[80]={0xff,0xf1},sps[]={0,0,0,1,0x67},pps[]={0,0,0,1,0x68};
    struct myvava_message m;
    /* One TCP read delivers an IDR, audio, and the next P frame before the
     * transport gets CPU time. All three must survive, in order. */
    mvm_client_frame(c,0,1,1,false,true,1000,video,sizeof(video),sps,sizeof(sps),pps,sizeof(pps));
    mvm_client_frame(c,0,1,1,true,false,1050,audio,sizeof(audio),NULL,0,NULL,0);
    mvm_client_frame(c,0,1,1,false,false,1100,video,sizeof(video),NULL,0,NULL,0);
    unsigned frames=0;uint64_t timestamps[]={1000,1050,1100};
    while(mvm_client_peek(c,&m,bytes)){assert(mvm_u64(bytes+8)==timestamps[frames]);assert(bytes[25]==(frames==1));if(m.offset+m.length-32==m.total)++frames;}
    assert(frames==3);
    /* Count bound drops only incoming excess; queued media remains intact.
     * Dropping a video requires an IDR before any subsequent P frame. */
    for(unsigned i=0;i<17;++i)mvm_client_frame(c,0,1,1,false,false,1200+i*100,video,sizeof(video),NULL,0,NULL,0);
    frames=0;while(mvm_client_peek(c,&m,bytes))if(m.offset+m.length-32==m.total)++frames;assert(frames==16);
    mvm_client_frame(c,0,1,1,false,false,3000,video,sizeof(video),NULL,0,NULL,0);assert(!mvm_client_peek(c,&m,bytes));
    mvm_client_frame(c,0,1,1,false,true,3100,video,sizeof(video),sps,sizeof(sps),pps,sizeof(pps));assert(mvm_client_peek(c,&m,bytes));
    /* Lease replacement discards even a partially sent old frame. */
    mvm_p64(control,2);assert(mvm_client_control(c,control,sizeof(control)));assert(!mvm_client_peek(c,&m,bytes));
    static uint8_t large[MVM_FRAME_MAX];
    mvm_client_frame(c,0,2,1,false,true,4000,large,sizeof(large)-sizeof(sps)-sizeof(pps),sps,sizeof(sps),pps,sizeof(pps));
    mvm_client_frame(c,0,2,1,false,false,4100,large,sizeof(large),NULL,0,NULL,0);
    mvm_client_frame(c,0,2,1,false,false,4200,large,sizeof(large),NULL,0,NULL,0);
    frames=0;while(mvm_client_peek(c,&m,bytes))if(m.offset+m.length-32==m.total)++frames;assert(frames==2);
    mvm_client_frame(c,0,2,1,false,true,4300,video,sizeof(video),sps,sizeof(sps),pps,sizeof(pps));
    mvm_client_reset(c);assert(!mvm_client_peek(c,&m,bytes));mvm_client_free(c);
}
static void raw_epochs(void){
    const uint8_t video[]={0,0,0,1,0x67,0x42,0,0x1e,0x96,0x35,0x41,0x40,0x5f,0xf2,0xcd,0xc0,0x40,0x40,0x80,
        0,0,1,0x68,0xe0,0,0,1,0x65,0xbc};
    struct mvm_store *s=mvm_store_new();mvm_store_device(s,"BASE_A",boot);
    uint64_t id=mvm_store_open_frames(s,"BASE_A",boot,0,"CAMERA_1",false,owner);assert(id);
    unsigned slot;char serial[32];assert(mvm_store_owner(s,"BASE_A",id,owner,&slot,serial)==409);
    uint8_t control[MYVAVA_CHUNK],bytes[128]={0};struct myvava_message poll={0};memcpy(poll.boot,boot,16);
    size_t n=mvm_store_control_version(s,"BASE_A",&poll,control,true);assert(n==MVM_CONTROL&&control[41]==2);
    struct mvm_client *c=mvm_client_new();assert(!mvm_client_control(c,control,n));assert(mvm_client_control_version(c,control,n,true));
    assert(mvm_store_owner(s,"BASE_A",id,owner,&slot,serial)==200&&slot==0);
    struct mvm_sample *sample=malloc(sizeof(*sample)+128);
    struct myvava_message m={.id=id,.data=bytes,.total=sizeof(video),.length=32+sizeof(video)};memcpy(m.boot,boot,16);
    mvm_p64(bytes,1);mvm_p64(bytes+8,1000);mvm_p64(bytes+16,7);bytes[26]=1;bytes[30]=1;bytes[31]=2;memcpy(bytes+32,video,sizeof(video));
    assert(mvm_store_frame(s,"BASE_A",&m));assert(mvm_store_read_sample(s,id,sample,128)==1&&sample->epoch==1&&sample->key);
    /* A lost old video frame must not mute complete audio after initialization. */
    mvm_p64(bytes,3);mvm_p64(bytes+8,1128);bytes[25]=1;bytes[26]=0;m.total=8;m.length=40;memset(bytes+32,0x55,8);
    assert(mvm_store_frame(s,"BASE_A",&m));assert(mvm_store_read_sample(s,id,sample,128)==1&&sample->audio);
    mvm_p64(bytes,4);mvm_p64(bytes+8,1200);bytes[25]=0;bytes[26]=1;bytes[30]=2;m.total=sizeof(video);m.length=32+sizeof(video);memcpy(bytes+32,video,sizeof(video));
    assert(mvm_store_frame(s,"BASE_A",&m));assert(mvm_store_read_sample(s,id,sample,128)==1&&sample->epoch==2);
    mvm_p64(bytes,5);bytes[30]=1;assert(!mvm_store_frame(s,"BASE_A",&m));assert(mvm_store_read_sample(s,id,sample,128)==0);
    bytes[30]=2;mvm_p64(bytes+16,8);assert(!mvm_store_frame(s,"BASE_A",&m));assert(mvm_store_read_sample(s,id,sample,128)==-1);
    free(sample);mvm_client_free(c);mvm_store_free(s);
}
int main(void){
    registry_churn();
    raw_epochs();
    continuous_timeline();
    burst_queue();
    struct mvm_store *s=mvm_store_new();struct mvm_client *c=mvm_client_new();assert(s&&c);mvm_store_device(s,"BASE_A",boot);
    uint64_t id=mvm_store_open(s,"BASE_A",boot,0,"CAMERA_1",true,owner);assert(id);assert(!mvm_store_open(s,"BASE_A",boot,0,"CAMERA_1",false,owner));
    uint8_t control[MYVAVA_CHUNK];struct myvava_message poll={.type=MYVAVA_MEDIA_POLL};memcpy(poll.boot,boot,16);size_t n=mvm_store_control(s,"BASE_A",&poll,control);assert(n==MVM_CONTROL);assert(mvm_client_control(c,control,n));
    struct mvm_lease leases[4];struct mvm_talk talk;mvm_client_snapshot(c,leases,&talk);assert(leases[0].id==id&&!talk.lease);
    struct packet packets[8];unsigned count=frame(c,id,true,packets);assert(count==3);
    assert(!mvm_store_frame(s,"BASE_B",&packets[0].m));
    for(unsigned i=count;i>0;--i)assert(mvm_store_frame(s,"BASE_A",&packets[i-1].m));
    uint8_t output[8192];int got=mvm_store_read(s,id,output,sizeof(output));assert(got>0&&got%188==0&&output[0]==0x47);
    assert(mvm_store_frame(s,"BASE_A",&packets[0].m));assert(mvm_store_read(s,id,output,sizeof(output))==0);
    count=frame(c,id,false,packets);assert(count==3);assert(mvm_store_frame(s,"BASE_A",&packets[0].m)); /* missing tail */
    count=frame(c,id,false,packets);for(unsigned i=0;i<count;++i)assert(mvm_store_frame(s,"BASE_A",&packets[i].m));assert(mvm_store_read(s,id,output,sizeof(output))==0);
    count=frame(c,id,true,packets);for(unsigned i=0;i<count;++i)assert(mvm_store_frame(s,"BASE_A",&packets[i].m));assert(mvm_store_read(s,id,output,sizeof(output))>0);
    /* Inconsistent duplicate cannot overwrite already assembled media. */
    count=frame(c,id,true,packets);assert(mvm_store_frame(s,"BASE_A",&packets[0].m));packets[0].bytes[32]^=1;assert(!mvm_store_frame(s,"BASE_A",&packets[0].m));assert(mvm_store_read(s,id,output,sizeof(output))==-1);mvm_store_close(s,id);
    id=mvm_store_open(s,"BASE_A",boot,0,"CAMERA_1",true,owner);assert(id);
    n=mvm_store_control(s,"BASE_A",&poll,control);assert(mvm_client_control(c,control,n));mvm_client_snapshot(c,leases,&talk);assert(leases[0].id==id);
    mvm_client_reset(c);mvm_client_snapshot(c,leases,&talk);assert(!leases[0].id&&!mvm_client_peek(c,&packets[0].m,packets[0].bytes));
    ++boot[0];mvm_store_device(s,"BASE_A",boot);assert(mvm_store_read(s,id,output,sizeof(output))==-1);mvm_store_close(s,id);
    /* Invalid fragment lengths, slots and zero generations are rejected. */
    packets[0].m.offset=1;assert(!mvm_store_frame(s,"BASE_A",&packets[0].m));
    mvm_client_free(c);mvm_store_free(s);puts("media reassembly, loss recovery, ownership and boot isolation passed");return 0;
}
