#include "pairing.h"
#include "myvava.h"
#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define PAIR_BYTES 192
static struct operation {uint64_t id,created,deadline;enum pair_phase phase;unsigned seconds;int slot;char serial[32],reason[40];} current;
static int directory=-1;static bool healthy;
static const char *const phases[]={"idle","opening_ap","waiting_camera","credentials_sent","waiting_formal_connection","waiting_confirmation","completed","cancelled","expired","interrupted","failed"};
static void put64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(x>>(i*8));}
static uint64_t get64(const uint8_t *p){uint64_t x=0;for(unsigned i=0;i<8;++i)x|=(uint64_t)p[i]<<(i*8);return x;}
static bool camera_serial(const char *s){uint8_t b[32]={0};char out[32];if(!s||strlen(s)>31)return false;memcpy(b,s,strlen(s));return vava_serial(b,out);}
bool pairing_active(void){return current.phase>=PAIR_OPENING&&current.phase<=PAIR_CONFIRMING;}
bool pairing_accepts(const char *serial){return healthy&&pairing_active()&&(!current.serial[0]||!strcmp(serial,current.serial));}
uint64_t pairing_id(void){return current.id;}
static bool persist(const struct operation *op){uint8_t b[PAIR_BYTES]={0};memcpy(b,"VAPAI001",8);put64(b+8,op->id);put64(b+16,op->created);b[24]=(uint8_t)op->phase;b[25]=op->slot<0?255:(uint8_t)op->slot;vava_put32(b+28,op->seconds);memcpy(b+32,op->serial,32);memcpy(b+64,op->reason,40);vava_put32(b+188,vava_crc32(b,188));
    int fd=openat(directory,"pairing.tmp",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0){healthy=false;return false;}struct stat st;bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&!ftruncate(fd,0);size_t n=0;while(ok&&n<sizeof(b)){ssize_t k=write(fd,b+n,sizeof(b)-n);if(k<0&&errno==EINTR)continue;if(k<=0)ok=false;else n+=(size_t)k;}if(ok)ok=!fsync(fd);if(close(fd))ok=false;if(ok)ok=!renameat(directory,"pairing.tmp",directory,"pairing.bin")&&!fsync(directory);if(!ok)healthy=false;return ok;
}
bool pairing_init(const char *path,const struct registration r[4]){
    current=(struct operation){.slot=-1};directory=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(directory<0)return false;healthy=true;
    int fd=openat(directory,"pairing.bin",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return errno==ENOENT;
    struct stat st;uint8_t b[PAIR_BYTES];bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&st.st_size==PAIR_BYTES;size_t n=0;while(ok&&n<sizeof(b)){ssize_t k=read(fd,b+n,sizeof(b)-n);if(k<0&&errno==EINTR)continue;if(k<=0)ok=false;else n+=(size_t)k;}close(fd);
    if(!ok||memcmp(b,"VAPAI001",8)||vava_u32(b+188)!=vava_crc32(b,188)||b[24]>PAIR_FAILED||(b[25]>3&&b[25]!=255)||!memchr(b+32,0,32)||!memchr(b+64,0,40)||vava_u32(b+28)>300)return false;
    current=(struct operation){.id=get64(b+8),.created=get64(b+16),.phase=b[24],.slot=b[25]==255?-1:b[25],.seconds=vava_u32(b+28)};memcpy(current.serial,b+32,32);memcpy(current.reason,b+64,40);
    if((current.serial[0]&&!camera_serial(current.serial))||(current.reason[0]&&!myvava_id_valid(current.reason)))return false;
    if(pairing_active()){current.phase=PAIR_INTERRUPTED;strcpy(current.reason,"process_restarted_ap_not_reopened");
        if(current.slot>=0&&r[current.slot].stage==3&&!strcmp(r[current.slot].serial,current.serial)){current.phase=PAIR_COMPLETE;strcpy(current.reason,"registry_confirmed_after_restart");}
        if(!persist(&current))return false;
    }return true;
}
bool pairing_begin(unsigned seconds,uint64_t now,uint64_t unix_ms){if(!healthy||pairing_active()||!seconds||seconds>300)return false;struct operation next={.created=unix_ms,.phase=PAIR_OPENING,.seconds=seconds,.slot=-1,.deadline=now+30000+(uint64_t)seconds*1000};if(!myvava_random((uint8_t *)&next.id,8)||!next.id||!persist(&next))return false;current=next;return true;}
bool pairing_change(enum pair_phase phase,int slot,const char *serial,const char *reason,uint64_t now,unsigned seconds){
    if(!healthy||!current.id||phase>PAIR_FAILED||slot>3||slot< -1||strlen(reason)>=sizeof(current.reason))return false;
    if(serial&&(!camera_serial(serial)||strlen(serial)>31||(current.serial[0]&&strcmp(serial,current.serial))))return false;
    if(!pairing_active()&&!(phase==PAIR_COMPLETE&&current.phase==PAIR_INTERRUPTED))return false;
    struct operation next=current;next.phase=phase;if(slot>=0)next.slot=slot;if(serial)strcpy(next.serial,serial);strcpy(next.reason,reason);if(seconds)next.deadline=now+(uint64_t)seconds*1000;
    if(!persist(&next))return false;current=next;return true;
}
void pairing_tick(uint64_t now,bool managed,const char *ap,uint64_t until){if(!pairing_active())return;
    if(current.phase==PAIR_OPENING){if(!managed||!strcmp(ap,"open")){if(until>now)(void)pairing_change(PAIR_WAITING,-1,NULL,"",now,(unsigned)((until-now+999)/1000));}
        else if(!strcmp(ap,"closed")||!strcmp(ap,"closing")||!strcmp(ap,"open_failed")||!strcmp(ap,"helper_timeout")||!strcmp(ap,"close_failed"))(void)pairing_change(PAIR_FAILED,-1,NULL,"temporary_ap_failed",now,0);
    }
    if(pairing_active()&&now>=current.deadline)(void)pairing_change(PAIR_EXPIRED,-1,NULL,"camera_pairing_timeout",now,0);
}
bool pairing_json(char *out,size_t size,uint64_t now){int n=snprintf(out,size,"{\"id\":\"%016" PRIx64 "\",\"phase\":\"%s\",\"active\":%s,\"durable\":%s,\"created_unix_ms\":%" PRIu64 ",\"remaining_ms\":%" PRIu64 ",\"slot\":%d,\"serial\":\"%s\",\"reason\":\"%s\"}",current.id,phases[current.phase],pairing_active()?"true":"false",healthy?"true":"false",current.created,pairing_active()&&current.deadline>now?current.deadline-now:0,current.slot,current.serial,healthy?current.reason:"pairing_state_write_failed");return n>0&&(size_t)n<size;}
