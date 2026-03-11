#pragma once
#include "ctfe.h"
#include "mir.h"

SL_API_BEGIN

int SLMirEvalChunk(
    SLArena*   arena,
    SLMirChunk chunk,
    SLStrView  src,
    SLCTFEResolveIdentFn _Nullable resolveIdent,
    SLCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag);

SL_API_END
