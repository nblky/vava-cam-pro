#include "sealed_file.h"
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"line %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void cleanup(const char *path){DIR *d=opendir(path);CHECK(d);struct dirent *e;while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;CHECK(!unlinkat(dirfd(d),e->d_name,0));}closedir(d);CHECK(!rmdir(path));}
int main(void){
    umask(0077);char root[]="/tmp/myvava-seal-XXXXXX";CHECK(mkdtemp(root));char keys_path[256],other_path[256],file[256],id[33],second[33];snprintf(keys_path,sizeof(keys_path),"%s/keys",root);snprintf(other_path,sizeof(other_path),"%s/other",root);snprintf(file,sizeof(file),"%s/video",root);
    CHECK(seal_keys_generate(keys_path,false,id));CHECK(!seal_keys_generate(keys_path,false,second));struct seal_keys *keys=seal_keys_load(keys_path);CHECK(keys&&!strcmp(seal_keys_active(keys),id));
    uint8_t plain[20021],out[20021];for(unsigned i=0;i<sizeof(plain);++i)plain[i]=(uint8_t)(i*31);
    int fd=open(file,O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);struct seal_writer *w=seal_writer_open(fd,keys,987);CHECK(w);
    for(size_t i=0;i<sizeof(plain);){size_t n=(i%997)+1;if(n>sizeof(plain)-i)n=sizeof(plain)-i;CHECK(seal_writer_write(w,plain+i,n));i+=n;}
    CHECK(!seal_reader_open(fd,keys,987,sizeof(plain))); /* unfinished files never authenticate */
    CHECK(seal_writer_finish(w));CHECK(!seal_writer_write(w,plain,1));seal_writer_free(w);CHECK(!fsync(fd));
    struct stat st;CHECK(!fstat(fd,&st)&&(uint64_t)st.st_size==seal_size(sizeof(plain)));
    struct seal_reader *r=seal_reader_open(fd,keys,987,sizeof(plain));CHECK(r);CHECK(seal_reader_read(r,0,out,sizeof(out))==(int)sizeof(out)&&!memcmp(out,plain,sizeof(out)));
    CHECK(seal_reader_read(r,4091,out,8931)==8931&&!memcmp(out,plain+4091,8931));CHECK(seal_reader_read(r,7,out,201)==201&&!memcmp(out,plain+7,201));CHECK(seal_reader_read(r,sizeof(plain)-17,out,100)==17&&!memcmp(out,plain+sizeof(plain)-17,17));seal_reader_free(r);
    CHECK(!seal_reader_open(fd,keys,988,sizeof(plain)));CHECK(!seal_reader_open(fd,keys,987,sizeof(plain)-1));
    CHECK(seal_keys_generate(other_path,false,second));struct seal_keys *other=seal_keys_load(other_path);CHECK(other&&!seal_reader_open(fd,other,987,sizeof(plain)));seal_keys_free(other);
    CHECK(seal_keys_generate(keys_path,true,second)&&strcmp(second,id));seal_keys_free(keys);keys=seal_keys_load(keys_path);CHECK(keys&&!strcmp(seal_keys_active(keys),second));r=seal_reader_open(fd,keys,987,sizeof(plain));CHECK(r);CHECK(seal_reader_read(r,0,out,sizeof(out))==(int)sizeof(out)&&!memcmp(out,plain,sizeof(out)));seal_reader_free(r);
    uint8_t byte;CHECK(pread(fd,&byte,1,SEAL_HEADER+500)==1);uint8_t corrupt=byte^1;CHECK(pwrite(fd,&corrupt,1,SEAL_HEADER+500)==1);r=seal_reader_open(fd,keys,987,sizeof(plain));CHECK(r);CHECK(seal_reader_read(r,0,out,700)<0);seal_reader_free(r);CHECK(pwrite(fd,&byte,1,SEAL_HEADER+500)==1);
    CHECK(pread(fd,&byte,1,72)==1);corrupt=byte^1;CHECK(pwrite(fd,&corrupt,1,72)==1);CHECK(!seal_reader_open(fd,keys,987,sizeof(plain)));CHECK(pwrite(fd,&byte,1,72)==1);
    CHECK(!ftruncate(fd,(off_t)seal_size(sizeof(plain))-1));CHECK(!seal_reader_open(fd,keys,987,sizeof(plain)));close(fd);
    char path[320];snprintf(path,sizeof(path),"%s/%s.key",keys_path,id);CHECK(!chmod(path,0644));CHECK(!seal_keys_load(keys_path));CHECK(!chmod(path,0600));
    seal_keys_free(keys);cleanup(keys_path);cleanup(other_path);cleanup(root);puts("sealed storage: block integrity, random access, identity, length, interrupted commit, key rotation and permissions passed");return 0;
}
