/* cli-libc platform for SL programs.
 *
 * Provides:
 *   - int main(void)              — calls sl_main()
 *   - int64_t sl_platform_call()  — routes platform ops through libc
 *
 * This file is compiled alongside the generated SL package source when using
 * `slc compile` or `slc run`. It can also be compiled manually:
 *
 *   cc -std=c11 your_pkg.c platforms/cli-libc/platform.c -o out
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Implemented by the compiled SL program. */
extern int sl_main(void);

int main(void) {
    return sl_main();
}

int64_t sl_platform_call(uint64_t op,
                         uint64_t a, uint64_t b, uint64_t c,
                         uint64_t d, uint64_t e, uint64_t f, uint64_t g) {
    (void)f;
    (void)g;
    switch (op) {
    case 1: { /* SLPlatformOp_PANIC: a=msg_ptr (cstr), b=msg_len (0=strlen), c=flags */
        size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
        fprintf(stderr, "panic: %.*s\n", (int)n, (const char*)(uintptr_t)a);
        fflush(stderr);
        abort();
    }
    case 2: { /* SLPlatformOp_CONSOLE_LOG: a=msg_ptr, b=msg_len, c=flags (0=stdout,1=stderr) */
        size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
        FILE*  out = (c & 1u) ? stderr : stdout;
        fprintf(out, "%.*s\n", (int)n, (const char*)(uintptr_t)a);
        fflush(out);
        return 0;
    }
    case 3: /* SLPlatformOp_MEM_ALLOC: a=size, b=align, c=flags */
        (void)b;
        (void)c;
        return (int64_t)(uintptr_t)(a ? malloc((size_t)a) : (void*)0);
    case 4: /* SLPlatformOp_MEM_RESIZE: a=addr, b=oldSize, c=newSize, d=align, e=flags */
        (void)b;
        (void)d;
        (void)e;
        return (int64_t)(uintptr_t)(
            c ? realloc((void*)(uintptr_t)a, (size_t)c) : (void*)0);
    case 5: /* SLPlatformOp_MEM_FREE: a=addr, b=size, c=flags */
        (void)b;
        (void)c;
        free((void*)(uintptr_t)a);
        return 0;
    default:
        return -1;
    }
}
