#ifndef MYVAVA_FILE_STORE_H
#define MYVAVA_FILE_STORE_H
#include "files.h"
#define FILE_STORE_JOBS 16
struct file_store;
struct file_store *file_store_start(const char *dir, uint64_t quota, unsigned limit);
struct file_store *file_store_start_encrypted(const char *dir,uint64_t quota,unsigned limit,const char *keyring);
bool file_store_encrypted(const struct file_store *);
/* Offline importer only: no concurrent submitters; store lock is held. */
bool file_store_mark_imported(struct file_store *,const char *device,const uint8_t descriptor[MYVAVA_FILE_DESC]);
void file_store_stop(struct file_store *);
/* Submit/poll/list only touch bounded memory. Cookie must remain unique until
 * poll consumes the result, even after its network session disappears. */
bool file_store_submit(struct file_store *, uint64_t cookie, const char *device, const struct myvava_message *);
bool file_store_poll(struct file_store *, uint64_t *cookie, struct myvava_message *, uint8_t [33]);
bool file_store_list(struct file_store *, const char *device, unsigned page, char *, size_t);
/* storage: empty/all, cloud, tf_export or legacy_import. Filter before paging. */
bool file_store_list_filtered(struct file_store *,const char *device,unsigned page,const char *storage,const char *serial,char *,size_t);
bool file_store_info_storage(struct file_store *,const char *device,uint64_t id,struct myvava_file *,const char **storage);
/* Worker-thread APIs. A successful acquire owns a read lease until release.
 * HTTP callers must authorize device/id before calling. Never use on CoAP loop. */
bool file_store_info(struct file_store *,const char *device,uint64_t id,struct myvava_file *);
int file_store_acquire(struct file_store *,const char *device,uint64_t id,struct myvava_file *);
int file_store_read(struct file_store *,int fd,const struct myvava_file *,uint32_t offset,void *,size_t);
void file_store_release(struct file_store *,const char *device,uint64_t id,int fd);
/* Durable deletion tombstone prevents a delayed uploader from resurrecting it.
 * Returns HTTP-style 200/404/409/503; refuses active read leases. */
int file_store_delete(struct file_store *,const char *device,uint64_t id);
void file_store_enable_retention(struct file_store *);
bool file_store_policy_json(struct file_store *,char *,size_t);
int file_store_policy_set(struct file_store *,unsigned revision,bool enabled,unsigned days);
#endif
