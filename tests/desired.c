#include "desired.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static void load_bytes(const char *path,uint8_t b[1040]){FILE *f=fopen(path,"rb");CHECK(f);CHECK(fread(b,1,1040,f)==1040);CHECK(!fclose(f));}
static void write_bytes(const char *path,uint8_t b[1040],size_t n){FILE *f=fopen(path,"wb");CHECK(f);CHECK(fwrite(b,1,n,f)==n);CHECK(!fclose(f));}
int main(void){
    char dir[]="/tmp/vava-desired-XXXXXX",path[256];CHECK(mkdtemp(dir));snprintf(path,sizeof(path),"%s/settings.bin",dir);
    struct registration r[4]={0};struct desired d[4]={0},actual[4],zero[4]={0};uint8_t bytes[1040];
    for(unsigned i=0;i<4;++i){r[i].stage=3;snprintf(r[i].serial,32,"CAM%u",i);snprintf(r[i].mac,18,"02:00:00:00:00:%02x",i);
        d[i].revision=i+10;d[i].mask=1u;d[i].values[0][0]=i;}
    CHECK(!desired_save(dir,r,d));load_bytes(path,bytes);CHECK(!memcmp(bytes,"VAVACFG1",8));
    CHECK(!desired_load(dir,r,actual));CHECK(!memcmp(actual,d,sizeof(d)));
    for(unsigned i=0;i<4;++i){d[i].mask|=1u<<SETTING_DETECTION_BOX;d[i].values[SETTING_DETECTION_BOX][0]=i%2;}
    CHECK(!desired_save(dir,r,d));load_bytes(path,bytes);CHECK(!memcmp(bytes,"VAVACFG2",8));
    CHECK(!desired_load(dir,r,actual));CHECK(!memcmp(actual,d,sizeof(d)));
    r[1].serial[0]='X';CHECK(!desired_load(dir,r,actual));CHECK(!memcmp(&actual[1],&zero[1],sizeof(actual[1])));CHECK(!memcmp(&actual[3],&d[3],sizeof(d[3])));r[1].serial[0]='C';
    for(unsigned n=0;n<16;++n){write_bytes(path,bytes,n);CHECK(desired_load(dir,r,actual)==-1);CHECK(!memcmp(actual,zero,sizeof(zero)));}
    bytes[1039]^=1;write_bytes(path,bytes,1040);CHECK(desired_load(dir,r,actual)==-1);bytes[1039]^=1;
    // A corrupt later record cannot partially install earlier valid slots.
    vava_put32(bytes+16+3*256+60,1u<<31);vava_put32(bytes+12,vava_crc32(bytes+16,1024));
    write_bytes(path,bytes,1040);CHECK(desired_load(dir,r,actual)==-1);CHECK(!memcmp(actual,zero,sizeof(zero)));
    CHECK(!desired_save(dir,r,d));load_bytes(path,bytes);memcpy(bytes,"VAVACFG1",8);write_bytes(path,bytes,1040);CHECK(desired_load(dir,r,actual)==-1);
    for(unsigned i=0;i<4;++i){d[i].mask&=~(1u<<SETTING_DETECTION_BOX);d[i].values[SETTING_DETECTION_BOX][0]=0;}
    CHECK(!desired_save(dir,r,d));load_bytes(path,bytes);CHECK(!memcmp(bytes,"VAVACFG1",8));CHECK(!desired_load(dir,r,actual));CHECK(!memcmp(actual,d,sizeof(d)));
    CHECK(!unlink(path));CHECK(!rmdir(dir));puts("desired: v1/v2 four-slot persistence, identity, corruption and migration passed");return 0;
}
