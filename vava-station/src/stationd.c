#include "protocol.h"
#include "registry.h"
#include "rtsp.h"
#include "settings.h"
#include "link_recovery.h"
#include "h264.h"
#include "desired.h"
#include "profile.h"
#include "platform.h"
#include "policy.h"
#include "recorder.h"
#include "record_archive.h"
#include "process_signal.h"
#include "auth.h"
#if VAVA_HTTP
#include "http.h"
#include "api.h"
#endif
#if MYVAVA_SDK
#include "myvava.h"
#include "upload.h"
#include "nas.h"
#include "nas_control.h"
#include "storage_io.h"
#include "sealed_file.h"
#include "media.h"
#include "pairing.h"
#include "removal.h"
#endif
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#if VAVA_HTTP
#include "web.h"
#endif

#define MAX_CLIENTS 32
#define LISTENER_COUNT 6
#define TX_LIMIT (2u * VAVA_MAX_FRAME + 4096u)
#define TOTAL_TX_LIMIT (8u * 1024u * 1024u)
enum kind { CONTROL, MEDIA, ALIVE, ADMIN, RTSP, HTTP, STREAM, DOWNLOAD, AUDIO, TALK, ASSET };
struct client {
    int fd, slot, phase;
    enum kind kind;
    uint64_t generation, born, last_rx, partial_since, tx_since;
    uint32_t peer;
    char mac[18];
    uint8_t input[RTSP_INPUT_LIMIT]; size_t used;
    struct rtsp_session rtsp;
    uint8_t *body; size_t body_used;
    struct vava_media header;
    uint8_t *tx; size_t tx_used, tx_sent;
    uint64_t tx_enqueued, tx_completed;
    bool close_after, keyframe;
    unsigned stream;
    int download_fd;
    uint64_t download_remaining;
    uint64_t storage_job;
    struct record_reader download_reader;
    bool download_ts;
    uint64_t camera_generation;
    bool snapshot;
    uint64_t audio_window;
    unsigned audio_bytes, audio_number;
    const char *const *asset;
    size_t asset_line, asset_offset;
    char cors_origin[160];
};
struct camera {
    int link[3];
    uint32_t peer;
    uint64_t generation, offer_until, heartbeat, video_at, frames, audio_frames, bytes;
    uint64_t heartbeat_generation;
    uint64_t first_at, detection_at, detection_generation, detection_reports;
    uint32_t detection_result, detection_type;
    uint64_t wake_until, next_wake, sleep_close_at, sleep_until;
    unsigned wake_attempts;
    bool control_ready, first_seen, sleep_seen, live_requested, manual_live, manual_wake;
    struct link_recovery recovery;
    uint8_t first[116], heart[116];
    uint32_t last_reply;
    struct settings settings;
    struct {
        unsigned pending, in_flight;
        uint32_t operation;
        uint64_t deadline,quality_lease,quality_generation;
        bool halted;
        const char *delivery;
        uint32_t operations[SETTINGS_COUNT];
        const char *blocked[SETTINGS_COUNT];
    } config;
    struct {
        bool active, manual, first_frame, confirm_required, confirmed, camera_duration;
        unsigned stream, seconds;const char *duration_source;uint64_t first_timestamp;
        uint64_t id, generation, first_deadline, end_at, consumed_pir_generation;
    } recording;
    struct {
        uint32_t revision, operation, mode_operation;
        uint64_t generation, deadline, next_try, report_at, rate_since, busy_at;
        unsigned target, attempts, rate_count;
        bool initialized, pending;
        const char *delivery;
    } arm;
    struct { uint8_t sps[1024],pps[1024]; size_t sps_len,pps_len; } parameters[2];
    struct { bool need_key; uint64_t rejected,skipped,key_requested,at; unsigned width,height; } video[2];
    struct h264_config live_codec;
    char error[64];
};
static struct client clients[MAX_CLIENTS];
static struct camera cameras[4];
static struct registration registry[4];
static struct policy policies[4];
static struct desired preferences[4];
static struct profile profiles[4];
static bool clock_valid;
#if MYVAVA_SDK
static bool diagnostic_defaults,continuous_quality;
#endif
static const char *record_dir;
static const char *storage_mount;
static const char *ap_helper;
static char default_record_dir[1024];
static unsigned record_max_mb=128;
static const char *state_dir="run", *bind_ip="127.0.0.1", *ssid="VAVA-Lab", *password_file;
static const char *pairing_serial;
static const char *archive_serial;
static const char *neighbor_file="/proc/net/arp", *rtsp_bind="127.0.0.1";
static unsigned rtsp_port=8554;
static bool rtsp_enabled;
static const char *rtsp_auth_file;
static char rtsp_authorization[256];
static const char *http_bind="127.0.0.1";
static unsigned http_port=8080;
static bool http_enabled;
#if VAVA_HTTP
static const char *api_token_file;
static bool web_enabled=true;
static const char *http_origins[4];
static unsigned http_origin_count;
#endif
static char api_authorization[256];
#if MYVAVA_SDK
static struct myvava_config server_config={.port=3731,.poll_ms=1000};
static struct myvava_client *server_client;
static struct mvm_client *remote_media;
static struct {struct mvm_lease lease;uint64_t seen_id,generation,last_key,blocked;bool speaking;} remote_view[4];
static struct uploader *uploader;
static struct nas *nas;
static struct nas_config nas_config;
static struct storage_io *storage_worker;
static uint64_t sdk_storage_job;
static const char *record_keyring;
static char executable_path[1024];
static struct myvava_events *event_spool;
static const char *event_dir;
static unsigned event_limit=1024;
#endif
/* A direct SDK command uses the same dispatcher as local maintenance, with a
 * bounded response sink instead of a socket. Only the device loop enters it. */
struct command_output { char *data; size_t used, capacity; bool failed; };
static struct command_output *direct_output;
#define DIRECT_COMMAND (-1)
static char password[32], socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
static unsigned ports[3]={16154,18154,20154};
static int time_offset=0, idle_timeout=90;
static bool lab_ip;
static uint64_t pair_until, sequence, started_at;
static uint32_t setting_sequence;
#if MYVAVA_SDK
static uint64_t removal_generation, removal_tx_end;
#endif
static int listeners[LISTENER_COUNT]={-1,-1,-1,-1,-1,-1}, udp_fd=-1, lock_fd=-1;
static volatile sig_atomic_t stopping;
static unsigned watchers(int s);
static void feed_rtsp(int i, const uint8_t *data, size_t n);
static void rtsp_timers(int i, uint64_t now);
static void rtsp_media(int j, int source, size_t n, bool idr);
static void arm_tick(int s, uint64_t now);
static void config_tick(int s, uint64_t now);
#if MYVAVA_SDK
static void removal_observe(int i,uint32_t command,const uint8_t *payload);
#endif
static void record_end(int s, const char *reason);
static bool record_start(int s, bool pir);
static bool camera_enabled(int s) {
#if MYVAVA_SDK
    if(removal_blocks((unsigned)s,registry[s].serial))return false;
#endif
    return policies[s].enabled;
}
static bool is_viewer(const struct client *c) {
    return c->kind==STREAM || c->kind==AUDIO || c->kind==TALK || (c->kind==RTSP && c->rtsp.demand);
}
static void release_demand(int s) {
    if (s>=0 && !watchers(s)) {
        cameras[s].live_requested=cameras[s].manual_live || (cameras[s].recording.active && cameras[s].recording.manual);
        if (!cameras[s].live_requested && !cameras[s].manual_wake && !cameras[s].arm.pending && !cameras[s].config.pending) cameras[s].wake_until=0;
    }
}

static uint64_t now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC,&ts)) abort();
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
static uint64_t unix_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME,&ts)) abort();
    return (uint64_t)ts.tv_sec*1000+(uint64_t)ts.tv_nsec/1000000;
}
static void signal_stop(int sig) { (void)sig; stopping=1; }
static void set_error(int slot, const char *error) {
    if (slot>=0 && slot<4) snprintf(cameras[slot].error,sizeof(cameras[slot].error),"%s",error);
}
static void clear_video(struct camera *cam) {
    memset(&cam->live_codec,0,sizeof(cam->live_codec));
    memset(cam->parameters,0,sizeof(cam->parameters));
    for(unsigned st=0;st<2;++st){
        cam->video[st].width=cam->video[st].height=0;
        cam->video[st].at=cam->video[st].key_requested=0;
        cam->video[st].need_key=true;
    }
}
static void close_client(int i) {
    if (i==DIRECT_COMMAND) { if (direct_output) direct_output->failed=true; return; }
    struct client *c=&clients[i];
    if (c->fd<0) return;
#if MYVAVA_SDK
    if(c->storage_job)storage_io_abandon(storage_worker,c->storage_job);
#endif
    if (c->slot>=0 && c->kind<=ALIVE) {
        struct camera *cam=&cameras[c->slot];
        if (cam->link[c->kind]==i) {
            cam->link[c->kind]=-1;
            if (c->kind==CONTROL) {
                if (cam->recording.active) record_end(c->slot,"control_disconnected");
                cam->first_seen=false; cam->control_ready=false;
                settings_disconnect(&cam->settings);
                int media=cam->link[MEDIA];
                if (media>=0 && clients[media].camera_generation==cam->generation) close_client(media);
            }
            if (c->kind==ALIVE) cam->sleep_seen=false;
            if (c->kind==MEDIA) {
                for (int j=0;j<MAX_CLIENTS;++j)
                    if (clients[j].fd>=0 && is_viewer(&clients[j]) && clients[j].slot==c->slot) {
                        clients[j].keyframe=false; clients[j].born=now_ms();
                    }
                clear_video(cam);
            }
        }
    }
    int stream_slot=is_viewer(c) ? c->slot : -1;
    if (c->kind==DOWNLOAD) {
        if (c->download_reader.token) recorder_reader_close(&c->download_reader);
        else if (c->download_fd>=0) close(c->download_fd);
    }
    close(c->fd); free(c->body); free(c->tx);
    memset(c,0,sizeof(*c)); c->fd=-1; c->slot=-1;
    release_demand(stream_slot);
}
static bool queue(int i, const void *data, size_t n) {
    if (i==DIRECT_COMMAND) {
        if (!direct_output || direct_output->failed) return false;
        if (n>=direct_output->capacity-direct_output->used) { direct_output->failed=true; return false; }
        memcpy(direct_output->data+direct_output->used,data,n); direct_output->used+=n;
        direct_output->data[direct_output->used]=0; return true;
    }
    struct client *c=&clients[i];
    size_t remaining=c->tx_used-c->tx_sent;
    size_t limit=(c->kind==STREAM || c->kind==RTSP || c->kind==AUDIO) ? TX_LIMIT : 16384;
    size_t others=0;
    for (int j=0;j<MAX_CLIENTS;++j) if (j!=i) others+=clients[j].tx_used;
    size_t total_limit=(c->kind==STREAM || c->kind==RTSP || c->kind==AUDIO) ? TOTAL_TX_LIMIT-65536 : TOTAL_TX_LIMIT;
    if (c->fd<0 || n>limit || remaining>limit-n || others>total_limit-n-remaining) {
        set_error(c->slot,"output_queue_limit"); close_client(i); return false;
    }
    if (c->tx_sent && remaining) memmove(c->tx,c->tx+c->tx_sent,remaining);
    c->tx_sent=0; c->tx_used=remaining;
    uint8_t *p=realloc(c->tx,remaining+n);
    if (!p) { close_client(i); return false; }
    c->tx=p; memcpy(p+remaining,data,n); c->tx_used+=n;
    c->tx_enqueued+=n;
    if (!remaining) c->tx_since=now_ms();
    return true;
}
static bool reply(int i, const char *fmt, ...) {
    char buf[12288]; va_list ap;
    va_start(ap,fmt); int n=vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
    if (n<0 || n>=(int)sizeof(buf)) { close_client(i); return false; }
    return queue(i,buf,(size_t)n);
}
static void fail_admin(int i, const char *reason) {
    if (i!=DIRECT_COMMAND) clients[i].close_after=true;
    (void)reply(i,"{\"ok\":false,\"error\":\"%s\"}\n",reason);
}
static bool send_command(int i, uint32_t command, const uint8_t *payload) {
    uint8_t packet[128]; vava_encode(packet,command,payload);
    return queue(i,packet,sizeof(packet));
}
static bool send_clock(int i, uint32_t command) {
    struct timeval tv; uint8_t payload[116]={0};
    if (gettimeofday(&tv,NULL)) return false;
    vava_put32(payload,(uint32_t)((int64_t)tv.tv_sec+time_offset));
    vava_put32(payload+4,(uint32_t)tv.tv_usec);
    if (command==0) payload[8]=0xee; /* policy byte 9 remains zero */
    return send_command(i,command,payload);
}
static int serial_slot(const char *serial) {
    for (int s=0;s<4;++s) if (registry[s].stage && !strcmp(registry[s].serial,serial)) return s;
    return -1;
}
static bool random_bytes(void *p, size_t n) {
    int fd=open("/dev/urandom",O_RDONLY);
    if (fd<0) return false;
    size_t off=0;
    while (off<n) {
        ssize_t k=read(fd,(uint8_t *)p+off,n-off);
        if (k<0 && errno==EINTR) continue;
        if (k<=0) { close(fd); return false; } off+=(size_t)k;
    }
    close(fd); return true;
}
#if MYVAVA_SDK
static bool recording_complete(const struct record_info *r, bool recovered, void *arg) {
    struct myvava_event e={.kind=recovered?MVE_RECORD_RECOVERED:r->failed?MVE_RECORD_FAILED:MVE_RECORD_COMMITTED,
        .slot=(uint8_t)r->slot,.related_id=r->id,.generation=r->generation,.monotonic_ms=now_ms(),
        .unix_ms=r->unix_seconds*1000,.clock_valid=r->clock_valid,.type_raw=r->stream};
    /* Use the recording identity, never the potentially reused current slot.
     * The event's wall time is the recording start time. */
    strcpy(e.serial,r->serial); snprintf(e.detail,sizeof(e.detail),"%s",recovered?"catalog_recovered":r->error);
    if(!arg)return true;
    struct myvava_event_status status=myvava_events_status(arg);
    if(recovered&&(!status.ready||status.queued>=MYVAVA_EVENT_QUEUE/2||status.retained>=status.limit))return false;
    return myvava_events_emit(arg,&e);
}
static void recording_event_saved(const struct myvava_event *event,void *arg){
    (void)arg;if(event->kind==MVE_RECORD_COMMITTED||event->kind==MVE_RECORD_RECOVERED)recorder_event_durable(event->slot,event->serial,event->related_id,event->generation);
}
static void emit_event(unsigned kind, int slot, uint32_t result, uint32_t type, uint64_t related, const char *detail) {
    struct myvava_event e={.kind=(uint8_t)kind,.slot=slot<0?255:(uint8_t)slot,.result_raw=result,.type_raw=type,
        .related_id=related,.monotonic_ms=now_ms(),.unix_ms=unix_ms(),.clock_valid=clock_valid};
    if (slot>=0) { strcpy(e.serial,registry[slot].serial); e.generation=cameras[slot].generation; }
    snprintf(e.detail,sizeof(e.detail),"%s",detail);
    (void)myvava_events_emit(event_spool,&e);
}
#else
#define emit_event(...) ((void)0)
#endif
static bool persist_stage(int s, uint8_t stage) {
    uint8_t previous=registry[s].stage; registry[s].stage=stage;
    if (registry_save(state_dir,registry)) {
        registry[s].stage=previous; set_error(s,"registry_write_failed"); return false;
    }
    if (stage!=previous) emit_event(MVE_PAIR_STAGE,s,stage,0,0,"registry_saved");
#if MYVAVA_SDK
    if(stage==3){upload_policy(uploader,(unsigned)s,registry[s].serial,policies[s].cloud_upload);nas_policy(nas,(unsigned)s,registry[s].serial,policies[s].nas_backup);}
    if(stage!=previous)(void)pairing_change(stage==3?PAIR_COMPLETE:PAIR_CONFIRMING,s,registry[s].serial,
        stage==3?"camera_confirmed":"formal_registration_durable",now_ms(),stage==3?0:30);
#endif
    return true;
}
static void lookup_mac(uint32_t peer, char out[18]) {
    out[0]=0;
    FILE *f=fopen(neighbor_file,"r");
    if (!f) return;
    char line[512],ip[64],mac[64],mask[64],dev[64]; unsigned type,flags;
    while (fgets(line,sizeof(line),f)) {
        struct in_addr a;
        if (sscanf(line,"%63s %x %x %63s %63s %63s",ip,&type,&flags,mac,mask,dev)==6 &&
            inet_pton(AF_INET,ip,&a)==1 && a.s_addr==peer && (flags&2) && registry_mac(mac)) {
            /* Overlapping interfaces may expose the same IP with different MACs. */
            if (out[0] && strcmp(out,mac)) { out[0]=0; break; }
            memcpy(out,mac,18);
        }
    }
    fclose(f);
}
static bool peer_allowed(int s, const struct client *c) {
    if (registry[s].mac[0]) return c->mac[0] && !strcmp(c->mac,registry[s].mac);
    return lab_ip;
}
static void attach_link(int i, int s) {
    struct client *c=&clients[i]; struct camera *cam=&cameras[s];
    int previous=cam->link[c->kind];
    if (previous>=0 && previous!=i) close_client(previous);
    if (c->kind==CONTROL) {
        /* A new address invalidates companion sockets from the previous lease. */
        if (cam->peer && cam->peer!=c->peer) {
            for (int k=MEDIA;k<=ALIVE;++k) if (cam->link[k]>=0) close_client(cam->link[k]);
        }
        cam->peer=c->peer; ++cam->generation;
        cam->control_ready=false; cam->first_seen=false; cam->sleep_seen=false;
        clear_video(cam);
    }
    c->slot=s; cam->link[c->kind]=i; c->camera_generation=cam->generation;
    /* Stock cameras often open media before their control first report after
     * every wake. Leave it unassigned until that report identifies the session. */
    if (c->kind==MEDIA && !cam->control_ready) c->camera_generation=0;
    if (c->kind==ALIVE) {
        if(c->mac[0])cam->peer=c->peer;
        (void)link_recovery_tick(&cam->recovery,now_ms(),false,false,true,false);
        if(!strcmp(cam->error,"keepalive_missing_recovering"))cam->error[0]=0;
    }
}
static void media_control_ready(int s) {
    struct camera *cam=&cameras[s]; int media=cam->link[MEDIA];
    if (media<0) return;
    if (!clients[media].camera_generation) clients[media].camera_generation=cam->generation;
    else if (clients[media].camera_generation!=cam->generation) close_client(media);
}
static bool companion(int i) {
    struct client *c=&clients[i];
    if (!c->mac[0]) lookup_mac(c->peer,c->mac);
    int found=-1;
    for (int s=0;s<4;++s) {
        if (registry[s].stage<2) continue;
        bool match=registry[s].mac[0] ? peer_allowed(s,c) :
                   lab_ip && cameras[s].peer==c->peer && cameras[s].link[CONTROL]>=0;
        if (match) { if (found>=0) return false; found=s; }
    }
    if (found<0) return false;
    if(c->kind==MEDIA&&link_recovery_blocked(&cameras[found].recovery,now_ms())){close_client(i);return false;}
    attach_link(i,found); return true;
}
static unsigned watchers(int s) {
    unsigned n=0;
#if MYVAVA_SDK
    n+=remote_view[s].lease.id!=0;
#endif
    for (int i=0;i<MAX_CLIENTS;++i) n+=clients[i].fd>=0 && is_viewer(&clients[i]) && clients[i].slot==s;
    return n;
}
static bool live_start(int s) {
    struct camera *cam=&cameras[s];
    int i=cam->link[CONTROL];
    if (i<0 || !cam->control_ready || !camera_enabled(s)) return false;
    uint8_t payload[116]={0}; vava_put32(payload,1);
    if (!send_command(i,102,NULL) || !send_command(i,20,payload)) return false;
    cam->live_requested=true;
    return true;
}
static bool wake_start(int s) {
    struct camera *cam=&cameras[s];
    if (link_recovery_blocked(&cam->recovery,now_ms())) return false;
    if (cam->control_ready && cam->link[CONTROL]>=0) return true;
    if (cam->link[ALIVE]<0 || !cam->peer || cam->sleep_until) return false;
    if (!cam->wake_until) {
        cam->wake_until=now_ms()+10000; cam->next_wake=0; cam->wake_attempts=0;
        cam->error[0]=0;
    }
    return true;
}
static void drop_streams(int s) {
#if MYVAVA_SDK
    remote_view[s].lease.id=0;remote_view[s].speaking=false;
#endif
    for (int i=0;i<MAX_CLIENTS;++i) if (clients[i].fd>=0 && is_viewer(&clients[i]) && clients[i].slot==s)
        close_client(i);
}
#if MYVAVA_SDK
static void remote_media_tick(void) {
    struct mvm_lease leases[4];struct mvm_talk talk;mvm_client_snapshot(remote_media,leases,&talk);uint64_t now=now_ms();
    for(int s=0;s<4;++s){struct camera *cam=&cameras[s];struct mvm_lease *lease=&leases[s];
        if(lease->id&&lease->id==remote_view[s].seen_id&&
            (remote_view[s].generation&&cam->control_ready&&cam->generation!=remote_view[s].generation))remote_view[s].blocked=lease->id;
        if(lease->id&&(lease->id==remote_view[s].blocked||!registry[s].serial[0]||strcmp(lease->serial,registry[s].serial)||!camera_enabled(s)||cam->sleep_until))lease->id=0;
        if(lease->id!=remote_view[s].lease.id){bool before=remote_view[s].lease.id!=0;remote_view[s].lease=*lease;
            if(lease->id&&lease->id!=remote_view[s].seen_id){remote_view[s].seen_id=lease->id;remote_view[s].generation=0;}
            remote_view[s].last_key=0;remote_view[s].speaking=false;
            if(!lease->id&&before)release_demand(s);
        }
        if(!lease->id)continue;
        bool already=cam->live_requested;cam->live_requested=true;
        if(!wake_start(s))continue;
        if(cam->control_ready&&(!already||!remote_view[s].last_key||now-remote_view[s].last_key>=2000)){
            if(!already)(void)live_start(s);else {uint8_t p[116]={0};vava_put32(p,1);(void)send_command(cam->link[CONTROL],20,p);}
            remote_view[s].last_key=now;
        }
        if(!remote_view[s].generation&&cam->control_ready)remote_view[s].generation=cam->generation;
        bool local_talk=false;for(int j=0;j<MAX_CLIENTS;++j)if(clients[j].fd>=0&&clients[j].kind==TALK&&clients[j].slot==s)local_talk=true;
        bool capable=lease->talk&&!local_talk&&cam->control_ready&&cam->link[MEDIA]>=0&&cam->first_seen&&cam->first[1]==3&&vava_u32(cam->first+28)==8000&&cam->generation==remote_view[s].generation;
        if(capable&&!remote_view[s].speaking){uint8_t header[24];vava_talk_header(header,0,0,unix_ms());remote_view[s].speaking=queue(cam->link[MEDIA],header,sizeof(header));}
        if(!capable)remote_view[s].speaking=false;
        if(remote_view[s].speaking&&talk.lease==lease->id&&talk.slot==(unsigned)s&&vava_talk_aac(talk.data,talk.length)){
            uint8_t packet[24+MVM_TALK_MAX];vava_talk_header(packet,talk.length,(uint8_t)talk.sequence,unix_ms());memcpy(packet+24,talk.data,talk.length);(void)queue(cam->link[MEDIA],packet,talk.length+24);
        }
    }
}
#endif

