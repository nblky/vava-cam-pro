#include "http.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
int http_parse(const uint8_t *data, size_t n, struct http_request *r) {
    if (n>=8192) return -1;
    size_t end=0;
    for (size_t i=0;i+3<n;++i) if (!memcmp(data+i,"\r\n\r\n",4)) { end=i+4; break; }
    if (!end) return 0;
    for (size_t i=0;i<n;++i) if (!data[i] || (data[i]<32 && data[i]!='\r' && data[i]!='\n' && data[i]!='\t') || data[i]>126) return -1;
    char b[8192]; memcpy(b,data,end); b[end]=0;
    memset(r,0,sizeof(*r)); char *next=strstr(b,"\r\n"); if (!next) return -1;
    *next=0; next+=2; char version[16],extra;
    if (sscanf(b,"%7s %255s %15s %c",r->method,r->path,version,&extra)!=3 || strcmp(version,"HTTP/1.1")) return -1;
    unsigned length=0; bool has_length=false,has_auth=false,has_host=false;
    bool has_type=false,has_cors_method=false,has_cors_headers=false;
    while (*next) {
        char *line=next; next=strstr(line,"\r\n"); if (!next) return -1;
        *next=0; next+=2; if (!*line) break;
        char *colon=strchr(line,':'); if (!colon || colon==line) return -1;
        for (char *p=line;p<colon;++p) if (!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || *p=='-')) return -1;
        *colon++=0; while (*colon==' ' || *colon=='\t') ++colon;
        char *tail=colon+strlen(colon); while (tail>colon && (tail[-1]==' ' || tail[-1]=='\t')) *--tail=0;
        if (!strcasecmp(line,"Content-Length")) {
            if (has_length || !*colon) return -1;
            has_length=true;
            for (char *p=colon;*p;++p) {
                if (*p<'0' || *p>'9' || length>204) return -1;
                length=length*10+(unsigned)(*p-'0'); if (length>=sizeof(r->body)) return -1;
            }
        } else if (!strcasecmp(line,"Authorization")) {
            if (has_auth || strlen(colon)>=sizeof(r->authorization)) return -1;
            has_auth=true; strcpy(r->authorization,colon);
        } else if (!strcasecmp(line,"Host")) {
            if (has_host || strlen(colon)>=sizeof(r->host)) return -1;
            has_host=true; strcpy(r->host,colon);
        } else if (!strcasecmp(line,"Origin")) {
            if (r->has_origin || !*colon || strlen(colon)>=sizeof(r->origin)) return -1;
            r->has_origin=true; strcpy(r->origin,colon);
        } else if (!strcasecmp(line,"Content-Type")) {
            if (has_type || strlen(colon)>=sizeof(r->content_type)) return -1;
            has_type=true; strcpy(r->content_type,colon);
        } else if (!strcasecmp(line,"Access-Control-Request-Method")) {
            if (has_cors_method || strlen(colon)>=sizeof(r->cors_method)) return -1;
            has_cors_method=true; strcpy(r->cors_method,colon);
        } else if (!strcasecmp(line,"Access-Control-Request-Headers")) {
            if (has_cors_headers || strlen(colon)>=sizeof(r->cors_headers)) return -1;
            has_cors_headers=true; strcpy(r->cors_headers,colon);
        } else if (!strcasecmp(line,"Transfer-Encoding") || !strcasecmp(line,"Expect")) return -1;
    }
    if (!has_host || !r->host[0]) return -1;
    if (!strcmp(r->method,"POST") && !has_length) return -1;
    if (n<end+length) return 0;
    if (n!=end+length) return -1;
    memcpy(r->body,data+end,length); r->body[length]=0;
    return 1;
}

bool http_origin_valid(const char *origin) {
    const char *host;
    if (!strncmp(origin,"http://",7)) host=origin+7;
    else if (!strncmp(origin,"https://",8)) host=origin+8;
    else return false;
    if (!*host || strlen(origin)>=160) return false;
    /* Exact browser-serialized origins. No paths, wildcards or userinfo. */
    for (const char *p=host;*p;++p)
        if (!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') ||
              *p=='.' || *p=='-' || *p==':' || *p=='[' || *p==']')) return false;
    return true;
}
bool http_same_origin(const struct http_request *r) {
    if (!r->has_origin) return true;
    char a[160],b[160]; snprintf(a,sizeof(a),"http://%s",r->host); snprintf(b,sizeof(b),"https://%s",r->host);
    return !strcmp(r->origin,a) || !strcmp(r->origin,b);
}
bool http_preflight_valid(const struct http_request *r) {
    if ((!strcmp(r->path,"/api/v1/commands") && strcmp(r->cors_method,"POST")) ||
        ((strcmp(r->path,"/api/v1/status")==0 || strcmp(r->path,"/api/v1/capabilities")==0) && strcmp(r->cors_method,"GET"))) return false;
    char headers[128]; strcpy(headers,r->cors_headers);
    char *save=NULL;
    for (char *s=strtok_r(headers,",",&save);s;s=strtok_r(NULL,",",&save)) {
        while (*s==' ' || *s=='\t') ++s;
        char *end=s+strlen(s);
        while (end>s && (end[-1]==' ' || end[-1]=='\t')) *--end=0;
        if (strcasecmp(s,"authorization") && strcasecmp(s,"content-type")) return false;
    }
    return !r->body[0] && r->has_origin;
}
