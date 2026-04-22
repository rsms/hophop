#pragma once
#include "ctfe.h"

HOP_API_BEGIN

#define HOPCTFE_EXEC_DEFAULT_FOR_LIMIT 100000u

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    int32_t  typeNode;
    uint8_t mutable;
    uint8_t      _reserved[3];
    HOPCTFEValue value;
} HOPCTFEExecBinding;

typedef struct HOPCTFEExecEnv {
    struct HOPCTFEExecEnv* _Nullable parent;
    HOPCTFEExecBinding* _Nullable bindings;
    uint32_t bindingLen;
} HOPCTFEExecEnv;

typedef int (*HOPCTFEExecEvalExprFn)(
    void* _Nullable ctx,
    int32_t exprNode,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*HOPCTFEExecEvalExprForTypeFn)(
    void* _Nullable ctx,
    int32_t exprNode,
    int32_t typeNode,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*HOPCTFEExecResolveTypeFn)(
    void* _Nullable ctx, int32_t typeNode, int32_t* _Nonnull outTypeId);

typedef int (*HOPCTFEExecInferValueTypeFn)(
    void* _Nullable ctx, const HOPCTFEValue* _Nonnull value, int32_t* _Nonnull outTypeId);

typedef int (*HOPCTFEExecInferExprTypeFn)(
    void* _Nullable ctx, int32_t exprNode, int32_t* _Nonnull outTypeId);

typedef int (*HOPCTFEExecIsOptionalTypeFn)(
    void* _Nullable ctx,
    int32_t typeId,
    int32_t* _Nullable outPayloadTypeId,
    int* _Nonnull outIsOptional);

typedef struct HOPCTFEExecCtx HOPCTFEExecCtx;

typedef int (*HOPCTFEExecZeroInitFn)(
    void* _Nullable ctx,
    int32_t typeNode,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*HOPCTFEExecAssignExprFn)(
    void* _Nullable ctx,
    HOPCTFEExecCtx* _Nonnull execCtx,
    int32_t exprNode,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*HOPCTFEExecAssignValueExprFn)(
    void* _Nullable ctx,
    HOPCTFEExecCtx* _Nonnull execCtx,
    int32_t lhsExprNode,
    const HOPCTFEValue* _Nonnull inValue,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*HOPCTFEExecMatchPatternFn)(
    void* _Nullable ctx,
    HOPCTFEExecCtx* _Nonnull execCtx,
    const HOPCTFEValue* _Nonnull subjectValue,
    int32_t labelExprNode,
    int* _Nonnull outMatched);

typedef int (*HOPCTFEExecForInIndexFn)(
    void* _Nullable ctx,
    HOPCTFEExecCtx* _Nonnull execCtx,
    const HOPCTFEValue* _Nonnull sourceValue,
    uint32_t index,
    int      byRef,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*HOPCTFEExecForInIterFn)(
    void* _Nullable ctx,
    HOPCTFEExecCtx* _Nonnull execCtx,
    int32_t sourceNode,
    const HOPCTFEValue* _Nonnull sourceValue,
    uint32_t index,
    int      hasKey,
    int      keyRef,
    int      valueRef,
    int      valueDiscard,
    int* _Nonnull outHasItem,
    HOPCTFEValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst);

struct HOPCTFEExecCtx {
    HOPArena* _Nonnull arena;
    const HOPAst* _Nonnull ast;
    HOPStrView src;
    HOPDiag* _Nullable diag;

    HOPCTFEExecEnv* _Nullable env;
    HOPCTFEExecEvalExprFn _Nonnull evalExpr;
    void* _Nullable evalExprCtx;
    HOPCTFEExecEvalExprForTypeFn _Nullable evalExprForType;
    void* _Nullable evalExprForTypeCtx;
    HOPCTFEExecResolveTypeFn _Nullable resolveType;
    void* _Nullable resolveTypeCtx;
    HOPCTFEExecInferValueTypeFn _Nullable inferValueType;
    void* _Nullable inferValueTypeCtx;
    HOPCTFEExecInferExprTypeFn _Nullable inferExprType;
    void* _Nullable inferExprTypeCtx;
    HOPCTFEExecIsOptionalTypeFn _Nullable isOptionalType;
    void* _Nullable isOptionalTypeCtx;
    HOPCTFEExecZeroInitFn _Nullable zeroInit;
    void* _Nullable zeroInitCtx;
    HOPCTFEExecAssignExprFn _Nullable assignExpr;
    void* _Nullable assignExprCtx;
    HOPCTFEExecAssignValueExprFn _Nullable assignValueExpr;
    void* _Nullable assignValueExprCtx;
    HOPCTFEExecMatchPatternFn _Nullable matchPattern;
    void* _Nullable matchPatternCtx;
    HOPCTFEExecForInIndexFn _Nullable forInIndex;
    void* _Nullable forInIndexCtx;
    HOPCTFEExecForInIterFn _Nullable forInIter;
    void* _Nullable forInIterCtx;

    const char* _Nullable nonConstReason;
    uint32_t nonConstStart;
    uint32_t nonConstEnd;

    int32_t  pendingReturnExprNode;
    uint32_t forIterLimit;
    uint8_t  skipConstBlocks;
    uint8_t  _reserved[3];
};

void HOPCTFEExecResetReason(HOPCTFEExecCtx* _Nonnull c);
void HOPCTFEExecSetReason(
    HOPCTFEExecCtx* _Nonnull c, uint32_t start, uint32_t end, const char* _Nonnull reason);
void HOPCTFEExecSetReasonNode(
    HOPCTFEExecCtx* _Nonnull c, int32_t nodeId, const char* _Nonnull reason);

int HOPCTFEExecEnvLookup(
    const HOPCTFEExecCtx* _Nonnull c,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPCTFEValue* _Nonnull outValue);

HOP_API_END
