#pragma once
#include "mir.h"

HOP_API_BEGIN

int HOPMirLowerExprAsFunction(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    nodeId,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
int HOPMirLowerAppendExprAsFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    nodeId,
    int32_t    resultTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
int HOPMirLowerAppendInst(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    HOPStrView src,
    const HOPMirInst* _Nonnull in,
    HOPDiag* _Nullable diag);

HOP_API_END
