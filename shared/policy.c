#include "policy.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

void policy_default(struct policy *p) {
    memset(p,0,sizeof(*p)); p->enabled=true; p->level=2;
    p->record_seconds=20; p->utc_offset_minutes=480;
}
void policy_daily_default(struct policy *p){policy_default(p);p->managed=true;p->mode=ARM_ON;p->idle_seconds=15;p->camera_duration=true;p->confirm_detection=true;}
bool policy_time(const char *s, unsigned *seconds) {
    if (strlen(s)!=8 || s[2]!=':' || s[5]!=':') return false;
    unsigned v[3];
    for (unsigned i=0;i<3;++i) {
        unsigned k=i*3;
        if (s[k]<'0' || s[k]>'9' || s[k+1]<'0' || s[k+1]>'9') return false;
        v[i]=(unsigned)(s[k]-'0')*10+(unsigned)(s[k+1]-'0');
    }
    if (v[0]>23 || v[1]>59 || v[2]>59) return false;
    *seconds=v[0]*3600+v[1]*60+v[2]; return true;
}
bool policy_days(const char *s, unsigned *mask) {
    *mask=0;
    for (;*s;++s) {
        if (*s<'0' || *s>'6' || (*mask & (1u<<(*s-'0')))) return false;
        *mask|=1u<<(*s-'0');
    }
    return *mask!=0;
}
bool policy_valid(const struct policy *p) {
    if (p->mode>ARM_SCHEDULE || p->level<1 || p->level>3 ||
        p->record_seconds<5 || p->record_seconds>120 || p->idle_seconds>3600 ||
        p->utc_offset_minutes< -720 || p->utc_offset_minutes>840) return false;
    for (unsigned i=0;i<POLICY_PLANS;++i) {
        const struct plan *q=&p->plans[i];
        if (!q->enabled && !q->days && !q->start && !q->end && !q->all_day) continue;
        if (!q->days || q->days>127 || q->start>=86400 || q->end>=86400 ||
            (!q->all_day && q->start==q->end)) return false;
    }
    return true;
}
bool policy_evaluate(const struct policy *p, time_t utc, bool valid) {
    if (!p->managed || !p->enabled || p->mode==ARM_OFF) return false;
    if (p->mode==ARM_ON) return true;
    if (!valid || utc<0 || (uint64_t)utc>UINT32_MAX) return false;
    int64_t local=(int64_t)utc+p->utc_offset_minutes*60;
    if (local<0) return false;
    unsigned day=(unsigned)((local/86400+4)%7), yesterday=(day+6)%7;
    unsigned sec=(unsigned)(local%86400);
    for (unsigned i=0;i<POLICY_PLANS;++i) {
        const struct plan *q=&p->plans[i];
        if (!q->enabled) continue;
        bool today=(q->days&(1u<<day))!=0, prev=(q->days&(1u<<yesterday))!=0;
        if (q->all_day && today) return true;
        if (q->start<q->end && today && sec>=q->start && sec<q->end) return true;
        if (q->start>q->end && ((today && sec>=q->start) || (prev && sec<q->end))) return true;
    }
    return false;
}
#define REC_SIZE 256u
#define FILE_SIZE (16u+4u*REC_SIZE)
static void encode(uint8_t *b, const struct policy *p) {
    vava_put32(b,p->revision); b[4]=p->managed; b[5]=p->enabled; b[6]=(uint8_t)p->mode; b[7]=(uint8_t)p->level;
    vava_put32(b+8,(uint32_t)(p->utc_offset_minutes+720));
    b[20]=p->cloud_upload; b[21]=p->nas_backup;b[22]=p->camera_duration;b[23]=p->confirm_detection;
    vava_put32(b+12,p->record_seconds); vava_put32(b+16,p->idle_seconds);
    for (unsigned i=0;i<POLICY_PLANS;++i) {
        uint8_t *q=b+24+16*i; const struct plan *a=&p->plans[i];
        q[0]=a->enabled; q[1]=a->all_day; q[2]=(uint8_t)a->days;
        vava_put32(q+4,a->start); vava_put32(q+8,a->end);
    }
}
static bool decode(const uint8_t *b, struct policy *p) {
    if (b[4]>1 || b[5]>1 || b[20]>1 || b[21]>1 || b[22]>1 || b[23]>1 || vava_u32(b+8)>1560) return false;
    p->cloud_upload=b[20]; p->nas_backup=b[21];p->camera_duration=b[22];p->confirm_detection=b[23];
    p->revision=vava_u32(b); p->managed=b[4]; p->enabled=b[5]; p->mode=b[6]; p->level=b[7];
    p->utc_offset_minutes=(int)vava_u32(b+8)-720;
    p->record_seconds=vava_u32(b+12); p->idle_seconds=vava_u32(b+16);
    for (unsigned i=0;i<POLICY_PLANS;++i) {
        const uint8_t *q=b+24+16*i; struct plan *a=&p->plans[i];
        if (q[0]>1 || q[1]>1) return false;
        a->enabled=q[0]; a->all_day=q[1]; a->days=q[2];
        a->start=vava_u32(q+4); a->end=vava_u32(q+8);
    }
    return policy_valid(p);
}
int policy_load_mode(const char *dir, const struct registration r[4], struct policy p[4],bool daily) {
    for (unsigned i=0;i<4;++i){if(daily)policy_daily_default(&p[i]);else policy_default(&p[i]);}
    char path[1024]; uint8_t data[FILE_SIZE], extra;
    if (snprintf(path,sizeof(path),"%s/policies.bin",dir)>=(int)sizeof(path)) return -1;
    FILE *f=fopen(path,"rb"); if (!f) return errno==ENOENT ? 0:-1;
    size_t n=fread(data,1,sizeof(data),f), more=fread(&extra,1,1,f);
    bool ok=n==sizeof(data) && !more && !ferror(f); fclose(f);
    if (!ok || memcmp(data,"VAVAPOL1",8) || vava_u32(data+8)!=4 ||
        vava_u32(data+12)!=vava_crc32(data+16,sizeof(data)-16)) return -1;
    for (unsigned i=0;i<4;++i) {
        const uint8_t *b=data+16+i*REC_SIZE;
        if (!b[0]) continue;
        struct policy parsed={0};
        if (b[0]!=1 || !decode(b+64,&parsed)) return -1;
        /* Ignore stale slot data; never transfer policy to a different identity. */
        if (r[i].stage>=2 && !memcmp(b+4,r[i].serial,32) && !memcmp(b+36,r[i].mac,18)) p[i]=parsed;
    }
    return 0;
}
int policy_load(const char *dir,const struct registration r[4],struct policy p[4]){return policy_load_mode(dir,r,p,false);}
int policy_save(const char *dir, const struct registration r[4], const struct policy p[4]) {
    uint8_t data[FILE_SIZE]={0}; char path[1024],tmp[1024];
    if (snprintf(path,sizeof(path),"%s/policies.bin",dir)>=(int)sizeof(path) ||
        snprintf(tmp,sizeof(tmp),"%s/.policy-XXXXXX",dir)>=(int)sizeof(tmp)) return -1;
    memcpy(data,"VAVAPOL1",8); vava_put32(data+8,4);
    for (unsigned i=0;i<4;++i) {
        if (!policy_valid(&p[i])) return -1;
        if (r[i].stage<2) continue;
        uint8_t *b=data+16+i*REC_SIZE; b[0]=1;
        memcpy(b+4,r[i].serial,32); memcpy(b+36,r[i].mac,18); encode(b+64,&p[i]);
    }
    vava_put32(data+12,vava_crc32(data+16,sizeof(data)-16));
    int fd=mkstemp(tmp); if (fd<0) return -1;
    int result=-1; size_t off=0;
    while (off<sizeof(data)) {
        ssize_t n=write(fd,data+off,sizeof(data)-off);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) goto done;
        off+=(size_t)n;
    }
    if (fsync(fd)) goto done;
    if (close(fd)) { fd=-1; goto done; } fd=-1;
    if (rename(tmp,path)) goto done;
    int dfd=open(dir,O_RDONLY);
    if (dfd<0) goto done;
    bool durable=fsync(dfd)==0; if (close(dfd)) durable=false;
    if (durable) result=0;
