/* userland/tests/dyn/libdyn2.c - loaded by dyntest with dlopen; calls back
 * into libdyn (its dependency). */
int dyn_add(int a, int b);
int dyn2_mul(int a, int b) { return a * b; }
int dyn2_add_via(int a, int b) { return dyn_add(a, b); }
