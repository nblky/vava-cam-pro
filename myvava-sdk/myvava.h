#ifndef MYVAVA_H
#define MYVAVA_H
#include "events.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MYVAVA_COMMAND_MAX 511
#define MYVAVA_RESULT_MAX 32768
#define MYVAVA_CHUNK 768
#define MYVAVA_HEADER 56
#define MYVAVA_PACKET_MAX (MYVAVA_HEADER + MYVAVA_CHUNK)
/* Negotiated result batches use CoAP Block1 below the unchanged 1200-byte
 * DTLS MTU. COMMANDs and unnegotiated peers keep the original chunk limit. */
#define MYVAVA_CONTROL_BATCH_CHUNKS 8u
#define MYVAVA_CONTROL_BATCH_MAX (MYVAVA_CHUNK*MYVAVA_CONTROL_BATCH_CHUNKS)
#define MYVAVA_CONTROL_BATCH_OPTION 65004u
#define MYVAVA_ID_MAX 63
#define MYVAVA_POLL_MAX 5000
enum myvava_type { MYVAVA_SYNC=1, MYVAVA_IDLE=2, MYVAVA_COMMAND=3, MYVAVA_ACK=4, MYVAVA_EVENT=5, MYVAVA_EVENT_ACK=6,
    MYVAVA_FILE_OFFER=7, MYVAVA_FILE_DATA=8, MYVAVA_FILE_COMMIT=9, MYVAVA_FILE_ACK=10,
    MYVAVA_MEDIA_POLL=11, MYVAVA_MEDIA_CONTROL=12, MYVAVA_MEDIA_FRAME=13 };
struct myvava_message {
    uint8_t type, boot[16], epoch[16];
    uint64_t id;
    uint32_t total, offset;
    size_t length;
    const uint8_t *data;
    /* Local transport metadata, never serialized in the MVA1 header. */
    bool file_batch;
};
bool myvava_decode(const uint8_t *data, size_t size, struct myvava_message *out);
size_t myvava_encode(uint8_t out[MYVAVA_PACKET_MAX], const struct myvava_message *message);
/* Extended capacity is accepted only for file DATA and negotiated SYNC results. */
size_t myvava_encode_sized(uint8_t *, size_t capacity, const struct myvava_message *);
uint64_t myvava_millis(void);
bool myvava_random(uint8_t *out, size_t size);
bool myvava_unhex(const char *text, uint8_t *out, size_t size);
void myvava_hex(const uint8_t *data, size_t size, char *out);
bool myvava_id_valid(const char *id);
bool myvava_key_read(const char *path, uint8_t key[32]);
/* Inspect the loaded TLS backend without connecting or reading credentials.
 * NULL means DTLS 1.2 and PSK ciphers are available; otherwise a stable error. */
const char *myvava_transport_error(void);

struct myvava_client;
struct myvava_file_channel;
struct mvm_client;
struct myvava_config {
    const char *host, *device_id, *key_file;
    unsigned port, poll_ms;
    struct myvava_events *events;
    const struct myvava_file_channel *files;
    struct mvm_client *media;
};
/* The network worker never calls the device core. dispatch() runs a pending
 * command on the caller's device event loop and copies its bounded result. */
typedef size_t (*myvava_execute_fn)(const char *command, char *out, size_t capacity, void *arg);
/* A device-loop adapter may admit storage work to a bounded I/O queue. Return
 * this sentinel until its result is ready; subsequent dispatch calls poll the
 * same command. The adapter must not admit the mutation a second time. */
#define MYVAVA_EXECUTE_PENDING SIZE_MAX
struct myvava_client *myvava_client_start(const struct myvava_config *config);
void myvava_client_dispatch(struct myvava_client *client, myvava_execute_fn execute, void *arg);
struct myvava_client_status {
    bool enabled, online;
    uint64_t requests, responses, reconnects, event_acks, event_rejections, last_response_ms;
    char phase[16], last_error[48];
};
struct myvava_client_status myvava_client_status(struct myvava_client *client);
void myvava_client_stop(struct myvava_client *client);
#endif
