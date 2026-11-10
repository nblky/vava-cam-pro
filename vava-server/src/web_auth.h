#ifndef MYVAVA_WEB_AUTH_H
#define MYVAVA_WEB_AUTH_H
#include <stdbool.h>
#include <stddef.h>
struct web_auth;
enum web_role { WEB_VIEWER=1, WEB_OPERATOR=2, WEB_ADMIN=3 };
struct web_user { char name[64], devices[1024]; enum web_role role; };
struct web_auth *web_auth_open(const char *state,const char *bootstrap);
void web_auth_close(struct web_auth *);
/* Tokens are 256-bit random secrets, stored only as SHA256 hashes. */
bool web_auth_session(struct web_auth *,const char *token,struct web_user *);
int web_auth_login(struct web_auth *,const char *name,const char *password,char token[65],struct web_user *);
bool web_auth_logout(struct web_auth *,const char *token);
int web_auth_password(struct web_auth *,const struct web_user *,const char *old,const char *password);
int web_auth_user_set(struct web_auth *,const char *name,const char *password,enum web_role,const char *devices,bool disabled);
bool web_auth_users(struct web_auth *,char *,size_t);
bool web_auth_allowed(const struct web_user *,const char *device);
bool web_auth_mark(struct web_auth *,const struct web_user *,const char *device,const char *event);
bool web_auth_cursor(struct web_auth *,const struct web_user *,const char *device,char out[17]);
#endif
