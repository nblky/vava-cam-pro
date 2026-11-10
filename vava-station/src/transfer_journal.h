#ifndef VAVA_TRANSFER_JOURNAL_H
#define VAVA_TRANSFER_JOURNAL_H
#include "files.h"
struct transfer_job {struct myvava_file file;uint8_t target[32];unsigned state;uint32_t offset;uint64_t retry_at;char error[40];};
struct transfer_journal;
/* Caller serializes access. Each atomic private row retains source identity even
 * after TF deletion. 65536 history rows; explicit terminal forgetting reclaims. */
struct transfer_journal *transfer_journal_open(int parent);
void transfer_journal_close(struct transfer_journal *);
unsigned transfer_journal_count(struct transfer_journal *);
bool transfer_journal_put(struct transfer_journal *,const struct transfer_job *);
int transfer_journal_get(struct transfer_journal *,uint64_t id,struct transfer_job *);
unsigned transfer_journal_page(struct transfer_journal *,unsigned slot,const char *serial,unsigned page,struct transfer_job *,unsigned cap,unsigned *total);
bool transfer_journal_delete(struct transfer_journal *,uint64_t id);
unsigned transfer_journal_scan(struct transfer_journal *,uint64_t before,struct transfer_job *,unsigned cap);
#endif
