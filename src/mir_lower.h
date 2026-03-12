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

SL_API_END
