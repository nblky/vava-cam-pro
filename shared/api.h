#ifndef VAVA_API_H
#define VAVA_API_H
#include <stdbool.h>
#include <stddef.h>
/* Decode the bounded v1 JSON envelope into a single internal command.
 * This never invokes a shell. Error strings are stable API identifiers. */
const char *api_command(const char *json, char out[512]);
const char *api_capabilities(bool web_ui);
#endif
