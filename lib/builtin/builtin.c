#include "builtin.h"

#if !__has_builtin(__builtin_memcmp)

int __hop_memcmp(const void* a, const void* b, __hop_uint n) {
    const __hop_u8* aa = (const __hop_u8*)a;
    const __hop_u8* bb = (const __hop_u8*)b;
    __hop_uint      i = 0;
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

void __hop_memcpy(void* dst, const void* src, __hop_uint n) {
    __hop_u8*       d = (__hop_u8*)dst;
    const __hop_u8* s = (const __hop_u8*)src;
    __hop_uint      i = 0;
    for (; i < n; i++) {
        d[i] = s[i];
    }
}

#endif

#if !__has_builtin(__builtin_memmove)

void __hop_memmove(void* dst, const void* src, __hop_uint n) {
    __hop_u8*       d = (__hop_u8*)dst;
    const __hop_u8* s = (const __hop_u8*)src;
    __hop_uint      i;
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

__hop_int __hop_copy(
    void* dst, __hop_int dst_len, const void* src, __hop_int src_len, __hop_int elem_size) {
    __hop_int  n;
    __hop_uint bytes;
    if __hop_unlikely (dst_len < 0 || src_len < 0 || elem_size < 0) {
        __hop_panic(__hop_strlitp("negative size"), "", 0);
    }
    n = dst_len < src_len ? dst_len : src_len;
    bytes = __hop_checked_uint_size(n * elem_size);
    if (n == 0 || elem_size == 0 || dst == src) {
        return n;
    }
    if (dst == NULL || src == NULL) {
        return 0;
    }
    __hop_memmove(dst, src, bytes);
    return n;
}

__hop_int __hop_mem_order(const void* a, __hop_int a_len, const void* b, __hop_int b_len) {
    __hop_int n;
    int       cmp;
    if __hop_unlikely (a_len < 0 || b_len < 0) {
        __hop_panic(__hop_strlitp("negative size"), "", 0);
    }
    n = a_len < b_len ? a_len : b_len;
    cmp = n > 0 ? __hop_memcmp(a, b, __hop_checked_uint_size(n)) : 0;
    return cmp != 0 ? (__hop_int)((cmp > 0) - (cmp < 0))
                    : (__hop_int)((a_len > b_len) - (a_len < b_len));
}
