#include "profile.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

bool profile_name_valid(const char *name) {
    size_t n=strnlen(name,PROFILE_NAME_BYTES+1),at=0; unsigned count=0;
    if (n>PROFILE_NAME_BYTES) return false;
    while (at<n) {
        uint32_t c=(unsigned char)name[at++],min=0; unsigned tail=0;
        if (c>=0xc2 && c<=0xdf) { c&=31; tail=1; min=0x80; }
        else if (c>=0xe0 && c<=0xef) { c&=15; tail=2; min=0x800; }
        else if (c>=0xf0 && c<=0xf4) { c&=7; tail=3; min=0x10000; }
        else if (c>=0x80) return false;
        if (at+tail>n) return false;
        for (unsigned j=0;j<tail;++j) {
            unsigned b=(unsigned char)name[at++];
            if ((b&0xc0)!=0x80) return false;
            c=(c<<6)|(b&63);
        }
        if (c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff) || c<0x20 ||
            (c>=0x7f && c<=0x9f) || (c>=0x2028 && c<=0x202e) ||
            (c>=0x2066 && c<=0x2069) || ++count>32) return false;
    }
    return true;
}
static int hex_digit(unsigned c) {
    if (c>='0' && c<='9') return (int)c-'0';
    if (c>='a' && c<='f') return (int)c-'a'+10;
    if (c>='A' && c<='F') return (int)c-'A'+10;
    return -1;
}
bool profile_from_hex(const char *hex, char name[PROFILE_NAME_BYTES+1]) {
    memset(name,0,PROFILE_NAME_BYTES+1);
    if (!strcmp(hex,"-")) return true;
    size_t n=strlen(hex);
    if (!n || n%2 || n>PROFILE_NAME_BYTES*2) return false;
    for (size_t j=0;j<n;j+=2) {
        int a=hex_digit((unsigned char)hex[j]),b=hex_digit((unsigned char)hex[j+1]);
        if (a<0 || b<0 || (!a && !b)) return false;
        name[j/2]=(char)((a<<4)|b);
    }
    return profile_name_valid(name);
}
bool profile_json(const struct profile *p, char *out, size_t capacity) {
    if (!profile_name_valid(p->name)) return false;
    char escaped[PROFILE_NAME_BYTES*2+1]; size_t at=0;
    for (const char *s=p->name;*s;++s) {
        if (*s=='"' || *s=='\\') escaped[at++]='\\';
        escaped[at++]=*s;
    }
    escaped[at]=0;
    int n=snprintf(out,capacity,"{\"name\":\"%s\",\"revision\":%u,\"storage\":\"station_profiles_bin\"}",escaped,p->revision);
    return n>=0 && (size_t)n<capacity;
}
#define PROFILE_REC 160u
#define PROFILE_FILE (16u+4u*PROFILE_REC)
int profile_load(const char *dir, const struct registration r[4], struct profile p[4]) {
    memset(p,0,4*sizeof(*p)); char path[1024]; uint8_t data[PROFILE_FILE+1];
    if (snprintf(path,sizeof(path),"%s/profiles.bin",dir)>=(int)sizeof(path)) return -1;
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
    if (off!=PROFILE_FILE || memcmp(data,"VAVAPRF1",8) || vava_u32(data+8)!=4 ||
        vava_u32(data+12)!=vava_crc32(data+16,PROFILE_FILE-16)) return -1;
    struct profile parsed[4]={0};
    for (unsigned j=0;j<4;++j) {
        const uint8_t *b=data+16+j*PROFILE_REC; if (!b[0]) continue;
        char serial[32];
        if (b[0]!=1 || !vava_serial(b+4,serial) || b[53] || b[156]) return -1;
        if (b[36] && !registry_mac((const char *)b+36)) return -1;
        struct profile value={.revision=vava_u32(b+56)};
        memcpy(value.name,b+60,sizeof(value.name));
        if (!profile_name_valid(value.name)) return -1;
        if (r[j].stage>=2 && !memcmp(b+4,r[j].serial,32) && !memcmp(b+36,r[j].mac,18)) parsed[j]=value;
    }
    memcpy(p,parsed,sizeof(parsed)); return 0;
}
int profile_save(const char *dir, const struct registration r[4], const struct profile p[4]) {
    uint8_t data[PROFILE_FILE]={0}; char path[1024],tmp[1024];
    if (snprintf(path,sizeof(path),"%s/profiles.bin",dir)>=(int)sizeof(path) ||
        snprintf(tmp,sizeof(tmp),"%s/.profiles-XXXXXX",dir)>=(int)sizeof(tmp)) return -1;
    memcpy(data,"VAVAPRF1",8); vava_put32(data+8,4);
    for (unsigned j=0;j<4;++j) {
        if (!profile_name_valid(p[j].name)) return -1;
        if (r[j].stage<2) continue;
        uint8_t *b=data+16+j*PROFILE_REC; b[0]=1;
        memcpy(b+4,r[j].serial,32); memcpy(b+36,r[j].mac,18);
        vava_put32(b+56,p[j].revision); memcpy(b+60,p[j].name,strlen(p[j].name));
    }
    vava_put32(data+12,vava_crc32(data+16,PROFILE_FILE-16));
    int fd=mkstemp(tmp); if (fd<0) return -1;
    bool ok=true; size_t off=0;
    while (off<sizeof(data)) {
        ssize_t n=write(fd,data+off,sizeof(data)-off);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { ok=false; break; }
        off+=(size_t)n;
    }
    if (ok && fsync(fd)) ok=false;
    if (close(fd)) ok=false;
    if (ok && rename(tmp,path)) ok=false;
    if (!ok) unlink(tmp);
    int dfd=open(dir,O_RDONLY); if (dfd>=0) { (void)fsync(dfd); close(dfd); }
    return ok?0:-1;
}
