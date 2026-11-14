/* net.c - lwIP in the kernel: the network interface over the virtio-net
 * card, DHCP, polling (net_poll: from the shell's idle loop and the
 * scheduler - the kernel isn't preempted, so lwIP has one caller at a
 * time), and the shell's commands IFCONFIG, PING and HOST. */
#include "net/net.h"
#include "kprintf.h"
#include "string.h"
#include "uart.h"

#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/dns.h"
#include "lwip/raw.h"
#include "lwip/icmp.h"
#include "lwip/inet_chksum.h"
#include "lwip/ip4.h"
#include "lwip/prot/ip4.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

static struct netif nif;
static int have_card, announced;
static uint8_t rxbuf[2048];

unsigned long net_now_ms(void)
{
    unsigned long c, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(c));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? c / (f / 1000) : 0;
}

u32_t sys_now(void) { return (u32_t)net_now_ms(); }

unsigned int lwip_rand(void)
{
    static unsigned long x;
    unsigned long c;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(c));
    x = x * 6364136223846793005UL + c + 1442695040888963407UL;
    return (unsigned int)(x >> 33);
}

static err_t linkoutput(struct netif *n, struct pbuf *p)
{
    (void)n;
    static uint8_t frame[1600];
    if (p->tot_len > sizeof frame) return ERR_BUF;
    pbuf_copy_partial(p, frame, p->tot_len, 0);
    return vnet_send(frame, p->tot_len) == 0 ? ERR_OK : ERR_MEM;
}

