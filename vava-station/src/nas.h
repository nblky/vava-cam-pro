#ifndef VAVA_NAS_H
#define VAVA_NAS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct nas_config { const char *host,*export_path,*mount_dir,*station_id,*executable; bool lab_local; };
struct nas;
bool nas_storage_ready(struct nas *);
struct nas *nas_start(const char *record_dir,const char *state_dir,const struct nas_config *);
void nas_stop(struct nas *);
bool nas_target(struct nas *,uint8_t out[32]);
bool nas_status(struct nas *,char *,size_t);
const char *nas_configure(struct nas *,uint32_t revision,bool enabled,const char *host,const char *path);
const char *nas_control(struct nas *,uint32_t revision,const char *action);
void nas_policy(struct nas *,unsigned slot,const char *serial,bool enabled);
bool nas_json(struct nas *,unsigned slot,const char *serial,unsigned page,char *,size_t);
bool nas_action(struct nas *,unsigned slot,const char *serial,uint64_t id,const char *action);
/* Invoked only by the same binary's private --nas-copy-worker entry point. */
int nas_copy_main(int argc,char **argv);
#endif
