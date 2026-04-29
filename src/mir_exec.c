#include "libhop-impl.h"
#include "ctfe.h"
#include "mir.h"
#include "mir_exec.h"

H2_API_BEGIN

typedef struct {
    const H2MirInst*     ip;
    uint32_t             len;
    uint32_t             pc;
    H2MirExecValue*      stack;
    uint32_t             stackLen;
    uint32_t             stackCap;
    H2MirExecValue*      locals;
    uint32_t             localCount;
    H2Arena*             arena;
    const H2MirProgram*  program;
    const H2MirFunction* function;
    H2MirExecEnv         env;
    uint32_t             backwardJumpCount;
} H2MirExecRun;

#define H2MIR_EXEC_FUNCTION_REF_TAG_FLAG   (UINT64_C(1) << 57)
#define H2MIR_EXEC_BYTE_REF_PROXY_TAG_FLAG (UINT64_C(1) << 56)

static void H2CTFESetDiag(H2Diag* diag, H2DiagCode code, uint32_t start, uint32_t end);
static void H2CTFEValueInvalid(H2CTFEValue* v);
static int  H2CTFEPush(H2MirExecRun* r, const H2MirExecValue* v);
static int  H2CTFEPop(H2MirExecRun* r, H2MirExecValue* out);
static int  H2CTFEParseIntLiteral(H2StrView src, uint32_t start, uint32_t end, int64_t* out);
static int  H2CTFEParseFloatLiteral(
    H2Arena* arena, H2StrView src, uint32_t start, uint32_t end, double* out);
static int H2CTFEParseBoolLiteral(H2StrView src, uint32_t start, uint32_t end, uint8_t* out);
static int H2CTFEOptionalPayload(const H2CTFEValue* opt, const H2CTFEValue** outPayload);
static int H2CTFEEvalUnary(H2TokenKind op, const H2CTFEValue* in, H2CTFEValue* out);
static int H2CTFEEvalBinary(
    H2MirExecRun*      r,
    H2TokenKind        op,
    const H2CTFEValue* lhs,
    const H2CTFEValue* rhs,
    H2CTFEValue*       out);
static int H2CTFEEvalCast(H2MirCastTarget target, const H2CTFEValue* in, H2CTFEValue* out);
static const H2MirLocal* H2MirGetLocalMeta(const H2MirExecRun* run, uint32_t slot);
static int               H2MirCoerceValueForType(
    const H2MirExecRun* run, uint32_t typeRefIndex, H2MirExecValue* inOutValue);
static void H2MirSetReason(
    const H2MirExecRun* _Nullable run, const H2MirInst* _Nullable ins, const char* _Nonnull reason);

