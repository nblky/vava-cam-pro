#include "resolver.h"
#include "myvava.h"
#undef NDEBUG
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static bool entered,release_dns;static atomic_bool left;
static int stalled(const char *h,const char *s,const struct addrinfo *hint,struct addrinfo **out){(void)h;(void)s;(void)hint;(void)out;pthread_mutex_lock(&lock);entered=true;pthread_cond_broadcast(&wake);while(!release_dns)pthread_cond_wait(&wake,&lock);pthread_mutex_unlock(&lock);atomic_store(&left,true);return EAI_AGAIN;}
static int failed(const char *h,const char *s,const struct addrinfo *hint,struct addrinfo **out){(void)h;(void)s;(void)hint;(void)out;return EAI_AGAIN;}
static struct myvava_resolution wait_result(struct myvava_resolver *r){struct myvava_resolution result;uint64_t end=myvava_millis()+5000;while(!myvava_resolver_result(r,&result)){assert(myvava_millis()<end);struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);}return result;}
int main(void){struct myvava_resolver *r=myvava_resolver_start("localhost",3731,NULL);assert(r);struct myvava_resolution result=wait_result(r);assert(!result.error&&result.count>0&&result.count<=MYVAVA_ADDRESSES);for(unsigned i=0;i<result.count;++i)assert(result.peers[i].address.ss_family==AF_INET||result.peers[i].address.ss_family==AF_INET6);myvava_resolver_release(r);
    r=myvava_resolver_start("localhost",3731,failed);assert(r);result=wait_result(r);assert(result.error==EAI_AGAIN&&!result.count);myvava_resolver_release(r);
    r=myvava_resolver_start("localhost",3731,stalled);assert(r);pthread_mutex_lock(&lock);while(!entered)pthread_cond_wait(&wake,&lock);pthread_mutex_unlock(&lock);assert(!myvava_resolver_result(r,&result));uint64_t start=myvava_millis();myvava_resolver_release(r);assert(myvava_millis()-start<100);pthread_mutex_lock(&lock);release_dns=true;pthread_cond_signal(&wake);pthread_mutex_unlock(&lock);while(!atomic_load(&left)){struct timespec delay={.tv_nsec=1000000};nanosleep(&delay,NULL);}puts("DNS ownership, shutdown and error handling passed");return 0;
}
