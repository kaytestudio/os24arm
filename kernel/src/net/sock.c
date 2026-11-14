/* sock.c - Internet sockets over lwIP's raw (callback) API: what the
 * Linux-ABI programs' AF_INET sockets are (see net/sock.h). TCP: a
 * connection's received data waits here in pbufs until it's read (the
 * window opens as it is); sending copies into lwIP as far as its send
 * buffer goes. UDP: datagrams queue with their sender. lwIP calls back
 * from net_poll(); every change wakes whoever sleeps on the socket.
 * IPv6 sockets use lwIP's dual stack pcbs (IPADDR_TYPE_ANY): IPv4 peers
 * as ::ffff:a.b.c.d, as Linux does. */
#include "net/sock.h"
#include "net/net.h"
#include "string.h"
#include "kprintf.h"

#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/raw.h"
#include "lwip/pbuf.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/prot/ip6.h"

#define E_AGAIN 11
#define E_NOMEM 12
#define E_INVAL 22
#define E_PIPE 32
#define E_ADDRINUSE 98
#define E_CONNRESET 104
#define E_ISCONN 106
#define E_NOTCONN 107
#define E_TIMEDOUT 110
#define E_CONNREFUSED 111
#define E_HOSTUNREACH 113
#define E_ALREADY 114
#define E_INPROGRESS 115
#define E_OPNOTSUPP 95
#define E_DESTADDRREQ 89

#define MAX_NSOCKS 128
#define BACKLOG    16
#define DGRAMS     64

typedef struct { struct pbuf *p; ip_addr_t from; uint16_t port; int16_t hop; } dgram_t;

struct nsock {
    int used, type, v6, hop;
    struct tcp_pcb *tcp;
    struct udp_pcb *udp;
    struct raw_pcb *raw;                   /* NS_RAW: ICMP */
    int listening, connecting, connected, eof, err, shut_wr, was_connected;
    struct pbuf *rx;                       /* TCP: received, unread */
    nsock_t *backlog[BACKLOG];             /* listening: accepted by lwIP, not yet by the program */
    int nbacklog;
    dgram_t dq[DGRAMS];                    /* UDP */
    int dhead, dcount;
};

static nsock_t socks[MAX_NSOCKS];
void (*nsock_wakeup)(void *chan);

static void wake(nsock_t *s) { if (nsock_wakeup) nsock_wakeup(s); }

static const uint8_t v4mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };

/* ours -> lwIP's: ::ffff:a.b.c.d and :: to what lwIP knows them as */
static void to_lwip(const nsock_t *s, const nsaddr_t *a, ip_addr_t *o)
{
    static const uint8_t zero[16];
    uint32_t v;
    if (!a->v6) { memcpy(&v, a->a, 4); ip_addr_set_ip4_u32(o, v); return; }
    if (!memcmp(a->a, v4mapped, 12)) { memcpy(&v, a->a + 12, 4); ip_addr_set_ip4_u32(o, v); return; }
    if (!memcmp(a->a, zero, 16)) { *o = s->v6 ? *IP_ANY_TYPE : *IP4_ADDR_ANY; return; }
    IP_SET_TYPE(o, IPADDR_TYPE_V6);
    memcpy(ip_2_ip6(o)->addr, a->a, 16);
    ip6_addr_clear_zone(ip_2_ip6(o));
    if (ip6_addr_islinklocal(ip_2_ip6(o))) ip6_addr_assign_zone(ip_2_ip6(o), IP6_UNICAST, netif_default);
}

/* lwIP's -> ours: for an IPv6 socket, IPv4 as ::ffff:a.b.c.d */
static void from_lwip(const nsock_t *s, const ip_addr_t *i, nsaddr_t *o)
{
    memset(o, 0, sizeof *o);
    o->v6 = (uint8_t)s->v6;
    if (!i || IP_IS_ANY_TYPE_VAL(*i)) return;
    if (IP_IS_V4(i)) {
        uint32_t v = ip4_addr_get_u32(ip_2_ip4(i));
        if (s->v6) { memcpy(o->a, v4mapped, 12); memcpy(o->a + 12, &v, 4); }
        else memcpy(o->a, &v, 4);
        return;
    }
    o->v6 = 1;
    memcpy(o->a, ip_2_ip6(i)->addr, 16);
}

