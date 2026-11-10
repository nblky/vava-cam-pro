#include "directory_scan.h"
#include "file_store.h"
#include "archive_cipher.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

struct entry {
    char device[MYVAVA_ID_MAX+1], base[96];
    uint8_t descriptor[MYVAVA_FILE_DESC];
    struct myvava_file file;
    uint32_t offset;
    uint64_t committed_at;
    bool committed, deleting, imported; unsigned readers;
};
struct job {
    unsigned state,operation; int status; uint64_t cookie;
    char device[MYVAVA_ID_MAX+1];
    struct myvava_message request, result;
    uint8_t data[MYVAVA_FILE_PAYLOAD_MAX], ack[33];
};
struct file_store {
    pthread_t thread; pthread_mutex_t lock; pthread_cond_t wake;
    bool stop; int dir, lockfd; unsigned count, limit, cursor, tombstones;
    uint64_t reserved, quota, retention_next, retention_removed, cache_removed;
    unsigned retention_revision, retention_days;bool retention_enabled,retention_healthy,retention_pressure,retention_running;
    struct seal_keys *keys;
    struct entry *entries;
    struct job jobs[FILE_STORE_JOBS];
};
/* The wire reserves the high ID bit for explicit TF exports. Imported objects
 * have a durable sidecar and take precedence; no descriptor/AAD migration. */
