#include "libsl-impl.h"
#include "ctfe.h"
#include "mir.h"
#include "mir_exec.h"

SL_API_BEGIN

int SLCTFEEvalExpr(
    SLArena*     arena,
    const SLAst* ast,
    SLStrView    src,
    int32_t      nodeId,
    SLCTFEResolveIdentFn _Nullable resolveIdent,
    SLCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag) {
    SLMirChunk chunk;
    int        supported = 0;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (arena == NULL || ast == NULL || outValue == NULL || outIsConst == NULL) {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }

    *outIsConst = 0;
    if (SLMirBuildExpr(arena, ast, src, nodeId, &chunk, &supported, diag) != 0) {
        return -1;
    }
    if (!supported) {
        return 0;
    }
    return SLMirEvalChunk(
        arena, chunk, src, resolveIdent, resolveCall, resolveCtx, outValue, outIsConst, diag);
}

int SLCTFEValueToInt64(const SLCTFEValue* value, int64_t* out) {
    if (value == NULL || out == NULL || value->kind != SLCTFEValue_INT) {
        return -1;
    }
    *out = value->i64;
    return 0;
}

SL_API_END
