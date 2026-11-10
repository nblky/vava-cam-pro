#ifndef VAVA_UPLOAD_H
#define VAVA_UPLOAD_H
#include "files.h"
struct uploader;
bool upload_storage_ready(struct uploader *);
struct uploader *upload_start(const char *record_dir,const struct myvava_config *);
void upload_stop(struct uploader *);
const uint8_t *upload_target(struct uploader *);
const struct myvava_file_channel *upload_channel(struct uploader *);
void upload_policy(struct uploader *,unsigned slot,const char *serial,bool enabled);
bool upload_json(struct uploader *,unsigned slot,const char *serial,unsigned page,char *,size_t);
/* Explicit, idempotent copy of a local recording; independent of cloud policy.
 * export_id must have its high bit set, separating it from recording IDs. */
bool upload_export(struct uploader *,unsigned slot,const char *serial,uint64_t source_id,uint64_t export_id);
bool upload_preview(struct uploader *,unsigned slot,const char *serial,uint64_t source_id,uint64_t export_id);
bool upload_export_action(struct uploader *,unsigned slot,const char *serial,uint64_t export_id,bool cancel);
bool upload_exports_json(struct uploader *,unsigned slot,const char *serial,char *,size_t);
bool upload_export_forget(struct uploader *,unsigned slot,const char *serial,uint64_t export_id);
bool upload_rate(struct uploader *,unsigned kib_per_second);
unsigned upload_get_rate(struct uploader *);
bool upload_action(struct uploader *,unsigned slot,const char *serial,uint64_t id,const char *action);
#endif
