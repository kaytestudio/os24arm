/* dgram.c - AF_UNIX SOCK_DGRAM: message boundaries over a socketpair
 * (truncation, zero-length datagrams, MSG_PEEK, MSG_TRUNC, SCM_RIGHTS, the
 * other end closing), named sockets (sendto/recvfrom with the sender's
 * name, a reply, connect + send, getpeername), non-blocking reads, poll,
 * a child sending across fork; SOCK_SEQPACKET (connect/accept, messages over
 * the connection, a socketpair, the other end closing). */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

static socklen_t addr(struct sockaddr_un *a, const char *path)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    strcpy(a->sun_path, path);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(path) + 1);
}

int main(void)
{
    int sv[2];
    char b[64];
    CHECK(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) == 0, "socketpair(AF_UNIX, SOCK_DGRAM)");
    int type = 0; socklen_t tl = sizeof type;
    getsockopt(sv[0], SOL_SOCKET, SO_TYPE, &type, &tl);
    CHECK(type == SOCK_DGRAM, "SO_TYPE is SOCK_DGRAM");
    send(sv[0], "a", 1, 0); send(sv[0], "bb", 2, 0); send(sv[0], "", 0, 0); send(sv[0], "cccc", 4, 0);
    ssize_t r1 = recv(sv[1], b, sizeof b, 0), r2 = recv(sv[1], b, sizeof b, 0), r3 = recv(sv[1], b, sizeof b, 0);
    CHECK(r1 == 1 && r2 == 2 && r3 == 0, "one message a read: %zd %zd %zd (a zero-length one too)", r1, r2, r3);
    ssize_t pk = recv(sv[1], b, 2, MSG_PEEK | MSG_TRUNC);
    struct iovec io = { b, 2 };
    struct msghdr mh = { .msg_iov = &io, .msg_iovlen = 1 };
    ssize_t rt = recvmsg(sv[1], &mh, 0);
    CHECK(pk == 4 && rt == 2 && (mh.msg_flags & MSG_TRUNC) && !memcmp(b, "cc", 2),
          "truncated to the buffer, MSG_TRUNC (peek %zd, got %zd)", pk, rt);
    fcntl(sv[1], F_SETFL, O_NONBLOCK);
    CHECK(recv(sv[1], b, sizeof b, 0) < 0 && errno == EAGAIN, "the rest of it dropped; empty: EAGAIN");

    int p[2];
    pipe(p);
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct iovec sio = { "fd", 2 };
    struct msghdr sm = { .msg_iov = &sio, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    struct cmsghdr *c = CMSG_FIRSTHDR(&sm);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &p[1], sizeof(int));
    sendmsg(sv[0], &sm, 0);
    char rbuf[CMSG_SPACE(sizeof(int))];
    struct iovec rio = { b, sizeof b };
    struct msghdr rm = { .msg_iov = &rio, .msg_iovlen = 1, .msg_control = rbuf, .msg_controllen = sizeof rbuf };
    ssize_t rr = recvmsg(sv[1], &rm, 0);
    int got = -1;
    struct cmsghdr *rc = CMSG_FIRSTHDR(&rm);
    if (rc && rc->cmsg_type == SCM_RIGHTS) memcpy(&got, CMSG_DATA(rc), sizeof got);
    if (got >= 0) write(got, "via", 3);
    char pb[4] = { 0 };
    read(p[0], pb, 3);
    CHECK(rr == 2 && got >= 0 && !strcmp(pb, "via"), "SCM_RIGHTS with a datagram");
    close(sv[0]);
    fcntl(sv[1], F_SETFL, 0);
    CHECK(recv(sv[1], b, sizeof b, 0) == 0, "the other end closed: 0");
    close(sv[1]);

    struct sockaddr_un sa, ca, from;
    socklen_t sl = addr(&sa, "/tmp/dg-server"), cl = addr(&ca, "/tmp/dg-client");
    int srv = socket(AF_UNIX, SOCK_DGRAM, 0), cli = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(bind(srv, (struct sockaddr *)&sa, sl) == 0 && bind(cli, (struct sockaddr *)&ca, cl) == 0, "bind two datagram sockets");
    struct pollfd pf = { srv, POLLIN, 0 };
    CHECK(poll(&pf, 1, 0) == 0, "poll: nothing to read yet");
    CHECK(sendto(cli, "hello", 5, 0, (struct sockaddr *)&sa, sl) == 5, "sendto the server");
    CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "poll: readable");
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(srv, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
    CHECK(n == 5 && !memcmp(b, "hello", 5) && !strcmp(from.sun_path, "/tmp/dg-client"), "recvfrom: the message and who sent it (%s)", from.sun_path);
    sendto(srv, "reply", 5, 0, (struct sockaddr *)&from, fl);
    n = recv(cli, b, sizeof b, 0);
    CHECK(n == 5 && !memcmp(b, "reply", 5), "a reply to the sender's name");
    CHECK(sendto(cli, "x", 1, 0, (struct sockaddr *)&from, addr(&from, "/tmp/dg-nobody")) < 0 && errno == ENOENT, "sendto a name nobody has: ENOENT");
    CHECK(connect(cli, (struct sockaddr *)&sa, sl) == 0 && send(cli, "conn", 4, 0) == 4, "connect, then send without an address");
    struct sockaddr_un pa; socklen_t pl = sizeof pa;
    CHECK(getpeername(cli, (struct sockaddr *)&pa, &pl) == 0 && !strcmp(pa.sun_path, "/tmp/dg-server"), "getpeername: %s", pa.sun_path);
    n = recv(srv, b, sizeof b, 0);
    CHECK(n == 4 && !memcmp(b, "conn", 4), "it arrived");
    pid_t ch = fork();
    if (ch == 0) {
        int s = socket(AF_UNIX, SOCK_DGRAM, 0);
        for (int i = 0; i < 5; i++) { char m[8]; int l = snprintf(m, sizeof m, "c%d", i); sendto(s, m, (size_t)l, 0, (struct sockaddr *)&sa, sl); }
        _exit(0);
    }
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        char want[8]; snprintf(want, sizeof want, "c%d", i);
        n = recv(srv, b, sizeof b, 0);
        if (n != (ssize_t)strlen(want) || memcmp(b, want, (size_t)n)) ok = 0;
    }
    waitpid(ch, 0, 0);
    CHECK(ok, "five datagrams from a child, in order");
    close(srv); close(cli);

    /* SOCK_SEQPACKET: a connection, carrying messages */
    struct sockaddr_un qa;
    socklen_t ql = addr(&qa, "/tmp/seq-server");
    int ls = socket(AF_UNIX, SOCK_SEQPACKET, 0), cs = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    type = 0; tl = sizeof type;
    getsockopt(ls, SOL_SOCKET, SO_TYPE, &type, &tl);
    CHECK(type == SOCK_SEQPACKET && bind(ls, (struct sockaddr *)&qa, ql) == 0 && listen(ls, 4) == 0, "SOCK_SEQPACKET: bind, listen");
    int ds = socket(AF_UNIX, SOCK_DGRAM, 0);
    CHECK(connect(ds, (struct sockaddr *)&qa, ql) < 0 && errno == EPROTOTYPE, "a datagram socket can't connect to it: EPROTOTYPE");
    close(ds);
    CHECK(send(cs, "x", 1, 0) < 0 && errno == ENOTCONN, "not connected: ENOTCONN");
    CHECK(connect(cs, (struct sockaddr *)&qa, ql) == 0, "connect");
    int as = accept(ls, 0, 0);
    CHECK(as >= 0, "accept");
    send(cs, "one", 3, 0); send(cs, "second", 6, 0); send(as, "back", 4, 0);
    ssize_t q1 = recv(as, b, sizeof b, 0), q2 = recv(as, b, 3, 0), q3 = recv(cs, b, sizeof b, 0);
    CHECK(q1 == 3 && q2 == 3 && !memcmp(b, "back", 4) && q3 == 4, "messages both ways, whole (%zd %zd %zd)", q1, q2, q3);
    int qp[2];
    CHECK(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, qp) == 0 && send(qp[0], "pp", 2, 0) == 2 && recv(qp[1], b, sizeof b, 0) == 2, "a SEQPACKET socketpair");
    close(qp[0]); close(qp[1]);
    close(cs);
    CHECK(recv(as, b, sizeof b, 0) == 0, "the other end closed: 0");
    signal(SIGPIPE, SIG_IGN);
    CHECK(send(as, "z", 1, 0) < 0 && errno == EPIPE, "send to it: EPIPE");
    close(as); close(ls);
    printf(fails ? "dgram: %d FAILED\n" : "dgram: all ok\n", fails);
    return fails != 0;
}