static const char *storage_kind(const struct entry *e) {
    return e->imported?"legacy_import":e->file.detection_flags&4?"tf_preview":e->file.id>>63?"tf_export":"cloud";
}
static bool matches(const struct entry *e,const char *device,const char *storage,const char *serial) {
    return !strcmp(e->device,device)&&(!storage||!*storage||!strcmp(storage,storage_kind(e)))&&(!serial||!*serial||!strcmp(serial,e->file.serial));
}
static int open_private(int dir,const char *name,int flags) {
    int fd=openat(dir,name,flags|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK,0600); struct stat st;
    if (fd<0) return -1;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077) || st.st_nlink!=1) {
        close(fd); errno=EINVAL; return -1;
    }
    return fd;
}
static bool write_full(int fd,const uint8_t *p,size_t n) {
    while (n) { ssize_t k=write(fd,p,n); if (k<0 && errno==EINTR) continue;
        if (k<=0) return false; p+=k; n-=(size_t)k; }
    return true;
}
static bool read_full(int fd,uint8_t *p,size_t n,off_t off) {
    while (n) { ssize_t k=pread(fd,p,n,off); if (k<0 && errno==EINTR) continue;
        if (k<=0) return false; p+=k; n-=(size_t)k; off+=k; }
    return true;
}
static bool read_descriptor(int dir,const char *name,uint8_t *p) {
    int fd=open_private(dir,name,O_RDONLY); struct stat st;
    if (fd<0) return false;
    bool ok=!fstat(fd,&st) && st.st_size==MYVAVA_FILE_DESC && read_full(fd,p,MYVAVA_FILE_DESC,0);
    close(fd); return ok;
}
static bool atomic_descriptor(int dir,const char *base,const char *ext,const uint8_t *p) {
    char temp[128],dest[128]; snprintf(temp,sizeof(temp),"%s.%s-tmp",base,ext); snprintf(dest,sizeof(dest),"%s.%s",base,ext);
    int fd=open_private(dir,temp,O_WRONLY|O_CREAT); if (fd<0) return false;
    bool ok=!ftruncate(fd,0) && write_full(fd,p,MYVAVA_FILE_DESC) && !fsync(fd);
    if (close(fd)) ok=false;
    if (ok) {
        struct stat st;
        if (!fstatat(dir,dest,&st,AT_SYMLINK_NOFOLLOW)) {
            uint8_t existing[MYVAVA_FILE_DESC];
            ok=read_descriptor(dir,dest,existing) && !memcmp(existing,p,sizeof(existing));
            if (ok) ok=!unlinkat(dir,temp,0);
        } else if (errno==ENOENT) ok=!renameat(dir,temp,dir,dest);
        else ok=false;
        if (ok) ok=!fsync(dir);
    }
    if (!ok) (void)unlinkat(dir,temp,0);
    return ok;
}
static int file_open(struct file_store *s,const struct entry *e,const char *ext,int flags) {
    char path[128]; snprintf(path,sizeof(path),"%s.%s",e->base,ext); return open_private(s->dir,path,flags);
}
static uint64_t physical_size(struct file_store *s,uint64_t n){return s->keys?archive_cipher_size(n):n;}
static const char *blob_ext(struct file_store *s,bool complete){return s->keys?(complete?"sealed":"sealed-part"):(complete?"ts":"part");}
bool file_store_encrypted(const struct file_store *s){return s&&s->keys;}
int file_store_read(struct file_store *s,int fd,const struct myvava_file *f,uint32_t off,void *out,size_t n){
    if(!s||!f||fd<0||!out||n>65536||off>f->size){errno=EINVAL;return -1;}
    if(n>f->size-off)n=f->size-off;
    int result=s->keys?archive_cipher_read(fd,s->keys,f,off,out,n):(read_full(fd,out,n,off)?(int)n:-1);
    /* Authentication failures are terminal; a previous interrupted syscall must
     * never turn a corrupt object into an endless retry in an HTTP reader. */
    if(result<0)errno=EIO;
    return result;
}
static bool current_offset(struct file_store *s,int fd,uint32_t total,uint32_t *off){if(s->keys)return archive_cipher_offset(fd,total,off,true);struct stat st;if(fstat(fd,&st)||st.st_size<0||(uint64_t)st.st_size>total)return false;*off=(uint32_t)st.st_size;return true;}
static bool digest_matches(struct file_store *s,int fd,const struct myvava_file *f) {
    struct stat st; if (fstat(fd,&st) || (uint64_t)st.st_size!=physical_size(s,f->size)) return false;
    EVP_MD_CTX *md=EVP_MD_CTX_new(); if (!md) return false;
    bool ok=EVP_DigestInit_ex(md,EVP_sha256(),NULL)==1;
    uint8_t buffer[65536],digest[32]; unsigned n=0;
    for (uint32_t off=0;ok && off<f->size;) {
        size_t len=f->size-off; if (len>sizeof(buffer)) len=sizeof(buffer);
        ok=file_store_read(s,fd,f,off,buffer,len)==(int)len && EVP_DigestUpdate(md,buffer,len)==1; off+=(uint32_t)len;
    }
    ok=ok && EVP_DigestFinal_ex(md,digest,&n)==1 && n==32 && !memcmp(digest,f->sha256,32);
    EVP_MD_CTX_free(md); return ok;
}
static int deleted(struct file_store *s,const char *base) {
    char name[128];snprintf(name,sizeof(name),"%s.deleted",base);uint8_t raw[MYVAVA_FILE_DESC];struct stat st;
    if(fstatat(s->dir,name,&st,AT_SYMLINK_NOFOLLOW))return errno==ENOENT?0:-1;
    struct myvava_file f;if(!read_descriptor(s->dir,name,raw)||!myvava_file_decode(raw,sizeof(raw),&f))return -1;
    const char *dot=strrchr(base,'.');char device[64],expected[96];
    if(!dot||(size_t)(dot-base)>=sizeof(device))return -1;memcpy(device,base,(size_t)(dot-base));device[dot-base]=0;
    snprintf(expected,sizeof(expected),"%s.%016" PRIx64,device,f.id);
    return myvava_id_valid(device)&&!strcmp(base,expected)?1:-1;
}
static bool delete_bytes(struct file_store *s,const char *base) {
    const char *ext[]={"part","ts","sealed","sealed-part","meta","offer","meta-tmp","offer-tmp","import","import-tmp"};
    for(unsigned i=0;i<sizeof(ext)/sizeof(*ext);++i){char name[128];snprintf(name,sizeof(name),"%s.%s",base,ext[i]);int fd=open_private(s->dir,name,O_RDONLY);
        if(fd<0){if(errno==ENOENT)continue;return false;}close(fd);if(unlinkat(s->dir,name,0))return false;}
    return !fsync(s->dir);
}
static int delete_entry(struct file_store *s,struct job *j) {
    unsigned index=0;while(index<s->count&&(s->entries[index].file.id!=j->request.id||strcmp(s->entries[index].device,j->device)))++index;
    if(index==s->count){char base[96];snprintf(base,sizeof(base),"%s.%016" PRIx64,j->device,j->request.id);return deleted(s,base)==1?200:404;}
    struct entry *e=&s->entries[index];pthread_mutex_lock(&s->lock);bool busy=e->readers!=0;if(!busy)e->deleting=true;pthread_mutex_unlock(&s->lock);if(busy)return 409;
    if(s->tombstones>=65536||!atomic_descriptor(s->dir,e->base,"deleted",e->descriptor)){
        /* rename may have published the marker before directory fsync failed.
         * Restore access only when absence is certain. */
        char marker[128];struct stat st;snprintf(marker,sizeof(marker),"%s.deleted",e->base);
        if(fstatat(s->dir,marker,&st,AT_SYMLINK_NOFOLLOW)&&errno==ENOENT){pthread_mutex_lock(&s->lock);e->deleting=false;pthread_mutex_unlock(&s->lock);}
        return 503;
    }
    /* Once the tombstone is durable, all readers and uploaders must fail closed,
     * even if removing the old bytes needs recovery after a disk error. */
    pthread_mutex_lock(&s->lock);e->committed=false;pthread_mutex_unlock(&s->lock);
    if(!delete_bytes(s,e->base))return 503;
    pthread_mutex_lock(&s->lock);s->reserved-=physical_size(s,e->file.size);memmove(e,e+1,(s->count-index-1)*sizeof(*e));--s->count;++s->tombstones;pthread_mutex_unlock(&s->lock);return 200;
}
static bool load(struct file_store *s,const char *name) {
    size_t n=strlen(name); if (n<24 || strcmp(name+n-6,".offer")) return true;
    struct entry e={0}; if (n-6>=sizeof(e.base) || s->count==s->limit) return false;
    memcpy(e.base,name,n-6); char *dot=strrchr(e.base,'.');
    if (!dot || strlen(dot+1)!=16) return false;
    size_t device_len=(size_t)(dot-e.base); if (device_len>MYVAVA_ID_MAX) return false;
    memcpy(e.device,e.base,device_len);
    uint8_t id[8];
    if (!myvava_id_valid(e.device) || !myvava_unhex(dot+1,id,8) ||
        !read_descriptor(s->dir,name,e.descriptor) || !myvava_file_decode(e.descriptor,MYVAVA_FILE_DESC,&e.file)) return false;
    char expected[96]; snprintf(expected,sizeof(expected),"%s.%016" PRIx64,e.device,e.file.id);
    if (strcmp(e.base,expected) || s->reserved>s->quota || physical_size(s,e.file.size)>s->quota-s->reserved) return false;
    char marker[128]; snprintf(marker,sizeof(marker),"%s.meta",e.base); struct stat st;
    if (!fstatat(s->dir,marker,&st,AT_SYMLINK_NOFOLLOW)) {
        e.committed_at=st.st_mtime>0?(uint64_t)st.st_mtime:0;
        uint8_t b[MYVAVA_FILE_DESC];
        if (!read_descriptor(s->dir,marker,b) || memcmp(b,e.descriptor,sizeof(b))) return false;
        int fd=file_open(s,&e,blob_ext(s,true),O_RDONLY);
        bool ok=fd>=0 && !fstat(fd,&st) && (uint64_t)st.st_size==physical_size(s,e.file.size) && (!s->keys||digest_matches(s,fd,&e.file));
        if (fd>=0) close(fd); if (!ok) return false;
        e.committed=true; e.offset=e.file.size;
    } else if (errno!=ENOENT) return false;
    else {
        int fd=file_open(s,&e,blob_ext(s,false),O_RDWR);
        if (fd>=0) {
            bool ok=current_offset(s,fd,e.file.size,&e.offset);
            if(ok&&s->keys){uint8_t plain[65536];for(uint32_t off=0;ok&&off<e.offset;){size_t size=e.offset-off;if(size>sizeof(plain))size=sizeof(plain);ok=file_store_read(s,fd,&e.file,off,plain,size)==(int)size;off+=(uint32_t)size;}}
            close(fd); if (!ok) return false;
        } else if (errno!=ENOENT) return false;
    }
    char import_name[128];uint8_t imported[128];snprintf(import_name,sizeof(import_name),"%s.import",e.base);e.imported=read_descriptor(s->dir,import_name,imported)&&!memcmp(imported,e.descriptor,128);
    s->entries[s->count++]=e; s->reserved+=physical_size(s,e.file.size); return true;
}
static unsigned process(struct file_store *s,struct job *j,uint32_t *offset) {
    struct myvava_message *m=&j->request; struct myvava_file f; struct entry *e=NULL;
    char base[96];snprintf(base,sizeof(base),"%s.%016" PRIx64,j->device,m->id);
    int tomb=deleted(s,base);if(tomb)return tomb>0?MVF_DELETED:MVF_IO;
    for (unsigned i=0;i<s->count;++i) if (s->entries[i].file.id==m->id && !strcmp(s->entries[i].device,j->device)) { e=&s->entries[i]; break; }
    if (m->type==MYVAVA_FILE_OFFER) {
        if (!myvava_file_decode(m->data,m->length,&f) || m->id!=f.id || m->total!=f.size || m->offset) return MVF_INVALID;
        if((f.detection_flags&4)&&!(f.id>>63))return MVF_INVALID;
        if (e && memcmp(e->descriptor,m->data,MYVAVA_FILE_DESC)) return MVF_CONFLICT;
        if (!e) {
            if (s->count==s->limit || s->reserved>s->quota || physical_size(s,f.size)>s->quota-s->reserved) return MVF_QUOTA;
            struct entry fresh={.file=f}; strcpy(fresh.device,j->device);
            snprintf(fresh.base,sizeof(fresh.base),"%s.%016" PRIx64,j->device,f.id);
            memcpy(fresh.descriptor,m->data,MYVAVA_FILE_DESC);
            bool durable=atomic_descriptor(s->dir,fresh.base,"offer",fresh.descriptor);
            if (!durable) {
                char name[128]; uint8_t existing[MYVAVA_FILE_DESC];
                snprintf(name,sizeof(name),"%s.offer",fresh.base);
                if (!read_descriptor(s->dir,name,existing)) return MVF_IO;
                if (memcmp(existing,fresh.descriptor,sizeof(existing))) return MVF_CONFLICT;
            }
            pthread_mutex_lock(&s->lock);
            e=&s->entries[s->count++]; *e=fresh; s->reserved+=physical_size(s,f.size);
            pthread_mutex_unlock(&s->lock);
            if (!durable) return MVF_IO;
        }
    } else if (!e || m->length<32 || memcmp(m->data,e->file.sha256,32) || m->total!=e->file.size) return MVF_CONFLICT;
    /* An earlier directory sync can have failed after publication. Reassert
     * the immutable offer's durability before acknowledging any offset. */
    if (fsync(s->dir)) return MVF_IO;
    if (e->committed) { *offset=e->file.size; return MVF_COMMITTED; }
    int fd=file_open(s,e,blob_ext(s,true),O_RDONLY);
    if (fd>=0) {
        bool ok=digest_matches(s,fd,&e->file); close(fd);
        if (!ok) return MVF_DIGEST;
        if (fsync(s->dir) || !atomic_descriptor(s->dir,e->base,"meta",e->descriptor)) return MVF_IO;
        pthread_mutex_lock(&s->lock); e->committed=true; e->committed_at=(uint64_t)time(NULL); e->offset=e->file.size; pthread_mutex_unlock(&s->lock);
        *offset=e->file.size; return MVF_COMMITTED;
    } else if (errno!=ENOENT) return MVF_IO;
    fd=file_open(s,e,blob_ext(s,false),O_RDWR|O_CREAT); if (fd<0) return MVF_IO;
    unsigned result=MVF_IO;
    if (!current_offset(s,fd,e->file.size,offset)) goto done;
    if (m->type==MYVAVA_FILE_DATA) {
        size_t bytes=m->length-32;
        if (!bytes || m->offset>e->file.size || bytes>e->file.size-m->offset || m->offset>*offset) { result=MVF_INVALID; goto done; }
        if (m->offset<*offset) {
            uint8_t old[MYVAVA_FILE_BATCH_DATA_MAX];
            if (bytes>*offset-m->offset || file_store_read(s,fd,&e->file,m->offset,old,bytes)!=(int)bytes || memcmp(old,m->data+32,bytes)) { result=MVF_CONFLICT; goto done; }
        } else {
            bool written=true;
            if(s->keys){
                /* Keep the established 736-byte encrypted capsule layout.
                 * Only the durable acknowledgment groups multiple capsules. */
                for(size_t at=0;written&&at<bytes;){size_t n=bytes-at;if(n>MYVAVA_FILE_DATA_MAX)n=MYVAVA_FILE_DATA_MAX;
                    written=archive_cipher_write(fd,s->keys,&e->file,*offset+(uint32_t)at,m->data+32+at,n);at+=n;}
            }else written=lseek(fd,0,SEEK_END)>=0&&write_full(fd,m->data+32,bytes);
            if (!written) {
                (void)ftruncate(fd,(off_t)physical_size(s,*offset)); (void)fsync(fd); goto done;
            }
            *offset+=(uint32_t)bytes;
        }
    } else if (m->type==MYVAVA_FILE_COMMIT && (m->length!=32 || m->offset!=e->file.size)) { result=MVF_INVALID; goto done; }
    if (fsync(fd) || fsync(s->dir)) goto done;
    if (m->type==MYVAVA_FILE_COMMIT) {
        if (*offset!=e->file.size) { result=MVF_INVALID; goto done; }
        if (!digest_matches(s,fd,&e->file)) { result=MVF_DIGEST; goto done; }
        char src[128],dest[128]; snprintf(src,sizeof(src),"%s.%s",e->base,blob_ext(s,false)); snprintf(dest,sizeof(dest),"%s.%s",e->base,blob_ext(s,true));
        if (renameat(s->dir,src,s->dir,dest) || fsync(s->dir) || !atomic_descriptor(s->dir,e->base,"meta",e->descriptor)) goto done;
        pthread_mutex_lock(&s->lock); e->committed=true; e->committed_at=(uint64_t)time(NULL); pthread_mutex_unlock(&s->lock);
        result=MVF_COMMITTED;
    } else result=MVF_PARTIAL;
done:
    if (close(fd)) result=MVF_IO;
    pthread_mutex_lock(&s->lock); e->offset=*offset; pthread_mutex_unlock(&s->lock);
    return result;
}
static void policy_bytes(uint8_t b[128],unsigned rev,bool enabled,unsigned days){
    memset(b,0,128);memcpy(b,"MVARCHKEEP1",11);for(unsigned i=0;i<4;++i){b[16+i]=(uint8_t)(rev>>(24-i*8));b[24+i]=(uint8_t)(days>>(24-i*8));}b[20]=enabled;unsigned n;EVP_Digest(b,96,b+96,&n,EVP_sha256(),NULL);
}
static bool retention_load(struct file_store *s){
    s->retention_revision=1;s->retention_days=30;s->retention_healthy=true;
    int fd=open_private(s->dir,".archive-policy",O_RDONLY);if(fd<0){s->retention_healthy=errno==ENOENT;return true;}close(fd);
    uint8_t b[128],wanted[128];bool ok=read_descriptor(s->dir,".archive-policy",b);unsigned rev=0,days=0;
    if(ok){for(unsigned i=0;i<4;++i){rev=rev*256+b[16+i];days=days*256+b[24+i];}policy_bytes(wanted,rev,b[20]!=0,days);ok=rev&&days<=3650&&b[20]<=1&&!memcmp(b,wanted,128);}
    s->retention_healthy=ok;if(ok){s->retention_revision=rev;s->retention_enabled=b[20];s->retention_days=days;}return true;
}
static int retention_save(struct file_store *s,struct job *j){
    unsigned rev=(unsigned)j->request.id,days=j->request.total;bool enabled=j->request.offset!=0;
    if(!s->retention_healthy)return 503;if(rev!=s->retention_revision||rev==UINT32_MAX)return 409;
    uint8_t b[128];policy_bytes(b,rev+1,enabled,days);int fd=open_private(s->dir,".archive-policy-tmp",O_WRONLY|O_CREAT);
    bool ok=fd>=0&&!ftruncate(fd,0)&&write_full(fd,b,128)&&!fsync(fd);if(fd>=0&&close(fd))ok=false;
    if(ok)ok=!renameat(s->dir,".archive-policy-tmp",s->dir,".archive-policy")&&!fsync(s->dir);
    pthread_mutex_lock(&s->lock);if(ok){s->retention_revision=rev+1;s->retention_days=days;s->retention_enabled=enabled;}else{s->retention_healthy=false;s->retention_enabled=false;}pthread_mutex_unlock(&s->lock);return ok?200:503;
}
static void retention_sweep(struct file_store *s){
    /* Runs only on the archive I/O worker; it shares the deletion transaction
     * and never removes unfinished uploads or an active reader. */
    uint64_t now=(uint64_t)time(NULL);if(now<1577836800)return;
    for(unsigned removed=0;removed<4;++removed){struct job j={0};bool found=false,cache_selected=false;
        pthread_mutex_lock(&s->lock);
        if(!s->retention_running||!s->retention_healthy){pthread_mutex_unlock(&s->lock);return;}
        s->retention_pressure=s->reserved>=s->quota/10*9||s->count>=s->limit||(s->retention_pressure&&s->reserved>s->quota/10*8);
        unsigned selected=s->count;
        for(unsigned i=0;i<s->count;++i){struct entry *e=&s->entries[i];bool aged=s->retention_days&&e->file.clock_valid&&now>=e->file.unix_seconds&&now-e->file.unix_seconds>=(uint64_t)s->retention_days*86400;
            bool cache=!e->imported&&(e->file.id>>63);
            bool expired=cache&&e->committed_at>=1577836800&&now>=e->committed_at&&now-e->committed_at>=3600;
            bool cloud=!e->imported&&!cache&&s->retention_enabled&&(s->retention_pressure||aged);
            if(!e->committed||e->deleting||e->readers||(!expired&&!cloud))continue;
            if(selected==s->count||e->file.unix_seconds<s->entries[selected].file.unix_seconds)selected=i;
        }
        if(selected<s->count){strcpy(j.device,s->entries[selected].device);j.request.id=s->entries[selected].file.id;cache_selected=(s->entries[selected].file.id>>63)!=0;found=true;}
        pthread_mutex_unlock(&s->lock);if(!found)return;
        int result=delete_entry(s,&j);pthread_mutex_lock(&s->lock);if(result==200){if(cache_selected)++s->cache_removed;else ++s->retention_removed;}else if(result!=409){s->retention_healthy=false;s->retention_enabled=false;}pthread_mutex_unlock(&s->lock);if(result!=200)return;
    }
}
static void *worker(void *arg) {
    struct file_store *s=arg;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        uint64_t now=myvava_millis();if(!s->stop&&now>=s->retention_next){s->retention_next=now+1000;pthread_mutex_unlock(&s->lock);retention_sweep(s);pthread_mutex_lock(&s->lock);}
        struct job *j=NULL;
        for (unsigned i=0;i<FILE_STORE_JOBS;++i) {
            unsigned k=(s->cursor+i)%FILE_STORE_JOBS;
            if (s->jobs[k].state==1) { j=&s->jobs[k]; s->cursor=(k+1)%FILE_STORE_JOBS; break; }
        }
        if (!j) { if (s->stop) break;struct timespec next;clock_gettime(CLOCK_REALTIME,&next);++next.tv_sec;pthread_cond_timedwait(&s->wake,&s->lock,&next);continue; }
        j->state=2; pthread_mutex_unlock(&s->lock);
        if(j->operation) {
            j->status=j->operation==1?delete_entry(s,j):retention_save(s,j);pthread_mutex_lock(&s->lock);j->state=3;pthread_cond_broadcast(&s->wake);continue;
        }
        uint32_t offset=0; unsigned code=process(s,j,&offset);
        j->result=j->request; j->result.type=MYVAVA_FILE_ACK; j->result.offset=offset; j->result.length=33;
        const uint8_t *sha=j->request.type==MYVAVA_FILE_OFFER?j->data+16:j->data;
        memcpy(j->ack,sha,32); j->ack[32]=(uint8_t)code;
        pthread_mutex_lock(&s->lock); j->state=3;
    }
    pthread_mutex_unlock(&s->lock); return NULL;
}
struct file_store *file_store_start(const char *dir,uint64_t quota,unsigned limit){return file_store_start_encrypted(dir,quota,limit,NULL);}
struct file_store *file_store_start_encrypted(const char *dir,uint64_t quota,unsigned limit,const char *keyring) {
    if (!dir || !quota || limit<1 || limit>4096) return NULL;
    struct stat st;
    if (lstat(dir,&st) || !S_ISDIR(st.st_mode) || st.st_uid!=geteuid() || (st.st_mode&0077)) return NULL;
    struct file_store *s=calloc(1,sizeof(*s)); if (!s) return NULL;
    s->lockfd=-1; s->dir=open(dir,O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW); s->quota=quota; s->limit=limit;
    s->entries=calloc(limit,sizeof(*s->entries));
    if (s->dir<0 || !s->entries) goto fail;
    if(keyring&&!(s->keys=seal_keys_load(keyring)))goto fail;
    s->lockfd=open_private(s->dir,".store-lock",O_RDWR|O_CREAT);
    struct flock exclusive={.l_type=F_WRLCK,.l_whence=SEEK_SET};
    if (s->lockfd<0 || fcntl(s->lockfd,F_SETLK,&exclusive)) goto fail;
    DIR *scan=directory_scan_open(s->dir); if (!scan) goto fail;
    struct dirent *d; bool ok=true;
    uint8_t mode[MYVAVA_FILE_DESC]={0},wanted[MYVAVA_FILE_DESC]={0};memcpy(wanted,"MVARCHIVESEALED1",15);
    int modefd=open_private(s->dir,".storage.mode",O_RDONLY);
    if(modefd>=0){close(modefd);ok=s->keys&&read_descriptor(s->dir,".storage.mode",mode)&&!memcmp(mode,wanted,sizeof(mode));}
    else if(errno!=ENOENT)ok=false;
    else if(s->keys){/* Never silently reinterpret or migrate existing plaintext. */
        while((d=directory_scan_next(scan,&ok)))if(strcmp(d->d_name,".")&&strcmp(d->d_name,"..")&&strcmp(d->d_name,".store-lock")){ok=false;break;}
        if(ok)ok=atomic_descriptor(s->dir,".storage","mode",wanted);rewinddir(scan);
    }
    if(!ok){closedir(scan);goto fail;}
    while ((d=directory_scan_next(scan,&ok))) {
        size_t n=strlen(d->d_name);if(n<9||strcmp(d->d_name+n-8,".deleted"))continue;
        char base[96];if(n-8>=sizeof(base)||s->tombstones++>=65536){ok=false;break;}memcpy(base,d->d_name,n-8);base[n-8]=0;
        if(deleted(s,base)!=1||!delete_bytes(s,base)){ok=false;break;}
    }
    rewinddir(scan);
    while (ok && (d=directory_scan_next(scan,&ok))) if (!load(s,d->d_name)) { ok=false; break; }
    rewinddir(scan);
    while (ok && (d=directory_scan_next(scan,&ok))) {
        const char *ext=strrchr(d->d_name,'.');
        if (!ext || (strcmp(ext,".part") && strcmp(ext,".ts") && strcmp(ext,".sealed") && strcmp(ext,".sealed-part") && strcmp(ext,".meta"))) continue;
        bool indexed=false; size_t n=(size_t)(ext-d->d_name);
        for (unsigned i=0;i<s->count;++i) if (strlen(s->entries[i].base)==n && !memcmp(s->entries[i].base,d->d_name,n)) indexed=true;
        if (!indexed) ok=false; /* preserve unindexed bytes; never hide them from quota */
    }
    closedir(scan); if (!ok) goto fail;
    if(!retention_load(s))goto fail;
    if (pthread_mutex_init(&s->lock,NULL)) goto fail;
    if (pthread_cond_init(&s->wake,NULL)) { pthread_mutex_destroy(&s->lock); goto fail; }
    if (pthread_create(&s->thread,NULL,worker,s)) { pthread_cond_destroy(&s->wake); pthread_mutex_destroy(&s->lock); goto fail; }
    return s;
fail:
    if (s->lockfd>=0) close(s->lockfd);
    if (s->dir>=0) close(s->dir); free(s->entries);seal_keys_free(s->keys); free(s); return NULL;
}
void file_store_stop(struct file_store *s) {
    if (!s) return;
    pthread_mutex_lock(&s->lock); s->stop=true; pthread_cond_broadcast(&s->wake); pthread_mutex_unlock(&s->lock);
    pthread_join(s->thread,NULL); pthread_mutex_destroy(&s->lock); pthread_cond_destroy(&s->wake);
    close(s->lockfd); close(s->dir); free(s->entries);seal_keys_free(s->keys); free(s);
}
bool file_store_submit(struct file_store *s,uint64_t cookie,const char *device,const struct myvava_message *m) {
    if (!s || !myvava_id_valid(device) || m->length>(m->type==MYVAVA_FILE_DATA?MYVAVA_FILE_PAYLOAD_MAX:MYVAVA_CHUNK) ||
        (m->type==MYVAVA_FILE_OFFER?m->length!=MYVAVA_FILE_DESC:m->length<32)) return false;
    bool ok=false; pthread_mutex_lock(&s->lock);
    for (unsigned i=0;i<FILE_STORE_JOBS;++i) if (!s->jobs[i].state) {
        struct job *j=&s->jobs[i]; j->operation=0; j->cookie=cookie; j->request=*m; strcpy(j->device,device);
        memcpy(j->data,m->data,m->length); j->request.data=j->data; j->state=1; ok=true; pthread_cond_broadcast(&s->wake); break;
    }
    pthread_mutex_unlock(&s->lock); return ok;
}
bool file_store_poll(struct file_store *s,uint64_t *cookie,struct myvava_message *m,uint8_t data[33]) {
    if (!s) return false;
    bool found=false; pthread_mutex_lock(&s->lock);
    for (unsigned i=0;i<FILE_STORE_JOBS;++i) if (s->jobs[i].state==3 && !s->jobs[i].operation) {
        struct job *j=&s->jobs[i]; *cookie=j->cookie; *m=j->result; memcpy(data,j->ack,33); m->data=data;
        j->state=0; found=true; break;
    }
    pthread_mutex_unlock(&s->lock); return found;
}
bool file_store_list(struct file_store *s,const char *device,unsigned page,char *out,size_t size) {
    return file_store_list_filtered(s,device,page,NULL,NULL,out,size);
}
/* List snapshots sort pointers, never entries: active leases retain identity.
 * Trusted timestamps come first; unknown camera clocks use ID order only. */
