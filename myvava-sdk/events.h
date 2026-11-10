#ifndef MYVAVA_EVENTS_H
#define MYVAVA_EVENTS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MYVAVA_EVENT_SIZE 160
#define MYVAVA_EVENT_QUEUE 64
enum myvava_event_kind {
    MVE_BOOT=1, MVE_PAIR_STAGE, MVE_DETECTION, MVE_PIR_WAKE,
    MVE_RECORD_START, MVE_RECORD_STOP_REQUEST, MVE_GAP,
    MVE_RECORD_COMMITTED, MVE_RECORD_FAILED, MVE_RECORD_RECOVERED
};
struct myvava_event {
    uint8_t origin[16];
    uint64_t sequence, monotonic_ms, unix_ms, generation, related_id, dropped;
    uint32_t result_raw, type_raw;
    uint8_t slot, kind;
    bool clock_valid;
    char serial[32], detail[48];
};
bool myvava_event_decode(const uint8_t *, size_t, struct myvava_event *);
void myvava_event_encode(uint8_t out[MYVAVA_EVENT_SIZE], const struct myvava_event *);
const char *myvava_event_name(unsigned kind);
const char *myvava_event_category(unsigned kind);
size_t myvava_event_json(const struct myvava_event *, char *, size_t);

struct myvava_events;
struct myvava_event_status {
    bool enabled, ready;
    unsigned retained, queued, limit;
    uint64_t dropped, committed, acknowledged;
    char error[64];
};
/* Directory must already exist on the chosen data volume. All spool I/O runs
 * on a dedicated worker; emit/peek/ack/status never perform filesystem I/O. */
struct myvava_events *myvava_events_start(const char *directory, unsigned limit);
struct myvava_events *myvava_events_start_guarded(const char *directory,unsigned limit,const char *mount);
typedef void (*myvava_event_durable_fn)(const struct myvava_event *,void *);
void myvava_events_on_durable(struct myvava_events *,myvava_event_durable_fn,void *);
bool myvava_events_emit(struct myvava_events *, const struct myvava_event *);
bool myvava_events_peek(struct myvava_events *, uint8_t out[MYVAVA_EVENT_SIZE]);
void myvava_events_ack(struct myvava_events *, const uint8_t origin[16], uint64_t sequence);
struct myvava_event_status myvava_events_status(struct myvava_events *);
void myvava_events_stop(struct myvava_events *);
#endif
