#include "settings.h"
#include "protocol.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *const names[]={"mirror","ir","antiflicker","pir-level","pir-mode","osd","volume","quality","playback-audio","detection-box"};
static const uint32_t commands[]={11,12,16,17,43,15,38,10,18,33};
static const char *const phases[]={"none","queued","sent","echo_observed","echo_timeout",
                                  "disconnected","queue_timeout"};
int settings_index(const char *name, unsigned value, uint32_t *wire_value) {
    for (unsigned j=0;j<SETTINGS_COUNT;++j) if (j!=SETTING_PIR_MODE && !strcmp(name,names[j])) {
        if ((j==0 && value>3) || (j==1 && value>1) ||
            (j==2 && value!=50 && value!=60) || (j==3 && value>3) ||
            (j==5 && value>1) || (j==6 && value>100) || (j==SETTING_QUALITY && value>2) ||
            (j==8 && value>1) || (j==SETTING_DETECTION_BOX && value>1)) return -1;
        *wire_value=j==2 ? value==60 : value;
        return (int)j;
    }
    return -1;
}
bool settings_detection_ready(const struct settings *s, uint64_t generation) {
    const struct setting_report *r=&s->reports[3]; const uint32_t *v=r->values;
    return r->seen && r->generation==generation && v[0]<=1 && v[1]<=1 &&
        v[2]>=1 && v[2]<=100 && v[3]<=1 && v[4]<=100 && v[5]<=1 &&
        v[6]<v[7] && v[7]<=100 && v[8]<v[9] && v[9]<=100;
}
static bool pir_valid(const uint32_t p[5]) {
    if (p[4]>1) return false;
    if (!p[0]) return p[1]==2 && p[2]==20 && p[3]==10;
    if (p[0]==1) return p[1]==1 && p[2]==0 && p[3]==20;
    return p[0]==2 && p[1]>=1 && p[1]<=3 && p[2]>=5 && p[2]<=120 && p[2]%5==0 &&
           p[3]>=5 && p[3]<=60 && p[3]%5==0;
}
unsigned settings_record_seconds(const struct settings *s,uint64_t generation,const char **source){
    const struct setting_report *r=&s->reports[2];const struct setting_operation *op=&s->operations[SETTING_PIR_MODE];
    if(op->phase==SET_ECHO&&op->generation==generation&&pir_valid(op->pir)&&(!r->seen||r->generation!=generation||op->echo_at>=r->at)){*source="camera_echo";return op->pir[3];}
    if(r->seen&&r->generation==generation&&pir_valid(r->values)){*source="camera_report";return r->values[3];}
    *source="fallback_10_seconds";return 10;
}
const char *settings_pir_parameters(const char *mode, unsigned sensitivity, unsigned wait,
                                   unsigned record, unsigned armed, uint32_t out[5]) {
    out[4]=armed;
    if (!strcmp(mode,"battery")) { out[0]=0; out[1]=2; out[2]=20; out[3]=10; }
    else if (!strcmp(mode,"monitor")) { out[0]=1; out[1]=1; out[2]=0; out[3]=20; }
    else if (!strcmp(mode,"custom")) { out[0]=2; out[1]=sensitivity; out[2]=wait; out[3]=record; }
    else return "invalid_pir_mode";
    if (!pir_valid(out)) return "invalid_pir_custom_parameters";
    return NULL;
}
const char *settings_pir_mode(const struct settings *s, const char *mode, unsigned sensitivity,
                             unsigned wait, unsigned record, uint64_t generation, uint32_t out[5]) {
    const char *error=settings_pir_parameters(mode,sensitivity,wait,record,0,out);
    if (error) return error;
    const struct setting_report *r=&s->reports[2];
    if (!r->seen || r->generation!=generation || !pir_valid(r->values))
        return "valid_current_pir_report_42_required";
    out[4]=r->values[4];
    return NULL;
}
uint32_t settings_command(unsigned index) { return commands[index]; }
const char *settings_name(unsigned index) { return names[index]; }
static int check(const struct settings *s, unsigned index, bool same, uint64_t generation, bool retry) {
    const struct setting_operation *op=&s->operations[index];
    same=same && op->phase!=SET_NONE && op->generation==generation && op->dedup_valid;
    if (s->active) return same && s->active==index+1 ? 1 : -1;
    return same && !retry ? 1 : 0;
}
int settings_check(const struct settings *s, unsigned index, unsigned value,
                   uint64_t generation, bool retry) {
    return check(s,index,s->operations[index].value==value,generation,retry);
}
int settings_check_pir(const struct settings *s, const uint32_t values[5], uint64_t generation, bool retry) {
    return check(s,SETTING_PIR_MODE,!memcmp(s->operations[SETTING_PIR_MODE].pir,values,5*sizeof(*values)),generation,retry);
}
void settings_begin(struct settings *s, unsigned index, uint32_t value, uint32_t wire_value,
                    uint32_t id, unsigned slot, uint64_t generation, uint64_t now, uint64_t tx_end) {
    struct setting_operation *op=&s->operations[index];
    memset(op,0,sizeof(*op));
    op->id=id; op->value=value; op->wire_value=wire_value; op->phase=SET_QUEUED;
    op->dedup_valid=true;
    op->generation=generation; op->queued_at=now; op->tx_end=tx_end;
    vava_put32(op->payload,wire_value);
    /* Stock session/context fields are ignored by these camera handlers and echoed.
     * Our process-unique marker only correlates bytes; it proves no device effect. */
    vava_put32(op->payload+4,id); vava_put32(op->payload+8,slot);
    if (index==SETTING_QUALITY) {
        static const uint32_t bitrate[]={1000,700,400};
        vava_put32(op->payload,1); vava_put32(op->payload+4,value);
        vava_put32(op->payload+8,15); vava_put32(op->payload+12,bitrate[value]);
        vava_put32(op->payload+16,id); vava_put32(op->payload+20,slot);
    }
    if (index==SETTING_DETECTION_BOX) {
        /* App 222 sets both flags; the station supplies sensitivity 75.
         * Camera 33 reads/preserves the remaining D.conf fields itself. */
        vava_put32(op->payload+4,75); vava_put32(op->payload+8,value);
        vava_put32(op->payload+12,id); vava_put32(op->payload+16,slot);
    }
    /* A mode operation can reprogram the current level, so its old dedup entry
     * must not suppress a later explicit level request. Keep history intact. */
    if (index==SETTING_PIR_MODE) s->operations[SETTING_PIR_LEVEL].dedup_valid=false;
    s->active=index+1;
}
void settings_begin_pir(struct settings *s, const uint32_t values[5], uint32_t id, unsigned slot,
                        uint64_t generation, uint64_t now, uint64_t tx_end) {
    settings_begin(s,SETTING_PIR_MODE,values[0],values[0],id,slot,generation,now,tx_end);
    struct setting_operation *op=&s->operations[SETTING_PIR_MODE];
    memcpy(op->pir,values,sizeof(op->pir));
    memset(op->payload,0,sizeof(op->payload));
    for (unsigned j=0;j<5;++j) vava_put32(op->payload+4*j,values[j]);
    vava_put32(op->payload+20,id); vava_put32(op->payload+24,slot);
}
void settings_written(struct settings *s, uint64_t completed, uint64_t now) {
    if (!s->active) return;
    struct setting_operation *op=&s->operations[s->active-1];
    if (op->phase==SET_QUEUED && completed>=op->tx_end) {
        op->phase=SET_SENT; op->sent_at=now;
    }
}
void settings_disconnect(struct settings *s) {
    if (!s->active) return;
    s->operations[s->active-1].phase=SET_DISCONNECTED; s->active=0;
}
bool settings_tick(struct settings *s, uint64_t now) {
    if (!s->active) return false;
    struct setting_operation *op=&s->operations[s->active-1];
    if (op->phase==SET_QUEUED && now>=op->queued_at && now-op->queued_at>=5000) {
        op->phase=SET_QUEUE_TIMEOUT; s->active=0; return true;
    }
    if (op->phase==SET_SENT && now>=op->sent_at && now-op->sent_at>=3000) {
        op->phase=SET_TIMEOUT; s->active=0;
    }
    return false;
}
void settings_observe(struct settings *s, uint32_t command, const uint8_t p[116],
                      uint64_t generation, uint64_t now) {
    bool matched_echo=false;
    if (s->active) {
        unsigned index=s->active-1;
        struct setting_operation *op=&s->operations[index];
        if (op->phase==SET_SENT && op->generation==generation && command==commands[index] &&
            now-op->sent_at<3000 && !memcmp(op->payload,p,116)) {
            op->phase=SET_ECHO; op->echo_at=now; s->active=0;
            matched_echo=true;
        }
    }
    int index=-1;
    switch (command) {
        case 5: index=0; break;
        case 37: index=1; break;
        case 42: index=2; break;
        case 203: index=3; break;
        case 30: index=4; break;
        case 17: index=5; break;
        default: return;
    }
    struct setting_report *r=&s->reports[index];
    memset(r,0,sizeof(*r)); r->seen=true; r->at=now; r->generation=generation;
    if (command==5) {
        static const unsigned offsets[]={2,11,12,13,14,18,19,9};
        for (unsigned j=0;j<8;++j) r->values[j]=p[offsets[j]];
    } else if (command==203) {
        for (unsigned j=0;j<10;++j) r->values[j]=p[j];
    } else {
        unsigned count=command==42 ? 5 : 1;
        for (unsigned j=0;j<count;++j) r->values[j]=vava_u32(p+4*j);
    }
    if (command==17) {
        r->matched_echo=matched_echo;
        r->zero_tail=true;
        for (unsigned j=4;j<116;++j) if (p[j]) r->zero_tail=false;
    }
}

