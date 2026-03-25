#pragma once
#include "mir.h"

SL_API_BEGIN

int SLMirLowerExprAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerAppendExprAsFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerAppendInst(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    SLStrView src,
    const SLMirInst* _Nonnull in,
    SLDiag* _Nullable diag);

SL_API_END
