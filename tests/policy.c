#include "policy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
int main(void) {
    struct policy p; policy_default(&p); p.managed=true; p.mode=ARM_SCHEDULE; p.utc_offset_minutes=0;
    /* Unix epoch is Thursday. A Wednesday interval crosses into Thursday. */
    p.plans[0]=(struct plan){.enabled=true,.days=1u<<3,.start=23*3600,.end=3600};
    CHECK(policy_evaluate(&p,0,true)); CHECK(policy_evaluate(&p,3599,true));
    CHECK(!policy_evaluate(&p,3600,true)); CHECK(!policy_evaluate(&p,0,false));
    p.plans[0]=(struct plan){.enabled=true,.days=1u<<4,.start=3600,.end=7200};
    CHECK(!policy_evaluate(&p,3599,true)); CHECK(policy_evaluate(&p,3600,true));
    CHECK(policy_evaluate(&p,7199,true)); CHECK(!policy_evaluate(&p,7200,true));
    p.plans[0].enabled=false;
    CHECK(!policy_evaluate(&p,3600,true)); CHECK(policy_valid(&p));
    char json[2048]; CHECK(policy_json(&p,false,true,"test",0,json,sizeof(json)));
    CHECK(strstr(json,"\"enabled\":false") && strstr(json,"\"index\":0"));
    p.plans[0].end=86400; CHECK(!policy_valid(&p));
    p.plans[0].end=7200;p.plans[0].enabled=true;
    p.utc_offset_minutes=60;
    CHECK(policy_evaluate(&p,0,true)); CHECK(!policy_evaluate(&p,3600,true));
    p.utc_offset_minutes=-60;
    CHECK(!policy_evaluate(&p,7199,true)); CHECK(policy_evaluate(&p,7200,true));
    p.utc_offset_minutes=0;
    p.plans[1]=(struct plan){.enabled=true,.days=1u<<6,.start=86399,.end=1};
    CHECK(policy_evaluate(&p,3*86400,true)); CHECK(!policy_evaluate(&p,3*86400+1,true));
    p.plans[2]=(struct plan){.enabled=true,.all_day=true,.days=1u<<1};
    CHECK(policy_evaluate(&p,4*86400,true)); CHECK(!policy_evaluate(&p,5*86400,true));
    unsigned value; CHECK(policy_time("23:59:59",&value) && value==86399);
    CHECK(!policy_time("24:00:00",&value)); CHECK(!policy_time("0a:00:00",&value));
    CHECK(policy_days("0123456",&value) && value==127); CHECK(!policy_days("11",&value));
    p.enabled=false; CHECK(!policy_evaluate(&p,3600,true)); p.enabled=true;
    p.mode=ARM_ON; CHECK(policy_evaluate(&p,0,false));
    struct policy all[4], loaded[4]; struct registration r[4]={0};
    for (unsigned i=0;i<4;++i) policy_default(&all[i]);
    all[0]=p; r[0].stage=3; strcpy(r[0].serial,"POLICY-TEST"); strcpy(r[0].mac,"02:00:00:00:00:01");
    all[0].plans[2].enabled=false;
    char dir[]="/tmp/vava-policy-XXXXXX"; CHECK(mkdtemp(dir)!=NULL);
    CHECK(!policy_save(dir,r,all)); CHECK(!policy_load(dir,r,loaded));
    CHECK(loaded[0].managed && loaded[0].mode==ARM_ON && loaded[0].plans[2].all_day);
    CHECK(!loaded[0].plans[2].enabled && loaded[0].plans[2].days==(1u<<1));
    strcpy(r[0].serial,"OTHER-DEVICE"); CHECK(!policy_load(dir,r,loaded)); CHECK(!loaded[0].managed);
    char name[256]; snprintf(name,sizeof(name),"%s/policies.bin",dir);
    FILE *f=fopen(name,"r+b"); CHECK(f!=NULL); CHECK(!fseek(f,-1,SEEK_END));
    int byte=fgetc(f); CHECK(byte!=EOF); CHECK(!fseek(f,-1,SEEK_END));
    CHECK(fputc(byte^1,f)!=EOF); CHECK(!fclose(f));
    CHECK(policy_load(dir,r,loaded)<0);
    unlink(name); rmdir(dir);
    puts("policy: boundary, midnight/week wrap, clock, validation, persistence and identity passed");
    return 0;
}