static void record_end(int s, const char *reason) {
    struct camera *cam=&cameras[s];
    if (!cam->recording.active) return;
    emit_event(MVE_RECORD_STOP_REQUEST,s,0,0,cam->recording.id,reason);
    if(!cam->recording.first_frame)recorder_abort((unsigned)s,reason);
    else if(cam->recording.confirm_required&&!cam->recording.confirmed)recorder_abort((unsigned)s,"detection_not_confirmed");
    else recorder_finish((unsigned)s,reason); cam->recording.active=false;
    release_demand(s);
}
static bool record_start(int s, bool pir) {
    struct camera *cam=&cameras[s];
    if (!camera_enabled(s) || cam->recording.active || cam->sleep_until || !recorder_available()) return false;
    /* Manual recording needs the live stream. PIR recording consumes the native
     * already-started stream 0; command 100 is not a verified start operation. */
    if (!pir && !wake_start(s)) return false;
    unsigned stream=pir?0:1; uint64_t id;
    const uint8_t *target=NULL,*nas_destination=NULL;
#if MYVAVA_SDK
    if (pir && policies[s].cloud_upload) target=upload_target(uploader);
    uint8_t nas_hash[32];
    if (pir && policies[s].nas_backup && nas_target(nas,nas_hash)) nas_destination=nas_hash;
#endif
    if (!recorder_start((unsigned)s,stream,cam->generation,registry[s].serial,pir,clock_valid,target,nas_destination,&id)) {release_demand(s);return false;}
    cam->recording.active=true; cam->recording.manual=!pir; cam->recording.first_frame=false;
    cam->recording.confirm_required=pir&&policies[s].confirm_detection;cam->recording.confirmed=false;
    cam->recording.camera_duration=pir&&policies[s].camera_duration;cam->recording.seconds=policies[s].record_seconds;cam->recording.duration_source="station_override";
    cam->recording.id=id; cam->recording.stream=stream; cam->recording.generation=cam->generation;
    cam->recording.first_deadline=now_ms()+20000; cam->recording.end_at=0;
    emit_event(MVE_RECORD_START,s,0,stream,id,pir?"pir":"manual");
    if (!pir) {
        bool already=cam->live_requested; cam->live_requested=true;
        if (cam->control_ready) {
            uint8_t p[116]={0}; vava_put32(p,stream);
            if (!(already ? send_command(cam->link[CONTROL],20,p) : live_start(s))) record_end(s,"control_send_failed");
        }
    } else if (cam->control_ready) {
        uint8_t p[116]={0}; (void)send_command(cam->link[CONTROL],20,p);
    }
    return cam->recording.active;
}

static const char *arm_delivery(int s) {
    return cameras[s].arm.delivery ? cameras[s].arm.delivery : "unmanaged";
}
static bool work_mode_saved(int s) {
    return (preferences[s].mask & (1u<<SETTING_PIR_MODE))!=0;
}
static unsigned effective_pir_level(int s) {
    return work_mode_saved(s) ? preferences[s].values[SETTING_PIR_MODE][1] : policies[s].level;
}
static bool camera_policy_json(int s, char *out, size_t size) {
    struct policy effective=policies[s];
    effective.level=effective_pir_level(s);
    if (!policy_json(&effective,policy_evaluate(&effective,time(NULL),clock_valid),clock_valid,
                     arm_delivery(s),cameras[s].arm.attempts,out,size)) return false;
    size_t used=strlen(out)-1;
    int n=snprintf(out+used,size-used,",\"level_source\":\"%s\",\"fallback_level\":%u,"
        "\"settings_revision\":%u}",work_mode_saved(s)?"work_mode":"legacy_policy",
        policies[s].level,preferences[s].revision);
    return n>=0 && (size_t)n<size-used;
}
static void arm_tick(int s, uint64_t now) {
#if MYVAVA_SDK
    if(removal_blocks((unsigned)s,registry[s].serial))return;
#endif
    struct camera *cam=&cameras[s]; const struct policy *p=&policies[s];
    if (!p->managed || registry[s].stage!=3) {
        cam->arm.delivery="unmanaged"; cam->arm.initialized=false; cam->arm.pending=false; return;
    }
    unsigned target=policy_evaluate(p,time(NULL),clock_valid) ? effective_pir_level(s) : 0;
    cam->arm.revision=p->revision;
    if (!cam->arm.initialized || cam->arm.generation!=cam->generation || cam->arm.target!=target) {
        cam->arm.initialized=true; cam->arm.revision=p->revision; cam->arm.generation=cam->generation;
        cam->arm.target=target; cam->arm.pending=true; cam->arm.operation=0;
        cam->arm.deadline=0; cam->arm.next_try=0; cam->arm.attempts=0;
    }
    const struct setting_operation *mode=&cam->settings.operations[SETTING_PIR_MODE];
    if (mode->id!=cam->arm.mode_operation) {
        cam->arm.mode_operation=mode->id; cam->arm.pending=true;
    }
    const struct setting_report *report=&cam->settings.reports[4];
    if (report->seen && report->generation==cam->generation && report->at!=cam->arm.report_at) {
        cam->arm.report_at=report->at;
        if (report->values[0]<=3 && report->values[0]!=target) cam->arm.pending=true;
    }
    const struct setting_operation *op=&cam->settings.operations[SETTING_PIR_LEVEL];
    if (cam->arm.operation && op->id==cam->arm.operation && !cam->settings.active) {
        cam->arm.operation=0;
        if (op->phase==SET_ECHO) { cam->arm.delivery="echo_observed_effect_unknown"; }
        else { cam->arm.pending=true; cam->arm.delivery="retry_pending"; cam->arm.next_try=now+1000; }
    }
    if (cam->settings.active) { cam->arm.delivery="setting_in_progress"; return; }
    if (!cam->arm.pending) return;
    if (cam->arm.attempts>=3 || (cam->arm.deadline && now>=cam->arm.deadline)) {
        cam->arm.delivery="retry_exhausted"; return;
    }
    if (cam->sleep_until || now<cam->arm.next_try) { cam->arm.delivery="waiting_transition"; return; }
    if (!cam->arm.rate_since || now-cam->arm.rate_since>=60000) {
        cam->arm.rate_since=now; cam->arm.rate_count=0;
    }
    if (cam->arm.rate_count>=6) { cam->arm.delivery="rate_limited"; return; }
    if (!cam->control_ready || cam->link[CONTROL]<0) {
        if (cam->link[ALIVE]<0 || !cam->peer) { cam->arm.delivery="waiting_connection"; return; }
        if (!cam->arm.deadline) cam->arm.deadline=now+30000;
        if (wake_start(s)) cam->arm.delivery="waking";
        return;
    }
    if (!cam->arm.deadline) cam->arm.deadline=now+30000;
    if (setting_sequence==UINT32_MAX) { cam->arm.delivery="operation_id_exhausted"; return; }
    int c=cam->link[CONTROL]; uint32_t id=++setting_sequence;
    settings_begin(&cam->settings,SETTING_PIR_LEVEL,target,target,id,(unsigned)s,cam->generation,now,
                   clients[c].tx_enqueued+128);
    cam->arm.operation=id; cam->arm.pending=false; ++cam->arm.attempts; ++cam->arm.rate_count;
    cam->arm.delivery="queued";
    (void)send_command(c,17,cam->settings.operations[SETTING_PIR_LEVEL].payload);
}

static const char *config_delivery(int s) {
    return cameras[s].config.delivery ? cameras[s].config.delivery : "explicit_apply_required";
}
static bool quality_blocked(int s) {
    return watchers(s) || cameras[s].live_requested || recorder_state((unsigned)s).active ||
        recorder_state((unsigned)s).finishing;
}
static bool quality_write_blocked(int s) {
#if MYVAVA_SDK
    struct camera *cam=&cameras[s];
    if(cam->config.quality_lease&&continuous_quality&&cam->config.quality_lease==remote_view[s].lease.id&&
       remote_view[s].lease.framing==2&&cam->config.quality_generation==cam->generation&&watchers(s)==1)
        return recorder_state((unsigned)s).active||recorder_state((unsigned)s).finishing;
#endif
    return quality_blocked(s);
}
static const char *detection_blocked(int s) {
    const struct camera *cam=&cameras[s];
    if (!cam->control_ready || !cam->first_seen) return "waiting_control";
    if (memcmp(cam->first+48,"VA-HS03",8) || memcmp(cam->first+32,"1.4.0.54",9))
        return "detection_box_firmware_not_verified";
    if (!settings_detection_ready(&cam->settings,cam->generation))
        return "valid_current_algorithm_report_203_required";
    if (quality_blocked(s)) return "detection_box_requires_media_idle";
    return NULL;
}
static const char *config_item_phase(int s, unsigned index) {
    const struct camera *cam=&cameras[s];
    if (cam->config.pending & (1u<<index)) {
        if (cam->config.halted) return config_delivery(s);
        if (!camera_enabled(s)) return "camera_disabled";
        if (index==SETTING_PIR_MODE && !policies[s].managed) return "managed_policy_required";
        if (cam->config.in_flight==index+1) return "new_value_pending";
        if (cam->sleep_until) return "waiting_transition";
        if (!cam->control_ready) return cam->wake_until ? "waking":"waiting_connection";
        if (index==SETTING_QUALITY && quality_write_blocked(s)) return "quality_requires_media_idle";
        if (index==SETTING_DETECTION_BOX && detection_blocked(s)) return detection_blocked(s);
        return cam->settings.active ? "waiting_other_setting":"queued";
    }
    if (cam->config.blocked[index]) return cam->config.blocked[index];
    const struct setting_operation *op=&cam->settings.operations[index];
    bool same=index==SETTING_PIR_MODE ? !memcmp(op->pir,preferences[s].values[index],4*sizeof(uint32_t)) :
        op->value==preferences[s].values[index][0];
    if (!cam->config.operations[index] || op->id!=cam->config.operations[index] || !same)
        return "explicit_apply_required";
    static const char *const phases[]={"explicit_apply_required","queued","sent","echo_observed",
                                      "echo_timeout","disconnected","queue_timeout"};
    return phases[op->phase];
}
static bool config_json(int s, char *out, size_t size) {
    const struct camera *cam=&cameras[s];
    if (!desired_json(&preferences[s],cam->config.pending,config_delivery(s),out,size)) return false;
    size_t used=strlen(out)-1; bool comma=false;
    int n=snprintf(out+used,size-used,",\"items\":{");
    if (n<0 || (size_t)n>=size-used) return false;
    used+=(size_t)n;
    for (unsigned j=0;j<SETTINGS_COUNT;++j) if (preferences[s].mask & (1u<<j)) {
        const struct setting_operation *op=&cam->settings.operations[j];
        bool tracked=cam->config.operations[j] && cam->config.operations[j]==op->id;
        n=snprintf(out+used,size-used,"%s\"%s\":{\"phase\":\"%s\",\"operation_id\":%u,\"current_connection\":%s}",
            comma?",":"",settings_name(j),config_item_phase(s,j),tracked?op->id:0,
            tracked && cam->control_ready && op->generation==cam->generation ? "true":"false");
        if (n<0 || (size_t)n>=size-used) return false;
        used+=(size_t)n; comma=true;
    }
    n=snprintf(out+used,size-used,"}}"); return n>=0 && (size_t)n<size-used;
}
static void config_tick(int s, uint64_t now) {
#if MYVAVA_SDK
    if(removal_blocks((unsigned)s,registry[s].serial))return;
#endif
    struct camera *cam=&cameras[s];
    if (cam->config.in_flight) {
        unsigned index=cam->config.in_flight-1;
        const struct setting_operation *op=&cam->settings.operations[index];
        if (cam->settings.active) return;
        cam->config.in_flight=0;
        if (op->id!=cam->config.operation || op->phase!=SET_ECHO) {
            cam->config.pending|=preferences[s].mask & (1u<<index);
            cam->config.halted=true; cam->config.delivery="explicit_retry_required";
        } else cam->config.delivery="echo_observed_effect_unknown";
    }
    #if MYVAVA_SDK
    if(cam->config.quality_lease&&(cam->config.quality_lease!=remote_view[s].lease.id||cam->config.quality_generation!=cam->generation)){
        cam->config.pending&=~(1u<<SETTING_QUALITY);cam->config.quality_lease=0;cam->config.delivery="viewing_lease_expired";
    }
#endif
    if (!cam->config.pending || cam->config.halted || registry[s].stage!=3) return;
    if ((cam->config.pending & (1u<<SETTING_PIR_MODE)) && !policies[s].managed) {
        /* Resolve this prerequisite before waking an offline camera. Changing
         * policy later must not silently resume a held camera Flash write. */
        cam->config.pending&=~(1u<<SETTING_PIR_MODE);
        cam->config.blocked[SETTING_PIR_MODE]="managed_policy_required";
        cam->config.delivery="managed_policy_required";
        if (!cam->config.pending) return;
    }
    if (!camera_enabled(s)) { cam->config.delivery="camera_disabled"; return; }
    if (cam->config.deadline && now>=cam->config.deadline) {
        cam->config.halted=true; cam->config.delivery="apply_deadline_expired"; return;
    }
    if (cam->settings.active || cam->sleep_until) { cam->config.delivery="waiting_transition"; return; }
    if (!cam->control_ready || cam->link[CONTROL]<0) {
        cam->config.delivery="waiting_connection";
        if (cam->link[ALIVE]>=0 && wake_start(s)) {
            if (!cam->config.deadline) cam->config.deadline=now+(cam->config.quality_lease?90000:30000);
            cam->config.delivery="waking";
        }
        return;
    }
    if (!cam->config.deadline) cam->config.deadline=now+(cam->config.quality_lease?90000:30000);
    unsigned index=0;
    for (;index<SETTINGS_COUNT;++index) {
        if (!(cam->config.pending & (1u<<index))) continue;
        if (index==SETTING_QUALITY && quality_write_blocked(s)) {
            cam->config.delivery="quality_requires_media_idle"; continue;
        }
        if (index==SETTING_DETECTION_BOX && detection_blocked(s)) {
            cam->config.delivery=detection_blocked(s); continue;
        }
        break;
    }
    if (index==SETTINGS_COUNT) return;
    if (setting_sequence==UINT32_MAX) { cam->config.halted=true; cam->config.delivery="operation_id_exhausted"; return; }
    int c=cam->link[CONTROL]; uint32_t id=++setting_sequence,wire=0;
    const uint32_t *v=preferences[s].values[index];
    if (index==SETTING_PIR_MODE) {
        uint32_t p[5]; memcpy(p,v,sizeof(p)); p[4]=policy_evaluate(&policies[s],time(NULL),clock_valid);
        settings_begin_pir(&cam->settings,p,id,(unsigned)s,cam->generation,now,clients[c].tx_enqueued+128);
        cam->settings.operations[index].policy_arming=true;
    } else {
        (void)settings_index(settings_name(index),v[0],&wire);
        settings_begin(&cam->settings,index,v[0],wire,id,(unsigned)s,cam->generation,now,clients[c].tx_enqueued+128);
    }
    cam->config.pending&=~(1u<<index); cam->config.in_flight=index+1; cam->config.operation=id;
    cam->config.operations[index]=id;
    cam->config.delivery="queued";
    if (index==SETTING_QUALITY && !cam->config.quality_lease) clear_video(cam);
    (void)send_command(c,settings_command(index),cam->settings.operations[index].payload);
}

static bool policy_admin(int i, int s, unsigned count, char **w) {
    const char *cmd=w[0]; struct camera *cam=&cameras[s];
    struct policy next=policies[s]; bool mutation=true;
    if (!strcmp(cmd,"policy") && count==2) mutation=false;
    else if (!strcmp(cmd,"policy-config") && count>=3) {
        unsigned seen=0;
        for (unsigned k=2;k<count;++k) {
            char *eq=strchr(w[k],'=');
            if (!eq || eq==w[k] || !eq[1]) { fail_admin(i,"invalid_policy_arguments"); return true; }
            *eq++=0; unsigned bit=0;
            if (!strcmp(w[k],"mode")) {
                bit=1; next.managed=strcmp(eq,"diagnostic")!=0;
                if (!strcmp(eq,"off") || !next.managed) next.mode=ARM_OFF;
                else if (!strcmp(eq,"on")) next.mode=ARM_ON;
                else if (!strcmp(eq,"schedule")) next.mode=ARM_SCHEDULE;
                else { fail_admin(i,"invalid_arm_mode"); return true; }
            } else {
                char *end; errno=0; long v=strtol(eq,&end,10);
                if (errno || end==eq || *end || v< -720 || v>3600) { fail_admin(i,"invalid_policy_value"); return true; }
                if (!strcmp(w[k],"level")) {
                    if (work_mode_saved(s)) { fail_admin(i,"pir_level_owned_by_work_mode"); return true; }
                    bit=2; next.level=(unsigned)v;
                }
                else if (!strcmp(w[k],"enabled") && (v==0 || v==1)) { bit=4; next.enabled=v!=0; }
                #if MYVAVA_SDK
                else if (!strcmp(w[k],"cloud") && (v==0 || v==1)) {
                    if (v && !uploader) { fail_admin(i,"remote_storage_not_configured"); return true; }
                    bit=64; next.cloud_upload=v!=0;
                }
                else if (!strcmp(w[k],"nas") && (v==0 || v==1)) {
                    if (v && !nas) { fail_admin(i,"nas_not_configured"); return true; }
                    bit=128; next.nas_backup=v!=0;
                }
#endif
                else if (!strcmp(w[k],"camera-duration")&&(v==0||v==1)) {bit=256;next.camera_duration=v!=0;}
                else if (!strcmp(w[k],"confirm-detection")&&(v==0||v==1)) {bit=512;next.confirm_detection=v!=0;}
                else if (!strcmp(w[k],"record")) { bit=8; next.record_seconds=(unsigned)v; }
                else if (!strcmp(w[k],"idle")) { bit=16; next.idle_seconds=(unsigned)v; }
                else if (!strcmp(w[k],"timezone")) { bit=32; next.utc_offset_minutes=(int)v; }
                else { fail_admin(i,"invalid_policy_field"); return true; }
            }
            if (seen&bit) { fail_admin(i,"duplicate_policy_field"); return true; }
            seen|=bit;
        }
        if ((seen&4) && (seen&1) && !next.managed) {
            fail_admin(i,"enable_requires_managed_policy"); return true;
        }
        if (seen&4) next.managed=true;
        mutation=memcmp(&next,&policies[s],sizeof(next))!=0;
    } else if (!strcmp(cmd,"arm") && (count==3 || count==4)) {
        if (!strcmp(w[2],"retry") && count==3) {
            cam->arm.initialized=false; arm_tick(s,now_ms()); mutation=false;
        } else {
            next.managed=strcmp(w[2],"diagnostic")!=0;
            if (!strcmp(w[2],"off") || !next.managed) next.mode=ARM_OFF;
            else if (!strcmp(w[2],"on")) next.mode=ARM_ON;
            else if (!strcmp(w[2],"schedule")) next.mode=ARM_SCHEDULE;
            else { fail_admin(i,"invalid_arm_mode"); return true; }
            if (count==4) {
                if (work_mode_saved(s)) { fail_admin(i,"pir_level_owned_by_work_mode"); return true; }
                unsigned level;
                if (strlen(w[3])!=1 || w[3][0]<'1' || w[3][0]>'3') { fail_admin(i,"invalid_level"); return true; }
                level=(unsigned)(w[3][0]-'0'); next.level=level;
            }
        }
    } else if (!strcmp(cmd,"enable") && count==3 && (!strcmp(w[2],"0") || !strcmp(w[2],"1"))) {
        next.enabled=w[2][0]=='1'; next.managed=true;
    } else if (!strcmp(cmd,"plan") && count>=3) {
        unsigned idx;
        if (!strcmp(w[2],"clear") && count==3) memset(next.plans,0,sizeof(next.plans));
        else if (count>=4 && strlen(w[3])==1 && w[3][0]>='0' && w[3][0]<='9') {
            idx=(unsigned)(w[3][0]-'0'); struct plan q={0};
            if (!strcmp(w[2],"delete") && count==4) next.plans[idx]=q;
            else if (!strcmp(w[2],"enable") && count==5 && (!strcmp(w[4],"0") || !strcmp(w[4],"1"))) {
                if (!next.plans[idx].days) { fail_admin(i,"plan_not_found"); return true; }
                next.plans[idx].enabled=w[4][0]=='1';
            }
            else if (!strcmp(w[2],"all-day") && count==5 && policy_days(w[4],&q.days)) {
                q.enabled=next.plans[idx].days?next.plans[idx].enabled:true;
                q.all_day=true; next.plans[idx]=q;
            } else if (!strcmp(w[2],"set") && count==7 && policy_days(w[4],&q.days) &&
                       policy_time(w[5],&q.start) && policy_time(w[6],&q.end) && q.start!=q.end) {
                q.enabled=next.plans[idx].days?next.plans[idx].enabled:true; next.plans[idx]=q;
            } else { fail_admin(i,"invalid_plan"); return true; }
        } else { fail_admin(i,"invalid_plan"); return true; }
    } else if ((!strcmp(cmd,"timezone") || !strcmp(cmd,"record-duration") || !strcmp(cmd,"idle-sleep")) && count==3) {
        char *end; errno=0; long v=strtol(w[2],&end,10);
        if (errno || end==w[2] || *end || v< -720 || v>3600) { fail_admin(i,"invalid_policy_value"); return true; }
        if (!strcmp(cmd,"timezone")) next.utc_offset_minutes=(int)v;
        else if (!strcmp(cmd,"record-duration")) next.record_seconds=(unsigned)v;
        else next.idle_seconds=(unsigned)v;
    } else return false;
    mutation=mutation && memcmp(&next,&policies[s],sizeof(next))!=0;
    if (mutation) {
        if (!policy_valid(&next)) { fail_admin(i,"invalid_policy_value"); return true; }
        if (next.revision==UINT32_MAX) { fail_admin(i,"policy_revision_exhausted"); return true; }
        ++next.revision;
        struct policy saved[4]; memcpy(saved,policies,sizeof(saved)); saved[s]=next;
        if (policy_save(state_dir,registry,saved)) { fail_admin(i,"policy_write_failed"); return true; }
        policies[s]=next;
#if MYVAVA_SDK
        upload_policy(uploader,(unsigned)s,registry[s].serial,next.cloud_upload);
        nas_policy(nas,(unsigned)s,registry[s].serial,next.nas_backup);
#endif
        if (!next.enabled) { record_end(s,"camera_disabled"); cam->manual_live=false; cam->live_requested=false; drop_streams(s); }
        arm_tick(s,now_ms());
    }
    char json[2048];
    if (!camera_policy_json(s,json,sizeof(json))) fail_admin(i,"policy_response_limit");
    else (void)reply(i,"{\"ok\":true,\"policy_saved\":%s,\"policy\":%s}\n",mutation?"true":"false",json);
    return true;
}

