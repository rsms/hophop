// constants, macros, types and functions only needed by the implementation, not part of the API
#pragma once
#include "libhop.h"
#include "ctfe.h"

#ifndef __has_builtin
    #define __has_builtin(...) 0
#endif

#ifndef H2_LIBC
    #define H2_LIBC __STDC_HOSTED__
#endif

#if H2_LIBC
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
    H2StringLitErr_NONE = 0,
    H2StringLitErr_UNTERMINATED,
    H2StringLitErr_INVALID_ESCAPE,
    H2StringLitErr_INVALID_CODEPOINT,
    H2StringLitErr_INVALID_UTF8,
    H2StringLitErr_ARENA_OOM,
} H2StringLitErrKind;

typedef struct {
    H2StringLitErrKind kind;
    uint32_t           start;
    uint32_t           end;
} H2StringLitErr;

typedef enum {
    H2RuneLitErr_NONE = 0,
    H2RuneLitErr_UNTERMINATED,
    H2RuneLitErr_EMPTY,
    H2RuneLitErr_MULTIPLE_CODEPOINTS,
    H2RuneLitErr_INVALID_ESCAPE,
    H2RuneLitErr_INVALID_CODEPOINT,
    H2RuneLitErr_INVALID_UTF8,
} H2RuneLitErrKind;

typedef struct {
    H2RuneLitErrKind kind;
    uint32_t         start;
    uint32_t         end;
} H2RuneLitErr;

H2DiagCode H2StringLitErrDiagCode(H2StringLitErrKind kind);
H2DiagCode H2RuneLitErrDiagCode(H2RuneLitErrKind kind);
int        H2DecodeStringLiteralValidate(
    const char* _Nonnull src, uint32_t start, uint32_t end, H2StringLitErr* _Nullable outErr);
int H2DecodeStringLiteralArena(
    H2Arena* _Nonnull arena,
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    H2StringLitErr* _Nullable outErr);
int H2DecodeStringLiteralMalloc(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    H2StringLitErr* _Nullable outErr);
int H2DecodeRuneLiteralValidate(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint32_t* _Nonnull outRune,
    H2RuneLitErr* _Nullable outErr);
int H2IsStringLiteralConcatChain(const H2Ast* _Nonnull ast, int32_t nodeId);

typedef struct H2ConstEvalSession H2ConstEvalSession;

typedef enum {
    H2ConstEvalTypeKind_INVALID = 0,
    H2ConstEvalTypeKind_BUILTIN,
    H2ConstEvalTypeKind_NAMED,
    H2ConstEvalTypeKind_ALIAS,
    H2ConstEvalTypeKind_ANON_STRUCT,
    H2ConstEvalTypeKind_ANON_UNION,
    H2ConstEvalTypeKind_PTR,
    H2ConstEvalTypeKind_REF,
    H2ConstEvalTypeKind_ARRAY,
    H2ConstEvalTypeKind_SLICE,
    H2ConstEvalTypeKind_UNTYPED_INT,
    H2ConstEvalTypeKind_UNTYPED_FLOAT,
    H2ConstEvalTypeKind_FUNCTION,
    H2ConstEvalTypeKind_TUPLE,
    H2ConstEvalTypeKind_OPTIONAL,
    H2ConstEvalTypeKind_NULL,
} H2ConstEvalTypeKind;

typedef enum {
    H2ConstEvalBuiltinKind_INVALID = 0,
    H2ConstEvalBuiltinKind_VOID,
    H2ConstEvalBuiltinKind_BOOL,
    H2ConstEvalBuiltinKind_STR,
    H2ConstEvalBuiltinKind_TYPE,
    H2ConstEvalBuiltinKind_U8,
    H2ConstEvalBuiltinKind_U16,
    H2ConstEvalBuiltinKind_U32,
    H2ConstEvalBuiltinKind_U64,
    H2ConstEvalBuiltinKind_I8,
    H2ConstEvalBuiltinKind_I16,
    H2ConstEvalBuiltinKind_I32,
    H2ConstEvalBuiltinKind_I64,
    H2ConstEvalBuiltinKind_USIZE,
    H2ConstEvalBuiltinKind_ISIZE,
    H2ConstEvalBuiltinKind_RAWPTR,
    H2ConstEvalBuiltinKind_F32,
    H2ConstEvalBuiltinKind_F64,
} H2ConstEvalBuiltinKind;

enum {
    H2ConstEvalTypeFlag_MUTABLE = 1u << 1,
};

typedef struct {
    H2ConstEvalTypeKind    kind;
    H2ConstEvalBuiltinKind builtin;
    int32_t                baseTypeId;
    int32_t                declNode;
    uint32_t               arrayLen;
    uint32_t               nameStart;
    uint32_t               nameEnd;
    uint16_t               flags;
} H2ConstEvalTypeInfo;

int H2ConstEvalSessionInit(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    H2ConstEvalSession* _Nullable* _Nonnull outSession,
    H2Diag* _Nullable diag);
int H2ConstEvalSessionEvalExpr(
    H2ConstEvalSession* _Nonnull session,
    int32_t exprNode,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int H2ConstEvalSessionEvalIntExpr(
    H2ConstEvalSession* _Nonnull session,
    int32_t exprNode,
    int64_t* _Nonnull outValue,
    int* _Nonnull outIsConst);
int H2ConstEvalSessionEvalTopLevelConst(
    H2ConstEvalSession* _Nonnull session,
    int32_t constNode,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int H2ConstEvalSessionDecodeTypeTag(
    H2ConstEvalSession* _Nonnull session, uint64_t typeTag, int32_t* _Nonnull outTypeId);
int H2ConstEvalSessionGetTypeInfo(
    H2ConstEvalSession* _Nonnull session,
    int32_t typeId,
    H2ConstEvalTypeInfo* _Nonnull outTypeInfo);
