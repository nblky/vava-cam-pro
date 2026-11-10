#ifndef VAVA_DESIRED_H
#define VAVA_DESIRED_H
#include "registry.h"
#include "settings.h"
/* Durable operator preferences. Arming is owned exclusively by policy.c.
 * Applying is explicit; loading this file never schedules camera Flash writes. */
struct desired { uint32_t revision, mask, values[SETTINGS_COUNT][5]; };
bool desired_valid(const struct desired *d);
int desired_load(const char *dir, const struct registration r[4], struct desired d[4]);
int desired_save(const char *dir, const struct registration r[4], const struct desired d[4]);
bool desired_json(const struct desired *d, unsigned pending, const char *delivery, char *out, size_t size);
#endif
