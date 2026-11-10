#ifndef VAVA_NAS_CONTROL_H
#define VAVA_NAS_CONTROL_H
#include "nas.h"
struct nas_control;
struct nas_lease {struct nas_config config;char host[254],export_path[512];uint32_t revision;};
struct nas_control *nas_control_start(const char *state_dir,const struct nas_config *);
void nas_control_stop(struct nas_control *);
bool nas_control_target(struct nas_control *,uint8_t out[32]);
bool nas_control_acquire(struct nas_control *,struct nas_lease *);
bool nas_control_valid(struct nas_control *,const struct nas_lease *);
void nas_control_release(struct nas_control *);
bool nas_control_json(struct nas_control *,char *,size_t);
const char *nas_control_configure(struct nas_control *,uint32_t revision,bool enabled,const char *host,const char *export_path);
const char *nas_control_action(struct nas_control *,uint32_t revision,const char *action);
bool nas_config_valid(const struct nas_config *);
int nas_admin_main(int argc,char **argv);
#endif
