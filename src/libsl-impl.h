// constants, macros, types and functions only needed by the implementation, not part of the API
#pragma once
#include "libsl.h"

#ifndef __has_builtin
    #define __has_builtin(...) 0
#endif

#ifndef SL_LIBC
    #define SL_LIBC __STDC_HOSTED__
#endif

#if SL_LIBC
    #include <string.h>
#else
    #if !defined(memcpy) && __has_builtin(__builtin_memcpy)
        #define memcpy __builtin_memcpy
    #endif
    #if !defined(memmove) && __has_builtin(__builtin_memmove)
        #define memmove __builtin_memmove
    #endif
    #if !defined(memset) && __has_builtin(__builtin_memset)
        #define memset __builtin_memset
    #endif
    #if !defined(memcmp) && __has_builtin(__builtin_memcmp)
        #define memcmp __builtin_memcmp
    #endif
#endif
