#include "auth.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
bool auth_read(const char *path, char *out, size_t cap, size_t minimum) {
    struct stat st;
    if (lstat(path,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) return false;
    FILE *f=fopen(path,"rb"); if (!f) return false;
    size_t n=fread(out,1,cap,f); bool ok=!ferror(f); fclose(f);
    if (!ok || n>=cap) return false;
    while (n && (out[n-1]=='\n' || out[n-1]=='\r')) --n;
    if (n<minimum) return false;
    for (size_t i=0;i<n;++i) if ((unsigned char)out[i]<33 || (unsigned char)out[i]>126) return false;
    out[n]=0; return true;
}
bool auth_equal(const char *a, const char *b) {
    size_t n=strlen(a),m=strlen(b); unsigned diff=(unsigned)(n^m);
    for (size_t i=0;i<n;++i) diff|=(unsigned char)a[i]^(i<m?(unsigned char)b[i]:0u);
    return !diff;
}
void auth_base64(char *out, const char *in) {
    static const char abc[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n=strlen(in),j=0;
    for (size_t i=0;i<n;i+=3) {
        unsigned v=(unsigned)(unsigned char)in[i]<<16;
        if (i+1<n) v|=(unsigned)(unsigned char)in[i+1]<<8;
        if (i+2<n) v|=(unsigned char)in[i+2];
        out[j++]=abc[(v>>18)&63]; out[j++]=abc[(v>>12)&63];
        out[j++]=i+1<n?abc[(v>>6)&63]:'='; out[j++]=i+2<n?abc[v&63]:'=';
    }
    out[j]=0;
}
