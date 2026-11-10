#include "server_web.h"
#include "web_auth.h"
#include "file_store.h"
#include "media_store.h"
#include "live_mux.h"
#include "json.h"
#include "myvava.h"
#include "http.h"
#include "api.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#define WEB_WORKERS 4
#define WEB_OUTPUT (MYVAVA_RESULT_MAX*6+4096)
struct asset {const char *path,*file,*type;char *data;size_t size;};
struct web_worker {struct server_web *web;char *output;};
struct server_web {
    int listener;atomic_bool stopping;pthread_t workers[WEB_WORKERS];unsigned started;struct web_worker work[WEB_WORKERS];
    struct mvm_store *media;struct file_store *files;char ffmpeg[PATH_MAX];atomic_uint downloads;
    struct web_auth *auth;SSL_CTX *tls;char socket[108],origin[160];
    struct asset assets[6];
};
struct connection {int fd;SSL *tls;uint64_t deadline;struct server_web *web;};
static bool nonblock(int fd) {int f=fcntl(fd,F_GETFL);return f>=0&&!fcntl(fd,F_SETFL,f|O_NONBLOCK)&&!fcntl(fd,F_SETFD,FD_CLOEXEC);}
static bool ready(struct connection *c,short events) {
    while(!atomic_load(&c->web->stopping)&&myvava_millis()<c->deadline) {
        struct pollfd p={.fd=c->fd,.events=events};int n=poll(&p,1,100);if(n>0)return !(p.revents&(POLLERR|POLLNVAL))&&(p.revents&(events|POLLHUP));if(n<0&&errno!=EINTR)return false;
    }return false;
}
static ssize_t transfer(struct connection *c,void *data,size_t n,bool writing) {
    short wait=writing?POLLOUT:POLLIN;
    while((c->tls&&!writing&&SSL_pending(c->tls)>0)||ready(c,wait)) {
        if(c->tls){int k=writing?SSL_write(c->tls,data,(int)n):SSL_read(c->tls,data,(int)n);if(k>0)return k;int e=SSL_get_error(c->tls,k);if(e==SSL_ERROR_WANT_READ)wait=POLLIN;else if(e==SSL_ERROR_WANT_WRITE)wait=POLLOUT;else return -1;}
        else {ssize_t k=writing?send(c->fd,data,n,0):recv(c->fd,data,n,0);if(k>=0)return k;if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)return -1;}
    }return -1;
}
static bool send_all(struct connection *c,const void *p,size_t n) {
    while(n){ssize_t k=transfer(c,(void *)p,n,true);if(k<=0)return false;p=(const char *)p+k;n-=(size_t)k;}return true;
}
static void respond(struct connection *c,int status,const char *type,const char *body,size_t length,const char *origin) {
    char headers[1536];int n=snprintf(headers,sizeof(headers),"HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; connect-src 'self' http: https:; img-src 'self' blob: data:; media-src 'self' blob:; frame-ancestors 'none'; base-uri 'none'; form-action 'self'\r\n%s%s%s\r\n",status,status==200?"OK":status==204?"No Content":"Request Failed",type,length,origin&&*origin?"Access-Control-Allow-Origin: ":"",origin&&*origin?origin:"",origin&&*origin?"\r\nVary: Origin\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Authorization, Content-Type\r\nAccess-Control-Max-Age: 600\r\n":"");
    if(n>0&&(size_t)n<sizeof(headers)&&send_all(c,headers,(size_t)n))send_all(c,body,length);
}
static void error(struct connection *c,int code,const char *reason,const char *origin) {char b[192];int n=snprintf(b,sizeof(b),"{\"ok\":false,\"error\":\"%s\"}",reason);respond(c,code,"application/json",b,(size_t)n,origin);}
static bool media_slot(struct server_web *w) {
    unsigned current=atomic_load(&w->downloads);while(current<2){if(atomic_compare_exchange_weak(&w->downloads,&current,current+1))return true;}return false;
}
static bool chunk(struct connection *c,const void *data,size_t size) {
    char header[32];int n=snprintf(header,sizeof(header),"%zx\r\n",size);
    return n>0&&send_all(c,header,(size_t)n)&&send_all(c,data,size)&&send_all(c,"\r\n",2);
}
static void download_file(struct connection *c,const char *device,uint64_t id,const char *origin,unsigned representation) {
    bool playback=representation!=0,thumbnail=representation==2;
    if(playback&&!c->web->ffmpeg[0]){error(c,501,"playback_requires_server_ffmpeg",origin);return;}
    if(!media_slot(c->web)){error(c,429,"media_capacity",origin);return;}
    struct myvava_file file;int fd=file_store_acquire(c->web->files,device,id,&file);
    if(fd<0){atomic_fetch_sub(&c->web->downloads,1);error(c,404,"file_unavailable_or_corrupt",origin);return;}
    c->deadline=myvava_millis()+60000;char headers[1024],sha[65];myvava_hex(file.sha256,32,sha);
    if(!playback) {
        int n=snprintf(headers,sizeof(headers),"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: video/mp2t\r\nContent-Length: %u\r\nContent-Disposition: attachment; filename=\"%016llx.ts\"\r\nX-Content-SHA256: %s\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s%s%s\r\n",file.size,(unsigned long long)id,sha,*origin?"Access-Control-Allow-Origin: ":"",origin,*origin?"\r\nVary: Origin\r\nAccess-Control-Expose-Headers: X-Content-SHA256\r\n":"");
        bool ok=n>0&&(size_t)n<sizeof(headers)&&send_all(c,headers,(size_t)n);uint8_t buffer[65536];uint32_t sent=0;
        while(ok&&sent<file.size){size_t size=file.size-sent;if(size>sizeof(buffer))size=sizeof(buffer);int k=file_store_read(c->web->files,fd,&file,sent,buffer,size);if(k<0&&errno==EINTR)continue;if(k<=0)break;ok=send_all(c,buffer,(size_t)k);sent+=(uint32_t)k;}
    } else {
        int output[2]={-1,-1},input[2]={-1,-1};pid_t pid=0;bool ok=pipe(output)==0&&pipe(input)==0;
        if(ok) {
            fcntl(output[0],F_SETFD,FD_CLOEXEC);fcntl(output[1],F_SETFD,FD_CLOEXEC);nonblock(output[0]);fcntl(input[0],F_SETFD,FD_CLOEXEC);fcntl(input[1],F_SETFD,FD_CLOEXEC);nonblock(input[1]);
            posix_spawn_file_actions_t actions;posix_spawnattr_t attr;posix_spawn_file_actions_init(&actions);posix_spawnattr_init(&attr);
            posix_spawn_file_actions_adddup2(&actions,input[0],STDIN_FILENO);posix_spawn_file_actions_adddup2(&actions,output[1],STDOUT_FILENO);
            posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
            posix_spawn_file_actions_addclose(&actions,output[0]);posix_spawn_file_actions_addclose(&actions,output[1]);posix_spawn_file_actions_addclose(&actions,input[0]);posix_spawn_file_actions_addclose(&actions,input[1]);
            posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attr,0);
            char *args[]={c->web->ffmpeg,"-nostdin","-v","error","-protocol_whitelist","pipe","-f","mpegts","-i","pipe:0","-map","0:v:0","-map","0:a:0?","-c:v","copy","-c:a","aac","-ar","48000","-ac","1","-movflags","frag_keyframe+empty_moov+default_base_moof","-f","mp4","pipe:1",NULL};
            char *thumb_args[]={c->web->ffmpeg,"-nostdin","-v","error","-protocol_whitelist","pipe","-f","mpegts","-i","pipe:0","-map","0:v:0","-frames:v","1","-vf","scale=480:-2","-c:v","mjpeg","-f","image2pipe","pipe:1",NULL};
            extern char **environ;ok=posix_spawn(&pid,c->web->ffmpeg,&actions,&attr,thumbnail?thumb_args:args,environ)==0;
            posix_spawnattr_destroy(&attr);posix_spawn_file_actions_destroy(&actions);close(output[1]);output[1]=-1;close(input[0]);input[0]=-1;
            if(ok) {
                bool started=false,eof=false;uint64_t total=0;uint8_t buffer[65536],source[65536];uint32_t offset=0;size_t source_size=0,source_sent=0;
                while(ok&&!eof&&!atomic_load(&c->web->stopping)&&myvava_millis()<c->deadline) {
                    if(input[1]>=0&&source_size==source_sent){
                        if(offset==file.size){close(input[1]);input[1]=-1;}
                        else{int n=file_store_read(c->web->files,fd,&file,offset,source,sizeof(source));if(n<=0){ok=false;break;}source_size=(size_t)n;source_sent=0;offset+=(uint32_t)n;}
                    }
                    struct pollfd p[2]={{.fd=output[0],.events=POLLIN},{.fd=input[1],.events=POLLOUT}};int r=poll(p,2,100);if(r<0&&errno==EINTR)continue;if(r<0){ok=false;break;}if(!r)continue;
                    if(p[1].revents&(POLLERR|POLLHUP|POLLNVAL)){if(thumbnail){close(input[1]);input[1]=-1;p[1].revents=0;}else{ok=false;break;}}
                    if(p[1].revents&POLLOUT){ssize_t n=write(input[1],source+source_sent,source_size-source_sent);if(n>0)source_sent+=(size_t)n;else if(n<0&&thumbnail&&errno==EPIPE){close(input[1]);input[1]=-1;}else if(n<0&&errno!=EAGAIN&&errno!=EINTR){ok=false;break;}}
                    if(!(p[0].revents&(POLLIN|POLLHUP)))continue;
                    ssize_t k=read(output[0],buffer,sizeof(buffer));if(k<0&&(errno==EINTR||errno==EAGAIN))continue;if(k<0){ok=false;break;}if(!k){eof=true;break;}
                    total+=(size_t)k;if(total>(thumbnail?1024u*1024:MYVAVA_FILE_MAX+32u*1024*1024)){ok=false;break;}
                    if(!started){int n=snprintf(headers,sizeof(headers),"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n%s%s%s\r\n",thumbnail?"image/jpeg":"video/mp4",*origin?"Access-Control-Allow-Origin: ":"",origin,*origin?"\r\nVary: Origin\r\n":"");ok=n>0&&(size_t)n<sizeof(headers)&&send_all(c,headers,(size_t)n);started=true;}
                    if(ok)ok=chunk(c,buffer,(size_t)k);
                }
                if(!eof||!ok)kill(-pid,SIGKILL);
                int status=0;pid_t result;do{result=waitpid(pid,&status,0);}while(result<0&&errno==EINTR);
                bool success=eof&&ok&&result==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
                if(success&&started)chunk(c,"",0);else if(!started)error(c,422,"recording_cannot_be_played",origin);
            } else error(c,503,"media_worker_unavailable",origin);
        } else error(c,503,"media_worker_unavailable",origin);
        for(unsigned k=0;k<2;++k){if(input[k]>=0)close(input[k]);if(output[k]>=0)close(output[k]);}
    }
    file_store_release(c->web->files,device,id,fd);atomic_fetch_sub(&c->web->downloads,1);
}
static bool live_record(void *arg,unsigned kind,uint32_t epoch,const uint8_t *data,size_t n){
    struct connection *c=arg;uint8_t h[16]={0};if(n>512u*1024u)return false;memcpy(h,"MVL2",4);h[4]=(uint8_t)kind;
    for(unsigned k=0;k<4;++k){h[8+k]=(uint8_t)(epoch>>(24-k*8));h[12+k]=(uint8_t)(n>>(24-k*8));}
    c->deadline=myvava_millis()+2000;return chunk(c,h,sizeof(h))&&chunk(c,data,n);
}
static void live_frames(struct connection *c,const char *device,const uint8_t boot[16],unsigned slot,const char *serial,bool talk,const char *token,const char *origin){
    if(!live_mux_available()){error(c,501,"continuous_media_unavailable",origin);return;}
    if(!media_slot(c->web)){error(c,429,"media_capacity",origin);return;}
    uint64_t lease=mvm_store_open_frames(c->web->media,device,boot,slot,serial,talk,token);
    if(!lease){atomic_fetch_sub(&c->web->downloads,1);error(c,409,"camera_busy_or_base_offline",origin);return;}
    struct live_mux *mux=live_mux_new(live_record,c);struct mvm_sample *sample=malloc(sizeof(*sample)+MVM_FRAME_MAX);bool ok=mux&&sample;
    if(ok){char headers[1024];int n=snprintf(headers,sizeof(headers),"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: application/vnd.myvava.live-v2\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Myvava-Lease: %016llx\r\n%s%s%s\r\n",(unsigned long long)lease,*origin?"Access-Control-Allow-Origin: ":"",origin,*origin?"\r\nVary: Origin\r\nAccess-Control-Expose-Headers: X-Myvava-Lease\r\n":"");ok=n>0&&(size_t)n<sizeof(headers)&&send_all(c,headers,(size_t)n);}
    uint64_t check=0;
    while(ok&&!atomic_load(&c->web->stopping)){
        uint64_t now=myvava_millis();if(now>=check){struct web_user u;ok=web_auth_session(c->web->auth,token,&u)&&web_auth_allowed(&u,device)&&(!talk||u.role>WEB_VIEWER);check=now+1000;if(!ok)break;}
        struct pollfd socket_state={.fd=c->fd,.events=POLLIN};if(poll(&socket_state,1,0)>0)break;
        int n=mvm_store_read_sample(c->web->media,lease,sample,MVM_FRAME_MAX);if(n<0)break;if(!n){poll(NULL,0,10);continue;}if(!live_mux_push(mux,sample))break;
    }
    mvm_store_close(c->web->media,lease);live_mux_free(mux);free(sample);atomic_fetch_sub(&c->web->downloads,1);
}
/* Live demand is owned by this authenticated connection and expires locally
 * even when the server process or transport disappears. */
