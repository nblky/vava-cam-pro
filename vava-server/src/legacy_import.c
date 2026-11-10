/* Offline import of the statically audited VAVA SD container. Original input
 * is O_RDONLY; clear frames exist only in bounded RAM and the selected archive. */
#include "legacy_import.h"
#include "file_store.h"
#include "protocol.h"
#include "ts_mux.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
struct importer {int source;off_t size;struct file_store *store;const char *device;struct myvava_file f;EVP_MD_CTX *sha;uint8_t chunk[MYVAVA_FILE_DATA_MAX],source_sha[32];size_t used;uint32_t offset,skip,total;bool transfer;uint64_t cookie;};
static bool read_at(int fd,void *p,size_t n,off_t at){size_t done=0;while(done<n){ssize_t k=pread(fd,(uint8_t *)p+done,n-done,at+(off_t)done);if(k<0&&errno==EINTR)continue;if(k<=0)return false;done+=(size_t)k;}return true;}
static int exchange(struct importer *c,uint8_t type,const uint8_t *data,size_t size,uint32_t offset,uint32_t *returned){
    struct myvava_message m={.type=type,.id=c->f.id,.total=c->f.size,.offset=offset,.data=data,.length=size};uint64_t cookie=++c->cookie;
    if(!file_store_submit(c->store,cookie,c->device,&m))return -1;uint64_t deadline=myvava_millis()+30000;
    while(myvava_millis()<deadline){uint8_t ack[33];uint64_t got=0;if(file_store_poll(c->store,&got,&m,ack)){if(got!=cookie||m.id!=c->f.id||m.length!=33||memcmp(ack,c->f.sha256,32))return -1;*returned=m.offset;return ack[32];}struct timespec pause={.tv_nsec=1000000};nanosleep(&pause,NULL);}return -1;
}
static bool flush(struct importer *c){if(!c->used)return true;uint8_t data[MYVAVA_CHUNK];memcpy(data,c->f.sha256,32);memcpy(data+32,c->chunk,c->used);uint32_t next=0;bool ok=exchange(c,MYVAVA_FILE_DATA,data,c->used+32,c->offset,&next)==MVF_PARTIAL&&next==c->offset+c->used;if(ok)c->offset=next;c->used=0;return ok;}
static bool output(void *arg,const uint8_t *p,size_t size){struct importer *c=arg;if(size>MYVAVA_FILE_MAX-c->total)return false;c->total+=(uint32_t)size;if(EVP_DigestUpdate(c->sha,p,size)!=1)return false;
    if(!c->transfer)return true;
    if(c->skip){size_t n=size<c->skip?size:c->skip;c->skip-=(uint32_t)n;p+=n;size-=n;}
    while(size){size_t n=sizeof(c->chunk)-c->used;if(n>size)n=size;memcpy(c->chunk+c->used,p,n);c->used+=n;p+=n;size-=n;if(c->used==sizeof(c->chunk)&&!flush(c))return false;}return true;
}
static bool decode(uint8_t *p,size_t n,unsigned kind,unsigned mode){if(!mode)return true;if(kind==0)return true;if(kind==8){if(!n||p[0]!=0xbb)return false;p[0]=0xff;++p;--n;}size_t count=n/16*16;
    const uint8_t key[16]={'v','a','v','a','l','i','c','2',0,0,0,0,0,0,0,0};EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();if(!ctx)return false;int got=0,tail=0;
    bool ok=EVP_DecryptInit_ex(ctx,EVP_aes_128_ecb(),NULL,key,NULL)==1&&EVP_CIPHER_CTX_set_padding(ctx,0)==1&&EVP_DecryptUpdate(ctx,p,&got,p,(int)count)==1&&got==(int)count&&EVP_DecryptFinal_ex(ctx,p+got,&tail)==1&&!tail;EVP_CIPHER_CTX_free(ctx);return ok;
}
static bool convert(struct importer *c,bool second){
    struct ts_mux mux={0};uint8_t header[16],frame[24],source_hash[32],ts_hash[32];uint8_t *payload=malloc(VAVA_MAX_FRAME);EVP_MD_CTX *source=EVP_MD_CTX_new();unsigned length=0;uint64_t payload_bytes=0;unsigned frames=0;
    bool ok=payload&&source&&read_at(c->source,header,16,0)&&header[1]==0&&header[2]==3&&(header[5]==0||header[5]==0x1a)&&EVP_DigestInit_ex(source,EVP_sha256(),NULL)==1&&EVP_DigestUpdate(source,header,16)==1&&EVP_DigestInit_ex(c->sha,EVP_sha256(),NULL)==1;
    c->total=0;
    for(off_t at=16;ok&&at<c->size;){ok=c->size-at>=24&&read_at(c->source,frame,24,at);if(!ok)break;at+=24;unsigned size=vava_u32(frame+4),kind=vava_u32(frame+8),seconds=vava_u32(frame+16),ms=vava_u32(frame+20);
        ok=vava_u32(frame)==0xeb0000aa&&size>0&&size<=VAVA_MAX_FRAME&&(kind==0||kind==1||kind==8)&&ms<1000&&seconds<=180&&size<=(uint64_t)(c->size-at)&&read_at(c->source,payload,size,at);if(!ok)break;at+=size;payload_bytes+=size;
        ok=EVP_DigestUpdate(source,frame,24)==1&&EVP_DigestUpdate(source,payload,size)==1&&decode(payload,size,kind,header[5])&&ts_mux_frame(&mux,kind==8,kind==1,(uint64_t)seconds*1000+ms,payload,size,output,c);if(kind!=8)++frames;
    }
    ok=ok&&frames&&payload_bytes==vava_u32(header+8)&&EVP_DigestFinal_ex(source,source_hash,&length)==1&&length==32&&EVP_DigestFinal_ex(c->sha,ts_hash,&length)==1&&length==32;
    if(ok&&second)ok=!memcmp(c->source_sha,source_hash,32)&&!memcmp(c->f.sha256,ts_hash,32)&&c->total==c->f.size&&flush(c);
    else if(ok){c->f.size=c->total;c->f.frames=frames;c->f.audio_frames=(uint32_t)mux.audio_frames;c->f.first_ms=mux.first_ms;c->f.last_ms=mux.last_ms;c->f.sample_rate=mux.sample_rate;c->f.channels=(uint8_t)mux.channels;c->f.file_type=header[0]>=5&&header[0]<=8?header[0]:0;memcpy(c->source_sha,source_hash,32);memcpy(c->f.sha256,ts_hash,32);}
    EVP_MD_CTX_free(source);free(payload);return ok;
}
static bool decimal(const char *s,uint64_t max,uint64_t *out){if(!s||!*s||*s<'0'||*s>'9')return false;char *end;errno=0;unsigned long long n=strtoull(s,&end,10);if(errno||*end||n>max)return false;*out=n;return true;}
int legacy_import_main(int argc,char **argv){
    if(argc!=11){fprintf(stderr,"usage: vava-server --import-original FILE ARCHIVE_DIR DEVICE SERIAL SLOT UNIX_SECONDS KEYRING_OR_DASH QUOTA_MIB FILE_LIMIT\n");return 2;}
    uint64_t slot,stamp,quota,limit;size_t sn=strlen(argv[5]);if(!myvava_id_valid(argv[4])||!sn||sn>=32||!decimal(argv[6],3,&slot)||!decimal(argv[7],INT64_MAX,&stamp)||!decimal(argv[9],1048576,&quota)||!quota||!decimal(argv[10],4096,&limit)||!limit)return 2;
    for(size_t i=0;i<sn;++i){unsigned char ch=argv[5][i];if(!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'||ch=='.'||ch==':'))return 2;}
    umask(0077);struct importer c={.source=-1,.device=argv[4],.f={.slot=(uint8_t)slot,.pir=1,.unix_seconds=stamp,.clock_valid=stamp!=0}};strcpy(c.f.serial,argv[5]);
    c.source=open(argv[2],O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);struct stat st;bool ok=c.source>=0&&!fstat(c.source,&st)&&S_ISREG(st.st_mode)&&st.st_size>=16&&st.st_size<=MYVAVA_FILE_MAX;
    if(ok){c.size=st.st_size;c.sha=EVP_MD_CTX_new();ok=c.sha&&convert(&c,false);}uint8_t identity[128]={0},digest[32];unsigned length=0;
    if(ok){memcpy(identity,c.source_sha,32);memcpy(identity+32,c.device,strlen(c.device));memcpy(identity+96,c.f.serial,32);ok=EVP_Digest(identity,sizeof(identity),digest,&length,EVP_sha256(),NULL)==1&&length==32;for(unsigned i=0;i<8;++i)c.f.id=c.f.id*256+digest[i];c.f.id|=UINT64_C(1)<<63;}
    uint8_t desc[128];if(ok)ok=myvava_file_encode(desc,&c.f);if(ok){c.store=file_store_start_encrypted(argv[3],quota*1024*1024,(unsigned)limit,strcmp(argv[8],"-")?argv[8]:NULL);ok=c.store!=NULL;}
    uint32_t offset=0;int state=-1;if(ok){state=exchange(&c,MYVAVA_FILE_OFFER,desc,128,0,&offset);ok=state==MVF_PARTIAL||state==MVF_COMMITTED;}
    if(ok&&state==MVF_PARTIAL){c.offset=c.skip=offset;c.transfer=true;ok=convert(&c,true);if(ok)ok=exchange(&c,MYVAVA_FILE_COMMIT,c.f.sha256,32,c.f.size,&offset)==MVF_COMMITTED;}
    if(ok)ok=file_store_mark_imported(c.store,c.device,desc);
    file_store_stop(c.store);EVP_MD_CTX_free(c.sha);if(c.source>=0)close(c.source);
    if(ok){char hash[65];myvava_hex(c.source_sha,32,hash);printf("{\"ok\":true,\"id\":\"%016" PRIx64 "\",\"source_sha256\":\"%s\",\"frames\":%u,\"audio_frames\":%u}\n",c.f.id,hash,c.f.frames,c.f.audio_frames);}else fprintf(stderr,"original import failed; input preserved; check format, destination identity, keyring, quota, and stop the archive server first\n");return ok?0:1;
}
