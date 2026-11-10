#ifndef VAVA_PAIRING_H
#define VAVA_PAIRING_H
#include "registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum pair_phase {PAIR_IDLE,PAIR_OPENING,PAIR_WAITING,PAIR_CREDENTIALS,PAIR_FORMAL,PAIR_CONFIRMING,PAIR_COMPLETE,PAIR_CANCELLED,PAIR_EXPIRED,PAIR_INTERRUPTED,PAIR_FAILED};
bool pairing_init(const char *directory,const struct registration [4]);
bool pairing_active(void);
bool pairing_accepts(const char *serial);
bool pairing_begin(unsigned seconds,uint64_t now,uint64_t unix_ms);
bool pairing_change(enum pair_phase,int slot,const char *serial,const char *reason,uint64_t now,unsigned remaining_seconds);
void pairing_tick(uint64_t now,bool managed,const char *ap_state,uint64_t until);
bool pairing_json(char *,size_t,uint64_t now);
uint64_t pairing_id(void);
#endif
