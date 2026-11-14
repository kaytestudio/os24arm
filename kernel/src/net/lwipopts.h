/* lwipopts.h - the kernel's lwIP (external/lwip): no OS underneath
 * (NO_SYS: the kernel polls it - net.c), IPv4, Ethernet with ARP, DHCP,
 * DNS, ICMP, UDP, TCP, raw sockets; memory of its own. */
#ifndef OS2WARP_LWIPOPTS_H
#define OS2WARP_LWIPOPTS_H

/* lwIP's heap has the kernel allocator's names (kernel/src/mem.c): its own */
#define mem_init    lwip_mem_init
#define mem_free    lwip_mem_free
#define mem_malloc  lwip_mem_malloc
#define mem_calloc  lwip_mem_calloc
#define mem_trim    lwip_mem_trim

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0      /* (the kernel isn't preempted: one caller at a time) */
#define LWIP_TIMERS                 1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

#define LWIP_NO_STDINT_H            1      /* (arch/cc.h has them) */
#define LWIP_NO_INTTYPES_H          1
#define LWIP_NO_LIMITS_H            1
#define LWIP_NO_CTYPE_H             1
#define LWIP_NO_UNISTD_H            1      /* (ssize_t: arch/cc.h) */

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_UDP                    1
#define LWIP_TCP                    1
#define LWIP_IGMP                   0
#define LWIP_DHCP                   1
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_ACD                    0
#define LWIP_AUTOIP                 0
#define LWIP_DNS                    1
#define DNS_MAX_SERVERS             2
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LOOPBACK         1      /* to its own address, and 127.0.0.1: */
#define LWIP_HAVE_LOOPIF            1      /* ... lo (net_poll runs it) */
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_STATS                  0
#define LWIP_SO_RCVBUF              0

#define MEM_ALIGNMENT               8
#define MEM_SIZE                    (1024 * 1024)
#define MEMP_NUM_PBUF               64
#define PBUF_POOL_SIZE              256
#define MEMP_NUM_RAW_PCB            8
#define MEMP_NUM_UDP_PCB            32
#define MEMP_NUM_TCP_PCB            64
#define MEMP_NUM_TCP_PCB_LISTEN     32
#define MEMP_NUM_TCP_SEG            512
#define MEMP_NUM_SYS_TIMEOUT        16
#define MEMP_NUM_ARP_QUEUE          32

#define TCP_MSS                     1460
#define TCP_WND                     (32 * TCP_MSS)
#define TCP_SND_BUF                 (32 * TCP_MSS)
#define TCP_SND_QUEUELEN            (4 * TCP_SND_BUF / TCP_MSS)
#define TCP_QUEUE_OOSEQ             1
#define LWIP_TCP_KEEPALIVE          1

#define LWIP_CHKSUM_ALGORITHM       3

#endif
