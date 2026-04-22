#pragma once
#include "mir.h"

HOP_API_BEGIN

typedef int (*HOPMirLowerConstExprFn)(
    void* _Nullable ctx, int32_t exprNode, HOPMirConst* _Nonnull outValue, HOPDiag* _Nullable diag);

typedef struct {
    HOPMirLowerConstExprFn _Nullable lowerConstExpr;
    void* _Nullable lowerConstExprCtx;
} HOPMirLowerOptions;

int HOPMirLowerSimpleFunction(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
int HOPMirLowerSimpleFunctionWithOptions(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    const HOPMirLowerOptions* _Nullable options,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
int HOPMirLowerAppendSimpleFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
int HOPMirLowerAppendSimpleFunctionWithOptions(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    const HOPMirLowerOptions* _Nullable options,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);

HOP_API_END
