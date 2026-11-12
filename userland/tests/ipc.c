/* ipc.c - stage 3 test: eventfd, timerfd, poll, epoll, signals (handlers,
 * SA_RESTART, signalfd, SIGPIPE), memfd + MAP_SHARED, AF_UNIX sockets
 * with SCM_RIGHTS, /tmp, /dev. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>

static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); printf("  (errno %d)", errno); fails++; } \
                           putchar('\n'); } while (0)

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static volatile int got_sig, got_pid;
static void on_usr1(int sig, siginfo_t *si, void *uc) { (void)uc; got_sig = sig; got_pid = si->si_pid; }
static void on_usr2(int sig) { got_sig = sig; }

static int send_fd(int sock, int fd, const char *msg)
{
    char cbuf[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { (void *)msg, strlen(msg) };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET; c->cmsg_type = SCM_RIGHTS; c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof fd);
    return sendmsg(sock, &m, MSG_NOSIGNAL);
}

static int recv_fd(int sock, char *buf, size_t n, int *fd)
{
    char cbuf[CMSG_SPACE(4 * sizeof(int))];
    struct iovec iov = { buf, n };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    int r = recvmsg(sock, &m, MSG_CMSG_CLOEXEC);
    *fd = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof *fd);
    return r;
}

int main(void)
{
    uint64_t v;
    int st;

    /* eventfd */
    int efd = eventfd(3, EFD_NONBLOCK | EFD_CLOEXEC);
    v = 4; write(efd, &v, 8);
    CHECK(read(efd, &v, 8) == 8 && v == 7, "eventfd counter %llu", (unsigned long long)v);
    CHECK(read(efd, &v, 8) == -1 && errno == EAGAIN, "eventfd empty: EAGAIN");
    int sem = eventfd(2, EFD_SEMAPHORE);
    read(sem, &v, 8);
    CHECK(v == 1 && read(sem, &v, 8) == 8 && v == 1, "eventfd semaphore");
    close(sem);

    /* timerfd */
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
    struct itimerspec its = { { 0, 50000000 }, { 0, 50000000 } };
    double t0 = now();
    CHECK(timerfd_settime(tfd, 0, &its, 0) == 0, "timerfd_settime");
    read(tfd, &v, 8);
    double dt = now() - t0;
    CHECK(v >= 1 && dt > 0.04 && dt < 0.5, "timerfd first expiry after %.3f s", dt);
    struct timespec ts = { 0, 160000000 };
    nanosleep(&ts, 0);
    read(tfd, &v, 8);
    CHECK(v >= 2 && v <= 5, "timerfd periodic: %llu expirations in 160 ms", (unsigned long long)v);

    /* poll */
    int p[2];
    pipe2(p, O_CLOEXEC);
    struct pollfd pf = { p[0], POLLIN, 0 };
    t0 = now();
    int r = poll(&pf, 1, 100);
    dt = now() - t0;
    CHECK(r == 0 && dt > 0.09 && dt < 0.5, "poll timeout after %.3f s", dt);
    write(p[1], "x", 1);
    CHECK(poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN), "poll: pipe readable");

    /* epoll */
    int ep = epoll_create1(EPOLL_CLOEXEC);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0x1111 };
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev);
    ev.data.u64 = 0x2222; epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev);
    ev.data.u64 = 0x3333; epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev);
    its.it_interval.tv_nsec = 0; its.it_value.tv_nsec = 0;
    timerfd_settime(tfd, 0, &its, 0);                     /* disarm */
    struct epoll_event out[8];
    int n = epoll_wait(ep, out, 8, 0);
    CHECK(n == 1 && out[0].data.u64 == 0x1111, "epoll: one ready (pipe), %d", n);
    char c; read(p[0], &c, 1);
    its.it_interval.tv_nsec = 0; its.it_value.tv_nsec = 30000000;
    timerfd_settime(tfd, 0, &its, 0);
    n = epoll_wait(ep, out, 8, 1000);
    CHECK(n == 1 && out[0].data.u64 == 0x3333, "epoll: wakes for the timer");
    read(tfd, &v, 8);
    v = 1; write(efd, &v, 8);
    n = epoll_wait(ep, out, 8, 1000);
    CHECK(n == 1 && out[0].data.u64 == 0x2222, "epoll: eventfd");
    read(efd, &v, 8);
    CHECK(epoll_wait(ep, out, 8, 50) == 0, "epoll: timeout");
    epoll_ctl(ep, EPOLL_CTL_DEL, efd, 0);

    /* signal handler, siginfo */
    struct sigaction sa = { 0 };
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    volatile double keep = 2.5;
    kill(getpid(), SIGUSR1);
    CHECK(got_sig == SIGUSR1 && got_pid == getpid() && keep == 2.5, "handler ran: sig %d from pid %d", got_sig, got_pid);

    /* SA_RESTART: read restarted after the handler */
    struct sigaction s2 = { 0 };
    s2.sa_handler = on_usr2;
    s2.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &s2, 0);
    got_sig = 0;
    pid_t pid = fork();
    if (pid == 0) {
        struct timespec d = { 0, 30000000 };
        nanosleep(&d, 0);
        kill(getppid(), SIGUSR2);
        nanosleep(&d, 0);
        write(p[1], "R", 1);
        _exit(0);
    }
    r = read(p[0], &c, 1);
    CHECK(r == 1 && c == 'R' && got_sig == SIGUSR2, "SA_RESTART read: %d '%c'", r, r == 1 ? c : '?');
    waitpid(pid, &st, 0);
    /* without SA_RESTART: EINTR */
    s2.sa_flags = 0;
    sigaction(SIGUSR2, &s2, 0);
    pid = fork();
    if (pid == 0) {
        struct timespec d = { 0, 30000000 };
        nanosleep(&d, 0);
        kill(getppid(), SIGUSR2);
        _exit(0);
    }
    r = read(p[0], &c, 1);
    CHECK(r == -1 && errno == EINTR, "no SA_RESTART: read -> EINTR");
    waitpid(pid, &st, 0);

    /* signalfd for SIGCHLD */
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, 0);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);
    pid = fork();
    if (pid == 0) _exit(3);
    struct signalfd_siginfo si;
    r = read(sfd, &si, sizeof si);
    CHECK(r == sizeof si && si.ssi_signo == SIGCHLD && (pid_t)si.ssi_pid == pid,
          "signalfd: SIGCHLD from pid %u", si.ssi_pid);
    waitpid(pid, &st, 0);
    CHECK(WEXITSTATUS(st) == 3, "child status after signalfd");
    sigprocmask(SIG_UNBLOCK, &mask, 0);

    /* SIGPIPE: default kills, ignored gives EPIPE */
    close(p[0]);
    pid = fork();
    if (pid == 0) { write(p[1], "x", 1); _exit(0); }
    waitpid(pid, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE, "SIGPIPE kills the writer");
    signal(SIGPIPE, SIG_IGN);
    CHECK(write(p[1], "x", 1) == -1 && errno == EPIPE, "SIGPIPE ignored: EPIPE");
    close(p[1]);

    /* memfd + MAP_SHARED across fork */
    int mfd = memfd_create("pool", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    CHECK(mfd >= 0 && ftruncate(mfd, 65536) == 0, "memfd_create + ftruncate");
    uint32_t *sh = mmap(0, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    CHECK(sh != MAP_FAILED, "mmap MAP_SHARED");
    int *anon = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pid = fork();
    if (pid == 0) {
        for (int i = 0; i < 16384; i++) sh[i] = i * 3u;
        anon[0] = 4242;
        _exit(0);
    }
    waitpid(pid, &st, 0);
    int okm = 1;
    for (int i = 0; i < 16384; i++) if (sh[i] != i * 3u) { okm = 0; break; }
    CHECK(okm && anon[0] == 4242, "child's writes visible (memfd and shared anonymous)");
    struct stat sst;
    fstat(mfd, &sst);
    CHECK(sst.st_size == 65536, "fstat memfd size %lld", (long long)sst.st_size);
    uint32_t w;
    CHECK(pread(mfd, &w, 4, 4 * 100) == 4 && w == 300, "pread memfd");

    /* socketpair + SCM_RIGHTS: child sends a memfd it filled */
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0, "socketpair");
    pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int f = memfd_create("child", 0);
        ftruncate(f, 4096);
        char *m = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
        strcpy(m, "hello through a passed fd");
        send_fd(sv[1], f, "fd!");
        char ack;
        read(sv[1], &ack, 1);
        _exit(0);
    }
    close(sv[1]);
    char buf[64] = { 0 };
    int rfd;
    r = recv_fd(sv[0], buf, sizeof buf, &rfd);
    CHECK(r == 3 && !memcmp(buf, "fd!", 3) && rfd >= 0, "recvmsg: data + fd %d", rfd);
    if (rfd >= 0) {
        char *m = mmap(0, 4096, PROT_READ, MAP_SHARED, rfd, 0);
        CHECK(m != MAP_FAILED && !strcmp(m, "hello through a passed fd"), "passed memfd contents: %s",
              m != MAP_FAILED ? m : "?");
        CHECK(fcntl(rfd, F_GETFD) == FD_CLOEXEC, "MSG_CMSG_CLOEXEC");
    }
    write(sv[0], "k", 1);
    waitpid(pid, &st, 0);
    CHECK(read(sv[0], buf, 1) == 0, "peer closed: EOF");
    close(sv[0]);

    /* named socket in /tmp: listen / connect / accept, SO_PEERCRED */
    mkdir("/tmp/run", 0700);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strcpy(a.sun_path, "/tmp/run/wayland-0");
    CHECK(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0 && listen(lfd, 4) == 0, "bind + listen");
    int lock = open("/tmp/run/wayland-0.lock", O_CREAT | O_RDWR | O_CLOEXEC, 0640);
    CHECK(lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0, "lock file in /tmp + flock");
    pid = fork();
    if (pid == 0) {
        int c = socket(AF_UNIX, SOCK_STREAM, 0);
        if (connect(c, (struct sockaddr *)&a, sizeof a)) _exit(1);
        write(c, "ping", 4);
        char b[8] = { 0 };
        if (read(c, b, 4) != 4 || strcmp(b, "pong")) _exit(2);
        _exit(0);
    }
    struct pollfd lp = { lfd, POLLIN, 0 };
    CHECK(poll(&lp, 1, 2000) == 1, "poll: listener readable on connect");
    int cfd = accept4(lfd, 0, 0, SOCK_CLOEXEC);
    struct ucred cr;
    socklen_t cl = sizeof cr;
    CHECK(cfd >= 0 && getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 && cr.pid == pid,
          "accept, SO_PEERCRED pid %d", cr.pid);
    char b4[8] = { 0 };
    CHECK(read(cfd, b4, 4) == 4 && !strcmp(b4, "ping"), "server got %s", b4);
    write(cfd, "pong", 4);
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "client exited %d", WEXITSTATUS(st));
    close(cfd); close(lfd); close(lock);
    unlink("/tmp/run/wayland-0.lock");
    a.sun_path[5] = 0;

    /* /tmp files, /dev */
    int tf = open("/tmp/note.txt", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    write(tf, "ramfs works\n", 12);
    close(tf);
    tf = open("/tmp/note.txt", O_RDONLY);
    char nb[32] = { 0 };
    CHECK(read(tf, nb, sizeof nb) == 12 && !strcmp(nb, "ramfs works\n"), "/tmp file round trip");
    close(tf);
    CHECK(truncate("/tmp/note.txt", 5) == 0 && stat("/tmp/note.txt", &sst) == 0 && sst.st_size == 5,
          "truncate in /tmp");
    CHECK(unlink("/tmp/note.txt") == 0 && rmdir("/tmp/run") == 0, "unlink + rmdir in /tmp");
    int dn = open("/dev/null", O_RDWR), du = open("/dev/urandom", O_RDONLY);
    uint64_t rv = 0;
    CHECK(write(dn, "x", 1) == 1 && read(dn, nb, 4) == 0 && read(du, &rv, 8) == 8 && rv,
          "/dev/null, /dev/urandom");
    close(dn); close(du);

    close(ep); close(efd); close(tfd); close(sfd); close(mfd);
    printf("ipc: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