static void control_packet(int i, const uint8_t packet[128]) {
    struct client *c=&clients[i]; const uint8_t *p=packet+12;
    uint32_t cmd; char serial[32]; uint8_t payload[116]={0};
    if (!vava_decode(packet,&cmd)) { set_error(c->slot,"invalid_control_crc_or_magic"); close_client(i); return; }
    if (!c->mac[0]) lookup_mac(c->peer,c->mac);
    if (cmd==400) {
        if ((c->slot>=0 && c->phase!=400 && c->phase!=402) || now_ms()>=pair_until || !password[0] ||
            !vava_guest_valid(p) || !vava_serial(p+40,serial) || (!c->mac[0] && !lab_ip)) {
            close_client(i); return;
        }
#if MYVAVA_SDK
        if(!pairing_accepts(serial)){close_client(i);return;}
#endif
        int s=serial_slot(serial);
        if (c->slot>=0 && c->slot!=s) { close_client(i); return; }
        if (s>=0 && (!peer_allowed(s,c) || registry[s].stage==3)) { close_client(i); return; }
        if (s<0) {
            for (s=0;s<4 && registry[s].stage;++s) {}
            if (s==4) { close_client(i); return; }
            for (int k=0;k<4;++k) if (registry[k].stage && c->mac[0] && !strcmp(registry[k].mac,c->mac)) {
                close_client(i); return;
            }
            registry[s].stage=1; memcpy(registry[s].serial,serial,32); memcpy(registry[s].mac,c->mac,18);
        }
        cameras[s].offer_until=pair_until; c->slot=s; c->phase=400;
#if MYVAVA_SDK
        cameras[s].offer_until=now_ms()+30000;
        if(!pairing_change(PAIR_CREDENTIALS,s,serial,"network_credentials_offered",now_ms(),30)){
            pair_until=0;platform_pair_close(now_ms());close_client(i);return;
        }
#endif
        vava_put32(payload+4,(uint32_t)s);
        memcpy(payload+8,ssid,strlen(ssid)); memcpy(payload+40,password,strlen(password));
        (void)send_command(i,401,payload); return;
    }
    if (cmd==402) {
        if (c->phase!=400) { close_client(i); return; }
#if MYVAVA_SDK
        cameras[c->slot].offer_until=now_ms()+30000;pair_until=0;platform_pair_close(now_ms());
        (void)pairing_change(PAIR_FORMAL,c->slot,registry[c->slot].serial,"temporary_network_acknowledged",now_ms(),30);
#endif
        c->phase=402; return;
    }
    if (cmd==1) {
        uint32_t slot=vava_u32(p);
        if (slot>=4 || !vava_serial(p+4,serial) || strcmp(serial,registry[slot].serial) ||
            !peer_allowed((int)slot,c) || (registry[slot].stage==1 && now_ms()>=cameras[slot].offer_until) ||
            registry[slot].stage==0 || (c->slot>=0 && c->slot!=(int)slot)) { close_client(i); return; }
        /* Save intent before sending success: the camera persists before ACK 3. */
        if (registry[slot].stage<2 && !persist_stage((int)slot,2)) { close_client(i); return; }
        attach_link(i,(int)slot); c->phase=2;
        uint32_t number;
        if (!random_bytes(&number,sizeof(number))) { close_client(i); return; }
        vava_put32(payload+4,1000+number%9000);
        (void)send_command(i,2,payload); return;
    }
    if (cmd==3) {
        if (c->slot<0 || (c->phase!=2 && c->phase!=3) ||
            (registry[c->slot].stage!=3 && !persist_stage(c->slot,3))) { close_client(i); return; }
        c->phase=3; return;
    }
    if (cmd==5) {
        if (!vava_serial(p+64,serial)) { close_client(i); return; }
        int s=serial_slot(serial);
        if (s<0 || registry[s].stage<2 || !peer_allowed(s,c) ||
            (c->slot>=0 && c->slot!=s) || (c->phase==400 || c->phase==402)) { close_client(i); return; }
        if(link_recovery_blocked(&cameras[s].recovery,now_ms())){close_client(i);return;}
        if (registry[s].stage==2 && !persist_stage(s,3)) { close_client(i); return; }
        if (cameras[s].link[CONTROL]!=i) attach_link(i,s);
        struct camera *cam=&cameras[s];
        memcpy(cam->first,p,116); cam->control_ready=true; cam->first_seen=true; cam->sleep_seen=false;
        cam->wake_until=0; cam->manual_wake=false; cam->sleep_close_at=0; cam->sleep_until=0;
        cam->first_at=now_ms(); c->phase=5;
        media_control_ready(s);
        settings_observe(&cam->settings,cmd,p,cam->generation,cam->first_at);
        if (!send_clock(i,6)) { close_client(i); return; }
        if (cam->live_requested || watchers(s)) (void)live_start(s);
        if (cam->recording.active && cam->recording.manual) cam->recording.generation=cam->generation;
        if (p[19] && cam->recording.consumed_pir_generation!=cam->generation) {
            cam->recording.consumed_pir_generation=cam->generation;
            emit_event(MVE_PIR_WAKE,s,p[19],5,0,"first_connect_flag");
            if (policy_evaluate(&policies[s],time(NULL),clock_valid)) (void)record_start(s,true);
        }
        return;
    }
    bool resumed=false;
    if (cmd==0 && c->phase!=5 && c->phase!=4) {
        /* A surviving camera sender thread can reconnect with heartbeat only.
         * Production identity comes from the durable MAC, never a guessed SN/IP. */
        int found=-1;
        if (c->phase!=0 && c->phase!=3) { close_client(i); return; }
        for (int s=0;s<4;++s) {
            bool match=registry[s].mac[0] ? peer_allowed(s,c) :
                lab_ip && cameras[s].peer && cameras[s].peer==c->peer;
            if (registry[s].stage>=2 && match) {
                if (found>=0) { close_client(i); return; }
                found=s;
            }
        }
        if (found<0 || registry[found].stage!=3 || (c->slot>=0 && c->slot!=found)) { close_client(i); return; }
        if(link_recovery_blocked(&cameras[found].recovery,now_ms())){close_client(i);return;}
        if (cameras[found].link[CONTROL]!=i) attach_link(i,found);
        struct camera *cam=&cameras[found];
        cam->control_ready=true; cam->sleep_seen=false;
        cam->wake_until=0; cam->manual_wake=false; cam->sleep_close_at=0; cam->sleep_until=0;
        c->phase=4; resumed=true;
        media_control_ready(found);
    }
    if (c->slot<0 || !cameras[c->slot].control_ready || cameras[c->slot].link[CONTROL]!=i) { close_client(i); return; }
    struct camera *cam=&cameras[c->slot];
#if MYVAVA_SDK
    removal_observe(i,cmd,p);
#endif
    settings_observe(&cam->settings,cmd,p,cam->generation,now_ms());
    if (cmd==0) {
        memcpy(cam->heart,p,116); cam->heartbeat=now_ms(); cam->heartbeat_generation=cam->generation;
        if (!send_clock(i,0)) { close_client(i); return; }
        if (resumed && (cam->live_requested || watchers(c->slot))) (void)live_start(c->slot);
        if (resumed && cam->recording.active && cam->recording.manual) cam->recording.generation=cam->generation;
    } else if (cmd==200) {
        /* Camera notification, not a control acknowledgement or recording state.
         * Stock sender: 0x40021688; station consumer: 0x0044ee84.
         * Count received reports, since the wire format has no event identity. */
        cam->detection_result=vava_u32(p); cam->detection_type=vava_u32(p+4);
        cam->detection_at=now_ms(); cam->detection_generation=cam->generation;
        if (cam->detection_reports<UINT64_MAX) ++cam->detection_reports;
        bool related=cam->recording.active&&!cam->recording.manual&&cam->recording.generation==cam->generation;
        if(related&&cam->detection_result==1)cam->recording.confirmed=true;
        if(related)recorder_detection((unsigned)c->slot,cam->recording.id,cam->generation,cam->detection_result,cam->detection_type);
        emit_event(MVE_DETECTION,c->slot,cam->detection_result,cam->detection_type,related?cam->recording.id:0,"camera_notification");
    } else {
        cam->last_reply=cmd; /* An echo is observed, never declared successful. */
    }
}

#if MYVAVA_SDK
static void removal_tick(uint64_t now) {
    if(!removal_active())return;
    unsigned s=removal_slot();struct camera *cam=&cameras[s];
    enum removal_phase phase=removal_phase();
    if(phase==REMOVE_ACKNOWLEDGED){
        if(removal_defaults_only()){
            if(registry[s].stage!=3||strcmp(registry[s].serial,removal_serial())){(void)removal_change(REMOVE_UNKNOWN,"camera_identity_changed",now);return;}
            /* The target revision is journalled before sending 22, so a restart
             * retries only this idempotent local commit, never a Flash command. */
            struct desired next[4];memcpy(next,preferences,sizeof(next));
            memset(&next[s],0,sizeof(next[s]));next[s].revision=removal_settings_revision();
            if(desired_save(state_dir,registry,next))return;
            memcpy(preferences,next,sizeof(next));
            memset(&cam->settings,0,sizeof(cam->settings));memset(&cam->config,0,sizeof(cam->config));
            memset(&cam->arm,0,sizeof(cam->arm));
            (void)removal_change(REMOVE_COMPLETE,"camera_defaults_echo_observed",now);return;
        }
        /* Commit registration first. Old settings are keyed by SN+MAC and
         * cannot apply to a new occupant if a later cleanup write fails. */
        if(registry[s].stage){
            if(strcmp(registry[s].serial,removal_serial())){(void)removal_change(REMOVE_UNKNOWN,"camera_identity_changed",now);return;}
            struct registration next[4];memcpy(next,registry,sizeof(next));memset(&next[s],0,sizeof(next[s]));
            if(registry_save(state_dir,next))return; /* retry local commit only */
            upload_policy(uploader,s,registry[s].serial,false);nas_policy(nas,s,registry[s].serial,false);
            memcpy(registry,next,sizeof(next));
            for(int j=0;j<MAX_CLIENTS;++j)if(clients[j].fd>=0&&clients[j].slot==(int)s&&clients[j].kind!=DOWNLOAD)close_client(j);
            memset(cam,0,sizeof(*cam));for(unsigned k=0;k<3;++k)cam->link[k]=-1;
            memset(&profiles[s],0,sizeof(profiles[s]));memset(&preferences[s],0,sizeof(preferences[s]));policy_default(&policies[s]);
            if(!diagnostic_defaults)policy_daily_default(&policies[s]);
            upload_policy(uploader,s,"",false);nas_policy(nas,s,"",false);
        }
        if(profile_save(state_dir,registry,profiles)||desired_save(state_dir,registry,preferences)||policy_save(state_dir,registry,policies))return;
        (void)removal_change(REMOVE_COMPLETE,removal_operator_confirmed()?"operator_confirmed_factory_reset":"reset_and_unpair_echoes_observed",now);return;
    }
    if(!registry[s].stage||strcmp(registry[s].serial,removal_serial())){(void)removal_change(REMOVE_UNKNOWN,"camera_identity_changed",now);return;}
    if(now>=removal_deadline()){
        (void)removal_change(phase==REMOVE_WAITING?REMOVE_FAILED:REMOVE_UNKNOWN,phase==REMOVE_WAITING?"camera_wake_timeout":"camera_reset_echo_timeout",now);return;
    }
    if(phase==REMOVE_WAITING){
        if(!cam->control_ready||cam->link[CONTROL]<0||cam->sleep_until){(void)wake_start((int)s);return;}
        if(recorder_state(s).active||recorder_state(s).finishing||cam->settings.active)return;
        /* Restrict destructive protocol use to the firmware actually analysed. */
        if(!cam->first_seen||memcmp(cam->first+48,"VA-HS03",8)||memcmp(cam->first+32,"1.4.0.54",9)){
            (void)removal_change(REMOVE_FAILED,"factory_reset_firmware_not_verified",now);return;
        }
        if(!removal_change(REMOVE_DEFAULTS_SENT,"factory_defaults_request",now))return;
        int c=cam->link[CONTROL];removal_generation=cam->generation;removal_tx_end=clients[c].tx_enqueued+128;
        uint8_t p[116]={0};vava_put32(p,removal_token());vava_put32(p+4,s);
        if(!send_command(c,22,p))(void)removal_change(REMOVE_UNKNOWN,"factory_defaults_send_unconfirmed",now);
    }else if(!cam->control_ready||cam->generation!=removal_generation){
        (void)removal_change(REMOVE_UNKNOWN,"camera_reset_disconnected_unconfirmed",now);
    }
}
static void removal_observe(int i,uint32_t command,const uint8_t *payload){
    if(!removal_active()||clients[i].slot!=(int)removal_slot())return;
    struct camera *cam=&cameras[removal_slot()];enum removal_phase phase=removal_phase();
    if(cam->generation!=removal_generation||clients[i].tx_completed<removal_tx_end||
       vava_u32(payload)!=removal_token()||vava_u32(payload+4)!=removal_slot())return;
    if(phase==REMOVE_DEFAULTS_SENT&&command==22){
        if(removal_defaults_only()){
            (void)removal_change(REMOVE_ACKNOWLEDGED,"camera_defaults_echo_observed",now_ms());return;
        }
        if(!removal_change(REMOVE_UNPAIR_SENT,"clear_pairing_request",now_ms()))return;
        /* Camera 0x40024850: persist matchFlag=0 in 02.conf, MCU 0x23/0x72.
         * Avoid the UINT32_MAX sentinel, which enters AP comparison instead. */
        uint8_t p[116]={0};vava_put32(p,removal_token());vava_put32(p+4,removal_slot());
        removal_tx_end=clients[i].tx_enqueued+128;
        if(!send_command(i,4,p))(void)removal_change(REMOVE_UNKNOWN,"clear_pairing_send_unconfirmed",now_ms());
    }else if(phase==REMOVE_UNPAIR_SENT&&command==4){
        (void)removal_change(REMOVE_ACKNOWLEDGED,"camera_reset_echoes_observed",now_ms());
    }
}
#endif

static size_t start_code(const uint8_t *p, size_t n, size_t at, size_t *prefix) {
    for (size_t j=at;j+3<=n;++j) {
        if (!p[j] && !p[j+1]) {
            if (p[j+2]==1) { *prefix=3; return j; }
            if (j+4<=n && !p[j+2] && p[j+3]==1) { *prefix=4; return j; }
        }
    }
    *prefix=0; return n;
}
static bool inspect_h264(int s, unsigned stream, const uint8_t *p, size_t n) {
    size_t prefix=0,at=start_code(p,n,0,&prefix); bool idr=false;
    while (at<n && at+prefix<n) {
        size_t next_prefix, next=start_code(p,n,at+prefix,&next_prefix);
        unsigned type=p[at+prefix]&31; size_t len=next-at;
        if (type==5) idr=true;
        if (type==7 && len<=1024) {
            memcpy(cameras[s].parameters[stream].sps,p+at,len); cameras[s].parameters[stream].sps_len=len;
            unsigned width,height;
            if(h264_dimensions(p+at,len,&width,&height)){
                cameras[s].video[stream].width=width;cameras[s].video[stream].height=height;
            }
        }
        if (type==8 && len<=1024) {
            memcpy(cameras[s].parameters[stream].pps,p+at,len); cameras[s].parameters[stream].pps_len=len;
        }
        at=next; prefix=next_prefix;
    }
    return idr;
}
static void media_frame(int i) {
    struct client *c=&clients[i]; size_t n;
    if (!vava_media_body(&c->header,c->body,&n)) {
        set_error(c->slot,"invalid_media_tail"); close_client(i); return;
    }
    struct camera *cam=&cameras[c->slot]; cam->bytes+=n;
    if(cam->recording.active&&cam->recording.first_frame&&cam->recording.camera_duration&&
       (c->header.stream==cam->recording.stream||c->header.stream==cam->recording.stream+8)&&
       c->camera_generation==cam->recording.generation&&c->header.timestamp>=cam->recording.first_timestamp&&
       c->header.timestamp-cam->recording.first_timestamp>=(uint64_t)cam->recording.seconds*1000+700)record_end(c->slot,"media_duration_reached");
    if (c->header.stream>1) {
        ++cam->audio_frames;
#if MYVAVA_SDK
        if(c->header.stream==9&&c->header.codec==3&&c->camera_generation==cam->generation)
            mvm_client_frame_epoch(remote_media,(unsigned)c->slot,remote_view[c->slot].lease.id,cam->generation,cam->live_codec.epoch?cam->live_codec.epoch:1,true,false,c->header.timestamp,c->body,n,NULL,0,NULL,0);
        /* Firmware avclient jump table 0x6dbd90 routes 0/8 to the record
         * cache and 1/9 to live; preserve that association. */
        if (cam->recording.active && cam->recording.first_frame && c->header.stream==cam->recording.stream+8 &&
            cam->recording.generation==cam->generation && c->camera_generation==cam->generation) {
            if (c->header.codec!=3 || !recorder_frame((unsigned)c->slot,true,false,c->header.timestamp,
                    c->body,n,NULL,0,NULL,0)) {
                recorder_abort((unsigned)c->slot,"audio_record_failed"); record_end(c->slot,"audio_record_failed");
            }
        }
#endif
        for (int j=0;j<MAX_CLIENTS;++j) {
            struct client *sink=&clients[j];
            if (sink->fd<0 || sink->kind!=AUDIO || sink->slot!=c->slot || sink->stream!=c->header.stream) continue;
            if (queue(j,c->input,24) && queue(j,c->body,n)) { sink->keyframe=true; sink->born=now_ms(); }
        }
        return;
    }
    ++cam->frames; cam->video_at=now_ms();
    unsigned vst=c->header.stream;
    if(!h264_valid(c->body,n)) {
        cam->video[vst].need_key=true;++cam->video[vst].rejected;
        set_error(c->slot,"corrupt_h264_dropped");
        if(cam->control_ready&&now_ms()-cam->video[vst].key_requested>=1000){
            uint8_t p[116]={0};vava_put32(p,1);(void)send_command(cam->link[CONTROL],20,p);
            cam->video[vst].key_requested=now_ms();
        }
        return;
    }
    bool idr=inspect_h264(c->slot,vst,c->body,n);
    if(cam->video[vst].need_key&&!idr){++cam->video[vst].skipped;return;}
    if(idr){
        cam->video[vst].need_key=false;
        if(!strcmp(cam->error,"corrupt_h264_dropped"))cam->error[0]=0;
    }
    cam->video[vst].at=now_ms();
#if MYVAVA_SDK
    bool remote_valid=true;
    if(c->header.stream==1&&remote_view[c->slot].lease.framing==2){
        bool key,changed;struct h264_config *cfg=&cam->live_codec;
        if(cam->parameters[1].sps_len)(void)h264_config_frame(cfg,cam->parameters[1].sps,cam->parameters[1].sps_len,&key,&changed);
        if(cam->parameters[1].pps_len)(void)h264_config_frame(cfg,cam->parameters[1].pps,cam->parameters[1].pps_len,&key,&changed);
        remote_valid=h264_config_frame(cfg,c->body,n,&key,&changed)==1&&key==idr;
    }
    if(c->header.stream==1&&c->camera_generation==cam->generation&&remote_valid){
        bool framed=remote_view[c->slot].lease.framing==2;
        const uint8_t *sps=framed?cam->live_codec.active_sps:cam->parameters[1].sps;
        const uint8_t *pps=framed?cam->live_codec.active_pps:cam->parameters[1].pps;
        size_t ns=framed?cam->live_codec.sps_size:cam->parameters[1].sps_len,np=framed?cam->live_codec.pps_size:cam->parameters[1].pps_len;
        mvm_client_frame_epoch(remote_media,(unsigned)c->slot,remote_view[c->slot].lease.id,cam->generation,cam->live_codec.epoch?cam->live_codec.epoch:1,false,idr,c->header.timestamp,c->body,n,
            idr?sps:NULL,idr?ns:0,idr?pps:NULL,idr?np:0);
    }
#endif
    if (cam->recording.active && c->header.stream==cam->recording.stream &&
        cam->recording.generation==cam->generation && c->camera_generation==cam->generation) {
        unsigned st=c->header.stream;
        bool first=!cam->recording.first_frame;
        if (!first || (idr && c->header.type==1 && cam->parameters[st].sps_len && cam->parameters[st].pps_len)) {
            bool ok=true;
#if MYVAVA_SDK
            ok=recorder_frame((unsigned)c->slot,false,idr,c->header.timestamp,c->body,n,
                first?cam->parameters[st].sps:NULL,first?cam->parameters[st].sps_len:0,
                first?cam->parameters[st].pps:NULL,first?cam->parameters[st].pps_len:0);
#endif
            if (first) {
#if !MYVAVA_SDK
                ok=recorder_data((unsigned)c->slot,cam->parameters[st].sps,cam->parameters[st].sps_len,false) &&
                   recorder_data((unsigned)c->slot,cam->parameters[st].pps,cam->parameters[st].pps_len,false);
#endif
                cam->recording.first_frame=true;
                cam->recording.first_timestamp=c->header.timestamp;
                if(cam->recording.camera_duration)cam->recording.seconds=settings_record_seconds(&cam->settings,cam->generation,&cam->recording.duration_source);
                cam->recording.end_at=now_ms()+((uint64_t)cam->recording.seconds+(cam->recording.camera_duration?10:0))*1000;
            }
#if !MYVAVA_SDK
            if (ok) ok=recorder_data((unsigned)c->slot,c->body,n,true);
#endif
            if (!ok) { recorder_abort((unsigned)c->slot,"record_queue_failed"); record_end(c->slot,"record_queue_failed"); }
        }
    }
    for (int j=0;j<MAX_CLIENTS;++j) {
        struct client *sink=&clients[j];
        if (sink->fd<0 || !is_viewer(sink) || sink->slot!=c->slot || sink->stream!=c->header.stream) continue;
        if (sink->kind==TALK || sink->kind==AUDIO || sink->close_after) continue;
        if (sink->snapshot && (c->camera_generation!=cam->generation || !cam->parameters[c->header.stream].sps_len ||
                              !cam->parameters[c->header.stream].pps_len)) continue;
        if (sink->kind==RTSP) { rtsp_media(j,i,n,idr); continue; }
        if (!sink->keyframe) {
            if (c->header.type!=1 || !idr) continue; /* Wire 0=P, 1=I. */
            unsigned st=c->header.stream;
            if (cam->parameters[st].sps_len && !queue(j,cam->parameters[st].sps,cam->parameters[st].sps_len)) continue;
            if (cam->parameters[st].pps_len && !queue(j,cam->parameters[st].pps,cam->parameters[st].pps_len)) continue;
            sink->keyframe=true;
        }
        if (queue(j,c->body,n) && sink->snapshot) sink->close_after=true;
    }
}
static void feed_media(int i, const uint8_t *data, size_t n) {
    struct client *c=&clients[i];
    while (n && c->fd>=0) {
        if (c->used<24) {
            size_t k=24-c->used; if (k>n) k=n;
            if (!c->used) c->partial_since=now_ms();
            memcpy(c->input+c->used,data,k); c->used+=k; data+=k; n-=k;
            if (c->used<24) continue;
            if (!vava_media_header(c->input,&c->header)) { set_error(c->slot,"invalid_media_header"); close_client(i); return; }
            c->body=malloc(c->header.length);
            if (!c->body) { close_client(i); return; }
        }
        size_t k=c->header.length-c->body_used; if (k>n) k=n;
        memcpy(c->body+c->body_used,data,k); c->body_used+=k; data+=k; n-=k;
        if (c->body_used==c->header.length) {
            media_frame(i);
            if (c->fd<0) return;
            free(c->body); c->body=NULL; c->body_used=0; c->used=0; c->partial_since=0;
        }
    }
}
static void feed_alive(int i, const uint8_t *data, size_t n) {
    struct client *c=&clients[i];
    for (size_t k=0;k<n;++k) {
        uint8_t b=data[k];
        if (!c->used && b==0) continue;
        if (b!=(uint8_t)"sleep"[c->used]) { set_error(c->slot,"unexpected_alive_payload"); close_client(i); return; }
        if (!c->used) c->partial_since=now_ms();
        if (++c->used==5) {
            cameras[c->slot].sleep_seen=true;
            c->used=0; c->partial_since=0;
        }
    }
}

