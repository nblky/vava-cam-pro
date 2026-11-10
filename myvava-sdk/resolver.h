#ifndef MYVAVA_RESOLVER_H
#define MYVAVA_RESOLVER_H
#include <stdbool.h>
#include <sys/socket.h>
#include <netdb.h>
#define MYVAVA_ADDRESSES 16
struct myvava_address {struct sockaddr_storage address;socklen_t size;};
struct myvava_resolution {unsigned count;int error;struct myvava_address peers[MYVAVA_ADDRESSES];};
struct myvava_resolver;
typedef int (*myvava_lookup_fn)(const char *,const char *,const struct addrinfo *,struct addrinfo **);
/* Detached worker owns independent storage. Releasing an unresolved job never
 * joins libc DNS or leaves it accessing a stopped client. One job/client only. */
struct myvava_resolver *myvava_resolver_start(const char *host,unsigned port,myvava_lookup_fn);
bool myvava_resolver_result(struct myvava_resolver *,struct myvava_resolution *);
void myvava_resolver_release(struct myvava_resolver *);
#endif
