#include <core/core.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNLIKELY   __sl_unlikely
#define panic(msg_lit) __sl_panic(__sl_strlit(msg_lit), __FILE__, __LINE__)

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

static void platform_log_handler(
    __sl_Logger*  self,
    __sl_str*     message,
    __sl_LogLevel level,
    __sl_LogFlags flags) {
    size_t n = message != NULL ? (size_t)message->len : 0u;
    FILE*  out = (level >= __sl_LogLevel_Error) ? stderr : stdout;

    if (self != NULL && level < self->min_level) {
        return;
    }

    if (self != NULL && (flags & __sl_LoggerFlag_Prefix) && self->prefix != NULL &&
        self->prefix->len > 0) {
        fprintf(out, "%.*s", (int)self->prefix->len, (const char*)self->prefix->bytes);
    }
    if (message != NULL) {
        fprintf(out, "%.*s", (int)n, (const char*)message->bytes);
    }
    fputc('\n', out);
    fflush(out);
}

typedef struct core__PrintContext {
    __sl_Logger log;
} core__PrintContext;

void core__print(core__PrintContext* context, __sl_str* message) {
    context->log.handler(&context->log, message, __sl_LogLevel_Info, (__sl_LogFlags)0);
}

__sl_noreturn void __sl_panic(const __sl_str* msg, const char* file, __sl_u32 line) {
    __sl_u32      message_len;
    const __sl_u8 empty[] = "";
    (void)file;
    (void)line;

    if (msg == NULL) {
        msg = __sl_strlit("panic: null message");
    }
    message_len = msg->len;

    if (message_len > 0x7FFFFFFFu) {
        message_len = 0x7FFFFFFFu;
    }
    fprintf(
        stderr,
        "panic: %.*s\n",
        (int)message_len,
        message_len > 0 ? (const char*)msg->bytes : (const char*)empty);
    fflush(stderr);
    abort();
}

__sl_noreturn void platform__exit(__sl_i32 status) {
    exit(status);
}

static __sl_Allocator gAllocator = { .impl = platform_mem_allocator_impl };
static __sl_Context   gMainContext = {
    .mem = &gAllocator,
    .temp_mem = &gAllocator,
    .log =
        {
            .handler = platform_log_handler,
            .min_level = __sl_LogLevel_Info,
            .flags = __sl_LoggerFlag_Level,
            .prefix = NULL,
        },
};

#ifndef SL_PLATFORM_NO_MAIN
int main(void) {
    return sl_main(&gMainContext);
}
#endif
