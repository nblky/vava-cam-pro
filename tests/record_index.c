#include "record_index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
int main(void){char dir[]="/tmp/myvava-index-XXXXXX";CHECK(mkdtemp(dir));CHECK(record_index_open(dir));
    for(unsigned i=10000;i;i--){struct record_info r={.id=i,.slot=i%4,.bytes=188,.stored_bytes=188,.frames=i,.transport_stream=true,.file_type=(uint8_t)(5+i%4),.detection_reported=true,.detection_result=1,.detection_type=i};snprintf(r.serial,sizeof(r.serial),"CAM-%u",i%4);CHECK(record_index_add(&r));}
    record_index_sort();CHECK(record_index_count()==10000);struct record_info batch[16],r;unsigned total=0;
    CHECK(record_index_page(0,"CAM-0",150,batch,16,&total)==16);CHECK(total==2500&&batch[0].id==400&&batch[15].id==340);
    CHECK(record_index_get(2,"CAM-2",2,&r)&&r.frames==2);CHECK(!record_index_get(1,"CAM-1",2,&r));
    uint64_t before=0;unsigned count=0,n;while((n=record_index_scan(before,batch,16))){CHECK(n<=16);for(unsigned k=1;k<n;++k)CHECK(batch[k-1].id>batch[k].id);count+=n;before=batch[n-1].id;}CHECK(count==10000);
    for(unsigned i=1;i<=9900;++i)CHECK(record_index_remove(i));CHECK(!record_index_has(100));
    r.id=10001;r.slot=2;strcpy(r.serial,"CAM-2");CHECK(record_index_add(&r));CHECK(record_index_count()==101);
    CHECK(record_index_get(0,"CAM-0",10000,&r)&&r.frames==10000&&r.file_type==5&&r.detection_type==10000);CHECK(record_index_get(2,"CAM-2",10001,&r));
    record_index_close();CHECK(record_index_open(dir));CHECK(record_index_count()==0);record_index_close();char name[1024];snprintf(name,sizeof(name),"%s/.catalog.cache",dir);unlink(name);rmdir(dir);puts("record index: 10000 rows, identity pages, bounded scanning, compaction and rebuild passed");return 0;
}
