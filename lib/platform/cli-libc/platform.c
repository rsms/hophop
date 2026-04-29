#include <builtin/builtin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNLIKELY   __hop_unlikely
#define panic(msg_lit) __hop_panic(__hop_strlitp(msg_lit), __FILE__, __LINE__)

#ifndef NDEBUG
    #define debugassert(cond) (UNLIKELY(!(cond)) ? panic("Assertion failure: " #cond) : ((void)0))
#else
    #define debugassert(cond) ((void)0)
#endif

extern int hop_main(__hop_Context* context);
static __hop_Context gMainContext;

#define MALLOC_ALIGN ((size_t)_Alignof(max_align_t))

static void* platform_mem_allocator_impl(
    __hop_Allocator* self,
    void*              addr,
    __hop_int           align,
    __hop_int           curSize,
    __hop_int*          newSizeInOut,
    __hop_u32           flags,
    __hop_SourceLocation srcLoc) {

    void* newPtr = NULL;
    (void)self;  // unused
    (void)flags; // unused
    (void)srcLoc;

    debugassert(newSizeInOut != NULL);

    if (*newSizeInOut < 0 || curSize < 0) {
        panic("negative allocation size");
    }

    if (*newSizeInOut == 0) {
        free(addr);
        return NULL;
    }

    if UNLIKELY (align <= 0 || (align & (align - 1)) != 0) {
        panic("invalid alignment");
    }

    if ((__hop_uint)align > MALLOC_ALIGN) {
        __hop_int alignedSize = __hop_align_up(*newSizeInOut, align);
        newPtr = aligned_alloc((size_t)align, (size_t)alignedSize);
        if (addr != NULL && newPtr != NULL) {
            // resize case
            __hop_int copySize = curSize < *newSizeInOut ? curSize : *newSizeInOut;
            if (copySize > 0) {
                memcpy(newPtr, addr, (size_t)copySize);
            }
            free(addr);
        }
    } else {
        if (addr == NULL) {
            newPtr = malloc((size_t)*newSizeInOut);
        } else {
            newPtr = realloc(addr, (size_t)*newSizeInOut);
        }
    }

    // zero allocated memory
    if (newPtr != NULL && *newSizeInOut > curSize) {
        memset((unsigned char*)newPtr + curSize, 0, (size_t)(*newSizeInOut - curSize));
    }

    return newPtr;
}

static void platform_log_handler(
    __hop_Logger*  self,
    __hop_str      message,
    __hop_LogLevel level,
    __hop_LogFlags flags) {
    __hop_int n = message.len;
    FILE*    out = (level >= __hop_LogLevel_Error) ? stderr : stdout;

    if (self != NULL && level < self->min_level) {
        return;
    }

    if (self != NULL && (flags & __hop_LogFlag_Prefix) && self->prefix.len > 0) {
        fprintf(out, "%.*s", (int)self->prefix.len, (const char*)self->prefix.ptr);
    }
    if (n < 0) {
        n = 0;
    }
    if (n > 0) {
        fprintf(out, "%.*s", (int)n, (const char*)message.ptr);
    }
    fputc('\n', out);
    fflush(out);
}

__hop_noreturn void __hop_panic(const __hop_str* msg, const char* file, __hop_u32 line) {
    __hop_int      message_len;
    const __hop_u8 empty[] = "";
    (void)file;
    (void)line;

    if (msg == NULL) {
        msg = __hop_strlitp("panic: null message");
    }
    message_len = msg->len;

    if (message_len < 0) {
        message_len = 0;
    }
    if (message_len > 0x7FFFFFFF) {
        message_len = 0x7FFFFFFF;
    }
    fprintf(
        stderr,
        "panic: %.*s\n",
        (int)message_len,
        message_len > 0 ? (const char*)msg->ptr : (const char*)empty);
    fflush(stderr);
    abort();
}

__hop_noreturn void platform__panic(__hop_str message, __hop_i32 flags) {
    (void)flags;
    __hop_panic(&message, "", 0);
}

__hop_noreturn void platform__exit(__hop_Context* context, __hop_i32 status) {
    (void)context;
    exit(status);
}

void platform__console_log(__hop_Context* context, __hop_str message, __hop_i32 flags) {
    __hop_Logger* logger = context != NULL ? &context->logger : &gMainContext.logger;
    platform_log_handler(logger, message, __hop_LogLevel_Info, (__hop_LogFlags)flags);
}

static __hop_Context   gMainContext = {
    .allocator = { .handler = platform_mem_allocator_impl },
    .temp_allocator = { .handler = platform_mem_allocator_impl },
    .logger =
        {
            .handler = platform_log_handler,
            .min_level = __hop_LogLevel_Info,
            .flags = __hop_LogFlag_Level,
            .prefix = (__hop_str){ (__hop_u8*)(uintptr_t)0, 0 },
        },
    .user1 = NULL,
    .user2 = NULL,
    ._reserved = NULL,
    .deadline = 0,
};

#ifndef HOP_PLATFORM_NO_MAIN
int main(void) {
    return hop_main(&gMainContext);
}
#endif
