#ifndef VAVA_AUTH_H
#define VAVA_AUTH_H
#include <stdbool.h>
#include <stddef.h>
bool auth_read(const char *path, char *out, size_t capacity, size_t minimum);
bool auth_equal(const char *a, const char *b);
void auth_base64(char *out, const char *in);
#endif
