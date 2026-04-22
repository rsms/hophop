#include "libhop-impl.h"
#include "ctfe.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_exec.h"

HOP_API_BEGIN

typedef struct {
    HOPCTFEResolveIdentFn _Nullable resolveIdent;
    HOPCTFEResolveCallFn _Nullable resolveCall;
    void* _Nullable resolveCtx;
} HOPCTFEMirResolveAdapterCtx;

static int HOPCTFEMirResolveIdentAdapter(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag) {
    const HOPCTFEMirResolveAdapterCtx* adapter = (const HOPCTFEMirResolveAdapterCtx*)ctx;
    if (adapter == NULL || adapter->resolveIdent == NULL) {
        return 0;
    }
    return adapter->resolveIdent(
        adapter->resolveCtx, nameStart, nameEnd, outValue, outIsConst, diag);
}

static int HOPCTFEMirResolveCallAdapter(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPMirExecValue* _Nonnull args,
    uint32_t argCount,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag) {
    const HOPCTFEMirResolveAdapterCtx* adapter = (const HOPCTFEMirResolveAdapterCtx*)ctx;
    (void)program;
    (void)function;
    (void)inst;
    if (adapter == NULL || adapter->resolveCall == NULL) {
        return 0;
    }
    return adapter->resolveCall(
        adapter->resolveCtx, nameStart, nameEnd, args, argCount, outValue, outIsConst, diag);
}

int HOPCTFEEvalExprEx(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       nodeId,
    HOPCTFEResolveIdentFn _Nullable resolveIdent,
    HOPCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    HOPCTFEMakeTupleFn _Nullable makeTuple,
    void* _Nullable makeTupleCtx,
    HOPCTFEIndexValueFn _Nullable indexValue,
    void* _Nullable indexValueCtx,
    HOPCTFEAggGetFieldFn _Nullable aggGetField,
    void* _Nullable aggGetFieldCtx,
    HOPCTFEAggAddrFieldFn _Nullable aggAddrField,
    void* _Nullable aggAddrFieldCtx,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    HOPMirProgram               program = { 0 };
    int                         supported = 0;
    HOPMirExecEnv               env = { 0 };
    HOPCTFEMirResolveAdapterCtx resolveAdapter = {
        .resolveIdent = resolveIdent,
        .resolveCall = resolveCall,
        .resolveCtx = resolveCtx,
    };

    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (arena == NULL || ast == NULL || outValue == NULL || outIsConst == NULL) {
        if (diag != NULL) {
            diag->code = HOPDiag_UNEXPECTED_TOKEN;
            diag->type = HOPDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }

    *outIsConst = 0;
    if (HOPMirLowerExprAsFunction(arena, ast, src, nodeId, &program, &supported, diag) != 0) {
        return -1;
    }
    if (!supported) {
        return 0;
    }
    env.src = src;
    env.resolveIdent = resolveIdent != NULL ? HOPCTFEMirResolveIdentAdapter : NULL;
    env.resolveCall = resolveCall != NULL ? HOPCTFEMirResolveCallAdapter : NULL;
    env.resolveCtx = &resolveAdapter;
    env.makeTuple = makeTuple;
    env.makeTupleCtx = makeTupleCtx;
    env.indexValue = indexValue;
    env.indexValueCtx = indexValueCtx;
    env.aggGetField = aggGetField;
    env.aggGetFieldCtx = aggGetFieldCtx;
    env.aggAddrField = aggAddrField;
    env.aggAddrFieldCtx = aggAddrFieldCtx;
    env.diag = diag;
    if (!HOPMirProgramNeedsDynamicResolution(&program)) {
        HOPMirExecEnvDisableDynamicResolution(&env);
    }
    return HOPMirEvalFunction(arena, &program, 0, NULL, 0, &env, outValue, outIsConst);
}

int HOPCTFEEvalExpr(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    int32_t       nodeId,
    HOPCTFEResolveIdentFn _Nullable resolveIdent,
    HOPCTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    return HOPCTFEEvalExprEx(
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

int HOPCTFEValueToInt64(const HOPCTFEValue* value, int64_t* out) {
    if (value == NULL || out == NULL || value->kind != HOPCTFEValue_INT) {
        return -1;
    }
    *out = value->i64;
    return 0;
}

HOP_API_END
