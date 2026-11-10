#include "myvava.h"
#include "files.h"
#include "media.h"
#include "resolver.h"
#include <coap3/coap.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct myvava_client {
    pthread_t thread;
    pthread_mutex_t mutex;
    struct myvava_client_status status;
    bool stop, queued, running, done;
    char host[254], identity[MYVAVA_ID_MAX+1], command[512];
    char result[MYVAVA_RESULT_MAX];
    size_t result_len;
    unsigned port, poll_ms;
    uint8_t key[32], boot[16], epoch[16], token[8];
    uint64_t command_id, request_started, next_request, execute_before, control_active_until;
    uint32_t result_offset;
    bool pending, reset, waiting_core, synchronized, event_pending, control_batch;
    struct myvava_events *events;
    uint8_t event[MYVAVA_EVENT_SIZE];
    uint64_t next_event;
    const struct myvava_file_channel *files;
    bool file_pending;
    uint64_t file_io_until;
    uint8_t file_token[8];
    struct myvava_message file_request;
    uint8_t file_data[MYVAVA_FILE_PAYLOAD_MAX];
    struct mvm_client *media;
    bool media_pending;
    uint8_t media_token[8];
    uint64_t media_started, next_media;
    struct myvava_resolver *resolver;
    struct myvava_resolution addresses;
    uint64_t resolving_since, addresses_until;unsigned address_cursor;
    coap_context_t *context;
    coap_session_t *session;
};

static void connection_state(struct myvava_client *c, const char *phase, const char *error) {
    pthread_mutex_lock(&c->mutex);
    snprintf(c->status.phase,sizeof(c->status.phase),"%s",phase);
    if (error) snprintf(c->status.last_error,sizeof(c->status.last_error),"%s",error);
    pthread_mutex_unlock(&c->mutex);
}
/* A prebuilt executable can load an OpenWrt libssl built with no-dtls/no-psk.
 * Exported DTLS symbols alone do not prove the runtime supports the protocol. */
const char *myvava_transport_error(void) {
    SSL_CTX *ctx=SSL_CTX_new(DTLS_client_method());
    if (!ctx) return "dtls_context_unavailable";
    const char *error=NULL;
    if (!SSL_CTX_set_min_proto_version(ctx,DTLS1_2_VERSION) ||
        !SSL_CTX_set_max_proto_version(ctx,DTLS1_2_VERSION)) error="dtls_1_2_unavailable";
    else if (!SSL_CTX_set_cipher_list(ctx,"PSK:!NULL")) error="dtls_psk_unavailable";
    SSL_CTX_free(ctx);
    return error;
}
static struct myvava_client *session_client(coap_session_t *session) {
    struct myvava_client *c=coap_session_get_app_data(session);
    /* Handshake callbacks can run inside session creation, before its return. */
    return c?c:coap_get_app_data(coap_session_get_context(session));
}
static int transport_event(coap_session_t *session, const coap_event_t event) {
    struct myvava_client *c=session_client(session);
    if (c && (event==COAP_EVENT_DTLS_ERROR || event==COAP_EVENT_SESSION_FAILED)) {
        c->reset=true;
        connection_state(c,"backoff","dtls_handshake_failed");
    }
    return 0;
}
static void clear_command(struct myvava_client *c) {
    c->command_id=0; c->result_len=0; c->result_offset=0;
    c->waiting_core=false; c->command[0]=0; c->control_active_until=0;
}
/* Follow-up browser requests arrive after the previous durable result. Keep
 * that short interaction responsive, then return to the configured idle poll.
 * Telemetry/event ACKs do not extend this window or wake a camera. */
