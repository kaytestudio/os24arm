/* sock.h - Internet sockets over lwIP (kernel/src/net/sock.c), for the
 * Linux-ABI programs' AF_INET and AF_INET6 sockets (kernel/src/linux/linux.c).
 * Addresses: nsaddr_t, ports in host order; errors are -errno
 * (Linux's numbers). Everything that changes a socket's readiness calls
 * nsock_wakeup(socket) - the Linux layer sleeps on the socket's address. */
#ifndef OS2WARP_NET_SOCK_H
#define OS2WARP_NET_SOCK_H
#include "types.h"

typedef struct nsock nsock_t;

/* an address: IPv4 (a[0..3], network order) or IPv6 (a, 16 bytes). An
 * IPv6 socket is dual stack, as Linux's are: IPv4 peers appear as
 * ::ffff:a.b.c.d, and such addresses reach IPv4. */
typedef struct { uint8_t v6; uint8_t a[16]; } nsaddr_t;
enum { NS_STREAM = 1, NS_DGRAM = 2, NS_RAW = 3 };   /* (RAW: ICMP - ping) */
enum { NS_IN = 1, NS_OUT = 4, NS_ERR = 8, NS_HUP = 16, NS_RDHUP = 0x2000 };   /* poll's bits */

extern void (*nsock_wakeup)(void *chan);         /* set by the Linux layer */

nsock_t *nsock_new(int type, int v6);            /* NS_RAW: ICMP (v4: packets with their IP header)
                                                    or ICMPv6 (without, as Linux) */
int      nsock_v6(nsock_t *s);
int      nsock_hoplimit(nsock_t *s);                /* the last ICMPv6 packet's hop limit read, or -1 */
void     nsock_close(nsock_t *s);
int      nsock_bind(nsock_t *s, const nsaddr_t *a, uint16_t port);
int      nsock_listen(nsock_t *s, int backlog);
int      nsock_connect(nsock_t *s, const nsaddr_t *a, uint16_t port);   /* 0: done (UDP); -115: under way */
int      nsock_connecting(nsock_t *s);           /* a TCP connect still under way? */
nsock_t *nsock_accept(nsock_t *s);               /* a new connection, or 0 */
long     nsock_send(nsock_t *s, const void *buf, unsigned long n, const nsaddr_t *to, uint16_t port);
long     nsock_recv(nsock_t *s, void *buf, unsigned long n, nsaddr_t *from, uint16_t *port, int peek);
unsigned nsock_poll(nsock_t *s);
int      nsock_shutdown(nsock_t *s, int how);    /* 0 read, 1 write, 2 both */
int      nsock_error(nsock_t *s, int clear);     /* the pending error (SO_ERROR), as errno */
int      nsock_addr(nsock_t *s, int peer, nsaddr_t *a, uint16_t *port);
int      nsock_type(nsock_t *s);
long     nsock_unread(nsock_t *s);               /* FIONREAD */
void     nsock_nodelay(nsock_t *s, int on);
#endif
