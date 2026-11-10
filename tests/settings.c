#include "settings.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at line %d: %s\n",__LINE__,#x); exit(1); } } while (0)

int main(void) {
    struct settings s={0};
    /* A partial write is not sent, even after unrelated preceding packets flush. */
    settings_begin(&s,0,2,2,1,0,7,100,384);
    settings_written(&s,256,200);
    settings_written(&s,383,300);
    CHECK(s.operations[0].phase==SET_QUEUED);
    settings_observe(&s,11,s.operations[0].payload,7,400);
    CHECK(s.active==1); /* premature echo cannot acknowledge unsent bytes */
    CHECK(!settings_tick(&s,5099));
    CHECK(settings_tick(&s,5100)); /* caller must close to discard pending bytes */
    CHECK(s.operations[0].phase==SET_QUEUE_TIMEOUT && !s.active);
    settings_disconnect(&s);
    CHECK(s.operations[0].phase==SET_QUEUE_TIMEOUT);

    settings_begin(&s,1,1,1,2,0,8,10000,512);
    settings_written(&s,512,11000);
    CHECK(s.operations[1].phase==SET_SENT);
    settings_observe(&s,12,s.operations[1].payload,7,11001);
    CHECK(s.active==2); /* old generation */
    settings_observe(&s,12,s.operations[1].payload,8,14000);
    CHECK(s.active==2); /* exact bytes at the deadline are already too late */
    CHECK(!settings_tick(&s,14000));
    CHECK(s.operations[1].phase==SET_TIMEOUT && !s.active);

    settings_begin(&s,2,60,1,3,0,8,15000,640);
    settings_written(&s,640,15001);
    settings_disconnect(&s);
    settings_observe(&s,16,s.operations[2].payload,8,15002);
    CHECK(s.operations[2].phase==SET_DISCONNECTED && !s.active);

    uint8_t payload[116]; memset(payload,255,sizeof(payload));
    const uint32_t reports[]={5,37,42,203,30,17};
    for (unsigned j=0;j<6;++j) settings_observe(&s,reports[j],payload,8,15003);
    /* Fill every operation/report to check the actual management response bound. */
    settings_begin(&s,SETTING_PIR_LEVEL,3,3,4,0,8,15003,768);
    settings_disconnect(&s);
    const uint32_t pir[]={2,3,120,60,1};
    settings_begin_pir(&s,pir,5,0,8,15003,896);
    settings_disconnect(&s);
    for (unsigned j=5;j<SETTINGS_COUNT;++j) {
        settings_begin(&s,j,1,1,100+j,0,8,15003,1024+128*j);
        settings_disconnect(&s);
    }
    char json[8192];
    CHECK(settings_json(&s,8,true,15004,json,sizeof(json)));
    CHECK(strlen(json)<sizeof(json)-1);
    CHECK(!settings_json(&s,8,true,15004,json,32));
    CHECK(!settings_json(&s,8,true,15004,json,0));
    memset(&s,0,sizeof(s));
    CHECK(!settings_detection_ready(&s,9));
    uint8_t algorithm[116]={0,0,75,0,0,0,0,100,0,100};
    settings_observe(&s,203,algorithm,9,20000);
    CHECK(settings_detection_ready(&s,9));CHECK(!settings_detection_ready(&s,10));
    algorithm[7]=0;settings_observe(&s,203,algorithm,9,20001);CHECK(!settings_detection_ready(&s,9));
    uint32_t wire=0;CHECK(settings_index("detection-box",1,&wire)==SETTING_DETECTION_BOX&&wire==1);
    CHECK(settings_index("detection-box",2,&wire)==-1);CHECK(settings_index("quality",3,&wire)==-1);
    settings_begin(&s,SETTING_DETECTION_BOX,1,1,121,3,9,20002,128);
    const uint8_t *box=s.operations[SETTING_DETECTION_BOX].payload;
    CHECK(vava_u32(box)==1&&vava_u32(box+4)==75&&vava_u32(box+8)==1);
    CHECK(vava_u32(box+12)==121&&vava_u32(box+16)==3);
    settings_written(&s,128,20003);uint8_t bad[116];memcpy(bad,box,116);bad[12]^=1;
    settings_observe(&s,33,bad,9,20004);CHECK(s.active);
    settings_observe(&s,33,box,9,20005);CHECK(!s.active&&s.operations[SETTING_DETECTION_BOX].phase==SET_ECHO);
    puts("settings: partial writes, deadlines, generation isolation, disconnect and bounded JSON passed");
    return 0;
}
