// macros for unifying differences in C/C++ compilers
#pragma once

#ifndef __has_include
    #define __has_include(...) 0
#endif
#ifndef __has_extension
    #define __has_extension(...) 0
#endif
#ifndef __has_feature
    #define __has_feature(...) 0
#endif
#ifndef __has_attribute
    #define __has_attribute(...) 0
#endif

#ifndef _Noreturn
    #define _Noreturn __attribute__((noreturn))
#endif

#if defined(__clang__)
    #define SL_ASSUME_NONNULL_BEGIN                                                    \
        _Pragma("clang diagnostic push");                                              \
        _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"");            \
        _Pragma("clang diagnostic ignored \"-Wnullability-inferred-on-nested-type\""); \
        _Pragma("clang assume_nonnull begin"); // assume T* means "T* _Nonnull"
    #define SL_ASSUME_NONNULL_END        \
        _Pragma("clang diagnostic pop"); \
        _Pragma("clang assume_nonnull end");
#else
    #define SL_ASSUME_NONNULL_BEGIN
    #define SL_ASSUME_NONNULL_END
#endif

#ifdef __cplusplus
    #define SL_API_BEGIN SL_ASSUME_NONNULL_BEGIN extern "C" {
    #define SL_API_END \
        }              \
        SL_ASSUME_NONNULL_END
#else
    #define SL_API_BEGIN SL_ASSUME_NONNULL_BEGIN
    #define SL_API_END   SL_ASSUME_NONNULL_END
#endif

#if !__has_feature(nullability)
    #define _Nullable
    #define _Nonnull
#endif

#if __has_attribute(__aligned__)
    #define SL_ATTR_ALIGNED(N) __attribute__((__aligned__(N)))
#else
    #define SL_ATTR_ALIGNED(N)
#endif

#if !defined(nullable)
    #define nullable _Nullable
    #define __SL_DEFINED_NULLABLE
#endif

// SL_ENUM_TYPE_SUPPORTED is defined if the compiler supports ": Type" in an enum declaration.
// C++: standard since C++11. MSVC historically supported it, but __cplusplus may lie
// unless /Zc:__cplusplus is enabled, so also gate on _MSC_VER.
// C: standard since C23; also supported as an extension in clang, and in GCC 13+.
#ifndef SL_ENUM_TYPE_SUPPORTED
    #if defined(__cplusplus) \
        && ((__cplusplus >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1700))
        #define SL_ENUM_TYPE_SUPPORTED
    #elif (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)) || defined(__clang__) \
        || defined(__GNUC__)
        #define SL_ENUM_TYPE_SUPPORTED
    #endif
#endif

// SL_ENUM_TYPE usage example:
//     enum Foo SL_ENUM_TYPE(uint16_t) { Foo_A, Foo_B };
#ifndef SL_ENUM_TYPE
    #ifdef SL_ENUM_TYPE_SUPPORTED
        #define SL_ENUM_TYPE(T) : T
    #else
        #define SL_ENUM_TYPE(T)
        #ifndef SL_ENUM_TYPE_SUPPORTED_DISABLE_WARNING
            #warning Enum types not supported; sizeof some structures may report incorrect value
        #endif
    #endif
#endif

#ifdef __wasm__
    #define SL_SYS_IMPORT_AS(name) __attribute__((__import_module__("sl"), __import_name__(#name)))
    #define SL_SYS_EXPORT_AS(name) __attribute__((export_name(#name)))
#else
    #define SL_SYS_IMPORT_AS(name)
    #define SL_SYS_EXPORT_AS(name) __attribute__((visibility("internal")))
#endif
