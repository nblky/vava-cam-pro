#include "live_mux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t get32(const uint8_t *p){return (uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];}
static void put32(uint8_t *p,uint32_t n){p[0]=(uint8_t)(n>>24);p[1]=(uint8_t)(n>>16);p[2]=(uint8_t)(n>>8);p[3]=(uint8_t)n;}
static bool emit(void *arg,unsigned kind,uint32_t epoch,const uint8_t *p,size_t n){uint8_t h[16]={0};memcpy(h,"MVL2",4);h[4]=(uint8_t)kind;put32(h+8,epoch);put32(h+12,(uint32_t)n);return fwrite(h,1,16,arg)==16&&fwrite(p,1,n,arg)==n;}
int main(int argc,char **argv){
    if(argc!=3)return 2;FILE *input=fopen(argv[1],"rb"),*out=fopen(argv[2],"wb");if(!input||!out)return 2;
    struct live_mux *mux=live_mux_new(emit,out);struct mvm_sample *s=malloc(sizeof(*s)+MVM_FRAME_MAX);if(!mux||!s)return 2;
    uint8_t h[18];int result=0;while(fread(h,1,18,input)==18){*s=(struct mvm_sample){.audio=h[0],.key=h[1],.epoch=get32(h+2),.timestamp=mvm_u64(h+6),.size=get32(h+14)};
        if(s->size>MVM_FRAME_MAX||fread(s->data,1,s->size,input)!=s->size||!live_mux_push(mux,s)){fprintf(stderr,"mux failure epoch=%u audio=%u timestamp=%llu\n",s->epoch,s->audio,(unsigned long long)s->timestamp);result=1;break;}}
    live_mux_free(mux);free(s);fclose(input);fclose(out);return result;
}
