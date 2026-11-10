#ifndef MYVAVA_MEDIA_H
#define MYVAVA_MEDIA_H
#include "myvava.h"
#define MVM_FRAME_MAX (256u*1024u)
#define MVM_FRAGMENT 736
#define MVM_CONTROL 192
#define MVM_TALK_MAX 512
#define MVM_LEASE_MS 2500
#define MVM_FRAMING_OPTION 65002u
#define MVM_FRAMING_VERSION 2u
struct mvm_lease {uint64_t id;char serial[32];bool talk;unsigned framing;};
struct mvm_talk {uint64_t lease;uint32_t sequence;unsigned slot;size_t length;uint8_t data[MVM_TALK_MAX];};
struct mvm_client;
struct mvm_client *mvm_client_new(void);
void mvm_client_free(struct mvm_client *);
void mvm_client_reset(struct mvm_client *);
bool mvm_client_active(struct mvm_client *);
/* These are bounded memory operations; network and camera loop never call one another. */
bool mvm_client_control(struct mvm_client *,const uint8_t *,size_t);
bool mvm_client_control_version(struct mvm_client *,const uint8_t *,size_t,bool);
void mvm_client_poll(struct mvm_client *,struct myvava_message *);
void mvm_client_snapshot(struct mvm_client *,struct mvm_lease [4],struct mvm_talk *);
void mvm_client_frame(struct mvm_client *,unsigned slot,uint64_t lease,uint64_t generation,
    bool audio,bool key,uint64_t timestamp,const uint8_t *,size_t,const uint8_t *,size_t,const uint8_t *,size_t);
void mvm_client_frame_epoch(struct mvm_client *,unsigned slot,uint64_t lease,uint64_t generation,uint32_t epoch,
    bool audio,bool key,uint64_t timestamp,const uint8_t *,size_t,const uint8_t *,size_t,const uint8_t *,size_t);
bool mvm_client_peek(struct mvm_client *,struct myvava_message *,uint8_t [MYVAVA_CHUNK]);
uint64_t mvm_u64(const uint8_t *);
void mvm_p64(uint8_t *,uint64_t);
#endif
