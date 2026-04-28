#include "libhop-impl.h"
#include "ctfe.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_exec.h"

H2_API_BEGIN

typedef struct {
    H2CTFEResolveIdentFn _Nullable resolveIdent;
    H2CTFEResolveCallFn _Nullable resolveCall;
    void* _Nullable resolveCtx;
} H2CTFEMirResolveAdapterCtx;

static int H2CTFEMirResolveIdentAdapter(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag) {
    const H2CTFEMirResolveAdapterCtx* adapter = (const H2CTFEMirResolveAdapterCtx*)ctx;
    if (adapter == NULL || adapter->resolveIdent == NULL) {
        return 0;
    }
    return adapter->resolveIdent(
        adapter->resolveCtx, nameStart, nameEnd, outValue, outIsConst, diag);
}

static int H2CTFEMirResolveCallAdapter(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2MirExecValue* _Nonnull args,
    uint32_t argCount,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag) {
    const H2CTFEMirResolveAdapterCtx* adapter = (const H2CTFEMirResolveAdapterCtx*)ctx;
    (void)program;
    (void)function;
    (void)inst;
    if (adapter == NULL || adapter->resolveCall == NULL) {
        return 0;
    }
    return adapter->resolveCall(
        adapter->resolveCtx, nameStart, nameEnd, args, argCount, outValue, outIsConst, diag);
}

int H2CTFEEvalExprEx(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    int32_t      nodeId,
    H2CTFEResolveIdentFn _Nullable resolveIdent,
    H2CTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    H2CTFEMakeTupleFn _Nullable makeTuple,
    void* _Nullable makeTupleCtx,
    H2CTFEIndexValueFn _Nullable indexValue,
    void* _Nullable indexValueCtx,
    H2CTFEAggGetFieldFn _Nullable aggGetField,
    void* _Nullable aggGetFieldCtx,
    H2CTFEAggAddrFieldFn _Nullable aggAddrField,
    void* _Nullable aggAddrFieldCtx,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    H2MirProgram               program = { 0 };
    int                        supported = 0;
    H2MirExecEnv               env = { 0 };
    H2CTFEMirResolveAdapterCtx resolveAdapter = {
        .resolveIdent = resolveIdent,
        .resolveCall = resolveCall,
        .resolveCtx = resolveCtx,
    };

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
        diag->phase = H2DiagPhase_CONSTEVAL;
    }
    if (arena == NULL || ast == NULL || outValue == NULL || outIsConst == NULL) {
        if (diag != NULL) {
            H2DiagReset(diag, H2Diag_UNEXPECTED_TOKEN);
            diag->phase = H2DiagPhase_CONSTEVAL;
        }
        return -1;
    }

    *outIsConst = 0;
    if (H2MirLowerExprAsFunction(arena, ast, src, nodeId, &program, &supported, diag) != 0) {
        return -1;
    }
    if (!supported) {
        return 0;
    }
    env.src = src;
    env.resolveIdent = resolveIdent != NULL ? H2CTFEMirResolveIdentAdapter : NULL;
    env.resolveCall = resolveCall != NULL ? H2CTFEMirResolveCallAdapter : NULL;
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
    if (!H2MirProgramNeedsDynamicResolution(&program)) {
        H2MirExecEnvDisableDynamicResolution(&env);
    }
    return H2MirEvalFunction(arena, &program, 0, NULL, 0, &env, outValue, outIsConst);
}

int H2CTFEEvalExpr(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    int32_t      nodeId,
    H2CTFEResolveIdentFn _Nullable resolveIdent,
    H2CTFEResolveCallFn _Nullable resolveCall,
    void* _Nullable resolveCtx,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    return H2CTFEEvalExprEx(
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

int H2CTFEValueToInt64(const H2CTFEValue* value, int64_t* out) {
    if (value == NULL || out == NULL || value->kind != H2CTFEValue_INT) {
        return -1;
    }
    *out = value->i64;
    return 0;
}

H2_API_END
