#pragma once
#include "ctfe.h"

SL_API_BEGIN

#define SLCTFE_EXEC_DEFAULT_FOR_LIMIT 100000u

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    uint8_t mutable;
    uint8_t     _reserved[3];
    SLCTFEValue value;
} SLCTFEExecBinding;

typedef struct SLCTFEExecEnv {
    struct SLCTFEExecEnv* _Nullable parent;
    SLCTFEExecBinding* _Nullable bindings;
    uint32_t bindingLen;
} SLCTFEExecEnv;

typedef int (*SLCTFEExecEvalExprFn)(
    void* _Nullable ctx,
    int32_t exprNode,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

typedef int (*SLCTFEExecResolveTypeFn)(
    void* _Nullable ctx, int32_t typeNode, int32_t* _Nonnull outTypeId);

typedef int (*SLCTFEExecInferValueTypeFn)(
    void* _Nullable ctx, const SLCTFEValue* _Nonnull value, int32_t* _Nonnull outTypeId);

typedef struct {
    SLArena* _Nonnull arena;
    const SLAst* _Nonnull ast;
    SLStrView src;
    SLDiag* _Nullable diag;

    SLCTFEExecEnv* _Nullable env;
    SLCTFEExecEvalExprFn _Nonnull evalExpr;
    void* _Nullable evalExprCtx;
    SLCTFEExecResolveTypeFn _Nullable resolveType;
    void* _Nullable resolveTypeCtx;
    SLCTFEExecInferValueTypeFn _Nullable inferValueType;
    void* _Nullable inferValueTypeCtx;

    const char* _Nullable nonConstReason;
    uint32_t nonConstStart;
    uint32_t nonConstEnd;

    int32_t  pendingReturnExprNode;
    uint32_t forIterLimit;
} SLCTFEExecCtx;

void SLCTFEExecResetReason(SLCTFEExecCtx* _Nonnull c);
void SLCTFEExecSetReason(
    SLCTFEExecCtx* _Nonnull c, uint32_t start, uint32_t end, const char* _Nonnull reason);
void SLCTFEExecSetReasonNode(
    SLCTFEExecCtx* _Nonnull c, int32_t nodeId, const char* _Nonnull reason);

int SLCTFEExecEnvLookup(
    const SLCTFEExecCtx* _Nonnull c,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLCTFEValue* _Nonnull outValue);

int SLCTFEExecEvalBlock(
    SLCTFEExecCtx* _Nonnull c,
    int32_t blockNode,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outDidReturn,
    int* _Nonnull outIsConst);

SL_API_END
