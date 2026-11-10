#include "desired.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REC_SIZE 256u
#define FILE_SIZE (16u+4u*REC_SIZE)
/* Keep the nine v1 slots at their original offsets; v2 uses a spare word
 * for the new scalar. Do not serialize SETTINGS_COUNT * 20 past this record. */
#define V1_SETTINGS 9u
bool desired_valid(const struct desired *d) {
    if (d->mask & ~(((1u<<SETTINGS_COUNT)-1u) & ~(1u<<SETTING_PIR_LEVEL))) return false;
    for (unsigned j=0;j<SETTINGS_COUNT;++j) if (d->mask & (1u<<j)) {
        uint32_t wire, p[5]; const uint32_t *v=d->values[j];
        if (j==SETTING_PIR_MODE) {
            const char *mode=v[0]==0?"battery":v[0]==1?"monitor":v[0]==2?"custom":"invalid";
            if (settings_pir_parameters(mode,v[1],v[2],v[3],0,p) || memcmp(v,p,sizeof(p))) return false;
        } else if (settings_index(settings_name(j),v[0],&wire)!=(int)j || v[1] || v[2] || v[3] || v[4]) return false;
    }
    return true;
}
int desired_load(const char *dir, const struct registration r[4], struct desired d[4]) {
    memset(d,0,4*sizeof(*d)); struct desired loaded[4]={0}; char path[1024]; uint8_t data[FILE_SIZE+1];
    if (snprintf(path,sizeof(path),"%s/settings.bin",dir)>=(int)sizeof(path)) return -1;
    int fd=open(path,O_RDONLY|O_NOFOLLOW); if (fd<0) return errno==ENOENT?0:-1;
    size_t off=0;
    while (off<sizeof(data)) {
        ssize_t n=read(fd,data+off,sizeof(data)-off);
        if (n<0 && errno==EINTR) continue;
        if (n<0) { close(fd); return -1; }
        if (!n) break;
        off+=(size_t)n;
    }
    close(fd);
    bool v2=off>=8 && !memcmp(data,"VAVACFG2",8);
    if (off!=FILE_SIZE || (!v2 && memcmp(data,"VAVACFG1",8)) || vava_u32(data+8)!=4 ||
        vava_u32(data+12)!=vava_crc32(data+16,FILE_SIZE-16)) return -1;
    for (unsigned i=0;i<4;++i) {
        const uint8_t *b=data+16+i*REC_SIZE; if (!b[0]) continue;
        struct desired parsed={.revision=vava_u32(b+56),.mask=vava_u32(b+60)};
        if (!v2 && (parsed.mask & ~((1u<<V1_SETTINGS)-1))) return -1;
        for (unsigned j=0;j<V1_SETTINGS;++j) for (unsigned k=0;k<5;++k)
            parsed.values[j][k]=vava_u32(b+64+20*j+4*k);
        if (v2) parsed.values[SETTING_DETECTION_BOX][0]=vava_u32(b+244);
        if (b[0]!=1 || !desired_valid(&parsed)) return -1;
        if (r[i].stage>=2 && !memcmp(b+4,r[i].serial,32) && !memcmp(b+36,r[i].mac,18)) loaded[i]=parsed;
    }
    memcpy(d,loaded,sizeof(loaded));
    return 0;
}
int desired_save(const char *dir, const struct registration r[4], const struct desired d[4]) {
    uint8_t data[FILE_SIZE]={0}; char path[1024],temp[1024];
    if (snprintf(path,sizeof(path),"%s/settings.bin",dir)>=(int)sizeof(path) ||
        snprintf(temp,sizeof(temp),"%s/.settings-XXXXXX",dir)>=(int)sizeof(temp)) return -1;
    bool v2=false;
    for (unsigned i=0;i<4;++i) if (r[i].stage>=2 && (d[i].mask&(1u<<SETTING_DETECTION_BOX))) v2=true;
    memcpy(data,v2?"VAVACFG2":"VAVACFG1",8); vava_put32(data+8,4);
    for (unsigned i=0;i<4;++i) {
        if (!desired_valid(&d[i])) return -1;
        if (r[i].stage<2) continue;
        uint8_t *b=data+16+i*REC_SIZE; b[0]=1;
        memcpy(b+4,r[i].serial,32); memcpy(b+36,r[i].mac,18);
        vava_put32(b+56,d[i].revision); vava_put32(b+60,d[i].mask);
        for (unsigned j=0;j<V1_SETTINGS;++j) for (unsigned k=0;k<5;++k)
            vava_put32(b+64+20*j+4*k,d[i].values[j][k]);
        if (v2) vava_put32(b+244,d[i].values[SETTING_DETECTION_BOX][0]);
    }
    vava_put32(data+12,vava_crc32(data+16,FILE_SIZE-16));
    int fd=mkstemp(temp); if (fd<0) return -1;
    bool ok=true; size_t off=0;
    while (off<sizeof(data)) {
        ssize_t n=write(fd,data+off,sizeof(data)-off);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { ok=false; break; }
        off+=(size_t)n;
    }
    if (ok && fsync(fd)) ok=false;
    if (close(fd)) ok=false;
    if (ok && rename(temp,path)) ok=false;
    if (!ok) unlink(temp);
    /* rename commits the visible state even when directory fsync is unsupported. */
    int dfd=open(dir,O_RDONLY); if (dfd>=0) { (void)fsync(dfd); close(dfd); }
    return ok?0:-1;
}
bool desired_json(const struct desired *d, unsigned pending, const char *delivery, char *out, size_t size) {
    int n=snprintf(out,size,"{\"revision\":%u,\"storage\":\"station_settings_bin\",\"automatic_replay\":false,"
        "\"pending_mask\":%u,\"delivery\":\"%s\",\"values\":{",d->revision,pending,delivery);
    if (n<0 || (size_t)n>=size) return false;
    size_t used=(size_t)n; bool comma=false;
    for (unsigned j=0;j<SETTINGS_COUNT;++j) if (d->mask & (1u<<j)) {
        const uint32_t *v=d->values[j];
        if (j==SETTING_PIR_MODE) n=snprintf(out+used,size-used,"%s\"pir-mode\":{\"mode\":%u,\"sensitivity\":%u,"
            "\"wait_seconds\":%u,\"record_seconds\":%u}",comma?",":"",v[0],v[1],v[2],v[3]);
        else n=snprintf(out+used,size-used,"%s\"%s\":%u",comma?",":"",settings_name(j),v[0]);
        if (n<0 || (size_t)n>=size-used) return false;
        used+=(size_t)n; comma=true;
    }
    n=snprintf(out+used,size-used,"}}"); return n>=0 && (size_t)n<size-used;
}
