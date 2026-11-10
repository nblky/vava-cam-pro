#include "json.h"
#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){
    struct json_doc d;char raw[128],ascii[32];
    const char *s="{\"device\":\"BASE_A\",\"result\":\"{\\\"name\\\":\\\"门前 📷\\\"}\"}";
    assert(json_parse(&d,s));assert(json_text(&d,json_field(&d,0,"device"),ascii,sizeof(ascii))&&!strcmp(ascii,"BASE_A"));
    assert(json_raw(&d,json_field(&d,0,"result"),raw,sizeof(raw)));assert(strstr(raw,"门前 📷"));
    assert(!json_text(&d,json_field(&d,0,"result"),ascii,sizeof(ascii)));
    const char *bad[]={"\"\xc0\x80\"","\"\xed\xa0\x80\"","\"\xf4\x90\x80\x80\"","\"\xe0\x80\x80\"","\"\xf0\x80\x80\x80\"","\"\x80\"","\"\xf5\x80\x80\x80\"","\"\xc2","\"\xe4\xb8","\"\xf0\x9f\x93","{\"name\":1,\"name\":2}","{\"中文\":1}"};
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)assert(!json_parse(&d,bad[i]));
    assert(json_parse(&d,"\"\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf\""));
    puts("UTF-8 result preservation, malformed sequence rejection and ASCII field boundaries passed");return 0;
}