static int lwip_errno(err_t e)
{
    switch (e) {
    case ERR_OK: return 0;
    case ERR_MEM: case ERR_BUF: return E_NOMEM;
    case ERR_RTE: return E_HOSTUNREACH;
    case ERR_INPROGRESS: return E_INPROGRESS;
    case ERR_ALREADY: return E_ALREADY;
    case ERR_ISCONN: return E_ISCONN;
    case ERR_CONN: return E_NOTCONN;
    case ERR_USE: return E_ADDRINUSE;
    case ERR_RST: return E_CONNRESET;
    case ERR_ABRT: return E_CONNRESET;
    case ERR_CLSD: return E_NOTCONN;
    case ERR_TIMEOUT: return E_TIMEDOUT;
    default: return E_INVAL;
    }
}

static nsock_t *alloc(int type, int v6)
{
    for (int i = 0; i < MAX_NSOCKS; i++)
        if (!socks[i].used) {
            memset(&socks[i], 0, sizeof socks[i]);
            socks[i].used = 1;
            socks[i].type = type;
            socks[i].v6 = v6;
            return &socks[i];
        }
    return 0;
}

/* ── TCP callbacks ──────────────────────────────────────────────────── */
static err_t tcp_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)pcb;
    nsock_t *s = arg;
    if (!s) { if (p) pbuf_free(p); return ERR_OK; }
    if (!p || err != ERR_OK) { s->eof = 1; if (p) pbuf_free(p); }
    else if (s->rx) pbuf_cat(s->rx, p);
    else s->rx = p;
    wake(s);
    return ERR_OK;
}

static err_t tcp_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t len)
{
    (void)pcb; (void)len;
    if (arg) wake(arg);
    return ERR_OK;
}

static void tcp_err_cb(void *arg, err_t err)
{
    nsock_t *s = arg;                      /* (the pcb is gone already) */
    if (!s) return;
    s->tcp = 0;
    s->err = s->connecting ? (err == ERR_RST ? E_CONNREFUSED : lwip_errno(err)) : lwip_errno(err);
    if (!s->err) s->err = E_CONNRESET;
    s->connecting = 0;
    s->eof = 1;
    wake(s);
}

static void tcp_hook(nsock_t *s, struct tcp_pcb *pcb)
{
    s->tcp = pcb;
    tcp_arg(pcb, s);
    tcp_recv(pcb, tcp_recv_cb);
    tcp_sent(pcb, tcp_sent_cb);
    tcp_err(pcb, tcp_err_cb);
}

static err_t tcp_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)pcb;
    nsock_t *s = arg;
    if (!s) return ERR_OK;
    s->connecting = 0;
    if (err == ERR_OK) { s->connected = s->was_connected = 1; }
    else s->err = lwip_errno(err);
    wake(s);
    return ERR_OK;
}

static err_t tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err)
{
    nsock_t *l = arg;
    if (err != ERR_OK || !newpcb || !l) return ERR_VAL;
    nsock_t *c = l->nbacklog < BACKLOG ? alloc(NS_STREAM, l->v6) : 0;
    if (!c) { tcp_abort(newpcb); return ERR_ABRT; }
    tcp_hook(c, newpcb);
    c->connected = c->was_connected = 1;
    l->backlog[l->nbacklog++] = c;
    wake(l);
    return ERR_OK;
}

/* ── UDP ───────────────────────────────────────────────────────────── */
static void udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    (void)pcb;
    nsock_t *s = arg;
    if (!s || s->dcount >= DGRAMS) { pbuf_free(p); return; }
    dgram_t *d = &s->dq[(s->dhead + s->dcount++) % DGRAMS];
    d->p = p;
    ip_addr_copy(d->from, *addr);
    d->port = port;
    d->hop = -1;
    wake(s);
}

/* ── raw (ICMP): a copy of every packet - ICMP: its IP header in front;
 *    ICMPv6: without it, as Linux - and lwIP goes on with them (its own
 *    echo replies, the shell's PING) ───────────────────────────────── */