static err_t nif_init(struct netif *n)
{
    n->name[0] = 'l'; n->name[1] = 'n';                 /* lan0, as OS/2 called it */
    n->output = etharp_output;
    n->linkoutput = linkoutput;
    n->mtu = 1500;
    n->hwaddr_len = 6;
    n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

static void status_cb(struct netif *n)
{
    if (!ip4_addr_isany_val(*netif_ip4_addr(n)) && !announced) {
        announced = 1;
        kprintf("[NET]    lan0: %s", ip4addr_ntoa(netif_ip4_addr(n)));
        kprintf(" (gateway %s)\n", ip4addr_ntoa(netif_ip4_gw(n)));
    }
}

void net_init(void)
{
    uint8_t mac[6];
    if (vnet_init(mac) != 0) return;                    /* no card: no network */
    lwip_init();
    memcpy(nif.hwaddr, mac, 6);
    if (!netif_add(&nif, IP4_ADDR_ANY4, IP4_ADDR_ANY4, IP4_ADDR_ANY4, 0, nif_init, ethernet_input)) return;
    netif_set_hostname(&nif, "os2warp");
    netif_set_status_callback(&nif, status_cb);
    netif_set_default(&nif);
    netif_set_up(&nif);
    dhcp_start(&nif);
    have_card = 1;
    kprintf("[NET]    lan0: virtio-net %02x:%02x:%02x:%02x:%02x:%02x, asking DHCP for an address\n",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int net_up(void) { return have_card && !ip4_addr_isany_val(*netif_ip4_addr(&nif)); }

void net_poll(void)
{
    if (!have_card) return;
    static int depth;
    if (depth) return;                                  /* (not from inside itself) */
    depth++;
    for (int k = 0; k < 64; k++) {
        int n = vnet_recv(rxbuf, sizeof rxbuf);
        if (n <= 0) break;
        struct pbuf *p = pbuf_alloc(PBUF_RAW, (u16_t)n, PBUF_POOL);
        if (!p) continue;
        pbuf_take(p, rxbuf, (u16_t)n);
        if (nif.input(p, &nif) != ERR_OK) pbuf_free(p);
    }
    sys_check_timeouts();
    depth--;
}

/* waits up to ms, the network running, until *done (or a key: Esc, Ctrl+C) */
extern void linux_kernel_yield(void) __attribute__((weak));   /* kernel/src/linux */
static int wait_for(volatile int *done, unsigned long ms)
{
    unsigned long t0 = net_now_ms();
    while (!*done && net_now_ms() - t0 < ms) {
        net_poll();
        if (linux_kernel_yield) linux_kernel_yield();   /* (a desktop's prompt: the others go on) */
        if (uart_ready()) { char c = uart_getc(); if (c == 3 || c == 27) return -1; }
    }
    return *done ? 0 : 1;
}

void net_cmd_ifconfig(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!have_card) { kprintf("\nNo network card (QEMU: -device virtio-net-device,netdev=n0 -netdev user,id=n0)\n"); return; }
    kprintf("\nlan0: flags=%s  mtu %d\n", netif_is_up(&nif) ? "UP" : "DOWN", nif.mtu);
    kprintf("      ether %02x:%02x:%02x:%02x:%02x:%02x\n", nif.hwaddr[0], nif.hwaddr[1], nif.hwaddr[2],
            nif.hwaddr[3], nif.hwaddr[4], nif.hwaddr[5]);
    if (!net_up()) { kprintf("      no address yet (DHCP)\n"); return; }
    kprintf("      inet %s", ip4addr_ntoa(netif_ip4_addr(&nif)));
    kprintf("  netmask %s", ip4addr_ntoa(netif_ip4_netmask(&nif)));
    kprintf("  gateway %s\n", ip4addr_ntoa(netif_ip4_gw(&nif)));
    for (int i = 0; i < DNS_MAX_SERVERS; i++) {
        const ip_addr_t *d = dns_getserver((u8_t)i);
        if (!ip_addr_isany(d)) kprintf("      nameserver %s\n", ipaddr_ntoa(d));
    }
}

/* a name or a dotted address -> out; 0, or -1 (said why) */
static volatile int dns_done;
static ip_addr_t dns_result;
static void dns_cb(const char *name, const ip_addr_t *addr, void *arg)
{
    (void)name; (void)arg;
    if (addr) dns_result = *addr; else ip_addr_set_zero(&dns_result);
    dns_done = 1;
}

static int resolve(const char *name, ip_addr_t *out)
{
    if (ipaddr_aton(name, out)) return 0;
    dns_done = 0;
    err_t e = dns_gethostbyname(name, out, dns_cb, 0);
    if (e == ERR_OK) return 0;
    if (e != ERR_INPROGRESS) { kprintf("\n%s: can't look that up\n", name); return -1; }
    if (wait_for(&dns_done, 5000) != 0 || ip_addr_isany(&dns_result)) { kprintf("\n%s: unknown host\n", name); return -1; }
    *out = dns_result;
    return 0;
}

static int need_net(void)
{
    if (!have_card) { net_cmd_ifconfig(0, 0); return 0; }
    if (!net_up()) {                                    /* (just booted: DHCP may still be at it) */
        volatile int up = 0;
        unsigned long t0 = net_now_ms();
        while (!(up = net_up()) && net_now_ms() - t0 < 5000) {
            net_poll();
            if (linux_kernel_yield) linux_kernel_yield();
        }
        if (!up) { kprintf("\nThe network has no address (DHCP didn't answer)\n"); return 0; }
    }
    return 1;
}

void net_cmd_host(int argc, char **argv)
{
    if (argc < 2) { kprintf("\nUsage: HOST name\n"); return; }
    if (!need_net()) return;
    ip_addr_t a;
    if (resolve(argv[1], &a) == 0) kprintf("\n%s has address %s\n", argv[1], ipaddr_ntoa(&a));
}

/* PING: ICMP echo, through a raw pcb */
static volatile int ping_got;
static u16_t ping_seq, ping_id = 0x4F32;
static unsigned long ping_t0, ping_rtt;
static u8_t ping_ttl;

static u8_t ping_recv(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)arg; (void)pcb; (void)addr;
    struct ip_hdr *iph = (struct ip_hdr *)p->payload;
    u16_t hl = (u16_t)(IPH_HL(iph) * 4);
    if (p->tot_len >= hl + sizeof(struct icmp_echo_hdr)) {
        struct icmp_echo_hdr e;
        pbuf_copy_partial(p, &e, sizeof e, hl);
        if (e.type == ICMP_ER && e.id == lwip_htons(ping_id) && e.seqno == lwip_htons(ping_seq)) {
            ping_rtt = net_now_ms() - ping_t0;
            ping_ttl = IPH_TTL(iph);
            ping_got = 1;
            pbuf_free(p);
            return 1;                                   /* ours */
        }
    }
    return 0;
}

void net_cmd_ping(int argc, char **argv)
{
    if (argc < 2) { kprintf("\nUsage: PING host [count]\n"); return; }
    if (!need_net()) return;
    ip_addr_t to;
    if (resolve(argv[1], &to) != 0) return;
    int count = 4;
    if (argc > 2) { count = 0; for (const char *c = argv[2]; *c >= '0' && *c <= '9'; c++) count = count * 10 + (*c - '0'); }
    if (count < 1) count = 1;
    struct raw_pcb *pcb = raw_new(IP_PROTO_ICMP);
    if (!pcb) { kprintf("\nPING: out of memory\n"); return; }
    raw_recv(pcb, ping_recv, 0);
    raw_bind(pcb, IP_ADDR_ANY);
    kprintf("\nPING %s (%s): 56 data bytes\n", argv[1], ipaddr_ntoa(&to));
    int sent = 0, got = 0;
    unsigned long total = 0;
    for (int i = 0; i < count; i++) {
        struct pbuf *p = pbuf_alloc(PBUF_IP, sizeof(struct icmp_echo_hdr) + 56, PBUF_RAM);
        if (!p) break;
        struct icmp_echo_hdr *e = (struct icmp_echo_hdr *)p->payload;
        ICMPH_TYPE_SET(e, ICMP_ECHO);
        ICMPH_CODE_SET(e, 0);
        e->id = lwip_htons(ping_id);
        e->seqno = lwip_htons(++ping_seq);
        for (int k = 0; k < 56; k++) ((u8_t *)p->payload)[sizeof *e + k] = (u8_t)k;
        e->chksum = 0;
        e->chksum = inet_chksum(e, (u16_t)p->len);
        ping_got = 0;
        ping_t0 = net_now_ms();
        raw_sendto(pcb, p, &to);
        pbuf_free(p);
        sent++;
        int r = wait_for(&ping_got, 1000);
        if (r < 0) break;
        if (r == 0) {
            got++;
            total += ping_rtt;
            kprintf("64 bytes from %s: icmp_seq=%d ttl=%d time=%u ms\n", ipaddr_ntoa(&to), ping_seq, ping_ttl, (unsigned)ping_rtt);
            volatile int never = 0;
            if (i + 1 < count && wait_for(&never, 1000 - (ping_rtt < 1000 ? ping_rtt : 999)) < 0) break;
        } else {
            kprintf("Request timeout for icmp_seq %d\n", ping_seq);
        }
    }
    raw_remove(pcb);
    kprintf("--- %s ping statistics ---\n%d packets transmitted, %d received, %d%% packet loss",
            argv[1], sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got) kprintf(", average %u ms", (unsigned)(total / (unsigned long)got));
    kprintf("\n");
}