static void live_stream(struct connection *c,const char *device,const uint8_t boot[16],unsigned slot,const char *serial,bool talk,bool mp4,const char *token,const char *origin) {
    if(mp4&&!c->web->ffmpeg[0]){error(c,501,"playback_requires_server_ffmpeg",origin);return;}
    if(!media_slot(c->web)){error(c,429,"media_capacity",origin);return;}
    uint64_t lease=mvm_store_open(c->web->media,device,boot,slot,serial,talk,token);
    if(!lease){atomic_fetch_sub(&c->web->downloads,1);error(c,409,"camera_busy_or_base_offline",origin);return;}
    int input[2]={-1,-1},output[2]={-1,-1};pid_t pid=0;bool ok=true;
    if(mp4){ok=!pipe(input)&&!pipe(output);if(ok){for(unsigned k=0;k<2;++k){fcntl(input[k],F_SETFD,FD_CLOEXEC);fcntl(output[k],F_SETFD,FD_CLOEXEC);}
        posix_spawn_file_actions_t actions;posix_spawnattr_t attr;posix_spawn_file_actions_init(&actions);posix_spawnattr_init(&attr);
        posix_spawn_file_actions_adddup2(&actions,input[0],STDIN_FILENO);posix_spawn_file_actions_adddup2(&actions,output[1],STDOUT_FILENO);posix_spawn_file_actions_addopen(&actions,STDERR_FILENO,"/dev/null",O_WRONLY,0);
        for(unsigned k=0;k<2;++k){posix_spawn_file_actions_addclose(&actions,input[k]);posix_spawn_file_actions_addclose(&actions,output[k]);}
        posix_spawnattr_setflags(&attr,POSIX_SPAWN_SETPGROUP);posix_spawnattr_setpgroup(&attr,0);
        char *args[]={c->web->ffmpeg,"-nostdin","-v","error","-protocol_whitelist","pipe","-analyzeduration","500000","-probesize","131072","-f","mpegts","-i","pipe:0","-map","0:v:0","-map","0:a:0?","-c:v","copy","-c:a","aac","-ar","48000","-ac","1","-movflags","frag_keyframe+empty_moov+default_base_moof","-frag_duration","250000","-flush_packets","1","-f","mp4","pipe:1",NULL};
        extern char **environ;ok=posix_spawn(&pid,c->web->ffmpeg,&actions,&attr,args,environ)==0;posix_spawnattr_destroy(&attr);posix_spawn_file_actions_destroy(&actions);
        close(input[0]);input[0]=-1;close(output[1]);output[1]=-1;if(ok)ok=nonblock(input[1])&&nonblock(output[0]);
    }}
    bool started=false;uint64_t check=0;uint8_t source[32768],converted[32768];size_t used=0,sent=0;
    while(ok&&!atomic_load(&c->web->stopping)){
        uint64_t now=myvava_millis();if(now>=check){struct web_user u;ok=web_auth_session(c->web->auth,token,&u)&&web_auth_allowed(&u,device)&&(!talk||u.role>WEB_VIEWER);check=now+1000;if(!ok)break;}
        struct pollfd socket_state={.fd=c->fd,.events=POLLIN};if(poll(&socket_state,1,0)>0){/* This request has no further client body. Readable means EOF or pipelining. */ok=false;break;}
        if(used==sent){int n=mvm_store_read(c->web->media,lease,source,sizeof(source));if(n<0)break;used=(size_t)n;sent=0;}
        const uint8_t *bytes=NULL;size_t length=0;
        if(mp4){struct pollfd fds[2]={{.fd=output[0],.events=POLLIN},{.fd=input[1],.events=used>sent?POLLOUT:0}};int p=poll(fds,2,20);if(p<0&&errno==EINTR)continue;if(p<0)break;
            if(fds[1].revents&(POLLHUP|POLLERR|POLLNVAL))break;
            if(fds[1].revents&POLLOUT){ssize_t n=write(input[1],source+sent,used-sent);if(n>0)sent+=(size_t)n;else if(n<0&&errno!=EAGAIN&&errno!=EINTR)break;}
            if(fds[0].revents&(POLLIN|POLLHUP)){ssize_t n=read(output[0],converted,sizeof(converted));if(n>0){bytes=converted;length=(size_t)n;}else if(!n)break;else if(errno!=EAGAIN&&errno!=EINTR)break;}
        }else if(used>sent){bytes=source+sent;length=used-sent;sent=used;}else{poll(NULL,0,20);}
        if(length){c->deadline=myvava_millis()+2000;if(!started){char headers[1024];int n=snprintf(headers,sizeof(headers),"HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Myvava-Lease: %016llx\r\n%s%s%s\r\n",mp4?"video/mp4":"video/mp2t",(unsigned long long)lease,*origin?"Access-Control-Allow-Origin: ":"",origin,*origin?"\r\nVary: Origin\r\nAccess-Control-Expose-Headers: X-Myvava-Lease\r\n":"");ok=n>0&&(size_t)n<sizeof(headers)&&send_all(c,headers,(size_t)n);started=true;}
            if(ok)ok=chunk(c,bytes,length);
        }
    }
    mvm_store_close(c->web->media,lease);
    for(unsigned k=0;k<2;++k){if(input[k]>=0)close(input[k]);if(output[k]>=0)close(output[k]);}
    if(pid>0){kill(-pid,SIGKILL);while(waitpid(pid,NULL,0)<0&&errno==EINTR){}}
    c->deadline=myvava_millis()+1000;if(started&&ok)chunk(c,"",0);else if(!started)error(c,503,"live_camera_unavailable",origin);
    atomic_fetch_sub(&c->web->downloads,1);
}

