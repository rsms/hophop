#pragma once

#include <stddef.h>
#include <stdint.h>

#ifndef __has_builtin
    #define __has_builtin(...) 0
#endif

#if defined(__GNUC__) || defined(__clang__) || defined(SL_HAS_BUILTIN_EXPECT)
    #define __sl_expect(x, expected_value) __builtin_expect((x), (expected_value))
#else
    #define __sl_expect(x, expected_value) (x)
#endif

#define __sl_unlikely(x) (__sl_expect(!!(x), 0))

#if __STDC_VERSION__ >= 202000L
    // _Noreturn deprecated in C23, replaced by [[noreturn]]
    #define __sl_noreturn [[noreturn]]
#else
    #define __sl_noreturn _Noreturn
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////
// types

typedef uint8_t   __sl_u8;
typedef uint16_t  __sl_u16;
typedef uint32_t  __sl_u32;
typedef uint64_t  __sl_u64;
typedef int8_t    __sl_i8;
typedef int16_t   __sl_i16;
typedef int32_t   __sl_i32;
typedef int64_t   __sl_i64;
typedef size_t    __sl_uint;
typedef ptrdiff_t __sl_int;
typedef float     __sl_f32;
typedef double    __sl_f64;
typedef _Bool     __sl_bool;

typedef struct {
    const void* ptr;
    __sl_uint   len;
} __sl_slice_ro;

typedef struct {
    void*     ptr;
    __sl_uint len;
    __sl_uint cap;
} __sl_slice_mut;

typedef __sl_slice_ro __sl_str;

typedef struct __sl_mem_Allocator __sl_mem_Allocator;
struct __sl_mem_Allocator {
    void* (*impl)(
        __sl_mem_Allocator* self,
        __sl_uint           addr,
        __sl_uint           align,
        __sl_uint           curSize,
        __sl_uint*          newSizeInOut,
        __sl_u32            flags);
};

typedef struct {
    __sl_mem_Allocator* mem;
    __sl_u64            console;
} __sl_MainContext;

enum __sl_PlatformOps {
    __sl_PlatformOp_NONE = 0,
    __sl_PlatformOp_PANIC = 1,
    __sl_PlatformOp_CONSOLE_LOG = 2,
    __sl_PlatformOp_EXIT = 3
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// functions

extern __sl_i64 __sl_platform_call(
    __sl_u64 op,
    __sl_u64 a,
    __sl_u64 b,
    __sl_u64 c,
    __sl_u64 d,
    __sl_u64 e,
    __sl_u64 f,
    __sl_u64 g);

#if !defined(__sl_trap) && __has_builtin(__builtin_trap)
    #define __sl_trap() __builtin_trap()
#elif !defined(__sl_trap)
static inline void __sl_trap(void) {
    *(volatile int*)0 = 0;
}
#endif

static inline __sl_noreturn void __sl_panic1(
    const char* file, __sl_u32 line, const char* msg, __sl_uint msgLen) {
    (void)__sl_platform_call(
        __sl_PlatformOp_PANIC,
        (__sl_u64)(uintptr_t)msg,
        (__sl_u64)msgLen,
        (__sl_u64)(uintptr_t)file,
        (__sl_u64)line,
        0,
        0,
        0);
    __sl_trap();
}

#define __sl_debugpanic1 __sl_panic1
// #define __sl_debugpanic1(...) __sl_trap()

static inline void __sl_panic(const char* file, __sl_u32 line, __sl_str msg) {
    __sl_panic1(file, line, (const char*)msg.ptr, msg.len);
}

#define __sl_strlit_tuple(strlit) strlit, sizeof((char[]){ strlit })

#define __sl_assert(expr)                                                           \
    do {                                                                            \
        if __sl_unlikely (!(expr)) {                                                \
            __sl_panic1(__FILE__, __LINE__, __sl_strlit_tuple("assertion failed")); \
        }                                                                           \
    } while (0)

#define __sl_assertf(expr, fmt_cstr_lit, ...)                                 \
    do {                                                                      \
        if __sl_unlikely (!(expr)) {                                          \
            __sl_panic1(__FILE__, __LINE__, __sl_strlit_tuple(fmt_cstr_lit)); \
        }                                                                     \
    } while (0)

static inline __sl_uint __sl_len(__sl_str s) {
    return s.len;
}

static inline const __sl_u8* __sl_cstr(__sl_str s) {
    return (const __sl_u8*)s.ptr;
}

static inline __sl_uint __sl_align_up(__sl_uint x, __sl_uint a) {
    return (x + (a - 1u)) & ~(a - 1u);
}

static inline void __sl_console_log(__sl_str msg, __sl_u64 flags) {
    (void)__sl_platform_call(
        __sl_PlatformOp_CONSOLE_LOG,
        (__sl_u64)(uintptr_t)msg.ptr,
        (__sl_u64)msg.len,
        flags,
        0,
        0,
        0,
        0);
}

static inline void* __sl_unwrap1(const char* file, __sl_u32 line, const void* p) {
    if __sl_unlikely (p == NULL) {
        __sl_panic1(file, line, __sl_strlit_tuple("unwrap: null value"));
    }
    return (void*)p;
}

#define __sl_unwrap(p) __sl_unwrap1(__FILE__, __LINE__, (p))

static inline void* __sl_new(__sl_mem_Allocator* ma, __sl_uint size, __sl_uint align) {
    __sl_uint newSize = size;

    if __sl_unlikely (align == 0 || (align & (align - 1u)) != 0) {
        __sl_debugpanic1("", 0, __sl_strlit_tuple("invalid alignment"));
    }

    // TODO FIXME: make ma or ma->impl being NULL an error. For now we return NULL in that case,
    // as if allocation failed, since several tests makes this assumption.
    if __sl_unlikely (ma == NULL || ma->impl == NULL) {
        // __sl_debugpanic1("", 0, __sl_strlit_tuple("ma.impl is null"));
        return NULL;
    }

    return ma->impl(ma, 0, align, 0, &newSize, 0);
}

static inline void* __sl_new_array(
    __sl_mem_Allocator* ma, __sl_uint elemSize, __sl_uint elemAlign, __sl_uint count) {
    return __sl_new(ma, elemSize * count, elemAlign);
}

static inline __sl_slice_ro __sl_new_array_slice_ro(
    __sl_mem_Allocator* ma, __sl_uint elemSize, __sl_uint elemAlign, __sl_uint count) {
    void* p = __sl_new_array(ma, elemSize, elemAlign, count);
    return (__sl_slice_ro){ .ptr = p, .len = p != NULL ? count : 0u };
}

static inline __sl_slice_mut __sl_new_array_slice_mut(
    __sl_mem_Allocator* ma, __sl_uint elemSize, __sl_uint elemAlign, __sl_uint count) {
    void* p = __sl_new_array(ma, elemSize, elemAlign, count);
    return (
        __sl_slice_mut){ .ptr = p, .len = p != NULL ? count : 0u, .cap = p != NULL ? count : 0u };
}
