#ifndef VAVA_LIVE_MUX_H
#define VAVA_LIVE_MUX_H
#include "media_store.h"
/* HTTP record: MVL2, kind, flags, zero16, BE epoch32, BE length32. */
#define LIVE_CONFIG 1
#define LIVE_INIT 2
#define LIVE_MEDIA 3
#define LIVE_STATE 4
typedef bool (*live_emit)(void *,unsigned,uint32_t,const uint8_t *,size_t);
struct live_mux;
bool live_mux_available(void);
struct live_mux *live_mux_new(live_emit,void *);
bool live_mux_push(struct live_mux *,const struct mvm_sample *);
void live_mux_free(struct live_mux *);
#endif