static bool ipc(struct server_web *w,const char *line,char *out) {
    struct connection c={.web=w,.deadline=myvava_millis()+4500};c.fd=socket(AF_UNIX,SOCK_STREAM,0);if(c.fd<0)return false;
    struct sockaddr_un address={.sun_family=AF_UNIX};strcpy(address.sun_path,w->socket);
    bool ok=nonblock(c.fd);if(ok&&connect(c.fd,(struct sockaddr *)&address,sizeof(address))&&errno!=EINPROGRESS)ok=false;
    if(ok)ok=send_all(&c,line,strlen(line))&&send_all(&c,"\n",1);size_t used=0;
    while(ok&&used<WEB_OUTPUT-1){ssize_t n=transfer(&c,out+used,WEB_OUTPUT-1-used,false);if(n<0){ok=false;break;}if(!n)break;used+=(size_t)n;}
    close(c.fd);out[used]=0;return ok&&used&&used<WEB_OUTPUT-1;
}
static bool field(const struct json_doc *d,const char *name,char *out,size_t size){return json_text(d,json_field(d,0,name),out,size);}
static bool hex(const char *s,unsigned n){uint8_t b[32];return n<=32&&myvava_unhex(s,b,n);}
static bool readonly_command(const char *s){const char *const names[]={"status","list","settings","policy","record-info","record-query","record-list","record-cameras","record-query-all","export-list","upload-list","nas-list","nas-status","storage-policy","capabilities","events"};for(unsigned i=0;i<sizeof(names)/sizeof(*names);++i)if(!strcmp(s,names[i]))return true;return false;}
static void user_json(char *out,size_t size,const struct web_user *u,const char *token){snprintf(out,size,"{\"ok\":true,\"user\":{\"name\":\"%s\",\"role\":%d,\"devices\":\"%s\"}%s%s%s}",u->name,u->role,u->devices,token?",\"token\":\"":"",token?token:"",token?"\"":"");}
static bool filter_devices(char *out,const struct web_user *u) {
    struct json_doc d;if(!json_parse(&d,out))return false;int a=json_field(&d,0,"devices");if(a<0||d.token[a].type!='[')return false;
    char *copy=strdup(out);if(!copy)return false;d.text=copy;size_t used=d.token[a].start+1;unsigned count=0;
    for(unsigned i=(unsigned)a+1;i<d.token[a].next;i=d.token[i].next) {char id[64];if(!json_text(&d,json_field(&d,i,"id"),id,sizeof(id))){free(copy);return false;}if(!web_auth_allowed(u,id))continue;
        if(count++)out[used++]=',';size_t n=d.token[i].end-d.token[i].start;memcpy(out+used,copy+d.token[i].start,n);used+=n;
    }
    strcpy(out+used,copy+d.token[a].end-1);free(copy);return true;
}
static void handle(struct connection *c,struct http_request *r,char *out) {
    struct server_web *w=c->web;const char *origin="";
    if(r->has_origin) {
        if(!http_origin_valid(r->origin)||(*w->origin?strcmp(w->origin,r->origin)!=0:!http_same_origin(r))){error(c,403,"origin_forbidden",NULL);return;}
        origin=r->origin;
    }
    if(!strcmp(r->method,"OPTIONS")){
        if(strncmp(r->path,"/api/v1/",8)||!http_preflight_valid(r)||(strcmp(r->cors_method,"GET")&&strcmp(r->cors_method,"POST"))){error(c,400,"invalid_preflight",origin);return;}
        respond(c,204,"application/json","",0,origin);return;
    }
    bool post=!strcmp(r->method,"POST"),get=!strcmp(r->method,"GET");
    if(!post&&!get){error(c,405,"method_not_allowed",origin);return;}
    if(get&&r->body[0]){error(c,400,"unexpected_body",origin);return;}
    if(get&&strncmp(r->path,"/api/",5)){
        for(unsigned i=0;i<6;++i)if(w->assets[i].data&&!strcmp(r->path,w->assets[i].path)){respond(c,200,w->assets[i].type,w->assets[i].data,w->assets[i].size,origin);return;}
        error(c,404,"not_found",origin);return;
    }
    struct json_doc d;
    if(post&&(strcmp(r->content_type,"application/json")||!json_parse(&d,r->body)||d.token[0].type!='{')){error(c,400,"invalid_json",origin);return;}
    struct web_user u={0};char token[65]={0};int code=200;
    if(post&&!strcmp(r->path,"/api/v1/login")) {
        const char *names[]={"username","password"};char name[64],password[129];
        if(!json_fields(&d,0,names,2)||!field(&d,"username",name,sizeof(name))||!field(&d,"password",password,sizeof(password))){error(c,400,"invalid_login",origin);return;}
        code=web_auth_login(w->auth,name,password,token,&u);OPENSSL_cleanse(password,sizeof(password));
        if(code!=200){error(c,code,code==429?"login_rate_limited":code==401?"invalid_credentials":"auth_unavailable",origin);return;}
        user_json(out,WEB_OUTPUT,&u,token);respond(c,200,"application/json",out,strlen(out),origin);return;
    }
    if(strncmp(r->authorization,"Bearer ",7)||!web_auth_session(w->auth,r->authorization+7,&u)){error(c,401,"session_required",origin);return;}
    if(get&&!strcmp(r->path,"/api/v1/media-capabilities")){snprintf(out,WEB_OUTPUT,"{\"ok\":true,\"continuous_live\":%s,\"live_framing\":2}",live_mux_available()?"true":"false");goto reply;}
    if(get&&!strcmp(r->path,"/api/v1/session")){user_json(out,WEB_OUTPUT,&u,NULL);goto reply;}
    if(post&&!strcmp(r->path,"/api/v1/logout")){if(!web_auth_logout(w->auth,r->authorization+7))goto unavailable;strcpy(out,"{\"ok\":true}");goto reply;}
    if(post&&!strcmp(r->path,"/api/v1/password")){
        const char *names[]={"old_password","new_password"};char old[129],password[129];
        if(!json_fields(&d,0,names,2)||!field(&d,"old_password",old,sizeof(old))||!field(&d,"new_password",password,sizeof(password)))goto invalid;
        code=web_auth_password(w->auth,&u,old,password);OPENSSL_cleanse(old,sizeof(old));OPENSSL_cleanse(password,sizeof(password));if(code!=200){error(c,code,"password_change_failed",origin);return;}
        strcpy(out,"{\"ok\":true,\"reauthenticate\":true}");goto reply;
    }
    if(!strcmp(r->path,"/api/v1/archive-policy")){
        if(u.role!=WEB_ADMIN)goto forbidden;
        if(post){const char *names[]={"revision","enabled","max_days"};char revision[16],enabled[8],days[8];
            if(!json_fields(&d,0,names,3)||!json_raw(&d,json_field(&d,0,"revision"),revision,sizeof(revision))||!json_raw(&d,json_field(&d,0,"enabled"),enabled,sizeof(enabled))||!json_raw(&d,json_field(&d,0,"max_days"),days,sizeof(days))||(strcmp(enabled,"true")&&strcmp(enabled,"false")))goto invalid;
            char *end=NULL;errno=0;unsigned long rev=strtoul(revision,&end,10);if(errno||*end||revision[0]<'0'||revision[0]>'9'||!rev||rev>UINT32_MAX)goto invalid;
            errno=0;unsigned long n=strtoul(days,&end,10);if(errno||*end||days[0]<'0'||days[0]>'9'||n>3650)goto invalid;
            int result=file_store_policy_set(w->files,(unsigned)rev,!strcmp(enabled,"true"),(unsigned)n);if(result!=200){error(c,result,result==409?"archive_policy_conflict":"archive_policy_unavailable",origin);return;}
        }
        if(!file_store_policy_json(w->files,out,WEB_OUTPUT))goto unavailable;goto reply;
    }
    if(!strcmp(r->path,"/api/v1/users")){
        if(u.role!=WEB_ADMIN)goto forbidden;
        if(get){if(!web_auth_users(w->auth,out,WEB_OUTPUT))goto unavailable;goto reply;}
        const char *names[]={"username","password","role","devices","disabled"};char name[64],password[129]={0},role[16],acl[1024],disabled[8];
        if(!json_fields(&d,0,names,5)||!field(&d,"username",name,sizeof(name))||!field(&d,"role",role,sizeof(role))||!field(&d,"devices",acl,sizeof(acl))||!json_raw(&d,json_field(&d,0,"disabled"),disabled,sizeof(disabled))||(strcmp(disabled,"true")&&strcmp(disabled,"false")))goto invalid;
        if(json_field(&d,0,"password")>=0&&!field(&d,"password",password,sizeof(password)))goto invalid;
        enum web_role role_id=!strcmp(role,"admin")?WEB_ADMIN:!strcmp(role,"operator")?WEB_OPERATOR:!strcmp(role,"viewer")?WEB_VIEWER:0;
        code=web_auth_user_set(w->auth,name,password,role_id,acl,!strcmp(disabled,"true"));OPENSSL_cleanse(password,sizeof(password));if(code!=200){error(c,code,"user_update_failed",origin);return;}strcpy(out,"{\"ok\":true}");goto reply;
    }
    if(get&&!strcmp(r->path,"/api/v1/devices")){if(!ipc(w,"devices",out)||!filter_devices(out,&u))goto unavailable;goto reply;}
    if(strncmp(r->path,"/api/v1/devices/",16)){error(c,404,"not_found",origin);return;}
    char path[256];strcpy(path,r->path+16);char *route=strchr(path,'/');if(!route)goto invalid;*route++=0;
    if(!myvava_id_valid(path))goto invalid;if(!web_auth_allowed(&u,path))goto forbidden;
    char line[1024];
    if(post&&!strcmp(route,"live")) {
        const char *names[]={"boot","slot","serial","talk","format"};char boot[33],slot[4],serial[32],talk[8],format[8];uint8_t raw[16];
        if(!json_fields(&d,0,names,5)||!field(&d,"boot",boot,sizeof(boot))||!myvava_unhex(boot,raw,16)||!field(&d,"serial",serial,sizeof(serial))||!json_raw(&d,json_field(&d,0,"slot"),slot,sizeof(slot))||strlen(slot)!=1||slot[0]<'0'||slot[0]>'3'||!json_raw(&d,json_field(&d,0,"talk"),talk,sizeof(talk))||(strcmp(talk,"true")&&strcmp(talk,"false"))||!field(&d,"format",format,sizeof(format))||(strcmp(format,"ts")&&strcmp(format,"mp4")&&strcmp(format,"mse-v2")))goto invalid;
        bool speaking=!strcmp(talk,"true");if(speaking&&u.role==WEB_VIEWER)goto forbidden;
        if(!strcmp(format,"mse-v2")){live_frames(c,path,raw,(unsigned)(slot[0]-'0'),serial,speaking,r->authorization+7,origin);return;}
        live_stream(c,path,raw,(unsigned)(slot[0]-'0'),serial,speaking,!strcmp(format,"mp4"),r->authorization+7,origin);return;
    }
    if(post&&!strncmp(route,"live/",5)) {
        char *action=strchr(route+5,'/');uint8_t raw[8];if(!action)goto invalid;*action++=0;if(!myvava_unhex(route+5,raw,8)||strcmp(action,"talk"))goto invalid;
        if(u.role==WEB_VIEWER)goto forbidden;const char *names[]={"aac"};char data[MVM_TALK_MAX*2+1];uint8_t aac[MVM_TALK_MAX];
        if(!json_fields(&d,0,names,1)||!field(&d,"aac",data,sizeof(data))||strlen(data)%2||!myvava_unhex(data,aac,strlen(data)/2))goto invalid;
        int result=mvm_store_talk(w->media,path,mvm_u64(raw),r->authorization+7,aac,strlen(data)/2);if(result!=200){error(c,result,"talk_unavailable_or_backpressure",origin);return;}strcpy(out,"{\"ok\":true,\"state\":\"queued\",\"playback_verified\":false}");goto reply;
    }
    if(get&&(!strncmp(route,"events?",7)||!strcmp(route,"events"))) {
        char before[17]="0000000000000000",category[16]="-",serial[32]="-";unsigned seen=0;char *query=strchr(route,'?'),*save=NULL;
        for(char *part=query?strtok_r(query+1,"&",&save):NULL;part;part=strtok_r(NULL,"&",&save)){
            char *value=strchr(part,'=');if(!value)goto invalid;*value++=0;
            unsigned bit=!strcmp(part,"before")?1:!strcmp(part,"category")?2:!strcmp(part,"serial")?4:0;if(!bit||(seen&bit)||!*value)goto invalid;seen|=bit;
            if(bit==1){if(!hex(value,8))goto invalid;strcpy(before,value);}
            else {char decoded[32];size_t n=0;for(size_t i=0;value[i];++i){unsigned char ch=(unsigned char)value[i];if(ch=='%'){char pair[3]={value[i+1],0,0};uint8_t raw;if(!pair[0]||!value[i+2])goto invalid;pair[1]=value[i+2];if(!myvava_unhex(pair,&raw,1))goto invalid;ch=raw;i+=2;}if(n>=31||!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'||ch==':'))goto invalid;decoded[n++]=(char)ch;}decoded[n]=0;
                if(bit==2){if(strcmp(decoded,"detection")&&strcmp(decoded,"recording")&&strcmp(decoded,"pairing")&&strcmp(decoded,"storage")&&strcmp(decoded,"system"))goto invalid;strcpy(category,decoded);}else strcpy(serial,decoded);
            }
        }
        snprintf(line,sizeof(line),"events %s %s %s %s",path,before,category,serial);if(!ipc(w,line,out))goto unavailable;
        char cursor[17];if(!web_auth_cursor(w->auth,&u,path,cursor))goto unavailable;size_t n=strlen(out);while(n&&(out[n-1]=='\n'||out[n-1]==' '))--n;if(!n||out[n-1]!='}')goto unavailable;snprintf(out+n-1,WEB_OUTPUT-n+1,",\"read_through\":\"%s\"}",cursor);goto reply;
    }
    if(get&&(!strncmp(route,"history?before=",15)||!strcmp(route,"history"))){const char *before=strchr(route,'=');before=before?before+1:"0000000000000000";if(!hex(before,8))goto invalid;snprintf(line,sizeof(line),"history %s %s",path,before);if(!ipc(w,line,out))goto unavailable;goto reply;}
    if(post&&!strcmp(route,"events/read")){
        const char *names[]={"through"};char id[17];if(!json_fields(&d,0,names,1)||!field(&d,"through",id,sizeof(id))||!hex(id,8))goto invalid;
        /* Do not accept a cursor beyond the latest retained event. */
        snprintf(line,sizeof(line),"event-latest %s",path);if(!ipc(w,line,out))goto unavailable;
        struct json_doc ed;if(!json_parse(&ed,out))goto unavailable;
        char latest[17];if(!json_text(&ed,json_field(&ed,0,"id"),latest,sizeof(latest)))goto invalid;
        uint8_t raw[8];myvava_unhex(id,raw,8);myvava_hex(raw,8,id);if(strcmp(id,latest)>0)goto invalid;
        if(!web_auth_mark(w->auth,&u,path,id))goto unavailable;strcpy(out,"{\"ok\":true}");goto reply;
    }
    if(!strncmp(route,"files/",6)) {
        char *action=strchr(route+6,'/');uint8_t raw[8];if(!action)goto invalid;*action++=0;if(!myvava_unhex(route+6,raw,8))goto invalid;
        uint64_t id=0;for(unsigned k=0;k<8;++k)id=id*256+raw[k];if(!id)goto invalid;
        if(get&&!strcmp(action,"info")){struct myvava_file f;const char *storage;if(!file_store_info_storage(w->files,path,id,&f,&storage)){error(c,404,"file_not_found",origin);return;}char sha[65];myvava_hex(f.sha256,32,sha);snprintf(out,WEB_OUTPUT,"{\"ok\":true,\"id\":\"%016llx\",\"serial\":\"%s\",\"sha256\":\"%s\",\"bytes\":%u,\"unix_seconds\":%llu,\"clock_valid\":%s,\"playback\":%s,\"encrypted\":%s,\"storage\":\"%s\",\"slot\":%u,\"duration_ms\":%llu,\"file_type\":%u,\"detection_reported\":%s,\"detection_confirmed\":%s,\"detection_type_raw\":%u}",(unsigned long long)id,f.serial,sha,f.size,(unsigned long long)f.unix_seconds,f.clock_valid?"true":"false",w->ffmpeg[0]?"true":"false",file_store_encrypted(w->files)?"true":"false",storage,f.slot,(unsigned long long)(f.last_ms-f.first_ms),f.file_type,(f.detection_flags&1)?"true":"false",(f.detection_flags&2)?"true":"false",f.detection_type);goto reply;}
        if(get&&(!strcmp(action,"download")||!strcmp(action,"playback")||!strcmp(action,"thumbnail"))){download_file(c,path,id,origin,!strcmp(action,"thumbnail")?2:!strcmp(action,"playback")?1:0);return;}
        if(post&&!strcmp(action,"delete")){if(u.role==WEB_VIEWER)goto forbidden;if(d.token[0].count)goto invalid;int result=file_store_delete(w->files,path,id);if(result!=200){error(c,result,result==409?"file_in_use":result==404?"file_not_found":"file_delete_failed",origin);return;}strcpy(out,"{\"ok\":true,\"deleted\":true}");goto reply;}
        goto invalid;
    }
    if(get&&(!strcmp(route,"files")||!strncmp(route,"files?",6))){
        unsigned page=0,seen=0;char storage[16]="",serial[32]="",*query=strchr(route,'?'),*save=NULL;
        if(query&&(!query[1]||query[1]=='&'||strstr(query,"&&")||query[strlen(query)-1]=='&'))goto invalid;
        for(char *part=query?strtok_r(query+1,"&",&save):NULL;part;part=strtok_r(NULL,"&",&save)){
            char *value=strchr(part,'=');if(!value)goto invalid;*value++=0;
            unsigned bit=!strcmp(part,"page")?1:!strcmp(part,"storage")?2:!strcmp(part,"serial")?4:0;
            if(!bit||(seen&bit)||!*value)goto invalid;seen|=bit;
            if(bit==1){if(strlen(value)>4)goto invalid;for(char *p=value;*p;++p){if(*p<'0'||*p>'9')goto invalid;page=page*10+(unsigned)(*p-'0');}if(page>4096)goto invalid;}
            else if(bit==2){if(strcmp(value,"cloud")&&strcmp(value,"tf_export")&&strcmp(value,"tf_preview")&&strcmp(value,"legacy_import"))goto invalid;strcpy(storage,value);}
            else {size_t n=0;for(size_t i=0;value[i];++i){unsigned char ch=(unsigned char)value[i];if(ch=='%'){char pair[3]={value[i+1],0,0};uint8_t raw;if(!pair[0]||!value[i+2])goto invalid;pair[1]=value[i+2];if(!myvava_unhex(pair,&raw,1))goto invalid;ch=raw;i+=2;}
                if(n>=31||!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'||ch==':'))goto invalid;serial[n++]=(char)ch;}serial[n]=0;}
        }
        if(!file_store_list_filtered(w->files,path,page,storage,serial,out,WEB_OUTPUT))goto unavailable;goto reply;
    }
    if(get&&!strncmp(route,"lookup/",7)){if(!hex(route+7,16))goto invalid;snprintf(line,sizeof(line),"lookup %s %s",path,route+7);if(!ipc(w,line,out))goto unavailable;goto reply;}
    if(get&&!strncmp(route,"tasks/",6)) {
        char *id=strchr(route+6,'/');if(!id)goto invalid;*id++=0;if(!hex(route+6,16)||!hex(id,8))goto invalid;
        snprintf(line,sizeof(line),"result %s %s",route+6,id);if(!ipc(w,line,out))goto unavailable;
        struct json_doc result;if(!json_parse(&result,out))goto unavailable;char device[64];int f=json_field(&result,0,"device");if(f>=0&&(!json_text(&result,f,device,sizeof(device))||strcmp(device,path)))goto forbidden;goto reply;
    }
    if(post&&!strcmp(route,"commands")) {
        const char *names[]={"boot","epoch","request_key","command"};char boot[33],epoch[33],key[33],envelope[2048],command[512],name[32];
        if(!json_fields(&d,0,names,4)||!field(&d,"boot",boot,sizeof(boot))||!hex(boot,16)||!field(&d,"epoch",epoch,sizeof(epoch))||!hex(epoch,16)||!field(&d,"request_key",key,sizeof(key))||!hex(key,16)||!json_raw(&d,json_field(&d,0,"command"),envelope,sizeof(envelope)))goto invalid;
        struct json_doc inner;if(!json_parse(&inner,envelope)||!field(&inner,"command",name,sizeof(name)))goto invalid;
        const char *e=api_command(envelope,command);if(e){error(c,400,e,origin);return;}if(u.role==WEB_VIEWER&&!readonly_command(name)&&strcmp(name,"record-export")&&strcmp(name,"record-preview"))goto forbidden;
        if(!strcmp(name,"live-quality")){
            int a=json_field(&inner,0,"args");char slot[4],lease[17],serial[32],bound_serial[32];uint8_t raw[8];unsigned bound_slot;
            if(a<0||inner.token[a].type!='['||inner.token[a].count!=3||!json_text(&inner,a+1,slot,sizeof(slot))||!json_text(&inner,a+2,lease,sizeof(lease))||!myvava_unhex(lease,raw,8)||!field(&inner,"camera_serial",serial,sizeof(serial))||json_field(&inner,0,"expected_revision")<0)goto invalid;
            int owner=mvm_store_owner(w->media,path,mvm_u64(raw),r->authorization+7,&bound_slot,bound_serial);
            if(owner!=200){error(c,owner,"viewing_owner_or_lease_changed",origin);return;}if(strlen(slot)!=1||slot[0]!=(char)('0'+bound_slot)||strcmp(serial,bound_serial))goto forbidden;
        }
        if(u.role!=WEB_ADMIN&&(!strcmp(name,"forget")||!strcmp(name,"remove")||!strcmp(name,"remove-confirm")||!strcmp(name,"reset")||!strcmp(name,"storage-cleanup")||!strcmp(name,"storage-policy-set")||!strcmp(name,"clock-valid")))goto forbidden;
        snprintf(line,sizeof(line),"submit %s %s %s %s %s",path,boot,epoch,key,command);if(!ipc(w,line,out))goto unavailable;goto reply;
    }
    error(c,404,"not_found",origin);return;
reply:respond(c,200,"application/json",out,strlen(out),origin);return;
invalid:error(c,400,"invalid_request",origin);return;
forbidden:error(c,403,"permission_denied",origin);return;
unavailable:error(c,503,"service_unavailable",origin);
}
static void *worker(void *arg) {
    struct web_worker *work=arg;struct server_web *w=work->web;char *out=work->output;
    while(!atomic_load(&w->stopping)) {
        struct pollfd p={.fd=w->listener,.events=POLLIN};if(poll(&p,1,100)<=0)continue;
        struct connection c={.fd=accept(w->listener,NULL,NULL),.web=w,.deadline=myvava_millis()+6000};if(c.fd<0)continue;
        if(!nonblock(c.fd)){close(c.fd);continue;}
        bool ok=true;
        if(w->tls){c.tls=SSL_new(w->tls);ok=c.tls&&SSL_set_fd(c.tls,c.fd)==1;if(ok){for(;;){int k=SSL_accept(c.tls);if(k==1)break;int e=SSL_get_error(c.tls,k);if((e!=SSL_ERROR_WANT_READ&&e!=SSL_ERROR_WANT_WRITE)||!ready(&c,e==SSL_ERROR_WANT_WRITE?POLLOUT:POLLIN)){ok=false;break;}}}}
        uint8_t input[8192];size_t used=0;struct http_request r;
        while(ok&&used<sizeof(input)) {ssize_t n=transfer(&c,input+used,sizeof(input)-used,false);if(n<=0)break;used+=(size_t)n;int parsed=http_parse(input,used,&r);if(parsed<0){error(&c,400,"invalid_http",NULL);break;}if(parsed==1){handle(&c,&r,out);break;}}
        OPENSSL_cleanse(input,sizeof(input));OPENSSL_cleanse(&r,sizeof(r));OPENSSL_cleanse(out,WEB_OUTPUT);SSL_free(c.tls);close(c.fd);
    }return NULL;
}
struct server_web *server_web_start(const struct server_web_config *cfg) {
    if(!cfg->port||strlen(cfg->socket)>=108||(cfg->origin&&*cfg->origin&&!http_origin_valid(cfg->origin)))return NULL;
    struct server_web *w=calloc(1,sizeof(*w));if(!w)return NULL;w->listener=-1;w->files=cfg->files;w->media=cfg->media;strcpy(w->socket,cfg->socket);if(cfg->origin)strcpy(w->origin,cfg->origin);
    if(cfg->ffmpeg){struct stat st;if(cfg->ffmpeg[0]!='/'||!realpath(cfg->ffmpeg,w->ffmpeg)||stat(w->ffmpeg,&st)||!S_ISREG(st.st_mode)||access(w->ffmpeg,X_OK))goto fail;}
    if((cfg->certificate&&!cfg->key)||(!cfg->certificate&&cfg->key))goto fail;
    if(cfg->certificate){w->tls=SSL_CTX_new(TLS_server_method());if(!w->tls||!SSL_CTX_set_min_proto_version(w->tls,TLS1_2_VERSION)||SSL_CTX_use_certificate_chain_file(w->tls,cfg->certificate)!=1||SSL_CTX_use_PrivateKey_file(w->tls,cfg->key,SSL_FILETYPE_PEM)!=1||SSL_CTX_check_private_key(w->tls)!=1)goto fail;SSL_CTX_set_options(w->tls,SSL_OP_NO_COMPRESSION);}
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons((uint16_t)cfg->port)};if(inet_pton(AF_INET,cfg->bind,&addr.sin_addr)!=1||(!w->tls&&(ntohl(addr.sin_addr.s_addr)>>24)!=127))goto fail;
    w->auth=web_auth_open(cfg->state,cfg->bootstrap);if(!w->auth)goto fail;
    const struct asset assets[6]={{"/","index.html","text/html; charset=utf-8",NULL,0},{"/style.css","style.css","text/css",NULL,0},{"/app.js","app.js","text/javascript",NULL,0},{"/api-client.js","api-client.js","text/javascript",NULL,0},{"/management.js","management.js","text/javascript",NULL,0},{"/config.js","config.js","text/javascript",NULL,0}};memcpy(w->assets,assets,sizeof(assets));
    if(cfg->directory){int dir=open(cfg->directory,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(dir<0)goto fail;bool ok=true;
        for(unsigned i=0;i<6&&ok;++i){int fd=openat(dir,assets[i].file,O_RDONLY|O_NOFOLLOW|O_NONBLOCK|O_CLOEXEC);struct stat st;if(fd<0){ok=false;break;}ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<1024*1024;
            if(ok){w->assets[i].size=(size_t)st.st_size;w->assets[i].data=malloc((size_t)st.st_size);ok=w->assets[i].data!=NULL;size_t n=0;while(ok&&n<(size_t)st.st_size){ssize_t k=read(fd,w->assets[i].data+n,(size_t)st.st_size-n);if(k<0&&errno==EINTR)continue;if(k<=0){ok=false;break;}n+=(size_t)k;}}close(fd);}
        close(dir);if(!ok)goto fail;
    }
    w->listener=socket(AF_INET,SOCK_STREAM,0);int yes=1;if(w->listener<0||!nonblock(w->listener)||setsockopt(w->listener,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes))||bind(w->listener,(struct sockaddr *)&addr,sizeof(addr))||listen(w->listener,16))goto fail;
    for(unsigned i=0;i<WEB_WORKERS;++i){w->work[i].web=w;w->work[i].output=malloc(WEB_OUTPUT);if(!w->work[i].output||pthread_create(&w->workers[i],NULL,worker,&w->work[i]))goto fail;++w->started;}return w;
fail:server_web_stop(w);return NULL;
}
void server_web_stop(struct server_web *w){if(!w)return;atomic_store(&w->stopping,true);for(unsigned i=0;i<w->started;++i)pthread_join(w->workers[i],NULL);for(unsigned i=0;i<WEB_WORKERS;++i)free(w->work[i].output);if(w->listener>=0)close(w->listener);web_auth_close(w->auth);SSL_CTX_free(w->tls);for(unsigned i=0;i<6;++i)free(w->assets[i].data);free(w);}