static const char *camera_state(int s) {
    struct camera *cam=&cameras[s];
    if (link_recovery_blocked(&cam->recovery,now_ms())) return "recovering_keepalive";
    if (registry[s].stage==1) return "pair_offered";
    if (registry[s].stage==2) return "pair_pending";
    if (cam->wake_until) return "waking";
    if (cam->sleep_until) return "sleep_requested";
    if (cam->link[CONTROL]>=0 && cam->control_ready) {
        if (cam->link[MEDIA]>=0 && cam->video_at && now_ms()-cam->video_at<3000) return "receiving_video";
        return "awake";
    }
    if (cam->link[ALIVE]>=0 && cam->sleep_seen) return "sleep_observed";
    if (cam->link[ALIVE]>=0) return "keepalive_connected";
    return "offline";
}
static void json_escape(const char *in, char *out) {
    static const char hex[]="0123456789abcdef";
    while (*in) {
        unsigned char c=(unsigned char)*in++;
        if (c=='"' || c=='\\') { *out++='\\'; *out++=(char)c; }
        else if (c<32) { memcpy(out,"\\u00",4); out+=4; *out++=hex[c>>4]; *out++=hex[c&15]; }
        else *out++=(char)c;
    }
    *out=0;
}
/* Bounded, quoted identifiers from report 5; missing or malformed fields stay null.
 * +32 firmware, +48 hardware and +96 MCU firmware are traced independently. */