struct json { char *out; size_t capacity, used; bool ok; };
static void append(struct json *b, const char *fmt, ...) {
    if (!b->ok) return;
    va_list ap; va_start(ap,fmt);
    int n=vsnprintf(b->out+b->used,b->capacity-b->used,fmt,ap); va_end(ap);
    if (n<0 || (size_t)n>=b->capacity-b->used) { b->ok=false; return; }
    b->used+=(size_t)n;
}
static void age(struct json *b, uint64_t now, uint64_t at) {
    if (at) append(b,"%" PRIu64,now-at); else append(b,"null");
}
bool settings_json(const struct settings *s, uint64_t generation, bool connected,
                   uint64_t now, char *out, size_t capacity) {
    struct json b={out,capacity,0,capacity>0};
    append(&b,"{\"pending\":%s,\"intent_storage\":\"process_memory\",\"operations\":{",s->active ? "true":"false");
    for (unsigned j=0;j<SETTINGS_COUNT;++j) {
        const struct setting_operation *op=&s->operations[j];
        append(&b,"%s\"%s\":",j ? ",":"",names[j]);
        if (op->phase==SET_NONE) { append(&b,"null"); continue; }
        append(&b,"{\"id\":%u,\"value\":%u,\"wire_value\":%u,\"command\":%u,\"phase\":\"%s\","
            "\"generation\":%" PRIu64 ",\"current_connection\":%s,\"queued_age_ms\":%" PRIu64 ",\"sent_age_ms\":",
            op->id,op->value,op->wire_value,commands[j],phases[op->phase],op->generation,
            connected && op->generation==generation ? "true":"false",now-op->queued_at);
        age(&b,now,op->sent_at); append(&b,",\"echo_age_ms\":"); age(&b,now,op->echo_at);
        append(&b,",\"applied\":null,\"persisted\":null");
        if (j==SETTING_PIR_MODE)
            append(&b,",\"parameters\":{\"mode\":%u,\"sensitivity\":%u,\"wait_seconds\":%u,\"record_seconds\":%u,"
                "\"arming_raw\":%u},\"arming_source\":\"%s\"",op->pir[0],op->pir[1],op->pir[2],op->pir[3],op->pir[4],
                op->policy_arming?"local_policy":"preserved_report_42");
        append(&b,"}");
    }
    static const char *const report_names[]={"first_connect","volume_37","pir_config_42","algorithm_203",
                                            "pir_level_30","command_17"};
    static const char *const sources[]={"ram_snapshot_with_overrides","camera_04_conf_report","camera_05_conf_report",
                                       "camera_D_conf_report","mcu_59_report","echo_or_mcu_5a_report"};
    static const char *const fields[6][10]={
        {"pir_control_raw","audio_playback_raw","mirror_raw","ir_raw","osd_raw","antiflicker_wire_raw","pir_trigger_raw","live_quality_raw"},
        {"volume_raw"},
        {"mode_raw","sensitivity_raw","wait_seconds_raw","record_seconds_raw","arming_raw"},
        {"pd_enable_raw","fd_enable_raw","pd_sensitivity_raw","pd_rect_raw","fd_sensitivity_raw","fd_rect_raw",
         "region_x0_raw","region_x1_raw","region_y0_raw","region_y1_raw"},
        {"level_raw"},{"level_raw"}
    };
    static const unsigned counts[]={8,1,5,10,1,1};
    append(&b,"},\"reports\":{");
    for (unsigned j=0;j<6;++j) {
        const struct setting_report *r=&s->reports[j];
        append(&b,"%s\"%s\":",j ? ",":"",report_names[j]);
        if (!r->seen) { append(&b,"null"); continue; }
        append(&b,"{\"source\":\"%s\",\"age_ms\":%" PRIu64 ",\"generation\":%" PRIu64 ",\"current_connection\":%s,\"values\":{",
            sources[j],now-r->at,r->generation,connected && r->generation==generation ? "true":"false");
        for (unsigned k=0;k<counts[j];++k)
            append(&b,"%s\"%s\":%u",k ? ",":"",fields[j][k],r->values[k]);
        append(&b,"}");
        if (j==5) append(&b,",\"zero_tail_hint\":%s,\"matched_request_echo\":%s,\"origin_verified\":false",
                         r->zero_tail ? "true":"false",r->matched_echo ? "true":"false");
        append(&b,"}");
    }
    append(&b,"}}"); return b.ok;
}
