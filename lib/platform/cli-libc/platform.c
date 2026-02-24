#include <core/core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNLIKELY   __sl_unlikely
#define panic(msg) __sl_panic1(__FILE__, __LINE__, (msg), strlen(msg))

#ifndef NDEBUG
    #define debugassert(cond) (UNLIKELY(!(cond)) ? panic("Assertion failure: " #cond) : ((void)0))
#else
    #define debugassert(cond) ((void)0)
#endif

extern int sl_main(__sl_Context* context);

#define MALLOC_ALIGN ((size_t)_Alignof(max_align_t))

static __sl_uint platform_mem_allocator_impl(
    __sl_Allocator* self,
    __sl_uint       addr,
    __sl_uint       align,
    __sl_uint       curSize,
    __sl_uint*      newSizeInOut,
    __sl_u32        flags) {

    void* newPtr = NULL;
    (void)self;  // unused
    (void)flags; // unused

    debugassert(newSizeInOut != NULL);

    if (*newSizeInOut == 0) {
        free((void*)(uintptr_t)addr);
        return 0;
    }

    if UNLIKELY (align == 0 || (align & (align - 1u)) != 0u) {
        panic("invalid alignment");
    }

    if (align > MALLOC_ALIGN) {
        __sl_uint alignedSize = __sl_align_up(*newSizeInOut, align);
        newPtr = aligned_alloc((size_t)align, (size_t)alignedSize);
        if (addr == 0 && newPtr != NULL) {
            // resize case
            __sl_uint copySize = curSize < *newSizeInOut ? curSize : *newSizeInOut;
            if (copySize > 0) {
                memcpy(newPtr, (void*)(uintptr_t)addr, (size_t)copySize);
            }
            free((void*)(uintptr_t)addr);
        }
    } else {
        if (addr == 0) {
            newPtr = malloc((size_t)*newSizeInOut);
        } else {
            newPtr = realloc((void*)(uintptr_t)addr, (size_t)*newSizeInOut);
        }
    }

    // zero new memory
    if (newPtr != NULL && *newSizeInOut > curSize) {
        memset((unsigned char*)newPtr + curSize, 0, (size_t)(*newSizeInOut - curSize));
    }

    return (__sl_uint)(uintptr_t)newPtr;
}

static void platform_log_handler(__sl_Logger* self, __sl_str* message, __sl_LogLevel level) {
    size_t n = message != NULL ? (size_t)message->len : 0u;
    FILE*  out = (level >= __sl_LogLevel_Error) ? stderr : stdout;

    if (self != NULL && level < self->min_level) {
        return;
    }

    if (self != NULL && self->prefix != NULL && self->prefix->len > 0) {
        fprintf(out, "%.*s", (int)self->prefix->len, (const char*)self->prefix->bytes);
    }
    if (message != NULL) {
        fprintf(out, "%.*s", (int)n, (const char*)message->bytes);
    }
    fputc('\n', out);
    fflush(out);
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
    (void)c;
    switch ((enum __sl_PlatformOps)op) {
        case __sl_PlatformOp_NONE:  return 0;
        case __sl_PlatformOp_PANIC: {
            size_t n = b ? (size_t)b : strlen((const char*)(uintptr_t)a);
            fprintf(stderr, "panic: %.*s\n", (int)n, (const char*)(uintptr_t)a);
            fflush(stderr);
            abort();
        }
        case __sl_PlatformOp_EXIT: exit((int)(__sl_i64)a); return 0;
    }
    // note: intentionally no 'default' case so that the compiler can warn us if we forgot an op
    return -1;
}

int main(void) {
    static __sl_Allocator gAllocator = { .impl = platform_mem_allocator_impl };
    __sl_Context          gMainContext = {
        .mem = &gAllocator,
        .log =
            {
                .handler = platform_log_handler,
                .min_level = __sl_LogLevel_Info,
                .flags = __sl_LoggerFlag_Level,
                .prefix = NULL,
            },
    };
    return sl_main(&gMainContext);
}
