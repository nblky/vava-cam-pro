#ifndef MYVAVA_FILES_H
#define MYVAVA_FILES_H
#include "myvava.h"
#define MYVAVA_FILE_DESC 128
#define MYVAVA_FILE_DATA_MAX 736u /* Existing archive capsule format: immutable. */
#define MYVAVA_FILE_BATCH_SEGMENTS 32u
#define MYVAVA_FILE_BATCH_DATA_MAX (MYVAVA_FILE_DATA_MAX*MYVAVA_FILE_BATCH_SEGMENTS)
#define MYVAVA_FILE_PAYLOAD_MAX (32u+MYVAVA_FILE_BATCH_DATA_MAX)
#define MYVAVA_FILE_PACKET_MAX (MYVAVA_HEADER+MYVAVA_FILE_PAYLOAD_MAX)
/* Elective CoAP option: OFFER requests/ACKs advertise 32-segment batches.
 * Absence retains the original 736-byte DATA protocol. */
#define MYVAVA_FILE_BATCH_OPTION 65000u
#define MYVAVA_FILE_MAX (128u*1024u*1024u)
enum myvava_file_result { MVF_PARTIAL=0, MVF_COMMITTED=1, MVF_CONFLICT=2,
    MVF_QUOTA=3, MVF_IO=4, MVF_DIGEST=5, MVF_INVALID=6, MVF_DELETED=7 };
struct myvava_file {
    uint64_t id, unix_seconds, first_ms, last_ms;
    uint32_t size, frames, audio_frames, sample_rate;
    uint8_t sha256[32], channels, slot, stream, pir, clock_valid;
    uint8_t file_type, detection_flags; /* bit 0 reported, bit 1 positive, bit 2 preview. */
    uint32_t detection_type;
    char serial[32];
};
bool myvava_file_decode(const uint8_t *, size_t, struct myvava_file *);
bool myvava_file_encode(uint8_t [MYVAVA_FILE_DESC], const struct myvava_file *);
/* All callbacks are memory-only. File I/O runs on a separate worker. One
 * request may be outstanding; reset returns it to the worker for re-offer. */
struct myvava_file_channel {
    void *arg;
    bool (*peek)(void *, struct myvava_message *, uint8_t [MYVAVA_FILE_PAYLOAD_MAX]);
    void (*reply)(void *, const struct myvava_message *);
    void (*reset)(void *);
};
#endif
