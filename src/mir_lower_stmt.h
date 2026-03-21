#pragma once
#include "mir.h"

SL_API_BEGIN

typedef int (*SLMirLowerConstExprFn)(
    void* _Nullable ctx, int32_t exprNode, SLMirConst* _Nonnull outValue, SLDiag* _Nullable diag);

typedef struct {
    SLMirLowerConstExprFn _Nullable lowerConstExpr;
    void* _Nullable lowerConstExprCtx;
} SLMirLowerOptions;

int SLMirLowerSimpleFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerSimpleFunctionWithOptions(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    const SLMirLowerOptions* _Nullable options,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerAppendSimpleFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerAppendSimpleFunctionWithOptions(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    const SLMirLowerOptions* _Nullable options,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);

SL_API_END
