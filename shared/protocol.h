#ifndef VAVA_PROTOCOL_H
#define VAVA_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define VAVA_CONTROL_SIZE 128
#define VAVA_PAYLOAD_SIZE 116
#define VAVA_MEDIA_HEADER 24
#define VAVA_MAX_FRAME (1024u * 1024u)
uint32_t vava_u32(const uint8_t *p);
uint64_t vava_u64(const uint8_t *p);
void vava_put32(uint8_t *p, uint32_t v);
uint32_t vava_crc32(const uint8_t *p, size_t n);
void vava_encode(uint8_t out[128], uint32_t command, const uint8_t payload[116]);
bool vava_decode(const uint8_t in[128], uint32_t *command);
bool vava_serial(const uint8_t field[32], char out[32]);
void vava_guest_digest(uint32_t seed, char out[33]);
bool vava_guest_valid(const uint8_t payload[116]);
struct vava_media {
    uint8_t stream, codec, type, flags;
    uint32_t length;
    uint64_t timestamp;
};
bool vava_media_header(const uint8_t in[24], struct vava_media *out);
bool vava_media_body(const struct vava_media *h, const uint8_t *p, size_t *length);
/* Downlink audio uses stream 2/type 8, independently of uplink streams 8/9. */
bool vava_talk_aac(const uint8_t *data, size_t n);
void vava_talk_header(uint8_t out[24], size_t n, uint8_t number, uint64_t ms);
#endif
