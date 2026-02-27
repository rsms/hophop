// constants, macros, types and functions only needed by the implementation, not part of the API
#pragma once
#include "libsl.h"
#include "ctfe.h"

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

typedef enum {
    SLStringLitErr_NONE = 0,
    SLStringLitErr_UNTERMINATED,
    SLStringLitErr_INVALID_ESCAPE,
    SLStringLitErr_INVALID_CODEPOINT,
    SLStringLitErr_INVALID_UTF8,
    SLStringLitErr_ARENA_OOM,
} SLStringLitErrKind;

typedef struct {
    SLStringLitErrKind kind;
    uint32_t           start;
    uint32_t           end;
} SLStringLitErr;

typedef enum {
    SLRuneLitErr_NONE = 0,
    SLRuneLitErr_UNTERMINATED,
    SLRuneLitErr_EMPTY,
    SLRuneLitErr_MULTIPLE_CODEPOINTS,
    SLRuneLitErr_INVALID_ESCAPE,
    SLRuneLitErr_INVALID_CODEPOINT,
    SLRuneLitErr_INVALID_UTF8,
} SLRuneLitErrKind;

typedef struct {
    SLRuneLitErrKind kind;
    uint32_t         start;
    uint32_t         end;
} SLRuneLitErr;

SLDiagCode SLStringLitErrDiagCode(SLStringLitErrKind kind);
SLDiagCode SLRuneLitErrDiagCode(SLRuneLitErrKind kind);
int        SLDecodeStringLiteralValidate(
           const char* _Nonnull src, uint32_t start, uint32_t end, SLStringLitErr* _Nullable outErr);
int SLDecodeStringLiteralArena(
    SLArena* _Nonnull arena,
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    SLStringLitErr* _Nullable outErr);
int SLDecodeStringLiteralMalloc(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    SLStringLitErr* _Nullable outErr);
int SLDecodeRuneLiteralValidate(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint32_t* _Nonnull outRune,
    SLRuneLitErr* _Nullable outErr);
int SLIsStringLiteralConcatChain(const SLAst* _Nonnull ast, int32_t nodeId);

typedef struct SLConstEvalSession SLConstEvalSession;

int SLConstEvalSessionInit(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    SLConstEvalSession* _Nullable* _Nonnull outSession,
    SLDiag* _Nullable diag);
int SLConstEvalSessionEvalExpr(
    SLConstEvalSession* _Nonnull session,
    int32_t exprNode,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int SLConstEvalSessionEvalIntExpr(
    SLConstEvalSession* _Nonnull session,
    int32_t exprNode,
    int64_t* _Nonnull outValue,
    int* _Nonnull outIsConst);
int SLConstEvalSessionEvalTopLevelConst(
    SLConstEvalSession* _Nonnull session,
    int32_t constNode,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
