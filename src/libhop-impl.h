// constants, macros, types and functions only needed by the implementation, not part of the API
#pragma once
#include "libhop.h"
#include "ctfe.h"

#ifndef __has_builtin
    #define __has_builtin(...) 0
#endif

#ifndef HOP_LIBC
    #define HOP_LIBC __STDC_HOSTED__
#endif

#if HOP_LIBC
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
    HOPStringLitErr_NONE = 0,
    HOPStringLitErr_UNTERMINATED,
    HOPStringLitErr_INVALID_ESCAPE,
    HOPStringLitErr_INVALID_CODEPOINT,
    HOPStringLitErr_INVALID_UTF8,
    HOPStringLitErr_ARENA_OOM,
} HOPStringLitErrKind;

typedef struct {
    HOPStringLitErrKind kind;
    uint32_t            start;
    uint32_t            end;
} HOPStringLitErr;

typedef enum {
    HOPRuneLitErr_NONE = 0,
    HOPRuneLitErr_UNTERMINATED,
    HOPRuneLitErr_EMPTY,
    HOPRuneLitErr_MULTIPLE_CODEPOINTS,
    HOPRuneLitErr_INVALID_ESCAPE,
    HOPRuneLitErr_INVALID_CODEPOINT,
    HOPRuneLitErr_INVALID_UTF8,
} HOPRuneLitErrKind;

typedef struct {
    HOPRuneLitErrKind kind;
    uint32_t          start;
    uint32_t          end;
} HOPRuneLitErr;

HOPDiagCode HOPStringLitErrDiagCode(HOPStringLitErrKind kind);
HOPDiagCode HOPRuneLitErrDiagCode(HOPRuneLitErrKind kind);
int         HOPDecodeStringLiteralValidate(
    const char* _Nonnull src, uint32_t start, uint32_t end, HOPStringLitErr* _Nullable outErr);
int HOPDecodeStringLiteralArena(
    HOPArena* _Nonnull arena,
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    HOPStringLitErr* _Nullable outErr);
int HOPDecodeStringLiteralMalloc(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint8_t* _Nullable* _Nonnull outBytes,
    uint32_t* _Nonnull outLen,
    HOPStringLitErr* _Nullable outErr);
int HOPDecodeRuneLiteralValidate(
    const char* _Nonnull src,
    uint32_t start,
    uint32_t end,
    uint32_t* _Nonnull outRune,
    HOPRuneLitErr* _Nullable outErr);
int HOPIsStringLiteralConcatChain(const HOPAst* _Nonnull ast, int32_t nodeId);

typedef struct HOPConstEvalSession HOPConstEvalSession;

typedef enum {
    HOPConstEvalTypeKind_INVALID = 0,
    HOPConstEvalTypeKind_BUILTIN,
    HOPConstEvalTypeKind_NAMED,
    HOPConstEvalTypeKind_ALIAS,
    HOPConstEvalTypeKind_ANON_STRUCT,
    HOPConstEvalTypeKind_ANON_UNION,
    HOPConstEvalTypeKind_PTR,
    HOPConstEvalTypeKind_REF,
    HOPConstEvalTypeKind_ARRAY,
    HOPConstEvalTypeKind_SLICE,
    HOPConstEvalTypeKind_UNTYPED_INT,
    HOPConstEvalTypeKind_UNTYPED_FLOAT,
    HOPConstEvalTypeKind_FUNCTION,
    HOPConstEvalTypeKind_TUPLE,
    HOPConstEvalTypeKind_OPTIONAL,
    HOPConstEvalTypeKind_NULL,
} HOPConstEvalTypeKind;

typedef enum {
    HOPConstEvalBuiltinKind_INVALID = 0,
    HOPConstEvalBuiltinKind_VOID,
    HOPConstEvalBuiltinKind_BOOL,
    HOPConstEvalBuiltinKind_STR,
    HOPConstEvalBuiltinKind_TYPE,
    HOPConstEvalBuiltinKind_U8,
    HOPConstEvalBuiltinKind_U16,
    HOPConstEvalBuiltinKind_U32,
    HOPConstEvalBuiltinKind_U64,
    HOPConstEvalBuiltinKind_I8,
    HOPConstEvalBuiltinKind_I16,
    HOPConstEvalBuiltinKind_I32,
    HOPConstEvalBuiltinKind_I64,
    HOPConstEvalBuiltinKind_USIZE,
    HOPConstEvalBuiltinKind_ISIZE,
    HOPConstEvalBuiltinKind_RAWPTR,
    HOPConstEvalBuiltinKind_F32,
    HOPConstEvalBuiltinKind_F64,
} HOPConstEvalBuiltinKind;

enum {
    HOPConstEvalTypeFlag_MUTABLE = 1u << 1,
};

typedef struct {
    HOPConstEvalTypeKind    kind;
    HOPConstEvalBuiltinKind builtin;
    int32_t                 baseTypeId;
    int32_t                 declNode;
    uint32_t                arrayLen;
    uint32_t                nameStart;
    uint32_t                nameEnd;
    uint16_t                flags;
} HOPConstEvalTypeInfo;

int HOPConstEvalSessionInit(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    HOPConstEvalSession* _Nullable* _Nonnull outSession,
    HOPDiag* _Nullable diag);
int HOPConstEvalSessionEvalExpr(
    HOPConstEvalSession* _Nonnull session,
    int32_t exprNode,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int HOPConstEvalSessionEvalIntExpr(
    HOPConstEvalSession* _Nonnull session,
    int32_t exprNode,
    int64_t* _Nonnull outValue,
    int* _Nonnull outIsConst);
int HOPConstEvalSessionEvalTopLevelConst(
    HOPConstEvalSession* _Nonnull session,
    int32_t constNode,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int HOPConstEvalSessionDecodeTypeTag(
    HOPConstEvalSession* _Nonnull session, uint64_t typeTag, int32_t* _Nonnull outTypeId);
int HOPConstEvalSessionGetTypeInfo(
    HOPConstEvalSession* _Nonnull session,
    int32_t typeId,
    HOPConstEvalTypeInfo* _Nonnull outTypeInfo);
