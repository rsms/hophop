#pragma once
#include "libhop.h"

H2_API_BEGIN

typedef enum {
    H2CTFEValue_INVALID = 0,
    H2CTFEValue_INT,
    H2CTFEValue_FLOAT,
    H2CTFEValue_BOOL,
    H2CTFEValue_STRING,
    H2CTFEValue_TYPE,
    H2CTFEValue_SPAN,
    H2CTFEValue_NULL,
    H2CTFEValue_OPTIONAL,
    H2CTFEValue_AGGREGATE,
    H2CTFEValue_ARRAY,
    H2CTFEValue_REFERENCE,
} H2CTFEValueKind;

typedef struct {
    const uint8_t* _Nullable bytes;
    uint32_t len;
} H2CTFEString;

typedef struct {
    const uint8_t* _Nullable fileBytes;
    uint32_t fileLen;
    uint32_t startLine;
    uint32_t startColumn;
    uint32_t endLine;
    uint32_t endColumn;
} H2CTFESpan;

typedef struct {
    H2CTFEValueKind kind;
    int64_t         i64;
    double          f64;
    uint8_t         b;
    uint64_t        typeTag;
    H2CTFEString    s;
    H2CTFESpan      span;
} H2CTFEValue;

static const uint64_t H2CTFEValueTag_AGG_PARTIAL = UINT64_C(1) << 57;

typedef int (*H2CTFEResolveIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

typedef int (*H2CTFEResolveCallFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nonnull args,
    uint32_t argCount,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2CTFEMakeTupleFn)(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2CTFEIndexValueFn)(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    const H2CTFEValue* _Nonnull index,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2CTFEAggGetFieldFn)(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2CTFEAggAddrFieldFn)(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

int H2CTFEEvalExprEx(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   nodeId,
    H2CTFEResolveIdentFn _Nullable resolveIdent,
    H2CTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    H2CTFEMakeTupleFn _Nullable makeTuple,
    void* _Nullable makeTupleCtx,
    H2CTFEIndexValueFn _Nullable indexValue,
    void* _Nullable indexValueCtx,
    H2CTFEAggGetFieldFn _Nullable aggGetField,
    void* _Nullable aggGetFieldCtx,
    H2CTFEAggAddrFieldFn _Nullable aggAddrField,
    void* _Nullable aggAddrFieldCtx,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

int H2CTFEEvalExpr(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   nodeId,
    H2CTFEResolveIdentFn _Nullable resolveIdent,
    H2CTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

int H2CTFEValueToInt64(const H2CTFEValue* _Nonnull value, int64_t* _Nonnull out);

H2_API_END