done:
    if (fd>=0) close(fd);
    if (result) unlink(tmp);
    return result;
}
bool policy_json(const struct policy *p, bool armed, bool valid, const char *delivery,
                 unsigned attempts, char *out, size_t size) {
    static const char *const modes[]={"off","on","schedule"};
    int n=snprintf(out,size,"{\"managed\":%s,\"enabled\":%s,\"mode\":\"%s\",\"level\":%u,"
        "\"revision\":%" PRIu32 ",\"desired_armed\":%s,\"target_level\":%u,\"clock_valid\":%s,"
        "\"utc_offset_minutes\":%d,\"record_seconds\":%u,\"idle_seconds\":%u,"
        "\"cloud_upload\":%s,\"nas_backup\":%s,\"camera_duration\":%s,\"confirm_detection\":%s,\"delivery\":\"%s\",\"attempts\":%u,\"applied\":null,\"plans\":[",
        p->managed?"true":"false",p->enabled?"true":"false",modes[p->mode],p->level,p->revision,
        armed?"true":"false",armed?p->level:0,valid?"true":"false",p->utc_offset_minutes,
        p->record_seconds,p->idle_seconds,p->cloud_upload?"true":"false",p->nas_backup?"true":"false",p->camera_duration?"true":"false",p->confirm_detection?"true":"false",delivery,attempts);
    if (n<0 || (size_t)n>=size) return false;
    size_t used=(size_t)n; bool comma=false;
    for (unsigned i=0;i<POLICY_PLANS;++i) {
        const struct plan *q=&p->plans[i]; if (!q->days) continue;
        n=snprintf(out+used,size-used,"%s{\"index\":%u,\"days_mask\":%u,\"start_seconds\":%u,\"end_seconds\":%u,\"all_day\":%s,\"enabled\":%s}",
            comma?",":"",i,q->days,q->start,q->end,q->all_day?"true":"false",q->enabled?"true":"false");
        if (n<0 || (size_t)n>=size-used) return false;
        used+=(size_t)n; comma=true;
    }
    n=snprintf(out+used,size-used,"]}"); return n>=0 && (size_t)n<size-used;
}
