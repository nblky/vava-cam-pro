#ifndef VAVA_RECORDER_H
#define VAVA_RECORDER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define RECORD_LIMIT 128
struct record_info {
    uint64_t id, generation, unix_seconds, bytes, stored_bytes, frames, audio_frames, first_ms, last_ms;
    unsigned slot, stream;
    unsigned sample_rate, channels;
    uint32_t detection_result, detection_type;
    uint8_t file_type; /* App archive types: 0 unknown, 5 movement, 6 human, 7 face, 8 human+face. */
    bool detection_reported, preview; /* preview is a transient upload view, never a TF recording. */
    bool pir, active, finishing, failed, transport_stream, clock_valid, encrypted;
    uint8_t sha256[32], upload_target[32], nas_target[32];
    char serial[32], error[40];
    char archive[80]; /* Persisted relative media path; empty for legacy/exception files. */
};
/* Configure before init. The keyring is immutable during this process. */
bool recorder_keys(const char *directory);
const char *recorder_keyring_path(void);
bool recorder_encrypted(void);
const char *recorder_key_id(void);
bool recorder_init(const char *dir, uint64_t quota);
/* Starts control independently of removable media. The worker attaches and
 * rescans when the private directory appears. A mount guard requires Linux. */
bool recorder_init_guarded(const char *dir,uint64_t quota,const char *mount);
/* Internal-flash policy; initialized before recorder startup. Defaults are
 * cyclic quota retention, 30 days, and 72 hours of pending-target protection. */
bool recorder_retention_init(const char *state_dir,bool enabled);
bool recorder_retention_json(char *,size_t);
const char *recorder_retention_config(unsigned revision,bool enabled,unsigned days,unsigned pending_hours);
void recorder_clock(bool valid);
bool recorder_hold(unsigned slot,const char *serial,uint64_t id,uint64_t until);
uint64_t recorder_storage_generation(void);
/* Per-thread queued command guard; zero clears. Never spans unrelated work. */
void recorder_expect_generation(uint64_t generation);
void recorder_shutdown(void);
unsigned recorder_snapshot(struct record_info *,unsigned capacity);
unsigned recorder_snapshot_before(uint64_t before,struct record_info *,unsigned capacity);
unsigned recorder_snapshot_generation(uint64_t before,struct record_info *,unsigned capacity,uint64_t *generation);
bool recorder_info(unsigned slot,const char *serial,uint64_t id,struct record_info *);
bool recorder_available(void);
/* Owned duplicate anchored to the current medium; caller closes it. */
int recorder_directory_fd(void);
bool recorder_start(unsigned slot, unsigned stream, uint64_t generation, const char *serial, bool pir, bool clock_valid, const uint8_t *upload_target, const uint8_t *nas_target, uint64_t *id);
bool recorder_data(unsigned slot, const void *data, size_t n, bool frame);
bool recorder_frame(unsigned slot, bool audio, bool keyframe, uint64_t timestamp_ms,
                    const void *data, size_t n, const void *sps, size_t sps_n, const void *pps, size_t pps_n);
/* Called by the storage worker after commit/rollback, outside the mutex.
 * Registered before init; recovered catalog rows are reported during init. */
typedef bool (*record_complete_fn)(const struct record_info *, bool recovered, void *);
void recorder_on_complete(record_complete_fn, void *);
void recorder_event_durable(unsigned slot,const char *serial,uint64_t id,uint64_t generation);
const char *recorder_format(const struct record_info *);
void recorder_finish(unsigned slot, const char *reason);
void recorder_abort(unsigned slot, const char *reason);
struct record_info recorder_state(unsigned slot);
/* Memory-only; the final metadata commit persists the report with its clip. */
void recorder_detection(unsigned slot,uint64_t id,uint64_t generation,uint32_t result,uint32_t type);
bool recorder_list(unsigned slot, const char *serial, unsigned page, char *out, size_t size);
bool recorder_identity(unsigned slot,const char *serial);
bool recorder_cameras(unsigned page,char *out,size_t size);
const char *recorder_query(unsigned slot,const char *serial,uint64_t before,uint64_t generation,uint64_t from,uint64_t to,char *out,size_t size);
const char *recorder_query_all(uint64_t before,uint64_t generation,uint64_t from,uint64_t to,char *out,size_t size);
bool recorder_info_json(unsigned slot,const char *serial,uint64_t id,char *out,size_t size);
/* NULL means deletion and both index updates are durable. */
const char *recorder_delete_result(unsigned slot,const char *serial,uint64_t id);
bool recorder_delete(unsigned slot, const char *serial, uint64_t id);
bool recorder_path(unsigned slot, const char *serial, uint64_t id, char *out, size_t size);
/* At most four readers protect immutable committed files from deletion. Call
 * close on every exit path. Reads are positional, <=64 KiB, with a 60s lease;
 * open/read/close perform I/O and belong on an I/O worker in new consumers. */
struct record_reader { uint64_t token, deadline_ms, storage_generation; int fd; struct record_info info; struct seal_reader *sealed; };
bool recorder_reader_open(unsigned slot, const char *serial, uint64_t id, struct record_reader *);
int recorder_reader_read(struct record_reader *, uint64_t offset, void *, size_t);
void recorder_reader_close(struct record_reader *);
/* Bounded authenticated read of the first complete IDR PES; no camera I/O. */
bool recorder_preview(unsigned slot,const char *serial,uint64_t id,struct record_info *prefix,struct record_info *source);
bool recorder_cleanup(uint64_t *removed_bytes);
struct storage_summary { uint64_t bytes, quota, generation; size_t queued; unsigned records, active; bool available; char error[48]; };
struct storage_summary recorder_summary(void);
#endif
