#include "files.h"
#include <fcntl.h>
#include <openssl/rand.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3];
}
static void put32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)(n>>24); p[1]=(uint8_t)(n>>16); p[2]=(uint8_t)(n>>8); p[3]=(uint8_t)n;
}
bool myvava_decode(const uint8_t *p, size_t n, struct myvava_message *m) {
    if (n<MYVAVA_HEADER || n>MYVAVA_FILE_PACKET_MAX || memcmp(p,"MVA1",4) || p[55] ||
        p[54]<MYVAVA_SYNC || p[54]>MYVAVA_MEDIA_FRAME) return false;
    size_t length=(size_t)p[52]*256+p[53];
    if (length!=n-MYVAVA_HEADER ||
        length>(p[54]==MYVAVA_FILE_DATA?MYVAVA_FILE_PAYLOAD_MAX:p[54]==MYVAVA_SYNC?MYVAVA_CONTROL_BATCH_MAX:MYVAVA_CHUNK)) return false;
    memset(m,0,sizeof(*m));
    m->type=p[54]; memcpy(m->boot,p+4,16); memcpy(m->epoch,p+20,16);
    m->id=(uint64_t)get32(p+36)<<32 | get32(p+40);
    m->total=get32(p+44); m->offset=get32(p+48);
    m->length=length; m->data=p+MYVAVA_HEADER;
    return true;
}
size_t myvava_encode(uint8_t out[MYVAVA_PACKET_MAX], const struct myvava_message *m) {
    return myvava_encode_sized(out,MYVAVA_PACKET_MAX,m);
}
size_t myvava_encode_sized(uint8_t *out, size_t capacity, const struct myvava_message *m) {
    if (m->length>(m->type==MYVAVA_FILE_DATA?MYVAVA_FILE_PAYLOAD_MAX:m->type==MYVAVA_SYNC?MYVAVA_CONTROL_BATCH_MAX:MYVAVA_CHUNK) ||
        capacity<MYVAVA_HEADER || m->length>capacity-MYVAVA_HEADER || (m->length && !m->data)) return 0;
    memset(out,0,MYVAVA_HEADER); memcpy(out,"MVA1",4);
    memcpy(out+4,m->boot,16); memcpy(out+20,m->epoch,16);
    put32(out+36,(uint32_t)(m->id>>32)); put32(out+40,(uint32_t)m->id);
    put32(out+44,m->total); put32(out+48,m->offset);
    out[52]=(uint8_t)(m->length>>8); out[53]=(uint8_t)m->length; out[54]=m->type;
    if (m->length) memcpy(out+MYVAVA_HEADER,m->data,m->length);
    return MYVAVA_HEADER+m->length;
}
uint64_t myvava_millis(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) return 0;
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
bool myvava_random(uint8_t *out, size_t n) { return n<=1024 && RAND_bytes(out,(int)n)==1; }
bool myvava_unhex(const char *text, uint8_t *out, size_t n) {
    if (strlen(text)!=n*2) return false;
    for (size_t i=0;i<n;++i) {
        unsigned v=0;
        for (unsigned k=0;k<2;++k) {
            unsigned char c=(unsigned char)text[2*i+k];
            if (c>='0' && c<='9') c-='0';
            else if (c>='a' && c<='f') c=(unsigned char)(c-'a'+10);
            else return false;
            v=v*16+c;
        }
        out[i]=(uint8_t)v;
    }
    return true;
}
void myvava_hex(const uint8_t *p, size_t n, char *out) {
    static const char chars[]="0123456789abcdef";
    for (size_t i=0;i<n;++i) { out[2*i]=chars[p[i]>>4]; out[2*i+1]=chars[p[i]&15]; }
    out[2*n]=0;
}
bool myvava_id_valid(const char *id) {
    size_t n=strlen(id);
    if (!n || n>MYVAVA_ID_MAX) return false;
    for (size_t i=0;i<n;++i) {
        char c=id[i];
        if (!((c>='a'&&c<='z') || (c>='A'&&c<='Z') || (c>='0'&&c<='9') || c=='-' || c=='_')) return false;
    }
    return true;
}
bool myvava_key_read(const char *path, uint8_t key[32]) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return false;
    struct stat st; char text[67];
    bool ok=!fstat(fd,&st) && S_ISREG(st.st_mode) && st.st_uid==geteuid() && !(st.st_mode&0077);
    ssize_t n=ok?read(fd,text,sizeof(text)-1):-1;
    close(fd);
    if (n!=64 && !(n==65 && text[64]=='\n')) return false;
    text[64]=0; ok=myvava_unhex(text,key,32); memset(text,0,sizeof(text));
    return ok;
}
