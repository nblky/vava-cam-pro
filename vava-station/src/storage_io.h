#ifndef VAVA_STORAGE_IO_H
#define VAVA_STORAGE_IO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct uploader;struct nas;struct storage_io;
struct storage_io *storage_io_start(struct uploader *,struct nas *);
void storage_io_stop(struct storage_io *);
bool storage_io_command(const char *name,bool station);
uint64_t storage_io_submit(struct storage_io *,unsigned slot,const char *serial,unsigned count,char **words);
/* 0 pending, 1 copied terminal result, -1 unknown/output capacity. */
int storage_io_poll(struct storage_io *,uint64_t id,char *out,size_t capacity,size_t *length);
void storage_io_abandon(struct storage_io *,uint64_t id);
#endif
