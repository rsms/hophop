#pragma once
#include "libhop.h"

HOP_API_BEGIN

typedef enum {
    HOPCTFEValue_INVALID = 0,
    HOPCTFEValue_INT,
    HOPCTFEValue_FLOAT,
    HOPCTFEValue_BOOL,
    HOPCTFEValue_STRING,
    HOPCTFEValue_TYPE,
    HOPCTFEValue_SPAN,
    HOPCTFEValue_NULL,
    HOPCTFEValue_OPTIONAL,
    HOPCTFEValue_AGGREGATE,
    HOPCTFEValue_ARRAY,
    HOPCTFEValue_REFERENCE,
} HOPCTFEValueKind;

typedef struct {
    const uint8_t* _Nullable bytes;
    uint32_t len;
} HOPCTFEString;

typedef struct {
    const uint8_t* _Nullable fileBytes;
    uint32_t fileLen;
    uint32_t startLine;
    uint32_t startColumn;
    uint32_t endLine;
    uint32_t endColumn;
} HOPCTFESpan;

typedef struct {
    HOPCTFEValueKind kind;
    int64_t          i64;
    double           f64;
    uint8_t          b;
    uint64_t         typeTag;
    HOPCTFEString    s;
    HOPCTFESpan      span;
} HOPCTFEValue;

static const uint64_t HOPCTFEValueTag_AGG_PARTIAL = UINT64_C(1) << 57;

typedef int (*HOPCTFEResolveIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

typedef int (*HOPCTFEResolveCallFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nonnull args,
    uint32_t argCount,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPCTFEMakeTupleFn)(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPCTFEIndexValueFn)(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    const HOPCTFEValue* _Nonnull index,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPCTFEAggGetFieldFn)(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPCTFEAggAddrFieldFn)(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

int HOPCTFEEvalExprEx(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    nodeId,
    HOPCTFEResolveIdentFn _Nullable resolveIdent,
    HOPCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    HOPCTFEMakeTupleFn _Nullable makeTuple,
    void* _Nullable makeTupleCtx,
    HOPCTFEIndexValueFn _Nullable indexValue,
    void* _Nullable indexValueCtx,
    HOPCTFEAggGetFieldFn _Nullable aggGetField,
    void* _Nullable aggGetFieldCtx,
    HOPCTFEAggAddrFieldFn _Nullable aggAddrField,
    void* _Nullable aggAddrFieldCtx,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

int HOPCTFEEvalExpr(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    nodeId,
    HOPCTFEResolveIdentFn _Nullable resolveIdent,
    HOPCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

int HOPCTFEValueToInt64(const HOPCTFEValue* _Nonnull value, int64_t* _Nonnull out);

HOP_API_END
