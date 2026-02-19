#include <sl-prelude.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int sl_main(void);
#if defined(__clang__) || defined(__GNUC__)
__attribute__((weak))
#endif
__sl_mem_Allocator* mem__platformAllocator;

#define MALLOC_ALIGN ((size_t)_Alignof(max_align_t))

static void* platform_mem_allocator_impl(
    __sl_mem_Allocator* self,
    __sl_uint           addr,
    __sl_uint           align,
    __sl_uint           curSize,
    __sl_uint*          newSizeInOut,
    __sl_u32            flags) {
    void* newPtr = NULL;
    (void)self;
    (void)flags;

    if (newSizeInOut == NULL) {
        return NULL;
    }
    if (*newSizeInOut == 0) {
        free((void*)(uintptr_t)addr);
        return NULL;
    }
    if (align == 0 || (align & (align - 1u)) != 0u) {
        abort();
    }

    if (addr == 0) {
        if (align > MALLOC_ALIGN) {
            __sl_uint alignedSize = __sl_align_up(*newSizeInOut, align);
            newPtr = aligned_alloc((size_t)align, (size_t)alignedSize);
        } else {
            newPtr = malloc((size_t)*newSizeInOut);
        }
    } else if (align > MALLOC_ALIGN) {
        __sl_uint alignedSize = __sl_align_up(*newSizeInOut, align);
        newPtr = aligned_alloc((size_t)align, (size_t)alignedSize);
        if (newPtr != NULL) {
            __sl_uint copySize = curSize < *newSizeInOut ? curSize : *newSizeInOut;
            if (copySize > 0) {
                memcpy(newPtr, (void*)(uintptr_t)addr, (size_t)copySize);
            }
            free((void*)(uintptr_t)addr);
        }
    } else {
        newPtr = realloc((void*)(uintptr_t)addr, (size_t)*newSizeInOut);
    }

    if (newPtr != NULL && *newSizeInOut > curSize) {
        memset((unsigned char*)newPtr + curSize, 0, (size_t)(*newSizeInOut - curSize));
    }
    return newPtr;
}

__sl_i64 __sl_platform_call(
    __sl_u64 op,
    __sl_u64 a,
    __sl_u64 b,
    __sl_u64 c,
    __sl_u64 d,
    __sl_u64 e,
    __sl_u64 f,
    __sl_u64 g) {
    (void)d;
    (void)e;
    (void)f;
    (void)g;
    switch ((enum __sl_PlatformOps)op) {
        case __sl_PlatformOp_NONE:  return 0;
        case __sl_PlatformOp_PANIC: {
            size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
            fprintf(stderr, "panic: %.*s\n", (int)n, (const char*)(uintptr_t)a);
            fflush(stderr);
            abort();
        }
        case __sl_PlatformOp_CONSOLE_LOG: {
            size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
            FILE*  out = (c & 1u) ? stderr : stdout;
            fprintf(out, "%.*s\n", (int)n, (const char*)(uintptr_t)a);
            fflush(out);
            return 0;
        }
        case __sl_PlatformOp_EXIT: exit((int)(__sl_i64)a); return 0;
    }
    // note: intentionally no 'default' case so that the compiler can warn us if we forgot an op
    return -1;
}

int main(void) {
    static __sl_mem_Allocator gAllocator = { .impl = platform_mem_allocator_impl };
    mem__platformAllocator = &gAllocator;
    return sl_main();
}
