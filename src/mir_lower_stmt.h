#pragma once
#include "mir.h"

H2_API_BEGIN

typedef int (*H2MirLowerConstExprFn)(
    void* _Nullable ctx, int32_t exprNode, H2MirConst* _Nonnull outValue, H2Diag* _Nullable diag);

typedef struct {
    H2MirLowerConstExprFn _Nullable lowerConstExpr;
    void* _Nullable lowerConstExprCtx;
} H2MirLowerOptions;

int H2MirLowerSimpleFunction(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
int H2MirLowerSimpleFunctionWithOptions(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    const H2MirLowerOptions* _Nullable options,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
int H2MirLowerAppendSimpleFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
int H2MirLowerAppendSimpleFunctionWithOptions(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    const H2MirLowerOptions* _Nullable options,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);

H2_API_END
