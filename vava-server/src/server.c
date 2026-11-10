#include "myvava.h"
#include "task_store.h"
#include "file_store.h"
#include "server_web.h"
#include "media_store.h"
#include "sealed_file.h"
#include "legacy_import.h"
#include "process_signal.h"
#include <coap3/coap.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define DEVICE_LIMIT 16
#define LOCAL_LIMIT 8
#define LOCAL_OUTPUT (MYVAVA_RESULT_MAX*6+4096)
static const char *state_names[]={"queued","dispatched","completed","expired","unknown"};
struct device {
    char name[MYVAVA_ID_MAX+1];
    uint8_t key[32], boot[16], retired[4][16];
    unsigned retired_count;
    bool seen;
    uint64_t last_seen;
    coap_bin_const_t psk;
    coap_session_t *session;
};
struct local_client {
    int fd; uint64_t since;
    char input[1024]; size_t used, sent, length;
    char *output;
};
struct transport { coap_session_t *session; uint64_t active_at; };
/* Borrowed pointers are removed synchronously by SERVER_SESSION_DEL. The
 * configured idle/handshake caps are below this fixed tracking capacity. */
static struct transport transports[64];
static struct device devices[DEVICE_LIMIT];
static struct local_client locals[LOCAL_LIMIT];
static unsigned device_count, history_limit=1024, event_limit=8192, request_window_ms=60000;
static struct task_store *journal;
static struct file_store *file_storage;
static struct mvm_store *media_storage;
static struct pending_file {
    uint64_t cookie, since; unsigned device;
    coap_session_t *session; coap_async_t *async;
    bool done, batch; struct myvava_message result; uint8_t data[33];
} pending_files[FILE_STORE_JOBS];
/* One bounded Block1 assembly per authenticated device; transport ACKs only
 * acknowledge buffered bytes. Durable offsets still come from file_store. */
static struct file_body {
    coap_session_t *session;
    size_t used,total,tag_size;
    uint8_t tag[8],data[MYVAVA_FILE_PACKET_MAX];
    uint64_t since;
} file_bodies[DEVICE_LIMIT];
static struct control_body {
    coap_session_t *session;
    size_t used,total,tag_size;
    uint8_t tag[8],data[MYVAVA_HEADER+MYVAVA_CONTROL_BATCH_MAX];
    uint64_t since;
} control_bodies[DEVICE_LIMIT];
static uint64_t file_sequence;