static u8_t raw_recv_cb(void *arg, struct raw_pcb *pcb, struct pbuf *p, const ip_addr_t *addr)
{
    (void)pcb;
    nsock_t *s = arg;
    if (!s || s->dcount >= DGRAMS) return 0;
    u16_t skip = IP_IS_V6(addr) ? IP6_HLEN : 0;
    if (p->tot_len <= skip) return 0;
    struct pbuf *c = pbuf_alloc(PBUF_RAW, (u16_t)(p->tot_len - skip), PBUF_RAM);
    if (!c) return 0;
    pbuf_copy_partial(p, c->payload, c->len, skip);
    dgram_t *d = &s->dq[(s->dhead + s->dcount++) % DGRAMS];
    d->p = c;
    ip_addr_copy(d->from, *addr);
    d->port = 0;
    d->hop = skip ? ((const uint8_t *)p->payload)[7] : -1;      /* (IPv6 header: hop limit) */
    wake(s);
    return 0;                                         /* (not eaten) */
}

/* ── the calls ─────────────────────────────────────────────────────── */
nsock_t *nsock_new(int type, int v6)
{
    nsock_t *s = alloc(type, v6);
    if (!s) return 0;
    u8_t t = v6 ? IPADDR_TYPE_ANY : IPADDR_TYPE_V4;      /* (IPv6: both, as Linux) */
    if (type == NS_STREAM) {
        struct tcp_pcb *pcb = tcp_new_ip_type(t);
        if (!pcb) { s->used = 0; return 0; }
        tcp_hook(s, pcb);
    } else if (type == NS_RAW) {
        s->raw = v6 ? raw_new_ip_type(IPADDR_TYPE_V6, IP6_NEXTH_ICMP6) : raw_new_ip_type(IPADDR_TYPE_V4, IP_PROTO_ICMP);
        if (!s->raw) { s->used = 0; return 0; }
        if (v6) { s->raw->chksum_reqd = 1; s->raw->chksum_offset = 2; }   /* (ICMPv6: lwIP sums it) */
        raw_recv(s->raw, raw_recv_cb, s);
    } else {
        s->udp = udp_new_ip_type(t);
        if (!s->udp) { s->used = 0; return 0; }
        udp_recv(s->udp, udp_recv_cb, s);
    }
    return s;
}

void nsock_close(nsock_t *s)
{
    if (!s || !s->used) return;
    if (s->tcp) {
        tcp_arg(s->tcp, 0);
        tcp_recv(s->tcp, 0);
        tcp_sent(s->tcp, 0);
        tcp_err(s->tcp, 0);
        if (s->listening) tcp_accept(s->tcp, 0);
        if (tcp_close(s->tcp) != ERR_OK) tcp_abort(s->tcp);
        s->tcp = 0;
    }
    for (int i = 0; i < s->nbacklog; i++) nsock_close(s->backlog[i]);   /* never accepted */
    if (s->udp) { udp_remove(s->udp); s->udp = 0; }
    if (s->raw) { raw_remove(s->raw); s->raw = 0; }
    if (s->rx) pbuf_free(s->rx);
    while (s->dcount) { pbuf_free(s->dq[s->dhead].p); s->dhead = (s->dhead + 1) % DGRAMS; s->dcount--; }
    s->used = 0;
    wake(s);
}

int nsock_type(nsock_t *s) { return s->type; }
int nsock_v6(nsock_t *s) { return s->v6; }
int nsock_hoplimit(nsock_t *s) { return s->hop; }

int nsock_bind(nsock_t *s, const nsaddr_t *na, uint16_t port)
{
    ip_addr_t a;
    to_lwip(s, na, &a);
    err_t e = s->tcp ? tcp_bind(s->tcp, &a, port) : s->udp ? udp_bind(s->udp, &a, port) :
              s->raw ? raw_bind(s->raw, &a) : ERR_CLSD;
    return e == ERR_OK ? 0 : e == ERR_USE ? -E_ADDRINUSE : -lwip_errno(e);
}

int nsock_listen(nsock_t *s, int backlog)
{
    if (s->type != NS_STREAM) return -E_OPNOTSUPP;
    if (s->listening) return 0;
    if (!s->tcp) return -E_INVAL;
    if (s->tcp->local_port == 0) tcp_bind(s->tcp, s->v6 ? IP_ANY_TYPE : IP4_ADDR_ANY, 0);
    err_t e;
    struct tcp_pcb *l = tcp_listen_with_backlog_and_err(s->tcp, (u8_t)(backlog > 0 && backlog < 255 ? backlog : BACKLOG), &e);
    if (!l) return -lwip_errno(e);
    s->tcp = l;                            /* (the old pcb was freed) */
    tcp_arg(l, s);
    tcp_accept(l, tcp_accept_cb);
    s->listening = 1;
    return 0;
}

