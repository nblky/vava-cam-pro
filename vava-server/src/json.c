#include "json.h"
#include <string.h>
static void ws(const char *s,unsigned *p) { while (s[*p]==' ' || s[*p]=='\n' || s[*p]=='\r' || s[*p]=='\t') ++*p; }
static int hx(char c) { return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1; }
/* Validate literal UTF-8 in opaque result strings without relaxing ASCII-only
 * extraction of command fields, identities, credentials or object keys. */
static bool utf8(const char *s,unsigned *p,unsigned lead) {
    unsigned left=lead>=0xc2&&lead<=0xdf?1:lead>=0xe0&&lead<=0xef?2:lead>=0xf0&&lead<=0xf4?3:0;
    if(!left)return false;unsigned code=lead&((1u<<(6-left))-1),minimum=left==1?0x80:left==2?0x800:0x10000;
    for(unsigned i=0;i<left;++i){unsigned c=(unsigned char)s[*p];if(c<0x80||c>0xbf)return false;++*p;code=code*64+(c&63);}
    return code>=minimum&&code<=0x10ffff&&!(code>=0xd800&&code<=0xdfff);
}
bool json_text(const struct json_doc *d,int i,char *out,size_t cap) {
    if (i<0 || (unsigned)i>=d->count || d->token[i].type!='"' || !cap) return false;
    unsigned p=d->token[i].start+1,end=d->token[i].end-1; size_t n=0;
    while (p<end) {
        unsigned c=(unsigned char)d->text[p++];
        if (c=='\\') {
            if (p==end) return false;
            c=(unsigned char)d->text[p++];
            if (c=='u') { c=0; for (unsigned k=0;k<4;++k) { if(p==end)return false; int x=hx(d->text[p++]);if(x<0)return false;c=c*16+(unsigned)x; } }
            else if(c=='n')c='\n';else if(c=='r')c='\r';else if(c=='t')c='\t';else if(c=='b')c='\b';else if(c=='f')c='\f';
            else if(c!='"'&&c!='\\'&&c!='/')return false;
        }
        if (!c || c>127 || n+1>=cap) return false;
        out[n++]=(char)c;
    }
    out[n]=0;return true;
}
static bool value(struct json_doc *d,unsigned *p,unsigned depth) {
    ws(d->text,p); if(depth>8 || d->count==JSON_TOKENS)return false;
    unsigned i=d->count++;struct json_token *t=&d->token[i];t->start=*p;t->type=d->text[*p];
    if(t->type=='"') {
        ++*p;while(d->text[*p]&&d->text[*p]!='"') {
            unsigned c=(unsigned char)d->text[(*p)++];if(c<32)return false;
            if(c>=128){if(!utf8(d->text,p,c))return false;continue;}
            if(c=='\\') { c=(unsigned char)d->text[(*p)++];if(c=='u'){for(unsigned k=0;k<4;++k)if(hx(d->text[(*p)++])<0)return false;}
                else if(!c||!strchr("\"\\/bfnrt",(int)c))return false; }
        }
        if(d->text[*p]!='"')return false;++*p;
    } else if(t->type=='{'||t->type=='[') {
        char end=t->type=='{'?'}':']';++*p;ws(d->text,p);
        if(d->text[*p]!=end) for(;;) {
            if(t->type=='{') {
                if(d->text[*p]!='"')return false;unsigned key=d->count;
                if(!value(d,p,depth+1))return false;
                char a[128],b[128];if(!json_text(d,(int)key,a,sizeof(a)))return false;
                for(unsigned j=i+1;j<key;) {if(!json_text(d,(int)j,b,sizeof(b))||!strcmp(a,b))return false;j=d->token[d->token[j].next].next;}
                ws(d->text,p);if(d->text[(*p)++]!=':')return false;
            }
            if(!value(d,p,depth+1))return false;++t->count;ws(d->text,p);
            if(d->text[*p]!=',')break;++*p;ws(d->text,p);
        }
        if(d->text[*p]!=end)return false;++*p;
    } else if(!strncmp(d->text+*p,"true",4))*p+=4;
    else if(!strncmp(d->text+*p,"false",5))*p+=5;
    else if(!strncmp(d->text+*p,"null",4))*p+=4;
    else {
        if(d->text[*p]=='-')++*p;
        if(d->text[*p]=='0')++*p;
        else {if(d->text[*p]<'1'||d->text[*p]>'9')return false;while(d->text[*p]>='0'&&d->text[*p]<='9')++*p;}
        if(d->text[*p]=='.'){++*p;if(d->text[*p]<'0'||d->text[*p]>'9')return false;while(d->text[*p]>='0'&&d->text[*p]<='9')++*p;}
        if(d->text[*p]=='e'||d->text[*p]=='E'){++*p;if(d->text[*p]=='+'||d->text[*p]=='-')++*p;if(d->text[*p]<'0'||d->text[*p]>'9')return false;while(d->text[*p]>='0'&&d->text[*p]<='9')++*p;}
    }
    t->end=*p;t->next=d->count;return true;
}
bool json_parse(struct json_doc *d,const char *s) { memset(d,0,sizeof(*d));d->text=s;unsigned p=0;return value(d,&p,0)&&(ws(s,&p),!s[p]); }
int json_field(const struct json_doc *d,unsigned i,const char *key) {
    if(i>=d->count||d->token[i].type!='{')return -1;
    for(unsigned j=i+1;j<d->token[i].next;) {char b[128];if(!json_text(d,(int)j,b,sizeof(b)))return -1;unsigned v=d->token[j].next;if(!strcmp(b,key))return (int)v;j=d->token[v].next;}return -1;
}
bool json_fields(const struct json_doc *d,unsigned i,const char *const *names,unsigned count) {
    if(i>=d->count||d->token[i].type!='{')return false;
    for(unsigned j=i+1;j<d->token[i].next;) {char b[128];if(!json_text(d,(int)j,b,sizeof(b)))return false;unsigned k=0;for(;k<count;++k)if(!strcmp(b,names[k]))break;if(k==count)return false;j=d->token[d->token[j].next].next;}return true;
}
bool json_raw(const struct json_doc *d,int i,char *out,size_t cap) {if(i<0||(unsigned)i>=d->count)return false;size_t n=d->token[i].end-d->token[i].start;if(n>=cap)return false;memcpy(out,d->text+d->token[i].start,n);out[n]=0;return true;}
