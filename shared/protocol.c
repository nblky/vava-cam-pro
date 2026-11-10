#include "protocol.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

uint32_t vava_u32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
uint64_t vava_u64(const uint8_t *p) { return vava_u32(p) | (uint64_t)vava_u32(p+4)<<32; }
void vava_put32(uint8_t *p, uint32_t v) {
    for (unsigned i=0; i<4; ++i) p[i]=(uint8_t)(v>>(i*8));
}
uint32_t vava_crc32(const uint8_t *p, size_t n) {
    uint32_t crc=UINT32_MAX;
    while (n--) {
        crc ^= *p++;
        for (unsigned i=0; i<8; ++i) crc=(crc>>1)^((0u-(crc&1u))&0xedb88320u);
    }
    return ~crc;
}
void vava_encode(uint8_t out[128], uint32_t command, const uint8_t payload[116]) {
    memset(out,0,128);
    vava_put32(out,0xeb000001u);
    vava_put32(out+8,command);
    if (payload) memcpy(out+12,payload,116);
    vava_put32(out+4,vava_crc32(out+8,120));
}
bool vava_decode(const uint8_t in[128], uint32_t *command) {
    if (vava_u32(in)!=0xeb000001u || vava_u32(in+4)!=vava_crc32(in+8,120)) return false;
    *command=vava_u32(in+8);
    return true;
}
bool vava_serial(const uint8_t field[32], char out[32]) {
    size_t n=0;
    while (n<32 && field[n]) {
        unsigned c=field[n];
        if (!((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9') ||
              c=='-' || c=='_' || c=='.' || c==':')) return false;
        ++n;
    }
    if (!n || n==32) return false;
    memset(out,0,32);
    memcpy(out,field,n);
    return true;
}

/* The legacy pairing string is at most 27 bytes. This MD5 implementation
 * handles exactly that single-block use, not authentication for our API. */
void vava_guest_digest(uint32_t seed, char out[33]) {
    static const uint32_t k[64]={
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
    static const unsigned shifts[16]={7,12,17,22,5,9,14,20,4,11,16,23,6,10,15,21};
    uint8_t block[64]={0}, digest[16];
    int64_t signed_seed=seed<=INT32_MAX ? (int64_t)seed : (int64_t)seed-INT64_C(4294967296);
    int len=snprintf((char *)block,56,"vava:%" PRId64 ":2019",signed_seed);
    block[len]=0x80;
    vava_put32(block+56,(uint32_t)len*8);
    uint32_t a=0x67452301,b=0xefcdab89,c=0x98badcfe,d=0x10325476;
    for (unsigned i=0;i<64;++i) {
        uint32_t f,g;
        if (i<16) { f=(b&c)|(~b&d); g=i; }
        else if (i<32) { f=(d&b)|(~d&c); g=(5*i+1)%16; }
        else if (i<48) { f=b^c^d; g=(3*i+5)%16; }
        else { f=c^(b|~d); g=(7*i)%16; }
        uint32_t x=a+f+k[i]+vava_u32(block+g*4);
        unsigned s=shifts[(i/16)*4+i%4];
        a=d; d=c; c=b; b+= (x<<s)|(x>>(32-s));
    }
    vava_put32(digest,a+0x67452301); vava_put32(digest+4,b+0xefcdab89);
    vava_put32(digest+8,c+0x98badcfe); vava_put32(digest+12,d+0x10325476);
    for (unsigned i=0;i<16;++i) snprintf(out+2*i,3,"%02x",digest[i]);
}
bool vava_guest_valid(const uint8_t payload[116]) {
    char digest[33];
    vava_guest_digest(vava_u32(payload),digest);
    for (unsigned i=0;i<32;++i) {
        unsigned c=payload[4+i];
        if (c>='A' && c<='F') c+=32;
        if (c!=(unsigned char)digest[i]) return false;
    }
    return payload[36]==0;
}
bool vava_media_header(const uint8_t in[24], struct vava_media *out) {
    if (vava_u32(in)!=0xeb000002u) return false;
    out->stream=in[4]; out->codec=in[5]; out->type=in[6]; out->flags=in[10];
    out->length=vava_u32(in+12); out->timestamp=vava_u64(in+16);
    return out->length>0 && out->length<=VAVA_MAX_FRAME &&
           ((out->stream<=1 && out->type<=1) ||
            ((out->stream==8 || out->stream==9) && out->type==8));
}
bool vava_media_body(const struct vava_media *h, const uint8_t *p, size_t *length) {
    *length=h->length;
    if (h->type<=1 && (h->flags&1)) {
        static const uint8_t tail[]={0xeb,0x55,0x55,0xeb};
        if (*length<4 || memcmp(p+*length-4,tail,4)) return false;
        *length-=4;
    }
    return *length>0;
}
bool vava_talk_aac(const uint8_t *p, size_t n) {
    /* One complete AAC-LC ADTS frame, 8 kHz mono, no CRC or raw-data blocks.
     * Keep below the stock station's 1024-byte envelope and decoder FIFO unit. */
    if (n<8 || n>1000 || p[0]!=0xff || (p[1]&0xf7)!=0xf1 || (p[2]>>6)!=1 ||
        ((p[2]>>2)&15)!=11 || (((p[2]&1)<<2)|(p[3]>>6))!=1 || (p[6]&3)) return false;
    return (((size_t)(p[3]&3)<<11)|((size_t)p[4]<<3)|(p[5]>>5))==n;
}
void vava_talk_header(uint8_t out[24], size_t n, uint8_t number, uint64_t ms) {
    memset(out,0,24); vava_put32(out,0xeb000002u);
    out[4]=2; out[5]=3; out[6]=8; out[9]=number; out[11]=1;
    vava_put32(out+12,(uint32_t)n); vava_put32(out+16,(uint32_t)ms); vava_put32(out+20,(uint32_t)(ms>>32));
}