static int newest_first(const void *a,const void *b) {
    const struct myvava_file *x=&(*(const struct entry *const *)a)->file;
    const struct myvava_file *y=&(*(const struct entry *const *)b)->file;
    if(x->clock_valid!=y->clock_valid)return x->clock_valid?-1:1;
    if(x->clock_valid&&x->unix_seconds!=y->unix_seconds)return x->unix_seconds>y->unix_seconds?-1:1;
    return x->id==y->id?0:x->id>y->id?-1:1;
}
bool file_store_list_filtered(struct file_store *s,const char *device,unsigned page,const char *storage,const char *serial,char *out,size_t size) {
    if (!s || !out || !size || !myvava_id_valid(device) || page>4096 || (storage&&*storage&&strcmp(storage,"cloud")&&strcmp(storage,"tf_export")&&strcmp(storage,"tf_preview")&&strcmp(storage,"legacy_import"))) return false;
    const struct entry **ordered=malloc(s->limit*sizeof(*ordered));if(!ordered)return false;
    pthread_mutex_lock(&s->lock); unsigned total=0,returned=0;
    for (unsigned i=0;i<s->count;++i)if(matches(&s->entries[i],device,storage,serial))ordered[total++]=&s->entries[i];
    qsort(ordered,total,sizeof(*ordered),newest_first);
    size_t used=(size_t)snprintf(out,size,"{\"ok\":true,\"total\":%u,\"page\":%u,\"quota\":%" PRIu64 ",\"reserved\":%" PRIu64 ",\"encrypted\":%s,\"files\":[",total,page,s->quota,s->reserved,s->keys?"true":"false");
    for (unsigned i=page*16;i<total && returned<16 && used<size;++i) {
        const struct entry *e=ordered[i];
        char sha[65]; myvava_hex(e->file.sha256,32,sha);
        used+=(size_t)snprintf(out+used,size-used,"%s{\"id\":\"%016" PRIx64 "\",\"serial\":\"%s\",\"sha256\":\"%s\",\"bytes\":%u,\"offset\":%u,\"unix_seconds\":%" PRIu64 ",\"clock_valid\":%s,\"state\":\"%s\",\"format\":\"mpegts-h264-aac\",\"source\":\"%s\",\"storage\":\"%s\",\"slot\":%u,\"duration_ms\":%llu,\"file_type\":%u,\"detection_reported\":%s,\"detection_confirmed\":%s,\"detection_type_raw\":%u}",
            returned?",":"",e->file.id,e->file.serial,sha,e->file.size,e->offset,e->file.unix_seconds,e->file.clock_valid?"true":"false",e->deleting?"deleting":e->committed?"committed":"receiving",e->imported?"legacy_import":"station",storage_kind(e),e->file.slot,(unsigned long long)(e->file.last_ms-e->file.first_ms),e->file.file_type,(e->file.detection_flags&1)?"true":"false",(e->file.detection_flags&2)?"true":"false",e->file.detection_type); ++returned;
    }
    if (used<size) used+=(size_t)snprintf(out+used,size-used,"]}\n");
    pthread_mutex_unlock(&s->lock); free(ordered); return used<size;
}

