/* procs.c - stage 2 test: fork, wait, exit status, separate address spaces,
 * pipes between processes, posix_spawn, execve, kill. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <sched.h>
#include <sys/wait.h>

extern char **environ;
static int fails;
#define CHECK(c, ...) do { if (c) printf("ok    " __VA_ARGS__); \
                           else { printf("FAIL  " __VA_ARGS__); fails++; } putchar('\n'); } while (0)

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child")) {        /* exec'd by the parent */
        printf("child: pid %d ppid %d argv[2]=%s\n", getpid(), getppid(), argc > 2 ? argv[2] : "-");
        return 42;
    }
    printf("procs: pid %d\n", getpid());

    /* fork: copy-of-memory semantics and exit status */
    static int shared = 1;
    char *heap = malloc(64);
    strcpy(heap, "parent");
    pid_t p = fork();
    if (p == 0) {
        shared = 2;
        strcpy(heap, "child");
        _exit(7);
    }
    int st = 0;
    pid_t w = waitpid(p, &st, 0);
    CHECK(w == p && WIFEXITED(st) && WEXITSTATUS(st) == 7, "fork + waitpid: status %d", WEXITSTATUS(st));
    CHECK(shared == 1 && !strcmp(heap, "parent"), "address spaces are separate");

    /* pipe: child writes, parent reads until EOF */
    int fd[2];
    CHECK(pipe(fd) == 0, "pipe");
    p = fork();
    if (p == 0) {
        close(fd[0]);
        for (int i = 0; i < 2000; i++) {                 /* > pipe buffer: blocks */
            char line[16];
            int n = snprintf(line, sizeof line, "%04d\n", i);
            if (write(fd[1], line, n) != n) _exit(1);
        }
        _exit(0);
    }
    close(fd[1]);
    char buf[512];
    long total = 0;
    int last = -1, order = 1;
    char part[8]; int pl = 0;
    for (ssize_t n; (n = read(fd[0], buf, sizeof buf)) > 0; ) {
        total += n;
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') { part[pl] = 0; int v = atoi(part); if (v != last + 1) order = 0; last = v; pl = 0; }
            else if (pl < 7) part[pl++] = buf[i];
        }
    }
    close(fd[0]);
    waitpid(p, &st, 0);
    CHECK(total == 10000 && order && last == 1999 && WEXITSTATUS(st) == 0,
          "pipe between processes: %ld bytes, in order", total);

    /* posix_spawn (vfork + execve) */
    char *args[] = { argv[0], "child", "spawned", 0 };
    int r = posix_spawn(&p, argv[0], 0, 0, args, environ);
    CHECK(r == 0, "posix_spawn %s: %d", argv[0], r);
    if (r == 0) {
        waitpid(p, &st, 0);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 42, "spawned child exit status %d", WEXITSTATUS(st));
    }

    /* fork + execve with a pipe on stdout */
    CHECK(pipe(fd) == 0, "pipe 2");
    p = fork();
    if (p == 0) {
        dup2(fd[1], 1);
        close(fd[0]); close(fd[1]);
        char *a[] = { argv[0], "child", "exec'd", 0 };
        execve(argv[0], a, environ);
        _exit(99);
    }
    close(fd[1]);
    ssize_t n = read(fd[0], buf, sizeof buf - 1);
    buf[n > 0 ? n : 0] = 0;
    close(fd[0]);
    waitpid(p, &st, 0);
    CHECK(strstr(buf, "argv[2]=exec'd") && WEXITSTATUS(st) == 42, "execve, output via pipe: %s", strtok(buf, "\n"));

    /* kill a child blocked in read */
    CHECK(pipe(fd) == 0, "pipe 3");
    p = fork();
    if (p == 0) { char c; read(fd[0], &c, 1); _exit(0); }
    sched_yield();
    CHECK(kill(p, SIGTERM) == 0, "kill");
    waitpid(p, &st, 0);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM, "child killed by signal %d", WTERMSIG(st));
    close(fd[0]); close(fd[1]);

    /* no children left */
    CHECK(waitpid(-1, &st, WNOHANG) == -1, "no children left");

    /* several at once */
    for (int i = 0; i < 5; i++) if ((p = fork()) == 0) _exit(10 + i);
    int sum = 0, cnt = 0;
    while ((w = wait(&st)) > 0) { sum += WEXITSTATUS(st); cnt++; }
    CHECK(cnt == 5 && sum == 60, "5 children, statuses sum %d", sum);

    printf("procs: %s (%d failures)\n", fails ? "FAILED" : "all passed", fails);
    return fails;
}
