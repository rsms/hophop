#pragma once
#include "mir.h"

H2_API_BEGIN

int H2MirLowerExprAsFunction(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   nodeId,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
int H2MirLowerAppendExprAsFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   nodeId,
    int32_t   resultTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
int H2MirLowerAppendInst(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    H2StrView src,
    const H2MirInst* _Nonnull in,
    H2Diag* _Nullable diag);

H2_API_END