int nsock_connect(nsock_t *s, const nsaddr_t *na, uint16_t port)
{
    ip_addr_t a;
    to_lwip(s, na, &a);
    if (s->type == NS_RAW) return raw_connect(s->raw, &a) == ERR_OK ? 0 : -E_INVAL;
    if (s->type == NS_DGRAM) {
        if (!s->udp) return -E_INVAL;
        err_t e = udp_connect(s->udp, &a, port);
        return e == ERR_OK ? 0 : -lwip_errno(e);
    }
    if (s->connected) return -E_ISCONN;
    if (s->connecting) return -E_ALREADY;
    if (!s->tcp || s->listening) return -E_INVAL;
    s->err = 0;
    err_t e = tcp_connect(s->tcp, &a, port, tcp_connected_cb);
    if (e != ERR_OK) return -lwip_errno(e);
    s->connecting = 1;
    return -E_INPROGRESS;
}

int nsock_connecting(nsock_t *s) { return s->connecting; }

nsock_t *nsock_accept(nsock_t *s)
{
    if (!s->listening || !s->nbacklog) return 0;
    nsock_t *c = s->backlog[0];
    for (int i = 1; i < s->nbacklog; i++) s->backlog[i - 1] = s->backlog[i];
    s->nbacklog--;
    if (s->tcp) tcp_backlog_accepted(c->tcp);
    return c;
}

long nsock_send(nsock_t *s, const void *buf, unsigned long n, const nsaddr_t *to, uint16_t port)
{
    if (s->type == NS_RAW) {                          /* the ICMP message: lwIP puts the IP header on */
        if (!s->raw) return -E_INVAL;
        if (n > 65507) return -90;
        struct pbuf *p = pbuf_alloc(PBUF_IP, (u16_t)n, PBUF_RAM);
        if (!p) return -E_NOMEM;
        if (n) pbuf_take(p, buf, (u16_t)n);
        err_t e;
        if (to) { ip_addr_t a; to_lwip(s, to, &a); e = raw_sendto(s->raw, p, &a); }
        else e = raw_send(s->raw, p);
        pbuf_free(p);
        return e == ERR_OK ? (long)n : -lwip_errno(e);
    }
    if (s->type == NS_DGRAM) {
        if (!s->udp) return -E_INVAL;
        if (n > 65507) return -90;                            /* EMSGSIZE */
        struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)n, PBUF_RAM);
        if (!p) return -E_NOMEM;
        if (n) pbuf_take(p, buf, (u16_t)n);
        err_t e;
        if (to) {
            ip_addr_t a;
            to_lwip(s, to, &a);
            e = udp_sendto(s->udp, p, &a, port);
        } else {
            if (ip_addr_isany(&s->udp->remote_ip) && !s->udp->remote_port) { pbuf_free(p); return -E_DESTADDRREQ; }
            e = udp_send(s->udp, p);
        }
        pbuf_free(p);
        return e == ERR_OK ? (long)n : -lwip_errno(e);
    }
    if (s->err) { int e = s->err; s->err = 0; return -e; }
    if (!s->connected || !s->tcp) return s->was_connected ? -E_PIPE : -E_NOTCONN;
    if (s->shut_wr) return -E_PIPE;
    unsigned long room = tcp_sndbuf(s->tcp);
    if (tcp_sndqueuelen(s->tcp) >= TCP_SND_QUEUELEN - 4) room = 0;
    if (!room) return -E_AGAIN;
    if (n > room) n = room;
    if (n > 0xFFFF) n = 0xFFFF;
    err_t e = tcp_write(s->tcp, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e == ERR_MEM) return -E_AGAIN;
    if (e != ERR_OK) return -lwip_errno(e);
    tcp_output(s->tcp);
    return (long)n;
}

