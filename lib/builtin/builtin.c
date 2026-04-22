#include "builtin.h"

#if !__has_builtin(__builtin_memcmp)

int __sl_memcmp(const void* a, const void* b, __sl_uint n) {
    const __sl_u8* aa = (const __sl_u8*)a;
    const __sl_u8* bb = (const __sl_u8*)b;
    __sl_uint      i = 0;
    for (; i < n; i++) {
        if (aa[i] < bb[i]) {
            return -1;
        }
        if (aa[i] > bb[i]) {
            return 1;
        }
    }
    return 0;
}

#endif

#if !__has_builtin(__builtin_memcpy)

void __sl_memcpy(void* dst, const void* src, __sl_uint n) {
    __sl_u8*       d = (__sl_u8*)dst;
    const __sl_u8* s = (const __sl_u8*)src;
    __sl_uint      i = 0;
    for (; i < n; i++) {
        d[i] = s[i];
    }
}

#endif

#if !__has_builtin(__builtin_memmove)

void __sl_memmove(void* dst, const void* src, __sl_uint n) {
    __sl_u8*       d = (__sl_u8*)dst;
    const __sl_u8* s = (const __sl_u8*)src;
    __sl_uint      i;
    if (dst == src || n == 0u) {
        return;
    }
    if (d < s || d >= s + n) {
        for (i = 0u; i < n; i++) {
            d[i] = s[i];
        }
        return;
    }
    for (i = n; i > 0u; i--) {
        d[i - 1u] = s[i - 1u];
    }
}

#endif

__sl_int __sl_copy(
    void* dst, __sl_int dst_len, const void* src, __sl_int src_len, __sl_int elem_size) {
    __sl_int  n;
    __sl_uint bytes;
    if __sl_unlikely (dst_len < 0 || src_len < 0 || elem_size < 0) {
        __sl_panic(__sl_strlitp("negative size"), "", 0);
    }
    n = dst_len < src_len ? dst_len : src_len;
    bytes = __sl_checked_uint_size(n * elem_size);
    if (n == 0 || elem_size == 0 || dst == src) {
        return n;
    }
    if (dst == NULL || src == NULL) {
        return 0;
    }
    __sl_memmove(dst, src, bytes);
    return n;
}

__sl_int __sl_mem_order(const void* a, __sl_int a_len, const void* b, __sl_int b_len) {
    __sl_int n;
    int      cmp;
    if __sl_unlikely (a_len < 0 || b_len < 0) {
        __sl_panic(__sl_strlitp("negative size"), "", 0);
    }
    n = a_len < b_len ? a_len : b_len;
    cmp = n > 0 ? __sl_memcmp(a, b, __sl_checked_uint_size(n)) : 0;
    return cmp != 0 ? (__sl_int)((cmp > 0) - (cmp < 0))
                    : (__sl_int)((a_len > b_len) - (a_len < b_len));
}
