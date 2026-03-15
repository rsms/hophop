#include "libsl-impl.h"
#include "ctfe.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_exec.h"

SL_API_BEGIN

int SLCTFEEvalExprEx(
    SLArena*     arena,
    const SLAst* ast,
    SLStrView    src,
    int32_t      nodeId,
    SLCTFEResolveIdentFn _Nullable resolveIdent,
    SLCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    SLCTFEMakeTupleFn _Nullable makeTuple,
    void* _Nullable makeTupleCtx,
    SLCTFEIndexValueFn _Nullable indexValue,
    void* _Nullable indexValueCtx,
    SLCTFEAggGetFieldFn _Nullable aggGetField,
    void* _Nullable aggGetFieldCtx,
    SLCTFEAggAddrFieldFn _Nullable aggAddrField,
    void* _Nullable aggAddrFieldCtx,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag) {
    SLMirProgram program = { 0 };
    int          supported = 0;
    SLMirExecEnv env = { 0 };

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
    if (SLMirLowerExprAsFunction(arena, ast, src, nodeId, &program, &supported, diag) != 0) {
        return -1;
    }
    if (!supported) {
        return 0;
    }
    env.src = src;
    env.resolveIdent = resolveIdent;
    env.resolveCall = resolveCall;
    env.resolveCtx = resolveCtx;
    env.makeTuple = makeTuple;
    env.makeTupleCtx = makeTupleCtx;
    env.indexValue = indexValue;
    env.indexValueCtx = indexValueCtx;
    env.aggGetField = aggGetField;
    env.aggGetFieldCtx = aggGetFieldCtx;
    env.aggAddrField = aggAddrField;
    env.aggAddrFieldCtx = aggAddrFieldCtx;
    env.diag = diag;
    return SLMirEvalFunction(arena, &program, 0, NULL, 0, &env, outValue, outIsConst);
}

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
    return SLCTFEEvalExprEx(
        arena,
        ast,
        src,
        nodeId,
        resolveIdent,
        resolveCall,
        resolveCtx,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        NULL,
        outValue,
        outIsConst,
        diag);
}

int SLCTFEValueToInt64(const SLCTFEValue* value, int64_t* out) {
    if (value == NULL || out == NULL || value->kind != SLCTFEValue_INT) {
        return -1;
    }
    *out = value->i64;
    return 0;
}

SL_API_END
