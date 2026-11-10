#ifndef VAVA_REMOVAL_H
#define VAVA_REMOVAL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum removal_phase { REMOVE_IDLE, REMOVE_WAITING, REMOVE_DEFAULTS_SENT, REMOVE_UNPAIR_SENT,
    REMOVE_ACKNOWLEDGED, REMOVE_COMPLETE, REMOVE_FAILED, REMOVE_UNKNOWN };
bool removal_init(const char *directory);
bool removal_begin(unsigned slot,const char *serial,uint64_t now,uint64_t wall,bool defaults_only,uint32_t revision);
bool removal_change(enum removal_phase phase,const char *reason,uint64_t now);
bool removal_confirm(uint64_t now);
enum removal_phase removal_phase(void);
bool removal_active(void);
bool removal_blocks(unsigned slot,const char *serial);
unsigned removal_slot(void);
const char *removal_serial(void);
uint64_t removal_id(void);
uint32_t removal_token(void);
uint64_t removal_deadline(void);
bool removal_operator_confirmed(void);
bool removal_defaults_only(void);
uint32_t removal_settings_revision(void);
bool removal_json(char *out,size_t size,uint64_t now);
#endif