static char request_prefix[17], previous_prefix[17];
static uint64_t prefix_since, previous_until;
static uint8_t epoch[16];
static volatile sig_atomic_t stopping, reload_requested;
static void reload_signal(int sig) { (void)sig; reload_requested=1; }
static void stop_signal(int sig) { (void)sig; stopping=1; }
static bool nonblock(int fd) {
    int flags=fcntl(fd,F_GETFL,0);
    return flags>=0 && !fcntl(fd,F_SETFL,flags|O_NONBLOCK) && !fcntl(fd,F_SETFD,FD_CLOEXEC);
}
static bool read_keys(const char *path,struct device *output,unsigned *count) {
    *count=0;
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK); struct stat st;
    if (fd<0) return false;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077) || st.st_nlink!=1 || st.st_size>4096) {
        close(fd); return false;
    }
    FILE *file=fdopen(fd,"r");
    if (!file) { close(fd); return false; }
    bool ok=true; char line[160];
    while (fgets(line,sizeof(line),file)) {
        size_t n=strlen(line); if (n && line[n-1]=='\n') line[--n]=0;
        char *space=strchr(line,' ');
        if ((*count)==DEVICE_LIMIT || !space) { ok=false; break; }
        *space++=0;
        struct device *d=&output[(*count)];
        if (!myvava_id_valid(line) || !myvava_unhex(space,d->key,32)) { ok=false; break; }
        for (unsigned i=0;i<(*count);++i)
            if (!strcmp(output[i].name,line) || !memcmp(output[i].key,d->key,32)) ok=false;
        if (!ok) break;
        strcpy(d->name,line); d->psk=(coap_bin_const_t){32,d->key}; ++(*count);
    }
    if (ferror(file)) ok=false;
    memset(line,0,sizeof(line)); fclose(file); return ok;
}
static int device_index(const coap_bin_const_t *identity) {
    if (!identity) return -1;
    for (unsigned i=0;i<device_count;++i)
        if (identity->length==strlen(devices[i].name) && !memcmp(identity->s,devices[i].name,identity->length)) return (int)i;
    return -1;
}
static const coap_bin_const_t *verify_identity(coap_bin_const_t *identity,
                                              coap_session_t *session, void *arg) {
    (void)session; (void)arg;
    int index=device_index(identity);
    return index<0?NULL:&devices[index].psk;
}
static void reply_packet(coap_pdu_t *response, const struct myvava_message *message) {
    uint8_t packet[MYVAVA_PACKET_MAX]; size_t n=myvava_encode(packet,message);
    coap_pdu_set_code(response,COAP_RESPONSE_CODE_CONTENT);
    if (!n || !coap_add_data(response,n,packet)) coap_pdu_set_code(response,COAP_RESPONSE_CODE_INTERNAL_ERROR);
}
static void file_request(coap_resource_t *resource, coap_session_t *session,
                         const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response) {
    (void)resource; (void)query;
    coap_async_t *async=coap_find_async(session,coap_pdu_get_token(request));
    if (async) {
        struct pending_file *p=coap_async_get_app_data(async);
        if (!p || !p->done) return;
        struct device *d=&devices[p->device];
        if (d->session==session && !memcmp(d->boot,p->result.boot,16)) {
            const uint8_t segments=MYVAVA_FILE_BATCH_SEGMENTS;
            if(p->batch)coap_add_option(response,MYVAVA_FILE_BATCH_OPTION,1,&segments);
            reply_packet(response,&p->result);
        }
        else coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
        memset(p,0,sizeof(*p)); return; /* libcoap frees async and its session reference */
    }
    coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST);
    int index=device_index(coap_session_get_psk_identity(session));
    if (index<0 || coap_session_get_proto(session)!=COAP_PROTO_DTLS) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_UNAUTHORIZED); return;
    }
    struct device *d=&devices[index];
    if(!file_storage||d->session!=session||!d->seen){coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);return;}
    size_t size=0,offset=0,total=0; const uint8_t *data=NULL; struct myvava_message m;
    if(!coap_get_data_large(request,&size,&data,&offset,&total)||!size||
        total>MYVAVA_FILE_PACKET_MAX||offset>total||size>total-offset)return;
    if(offset||total!=size){
        coap_opt_iterator_t oi;coap_opt_t *tag=coap_check_option(request,COAP_OPTION_RTAG,&oi);
        if(!tag||!coap_opt_length(tag)||coap_opt_length(tag)>8)return;
        struct file_body *body=&file_bodies[index];uint64_t now=myvava_millis();
        if(!offset){
            /* Validate the session generation before accepting any large body. */
            if(size<MYVAVA_HEADER||memcmp(data,"MVA1",4)||data[54]!=MYVAVA_FILE_DATA||data[55]||
                memcmp(data+4,d->boot,16)||memcmp(data+20,epoch,16)||
                (size_t)(data[52]*256u+data[53])+MYVAVA_HEADER!=total)return;
            body->session=session;body->used=0;body->total=total;body->since=now;
            body->tag_size=coap_opt_length(tag);memcpy(body->tag,coap_opt_value(tag),body->tag_size);
        }
        if(body->session!=session||now-body->since>20000||total!=body->total||
            body->tag_size!=coap_opt_length(tag)||memcmp(body->tag,coap_opt_value(tag),body->tag_size)||
            offset>body->used){coap_pdu_set_code(response,COAP_RESPONSE_CODE_INCOMPLETE);return;}
        if(offset<body->used){
            if(size>body->used-offset||memcmp(body->data+offset,data,size))return;
        }else {memcpy(body->data+offset,data,size);body->used+=size;}
        if(body->used<total){coap_pdu_set_code(response,COAP_RESPONSE_CODE_CONTINUE);return;}
        size=total;data=body->data;
    }
    if (!myvava_decode(data,size,&m) ||
        m.type<MYVAVA_FILE_OFFER || m.type>MYVAVA_FILE_COMMIT || !m.id || !m.total ||
        m.total>MYVAVA_FILE_MAX || m.offset>m.total ||
        (m.type==MYVAVA_FILE_OFFER?m.length!=MYVAVA_FILE_DESC:m.length<32)) return;
    if (memcmp(d->boot,m.boot,16) || memcmp(epoch,m.epoch,16)) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
    }
    struct pending_file *p=NULL;
    for (unsigned i=0;i<FILE_STORE_JOBS;++i) {
        if (pending_files[i].cookie && pending_files[i].device==(unsigned)index) {
            coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
        }
        if (!pending_files[i].cookie) p=&pending_files[i];
    }
    if (!p || file_sequence==UINT64_MAX || !(async=coap_register_async(session,request,0))) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
    }
    *p=(struct pending_file){.cookie=++file_sequence,.since=myvava_millis(),.device=(unsigned)index,.session=session,.async=async};
    coap_opt_iterator_t oi;coap_opt_t *option=coap_check_option(request,MYVAVA_FILE_BATCH_OPTION,&oi);
    p->batch=m.type==MYVAVA_FILE_OFFER&&option&&coap_opt_length(option)==1&&
        coap_opt_value(option)[0]==MYVAVA_FILE_BATCH_SEGMENTS;
    coap_async_set_app_data(async,p);
    if (!file_store_submit(file_storage,p->cookie,d->name,&m)) {
        coap_free_async(session,async); memset(p,0,sizeof(*p));
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
    }
    coap_pdu_set_code(response,0); /* empty ACK; storage worker supplies separate response */
}
static void file_tick(void) {
    uint64_t cookie; struct myvava_message m; uint8_t data[33];
    while (file_store_poll(file_storage,&cookie,&m,data)) {
        for (unsigned i=0;i<FILE_STORE_JOBS;++i) if (pending_files[i].cookie==cookie) {
            struct pending_file *p=&pending_files[i]; p->result=m; memcpy(p->data,data,33); p->result.data=p->data;
            p->done=true; coap_async_trigger(p->async); break;
        }
    }
    uint64_t now=myvava_millis();
    for (unsigned i=0;i<FILE_STORE_JOBS;++i) {
        struct pending_file *p=&pending_files[i];
        if (p->cookie && now-p->since>10000) {
            coap_free_async(p->session,p->async); memset(p,0,sizeof(*p));
        }
    }
}
static unsigned file_io_wait(void) {
    /* Storage completes on a worker thread; collect its durable ACK promptly
     * while a transfer is active, retaining the normal idle polling budget. */
    for(unsigned i=0;i<FILE_STORE_JOBS;++i)if(pending_files[i].cookie)return 1;
    return 10;
}
static void media_request(coap_resource_t *resource,coap_session_t *session,const coap_pdu_t *request,const coap_string_t *query,coap_pdu_t *response) {
    (void)resource;(void)query;coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST);
    int index=device_index(coap_session_get_psk_identity(session));
    if(index<0||coap_session_get_proto(session)!=COAP_PROTO_DTLS){coap_pdu_set_code(response,COAP_RESPONSE_CODE_UNAUTHORIZED);return;}
    size_t size=0;const uint8_t *data=NULL;struct myvava_message m;
    if(!coap_get_data(request,&size,&data)||!myvava_decode(data,size,&m))return;
    struct device *d=&devices[index];
    if(d->session!=session||!d->seen||memcmp(d->boot,m.boot,16)||memcmp(epoch,m.epoch,16)){coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);return;}
    if(m.type==MYVAVA_MEDIA_FRAME){if(mvm_store_frame(media_storage,d->name,&m))coap_pdu_set_code(response,COAP_RESPONSE_CODE_CHANGED);return;}
    if(m.type!=MYVAVA_MEDIA_POLL||m.length||m.total)return;
    uint8_t payload[MYVAVA_CHUNK];struct myvava_message reply={.type=MYVAVA_MEDIA_CONTROL,.data=payload};
    coap_opt_iterator_t oi;coap_opt_t *cap=coap_check_option(request,MVM_FRAMING_OPTION,&oi);
    bool v2=cap&&coap_opt_length(cap)==1&&coap_opt_value(cap)[0]==MVM_FRAMING_VERSION;
    reply.length=mvm_store_control_version(media_storage,d->name,&m,payload,v2);if(v2){uint8_t version=2;coap_add_option(response,MVM_FRAMING_OPTION,1,&version);}memcpy(reply.boot,m.boot,16);memcpy(reply.epoch,epoch,16);reply_packet(response,&reply);
}
static void event_request(coap_resource_t *resource, coap_session_t *session,
                          const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response) {
    (void)resource; (void)query;
    coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST);
    int index=device_index(coap_session_get_psk_identity(session));
    if (index<0 || coap_session_get_proto(session)!=COAP_PROTO_DTLS) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_UNAUTHORIZED); return;
    }
    size_t size=0; const uint8_t *data=NULL; struct myvava_message m; struct myvava_event e;
    if (!coap_get_data(request,&size,&data) || !myvava_decode(data,size,&m) || m.type!=MYVAVA_EVENT ||
        m.total!=MYVAVA_EVENT_SIZE || m.offset || !myvava_event_decode(m.data,m.length,&e) || m.id!=e.sequence) return;
    struct device *d=&devices[index];
    if (!d->seen || memcmp(d->boot,m.boot,16) || memcmp(epoch,m.epoch,16)) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
    }
    if (event_store_put(journal,d->name,m.data,event_limit)!=1) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
    }
    struct myvava_message reply={.type=MYVAVA_EVENT_ACK,.id=e.sequence,.total=MYVAVA_EVENT_SIZE,.length=16,.data=e.origin};
    memcpy(reply.boot,m.boot,16); memcpy(reply.epoch,epoch,16); reply_packet(response,&reply);
}
static bool update_prefix(uint64_t now) {
    if (now>=previous_until) previous_prefix[0]=0;
    if (*request_prefix && now-prefix_since<request_window_ms) return true;
    uint8_t random[8];
    if (!myvava_random(random,sizeof(random))) return false;
    if (*request_prefix && now-prefix_since<2*(uint64_t)request_window_ms) {
        strcpy(previous_prefix,request_prefix); previous_until=prefix_since+2*(uint64_t)request_window_ms;
    }
    else previous_prefix[0]=0;
    myvava_hex(random,sizeof(random),request_prefix); prefix_since=now;
    return true;
}
static void sync_request(coap_resource_t *resource, coap_session_t *session,
                         const coap_pdu_t *request, const coap_string_t *query, coap_pdu_t *response) {
    (void)resource; (void)query;
    coap_pdu_set_code(response,COAP_RESPONSE_CODE_BAD_REQUEST);
    int index=device_index(coap_session_get_psk_identity(session));
    if (index<0 || coap_session_get_proto(session)!=COAP_PROTO_DTLS) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_UNAUTHORIZED); return;
    }
    coap_opt_iterator_t control_oi;coap_opt_t *control_option=coap_check_option(request,MYVAVA_CONTROL_BATCH_OPTION,&control_oi);
    bool control_batch=control_option&&coap_opt_length(control_option)==1&&coap_opt_value(control_option)[0]==MYVAVA_CONTROL_BATCH_CHUNKS;
    size_t size=0,offset=0,total=0;const uint8_t *data=NULL;struct myvava_message m;
    struct device *d=&devices[index];
    if(!coap_get_data_large(request,&size,&data,&offset,&total)||!size||
        total>MYVAVA_HEADER+(control_batch?MYVAVA_CONTROL_BATCH_MAX:MYVAVA_CHUNK)||offset>total||size>total-offset)return;
    if(offset||size!=total){
        coap_opt_iterator_t oi;coap_opt_t *tag=coap_check_option(request,COAP_OPTION_RTAG,&oi);
        if(!control_batch||!tag||!coap_opt_length(tag)||coap_opt_length(tag)>8)return;
        struct control_body *body=&control_bodies[index];uint64_t now=myvava_millis();
        if(!offset){
            if(!d->seen||d->session!=session||size<MYVAVA_HEADER||memcmp(data,"MVA1",4)||
                data[54]!=MYVAVA_SYNC||data[55]||memcmp(data+4,d->boot,16)||memcmp(data+20,epoch,16)||
                (size_t)(data[52]*256u+data[53])+MYVAVA_HEADER!=total)return;
            body->session=session;body->used=0;body->total=total;body->since=now;
            body->tag_size=coap_opt_length(tag);memcpy(body->tag,coap_opt_value(tag),body->tag_size);
        }
        if(body->session!=session||now-body->since>60000||total!=body->total||body->tag_size!=coap_opt_length(tag)||
            memcmp(body->tag,coap_opt_value(tag),body->tag_size)||offset>body->used){coap_pdu_set_code(response,COAP_RESPONSE_CODE_INCOMPLETE);return;}
        if(offset<body->used){if(size>body->used-offset||memcmp(body->data+offset,data,size))return;}
        else {memcpy(body->data+offset,data,size);body->used+=size;}
        if(d->session!=session||memcmp(body->data+4,d->boot,16)||memcmp(body->data+20,epoch,16))return;
        d->last_seen=now;
        for(unsigned i=0;i<64;++i)if(transports[i].session==session)transports[i].active_at=now;
        if(body->used<total){coap_pdu_set_code(response,COAP_RESPONSE_CODE_CONTINUE);return;}
        size=total;data=body->data;
    }
    if (!myvava_decode(data,size,&m) || m.type!=MYVAVA_SYNC ||
        m.total>=MYVAVA_RESULT_MAX || m.offset>m.total || m.length>m.total-m.offset ||
        (!m.id && (m.total || m.offset || m.length))) return;
    if(m.length>MYVAVA_CHUNK&&!control_batch)return;
    uint64_t now=myvava_millis();
    if (d->seen && memcmp(d->boot,m.boot,16)) {
        for (unsigned i=0;i<d->retired_count;++i) if (!memcmp(d->retired[i],m.boot,16)) return;
    }
    if (!task_new_boot(journal,epoch,d->name,m.boot) || !task_expire(journal,epoch,now)) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
    }
    if (d->seen && memcmp(d->boot,m.boot,16)) {
        if (d->retired_count==4) memmove(d->retired,d->retired+1,3*16);
        else ++d->retired_count;
        memcpy(d->retired[d->retired_count-1],d->boot,16);
    }
    if (d->session!=session) {
        if (d->session) {
            coap_session_disconnected(d->session,COAP_NACK_NOT_DELIVERABLE);
        }
        d->session=session;
    }
    for (unsigned i=0;i<64;++i) if (transports[i].session==session) transports[i].active_at=now;
    memcpy(d->boot,m.boot,16); d->seen=true; d->last_seen=now;
    mvm_store_device(media_storage,d->name,d->boot);
    struct myvava_message out={.type=MYVAVA_IDLE};
    memcpy(out.boot,m.boot,16); memcpy(out.epoch,epoch,16);
    const uint8_t control_chunks=MYVAVA_CONTROL_BATCH_CHUNKS;
    if(control_batch&&!coap_add_option(response,MYVAVA_CONTROL_BATCH_OPTION,1,&control_chunks))return;
    if (memcmp(m.epoch,epoch,16)) { reply_packet(response,&out); return; }
    if (m.length) {
        struct task t={0};
        int found=task_get(journal,epoch,m.id,&t);
        if (found<0) coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE);
        if (found!=1 || strcmp(t.device,d->name) || memcmp(t.boot,m.boot,16) ||
            (t.state!=SENT && t.state!=COMPLETE && t.state!=UNKNOWN) || !m.total) { task_clear(&t); return; }
        if (!t.result_size) {
            if (m.offset) { task_clear(&t); return; }
            free(t.result); t.result=calloc(1,(size_t)m.total+1);
            if (!t.result) { coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); task_clear(&t); return; }
            t.result_size=m.total;
        }
        if (m.total!=t.result_size || m.offset>t.received) { task_clear(&t); return; }
        if (m.offset<t.received) {
            if (m.length>t.received-m.offset || memcmp(t.result+m.offset,m.data,m.length)) { task_clear(&t); return; }
        } else {
            memcpy(t.result+m.offset,m.data,m.length); t.received+=(uint32_t)m.length;
            if (!task_save_result(journal,&t)) {
                coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); task_clear(&t); return;
            }
        }
        out.type=MYVAVA_ACK; out.id=m.id; out.total=t.result_size; out.offset=t.received;
        task_clear(&t); reply_packet(response,&out); return;
    }
    if (m.offset==m.total && !task_confirm_result(journal,epoch,d->name,m.boot,m.id,m.total)) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); return;
    }
    struct task t={0}; int found=task_pending(journal,epoch,d->name,&t);
    if (found<0 || (found && !task_dispatch(journal,&t))) {
        coap_pdu_set_code(response,COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE); task_clear(&t); return;
    }
    if (found) {
        out.type=MYVAVA_COMMAND; out.id=t.id;
        out.total=(uint32_t)(t.expires-now); out.data=(const uint8_t *)t.command; out.length=strlen(t.command);
    }
    reply_packet(response,&out); task_clear(&t);
}
static size_t json_string(char *out, const char *text, size_t size) {
    static const char hex[]="0123456789abcdef"; size_t n=0; out[n++]='"';
    for (size_t i=0;i<size;++i) {
        unsigned char c=(unsigned char)text[i];
        if (c=='"' || c=='\\') { out[n++]='\\'; out[n++]=(char)c; }
        else if (c<32) {
            memcpy(out+n,"\\u00",4); n+=4; out[n++]=hex[c>>4]; out[n++]=hex[c&15];
        } else out[n++]=(char)c;
    }
    out[n++]='"'; out[n]=0; return n;
}
static size_t error_json(char *out, const char *error) {
    return (size_t)snprintf(out,LOCAL_OUTPUT,"{\"ok\":false,\"error\":\"%s\"}\n",error);
}
static bool parse_task_id(const char *text, uint64_t *id) {
    uint8_t raw[8]; if (!myvava_unhex(text,raw,8)) return false;
    *id=0; for (unsigned i=0;i<8;++i) *id=*id*256+raw[i];
    return *id<=INT64_MAX;
}
static size_t task_json(char *out, const struct task *t, bool with_result) {
    char task_epoch[33], boot[33]; myvava_hex(t->epoch,16,task_epoch); myvava_hex(t->boot,16,boot);
    size_t n=(size_t)snprintf(out,LOCAL_OUTPUT,"{\"ok\":true,\"durable\":true,\"epoch\":\"%s\",\"id\":\"%016" PRIx64
        "\",\"device\":\"%s\",\"boot\":\"%s\",\"request_key\":\"%s\",\"state\":\"%s\",\"created_at\":%" PRIu64 ",\"reason\":",
        task_epoch,t->id,t->device,boot,t->key,state_names[t->state],t->created_at);
    n+=json_string(out+n,t->reason,strlen(t->reason));
    memcpy(out+n,",\"command\":",11); n+=11;
    n+=json_string(out+n,t->command,strlen(t->command));
    if (with_result) {
        memcpy(out+n,",\"result\":",10); n+=10;
        if (t->state==COMPLETE) n+=json_string(out+n,(const char *)t->result,t->result_size);
        else { memcpy(out+n,"null",4); n+=4; }
    }
    out[n++]='}'; out[n]=0; return n;
}
static size_t submission_json(char *out, const struct task *t, bool duplicate) {
    char task_epoch[33]; myvava_hex(t->epoch,16,task_epoch);
    return (size_t)snprintf(out,LOCAL_OUTPUT,"{\"ok\":true,\"durable\":true,\"epoch\":\"%s\",\"id\":\"%016" PRIx64
        "\",\"duplicate\":%s}\n",task_epoch,t->id,duplicate?"true":"false");
}
static size_t local_command(char *line, char *out) {
    uint64_t now=myvava_millis();
    if (!update_prefix(now) || !task_expire(journal,epoch,now)) return error_json(out,"storage_unavailable");
    char epoch_text[33]; myvava_hex(epoch,16,epoch_text);
    if (!strncmp(line,"files ",6)) {
        char device[MYVAVA_ID_MAX+1], tail; unsigned page;
        if (sscanf(line+6,"%63s %u %c",device,&page,&tail)!=2 || !myvava_id_valid(device) || page>4096)
            return error_json(out,"files_requires_device_page");
        if (!file_store_list(file_storage,device,page,out,LOCAL_OUTPUT)) return error_json(out,"file_storage_unavailable");
        return strlen(out);
    }
    if (!strcmp(line,"devices")) {
        uint64_t count=0,pruned=0,event_count=0,event_pruned=0,streams=0;
        if (!task_stats(journal,&count,&pruned) || !event_store_stats(journal,&event_count,&event_pruned,&streams)) return error_json(out,"storage_unavailable");
        size_t n=(size_t)snprintf(out,LOCAL_OUTPUT,"{\"ok\":true,\"protocol\":\"myvava-control/1\",\"ipc_version\":3,\"transport\":\"coap-dtls-psk\","
            "\"epoch\":\"%s\",\"request_prefix\":\"%s\",\"request_valid_for_ms\":%" PRIu64 ",\"channels\":[\"control\",\"events\",\"files\",\"media\"],\"file_storage\":%s,"
            "\"journal\":{\"durable\":true,\"retained\":%" PRIu64 ",\"limit\":%u,\"pruned\":%" PRIu64 "},"
            "\"event_journal\":{\"retained\":%" PRIu64 ",\"limit\":%u,\"pruned\":%" PRIu64 ","
            "\"origins\":%" PRIu64 ",\"origin_limit\":65536},\"devices\":[",
            epoch_text,request_prefix,2*(uint64_t)request_window_ms-(now-prefix_since),file_storage?"true":"false",count,history_limit,pruned,event_count,event_limit,event_pruned,streams);
        for (unsigned i=0;i<device_count;++i) {
            struct device *d=&devices[i]; char boot[33]; myvava_hex(d->boot,16,boot);
            n+=(size_t)snprintf(out+n,LOCAL_OUTPUT-n,"%s{\"id\":\"%s\",\"boot\":\"%s\",\"online\":%s,\"last_seen_ms\":%" PRIu64 "}",
                i?",":"",d->name,boot,d->seen && now-d->last_seen<15000?"true":"false",d->seen?now-d->last_seen:UINT64_MAX);
        }
        n+=(size_t)snprintf(out+n,LOCAL_OUTPUT-n,"]}\n"); return n;
    }
    if (!strncmp(line,"event-latest ",13)) {
        struct stored_event entry; const char *device=line+13;
        if (!myvava_id_valid(device)) return error_json(out,"invalid_device");
        int count=event_store_history(journal,device,0,&entry,1);
        if (count<0) return error_json(out,"storage_unavailable");
        return (size_t)snprintf(out,LOCAL_OUTPUT,"{\"ok\":true,\"id\":\"%016" PRIx64 "\"}\n",count?entry.id:0);
    }
    if (!strncmp(line,"events ",7)) {
        char *device=line+7,*value=strchr(device,' '); uint64_t before;
        if (!value) return error_json(out,"events_requires_device_before");
        *value++=0;char *category=strchr(value,' '),*serial=NULL;
        if(category){*category++=0;serial=strchr(category,' ');if(!serial)return error_json(out,"invalid_event_query");*serial++=0;}
        if(!myvava_id_valid(device)||!parse_task_id(value,&before))return error_json(out,"invalid_event_query");
        if(category&&!strcmp(category,"-"))category="";if(serial&&!strcmp(serial,"-"))serial="";
        if(category&&*category&&strcmp(category,"detection")&&strcmp(category,"recording")&&strcmp(category,"pairing")&&strcmp(category,"storage")&&strcmp(category,"system"))return error_json(out,"invalid_event_category");
        if(serial&&strlen(serial)>=32)return error_json(out,"invalid_event_serial");
        struct stored_event entries[32];int count=event_store_filtered(journal,device,before,category?category:"",serial?serial:"",entries,32);
        if (count<0) return error_json(out,"storage_unavailable");
        size_t n=(size_t)snprintf(out,LOCAL_OUTPUT,"{\"ok\":true,\"events\":[");
        for (int i=0;i<count;++i) {
            struct stored_event *e=&entries[i];
            n+=(size_t)snprintf(out+n,LOCAL_OUTPUT-n,"%s{\"id\":\"%016" PRIx64 "\",\"received_at\":%" PRIu64
                ",\"sequence_gap\":%" PRIu64 ",\"event\":",i?",":"",e->id,e->received_at,e->gap);
            size_t added=myvava_event_json(&e->event,out+n,LOCAL_OUTPUT-n);
            if (!added) return error_json(out,"invalid_stored_event");
            n+=added;static const char *states[]={"unknown","recording","finishing","failed","committed"};
            n+=(size_t)snprintf(out+n,LOCAL_OUTPUT-n,",\"clip_state_reported\":\"%s\"}",e->event.related_id?(e->clip_state<=4?states[e->clip_state]:"unknown"):"none");
        }
        n+=(size_t)snprintf(out+n,LOCAL_OUTPUT-n,"],\"next_before\":\"%016" PRIx64 "\"}\n",count?entries[count-1].id:0);
        return n;
    }
    if (!strncmp(line,"result ",7)) {
        char *id_text=strchr(line+7,' '); uint8_t expected_epoch[16]; uint64_t id;
        if (!id_text) return error_json(out,"result_requires_epoch_and_task_id");
        *id_text++=0;
        if (!myvava_unhex(line+7,expected_epoch,16) || !parse_task_id(id_text,&id)) return error_json(out,"invalid_task_reference");
        struct task t={0}; int found=task_get(journal,expected_epoch,id,&t);
        if (found!=1) return error_json(out,found<0?"storage_unavailable":"task_not_retained");
        size_t n=task_json(out,&t,true); task_clear(&t); out[n++]='\n'; return n;
    }
    if (!strncmp(line,"lookup ",7) || !strncmp(line,"history ",8)) {
        bool history=line[0]=='h'; char *device=line+(history?8:7), *value=strchr(device,' ');
        if (!value) return error_json(out,"invalid_history_request");
        *value++=0;
        if (!myvava_id_valid(device)) return error_json(out,"invalid_device");
        if (!history) {
            uint8_t key[16]; if (!myvava_unhex(value,key,16)) return error_json(out,"invalid_request_key");
            struct task t={0}; int found=task_find_key(journal,device,value,&t);
            if (found!=1) return error_json(out,found<0?"storage_unavailable":"task_not_retained");
            size_t n=task_json(out,&t,true); task_clear(&t); out[n++]='\n'; return n;
        }
        uint64_t before; if (!parse_task_id(value,&before)) return error_json(out,"invalid_task_id");
        struct task entries[32]; int count=task_history(journal,device,before,entries,32);
        if (count<0) return error_json(out,"storage_unavailable");
        size_t n=(size_t)snprintf(out,LOCAL_OUTPUT,"{\"ok\":true,\"tasks\":[");
        for (int i=0;i<count;++i) { if (i) out[n++]=','; n+=task_json(out+n,&entries[i],false); }
        n+=(size_t)snprintf(out+n,LOCAL_OUTPUT-n,"],\"next_before\":\"%016" PRIx64 "\"}\n",count?entries[count-1].id:0);
        return n;
    }
    if (strncmp(line,"submit ",7)) return error_json(out,"unsupported_command");
    char *parts[5], *p=line+7;
    for (unsigned i=0;i<4;++i) {
        parts[i]=p; p=strchr(p,' ');
        if (!p) return error_json(out,"submit_requires_device_boot_epoch_key_command");
        *p++=0;
    }
    parts[4]=p;
    uint8_t boot[16], expected_epoch[16], key[16];
    if (!myvava_unhex(parts[1],boot,16) || !myvava_unhex(parts[2],expected_epoch,16) ||
        !myvava_unhex(parts[3],key,16) || !*p || strlen(p)>MYVAVA_COMMAND_MAX || strpbrk(p,"\r\n"))
        return error_json(out,"invalid_submission");
    if (memcmp(expected_epoch,epoch,16)) return error_json(out,"server_epoch_changed");
    int index=-1;
    for (unsigned i=0;i<device_count;++i) if (!strcmp(parts[0],devices[i].name)) index=(int)i;
    if (index<0) return error_json(out,"unknown_device");
    struct task old={0}; int found=task_find_key(journal,parts[0],parts[3],&old);
    if (found<0) return error_json(out,"storage_unavailable");
    if (found) {
        bool conflict=strcmp(old.command,p) || memcmp(old.boot,boot,16);
        size_t n=conflict?error_json(out,"request_key_conflict"):submission_json(out,&old,true);
        task_clear(&old); return n;
    }
    if (strncmp(parts[3],request_prefix,16) && (!*previous_prefix || strncmp(parts[3],previous_prefix,16)))
        return error_json(out,"request_key_expired_or_unissued");
    struct device *d=&devices[index];
    if (!d->seen || memcmp(boot,d->boot,16)) return error_json(out,"device_boot_changed");
    if (now-d->last_seen>=15000) return error_json(out,"device_offline");
    found=task_pending(journal,epoch,d->name,&old); task_clear(&old);
    if (found<0) return error_json(out,"storage_unavailable");
    if (found) return error_json(out,"device_busy");
    struct task t={.expires=now+30000,.state=QUEUED};
    memcpy(t.epoch,epoch,16); memcpy(t.boot,boot,16);
    strcpy(t.device,d->name); strcpy(t.key,parts[3]); strcpy(t.command,p);
    int inserted=task_insert(journal,&t,request_prefix,previous_prefix);
    if (inserted!=1) return error_json(out,inserted<0?"storage_unavailable":"history_full_with_protected_requests");
    return submission_json(out,&t,false);
}
static int transport_event(coap_session_t *session, const coap_event_t event) {
    if (event==COAP_EVENT_SERVER_SESSION_NEW) {
        for (unsigned i=0;i<64;++i) if (!transports[i].session) {
            transports[i]=(struct transport){session,myvava_millis()}; break;
        }
    } else if (event==COAP_EVENT_SERVER_SESSION_DEL) {
        for (unsigned i=0;i<64;++i) if (transports[i].session==session) transports[i].session=NULL;
        for (unsigned i=0;i<device_count;++i) {
            if (devices[i].session==session) devices[i].session=NULL;
            if (file_bodies[i].session==session)memset(&file_bodies[i],0,sizeof(file_bodies[i]));
            if (control_bodies[i].session==session)memset(&control_bodies[i],0,sizeof(control_bodies[i]));
        }
    }
    return 0;
}
static void expire_sessions(void) {
    uint64_t now=myvava_millis();
    for (unsigned i=0;i<64;++i) {
        coap_session_t *s=transports[i].session;
        bool result_in_progress=false;
        for(unsigned k=0;s&&k<device_count;++k)if(devices[k].session==s&&control_bodies[k].session==s&&
            control_bodies[k].used<control_bodies[k].total&&now-control_bodies[k].since<60000){result_in_progress=true;break;}
        /* Include incomplete DTLS sessions. Retries on a reused NAT tuple can
         * otherwise keep both a dead established session and a stalled new
         * handshake alive forever through libcoap's raw-packet idle timer. */
        if (s && coap_session_get_type(s)==COAP_SESSION_TYPE_SERVER &&
            coap_session_get_state(s)!=COAP_SESSION_STATE_NONE && !result_in_progress && now-transports[i].active_at>=15000)
            coap_session_disconnected(s,COAP_NACK_NOT_DELIVERABLE);
    }
}
static void reload_keys(const char *path) {
    struct device next[DEVICE_LIMIT]={0};unsigned count=0;
    if(!read_keys(path,next,&count)){fprintf(stderr,"key registry reload rejected; previous registry retained\n");memset(next,0,sizeof(next));return;}
    /* Revoke live sessions before publishing the new registry. Any disk job
     * already authorized may finish, but cannot acknowledge a removed session. */
    for(unsigned i=0;i<FILE_STORE_JOBS;++i)if(pending_files[i].cookie){coap_free_async(pending_files[i].session,pending_files[i].async);memset(&pending_files[i],0,sizeof(pending_files[i]));}
    uint8_t unavailable[16]={0};for(unsigned i=0;i<device_count;++i)mvm_store_device(media_storage,devices[i].name,unavailable);
    for(unsigned i=0;i<64;++i)if(transports[i].session&&coap_session_get_type(transports[i].session)==COAP_SESSION_TYPE_SERVER&&coap_session_get_state(transports[i].session)!=COAP_SESSION_STATE_NONE)coap_session_disconnected(transports[i].session,COAP_NACK_NOT_DELIVERABLE);
    memset(file_bodies,0,sizeof(file_bodies));
    memset(control_bodies,0,sizeof(control_bodies));
    memset(devices,0,sizeof(devices));memcpy(devices,next,sizeof(next));device_count=count;
    for(unsigned i=0;i<count;++i)devices[i].psk=(coap_bin_const_t){32,devices[i].key};
    memset(next,0,sizeof(next));fprintf(stderr,"key registry reloaded; %u authorized devices; all transport sessions must authenticate again\n",count);
}
static void close_local(unsigned i) {
    close(locals[i].fd); free(locals[i].output); memset(&locals[i],0,sizeof(locals[i])); locals[i].fd=-1;
}
static void local_tick(int listener) {
    int fd=accept(listener,NULL,NULL);
    if (fd>=0) {
        unsigned i=0; while (i<LOCAL_LIMIT && locals[i].fd>=0) ++i;
        if (i==LOCAL_LIMIT || !nonblock(fd)) close(fd);
        else { locals[i].fd=fd; locals[i].since=myvava_millis(); }
    }
    for (unsigned i=0;i<LOCAL_LIMIT;++i) {
        struct local_client *c=&locals[i]; if (c->fd<0) continue;
        if (myvava_millis()-c->since>5000) { close_local(i); continue; }
        if (!c->output) {
            ssize_t n=recv(c->fd,c->input+c->used,sizeof(c->input)-1-c->used,0);
            if (n==0 || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) { close_local(i); continue; }
            if (n>0) {
                c->used+=(size_t)n; char *end=memchr(c->input,'\n',c->used);
                if (end) {
                    *end=0;
                    c->output=malloc(LOCAL_OUTPUT);
                    if (!c->output) { close_local(i); continue; }
                    c->length=memchr(c->input,0,(size_t)(end-c->input))?error_json(c->output,"invalid_request"):local_command(c->input,c->output);
                } else if (c->used==sizeof(c->input)-1) { close_local(i); continue; }
            }
        }
        if (c->output) {
            ssize_t n=send(c->fd,c->output+c->sent,c->length-c->sent,0);
            if (n>0) c->sent+=(size_t)n;
            if (c->sent==c->length || (n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR)) close_local(i);
        }
    }
}
int main(int argc, char **argv) {
    if(argc>1&&!strcmp(argv[1],"--import-original"))return legacy_import_main(argc,argv);
    if(argc==3&&(!strcmp(argv[1],"--storage-key-create")||!strcmp(argv[1],"--storage-key-rotate"))){char id[33];if(!seal_keys_generate(argv[2],!strcmp(argv[1],"--storage-key-rotate"),id)){fprintf(stderr,"storage key maintenance failed; existing keys retained\n");return 1;}printf("%s\n",id);return 0;}
    if(argc==3&&!strcmp(argv[1],"--keys-check")){struct device check[DEVICE_LIMIT]={0};unsigned count=0;bool ok=read_keys(argv[2],check,&count);memset(check,0,sizeof(check));if(ok)printf("valid registry: %u devices\n",count);return ok?0:1;}

    const char *bind_ip="127.0.0.1", *keys=NULL, *state=NULL; unsigned port=3731;
    struct server_web_config web_cfg={.bind="127.0.0.1"};
    struct server_web *web=NULL;
    const char *file_dir=NULL,*file_keyring=NULL; unsigned file_limit=1024; uint64_t file_quota=1024ull*1024*1024;
    for (int i=1;i<argc;++i) {
        if (i+1>=argc) goto usage;
        const char *option=argv[i++], *value=argv[i];
        if (!strcmp(option,"--bind")) bind_ip=value;
        else if (!strcmp(option,"--keys")) keys=value;
        else if (!strcmp(option,"--state-dir")) state=value;
        else if (!strcmp(option,"--file-dir")) file_dir=value;
        else if (!strcmp(option,"--file-keyring")) file_keyring=value;
        else if (!strcmp(option,"--web-bind")) web_cfg.bind=value;
        else if (!strcmp(option,"--web-origin")) web_cfg.origin=value;
        else if (!strcmp(option,"--web-dir")) web_cfg.directory=value;
        else if (!strcmp(option,"--web-admin-password-file")) web_cfg.bootstrap=value;
        else if (!strcmp(option,"--web-cert")) web_cfg.certificate=value;
        else if (!strcmp(option,"--web-key")) web_cfg.key=value;
        else if (!strcmp(option,"--media-ffmpeg")) web_cfg.ffmpeg=value;
        else if (!strcmp(option,"--web-port")) {
            char *end=NULL; unsigned long n=strtoul(value,&end,10);
            if (!*value || *end || !n || n>65535) goto usage; web_cfg.port=(unsigned)n;
        }
        else if (!strcmp(option,"--file-limit") || !strcmp(option,"--file-quota-mb")) {
            char *end=NULL; errno=0; unsigned long n=strtoul(value,&end,10);
            if (errno || !*value || *end || !n || n>1048576) goto usage;
            if (!strcmp(option,"--file-limit")) { if (n>4096) goto usage; file_limit=(unsigned)n; }
            else file_quota=(uint64_t)n*1024*1024;
        }
        else if (!strcmp(option,"--history-limit") || !strcmp(option,"--event-history-limit") || !strcmp(option,"--request-window-seconds")) {
            char *end=NULL; unsigned long n=strtoul(value,&end,10);
            if (!*value || *end) goto usage;
            if (!strcmp(option,"--history-limit")) { if (n<16 || n>8192) goto usage; history_limit=(unsigned)n; }
            else if (!strcmp(option,"--event-history-limit")) { if (n<16 || n>65536) goto usage; event_limit=(unsigned)n; }
            else { if (n<1 || n>300) goto usage; request_window_ms=(unsigned)n*1000; }
        }
        else if (!strcmp(option,"--port")) {
            char *end=NULL; unsigned long n=strtoul(value,&end,10);
            if (!*value || *end || !n || n>65535) goto usage; port=(unsigned)n;
        } else goto usage;
    }
    if (!keys || !state || (file_keyring&&!file_dir)) goto usage;
    umask(0077);
    if (!process_signal_install(SIGPIPE,SIG_IGN) || !process_signal_install(SIGINT,stop_signal) ||
        !process_signal_install(SIGTERM,stop_signal) || !process_signal_install(SIGHUP,reload_signal)) {
        perror("sigaction"); return 1;
    }
    if (!read_keys(keys,devices,&device_count) || !device_count || !myvava_random(epoch,16)) { fprintf(stderr,"invalid private key registry\n"); return 1; }
    struct stat st; char path[sizeof(((struct sockaddr_un *)0)->sun_path)], lockpath[1024];
    if ((mkdir(state,0700) && errno!=EEXIST) || lstat(state,&st) || !S_ISDIR(st.st_mode) ||
        st.st_uid!=geteuid() || (st.st_mode&0077) || snprintf(path,sizeof(path),"%s/server.sock",state)>=(int)sizeof(path) ||
        snprintf(lockpath,sizeof(lockpath),"%s/server.lock",state)>=(int)sizeof(lockpath)) return 1;
    int lockfd=open(lockpath,O_RDWR|O_CREAT|O_NOFOLLOW|O_CLOEXEC,0600);
    struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    if (lockfd<0 || fcntl(lockfd,F_SETLK,&lock)) { fprintf(stderr,"server state already in use\n"); return 1; }
    journal=task_store_open(state,history_limit);
    if (!journal) { fprintf(stderr,"private task database unavailable or incompatible; not reset\n"); close(lockfd); return 1; }
    media_storage=mvm_store_new();if(!media_storage)return 1;
    if (file_dir && !(file_storage=file_store_start_encrypted(file_dir,file_quota,file_limit,file_keyring))) {
        fprintf(stderr,"private file store unavailable or incompatible; not reset\n"); return 1;
    }
    if (!lstat(path,&st)) { if (!S_ISSOCK(st.st_mode) || st.st_uid!=geteuid() || unlink(path)) return 1; }
    else if (errno!=ENOENT) return 1;
    int listener=socket(AF_UNIX,SOCK_STREAM,0);
    struct sockaddr_un address={.sun_family=AF_UNIX}; strcpy(address.sun_path,path);
    if (listener<0 || !nonblock(listener) || bind(listener,(struct sockaddr *)&address,sizeof(address)) || chmod(path,0600) || listen(listener,8)) return 1;
    for (unsigned i=0;i<LOCAL_LIMIT;++i) locals[i].fd=-1;
    coap_startup(); coap_set_log_level(COAP_LOG_ERR); coap_dtls_set_log_level(COAP_LOG_ERR);
    coap_context_t *context=coap_new_context(NULL);
    if (!context) return 1;
    coap_context_set_block_mode(context,COAP_BLOCK_USE_LIBCOAP);
    coap_register_event_handler(context,transport_event);
    coap_context_set_max_idle_sessions(context,DEVICE_LIMIT*2);
    coap_context_set_max_handshake_sessions(context,4);
    coap_context_set_session_timeout(context,30);
    coap_dtls_spsk_t psk={0}; psk.version=COAP_DTLS_SPSK_SETUP_VERSION;
    psk.validate_id_call_back=verify_identity;
    psk.psk_info.hint=(coap_bin_const_t){9,(const uint8_t *)"myvava-v1"};
    /* The default key only enables PSK support. The callback never falls back
     * to it: every supplied identity must match its own provisioned key. */
    psk.psk_info.key=devices[0].psk;
    if (!coap_context_set_psk2(context,&psk)) return 1;
    coap_address_t local; coap_address_init(&local);
    local.addr.sin.sin_family=AF_INET; local.addr.sin.sin_port=htons((uint16_t)port);
    local.size=sizeof(local.addr.sin);
    if (inet_pton(AF_INET,bind_ip,&local.addr.sin.sin_addr)!=1) goto usage;
    coap_endpoint_t *endpoint=coap_new_endpoint(context,&local,COAP_PROTO_DTLS);
    if (!endpoint) { fprintf(stderr,"DTLS endpoint failed\n"); return 1; }
    coap_endpoint_set_default_mtu(endpoint,1200);
    coap_resource_t *resource=coap_resource_init(coap_make_str_const("myvava/v1/sync"),0);
    coap_register_handler(resource,COAP_REQUEST_POST,sync_request); coap_add_resource(context,resource);
    resource=coap_resource_init(coap_make_str_const("myvava/v1/events"),0);
    coap_register_handler(resource,COAP_REQUEST_POST,event_request); coap_add_resource(context,resource);
    resource=coap_resource_init(coap_make_str_const("myvava/v1/files"),0);
    coap_register_handler(resource,COAP_REQUEST_POST,file_request); coap_add_resource(context,resource);
    resource=coap_resource_init(coap_make_str_const("myvava/v1/media"),0);
    coap_register_handler(resource,COAP_REQUEST_POST,media_request); coap_add_resource(context,resource);
    if (web_cfg.port) {
        file_store_enable_retention(file_storage);
    web_cfg.state=state; web_cfg.socket=path; web_cfg.files=file_storage; web_cfg.media=media_storage; web=server_web_start(&web_cfg);
        if (!web) { fprintf(stderr,"management Web startup failed; check private bootstrap, assets, bind and TLS configuration\n"); return 1; }
    }
    fprintf(stderr,"vava-server ready; UDP %s:%u; devices=%u; control and event channels\n",bind_ip,port,device_count);
    while (!stopping) { if(reload_requested){reload_requested=0;reload_keys(keys);} if (coap_io_process(context,file_io_wait())<0) break; file_tick(); local_tick(listener); expire_sessions(); }
    server_web_stop(web);
    mvm_store_free(media_storage);
    for (unsigned i=0;i<LOCAL_LIMIT;++i) if (locals[i].fd>=0) close_local(i);
    file_store_stop(file_storage);
    task_store_close(journal);
    coap_free_context(context); coap_cleanup(); close(listener); unlink(path); close(lockfd);
    memset(devices,0,sizeof(devices)); return 0;
usage:
    fprintf(stderr,"vava-server --keys FILE --state-dir DIR [--bind IPv4] [--port 3731]\n"
                   "  [--web-port PORT] [--web-bind IPv4] [--web-admin-password-file FILE]\n"
                   "  [--web-dir DIR] [--web-origin ORIGIN] [--web-cert FILE --web-key FILE] [--media-ffmpeg ABSOLUTE_PATH]\n"
                   "  [--file-dir PRIVATE_DIR] [--file-keyring PRIVATE_DIR] [--file-limit 1024] [--file-quota-mb 1024]\n"
                   "  [--history-limit 1024] [--event-history-limit 8192] [--request-window-seconds 60]\n"); return 2;
}
