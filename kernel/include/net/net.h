/* net.h - the kernel's networking: a virtio-net card (virtio_net.c) and
 * lwIP (external/lwip, net.c), polled. */
#ifndef OS2WARP_NET_H
#define OS2WARP_NET_H
#include "types.h"

int  vnet_init(uint8_t mac[6]);                  /* 0: there's a card */
int  vnet_send(const void *frame, unsigned len);
int  vnet_recv(void *buf, unsigned max);         /* a frame's length, or 0 */

void net_init(void);                             /* the card, DHCP */
void net_poll(void);                             /* frames in, timers: call often */
int  net_up(void);                               /* an address yet? */
int  net_resolv_conf(char *buf, int cap);        /* "nameserver ..." lines; their length */
unsigned long net_now_ms(void);

void net_cmd_ifconfig(int argc, char **argv);    /* the shell's commands */
void net_cmd_ping(int argc, char **argv);
void net_cmd_host(int argc, char **argv);
#endif
