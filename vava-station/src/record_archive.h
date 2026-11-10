#ifndef VAVA_RECORD_ARCHIVE_H
#define VAVA_RECORD_ARCHIVE_H
#include "recorder.h"
/* Must be configured before recorder startup. Empty serial keeps legacy layout. */
bool record_archive_config(const char *serial,int offset_seconds);
bool record_archive_valid(const char *relative);
/* Anchored private directory walk, including same-device and no-symlink checks. */
int record_archive_parent(int root,const char *relative,bool create);
/* Empty result means an unrepresentable clock or a name collision: keep flat. */
bool record_archive_choose(int root,struct record_info *);
/* Derived IDX files are rebuilt from committed metadata, never trusted as a
 * deletion authority. Call under catalog lock with no concurrent file commit. */
bool record_archive_reconcile(int root,bool cleanup,uint64_t *bytes,uint64_t *removed);
/* Normal commits/deletes touch only the changed camera index and base index. */
bool record_archive_update(int root,const char *relative);
#endif
