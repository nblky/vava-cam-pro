#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static bool write_all(int fd, const void *p, size_t n) {
    const unsigned char *bytes=p;
    while (n) {
        ssize_t k=write(fd,bytes,n);
        if (k<0 && errno==EINTR) continue;
        if (k<=0) return false;
        bytes+=k; n-=(size_t)k;
    }
    return true;
}
int main(int argc, char **argv) {
    const char *path="run/control.sock"; int start=1;
    if (argc>2 && (!strcmp(argv[1],"--socket") || !strcmp(argv[1],"-s"))) { path=argv[2]; start=3; }
    if (argc<=start || !strcmp(argv[start],"--help")) {
        fprintf(stderr,"vavactl [-s SOCKET] status | list | pair-open SECONDS | pair-close\n"
            "  live SLOT | stop SLOT | wake SLOT | sleep SLOT | iframe SLOT\n"
            "  if-serial SERIAL remove SLOT (factory reset + unpair; retains recordings)\n"
            "  if-serial SERIAL reset SLOT (restore camera defaults; retains pairing and recordings)\n"
            "  if-serial SERIAL remove-confirm SLOT OPERATION_ID (operator verified physical reset only)\n"
            "  status.removal reports progress; production Agent refuses legacy forget.\n"
            "  settings SLOT (durable preferences, requests and observed reports)\n"
            "  name SLOT \"DISPLAY NAME\" (local UTF-8 name; empty string clears it)\n"
            "  set SLOT mirror 0..3 | set SLOT ir 0|1 | set SLOT antiflicker 50|60\n"
            "  set SLOT osd 0|1 | set SLOT volume 0..100 | set SLOT quality 0..2\n"
            "  set SLOT playback-audio 0|1 (decoder playback gate, not microphone mute)\n"
            "  set SLOT pir-level 0..3 (current MCU level, not a full arming policy)\n"
            "  pir-mode SLOT battery|monitor [retry]\n"
            "  pir-mode SLOT custom SENS WAIT_SECONDS RECORD_SECONDS [retry]\n"
            "  PIR mode uses local managed policy, or a valid current report 42 in diagnostic mode.\n"
            "  Add retry to explicitly resend an identical completed/timed-out request.\n"
            "  configure SLOT NAME VALUE | configure-pir SLOT MODE [SENS WAIT RECORD]\n"
            "  configure-many SLOT NAME=VALUE ... | policy-config SLOT KEY=VALUE ...\n"
            "  configure SLOT detection-box 0|1 (VA-HS03 1.4.0.54; valid current report required)\n"
            "  Prefix camera requests with if-serial SERIAL; saved config supports if-revision REV.\n"
            "  apply-settings SLOT [NAME ...] | clear-settings SLOT (clear local preferences only)\n"
            "  policy SLOT | arm SLOT off|on|schedule|diagnostic [LEVEL] | arm SLOT retry\n"
            "  enable SLOT 0|1 | timezone SLOT UTC_MINUTES | clock-valid 0|1\n"
            "  plan SLOT set INDEX DAYS HH:MM:SS HH:MM:SS | plan SLOT all-day INDEX DAYS\n"
            "  plan SLOT enable INDEX 0|1 (keeps the saved days and times)\n"
            "  plan SLOT delete INDEX | plan SLOT clear (DAYS: 0=Sun..6=Sat, e.g. 12345)\n"
            "  record-duration SLOT 5..120 | idle-sleep SLOT 0..3600\n"
            "  record SLOT start|stop | record-list SLOT [PAGE] | record-get SLOT HEX_ID\n"
            "  record-query SLOT BEFORE_HEX GENERATION FROM_UNIX TO_UNIX (0 0: all times)\n"
            "  record-query-all BEFORE_HEX GENERATION FROM_UNIX TO_UNIX\n"
            "  record-preview SLOT SOURCE_HEX EXPORT_HEX\n"
            "  record-info SLOT HEX_ID | record-cameras [PAGE] | capabilities\n"
            "  record-delete SLOT HEX_ID | storage-cleanup (remove abandoned recording files)\n"
            "  stream SLOT 0|1 | snapshot SLOT (H.264 stdout) | audio SLOT 8|9 (wire frames)\n"
            "  Use tools/media_client.py for JPEG conversion, AAC capture and talk input.\n");
        return argc<=start ? 2 : 0;
    }
    struct sockaddr_un a={.sun_family=AF_UNIX};
    if (strlen(path)>=sizeof(a.sun_path)) { fprintf(stderr,"socket path too long\n"); return 2; }
    memcpy(a.sun_path,path,strlen(path)+1);
    int command=start;
    while (command+2<argc && (!strcmp(argv[command],"if-serial") || !strcmp(argv[command],"if-revision"))) command+=2;
    bool naming=!strcmp(argv[command],"name");
    if (naming && argc!=command+3) { fprintf(stderr,"name requires SLOT and one quoted display name\n"); return 2; }
    char request[512]={0}; size_t used=0;
    for (int j=start;j<argc;++j) {
        const char *arg=argv[j]; char encoded[193];
        if (naming && j==command+2) {
            size_t len=strlen(arg);
            if (len>96) { fprintf(stderr,"name exceeds 96 UTF-8 bytes\n"); return 2; }
            if (!len) memcpy(encoded,"-",2);
            else for (size_t k=0;k<len;++k) (void)snprintf(encoded+2*k,3,"%02x",(unsigned char)arg[k]);
            arg=encoded;
        }
        if (strpbrk(arg,"\r\n\t ")) { fprintf(stderr,"invalid argument\n"); return 2; }
        int n=snprintf(request+used,sizeof(request)-used,"%s%s",j==start ? "":" ",arg);
        if (n<0 || (size_t)n>=sizeof(request)-used-1) return 2;
        used+=(size_t)n;
    }
    request[used++]='\n';
    int fd=socket(AF_UNIX,SOCK_STREAM,0);
    if (fd<0 || connect(fd,(struct sockaddr *)&a,sizeof(a))) { perror("connect"); return 1; }
    signal(SIGPIPE,SIG_IGN);
    struct timeval timeout={.tv_sec=5};
    (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    (void)setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout));
    if (!write_all(fd,request,used)) { perror("send"); close(fd); return 1; }
    char header[16384]; used=0;
    /* Reading the line bytewise leaves any coalesced media in the socket. */
    while (used<sizeof(header)-1) {
        ssize_t n=read(fd,header+used,1);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { fprintf(stderr,"incomplete response\n"); close(fd); return 1; }
        if (header[used++]=='\n') break;
    }
    header[used]=0;
    bool ok=strstr(header,"\"ok\":true")!=NULL;
    bool stream=(!strcmp(argv[command],"stream") || !strcmp(argv[command],"record-get") ||
                 !strcmp(argv[command],"snapshot") || !strcmp(argv[command],"audio")) && ok;
    if (!stream) {
        bool written=write_all(STDOUT_FILENO,header,used);
        close(fd); return ok && written ? 0 : 1;
    }
    bool bounded=!strcmp(argv[command],"record-get"); uint64_t expected=0,total=0;
    if (bounded) {
        const char *field=strstr(header,"\"bytes\":"); char *end=NULL; errno=0;
        if (field && field[8]>='0' && field[8]<='9') expected=strtoull(field+8,&end,10);
        if (!end || errno || (*end!=',' && *end!='}')) {
            fprintf(stderr,"recording response has no valid length\n"); close(fd); return 1;
        }
    }
    timeout.tv_sec=bounded?30:0; (void)setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout));
    unsigned char data[16384]; bool received=false;
    for (;;) {
        ssize_t n=read(fd,data,sizeof(data));
        if (n<0 && errno==EINTR) continue;
        if (n<0) { perror("stream"); close(fd); return 1; }
        if (!n) break;
        if (bounded && (total>expected || (uint64_t)n>expected-total)) {
            fprintf(stderr,"recording exceeded declared length\n"); close(fd); return 1;
        }
        total+=(uint64_t)n;
        received=true;
        if (!write_all(STDOUT_FILENO,data,(size_t)n)) { close(fd); return 1; }
    }
    close(fd);
    if (bounded && total!=expected) {
        fprintf(stderr,"incomplete recording: received %" PRIu64 " of %" PRIu64 " bytes\n",total,expected); return 1;
    }
    if (!received) fprintf(stderr,"stream ended without video; inspect vavactl status\n");
    return received ? 0 : 1;
}
