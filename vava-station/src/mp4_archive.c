#include "mp4_archive.h"
#include "protocol.h"
#include <errno.h>
#include <unistd.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define MINIMP4_IMPLEMENTATION
#include "../../third_party/minimp4/minimp4.h"
#pragma GCC diagnostic pop
#define SAMPLE_MAX (VAVA_MAX_FRAME+4096)
struct bits {uint8_t b[1024];size_t bytes,at;bool ok;};
static uint32_t bits(struct bits *b,unsigned n){if(n>32||b->at+n>b->bytes*8){b->ok=false;return 0;}uint32_t v=0;while(n--){v=v*2+((b->b[b->at/8]>>(7-b->at%8))&1);++b->at;}return v;}
static uint32_t ue(struct bits *b){unsigned n=0;while(b->ok&&!bits(b,1))if(++n>30){b->ok=false;return 0;}return ((1u<<n)-1)+bits(b,n);}
static int32_t se(struct bits *b){uint32_t n=ue(b);return n&1?(int32_t)((n+1)/2):-(int32_t)(n/2);}
static struct bits rbsp(const uint8_t *p,size_t n){struct bits b={.ok=true};unsigned zeros=0;for(size_t i=0;i<n&&b.bytes<sizeof(b.b);++i){if(zeros==2&&p[i]==3){zeros=0;continue;}b.b[b.bytes++]=p[i];zeros=p[i]==0?zeros+1:0;}return b;}
static bool dimensions(const uint8_t *p,size_t n,int *width,int *height){
    if(n<5||n>1024||(p[0]&31)!=7)return false;struct bits b=rbsp(p+1,n-1);unsigned profile=bits(&b,8);(void)bits(&b,16);if(ue(&b)>31)return false;
    unsigned chroma=1,separate=0;
    if(profile==100||profile==110||profile==122||profile==244||profile==44||profile==83||profile==86||profile==118||profile==128||profile==138||profile==139||profile==134||profile==135){
        chroma=ue(&b);if(chroma>3)return false;if(chroma==3)separate=bits(&b,1);if(ue(&b)>6||ue(&b)>6)return false;(void)bits(&b,1);
        if(bits(&b,1))for(unsigned i=0;i<(chroma==3?12u:8u);++i)if(bits(&b,1)){int last=8,next=8;for(unsigned k=0;k<(i<6?16u:64u);++k){if(next){int32_t d=se(&b);if(d<-128||d>127)return false;next=(last+d+256)%256;}last=next?next:last;}}
    }else if(profile!=66&&profile!=77&&profile!=88)return false;
    if(ue(&b)>12)return false;unsigned order=ue(&b);if(order==0){if(ue(&b)>12)return false;}else if(order==1){(void)bits(&b,1);(void)se(&b);(void)se(&b);unsigned cycle=ue(&b);if(cycle>255)return false;for(unsigned i=0;i<cycle;++i)(void)se(&b);}else if(order!=2)return false;
    if(ue(&b)>16)return false;(void)bits(&b,1);uint32_t w=ue(&b)+1,h=ue(&b)+1,frame=bits(&b,1);if(w>256||h>256||!w||!h)return false;
    if(!frame)(void)bits(&b,1);(void)bits(&b,1);unsigned l=0,r=0,t=0,bot=0;if(bits(&b,1)){l=ue(&b);r=ue(&b);t=ue(&b);bot=ue(&b);}
    unsigned sub=separate?0:chroma,cx=sub==1||sub==2?2:1,cy=(sub==1?2:1)*(2-frame);
    if(!b.ok||l>4096||r>4096||t>4096||bot>4096||(l+r)*cx>=w*16||(t+bot)*cy>=h*16*(2-frame))return false;
    *width=(int)(w*16-(l+r)*cx);*height=(int)(h*16*(2-frame)-(t+bot)*cy);return true;
}
struct track {uint8_t *data;size_t used;uint64_t pts,last;unsigned cc;bool seen;};
struct conversion {int source,dest;const struct myvava_file *file;MP4E_mux_t *mux;bool emit;uint8_t sps[1024],pps[1024],*sample;size_t sn,pn;int width,height;unsigned video,audio,seq,last_duration;struct track tracks[2];};
static bool at(int fd,void *out,size_t n,uint32_t off){size_t got=0;while(got<n){ssize_t k=pread(fd,(uint8_t *)out+got,n-got,(off_t)off+(off_t)got);if(k<0&&errno==EINTR)continue;if(k<=0)return false;got+=(size_t)k;}return true;}
static int write_mp4(int64_t offset,const void *p,size_t n,void *arg){struct conversion *c=arg;if(offset<0||(uint64_t)offset+n>MYVAVA_FILE_MAX+16u*1024*1024)return -1;size_t sent=0;while(sent<n){ssize_t k=pwrite(c->dest,(const uint8_t *)p+sent,n-sent,(off_t)offset+(off_t)sent);if(k<0&&errno==EINTR)continue;if(k<=0)return -1;sent+=(size_t)k;}return 0;}
static size_t prefix(const uint8_t *p,size_t n){return n>=4&&!p[0]&&!p[1]&&!p[2]&&p[3]==1?4:n>=3&&!p[0]&&!p[1]&&p[2]==1?3:0;}
static bool fragment(struct conversion *c,int track,const void *p,size_t n,uint64_t timestamp,unsigned duration,bool key){
    MP4E_mux_t *m=c->mux;if(!m->fragments_count++&&mp4e_flush_index(m))return false;
    if(mp4e_write_fragment_header(m,track,(int)n,(int)duration,key?MP4E_SAMPLE_RANDOM_ACCESS:MP4E_SAMPLE_DEFAULT,timestamp)||mp4e_write_mdat_box(m,(uint32_t)n+8)||write_mp4(m->write_pos,p,n,c))return false;m->write_pos+=(int64_t)n;return true;
}
static bool sample(struct conversion *c,unsigned track,unsigned duration){
    struct track *t=&c->tracks[track];if(!t->used||t->pts<90000||t->pts>90000+UINT64_C(180000)*90)return false;
    if(t->last&&t->pts<=t->last)return false;t->last=t->pts;
    if(track){const uint8_t *a=t->data;size_t n=t->used;if(n<8||a[0]!=255||(a[1]&0xf6)!=0xf0||(a[2]>>6)!=1||(a[6]&3))return false;
        unsigned index=(a[2]>>2)&15,ch=((a[2]&1)<<2)|(a[3]>>6),header=a[1]&1?7:9;const unsigned rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
        if(index>=13||rates[index]!=c->file->sample_rate||ch!=c->file->channels||n<=header||n!=(((size_t)(a[3]&3)<<11)|((size_t)a[4]<<3)|(a[5]>>5)))return false;
        ++c->audio;return !c->emit||fragment(c,1,a+header,n-header,((t->pts-90000)*c->file->sample_rate+45000)/90000,1024,true);
    }
    size_t pos=0,used=0;bool key=false,slice=false;
    while(pos<t->used){size_t pre=prefix(t->data+pos,t->used-pos);if(!pre)return false;size_t start=pos+pre,end=start;while(end<t->used&&!prefix(t->data+end,t->used-end))++end;while(end>start&&t->data[end-1]==0)--end;
        if(end==start)return false;const uint8_t *nal=t->data+start;size_t n=end-start;unsigned type=nal[0]&31;
        if(nal[0]&0x80)return false;
        if(type==7||type==8){uint8_t *saved=type==7?c->sps:c->pps;size_t *length=type==7?&c->sn:&c->pn;if(n>1024||(*length&&(*length!=n||memcmp(saved,nal,n))))return false;if(!*length){memcpy(saved,nal,n);*length=n;}}
        else if(type==1||type==5||type==6){if(type==1||type==5){struct bits b=rbsp(nal+1,n-1);(void)ue(&b);unsigned kind=ue(&b);if(!b.ok||kind>9||(kind%5!=0&&kind%5!=2))return false;slice=true;key|=type==5;}
            if(used+n+4>SAMPLE_MAX)return false;c->sample[used]=(uint8_t)(n>>24);c->sample[used+1]=(uint8_t)(n>>16);c->sample[used+2]=(uint8_t)(n>>8);c->sample[used+3]=(uint8_t)n;memcpy(c->sample+used+4,nal,n);used+=n+4;
        }else if(type!=9&&type!=12)return false;
        pos=end;while(pos<t->used&&!prefix(t->data+pos,t->used-pos))++pos;
    }
    if(!slice||(!c->video&&!key)||!c->sn||!c->pn)return false;++c->video;
    return !c->emit||fragment(c,0,c->sample,used,t->pts-90000,duration,key);
}
static bool pass(struct conversion *c){
    c->video=c->audio=0;c->last_duration=1;for(unsigned i=0;i<2;++i){uint8_t *data=c->tracks[i].data;c->tracks[i]=(struct track){.data=data};}
    for(uint32_t off=0;off<c->file->size;off+=188){uint8_t b[188];if(!at(c->source,b,188,off)||b[0]!=0x47||(b[1]&0x80)||(b[3]&0xc0))return false;
        unsigned pid=((unsigned)(b[1]&31)<<8)|b[2];if(pid==0||pid==0x1000)continue;if(pid!=0x100&&pid!=0x101)return false;unsigned track=pid-0x100;struct track *t=&c->tracks[track];
        if(!(b[3]&0x10)||!(b[3]&0x30))return false;size_t pos=4;if(b[3]&0x20)pos+=1+b[4];if(pos>=188)return false;
        unsigned cc=b[3]&15;if(t->seen&&cc!=((t->cc+1)&15))return false;t->seen=true;t->cc=cc;
        if(b[1]&0x40){if(188-pos<14||memcmp(b+pos,"\0\0\1",3)||b[pos+3]!=(track?0xc0:0xe0)||b[pos+6]!=0x80||b[pos+7]!=0x80||b[pos+8]!=5)return false;
            const uint8_t *p=b+pos+9;if((p[0]&0xf1)!=0x21||!(p[2]&1)||!(p[4]&1))return false;uint64_t pts=((uint64_t)(p[0]&14)<<29)|((uint64_t)p[1]<<22)|((uint64_t)(p[2]&254)<<14)|((uint64_t)p[3]<<7)|(p[4]>>1);
            if(t->used){if(pts<=t->pts||pts-t->pts>UINT64_C(180000)*90)return false;unsigned duration=(unsigned)(pts-t->pts);if(!sample(c,track,duration))return false;if(!track)c->last_duration=duration;}
            t->pts=pts;t->used=0;pos+=14;
        }else if(!t->used)return false;
        size_t n=188-pos;if(n>SAMPLE_MAX-t->used)return false;memcpy(t->data+t->used,b+pos,n);t->used+=n;
    }
    for(unsigned i=0;i<2;++i)if(c->tracks[i].used&&!sample(c,i,i?1024:c->last_duration))return false;
    return c->video==c->file->frames&&c->audio==c->file->audio_frames&&c->video>0;
}
bool mp4_archive(int source,int dest,const struct myvava_file *f){
    if(!f||!f->size||f->size%188||f->size>MYVAVA_FILE_MAX||f->frames>20000||f->audio_frames>30000)return false;
    struct conversion *c=calloc(1,sizeof(*c));if(!c)return false;c->source=source;c->dest=dest;c->file=f;c->sample=malloc(SAMPLE_MAX);for(unsigned i=0;i<2;++i)c->tracks[i].data=malloc(SAMPLE_MAX);
    bool ok=c->sample&&c->tracks[0].data&&c->tracks[1].data&&pass(c)&&dimensions(c->sps,c->sn,&c->width,&c->height);
    if(ok){c->mux=MP4E_open(1,1,c,write_mp4);ok=c->mux!=NULL;}
    if(ok){MP4E_track_t v={.object_type_indication=MP4_OBJECT_TYPE_AVC,.language="und",.track_media_kind=e_video,.time_scale=90000,.u.v={c->width,c->height}};
        ok=MP4E_add_track(c->mux,&v)==0&&!MP4E_set_sps(c->mux,0,c->sps,(int)c->sn)&&!MP4E_set_pps(c->mux,0,c->pps,(int)c->pn);
        if(ok&&f->audio_frames){MP4E_track_t a={.object_type_indication=MP4_OBJECT_TYPE_AUDIO_ISO_IEC_14496_3,.language="und",.track_media_kind=e_audio,.time_scale=f->sample_rate,.default_duration=1024,.u.a={f->channels}};
            const unsigned rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};unsigned i=0;while(i<13&&rates[i]!=f->sample_rate)++i;uint8_t asc[2]={(uint8_t)(0x10|(i>>1)),(uint8_t)((i<<7)|(f->channels<<3))};
            ok=i<13&&MP4E_add_track(c->mux,&a)==1&&!MP4E_set_dsi(c->mux,1,asc,2);
        }
    }
    if(ok){c->emit=true;ok=pass(c);}if(c->mux&&MP4E_close(c->mux))ok=false;
    free(c->sample);for(unsigned i=0;i<2;++i)free(c->tracks[i].data);free(c);return ok;
}
