/* Standalone libFuzzer entry point; build/run instructions are in the review. */
#include "myvava.h"
#include "files.h"
#include "events.h"
#include "http.h"
#include "h264.h"
#include "json.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data,size_t size){
    if(!size||size>32768)return 0;
    unsigned kind=*data++%6;--size;
    if(kind==0){
        char *text=malloc(size+1);if(!text)return 0;memcpy(text,data,size);text[size]=0;
        struct json_doc doc;
        if(json_parse(&doc,text)){
            char out[256];(void)json_field(&doc,0,"command");
            for(unsigned i=0;i<doc.count;++i){(void)json_text(&doc,(int)i,out,sizeof(out));(void)json_raw(&doc,(int)i,out,sizeof(out));}
        }
        free(text);
    }else if(kind==1){
        struct http_request request;
        if(http_parse(data,size,&request)>0){(void)http_same_origin(&request);(void)http_preflight_valid(&request);}
    }else if(kind==2){
        struct myvava_message message;
        if(myvava_decode(data,size,&message)){uint8_t out[MYVAVA_FILE_PACKET_MAX];(void)myvava_encode_sized(out,sizeof(out),&message);}
    }else if(kind==3){
        struct h264_config config={0};unsigned width,height;bool key,changed;uint8_t out[32768];
        (void)h264_valid(data,size);(void)h264_dimensions(data,size,&width,&height);
        (void)h264_sps_low_delay(data,size,out,sizeof(out));(void)h264_without_parameter_sets(data,size,out,sizeof(out));
        /* Input can contain two AUs to exercise parameter-set state changes. */
        size_t split=size?data[0]%size:0;
        (void)h264_config_frame(&config,data,split,&key,&changed);
        (void)h264_config_frame(&config,data+split,size-split,&key,&changed);
    }else if(kind==4){
        struct myvava_file file;
        if(myvava_file_decode(data,size,&file)){uint8_t out[MYVAVA_FILE_DESC];(void)myvava_file_encode(out,&file);}
    }else{
        struct myvava_event event;
        if(myvava_event_decode(data,size,&event)){char text[1024];uint8_t out[MYVAVA_EVENT_SIZE];myvava_event_encode(out,&event);(void)myvava_event_json(&event,text,sizeof(text));}
    }
    return 0;
}
