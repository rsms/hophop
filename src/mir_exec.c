#include "libsl-impl.h"
#include "ctfe.h"
#include "mir.h"
#include "mir_exec.h"

SL_API_BEGIN

typedef struct {
    const SLMirInst*     ip;
    uint32_t             len;
    uint32_t             pc;
    SLMirExecValue*      stack;
    uint32_t             stackLen;
    uint32_t             stackCap;
    SLMirExecValue*      locals;
    uint32_t             localCount;
    SLArena*             arena;
    const SLMirProgram*  program;
    const SLMirFunction* function;
    SLMirExecEnv         env;
    uint32_t             backwardJumpCount;
} SLMirExecRun;

#define SLMIR_EXEC_FUNCTION_REF_TAG_FLAG (UINT64_C(1) << 57)

static void SLCTFESetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end);
static void SLCTFEValueInvalid(SLCTFEValue* v);
static int  SLCTFEPush(SLMirExecRun* r, const SLMirExecValue* v);
static int  SLCTFEPop(SLMirExecRun* r, SLMirExecValue* out);
static int  SLCTFEParseIntLiteral(SLStrView src, uint32_t start, uint32_t end, int64_t* out);
static int  SLCTFEParseFloatLiteral(
     SLArena* arena, SLStrView src, uint32_t start, uint32_t end, double* out);
static int SLCTFEParseBoolLiteral(SLStrView src, uint32_t start, uint32_t end, uint8_t* out);
static int SLCTFEOptionalPayload(const SLCTFEValue* opt, const SLCTFEValue** outPayload);
static int SLCTFEEvalUnary(SLTokenKind op, const SLCTFEValue* in, SLCTFEValue* out);
static int SLCTFEEvalBinary(
    SLMirExecRun*      r,
    SLTokenKind        op,
    const SLCTFEValue* lhs,
    const SLCTFEValue* rhs,
    SLCTFEValue*       out);
static int SLCTFEEvalCast(SLMirCastTarget target, const SLCTFEValue* in, SLCTFEValue* out);
static const SLMirLocal* SLMirGetLocalMeta(const SLMirExecRun* run, uint32_t slot);
static int               SLMirCoerceValueForType(
                  const SLMirExecRun* run, uint32_t typeRefIndex, SLMirExecValue* inOutValue);
static void SLMirSetReason(
    const SLMirExecRun* _Nullable run, const SLMirInst* _Nullable ins, const char* _Nonnull reason);

