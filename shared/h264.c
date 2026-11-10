#include "h264.h"
#include <string.h>
static size_t start(const uint8_t *p,size_t n,size_t at,size_t *prefix) {
    for(size_t i=at;i+3<=n;++i)if(!p[i]&&!p[i+1]) {
        if(p[i+2]==1){*prefix=3;return i;}
        if(i+4<=n&&!p[i+2]&&p[i+3]==1){*prefix=4;return i;}
    }
    *prefix=0;return n;
}
static bool escaped(const uint8_t *p,size_t n) {
    unsigned zero=0;
    for(size_t i=0;i<n;++i) {
        if(zero>=2) {
            if(p[i]<3)return false;
            if(p[i]==3){if(i+1==n||p[i+1]>3)return false;zero=0;continue;}
        }
        zero=p[i]?0:zero+1;
    }
    return true;
}
bool h264_valid(const uint8_t *p,size_t n) {
    size_t prefix,at=start(p,n,0,&prefix);
    if(at==n)return false;
    for(size_t i=0;i<at;++i)if(p[i])return false;
    while(at<n) {
        size_t next_prefix,end=start(p,n,at+prefix,&next_prefix),last=end;
        while(last>at+prefix&&p[last-1]==0)--last; /* Annex B trailing_zero_8bits */
        if(last<=at+prefix)return false;
        unsigned type=p[at+prefix]&31;
        if((p[at+prefix]&128)||!(type==1||type==5||(type>=6&&type<=12))||
            !escaped(p+at+prefix+1,last-at-prefix-1))return false;
        at=end;prefix=next_prefix;
    }
    return true;
}
struct bits {uint8_t p[1024];size_t n,at;bool bad;};
static uint32_t read_bits(struct bits *b,unsigned n) {
    if(n>32||b->at+n>b->n*8){b->bad=true;return 0;}
    uint32_t v=0;while(n--){v=(v<<1)|((b->p[b->at/8]>>(7-b->at%8))&1);++b->at;}return v;
}
static uint32_t ue(struct bits *b) {
    unsigned zeros=0;while(!b->bad&&!read_bits(b,1))if(++zeros>30){b->bad=true;return 0;}
    return b->bad?0:((1u<<zeros)-1)+read_bits(b,zeros);
}
static int se(struct bits *b) {uint32_t n=ue(b);return n&1?(int)(n/2+1):-(int)(n/2);}
static bool parse_sps(const uint8_t *p,size_t n,unsigned *width,unsigned *height,struct bits *syntax,unsigned *references,bool *progressive) {
    size_t prefix,at=start(p,n,0,&prefix);struct bits b={0};
    if(at==n||n-at-prefix<4||(p[at+prefix]&31)!=7||n-at-prefix>sizeof(b.p))return false;
    p+=at+prefix+1;n-=at+prefix+1;unsigned zeros=0;
    for(size_t i=0;i<n;++i){if(zeros>=2&&p[i]==3){zeros=0;continue;}b.p[b.n++]=p[i];zeros=p[i]?0:zeros+1;}
    unsigned profile=read_bits(&b,8);read_bits(&b,8);read_bits(&b,8);if(ue(&b)>31)return false;
    unsigned chroma=1,separate=0;
    if(profile==100||profile==110||profile==122||profile==244||profile==44||profile==83||profile==86||
       profile==118||profile==128||profile==138||profile==139||profile==134||profile==135) {
        chroma=ue(&b);if(chroma>3)return false;if(chroma==3)separate=read_bits(&b,1);
        if(ue(&b)>6||ue(&b)>6)return false;
        read_bits(&b,1);
        if(read_bits(&b,1))for(unsigned i=0;i<(chroma==3?12u:8u);++i)if(read_bits(&b,1)){
            int last=8,next=8;for(unsigned j=0;j<(i<6?16u:64u);++j){if(next){int delta=se(&b);if(delta< -128||delta>127)return false;next=(last+delta+256)%256;}last=next?next:last;}
        }
    }
    if(ue(&b)>12)return false;
    unsigned poc=ue(&b);
    if(poc==0){if(ue(&b)>12)return false;}
    else if(poc==1){read_bits(&b,1);se(&b);se(&b);unsigned count=ue(&b);if(count>255)return false;for(unsigned i=0;i<count;++i)se(&b);}
    else if(poc>2)return false;
    unsigned refs=ue(&b);read_bits(&b,1);unsigned w=ue(&b),h=ue(&b),frame=read_bits(&b,1);
    if(w>=1024||h>=1024)return false;
    if(!frame)read_bits(&b,1);
    read_bits(&b,1);
    unsigned left=0,right=0,top=0,bottom=0;
    if(read_bits(&b,1)){left=ue(&b);right=ue(&b);top=ue(&b);bottom=ue(&b);}
    unsigned x=(chroma==0||separate||chroma==3)?1:2,y=(chroma==1&&!separate?2:1)*(2-frame);
    uint64_t cropw=((uint64_t)left+right)*x,croph=((uint64_t)top+bottom)*y;
    w=(w+1)*16;h=(h+1)*16*(2-frame);
    if(b.bad||cropw>=w||croph>=h)return false;
    *width=w-(unsigned)cropw;*height=h-(unsigned)croph;
    if(syntax)*syntax=b;if(references)*references=refs;if(progressive)*progressive=frame;return true;
}
bool h264_dimensions(const uint8_t *p,size_t n,unsigned *width,unsigned *height) {
    return parse_sps(p,n,width,height,NULL,NULL,NULL);
}

