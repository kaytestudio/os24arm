/*
 * llvm-launcher - one small program per LLVM tool name (clang, clang++,
 * cc, ld.lld, llvm-ar, ...): it runs the multi-call /usr/bin/llvm, which
 * picks the tool from argv[0] - what symlinks do elsewhere (this system's
 * file systems have none). Installed by llvm.spec.
 */
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    (void)argc;
    const char *self = argv[0] ? argv[0] : "clang";
    const char *base = strrchr(self, '/');
    argv[0] = (char *)(base ? base + 1 : self);
    execv("/usr/bin/llvm", argv);
    static const char msg[] = "llvm-launcher: /usr/bin/llvm is missing\n";
    write(2, msg, sizeof msg - 1);
    return 127;
}
