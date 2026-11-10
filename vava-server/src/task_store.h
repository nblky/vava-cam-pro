#ifndef MYVAVA_TASK_STORE_H
#define MYVAVA_TASK_STORE_H
#include "myvava.h"
#include "events.h"

enum task_state { QUEUED, SENT, COMPLETE, EXPIRED, UNKNOWN };
struct task {
    uint64_t id, expires, created_at;
    uint8_t epoch[16], boot[16];
    char device[MYVAVA_ID_MAX+1], key[33], command[512], reason[64];
    enum task_state state;
    uint8_t *result;
    uint32_t result_size, received;
};
struct task_store;
struct task_store *task_store_open(const char *directory, unsigned history_limit);
void task_store_close(struct task_store *store);
void task_clear(struct task *task);
/* Reads: 1 found, 0 missing, -1 storage/schema error. */
int task_get(struct task_store *, const uint8_t epoch[16], uint64_t id, struct task *);
int task_find_key(struct task_store *, const char *device, const char *key, struct task *);
int task_pending(struct task_store *, const uint8_t epoch[16], const char *device, struct task *);
/* Insert: 1 committed, 0 full with protected requests, -1 storage error. */
int task_insert(struct task_store *, struct task *, const char *prefix, const char *previous);
bool task_dispatch(struct task_store *, struct task *);
bool task_save_result(struct task_store *, struct task *);
bool task_confirm_result(struct task_store *, const uint8_t epoch[16], const char *device,
                         const uint8_t boot[16], uint64_t id, uint32_t total);
bool task_expire(struct task_store *, const uint8_t epoch[16], uint64_t now);
bool task_new_boot(struct task_store *, const uint8_t epoch[16], const char *device, const uint8_t boot[16]);
bool task_stats(struct task_store *, uint64_t *count, uint64_t *pruned);
/* Metadata only, newest first; exclusive before ID (zero = newest). */
int task_history(struct task_store *, const char *device, uint64_t before, struct task *out, unsigned capacity);
/* Event commit and stream cursor advance are one transaction. Cursors outlive
 * history pruning, so a restored spool cannot create duplicate history rows. */
struct stored_event { uint64_t id, received_at, gap; struct myvava_event event; unsigned clip_state; };
int event_store_put(struct task_store *, const char *device, const uint8_t payload[MYVAVA_EVENT_SIZE], unsigned limit);
int event_store_history(struct task_store *, const char *device, uint64_t before, struct stored_event *, unsigned capacity);
int event_store_filtered(struct task_store *,const char *device,uint64_t before,const char *category,const char *serial,struct stored_event *,unsigned capacity);
bool event_store_stats(struct task_store *, uint64_t *retained, uint64_t *pruned, uint64_t *streams);
#endif
