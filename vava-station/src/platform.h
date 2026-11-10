#ifndef VAVA_PLATFORM_H
#define VAVA_PLATFORM_H
#include <stdbool.h>
#include <stdint.h>
bool platform_init(const char *helper);
bool platform_managed(void);
bool platform_pair_open(const char *ssid, const char *password, unsigned seconds, uint64_t now);
void platform_pair_close(uint64_t now);
void platform_tick(uint64_t now);
uint64_t platform_pair_until(void);
const char *platform_state(void);
void platform_shutdown(uint64_t now);
#endif
