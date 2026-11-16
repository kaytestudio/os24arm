/* userland/tests/dyn/libdyn.c - a shared library dyntest links with: a
 * constructor, data the program sees, TLS. */
static int inited;
__thread int dyn_tls = 7;
int dyn_counter = 40;
__attribute__((constructor)) static void init(void) { inited = 1; }
int dyn_inited(void) { return inited; }
int dyn_add(int a, int b) { return a + b + (dyn_counter++ - 40); }
int *dyn_tls_addr(void) { return &dyn_tls; }