long nsock_recv(nsock_t *s, void *buf, unsigned long n, nsaddr_t *from, uint16_t *port, int peek)
{
    if (s->type == NS_DGRAM || s->type == NS_RAW) {
        if (!s->dcount) return -E_AGAIN;
        dgram_t *d = &s->dq[s->dhead];
        unsigned long c = n < d->p->tot_len ? n : d->p->tot_len;
        pbuf_copy_partial(d->p, buf, (u16_t)c, 0);
        if (from) from_lwip(s, &d->from, from);
        s->hop = d->hop;
        if (port) *port = d->port;
        if (!peek) {
            pbuf_free(d->p);
            s->dhead = (s->dhead + 1) % DGRAMS;
            s->dcount--;
        }
        return (long)c;                                     /* (the rest of a long one: gone) */
    }
    if (!s->rx) {
        if (s->err && !s->was_connected) { int e = s->err; s->err = 0; return -e; }
        if (s->eof || !s->tcp) return 0;                     /* the end of the stream */
        if (!s->connected) return -E_NOTCONN;
        return -E_AGAIN;
    }
    unsigned long done = n < s->rx->tot_len ? n : s->rx->tot_len;
    if (done > 0xFFFF) done = 0xFFFF;
    pbuf_copy_partial(s->rx, buf, (u16_t)done, 0);
    if (from || port) nsock_addr(s, 1, from, port);
    if (peek) return (long)done;
    s->rx = pbuf_free_header(s->rx, (u16_t)done);        /* what was read: gone */
    if (s->tcp) tcp_recved(s->tcp, (u16_t)(done > 0xFFFF ? 0xFFFF : done));
    return (long)done;
}

unsigned nsock_poll(nsock_t *s)
{
    unsigned m = 0;
    if (s->type == NS_DGRAM || s->type == NS_RAW) return (s->dcount ? NS_IN : 0) | NS_OUT;
    if (s->listening) return s->nbacklog ? NS_IN : 0;
    if (s->rx || s->eof) m |= NS_IN;
    if (s->eof) m |= NS_RDHUP;
    if (s->err) m |= NS_ERR | NS_IN | NS_OUT;
    if (s->connected && s->tcp && !s->shut_wr && tcp_sndbuf(s->tcp) > 0) m |= NS_OUT;
    if (!s->tcp && s->was_connected) m |= NS_HUP;
    if (!s->connected && !s->connecting && !s->listening && !s->was_connected) m |= NS_OUT | NS_HUP;
    return m;
}

int nsock_shutdown(nsock_t *s, int how)
{
    if (s->type != NS_STREAM || !s->tcp) return s->type == NS_STREAM ? -E_NOTCONN : 0;
    if (!s->connected) return -E_NOTCONN;
    err_t e = tcp_shutdown(s->tcp, how == 0 || how == 2, how == 1 || how == 2);
    if (how == 1 || how == 2) s->shut_wr = 1;
    if (how == 0 || how == 2) s->eof = 1;
    wake(s);
    return e == ERR_OK ? 0 : -lwip_errno(e);
}

int nsock_error(nsock_t *s, int clear)
{
    int e = s->err;
    if (clear) s->err = 0;
    return e;
}

int nsock_addr(nsock_t *s, int peer, nsaddr_t *na, uint16_t *port)
{
    const ip_addr_t *a = 0;
    uint16_t p = 0;
    if (s->tcp) { a = peer ? &s->tcp->remote_ip : &s->tcp->local_ip; p = peer ? s->tcp->remote_port : s->tcp->local_port; }
    else if (s->udp) { a = peer ? &s->udp->remote_ip : &s->udp->local_ip; p = peer ? s->udp->remote_port : s->udp->local_port; }
    if (peer && (!s->connected && !(s->udp && p))) return -E_NOTCONN;
    if (na) from_lwip(s, a, na);
    if (port) *port = p;
    return 0;
}

long nsock_unread(nsock_t *s)
{
    if (s->type == NS_DGRAM || s->type == NS_RAW) return s->dcount ? s->dq[s->dhead].p->tot_len : 0;
    return s->rx ? (long)s->rx->tot_len : 0;
}

void nsock_nodelay(nsock_t *s, int on)
{
    if (s->tcp && !s->listening) { if (on) tcp_nagle_disable(s->tcp); else tcp_nagle_enable(s->tcp); }
}
