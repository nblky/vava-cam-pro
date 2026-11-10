#ifndef VAVA_HTTP_H
#define VAVA_HTTP_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
struct http_request {
    char method[8], path[256], authorization[256], body[2048];
    char host[128], origin[160], content_type[80], cors_method[16], cors_headers[128];
    bool has_origin;
};
/* One request per connection. No chunked body or pipelining. */
int http_parse(const uint8_t *data, size_t n, struct http_request *r);
bool http_origin_valid(const char *origin);
bool http_same_origin(const struct http_request *r);
bool http_preflight_valid(const struct http_request *r);
#endif