static unsigned control_poll_delay(const struct myvava_client *c,uint64_t now) {
    return now<c->control_active_until && c->poll_ms>100?100:c->poll_ms;
}
static coap_response_t response(coap_session_t *session, const coap_pdu_t *sent,
                                const coap_pdu_t *received, const coap_mid_t mid) {
    (void)sent; (void)mid;
    struct myvava_client *c=session_client(session);
    coap_bin_const_t token=coap_pdu_get_token(received);
    if (c->media_pending && token.length==8 && !memcmp(token.s,c->media_token,8)) {
        if (!coap_pdu_get_code(received)) return COAP_RESPONSE_OK;
        c->media_pending=false; c->next_media=myvava_millis()+100;
        if(myvava_millis()-c->media_started>MVM_LEASE_MS){mvm_client_reset(c->media);return COAP_RESPONSE_OK;}
        size_t size=0; const uint8_t *data=NULL; struct myvava_message m;
        coap_opt_iterator_t oi;coap_opt_t *cap=coap_check_option(received,MVM_FRAMING_OPTION,&oi);bool v2=cap&&coap_opt_length(cap)==1&&coap_opt_value(cap)[0]==2;
        if (coap_pdu_get_code(received)!=COAP_RESPONSE_CODE_CONTENT ||
            !coap_get_data(received,&size,&data) || !myvava_decode(data,size,&m) ||
            m.type!=MYVAVA_MEDIA_CONTROL || m.id || m.total || m.offset ||
            memcmp(m.boot,c->boot,16) || memcmp(m.epoch,c->epoch,16) ||
            !mvm_client_control_version(c->media,m.data,m.length,v2)) mvm_client_reset(c->media);
        if(!mvm_client_active(c->media))c->next_media=myvava_millis()+1000;
        return COAP_RESPONSE_OK;
    }
    if (c->file_pending && token.length==8 && !memcmp(token.s,c->file_token,8)) {
        if (coap_pdu_get_code(received)==0) return COAP_RESPONSE_OK;
        c->file_pending=false;
        if (coap_pdu_get_code(received)!=COAP_RESPONSE_CODE_CONTENT) {
            c->files->reset(c->files->arg); return COAP_RESPONSE_OK;
        }
        size_t size=0; const uint8_t *data=NULL; struct myvava_message m;
        const uint8_t *sha=c->file_request.type==MYVAVA_FILE_OFFER?c->file_data+16:c->file_data;
        if (!coap_get_data(received,&size,&data) || !myvava_decode(data,size,&m) ||
            m.type!=MYVAVA_FILE_ACK || memcmp(m.boot,c->boot,16) || memcmp(m.epoch,c->epoch,16) ||
            m.id!=c->file_request.id || m.total!=c->file_request.total || m.offset>m.total ||
            m.length!=33 || memcmp(m.data,sha,32) || m.data[32]>MVF_DELETED) {
            c->reset=true; return COAP_RESPONSE_OK;
        }
        pthread_mutex_lock(&c->mutex); ++c->status.responses; pthread_mutex_unlock(&c->mutex);
        coap_opt_iterator_t oi;coap_opt_t *option=coap_check_option(received,MYVAVA_FILE_BATCH_OPTION,&oi);
        m.file_batch=c->file_request.type==MYVAVA_FILE_OFFER&&option&&coap_opt_length(option)==1&&
            coap_opt_value(option)[0]==MYVAVA_FILE_BATCH_SEGMENTS;
        c->files->reply(c->files->arg,&m); return COAP_RESPONSE_OK;
    }
    if (!c->pending || token.length!=8 || memcmp(token.s,c->token,8)) return COAP_RESPONSE_OK;
    if (coap_pdu_get_code(received)==0) return COAP_RESPONSE_OK;
    c->pending=false;
    if (!c->event_pending) { uint64_t now=myvava_millis(); c->next_request=now+control_poll_delay(c,now); }
    /* A busy/full server event store must not tear down working control. */
    if (c->event_pending && coap_pdu_get_code(received)==COAP_RESPONSE_CODE_SERVICE_UNAVAILABLE) {
        pthread_mutex_lock(&c->mutex); ++c->status.event_rejections; pthread_mutex_unlock(&c->mutex);
        c->next_event=myvava_millis()+1000; return COAP_RESPONSE_OK;
    }
    size_t size=0; const uint8_t *data=NULL; struct myvava_message m;
    if (coap_pdu_get_code(received)!=COAP_RESPONSE_CODE_CONTENT ||
        !coap_get_data(received,&size,&data) || !myvava_decode(data,size,&m) ||
        memcmp(m.boot,c->boot,16)) { c->reset=true; return COAP_RESPONSE_OK; }
    pthread_mutex_lock(&c->mutex);
    ++c->status.responses; c->status.last_response_ms=myvava_millis();
    pthread_mutex_unlock(&c->mutex);
    connection_state(c,"online","");
    if (c->event_pending) {
        struct myvava_event e;
        if (!myvava_event_decode(c->event,sizeof(c->event),&e) || m.type!=MYVAVA_EVENT_ACK ||
            memcmp(m.epoch,c->epoch,16) || m.id!=e.sequence || m.total!=MYVAVA_EVENT_SIZE ||
            m.offset || m.length!=16 || memcmp(m.data,e.origin,16)) { c->reset=true; return COAP_RESPONSE_OK; }
        pthread_mutex_lock(&c->mutex); ++c->status.event_acks; pthread_mutex_unlock(&c->mutex);
        myvava_events_ack(c->events,e.origin,e.sequence); c->next_event=myvava_millis()+50;
        return COAP_RESPONSE_OK;
    }
    coap_opt_iterator_t control_oi;coap_opt_t *control_option=coap_check_option(received,MYVAVA_CONTROL_BATCH_OPTION,&control_oi);
    c->control_batch=control_option&&coap_opt_length(control_option)==1&&coap_opt_value(control_option)[0]==MYVAVA_CONTROL_BATCH_CHUNKS;
    c->synchronized=true;
    if (memcmp(m.epoch,c->epoch,16)) {
        if (m.type!=MYVAVA_IDLE || m.id || m.length) { c->reset=true; return COAP_RESPONSE_OK; }
        memcpy(c->epoch,m.epoch,16); clear_command(c);
        mvm_client_reset(c->media);
        return COAP_RESPONSE_OK;
    }
    if (m.type==MYVAVA_ACK) {
        if (m.id!=c->command_id || m.total!=c->result_len || m.offset>c->result_len || m.length) {
            c->reset=true; return COAP_RESPONSE_OK;
        }
        c->result_offset=m.offset;
        c->control_active_until=myvava_millis()+2000;
        /* Even the final chunk needs an immediate receipt confirmation.
         * Waiting an idle poll here holds up the next queued command. */
        c->next_request=0;
    } else if (m.type==MYVAVA_COMMAND) {
        if (!m.id || !m.length || m.length>MYVAVA_COMMAND_MAX || m.offset ||
            !m.total || m.total>60000 || memchr(m.data,0,m.length) ||
            memchr(m.data,'\n',m.length) || memchr(m.data,'\r',m.length)) { c->reset=true; return COAP_RESPONSE_OK; }
        c->control_active_until=myvava_millis()+2000;
        if (m.id==c->command_id) {
            if (strlen(c->command)!=m.length || memcmp(c->command,m.data,m.length)) c->reset=true;
            else { c->result_offset=0; c->next_request=0; }
            return COAP_RESPONSE_OK;
        }
        if (m.id<c->command_id) { c->reset=true; return COAP_RESPONSE_OK; }
        c->command_id=m.id; c->result_offset=0; c->result_len=0;
        pthread_mutex_lock(&c->mutex);
        memcpy(c->command,m.data,m.length); c->command[m.length]=0;
        c->execute_before=c->request_started+m.total;
        c->queued=true; c->done=false; c->waiting_core=true;
        pthread_mutex_unlock(&c->mutex);
    } else if (m.type!=MYVAVA_IDLE || m.id || m.length || m.total || m.offset) c->reset=true;
    return COAP_RESPONSE_OK;
}
static void nack(coap_session_t *session, const coap_pdu_t *sent,
                 const coap_nack_reason_t reason, const coap_mid_t mid) {
    (void)mid;
    struct myvava_client *c=session_client(session);
    if(c&&sent&&reason==COAP_NACK_TOO_MANY_RETRIES){
        coap_bin_const_t token=coap_pdu_get_token(sent);
        if(c->media_pending&&token.length==8&&!memcmp(token.s,c->media_token,8)){
            c->media_pending=false;c->next_media=myvava_millis()+100;mvm_client_reset(c->media);return;
        }
        if(c->file_pending&&token.length==8&&!memcmp(token.s,c->file_token,8)){
            c->file_pending=false;if(c->files)c->files->reset(c->files->arg);return;
        }
    }
    if (c && (!sent || coap_pdu_get_type(sent)!=COAP_MESSAGE_NON)) { c->reset=true; connection_state(c,"backoff","dtls_or_transport_nack"); }
}
static int connect_server(struct myvava_client *c) {
    uint64_t now=myvava_millis();
    if(!c->addresses.count||now>=c->addresses_until){
        connection_state(c,"resolving",NULL);
        if(!c->resolver){c->resolver=myvava_resolver_start(c->host,c->port,NULL);c->resolving_since=now;if(!c->resolver)return -1;}
        struct myvava_resolution result;
        if(!myvava_resolver_result(c->resolver,&result)){if(now-c->resolving_since>=5000){connection_state(c,"backoff","dns_timeout");return -1;}return 0;}
        myvava_resolver_release(c->resolver);c->resolver=NULL;
        if(result.error||!result.count){connection_state(c,"backoff","dns_failed");return -1;}
        c->addresses=result;c->addresses_until=now+60000;
    }
    connection_state(c,"connecting",NULL);
    const char *runtime_error=myvava_transport_error();
    if (runtime_error) { connection_state(c,"backoff",runtime_error); return -1; }
    const struct myvava_address *a=&c->addresses.peers[c->address_cursor++%c->addresses.count];
    coap_address_t peer;coap_address_init(&peer);peer.size=a->size;memcpy(&peer.addr,&a->address,a->size);
    c->context=coap_new_context(NULL);
    if (!c->context) { connection_state(c,"backoff","coap_context_unavailable"); return -1; }
    coap_context_set_block_mode(c->context,COAP_BLOCK_USE_LIBCOAP);
    coap_set_app_data(c->context,c);
    coap_register_response_handler(c->context,response);
    coap_register_nack_handler(c->context,nack);
    coap_register_event_handler(c->context,transport_event);
    coap_dtls_cpsk_t psk={0}; psk.version=COAP_DTLS_CPSK_SETUP_VERSION;
    psk.psk_info.identity=(coap_bin_const_t){strlen(c->identity),(const uint8_t *)c->identity};
    psk.psk_info.key=(coap_bin_const_t){32,c->key};
    c->reset=false;
    c->session=coap_new_client_session_psk2(c->context,NULL,&peer,COAP_PROTO_DTLS,&psk);
    if (!c->session || c->reset) {
        if (!c->reset) connection_state(c,"backoff","dtls_session_unavailable");
        if (c->session) coap_session_release(c->session);
        c->session=NULL; coap_free_context(c->context); c->context=NULL; return -1;
    }
    coap_session_set_app_data(c->session,c);
    coap_session_set_mtu(c->session,1200);
    coap_session_set_max_retransmit(c->session,3);
    /* At most one control/event, file and media CON is outstanding. libcoap's
     * default NSTART=1 would let a lost media ACK block the other two lanes. */
    coap_session_set_nstart(c->session,3);
    c->pending=false; c->reset=false; c->next_request=0; c->control_batch=false;
    return 1;
}
static void release_packet(coap_session_t *session,void *packet) {
    (void)session;free(packet);
}
static bool send_sync(struct myvava_client *c) {
    struct myvava_message m={.type=MYVAVA_SYNC,.id=c->command_id,
        .total=(uint32_t)c->result_len,.offset=c->result_offset};
    memcpy(m.boot,c->boot,16); memcpy(m.epoch,c->epoch,16);
    if (c->result_offset<c->result_len) {
        m.length=c->result_len-c->result_offset;
        size_t limit=c->control_batch?MYVAVA_CONTROL_BATCH_MAX:MYVAVA_CHUNK;
        if (m.length>limit) m.length=limit;
        m.data=(const uint8_t *)c->result+c->result_offset;
    }
    uint8_t *packet=malloc(MYVAVA_HEADER+m.length);
    size_t n=packet?myvava_encode_sized(packet,MYVAVA_HEADER+m.length,&m):0;
    if(!n){free(packet);return false;}
    coap_pdu_t *pdu=coap_new_pdu(COAP_MESSAGE_CON,COAP_REQUEST_CODE_POST,c->session);
    if (!pdu || !myvava_random(c->token,8)) { free(packet);coap_delete_pdu(pdu); return false; }
    const uint8_t chunks=MYVAVA_CONTROL_BATCH_CHUNKS;
    if (!coap_add_token(pdu,8,c->token) ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,6,(const uint8_t *)"myvava") ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,2,(const uint8_t *)"v1") ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,4,(const uint8_t *)"sync") ||
        !coap_add_option(pdu,MYVAVA_CONTROL_BATCH_OPTION,1,&chunks)) { free(packet);coap_delete_pdu(pdu); return false; }
    /* libcoap retains the immutable batch through Block1 retries. A protocol
     * result ACK still follows its durable server commit, never a block ACK. */
    if(!coap_add_data_large_request(c->session,pdu,n,packet,release_packet,packet)){coap_delete_pdu(pdu);return false;}
    c->event_pending=false; c->pending=true; c->request_started=myvava_millis();
    pthread_mutex_lock(&c->mutex); ++c->status.requests; pthread_mutex_unlock(&c->mutex);
    return coap_send(c->session,pdu)!=COAP_INVALID_MID;
}
static bool send_event(struct myvava_client *c) {
    struct myvava_event e;
    if (!myvava_event_decode(c->event,sizeof(c->event),&e)) return false;
    struct myvava_message m={.type=MYVAVA_EVENT,.id=e.sequence,.total=MYVAVA_EVENT_SIZE,
        .length=MYVAVA_EVENT_SIZE,.data=c->event};
    memcpy(m.boot,c->boot,16); memcpy(m.epoch,c->epoch,16);
    uint8_t packet[MYVAVA_PACKET_MAX]; size_t n=myvava_encode(packet,&m);
    coap_pdu_t *pdu=coap_new_pdu(COAP_MESSAGE_CON,COAP_REQUEST_CODE_POST,c->session);
    if (!pdu || !myvava_random(c->token,8)) { coap_delete_pdu(pdu); return false; }
    if (!coap_add_token(pdu,8,c->token) ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,6,(const uint8_t *)"myvava") ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,2,(const uint8_t *)"v1") ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,6,(const uint8_t *)"events") ||
        !coap_add_data(pdu,n,packet)) { coap_delete_pdu(pdu); return false; }
    c->event_pending=true; c->pending=true; c->request_started=myvava_millis();
    pthread_mutex_lock(&c->mutex); ++c->status.requests; pthread_mutex_unlock(&c->mutex);
    return coap_send(c->session,pdu)!=COAP_INVALID_MID;
}
static bool send_file(struct myvava_client *c) {
    struct myvava_message *m=&c->file_request;
    memcpy(m->boot,c->boot,16); memcpy(m->epoch,c->epoch,16);
    uint8_t *packet=malloc(MYVAVA_HEADER+m->length);
    size_t n=packet?myvava_encode_sized(packet,MYVAVA_HEADER+m->length,m):0;
    coap_pdu_t *pdu=coap_new_pdu(COAP_MESSAGE_CON,COAP_REQUEST_CODE_POST,c->session);
    if (!pdu || !n || !myvava_random(c->file_token,8)) { free(packet);coap_delete_pdu(pdu); return false; }
    if (!coap_add_token(pdu,8,c->file_token) ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,6,(const uint8_t *)"myvava") ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,2,(const uint8_t *)"v1") ||
        !coap_add_option(pdu,COAP_OPTION_URI_PATH,5,(const uint8_t *)"files")) { free(packet);coap_delete_pdu(pdu); return false; }
    const uint8_t segments=MYVAVA_FILE_BATCH_SEGMENTS;
    if(m->type==MYVAVA_FILE_OFFER&&!coap_add_option(pdu,MYVAVA_FILE_BATCH_OPTION,1,&segments)){free(packet);coap_delete_pdu(pdu);return false;}
    /* libcoap owns this immutable buffer through Block1 completion/retries,
     * including a timeout followed by a new uploader request. MTU stays 1200. */
    if(!coap_add_data_large_request(c->session,pdu,n,packet,release_packet,packet)){coap_delete_pdu(pdu);return false;}
    c->file_pending=true;c->file_io_until=myvava_millis()+50;
    pthread_mutex_lock(&c->mutex); ++c->status.requests; pthread_mutex_unlock(&c->mutex);
    return coap_send(c->session,pdu)!=COAP_INVALID_MID;
}
static void disconnect_server(struct myvava_client *c) {
    mvm_client_reset(c->media); c->media_pending=false; c->next_media=0;
    if (c->files) c->files->reset(c->files->arg);
    if (c->session) coap_session_release(c->session);
    if (c->context) coap_free_context(c->context);
    c->session=NULL; c->context=NULL; c->pending=false; c->file_pending=false; c->synchronized=false;
}
static bool send_media(struct myvava_client *c,bool poll) {
    struct myvava_message m; uint8_t data[MYVAVA_CHUNK],packet[MYVAVA_PACKET_MAX];
    if (poll) mvm_client_poll(c->media,&m);
    else if (!mvm_client_peek(c->media,&m,data)) return false;
    memcpy(m.boot,c->boot,16);memcpy(m.epoch,c->epoch,16);
    size_t n=myvava_encode(packet,&m);uint8_t token[8];
    coap_pdu_t *pdu=coap_new_pdu(poll?COAP_MESSAGE_CON:COAP_MESSAGE_NON,COAP_REQUEST_CODE_POST,c->session);
    if(!pdu||!myvava_random(token,8)){coap_delete_pdu(pdu);return false;}
    uint8_t no_response=2;
    bool ok=coap_add_token(pdu,8,token)&&coap_add_option(pdu,COAP_OPTION_URI_PATH,6,(const uint8_t *)"myvava")&&
        coap_add_option(pdu,COAP_OPTION_URI_PATH,2,(const uint8_t *)"v1")&&coap_add_option(pdu,COAP_OPTION_URI_PATH,5,(const uint8_t *)"media");
    if(!poll)ok=ok&&coap_add_option(pdu,COAP_OPTION_NORESPONSE,1,&no_response);
    else {uint8_t version=MVM_FRAMING_VERSION;ok=ok&&coap_add_option(pdu,MVM_FRAMING_OPTION,1,&version);}
    if(!ok||!coap_add_data(pdu,n,packet)){coap_delete_pdu(pdu);return false;}
    if(poll){memcpy(c->media_token,token,8);c->media_pending=true;c->media_started=myvava_millis();}
    return coap_send(c->session,pdu)!=COAP_INVALID_MID;
}
static void expire_media(struct myvava_client *c,uint64_t now) {
    /* Retain the outstanding CON until response/NACK; opening another one on
     * every lease expiry would accumulate retransmissions during an outage. */
    if(c->media_pending&&now>=c->media_started&&now-c->media_started>MVM_LEASE_MS)mvm_client_reset(c->media);
}
static void *network_thread(void *arg) {
    struct myvava_client *c=arg; unsigned failures=0;
    uint64_t retry_at=0;
    coap_startup(); coap_set_log_level(COAP_LOG_ERR); coap_dtls_set_log_level(COAP_LOG_ERR);
    for (;;) {
        pthread_mutex_lock(&c->mutex);
        bool stop=c->stop;
        if (c->waiting_core && c->done) { c->waiting_core=false; c->done=false; c->next_request=0; }
        pthread_mutex_unlock(&c->mutex);
        if (stop) break;
        uint64_t now=myvava_millis();
        if (!c->session && now>=retry_at) {
            if (connect_server(c)<0) c->reset=true;
        }
        if (c->session) {
            if (c->synchronized && c->media) {
                if (!c->media_pending && now>=c->next_media && !send_media(c,true)) c->reset=true;
                for(unsigned burst=0;burst<32&&!c->reset;++burst) if(!send_media(c,false))break;
                expire_media(c,now);
            }
            if (!c->pending) {
                if (!c->waiting_core && now>=c->next_request) { if (!send_sync(c)) c->reset=true; }
                else if (c->synchronized && (c->waiting_core || c->result_offset==c->result_len) && now>=c->next_event &&
                    myvava_events_peek(c->events,c->event)) { if (!send_event(c)) c->reset=true; }
                else if (c->synchronized && (c->waiting_core || c->result_offset==c->result_len) && c->files && !c->file_pending &&
                    c->files->peek(c->files->arg,&c->file_request,c->file_data) && !send_file(c)) c->reset=true;
            }
            /* A file ACK wakes the upload worker, which prepares the next
             * chunk on another thread. Avoid adding an idle 20 ms poll to
             * each 736-byte chunk; return to the idle budget between jobs. */
            if (coap_io_process(c->context,c->file_pending||myvava_millis()<c->file_io_until?1:20)<0) {
                c->reset=true; connection_state(c,"backoff","transport_io_failed");
            }
            /* Only a result that actually spans multiple blocks needs the
             * longer retry window. Negotiation alone must not delay recovery
             * of a small control reply after the server restarts. */
            bool large_result=!c->event_pending&&c->control_batch&&
                c->result_len>(size_t)c->result_offset+MYVAVA_CHUNK;
            uint64_t control_timeout=large_result?60000:20000;
            if (c->pending && now>=c->request_started && now-c->request_started>control_timeout) {
                c->reset=true; connection_state(c,"backoff","exchange_timeout");
            }
            /* File retry exhaustion is handled by its NACK, without resetting
             * healthy control/media or adding another outstanding file CON. */
            if (!c->pending && !c->reset) failures=0;
        }
        if (c->reset) {
            disconnect_server(c); c->reset=false;
            connection_state(c,"backoff",NULL);
            pthread_mutex_lock(&c->mutex); ++c->status.reconnects; pthread_mutex_unlock(&c->mutex);
            uint8_t jitter=0; (void)myvava_random(&jitter,1);
            if (failures<5) ++failures;
            retry_at=myvava_millis()+((uint64_t)250<<failures)+(unsigned)jitter;
        }
        if (!c->session) { struct timespec delay={.tv_nsec=20000000}; nanosleep(&delay,NULL); }
    }
    disconnect_server(c);myvava_resolver_release(c->resolver);c->resolver=NULL; coap_cleanup(); return NULL;
}
struct myvava_client *myvava_client_start(const struct myvava_config *config) {
    if (!config->host || !config->device_id || !config->key_file || !myvava_id_valid(config->device_id) ||
        !*config->host || strlen(config->host)>253 || !config->port || config->port>65535 ||
        config->poll_ms<50 || config->poll_ms>MYVAVA_POLL_MAX) return NULL;
    struct myvava_client *c=calloc(1,sizeof(*c));
    if (!c) return NULL;
    c->status.enabled=true; strcpy(c->status.phase,"starting");
    strcpy(c->host,config->host); strcpy(c->identity,config->device_id);
    c->port=config->port; c->poll_ms=config->poll_ms; c->events=config->events; c->files=config->files; c->media=config->media;
    if (!myvava_key_read(config->key_file,c->key) || !myvava_random(c->boot,16) ||
        pthread_mutex_init(&c->mutex,NULL)) { free(c); return NULL; }
    if (pthread_create(&c->thread,NULL,network_thread,c)) {
        pthread_mutex_destroy(&c->mutex); memset(c,0,sizeof(*c)); free(c); return NULL;
    }
    return c;
}
void myvava_client_dispatch(struct myvava_client *c, myvava_execute_fn execute, void *arg) {
    if (!c) return;
    char command[512];
    pthread_mutex_lock(&c->mutex);
    if (!c->queued && !c->running) { pthread_mutex_unlock(&c->mutex); return; }
    bool continuing=c->running;
    c->queued=false; c->running=true; strcpy(command,c->command);
    bool expired=!continuing&&myvava_millis()>=c->execute_before;
    pthread_mutex_unlock(&c->mutex);
    size_t n;
    if (expired) {
        strcpy(c->result,"{\"ok\":false,\"error\":\"command_expired\"}\n"); n=strlen(c->result);
    } else n=execute(command,c->result,sizeof(c->result),arg);
    if(n==MYVAVA_EXECUTE_PENDING)return;
    if (!n || n>=sizeof(c->result)) {
        strcpy(c->result,"{\"ok\":false,\"error\":\"response_limit\"}\n"); n=strlen(c->result);
    }
    pthread_mutex_lock(&c->mutex);
    c->result_len=n; c->running=false; c->done=true;
    pthread_mutex_unlock(&c->mutex);
}
struct myvava_client_status myvava_client_status(struct myvava_client *c) {
    struct myvava_client_status result={0};
    if (!c) { strcpy(result.phase,"disabled"); return result; }
    pthread_mutex_lock(&c->mutex); result=c->status; pthread_mutex_unlock(&c->mutex);
    uint64_t now=myvava_millis();
    result.online=!strcmp(result.phase,"online") && result.last_response_ms && now-result.last_response_ms<15000;
    result.last_response_ms=result.last_response_ms?now-result.last_response_ms:UINT64_MAX;
    return result;
}
void myvava_client_stop(struct myvava_client *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mutex); c->stop=true; pthread_mutex_unlock(&c->mutex);
    pthread_join(c->thread,NULL); pthread_mutex_destroy(&c->mutex);
    memset(c,0,sizeof(*c)); free(c);
}
