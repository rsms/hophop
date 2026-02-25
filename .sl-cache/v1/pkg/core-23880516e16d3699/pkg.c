#ifndef CORE_23880516E16D3699_H
#define CORE_23880516E16D3699_H

#include <core/core.h>
typedef struct core__str__hdr core__str__hdr;
typedef core__str__hdr core__str;
typedef enum core__LogLevel core__LogLevel;
typedef enum core__LoggerFlag core__LoggerFlag;
typedef struct core__Logger core__Logger;
typedef struct core__Allocator core__Allocator;
typedef struct core__Context core__Context;
typedef struct core__PrintContext core__PrintContext;
typedef struct core__str__hdr core__str__hdr;
typedef core__str__hdr core__str;
typedef enum core__LogLevel core__LogLevel;
typedef enum core__LoggerFlag core__LoggerFlag;
typedef struct core__Logger core__Logger;
typedef struct core__Allocator core__Allocator;
typedef struct core__Context core__Context;
typedef struct core__PrintContext core__PrintContext;

typedef __sl_u32 core__LoggerFlags;

typedef core__LoggerFlags core__LogFlags;


typedef void (*__sl_fn_t_0)(__sl_Logger *p0, __sl_str *p1, core__LogLevel p2, core__LogFlags p3);
typedef __sl_uint (*__sl_fn_t_1)(__sl_Allocator *p0, __sl_uint p1, __sl_uint p2, __sl_uint p3, __sl_uint *p4, __sl_u32 p5);

void core__print(core__PrintContext *context, __sl_str *message);

typedef struct core__str__hdr {
    __sl_u32 len;
} core__str__hdr;
typedef core__str__hdr core__str;
static inline __sl_u8* core__str__bytes(core__str* p) {
    __sl_uint off = sizeof(core__str__hdr);
    off = __sl_align_up(off, _Alignof(__sl_u8));
    return (__sl_u8*)((__sl_u8*)p + off);
}
static inline __sl_uint core__str__sizeof(core__str* p) {
    __sl_uint off = sizeof(core__str__hdr);
    off = __sl_align_up(off, _Alignof(__sl_u8));
    off += (__sl_uint)p->len * sizeof(__sl_u8);
    off = __sl_align_up(off, _Alignof(core__str__hdr));
    return off;
}

typedef enum core__LogLevel {
    core__LogLevel__Trace = (-20)    ,
core__LogLevel__Debug = (-10)    ,
core__LogLevel__Info = 0    ,
core__LogLevel__Warning = 20    ,
core__LogLevel__Error = 30    ,
core__LogLevel__Fatal = 40
} core__LogLevel;

typedef enum core__LoggerFlag {
    core__LoggerFlag__Level = (1 << 0)    ,
core__LoggerFlag__Date = (1 << 1)    ,
core__LoggerFlag__Time = (1 << 2)    ,
core__LoggerFlag__ShortFile = (1 << 3)    ,
core__LoggerFlag__LongFile = (1 << 4)    ,
core__LoggerFlag__Line = (1 << 5)    ,
core__LoggerFlag__Function = (1 << 6)    ,
core__LoggerFlag__Thread = (1 << 7)    ,
core__LoggerFlag__Styling = (1 << 8)    ,
core__LoggerFlag__Prefix = (1 << 9)
} core__LoggerFlag;

typedef __sl_u32 core__LoggerFlags;

typedef core__LoggerFlags core__LogFlags;

typedef struct core__Logger {
    __sl_fn_t_0 handler;
    core__LogLevel min_level;
    core__LogFlags flags;
    __sl_str *prefix;
} core__Logger;

typedef struct core__Allocator {
    __sl_fn_t_1 impl;
} core__Allocator;

typedef struct core__Context {
    __sl_Allocator *mem;
    __sl_Allocator *temp_mem;
    __sl_Logger log;
} core__Context;

typedef struct core__PrintContext {
    __sl_Logger log;
} core__PrintContext;

#ifdef CORE_23880516E16D3699_IMPL

static __sl_u32 core__len(__sl_str *x);

static __sl_u8 *core__cstr(__sl_str *s);

static __sl_str *core__concat(__sl_str *a, __sl_str *b);

static void core__free(void);

static void core__panic(__sl_str *message);

void core__print(core__PrintContext *context, __sl_str *message);

static __sl_uint core__sizeof(void);

static __sl_u32 core__len(__sl_str *x) {
    return (__sl_u32)(((__sl_str*)(x))->len);
}

static __sl_u8 *core__cstr(__sl_str *s) {
    return __sl_cstr(s);
}

static __sl_str *core__concat(__sl_str *a, __sl_str *b) {
    return ((__sl_str*)(0));
}

static void core__free(void) {
}

static void core__panic(__sl_str *message) {
}

void core__print(core__PrintContext *context, __sl_str *message) {
    context->log.handler((&context->log), message, core__LogLevel__Info, ((core__LogFlags)(0)));
}

static __sl_uint core__sizeof(void) {
    return 0;
}

#endif /* CORE_23880516E16D3699_IMPL */

#endif /* CORE_23880516E16D3699_H */