int file_store_acquire(struct file_store *s,const char *device,uint64_t id,struct myvava_file *file) {
    if(!s||!myvava_id_valid(device))return -1;
    struct entry copy={0};bool found=false;pthread_mutex_lock(&s->lock);
    for(unsigned i=0;i<s->count;++i){struct entry *e=&s->entries[i];if(e->file.id==id&&!strcmp(e->device,device)&&e->committed&&!e->deleting){++e->readers;copy=*e;found=true;break;}}
    pthread_mutex_unlock(&s->lock);if(!found)return -1;
    int fd=file_open(s,&copy,blob_ext(s,true),O_RDONLY);
    if(fd<0||deleted(s,copy.base)!=0||!digest_matches(s,fd,&copy.file)){file_store_release(s,device,id,fd);return -1;}
    *file=copy.file;return fd;
}
void file_store_release(struct file_store *s,const char *device,uint64_t id,int fd) {
    if(fd>=0)close(fd);if(!s)return;pthread_mutex_lock(&s->lock);
    for(unsigned i=0;i<s->count;++i){struct entry *e=&s->entries[i];if(e->file.id==id&&!strcmp(e->device,device)&&e->readers){--e->readers;break;}}
    pthread_mutex_unlock(&s->lock);
}
int file_store_delete(struct file_store *s,const char *device,uint64_t id) {
    if(!s||!myvava_id_valid(device)||!id)return 404;
    pthread_mutex_lock(&s->lock);struct job *job=NULL;
    for(unsigned i=0;i<FILE_STORE_JOBS;++i)if(!s->jobs[i].state){job=&s->jobs[i];memset(job,0,sizeof(*job));job->operation=1;job->request.id=id;strcpy(job->device,device);job->state=1;break;}
    if(!job){pthread_mutex_unlock(&s->lock);return 503;}pthread_cond_broadcast(&s->wake);
    while(job->state!=3)pthread_cond_wait(&s->wake,&s->lock);
    int result=job->status;job->state=0;pthread_mutex_unlock(&s->lock);return result;
}

