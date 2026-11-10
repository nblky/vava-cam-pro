#include "h264.h"
#undef NDEBUG
#include <assert.h>
#include <string.h>
static const uint8_t sps[][19]={
 {0,0,0,1,0x67,0x42,0,0x2a,0x96,0x35,0x40,0xf0,0x04,0x4f,0xcb,0x37,1,1,1},
 {0,0,0,1,0x67,0x42,0,0x1f,0x96,0x35,0x40,0xa0,0x0b,0x74,0xdc,4,4,4,8},
 {0,0,0,1,0x67,0x42,0,0x1e,0x96,0x35,0x41,0x40,0x5f,0xf2,0xcd,0xc0,0x40,0x40,0x80}
};
static void config_epochs(void) {
    const uint8_t pps[]={0,0,1,0x68,0xe0},idr[]={0,0,1,0x65,0xbc},pred[]={0,0,1,0x41,0xf0};
    const uint8_t unknown[]={0,0,1,0x65,0xb5},bframe[]={0,0,1,0x41,0xac};
    struct h264_config c={0};bool key,changed;
    assert(h264_config_frame(&c,idr,sizeof(idr),&key,&changed)==0);
    assert(h264_config_frame(&c,sps[2],sizeof(sps[2]),&key,&changed)==0);
    assert(h264_config_frame(&c,pps,sizeof(pps),&key,&changed)==0);
    assert(h264_config_frame(&c,pred,sizeof(pred),&key,&changed)==0);
    assert(h264_config_frame(&c,idr,sizeof(idr),&key,&changed)==1&&key&&changed);
    assert(c.epoch==1&&c.width==640&&c.height==360);
    assert(h264_config_frame(&c,idr,sizeof(idr),&key,&changed)==1&&!changed&&c.epoch==1);
    assert(h264_config_frame(&c,pred,sizeof(pred),&key,&changed)==1&&!key&&!changed);
    assert(h264_config_frame(&c,unknown,sizeof(unknown),&key,&changed)==0);
    assert(h264_config_frame(&c,bframe,sizeof(bframe),&key,&changed)==-1);
    /* Reusing parameter ID 0 does not commit until its IDR arrives. */
    assert(h264_config_frame(&c,sps[1],sizeof(sps[1]),&key,&changed)==0);
    assert(h264_config_frame(&c,pred,sizeof(pred),&key,&changed)==0&&c.epoch==1);
    uint8_t au[64];memcpy(au,sps[1],19);memcpy(au+19,pps,sizeof(pps));memcpy(au+19+sizeof(pps),idr,sizeof(idr));
    assert(h264_config_frame(&c,au,19+sizeof(pps)+sizeof(idr),&key,&changed)==1&&changed&&c.epoch==2&&c.width==1280);
    memcpy(au,idr,sizeof(idr));memcpy(au+sizeof(idr),pred,sizeof(pred));
    assert(h264_config_frame(&c,au,sizeof(idr)+sizeof(pred),&key,&changed)==-1);
    /* Four distinct candidate PPS IDs are bounded; no unbounded allocation. */
    const uint8_t sets[]={0xe0,0x58,0x78,0x26,0x2e};
    for(unsigned i=1;i<4;++i){uint8_t p[]={0,0,1,0x68,sets[i]};assert(h264_config_frame(&c,p,sizeof(p),&key,&changed)==0);}
    uint8_t extra[]={0,0,1,0x68,sets[4]};assert(h264_config_frame(&c,extra,sizeof(extra),&key,&changed)==-1);
    c.epoch=UINT32_MAX;assert(h264_config_frame(&c,sps[0],sizeof(sps[0]),&key,&changed)==0);
    assert(h264_config_frame(&c,idr,sizeof(idr),&key,&changed)==-1);
}
int main(void) {
    config_epochs();
    unsigned width,height;
    const unsigned w[]={1920,1280,640},h[]={1080,720,360};
    for(unsigned i=0;i<3;++i){assert(h264_valid(sps[i],sizeof(sps[i])));assert(h264_dimensions(sps[i],sizeof(sps[i]),&width,&height));assert(width==w[i]&&height==h[i]);}
    /* Full camera SPS, including VUI and RBSP termination. The cache fixtures
     * above deliberately contain only enough syntax to establish dimensions. */
    const uint8_t camera_sps[]={0,0,0,1,0x67,0x42,0,0x1e,0x96,0x35,0x41,0x40,0x5f,0xf2,0xcd,0xc0,0x40,0x40,0x40,0x80};
    uint8_t low[2048],again[2048];size_t n=h264_sps_low_delay(camera_sps,sizeof(camera_sps),low,sizeof(low));assert(n>0&&h264_valid(low,n));
    assert(h264_dimensions(low,n,&width,&height)&&width==640&&height==360);
    assert(h264_sps_low_delay(low,n,again,sizeof(again))==n&&!memcmp(low,again,n));
    assert(!h264_sps_low_delay(camera_sps,sizeof(camera_sps),low,5));
    for(size_t j=0;j<sizeof(camera_sps);++j)assert(!h264_sps_low_delay(camera_sps,j,low,sizeof(low)));
    /* Independently constructed SPS: optional VUI absent; colour/timing/HRD
     * present; and a declared reorder requirement which must not be erased. */
    const uint8_t no_vui[]={0,0,0,1,0x67,0x42,0,0x1e,0xda,2,0x80,0xbf,0xe5,0x40};
    const uint8_t hrd[]={0,0,0,1,0x67,0x42,0,0x1e,0xda,2,0x80,0xbf,0xe5,0x9b,0x80,0x80,0x80,0xa0,0,0,3,0,0x20,0,0,3,2,0x9c,3,0x80,0,2,0x40};
    const uint8_t reordered[]={0,0,0,1,0x67,0x42,0,0x1e,0xda,2,0x80,0xbf,0xe5,0x80,0x6d,4,0x42,0x29,0xc0};
    assert(h264_sps_low_delay(no_vui,sizeof(no_vui),low,sizeof(low))>0);
    n=h264_sps_low_delay(hrd,sizeof(hrd),low,sizeof(low));assert(n>0);
    assert(h264_sps_low_delay(low,n,again,sizeof(again))==n&&!memcmp(low,again,n));
    assert(!h264_sps_low_delay(reordered,sizeof(reordered),low,sizeof(low)));
    for(size_t byte=5;byte<sizeof(camera_sps);++byte)for(unsigned bit=0;bit<8;++bit){
        uint8_t mutated[sizeof(camera_sps)];memcpy(mutated,camera_sps,sizeof(mutated));mutated[byte]^=(uint8_t)(1u<<bit);
        size_t bytes=h264_sps_low_delay(mutated,sizeof(mutated),low,sizeof(low));
        if(bytes)assert(h264_valid(low,bytes)&&h264_dimensions(low,bytes,&width,&height));
    }
    const uint8_t au[]={0,0,0,1,0x67,0x42,0,0x1e,0x80,0,0,1,0x68,0xe0,0,0,0,1,0x65,0xbc};
    const uint8_t only_vcl[]={0,0,0,1,0x65,0xbc};uint8_t stripped[sizeof(au)];
    assert(h264_without_parameter_sets(au,sizeof(au),stripped,sizeof(stripped))==sizeof(only_vcl)&&!memcmp(stripped,only_vcl,sizeof(only_vcl)));
    assert(!h264_without_parameter_sets(au,sizeof(au),stripped,5));
    const uint8_t escaped[]={0,0,1,0x65,0x80,0,0,3,0,1};
    assert(h264_valid(escaped,sizeof(escaped)));
    uint8_t bad[sizeof(escaped)];memcpy(bad,escaped,sizeof(bad));
    bad[7]=0;assert(!h264_valid(bad,sizeof(bad)));
    bad[7]=3;bad[8]=4;assert(!h264_valid(bad,sizeof(bad)));
    bad[8]=0;bad[3]=0xe5;assert(!h264_valid(bad,sizeof(bad)));
    bad[3]=4;assert(!h264_valid(bad,sizeof(bad)));
    assert(!h264_valid(escaped,8)); /* truncated escape */
    assert(!h264_valid(escaped,3));assert(!h264_valid(escaped,0));
    assert(!h264_dimensions(escaped,sizeof(escaped),&width,&height));
    for(size_t i=0;i<12;++i)assert(!h264_dimensions(sps[0],i,&width,&height));
    return 0;
}
