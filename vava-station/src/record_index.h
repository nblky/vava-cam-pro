#ifndef VAVA_RECORD_INDEX_H
#define VAVA_RECORD_INDEX_H
#include "recorder.h"
#define RECORD_CATALOG_MAX 65536u
/* Metadata files remain authoritative. This disposable disk index has compact
 * identity references and a fixed 32-row decoded cache, never a full RAM copy. */
bool record_index_open(const char *directory);
bool record_index_open_fd(int directory_fd);
void record_index_close(void);
bool record_index_add(const struct record_info *);
void record_index_sort(void);
unsigned record_index_count(void);
struct record_owner { unsigned slot,records;char serial[32]; };
unsigned record_index_owners(unsigned page,struct record_owner *out,unsigned capacity,unsigned *total);
bool record_index_identity(unsigned slot,const char *serial);
/* 1 found, 0 absent for this identity, -1 unreadable catalog row. */
int record_index_lookup(unsigned slot,const char *serial,uint64_t id,struct record_info *);
bool record_index_get(unsigned slot,const char *serial,uint64_t id,struct record_info *);
bool record_index_has(uint64_t id);
bool record_index_remove(uint64_t id);
unsigned record_index_page(unsigned slot,const char *serial,unsigned page,struct record_info *,unsigned capacity,unsigned *total);
unsigned record_index_query(unsigned slot,const char *serial,uint64_t before,uint64_t from,uint64_t to,struct record_info *,unsigned capacity,unsigned *total,bool *more);
unsigned record_index_scan(uint64_t before,struct record_info *,unsigned capacity);
unsigned record_index_after(uint64_t after,struct record_info *,unsigned capacity);
#endif
