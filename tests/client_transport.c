/* Exercise the real libcoap callbacks with controlled response age and NACKs.
 * No wall-clock sleep, production test hooks, vendor services or packet mocks. */
#include "../myvava-sdk/client.c"
#undef NDEBUG
#include <assert.h>
#include <arpa/inet.h>

static void file_reset(void *arg){++*(unsigned *)arg;}
static coap_pdu_t *packet(coap_pdu_type_t type,coap_pdu_code_t code,const uint8_t token[8]){
    coap_pdu_t *p=coap_pdu_init(type,code,1,1200);assert(p);assert(coap_add_token(p,8,token));return p;
}
int main(void){
    struct myvava_client c={0};unsigned resets=0;
    struct myvava_file_channel files={.arg=&resets,.reset=file_reset};
    assert(!pthread_mutex_init(&c.mutex,NULL));c.files=&files;c.media=mvm_client_new();assert(c.media);
    strcpy(c.identity,"TRANSPORT_TEST");memset(c.key,7,sizeof(c.key));
    c.addresses.count=1;c.addresses_until=myvava_millis()+60000;
    struct sockaddr_in peer={.sin_family=AF_INET,.sin_port=htons(3731)};
    assert(inet_pton(AF_INET,"127.0.0.1",&peer.sin_addr)==1);
    memcpy(&c.addresses.peers[0].address,&peer,sizeof(peer));c.addresses.peers[0].size=sizeof(peer);
    coap_startup();assert(connect_server(&c)==1);
    assert(myvava_transport_error()==NULL);
    /* Initial handshake failure can arrive before session creation returns.
     * The context already owns the client; do not silently lose that error. */
    coap_session_set_app_data(c.session,NULL);
    transport_event(c.session,COAP_EVENT_DTLS_ERROR);
    assert(c.reset&&!strcmp(c.status.last_error,"dtls_handshake_failed"));
    c.reset=false;
    nack(c.session,NULL,COAP_NACK_TLS_LAYER_FAILED,0);
    assert(c.reset&&!strcmp(c.status.last_error,"dtls_or_transport_nack"));
    c.reset=false;coap_session_set_app_data(c.session,&c);
    /* Control/event, file and media each have one bounded outstanding CON. */
    assert(coap_session_get_nstart(c.session)==3);
    uint8_t batch_data[MYVAVA_CONTROL_BATCH_MAX+1],batch_packet[MYVAVA_HEADER+sizeof(batch_data)];
    memset(batch_data,1,sizeof(batch_data));struct myvava_message batch={.type=MYVAVA_SYNC,.length=MYVAVA_CONTROL_BATCH_MAX,.data=batch_data},decoded;
    size_t batch_n=myvava_encode_sized(batch_packet,sizeof(batch_packet),&batch);
    assert(batch_n==MYVAVA_HEADER+MYVAVA_CONTROL_BATCH_MAX&&myvava_decode(batch_packet,batch_n,&decoded));
    assert(decoded.length==MYVAVA_CONTROL_BATCH_MAX&&!memcmp(decoded.data,batch_data,decoded.length));
    batch_packet[54]=MYVAVA_COMMAND;assert(!myvava_decode(batch_packet,batch_n,&decoded));
    batch.type=MYVAVA_COMMAND;assert(!myvava_encode_sized(batch_packet,sizeof(batch_packet),&batch));
    batch.type=MYVAVA_SYNC;batch.length++;assert(!myvava_encode_sized(batch_packet,sizeof(batch_packet),&batch));
    memset(c.media_token,1,8);memset(c.file_token,2,8);memset(c.token,3,8);
    uint8_t control[MVM_CONTROL]={0};mvm_p64(control,42);strcpy((char *)control+8,"CAMERA_1");
    assert(mvm_client_control(c.media,control,sizeof(control)));assert(mvm_client_active(c.media));
    c.pending=c.file_pending=c.media_pending=true;c.media_started=myvava_millis();
    /* Final result ACKs confirm receipt immediately; only subsequent IDLEs
     * wait. The short follow-up budget must expire, and never slow an already
     * faster configured poll. No duplicate command execution is involved. */
    c.poll_ms=1000;c.command_id=42;c.result_len=20;
    struct myvava_message ack={.type=MYVAVA_ACK,.id=42,.total=20,.offset=20};
    uint8_t ack_bytes[MYVAVA_PACKET_MAX];size_t ack_size=myvava_encode(ack_bytes,&ack);
    coap_pdu_t *ack_reply=packet(COAP_MESSAGE_ACK,COAP_RESPONSE_CODE_CONTENT,c.token);
    const uint8_t control_chunks=MYVAVA_CONTROL_BATCH_CHUNKS;
    assert(!c.control_batch&&coap_add_option(ack_reply,MYVAVA_CONTROL_BATCH_OPTION,1,&control_chunks));
    assert(coap_add_data(ack_reply,ack_size,ack_bytes));
    assert(response(c.session,NULL,ack_reply,1)==COAP_RESPONSE_OK);coap_delete_pdu(ack_reply);
    assert(!c.pending&&!c.reset&&c.result_offset==20&&c.next_request==0&&c.control_batch);
    assert(control_poll_delay(&c,c.control_active_until-1)==100);
    assert(control_poll_delay(&c,c.control_active_until)==1000);
    c.poll_ms=50;assert(control_poll_delay(&c,c.control_active_until-1)==50);c.poll_ms=1000;
    ack=(struct myvava_message){.type=MYVAVA_IDLE};ack_size=myvava_encode(ack_bytes,&ack);
    ack_reply=packet(COAP_MESSAGE_ACK,COAP_RESPONSE_CODE_CONTENT,c.token);c.pending=true;
    assert(coap_add_data(ack_reply,ack_size,ack_bytes));
    uint64_t before_idle=myvava_millis();
    assert(response(c.session,NULL,ack_reply,1)==COAP_RESPONSE_OK);coap_delete_pdu(ack_reply);
    assert(!c.pending&&!c.reset&&!c.control_batch&&c.next_request>=before_idle+100&&c.next_request<=myvava_millis()+100);
    clear_command(&c);assert(!c.control_active_until);c.pending=true;
    /* The loop samples now before sending the poll, so the request's start
     * timestamp can be a little newer. Unsigned subtraction must not expire it. */
    expire_media(&c,c.media_started-1);assert(mvm_client_active(c.media));
    expire_media(&c,c.media_started+MVM_LEASE_MS+1);
    assert(!mvm_client_active(c.media)&&c.pending&&c.file_pending&&c.media_pending&&!c.reset&&!resets);
    /* A delayed valid response cannot renew an old viewer lease. */
    struct myvava_message m={.type=MYVAVA_MEDIA_CONTROL,.length=sizeof(control),.data=control};
    uint8_t bytes[MYVAVA_PACKET_MAX];size_t n=myvava_encode(bytes,&m);
    coap_pdu_t *reply=packet(COAP_MESSAGE_ACK,COAP_RESPONSE_CODE_CONTENT,c.media_token);
    assert(coap_add_data(reply,n,bytes));c.media_started=myvava_millis()-MVM_LEASE_MS-1;
    assert(response(c.session,NULL,reply,1)==COAP_RESPONSE_OK);coap_delete_pdu(reply);
    assert(!c.media_pending&&!mvm_client_active(c.media)&&c.pending&&c.file_pending&&!c.reset&&!resets);
    /* Exhausting media retries closes that lease only. File retries restart
     * only the durable file transaction. Neither destroys a healthy session. */
    assert(mvm_client_control(c.media,control,sizeof(control)));c.media_pending=true;
    coap_pdu_t *sent=packet(COAP_MESSAGE_CON,COAP_REQUEST_CODE_POST,c.media_token);
    nack(c.session,sent,COAP_NACK_TOO_MANY_RETRIES,1);coap_delete_pdu(sent);
    assert(!c.media_pending&&!mvm_client_active(c.media)&&c.pending&&c.file_pending&&!c.reset&&!resets);
    assert(mvm_client_control(c.media,control,sizeof(control)));
    sent=packet(COAP_MESSAGE_CON,COAP_REQUEST_CODE_POST,c.file_token);
    nack(c.session,sent,COAP_NACK_TOO_MANY_RETRIES,1);coap_delete_pdu(sent);
    assert(!c.file_pending&&c.pending&&!c.reset&&resets==1&&mvm_client_active(c.media));
    /* A control exchange failure still requests connection recovery. */
    sent=packet(COAP_MESSAGE_CON,COAP_REQUEST_CODE_POST,c.token);
    nack(c.session,sent,COAP_NACK_TOO_MANY_RETRIES,1);coap_delete_pdu(sent);assert(c.reset);
    disconnect_server(&c);mvm_client_free(c.media);pthread_mutex_destroy(&c.mutex);coap_cleanup();
    puts("bounded interactive polling, concurrent exchanges, expired response and NACK isolation passed");return 0;
}
