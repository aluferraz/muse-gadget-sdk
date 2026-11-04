/* SPDX-License-Identifier: Apache-2.0 */
#include "phone_relay.h"
#include <errno.h>
#include <string.h>
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#define PORT_BASE 18442
/* Link-time adapters keep TLS hostnames/certificates unchanged. They only
 * replace resolution and TCP destination in this dedicated relay build. */
int __real_lwip_getaddrinfo(const char *, const char *, const struct addrinfo *, struct addrinfo **);
int __wrap_lwip_getaddrinfo(const char *host, const char *service, const struct addrinfo *hints, struct addrinfo **out)
{
    if (out)
        *out = NULL;
    if (!host || !out || !muse_phone_relay_ready())
        return EAI_AGAIN;
    const char *ip = !strcmp(host, "api.muse.ai")          ? "198.18.0.1"
                     : !strcmp(host, "hatch.metaaivm.com") ? "198.18.0.2"
                                                           : NULL;
    if (!ip)
        return EAI_NONAME;
    return __real_lwip_getaddrinfo(ip, service, hints, out);
}
int __real_lwip_connect(int, const struct sockaddr *, socklen_t);
int __wrap_lwip_connect(int fd, const struct sockaddr *a, socklen_t len)
{
    if (!a || len < sizeof(struct sockaddr_in) || a->sa_family != AF_INET || !muse_phone_relay_ready()) {
        errno = ENETUNREACH;
        return -1;
    }
    struct sockaddr_in dst = *(const struct sockaddr_in *)a;
    uint32_t ip = ntohl(dst.sin_addr.s_addr);
    unsigned endpoint = ip - 0xc6120000u;
    if (endpoint < 1 || endpoint > 2 || ntohs(dst.sin_port) != 443) {
        errno = EACCES;
        return -1;
    }
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(PORT_BASE + endpoint);
    return __real_lwip_connect(fd, (struct sockaddr *)&dst, sizeof(dst));
}
