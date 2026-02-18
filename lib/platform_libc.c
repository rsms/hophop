#include <sl-prelude.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sl_main(void);

int main(void) {
    return sl_main();
}

__sl_i64 sl_platform_call(
    __sl_u64 op,
    __sl_u64 a,
    __sl_u64 b,
    __sl_u64 c,
    __sl_u64 d,
    __sl_u64 e,
    __sl_u64 f,
    __sl_u64 g) {
    (void)f;
    (void)g;
    switch ((enum SLPlatformOps)op) {
        case SLPlatformOp_NONE:  return 0;
        case SLPlatformOp_PANIC: {
            size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
            fprintf(stderr, "panic: %.*s\n", (int)n, (const char*)(uintptr_t)a);
            fflush(stderr);
            abort();
        }
        case SLPlatformOp_CONSOLE_LOG: {
            size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
            FILE*  out = (c & 1u) ? stderr : stdout;
            fprintf(out, "%.*s\n", (int)n, (const char*)(uintptr_t)a);
            fflush(out);
            return 0;
        }
        case SLPlatformOp_MEM_ALLOC:
            (void)b;
            (void)c;
            return (__sl_i64)(uintptr_t)(a ? malloc((size_t)a) : (void*)0);
        case SLPlatformOp_MEM_RESIZE:
            (void)b;
            (void)d;
            (void)e;
            return (__sl_i64)(uintptr_t)(c ? realloc((void*)(uintptr_t)a, (size_t)c) : (void*)0);
        case SLPlatformOp_MEM_FREE:
            (void)b;
            (void)c;
            free((void*)(uintptr_t)a);
            return 0;
    }
    // note: intentionally no 'default' case so that the compiler can warn us if we forgot an op
    return -1;
}
