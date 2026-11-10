#ifndef MYVAVA_MEDIA_STORE_H
#define MYVAVA_MEDIA_STORE_H
#include "media.h"
struct mvm_store;
struct mvm_store *mvm_store_new(void);
void mvm_store_free(struct mvm_store *);
/* An all-zero boot revokes the station and releases its tracking entry. */
void mvm_store_device(struct mvm_store *,const char *device,const uint8_t boot[16]);
/* One exclusive viewer/talker per camera; no durable replay of live demand. */
uint64_t mvm_store_open(struct mvm_store *,const char *device,const uint8_t boot[16],unsigned slot,const char *serial,bool talk,const char *owner);
struct mvm_sample {uint64_t generation,sequence,timestamp;uint32_t epoch;bool audio,key;size_t size;uint8_t data[];};
uint64_t mvm_store_open_frames(struct mvm_store *,const char *,const uint8_t [16],unsigned,const char *,bool,const char *);
int mvm_store_read_sample(struct mvm_store *,uint64_t,struct mvm_sample *,size_t);
int mvm_store_owner(struct mvm_store *,const char *,uint64_t,const char *,unsigned *,char [32]);
size_t mvm_store_control_version(struct mvm_store *,const char *,const struct myvava_message *,uint8_t [MYVAVA_CHUNK],bool);
void mvm_store_close(struct mvm_store *,uint64_t lease);
int mvm_store_read(struct mvm_store *,uint64_t lease,void *,size_t);
int mvm_store_talk(struct mvm_store *,const char *device,uint64_t lease,const char *owner,const uint8_t *,size_t);
size_t mvm_store_control(struct mvm_store *,const char *device,const struct myvava_message *,uint8_t [MYVAVA_CHUNK]);
bool mvm_store_frame(struct mvm_store *,const char *device,const struct myvava_message *);
#endif