static int H2MirInitRun(
    H2MirExecRun* _Nonnull run,
    H2Arena* _Nonnull arena,
    H2MirChunk chunk,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    uint32_t localCount,
    const H2MirExecValue* _Nullable args,
    uint32_t argCount,
    const H2MirExecEnv* _Nullable env,
    int clearDiag,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    if (clearDiag && env != NULL && env->diag != NULL) {
        *env->diag = (H2Diag){ 0 };
    }
    if (run == NULL || arena == NULL || outValue == NULL || outIsConst == NULL) {
        if (env != NULL) {
            H2CTFESetDiag(env->diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }

    H2CTFEValueInvalid(outValue);
    *outIsConst = 0;

    memset(run, 0, sizeof(*run));
    run->ip = chunk.v;
    run->len = chunk.len;
    run->pc = 0;
    run->stackCap = chunk.len + argCount + 1u;
    run->stack = (H2MirExecValue*)H2ArenaAlloc(
        arena, sizeof(H2MirExecValue) * run->stackCap, (uint32_t)_Alignof(H2MirExecValue));
    if (run->stack == NULL) {
        if (env != NULL) {
            H2CTFESetDiag(env->diag, H2Diag_ARENA_OOM, 0, 0);
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
        run->locals = (H2MirExecValue*)H2ArenaAlloc(
            arena, sizeof(H2MirExecValue) * localCount, (uint32_t)_Alignof(H2MirExecValue));
        if (run->locals == NULL) {
            if (env != NULL) {
                H2CTFESetDiag(env->diag, H2Diag_ARENA_OOM, 0, 0);
            }
            return -1;
        }
        for (i = 0; i < localCount; i++) {
            H2CTFEValueInvalid(&run->locals[i]);
        }
        if (argCount > localCount) {
            return 0;
        }
        for (i = 0; i < argCount; i++) {
            run->locals[i] = args[i];
            if (program != NULL && function != NULL) {
                const H2MirLocal* local = H2MirGetLocalMeta(run, i);
                if (local->typeRef != UINT32_MAX) {
                    int coerceRc = H2MirCoerceValueForType(run, local->typeRef, &run->locals[i]);
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

static const H2MirLocal* H2MirGetLocalMeta(const H2MirExecRun* run, uint32_t slot) {
    static const H2MirLocal invalidLocal = { UINT32_MAX, H2MirLocalFlag_NONE, 0u, 0u };
    if (run == NULL || run->program == NULL || run->function == NULL || slot >= run->localCount
        || run->function->localStart > run->program->localLen
        || slot >= run->program->localLen - run->function->localStart)
    {
        return &invalidLocal;
    }
    return &run->program->locals[run->function->localStart + slot];
}

static int H2MirCoerceValueForType(
    const H2MirExecRun* run, uint32_t typeRefIndex, H2MirExecValue* inOutValue) {
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

static uint32_t H2MirResolveHostId(const H2MirExecRun* run, const H2MirInst* ins) {
    if (run == NULL || ins == NULL || run->program == NULL || run->program->hostLen == 0
        || ins->aux >= run->program->hostLen)
    {
        return H2MirHostTarget_INVALID;
    }
    return run->program->hosts[ins->aux].target;
}

static uint32_t H2MirResolvedCallArgCount(const H2MirInst* ins) {
    if (ins == NULL) {
        return 0;
    }
    return H2MirCallArgCountFromTok(ins->tok);
}

static int H2MirResolvedCallDropsReceiverArg0(const H2MirInst* ins) {
    return ins != NULL && H2MirCallTokDropsReceiverArg0(ins->tok);
}

static void H2MirSetReason(
    const H2MirExecRun* _Nullable run,
    const H2MirInst* _Nullable ins,
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

static H2CTFEValue* _Nullable H2MirReferenceTarget(H2CTFEValue* value) {
    if (value == NULL || value->kind != H2CTFEValue_REFERENCE || value->s.bytes == NULL) {
        return NULL;
    }
    return (H2CTFEValue*)value->s.bytes;
}

static int H2MirEvalFunctionInternal(
    H2Arena* _Nonnull arena,
    const H2MirProgram* _Nonnull program,
    uint32_t functionIndex,
    const H2MirExecValue* _Nullable args,
    uint32_t argCount,
    uint16_t callFlags,
    const H2MirExecEnv* _Nullable env,
    int validateProgram,
    int clearDiag,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

static int H2MirConstToValue(const H2MirConst* _Nonnull in, H2MirExecValue* _Nonnull out) {
    double f64 = 0.0;
    if (in == NULL || out == NULL) {
        return 0;
    }
    H2CTFEValueInvalid(out);
    switch (in->kind) {
        case H2MirConst_INT:
            out->kind = H2CTFEValue_INT;
            out->i64 = (int64_t)in->bits;
            return 1;
        case H2MirConst_FLOAT:
            out->kind = H2CTFEValue_FLOAT;
            memcpy(&f64, &in->bits, sizeof(f64));
            out->f64 = f64;
            return 1;
        case H2MirConst_BOOL:
            out->kind = H2CTFEValue_BOOL;
            out->b = in->bits != 0;
            return 1;
        case H2MirConst_STRING:
            out->kind = H2CTFEValue_STRING;
            out->s.bytes = (const uint8_t*)in->bytes.ptr;
            out->s.len = in->bytes.len;
            return 1;
        case H2MirConst_TYPE:
            out->kind = H2CTFEValue_TYPE;
            out->typeTag = in->bits;
            out->s.bytes = (const uint8_t*)in->bytes.ptr;
            out->s.len = in->bytes.len;
            return 1;
        case H2MirConst_FUNCTION: H2MirValueSetFunctionRef(out, (uint32_t)in->bits); return 1;
        case H2MirConst_NULL:     out->kind = H2CTFEValue_NULL; return 1;
        default:                  return 0;
    }
}

void H2MirValueSetFunctionRef(H2MirExecValue* _Nonnull value, uint32_t functionIndex) {
    if (value == NULL) {
        return;
    }
    H2CTFEValueInvalid(value);
    value->kind = H2CTFEValue_TYPE;
    value->typeTag = H2MIR_EXEC_FUNCTION_REF_TAG_FLAG | (uint64_t)functionIndex;
}

static int H2MirValueIsFunctionRef(const H2MirExecValue* value, uint32_t* _Nullable outFnIndex) {
    if (value == NULL || value->kind != H2CTFEValue_TYPE
        || (value->typeTag & H2MIR_EXEC_FUNCTION_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outFnIndex != NULL) {
        *outFnIndex = (uint32_t)(value->typeTag & ~H2MIR_EXEC_FUNCTION_REF_TAG_FLAG);
    }
    return 1;
}

int H2MirValueAsFunctionRef(
    const H2MirExecValue* _Nonnull value, uint32_t* _Nullable outFunctionIndex) {
    return H2MirValueIsFunctionRef(value, outFunctionIndex);
}

void H2MirValueSetByteRefProxy(H2MirExecValue* _Nonnull value, uint8_t* _Nullable targetByte) {
    if (value == NULL) {
        return;
    }
    H2CTFEValueInvalid(value);
    value->kind = H2CTFEValue_INT;
    value->i64 = targetByte != NULL ? (int64_t)(*targetByte) : 0;
    value->typeTag = H2MIR_EXEC_BYTE_REF_PROXY_TAG_FLAG;
    value->s.bytes = targetByte;
    value->s.len = targetByte != NULL ? 1u : 0u;
}

int H2MirValueAsByteRefProxy(
    const H2MirExecValue* _Nonnull value, uint8_t* _Nullable* _Nullable outTargetByte) {
    if (value == NULL || value->kind != H2CTFEValue_INT
        || (value->typeTag & H2MIR_EXEC_BYTE_REF_PROXY_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return 0;
    }
    if (outTargetByte != NULL) {
        *outTargetByte = (uint8_t*)value->s.bytes;
    }
    return 1;
}

void H2MirExecEnvDisableDynamicResolution(H2MirExecEnv* env) {
    if (env == NULL) {
        return;
    }
    env->resolveIdent = NULL;
    env->resolveCall = NULL;
    env->resolveCtx = NULL;
}

static void H2MirResolveSymbolName(
    const H2MirExecRun* _Nonnull run,
    const H2MirInst* _Nonnull ins,
    H2MirSymbolKind expectedKind,
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

static void H2MirResolveFieldName(
    const H2MirExecRun* _Nonnull run,
    const H2MirInst* _Nonnull ins,
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

static int H2MirRunLoop(
    H2MirExecRun* _Nonnull run, H2MirExecValue* _Nonnull outValue, int* _Nonnull outIsConst) {
    while (run->pc < run->len) {
        const H2MirInst* ins = &run->ip[run->pc++];
        switch (ins->op) {
            case H2MirOp_PUSH_CONST: {
                H2MirExecValue v;
                if (run->program == NULL || ins->aux >= run->program->constLen) {
                    return 0;
                }
                if (!H2MirConstToValue(&run->program->consts[ins->aux], &v)) {
                    return 0;
                }
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_PUSH_INT: {
                H2CTFEValue  v;
                uint32_t     rune = 0;
                H2RuneLitErr runeErr = { 0 };
                v.kind = H2CTFEValue_INT;
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
                if ((H2TokenKind)ins->tok == H2Tok_RUNE) {
                    if (H2DecodeRuneLiteralValidate(
                            run->env.src.ptr, ins->start, ins->end, &rune, &runeErr)
                        != 0)
                    {
                        H2CTFESetDiag(
                            run->env.diag,
                            H2RuneLitErrDiagCode(runeErr.kind),
                            runeErr.start,
                            runeErr.end);
                        return -1;
                    }
                    v.i64 = (int64_t)rune;
                } else if ((H2TokenKind)ins->tok == H2Tok_INVALID) {
                    v.i64 = (int64_t)(int32_t)ins->aux;
                } else {
                    if (H2CTFEParseIntLiteral(run->env.src, ins->start, ins->end, &v.i64) != 0) {
                        return 0;
                    }
                }
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_PUSH_FLOAT: {
                H2CTFEValue v;
                v.kind = H2CTFEValue_FLOAT;
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
                if (H2CTFEParseFloatLiteral(run->arena, run->env.src, ins->start, ins->end, &v.f64)
                    != 0)
                {
                    return 0;
                }
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_PUSH_BOOL: {
                H2CTFEValue v;
                uint8_t     b = 0;
                if (H2CTFEParseBoolLiteral(run->env.src, ins->start, ins->end, &b) != 0) {
                    return 0;
                }
                v.kind = H2CTFEValue_BOOL;
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
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_PUSH_STRING: {
                H2CTFEValue    v;
                H2StringLitErr litErr = { 0 };
                uint8_t*       bytes = NULL;
                uint32_t       len = 0;
                if (H2DecodeStringLiteralArena(
                        run->arena, run->env.src.ptr, ins->start, ins->end, &bytes, &len, &litErr)
                    != 0)
                {
                    H2CTFESetDiag(
                        run->env.diag,
                        H2StringLitErrDiagCode(litErr.kind),
                        litErr.start,
                        litErr.end);
                    return -1;
                }
                v.kind = H2CTFEValue_STRING;
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
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_PUSH_NULL: {
                H2CTFEValue v;
                v.kind = H2CTFEValue_NULL;
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
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_LOCAL_LOAD:
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                if (H2CTFEPush(run, &run->locals[ins->aux]) != 0) {
                    return -1;
                }
                break;
            case H2MirOp_LOCAL_ADDR: {
                H2MirExecValue v;
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                H2CTFEValueInvalid(&v);
                v.kind = H2CTFEValue_REFERENCE;
                v.s.bytes = (const uint8_t*)&run->locals[ins->aux];
                v.s.len = 0;
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_ADDR_OF: {
                H2MirExecValue  value;
                H2MirExecValue* target;
                if (H2CTFEPop(run, &value) != 0) {
                    return 0;
                }
                target = (H2MirExecValue*)H2ArenaAlloc(
                    run->arena, sizeof(*target), (uint32_t)_Alignof(H2MirExecValue));
                if (target == NULL) {
                    H2CTFESetDiag(run->env.diag, H2Diag_ARENA_OOM, ins->start, ins->end);
                    return -1;
                }
                *target = value;
                H2CTFEValueInvalid(&value);
                value.kind = H2CTFEValue_REFERENCE;
                value.s.bytes = (const uint8_t*)target;
                value.s.len = 0;
                if (H2CTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_LOCAL_ZERO: {
                const H2MirLocal* local;
                H2MirExecValue    v;
                int               isConst = 0;
                if (ins->aux >= run->localCount || run->env.zeroInitLocal == NULL) {
                    return 0;
                }
                local = H2MirGetLocalMeta(run, ins->aux);
                if (local->typeRef == UINT32_MAX || local->typeRef >= run->program->typeLen) {
                    return 0;
                }
                H2CTFEValueInvalid(&v);
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
            case H2MirOp_DEREF_LOAD: {
                H2MirExecValue ref;
                H2CTFEValue*   target;
                if (H2CTFEPop(run, &ref) != 0) {
                    return 0;
                }
                target = H2MirReferenceTarget(&ref);
                if (target == NULL) {
                    return 0;
                }
                {
                    uint8_t*       bytePtr = NULL;
                    H2MirExecValue out;
                    if (H2MirValueAsByteRefProxy(target, &bytePtr) && bytePtr != NULL) {
                        out = *target;
                        out.i64 = (int64_t)(*bytePtr);
                        out.f64 = 0.0;
                        out.b = 0;
                        out.typeTag = 0;
                        out.s.bytes = NULL;
                        out.s.len = 0;
                        if (H2CTFEPush(run, &out) != 0) {
                            return -1;
                        }
                        break;
                    }
                }
                if (H2CTFEPush(run, target) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_DEREF_STORE: {
                H2MirExecValue ref;
                H2MirExecValue v;
                H2CTFEValue*   target;
                uint8_t*       bytePtr = NULL;
                int64_t        byteValue = 0;
                if (H2CTFEPop(run, &ref) != 0 || H2CTFEPop(run, &v) != 0) {
                    return 0;
                }
                target = H2MirReferenceTarget(&ref);
                if (target == NULL) {
                    return 0;
                }
                if (H2MirValueAsByteRefProxy(target, &bytePtr) && bytePtr != NULL) {
                    if (H2CTFEValueToInt64(&v, &byteValue) != 0 || byteValue < 0 || byteValue > 255)
                    {
                        H2MirSetReason(run, ins, "byte reference store value is not supported");
                        return 0;
                    }
                    *bytePtr = (uint8_t)byteValue;
                    target->i64 = byteValue;
                    break;
                }
                if (H2MirValueAsByteRefProxy(target, &bytePtr)) {
                    H2MirSetReason(run, ins, "byte reference store target is null");
                    return 0;
                }
                *target = v;
                break;
            }
            case H2MirOp_LOCAL_STORE: {
                const H2MirLocal* local;
                H2MirExecValue    v;
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                if (H2CTFEPop(run, &v) != 0) {
                    return 0;
                }
                local = H2MirGetLocalMeta(run, ins->aux);
                if (local->typeRef != UINT32_MAX) {
                    int coerceRc = H2MirCoerceValueForType(run, local->typeRef, &v);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                run->locals[ins->aux] = v;
                break;
            }
            case H2MirOp_DROP: {
                H2MirExecValue v;
                if (H2CTFEPop(run, &v) != 0) {
                    return 0;
                }
                break;
            }
            case H2MirOp_JUMP:
                if (ins->aux >= run->len) {
                    return 0;
                }
                if (run->env.backwardJumpLimit != 0 && ins->aux < run->pc) {
                    if (++run->backwardJumpCount > run->env.backwardJumpLimit) {
                        H2MirSetReason(run, ins, "for-loop exceeded const-eval iteration limit");
                        return 0;
                    }
                }
                run->pc = ins->aux;
                break;
            case H2MirOp_JUMP_IF_FALSE: {
                H2MirExecValue cond;
                H2MirExecValue condBool;
                if (ins->aux >= run->len) {
                    return 0;
                }
                if (H2CTFEPop(run, &cond) != 0) {
                    return 0;
                }
                if (!H2CTFEEvalCast(H2MirCastTarget_BOOL, &cond, &condBool)) {
                    return 0;
                }
                if (!condBool.b) {
                    if (run->env.backwardJumpLimit != 0 && ins->aux < run->pc) {
                        if (++run->backwardJumpCount > run->env.backwardJumpLimit) {
                            H2MirSetReason(
                                run, ins, "for-loop exceeded const-eval iteration limit");
                            return 0;
                        }
                    }
                    run->pc = ins->aux;
                }
                break;
            }
            case H2MirOp_ASSERT: {
                H2MirExecValue cond;
                H2MirExecValue condBool;
                if (H2CTFEPop(run, &cond) != 0) {
                    return 0;
                }
                if (!H2CTFEEvalCast(H2MirCastTarget_BOOL, &cond, &condBool)) {
                    return 0;
                }
                if (!condBool.b) {
                    H2MirSetReason(
                        run, ins, "assert condition evaluated to false during const evaluation");
                    return 0;
                }
                break;
            }
            case H2MirOp_LOAD_IDENT: {
                H2CTFEValue v;
                int         idIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (run->env.resolveIdent == NULL) {
                    return 0;
                }
                H2MirResolveSymbolName(run, ins, H2MirSymbol_IDENT, &nameStart, &nameEnd);
                H2CTFEValueInvalid(&v);
                if (run->env.resolveIdent(
                        run->env.resolveCtx, nameStart, nameEnd, &v, &idIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!idIsConst) {
                    return 0;
                }
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_STORE_IDENT: {
                H2CTFEValue value;
                int         idIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (H2CTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (run->env.assignIdent == NULL) {
                    return 0;
                }
                H2MirResolveSymbolName(run, ins, H2MirSymbol_IDENT, &nameStart, &nameEnd);
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
            case H2MirOp_CALL: {
                H2CTFEValue* args = NULL;
                H2CTFEValue  v;
                int          callIsConst = 0;
                uint32_t     argCount = H2MirResolvedCallArgCount(ins);
                uint32_t     i;
                uint32_t     nameStart = ins->start;
                uint32_t     nameEnd = ins->end;
                if (run->env.resolveCall == NULL && run->env.resolveCallPre == NULL) {
                    return 0;
                }
                H2MirResolveSymbolName(run, ins, H2MirSymbol_CALL, &nameStart, &nameEnd);
                if (run->env.resolveCallPre != NULL) {
                    int preRc;
                    H2CTFEValueInvalid(&v);
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
                        if (H2CTFEPush(run, &v) != 0) {
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
                    args = (H2CTFEValue*)H2ArenaAlloc(
                        run->arena,
                        sizeof(H2CTFEValue) * argCount,
                        (uint32_t)_Alignof(H2CTFEValue));
                    if (args == NULL) {
                        H2CTFESetDiag(run->env.diag, H2Diag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (H2CTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                H2CTFEValueInvalid(&v);
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
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_CALL_HOST: {
                H2MirExecValue* args = NULL;
                H2MirExecValue  v;
                int             callOk = 0;
                uint32_t        argCount = H2MirResolvedCallArgCount(ins);
                uint32_t        i;
                uint32_t        callArgOffset = 0;
                if (run->env.hostCall == NULL) {
                    return 0;
                }
                if (argCount > run->stackLen) {
                    return 0;
                }
                if (argCount > 0u) {
                    args = (H2MirExecValue*)H2ArenaAlloc(
                        run->arena,
                        sizeof(H2MirExecValue) * argCount,
                        (uint32_t)_Alignof(H2MirExecValue));
                    if (args == NULL) {
                        H2CTFESetDiag(run->env.diag, H2Diag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (H2CTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                H2CTFEValueInvalid(&v);
                if (H2MirResolvedCallDropsReceiverArg0(ins)) {
                    if (argCount == 0u) {
                        return 0;
                    }
                    callArgOffset = 1u;
                }
                if (run->env.hostCall(
                        run->env.hostCtx,
                        H2MirResolveHostId(run, ins),
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
                    H2MirSetReason(run, ins, "host call is not supported by evaluator backend");
                    return 0;
                }
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_CALL_FN: {
                H2MirExecValue  v;
                H2MirExecValue* args = NULL;
                int             callOk = 0;
                uint32_t        argCount = H2MirResolvedCallArgCount(ins);
                uint32_t        i;
                uint32_t        callArgOffset = 0;
                if (run->program == NULL || ins->aux >= run->program->funcLen) {
                    return 0;
                }
                if (argCount > run->stackLen) {
                    return 0;
                }
                if (argCount != 0u) {
                    args = (H2MirExecValue*)H2ArenaAlloc(
                        run->arena,
                        sizeof(H2MirExecValue) * argCount,
                        (uint32_t)_Alignof(H2MirExecValue));
                    if (args == NULL) {
                        H2CTFESetDiag(run->env.diag, H2Diag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (H2CTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                if (H2MirResolvedCallDropsReceiverArg0(ins)) {
                    if (argCount == 0u) {
                        return 0;
                    }
                    callArgOffset = 1u;
                }
                if (run->env.adjustCallArgs != NULL) {
                    int adjustRc = run->env.adjustCallArgs(
                        run->env.adjustCallArgsCtx,
                        run->program,
                        run->function,
                        ins,
                        ins->aux,
                        args,
                        argCount,
                        run->env.diag);
                    if (adjustRc < 0) {
                        return -1;
                    }
                    if (adjustRc > 0) {
                        return 0;
                    }
                }
                if (H2MirEvalFunctionInternal(
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
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_CALL_INDIRECT: {
                H2MirExecValue  callee;
                H2MirExecValue  v;
                H2MirExecValue* args = NULL;
                int             callOk = 0;
                uint32_t        fnIndex = 0;
                uint32_t        argCount = H2MirResolvedCallArgCount(ins);
                uint32_t        i;
                if (run->program == NULL || argCount + 1u > run->stackLen) {
                    H2MirSetReason(
                        run, ins, "indirect call stack is invalid during const evaluation");
                    return 0;
                }
                if (argCount != 0u) {
                    args = (H2MirExecValue*)H2ArenaAlloc(
                        run->arena,
                        sizeof(H2MirExecValue) * argCount,
                        (uint32_t)_Alignof(H2MirExecValue));
                    if (args == NULL) {
                        H2CTFESetDiag(run->env.diag, H2Diag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (H2CTFEPop(run, &args[i - 1]) != 0) {
                        H2MirSetReason(
                            run,
                            ins,
                            "indirect call arguments are not available during const evaluation");
                        return 0;
                    }
                }
                if (H2CTFEPop(run, &callee) != 0) {
                    H2MirSetReason(
                        run, ins, "indirect call target is not available during const evaluation");
                    return 0;
                }
                if (!H2MirValueIsFunctionRef(&callee, &fnIndex) || fnIndex >= run->program->funcLen)
                {
                    H2MirSetReason(run, ins, "indirect call target is not a function");
                    return 0;
                }
                if (run->env.adjustCallArgs != NULL) {
                    int adjustRc = run->env.adjustCallArgs(
                        run->env.adjustCallArgsCtx,
                        run->program,
                        run->function,
                        ins,
                        fnIndex,
                        args,
                        argCount,
                        run->env.diag);
                    if (adjustRc < 0) {
                        return -1;
                    }
                    if (adjustRc > 0) {
                        return 0;
                    }
                }
                if (H2MirEvalFunctionInternal(
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
                if (H2CTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_UNARY: {
                H2CTFEValue in;
                H2CTFEValue out;
                if (H2CTFEPop(run, &in) != 0) {
                    return 0;
                }
                if (!H2CTFEEvalUnary((H2TokenKind)ins->tok, &in, &out)) {
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_BINARY: {
                H2CTFEValue lhs;
                H2CTFEValue rhs;
                H2CTFEValue out;
                int         binaryRc;
                if (H2CTFEPop(run, &rhs) != 0 || H2CTFEPop(run, &lhs) != 0) {
                    return 0;
                }
                binaryRc = H2CTFEEvalBinary(run, (H2TokenKind)ins->tok, &lhs, &rhs, &out);
                if (binaryRc < 0) {
                    return -1;
                }
                if (binaryRc == 0) {
                    H2MirSetReason(
                        run, ins, "binary operation is not supported during const evaluation");
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_INDEX: {
                H2CTFEValue base;
                H2CTFEValue idx;
                H2CTFEValue out;
                int64_t     idxInt = 0;
                int         indexIsConst = 0;
                if (H2CTFEPop(run, &idx) != 0 || H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (base.kind == H2CTFEValue_STRING && H2CTFEValueToInt64(&idx, &idxInt) == 0) {
                    if (idxInt < 0) {
                        H2MirSetReason(run, ins, "index is negative in const evaluation");
                        return 0;
                    }
                    if ((uint64_t)idxInt >= (uint64_t)base.s.len) {
                        H2MirSetReason(run, ins, "index is out of bounds in const evaluation");
                        return 0;
                    }
                    out.kind = H2CTFEValue_INT;
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
                    if (H2CTFEPush(run, &out) != 0) {
                        return -1;
                    }
                    break;
                }
                if (run->env.indexValue == NULL) {
                    return 0;
                }
                H2CTFEValueInvalid(&out);
                if (run->env.indexValue(
                        run->env.indexValueCtx, &base, &idx, &out, &indexIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!indexIsConst) {
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_SEQ_LEN: {
                H2CTFEValue base;
                H2CTFEValue out;
                int         lenIsConst = 0;
                if (H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (base.kind == H2CTFEValue_STRING || base.kind == H2CTFEValue_ARRAY
                    || base.kind == H2CTFEValue_NULL)
                {
                    out.kind = H2CTFEValue_INT;
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
                    if (H2CTFEPush(run, &out) != 0) {
                        return -1;
                    }
                    break;
                }
                if (run->env.sequenceLen == NULL) {
                    return 0;
                }
                H2CTFEValueInvalid(&out);
                if (run->env.sequenceLen(
                        run->env.sequenceLenCtx, &base, &out, &lenIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!lenIsConst) {
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_STR_CSTR: {
                H2CTFEValue  base;
                H2CTFEValue* target;
                if (H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                target = H2MirReferenceTarget(&base);
                if (target != NULL) {
                    base = *target;
                }
                if (base.kind != H2CTFEValue_STRING) {
                    return 0;
                }
                if (H2CTFEPush(run, &base) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_ARRAY_ADDR: {
                H2CTFEValue base;
                H2CTFEValue idx;
                H2CTFEValue out;
                int         addrIsConst = 0;
                if (H2CTFEPop(run, &idx) != 0 || H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.indexAddr == NULL) {
                    return 0;
                }
                H2CTFEValueInvalid(&out);
                if (run->env.indexAddr(
                        run->env.indexAddrCtx, &base, &idx, &out, &addrIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!addrIsConst) {
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_SLICE_MAKE: {
                H2CTFEValue  base;
                H2CTFEValue  startValue;
                H2CTFEValue  endValue;
                H2CTFEValue  out;
                H2CTFEValue* startPtr = NULL;
                H2CTFEValue* endPtr = NULL;
                int          sliceIsConst = 0;
                uint16_t     sliceFlags = ins->tok;
                if (run->env.sliceValue == NULL) {
                    return 0;
                }
                if ((sliceFlags & H2AstFlag_INDEX_HAS_END) != 0u) {
                    if (H2CTFEPop(run, &endValue) != 0) {
                        return 0;
                    }
                    endPtr = &endValue;
                }
                if ((sliceFlags & H2AstFlag_INDEX_HAS_START) != 0u) {
                    if (H2CTFEPop(run, &startValue) != 0) {
                        return 0;
                    }
                    startPtr = &startValue;
                }
                if (H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                H2CTFEValueInvalid(&out);
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
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_AGG_MAKE: {
                H2CTFEValue out;
                int         aggIsConst = 0;
                if (run->env.makeAggregate == NULL) {
                    return 0;
                }
                H2CTFEValueInvalid(&out);
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
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_AGG_ZERO: {
                H2CTFEValue out;
                int         aggIsConst = 0;
                if (run->program == NULL || run->env.zeroInitLocal == NULL
                    || ins->aux >= run->program->typeLen)
                {
                    return 0;
                }
                H2CTFEValueInvalid(&out);
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
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_AGG_SET: {
                H2CTFEValue value;
                H2CTFEValue base;
                int         fieldIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (H2CTFEPop(run, &value) != 0 || H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggSetField == NULL) {
                    return 0;
                }
                H2MirResolveFieldName(run, ins, &nameStart, &nameEnd);
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
                if (H2CTFEPush(run, &base) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_AGG_GET: {
                H2CTFEValue base;
                H2CTFEValue out;
                int         fieldIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggGetField == NULL) {
                    return 0;
                }
                H2MirResolveFieldName(run, ins, &nameStart, &nameEnd);
                H2CTFEValueInvalid(&out);
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
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_AGG_ADDR: {
                H2CTFEValue base;
                H2CTFEValue out;
                int         fieldIsConst = 0;
                uint32_t    nameStart = ins->start;
                uint32_t    nameEnd = ins->end;
                if (H2CTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggAddrField == NULL) {
                    return 0;
                }
                H2MirResolveFieldName(run, ins, &nameStart, &nameEnd);
                H2CTFEValueInvalid(&out);
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
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_TUPLE_MAKE: {
                H2MirExecValue* elems = NULL;
                H2MirExecValue  out;
                int             tupleIsConst = 0;
                uint32_t        elemCount = (uint32_t)ins->tok;
                uint32_t        i;
                if (run->env.makeTuple == NULL || elemCount > run->stackLen) {
                    return 0;
                }
                if (elemCount != 0u) {
                    elems = (H2MirExecValue*)H2ArenaAlloc(
                        run->arena,
                        sizeof(H2MirExecValue) * elemCount,
                        (uint32_t)_Alignof(H2MirExecValue));
                    if (elems == NULL) {
                        H2CTFESetDiag(run->env.diag, H2Diag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = elemCount; i > 0; i--) {
                    if (H2CTFEPop(run, &elems[i - 1u]) != 0) {
                        return 0;
                    }
                }
                H2CTFEValueInvalid(&out);
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
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_ITER_INIT: {
                H2CTFEValue source;
                H2CTFEValue iter;
                int         iterIsConst = 0;
                if (H2CTFEPop(run, &source) != 0) {
                    return 0;
                }
                if (run->env.iterInit == NULL) {
                    return 0;
                }
                H2CTFEValueInvalid(&iter);
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
                if (H2CTFEPush(run, &iter) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_ITER_NEXT: {
                H2CTFEValue iter;
                H2CTFEValue key;
                H2CTFEValue value;
                H2CTFEValue hasItemValue;
                int         hasItem = 0;
                int         keyIsConst = 0;
                int         valueIsConst = 0;
                if (H2CTFEPop(run, &iter) != 0) {
                    return 0;
                }
                if (run->env.iterNext == NULL) {
                    return 0;
                }
                H2CTFEValueInvalid(&key);
                H2CTFEValueInvalid(&value);
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
                    if ((ins->tok & H2MirIterFlag_HAS_KEY) != 0u && !keyIsConst) {
                        return 0;
                    }
                    if ((ins->tok & H2MirIterFlag_VALUE_DISCARD) == 0u && !valueIsConst) {
                        return 0;
                    }
                    if ((ins->tok & H2MirIterFlag_HAS_KEY) != 0u) {
                        if (H2CTFEPush(run, &key) != 0) {
                            return -1;
                        }
                    }
                    if ((ins->tok & H2MirIterFlag_VALUE_DISCARD) == 0u) {
                        if (H2CTFEPush(run, &value) != 0) {
                            return -1;
                        }
                    }
                }
                hasItemValue.kind = H2CTFEValue_BOOL;
                hasItemValue.i64 = 0;
                hasItemValue.f64 = 0.0;
                hasItemValue.b = hasItem ? 1u : 0u;
                hasItemValue.typeTag = 0;
                hasItemValue.s.bytes = NULL;
                hasItemValue.s.len = 0;
                hasItemValue.span = (H2CTFESpan){ 0 };
                if (H2CTFEPush(run, &hasItemValue) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_CAST: {
                H2CTFEValue in;
                H2CTFEValue out;
                if (H2CTFEPop(run, &in) != 0) {
                    return 0;
                }
                if (!H2CTFEEvalCast((H2MirCastTarget)ins->tok, &in, &out)) {
                    return 0;
                }
                {
                    int coerceRc = H2MirCoerceValueForType(run, ins->aux, &out);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_COERCE: {
                H2CTFEValue value;
                int         coerceRc;
                if (H2CTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (value.kind == H2CTFEValue_AGGREGATE) {
                    value.typeTag |= H2CTFEValueTag_AGG_PARTIAL;
                }
                coerceRc = H2MirCoerceValueForType(run, ins->aux, &value);
                if (coerceRc <= 0) {
                    return coerceRc < 0 ? -1 : 0;
                }
                if (H2CTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_OPTIONAL_WRAP: {
                H2CTFEValue  value;
                H2CTFEValue* payloadCopy;
                if (H2CTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (value.kind == H2CTFEValue_OPTIONAL) {
                    if (H2CTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                if (value.kind == H2CTFEValue_NULL) {
                    value.kind = H2CTFEValue_OPTIONAL;
                    value.i64 = 0;
                    value.f64 = 0.0;
                    value.b = 0u;
                    value.typeTag = 0;
                    value.s.bytes = NULL;
                    value.s.len = 0;
                    if (H2CTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                payloadCopy = (H2CTFEValue*)H2ArenaAlloc(
                    run->arena, sizeof(*payloadCopy), (uint32_t)_Alignof(H2CTFEValue));
                if (payloadCopy == NULL) {
                    H2CTFESetDiag(run->env.diag, H2Diag_ARENA_OOM, ins->start, ins->end);
                    return -1;
                }
                *payloadCopy = value;
                value.kind = H2CTFEValue_OPTIONAL;
                value.i64 = 0;
                value.f64 = 0.0;
                value.b = 1u;
                value.typeTag = 0;
                value.s.bytes = (const uint8_t*)payloadCopy;
                value.s.len = 0;
                if (H2CTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_OPTIONAL_UNWRAP: {
                H2CTFEValue        value;
                const H2CTFEValue* payload = NULL;
                if (H2CTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (!H2CTFEOptionalPayload(&value, &payload)) {
                    if (value.kind == H2CTFEValue_NULL) {
                        H2MirSetReason(run, ins, "unwrap of empty optional in evaluator backend");
                        return 0;
                    }
                    if (H2CTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                if (value.b == 0u || payload == NULL) {
                    H2MirSetReason(run, ins, "unwrap of empty optional in evaluator backend");
                    return 0;
                }
                if (H2CTFEPush(run, payload) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_ALLOC_NEW: {
                H2CTFEValue out;
                int         allocRc;
                int         allocIsConst = 0;
                if (run->env.allocNew == NULL) {
                    H2MirSetReason(
                        run, ins, "alloc expression is not supported during const evaluation");
                    return 0;
                }
                allocRc = run->env.allocNew(
                    run->env.allocNewCtx, ins->aux, &out, &allocIsConst, run->env.diag);
                if (allocRc < 0) {
                    return -1;
                }
                if (allocRc == 0 || !allocIsConst) {
                    H2MirSetReason(
                        run, ins, "alloc expression is not supported during const evaluation");
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_CTX_GET: {
                H2CTFEValue out;
                int         getRc;
                int         isConst = 0;
                if (run->env.contextGet == NULL) {
                    H2MirSetReason(
                        run, ins, "context access is not supported during const evaluation");
                    return 0;
                }
                getRc = run->env.contextGet(
                    run->env.contextGetCtx, ins->aux, &out, &isConst, run->env.diag);
                if (getRc < 0) {
                    return -1;
                }
                if (getRc == 0 || !isConst) {
                    H2MirSetReason(
                        run, ins, "context access is not supported during const evaluation");
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_CTX_ADDR: {
                H2CTFEValue out;
                int         addrRc;
                int         isConst = 0;
                if (run->env.contextAddr == NULL) {
                    H2MirSetReason(
                        run, ins, "context address is not supported during const evaluation");
                    return 0;
                }
                addrRc = run->env.contextAddr(
                    run->env.contextAddrCtx, ins->aux, &out, &isConst, run->env.diag);
                if (addrRc < 0) {
                    return -1;
                }
                if (addrRc == 0 || !isConst) {
                    H2MirSetReason(
                        run, ins, "context address is not supported during const evaluation");
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_CTX_SET: {
                H2CTFEValue out;
                int         evalRc;
                int         isConst = 0;
                if (run->env.evalWithContext == NULL) {
                    H2MirSetReason(
                        run, ins, "context overlay is not supported during const evaluation");
                    return 0;
                }
                evalRc = run->env.evalWithContext(
                    run->env.evalWithContextCtx, ins->aux, &out, &isConst, run->env.diag);
                if (evalRc < 0) {
                    return -1;
                }
                if (evalRc == 0 || !isConst) {
                    H2MirSetReason(
                        run, ins, "context overlay is not supported during const evaluation");
                    return 0;
                }
                if (H2CTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case H2MirOp_RETURN:
                if (run->stackLen != 1) {
                    H2MirSetReason(run, ins, "return stack is invalid during const evaluation");
                    return 0;
                }
                *outValue = run->stack[0];
                if (run->function != NULL && run->function->typeRef != UINT32_MAX) {
                    int coerceRc = H2MirCoerceValueForType(run, run->function->typeRef, outValue);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                *outIsConst = 1;
                return 0;
            case H2MirOp_RETURN_VOID:
                H2CTFEValueInvalid(outValue);
                *outIsConst = 1;
                return 0;
            default: return 0;
        }
    }
    return 0;
}

static void H2CTFESetDiag(H2Diag* diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    H2DiagReset(diag, code);

    diag->start = start;

    diag->end = end;
}

static void H2CTFEValueInvalid(H2CTFEValue* v) {
    v->kind = H2CTFEValue_INVALID;
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

static int H2CTFEPush(H2MirExecRun* r, const H2MirExecValue* v) {
    if (r->stackLen >= r->stackCap) {
        H2CTFESetDiag(r->env.diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }
    r->stack[r->stackLen++] = *v;
    return 0;
}

static int H2CTFEPop(H2MirExecRun* r, H2MirExecValue* out) {
    if (r->stackLen == 0) {
        return -1;
    }
    *out = r->stack[--r->stackLen];
    return 0;
}

static int H2CTFEParseIntLiteral(H2StrView src, uint32_t start, uint32_t end, int64_t* out) {
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

static int H2CTFEParseFloatLiteral(
    H2Arena* arena, H2StrView src, uint32_t start, uint32_t end, double* out) {
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

static int H2CTFEParseBoolLiteral(H2StrView src, uint32_t start, uint32_t end, uint8_t* out) {
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

static int H2CTFEStringEq(const H2CTFEString* a, const H2CTFEString* b) {
    if (a->len != b->len) {
        return 0;
    }
    if (a->len == 0) {
        return 1;
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

static int H2CTFEOptionalPayload(const H2CTFEValue* opt, const H2CTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (opt == NULL || opt->kind != H2CTFEValue_OPTIONAL || outPayload == NULL) {
        return 0;
    }
    if (opt->b == 0u) {
        return 1;
    }
    if (opt->s.bytes == NULL) {
        return 0;
    }
    *outPayload = (const H2CTFEValue*)opt->s.bytes;
    return 1;
}

static int H2CTFEValueEqRec(const H2CTFEValue* a, const H2CTFEValue* b, uint32_t depth) {
    const H2CTFEValue* aPayload = NULL;
    const H2CTFEValue* bPayload = NULL;
    if (a == NULL || b == NULL || a->kind != b->kind || depth > 32u) {
        return 0;
    }
    switch (a->kind) {
        case H2CTFEValue_INT:    return a->i64 == b->i64;
        case H2CTFEValue_FLOAT:  return a->f64 == b->f64;
        case H2CTFEValue_BOOL:   return a->b == b->b;
        case H2CTFEValue_STRING: return H2CTFEStringEq(&a->s, &b->s);
        case H2CTFEValue_TYPE:   return a->typeTag == b->typeTag;
        case H2CTFEValue_SPAN:
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
        case H2CTFEValue_NULL: return 1;
        case H2CTFEValue_OPTIONAL:
            if (!H2CTFEOptionalPayload(a, &aPayload) || !H2CTFEOptionalPayload(b, &bPayload)) {
                return 0;
            }
            if (a->b == 0u || b->b == 0u) {
                return a->b == b->b;
            }
            return H2CTFEValueEqRec(aPayload, bPayload, depth + 1u);
        default: return 0;
    }
}

static int H2CTFEStringConcat(
    H2MirExecRun* r, const H2CTFEString* a, const H2CTFEString* b, H2CTFEString* out) {
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
    dst = (uint8_t*)H2ArenaAlloc(r->arena, totalLen, (uint32_t)_Alignof(uint8_t));
    if (dst == NULL) {
        H2CTFESetDiag(r->env.diag, H2Diag_ARENA_OOM, 0, 0);
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

static int H2CTFEEvalUnary(H2TokenKind op, const H2CTFEValue* in, H2CTFEValue* out) {
    H2CTFEValueInvalid(out);
    if (op == H2Tok_ADD && in->kind == H2CTFEValue_INT) {
        *out = *in;
        return 1;
    }
    if (op == H2Tok_ADD && in->kind == H2CTFEValue_FLOAT) {
        *out = *in;
        return 1;
    }
    if (op == H2Tok_SUB && in->kind == H2CTFEValue_INT) {
        if (in->i64 == INT64_MIN) {
            return 0;
        }
        out->kind = H2CTFEValue_INT;
        out->i64 = -in->i64;
        return 1;
    }
    if (op == H2Tok_SUB && in->kind == H2CTFEValue_FLOAT) {
        out->kind = H2CTFEValue_FLOAT;
        out->f64 = -in->f64;
        return 1;
    }
    if (op == H2Tok_NOT && in->kind == H2CTFEValue_BOOL) {
        out->kind = H2CTFEValue_BOOL;
        out->b = in->b ? 0u : 1u;
        return 1;
    }
    return 0;
}

static int H2CTFEValueToF64(const H2CTFEValue* value, double* out) {
    if (value == NULL || out == NULL) {
        return 0;
    }
    if (value->kind == H2CTFEValue_INT) {
        *out = (double)value->i64;
        return 1;
    }
    if (value->kind == H2CTFEValue_FLOAT) {
        *out = value->f64;
        return 1;
    }
    return 0;
}

static int H2CTFEAddI64(int64_t a, int64_t b, int64_t* out) {
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

static int H2CTFESubI64(int64_t a, int64_t b, int64_t* out) {
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

static int H2CTFEMulI64(int64_t a, int64_t b, int64_t* out) {
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

static int H2CTFEEvalBinary(
    H2MirExecRun*      r,
    H2TokenKind        op,
    const H2CTFEValue* lhs,
    const H2CTFEValue* rhs,
    H2CTFEValue*       out) {
    int64_t            i = 0;
    double             lf = 0.0;
    double             rf = 0.0;
    const H2CTFEValue* lhsPayload = NULL;
    const H2CTFEValue* rhsPayload = NULL;
    H2CTFEValueInvalid(out);

    if (lhs == NULL || rhs == NULL) {
        return 0;
    }
    if (lhs->kind == H2CTFEValue_OPTIONAL && rhs->kind != H2CTFEValue_OPTIONAL
        && rhs->kind != H2CTFEValue_NULL)
    {
        if (!H2CTFEOptionalPayload(lhs, &lhsPayload) || lhs->b == 0u || lhsPayload == NULL) {
            return 0;
        }
        return H2CTFEEvalBinary(r, op, lhsPayload, rhs, out);
    }
    if (rhs->kind == H2CTFEValue_OPTIONAL && lhs->kind != H2CTFEValue_OPTIONAL
        && lhs->kind != H2CTFEValue_NULL)
    {
        if (!H2CTFEOptionalPayload(rhs, &rhsPayload) || rhs->b == 0u || rhsPayload == NULL) {
            return 0;
        }
        return H2CTFEEvalBinary(r, op, lhs, rhsPayload, out);
    }

    if (lhs->kind == H2CTFEValue_INT && rhs->kind == H2CTFEValue_INT) {
        switch (op) {
            case H2Tok_ADD:
                if (H2CTFEAddI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = H2CTFEValue_INT;
                out->i64 = i;
                return 1;
            case H2Tok_SUB:
                if (H2CTFESubI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = H2CTFEValue_INT;
                out->i64 = i;
                return 1;
            case H2Tok_MUL:
                if (H2CTFEMulI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = H2CTFEValue_INT;
                out->i64 = i;
                return 1;
            case H2Tok_DIV:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                out->kind = H2CTFEValue_INT;
                out->i64 = lhs->i64 / rhs->i64;
                return 1;
            case H2Tok_MOD:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                out->kind = H2CTFEValue_INT;
                out->i64 = lhs->i64 % rhs->i64;
                return 1;
            case H2Tok_AND:
                out->kind = H2CTFEValue_INT;
                out->i64 = lhs->i64 & rhs->i64;
                return 1;
            case H2Tok_OR:
                out->kind = H2CTFEValue_INT;
                out->i64 = lhs->i64 | rhs->i64;
                return 1;
            case H2Tok_XOR:
                out->kind = H2CTFEValue_INT;
                out->i64 = lhs->i64 ^ rhs->i64;
                return 1;
            case H2Tok_LSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                out->kind = H2CTFEValue_INT;
                out->i64 = (int64_t)((uint64_t)lhs->i64 << (uint32_t)rhs->i64);
                return 1;
            case H2Tok_RSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                out->kind = H2CTFEValue_INT;
                out->i64 = (int64_t)((uint64_t)lhs->i64 >> (uint32_t)rhs->i64);
                return 1;
            case H2Tok_EQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->i64 == rhs->i64;
                return 1;
            case H2Tok_NEQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->i64 != rhs->i64;
                return 1;
            case H2Tok_LT:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->i64 < rhs->i64;
                return 1;
            case H2Tok_GT:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->i64 > rhs->i64;
                return 1;
            case H2Tok_LTE:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->i64 <= rhs->i64;
                return 1;
            case H2Tok_GTE:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->i64 >= rhs->i64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == H2CTFEValue_BOOL && rhs->kind == H2CTFEValue_BOOL) {
        switch (op) {
            case H2Tok_LOGICAL_AND:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->b && rhs->b;
                return 1;
            case H2Tok_LOGICAL_OR:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->b || rhs->b;
                return 1;
            case H2Tok_EQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->b == rhs->b;
                return 1;
            case H2Tok_NEQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->b != rhs->b;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == H2CTFEValue_STRING && rhs->kind == H2CTFEValue_STRING) {
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
            case H2Tok_ADD:
                out->kind = H2CTFEValue_STRING;
                return H2CTFEStringConcat(r, &lhs->s, &rhs->s, &out->s) == 0;
            case H2Tok_EQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = H2CTFEStringEq(&lhs->s, &rhs->s);
                return 1;
            case H2Tok_NEQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = !H2CTFEStringEq(&lhs->s, &rhs->s);
                return 1;
            case H2Tok_LT:
                out->kind = H2CTFEValue_BOOL;
                out->b = cmp < 0;
                return 1;
            case H2Tok_GT:
                out->kind = H2CTFEValue_BOOL;
                out->b = cmp > 0;
                return 1;
            case H2Tok_LTE:
                out->kind = H2CTFEValue_BOOL;
                out->b = cmp <= 0;
                return 1;
            case H2Tok_GTE:
                out->kind = H2CTFEValue_BOOL;
                out->b = cmp >= 0;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == H2CTFEValue_TYPE && rhs->kind == H2CTFEValue_TYPE && r != NULL
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

    if (lhs->kind == H2CTFEValue_TYPE && rhs->kind == H2CTFEValue_TYPE) {
        switch (op) {
            case H2Tok_EQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->typeTag == rhs->typeTag;
                return 1;
            case H2Tok_NEQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lhs->typeTag != rhs->typeTag;
                return 1;
            default: break;
        }
    }

    if (H2CTFEValueToF64(lhs, &lf) && H2CTFEValueToF64(rhs, &rf)) {
        switch (op) {
            case H2Tok_ADD:
                out->kind = H2CTFEValue_FLOAT;
                out->f64 = lf + rf;
                return 1;
            case H2Tok_SUB:
                out->kind = H2CTFEValue_FLOAT;
                out->f64 = lf - rf;
                return 1;
            case H2Tok_MUL:
                out->kind = H2CTFEValue_FLOAT;
                out->f64 = lf * rf;
                return 1;
            case H2Tok_DIV:
                out->kind = H2CTFEValue_FLOAT;
                out->f64 = lf / rf;
                return 1;
            case H2Tok_EQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lf == rf;
                return 1;
            case H2Tok_NEQ:
                out->kind = H2CTFEValue_BOOL;
                out->b = lf != rf;
                return 1;
            case H2Tok_LT:
                out->kind = H2CTFEValue_BOOL;
                out->b = lf < rf;
                return 1;
            case H2Tok_GT:
                out->kind = H2CTFEValue_BOOL;
                out->b = lf > rf;
                return 1;
            case H2Tok_LTE:
                out->kind = H2CTFEValue_BOOL;
                out->b = lf <= rf;
                return 1;
            case H2Tok_GTE:
                out->kind = H2CTFEValue_BOOL;
                out->b = lf >= rf;
                return 1;
            default: return 0;
        }
    }

    if (op == H2Tok_EQ || op == H2Tok_NEQ) {
        int eq = 0;
        if (lhs->kind == H2CTFEValue_NULL && rhs->kind == H2CTFEValue_NULL) {
            eq = 1;
        } else if (lhs->kind == H2CTFEValue_REFERENCE && rhs->kind == H2CTFEValue_NULL) {
            eq = lhs->s.bytes == NULL;
        } else if (lhs->kind == H2CTFEValue_NULL && rhs->kind == H2CTFEValue_REFERENCE) {
            eq = rhs->s.bytes == NULL;
        } else if (lhs->kind == H2CTFEValue_STRING && rhs->kind == H2CTFEValue_NULL) {
            eq = lhs->s.bytes == NULL;
        } else if (lhs->kind == H2CTFEValue_NULL && rhs->kind == H2CTFEValue_STRING) {
            eq = rhs->s.bytes == NULL;
        } else if (lhs->kind == H2CTFEValue_OPTIONAL && rhs->kind == H2CTFEValue_NULL) {
            eq = lhs->b == 0u;
        } else if (lhs->kind == H2CTFEValue_NULL && rhs->kind == H2CTFEValue_OPTIONAL) {
            eq = rhs->b == 0u;
        } else if (lhs->kind == H2CTFEValue_OPTIONAL && rhs->kind == H2CTFEValue_OPTIONAL) {
            eq = H2CTFEValueEqRec(lhs, rhs, 0);
        }
        if (lhs->kind == H2CTFEValue_NULL || rhs->kind == H2CTFEValue_NULL
            || (lhs->kind == H2CTFEValue_OPTIONAL && rhs->kind == H2CTFEValue_OPTIONAL))
        {
            out->kind = H2CTFEValue_BOOL;
            out->b = (op == H2Tok_EQ) ? (eq ? 1u : 0u) : (eq ? 0u : 1u);
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

static int H2CTFEEvalCast(H2MirCastTarget target, const H2CTFEValue* in, H2CTFEValue* out) {
    H2CTFEValueInvalid(out);
    switch (target) {
        case H2MirCastTarget_INT: {
            int64_t asInt = 0;
            if (in->kind == H2CTFEValue_INT) {
                asInt = in->i64;
            } else if (in->kind == H2CTFEValue_BOOL) {
                asInt = in->b ? 1 : 0;
            } else if (in->kind == H2CTFEValue_FLOAT) {
                if (in->f64 != in->f64 || in->f64 > (double)INT64_MAX
                    || in->f64 < (double)INT64_MIN)
                {
                    return 0;
                }
                asInt = (int64_t)in->f64;
            } else if (in->kind == H2CTFEValue_NULL) {
                asInt = 0;
            } else {
                return 0;
            }
            out->kind = H2CTFEValue_INT;
            out->i64 = asInt;
            return 1;
        }
        case H2MirCastTarget_FLOAT: {
            double asFloat = 0.0;
            if (in->kind == H2CTFEValue_FLOAT) {
                asFloat = in->f64;
            } else if (in->kind == H2CTFEValue_INT) {
                asFloat = (double)in->i64;
            } else if (in->kind == H2CTFEValue_BOOL) {
                asFloat = in->b ? 1.0 : 0.0;
            } else if (in->kind == H2CTFEValue_NULL) {
                asFloat = 0.0;
            } else {
                return 0;
            }
            out->kind = H2CTFEValue_FLOAT;
            out->f64 = asFloat;
            return 1;
        }
        case H2MirCastTarget_BOOL: {
            uint8_t asBool = 0;
            if (in->kind == H2CTFEValue_BOOL) {
                asBool = in->b ? 1u : 0u;
            } else if (in->kind == H2CTFEValue_INT) {
                asBool = in->i64 != 0 ? 1u : 0u;
            } else if (in->kind == H2CTFEValue_FLOAT) {
                asBool = in->f64 != 0.0 ? 1u : 0u;
            } else if (in->kind == H2CTFEValue_OPTIONAL) {
                asBool = in->b != 0u ? 1u : 0u;
            } else if (in->kind == H2CTFEValue_STRING) {
                asBool = 1u;
            } else if (in->kind == H2CTFEValue_NULL) {
                asBool = 0u;
            } else {
                return 0;
            }
            out->kind = H2CTFEValue_BOOL;
            out->b = asBool;
            return 1;
        }
        case H2MirCastTarget_PTR_LIKE:
            if (in->kind == H2CTFEValue_REFERENCE || in->kind == H2CTFEValue_NULL
                || in->kind == H2CTFEValue_STRING)
            {
                *out = *in;
                return 1;
            }
            return 0;
        case H2MirCastTarget_STR_VIEW: *out = *in; return 1;
        default:                       return 0;
    }
}

int H2MirEvalChunk(
    H2Arena* _Nonnull arena,
    H2MirChunk chunk,
    const H2MirExecEnv* _Nullable env,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    H2MirExecRun run;
    if (H2MirInitRun(&run, arena, chunk, NULL, NULL, 0u, NULL, 0u, env, 1, outValue, outIsConst)
        != 0)
    {
        return -1;
    }
    return H2MirRunLoop(&run, outValue, outIsConst);
}

static int H2MirEvalFunctionInternal(
    H2Arena* _Nonnull arena,
    const H2MirProgram* _Nonnull program,
    uint32_t functionIndex,
    const H2MirExecValue* _Nullable args,
    uint32_t argCount,
    uint16_t callFlags,
    const H2MirExecEnv* _Nullable env,
    int validateProgram,
    int clearDiag,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    const H2MirFunction* fn;
    H2MirChunk           chunk;
    H2MirExecEnv         frameEnv;
    H2MirExecRun         run;
    H2MirExecValue       variadicPackValue;
    int                  enteredFunction = 0;
    int                  boundFrame = 0;
    int                  rc = 0;
    if (program == NULL || functionIndex >= program->funcLen) {
        if (env != NULL) {
            H2CTFESetDiag(env->diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }
    if (validateProgram && H2MirValidateProgram(program, env != NULL ? env->diag : NULL) != 0) {
        return -1;
    }
    fn = &program->funcs[functionIndex];
    if (fn->instStart > program->instLen || fn->instLen > program->instLen - fn->instStart) {
        if (env != NULL) {
            H2CTFESetDiag(env->diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }
    if (fn->localCount < fn->paramCount) {
        return 0;
    }
    if ((fn->flags & H2MirFunctionFlag_VARIADIC) != 0u) {
        uint32_t          fixedCount = fn->paramCount > 0u ? fn->paramCount - 1u : 0u;
        const H2MirLocal* variadicLocal = NULL;
        int               packIsConst = 0;
        if (argCount < fixedCount || fn->paramCount == 0u || env == NULL
            || env->makeVariadicPack == NULL)
        {
            return 0;
        }
        variadicLocal = &program->locals[fn->localStart + fn->paramCount - 1u];
        H2CTFEValueInvalid(&variadicPackValue);
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
    if (H2MirInitRun(
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
    if ((fn->flags & H2MirFunctionFlag_VARIADIC) != 0u) {
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
    rc = H2MirRunLoop(&run, outValue, outIsConst);
end:
    if (boundFrame && frameEnv.unbindFrame != NULL) {
        frameEnv.unbindFrame(frameEnv.frameCtx);
    }
    if (enteredFunction && frameEnv.leaveFunction != NULL) {
        frameEnv.leaveFunction(frameEnv.functionCtx);
    }
    return rc;
}

int H2MirEvalFunction(
    H2Arena* _Nonnull arena,
    const H2MirProgram* _Nonnull program,
    uint32_t functionIndex,
    const H2MirExecValue* _Nullable args,
    uint32_t argCount,
    const H2MirExecEnv* _Nullable env,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    return H2MirEvalFunctionInternal(
        arena, program, functionIndex, args, argCount, 0u, env, 1, 1, outValue, outIsConst);
}
H2_API_END