bool file_store_info(struct file_store *s,const char *device,uint64_t id,struct myvava_file *out) {
    return file_store_info_storage(s,device,id,out,NULL);
}
bool file_store_info_storage(struct file_store *s,const char *device,uint64_t id,struct myvava_file *out,const char **storage) {
    if(!s||!myvava_id_valid(device))return false;bool found=false;pthread_mutex_lock(&s->lock);
    for(unsigned i=0;i<s->count;++i){struct entry *e=&s->entries[i];if(e->file.id==id&&!strcmp(e->device,device)&&e->committed&&!e->deleting){*out=e->file;if(storage)*storage=storage_kind(e);found=true;break;}}
    pthread_mutex_unlock(&s->lock);return found;
}

bool file_store_mark_imported(struct file_store *s,const char *device,const uint8_t descriptor[MYVAVA_FILE_DESC]) {
    if(!s||!myvava_id_valid(device))return false;
    pthread_mutex_lock(&s->lock);bool ok=false;
    for(unsigned i=0;i<s->count;++i){struct entry *e=&s->entries[i];
        if(!strcmp(e->device,device)&&e->committed&&!e->deleting&&!memcmp(e->descriptor,descriptor,MYVAVA_FILE_DESC)){
            ok=atomic_descriptor(s->dir,e->base,"import",descriptor);if(ok)e->imported=true;break;
        }
    }
    pthread_mutex_unlock(&s->lock);return ok;
}

