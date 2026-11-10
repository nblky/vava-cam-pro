#include "events.h"
#include "myvava.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static uint64_t get(const uint8_t *p, unsigned n) {
    uint64_t v=0; while (n--) v=(v<<8)|*p++; return v;
}
static void put(uint8_t *p, uint64_t v, unsigned n) {
    while (n) { p[--n]=(uint8_t)v; v>>=8; }
}
static bool text_valid(const uint8_t *p, size_t n) {
    bool ended=false;
    for (size_t i=0;i<n;++i) {
        if (!p[i]) ended=true;
        else if (ended || !((p[i]>='a' && p[i]<='z') || (p[i]>='A' && p[i]<='Z') ||
                 (p[i]>='0' && p[i]<='9') || p[i]=='_' || p[i]=='-' || p[i]=='.' || p[i]==':')) return false;
    }
    return ended;
}
const char *myvava_event_name(unsigned k) {
    static const char *names[]={"invalid","station_started","pairing_stage_saved","camera_report_200",
        "pir_wake_report","recording_requested","recording_stop_requested","journal_gap",
        "recording_committed","recording_failed","recording_recovered"};
    return k>=MVE_BOOT && k<=MVE_RECORD_RECOVERED?names[k]:names[0];
}
const char *myvava_event_category(unsigned kind) {
    if(kind==MVE_DETECTION||kind==MVE_PIR_WAKE)return "detection";
    if(kind==MVE_PAIR_STAGE)return "pairing";
    if(kind==MVE_GAP)return "storage";
    if(kind==MVE_RECORD_START||kind==MVE_RECORD_STOP_REQUEST||kind==MVE_RECORD_COMMITTED||kind==MVE_RECORD_FAILED||kind==MVE_RECORD_RECOVERED)return "recording";
    return "system";
}
void myvava_event_encode(uint8_t p[MYVAVA_EVENT_SIZE], const struct myvava_event *e) {
    memset(p,0,MYVAVA_EVENT_SIZE); memcpy(p,"MVE1",4); memcpy(p+4,e->origin,16);
    put(p+20,e->sequence,8); put(p+28,e->monotonic_ms,8); put(p+36,e->unix_ms,8);
    put(p+44,e->generation,8); put(p+52,e->related_id,8); put(p+60,e->dropped,8);
    put(p+68,e->result_raw,4); put(p+72,e->type_raw,4);
    p[76]=e->slot; p[77]=e->kind; p[78]=e->clock_valid;
    memcpy(p+80,e->serial,strnlen(e->serial,31)); memcpy(p+112,e->detail,strnlen(e->detail,47));
}
bool myvava_event_decode(const uint8_t *p, size_t n, struct myvava_event *e) {
    if (n!=MYVAVA_EVENT_SIZE || memcmp(p,"MVE1",4) || p[79] || p[78]>1 ||
        (p[76]>3 && p[76]!=255) || p[77]<MVE_BOOT || p[77]>MVE_RECORD_RECOVERED ||
        !text_valid(p+80,32) || !text_valid(p+112,48) || !get(p+20,8) || get(p+20,8)>INT64_MAX) return false;
    memset(e,0,sizeof(*e)); memcpy(e->origin,p+4,16); e->sequence=get(p+20,8);
    e->monotonic_ms=get(p+28,8); e->unix_ms=get(p+36,8); e->generation=get(p+44,8);
    e->related_id=get(p+52,8); e->dropped=get(p+60,8);
    e->result_raw=(uint32_t)get(p+68,4); e->type_raw=(uint32_t)get(p+72,4);
    e->slot=p[76]; e->kind=p[77]; e->clock_valid=p[78]!=0;
    memcpy(e->serial,p+80,32); memcpy(e->detail,p+112,48); return true;
}
size_t myvava_event_json(const struct myvava_event *e, char *out, size_t cap) {
    char origin[33]; myvava_hex(e->origin,16,origin);
    int n=snprintf(out,cap,"{\"origin\":\"%s\",\"sequence\":\"%016" PRIx64 "\",\"kind\":\"%s\","
        "\"category\":\"%s\",\"slot\":%u,\"serial\":\"%s\",\"monotonic_ms\":%" PRIu64 ",\"unix_ms\":%" PRIu64 ","
        "\"clock_valid\":%s,\"generation\":%" PRIu64 ",\"related_id\":\"%016" PRIx64 "\","
        "\"dropped_since_boot\":%" PRIu64 ",\"result_raw\":%" PRIu32 ",\"type_raw\":%" PRIu32 ",\"detail\":\"%s\"}",
        origin,e->sequence,myvava_event_name(e->kind),myvava_event_category(e->kind),e->slot,e->serial,e->monotonic_ms,e->unix_ms,
        e->clock_valid?"true":"false",e->generation,e->related_id,e->dropped,e->result_raw,e->type_raw,e->detail);
    return n<0 || (size_t)n>=cap?0:(size_t)n;
}