static void skip_hrd(struct bits *b) {
    unsigned count=ue(b);if(count>31){b->bad=true;return;}read_bits(b,8);
    for(unsigned i=0;i<=count&&!b->bad;++i){ue(b);ue(b);read_bits(b,1);}read_bits(b,20);
}
struct writer {uint8_t p[1040];size_t at;bool bad;};
static void write_bits(struct writer *w,uint32_t n,unsigned count) {
    if(count>32||w->at+count>sizeof(w->p)*8){w->bad=true;return;}
    while(count--){if((n>>count)&1)w->p[w->at/8]|=(uint8_t)(1u<<(7-w->at%8));++w->at;}
}
static void write_ue(struct writer *w,uint32_t n) {
    if(n==UINT32_MAX){w->bad=true;return;}unsigned size=0;for(uint32_t v=n+1;v;v>>=1)++size;
    write_bits(w,0,size-1);write_bits(w,n+1,size);
}
size_t h264_sps_low_delay(const uint8_t *p,size_t n,uint8_t *out,size_t capacity) {
    size_t prefix,at=start(p,n,0,&prefix),next_prefix;
    if(at||at==n||start(p,n,prefix,&next_prefix)!=n||!h264_valid(p,n))return 0;
    unsigned width,height,refs;bool progressive;struct bits b;
    if(!parse_sps(p,n,&width,&height,&b,&refs,&progressive)||!progressive||refs>16)return 0;
    size_t vui_at=b.at,restriction_at=0;bool vui=read_bits(&b,1),restricted=false;
    if(vui){
        if(read_bits(&b,1)){unsigned aspect=read_bits(&b,8);if(aspect==255){read_bits(&b,16);read_bits(&b,16);}}
        if(read_bits(&b,1))read_bits(&b,1);
        if(read_bits(&b,1)){read_bits(&b,4);if(read_bits(&b,1))read_bits(&b,24);}
        if(read_bits(&b,1)){ue(&b);ue(&b);}
        if(read_bits(&b,1)){read_bits(&b,32);read_bits(&b,32);read_bits(&b,1);}
        bool nal=read_bits(&b,1);if(nal)skip_hrd(&b);bool vcl=read_bits(&b,1);if(vcl)skip_hrd(&b);
        if(nal||vcl)read_bits(&b,1);read_bits(&b,1);restriction_at=b.at;
        restricted=read_bits(&b,1);
        if(restricted){read_bits(&b,1);ue(&b);ue(&b);ue(&b);ue(&b);unsigned reorder=ue(&b),buffering=ue(&b);if(reorder||buffering<refs||buffering>16)return 0;}
    }
    if(b.bad||read_bits(&b,1)!=1)return 0; /* rbsp_stop_one_bit */
    while(!b.bad&&b.at<b.n*8)if(read_bits(&b,1))return 0;
    if(b.bad)return 0;
    if(restricted){if(n>capacity)return 0;memcpy(out,p,n);return n;}
    struct writer w={0};size_t keep=vui?restriction_at:vui_at;
    for(size_t bit=0;bit<keep;++bit)write_bits(&w,(b.p[bit/8]>>(7-bit%8))&1,1);
    if(!vui){write_bits(&w,1,1);write_bits(&w,0,8);} /* VUI, optional fields absent */
    write_bits(&w,1,1);write_bits(&w,1,1); /* restriction, motion vectors over picture boundaries */
    write_ue(&w,2);write_ue(&w,1);write_ue(&w,16);write_ue(&w,16);write_ue(&w,0);write_ue(&w,refs?refs:1);
    write_bits(&w,1,1);while(w.at%8)write_bits(&w,0,1);
    if(w.bad||capacity<5)return 0;memset(out,0,3);out[3]=1;out[4]=p[prefix];size_t used=5;unsigned zeros=0;
    for(size_t i=0;i<w.at/8;++i){uint8_t v=w.p[i];if(zeros>=2&&v<=3){if(used==capacity)return 0;out[used++]=3;zeros=0;}
        if(used==capacity)return 0;out[used++]=v;zeros=v?0:zeros+1;}
    return used;
}
size_t h264_without_parameter_sets(const uint8_t *p,size_t n,uint8_t *out,size_t capacity) {
    if(!h264_valid(p,n))return 0;size_t prefix,at=start(p,n,0,&prefix),used=0;bool vcl=false;
    while(at<n){size_t np,end=start(p,n,at+prefix,&np),last=end;while(last>at+prefix&&p[last-1]==0)--last;
        unsigned type=p[at+prefix]&31;size_t size=last-at;
        if(type!=7&&type!=8){if(size>capacity-used)return 0;memcpy(out+used,p+at,size);used+=size;if(type==1||type==5)vcl=true;}
        at=end;prefix=np;
    }return vcl?used:0;
}

