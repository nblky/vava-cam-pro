#include "ts_mux.h"
#include <string.h>

enum { PAT=0, PMT=1, VIDEO=2, AUDIO=3 };
static const unsigned pids[]={0,0x1000,0x100,0x101};
static uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t v=UINT32_MAX;
    while (n--) {
        v^=(uint32_t)*p++<<24;
        for (unsigned i=0;i<8;++i) v=(v<<1)^((v&0x80000000)?0x04c11db7u:0);
    }
    return v;
}
static bool table(struct ts_mux *m, unsigned kind, uint8_t *section, size_t n, ts_write_fn write, void *arg) {
    uint32_t sum=crc(section,n);
    for (unsigned i=0;i<4;++i) section[n+i]=(uint8_t)(sum>>(24-i*8));
    uint8_t packet[188]; memset(packet,0xff,sizeof(packet));
    packet[0]=0x47; packet[1]=(uint8_t)(0x40|(pids[kind]>>8)); packet[2]=(uint8_t)pids[kind];
    packet[3]=(uint8_t)(0x10|(m->cc[kind]++&15)); packet[4]=0;
    memcpy(packet+5,section,n+4); return write(arg,packet,sizeof(packet));
}
static bool tables(struct ts_mux *m, ts_write_fn write, void *arg) {
    uint8_t pat[16]={0,0xb0,13,0,1,0xc1,0,0,0,1,0xf0,0};
    uint8_t pmt[26]={2,0xb0,18,0,1,0xc1,0,0,0xe1,0,0xf0,0,0x1b,0xe1,0,0xf0,0,
                    0x0f,0xe1,1,0xf0,0};
    if (m->audio) { pmt[2]=23; pmt[5]=0xc3; }
    return table(m,PAT,pat,12,write,arg) && table(m,PMT,pmt,m->audio?22:17,write,arg);
}
static void stamp(uint8_t *p, uint64_t pts) {
    p[0]=(uint8_t)(0x21|((pts>>29)&14)); p[1]=(uint8_t)(pts>>22);
    p[2]=(uint8_t)(((pts>>14)&0xfe)|1); p[3]=(uint8_t)(pts>>7); p[4]=(uint8_t)((pts<<1)|1);
}
static bool pes(struct ts_mux *m, unsigned track, uint64_t pts, bool key,
                const uint8_t *p, size_t n, ts_write_fn write, void *arg) {
    uint8_t prefix[20]={0,0,1,0,0,0,0x80,0x80,5};
    prefix[3]=track==VIDEO?0xe0:0xc0;
    if (track==AUDIO) { prefix[4]=(uint8_t)((n+8)>>8); prefix[5]=(uint8_t)(n+8); }
    stamp(prefix+9,pts);
    size_t headers=14;
    if (track==VIDEO) { const uint8_t aud[]={0,0,0,1,9,0xf0}; memcpy(prefix+14,aud,6); headers+=6; }
    size_t at=0,total=headers+n;
    while (at<total) {
        uint8_t packet[188]; memset(packet,0xff,sizeof(packet));
        bool start=at==0, pcr=start && track==VIDEO;
        packet[0]=0x47; packet[1]=(uint8_t)((start?0x40:0)|(pids[track]>>8)); packet[2]=(uint8_t)pids[track];
        packet[3]=(uint8_t)(0x10|(m->cc[track]++&15));
        size_t payload=total-at, room=pcr?176:184;
        if (payload>room) payload=room;
        size_t pos=188-payload;
        if (pos>4) {
            packet[3]|=0x20; packet[4]=(uint8_t)(pos-5);
            if (pos>5) packet[5]=(uint8_t)((pcr?0x10:0)|(key && start?0x40:0));
            if (pcr) {
                packet[6]=(uint8_t)(pts>>25); packet[7]=(uint8_t)(pts>>17); packet[8]=(uint8_t)(pts>>9);
                packet[9]=(uint8_t)(pts>>1); packet[10]=(uint8_t)((pts<<7)|0x7e); packet[11]=0;
            }
        }
        for (size_t k=0;k<payload;++k,++at) packet[pos+k]=at<headers?prefix[at]:p[at-headers];
        if (!write(arg,packet,sizeof(packet))) return false;
    }
    return true;
}
static bool bad(struct ts_mux *m, const char *error) { m->error=error; return false; }
bool ts_mux_frame(struct ts_mux *m, bool audio, bool key, uint64_t ms,
                  const uint8_t *p, size_t n, ts_write_fn write, void *arg) {
    if (m->error) return false;
    if (!m->started) {
        if (audio || !key) return bad(m,"first_frame_not_idr");
        m->first_ms=m->last_ms=ms; m->started=true;
    }
    /* Clip length and a live clock discontinuity are different constraints.
     * Keep full-width PTS for ordering; the TS fields naturally wrap at 33 bits. */
    if (ms<m->first_ms || ms-m->first_ms>(UINT64_MAX-90000)/90 ||
        (!m->continuous && ms-m->first_ms>180000) ||
        (m->continuous && ms>m->last_ms && ms-m->last_ms>15000))
        return bad(m,"media_timestamp_range");
    uint64_t pts=90000+(ms-m->first_ms)*90;
    if (!audio) {
        if (m->video_pts && pts<=m->video_pts) return bad(m,"video_timestamp_order");
        if (!m->video_pts || key) if (!tables(m,write,arg)) return bad(m,"storage_write_failed_or_quota");
        if (!pes(m,VIDEO,pts,key,p,n,write,arg)) return bad(m,"storage_write_failed_or_quota");
        m->video_pts=pts;
    } else {
        /* Validate the complete ADTS bundle before publishing any of it. */
        static const unsigned rates[]={96000,88200,64000,48000,44100,32000,24000,22050,16000,12000,11025,8000,7350};
        size_t off=0; unsigned frames=0, rate=0, channels=0;
        while (off<n) {
            const uint8_t *a=p+off; size_t remain=n-off;
            if (remain<7 || a[0]!=255 || (a[1]&0xf6)!=0xf0 || (a[2]>>6)!=1 || (a[6]&3))
                return bad(m,"invalid_aac_adts");
            unsigned index=(a[2]>>2)&15, ch=((a[2]&1)<<2)|(a[3]>>6);
            size_t len=((size_t)(a[3]&3)<<11)|((size_t)a[4]<<3)|(a[5]>>5), header=(a[1]&1)?7:9;
            if (index>=13 || !ch || ch>2 || len<=header || len>remain) return bad(m,"invalid_aac_adts");
            if ((rate && (rate!=rates[index] || channels!=ch)) ||
                (m->audio && (m->sample_rate!=rates[index] || m->channels!=ch))) return bad(m,"audio_format_changed");
            rate=rates[index]; channels=ch; off+=len;
            if (++frames>64) return bad(m,"audio_bundle_limit");
        }
        if (!frames) return bad(m,"invalid_aac_adts");
        uint64_t tail=(uint64_t)(frames-1)*1024*90000/rate;
        if (pts>UINT64_MAX-tail ||
            (!m->continuous && pts+tail>90000+UINT64_C(180000)*90))
            return bad(m,"media_timestamp_range");
        if (m->audio_pts && pts<=m->audio_pts) return bad(m,"audio_timestamp_order");
        if (!m->audio) {
            m->audio=true; m->sample_rate=rate; m->channels=channels;
            if (!tables(m,write,arg)) return bad(m,"storage_write_failed_or_quota");
        }
        off=0;
        for (unsigned i=0;i<frames;++i) {
            const uint8_t *a=p+off;
            size_t len=((size_t)(a[3]&3)<<11)|((size_t)a[4]<<3)|(a[5]>>5);
            uint64_t at=pts+(uint64_t)i*1024*90000/rate;
            if (!pes(m,AUDIO,at,false,a,len,write,arg)) return bad(m,"storage_write_failed_or_quota");
            m->audio_pts=at; off+=len; ++m->audio_frames;
        }
    }
    if (ms>m->last_ms) m->last_ms=ms;
    return true;
}
