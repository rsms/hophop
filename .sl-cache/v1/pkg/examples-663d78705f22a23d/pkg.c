#include <core-23880516e16d3699/pkg.c>

#ifndef EXAMPLES_663D78705F22A23D_H
#define EXAMPLES_663D78705F22A23D_H

#include <core/core.h>
typedef void (*__sl_fn_t_0)(__sl_Logger *p0, __sl_str *p1, core__LogLevel p2, core__LogFlags p3);
typedef __sl_uint (*__sl_fn_t_1)(__sl_Allocator *p0, __sl_uint p1, __sl_uint p2, __sl_uint p3, __sl_uint *p4, __sl_u32 p5);

void examples__main(__sl_Context *context __attribute__((unused)));

#ifdef EXAMPLES_663D78705F22A23D_IMPL

void examples__main(__sl_Context *context __attribute__((unused)));

void examples__main(__sl_Context *context __attribute__((unused))) {
    __sl_int x = 42;
    __sl_assert((x == 42));
}

#endif /* EXAMPLES_663D78705F22A23D_IMPL */

#endif /* EXAMPLES_663D78705F22A23D_H */
