#include "files.h"
#include <string.h>
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3]; }
static uint64_t u64(const uint8_t *p) { return (uint64_t)u32(p)<<32|u32(p+4); }
static void p32(uint8_t *p,uint32_t v) { for (int i=3;i>=0;--i) { p[i]=(uint8_t)v; v>>=8; } }
static void p64(uint8_t *p,uint64_t v) { p32(p,(uint32_t)(v>>32)); p32(p+4,(uint32_t)v); }
static bool serial_valid(const char *s) {
    size_t n=strlen(s); if (!n || n>31) return false;
    for (size_t i=0;i<n;++i) {
        char c=s[i];
        if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_' || c=='.' || c==':')) return false;
    }
    return true;
}
bool myvava_file_decode(const uint8_t *p, size_t n, struct myvava_file *f) {
    if (n!=MYVAVA_FILE_DESC || (memcmp(p,"MVR1",4)&&memcmp(p,"MVR2",4))) return false;
    bool extended=p[3]=='2';
    memset(f,0,sizeof(*f)); f->id=u64(p+4); f->size=u32(p+12); memcpy(f->sha256,p+16,32);
    memcpy(f->serial,p+48,32); f->unix_seconds=u64(p+80); f->first_ms=u64(p+88); f->last_ms=u64(p+96);
    f->frames=u32(p+104); f->audio_frames=u32(p+108); f->sample_rate=u32(p+112);
    f->channels=p[116]; f->slot=p[117]; f->stream=p[118]; f->pir=p[119]; f->clock_valid=p[120];
    if (!f->id || !f->size || f->size>MYVAVA_FILE_MAX || f->size%188 || !f->frames ||
        !memchr(f->serial,0,32) || !serial_valid(f->serial) || f->slot>3 || f->stream>1 ||
        f->pir>1 || f->clock_valid>1 || p[121]!=1 || f->channels>2 ||
        (!!f->audio_frames!=(f->sample_rate>0 && f->channels>0)) ||
        f->last_ms<f->first_ms || f->last_ms-f->first_ms>180000) return false;
    if(extended){f->file_type=p[122];f->detection_flags=p[123];f->detection_type=u32(p+124);
        if((!f->file_type&&!f->detection_flags&&!f->detection_type)||(f->file_type&&(f->file_type<5||f->file_type>8))||f->detection_flags>7||
            (!(f->detection_flags&1)&&((f->detection_flags&2)||f->detection_type)))return false;
        if((f->detection_flags&4)&&(f->size>1048476||f->frames!=1))return false;
    }else for (unsigned i=122;i<128;++i) if (p[i]) return false;
    for (size_t i=strlen(f->serial)+1;i<32;++i) if (f->serial[i]) return false;
    uint8_t nonzero=0; for (unsigned i=0;i<32;++i) nonzero|=f->sha256[i];
    return nonzero!=0;
}
bool myvava_file_encode(uint8_t p[MYVAVA_FILE_DESC], const struct myvava_file *f) {
    memset(p,0,MYVAVA_FILE_DESC); memcpy(p,"MVR1",4); p64(p+4,f->id); p32(p+12,f->size);
    memcpy(p+16,f->sha256,32); memcpy(p+48,f->serial,32); p64(p+80,f->unix_seconds);
    p64(p+88,f->first_ms); p64(p+96,f->last_ms); p32(p+104,f->frames);
    p32(p+108,f->audio_frames); p32(p+112,f->sample_rate);
    p[116]=f->channels; p[117]=f->slot; p[118]=f->stream; p[119]=f->pir; p[120]=f->clock_valid; p[121]=1;
    if(f->file_type||f->detection_flags||f->detection_type){p[3]='2';p[122]=f->file_type;p[123]=f->detection_flags;p32(p+124,f->detection_type);}
    struct myvava_file check; return myvava_file_decode(p,MYVAVA_FILE_DESC,&check);
}
