#include "resolver.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct myvava_resolver {pthread_mutex_t lock;unsigned references;bool ready;char host[254],service[8];myvava_lookup_fn lookup;struct myvava_resolution result;};
void myvava_resolver_release(struct myvava_resolver *r){if(!r)return;pthread_mutex_lock(&r->lock);bool last=!--r->references;pthread_mutex_unlock(&r->lock);if(last){pthread_mutex_destroy(&r->lock);free(r);}}
static void *resolve(void *arg){struct myvava_resolver *r=arg;struct myvava_resolution result={0};struct addrinfo hints={.ai_socktype=SOCK_DGRAM,.ai_family=AF_UNSPEC},*addresses=NULL;result.error=r->lookup(r->host,r->service,&hints,&addresses);
    if(!result.error){/* Keep both families and every distinct address. Connection
         * attempts rotate through the bounded result instead of pinning #1. */
        for(unsigned pass=0;pass<2;++pass)for(struct addrinfo *a=addresses;a&&result.count<MYVAVA_ADDRESSES;a=a->ai_next){if(a->ai_family!=(pass?AF_INET6:AF_INET)||a->ai_addrlen>sizeof(struct sockaddr_storage))continue;bool duplicate=false;
            for(unsigned i=0;i<result.count;++i)if(result.peers[i].size==a->ai_addrlen&&!memcmp(&result.peers[i].address,a->ai_addr,a->ai_addrlen))duplicate=true;
            if(!duplicate){struct myvava_address *p=&result.peers[result.count++];p->size=(socklen_t)a->ai_addrlen;memcpy(&p->address,a->ai_addr,a->ai_addrlen);}
        }
    }if(addresses)freeaddrinfo(addresses);pthread_mutex_lock(&r->lock);r->result=result;r->ready=true;pthread_mutex_unlock(&r->lock);myvava_resolver_release(r);return NULL;
}
struct myvava_resolver *myvava_resolver_start(const char *host,unsigned port,myvava_lookup_fn lookup){if(!host||!*host||strlen(host)>253||!port||port>65535)return NULL;struct myvava_resolver *r=calloc(1,sizeof(*r));if(!r)return NULL;strcpy(r->host,host);snprintf(r->service,sizeof(r->service),"%u",port);r->lookup=lookup?lookup:getaddrinfo;r->references=2;if(pthread_mutex_init(&r->lock,NULL)){free(r);return NULL;}pthread_attr_t attr;if(pthread_attr_init(&attr)){pthread_mutex_destroy(&r->lock);free(r);return NULL;}
    pthread_t thread;int error=pthread_attr_setdetachstate(&attr,PTHREAD_CREATE_DETACHED);if(!error)error=pthread_create(&thread,&attr,resolve,r);pthread_attr_destroy(&attr);if(error){pthread_mutex_destroy(&r->lock);free(r);return NULL;}return r;
}
bool myvava_resolver_result(struct myvava_resolver *r,struct myvava_resolution *out){if(!r||!out)return false;pthread_mutex_lock(&r->lock);bool ready=r->ready;if(ready)*out=r->result;pthread_mutex_unlock(&r->lock);return ready;}