static void identity_text(const uint8_t *field, char out[18]) {
    size_t n=0;
    while (n<16 && field[n]) {
        unsigned c=field[n];
        if (!((c>='A' && c<='Z') || (c>='a' && c<='z') ||
              (c>='0' && c<='9') || c=='-' || c=='_' || c=='.')) break;
        ++n;
    }
    if (!n || n==16 || field[n]) { memcpy(out,"null",5); return; }
    out[0]='"'; memcpy(out+1,field,n); out[n+1]='"'; out[n+2]=0;
}
static void status(int i) {
    uint64_t now=now_ms();
    size_t queued=0,media_allocated=0; unsigned connections=0;
    for (int j=0;j<MAX_CLIENTS;++j) {
        queued+=clients[j].tx_used;
        if (clients[j].body) media_allocated+=clients[j].header.length;
        connections+=clients[j].fd>=0;
    }
    struct storage_summary storage=recorder_summary();
    char network_name[6*32+1]; json_escape(ssid,network_name);
    char pairing[512]="null",removal[640]="null";
#if MYVAVA_SDK
    if(!pairing_json(pairing,sizeof(pairing),now))strcpy(pairing,"null");
    if(!removal_json(removal,sizeof(removal),now))strcpy(removal,"null");
#endif
    (void)reply(i,"{\"ok\":true,\"version\":\"0.10.0\",\"hardware_verified\":false,"
        "\"uptime_seconds\":%" PRIu64 ",\"unix_seconds\":%" PRIu64 ",\"clock_valid\":%s,"
        "\"network\":{\"camera_bind\":\"%s\",\"ssid\":\"%s\"},"
        "\"storage\":{\"bytes\":%" PRIu64 ",\"quota_bytes\":%" PRIu64 ",\"queued_bytes\":%zu,\"records\":%u,\"active\":%u,\"encrypted\":%s,\"key_id\":\"%s\",\"available\":%s,\"generation\":%" PRIu64 ",\"error\":\"%s\"},"
        "\"ports\":[%u,%u,%u],\"pair_window_ms\":%" PRIu64 ",\"ap_managed\":%s,\"ap_state\":\"%s\","
        "\"rtsp\":{\"enabled\":%s,\"bind\":\"%s\",\"port\":%u,\"authentication\":%s},"
        "\"http\":{\"enabled\":%s,\"port\":%u,\"bind\":\"%s\",\"authentication\":%s},"
        "\"resources\":{\"clients\":%u,\"tx_allocated\":%zu,\"media_allocated\":%zu},\"pairing\":%s,\"removal\":%s,\"devices\":[",
        (now-started_at)/1000,unix_ms()/1000,clock_valid?"true":"false",bind_ip,network_name,
        storage.bytes,storage.quota,storage.queued,storage.records,storage.active,recorder_encrypted()?"true":"false",recorder_key_id(),storage.available?"true":"false",storage.generation,storage.error,
        ports[0],ports[1],ports[2],pair_until>now ? pair_until-now : 0,
        platform_managed()?"true":"false",platform_state(),
        rtsp_enabled ? "true":"false",rtsp_bind,rtsp_enabled ? rtsp_port : 0,rtsp_authorization[0]?"true":"false",
        http_enabled?"true":"false",http_enabled?http_port:0,http_bind,api_authorization[0]?"true":"false",
        connections,queued,media_allocated,pairing,removal);
    bool comma=false;
    for (int s=0;s<4;++s) {
        if (!registry[s].stage) continue;
        struct camera *cam=&cameras[s]; char ip[INET_ADDRSTRLEN];
        struct in_addr a={.s_addr=cam->peer}; inet_ntop(AF_INET,&a,ip,sizeof(ip));
        (void)reply(i,"%s{\"slot\":%d,\"serial\":\"%s\",\"paired\":%s,\"state\":\"%s\","
            "\"ip\":\"%s\",\"association\":\"%s\",\"generation\":%" PRIu64 ","
            "\"connections\":[%s,%s,%s],\"viewers\":%u,\"live_requested\":%s,"
            "\"video_frames\":%" PRIu64 ",\"audio_frames\":%" PRIu64 ",\"media_bytes\":%" PRIu64 ","
            "\"heartbeat_age_ms\":%" PRId64 ",\"heartbeat_current\":%s,\"alive_age_ms\":%" PRId64 ",\"signal_raw\":%" PRId64 ",\"battery_raw\":%u,"
            "\"power_raw\":%u,\"pir_raw\":%u,\"control_ready\":%s,\"first_connect_current\":%s,"
            "\"first_connect_age_ms\":%" PRId64 ",\"last_reply_command\":%u,\"error\":\"%s\","
            "\"live_media\":{\"codec_epoch\":%u,\"width\":%u,\"height\":%u,\"age_ms\":%" PRId64 ",\"rejected_frames\":%" PRIu64 ",\"dependent_frames_skipped\":%" PRIu64 "},"
            "\"keepalive_recovery\":{\"active\":%s,\"attempts\":%u,\"remaining_ms\":%" PRIu64 "},"
            "\"detection_reports\":%" PRIu64 ",\"last_detection\":",
            comma ? "," : "",s,registry[s].serial,registry[s].stage==3 ? "true":"false",camera_state(s),ip,
            registry[s].mac[0] ? "neighbor_mac":"lab_unique_ip",cam->generation,
            cam->link[0]>=0 ? "true":"false",cam->link[1]>=0 ? "true":"false",cam->link[2]>=0 ? "true":"false",
            watchers(s),cam->live_requested ? "true":"false",cam->frames,cam->audio_frames,cam->bytes,
            cam->heartbeat ? (int64_t)(now-cam->heartbeat) : -1,
            cam->heartbeat && cam->control_ready && cam->heartbeat_generation==cam->generation ? "true":"false",
            cam->link[ALIVE]>=0 ? (int64_t)(now-clients[cam->link[ALIVE]].last_rx) : -1,
            vava_u32(cam->heart)<=INT32_MAX ? (int64_t)vava_u32(cam->heart) : (int64_t)vava_u32(cam->heart)-INT64_C(4294967296),
            cam->heart[4],cam->heart[5],cam->first[19],cam->control_ready ? "true":"false",cam->first_seen ? "true":"false",
            cam->first_at ? (int64_t)(now-cam->first_at) : -1,
            cam->last_reply,cam->error,cam->live_codec.epoch,cam->video[1].width,cam->video[1].height,
            cam->video[1].at?(int64_t)(now-cam->video[1].at):-1,cam->video[1].rejected,cam->video[1].skipped,
            link_recovery_blocked(&cam->recovery,now)?"true":"false",cam->recovery.attempts,
            cam->recovery.quiet_until>now?cam->recovery.quiet_until-now:0,cam->detection_reports);
        if (cam->detection_reports) {
            (void)reply(i,"{\"result_raw\":%" PRIu32 ",\"type_raw\":%" PRIu32 ","
                "\"age_ms\":%" PRIu64 ",\"generation\":%" PRIu64 ",\"current_connection\":%s}",
                cam->detection_result,cam->detection_type,now-cam->detection_at,cam->detection_generation,
                cam->control_ready && cam->detection_generation==cam->generation ? "true":"false");
        } else (void)reply(i,"null");
        char profile[320],firmware[18],hardware[18],mcu[18];
        if (!profile_json(&profiles[s],profile,sizeof(profile))) { close_client(i); return; }
        identity_text(cam->first+32,firmware); identity_text(cam->first+48,hardware); identity_text(cam->first+96,mcu);
        (void)reply(i,",\"profile\":%s,\"identity_report\":{\"source\":\"first_connect\",\"seen\":%s,"
            "\"age_ms\":%" PRId64 ",\"current_connection\":%s,\"retail_model\":null,"
            "\"hardware\":%s,\"firmware\":%s,\"mcu_firmware\":%s}",profile,
            cam->first_at?"true":"false",cam->first_at?(int64_t)(now-cam->first_at):-1,
            cam->first_seen && cam->control_ready?"true":"false",hardware,firmware,mcu);
        char pj[2048];
        if (!camera_policy_json(s,pj,sizeof(pj))) { close_client(i); return; }
        struct record_info rec=recorder_state((unsigned)s);
        (void)reply(i,",\"policy\":%s,\"recording\":{\"active\":%s,\"finishing\":%s,\"id\":\"%016" PRIx64
            "\",\"frames\":%" PRIu64 ",\"bytes\":%" PRIu64 ",\"failed\":%s,\"reason\":\"%s\","
            "\"format\":\"%s\",\"audio_frames\":%" PRIu64 ",\"sample_rate\":%u,\"channels\":%u,"
            "\"duration_seconds\":%u,\"duration_source\":\"%s\",\"confirmation_required\":%s,\"confirmed\":%s}}",pj,
            cam->recording.active?"true":"false",rec.finishing?"true":"false",rec.id,rec.frames,rec.bytes,rec.failed?"true":"false",rec.error,
            recorder_format(&rec),rec.audio_frames,rec.sample_rate,rec.channels,cam->recording.seconds,
            cam->recording.duration_source?cam->recording.duration_source:"not_started",cam->recording.confirm_required?"true":"false",cam->recording.confirmed?"true":"false");
        comma=true;
    }
    (void)reply(i,"]");
#if MYVAVA_SDK
    (void)reply(i,",\"storage_workers\":{\"upload_ready\":%s,\"nas_configured\":%s,\"nas_ready\":%s}",upload_storage_ready(uploader)?"true":"false",nas?"true":"false",nas_storage_ready(nas)?"true":"false");
    struct myvava_client_status connection=myvava_client_status(server_client);
    (void)reply(i,",\"server_connection\":{\"enabled\":%s,\"online\":%s,\"phase\":\"%s\","
        "\"last_error\":\"%s\",\"last_response_age_ms\":%" PRIu64 ",\"requests\":%" PRIu64 ",\"responses\":%" PRIu64 ","
        "\"reconnects\":%" PRIu64 ",\"event_acks\":%" PRIu64 ",\"event_rejections\":%" PRIu64 "}",
        connection.enabled?"true":"false",connection.online?"true":"false",connection.phase,connection.last_error,
        connection.last_response_ms,connection.requests,connection.responses,connection.reconnects,connection.event_acks,connection.event_rejections);
    struct myvava_event_status events=myvava_events_status(event_spool);
    (void)reply(i,",\"event_spool\":{\"enabled\":%s,\"ready\":%s,\"retained\":%u,\"limit\":%u,"
        "\"queued\":%u,\"queue_limit\":64,\"dropped_since_boot\":%" PRIu64 ",\"committed_since_boot\":%" PRIu64 ","
        "\"acknowledged_since_boot\":%" PRIu64 ",\"error\":\"%s\"}",events.enabled?"true":"false",events.ready?"true":"false",
        events.retained,events.limit,events.queued,events.dropped,events.committed,events.acknowledged,events.error);
#endif
    (void)reply(i,"}\n");
}
static bool parse_number(const char *s, unsigned max, unsigned *out) {
    if (!s || !*s || *s<'0' || *s>'9') return false;
    errno=0; char *end; unsigned long n=strtoul(s,&end,10);
    if (errno || *end || n>max) return false;
    *out=(unsigned)n; return true;
}
static bool config_admin(int i, int s, unsigned count, char **w) {
    bool configure=!strcmp(w[0],"configure"), mode=!strcmp(w[0],"configure-pir");
    bool batch=!strcmp(w[0],"configure-many");
    bool apply=!strcmp(w[0],"apply-settings"), clear=!strcmp(w[0],"clear-settings");
    if (!configure && !mode && !apply && !clear && !batch) return false;
    struct desired next=preferences[s]; unsigned mask=0;
    if (apply && count>=2) {
        if (count==2) mask=next.mask;
        for (unsigned k=2;k<count;++k) {
            unsigned index=0;
            while (index<SETTINGS_COUNT && strcmp(w[k],settings_name(index))) ++index;
            if (index==SETTINGS_COUNT || !(next.mask & (1u<<index))) { fail_admin(i,"setting_not_saved"); return true; }
            if (mask & (1u<<index)) { fail_admin(i,"duplicate_setting"); return true; }
            mask|=1u<<index;
        }
    } else if (clear && count==2) {
        next.mask=0; memset(next.values,0,sizeof(next.values));
    } else if (batch && count>=3) {
        unsigned seen=0;
        for (unsigned k=2;k<count;++k) {
            char *eq=strchr(w[k],'='); unsigned value; uint32_t wire;
            if (!eq || eq==w[k]) { fail_admin(i,"invalid_setting_arguments"); return true; }
            *eq++=0;
            if (!parse_number(eq,100,&value)) { fail_admin(i,"invalid_setting_arguments"); return true; }
            int index=settings_index(w[k],value,&wire);
            if (index<0 || index==SETTING_PIR_LEVEL) { fail_admin(i,"invalid_persistent_setting_use_arm_for_pir_level"); return true; }
            unsigned bit=1u<<(unsigned)index;
            if (seen&bit) { fail_admin(i,"duplicate_setting"); return true; }
            seen|=bit;
            if (!(next.mask&bit) || next.values[index][0]!=value) mask|=bit;
            next.mask|=bit; next.values[index][0]=value;
        }
    } else if (configure && count==4) {
        unsigned value; uint32_t wire;
        if (!parse_number(w[3],100,&value)) { fail_admin(i,"invalid_setting_arguments"); return true; }
        int index=settings_index(w[2],value,&wire);
        if (index<0 || index==SETTING_PIR_LEVEL) { fail_admin(i,"invalid_persistent_setting_use_arm_for_pir_level"); return true; }
        mask=1u<<(unsigned)index; next.mask|=mask; next.values[index][0]=value;
    } else if (mode && (count==3 || count==6)) {
        unsigned sensitivity=0,wait=0,record=0;
        bool custom=!strcmp(w[2],"custom");
        if ((custom && (count!=6 || !parse_number(w[3],3,&sensitivity) || !parse_number(w[4],120,&wait) ||
            !parse_number(w[5],60,&record))) || (!custom && count!=3)) {
            fail_admin(i,"invalid_pir_mode_arguments"); return true;
        }
        const char *error=settings_pir_parameters(w[2],sensitivity,wait,record,0,next.values[SETTING_PIR_MODE]);
        if (error) { fail_admin(i,error); return true; }
        mask=1u<<SETTING_PIR_MODE; next.mask|=mask;
    } else { fail_admin(i,"invalid_configuration_arguments"); return true; }
    bool changed=memcmp(&next,&preferences[s],sizeof(next))!=0;
    if (!apply && changed) {
        if (next.revision==UINT32_MAX) { fail_admin(i,"configuration_revision_exhausted"); return true; }
        ++next.revision;
        struct desired saved[4]; memcpy(saved,preferences,sizeof(saved)); saved[s]=next;
        if (desired_save(state_dir,registry,saved)) { fail_admin(i,"configuration_write_failed"); return true; }
        preferences[s]=next;
    }
    struct camera *cam=&cameras[s];
    if (!apply && !changed) mask=0;
    if (clear) { cam->config.pending=0; cam->config.delivery="preferences_cleared_in_flight_unchanged"; }
    else if (mask) {
        if (cam->config.halted) cam->config.pending=0;
        for (unsigned j=0;j<SETTINGS_COUNT;++j) if (mask & (1u<<j)) cam->config.blocked[j]=NULL;
        cam->config.pending|=mask; cam->config.halted=false; cam->config.deadline=0;
        config_tick(s,now_ms());
    }
    char json[3072];
    if (!config_json(s,json,sizeof(json))) fail_admin(i,"configuration_response_limit");
    else (void)reply(i,"{\"ok\":true,\"station_saved\":true,\"camera_applied\":null,\"desired\":%s}\n",json);
    return true;
}
#if MYVAVA_SDK
static void storage_dispatch(int i,unsigned slot,const char *serial,unsigned count,char **words){
    uint64_t id=storage_io_submit(storage_worker,slot,serial,count,words);
    if(!id){fail_admin(i,"storage_worker_busy");return;}
    if(i==DIRECT_COMMAND)sdk_storage_job=id;
    else {clients[i].storage_job=id;clients[i].close_after=true;clients[i].born=now_ms();}
}
#endif
static void admin_command(int i, char *line) {
    char *words[16]={0}, *save=NULL; unsigned count=0;
    for (char *t=strtok_r(line," \r\t",&save); t; t=strtok_r(NULL," \r\t",&save)) {
        if (count==16) { fail_admin(i,"invalid_request"); return; } words[count++]=t;
    }
    if (i!=DIRECT_COMMAND) clients[i].close_after=true;
    if (!count) { fail_admin(i,"invalid_request"); return; }
    /* Optional identity and compare-and-save guards are checked before mutation. */
    const char *expected_serial=NULL; bool guarded_revision=false; unsigned expected_revision=0;
    while (!strcmp(words[0],"if-serial") || !strcmp(words[0],"if-revision")) {
        if (count<4) { fail_admin(i,"invalid_request"); return; }
        if (!strcmp(words[0],"if-serial")) {
            if (expected_serial) { fail_admin(i,"invalid_request"); return; }
            expected_serial=words[1];
        } else {
            if (guarded_revision || !parse_number(words[1],UINT32_MAX,&expected_revision)) {
                fail_admin(i,"invalid_revision_guard"); return;
            }
            guarded_revision=true;
        }
        count-=2; memmove(words,words+2,count*sizeof(*words));
    }
    if (expected_serial || guarded_revision) {
        unsigned slot;
        bool archive_command=false;
#if MYVAVA_SDK
        archive_command=expected_serial&&!guarded_revision&&(storage_io_command(words[0],false)||!strcmp(words[0],"record-get"));
#endif
        if (count<2 || !parse_number(words[1],3,&slot) ||
            ((!registry[slot].stage || (expected_serial && strcmp(registry[slot].serial,expected_serial))) &&
             !(archive_command&&recorder_identity(slot,expected_serial)))) {
            fail_admin(i,"camera_identity_changed"); return;
        }
        if (guarded_revision) {
            const char *guarded=words[0]; uint32_t revision;
            if (!strcmp(guarded,"configure") || !strcmp(guarded,"configure-many") || !strcmp(guarded,"configure-pir") ||
                !strcmp(guarded,"clear-settings") || !strcmp(guarded,"apply-settings") || !strcmp(guarded,"live-quality")) revision=preferences[slot].revision;
            else if (!strcmp(guarded,"policy-config") || !strcmp(guarded,"plan")) revision=policies[slot].revision;
            else if (!strcmp(guarded,"name")) revision=profiles[slot].revision;
            else { fail_admin(i,"revision_guard_not_supported"); return; }
            if (revision!=expected_revision) { fail_admin(i,"configuration_conflict"); return; }
        }
    }
    const char *cmd=words[0];
#if MYVAVA_SDK
    {
        if ((!strcmp(cmd,"capabilities")) && count==1) {
            (void)reply(i,"{\"ok\":true,\"protocol\":\"myvava-control/1\",\"control\":true,"
                "\"media\":true,\"media_lease_seconds\":null,\"live_time_limit_seconds\":null,\"media_idle_lease_ms\":2500,\"talk_playback_verified\":false,\"files\":true,\"file_download\":true,\"on_demand_exports\":true,\"at_rest_encryption\":%s,\"durable_events\":%s,\"cloud_upload\":%s,\"nas\":%s,"
                "\"work_mode_owns_sensitivity\":true,\"plan_enable\":true,\"detection_box_control\":true,\"live_quality_switch\":true,\"quality_switch_requires_media_idle\":true,\"automatic_quality\":false,\"quality_policy_location\":\"viewer\","
                "\"continuous_quality_switch\":%s,\"media_framing\":2,\"local_av_clips\":true,\"clip_format\":\"mpegts-h264-aac\",\"record_query\":true,\"record_query_all\":true,\"clip_detection_types\":true,\"record_preview\":true,\"record_info_details\":true,\"record_delete_errors\":true,\"record_page_size\":16,\"record_limit\":65536,\"archive_layout\":\"vava-idx/1\",\"camera_remove_factory_reset\":true,\"camera_restore_defaults\":true,\"record_cameras\":true,\"historic_record_identity\":true}\n",
#if MYVAVA_SDK
                recorder_encrypted()?"true":"false",myvava_events_status(event_spool).ready?"true":"false",uploader?"true":"false",nas?"true":"false",continuous_quality?"true":"false"
#else
                "false","false","false","false","false"
#endif
            ); return;
        }
    }
#endif
    if (i==DIRECT_COMMAND) {
        if (!strcmp(cmd,"stream") || !strcmp(cmd,"audio") || !strcmp(cmd,"talk") ||
            !strcmp(cmd,"snapshot") || !strcmp(cmd,"record-get") || !strcmp(cmd,"live") || !strcmp(cmd,"wake")) {
            fail_admin(i,"use_sdk_media_or_file_channel"); return;
        }
        bool station_command=!strcmp(cmd,"status") || !strcmp(cmd,"list") || !strcmp(cmd,"clock-valid") ||
            !strcmp(cmd,"storage-policy") || !strcmp(cmd,"storage-policy-set") || !strcmp(cmd,"storage-cleanup") || !strcmp(cmd,"upload-rate") || !strcmp(cmd,"nas-status") || !strcmp(cmd,"nas-config") || !strcmp(cmd,"nas-control") || !strcmp(cmd,"record-cameras") || !strcmp(cmd,"record-query-all") || !strcmp(cmd,"pair-open") || !strcmp(cmd,"pair-close");
        if (!station_command && !expected_serial) { fail_admin(i,"camera_identity_required"); return; }
    }
    if (i!=DIRECT_COMMAND && clients[i].kind==HTTP && (!strcmp(cmd,"stream") || !strcmp(cmd,"audio") || !strcmp(cmd,"talk"))) {
        fail_admin(i,"use_rtsp_or_local_media_client"); return;
    }
    if ((!strcmp(cmd,"status") || !strcmp(cmd,"list")) && count==1) { status(i); return; }
    if (!strcmp(cmd,"clock-valid") && count==2 && (!strcmp(words[1],"0") || !strcmp(words[1],"1"))) {
        clock_valid=words[1][0]=='1';recorder_clock(clock_valid);
        for (int s=0;s<4;++s) arm_tick(s,now_ms());
        (void)reply(i,"{\"ok\":true,\"clock_valid\":%s}\n",clock_valid?"true":"false"); return;
    }
#if MYVAVA_SDK
    if(storage_io_command(cmd,true)){storage_dispatch(i,0,"",count,words);return;}
    if(!strcmp(cmd,"nas-status")&&count==1){char json[2048];if(!nas_status(nas,json,sizeof(json)))fail_admin(i,"nas_status_unavailable");else (void)reply(i,"%s\n",json);return;}
    if((!strcmp(cmd,"nas-config")&&count==5)||(!strcmp(cmd,"nas-control")&&count==3)){
        unsigned revision,value=0;if(!parse_number(words[1],UINT32_MAX,&revision)||(!strcmp(cmd,"nas-config")&&!parse_number(words[2],1,&value))){fail_admin(i,"invalid_nas_arguments");return;}
        const char *error=!strcmp(cmd,"nas-config")?nas_configure(nas,revision,value!=0,words[3],words[4]):nas_control(nas,revision,words[2]);
        if(error)fail_admin(i,error);else {char json[2048];if(nas_status(nas,json,sizeof(json)))(void)reply(i,"%s\n",json);else fail_admin(i,"nas_status_unavailable");}return;
    }
    if(!strcmp(cmd,"upload-rate")&&(count==1||count==2)){
        unsigned rate=0;if(!uploader||(count==2&&(!parse_number(words[1],4096,&rate)||!upload_rate(uploader,rate))))fail_admin(i,"upload_rate_unavailable_or_invalid");
        else (void)reply(i,"{\"ok\":true,\"rate_kib\":%u,\"scope\":\"cloud_upload\"}\n",upload_get_rate(uploader));return;
    }
#endif
    if (!strcmp(cmd,"storage-cleanup") && count==1) {
        uint64_t removed=0;
        if (!recorder_cleanup(&removed)) fail_admin(i,"cleanup_requires_idle_storage_or_io_failed");
        else (void)reply(i,"{\"ok\":true,\"removed_bytes\":%" PRIu64 "}\n",removed);
        return;
    }
    if (!strcmp(cmd,"pair-open") && count==2) {
        unsigned seconds; uint32_t random;
        if (!parse_number(words[1],300,&seconds) || !seconds) { fail_admin(i,"window_must_be_1_to_300_seconds"); return; }
        if (!password[0]) { fail_admin(i,"password_file_required"); return; }
#if MYVAVA_SDK
        bool available=false;for(unsigned s=0;s<4;++s)if(!registry[s].stage)available=true;
        if(!available){fail_admin(i,"camera_slots_full");return;}
        if(pairing_active()){fail_admin(i,"pair_operation_active");return;}
        if(removal_active()||removal_phase()==REMOVE_UNKNOWN){fail_admin(i,"camera_removal_active_or_unconfirmed");return;}
#endif
        char suffix[9],pair_name[32],pair_key[16];
        if (pairing_serial) { memcpy(suffix,pairing_serial+17,8); suffix[8]=0; }
        else {
            if (!random_bytes(&random,sizeof(random))) { fail_admin(i,"random_failed"); return; }
            snprintf(suffix,sizeof(suffix),"%08" PRIx32,random);
        }
        snprintf(pair_name,sizeof(pair_name),"VAPair-%s",suffix);
        snprintf(pair_key,sizeof(pair_key),"va%.2s%.2s%.2s",suffix+6,suffix,suffix+4);
#if MYVAVA_SDK
        if(!pairing_begin(seconds,now_ms(),unix_ms())){fail_admin(i,"pair_operation_write_failed");return;}
#endif
        if (platform_managed()) {
            if (!platform_pair_open(pair_name,pair_key,seconds,now_ms())) {
#if MYVAVA_SDK
                (void)pairing_change(PAIR_FAILED,-1,NULL,"pair_ap_busy_or_helper_failed",now_ms(),0);
#endif
                fail_admin(i,"pair_ap_busy_or_helper_failed"); return;
            }
            pair_until=0;
        } else pair_until=now_ms()+seconds*1000;
        uint64_t operation_id=0;
#if MYVAVA_SDK
        operation_id=pairing_id();
#endif
        (void)reply(i,"{\"ok\":true,\"operation_id\":\"%016" PRIx64 "\",\"ap_managed\":%s,\"ap_state\":\"%s\",\"pair_ssid\":\"%s\","
            "\"pair_password\":\"%s\",\"window_seconds\":%u}\n",
            operation_id,platform_managed()?"true":"false",platform_state(),pair_name,pair_key,seconds); return;
    }
    if (!strcmp(cmd,"pair-close") && count==1) {
#if MYVAVA_SDK
        (void)pairing_change(PAIR_CANCELLED,-1,NULL,"operator_cancelled",now_ms(),0);
#endif
        pair_until=0;
        platform_pair_close(now_ms());
        for (int s=0;s<4;++s) if (registry[s].stage==1) {
            for (int j=0;j<MAX_CLIENTS;++j) if (clients[j].fd>=0 && clients[j].slot==s) close_client(j);
            memset(&registry[s],0,sizeof(registry[s]));
        }
        (void)reply(i,"{\"ok\":true}\n"); return;
    }
    unsigned slot;
    bool archived_request=false;
#if MYVAVA_SDK
    archived_request=expected_serial&&(storage_io_command(cmd,false)||!strcmp(cmd,"record-get"));
#endif
    if (count<2 || !parse_number(words[1],3,&slot) ||
        (!registry[slot].stage && !archived_request)) {
        fail_admin(i,"unknown_slot_or_command"); return;
    }
    int s=(int)slot; struct camera *cam=&cameras[s];
    if (!strcmp(cmd,"forget") && count==2) {
#if MYVAVA_SDK
        fail_admin(i,"factory_reset_required_use_remove");return;
#else
        if (recorder_state(slot).active) { fail_admin(i,"recording_must_finish_before_forget"); return; }
        struct profile cleared_profiles[4]; memcpy(cleared_profiles,profiles,sizeof(cleared_profiles));
        memset(cleared_profiles[s].name,0,sizeof(cleared_profiles[s].name));
        if (cleared_profiles[s].revision<UINT32_MAX) ++cleared_profiles[s].revision;
        if (profile_save(state_dir,registry,cleared_profiles)) { fail_admin(i,"profile_write_failed"); return; }
        profiles[s]=cleared_profiles[s];
        struct policy cleared[4]; memcpy(cleared,policies,sizeof(cleared)); policy_default(&cleared[s]);
#if MYVAVA_SDK
        if(!diagnostic_defaults)policy_daily_default(&cleared[s]);
#endif
        struct desired reset[4]; memcpy(reset,preferences,sizeof(reset)); memset(&reset[s],0,sizeof(reset[s]));
        if (desired_save(state_dir,registry,reset)) { fail_admin(i,"configuration_write_failed"); return; }
        preferences[s]=reset[s]; cam->config.pending=0;
        if (policy_save(state_dir,registry,cleared)) { fail_admin(i,"policy_write_failed"); return; }
        policies[s]=cleared[s];
#if MYVAVA_SDK
        upload_policy(uploader,(unsigned)s,registry[s].serial,false);
        nas_policy(nas,(unsigned)s,registry[s].serial,false);
#endif
        struct registration old=registry[s]; memset(&registry[s],0,sizeof(registry[s]));
        if (registry_save(state_dir,registry)) { registry[s]=old; fail_admin(i,"registry_write_failed"); return; }
#if MYVAVA_SDK
        upload_policy(uploader,(unsigned)s,"",false);nas_policy(nas,(unsigned)s,"",false);
#endif
        memset(&profiles[s],0,sizeof(profiles[s]));
        for (int j=0;j<MAX_CLIENTS;++j) if (clients[j].fd>=0 && clients[j].slot==s) close_client(j);
        memset(cam,0,sizeof(*cam)); for (int k=0;k<3;++k) cam->link[k]=-1;
        (void)reply(i,"{\"ok\":true,\"camera_reset_sent\":false}\n"); return;
#endif
    }
#if MYVAVA_SDK
    if(!strcmp(cmd,"remove-confirm")&&count==3){
        char id[17];snprintf(id,sizeof(id),"%016" PRIx64,removal_id());
        if(!expected_serial||strcmp(id,words[2])||removal_slot()!=slot||strcmp(removal_serial(),registry[s].serial)||!removal_confirm(now_ms())){
            fail_admin(i,"removal_confirmation_conflict");return;
        }
        (void)reply(i,"{\"ok\":true,\"operation_id\":\"%s\"}\n",id);return;
    }
    if((!strcmp(cmd,"remove")||!strcmp(cmd,"reset"))&&count==2){
        bool defaults_only=!strcmp(cmd,"reset");
        if(!expected_serial){fail_admin(i,"camera_identity_required");return;}
        if(registry[s].stage!=3){fail_admin(i,"pairing_incomplete");return;}
        if(pairing_active()){fail_admin(i,"pair_operation_active");return;}
        if(removal_active()){fail_admin(i,"camera_removal_active");return;}
        if(recorder_state(slot).active||recorder_state(slot).finishing){fail_admin(i,"recording_must_finish_before_remove");return;}
        if(watchers(s)||cam->live_requested||cam->settings.active){fail_admin(i,"camera_busy_stop_live_or_settings");return;}
        if(defaults_only&&preferences[s].revision==UINT32_MAX){fail_admin(i,"configuration_revision_exhausted");return;}
        if(!removal_begin(slot,registry[s].serial,now_ms(),unix_ms(),defaults_only,defaults_only?preferences[s].revision+1:0)){fail_admin(i,"removal_state_unavailable_or_unresolved");return;}
        cam->config.halted=true;cam->config.pending=0;cam->arm.pending=false;
        char json[640];if(!removal_json(json,sizeof(json),now_ms())){fail_admin(i,"removal_response_limit");return;}
        (void)reply(i,"{\"ok\":true,\"operation_id\":\"%016" PRIx64 "\",\"removal\":%s}\n",removal_id(),json);return;
    }
    if(storage_io_command(cmd,false)){storage_dispatch(i,slot,expected_serial?expected_serial:registry[s].serial,count,words);return;}
    if(!archived_request&&removal_blocks(slot,registry[s].serial)){fail_admin(i,"camera_removal_in_progress_or_unconfirmed");return;}
#endif
    if (registry[s].stage!=3 && !archived_request) { fail_admin(i,"pairing_incomplete"); return; }
    if (!strcmp(cmd,"name") && count==3) {
        struct profile next[4]; memcpy(next,profiles,sizeof(next));
        if (!profile_from_hex(words[2],next[s].name)) { fail_admin(i,"invalid_camera_name"); return; }
        if (strcmp(next[s].name,profiles[s].name)) {
            if (next[s].revision==UINT32_MAX) { fail_admin(i,"profile_revision_exhausted"); return; }
            ++next[s].revision;
            if (profile_save(state_dir,registry,next)) { fail_admin(i,"profile_write_failed"); return; }
            profiles[s]=next[s];
        }
        char json[320];
        if (!profile_json(&profiles[s],json,sizeof(json))) { fail_admin(i,"profile_response_limit"); return; }
        (void)reply(i,"{\"ok\":true,\"station_saved\":true,\"camera_command_sent\":false,\"profile\":%s}\n",json);
        return;
    }
#if MYVAVA_SDK
    if((!strcmp(cmd,"upload-task")||!strcmp(cmd,"nas-task"))&&count==4){
        uint8_t raw[8];uint64_t id=0;if(!myvava_unhex(words[2],raw,8)){fail_admin(i,"invalid_record_id");return;}for(unsigned k=0;k<8;++k)id=id*256+raw[k];
        bool ok=!strcmp(cmd,"upload-task")?upload_action(uploader,slot,registry[s].serial,id,words[3]):nas_action(nas,slot,registry[s].serial,id,words[3]);
        if(!ok)fail_admin(i,"task_unavailable_or_action_refused");else (void)reply(i,"{\"ok\":true}\n");return;
    }
    if (!strcmp(cmd,"export-list") && count==2) {
        char json[8192];if(!upload_exports_json(uploader,slot,registry[s].serial,json,sizeof(json)))fail_admin(i,"export_status_unavailable");
        else (void)reply(i,"%s",json);return;
    }
    if ((!strcmp(cmd,"record-export")&&count==4)||
        ((!strcmp(cmd,"export-cancel")||!strcmp(cmd,"export-retry")||!strcmp(cmd,"export-forget"))&&count==3)) {
        uint8_t raw[8];uint64_t source=0,id=0;
        if(!myvava_unhex(words[2],raw,8)){fail_admin(i,"invalid_record_id");return;}
        for(unsigned k=0;k<8;++k)source=source*256+raw[k];id=source;
        bool ok;
        if(!strcmp(cmd,"record-export")){
            if(!myvava_unhex(words[3],raw,8)){fail_admin(i,"invalid_export_id");return;}
            id=0;for(unsigned k=0;k<8;++k)id=id*256+raw[k];
            ok=upload_export(uploader,slot,registry[s].serial,source,id);
        }else if(!strcmp(cmd,"export-forget"))ok=upload_export_forget(uploader,slot,registry[s].serial,id);
        else ok=upload_export_action(uploader,slot,registry[s].serial,id,!strcmp(cmd,"export-cancel"));
        if(!ok)fail_admin(i,"export_unavailable_or_conflict");else (void)reply(i,"{\"ok\":true,\"id\":\"%016" PRIx64 "\"}\n",id);return;
    }
    if (!strcmp(cmd,"nas-list") && (count==2 || count==3)) {
        unsigned page=0; char json[8192];
        if ((count==3 && !parse_number(words[2],4095,&page)) || !nas_json(nas,(unsigned)s,registry[s].serial,page,json,sizeof(json)))
            fail_admin(i,"nas_status_unavailable");
        else (void)reply(i,"%s",json);
        return;
    }
    if (!strcmp(cmd,"upload-list") && (count==2 || count==3)) {
        unsigned page=0; char json[8192];
        if ((count==3 && !parse_number(words[2],4095,&page)) || !upload_json(uploader,(unsigned)s,registry[s].serial,page,json,sizeof(json)))
            fail_admin(i,"upload_status_unavailable");
        else (void)reply(i,"%s",json);
        return;
    }
#endif
    if (policy_admin(i,s,count,words)) return;
#if MYVAVA_SDK
    if(!strcmp(cmd,"live-quality")){
        uint8_t raw[8];unsigned target;uint64_t lease;
        if(!continuous_quality||!expected_serial||!guarded_revision||count!=4||!myvava_unhex(words[2],raw,8)||!parse_number(words[3],2,&target)) {fail_admin(i,"continuous_quality_unavailable");return;}
        lease=mvm_u64(raw);
        if(!lease||lease!=remote_view[s].lease.id||remote_view[s].lease.framing!=2||remote_view[s].generation!=cam->generation||watchers(s)!=1||!cam->control_ready||cam->link[MEDIA]<0){fail_admin(i,"viewing_lease_changed_or_other_viewer");return;}
        if(memcmp(cam->first+48,"VA-HS03",8)||memcmp(cam->first+32,"1.4.0.54",9)){fail_admin(i,"live_quality_firmware_not_verified");return;}
        if(cam->config.pending||cam->config.in_flight||cam->settings.active){fail_admin(i,"persistent_configuration_in_progress");return;}
        cam->config.quality_lease=lease;cam->config.quality_generation=cam->generation;
        char *args[]={"configure",words[1],"quality",words[3]};
        if((preferences[s].mask&(1u<<SETTING_QUALITY))&&preferences[s].values[SETTING_QUALITY][0]==target){args[0]="apply-settings";(void)config_admin(i,s,3,args);}
        else (void)config_admin(i,s,4,args);
        return;
    }
#endif
    if(!strcmp(cmd,"configure")||!strcmp(cmd,"configure-many")||!strcmp(cmd,"apply-settings")||!strcmp(cmd,"clear-settings"))cam->config.quality_lease=0;
    if (config_admin(i,s,count,words)) return;
    if (!strcmp(cmd,"record") && count==3) {
        if (!strcmp(words[2],"start")) {
            if (!record_start(s,false)) { fail_admin(i,"record_start_unavailable"); return; }
        } else if (!strcmp(words[2],"stop")) record_end(s,"manual_stop");
        else { fail_admin(i,"invalid_record_action"); return; }
        (void)reply(i,"{\"ok\":true,\"id\":\"%016" PRIx64 "\",\"active\":%s}\n",cam->recording.id,cam->recording.active?"true":"false"); return;
    }
    if (!strcmp(cmd,"record-list") && (count==2 || count==3)) {
        unsigned page=0; char json[16384];
        if (count==3 && !parse_number(words[2],4095,&page)) { fail_admin(i,"invalid_page"); return; }
        if (!recorder_list(slot,registry[s].serial,page,json,sizeof(json))) fail_admin(i,"record_list_failed");
        else (void)queue(i,json,strlen(json));
        return;
    }
    if ((!strcmp(cmd,"record-get") || !strcmp(cmd,"record-delete")) && count==3) {
        uint64_t id=0; bool valid=strlen(words[2])==16;
        for (unsigned k=0;valid && k<16;++k) {
            char c=words[2][k];
            if (!((c>='0' && c<='9') || (c>='a' && c<='f'))) valid=false;
            else id=(id<<4)+(unsigned)(c<='9'?c-'0':c-'a'+10);
        }
        if (!valid) { fail_admin(i,"invalid_record_id"); return; }
        if (!strcmp(cmd,"record-delete")) {
            if (!recorder_delete(slot,registry[s].serial,id)) fail_admin(i,"record_delete_failed");
            else (void)reply(i,"{\"ok\":true}\n");
            return;
        }
#if MYVAVA_SDK
        struct record_reader reader={.fd=-1};
        if (!recorder_reader_open(slot,expected_serial?expected_serial:registry[s].serial,id,&reader)) { fail_admin(i,"record_read_failed_or_busy"); return; }
        clients[i].kind=DOWNLOAD; clients[i].slot=s; clients[i].download_reader=reader;
        clients[i].download_fd=-1; clients[i].download_remaining=reader.info.bytes; clients[i].close_after=false;
        clients[i].download_ts=reader.info.transport_stream;
        char hash[65]={0}; if (reader.info.transport_stream) myvava_hex(reader.info.sha256,32,hash);
        (void)reply(i,"{\"ok\":true,\"format\":\"%s\",\"bytes\":%" PRIu64 ",\"sha256\":\"%s\"}\n",
                    recorder_format(&reader.info),reader.info.bytes,hash); return;
#else
        char path[1024]; struct stat st;
        if (!recorder_path(slot,registry[s].serial,id,path,sizeof(path)) || lstat(path,&st) || !S_ISREG(st.st_mode)) {
            fail_admin(i,"record_not_found"); return;
        }
        int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
        if (fd<0) { fail_admin(i,"record_read_failed"); return; }
        if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size<0) {
            close(fd); fail_admin(i,"record_read_failed"); return;
        }
        clients[i].kind=DOWNLOAD; clients[i].slot=s; clients[i].download_fd=fd; clients[i].close_after=false;
        clients[i].download_remaining=(uint64_t)st.st_size;
        const char *format=strlen(path)>3 && !strcmp(path+strlen(path)-3,".ts")?"mpegts-h264-aac":"h264-annex-b";
        clients[i].download_ts=!strcmp(format,"mpegts-h264-aac");
        (void)reply(i,"{\"ok\":true,\"format\":\"%s\",\"bytes\":%" PRIu64 "}\n",format,clients[i].download_remaining); return;
#endif
    }
    if (!strcmp(cmd,"settings") && count==2) {
        char json[8192],desired[3072];
        if (!settings_json(&cam->settings,cam->generation,cam->control_ready,now_ms(),json,sizeof(json))) {
            fail_admin(i,"settings_response_limit"); return;
        }
        if (!config_json(s,desired,sizeof(desired))) {
            fail_admin(i,"configuration_response_limit"); return;
        }
        (void)reply(i,"{\"ok\":true,\"slot\":%u,\"desired\":%s,\"settings\":%s}\n",slot,desired,json); return;
    }
    bool pir_mode=!strcmp(cmd,"pir-mode");
    if ((!strcmp(cmd,"set") && (count==4 || count==5)) || pir_mode) {
        unsigned value=0; uint32_t wire_value=0,pir[5]={0}; int index; bool retry=false;
        if (pir_mode) {
            if (policies[s].managed && work_mode_saved(s)) {
                fail_admin(i,"pir_level_owned_by_work_mode"); return;
            }
            bool custom=count>=3 && !strcmp(words[2],"custom");
            unsigned expected=custom ? 6 : 3, sensitivity=0,wait=0,record=0;
            retry=count==expected+1;
            if ((count!=expected && !retry) || (retry && strcmp(words[expected],"retry")) ||
                (custom && (!parse_number(words[3],3,&sensitivity) || !parse_number(words[4],120,&wait) ||
                            !parse_number(words[5],60,&record)))) {
                fail_admin(i,"invalid_pir_mode_arguments"); return;
            }
            const char *error=policies[s].managed ?
                settings_pir_parameters(words[2],sensitivity,wait,record,
                    policy_evaluate(&policies[s],time(NULL),clock_valid),pir) :
                settings_pir_mode(&cam->settings,words[2],sensitivity,wait,record,cam->generation,pir);
            if (error) { fail_admin(i,error); return; }
            index=SETTING_PIR_MODE;
        } else {
            retry=count==5;
            if ((retry && strcmp(words[4],"retry")) || !parse_number(words[3],100,&value)) {
                fail_admin(i,"invalid_setting_arguments"); return;
            }
            index=settings_index(words[2],value,&wire_value);
            if (index<0) { fail_admin(i,"supported_settings_mirror_0_to_3_ir_0_or_1_antiflicker_50_or_60_pir_level_0_to_3"); return; }
        }
        if (!camera_enabled(s)) { fail_admin(i,"camera_disabled"); return; }
        if (cam->config.pending || cam->config.in_flight) { fail_admin(i,"persistent_configuration_in_progress"); return; }
        if (index==SETTING_PIR_LEVEL && policies[s].managed) { fail_admin(i,"arming_policy_owns_pir_level"); return; }
        if (index==SETTING_QUALITY && quality_blocked(s)) {
            fail_admin(i,"quality_requires_media_idle"); return;
        }
        if (index==SETTING_DETECTION_BOX && detection_blocked(s)) {
            fail_admin(i,detection_blocked(s)); return;
        }
        if (cam->sleep_until) { fail_admin(i,"sleep_transition_in_progress"); return; }
        int control=cam->link[CONTROL];
        if (control<0 || !cam->control_ready) { fail_admin(i,"no_usable_control"); return; }
        int decision=pir_mode ? settings_check_pir(&cam->settings,pir,cam->generation,retry) :
            settings_check(&cam->settings,(unsigned)index,value,cam->generation,retry);
        if (decision<0) { fail_admin(i,"setting_in_progress"); return; }
        if (!decision) {
            if (pir_mode && policies[s].managed && policies[s].level!=pir[1]) {
                struct policy saved[4]; memcpy(saved,policies,sizeof(saved));
                if (saved[s].revision==UINT32_MAX) { fail_admin(i,"policy_revision_exhausted"); return; }
                saved[s].level=pir[1]; ++saved[s].revision;
                if (policy_save(state_dir,registry,saved)) { fail_admin(i,"policy_write_failed"); return; }
                policies[s]=saved[s];
            }
            if (setting_sequence==UINT32_MAX) { fail_admin(i,"setting_id_exhausted"); return; }
            uint32_t id=++setting_sequence;
            if (pir_mode) settings_begin_pir(&cam->settings,pir,id,slot,cam->generation,now_ms(),clients[control].tx_enqueued+128);
            else settings_begin(&cam->settings,(unsigned)index,value,wire_value,id,slot,
                                cam->generation,now_ms(),clients[control].tx_enqueued+128);
            if (pir_mode) cam->settings.operations[index].policy_arming=policies[s].managed;
            if (index==SETTING_QUALITY) clear_video(cam);
            if (!send_command(control,settings_command((unsigned)index),cam->settings.operations[index].payload)) {
                fail_admin(i,"control_send_failed"); return;
            }
        }
        (void)reply(i,"{\"ok\":true,\"result\":\"%s\",\"operation_id\":%u,\"applied\":null,\"persisted\":null}\n",
            decision ? "duplicate_suppressed":"queued",cam->settings.operations[index].id); return;
    }
    if (!strcmp(cmd,"stop") && count==2) {
        cam->manual_live=false; cam->live_requested=watchers(s)>0 || (cam->recording.active && cam->recording.manual);
        (void)reply(i,"{\"ok\":true,\"camera_stop_sent\":false,\"remaining_viewers\":%u}\n",watchers(s)); return;
    }
    if ((!strcmp(cmd,"live") || !strcmp(cmd,"wake")) && count==2) {
        if (!camera_enabled(s)) { fail_admin(i,"camera_disabled"); return; }
        if (cam->sleep_until) { fail_admin(i,"sleep_transition_in_progress"); return; }
        if (!wake_start(s)) { fail_admin(i,"no_usable_control_or_keepalive"); return; }
        if (!strcmp(cmd,"wake")) cam->manual_wake=cam->wake_until!=0;
        if (!strcmp(cmd,"live")) {
            bool already_requested=cam->live_requested;
            cam->manual_live=true; cam->live_requested=true;
            if (cam->control_ready && !already_requested && !live_start(s)) { fail_admin(i,"control_send_failed"); return; }
        }
        (void)reply(i,"{\"ok\":true,\"result\":\"requested\",\"state\":\"%s\"}\n",camera_state(s)); return;
    }
    if (!strcmp(cmd,"iframe") && count==2) {
        uint8_t p[116]={0}; vava_put32(p,1);
        if (cam->link[CONTROL]<0 || !cam->control_ready || !send_command(cam->link[CONTROL],20,p)) {
            fail_admin(i,"no_usable_control"); return;
        }
        (void)reply(i,"{\"ok\":true,\"result\":\"queued\"}\n"); return;
    }
    if (!strcmp(cmd,"sleep") && count==2) {
        if (cam->live_requested || watchers(s) || cam->wake_until || cam->sleep_until || cam->settings.active ||
            (cam->config.pending && !cam->config.halted) || recorder_state(slot).active) {
            fail_admin(i,"active_task_or_transition"); return;
        }
        if (cam->link[ALIVE]<0 || cam->link[CONTROL]<0 || !cam->control_ready) {
            fail_admin(i,"control_and_keepalive_required"); return;
        }
        if (!send_command(cam->link[CONTROL],403,NULL)) { fail_admin(i,"control_send_failed"); return; }
        cam->sleep_seen=false; cam->sleep_close_at=now_ms()+1000; cam->sleep_until=now_ms()+15000;
        (void)reply(i,"{\"ok\":true,\"result\":\"requested\"}\n"); return;
    }
    if (!strcmp(cmd,"talk") && count==2) {
#if MYVAVA_SDK
        if(remote_view[s].lease.id&&remote_view[s].lease.talk){fail_admin(i,"talk_busy");return;}
#endif
        if (!camera_enabled(s) || !cam->control_ready || cam->link[MEDIA]<0 || !cam->live_requested || cam->sleep_until) {
            fail_admin(i,"talk_requires_live_control_and_media"); return;
        }
        if (!cam->first_seen || cam->first[1]!=3 || vava_u32(cam->first+28)!=8000) {
            fail_admin(i,"current_aac_8000_capability_required"); return;
        }
        for (int j=0;j<MAX_CLIENTS;++j) if (clients[j].fd>=0 && clients[j].kind==TALK && clients[j].slot==s) {
            fail_admin(i,"talk_busy"); return;
        }
        struct client *sink=&clients[i]; sink->kind=TALK; sink->slot=s; sink->camera_generation=cam->generation;
        sink->close_after=false; sink->used=0;
        uint8_t header[24]; vava_talk_header(header,0,0,unix_ms());
        if (!queue(cam->link[MEDIA],header,sizeof(header))) { fail_admin(i,"media_send_failed"); return; }
        (void)reply(i,"{\"ok\":true,\"format\":\"u32le_length_then_aac_adts_8000_mono\",\"max_frame\":1000,\"max_seconds\":null,\"playback_verified\":false}\n"); return;
    }
    bool audio=!strcmp(cmd,"audio"), snapshot=!strcmp(cmd,"snapshot");
    if ((!strcmp(cmd,"stream") && count==3) || (audio && count==3) || (snapshot && count==2)) {
        if (!camera_enabled(s)) { fail_admin(i,"camera_disabled"); return; }
        unsigned stream=1;
        if (!snapshot && (!parse_number(words[2],audio?9:1,&stream) || (audio && stream<8))) {
            fail_admin(i,"video_stream_0_or_1_audio_stream_8_or_9_required"); return;
        }
        if (cam->sleep_until || !wake_start(s)) { fail_admin(i,"no_usable_control_or_keepalive"); return; }
        bool already_requested=cam->live_requested;
        struct client *sink=&clients[i]; sink->kind=audio?AUDIO:STREAM; sink->slot=s; sink->stream=stream; sink->snapshot=snapshot;
        sink->close_after=false; cam->live_requested=true;
        if (!reply(i,"{\"ok\":true,\"format\":\"%s\",\"stream\":%u}\n",audio?"vava-audio-frames":"h264-annex-b",stream)) return;
        if (cam->control_ready) {
            uint8_t p[116]={0}; vava_put32(p,1);
            bool sent=already_requested ? send_command(cam->link[CONTROL],20,p) : live_start(s);
            if (!sent) { set_error(s,"control_send_failed"); close_client(i); }
        }
        return;
    }
    fail_admin(i,"unsupported_command");
}