static struct bits nal_bits(const uint8_t *p,size_t n) {
    struct bits b={0};unsigned zeros=0;
    for(size_t i=0;i<n&&b.n<sizeof(b.p);++i){if(zeros>=2&&p[i]==3){zeros=0;continue;}b.p[b.n++]=p[i];zeros=p[i]?0:zeros+1;}
    return b;
}
static bool save_set(struct h264_set sets[4],uint32_t id,uint32_t parent,const uint8_t *p,size_t n) {
    if(n+4>1024)return false;
    for(unsigned k=0;k<4;++k)if((sets[k].valid&&sets[k].id==id)||!sets[k].valid){
        sets[k]=(struct h264_set){.id=id,.parent=parent,.size=n+4,.valid=true};
        sets[k].data[3]=1;memcpy(sets[k].data+4,p,n);return true;
    }
    return false;
}
static struct h264_set *find_set(struct h264_set sets[4],uint32_t id) {
    for(unsigned k=0;k<4;++k)if(sets[k].valid&&sets[k].id==id)return &sets[k];return NULL;
}
int h264_config_frame(struct h264_config *c,const uint8_t *p,size_t n,bool *key,bool *changed) {
    *key=*changed=false;if(!h264_valid(p,n))return -1;
    size_t prefix,at=start(p,n,0,&prefix);bool vcl=false;uint32_t pps_id=0;
    while(at<n){size_t np,end=start(p,n,at+prefix,&np),last=end;while(last>at+prefix&&p[last-1]==0)--last;
        const uint8_t *nal=p+at+prefix;size_t size=last-at-prefix;unsigned type=nal[0]&31;
        struct bits b=nal_bits(nal+1,size-1);
        if(type==7){read_bits(&b,24);uint32_t id=ue(&b);if(b.bad||id>31||!save_set(c->sps,id,0,nal,size))return -1;}
        else if(type==8){uint32_t id=ue(&b),parent=ue(&b);if(b.bad||id>255||parent>31||!save_set(c->pps,id,parent,nal,size))return -1;}
        else if(type==1||type==5){ue(&b);uint32_t slice=ue(&b),id=ue(&b);if(b.bad||id>255||slice>9||slice%5==1||slice%5==3||slice%5==4)return -1;
            if(vcl&&(pps_id!=id||*key!=(type==5)))return -1;vcl=true;pps_id=id;*key=type==5;}
        at=end;prefix=np;
    }
    if(!vcl)return 0;
    struct h264_set *pps=find_set(c->pps,pps_id),*sps=pps?find_set(c->sps,pps->parent):NULL;
    if(!pps||!sps)return 0;
    bool different=c->sps_size!=sps->size||c->pps_size!=pps->size||memcmp(c->active_sps,sps->data,sps->size)||memcmp(c->active_pps,pps->data,pps->size);
    if(different){unsigned w,h;if(!*key)return 0;if(c->epoch==UINT32_MAX||!h264_dimensions(sps->data,sps->size,&w,&h))return -1;
        memcpy(c->active_sps,sps->data,sps->size);memcpy(c->active_pps,pps->data,pps->size);c->sps_size=sps->size;c->pps_size=pps->size;c->pps_id=pps_id;c->width=w;c->height=h;++c->epoch;*changed=true;
    }
    return 1;
}
