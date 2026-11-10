#ifndef VAVA_REGISTRY_H
#define VAVA_REGISTRY_H
#include <stdbool.h>
#include <stdint.h>
#define VAVA_SLOTS 4
/* stage 1 is a volatile offer; 2 is a durable formal reply intent; 3 paired. */
struct registration { uint8_t stage; char serial[32]; char mac[18]; };
int registry_load(const char *dir, struct registration records[4]);
int registry_save(const char *dir, const struct registration records[4]);
bool registry_mac(const char *s);
#endif