#if MYVAVA_SDK
static size_t sdk_execute(const char *command, char *out, size_t capacity, void *arg) {
    (void)arg;
    if(sdk_storage_job){size_t length=0;int result=storage_io_poll(storage_worker,sdk_storage_job,out,capacity,&length);if(!result)return MYVAVA_EXECUTE_PENDING;sdk_storage_job=0;return result==1?length:0;}
    char line[512]; size_t n=strlen(command);
    if (!n || n>=sizeof(line) || strpbrk(command,"\r\n")) return 0;
    memcpy(line,command,n+1);
    struct command_output capture={.data=out,.capacity=capacity};
    direct_output=&capture;
    admin_command(DIRECT_COMMAND,line);
    direct_output=NULL;
    if(sdk_storage_job)return MYVAVA_EXECUTE_PENDING;
    return capture.failed?0:capture.used;
}
#endif

static bool rtp_queue(void *opaque, const void *data, size_t n) {
    return queue(*(int *)opaque,data,n);
}
static bool rtsp_response(int i, unsigned seq, unsigned code, const char *reason,
                          const char *headers, const char *body) {
    char session[80]={0};
    if (clients[i].rtsp.setup)
        snprintf(session,sizeof(session),"Session: %s;timeout=60\r\n",clients[i].rtsp.id);
    return reply(i,"RTSP/1.0 %u %s\r\nCSeq: %u\r\nServer: vava-station/0.5.0\r\n"
        "%s%sContent-Length: %zu\r\n\r\n%s",code,reason,seq,session,headers,strlen(body),body);
}
static void rtsp_error(int i, unsigned seq, unsigned code, const char *reason, bool finish) {
    if (finish) {
        clients[i].close_after=true; clients[i].rtsp.demand=false;
        clients[i].rtsp.describing=false; clients[i].rtsp.playing=false;
        release_demand(clients[i].slot);
    }
    (void)rtsp_response(i,seq,code,reason,"","");
}
static bool rtsp_request_live(int i, int s, unsigned stream) {
    struct client *c=&clients[i]; struct camera *cam=&cameras[s];
    if (!camera_enabled(s) || cam->sleep_until || !wake_start(s)) return false;
    bool previous=cam->live_requested;
    c->slot=s; c->stream=stream; c->rtsp.demand=true; cam->live_requested=true;
    if (cam->control_ready) {
        uint8_t p[116]={0}; vava_put32(p,stream);
        if (!(previous ? send_command(cam->link[CONTROL],20,p) : live_start(s))) {
            c->rtsp.demand=false; release_demand(s); return false;
        }
    }
    return true;
}
static bool rtsp_describe_ready(int i) {
    struct client *c=&clients[i]; struct camera *cam=&cameras[c->slot]; unsigned st=c->stream;
    char sdp[4096];
    int size=rtsp_sdp(sdp,sizeof(sdp),cam->parameters[st].sps,cam->parameters[st].sps_len,
                      cam->parameters[st].pps,cam->parameters[st].pps_len);
    if (size<0) return false;
    struct sockaddr_in local; socklen_t len=sizeof(local); char ip[INET_ADDRSTRLEN], headers[256];
    if (getsockname(c->fd,(struct sockaddr *)&local,&len) || !inet_ntop(AF_INET,&local.sin_addr,ip,sizeof(ip))) {
        close_client(i); return true;
    }
    snprintf(headers,sizeof(headers),"Content-Type: application/sdp\r\n"
        "Content-Base: rtsp://%s:%u/camera/%d/%u/\r\n",ip,rtsp_port,c->slot,st);
    c->rtsp.describing=false; c->rtsp.deadline=now_ms()+15000;
    (void)rtsp_response(i,c->rtsp.cseq,200,"OK",headers,sdp);
    return true;
}
static bool rtsp_session_matches(const struct client *c, const char *session) {
    size_t n=strcspn(session,"; ");
    return c->rtsp.setup && n==16 && !memcmp(session,c->rtsp.id,16);
}
static void rtsp_command(int i, const struct rtsp_request *r) {
    struct client *c=&clients[i]; struct rtsp_session *session=&c->rtsp;
    if (rtsp_authorization[0] && !auth_equal(r->authorization,rtsp_authorization)) {
        (void)rtsp_response(i,r->cseq,401,"Unauthorized","WWW-Authenticate: Basic realm=\"VAVA Local\"\r\n",""); return;
    }
    if (!strcmp(r->method,"OPTIONS")) {
        (void)rtsp_response(i,r->cseq,200,"OK","Public: OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN\r\n",""); return;
    }
    unsigned slot,stream; bool track;
    if (!rtsp_path(r->uri,&slot,&stream,&track) || registry[slot].stage!=3) {
        rtsp_error(i,r->cseq,404,"Not Found",false); return;
    }
    if (session->demand && (c->slot!=(int)slot || c->stream!=stream)) {
        rtsp_error(i,r->cseq,455,"Method Not Valid in This State",false); return;
    }
    if (!strcmp(r->method,"DESCRIBE")) {
        if (session->setup || track) { rtsp_error(i,r->cseq,455,"Method Not Valid in This State",false); return; }
        if (!rtsp_request_live(i,(int)slot,stream)) { rtsp_error(i,r->cseq,503,"Service Unavailable",true); return; }
        session->describing=true; session->cseq=r->cseq; session->deadline=now_ms()+15000;
        (void)rtsp_describe_ready(i); return;
    }
    if (!strcmp(r->method,"SETUP")) {
        unsigned channel;
        if (!rtsp_transport(r->transport,&channel)) { rtsp_error(i,r->cseq,461,"Unsupported Transport",false); return; }
        if (!session->demand || session->setup || !track) { rtsp_error(i,r->cseq,455,"Method Not Valid in This State",false); return; }
        uint8_t random[18];
        if (!random_bytes(random,sizeof(random))) { rtsp_error(i,r->cseq,500,"Internal Server Error",true); return; }
        for (unsigned j=0;j<8;++j) snprintf(session->id+j*2,3,"%02x",random[j]);
        memcpy(&session->ssrc,random+8,4); memcpy(&session->timestamp,random+12,4); memcpy(&session->sequence,random+16,2);
        session->channel=channel; session->setup=true; session->deadline=now_ms()+15000;
        char headers[160];
        snprintf(headers,sizeof(headers),"Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u;ssrc=%08" PRIX32 "\r\n",
            channel,channel+1,session->ssrc);
        (void)rtsp_response(i,r->cseq,200,"OK",headers,""); return;
    }
    if (!strcmp(r->method,"PLAY") || !strcmp(r->method,"GET_PARAMETER") || !strcmp(r->method,"TEARDOWN")) {
        if (!rtsp_session_matches(c,r->session)) { rtsp_error(i,r->cseq,454,"Session Not Found",false); return; }
        if (!strcmp(r->method,"TEARDOWN")) {
            session->demand=false; session->playing=false; release_demand(c->slot);
            c->close_after=true; (void)rtsp_response(i,r->cseq,200,"OK","",""); return;
        }
        if (!strcmp(r->method,"PLAY") && !session->playing) {
            session->playing=true; c->keyframe=false; c->born=now_ms();
            if (!rtsp_request_live(i,c->slot,c->stream)) { rtsp_error(i,r->cseq,503,"Service Unavailable",true); return; }
        }
        (void)rtsp_response(i,r->cseq,200,"OK",!strcmp(r->method,"PLAY") ? "Range: npt=0.000-\r\n" : "",""); return;
    }
    rtsp_error(i,r->cseq,405,"Method Not Allowed",false);
}
static void feed_rtsp(int i, const uint8_t *data, size_t n) {
    struct client *c=&clients[i];
    do {
        size_t k=sizeof(c->input)-c->used; if (k>n) k=n;
        if (k) {
            if (!c->used) c->partial_since=now_ms();
            memcpy(c->input+c->used,data,k); c->used+=k; data+=k; n-=k;
        }
        while (c->used && !c->rtsp.describing && !c->close_after) {
            size_t consumed=0;
            if (c->input[0]=='$') {
                if (c->used<4) break;
                size_t len=(size_t)c->input[2]*256+c->input[3];
                if (!c->rtsp.setup || c->input[1]!=c->rtsp.channel+1 || len>4096 || !len) { close_client(i); return; }
                if (c->used<len+4) break;
                if (!rtsp_rtcp_valid(c->input+4,len)) { close_client(i); return; }
                consumed=len+4;
            } else {
                struct rtsp_request r; int parsed=rtsp_parse(c->input,c->used,&r,&consumed);
                if (!parsed) break;
                if (parsed<0) { rtsp_error(i,0,400,"Bad Request",true); return; }
                rtsp_command(i,&r); if (c->fd<0) return;
            }
            memmove(c->input,c->input+consumed,c->used-consumed); c->used-=consumed;
            c->partial_since=c->used ? now_ms() : 0;
        }
        if (c->close_after) return;
        if (c->used==sizeof(c->input)) { rtsp_error(i,0,400,"Bad Request",true); return; }
    } while (n);
}
static void rtsp_timers(int i, uint64_t now) {
    struct client *c=&clients[i]; struct rtsp_session *s=&c->rtsp;
    if (c->close_after) return;
    if (s->describing) {
        if (rtsp_describe_ready(i)) { if (c->fd>=0) feed_rtsp(i,NULL,0); }
        else if (now>=s->deadline) rtsp_error(i,s->cseq,503,"Service Unavailable",true);
    } else if (s->demand && !s->playing && now>=s->deadline) close_client(i);
    else if (s->playing && ((!c->keyframe && now-c->born>15000) ||
                           (c->keyframe && now-s->local_ms>15000))) {
        set_error(c->slot,"video_start_or_stall_timeout"); close_client(i);
    } else if (now-c->last_rx>60000 || (!s->demand && now-c->born>10000)) close_client(i);
}
static void rtsp_media(int j, int source, size_t n, bool idr) {
    struct client *sink=&clients[j], *c=&clients[source]; struct rtsp_session *s=&sink->rtsp;
    if (!s->playing || sink->close_after) return;
    struct camera *cam=&cameras[c->slot]; unsigned st=c->header.stream;
    bool first=!sink->keyframe;
    if (first && (c->header.type!=1 || !idr || !cam->parameters[st].sps_len || !cam->parameters[st].pps_len)) return;
    uint64_t now=now_ms();
    if (s->timed) {
        /* Preserve source timing, rebase on camera clock resets/reconnects. */
        uint64_t delta=c->header.timestamp-s->source_ms;
        if (c->header.timestamp<s->source_ms || delta>10000) {
            delta=now-s->local_ms; if (!delta) delta=1; if (delta>1000) delta=1000;
        }
        s->timestamp+=(uint32_t)(delta*90);
    }
    s->timed=true; s->source_ms=c->header.timestamp; s->local_ms=now;
    if (first) {
        if (!rtsp_h264(s,cam->parameters[st].sps,cam->parameters[st].sps_len,false,rtp_queue,&j) ||
            !rtsp_h264(s,cam->parameters[st].pps,cam->parameters[st].pps_len,false,rtp_queue,&j)) return;
        sink->keyframe=true;
    }
    if (!rtsp_h264(s,c->body,n,true,rtp_queue,&j)) return;
    if (!s->report_at || now-s->report_at>=5000) {
        struct timeval tv;
        if (!gettimeofday(&tv,NULL)) {
            s->report_at=now;
            (void)rtsp_report(s,(uint64_t)tv.tv_sec*1000+(uint64_t)tv.tv_usec/1000,rtp_queue,&j);
        }
    }
}

