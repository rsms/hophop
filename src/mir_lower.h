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
int SLMirLowerZeroInitTypeAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   typeNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerTopInitAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerAppendInst(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    SLStrView src,
    const SLMirInst* _Nonnull in,
    SLDiag* _Nullable diag);

SL_API_END
