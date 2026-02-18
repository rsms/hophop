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
typedef struct { const void* ptr; __sl_uint len; } sl_slice_ro;
typedef struct { void* ptr; __sl_uint len; __sl_uint cap; } sl_slice_mut;
typedef struct MemAllocator MemAllocator;
struct MemAllocator {
    void* ctx;
    void* (*alloc)(void* ctx, __sl_uint size, __sl_uint align);
};
static inline void* sl_new(MemAllocator* ma, __sl_uint size, __sl_uint align) {
    return (ma != (MemAllocator*)0 && ma->alloc != (void*(*)(void*,__sl_uint,__sl_uint))0)
               ? ma->alloc(ma->ctx, size, align)
               : (void*)0;
}
static inline void* sl_new_array(
    MemAllocator* ma, __sl_uint elemSize, __sl_uint elemAlign, __sl_uint count) {
    return sl_new(ma, elemSize * count, elemAlign);
}
typedef const __sl_u8* __sl_str;
typedef struct { __sl_u32 len; __sl_u8 bytes[1]; } sl_strhdr;
static inline __sl_u32 len(__sl_str s) {
    return s == (__sl_str)0 ? 0u : ((const sl_strhdr*)(const void*)s)->len;
}
static inline const __sl_u8* cstr(__sl_str s) {
    return s == (__sl_str)0 ? (const __sl_u8*)0 : ((const sl_strhdr*)(const void*)s)->bytes;
}
static inline __sl_uint sl_align_up(__sl_uint x, __sl_uint a) {
    return (x + (a - 1u)) & ~(a - 1u);
}
#ifndef SL_PLATFORM_ABI
#define SL_PLATFORM_ABI
  enum {
    SLPlatformOp_NONE        = 0,
    SLPlatformOp_PANIC       = 1,
    SLPlatformOp_CONSOLE_LOG = 2,
    SLPlatformOp_MEM_ALLOC   = 3,
    SLPlatformOp_MEM_RESIZE  = 4,
    SLPlatformOp_MEM_FREE    = 5
  };
  extern __sl_i64 sl_platform_call(__sl_u64 op, __sl_u64 a, __sl_u64 b, __sl_u64 c,
                                   __sl_u64 d, __sl_u64 e, __sl_u64 f, __sl_u64 g);
#endif
#ifndef SL_TRAP
  #if defined(__clang__) || defined(__GNUC__)
    #define SL_TRAP() __builtin_trap()
  #else
    #define SL_TRAP() do { *(volatile int*)0 = 0; } while (0)
  #endif
#endif
#ifndef __sl_trap
  #define __sl_trap() SL_TRAP()
#endif
#ifndef __sl_panic
  #define __sl_panic(file,line,msg) \
    (sl_platform_call(SLPlatformOp_PANIC,(uint64_t)(uintptr_t)(msg),0,0,0,0,0,0), __sl_trap())
#endif
#ifndef SL_ASSERT_FAIL
  #define SL_ASSERT_FAIL(file,line,msg) \
    do { sl_platform_call(SLPlatformOp_PANIC,(uint64_t)(uintptr_t)(msg),0,0,0,0,0,0); \
         SL_TRAP(); } while(0)
#endif
#ifndef SL_ASSERTF_FAIL
  #define SL_ASSERTF_FAIL(file,line,fmt,...) SL_ASSERT_FAIL(file,line,fmt)
#endif
#ifndef sl_unwrap
  #define sl_unwrap(p) ((p) != (void*)0 ? (p) : \
    ((void)sl_platform_call(SLPlatformOp_PANIC, \
      (uint64_t)(uintptr_t)"unwrap: null value",18,0,0,0,0,0), SL_TRAP(), (p)))
#endif