#if VAVA_HTTP
static bool http_header(int i, unsigned code, const char *reason, const char *type, uint64_t length, bool unknown_length) {
    char size[64]={0}; if (!unknown_length) snprintf(size,sizeof(size),"Content-Length: %" PRIu64 "\r\n",length);
    char cors[512]={0};
    if (clients[i].cors_origin[0]) snprintf(cors,sizeof(cors),
        "Access-Control-Allow-Origin: %s\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type\r\n"
        "Access-Control-Expose-Headers: Content-Length, Content-Type, X-Vava-API-Version\r\n",clients[i].cors_origin);
    return reply(i,"HTTP/1.1 %u %s\r\nContent-Type: %s\r\n%sConnection: close\r\nCache-Control: no-store\r\n"
        "Vary: Origin\r\nX-Vava-API-Version: 1\r\n%s"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\nReferrer-Policy: no-referrer\r\n\r\n",code,reason,type,size,cors);
}
static void http_error(int i, unsigned code, const char *reason) {
    clients[i].close_after=true;
    if (http_header(i,code,reason,"application/json",strlen(reason)+24,false))
        (void)reply(i,"{\"ok\":false,\"error\":\"%s\"}\n",reason);
}
static void feed_http(int i, const uint8_t *data, size_t n) {
    struct client *c=&clients[i];
    if (c->used+n>=sizeof(c->input)) { http_error(i,400,"Bad Request"); return; }
    memcpy(c->input+c->used,data,n); c->used+=n;
    struct http_request request; int result=http_parse(c->input,c->used,&request);
    if (!result) return;
    if (result<0) { http_error(i,400,"Bad Request"); return; }
    c->used=0;
    bool same_origin=http_same_origin(&request), allowed_origin=same_origin;
    for (unsigned k=0;k<http_origin_count;++k)
        if (!strcmp(request.origin,http_origins[k])) allowed_origin=true;
    if (!allowed_origin) { http_error(i,400,"Origin Not Allowed"); return; }
    if (request.has_origin) strcpy(c->cors_origin,request.origin);
    bool v1_status=!strcmp(request.path,"/api/v1/status");
    bool v1_caps=!strcmp(request.path,"/api/v1/capabilities");
    bool v1_commands=!strcmp(request.path,"/api/v1/commands");
    bool v1=v1_status || v1_caps || v1_commands;
    if (!strcmp(request.method,"OPTIONS")) {
        if (!v1 || !http_preflight_valid(&request)) { http_error(i,400,"Invalid Preflight"); return; }
        c->close_after=true; (void)http_header(i,204,"No Content","application/json",0,false); return;
    }
    if (web_enabled && !strcmp(request.method,"GET") && (!strcmp(request.path,"/") ||
        !strcmp(request.path,"/style.css") || !strcmp(request.path,"/app.js") ||
        !strcmp(request.path,"/api-client.js") || !strcmp(request.path,"/config.js"))) {
        const char *type="text/html; charset=utf-8"; c->asset=web_page;
        if (!strcmp(request.path,"/style.css")) { type="text/css; charset=utf-8"; c->asset=web_css; }
        if (!strcmp(request.path,"/app.js")) { type="text/javascript; charset=utf-8"; c->asset=web_js; }
        if (!strcmp(request.path,"/api-client.js")) { type="text/javascript; charset=utf-8"; c->asset=web_api; }
        if (!strcmp(request.path,"/config.js")) { type="text/javascript; charset=utf-8"; c->asset=web_config; }
        size_t size=0; for (unsigned k=0;c->asset[k];++k) size+=strlen(c->asset[k]);
        c->kind=ASSET;
        (void)http_header(i,200,"OK",type,size,false);
        return;
    }
    if (!auth_equal(api_authorization,request.authorization)) { http_error(i,401,"Unauthorized"); return; }
    if (!strcmp(request.method,"GET") && v1_caps) {
        const char *body=api_capabilities(web_enabled); c->close_after=true;
        if (http_header(i,200,"OK","application/json",strlen(body),false)) (void)queue(i,body,strlen(body));
        return;
    }
    if (!strcmp(request.method,"GET") && (!strcmp(request.path,"/api/status") || v1_status)) strcpy(request.body,"status");
    else if (!strcmp(request.method,"POST") && v1_commands) {
        if (strcmp(request.content_type,"application/json") && strcmp(request.content_type,"application/json; charset=utf-8")) {
            http_error(i,415,"application_json_required"); return;
        }
        char command[512]; const char *error=api_command(request.body,command);
        if (error) { http_error(i,400,error); return; }
        strcpy(request.body,command);
    }
    else if (strcmp(request.method,"POST") || strcmp(request.path,"/api")) { http_error(i,404,"Not Found"); return; }
    if (strlen(request.body)>511 || strpbrk(request.body,"\r\n")) { http_error(i,400,"Bad Request"); return; }
    if (!strncmp(request.body,"stream",6) || !strncmp(request.body,"audio",5) || !strncmp(request.body,"talk",4)) {
        http_error(i,400,"Use RTSP or local media client"); return;
    }
    admin_command(i,request.body);
    if (c->fd<0) return;
    /* Local command handlers produced bounded JSON; wrap it before any socket
     * write. Recordings use the length of the opened file; snapshots await an IDR. */
    uint8_t *body=c->tx; size_t size=c->tx_used;
    c->tx=NULL; c->tx_used=c->tx_sent=0; c->tx_enqueued=c->tx_completed=0;
    bool download=c->kind==DOWNLOAD || c->snapshot;
    uint64_t length=c->kind==DOWNLOAD ? c->download_remaining : (uint64_t)size;
    bool failed=v1 && size>=11 && !memcmp(body,"{\"ok\":false",11);
    const char *mime=download?(c->download_ts?"video/mp2t":"video/h264"):"application/json";
    if (http_header(i,failed?422:200,failed?"Unprocessable Content":"OK",mime,length,c->snapshot) && !download)
        (void)queue(i,body,size);
    free(body);
}
#endif

static void feed_talk(int i, const uint8_t *data, size_t n) {
    struct client *c=&clients[i]; struct camera *cam=&cameras[c->slot];
    if (!cam->control_ready || cam->generation!=c->camera_generation || cam->link[MEDIA]<0) { close_client(i); return; }
    while (n && c->fd>=0 && !c->close_after) {
        size_t need=c->used<4?4:4+vava_u32(c->input);
        size_t k=need-c->used; if (k>n) k=n;
        if (!c->used) c->partial_since=now_ms();
        memcpy(c->input+c->used,data,k); c->used+=k; data+=k; n-=k;
        if (c->used<4) continue;
        size_t length=vava_u32(c->input);
        if (length<8 || length>1000) { fail_admin(i,"invalid_audio_length"); return; }
        if (c->used<length+4) continue;
        if (!vava_talk_aac(c->input+4,length)) { fail_admin(i,"aac_lc_adts_8000_mono_required"); return; }
        uint64_t now=now_ms();
        if (!c->audio_window || now-c->audio_window>=1000) { c->audio_window=now; c->audio_bytes=0; }
        if (c->audio_bytes+length>16000) { fail_admin(i,"audio_rate_limit"); return; }
        c->audio_bytes+=(unsigned)length;
        uint8_t packet[1024]; vava_talk_header(packet,length,(uint8_t)c->audio_number++,unix_ms());
        memcpy(packet+24,c->input+4,length);
        if (!queue(cam->link[MEDIA],packet,length+24)) { fail_admin(i,"media_send_failed"); return; }
        c->used=0; c->partial_since=0;
    }
}
static void feed(int i, const uint8_t *data, size_t n) {
    struct client *c=&clients[i];
#if VAVA_HTTP
    if (c->kind==HTTP) { if (!c->close_after) feed_http(i,data,n); return; }
#endif
    if (c->kind==RTSP) { feed_rtsp(i,data,n); return; }
    if (c->kind==TALK) { feed_talk(i,data,n); return; }
    if (c->kind==MEDIA) { feed_media(i,data,n); return; }
    if (c->kind==ALIVE) { feed_alive(i,data,n); return; }
    if (c->kind==STREAM || c->kind==DOWNLOAD || c->kind==AUDIO || c->kind==ASSET) { close_client(i); return; }
    if (c->kind==ADMIN) {
        if (c->close_after) return;
        if (c->used>=512 || n>=512-c->used) { fail_admin(i,"request_too_long"); return; }
        memcpy(c->input+c->used,data,n); c->used+=n; c->input[c->used]=0;
        uint8_t *end=memchr(c->input,'\n',c->used);
        if (end) {
            *end=0;
            if (strlen((char *)c->input)!=(size_t)(end-c->input)) { fail_admin(i,"invalid_request"); return; }
            admin_command(i,(char *)c->input);
        }
        return;
    }
    while (n && c->fd>=0) {
        if (c->kind!=CONTROL || c->used>=128) { close_client(i); return; }
        size_t k=128-c->used; if (k>n) k=n;
        if (!c->used) c->partial_since=now_ms();
        memcpy(c->input+c->used,data,k); c->used+=k; data+=k; n-=k;
        if (c->used==128) {
            c->used=0; c->partial_since=0;
            control_packet(i,c->input);
        }
    }
}

static bool nonblock(int fd) {
    int flags=fcntl(fd,F_GETFL,0);
    return flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0 && fcntl(fd,F_SETFD,FD_CLOEXEC)==0;
}
static int tcp_listener(const char *address, unsigned *port) {
    int fd=socket(AF_INET,SOCK_STREAM,0),one=1;
    if (fd<0) return -1;
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)*port)};
    if (inet_pton(AF_INET,address,&a.sin_addr)!=1) { close(fd); return -1; }
    (void)setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one));
    if (!nonblock(fd) || bind(fd,(struct sockaddr *)&a,sizeof(a)) || listen(fd,16)) { close(fd); return -1; }
    socklen_t len=sizeof(a);
    if (getsockname(fd,(struct sockaddr *)&a,&len)) { close(fd); return -1; }
    *port=ntohs(a.sin_port); return fd;
}
static void accept_clients(int which) {
    /* Bound accept work so camera floods cannot starve timers or management. */
    for (int attempt=0;attempt<8;++attempt) {
        struct sockaddr_storage remote; socklen_t len=sizeof(remote);
        int fd=accept(listeners[which],(struct sockaddr *)&remote,&len);
        if (fd<0) return;
        int i=0; while (i<MAX_CLIENTS && clients[i].fd>=0) ++i;
        if (i==MAX_CLIENTS || !nonblock(fd)) { close(fd); continue; }
        unsigned total=0;
        for (int j=0;j<MAX_CLIENTS;++j) total+=clients[j].fd>=0;
        if (which!=ADMIN && which!=HTTP && total>=MAX_CLIENTS-4) { close(fd); continue; }
        struct client *c=&clients[i]; memset(c,0,sizeof(*c));
        c->fd=fd; c->slot=-1; c->kind=(enum kind)which;
        c->generation=++sequence; c->born=c->last_rx=now_ms();
        if (which!=ADMIN) {
            int one=1; (void)setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&one,sizeof(one));
            c->peer=((struct sockaddr_in *)&remote)->sin_addr.s_addr;
            if (which<3) lookup_mac(c->peer,c->mac);
        }
        if (which==CONTROL || which==MEDIA) {
            for(int s=0;s<4;++s) if(registry[s].stage==3 && registry[s].mac[0] && peer_allowed(s,c) &&
                link_recovery_blocked(&cameras[s].recovery,c->born)){close_client(i);break;}
        }
    }
}
static void timers(void) {
    uint64_t now=now_ms();
    platform_tick(now);
    if (platform_managed()) pair_until=platform_pair_until();
#if MYVAVA_SDK
    pairing_tick(now,platform_managed(),platform_state(),pair_until);
    removal_tick(now);
    if(!pairing_active()&&pair_until){pair_until=0;platform_pair_close(now);}
#endif
    for (int s=0;s<4;++s) {
        struct camera *cam=&cameras[s];
        if (settings_tick(&cam->settings,now) && cam->link[CONTROL]>=0) {
            set_error(s,"setting_queue_timeout"); close_client(cam->link[CONTROL]);
        }
        arm_tick(s,now);
        config_tick(s,now);
        struct record_info rec=recorder_state((unsigned)s);
        if (cam->recording.active) {
            if (rec.failed) record_end(s,"storage_failed");
            else if (!cam->recording.manual && !policy_evaluate(&policies[s],time(NULL),clock_valid)) record_end(s,"disarmed");
            else if (!cam->recording.first_frame && now>=cam->recording.first_deadline) record_end(s,"first_frame_timeout");
            else if (cam->recording.end_at && now>=cam->recording.end_at) record_end(s,"duration_reached");
        }
        if (registry[s].stage==1 && now>=cam->offer_until) {
            for (int i=0;i<MAX_CLIENTS;++i) if (clients[i].fd>=0 && clients[i].slot==s) close_client(i);
            memset(&registry[s],0,sizeof(registry[s]));
        }
        if (cam->wake_until) {
            if (now>=cam->wake_until) {
                cam->wake_until=0; cam->live_requested=false; cam->manual_live=false; cam->manual_wake=false;
                set_error(s,"wake_timeout"); drop_streams(s);
            } else if (now>=cam->next_wake && cam->wake_attempts<25) {
                int alive=cam->link[ALIVE];
                if (alive>=0) (void)queue(alive,"wakeup",6);
                struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons((uint16_t)ports[ALIVE]),.sin_addr={.s_addr=cam->peer}};
                if (cam->peer) (void)sendto(udp_fd,"wakeup",6,0,(struct sockaddr *)&a,sizeof(a));
                ++cam->wake_attempts; cam->next_wake=now+200;
            }
        }
        if (cam->sleep_close_at && now>=cam->sleep_close_at) {
            int ctrl=cam->link[CONTROL];
            if (ctrl<0 || clients[ctrl].tx_used==clients[ctrl].tx_sent) {
                if (ctrl>=0) close_client(ctrl);
                if (cam->link[MEDIA]>=0) close_client(cam->link[MEDIA]);
                cam->sleep_close_at=0;
            }
        }
        if (cam->sleep_until) {
            if (!cam->sleep_close_at && cam->link[CONTROL]<0 && cam->sleep_seen && cam->link[ALIVE]>=0) cam->sleep_until=0;
            else if (now>=cam->sleep_until) {
                cam->sleep_until=0; cam->sleep_close_at=0; set_error(s,"sleep_not_observed");
            }
        }
        bool removing=false;
#if MYVAVA_SDK
        removing=removal_blocks((unsigned)s,registry[s].serial);
#endif
        bool busy=removing || cam->live_requested || watchers(s) || cam->settings.active ||
            (cam->config.pending && !cam->config.halted) || cam->wake_until || cam->sleep_until || rec.active;
        if(link_recovery_tick(&cam->recovery,now,registry[s].stage==3 && registry[s].mac[0] &&
                policies[s].managed && policies[s].idle_seconds,cam->control_ready,
                cam->link[ALIVE]>=0,busy || rec.finishing)) {
            set_error(s,"keepalive_missing_recovering");
            fprintf(stderr,"camera %d: recovering missing keepalive, attempt %u\n",s,cam->recovery.attempts);
            if(cam->link[CONTROL]>=0)close_client(cam->link[CONTROL]);
            if(cam->link[MEDIA]>=0)close_client(cam->link[MEDIA]);
        }
        if (busy || !cam->arm.busy_at) cam->arm.busy_at=now;
        if (policies[s].managed && policies[s].idle_seconds && !busy &&
            now-cam->arm.busy_at>=(uint64_t)policies[s].idle_seconds*1000 &&
            cam->control_ready && cam->link[CONTROL]>=0 && cam->link[ALIVE]>=0) {
            if (send_command(cam->link[CONTROL],403,NULL)) {
                cam->sleep_seen=false; cam->sleep_close_at=now+1000; cam->sleep_until=now+15000;
            }
        }
    }
    for (int i=0;i<MAX_CLIENTS;++i) {
        struct client *c=&clients[i]; if (c->fd<0) continue;
#if MYVAVA_SDK
        if(c->storage_job){char result[16384];size_t length=0;int done=storage_io_poll(storage_worker,c->storage_job,result,sizeof(result),&length);
            if(done){c->storage_job=0;if(done==1)(void)queue(i,result,length);else fail_admin(i,"storage_result_unavailable");}
            else if(now-c->born>30000){close_client(i);continue;}
        }
#endif
        if (c->kind==ASSET) {
            if (now-c->born>15000) { close_client(i); continue; }
            if (c->tx_used==c->tx_sent) {
                uint8_t block[8192]; size_t n=0;
                while (c->asset[c->asset_line] && n<sizeof(block)) {
                    const char *p=c->asset[c->asset_line]+c->asset_offset;
                    size_t left=strlen(p), k=left;
                    if (k>sizeof(block)-n) k=sizeof(block)-n;
                    memcpy(block+n,p,k); n+=k; c->asset_offset+=k;
                    if (k==left) { ++c->asset_line; c->asset_offset=0; }
                }
                c->close_after=c->asset[c->asset_line]==NULL;
                if (!n) { close_client(i); continue; }
                if (!queue(i,block,n)) continue;
            }
        }
        if (c->kind==DOWNLOAD && c->download_reader.token && now>=c->download_reader.deadline_ms) {
            set_error(c->slot,"record_reader_expired"); close_client(i); continue;
        }
        if (c->kind==DOWNLOAD && c->tx_used==c->tx_sent) {
            if (!c->download_remaining) { close_client(i); continue; }
            uint8_t block[8192]; size_t take=sizeof(block);
            if (c->download_remaining<take) take=(size_t)c->download_remaining;
            ssize_t got=c->download_reader.token?recorder_reader_read(&c->download_reader,
                c->download_reader.info.bytes-c->download_remaining,block,take):read(c->download_fd,block,take);
            if (got>0) { c->download_remaining-=(uint64_t)got; (void)queue(i,block,(size_t)got); }
            else if (!got || errno!=EINTR) { set_error(c->slot,"record_download_incomplete"); close_client(i); }
            if (c->fd<0) continue;
        }
        if ((c->kind==MEDIA || c->kind==ALIVE) && c->slot<0) (void)companion(i);
        if (c->kind==MEDIA && c->slot>=0 && !c->camera_generation && now-c->born>10000) {
            set_error(c->slot,"media_control_timeout"); close_client(i); continue;
        }
        if (c->slot<0 && c->kind!=ADMIN && c->kind!=RTSP && c->kind!=HTTP && c->kind!=ASSET && now-c->born>10000) { close_client(i); continue; }
        if ((c->kind==ADMIN || c->kind==HTTP) && now-c->born>5000 && !c->close_after) { close_client(i); continue; }
        if (c->kind==STREAM && !c->keyframe && now-c->born>15000) {
            set_error(c->slot,"video_start_timeout"); close_client(i); continue;
        }
        if ((c->kind==AUDIO && now-c->born>15000) || (c->kind==TALK &&
            (now-c->last_rx>5000 || cameras[c->slot].generation!=c->camera_generation ||
             !cameras[c->slot].control_ready || cameras[c->slot].link[MEDIA]<0))) { close_client(i); continue; }
        if (c->kind==RTSP) { rtsp_timers(i,now); if (c->fd<0) continue; }
        if (c->kind==CONTROL && now-c->last_rx>(uint64_t)idle_timeout*1000) { set_error(c->slot,"control_timeout"); close_client(i); continue; }
        if (c->partial_since && now-c->partial_since>10000) { set_error(c->slot,"partial_record_timeout"); close_client(i); continue; }
        if (c->tx_used>c->tx_sent && now>=c->tx_since && now-c->tx_since>10000) { set_error(c->slot,"output_timeout"); close_client(i); continue; }
    }
}
static void event_loop(void) {
    while (!stopping) {
#if MYVAVA_SDK
        myvava_client_dispatch(server_client,sdk_execute,NULL);
        remote_media_tick();
#endif
        timers();
        struct pollfd fds[LISTENER_COUNT+MAX_CLIENTS]; int map[LISTENER_COUNT+MAX_CLIENTS]; uint64_t gens[LISTENER_COUNT+MAX_CLIENTS];
        nfds_t n=0;
        for (int k=0;k<LISTENER_COUNT;++k) { fds[n]=(struct pollfd){listeners[k],POLLIN,0}; map[n]=-1-k; gens[n++]=0; }
        for (int i=0;i<MAX_CLIENTS;++i) if (clients[i].fd>=0) {
            struct client *c=&clients[i]; short events=0;
            bool waiting_media=c->kind==MEDIA && (c->slot<0 || !c->camera_generation || !cameras[c->slot].control_ready);
            if (!c->close_after && c->kind!=ASSET && !waiting_media && !(c->kind==ALIVE && c->slot<0)) events|=POLLIN;
            if (c->tx_used>c->tx_sent) events|=POLLOUT;
            fds[n]=(struct pollfd){c->fd,events,0}; map[n]=i; gens[n++]=c->generation;
        }
        int r=poll(fds,n,100);
        if (r<0) { if (errno==EINTR) continue; perror("poll"); break; }
        /* Process existing connections before accepting into recycled slots. */
        for (nfds_t k=LISTENER_COUNT;k<n;++k) {
            int i=map[k]; struct client *c=&clients[i]; short events=fds[k].revents;
            if (!events || c->fd<0 || c->generation!=gens[k]) continue;
            if (events&POLLIN) {
                uint8_t data[8192]; ssize_t got=recv(c->fd,data,sizeof(data),0);
                if (got>0) { c->last_rx=now_ms(); feed(i,data,(size_t)got); }
                else if (!got || (errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) close_client(i);
            }
            if (c->fd<0) continue;
            if (events&POLLOUT) {
                ssize_t sent=send(c->fd,c->tx+c->tx_sent,c->tx_used-c->tx_sent,0);
                if (sent>0) {
                    c->tx_sent+=(size_t)sent;
                    c->tx_completed+=(uint64_t)sent;
                    if (c->kind==CONTROL && c->slot>=0)
                        settings_written(&cameras[c->slot].settings,c->tx_completed,now_ms());
                    if (c->tx_sent==c->tx_used) {
                        free(c->tx); c->tx=NULL; c->tx_sent=c->tx_used=0;
                        if (c->close_after) close_client(i);
                    }
                } else if (sent<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) close_client(i);
            }
            if (c->fd>=0 && (events&(POLLERR|POLLNVAL))) close_client(i);
            if (c->fd>=0 && (events&POLLHUP) && !(events&POLLIN)) close_client(i);
        }
        for (unsigned k=0;k<LISTENER_COUNT;++k) if (fds[k].revents&POLLIN) accept_clients((int)k);
    }
}
static void usage(void) {
    fprintf(stderr,"vava-stationd [--state-dir DIR] [--bind IPv4] [--ssid VAVA-name]\n"
        "  [--password-file FILE] [--control-port N] [--media-port N] [--alive-port N]\n"
        "  [--pairing-serial 25_ASCII_ALNUM] (original VAPair name/key derivation)\n"
        "  [--archive-serial 25_ASCII_ALNUM] (station/date/camera native IDX hierarchy)\n"
        "  [--rtsp-port N] [--rtsp-bind IPv4] [--rtsp-auth-file FILE] [--neighbor-file FILE]\n"
#if VAVA_HTTP
        "  [--http-port N] [--http-bind IPv4] [--api-token-file FILE]\n"
        "  [--http-origin http(s)://host[:port]] (up to 4) [--no-web-ui]\n"
#endif
#if MYVAVA_SDK
        "  [--server-host HOST --device-id ID --server-key-file FILE]\n"
        "  [--nas-host HOST --nas-export /EXPORT --nas-mount /MOUNT --nas-station-id ID]\n"
        "  [--server-port 3731] [--server-poll-ms 1000] (DTLS/UDP; control and events)\n"
        "  [--record-keyring PRIVATE_KEY_DIR] (AES-256-GCM storage)\n"
        "  [--continuous-quality 0|1] (experimental in-place switching)\n"
        "  [--diagnostic-defaults 1] (new cameras only; disables daily arm/sleep defaults)\n"
        "  Maintenance: --storage-key-create DIR | --storage-key-rotate DIR\n"
        "  Check loaded DTLS/PSK support without starting: --transport-check\n"
        "  [--event-dir EXISTING_PRIVATE_DATA_DIR] [--event-limit 1024]\n"
#endif
        "  [--record-dir DIR] [--storage-mount MOUNT] [--record-max-mb N] [--ap-helper ABSOLUTE_FILE] [--clock-valid]\n"
        "  [--lab-ip-association] [--time-offset-seconds N] [--idle-timeout-seconds N]\n"
        "Default bind: 127.0.0.1; management: DIR/control.sock (0600).\n");
}
static bool options(int argc, char **argv) {
#if VAVA_HTTP
    web_enabled=VAVA_EMBED_WEB!=0;
#endif
    for (int i=1;i<argc;++i) {
        const char *key=argv[i];
        if (!strcmp(key,"--lab-ip-association")) { lab_ip=true; continue; }
        if (!strcmp(key,"--clock-valid")) { clock_valid=true; continue; }
#if VAVA_HTTP
        if (!strcmp(key,"--no-web-ui")) { web_enabled=false; continue; }
#endif
        if (!strcmp(key,"--help")) { usage(); exit(0); }
        if (++i>=argc) return false;
        const char *val=argv[i]; unsigned num;
        if (!strcmp(key,"--state-dir")) state_dir=val;
        else if (!strcmp(key,"--bind")) bind_ip=val;
        else if (!strcmp(key,"--neighbor-file")) neighbor_file=val;
        else if (!strcmp(key,"--rtsp-bind")) rtsp_bind=val;
        else if (!strcmp(key,"--rtsp-auth-file")) rtsp_auth_file=val;
#if VAVA_HTTP
        else if (!strcmp(key,"--api-token-file")) api_token_file=val;
        else if (!strcmp(key,"--http-bind")) http_bind=val;
        else if (!strcmp(key,"--http-origin")) {
            if (http_origin_count==4 || !http_origin_valid(val)) return false;
            http_origins[http_origin_count++]=val;
        }
        else if (!strcmp(key,"--http-port") && parse_number(val,65535,&num)) { http_port=num; http_enabled=true; }
#endif
#if MYVAVA_SDK
        else if (!strcmp(key,"--event-dir")) event_dir=val;
        else if (!strcmp(key,"--record-keyring")) record_keyring=val;
        else if (!strcmp(key,"--event-limit") && parse_number(val,4096,&num) && num>=16) event_limit=num;
        else if (!strcmp(key,"--server-host")) server_config.host=val;
        else if (!strcmp(key,"--device-id")) server_config.device_id=val;
        else if (!strcmp(key,"--server-key-file")) server_config.key_file=val;
        else if (!strcmp(key,"--nas-host")) nas_config.host=val;
        else if (!strcmp(key,"--nas-export")) nas_config.export_path=val;
        else if (!strcmp(key,"--nas-mount")) nas_config.mount_dir=val;
        else if (!strcmp(key,"--nas-station-id")) nas_config.station_id=val;
        else if (!strcmp(key,"--nas-lab-local") && !strcmp(val,"1")) nas_config.lab_local=true;
        else if (!strcmp(key,"--server-port") && parse_number(val,65535,&num) && num) server_config.port=num;
        else if (!strcmp(key,"--server-poll-ms") && parse_number(val,MYVAVA_POLL_MAX,&num) && num>=50) server_config.poll_ms=num;
#endif
        else if (!strcmp(key,"--rtsp-port") && parse_number(val,65535,&num)) { rtsp_port=num; rtsp_enabled=true; }
        else if (!strcmp(key,"--ssid")) ssid=val;
        else if (!strcmp(key,"--pairing-serial")) pairing_serial=val;
        else if (!strcmp(key,"--archive-serial")) archive_serial=val;
        else if (!strcmp(key,"--password-file")) password_file=val;
        else if (!strcmp(key,"--record-dir")) record_dir=val;
        else if (!strcmp(key,"--storage-mount")) storage_mount=val;
        else if (!strcmp(key,"--ap-helper")) ap_helper=val;
        else if (!strcmp(key,"--record-max-mb") && parse_number(val,1048576,&num) && num) record_max_mb=num;
        else if (!strcmp(key,"--control-port") && parse_number(val,65535,&num)) ports[0]=num;
#if MYVAVA_SDK
        else if (!strcmp(key,"--continuous-quality") && parse_number(val,1,&num)) continuous_quality=num!=0;
        else if (!strcmp(key,"--diagnostic-defaults") && parse_number(val,1,&num)) diagnostic_defaults=num!=0;
#endif
        else if (!strcmp(key,"--media-port") && parse_number(val,65535,&num)) ports[1]=num;
        else if (!strcmp(key,"--alive-port") && parse_number(val,65535,&num)) ports[2]=num;
        else if (!strcmp(key,"--idle-timeout-seconds") && parse_number(val,3600,&num) && num>=5) idle_timeout=(int)num;
        else if (!strcmp(key,"--time-offset-seconds")) {
            char *end; errno=0; long v=strtol(val,&end,10);
            if (errno || end==val || *end || v< -86400 || v>86400) return false; time_offset=(int)v;
        } else return false;
    }
    struct in_addr a;
    if (inet_pton(AF_INET,bind_ip,&a)!=1 || inet_pton(AF_INET,rtsp_bind,&a)!=1 ||
        inet_pton(AF_INET,http_bind,&a)!=1 || strlen(ssid)>31 || strncmp(ssid,"VAVA-",5)) return false;
    for (const char *s=ssid;*s;++s) if ((unsigned char)*s<32 || (unsigned char)*s>126) return false;
    if (pairing_serial) {
        if (strlen(pairing_serial)!=25) return false;
        for (const char *s=pairing_serial;*s;++s)
            if (!((*s>='0'&&*s<='9')||(*s>='A'&&*s<='Z')||(*s>='a'&&*s<='z'))) return false;
    }
    if (password_file) {
        FILE *f=fopen(password_file,"rb"); char buffer[64];
        if (!f) return false;
        size_t n=fread(buffer,1,sizeof(buffer),f); bool failed=ferror(f); fclose(f);
        if (failed || n==sizeof(buffer)) return false;
        while (n && (buffer[n-1]=='\n' || buffer[n-1]=='\r')) --n;
        if (n<8 || n>31) return false;
        for (size_t k=0;k<n;++k) if ((unsigned char)buffer[k]<32 || (unsigned char)buffer[k]>126) return false;
        memcpy(password,buffer,n);
    }
    if (rtsp_auth_file) {
        char credentials[160];
        if (!auth_read(rtsp_auth_file,credentials,sizeof(credentials),12) || !strchr(credentials,':')) return false;
        strcpy(rtsp_authorization,"Basic "); auth_base64(rtsp_authorization+6,credentials);
        memset(credentials,0,sizeof(credentials));
    }
#if VAVA_HTTP
    if (http_enabled) {
        char token[160];
        if (!api_token_file || !auth_read(api_token_file,token,sizeof(token),32)) return false;
        snprintf(api_authorization,sizeof(api_authorization),"Bearer %s",token); memset(token,0,sizeof(token));
    }
#endif
#if MYVAVA_SDK
    if ((server_config.host || server_config.device_id || server_config.key_file) &&
        !(server_config.host && server_config.device_id && server_config.key_file)) return false;
#endif
    return platform_init(ap_helper);
}
static bool setup(void) {
    if (mkdir(state_dir,0700) && errno!=EEXIST) return false;
    struct stat st;
    if (lstat(state_dir,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) {
        fprintf(stderr,"state directory must be owned by this user and mode 0700\n"); return false;
    }
    if (snprintf(socket_path,sizeof(socket_path),"%s/control.sock",state_dir)>=(int)sizeof(socket_path)) return false;
    char lockpath[1024];
    if (snprintf(lockpath,sizeof(lockpath),"%s/daemon.lock",state_dir)>=(int)sizeof(lockpath)) return false;
    lock_fd=open(lockpath,O_RDWR|O_CREAT|O_CLOEXEC,0600);
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0,.l_len=0};
    if (lock_fd<0 || fcntl(lock_fd,F_SETLK,&lock)) { fprintf(stderr,"state directory already in use\n"); return false; }
    if (registry_load(state_dir,registry)) { fprintf(stderr,"invalid registry; refusing to overwrite it\n"); return false; }
#if MYVAVA_SDK
    if(!removal_init(state_dir)){fprintf(stderr,"camera removal state unavailable or corrupt\n");return false;}
    if(!pairing_init(state_dir,registry)){fprintf(stderr,"pairing state unavailable or corrupt; refusing to overwrite it\n");return false;}
#endif
    if (policy_load_mode(state_dir,registry,policies,
#if MYVAVA_SDK
        !diagnostic_defaults
#else
        false
#endif
    )) { fprintf(stderr,"invalid policy store; refusing to overwrite it\n"); return false; }
    if (desired_load(state_dir,registry,preferences)) { fprintf(stderr,"invalid settings store; refusing to overwrite it\n"); return false; }
    if (profile_load(state_dir,registry,profiles)) { fprintf(stderr,"invalid profiles store; refusing to overwrite it\n"); return false; }
    if (!record_dir) {
        if (snprintf(default_record_dir,sizeof(default_record_dir),"%s/recordings",state_dir)>=(int)sizeof(default_record_dir)) return false;
        record_dir=default_record_dir;
    }
#if MYVAVA_SDK
    if (event_dir && !(event_spool=myvava_events_start_guarded(event_dir,event_limit,storage_mount))) {
        fprintf(stderr,"event worker initialization failed\n"); return false;
    }
    if(!recorder_retention_init(state_dir,!diagnostic_defaults))return false;
    recorder_clock(clock_valid);
    if(record_keyring&&!recorder_keys(record_keyring)){fprintf(stderr,"private recording keyring unavailable; encrypted recordings preserved\n");return false;}
    if(event_spool){myvava_events_on_durable(event_spool,recording_event_saved,NULL);recorder_on_complete(recording_complete,event_spool);}
#endif
    if(!record_archive_config(archive_serial,time_offset))return false;
    if (!(
#if MYVAVA_SDK
        recorder_init_guarded(record_dir,(uint64_t)record_max_mb*1024*1024,storage_mount)
#else
        recorder_init(record_dir,(uint64_t)record_max_mb*1024*1024)
#endif
    )) { fprintf(stderr,"recording store initialization failed\n"); return false; }
#if MYVAVA_SDK
    nas_config.executable=executable_path;
    if(!nas_config.mount_dir)nas_config.mount_dir="/mnt/vava-nas";
    if(!nas_config.station_id)nas_config.station_id=server_config.device_id?server_config.device_id:"local";
    nas=nas_start(record_dir,state_dir,&nas_config);
    if(!nas){fprintf(stderr,"NAS configuration unavailable or corrupt\n");return false;}
    for(unsigned s=0;s<4;++s)nas_policy(nas,s,registry[s].serial,policies[s].nas_backup);
    if (server_config.host) {
        uploader=upload_start(record_dir,&server_config);
        if (!uploader) { fprintf(stderr,"upload worker initialization failed\n"); return false; }
        server_config.files=upload_channel(uploader);
        for (unsigned s=0;s<4;++s) upload_policy(uploader,s,registry[s].serial,policies[s].cloud_upload);
    }
    storage_worker=storage_io_start(uploader,nas);if(!storage_worker){fprintf(stderr,"storage command worker initialization failed\n");return false;}
#endif
    for (int i=0;i<3;++i) if ((listeners[i]=tcp_listener(bind_ip,&ports[i]))<0) return false;
    if (rtsp_enabled && (listeners[RTSP]=tcp_listener(rtsp_bind,&rtsp_port))<0) return false;
    if (http_enabled && (listeners[HTTP]=tcp_listener(http_bind,&http_port))<0) return false;
    udp_fd=socket(AF_INET,SOCK_DGRAM,0);
    struct sockaddr_in local={.sin_family=AF_INET}; inet_pton(AF_INET,bind_ip,&local.sin_addr);
    if (udp_fd<0 || !nonblock(udp_fd) || bind(udp_fd,(struct sockaddr *)&local,sizeof(local))) return false;
    struct sockaddr_un a={.sun_family=AF_UNIX}; memcpy(a.sun_path,socket_path,strlen(socket_path)+1);
    if (!lstat(socket_path,&st)) {
        if (!S_ISSOCK(st.st_mode) || st.st_uid!=geteuid() || unlink(socket_path)) return false;
    } else if (errno!=ENOENT) return false;
    listeners[3]=socket(AF_UNIX,SOCK_STREAM,0);
    if (listeners[3]<0 || !nonblock(listeners[3]) || bind(listeners[3],(struct sockaddr *)&a,sizeof(a)) ||
        chmod(socket_path,0600) || listen(listeners[3],8)) return false;
    return true;
}
int main(int argc, char **argv) {
#if MYVAVA_SDK
    if (argc==2 && !strcmp(argv[1],"--transport-check")) {
        const char *error=myvava_transport_error();
        if (error) printf("{\"ok\":false,\"error\":\"%s\"}\n",error);
        else puts("{\"ok\":true,\"dtls_version\":\"1.2\",\"psk\":true}");
        return error?1:0;
    }
    if(argc==3&&(!strcmp(argv[1],"--storage-key-create")||!strcmp(argv[1],"--storage-key-rotate"))){
        char id[33];umask(0077);bool ok=seal_keys_generate(argv[2],!strcmp(argv[1],"--storage-key-rotate"),id);
        if(ok)printf("{\"ok\":true,\"active_key_id\":\"%s\",\"restart_required\":true}\n",id);
        else fprintf(stderr,"keyring change failed; existing keys were not removed\n");return ok?0:1;
    }
    if (argc>1 && !strcmp(argv[1],"--nas-admin-worker")) return nas_admin_main(argc,argv);
    if (argc>1 && !strcmp(argv[1],"--nas-copy-worker")) return nas_copy_main(argc,argv);
    char *resolved=realpath(argv[0],NULL);
    if (resolved) {
        if (strlen(resolved)<sizeof(executable_path)) strcpy(executable_path,resolved);
        free(resolved);
    }
#ifdef __linux__
    if (!executable_path[0]) {
        ssize_t n=readlink("/proc/self/exe",executable_path,sizeof(executable_path)-1);
        if (n>0) executable_path[n]=0;
    }
#endif
#endif
    umask(0077);
    started_at=now_ms();
    for (int i=0;i<MAX_CLIENTS;++i) { clients[i].fd=-1; clients[i].slot=-1; }
    for (int s=0;s<4;++s) for (int k=0;k<3;++k) cameras[s].link[k]=-1;
    if (!options(argc,argv)) { usage(); return 2; }
    if (!process_signal_install(SIGPIPE,SIG_IGN) || !process_signal_install(SIGINT,signal_stop) ||
        !process_signal_install(SIGTERM,signal_stop)) {
        perror("sigaction"); return 1;
    }
    if (!setup()) { perror("setup"); return 1; }
#if MYVAVA_SDK
    emit_event(MVE_BOOT,-1,0,0,0,"process_started");
    server_config.events=event_spool;
    if(server_config.host){remote_media=mvm_client_new();if(!remote_media)return 1;server_config.media=remote_media;}
    if (server_config.host && !(server_client=myvava_client_start(&server_config))) {
        fprintf(stderr,"invalid SDK configuration or private key; refusing remote startup\n"); return 1;
    }
#endif
    platform_pair_close(now_ms());
    fprintf(stderr,"vava-stationd ready; bind=%s ports=%u/%u/%u; hardware unverified\n",
        bind_ip,ports[0],ports[1],ports[2]);
    event_loop();
#if MYVAVA_SDK
    myvava_client_stop(server_client);
    mvm_client_free(remote_media);
    storage_io_stop(storage_worker);storage_worker=NULL;
    upload_stop(uploader);
    nas_stop(nas);
#endif
    for (int i=0;i<MAX_CLIENTS;++i) close_client(i);
    recorder_shutdown();
#if MYVAVA_SDK
    myvava_events_stop(event_spool);
#endif
    platform_shutdown(now_ms());
    for (int i=0;i<LISTENER_COUNT;++i) if (listeners[i]>=0) close(listeners[i]);
    close(udp_fd); unlink(socket_path); close(lock_fd);
    return 0;
}