bool file_store_policy_json(struct file_store *s,char *out,size_t size){
    if(!s)return false;pthread_mutex_lock(&s->lock);int n=snprintf(out,size,"{\"ok\":true,\"revision\":%u,\"enabled\":%s,\"max_days\":%u,\"healthy\":%s,\"removed_since_boot\":%" PRIu64 ",\"quota_bytes\":%" PRIu64 ",\"reserved_bytes\":%" PRIu64 ",\"file_limit\":%u,\"tombstones\":%u,\"tf_cache_ttl_seconds\":3600,\"tf_cache_removed_since_boot\":%" PRIu64 "}",s->retention_revision,s->retention_enabled?"true":"false",s->retention_days,s->retention_healthy?"true":"false",s->retention_removed,s->quota,s->reserved,s->limit,s->tombstones,s->cache_removed);pthread_mutex_unlock(&s->lock);return n>=0&&(size_t)n<size;
}
int file_store_policy_set(struct file_store *s,unsigned revision,bool enabled,unsigned days){
    if(!s)return 503;if(!revision||days>3650)return 400;pthread_mutex_lock(&s->lock);struct job *j=NULL;
    for(unsigned i=0;i<FILE_STORE_JOBS;++i)if(!s->jobs[i].state){j=&s->jobs[i];memset(j,0,sizeof(*j));j->operation=2;j->request.id=revision;j->request.offset=enabled;j->request.total=days;j->state=1;break;}
    if(!j){pthread_mutex_unlock(&s->lock);return 503;}pthread_cond_broadcast(&s->wake);while(j->state!=3)pthread_cond_wait(&s->wake,&s->lock);int result=j->status;j->state=0;pthread_mutex_unlock(&s->lock);return result;
}

void file_store_enable_retention(struct file_store *s){if(!s)return;pthread_mutex_lock(&s->lock);s->retention_running=true;pthread_cond_signal(&s->wake);pthread_mutex_unlock(&s->lock);}
