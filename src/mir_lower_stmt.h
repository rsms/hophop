#pragma once
#include "mir.h"

SL_API_BEGIN

int SLMirLowerSimpleFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);

SL_API_END
