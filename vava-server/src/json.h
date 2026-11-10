#ifndef MYVAVA_JSON_H
#define MYVAVA_JSON_H
#include <stdbool.h>
#include <stddef.h>
#define JSON_TOKENS 512
struct json_token { unsigned start,end,next,count; char type; };
struct json_doc { const char *text; unsigned count; struct json_token token[JSON_TOKENS]; };
/* Bounded strict JSON; object keys must be unique ASCII. Opaque string values
 * accept valid UTF-8; json_text extracts ASCII fields only. Camera names travel
 * as UTF-8 hex in commands and literal UTF-8 in task results. */
bool json_parse(struct json_doc *, const char *);
int json_field(const struct json_doc *, unsigned object, const char *);
bool json_text(const struct json_doc *, int, char *, size_t);
bool json_fields(const struct json_doc *, unsigned object, const char *const *, unsigned);
bool json_raw(const struct json_doc *, int, char *, size_t);
#endif
