#include "rtsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static bool decimal(const char *p, unsigned max, unsigned *out) {
    if (!*p) return false;
    unsigned n=0;
    for (;*p;++p) {
        if (*p<'0' || *p>'9' || n>max/10 || (n==max/10 && (unsigned)(*p-'0')>max%10)) return false;
        n=n*10+(unsigned)(*p-'0');
    }
    *out=n; return true;
}
int rtsp_parse(const uint8_t *data, size_t n, struct rtsp_request *out, size_t *used) {
    size_t end=0;
    for (size_t i=0;i+3<n;++i) if (!memcmp(data+i,"\r\n\r\n",4)) { end=i+4; break; }
    if (!end) return n>=RTSP_INPUT_LIMIT ? -1 : 0;
    if (end>=RTSP_INPUT_LIMIT) return -1;
    for (size_t i=0;i<end;++i)
        if ((data[i]<32 && data[i]!='\r' && data[i]!='\n' && data[i]!='\t') || data[i]>126) return -1;
    char buffer[RTSP_INPUT_LIMIT]; memcpy(buffer,data,end); buffer[end]=0;
    memset(out,0,sizeof(*out));
    char *line=buffer, *next=strstr(line,"\r\n");
    if (!next) return -1;
    *next=0; next+=2;
    char *space=strchr(line,' ');
    if (!space || space==line || (size_t)(space-line)>=sizeof(out->method)) return -1;
    for (char *p=line;p<space;++p) if (*p<'A' || *p>'Z') {
        if (*p!='_') return -1;
    }
    memcpy(out->method,line,(size_t)(space-line)); line=space+1; space=strchr(line,' ');
    if (!space || space==line || (size_t)(space-line)>=sizeof(out->uri) || strcmp(space+1,"RTSP/1.0")) return -1;
    memcpy(out->uri,line,(size_t)(space-line));
    bool seq=false,transport=false,session=false,length=false,auth=false;
    while (*next) {
        line=next; next=strstr(line,"\r\n"); if (!next) return -1;
        *next=0; next+=2; if (!*line) break;
        char *colon=strchr(line,':'); if (!colon || colon==line) return -1;
        for (char *p=line;p<colon;++p)
            if (!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') || *p=='-')) return -1;
        *colon++=0; while (*colon==' ' || *colon=='\t') ++colon;
        char *tail=colon+strlen(colon);
        while (tail>colon && (tail[-1]==' ' || tail[-1]=='\t')) *--tail=0;
        if (!strcasecmp(line,"CSeq")) {
            if (seq || !decimal(colon,2147483647,&out->cseq)) return -1; seq=true;
        } else if (!strcasecmp(line,"Transport")) {
            if (transport || strlen(colon)>=sizeof(out->transport)) return -1;
            strcpy(out->transport,colon); transport=true;
        } else if (!strcasecmp(line,"Session")) {
            if (session || strlen(colon)>=sizeof(out->session)) return -1;
            strcpy(out->session,colon); session=true;
        } else if (!strcasecmp(line,"Authorization")) {
            if (auth || strlen(colon)>=sizeof(out->authorization)) return -1;
            strcpy(out->authorization,colon); auth=true;
        } else if (!strcasecmp(line,"Content-Length")) {
            unsigned value;
            if (length || !decimal(colon,0,&value)) return -1; length=true;
        } else if (!strcasecmp(line,"Transfer-Encoding")) return -1;
    }
    if (!seq) return -1;
    *used=end; return 1;
}
bool rtsp_path(const char *uri, unsigned *slot, unsigned *stream, bool *track) {
    const char *p=uri;
    if (!strncmp(p,"rtsp://",7)) {
        p+=7; const char *slash=strchr(p,'/');
        if (!slash || slash==p) return false;
        for (const char *q=p;q<slash;++q)
            if (!((*q>='a' && *q<='z') || (*q>='A' && *q<='Z') || (*q>='0' && *q<='9') || *q=='.' || *q==':' || *q=='-')) return false;
        p=slash;
    }
    if (strncmp(p,"/camera/",8)) return false; p+=8;
    if (*p<'0' || *p>'3') return false; *slot=(unsigned)(*p++-'0');
    *stream=1; *track=false;
    if (*p=='/' && (p[1]=='0' || p[1]=='1')) { *stream=(unsigned)(p[1]-'0'); p+=2; }
    if (!strcmp(p,"/trackID=0")) { *track=true; return true; }
    return !*p || !strcmp(p,"/");
}
bool rtsp_transport(const char *text, unsigned *channel) {
    char buf[256]; if (strlen(text)>=sizeof(buf)) return false;
    strcpy(buf,text); char *save=NULL, *p=strtok_r(buf,";",&save);
    if (!p || strcasecmp(p,"RTP/AVP/TCP")) return false;
    bool channels=false,unicast=false;
    while ((p=strtok_r(NULL,";",&save))) {
        while (*p==' ') ++p;
        if (!strcasecmp(p,"unicast") && !unicast) unicast=true;
        else if (!strncasecmp(p,"interleaved=",12) && !channels) {
            char *dash=strchr(p+12,'-'); unsigned a,b;
            if (!dash) return false; *dash++=0;
            if (!decimal(p+12,254,&a) || !decimal(dash,255,&b) || b!=a+1) return false;
            *channel=a; channels=true;
        } else if (strcasecmp(p,"mode=play") && strcasecmp(p,"mode=\"PLAY\"")) return false;
    }
    return channels && unicast;
}
static size_t prefix(const uint8_t *p, size_t n) {
    if (n>=4 && !memcmp(p,"\0\0\0\1",4)) return 4;
    if (n>=3 && !memcmp(p,"\0\0\1",3)) return 3;
    return 0;
}
static size_t next_nal(const uint8_t *p, size_t n, size_t at) {
    for (;at<n;++at) if (prefix(p+at,n-at)) return at;
    return n;
}
static void base64(char *out, const uint8_t *p, size_t n) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t j=0;
    for (size_t i=0;i<n;i+=3) {
        unsigned x=(unsigned)p[i]<<16;
        if (i+1<n) x|=(unsigned)p[i+1]<<8;
        if (i+2<n) x|=p[i+2];
        out[j++]=alphabet[x>>18]; out[j++]=alphabet[(x>>12)&63];
        out[j++]=i+1<n ? alphabet[(x>>6)&63] : '=';
        out[j++]=i+2<n ? alphabet[x&63] : '=';
    }
    out[j]=0;
}
int rtsp_sdp(char *out, size_t cap, const uint8_t *sps, size_t ns, const uint8_t *pps, size_t np) {
    size_t ps=prefix(sps,ns), pp=prefix(pps,np);
    if (!ps || !pp || ns<ps+4 || np<=pp || ns>1024 || np>1024 || (sps[ps]&31)!=7 || (pps[pp]&31)!=8) return -1;
    sps+=ps; ns-=ps; pps+=pp; np-=pp;
    char a[1369],b[1369]; base64(a,sps,ns); base64(b,pps,np);
    int n=snprintf(out,cap,"v=0\r\no=- 0 1 IN IP4 127.0.0.1\r\ns=VAVA local camera\r\n"
        "c=IN IP4 0.0.0.0\r\nt=0 0\r\na=control:*\r\nm=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\na=fmtp:96 packetization-mode=1;profile-level-id=%02X%02X%02X;"
        "sprop-parameter-sets=%s,%s\r\na=control:trackID=0\r\n",sps[1],sps[2],sps[3],a,b);
    return n>=0 && (size_t)n<cap ? n : -1;
}
static void put16(uint8_t *p, unsigned v) { p[0]=(uint8_t)(v>>8); p[1]=(uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static bool packet(struct rtsp_session *s, const uint8_t *p, size_t n, bool marker,
                   rtsp_emit emit, void *opaque) {
    uint8_t out[1416]; if (n>1400) return false;
    out[0]='$'; out[1]=(uint8_t)s->channel; put16(out+2,(unsigned)n+12);
    out[4]=0x80; out[5]=(uint8_t)(96|(marker ? 128 : 0)); put16(out+6,s->sequence++);
    put32(out+8,s->timestamp); put32(out+12,s->ssrc); memcpy(out+16,p,n);
    ++s->packets; s->octets+=(uint32_t)n;
    return emit(opaque,out,n+16);
}
bool rtsp_h264(struct rtsp_session *s, const uint8_t *data, size_t n, bool marker,
               rtsp_emit emit, void *opaque) {
    size_t at=next_nal(data,n,0);
    while (at<n) {
        size_t start=at+prefix(data+at,n-at), next=next_nal(data,n,start);
        size_t end=next; while (end>start && data[end-1]==0) --end;
        at=next; if (end==start) continue;
        const uint8_t *p=data+start; size_t len=end-start;
        unsigned type=p[0]&31;
        if ((p[0]&0x80) || !type || type>=24) continue;
        bool last=marker && next==n;
        if (len<=1400) { if (!packet(s,p,len,last,emit,opaque)) return false; }
        else {
            uint8_t fragment[1400]; size_t offset=1;
            fragment[0]=(uint8_t)((p[0]&0xe0)|28);
            while (offset<len) {
                size_t size=len-offset; if (size>1398) size=1398;
                bool final=offset+size==len;
                fragment[1]=(uint8_t)(type|(offset==1 ? 0x80 : 0)|(final ? 0x40 : 0));
                memcpy(fragment+2,p+offset,size);
                if (!packet(s,fragment,size+2,last && final,emit,opaque)) return false;
                offset+=size;
            }
        }
    }
    return true;
}
bool rtsp_rtcp_valid(const uint8_t *data, size_t n) {
    if (!n) return false;
    while (n) {
        if (n<4 || (data[0]>>6)!=2 || data[1]<192 || data[1]>223) return false;
        size_t len=((size_t)data[2]*256+data[3]+1)*4;
        if (len>n) return false;
        data+=len; n-=len;
    }
    return true;
}
bool rtsp_report(struct rtsp_session *s, uint64_t unix_ms, rtsp_emit emit, void *opaque) {
    /* Compound SR + SDES CNAME, RFC 3550. 28 + 24 bytes. */
    uint8_t out[56]={0}; out[0]='$'; out[1]=(uint8_t)(s->channel+1); put16(out+2,52);
    out[4]=0x80; out[5]=200; put16(out+6,6); put32(out+8,s->ssrc);
    put32(out+12,(uint32_t)(unix_ms/1000+2208988800UL));
    put32(out+16,(uint32_t)(((unix_ms%1000)<<32)/1000));
    put32(out+20,s->timestamp); put32(out+24,s->packets); put32(out+28,s->octets);
    out[32]=0x81; out[33]=202; put16(out+34,5); put32(out+36,s->ssrc);
    out[40]=1; out[41]=12; memcpy(out+42,"vava-station",12);
    return emit(opaque,out,sizeof(out));
}
