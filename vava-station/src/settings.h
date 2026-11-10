#ifndef VAVA_SETTINGS_H
#define VAVA_SETTINGS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define SETTINGS_COUNT 10
#define SETTING_PIR_LEVEL 3
#define SETTING_PIR_MODE 4
#define SETTING_QUALITY 7
#define SETTING_DETECTION_BOX 9

enum setting_phase { SET_NONE, SET_QUEUED, SET_SENT, SET_ECHO, SET_TIMEOUT,
                     SET_DISCONNECTED, SET_QUEUE_TIMEOUT };
struct setting_operation {
    uint32_t id, value, wire_value;
    uint32_t pir[5];
    bool dedup_valid;
    bool policy_arming;
    enum setting_phase phase;
    uint64_t generation, queued_at, sent_at, echo_at, tx_end;
    uint8_t payload[116];
};
struct setting_report {
    bool seen;
    uint64_t at, generation;
    uint32_t values[10];
    bool zero_tail;
    bool matched_echo;
};
struct settings {
    unsigned active; /* index + 1, or zero */
    struct setting_operation operations[SETTINGS_COUNT];
    struct setting_report reports[6];
};
/* Scalar setters have a reviewed, bounded wire-value whitelist. */
int settings_index(const char *name, unsigned value, uint32_t *wire_value);
bool settings_detection_ready(const struct settings *, uint64_t generation);
/* Prepare mode/sensitivity/wait/record, with arming preserved from a valid,
 * current-connection report 42. This is not a station arming policy. */
/* Returns NULL on success, otherwise a stable error reason. */
const char *settings_pir_mode(const struct settings *s, const char *mode, unsigned sensitivity,
                             unsigned wait, unsigned record, uint64_t generation, uint32_t out[5]);
const char *settings_pir_parameters(const char *mode, unsigned sensitivity, unsigned wait,
                                    unsigned record, unsigned armed, uint32_t out[5]);
unsigned settings_record_seconds(const struct settings *,uint64_t generation,const char **source);
uint32_t settings_command(unsigned index);
const char *settings_name(unsigned index);
/* 0=new, 1=duplicate, -1=another pending operation. */
int settings_check(const struct settings *s, unsigned index, unsigned value,
                   uint64_t generation, bool retry);
int settings_check_pir(const struct settings *s, const uint32_t values[5], uint64_t generation, bool retry);
void settings_begin(struct settings *s, unsigned index, uint32_t value, uint32_t wire_value,
                    uint32_t id, unsigned slot, uint64_t generation, uint64_t now, uint64_t tx_end);
void settings_begin_pir(struct settings *s, const uint32_t values[5], uint32_t id, unsigned slot,
                        uint64_t generation, uint64_t now, uint64_t tx_end);
void settings_written(struct settings *s, uint64_t completed, uint64_t now);
void settings_disconnect(struct settings *s);
/* True requires closing the control socket to discard unsent setting bytes. */
bool settings_tick(struct settings *s, uint64_t now);
void settings_observe(struct settings *s, uint32_t command, const uint8_t payload[116],
                      uint64_t generation, uint64_t now);
bool settings_json(const struct settings *s, uint64_t generation, bool connected,
                   uint64_t now, char *out, size_t capacity);
#endif
