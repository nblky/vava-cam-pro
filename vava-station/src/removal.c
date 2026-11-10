#include "removal.h"
#include "protocol.h"
#include "myvava.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
/* A durable intent precedes each destructive wire request. Restart never
 * replays a request: only an already acknowledged local commit is resumed. */
#define BYTES 192
static struct operation {uint64_t id,created,deadline;unsigned slot;enum removal_phase phase;bool operator_confirmed,defaults_only;uint32_t revision;char serial[32],reason[48];} current;
static int directory=-1;static bool healthy;
static const char *const phases[]={"idle","waiting_camera","restoring_defaults","clearing_pairing","committing","completed","failed","result_unknown"};
static void p64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;++i)p[i]=(uint8_t)(x>>(8*i));}
static uint64_t g64(const uint8_t *p){uint64_t x=0;for(unsigned i=0;i<8;++i)x|=(uint64_t)p[i]<<(8*i);return x;}
enum removal_phase removal_phase(void){return current.phase;}
bool removal_active(void){return current.phase>=REMOVE_WAITING&&current.phase<=REMOVE_ACKNOWLEDGED;}
bool removal_blocks(unsigned slot,const char *serial){return current.slot==slot&&!strcmp(current.serial,serial)&&(removal_active()||current.phase==REMOVE_UNKNOWN);}
unsigned removal_slot(void){return current.slot;}
const char *removal_serial(void){return current.serial;}
uint64_t removal_id(void){return current.id;}
uint32_t removal_token(void){return (uint32_t)current.id;}
uint64_t removal_deadline(void){return current.deadline;}
bool removal_operator_confirmed(void){return current.operator_confirmed;}
bool removal_defaults_only(void){return current.defaults_only;}
uint32_t removal_settings_revision(void){return current.revision;}
static bool persist(const struct operation *op){
    uint8_t b[BYTES]={0};memcpy(b,"VARMV001",8);p64(b+8,op->id);p64(b+16,op->created);b[24]=(uint8_t)op->phase;b[25]=(uint8_t)op->slot;b[26]=op->operator_confirmed;b[27]=op->defaults_only;memcpy(b+32,op->serial,32);memcpy(b+64,op->reason,48);vava_put32(b+112,op->revision);vava_put32(b+188,vava_crc32(b,188));
    int fd=openat(directory,"removal.tmp",O_WRONLY|O_CREAT|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);if(fd<0){healthy=false;return false;}
    struct stat st;bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&!ftruncate(fd,0);size_t n=0;
    while(ok&&n<sizeof(b)){ssize_t k=write(fd,b+n,sizeof(b)-n);if(k<0&&errno==EINTR)continue;if(k<=0)ok=false;else n+=(size_t)k;}
    if(ok)ok=!fsync(fd);if(close(fd))ok=false;if(ok)ok=!renameat(directory,"removal.tmp",directory,"removal.bin")&&!fsync(directory);if(!ok)healthy=false;return ok;
}
bool removal_init(const char *path){
    current=(struct operation){0};directory=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);if(directory<0)return false;healthy=true;
    int fd=openat(directory,"removal.bin",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);if(fd<0)return errno==ENOENT;
    uint8_t b[BYTES];struct stat st;bool ok=!fstat(fd,&st)&&S_ISREG(st.st_mode)&&st.st_uid==geteuid()&&!(st.st_mode&0077)&&st.st_nlink==1&&st.st_size==BYTES;size_t n=0;
    while(ok&&n<sizeof(b)){ssize_t k=read(fd,b+n,sizeof(b)-n);if(k<0&&errno==EINTR)continue;if(k<=0)ok=false;else n+=(size_t)k;}close(fd);
    char serial[32];if(!ok||memcmp(b,"VARMV001",8)||vava_u32(b+188)!=vava_crc32(b,188)||b[24]>REMOVE_UNKNOWN||b[25]>3||!vava_serial(b+32,serial)||!memchr(b+64,0,48)||strspn((char *)b+64,"abcdefghijklmnopqrstuvwxyz0123456789_")!=strlen((char *)b+64))return false;
    if(b[26]>1||b[27]>1||(b[27]&&(b[26]||b[24]==REMOVE_UNPAIR_SENT||!vava_u32(b+112))))return false;
    current=(struct operation){.id=g64(b+8),.created=g64(b+16),.slot=b[25],.phase=b[24],.operator_confirmed=b[26],.defaults_only=b[27],.revision=vava_u32(b+112)};memcpy(current.serial,b+32,32);memcpy(current.reason,b+64,48);
    if(!current.id||!removal_token()||removal_token()==UINT32_MAX)return false;
    if(current.phase>=REMOVE_WAITING&&current.phase<=REMOVE_UNPAIR_SENT){current.phase=current.phase==REMOVE_WAITING?REMOVE_FAILED:REMOVE_UNKNOWN;strcpy(current.reason,"process_restarted_no_destructive_replay");return persist(&current);}return true;
}
bool removal_begin(unsigned slot,const char *serial,uint64_t now,uint64_t wall,bool defaults_only,uint32_t revision){
    uint8_t raw[32]={0};char validated[32];if(!healthy||removal_active()||slot>3||strlen(serial)>31)return false;memcpy(raw,serial,strlen(serial));if(!vava_serial(raw,validated))return false;
    /* Keep an unresolved reset quarantined until the same identity is retried. */
    if(defaults_only&&!revision)return false;
    if(current.phase==REMOVE_UNKNOWN&&(current.slot!=slot||strcmp(current.serial,serial)||current.defaults_only!=defaults_only))return false;
    struct operation next={.created=wall,.deadline=now+45000,.slot=slot,.phase=REMOVE_WAITING,.defaults_only=defaults_only,.revision=revision};strcpy(next.serial,serial);strcpy(next.reason,"waiting_for_camera");
    do{if(!myvava_random((uint8_t *)&next.id,8))return false;}while(!(uint32_t)next.id||(uint32_t)next.id==UINT32_MAX);
    if(!persist(&next))return false;current=next;return true;
}
bool removal_change(enum removal_phase phase,const char *reason,uint64_t now){
    if(!healthy||!current.id||phase>REMOVE_UNKNOWN||strlen(reason)>=sizeof(current.reason))return false;
    struct operation next=current;next.phase=phase;strcpy(next.reason,reason);next.deadline=now+15000;
    if(!persist(&next)){current.phase=REMOVE_UNKNOWN;strcpy(current.reason,"removal_state_write_failed");return false;}current=next;return true;
}
bool removal_confirm(uint64_t now){
    if(!healthy||current.phase!=REMOVE_UNKNOWN||current.defaults_only)return false;
    struct operation next=current;next.phase=REMOVE_ACKNOWLEDGED;next.operator_confirmed=true;next.deadline=now+15000;strcpy(next.reason,"operator_confirmed_factory_reset");
    if(!persist(&next))return false;current=next;return true;
}
bool removal_json(char *out,size_t size,uint64_t now){int n=snprintf(out,size,"{\"id\":\"%016" PRIx64 "\",\"action\":\"%s\",\"phase\":\"%s\",\"active\":%s,\"durable\":%s,\"slot\":%u,\"serial\":\"%s\",\"reason\":\"%s\",\"remaining_ms\":%" PRIu64 ",\"recordings_preserved\":true,\"pairing_preserved\":%s,\"operator_confirmed\":%s,\"camera_effect_verified\":false}",current.id,current.defaults_only?"reset":"remove",phases[current.phase],removal_active()?"true":"false",healthy?"true":"false",current.slot,current.serial,current.reason,removal_active()&&current.deadline>now?current.deadline-now:0,current.defaults_only?"true":"false",current.operator_confirmed?"true":"false");return n>0&&(size_t)n<size;}
