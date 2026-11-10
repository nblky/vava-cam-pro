#include "archive_cipher.h"
#include <errno.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define SEGMENT_PLAIN MYVAVA_FILE_DATA_MAX
#define SEGMENT_CIPHER (SEAL_HEADER+SEGMENT_PLAIN+16)
uint64_t archive_cipher_size(uint64_t n){return n+(SEAL_HEADER+16)*((n+SEGMENT_PLAIN-1)/SEGMENT_PLAIN);}
static uint64_t identity(const struct myvava_file *f,uint32_t offset){uint8_t b[MYVAVA_FILE_DESC+4],digest[32];unsigned n=0;if(!myvava_file_encode(b,f))return 0;b[128]=(uint8_t)(offset>>24);b[129]=(uint8_t)(offset>>16);b[130]=(uint8_t)(offset>>8);b[131]=(uint8_t)offset;if(EVP_Digest(b,sizeof(b),digest,&n,EVP_sha256(),NULL)!=1||n!=32)return 0;uint64_t id=0;for(unsigned i=0;i<8;++i)id=id*256+digest[i];return id?id:UINT64_MAX;}
bool archive_cipher_offset(int fd,uint32_t total,uint32_t *offset,bool repair){struct stat st;if(fstat(fd,&st)||st.st_size<0||(uint64_t)st.st_size>archive_cipher_size(total))return false;uint64_t physical=(uint64_t)st.st_size;if(physical==archive_cipher_size(total)){*offset=total;return true;}uint64_t full=physical/SEGMENT_CIPHER;if(full*SEGMENT_PLAIN>total)return false;*offset=(uint32_t)(full*SEGMENT_PLAIN);if(physical%SEGMENT_CIPHER){if(!repair||ftruncate(fd,(off_t)(full*SEGMENT_CIPHER))||fsync(fd))return false;}return true;}
bool archive_cipher_write(int fd,const struct seal_keys *keys,const struct myvava_file *f,uint32_t offset,const uint8_t *p,size_t n){if(offset>=f->size||offset%SEGMENT_PLAIN||n!=(f->size-offset>SEGMENT_PLAIN?SEGMENT_PLAIN:f->size-offset))return false;uint8_t capsule[SEGMENT_CIPHER];uint64_t id=identity(f,offset);if(!id||!seal_capsule_encrypt(keys,id,p,n,capsule))return false;uint64_t at=(uint64_t)(offset/SEGMENT_PLAIN)*SEGMENT_CIPHER;size_t used=0,total=SEAL_HEADER+n+16;bool ok=true;while(used<total){ssize_t k=pwrite(fd,capsule+used,total-used,(off_t)(at+used));if(k<0&&errno==EINTR)continue;if(k<=0){ok=false;break;}used+=(size_t)k;}OPENSSL_cleanse(capsule,sizeof(capsule));if(!ok){(void)ftruncate(fd,(off_t)at);(void)fsync(fd);}return ok;}
int archive_cipher_read(int fd,const struct seal_keys *keys,const struct myvava_file *f,uint32_t offset,void *out,size_t n){if(!out||n>65536||offset>f->size)return -1;if(n>f->size-offset)n=f->size-offset;size_t result=n;uint8_t capsule[SEGMENT_CIPHER],plain[SEGMENT_PLAIN];
    while(n){uint32_t start=offset-offset%SEGMENT_PLAIN;size_t bytes=f->size-start;if(bytes>SEGMENT_PLAIN)bytes=SEGMENT_PLAIN;size_t size=SEAL_HEADER+bytes+16,used=0;uint64_t at=(uint64_t)(start/SEGMENT_PLAIN)*SEGMENT_CIPHER;
        while(used<size){ssize_t k=pread(fd,capsule+used,size-used,(off_t)(at+used));if(k<0&&errno==EINTR)continue;if(k<=0)goto fail;used+=(size_t)k;}
        uint64_t id=identity(f,start);if(!id||!seal_capsule_decrypt(keys,id,capsule,size,plain))goto fail;
        size_t skip=offset-start,k=bytes-skip;if(k>n)k=n;memcpy(out,plain+skip,k);out=(uint8_t *)out+k;offset+=(uint32_t)k;n-=k;
    }OPENSSL_cleanse(plain,sizeof(plain));return (int)result;
fail:OPENSSL_cleanse(plain,sizeof(plain));return -1;
}
