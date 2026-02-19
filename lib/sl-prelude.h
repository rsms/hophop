#pragma once

#include <stddef.h>
#include <stdint.h>

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

typedef struct __sl_MemAllocator __sl_MemAllocator;
struct __sl_MemAllocator {
    void* (*alloc)(__sl_MemAllocator* ma, __sl_uint size, __sl_uint align);
};

static inline void* __sl_new(__sl_MemAllocator* ma, __sl_uint size, __sl_uint align) {
    return (ma != (__sl_MemAllocator*)0 && ma->alloc != (void* (*)(void*, __sl_uint, __sl_uint))0)
             ? ma->alloc(ma, size, align)
             : (void*)0;
}

static inline void* __sl_new_array(
    __sl_MemAllocator* ma, __sl_uint elemSize, __sl_uint elemAlign, __sl_uint count) {
    return __sl_new(ma, elemSize * count, elemAlign);
}

static inline __sl_uint __sl_len(__sl_str s) {
    return s.len;
}

static inline const __sl_u8* __sl_cstr(__sl_str s) {
    return (const __sl_u8*)s.ptr;
}

static inline __sl_uint __sl_align_up(__sl_uint x, __sl_uint a) {
    return (x + (a - 1u)) & ~(a - 1u);
}

enum __sl_PlatformOps {
    __sl_PlatformOp_NONE = 0,
    __sl_PlatformOp_PANIC = 1,
    __sl_PlatformOp_CONSOLE_LOG = 2,
    __sl_PlatformOp_MEM_ALLOC = 3,
    __sl_PlatformOp_MEM_RESIZE = 4,
    __sl_PlatformOp_MEM_FREE = 5,
    __sl_PlatformOp_EXIT = 6
};

extern __sl_i64 __sl_platform_call(
    __sl_u64 op,
    __sl_u64 a,
    __sl_u64 b,
    __sl_u64 c,
    __sl_u64 d,
    __sl_u64 e,
    __sl_u64 f,
    __sl_u64 g);

#ifndef __sl_trap
    #if defined(__clang__) || defined(__GNUC__)
        #define __sl_trap() __builtin_trap()
    #else
static inline void __sl_trap(void) {
    *(volatile int*)0 = 0;
}
    #endif
#endif

static inline void __sl_panic(const char* file, __sl_u32 line, __sl_str msg) {
    (void)file;
    (void)line;
    (void)__sl_platform_call(
        __sl_PlatformOp_PANIC, (__sl_u64)(uintptr_t)msg.ptr, (__sl_u64)msg.len, 0, 0, 0, 0, 0);
    __sl_trap();
}

static inline void __sl_assert_fail(const char* file, __sl_u32 line, const char* msg) {
    (void)file;
    (void)line;
    (void)__sl_platform_call(__sl_PlatformOp_PANIC, (__sl_u64)(uintptr_t)msg, 0, 0, 0, 0, 0, 0);
    __sl_trap();
}

static inline void __sl_assertf_fail(const char* file, __sl_u32 line, const char* fmt, ...) {
    __sl_assert_fail(file, line, fmt);
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

static inline void* __sl_unwrap(const void* p) {
    if (p != (const void*)0) {
        return (void*)p;
    }
    (void)__sl_platform_call(
        __sl_PlatformOp_PANIC, (__sl_u64)(uintptr_t)"unwrap: null value", 18u, 0, 0, 0, 0, 0);
    __sl_trap();
    return (void*)p;
}
