#pragma once
#include "libsl.h"

SL_API_BEGIN

typedef enum {
    SLCTFEValue_INVALID = 0,
    SLCTFEValue_INT,
    SLCTFEValue_FLOAT,
    SLCTFEValue_BOOL,
    SLCTFEValue_STRING,
    SLCTFEValue_TYPE,
    SLCTFEValue_SPAN,
    SLCTFEValue_NULL,
    SLCTFEValue_OPTIONAL,
    SLCTFEValue_AGGREGATE,
    SLCTFEValue_ARRAY,
    SLCTFEValue_REFERENCE,
} SLCTFEValueKind;

typedef struct {
    const uint8_t* _Nullable bytes;
    uint32_t len;
} SLCTFEString;

typedef struct {
    const uint8_t* _Nullable fileBytes;
    uint32_t fileLen;
    uint32_t startLine;
    uint32_t startColumn;
    uint32_t endLine;
    uint32_t endColumn;
} SLCTFESpan;

typedef struct {
    SLCTFEValueKind kind;
    int64_t         i64;
    double          f64;
    uint8_t         b;
    uint64_t        typeTag;
    SLCTFEString    s;
    SLCTFESpan      span;
} SLCTFEValue;

typedef int (*SLCTFEResolveIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

typedef int (*SLCTFEResolveCallFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLCTFEValue* _Nonnull args,
    uint32_t argCount,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLCTFEMakeTupleFn)(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLCTFEIndexValueFn)(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull base,
    const SLCTFEValue* _Nonnull index,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

int SLCTFEEvalExprEx(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    SLCTFEResolveIdentFn _Nullable resolveIdent,
    SLCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    SLCTFEMakeTupleFn _Nullable makeTuple,
    void* _Nullable makeTupleCtx,
    SLCTFEIndexValueFn _Nullable indexValue,
    void* _Nullable indexValueCtx,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

int SLCTFEEvalExpr(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    SLCTFEResolveIdentFn _Nullable resolveIdent,
    SLCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

int SLCTFEValueToInt64(const SLCTFEValue* _Nonnull value, int64_t* _Nonnull out);

SL_API_END
