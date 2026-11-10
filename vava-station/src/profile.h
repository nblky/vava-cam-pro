#ifndef VAVA_PROFILE_H
#define VAVA_PROFILE_H
#include "registry.h"
#include <stddef.h>
#define PROFILE_NAME_BYTES 96
struct profile { uint32_t revision; char name[PROFILE_NAME_BYTES+1]; };
/* UTF-8 display name only; never used as a filename or camera protocol value. */
bool profile_name_valid(const char *name);
bool profile_from_hex(const char *hex, char name[PROFILE_NAME_BYTES+1]);
bool profile_json(const struct profile *p, char *out, size_t capacity);
int profile_load(const char *dir, const struct registration r[4], struct profile p[4]);
int profile_save(const char *dir, const struct registration r[4], const struct profile p[4]);
#endif
