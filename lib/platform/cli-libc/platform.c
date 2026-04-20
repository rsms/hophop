#include <builtin/builtin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNLIKELY   __sl_unlikely
#define panic(msg_lit) __sl_panic(__sl_strlitp(msg_lit), __FILE__, __LINE__)

#ifndef NDEBUG
    #define debugassert(cond) (UNLIKELY(!(cond)) ? panic("Assertion failure: " #cond) : ((void)0))
#else
    #define debugassert(cond) ((void)0)
#endif

extern int sl_main(__sl_Context* context);
static __sl_Context gMainContext;

#define MALLOC_ALIGN ((size_t)_Alignof(max_align_t))

static void* platform_mem_allocator_impl(
    __sl_Allocator* self,
    void*           addr,
    __sl_uint       align,
    __sl_uint       curSize,
    __sl_uint*      newSizeInOut,
    __sl_u32        flags) {

    void* newPtr = NULL;
    (void)self;  // unused
    (void)flags; // unused

    debugassert(newSizeInOut != NULL);

    if (*newSizeInOut == 0) {
        free(addr);
        return NULL;
    }

    if UNLIKELY (align == 0 || (align & (align - 1u)) != 0u) {
        panic("invalid alignment");
    }

    if (align > MALLOC_ALIGN) {
        __sl_uint alignedSize = __sl_align_up(*newSizeInOut, align);
        newPtr = aligned_alloc((size_t)align, (size_t)alignedSize);
        if (addr != NULL && newPtr != NULL) {
            // resize case
            __sl_uint copySize = curSize < *newSizeInOut ? curSize : *newSizeInOut;
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

    // zero new memory
    if (newPtr != NULL && *newSizeInOut > curSize) {
        memset((unsigned char*)newPtr + curSize, 0, (size_t)(*newSizeInOut - curSize));
    }

    return newPtr;
}

static void platform_log_handler(
    __sl_Logger*  self,
    __sl_str      message,
    __sl_LogLevel level,
    __sl_LogFlags flags) {
    size_t n = (size_t)message.len;
    FILE*  out = (level >= __sl_LogLevel_Error) ? stderr : stdout;

    if (self != NULL && level < self->min_level) {
        return;
    }

    if (self != NULL && (flags & __sl_LogFlag_Prefix) && self->prefix.len > 0) {
        fprintf(out, "%.*s", (int)self->prefix.len, (const char*)self->prefix.ptr);
    }
    if (n > 0u) {
        fprintf(out, "%.*s", (int)n, (const char*)message.ptr);
    }
    fputc('\n', out);
    fflush(out);
}

__sl_noreturn void __sl_panic(const __sl_str* msg, const char* file, __sl_u32 line) {
    __sl_uint     message_len;
    const __sl_u8 empty[] = "";
    (void)file;
    (void)line;

    if (msg == NULL) {
        msg = __sl_strlitp("panic: null message");
    }
    message_len = msg->len;

    if (message_len > 0x7FFFFFFFu) {
        message_len = 0x7FFFFFFFu;
    }
    fprintf(
        stderr,
        "panic: %.*s\n",
        (int)message_len,
        message_len > 0 ? (const char*)msg->ptr : (const char*)empty);
    fflush(stderr);
    abort();
}

__sl_noreturn void platform__panic(__sl_str message, __sl_i32 flags) {
    (void)flags;
    __sl_panic(&message, "", 0);
}

__sl_noreturn void platform__exit(__sl_i32 status) {
    exit(status);
}

void platform__console_log(__sl_str message, __sl_i32 flags) {
    platform_log_handler(&gMainContext.log, message, __sl_LogLevel_Info, (__sl_LogFlags)flags);
}

static __sl_Allocator gAllocator = { .impl = platform_mem_allocator_impl };
static __sl_Context   gMainContext = {
    .mem = &gAllocator,
    .temp_mem = &gAllocator,
    .log =
        {
            .handler = platform_log_handler,
            .min_level = __sl_LogLevel_Info,
            .flags = __sl_LogFlag_Level,
            .prefix = (__sl_str){ (__sl_u8*)(uintptr_t)0, 0 },
        },
};

#ifndef SL_PLATFORM_NO_MAIN
int main(void) {
    return sl_main(&gMainContext);
}
#endif
