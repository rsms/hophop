#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sl_main(void);

int main(void) {
    return sl_main();
}

int64_t sl_platform_call(
    uint64_t op,
    uint64_t a,
    uint64_t b,
    uint64_t c,
    uint64_t d,
    uint64_t e,
    uint64_t f,
    uint64_t g) {
    (void)f;
    (void)g;
    switch (op) {
        case 1: { /* PANIC */
            size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
            fprintf(stderr, "panic: %.*s\n", (int)n, (const char*)(uintptr_t)a);
            fflush(stderr);
            abort();
        }
        case 2: { /* CONSOLE_LOG */
            size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
            FILE*  out = (c & 1u) ? stderr : stdout;
            fprintf(out, "%.*s\n", (int)n, (const char*)(uintptr_t)a);
            fflush(out);
            return 0;
        }
        case 3: /* MEM_ALLOC */
            (void)b;
            (void)c;
            return (int64_t)(uintptr_t)(a ? malloc((size_t)a) : (void*)0);
        case 4: /* MEM_RESIZE */
            (void)b;
            (void)d;
            (void)e;
            return (int64_t)(uintptr_t)(c ? realloc((void*)(uintptr_t)a, (size_t)c) : (void*)0);
        case 5: /* MEM_FREE */
            (void)b;
            (void)c;
            free((void*)(uintptr_t)a);
            return 0;
        default: return -1;
    }
}