static int SLMirInitRun(
    SLMirExecRun* _Nonnull run,
    SLArena* _Nonnull arena,
    SLMirChunk chunk,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    uint32_t localCount,
    const SLMirExecValue* _Nullable args,
    uint32_t argCount,
    const SLMirExecEnv* _Nullable env,
    int clearDiag,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    if (clearDiag && env != NULL && env->diag != NULL) {
        *env->diag = (SLDiag){ 0 };
    }
    if (run == NULL || arena == NULL || outValue == NULL || outIsConst == NULL) {
        if (env != NULL) {
            SLCTFESetDiag(env->diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }

    SLCTFEValueInvalid(outValue);
    *outIsConst = 0;

    memset(run, 0, sizeof(*run));
    run->ip = chunk.v;
    run->len = chunk.len;
    run->pc = 0;
    run->stackCap = chunk.len + argCount + 1u;
    run->stack = (SLMirExecValue*)SLArenaAlloc(
        arena, sizeof(SLMirExecValue) * run->stackCap, (uint32_t)_Alignof(SLMirExecValue));
    if (run->stack == NULL) {
        if (env != NULL) {
            SLCTFESetDiag(env->diag, SLDiag_ARENA_OOM, 0, 0);
        }
        return -1;
    }
    run->arena = arena;
    run->program = program;
    run->function = function;
    run->localCount = localCount;
    if (env != NULL) {
        run->env = *env;
    } else {
        memset(&run->env, 0, sizeof(run->env));
    }
    if (localCount != 0u) {
        uint32_t i;
        run->locals = (SLMirExecValue*)SLArenaAlloc(
            arena, sizeof(SLMirExecValue) * localCount, (uint32_t)_Alignof(SLMirExecValue));
        if (run->locals == NULL) {
            if (env != NULL) {
                SLCTFESetDiag(env->diag, SLDiag_ARENA_OOM, 0, 0);
            }
            return -1;
        }
        for (i = 0; i < localCount; i++) {
            SLCTFEValueInvalid(&run->locals[i]);
        }
        if (argCount > localCount) {
            return 0;
        }
        for (i = 0; i < argCount; i++) {
            run->locals[i] = args[i];
            if (program != NULL && function != NULL) {
                const SLMirLocal* local = SLMirGetLocalMeta(run, i);
                if (local->typeRef != UINT32_MAX) {
                    int coerceRc = SLMirCoerceValueForType(run, local->typeRef, &run->locals[i]);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
            }
        }
    } else if (argCount != 0u) {
        return 0;
    }
    return 0;
}

static const SLMirLocal* SLMirGetLocalMeta(const SLMirExecRun* run, uint32_t slot) {
    static const SLMirLocal invalidLocal = { UINT32_MAX, SLMirLocalFlag_NONE, 0u, 0u };
    if (run == NULL || run->program == NULL || run->function == NULL || slot >= run->localCount
        || run->function->localStart > run->program->localLen
        || slot >= run->program->localLen - run->function->localStart)
    {
        return &invalidLocal;
    }
    return &run->program->locals[run->function->localStart + slot];
}

static int SLMirCoerceValueForType(
    const SLMirExecRun* run, uint32_t typeRefIndex, SLMirExecValue* inOutValue) {
    if (run == NULL || inOutValue == NULL || typeRefIndex == UINT32_MAX) {
        return 1;
    }
    if (run->env.coerceValueForType == NULL) {
        return 1;
    }
    if (run->program == NULL || typeRefIndex >= run->program->typeLen) {
        return 0;
    }
    if (run->env.coerceValueForType(
            run->env.coerceValueCtx, &run->program->types[typeRefIndex], inOutValue, run->env.diag)
        != 0)
    {
        return -1;
    }
    return 1;
}

static uint32_t SLMirResolveHostId(const SLMirExecRun* run, const SLMirInst* ins) {
    if (run == NULL || ins == NULL || run->program == NULL || run->program->hostLen == 0
        || ins->aux >= run->program->hostLen)
    {
        return SLMirHostTarget_INVALID;
    }
    return run->program->hosts[ins->aux].target;
}

static uint32_t SLMirResolvedCallArgCount(const SLMirInst* ins) {
    if (ins == NULL) {
        return 0;
    }
    return SLMirCallArgCountFromTok(ins->tok);
}

static int SLMirResolvedCallDropsReceiverArg0(const SLMirInst* ins) {
    return ins != NULL && SLMirCallTokDropsReceiverArg0(ins->tok);
}

static void SLMirSetReason(
    const SLMirExecRun* _Nullable run,
    const SLMirInst* _Nullable ins,
    const char* _Nonnull reason) {
    uint32_t start = 0;
    uint32_t end = 0;
    if (run == NULL || reason == NULL || reason[0] == '\0' || run->env.setReason == NULL) {
        return;
    }
    if (ins != NULL) {
        start = ins->start;
        end = ins->end;
    }
    run->env.setReason(run->env.setReasonCtx, start, end, reason);
}

static SLCTFEValue* _Nullable SLMirReferenceTarget(SLCTFEValue* value) {
    if (value == NULL || value->kind != SLCTFEValue_REFERENCE || value->s.bytes == NULL) {
        return NULL;
    }
    return (SLCTFEValue*)value->s.bytes;
}

static int SLMirEvalFunctionInternal(
    SLArena* _Nonnull arena,
    const SLMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const SLMirExecValue* _Nullable args,
    uint32_t argCount,
    uint16_t callFlags,
    const SLMirExecEnv* _Nullable env,
    int validateProgram,
    int clearDiag,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

static int SLMirConstToValue(const SLMirConst* _Nonnull in, SLMirExecValue* _Nonnull out) {
    double f64 = 0.0;
    if (in == NULL || out == NULL) {
        return 0;
    }
    SLCTFEValueInvalid(out);
    switch (in->kind) {
        case SLMirConst_INT:
            out->kind = SLCTFEValue_INT;
            out->i64 = (int64_t)in->bits;
            return 1;
        case SLMirConst_FLOAT:
            out->kind = SLCTFEValue_FLOAT;
            memcpy(&f64, &in->bits, sizeof(f64));
            out->f64 = f64;
            return 1;
        case SLMirConst_BOOL:
            out->kind = SLCTFEValue_BOOL;
            out->b = in->bits != 0;
            return 1;
        case SLMirConst_STRING:
            out->kind = SLCTFEValue_STRING;
            out->s.bytes = (const uint8_t*)in->bytes.ptr;
            out->s.len = in->bytes.len;
            return 1;
        case SLMirConst_TYPE:
            out->kind = SLCTFEValue_TYPE;
            out->typeTag = in->bits;
            return 1;
        case SLMirConst_FUNCTION: SLMirValueSetFunctionRef(out, (uint32_t)in->bits); return 1;
        case SLMirConst_NULL:     out->kind = SLCTFEValue_NULL; return 1;
        default:                  return 0;
    }
}

void SLMirValueSetFunctionRef(SLMirExecValue* _Nonnull value, uint32_t functionIndex) {
    if (value == NULL) {
        return;
    }
    SLCTFEValueInvalid(value);
    value->kind = SLCTFEValue_TYPE;
    value->typeTag = SLMIR_EXEC_FUNCTION_REF_TAG_FLAG | (uint64_t)functionIndex;
}

static int SLMirValueIsFunctionRef(const SLMirExecValue* value, uint32_t* _Nullable outFnIndex) {
    if (value == NULL || value->kind != SLCTFEValue_TYPE
        || (value->typeTag & SLMIR_EXEC_FUNCTION_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outFnIndex != NULL) {
        *outFnIndex = (uint32_t)(value->typeTag & ~SLMIR_EXEC_FUNCTION_REF_TAG_FLAG);
    }
    return 1;
}

int SLMirValueAsFunctionRef(
    const SLMirExecValue* _Nonnull value, uint32_t* _Nullable outFunctionIndex) {
    return SLMirValueIsFunctionRef(value, outFunctionIndex);
}

void SLMirExecEnvDisableDynamicResolution(SLMirExecEnv* env) {
    if (env == NULL) {
        return;
    }
    env->resolveIdent = NULL;
    env->resolveCall = NULL;
    env->resolveCtx = NULL;
}

static void SLMirResolveSymbolName(
    const SLMirExecRun* _Nonnull run,
    const SLMirInst* _Nonnull ins,
    SLMirSymbolKind expectedKind,
    uint32_t* _Nonnull outStart,
    uint32_t* _Nonnull outEnd) {
    *outStart = 0;
    *outEnd = 0;
    if (run == NULL || ins == NULL || outStart == NULL || outEnd == NULL || run->program == NULL
        || run->program->symbolLen == 0 || ins->aux >= run->program->symbolLen)
    {
        return;
    }
    if (run->program->symbols[ins->aux].kind != expectedKind) {
        return;
    }
    *outStart = run->program->symbols[ins->aux].nameStart;
    *outEnd = run->program->symbols[ins->aux].nameEnd;
}

static void SLMirResolveFieldName(
    const SLMirExecRun* _Nonnull run,
    const SLMirInst* _Nonnull ins,
    uint32_t* _Nonnull outStart,
    uint32_t* _Nonnull outEnd) {
    *outStart = 0;
    *outEnd = 0;
    if (run == NULL || ins == NULL || outStart == NULL || outEnd == NULL || run->program == NULL
        || run->program->fieldLen == 0 || ins->aux >= run->program->fieldLen)
    {
        return;
    }
    *outStart = run->program->fields[ins->aux].nameStart;
    *outEnd = run->program->fields[ins->aux].nameEnd;
}

static int SLMirRunLoop(
    SLMirExecRun* _Nonnull run, SLMirExecValue* _Nonnull outValue, int* _Nonnull outIsConst) {
    while (run->pc < run->len) {
        const SLMirInst* ins = &run->ip[run->pc++];
        switch (ins->op) {
            case SLMirOp_PUSH_CONST: {
                SLMirExecValue v;
                if (run->program == NULL || ins->aux >= run->program->constLen) {
                    return 0;
                }
                if (!SLMirConstToValue(&run->program->consts[ins->aux], &v)) {
                    return 0;
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_PUSH_INT: {
                SLCTFEValue  v;
                uint32_t     rune = 0;
                SLRuneLitErr runeErr = { 0 };
                v.kind = SLCTFEValue_INT;
                v.f64 = 0.0;
                v.b = 0;
                v.typeTag = 0;
                v.s.bytes = NULL;
                v.s.len = 0;
                v.span.fileBytes = NULL;
                v.span.fileLen = 0;
                v.span.startLine = 0;
                v.span.startColumn = 0;
                v.span.endLine = 0;
                v.span.endColumn = 0;
                if ((SLTokenKind)ins->tok == SLTok_RUNE) {
                    if (SLDecodeRuneLiteralValidate(
                            run->env.src.ptr, ins->start, ins->end, &rune, &runeErr)
                        != 0)
                    {
                        SLCTFESetDiag(
                            run->env.diag,
                            SLRuneLitErrDiagCode(runeErr.kind),
                            runeErr.start,
                            runeErr.end);
                        return -1;
                    }
                    v.i64 = (int64_t)rune;
                } else if ((SLTokenKind)ins->tok == SLTok_INVALID) {
                    v.i64 = (int64_t)(int32_t)ins->aux;
                } else {
                    if (SLCTFEParseIntLiteral(run->env.src, ins->start, ins->end, &v.i64) != 0) {
                        return 0;
                    }
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_PUSH_FLOAT: {
                SLCTFEValue v;
                v.kind = SLCTFEValue_FLOAT;
                v.i64 = 0;
                v.b = 0;
                v.typeTag = 0;
                v.s.bytes = NULL;
                v.s.len = 0;
                v.span.fileBytes = NULL;
                v.span.fileLen = 0;
                v.span.startLine = 0;
                v.span.startColumn = 0;
                v.span.endLine = 0;
                v.span.endColumn = 0;
                if (SLCTFEParseFloatLiteral(run->arena, run->env.src, ins->start, ins->end, &v.f64)
                    != 0)
                {
                    return 0;
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_PUSH_BOOL: {
                SLCTFEValue v;
                uint8_t     b = 0;
                if (SLCTFEParseBoolLiteral(run->env.src, ins->start, ins->end, &b) != 0) {
                    return 0;
                }
                v.kind = SLCTFEValue_BOOL;
                v.i64 = 0;
                v.f64 = 0.0;
                v.b = b;
                v.typeTag = 0;
                v.s.bytes = NULL;
                v.s.len = 0;
                v.span.fileBytes = NULL;
                v.span.fileLen = 0;
                v.span.startLine = 0;
                v.span.startColumn = 0;
                v.span.endLine = 0;
                v.span.endColumn = 0;
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_PUSH_STRING: {
                SLCTFEValue    v;
                SLStringLitErr litErr = { 0 };
                uint8_t*       bytes = NULL;
                uint32_t       len = 0;
                if (SLDecodeStringLiteralArena(
                        run->arena, run->env.src.ptr, ins->start, ins->end, &bytes, &len, &litErr)
                    != 0)
                {
                    SLCTFESetDiag(
                        run->env.diag,
                        SLStringLitErrDiagCode(litErr.kind),
                        litErr.start,
                        litErr.end);
                    return -1;
                }
                v.kind = SLCTFEValue_STRING;
                v.i64 = 0;
                v.f64 = 0.0;
                v.b = 0;
                v.typeTag = 0;
                v.s.bytes = bytes;
                v.s.len = len;
                v.span.fileBytes = NULL;
                v.span.fileLen = 0;
                v.span.startLine = 0;
                v.span.startColumn = 0;
                v.span.endLine = 0;
                v.span.endColumn = 0;
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_PUSH_NULL: {
                SLCTFEValue v;
                v.kind = SLCTFEValue_NULL;
                v.i64 = 0;
                v.f64 = 0.0;
                v.b = 0;
                v.typeTag = 0;
                v.s.bytes = NULL;
                v.s.len = 0;
                v.span.fileBytes = NULL;
                v.span.fileLen = 0;
                v.span.startLine = 0;
                v.span.startColumn = 0;
                v.span.endLine = 0;
                v.span.endColumn = 0;
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_LOCAL_LOAD:
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                if (SLCTFEPush(run, &run->locals[ins->aux]) != 0) {
                    return -1;
                }
                break;
            case SLMirOp_LOCAL_ADDR: {
                SLMirExecValue v;
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                SLCTFEValueInvalid(&v);
                v.kind = SLCTFEValue_REFERENCE;
                v.s.bytes = (const uint8_t*)&run->locals[ins->aux];
                v.s.len = 0;
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_ADDR_OF: {
                SLMirExecValue  value;
                SLMirExecValue* target;
                if (SLCTFEPop(run, &value) != 0) {
                    return 0;
                }
                target = (SLMirExecValue*)SLArenaAlloc(
                    run->arena, sizeof(*target), (uint32_t)_Alignof(SLMirExecValue));
                if (target == NULL) {
                    SLCTFESetDiag(run->env.diag, SLDiag_ARENA_OOM, ins->start, ins->end);
                    return -1;
                }
                *target = value;
                SLCTFEValueInvalid(&value);
                value.kind = SLCTFEValue_REFERENCE;
                value.s.bytes = (const uint8_t*)target;
                value.s.len = 0;
                if (SLCTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_LOCAL_ZERO: {
                const SLMirLocal* local;
                SLMirExecValue    v;
                int               isConst = 0;
                if (ins->aux >= run->localCount || run->env.zeroInitLocal == NULL) {
                    return 0;
                }
                local = SLMirGetLocalMeta(run, ins->aux);
                if (local->typeRef == UINT32_MAX || local->typeRef >= run->program->typeLen) {
                    return 0;
                }
                SLCTFEValueInvalid(&v);
                if (run->env.zeroInitLocal(
                        run->env.zeroInitCtx,
                        &run->program->types[local->typeRef],
                        &v,
                        &isConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!isConst) {
                    return 0;
                }
                run->locals[ins->aux] = v;
                break;
            }
            case SLMirOp_DEREF_LOAD: {
                SLMirExecValue ref;
                SLCTFEValue*   target;
                if (SLCTFEPop(run, &ref) != 0) {
                    return 0;
                }
                target = SLMirReferenceTarget(&ref);
                if (target == NULL) {
                    return 0;
                }
                if (SLCTFEPush(run, target) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_DEREF_STORE: {
                SLMirExecValue ref;
                SLMirExecValue v;
                SLCTFEValue*   target;
                if (SLCTFEPop(run, &ref) != 0 || SLCTFEPop(run, &v) != 0) {
                    return 0;
                }
                target = SLMirReferenceTarget(&ref);
                if (target == NULL) {
                    return 0;
                }
                *target = v;
                break;
            }
            case SLMirOp_LOCAL_STORE: {
                const SLMirLocal* local;
                SLMirExecValue    v;
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                if (SLCTFEPop(run, &v) != 0) {
                    return 0;
                }
                local = SLMirGetLocalMeta(run, ins->aux);
                if (local->typeRef != UINT32_MAX) {
                    int coerceRc = SLMirCoerceValueForType(run, local->typeRef, &v);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                run->locals[ins->aux] = v;
                break;
            }
            case SLMirOp_DROP: {
                SLMirExecValue v;
                if (SLCTFEPop(run, &v) != 0) {
                    return 0;
                }
                break;
            }
            case SLMirOp_JUMP:
                if (ins->aux >= run->len) {
                    return 0;
                }
                if (run->env.backwardJumpLimit != 0 && ins->aux < run->pc) {
                    if (++run->backwardJumpCount > run->env.backwardJumpLimit) {
                        SLMirSetReason(run, ins, "for-loop exceeded const-eval iteration limit");
                        return 0;
                    }
                }
                run->pc = ins->aux;
                break;
            case SLMirOp_JUMP_IF_FALSE: {
                SLMirExecValue cond;
                SLMirExecValue condBool;
                if (ins->aux >= run->len) {
                    return 0;
                }
                if (SLCTFEPop(run, &cond) != 0) {
                    return 0;
                }
                if (!SLCTFEEvalCast(SLMirCastTarget_BOOL, &cond, &condBool)) {
                    return 0;
                }
                if (!condBool.b) {
                    if (run->env.backwardJumpLimit != 0 && ins->aux < run->pc) {
                        if (++run->backwardJumpCount > run->env.backwardJumpLimit) {
                            SLMirSetReason(
                                run, ins, "for-loop exceeded const-eval iteration limit");
                            return 0;
                        }
                    }
                    run->pc = ins->aux;
                }
                break;
            }
            case SLMirOp_ASSERT: {
                SLMirExecValue cond;
                SLMirExecValue condBool;
                if (SLCTFEPop(run, &cond) != 0) {
                    return 0;
                }
                if (!SLCTFEEvalCast(SLMirCastTarget_BOOL, &cond, &condBool)) {
                    return 0;
                }
                if (!condBool.b) {
                    SLMirSetReason(
                        run, ins, "assert condition evaluated to false during const evaluation");
                    return 0;
                }
                break;
            }
            case SLMirOp_LOAD_IDENT: {
                SLCTFEValue v;
                int         idIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (run->env.resolveIdent == NULL) {
                    return 0;
                }
                SLMirResolveSymbolName(run, ins, SLMirSymbol_IDENT, &nameStart, &nameEnd);
                SLCTFEValueInvalid(&v);
                if (run->env.resolveIdent(
                        run->env.resolveCtx, nameStart, nameEnd, &v, &idIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!idIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_STORE_IDENT: {
                SLCTFEValue value;
                int         idIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (SLCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (run->env.assignIdent == NULL) {
                    return 0;
                }
                SLMirResolveSymbolName(run, ins, SLMirSymbol_IDENT, &nameStart, &nameEnd);
                if (run->env.assignIdent(
                        run->env.assignIdentCtx,
                        nameStart,
                        nameEnd,
                        &value,
                        &idIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!idIsConst) {
                    return 0;
                }
                break;
            }
            case SLMirOp_CALL: {
                SLCTFEValue* args = NULL;
                SLCTFEValue  v;
                int          callIsConst = 0;
                uint32_t     argCount = SLMirResolvedCallArgCount(ins);
                uint32_t     i;
                uint32_t     nameStart = ins->start;
                uint32_t     nameEnd = ins->end;
                if (run->env.resolveCall == NULL && run->env.resolveCallPre == NULL) {
                    return 0;
                }
                SLMirResolveSymbolName(run, ins, SLMirSymbol_CALL, &nameStart, &nameEnd);
                if (run->env.resolveCallPre != NULL) {
                    int preRc;
                    SLCTFEValueInvalid(&v);
                    preRc = run->env.resolveCallPre(
                        run->env.resolveCtx,
                        run->program,
                        run->function,
                        ins,
                        nameStart,
                        nameEnd,
                        &v,
                        &callIsConst,
                        run->env.diag);
                    if (preRc < 0) {
                        return -1;
                    }
                    if (preRc > 0) {
                        if (!callIsConst) {
                            return 0;
                        }
                        if (SLCTFEPush(run, &v) != 0) {
                            return -1;
                        }
                        break;
                    }
                }
                if (run->env.resolveCall == NULL) {
                    return 0;
                }
                if (argCount > run->stackLen) {
                    return 0;
                }
                if (argCount > 0) {
                    args = (SLCTFEValue*)SLArenaAlloc(
                        run->arena,
                        sizeof(SLCTFEValue) * argCount,
                        (uint32_t)_Alignof(SLCTFEValue));
                    if (args == NULL) {
                        SLCTFESetDiag(run->env.diag, SLDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (SLCTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                SLCTFEValueInvalid(&v);
                if (run->env.resolveCall(
                        run->env.resolveCtx,
                        run->program,
                        run->function,
                        ins,
                        nameStart,
                        nameEnd,
                        args,
                        argCount,
                        &v,
                        &callIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!callIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_CALL_HOST: {
                SLMirExecValue* args = NULL;
                SLMirExecValue  v;
                int             callOk = 0;
                uint32_t        argCount = SLMirResolvedCallArgCount(ins);
                uint32_t        i;
                uint32_t        callArgOffset = 0;
                if (run->env.hostCall == NULL) {
                    return 0;
                }
                if (argCount > run->stackLen) {
                    return 0;
                }
                if (argCount > 0u) {
                    args = (SLMirExecValue*)SLArenaAlloc(
                        run->arena,
                        sizeof(SLMirExecValue) * argCount,
                        (uint32_t)_Alignof(SLMirExecValue));
                    if (args == NULL) {
                        SLCTFESetDiag(run->env.diag, SLDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (SLCTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                SLCTFEValueInvalid(&v);
                if (SLMirResolvedCallDropsReceiverArg0(ins)) {
                    if (argCount == 0u) {
                        return 0;
                    }
                    callArgOffset = 1u;
                }
                if (run->env.hostCall(
                        run->env.hostCtx,
                        SLMirResolveHostId(run, ins),
                        args != NULL ? args + callArgOffset : NULL,
                        argCount - callArgOffset,
                        &v,
                        &callOk,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!callOk) {
                    return 0;
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_CALL_FN: {
                SLMirExecValue  v;
                SLMirExecValue* args = NULL;
                int             callOk = 0;
                uint32_t        argCount = SLMirResolvedCallArgCount(ins);
                uint32_t        i;
                uint32_t        callArgOffset = 0;
                if (run->program == NULL || ins->aux >= run->program->funcLen) {
                    return 0;
                }
                if (argCount > run->stackLen) {
                    return 0;
                }
                if (argCount != 0u) {
                    args = (SLMirExecValue*)SLArenaAlloc(
                        run->arena,
                        sizeof(SLMirExecValue) * argCount,
                        (uint32_t)_Alignof(SLMirExecValue));
                    if (args == NULL) {
                        SLCTFESetDiag(run->env.diag, SLDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (SLCTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                if (SLMirResolvedCallDropsReceiverArg0(ins)) {
                    if (argCount == 0u) {
                        return 0;
                    }
                    callArgOffset = 1u;
                }
                if (run->env.adjustCallArgs != NULL
                    && run->env.adjustCallArgs(
                           run->env.adjustCallArgsCtx,
                           run->program,
                           run->function,
                           ins,
                           ins->aux,
                           args,
                           argCount,
                           run->env.diag)
                           != 0)
                {
                    return -1;
                }
                if (SLMirEvalFunctionInternal(
                        run->arena,
                        run->program,
                        ins->aux,
                        args != NULL ? args + callArgOffset : NULL,
                        argCount - callArgOffset,
                        ins->tok,
                        &run->env,
                        0,
                        0,
                        &v,
                        &callOk)
                    != 0)
                {
                    return -1;
                }
                if (!callOk) {
                    return 0;
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_CALL_INDIRECT: {
                SLMirExecValue  callee;
                SLMirExecValue  v;
                SLMirExecValue* args = NULL;
                int             callOk = 0;
                uint32_t        fnIndex = 0;
                uint32_t        argCount = SLMirResolvedCallArgCount(ins);
                uint32_t        i;
                if (run->program == NULL || argCount + 1u > run->stackLen) {
                    SLMirSetReason(
                        run, ins, "indirect call stack is invalid during const evaluation");
                    return 0;
                }
                if (argCount != 0u) {
                    args = (SLMirExecValue*)SLArenaAlloc(
                        run->arena,
                        sizeof(SLMirExecValue) * argCount,
                        (uint32_t)_Alignof(SLMirExecValue));
                    if (args == NULL) {
                        SLCTFESetDiag(run->env.diag, SLDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (SLCTFEPop(run, &args[i - 1]) != 0) {
                        SLMirSetReason(
                            run,
                            ins,
                            "indirect call arguments are not available during const evaluation");
                        return 0;
                    }
                }
                if (SLCTFEPop(run, &callee) != 0) {
                    SLMirSetReason(
                        run, ins, "indirect call target is not available during const evaluation");
                    return 0;
                }
                if (!SLMirValueIsFunctionRef(&callee, &fnIndex) || fnIndex >= run->program->funcLen)
                {
                    SLMirSetReason(run, ins, "indirect call target is not a function");
                    return 0;
                }
                if (run->env.adjustCallArgs != NULL
                    && run->env.adjustCallArgs(
                           run->env.adjustCallArgsCtx,
                           run->program,
                           run->function,
                           ins,
                           fnIndex,
                           args,
                           argCount,
                           run->env.diag)
                           != 0)
                {
                    return -1;
                }
                if (SLMirEvalFunctionInternal(
                        run->arena,
                        run->program,
                        fnIndex,
                        args,
                        argCount,
                        ins->tok,
                        &run->env,
                        0,
                        0,
                        &v,
                        &callOk)
                    != 0)
                {
                    return -1;
                }
                if (!callOk) {
                    return 0;
                }
                if (SLCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_UNARY: {
                SLCTFEValue in;
                SLCTFEValue out;
                if (SLCTFEPop(run, &in) != 0) {
                    return 0;
                }
                if (!SLCTFEEvalUnary((SLTokenKind)ins->tok, &in, &out)) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_BINARY: {
                SLCTFEValue lhs;
                SLCTFEValue rhs;
                SLCTFEValue out;
                int         binaryRc;
                if (SLCTFEPop(run, &rhs) != 0 || SLCTFEPop(run, &lhs) != 0) {
                    return 0;
                }
                binaryRc = SLCTFEEvalBinary(run, (SLTokenKind)ins->tok, &lhs, &rhs, &out);
                if (binaryRc < 0) {
                    return -1;
                }
                if (binaryRc == 0) {
                    SLMirSetReason(
                        run, ins, "binary operation is not supported during const evaluation");
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_INDEX: {
                SLCTFEValue base;
                SLCTFEValue idx;
                SLCTFEValue out;
                int64_t     idxInt = 0;
                int         indexIsConst = 0;
                if (SLCTFEPop(run, &idx) != 0 || SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (base.kind == SLCTFEValue_STRING && SLCTFEValueToInt64(&idx, &idxInt) == 0) {
                    if (idxInt < 0) {
                        SLMirSetReason(run, ins, "index is negative in const evaluation");
                        return 0;
                    }
                    if ((uint64_t)idxInt >= (uint64_t)base.s.len) {
                        SLMirSetReason(run, ins, "index is out of bounds in const evaluation");
                        return 0;
                    }
                    out.kind = SLCTFEValue_INT;
                    out.i64 = (int64_t)base.s.bytes[(uint32_t)idxInt];
                    out.f64 = 0.0;
                    out.b = 0;
                    out.typeTag = 0;
                    out.s.bytes = NULL;
                    out.s.len = 0;
                    out.span.fileBytes = NULL;
                    out.span.fileLen = 0;
                    out.span.startLine = 0;
                    out.span.startColumn = 0;
                    out.span.endLine = 0;
                    out.span.endColumn = 0;
                    if (SLCTFEPush(run, &out) != 0) {
                        return -1;
                    }
                    break;
                }
                if (run->env.indexValue == NULL) {
                    return 0;
                }
                SLCTFEValueInvalid(&out);
                if (run->env.indexValue(
                        run->env.indexValueCtx, &base, &idx, &out, &indexIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!indexIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_SEQ_LEN: {
                SLCTFEValue base;
                SLCTFEValue out;
                int         lenIsConst = 0;
                if (SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (base.kind == SLCTFEValue_STRING || base.kind == SLCTFEValue_ARRAY
                    || base.kind == SLCTFEValue_NULL)
                {
                    out.kind = SLCTFEValue_INT;
                    out.i64 = (int64_t)base.s.len;
                    out.f64 = 0.0;
                    out.b = 0;
                    out.typeTag = 0;
                    out.s.bytes = NULL;
                    out.s.len = 0;
                    out.span.fileBytes = NULL;
                    out.span.fileLen = 0;
                    out.span.startLine = 0;
                    out.span.startColumn = 0;
                    out.span.endLine = 0;
                    out.span.endColumn = 0;
                    if (SLCTFEPush(run, &out) != 0) {
                        return -1;
                    }
                    break;
                }
                if (run->env.sequenceLen == NULL) {
                    return 0;
                }
                SLCTFEValueInvalid(&out);
                if (run->env.sequenceLen(
                        run->env.sequenceLenCtx, &base, &out, &lenIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!lenIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_STR_CSTR: {
                SLCTFEValue  base;
                SLCTFEValue* target;
                if (SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                target = SLMirReferenceTarget(&base);
                if (target != NULL) {
                    base = *target;
                }
                if (base.kind != SLCTFEValue_STRING) {
                    return 0;
                }
                if (SLCTFEPush(run, &base) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_ARRAY_ADDR: {
                SLCTFEValue base;
                SLCTFEValue idx;
                SLCTFEValue out;
                int         addrIsConst = 0;
                if (SLCTFEPop(run, &idx) != 0 || SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.indexAddr == NULL) {
                    return 0;
                }
                SLCTFEValueInvalid(&out);
                if (run->env.indexAddr(
                        run->env.indexAddrCtx, &base, &idx, &out, &addrIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!addrIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_SLICE_MAKE: {
                SLCTFEValue  base;
                SLCTFEValue  startValue;
                SLCTFEValue  endValue;
                SLCTFEValue  out;
                SLCTFEValue* startPtr = NULL;
                SLCTFEValue* endPtr = NULL;
                int          sliceIsConst = 0;
                uint16_t     sliceFlags = ins->tok;
                if (run->env.sliceValue == NULL) {
                    return 0;
                }
                if ((sliceFlags & SLAstFlag_INDEX_HAS_END) != 0u) {
                    if (SLCTFEPop(run, &endValue) != 0) {
                        return 0;
                    }
                    endPtr = &endValue;
                }
                if ((sliceFlags & SLAstFlag_INDEX_HAS_START) != 0u) {
                    if (SLCTFEPop(run, &startValue) != 0) {
                        return 0;
                    }
                    startPtr = &startValue;
                }
                if (SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                SLCTFEValueInvalid(&out);
                if (run->env.sliceValue(
                        run->env.sliceValueCtx,
                        &base,
                        startPtr,
                        endPtr,
                        sliceFlags,
                        &out,
                        &sliceIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!sliceIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_AGG_MAKE: {
                SLCTFEValue out;
                int         aggIsConst = 0;
                if (run->env.makeAggregate == NULL) {
                    return 0;
                }
                SLCTFEValueInvalid(&out);
                if (run->env.makeAggregate(
                        run->env.makeAggregateCtx,
                        ins->aux,
                        (uint32_t)ins->tok,
                        &out,
                        &aggIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!aggIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_AGG_ZERO: {
                SLCTFEValue out;
                int         aggIsConst = 0;
                if (run->program == NULL || run->env.zeroInitLocal == NULL
                    || ins->aux >= run->program->typeLen)
                {
                    return 0;
                }
                SLCTFEValueInvalid(&out);
                if (run->env.zeroInitLocal(
                        run->env.zeroInitCtx,
                        &run->program->types[ins->aux],
                        &out,
                        &aggIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!aggIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_AGG_SET: {
                SLCTFEValue value;
                SLCTFEValue base;
                int         fieldIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (SLCTFEPop(run, &value) != 0 || SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggSetField == NULL) {
                    return 0;
                }
                SLMirResolveFieldName(run, ins, &nameStart, &nameEnd);
                if (run->env.aggSetField(
                        run->env.aggSetFieldCtx,
                        &base,
                        nameStart,
                        nameEnd,
                        &value,
                        &fieldIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!fieldIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &base) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_AGG_GET: {
                SLCTFEValue base;
                SLCTFEValue out;
                int         fieldIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggGetField == NULL) {
                    return 0;
                }
                SLMirResolveFieldName(run, ins, &nameStart, &nameEnd);
                SLCTFEValueInvalid(&out);
                if (run->env.aggGetField(
                        run->env.aggGetFieldCtx,
                        &base,
                        nameStart,
                        nameEnd,
                        &out,
                        &fieldIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!fieldIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_AGG_ADDR: {
                SLCTFEValue base;
                SLCTFEValue out;
                int         fieldIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (SLCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggAddrField == NULL) {
                    return 0;
                }
                SLMirResolveFieldName(run, ins, &nameStart, &nameEnd);
                SLCTFEValueInvalid(&out);
                if (run->env.aggAddrField(
                        run->env.aggAddrFieldCtx,
                        &base,
                        nameStart,
                        nameEnd,
                        &out,
                        &fieldIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!fieldIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_TUPLE_MAKE: {
                SLMirExecValue* elems = NULL;
                SLMirExecValue  out;
                int             tupleIsConst = 0;
                uint32_t        elemCount = (uint32_t)ins->tok;
                uint32_t        i;
                if (run->env.makeTuple == NULL || elemCount > run->stackLen) {
                    return 0;
                }
                if (elemCount != 0u) {
                    elems = (SLMirExecValue*)SLArenaAlloc(
                        run->arena,
                        sizeof(SLMirExecValue) * elemCount,
                        (uint32_t)_Alignof(SLMirExecValue));
                    if (elems == NULL) {
                        SLCTFESetDiag(run->env.diag, SLDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = elemCount; i > 0; i--) {
                    if (SLCTFEPop(run, &elems[i - 1u]) != 0) {
                        return 0;
                    }
                }
                SLCTFEValueInvalid(&out);
                if (run->env.makeTuple(
                        run->env.makeTupleCtx,
                        elems,
                        elemCount,
                        ins->aux,
                        &out,
                        &tupleIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!tupleIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_ITER_INIT: {
                SLCTFEValue source;
                SLCTFEValue iter;
                int         iterIsConst = 0;
                if (SLCTFEPop(run, &source) != 0) {
                    return 0;
                }
                if (run->env.iterInit == NULL) {
                    return 0;
                }
                SLCTFEValueInvalid(&iter);
                if (run->env.iterInit(
                        run->env.iterInitCtx,
                        ins->aux,
                        &source,
                        ins->tok,
                        &iter,
                        &iterIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!iterIsConst) {
                    return 0;
                }
                if (SLCTFEPush(run, &iter) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_ITER_NEXT: {
                SLCTFEValue iter;
                SLCTFEValue key;
                SLCTFEValue value;
                SLCTFEValue hasItemValue;
                int         hasItem = 0;
                int         keyIsConst = 0;
                int         valueIsConst = 0;
                if (SLCTFEPop(run, &iter) != 0) {
                    return 0;
                }
                if (run->env.iterNext == NULL) {
                    return 0;
                }
                SLCTFEValueInvalid(&key);
                SLCTFEValueInvalid(&value);
                if (run->env.iterNext(
                        run->env.iterNextCtx,
                        &iter,
                        ins->tok,
                        &hasItem,
                        &key,
                        &keyIsConst,
                        &value,
                        &valueIsConst,
                        run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (hasItem) {
                    if ((ins->tok & SLMirIterFlag_HAS_KEY) != 0u && !keyIsConst) {
                        return 0;
                    }
                    if ((ins->tok & SLMirIterFlag_VALUE_DISCARD) == 0u && !valueIsConst) {
                        return 0;
                    }
                    if ((ins->tok & SLMirIterFlag_HAS_KEY) != 0u) {
                        if (SLCTFEPush(run, &key) != 0) {
                            return -1;
                        }
                    }
                    if ((ins->tok & SLMirIterFlag_VALUE_DISCARD) == 0u) {
                        if (SLCTFEPush(run, &value) != 0) {
                            return -1;
                        }
                    }
                }
                hasItemValue.kind = SLCTFEValue_BOOL;
                hasItemValue.i64 = 0;
                hasItemValue.f64 = 0.0;
                hasItemValue.b = hasItem ? 1u : 0u;
                hasItemValue.typeTag = 0;
                hasItemValue.s.bytes = NULL;
                hasItemValue.s.len = 0;
                hasItemValue.span = (SLCTFESpan){ 0 };
                if (SLCTFEPush(run, &hasItemValue) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_CAST: {
                SLCTFEValue in;
                SLCTFEValue out;
                if (SLCTFEPop(run, &in) != 0) {
                    return 0;
                }
                if (!SLCTFEEvalCast((SLMirCastTarget)ins->tok, &in, &out)) {
                    return 0;
                }
                {
                    int coerceRc = SLMirCoerceValueForType(run, ins->aux, &out);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_COERCE: {
                SLCTFEValue value;
                int         coerceRc;
                if (SLCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (value.kind == SLCTFEValue_AGGREGATE) {
                    value.typeTag |= SLCTFEValueTag_AGG_PARTIAL;
                }
                coerceRc = SLMirCoerceValueForType(run, ins->aux, &value);
                if (coerceRc <= 0) {
                    return coerceRc < 0 ? -1 : 0;
                }
                if (SLCTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_OPTIONAL_WRAP: {
                SLCTFEValue  value;
                SLCTFEValue* payloadCopy;
                if (SLCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (value.kind == SLCTFEValue_OPTIONAL) {
                    if (SLCTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                if (value.kind == SLCTFEValue_NULL) {
                    value.kind = SLCTFEValue_OPTIONAL;
                    value.i64 = 0;
                    value.f64 = 0.0;
                    value.b = 0u;
                    value.typeTag = 0;
                    value.s.bytes = NULL;
                    value.s.len = 0;
                    if (SLCTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                payloadCopy = (SLCTFEValue*)SLArenaAlloc(
                    run->arena, sizeof(*payloadCopy), (uint32_t)_Alignof(SLCTFEValue));
                if (payloadCopy == NULL) {
                    SLCTFESetDiag(run->env.diag, SLDiag_ARENA_OOM, ins->start, ins->end);
                    return -1;
                }
                *payloadCopy = value;
                value.kind = SLCTFEValue_OPTIONAL;
                value.i64 = 0;
                value.f64 = 0.0;
                value.b = 1u;
                value.typeTag = 0;
                value.s.bytes = (const uint8_t*)payloadCopy;
                value.s.len = 0;
                if (SLCTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_OPTIONAL_UNWRAP: {
                SLCTFEValue        value;
                const SLCTFEValue* payload = NULL;
                if (SLCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (!SLCTFEOptionalPayload(&value, &payload)) {
                    if (value.kind == SLCTFEValue_NULL) {
                        SLMirSetReason(run, ins, "unwrap of empty optional in evaluator backend");
                        return 0;
                    }
                    if (SLCTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                if (value.b == 0u || payload == NULL) {
                    SLMirSetReason(run, ins, "unwrap of empty optional in evaluator backend");
                    return 0;
                }
                if (SLCTFEPush(run, payload) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_ALLOC_NEW: {
                SLCTFEValue out;
                int         allocRc;
                int         allocIsConst = 0;
                if (run->env.allocNew == NULL) {
                    SLMirSetReason(
                        run, ins, "new expression is not supported during const evaluation");
                    return 0;
                }
                allocRc = run->env.allocNew(
                    run->env.allocNewCtx, ins->aux, &out, &allocIsConst, run->env.diag);
                if (allocRc < 0) {
                    return -1;
                }
                if (allocRc == 0 || !allocIsConst) {
                    SLMirSetReason(
                        run, ins, "new expression is not supported during const evaluation");
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_CTX_GET: {
                SLCTFEValue out;
                int         getRc;
                int         isConst = 0;
                if (run->env.contextGet == NULL) {
                    SLMirSetReason(
                        run, ins, "context access is not supported during const evaluation");
                    return 0;
                }
                getRc = run->env.contextGet(
                    run->env.contextGetCtx, ins->aux, &out, &isConst, run->env.diag);
                if (getRc < 0) {
                    return -1;
                }
                if (getRc == 0 || !isConst) {
                    SLMirSetReason(
                        run, ins, "context access is not supported during const evaluation");
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_CTX_SET: {
                SLCTFEValue out;
                int         evalRc;
                int         isConst = 0;
                if (run->env.evalWithContext == NULL) {
                    SLMirSetReason(
                        run, ins, "context overlay is not supported during const evaluation");
                    return 0;
                }
                evalRc = run->env.evalWithContext(
                    run->env.evalWithContextCtx, ins->aux, &out, &isConst, run->env.diag);
                if (evalRc < 0) {
                    return -1;
                }
                if (evalRc == 0 || !isConst) {
                    SLMirSetReason(
                        run, ins, "context overlay is not supported during const evaluation");
                    return 0;
                }
                if (SLCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case SLMirOp_RETURN:
                if (run->stackLen != 1) {
                    return 0;
                }
                *outValue = run->stack[0];
                if (run->function != NULL && run->function->typeRef != UINT32_MAX) {
                    int coerceRc = SLMirCoerceValueForType(run, run->function->typeRef, outValue);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                *outIsConst = 1;
                return 0;
            case SLMirOp_RETURN_VOID:
                SLCTFEValueInvalid(outValue);
                *outIsConst = 1;
                return 0;
            default: return 0;
        }
    }
    return 0;
}

static void SLCTFESetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

static void SLCTFEValueInvalid(SLCTFEValue* v) {
    v->kind = SLCTFEValue_INVALID;
    v->i64 = 0;
    v->f64 = 0.0;
    v->b = 0;
    v->typeTag = 0;
    v->s.bytes = NULL;
    v->s.len = 0;
    v->span.fileBytes = NULL;
    v->span.fileLen = 0;
    v->span.startLine = 0;
    v->span.startColumn = 0;
    v->span.endLine = 0;
    v->span.endColumn = 0;
}

static int SLCTFEPush(SLMirExecRun* r, const SLMirExecValue* v) {
    if (r->stackLen >= r->stackCap) {
        SLCTFESetDiag(r->env.diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    r->stack[r->stackLen++] = *v;
    return 0;
}

static int SLCTFEPop(SLMirExecRun* r, SLMirExecValue* out) {
    if (r->stackLen == 0) {
        return -1;
    }
    *out = r->stack[--r->stackLen];
    return 0;
}

static int SLCTFEParseIntLiteral(SLStrView src, uint32_t start, uint32_t end, int64_t* out) {
    uint64_t v = 0;
    uint32_t i;
    uint32_t base = 10;
    if (end <= start || end > src.len) {
        return -1;
    }
    if (end - start >= 3 && src.ptr[start] == '0'
        && (src.ptr[start + 1] == 'x' || src.ptr[start + 1] == 'X'))
    {
        base = 16;
        start += 2;
        if (end <= start) {
            return -1;
        }
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)src.ptr[i];
        uint32_t      digit;
        if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') {
            digit = (uint32_t)(ch - (unsigned char)'0');
        } else if (base == 16 && ch >= (unsigned char)'a' && ch <= (unsigned char)'f') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'a');
        } else if (base == 16 && ch >= (unsigned char)'A' && ch <= (unsigned char)'F') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'A');
        } else {
            return -1;
        }
        if (digit >= base) {
            return -1;
        }
        if (v > (uint64_t)INT64_MAX / (uint64_t)base
            || (v == (uint64_t)INT64_MAX / (uint64_t)base
                && (uint64_t)digit > (uint64_t)INT64_MAX % (uint64_t)base))
        {
            return -1;
        }
        v = v * (uint64_t)base + (uint64_t)digit;
    }
    *out = (int64_t)v;
    return 0;
}

static int SLCTFEParseFloatLiteral(
    SLArena* arena, SLStrView src, uint32_t start, uint32_t end, double* out) {
    uint32_t i;
    double   v = 0.0;
    int      sawDigit = 0;
    int32_t  expSign = 1;
    uint32_t expValue = 0;
    uint32_t expIter;
    if (arena == NULL || out == NULL || end <= start || end > src.len) {
        return -1;
    }
    (void)arena;

    i = start;
    while (i < end && (unsigned char)src.ptr[i] >= (unsigned char)'0'
           && (unsigned char)src.ptr[i] <= (unsigned char)'9')
    {
        v = v * 10.0 + (double)(src.ptr[i] - '0');
        sawDigit = 1;
        i++;
    }
    if (i < end && src.ptr[i] == '.') {
        double place = 0.1;
        i++;
        while (i < end && (unsigned char)src.ptr[i] >= (unsigned char)'0'
               && (unsigned char)src.ptr[i] <= (unsigned char)'9')
        {
            v += (double)(src.ptr[i] - '0') * place;
            place *= 0.1;
            sawDigit = 1;
            i++;
        }
    }
    if (!sawDigit) {
        return -1;
    }
    if (i < end && (src.ptr[i] == 'e' || src.ptr[i] == 'E')) {
        i++;
        if (i < end && (src.ptr[i] == '+' || src.ptr[i] == '-')) {
            expSign = src.ptr[i] == '-' ? -1 : 1;
            i++;
        }
        if (i >= end || (unsigned char)src.ptr[i] < (unsigned char)'0'
            || (unsigned char)src.ptr[i] > (unsigned char)'9')
        {
            return -1;
        }
        while (i < end && (unsigned char)src.ptr[i] >= (unsigned char)'0'
               && (unsigned char)src.ptr[i] <= (unsigned char)'9')
        {
            if (expValue > UINT32_MAX / 10u) {
                return -1;
            }
            expValue = expValue * 10u + (uint32_t)(src.ptr[i] - '0');
            i++;
        }
    }
    if (i != end) {
        return -1;
    }
    for (expIter = 0; expIter < expValue; expIter++) {
        if (expSign < 0) {
            v /= 10.0;
        } else {
            v *= 10.0;
        }
    }
    *out = v;
    return 0;
}

static int SLCTFEParseBoolLiteral(SLStrView src, uint32_t start, uint32_t end, uint8_t* out) {
    uint32_t len = end > start ? end - start : 0;
    if (len == 4 && memcmp(src.ptr + start, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (len == 5 && memcmp(src.ptr + start, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

static int SLCTFEStringEq(const SLCTFEString* a, const SLCTFEString* b) {
    if (a->len != b->len) {
        return 0;
    }
    if (a->len == 0) {
        return 1;
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

static int SLCTFEOptionalPayload(const SLCTFEValue* opt, const SLCTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (opt == NULL || opt->kind != SLCTFEValue_OPTIONAL || outPayload == NULL) {
        return 0;
    }
    if (opt->b == 0u) {
        return 1;
    }
    if (opt->s.bytes == NULL) {
        return 0;
    }
    *outPayload = (const SLCTFEValue*)opt->s.bytes;
    return 1;
}

static int SLCTFEValueEqRec(const SLCTFEValue* a, const SLCTFEValue* b, uint32_t depth) {
    const SLCTFEValue* aPayload = NULL;
    const SLCTFEValue* bPayload = NULL;
    if (a == NULL || b == NULL || a->kind != b->kind || depth > 32u) {
        return 0;
    }
    switch (a->kind) {
        case SLCTFEValue_INT:    return a->i64 == b->i64;
        case SLCTFEValue_FLOAT:  return a->f64 == b->f64;
        case SLCTFEValue_BOOL:   return a->b == b->b;
        case SLCTFEValue_STRING: return SLCTFEStringEq(&a->s, &b->s);
        case SLCTFEValue_TYPE:   return a->typeTag == b->typeTag;
        case SLCTFEValue_SPAN:
            if (a->span.startLine != b->span.startLine || a->span.startColumn != b->span.startColumn
                || a->span.endLine != b->span.endLine || a->span.endColumn != b->span.endColumn
                || a->span.fileLen != b->span.fileLen)
            {
                return 0;
            }
            if (a->span.fileLen == 0) {
                return 1;
            }
            return a->span.fileBytes != NULL && b->span.fileBytes != NULL
                && memcmp(a->span.fileBytes, b->span.fileBytes, a->span.fileLen) == 0;
        case SLCTFEValue_NULL: return 1;
        case SLCTFEValue_OPTIONAL:
            if (!SLCTFEOptionalPayload(a, &aPayload) || !SLCTFEOptionalPayload(b, &bPayload)) {
                return 0;
            }
            if (a->b == 0u || b->b == 0u) {
                return a->b == b->b;
            }
            return SLCTFEValueEqRec(aPayload, bPayload, depth + 1u);
        default: return 0;
    }
}

static int SLCTFEStringConcat(
    SLMirExecRun* r, const SLCTFEString* a, const SLCTFEString* b, SLCTFEString* out) {
    uint64_t total64 = (uint64_t)a->len + (uint64_t)b->len;
    uint32_t totalLen;
    uint8_t* dst;
    if (total64 > UINT32_MAX) {
        return -1;
    }
    totalLen = (uint32_t)total64;
    if (totalLen == 0) {
        out->bytes = NULL;
        out->len = 0;
        return 0;
    }
    dst = (uint8_t*)SLArenaAlloc(r->arena, totalLen, (uint32_t)_Alignof(uint8_t));
    if (dst == NULL) {
        SLCTFESetDiag(r->env.diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    if (a->len > 0) {
        memcpy(dst, a->bytes, a->len);
    }
    if (b->len > 0) {
        memcpy(dst + a->len, b->bytes, b->len);
    }
    out->bytes = dst;
    out->len = totalLen;
    return 0;
}

static int SLCTFEEvalUnary(SLTokenKind op, const SLCTFEValue* in, SLCTFEValue* out) {
    SLCTFEValueInvalid(out);
    if (op == SLTok_ADD && in->kind == SLCTFEValue_INT) {
        *out = *in;
        return 1;
    }
    if (op == SLTok_ADD && in->kind == SLCTFEValue_FLOAT) {
        *out = *in;
        return 1;
    }
    if (op == SLTok_SUB && in->kind == SLCTFEValue_INT) {
        if (in->i64 == INT64_MIN) {
            return 0;
        }
        out->kind = SLCTFEValue_INT;
        out->i64 = -in->i64;
        return 1;
    }
    if (op == SLTok_SUB && in->kind == SLCTFEValue_FLOAT) {
        out->kind = SLCTFEValue_FLOAT;
        out->f64 = -in->f64;
        return 1;
    }
    if (op == SLTok_NOT && in->kind == SLCTFEValue_BOOL) {
        out->kind = SLCTFEValue_BOOL;
        out->b = in->b ? 0u : 1u;
        return 1;
    }
    return 0;
}

static int SLCTFEValueToF64(const SLCTFEValue* value, double* out) {
    if (value == NULL || out == NULL) {
        return 0;
    }
    if (value->kind == SLCTFEValue_INT) {
        *out = (double)value->i64;
        return 1;
    }
    if (value->kind == SLCTFEValue_FLOAT) {
        *out = value->f64;
        return 1;
    }
    return 0;
}

static int SLCTFEAddI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_add_overflow)
    if (__builtin_add_overflow(a, b, out)) {
        return -1;
    }
    return 0;
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return -1;
    }
    *out = a + b;
    return 0;
#endif
}

static int SLCTFESubI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_sub_overflow)
    if (__builtin_sub_overflow(a, b, out)) {
        return -1;
    }
    return 0;
#else
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) {
        return -1;
    }
    *out = a - b;
    return 0;
#endif
}

static int SLCTFEMulI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_mul_overflow)
    if (__builtin_mul_overflow(a, b, out)) {
        return -1;
    }
    return 0;
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if (a == -1 && b == INT64_MIN) {
        return -1;
    }
    if (b == -1 && a == INT64_MIN) {
        return -1;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return -1;
            }
        } else {
            if (b < INT64_MIN / a) {
                return -1;
            }
        }
    } else {
        if (b > 0) {
            if (a < INT64_MIN / b) {
                return -1;
            }
        } else {
            if (a != 0 && b < INT64_MAX / a) {
                return -1;
            }
        }
    }
    *out = a * b;
    return 0;
#endif
}

static int SLCTFEEvalBinary(
    SLMirExecRun*      r,
    SLTokenKind        op,
    const SLCTFEValue* lhs,
    const SLCTFEValue* rhs,
    SLCTFEValue*       out) {
    int64_t            i = 0;
    double             lf = 0.0;
    double             rf = 0.0;
    const SLCTFEValue* lhsPayload = NULL;
    const SLCTFEValue* rhsPayload = NULL;
    SLCTFEValueInvalid(out);

    if (lhs == NULL || rhs == NULL) {
        return 0;
    }
    if (lhs->kind == SLCTFEValue_OPTIONAL && rhs->kind != SLCTFEValue_OPTIONAL
        && rhs->kind != SLCTFEValue_NULL)
    {
        if (!SLCTFEOptionalPayload(lhs, &lhsPayload) || lhs->b == 0u || lhsPayload == NULL) {
            return 0;
        }
        return SLCTFEEvalBinary(r, op, lhsPayload, rhs, out);
    }
    if (rhs->kind == SLCTFEValue_OPTIONAL && lhs->kind != SLCTFEValue_OPTIONAL
        && lhs->kind != SLCTFEValue_NULL)
    {
        if (!SLCTFEOptionalPayload(rhs, &rhsPayload) || rhs->b == 0u || rhsPayload == NULL) {
            return 0;
        }
        return SLCTFEEvalBinary(r, op, lhs, rhsPayload, out);
    }

    if (lhs->kind == SLCTFEValue_INT && rhs->kind == SLCTFEValue_INT) {
        switch (op) {
            case SLTok_ADD:
                if (SLCTFEAddI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = SLCTFEValue_INT;
                out->i64 = i;
                return 1;
            case SLTok_SUB:
                if (SLCTFESubI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = SLCTFEValue_INT;
                out->i64 = i;
                return 1;
            case SLTok_MUL:
                if (SLCTFEMulI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = SLCTFEValue_INT;
                out->i64 = i;
                return 1;
            case SLTok_DIV:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                out->kind = SLCTFEValue_INT;
                out->i64 = lhs->i64 / rhs->i64;
                return 1;
            case SLTok_MOD:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                out->kind = SLCTFEValue_INT;
                out->i64 = lhs->i64 % rhs->i64;
                return 1;
            case SLTok_AND:
                out->kind = SLCTFEValue_INT;
                out->i64 = lhs->i64 & rhs->i64;
                return 1;
            case SLTok_OR:
                out->kind = SLCTFEValue_INT;
                out->i64 = lhs->i64 | rhs->i64;
                return 1;
            case SLTok_XOR:
                out->kind = SLCTFEValue_INT;
                out->i64 = lhs->i64 ^ rhs->i64;
                return 1;
            case SLTok_LSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                out->kind = SLCTFEValue_INT;
                out->i64 = (int64_t)((uint64_t)lhs->i64 << (uint32_t)rhs->i64);
                return 1;
            case SLTok_RSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                out->kind = SLCTFEValue_INT;
                out->i64 = (int64_t)((uint64_t)lhs->i64 >> (uint32_t)rhs->i64);
                return 1;
            case SLTok_EQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->i64 == rhs->i64;
                return 1;
            case SLTok_NEQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->i64 != rhs->i64;
                return 1;
            case SLTok_LT:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->i64 < rhs->i64;
                return 1;
            case SLTok_GT:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->i64 > rhs->i64;
                return 1;
            case SLTok_LTE:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->i64 <= rhs->i64;
                return 1;
            case SLTok_GTE:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->i64 >= rhs->i64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == SLCTFEValue_BOOL && rhs->kind == SLCTFEValue_BOOL) {
        switch (op) {
            case SLTok_LOGICAL_AND:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->b && rhs->b;
                return 1;
            case SLTok_LOGICAL_OR:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->b || rhs->b;
                return 1;
            case SLTok_EQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->b == rhs->b;
                return 1;
            case SLTok_NEQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->b != rhs->b;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == SLCTFEValue_STRING && rhs->kind == SLCTFEValue_STRING) {
        int cmp = 0;
        if (lhs->s.len != rhs->s.len) {
            uint32_t minLen = lhs->s.len < rhs->s.len ? lhs->s.len : rhs->s.len;
            if (minLen > 0u) {
                cmp = memcmp(lhs->s.bytes, rhs->s.bytes, minLen);
            }
            if (cmp == 0) {
                cmp = lhs->s.len < rhs->s.len ? -1 : 1;
            }
        } else if (lhs->s.len > 0u) {
            cmp = memcmp(lhs->s.bytes, rhs->s.bytes, lhs->s.len);
        }
        switch (op) {
            case SLTok_ADD:
                out->kind = SLCTFEValue_STRING;
                return SLCTFEStringConcat(r, &lhs->s, &rhs->s, &out->s) == 0;
            case SLTok_EQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = SLCTFEStringEq(&lhs->s, &rhs->s);
                return 1;
            case SLTok_NEQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = !SLCTFEStringEq(&lhs->s, &rhs->s);
                return 1;
            case SLTok_LT:
                out->kind = SLCTFEValue_BOOL;
                out->b = cmp < 0;
                return 1;
            case SLTok_GT:
                out->kind = SLCTFEValue_BOOL;
                out->b = cmp > 0;
                return 1;
            case SLTok_LTE:
                out->kind = SLCTFEValue_BOOL;
                out->b = cmp <= 0;
                return 1;
            case SLTok_GTE:
                out->kind = SLCTFEValue_BOOL;
                out->b = cmp >= 0;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == SLCTFEValue_TYPE && rhs->kind == SLCTFEValue_TYPE && r != NULL
        && r->env.evalBinary != NULL)
    {
        int hookIsConst = 0;
        int hookRc = r->env.evalBinary(
            r->env.evalBinaryCtx, op, lhs, rhs, out, &hookIsConst, r->env.diag);
        if (hookRc < 0) {
            return -1;
        }
        if (hookRc > 0 && hookIsConst) {
            return 1;
        }
    }

    if (lhs->kind == SLCTFEValue_TYPE && rhs->kind == SLCTFEValue_TYPE) {
        switch (op) {
            case SLTok_EQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->typeTag == rhs->typeTag;
                return 1;
            case SLTok_NEQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lhs->typeTag != rhs->typeTag;
                return 1;
            default: break;
        }
    }

    if (SLCTFEValueToF64(lhs, &lf) && SLCTFEValueToF64(rhs, &rf)) {
        switch (op) {
            case SLTok_ADD:
                out->kind = SLCTFEValue_FLOAT;
                out->f64 = lf + rf;
                return 1;
            case SLTok_SUB:
                out->kind = SLCTFEValue_FLOAT;
                out->f64 = lf - rf;
                return 1;
            case SLTok_MUL:
                out->kind = SLCTFEValue_FLOAT;
                out->f64 = lf * rf;
                return 1;
            case SLTok_DIV:
                out->kind = SLCTFEValue_FLOAT;
                out->f64 = lf / rf;
                return 1;
            case SLTok_EQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lf == rf;
                return 1;
            case SLTok_NEQ:
                out->kind = SLCTFEValue_BOOL;
                out->b = lf != rf;
                return 1;
            case SLTok_LT:
                out->kind = SLCTFEValue_BOOL;
                out->b = lf < rf;
                return 1;
            case SLTok_GT:
                out->kind = SLCTFEValue_BOOL;
                out->b = lf > rf;
                return 1;
            case SLTok_LTE:
                out->kind = SLCTFEValue_BOOL;
                out->b = lf <= rf;
                return 1;
            case SLTok_GTE:
                out->kind = SLCTFEValue_BOOL;
                out->b = lf >= rf;
                return 1;
            default: return 0;
        }
    }

    if (op == SLTok_EQ || op == SLTok_NEQ) {
        int eq = 0;
        if (lhs->kind == SLCTFEValue_NULL && rhs->kind == SLCTFEValue_NULL) {
            eq = 1;
        } else if (lhs->kind == SLCTFEValue_OPTIONAL && rhs->kind == SLCTFEValue_NULL) {
            eq = lhs->b == 0u;
        } else if (lhs->kind == SLCTFEValue_NULL && rhs->kind == SLCTFEValue_OPTIONAL) {
            eq = rhs->b == 0u;
        } else if (lhs->kind == SLCTFEValue_OPTIONAL && rhs->kind == SLCTFEValue_OPTIONAL) {
            eq = SLCTFEValueEqRec(lhs, rhs, 0);
        }
        if (lhs->kind == SLCTFEValue_NULL || rhs->kind == SLCTFEValue_NULL
            || (lhs->kind == SLCTFEValue_OPTIONAL && rhs->kind == SLCTFEValue_OPTIONAL))
        {
            out->kind = SLCTFEValue_BOOL;
            out->b = (op == SLTok_EQ) ? (eq ? 1u : 0u) : (eq ? 0u : 1u);
            return 1;
        }
    }

    if (r != NULL && r->env.evalBinary != NULL) {
        int hookIsConst = 0;
        int hookRc = r->env.evalBinary(
            r->env.evalBinaryCtx, op, lhs, rhs, out, &hookIsConst, r->env.diag);
        if (hookRc < 0) {
            return -1;
        }
        if (hookRc > 0 && hookIsConst) {
            return 1;
        }
    }

    return 0;
}

static int SLCTFEEvalCast(SLMirCastTarget target, const SLCTFEValue* in, SLCTFEValue* out) {
    SLCTFEValueInvalid(out);
    switch (target) {
        case SLMirCastTarget_INT: {
            int64_t asInt = 0;
            if (in->kind == SLCTFEValue_INT) {
                asInt = in->i64;
            } else if (in->kind == SLCTFEValue_BOOL) {
                asInt = in->b ? 1 : 0;
            } else if (in->kind == SLCTFEValue_FLOAT) {
                if (in->f64 != in->f64 || in->f64 > (double)INT64_MAX
                    || in->f64 < (double)INT64_MIN)
                {
                    return 0;
                }
                asInt = (int64_t)in->f64;
            } else if (in->kind == SLCTFEValue_NULL) {
                asInt = 0;
            } else {
                return 0;
            }
            out->kind = SLCTFEValue_INT;
            out->i64 = asInt;
            return 1;
        }
        case SLMirCastTarget_FLOAT: {
            double asFloat = 0.0;
            if (in->kind == SLCTFEValue_FLOAT) {
                asFloat = in->f64;
            } else if (in->kind == SLCTFEValue_INT) {
                asFloat = (double)in->i64;
            } else if (in->kind == SLCTFEValue_BOOL) {
                asFloat = in->b ? 1.0 : 0.0;
            } else if (in->kind == SLCTFEValue_NULL) {
                asFloat = 0.0;
            } else {
                return 0;
            }
            out->kind = SLCTFEValue_FLOAT;
            out->f64 = asFloat;
            return 1;
        }
        case SLMirCastTarget_BOOL: {
            uint8_t asBool = 0;
            if (in->kind == SLCTFEValue_BOOL) {
                asBool = in->b ? 1u : 0u;
            } else if (in->kind == SLCTFEValue_INT) {
                asBool = in->i64 != 0 ? 1u : 0u;
            } else if (in->kind == SLCTFEValue_FLOAT) {
                asBool = in->f64 != 0.0 ? 1u : 0u;
            } else if (in->kind == SLCTFEValue_OPTIONAL) {
                asBool = in->b != 0u ? 1u : 0u;
            } else if (in->kind == SLCTFEValue_STRING) {
                asBool = 1u;
            } else if (in->kind == SLCTFEValue_NULL) {
                asBool = 0u;
            } else {
                return 0;
            }
            out->kind = SLCTFEValue_BOOL;
            out->b = asBool;
            return 1;
        }
        case SLMirCastTarget_STR_VIEW: *out = *in; return 1;
        default:                       return 0;
    }
}

int SLMirEvalChunk(
    SLArena* _Nonnull arena,
    SLMirChunk chunk,
    const SLMirExecEnv* _Nullable env,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    SLMirExecRun run;
    if (SLMirInitRun(&run, arena, chunk, NULL, NULL, 0u, NULL, 0u, env, 1, outValue, outIsConst)
        != 0)
    {
        return -1;
    }
    return SLMirRunLoop(&run, outValue, outIsConst);
}

static int SLMirEvalFunctionInternal(
    SLArena* _Nonnull arena,
    const SLMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const SLMirExecValue* _Nullable args,
    uint32_t argCount,
    uint16_t callFlags,
    const SLMirExecEnv* _Nullable env,
    int validateProgram,
    int clearDiag,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    const SLMirFunction* fn;
    SLMirChunk           chunk;
    SLMirExecEnv         frameEnv;
    SLMirExecRun         run;
    SLMirExecValue       variadicPackValue;
    int                  enteredFunction = 0;
    int                  boundFrame = 0;
    int                  rc = 0;
    if (program == NULL || functionIndex >= program->funcLen) {
        if (env != NULL) {
            SLCTFESetDiag(env->diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }
    if (validateProgram && SLMirValidateProgram(program, env != NULL ? env->diag : NULL) != 0) {
        return -1;
    }
    fn = &program->funcs[functionIndex];
    if (fn->instStart > program->instLen || fn->instLen > program->instLen - fn->instStart) {
        if (env != NULL) {
            SLCTFESetDiag(env->diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }
    if (fn->localCount < fn->paramCount) {
        return 0;
    }
    if ((fn->flags & SLMirFunctionFlag_VARIADIC) != 0u) {
        uint32_t          fixedCount = fn->paramCount > 0u ? fn->paramCount - 1u : 0u;
        const SLMirLocal* variadicLocal = NULL;
        int               packIsConst = 0;
        if (argCount < fixedCount || fn->paramCount == 0u || env == NULL
            || env->makeVariadicPack == NULL)
        {
            return 0;
        }
        variadicLocal = &program->locals[fn->localStart + fn->paramCount - 1u];
        SLCTFEValueInvalid(&variadicPackValue);
        if (env->makeVariadicPack(
                env->makeVariadicPackCtx,
                program,
                fn,
                variadicLocal->typeRef != UINT32_MAX
                    ? &program->types[variadicLocal->typeRef]
                    : NULL,
                callFlags,
                args != NULL ? args + fixedCount : NULL,
                argCount - fixedCount,
                &variadicPackValue,
                &packIsConst,
                env->diag)
            != 0)
        {
            return -1;
        }
        if (!packIsConst) {
            return 0;
        }
        argCount = fixedCount;
    } else if (argCount != fn->paramCount) {
        return 0;
    }
    memset(&frameEnv, 0, sizeof(frameEnv));
    if (env != NULL) {
        frameEnv = *env;
    }
    if (fn->sourceRef < program->sourceLen) {
        frameEnv.src = program->sources[fn->sourceRef].src;
    }
    if (frameEnv.enterFunction != NULL) {
        if (frameEnv.enterFunction(
                frameEnv.functionCtx, functionIndex, fn->sourceRef, frameEnv.diag)
            != 0)
        {
            return -1;
        }
        enteredFunction = 1;
    }
    chunk.v = program->insts + fn->instStart;
    chunk.len = fn->instLen;
    if (SLMirInitRun(
            &run,
            arena,
            chunk,
            program,
            fn,
            fn->localCount,
            args,
            argCount,
            &frameEnv,
            clearDiag,
            outValue,
            outIsConst)
        != 0)
    {
        rc = -1;
        goto end;
    }
    if ((fn->flags & SLMirFunctionFlag_VARIADIC) != 0u) {
        run.locals[fn->paramCount - 1u] = variadicPackValue;
    }
    if (frameEnv.bindFrame != NULL) {
        if (frameEnv.bindFrame(
                frameEnv.frameCtx, program, fn, run.locals, run.localCount, frameEnv.diag)
            != 0)
        {
            rc = -1;
            goto end;
        }
        boundFrame = 1;
    }
    rc = SLMirRunLoop(&run, outValue, outIsConst);
end:
    if (boundFrame && frameEnv.unbindFrame != NULL) {
        frameEnv.unbindFrame(frameEnv.frameCtx);
    }
    if (enteredFunction && frameEnv.leaveFunction != NULL) {
        frameEnv.leaveFunction(frameEnv.functionCtx);
    }
    return rc;
}

int SLMirEvalFunction(
    SLArena* _Nonnull arena,
    const SLMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const SLMirExecValue* _Nullable args,
    uint32_t argCount,
    const SLMirExecEnv* _Nullable env,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    return SLMirEvalFunctionInternal(
        arena, program, functionIndex, args, argCount, 0u, env, 1, 1, outValue, outIsConst);
}
SL_API_END
