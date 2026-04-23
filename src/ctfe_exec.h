#pragma once
#include "ctfe.h"

H2_API_BEGIN

#define H2CTFE_EXEC_DEFAULT_FOR_LIMIT 100000u

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    int32_t  typeNode;
    uint8_t mutable;
    uint8_t     _reserved[3];
    H2CTFEValue value;
} H2CTFEExecBinding;

typedef struct H2CTFEExecEnv {
    struct H2CTFEExecEnv* _Nullable parent;
    H2CTFEExecBinding* _Nullable bindings;
    uint32_t bindingLen;
} H2CTFEExecEnv;

typedef int (*H2CTFEExecEvalExprFn)(
    void* _Nullable ctx,
    int32_t exprNode,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*H2CTFEExecEvalExprForTypeFn)(
    void* _Nullable ctx,
    int32_t exprNode,
    int32_t typeNode,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*H2CTFEExecResolveTypeFn)(
    void* _Nullable ctx, int32_t typeNode, int32_t* _Nonnull outTypeId);

typedef int (*H2CTFEExecInferValueTypeFn)(
    void* _Nullable ctx, const H2CTFEValue* _Nonnull value, int32_t* _Nonnull outTypeId);

typedef int (*H2CTFEExecInferExprTypeFn)(
    void* _Nullable ctx, int32_t exprNode, int32_t* _Nonnull outTypeId);

typedef int (*H2CTFEExecIsOptionalTypeFn)(
    void* _Nullable ctx,
    int32_t typeId,
    int32_t* _Nullable outPayloadTypeId,
    int* _Nonnull outIsOptional);

typedef struct H2CTFEExecCtx H2CTFEExecCtx;

typedef int (*H2CTFEExecZeroInitFn)(
    void* _Nullable ctx,
    int32_t typeNode,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*H2CTFEExecAssignExprFn)(
    void* _Nullable ctx,
    H2CTFEExecCtx* _Nonnull execCtx,
    int32_t exprNode,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*H2CTFEExecAssignValueExprFn)(
    void* _Nullable ctx,
    H2CTFEExecCtx* _Nonnull execCtx,
    int32_t lhsExprNode,
    const H2CTFEValue* _Nonnull inValue,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*H2CTFEExecMatchPatternFn)(
    void* _Nullable ctx,
    H2CTFEExecCtx* _Nonnull execCtx,
    const H2CTFEValue* _Nonnull subjectValue,
    int32_t labelExprNode,
    int* _Nonnull outMatched);

typedef int (*H2CTFEExecForInIndexFn)(
    void* _Nullable ctx,
    H2CTFEExecCtx* _Nonnull execCtx,
    const H2CTFEValue* _Nonnull sourceValue,
    uint32_t index,
    int      byRef,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*H2CTFEExecForInIterFn)(
    void* _Nullable ctx,
    H2CTFEExecCtx* _Nonnull execCtx,
    int32_t sourceNode,
    const H2CTFEValue* _Nonnull sourceValue,
    uint32_t index,
    int      hasKey,
    int      keyRef,
    int      valueRef,
    int      valueDiscard,
    int* _Nonnull outHasItem,
    H2CTFEValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst);

struct H2CTFEExecCtx {
    H2Arena* _Nonnull arena;
    const H2Ast* _Nonnull ast;
    H2StrView src;
    H2Diag* _Nullable diag;

    H2CTFEExecEnv* _Nullable env;
    H2CTFEExecEvalExprFn _Nonnull evalExpr;
    void* _Nullable evalExprCtx;
    H2CTFEExecEvalExprForTypeFn _Nullable evalExprForType;
    void* _Nullable evalExprForTypeCtx;
    H2CTFEExecResolveTypeFn _Nullable resolveType;
    void* _Nullable resolveTypeCtx;
    H2CTFEExecInferValueTypeFn _Nullable inferValueType;
    void* _Nullable inferValueTypeCtx;
    H2CTFEExecInferExprTypeFn _Nullable inferExprType;
    void* _Nullable inferExprTypeCtx;
    H2CTFEExecIsOptionalTypeFn _Nullable isOptionalType;
    void* _Nullable isOptionalTypeCtx;
    H2CTFEExecZeroInitFn _Nullable zeroInit;
    void* _Nullable zeroInitCtx;
    H2CTFEExecAssignExprFn _Nullable assignExpr;
    void* _Nullable assignExprCtx;
    H2CTFEExecAssignValueExprFn _Nullable assignValueExpr;
    void* _Nullable assignValueExprCtx;
    H2CTFEExecMatchPatternFn _Nullable matchPattern;
    void* _Nullable matchPatternCtx;
    H2CTFEExecForInIndexFn _Nullable forInIndex;
    void* _Nullable forInIndexCtx;
    H2CTFEExecForInIterFn _Nullable forInIter;
    void* _Nullable forInIterCtx;

    const char* _Nullable nonConstReason;
    uint32_t nonConstStart;
    uint32_t nonConstEnd;

    int32_t  pendingReturnExprNode;
    uint32_t forIterLimit;
    uint8_t  skipConstBlocks;
    uint8_t  _reserved[3];
};

void H2CTFEExecResetReason(H2CTFEExecCtx* _Nonnull c);
void H2CTFEExecSetReason(
    H2CTFEExecCtx* _Nonnull c, uint32_t start, uint32_t end, const char* _Nonnull reason);
void H2CTFEExecSetReasonNode(
    H2CTFEExecCtx* _Nonnull c, int32_t nodeId, const char* _Nonnull reason);

int H2CTFEExecEnvLookup(
    const H2CTFEExecCtx* _Nonnull c,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2CTFEValue* _Nonnull outValue);

H2_API_END
