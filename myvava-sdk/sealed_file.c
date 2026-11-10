#include "directory_scan.h"
#include "sealed_file.h"
#include "myvava.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/kdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define SEAL_MAX (128u*1024u*1024u)
struct key {uint8_t id[16],secret[32];};
struct seal_keys {struct key key[SEAL_KEYS_MAX];unsigned count,active;char active_text[33];};
struct seal_writer {int fd;uint8_t header[SEAL_HEADER],keys[64],block[SEAL_BLOCK];size_t used;uint64_t bytes,index;bool failed,finished;};
struct seal_reader {int fd;uint8_t header[SEAL_HEADER],keys[64],cache[SEAL_BLOCK];uint64_t bytes,cached;size_t cache_size;bool valid;};
static void put64(uint8_t *p,uint64_t x){for(unsigned i=0;i<8;++i){p[7-i]=(uint8_t)x;x>>=8;}}
static uint64_t get64(const uint8_t *p){uint64_t x=0;for(unsigned i=0;i<8;++i)x=x*256+p[i];return x;}
static bool write_all(int fd,const void *p,size_t n){while(n){ssize_t k=write(fd,p,n);if(k<0&&errno==EINTR)continue;if(k<=0)return false;p=(const uint8_t *)p+k;n-=(size_t)k;}return true;}
static bool read_at(int fd,void *p,size_t n,uint64_t offset){while(n){ssize_t k=pread(fd,p,n,(off_t)offset);if(k<0&&errno==EINTR)continue;if(k<=0)return false;p=(uint8_t *)p+k;offset+=(size_t)k;n-=(size_t)k;}return true;}
static int private_dir(const char *path,bool create){struct stat st;if(create&&mkdir(path,0700)&&errno!=EEXIST)return -1;if(lstat(path,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077))return -1;return open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);}
static int private_file(int dir,const char *name,int flags){int fd=openat(dir,name,flags|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600);struct stat st;if(fd<0)return -1;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=geteuid()||(st.st_mode&0077)||st.st_nlink!=1){close(fd);return -1;}return fd;}
static bool read_hex(int dir,const char *name,uint8_t *out,size_t bytes){int fd=private_file(dir,name,O_RDONLY);if(fd<0)return false;char b[66]={0};struct stat st;bool ok=!fstat(fd,&st)&&(st.st_size==(off_t)(bytes*2)||st.st_size==(off_t)(bytes*2+1))&&read_at(fd,b,(size_t)st.st_size,0);close(fd);if(!ok)return false;if(st.st_size==(off_t)(bytes*2+1)&&b[bytes*2]!='\n')return false;b[bytes*2]=0;return myvava_unhex(b,out,bytes);}
static bool key_id(const uint8_t key[32],uint8_t id[16]){uint8_t b[32];unsigned n=0;bool ok=EVP_Digest(key,32,b,&n,EVP_sha256(),NULL)==1&&n==32;if(ok)memcpy(id,b,16);OPENSSL_cleanse(b,sizeof(b));return ok;}
struct seal_keys *seal_keys_load(const char *path){int dir=private_dir(path,false);if(dir<0)return NULL;struct seal_keys *keys=calloc(1,sizeof(*keys));if(!keys){close(dir);return NULL;}uint8_t active[16];bool ok=read_hex(dir,"active",active,16);DIR *scan=directory_scan_open(dir);if(!scan)ok=false;struct dirent *e;
    while(ok&&(e=directory_scan_next(scan,&ok))){size_t n=strlen(e->d_name);if(n<4||strcmp(e->d_name+n-4,".key"))continue;if(n!=36||keys->count==SEAL_KEYS_MAX){ok=false;break;}char id[33];memcpy(id,e->d_name,32);id[32]=0;struct key *k=&keys->key[keys->count];uint8_t expected[16];
        if(!myvava_unhex(id,k->id,16)||!read_hex(dir,e->d_name,k->secret,32)||!key_id(k->secret,expected)||CRYPTO_memcmp(k->id,expected,16)){ok=false;break;}
        ++keys->count;
    }
    if(scan)closedir(scan);close(dir);bool found=false;
    if(ok)for(unsigned i=0;i<keys->count;++i)if(!memcmp(active,keys->key[i].id,16)){keys->active=i;found=true;break;}
    if(!ok||!found){seal_keys_free(keys);return NULL;}myvava_hex(active,16,keys->active_text);return keys;
}
void seal_keys_free(struct seal_keys *keys){if(keys){OPENSSL_cleanse(keys,sizeof(*keys));free(keys);}}
const char *seal_keys_active(const struct seal_keys *keys){return keys?keys->active_text:NULL;}
bool seal_keys_generate(const char *path,bool rotate,char id[33]) {
    int dir=private_dir(path,true);if(dir<0)return false;int lockfd=private_file(dir,".keyring-lock",O_RDWR|O_CREAT);struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    if(lockfd<0||fcntl(lockfd,F_SETLK,&lock)){if(lockfd>=0)close(lockfd);close(dir);return false;}
    struct seal_keys *old=seal_keys_load(path);bool ok=rotate?old&&old->count<SEAL_KEYS_MAX:!old;
    if(!rotate){DIR *scan=directory_scan_open(dir);struct dirent *e;if(!scan)ok=false;while(ok&&(e=directory_scan_next(scan,&ok)))if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")&&strcmp(e->d_name,".keyring-lock"))ok=false;if(scan)closedir(scan);}
    seal_keys_free(old);uint8_t secret[32],kid[16];char text[65],name[40],temp[64];temp[0]=0;
    if(ok)ok=myvava_random(secret,32)&&key_id(secret,kid);
    if(ok){myvava_hex(kid,16,id);myvava_hex(secret,32,text);snprintf(name,sizeof(name),"%s.key",id);int fd=private_file(dir,name,O_WRONLY|O_CREAT|O_EXCL);ok=fd>=0&&write_all(fd,text,64)&&!fsync(fd);if(fd>=0&&close(fd))ok=false;if(ok)ok=!fsync(dir);}
    if(ok){snprintf(temp,sizeof(temp),".active-%s",id);int fd=private_file(dir,temp,O_WRONLY|O_CREAT|O_EXCL);ok=fd>=0&&write_all(fd,id,32)&&!fsync(fd);if(fd>=0&&close(fd))ok=false;if(ok)ok=!renameat(dir,temp,dir,"active")&&!fsync(dir);}
    if(temp[0])unlinkat(dir,temp,0);OPENSSL_cleanse(secret,sizeof(secret));OPENSSL_cleanse(text,sizeof(text));close(lockfd);close(dir);return ok;
}
uint64_t seal_size(uint64_t n){return n>SEAL_MAX?0:SEAL_HEADER+n+16*((n+SEAL_BLOCK-1)/SEAL_BLOCK);}
static bool derive(const uint8_t key[32],const uint8_t salt[32],uint8_t out[64]) {
    static const uint8_t info[]="myvava-storage-aes256gcm-v1";EVP_PKEY_CTX *ctx=EVP_PKEY_CTX_new_id(EVP_PKEY_HKDF,NULL);if(!ctx)return false;size_t n=64;
    bool ok=EVP_PKEY_derive_init(ctx)>0&&EVP_PKEY_CTX_set_hkdf_md(ctx,EVP_sha256())>0&&EVP_PKEY_CTX_set1_hkdf_salt(ctx,salt,32)>0&&EVP_PKEY_CTX_set1_hkdf_key(ctx,key,32)>0&&EVP_PKEY_CTX_add1_hkdf_info(ctx,info,sizeof(info)-1)>0&&EVP_PKEY_derive(ctx,out,&n)>0&&n==64;EVP_PKEY_CTX_free(ctx);return ok;
}
static bool header_tag(const uint8_t keys[64],const uint8_t header[SEAL_HEADER],uint8_t tag[16]) {uint8_t full[32];unsigned n=0;bool ok=HMAC(EVP_sha256(),keys+32,32,header,72,full,&n)&&n==32;if(ok)memcpy(tag,full,16);OPENSSL_cleanse(full,sizeof(full));return ok;}
static bool seal_crypt(bool encrypt,const uint8_t key[32],const uint8_t header[SEAL_HEADER],uint64_t index,void *data,size_t size,uint8_t tag[16]) {
    uint8_t nonce[12]={0},aad[76];put64(nonce+4,index);memcpy(aad,header,64);put64(aad+64,index);aad[72]=(uint8_t)(size>>24);aad[73]=(uint8_t)(size>>16);aad[74]=(uint8_t)(size>>8);aad[75]=(uint8_t)size;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();if(!ctx)return false;int n=0,last=0;
    bool ok=EVP_CipherInit_ex(ctx,EVP_aes_256_gcm(),NULL,NULL,NULL,encrypt)==1&&EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,12,NULL)==1&&EVP_CipherInit_ex(ctx,NULL,NULL,key,nonce,encrypt)==1&&EVP_CipherUpdate(ctx,NULL,&n,aad,sizeof(aad))==1&&EVP_CipherUpdate(ctx,data,&n,data,(int)size)==1&&n==(int)size;
    if(ok&&!encrypt)ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,16,tag)==1;
    if(ok)ok=EVP_CipherFinal_ex(ctx,(uint8_t *)data+n,&last)==1&&last==0;
    if(ok&&encrypt)ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,16,tag)==1;
    EVP_CIPHER_CTX_free(ctx);return ok;
}
bool seal_capsule_encrypt(const struct seal_keys *keys,uint64_t id,const void *plain,size_t n,void *out) {
    if(!keys||!id||!plain||!out||!n||n>SEAL_BLOCK)return false;uint8_t *p=out,k[64];memset(p,0,SEAL_HEADER);memcpy(p,"MVSEAL01",8);memcpy(p+8,keys->key[keys->active].id,16);put64(p+56,id);put64(p+64,n);memcpy(p+SEAL_HEADER,plain,n);
    bool ok=myvava_random(p+24,32)&&derive(keys->key[keys->active].secret,p+24,k)&&header_tag(k,p,p+72)&&seal_crypt(true,k,p,0,p+SEAL_HEADER,n,p+SEAL_HEADER+n);OPENSSL_cleanse(k,sizeof(k));if(!ok)OPENSSL_cleanse(p,SEAL_HEADER+n+16);return ok;
}
bool seal_capsule_decrypt(const struct seal_keys *keys,uint64_t id,const void *data,size_t n,void *out) {
    if(!keys||!id||!data||!out||n<=SEAL_HEADER+16||n>SEAL_HEADER+SEAL_BLOCK+16)return false;const uint8_t *p=data;size_t bytes=n-SEAL_HEADER-16;
    if(memcmp(p,"MVSEAL01",8)||get64(p+56)!=id||get64(p+64)!=bytes)return false;unsigned i=0;for(;i<keys->count;++i)if(!memcmp(keys->key[i].id,p+8,16))break;if(i==keys->count)return false;
    uint8_t k[64],tag[16];bool ok=derive(keys->key[i].secret,p+24,k)&&header_tag(k,p,tag)&&!CRYPTO_memcmp(tag,p+72,16);
    if(ok){memcpy(out,p+SEAL_HEADER,bytes);memcpy(tag,p+SEAL_HEADER+bytes,16);ok=seal_crypt(false,k,p,0,out,bytes,tag);}
    OPENSSL_cleanse(k,sizeof(k));if(!ok)OPENSSL_cleanse(out,bytes);return ok;
}
struct seal_writer *seal_writer_open(int fd,const struct seal_keys *keys,uint64_t id) {
    if(fd<0||!keys||!id)return NULL;struct seal_writer *w=calloc(1,sizeof(*w));if(!w)return NULL;w->fd=fd;memcpy(w->header,"MVSEAL01",8);memcpy(w->header+8,keys->key[keys->active].id,16);put64(w->header+56,id);
    if(!myvava_random(w->header+24,32)||!derive(keys->key[keys->active].secret,w->header+24,w->keys)||!write_all(fd,w->header,SEAL_HEADER)){seal_writer_free(w);return NULL;}return w;
}
static bool flush_block(struct seal_writer *w) {
    if(!w->used)return true;uint8_t tag[16];bool ok=seal_crypt(true,w->keys,w->header,w->index,w->block,w->used,tag)&&write_all(w->fd,w->block,w->used)&&write_all(w->fd,tag,16);if(!ok)w->failed=true;else{++w->index;w->used=0;}return ok;
}
bool seal_writer_write(struct seal_writer *w,const void *p,size_t n) {
    if(!w||w->failed||w->finished||n>SEAL_MAX-w->bytes)return false;
    while(n){size_t k=SEAL_BLOCK-w->used;if(k>n)k=n;memcpy(w->block+w->used,p,k);w->used+=k;w->bytes+=k;p=(const uint8_t *)p+k;n-=k;if(w->used==SEAL_BLOCK&&!flush_block(w))return false;}return true;
}
bool seal_writer_finish(struct seal_writer *w) {
    if(!w||w->failed||w->finished||!w->bytes||!flush_block(w))return false;put64(w->header+64,w->bytes);
    if(!header_tag(w->keys,w->header,w->header+72))return false;size_t written=0;
    while(written<SEAL_HEADER){ssize_t n=pwrite(w->fd,w->header+written,SEAL_HEADER-written,(off_t)written);if(n<0&&errno==EINTR)continue;if(n<=0){w->failed=true;return false;}written+=(size_t)n;}
    w->finished=true;return true;
}
void seal_writer_free(struct seal_writer *w){if(w){OPENSSL_cleanse(w,sizeof(*w));free(w);}}
struct seal_reader *seal_reader_open(int fd,const struct seal_keys *keys,uint64_t id,uint64_t bytes) {
    if(fd<0||!keys||!id||!bytes||bytes>SEAL_MAX)return NULL;struct stat st;if(fstat(fd,&st)||!S_ISREG(st.st_mode)||(uint64_t)st.st_size!=seal_size(bytes))return NULL;
    struct seal_reader *r=calloc(1,sizeof(*r));if(!r)return NULL;r->fd=fd;r->bytes=bytes;
    if(!read_at(fd,r->header,SEAL_HEADER,0)||memcmp(r->header,"MVSEAL01",8)||get64(r->header+56)!=id||get64(r->header+64)!=bytes)goto fail;
    unsigned k=0;for(;k<keys->count;++k)if(!memcmp(keys->key[k].id,r->header+8,16))break;uint8_t tag[16];
    if(k==keys->count||!derive(keys->key[k].secret,r->header+24,r->keys)||!header_tag(r->keys,r->header,tag)||CRYPTO_memcmp(tag,r->header+72,16))goto fail;return r;
fail:seal_reader_free(r);return NULL;
}
int seal_reader_read(struct seal_reader *r,uint64_t offset,void *out,size_t n) {
    if(!r||!out||!n||n>65536||offset>r->bytes)return -1;if(n>r->bytes-offset)n=(size_t)(r->bytes-offset);size_t total=n;
    while(n){uint64_t index=offset/SEAL_BLOCK;size_t skip=(size_t)(offset%SEAL_BLOCK),size=(size_t)(r->bytes-index*SEAL_BLOCK);if(size>SEAL_BLOCK)size=SEAL_BLOCK;
        if(!r->valid||r->cached!=index){uint8_t block[SEAL_BLOCK+16];if(!read_at(r->fd,block,size+16,SEAL_HEADER+index*(SEAL_BLOCK+16))||!seal_crypt(false,r->keys,r->header,index,block,size,block+size)){OPENSSL_cleanse(block,sizeof(block));r->valid=false;return -1;}memcpy(r->cache,block,size);OPENSSL_cleanse(block,sizeof(block));r->cache_size=size;r->cached=index;r->valid=true;}
        size_t k=r->cache_size-skip;if(k>n)k=n;memcpy(out,r->cache+skip,k);out=(uint8_t *)out+k;offset+=k;n-=k;
    }return (int)total;
}
void seal_reader_free(struct seal_reader *r){if(r){OPENSSL_cleanse(r,sizeof(*r));free(r);}}
