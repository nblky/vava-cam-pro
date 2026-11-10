#include "registry.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool registry_mac(const char *s) {
    if (strlen(s)!=17) return false;
    bool nonzero=false;
    for (unsigned i=0;i<17;++i) {
        if (i%3==2) { if (s[i]!=':') return false; }
        else { if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return false;
               nonzero |= s[i]!='0'; }
    }
    return nonzero;
}
int registry_load(const char *dir, struct registration r[4]) {
    char path[1024]; uint8_t data[240], extra;
    if (snprintf(path,sizeof(path),"%s/registry.bin",dir)>=(int)sizeof(path)) return -1;
    FILE *f=fopen(path,"rb");
    if (!f) return errno==ENOENT ? 0 : -1;
    size_t n=fread(data,1,sizeof(data),f);
    if (n!=sizeof(data) || ferror(f)) { fclose(f); return -1; }
    size_t more=fread(&extra,1,1,f);
    bool ok=!ferror(f) && !more;
    fclose(f);
    if (!ok || memcmp(data,"VAVAREG1",8) || vava_u32(data+8)!=4 ||
        vava_u32(data+12)!=vava_crc32(data+16,224)) return -1;
    struct registration parsed[4]={0};
    for (unsigned i=0;i<4;++i) {
        const uint8_t *p=data+16+i*56;
        if (p[0]==0) continue;
        if (p[0]!=2 && p[0]!=3) return -1;
        parsed[i].stage=p[0];
        if (!vava_serial(p+4,parsed[i].serial) || p[53]) return -1;
        memcpy(parsed[i].mac,p+36,18);
        if (parsed[i].mac[0] && !registry_mac(parsed[i].mac)) return -1;
        for (unsigned j=0;j<i;++j) {
            if (!strcmp(parsed[i].serial,parsed[j].serial)) return -1;
            if (parsed[i].mac[0] && !strcmp(parsed[i].mac,parsed[j].mac)) return -1;
        }
    }
    memcpy(r,parsed,sizeof(parsed));
    return 0;
}
int registry_save(const char *dir, const struct registration r[4]) {
    char path[1024],tmp[1024]; uint8_t data[240]={0};
    if (snprintf(path,sizeof(path),"%s/registry.bin",dir)>=(int)sizeof(path) ||
        snprintf(tmp,sizeof(tmp),"%s/.registry-XXXXXX",dir)>=(int)sizeof(tmp)) return -1;
    memcpy(data,"VAVAREG1",8); vava_put32(data+8,4);
    for (unsigned i=0;i<4;++i) {
        if (r[i].stage<2) continue;
        uint8_t *p=data+16+i*56; p[0]=r[i].stage;
        memcpy(p+4,r[i].serial,32); memcpy(p+36,r[i].mac,18);
    }
    vava_put32(data+12,vava_crc32(data+16,224));
    int fd=mkstemp(tmp);
    if (fd<0) return -1;
    size_t off=0; int result=-1;
    while (off<sizeof(data)) {
        ssize_t n=write(fd,data+off,sizeof(data)-off);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) goto done;
        off+=(size_t)n;
    }
    if (fsync(fd)) goto done;
    if (close(fd)) { fd=-1; goto done; } fd=-1;
    if (rename(tmp,path)) goto done;
    /* Directory fsync is supported on Linux; some host filesystems reject it. */
    int dfd=open(dir,O_RDONLY);
    if (dfd>=0) { (void)fsync(dfd); close(dfd); }
    result=0;
done:
    if (fd>=0) close(fd);
    if (result) unlink(tmp);
    return result;
}
