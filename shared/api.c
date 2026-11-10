#include "api.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define STATION_COMMANDS "status", "list", "capabilities", "events", "clock-valid", "storage-cleanup", "storage-policy", "storage-policy-set", "pair-open", "pair-close", "upload-rate", "nas-status", "nas-config", "nas-control", "record-cameras", "record-query-all"
#define CAMERA_COMMANDS "settings", "policy", "live-quality", "name", "forget", "remove", "remove-confirm", "reset", "configure", "configure-many", "configure-pir", \
    "apply-settings", "clear-settings", "policy-config", "arm", "enable", "plan", "timezone", \
    "record-duration", "idle-sleep", "set", "pir-mode", "live", "stop", "wake", "sleep", "iframe", \
    "record", "record-info","record-query","record-list", "record-get", "record-delete", "snapshot", "upload-list", "nas-list", \
    "record-export", "record-preview", "export-list", "export-cancel", "export-retry", "export-forget", "upload-task", "nas-task"
static const char *const station_commands[]={STATION_COMMANDS};
static const char *const camera_commands[]={CAMERA_COMMANDS};

static void space(const char **p) { while (**p==' ' || **p=='\r' || **p=='\n' || **p=='\t') ++*p; }
static bool take(const char **p, char c) { space(p); if (**p!=c) return false; ++*p; return true; }
static int hex(char c) {
    if (c>='0' && c<='9') return c-'0';
    if (c>='a' && c<='f') return c-'a'+10;
    if (c>='A' && c<='F') return c-'A'+10;
    return -1;
}
/* Envelope tokens are ASCII atoms; Unicode camera names use UTF-8 hex in the
 * name command. Reject token separators even when JSON escaped. */
static bool atom(const char **p, char *out, size_t size) {
    if (!take(p,'"')) return false;
    size_t n=0;
    while (**p && **p!='"') {
        unsigned c=(unsigned char)*(*p)++;
        if (c=='\\') {
            if (!**p) return false;
            c=(unsigned char)*(*p)++;
            if (c=='u') {
                c=0;
                for (unsigned k=0;k<4;++k) {
                    int v=hex(**p); if (v<0) return false;
                    c=c*16+(unsigned)v; ++*p;
                }
            } else if (c!='"' && c!='\\' && c!='/') return false;
        }
        if (c<=32 || c>126 || n+1>=size) return false;
        out[n++]=(char)c;
    }
    if (**p!='"' || !n) return false;
    ++*p; out[n]=0; return true;
}
static bool number(const char **p, uint32_t *value) {
    space(p); const char *start=*p; uint32_t n=0;
    while (**p>='0' && **p<='9') {
        unsigned digit=(unsigned)(**p-'0');
        if (n>(UINT32_MAX-digit)/10) return false;
        n=n*10+digit; ++*p;
    }
    if (*p==start || (*start=='0' && *p-start>1)) return false;
    *value=n; return true;
}
static bool append(char *dst, const char *token) {
    size_t n=strlen(dst), m=strlen(token);
    if (n+m+1>=512) return false;
    if (n) dst[n++]=' ';
    memcpy(dst+n,token,m+1); return true;
}
static bool contains(const char *s, const char *const *names, size_t n) {
    for (size_t i=0;i<n;++i) if (!strcmp(s,names[i])) return true;
    return false;
}
const char *api_command(const char *json, char out[512]) {
    char command[32]={0}, serial[32]={0}, args[512]={0};
    uint32_t revision=0; unsigned fields=0,count=0;
    const char *p=json;
    if (!take(&p,'{')) return "invalid_json_command";
    do {
        char key[32]; unsigned bit;
        if (!atom(&p,key,sizeof(key)) || !take(&p,':')) return "invalid_json_command";
        if (!strcmp(key,"command")) bit=1;
        else if (!strcmp(key,"args")) bit=2;
        else if (!strcmp(key,"camera_serial")) bit=4;
        else if (!strcmp(key,"expected_revision")) bit=8;
        else return "unknown_json_field";
        if (fields&bit) return "duplicate_json_field";
        fields|=bit;
        if (bit==1 && !atom(&p,command,sizeof(command))) return "invalid_json_command";
        if (bit==4 && !atom(&p,serial,sizeof(serial))) return "invalid_camera_serial";
        if (bit==8 && !number(&p,&revision)) return "invalid_expected_revision";
        if (bit==2) {
            if (!take(&p,'[')) return "invalid_arguments";
            if (!take(&p,']')) {
                do {
                    char token[256];
                    if (++count>15 || !atom(&p,token,sizeof(token)) || !append(args,token)) return "invalid_arguments";
                } while (take(&p,','));
                if (!take(&p,']')) return "invalid_arguments";
            }
        }
    } while (take(&p,','));
    if (!take(&p,'}')) return "invalid_json_command";
    space(&p); if (*p || (fields&3)!=3) return "invalid_json_command";
    bool station=contains(command,station_commands,sizeof(station_commands)/sizeof(*station_commands));
    bool camera=contains(command,camera_commands,sizeof(camera_commands)/sizeof(*camera_commands));
    if (!station && !camera) return "unsupported_command";
    if (camera && !(fields&4)) return "camera_serial_required";
    if (station && (fields&12)) return "camera_guard_not_applicable";
    if (count+1+((fields&4)?2:0)+((fields&8)?2:0)>16) return "too_many_arguments";
    out[0]=0;
    if ((fields&4) && (!append(out,"if-serial") || !append(out,serial))) return "command_too_long";
    if (fields&8) {
        char value[16]; snprintf(value,sizeof(value),"%u",revision);
        if (!append(out,"if-revision") || !append(out,value)) return "command_too_long";
    }
    if (!append(out,command) || (count && !append(out,args))) return "command_too_long";
    return NULL;
}
const char *api_capabilities(bool web_ui) {
    /* Built once per request into a bounded static buffer, main event loop only. */
    static char body[2048]; size_t n=(size_t)snprintf(body,sizeof(body),
        "{\"ok\":true,\"api_version\":\"1\",\"software_version\":\"0.10.0\",\"web_ui\":%s,"
        "\"auth\":\"bearer\",\"camera_guard_required\":true,\"max_cameras\":4,"
        "\"features\":{\"pairing\":true,\"settings\":true,\"recording_download\":true,"
        "\"rtsp_h264\":true,\"browser_playback\":false,\"event_history\":false,\"recording_audio\":false},\"commands\":[",
        web_ui?"true":"false");
    const char *const *groups[]={station_commands,camera_commands};
    const size_t sizes[]={sizeof(station_commands)/sizeof(*station_commands),sizeof(camera_commands)/sizeof(*camera_commands)};
    for (unsigned g=0;g<2;++g) for (size_t k=0;k<sizes[g];++k)
        n+=(size_t)snprintf(body+n,sizeof(body)-n,"%s\"%s\"",(g || k)?",":"",groups[g][k]);
    (void)snprintf(body+n,sizeof(body)-n,"]}\n"); return body;
}
