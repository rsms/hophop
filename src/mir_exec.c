#include "libhop-impl.h"
#include "ctfe.h"
#include "mir.h"
#include "mir_exec.h"

HOP_API_BEGIN

typedef struct {
    const HOPMirInst*     ip;
    uint32_t              len;
    uint32_t              pc;
    HOPMirExecValue*      stack;
    uint32_t              stackLen;
    uint32_t              stackCap;
    HOPMirExecValue*      locals;
    uint32_t              localCount;
    HOPArena*             arena;
    const HOPMirProgram*  program;
    const HOPMirFunction* function;
    HOPMirExecEnv         env;
    uint32_t              backwardJumpCount;
} HOPMirExecRun;

#define HOPMIR_EXEC_FUNCTION_REF_TAG_FLAG   (UINT64_C(1) << 57)
#define HOPMIR_EXEC_BYTE_REF_PROXY_TAG_FLAG (UINT64_C(1) << 56)

static void HOPCTFESetDiag(HOPDiag* diag, HOPDiagCode code, uint32_t start, uint32_t end);
static void HOPCTFEValueInvalid(HOPCTFEValue* v);
static int  HOPCTFEPush(HOPMirExecRun* r, const HOPMirExecValue* v);
static int  HOPCTFEPop(HOPMirExecRun* r, HOPMirExecValue* out);
static int  HOPCTFEParseIntLiteral(HOPStrView src, uint32_t start, uint32_t end, int64_t* out);
static int  HOPCTFEParseFloatLiteral(
    HOPArena* arena, HOPStrView src, uint32_t start, uint32_t end, double* out);
static int HOPCTFEParseBoolLiteral(HOPStrView src, uint32_t start, uint32_t end, uint8_t* out);
static int HOPCTFEOptionalPayload(const HOPCTFEValue* opt, const HOPCTFEValue** outPayload);
static int HOPCTFEEvalUnary(HOPTokenKind op, const HOPCTFEValue* in, HOPCTFEValue* out);
static int HOPCTFEEvalBinary(
    HOPMirExecRun*      r,
    HOPTokenKind        op,
    const HOPCTFEValue* lhs,
    const HOPCTFEValue* rhs,
    HOPCTFEValue*       out);
static int HOPCTFEEvalCast(HOPMirCastTarget target, const HOPCTFEValue* in, HOPCTFEValue* out);
static const HOPMirLocal* HOPMirGetLocalMeta(const HOPMirExecRun* run, uint32_t slot);
static int                HOPMirCoerceValueForType(
    const HOPMirExecRun* run, uint32_t typeRefIndex, HOPMirExecValue* inOutValue);
static void HOPMirSetReason(
    const HOPMirExecRun* _Nullable run,
    const HOPMirInst* _Nullable ins,
    const char* _Nonnull reason);

static int HOPMirInitRun(
    HOPMirExecRun* _Nonnull run,
    HOPArena* _Nonnull arena,
    HOPMirChunk chunk,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    uint32_t localCount,
    const HOPMirExecValue* _Nullable args,
    uint32_t argCount,
    const HOPMirExecEnv* _Nullable env,
    int clearDiag,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    if (clearDiag && env != NULL && env->diag != NULL) {
        *env->diag = (HOPDiag){ 0 };
    }
    if (run == NULL || arena == NULL || outValue == NULL || outIsConst == NULL) {
        if (env != NULL) {
            HOPCTFESetDiag(env->diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }

    HOPCTFEValueInvalid(outValue);
    *outIsConst = 0;

    memset(run, 0, sizeof(*run));
    run->ip = chunk.v;
    run->len = chunk.len;
    run->pc = 0;
    run->stackCap = chunk.len + argCount + 1u;
    run->stack = (HOPMirExecValue*)HOPArenaAlloc(
        arena, sizeof(HOPMirExecValue) * run->stackCap, (uint32_t)_Alignof(HOPMirExecValue));
    if (run->stack == NULL) {
        if (env != NULL) {
            HOPCTFESetDiag(env->diag, HOPDiag_ARENA_OOM, 0, 0);
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
        run->locals = (HOPMirExecValue*)HOPArenaAlloc(
            arena, sizeof(HOPMirExecValue) * localCount, (uint32_t)_Alignof(HOPMirExecValue));
        if (run->locals == NULL) {
            if (env != NULL) {
                HOPCTFESetDiag(env->diag, HOPDiag_ARENA_OOM, 0, 0);
            }
            return -1;
        }
        for (i = 0; i < localCount; i++) {
            HOPCTFEValueInvalid(&run->locals[i]);
        }
        if (argCount > localCount) {
            return 0;
        }
        for (i = 0; i < argCount; i++) {
            run->locals[i] = args[i];
            if (program != NULL && function != NULL) {
                const HOPMirLocal* local = HOPMirGetLocalMeta(run, i);
                if (local->typeRef != UINT32_MAX) {
                    int coerceRc = HOPMirCoerceValueForType(run, local->typeRef, &run->locals[i]);
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

static const HOPMirLocal* HOPMirGetLocalMeta(const HOPMirExecRun* run, uint32_t slot) {
    static const HOPMirLocal invalidLocal = { UINT32_MAX, HOPMirLocalFlag_NONE, 0u, 0u };
    if (run == NULL || run->program == NULL || run->function == NULL || slot >= run->localCount
        || run->function->localStart > run->program->localLen
        || slot >= run->program->localLen - run->function->localStart)
    {
        return &invalidLocal;
    }
    return &run->program->locals[run->function->localStart + slot];
}

static int HOPMirCoerceValueForType(
    const HOPMirExecRun* run, uint32_t typeRefIndex, HOPMirExecValue* inOutValue) {
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

static uint32_t HOPMirResolveHostId(const HOPMirExecRun* run, const HOPMirInst* ins) {
    if (run == NULL || ins == NULL || run->program == NULL || run->program->hostLen == 0
        || ins->aux >= run->program->hostLen)
    {
        return HOPMirHostTarget_INVALID;
    }
    return run->program->hosts[ins->aux].target;
}

static uint32_t HOPMirResolvedCallArgCount(const HOPMirInst* ins) {
    if (ins == NULL) {
        return 0;
    }
    return HOPMirCallArgCountFromTok(ins->tok);
}

static int HOPMirResolvedCallDropsReceiverArg0(const HOPMirInst* ins) {
    return ins != NULL && HOPMirCallTokDropsReceiverArg0(ins->tok);
}

static void HOPMirSetReason(
    const HOPMirExecRun* _Nullable run,
    const HOPMirInst* _Nullable ins,
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

static HOPCTFEValue* _Nullable HOPMirReferenceTarget(HOPCTFEValue* value) {
    if (value == NULL || value->kind != HOPCTFEValue_REFERENCE || value->s.bytes == NULL) {
        return NULL;
    }
    return (HOPCTFEValue*)value->s.bytes;
}

static int HOPMirEvalFunctionInternal(
    HOPArena* _Nonnull arena,
    const HOPMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const HOPMirExecValue* _Nullable args,
    uint32_t argCount,
    uint16_t callFlags,
    const HOPMirExecEnv* _Nullable env,
    int validateProgram,
    int clearDiag,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);

static int HOPMirConstToValue(const HOPMirConst* _Nonnull in, HOPMirExecValue* _Nonnull out) {
    double f64 = 0.0;
    if (in == NULL || out == NULL) {
        return 0;
    }
    HOPCTFEValueInvalid(out);
    switch (in->kind) {
        case HOPMirConst_INT:
            out->kind = HOPCTFEValue_INT;
            out->i64 = (int64_t)in->bits;
            return 1;
        case HOPMirConst_FLOAT:
            out->kind = HOPCTFEValue_FLOAT;
            memcpy(&f64, &in->bits, sizeof(f64));
            out->f64 = f64;
            return 1;
        case HOPMirConst_BOOL:
            out->kind = HOPCTFEValue_BOOL;
            out->b = in->bits != 0;
            return 1;
        case HOPMirConst_STRING:
            out->kind = HOPCTFEValue_STRING;
            out->s.bytes = (const uint8_t*)in->bytes.ptr;
            out->s.len = in->bytes.len;
            return 1;
        case HOPMirConst_TYPE:
            out->kind = HOPCTFEValue_TYPE;
            out->typeTag = in->bits;
            out->s.bytes = (const uint8_t*)in->bytes.ptr;
            out->s.len = in->bytes.len;
            return 1;
        case HOPMirConst_FUNCTION: HOPMirValueSetFunctionRef(out, (uint32_t)in->bits); return 1;
        case HOPMirConst_NULL:     out->kind = HOPCTFEValue_NULL; return 1;
        default:                   return 0;
    }
}

void HOPMirValueSetFunctionRef(HOPMirExecValue* _Nonnull value, uint32_t functionIndex) {
    if (value == NULL) {
        return;
    }
    HOPCTFEValueInvalid(value);
    value->kind = HOPCTFEValue_TYPE;
    value->typeTag = HOPMIR_EXEC_FUNCTION_REF_TAG_FLAG | (uint64_t)functionIndex;
}

static int HOPMirValueIsFunctionRef(const HOPMirExecValue* value, uint32_t* _Nullable outFnIndex) {
    if (value == NULL || value->kind != HOPCTFEValue_TYPE
        || (value->typeTag & HOPMIR_EXEC_FUNCTION_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outFnIndex != NULL) {
        *outFnIndex = (uint32_t)(value->typeTag & ~HOPMIR_EXEC_FUNCTION_REF_TAG_FLAG);
    }
    return 1;
}

int HOPMirValueAsFunctionRef(
    const HOPMirExecValue* _Nonnull value, uint32_t* _Nullable outFunctionIndex) {
    return HOPMirValueIsFunctionRef(value, outFunctionIndex);
}

void HOPMirValueSetByteRefProxy(HOPMirExecValue* _Nonnull value, uint8_t* _Nullable targetByte) {
    if (value == NULL) {
        return;
    }
    HOPCTFEValueInvalid(value);
    value->kind = HOPCTFEValue_INT;
    value->i64 = targetByte != NULL ? (int64_t)(*targetByte) : 0;
    value->typeTag = HOPMIR_EXEC_BYTE_REF_PROXY_TAG_FLAG;
    value->s.bytes = targetByte;
    value->s.len = targetByte != NULL ? 1u : 0u;
}

int HOPMirValueAsByteRefProxy(
    const HOPMirExecValue* _Nonnull value, uint8_t* _Nullable* _Nullable outTargetByte) {
    if (value == NULL || value->kind != HOPCTFEValue_INT
        || (value->typeTag & HOPMIR_EXEC_BYTE_REF_PROXY_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return 0;
    }
    if (outTargetByte != NULL) {
        *outTargetByte = (uint8_t*)value->s.bytes;
    }
    return 1;
}

void HOPMirExecEnvDisableDynamicResolution(HOPMirExecEnv* env) {
    if (env == NULL) {
        return;
    }
    env->resolveIdent = NULL;
    env->resolveCall = NULL;
    env->resolveCtx = NULL;
}

static void HOPMirResolveSymbolName(
    const HOPMirExecRun* _Nonnull run,
    const HOPMirInst* _Nonnull ins,
    HOPMirSymbolKind expectedKind,
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

static void HOPMirResolveFieldName(
    const HOPMirExecRun* _Nonnull run,
    const HOPMirInst* _Nonnull ins,
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

static int HOPMirRunLoop(
    HOPMirExecRun* _Nonnull run, HOPMirExecValue* _Nonnull outValue, int* _Nonnull outIsConst) {
    while (run->pc < run->len) {
        const HOPMirInst* ins = &run->ip[run->pc++];
        switch (ins->op) {
            case HOPMirOp_PUSH_CONST: {
                HOPMirExecValue v;
                if (run->program == NULL || ins->aux >= run->program->constLen) {
                    return 0;
                }
                if (!HOPMirConstToValue(&run->program->consts[ins->aux], &v)) {
                    return 0;
                }
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_PUSH_INT: {
                HOPCTFEValue  v;
                uint32_t      rune = 0;
                HOPRuneLitErr runeErr = { 0 };
                v.kind = HOPCTFEValue_INT;
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
                if ((HOPTokenKind)ins->tok == HOPTok_RUNE) {
                    if (HOPDecodeRuneLiteralValidate(
                            run->env.src.ptr, ins->start, ins->end, &rune, &runeErr)
                        != 0)
                    {
                        HOPCTFESetDiag(
                            run->env.diag,
                            HOPRuneLitErrDiagCode(runeErr.kind),
                            runeErr.start,
                            runeErr.end);
                        return -1;
                    }
                    v.i64 = (int64_t)rune;
                } else if ((HOPTokenKind)ins->tok == HOPTok_INVALID) {
                    v.i64 = (int64_t)(int32_t)ins->aux;
                } else {
                    if (HOPCTFEParseIntLiteral(run->env.src, ins->start, ins->end, &v.i64) != 0) {
                        return 0;
                    }
                }
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_PUSH_FLOAT: {
                HOPCTFEValue v;
                v.kind = HOPCTFEValue_FLOAT;
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
                if (HOPCTFEParseFloatLiteral(run->arena, run->env.src, ins->start, ins->end, &v.f64)
                    != 0)
                {
                    return 0;
                }
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_PUSH_BOOL: {
                HOPCTFEValue v;
                uint8_t      b = 0;
                if (HOPCTFEParseBoolLiteral(run->env.src, ins->start, ins->end, &b) != 0) {
                    return 0;
                }
                v.kind = HOPCTFEValue_BOOL;
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
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_PUSH_STRING: {
                HOPCTFEValue    v;
                HOPStringLitErr litErr = { 0 };
                uint8_t*        bytes = NULL;
                uint32_t        len = 0;
                if (HOPDecodeStringLiteralArena(
                        run->arena, run->env.src.ptr, ins->start, ins->end, &bytes, &len, &litErr)
                    != 0)
                {
                    HOPCTFESetDiag(
                        run->env.diag,
                        HOPStringLitErrDiagCode(litErr.kind),
                        litErr.start,
                        litErr.end);
                    return -1;
                }
                v.kind = HOPCTFEValue_STRING;
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
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_PUSH_NULL: {
                HOPCTFEValue v;
                v.kind = HOPCTFEValue_NULL;
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
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_LOCAL_LOAD:
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                if (HOPCTFEPush(run, &run->locals[ins->aux]) != 0) {
                    return -1;
                }
                break;
            case HOPMirOp_LOCAL_ADDR: {
                HOPMirExecValue v;
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                HOPCTFEValueInvalid(&v);
                v.kind = HOPCTFEValue_REFERENCE;
                v.s.bytes = (const uint8_t*)&run->locals[ins->aux];
                v.s.len = 0;
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_ADDR_OF: {
                HOPMirExecValue  value;
                HOPMirExecValue* target;
                if (HOPCTFEPop(run, &value) != 0) {
                    return 0;
                }
                target = (HOPMirExecValue*)HOPArenaAlloc(
                    run->arena, sizeof(*target), (uint32_t)_Alignof(HOPMirExecValue));
                if (target == NULL) {
                    HOPCTFESetDiag(run->env.diag, HOPDiag_ARENA_OOM, ins->start, ins->end);
                    return -1;
                }
                *target = value;
                HOPCTFEValueInvalid(&value);
                value.kind = HOPCTFEValue_REFERENCE;
                value.s.bytes = (const uint8_t*)target;
                value.s.len = 0;
                if (HOPCTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_LOCAL_ZERO: {
                const HOPMirLocal* local;
                HOPMirExecValue    v;
                int                isConst = 0;
                if (ins->aux >= run->localCount || run->env.zeroInitLocal == NULL) {
                    return 0;
                }
                local = HOPMirGetLocalMeta(run, ins->aux);
                if (local->typeRef == UINT32_MAX || local->typeRef >= run->program->typeLen) {
                    return 0;
                }
                HOPCTFEValueInvalid(&v);
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
            case HOPMirOp_DEREF_LOAD: {
                HOPMirExecValue ref;
                HOPCTFEValue*   target;
                if (HOPCTFEPop(run, &ref) != 0) {
                    return 0;
                }
                target = HOPMirReferenceTarget(&ref);
                if (target == NULL) {
                    return 0;
                }
                {
                    uint8_t*        bytePtr = NULL;
                    HOPMirExecValue out;
                    if (HOPMirValueAsByteRefProxy(target, &bytePtr) && bytePtr != NULL) {
                        out = *target;
                        out.i64 = (int64_t)(*bytePtr);
                        out.f64 = 0.0;
                        out.b = 0;
                        out.typeTag = 0;
                        out.s.bytes = NULL;
                        out.s.len = 0;
                        if (HOPCTFEPush(run, &out) != 0) {
                            return -1;
                        }
                        break;
                    }
                }
                if (HOPCTFEPush(run, target) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_DEREF_STORE: {
                HOPMirExecValue ref;
                HOPMirExecValue v;
                HOPCTFEValue*   target;
                if (HOPCTFEPop(run, &ref) != 0 || HOPCTFEPop(run, &v) != 0) {
                    return 0;
                }
                target = HOPMirReferenceTarget(&ref);
                if (target == NULL) {
                    return 0;
                }
                {
                    uint8_t* bytePtr = NULL;
                    int64_t  byteValue = 0;
                    if (HOPMirValueAsByteRefProxy(target, &bytePtr) && bytePtr != NULL) {
                        if (HOPCTFEValueToInt64(&v, &byteValue) != 0 || byteValue < 0
                            || byteValue > 255)
                        {
                            return 0;
                        }
                        *bytePtr = (uint8_t)byteValue;
                        target->i64 = byteValue;
                        break;
                    }
                }
                *target = v;
                break;
            }
            case HOPMirOp_LOCAL_STORE: {
                const HOPMirLocal* local;
                HOPMirExecValue    v;
                if (ins->aux >= run->localCount) {
                    return 0;
                }
                if (HOPCTFEPop(run, &v) != 0) {
                    return 0;
                }
                local = HOPMirGetLocalMeta(run, ins->aux);
                if (local->typeRef != UINT32_MAX) {
                    int coerceRc = HOPMirCoerceValueForType(run, local->typeRef, &v);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                run->locals[ins->aux] = v;
                break;
            }
            case HOPMirOp_DROP: {
                HOPMirExecValue v;
                if (HOPCTFEPop(run, &v) != 0) {
                    return 0;
                }
                break;
            }
            case HOPMirOp_JUMP:
                if (ins->aux >= run->len) {
                    return 0;
                }
                if (run->env.backwardJumpLimit != 0 && ins->aux < run->pc) {
                    if (++run->backwardJumpCount > run->env.backwardJumpLimit) {
                        HOPMirSetReason(run, ins, "for-loop exceeded const-eval iteration limit");
                        return 0;
                    }
                }
                run->pc = ins->aux;
                break;
            case HOPMirOp_JUMP_IF_FALSE: {
                HOPMirExecValue cond;
                HOPMirExecValue condBool;
                if (ins->aux >= run->len) {
                    return 0;
                }
                if (HOPCTFEPop(run, &cond) != 0) {
                    return 0;
                }
                if (!HOPCTFEEvalCast(HOPMirCastTarget_BOOL, &cond, &condBool)) {
                    return 0;
                }
                if (!condBool.b) {
                    if (run->env.backwardJumpLimit != 0 && ins->aux < run->pc) {
                        if (++run->backwardJumpCount > run->env.backwardJumpLimit) {
                            HOPMirSetReason(
                                run, ins, "for-loop exceeded const-eval iteration limit");
                            return 0;
                        }
                    }
                    run->pc = ins->aux;
                }
                break;
            }
            case HOPMirOp_ASSERT: {
                HOPMirExecValue cond;
                HOPMirExecValue condBool;
                if (HOPCTFEPop(run, &cond) != 0) {
                    return 0;
                }
                if (!HOPCTFEEvalCast(HOPMirCastTarget_BOOL, &cond, &condBool)) {
                    return 0;
                }
                if (!condBool.b) {
                    HOPMirSetReason(
                        run, ins, "assert condition evaluated to false during const evaluation");
                    return 0;
                }
                break;
            }
            case HOPMirOp_LOAD_IDENT: {
                HOPCTFEValue v;
                int          idIsConst = 0;
                uint32_t     nameStart = ins->start;
                uint32_t     nameEnd = ins->end;
                if (run->env.resolveIdent == NULL) {
                    return 0;
                }
                HOPMirResolveSymbolName(run, ins, HOPMirSymbol_IDENT, &nameStart, &nameEnd);
                HOPCTFEValueInvalid(&v);
                if (run->env.resolveIdent(
                        run->env.resolveCtx, nameStart, nameEnd, &v, &idIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!idIsConst) {
                    return 0;
                }
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_STORE_IDENT: {
                HOPCTFEValue value;
                int          idIsConst = 0;
                uint32_t     nameStart = ins->start;
                uint32_t     nameEnd = ins->end;
                if (HOPCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (run->env.assignIdent == NULL) {
                    return 0;
                }
                HOPMirResolveSymbolName(run, ins, HOPMirSymbol_IDENT, &nameStart, &nameEnd);
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
            case HOPMirOp_CALL: {
                HOPCTFEValue* args = NULL;
                HOPCTFEValue  v;
                int           callIsConst = 0;
                uint32_t      argCount = HOPMirResolvedCallArgCount(ins);
                uint32_t      i;
                uint32_t      nameStart = ins->start;
                uint32_t      nameEnd = ins->end;
                if (run->env.resolveCall == NULL && run->env.resolveCallPre == NULL) {
                    return 0;
                }
                HOPMirResolveSymbolName(run, ins, HOPMirSymbol_CALL, &nameStart, &nameEnd);
                if (run->env.resolveCallPre != NULL) {
                    int preRc;
                    HOPCTFEValueInvalid(&v);
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
                        if (HOPCTFEPush(run, &v) != 0) {
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
                    args = (HOPCTFEValue*)HOPArenaAlloc(
                        run->arena,
                        sizeof(HOPCTFEValue) * argCount,
                        (uint32_t)_Alignof(HOPCTFEValue));
                    if (args == NULL) {
                        HOPCTFESetDiag(run->env.diag, HOPDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (HOPCTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                HOPCTFEValueInvalid(&v);
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
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_CALL_HOST: {
                HOPMirExecValue* args = NULL;
                HOPMirExecValue  v;
                int              callOk = 0;
                uint32_t         argCount = HOPMirResolvedCallArgCount(ins);
                uint32_t         i;
                uint32_t         callArgOffset = 0;
                if (run->env.hostCall == NULL) {
                    return 0;
                }
                if (argCount > run->stackLen) {
                    return 0;
                }
                if (argCount > 0u) {
                    args = (HOPMirExecValue*)HOPArenaAlloc(
                        run->arena,
                        sizeof(HOPMirExecValue) * argCount,
                        (uint32_t)_Alignof(HOPMirExecValue));
                    if (args == NULL) {
                        HOPCTFESetDiag(run->env.diag, HOPDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (HOPCTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                HOPCTFEValueInvalid(&v);
                if (HOPMirResolvedCallDropsReceiverArg0(ins)) {
                    if (argCount == 0u) {
                        return 0;
                    }
                    callArgOffset = 1u;
                }
                if (run->env.hostCall(
                        run->env.hostCtx,
                        HOPMirResolveHostId(run, ins),
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
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_CALL_FN: {
                HOPMirExecValue  v;
                HOPMirExecValue* args = NULL;
                int              callOk = 0;
                uint32_t         argCount = HOPMirResolvedCallArgCount(ins);
                uint32_t         i;
                uint32_t         callArgOffset = 0;
                if (run->program == NULL || ins->aux >= run->program->funcLen) {
                    return 0;
                }
                if (argCount > run->stackLen) {
                    return 0;
                }
                if (argCount != 0u) {
                    args = (HOPMirExecValue*)HOPArenaAlloc(
                        run->arena,
                        sizeof(HOPMirExecValue) * argCount,
                        (uint32_t)_Alignof(HOPMirExecValue));
                    if (args == NULL) {
                        HOPCTFESetDiag(run->env.diag, HOPDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (HOPCTFEPop(run, &args[i - 1]) != 0) {
                        return 0;
                    }
                }
                if (HOPMirResolvedCallDropsReceiverArg0(ins)) {
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
                if (HOPMirEvalFunctionInternal(
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
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_CALL_INDIRECT: {
                HOPMirExecValue  callee;
                HOPMirExecValue  v;
                HOPMirExecValue* args = NULL;
                int              callOk = 0;
                uint32_t         fnIndex = 0;
                uint32_t         argCount = HOPMirResolvedCallArgCount(ins);
                uint32_t         i;
                if (run->program == NULL || argCount + 1u > run->stackLen) {
                    HOPMirSetReason(
                        run, ins, "indirect call stack is invalid during const evaluation");
                    return 0;
                }
                if (argCount != 0u) {
                    args = (HOPMirExecValue*)HOPArenaAlloc(
                        run->arena,
                        sizeof(HOPMirExecValue) * argCount,
                        (uint32_t)_Alignof(HOPMirExecValue));
                    if (args == NULL) {
                        HOPCTFESetDiag(run->env.diag, HOPDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = argCount; i > 0; i--) {
                    if (HOPCTFEPop(run, &args[i - 1]) != 0) {
                        HOPMirSetReason(
                            run,
                            ins,
                            "indirect call arguments are not available during const evaluation");
                        return 0;
                    }
                }
                if (HOPCTFEPop(run, &callee) != 0) {
                    HOPMirSetReason(
                        run, ins, "indirect call target is not available during const evaluation");
                    return 0;
                }
                if (!HOPMirValueIsFunctionRef(&callee, &fnIndex)
                    || fnIndex >= run->program->funcLen)
                {
                    HOPMirSetReason(run, ins, "indirect call target is not a function");
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
                if (HOPMirEvalFunctionInternal(
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
                if (HOPCTFEPush(run, &v) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_UNARY: {
                HOPCTFEValue in;
                HOPCTFEValue out;
                if (HOPCTFEPop(run, &in) != 0) {
                    return 0;
                }
                if (!HOPCTFEEvalUnary((HOPTokenKind)ins->tok, &in, &out)) {
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_BINARY: {
                HOPCTFEValue lhs;
                HOPCTFEValue rhs;
                HOPCTFEValue out;
                int          binaryRc;
                if (HOPCTFEPop(run, &rhs) != 0 || HOPCTFEPop(run, &lhs) != 0) {
                    return 0;
                }
                binaryRc = HOPCTFEEvalBinary(run, (HOPTokenKind)ins->tok, &lhs, &rhs, &out);
                if (binaryRc < 0) {
                    return -1;
                }
                if (binaryRc == 0) {
                    HOPMirSetReason(
                        run, ins, "binary operation is not supported during const evaluation");
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_INDEX: {
                HOPCTFEValue base;
                HOPCTFEValue idx;
                HOPCTFEValue out;
                int64_t      idxInt = 0;
                int          indexIsConst = 0;
                if (HOPCTFEPop(run, &idx) != 0 || HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (base.kind == HOPCTFEValue_STRING && HOPCTFEValueToInt64(&idx, &idxInt) == 0) {
                    if (idxInt < 0) {
                        HOPMirSetReason(run, ins, "index is negative in const evaluation");
                        return 0;
                    }
                    if ((uint64_t)idxInt >= (uint64_t)base.s.len) {
                        HOPMirSetReason(run, ins, "index is out of bounds in const evaluation");
                        return 0;
                    }
                    out.kind = HOPCTFEValue_INT;
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
                    if (HOPCTFEPush(run, &out) != 0) {
                        return -1;
                    }
                    break;
                }
                if (run->env.indexValue == NULL) {
                    return 0;
                }
                HOPCTFEValueInvalid(&out);
                if (run->env.indexValue(
                        run->env.indexValueCtx, &base, &idx, &out, &indexIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!indexIsConst) {
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_SEQ_LEN: {
                HOPCTFEValue base;
                HOPCTFEValue out;
                int          lenIsConst = 0;
                if (HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (base.kind == HOPCTFEValue_STRING || base.kind == HOPCTFEValue_ARRAY
                    || base.kind == HOPCTFEValue_NULL)
                {
                    out.kind = HOPCTFEValue_INT;
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
                    if (HOPCTFEPush(run, &out) != 0) {
                        return -1;
                    }
                    break;
                }
                if (run->env.sequenceLen == NULL) {
                    return 0;
                }
                HOPCTFEValueInvalid(&out);
                if (run->env.sequenceLen(
                        run->env.sequenceLenCtx, &base, &out, &lenIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!lenIsConst) {
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_STR_CSTR: {
                HOPCTFEValue  base;
                HOPCTFEValue* target;
                if (HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                target = HOPMirReferenceTarget(&base);
                if (target != NULL) {
                    base = *target;
                }
                if (base.kind != HOPCTFEValue_STRING) {
                    return 0;
                }
                if (HOPCTFEPush(run, &base) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_ARRAY_ADDR: {
                HOPCTFEValue base;
                HOPCTFEValue idx;
                HOPCTFEValue out;
                int          addrIsConst = 0;
                if (HOPCTFEPop(run, &idx) != 0 || HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.indexAddr == NULL) {
                    return 0;
                }
                HOPCTFEValueInvalid(&out);
                if (run->env.indexAddr(
                        run->env.indexAddrCtx, &base, &idx, &out, &addrIsConst, run->env.diag)
                    != 0)
                {
                    return -1;
                }
                if (!addrIsConst) {
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_SLICE_MAKE: {
                HOPCTFEValue  base;
                HOPCTFEValue  startValue;
                HOPCTFEValue  endValue;
                HOPCTFEValue  out;
                HOPCTFEValue* startPtr = NULL;
                HOPCTFEValue* endPtr = NULL;
                int           sliceIsConst = 0;
                uint16_t      sliceFlags = ins->tok;
                if (run->env.sliceValue == NULL) {
                    return 0;
                }
                if ((sliceFlags & HOPAstFlag_INDEX_HAS_END) != 0u) {
                    if (HOPCTFEPop(run, &endValue) != 0) {
                        return 0;
                    }
                    endPtr = &endValue;
                }
                if ((sliceFlags & HOPAstFlag_INDEX_HAS_START) != 0u) {
                    if (HOPCTFEPop(run, &startValue) != 0) {
                        return 0;
                    }
                    startPtr = &startValue;
                }
                if (HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                HOPCTFEValueInvalid(&out);
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
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_AGG_MAKE: {
                HOPCTFEValue out;
                int          aggIsConst = 0;
                if (run->env.makeAggregate == NULL) {
                    return 0;
                }
                HOPCTFEValueInvalid(&out);
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
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_AGG_ZERO: {
                HOPCTFEValue out;
                int          aggIsConst = 0;
                if (run->program == NULL || run->env.zeroInitLocal == NULL
                    || ins->aux >= run->program->typeLen)
                {
                    return 0;
                }
                HOPCTFEValueInvalid(&out);
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
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_AGG_SET: {
                HOPCTFEValue value;
                HOPCTFEValue base;
                int          fieldIsConst = 0;
                uint32_t     nameStart = ins->start;
                uint32_t     nameEnd = ins->end;
                if (HOPCTFEPop(run, &value) != 0 || HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggSetField == NULL) {
                    return 0;
                }
                HOPMirResolveFieldName(run, ins, &nameStart, &nameEnd);
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
                if (HOPCTFEPush(run, &base) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_AGG_GET: {
                HOPCTFEValue base;
                HOPCTFEValue out;
                int          fieldIsConst = 0;
                uint32_t     nameStart = ins->start;
                uint32_t     nameEnd = ins->end;
                if (HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggGetField == NULL) {
                    return 0;
                }
                HOPMirResolveFieldName(run, ins, &nameStart, &nameEnd);
                HOPCTFEValueInvalid(&out);
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
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_AGG_ADDR: {
                HOPCTFEValue base;
                HOPCTFEValue out;
                int          fieldIsConst = 0;
                uint32_t     nameStart = ins->start;
                uint32_t     nameEnd = ins->end;
                if (HOPCTFEPop(run, &base) != 0) {
                    return 0;
                }
                if (run->env.aggAddrField == NULL) {
                    return 0;
                }
                HOPMirResolveFieldName(run, ins, &nameStart, &nameEnd);
                HOPCTFEValueInvalid(&out);
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
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_TUPLE_MAKE: {
                HOPMirExecValue* elems = NULL;
                HOPMirExecValue  out;
                int              tupleIsConst = 0;
                uint32_t         elemCount = (uint32_t)ins->tok;
                uint32_t         i;
                if (run->env.makeTuple == NULL || elemCount > run->stackLen) {
                    return 0;
                }
                if (elemCount != 0u) {
                    elems = (HOPMirExecValue*)HOPArenaAlloc(
                        run->arena,
                        sizeof(HOPMirExecValue) * elemCount,
                        (uint32_t)_Alignof(HOPMirExecValue));
                    if (elems == NULL) {
                        HOPCTFESetDiag(run->env.diag, HOPDiag_ARENA_OOM, ins->start, ins->end);
                        return -1;
                    }
                }
                for (i = elemCount; i > 0; i--) {
                    if (HOPCTFEPop(run, &elems[i - 1u]) != 0) {
                        return 0;
                    }
                }
                HOPCTFEValueInvalid(&out);
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
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_ITER_INIT: {
                HOPCTFEValue source;
                HOPCTFEValue iter;
                int          iterIsConst = 0;
                if (HOPCTFEPop(run, &source) != 0) {
                    return 0;
                }
                if (run->env.iterInit == NULL) {
                    return 0;
                }
                HOPCTFEValueInvalid(&iter);
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
                if (HOPCTFEPush(run, &iter) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_ITER_NEXT: {
                HOPCTFEValue iter;
                HOPCTFEValue key;
                HOPCTFEValue value;
                HOPCTFEValue hasItemValue;
                int          hasItem = 0;
                int          keyIsConst = 0;
                int          valueIsConst = 0;
                if (HOPCTFEPop(run, &iter) != 0) {
                    return 0;
                }
                if (run->env.iterNext == NULL) {
                    return 0;
                }
                HOPCTFEValueInvalid(&key);
                HOPCTFEValueInvalid(&value);
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
                    if ((ins->tok & HOPMirIterFlag_HAS_KEY) != 0u && !keyIsConst) {
                        return 0;
                    }
                    if ((ins->tok & HOPMirIterFlag_VALUE_DISCARD) == 0u && !valueIsConst) {
                        return 0;
                    }
                    if ((ins->tok & HOPMirIterFlag_HAS_KEY) != 0u) {
                        if (HOPCTFEPush(run, &key) != 0) {
                            return -1;
                        }
                    }
                    if ((ins->tok & HOPMirIterFlag_VALUE_DISCARD) == 0u) {
                        if (HOPCTFEPush(run, &value) != 0) {
                            return -1;
                        }
                    }
                }
                hasItemValue.kind = HOPCTFEValue_BOOL;
                hasItemValue.i64 = 0;
                hasItemValue.f64 = 0.0;
                hasItemValue.b = hasItem ? 1u : 0u;
                hasItemValue.typeTag = 0;
                hasItemValue.s.bytes = NULL;
                hasItemValue.s.len = 0;
                hasItemValue.span = (HOPCTFESpan){ 0 };
                if (HOPCTFEPush(run, &hasItemValue) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_CAST: {
                HOPCTFEValue in;
                HOPCTFEValue out;
                if (HOPCTFEPop(run, &in) != 0) {
                    return 0;
                }
                if (!HOPCTFEEvalCast((HOPMirCastTarget)ins->tok, &in, &out)) {
                    return 0;
                }
                {
                    int coerceRc = HOPMirCoerceValueForType(run, ins->aux, &out);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_COERCE: {
                HOPCTFEValue value;
                int          coerceRc;
                if (HOPCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (value.kind == HOPCTFEValue_AGGREGATE) {
                    value.typeTag |= HOPCTFEValueTag_AGG_PARTIAL;
                }
                coerceRc = HOPMirCoerceValueForType(run, ins->aux, &value);
                if (coerceRc <= 0) {
                    return coerceRc < 0 ? -1 : 0;
                }
                if (HOPCTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_OPTIONAL_WRAP: {
                HOPCTFEValue  value;
                HOPCTFEValue* payloadCopy;
                if (HOPCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (value.kind == HOPCTFEValue_OPTIONAL) {
                    if (HOPCTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                if (value.kind == HOPCTFEValue_NULL) {
                    value.kind = HOPCTFEValue_OPTIONAL;
                    value.i64 = 0;
                    value.f64 = 0.0;
                    value.b = 0u;
                    value.typeTag = 0;
                    value.s.bytes = NULL;
                    value.s.len = 0;
                    if (HOPCTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                payloadCopy = (HOPCTFEValue*)HOPArenaAlloc(
                    run->arena, sizeof(*payloadCopy), (uint32_t)_Alignof(HOPCTFEValue));
                if (payloadCopy == NULL) {
                    HOPCTFESetDiag(run->env.diag, HOPDiag_ARENA_OOM, ins->start, ins->end);
                    return -1;
                }
                *payloadCopy = value;
                value.kind = HOPCTFEValue_OPTIONAL;
                value.i64 = 0;
                value.f64 = 0.0;
                value.b = 1u;
                value.typeTag = 0;
                value.s.bytes = (const uint8_t*)payloadCopy;
                value.s.len = 0;
                if (HOPCTFEPush(run, &value) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_OPTIONAL_UNWRAP: {
                HOPCTFEValue        value;
                const HOPCTFEValue* payload = NULL;
                if (HOPCTFEPop(run, &value) != 0) {
                    return 0;
                }
                if (!HOPCTFEOptionalPayload(&value, &payload)) {
                    if (value.kind == HOPCTFEValue_NULL) {
                        HOPMirSetReason(run, ins, "unwrap of empty optional in evaluator backend");
                        return 0;
                    }
                    if (HOPCTFEPush(run, &value) != 0) {
                        return -1;
                    }
                    break;
                }
                if (value.b == 0u || payload == NULL) {
                    HOPMirSetReason(run, ins, "unwrap of empty optional in evaluator backend");
                    return 0;
                }
                if (HOPCTFEPush(run, payload) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_ALLOC_NEW: {
                HOPCTFEValue out;
                int          allocRc;
                int          allocIsConst = 0;
                if (run->env.allocNew == NULL) {
                    HOPMirSetReason(
                        run, ins, "new expression is not supported during const evaluation");
                    return 0;
                }
                allocRc = run->env.allocNew(
                    run->env.allocNewCtx, ins->aux, &out, &allocIsConst, run->env.diag);
                if (allocRc < 0) {
                    return -1;
                }
                if (allocRc == 0 || !allocIsConst) {
                    HOPMirSetReason(
                        run, ins, "new expression is not supported during const evaluation");
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_CTX_GET: {
                HOPCTFEValue out;
                int          getRc;
                int          isConst = 0;
                if (run->env.contextGet == NULL) {
                    HOPMirSetReason(
                        run, ins, "context access is not supported during const evaluation");
                    return 0;
                }
                getRc = run->env.contextGet(
                    run->env.contextGetCtx, ins->aux, &out, &isConst, run->env.diag);
                if (getRc < 0) {
                    return -1;
                }
                if (getRc == 0 || !isConst) {
                    HOPMirSetReason(
                        run, ins, "context access is not supported during const evaluation");
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_CTX_ADDR: {
                HOPCTFEValue out;
                int          addrRc;
                int          isConst = 0;
                if (run->env.contextAddr == NULL) {
                    HOPMirSetReason(
                        run, ins, "context address is not supported during const evaluation");
                    return 0;
                }
                addrRc = run->env.contextAddr(
                    run->env.contextAddrCtx, ins->aux, &out, &isConst, run->env.diag);
                if (addrRc < 0) {
                    return -1;
                }
                if (addrRc == 0 || !isConst) {
                    HOPMirSetReason(
                        run, ins, "context address is not supported during const evaluation");
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_CTX_SET: {
                HOPCTFEValue out;
                int          evalRc;
                int          isConst = 0;
                if (run->env.evalWithContext == NULL) {
                    HOPMirSetReason(
                        run, ins, "context overlay is not supported during const evaluation");
                    return 0;
                }
                evalRc = run->env.evalWithContext(
                    run->env.evalWithContextCtx, ins->aux, &out, &isConst, run->env.diag);
                if (evalRc < 0) {
                    return -1;
                }
                if (evalRc == 0 || !isConst) {
                    HOPMirSetReason(
                        run, ins, "context overlay is not supported during const evaluation");
                    return 0;
                }
                if (HOPCTFEPush(run, &out) != 0) {
                    return -1;
                }
                break;
            }
            case HOPMirOp_RETURN:
                if (run->stackLen != 1) {
                    HOPMirSetReason(run, ins, "return stack is invalid during const evaluation");
                    return 0;
                }
                *outValue = run->stack[0];
                if (run->function != NULL && run->function->typeRef != UINT32_MAX) {
                    int coerceRc = HOPMirCoerceValueForType(run, run->function->typeRef, outValue);
                    if (coerceRc <= 0) {
                        return coerceRc < 0 ? -1 : 0;
                    }
                }
                *outIsConst = 1;
                return 0;
            case HOPMirOp_RETURN_VOID:
                HOPCTFEValueInvalid(outValue);
                *outIsConst = 1;
                return 0;
            default: return 0;
        }
    }
    return 0;
}

static void HOPCTFESetDiag(HOPDiag* diag, HOPDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

static void HOPCTFEValueInvalid(HOPCTFEValue* v) {
    v->kind = HOPCTFEValue_INVALID;
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

static int HOPCTFEPush(HOPMirExecRun* r, const HOPMirExecValue* v) {
    if (r->stackLen >= r->stackCap) {
        HOPCTFESetDiag(r->env.diag, HOPDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    r->stack[r->stackLen++] = *v;
    return 0;
}

static int HOPCTFEPop(HOPMirExecRun* r, HOPMirExecValue* out) {
    if (r->stackLen == 0) {
        return -1;
    }
    *out = r->stack[--r->stackLen];
    return 0;
}

static int HOPCTFEParseIntLiteral(HOPStrView src, uint32_t start, uint32_t end, int64_t* out) {
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

static int HOPCTFEParseFloatLiteral(
    HOPArena* arena, HOPStrView src, uint32_t start, uint32_t end, double* out) {
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

static int HOPCTFEParseBoolLiteral(HOPStrView src, uint32_t start, uint32_t end, uint8_t* out) {
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

static int HOPCTFEStringEq(const HOPCTFEString* a, const HOPCTFEString* b) {
    if (a->len != b->len) {
        return 0;
    }
    if (a->len == 0) {
        return 1;
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

static int HOPCTFEOptionalPayload(const HOPCTFEValue* opt, const HOPCTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (opt == NULL || opt->kind != HOPCTFEValue_OPTIONAL || outPayload == NULL) {
        return 0;
    }
    if (opt->b == 0u) {
        return 1;
    }
    if (opt->s.bytes == NULL) {
        return 0;
    }
    *outPayload = (const HOPCTFEValue*)opt->s.bytes;
    return 1;
}

static int HOPCTFEValueEqRec(const HOPCTFEValue* a, const HOPCTFEValue* b, uint32_t depth) {
    const HOPCTFEValue* aPayload = NULL;
    const HOPCTFEValue* bPayload = NULL;
    if (a == NULL || b == NULL || a->kind != b->kind || depth > 32u) {
        return 0;
    }
    switch (a->kind) {
        case HOPCTFEValue_INT:    return a->i64 == b->i64;
        case HOPCTFEValue_FLOAT:  return a->f64 == b->f64;
        case HOPCTFEValue_BOOL:   return a->b == b->b;
        case HOPCTFEValue_STRING: return HOPCTFEStringEq(&a->s, &b->s);
        case HOPCTFEValue_TYPE:   return a->typeTag == b->typeTag;
        case HOPCTFEValue_SPAN:
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
        case HOPCTFEValue_NULL: return 1;
        case HOPCTFEValue_OPTIONAL:
            if (!HOPCTFEOptionalPayload(a, &aPayload) || !HOPCTFEOptionalPayload(b, &bPayload)) {
                return 0;
            }
            if (a->b == 0u || b->b == 0u) {
                return a->b == b->b;
            }
            return HOPCTFEValueEqRec(aPayload, bPayload, depth + 1u);
        default: return 0;
    }
}

static int HOPCTFEStringConcat(
    HOPMirExecRun* r, const HOPCTFEString* a, const HOPCTFEString* b, HOPCTFEString* out) {
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
    dst = (uint8_t*)HOPArenaAlloc(r->arena, totalLen, (uint32_t)_Alignof(uint8_t));
    if (dst == NULL) {
        HOPCTFESetDiag(r->env.diag, HOPDiag_ARENA_OOM, 0, 0);
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

static int HOPCTFEEvalUnary(HOPTokenKind op, const HOPCTFEValue* in, HOPCTFEValue* out) {
    HOPCTFEValueInvalid(out);
    if (op == HOPTok_ADD && in->kind == HOPCTFEValue_INT) {
        *out = *in;
        return 1;
    }
    if (op == HOPTok_ADD && in->kind == HOPCTFEValue_FLOAT) {
        *out = *in;
        return 1;
    }
    if (op == HOPTok_SUB && in->kind == HOPCTFEValue_INT) {
        if (in->i64 == INT64_MIN) {
            return 0;
        }
        out->kind = HOPCTFEValue_INT;
        out->i64 = -in->i64;
        return 1;
    }
    if (op == HOPTok_SUB && in->kind == HOPCTFEValue_FLOAT) {
        out->kind = HOPCTFEValue_FLOAT;
        out->f64 = -in->f64;
        return 1;
    }
    if (op == HOPTok_NOT && in->kind == HOPCTFEValue_BOOL) {
        out->kind = HOPCTFEValue_BOOL;
        out->b = in->b ? 0u : 1u;
        return 1;
    }
    return 0;
}

static int HOPCTFEValueToF64(const HOPCTFEValue* value, double* out) {
    if (value == NULL || out == NULL) {
        return 0;
    }
    if (value->kind == HOPCTFEValue_INT) {
        *out = (double)value->i64;
        return 1;
    }
    if (value->kind == HOPCTFEValue_FLOAT) {
        *out = value->f64;
        return 1;
    }
    return 0;
}

static int HOPCTFEAddI64(int64_t a, int64_t b, int64_t* out) {
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

static int HOPCTFESubI64(int64_t a, int64_t b, int64_t* out) {
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

static int HOPCTFEMulI64(int64_t a, int64_t b, int64_t* out) {
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

static int HOPCTFEEvalBinary(
    HOPMirExecRun*      r,
    HOPTokenKind        op,
    const HOPCTFEValue* lhs,
    const HOPCTFEValue* rhs,
    HOPCTFEValue*       out) {
    int64_t             i = 0;
    double              lf = 0.0;
    double              rf = 0.0;
    const HOPCTFEValue* lhsPayload = NULL;
    const HOPCTFEValue* rhsPayload = NULL;
    HOPCTFEValueInvalid(out);

    if (lhs == NULL || rhs == NULL) {
        return 0;
    }
    if (lhs->kind == HOPCTFEValue_OPTIONAL && rhs->kind != HOPCTFEValue_OPTIONAL
        && rhs->kind != HOPCTFEValue_NULL)
    {
        if (!HOPCTFEOptionalPayload(lhs, &lhsPayload) || lhs->b == 0u || lhsPayload == NULL) {
            return 0;
        }
        return HOPCTFEEvalBinary(r, op, lhsPayload, rhs, out);
    }
    if (rhs->kind == HOPCTFEValue_OPTIONAL && lhs->kind != HOPCTFEValue_OPTIONAL
        && lhs->kind != HOPCTFEValue_NULL)
    {
        if (!HOPCTFEOptionalPayload(rhs, &rhsPayload) || rhs->b == 0u || rhsPayload == NULL) {
            return 0;
        }
        return HOPCTFEEvalBinary(r, op, lhs, rhsPayload, out);
    }

    if (lhs->kind == HOPCTFEValue_INT && rhs->kind == HOPCTFEValue_INT) {
        switch (op) {
            case HOPTok_ADD:
                if (HOPCTFEAddI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = HOPCTFEValue_INT;
                out->i64 = i;
                return 1;
            case HOPTok_SUB:
                if (HOPCTFESubI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = HOPCTFEValue_INT;
                out->i64 = i;
                return 1;
            case HOPTok_MUL:
                if (HOPCTFEMulI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                out->kind = HOPCTFEValue_INT;
                out->i64 = i;
                return 1;
            case HOPTok_DIV:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                out->kind = HOPCTFEValue_INT;
                out->i64 = lhs->i64 / rhs->i64;
                return 1;
            case HOPTok_MOD:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                out->kind = HOPCTFEValue_INT;
                out->i64 = lhs->i64 % rhs->i64;
                return 1;
            case HOPTok_AND:
                out->kind = HOPCTFEValue_INT;
                out->i64 = lhs->i64 & rhs->i64;
                return 1;
            case HOPTok_OR:
                out->kind = HOPCTFEValue_INT;
                out->i64 = lhs->i64 | rhs->i64;
                return 1;
            case HOPTok_XOR:
                out->kind = HOPCTFEValue_INT;
                out->i64 = lhs->i64 ^ rhs->i64;
                return 1;
            case HOPTok_LSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                out->kind = HOPCTFEValue_INT;
                out->i64 = (int64_t)((uint64_t)lhs->i64 << (uint32_t)rhs->i64);
                return 1;
            case HOPTok_RSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                out->kind = HOPCTFEValue_INT;
                out->i64 = (int64_t)((uint64_t)lhs->i64 >> (uint32_t)rhs->i64);
                return 1;
            case HOPTok_EQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->i64 == rhs->i64;
                return 1;
            case HOPTok_NEQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->i64 != rhs->i64;
                return 1;
            case HOPTok_LT:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->i64 < rhs->i64;
                return 1;
            case HOPTok_GT:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->i64 > rhs->i64;
                return 1;
            case HOPTok_LTE:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->i64 <= rhs->i64;
                return 1;
            case HOPTok_GTE:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->i64 >= rhs->i64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == HOPCTFEValue_BOOL && rhs->kind == HOPCTFEValue_BOOL) {
        switch (op) {
            case HOPTok_LOGICAL_AND:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->b && rhs->b;
                return 1;
            case HOPTok_LOGICAL_OR:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->b || rhs->b;
                return 1;
            case HOPTok_EQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->b == rhs->b;
                return 1;
            case HOPTok_NEQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->b != rhs->b;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == HOPCTFEValue_STRING && rhs->kind == HOPCTFEValue_STRING) {
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
            case HOPTok_ADD:
                out->kind = HOPCTFEValue_STRING;
                return HOPCTFEStringConcat(r, &lhs->s, &rhs->s, &out->s) == 0;
            case HOPTok_EQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = HOPCTFEStringEq(&lhs->s, &rhs->s);
                return 1;
            case HOPTok_NEQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = !HOPCTFEStringEq(&lhs->s, &rhs->s);
                return 1;
            case HOPTok_LT:
                out->kind = HOPCTFEValue_BOOL;
                out->b = cmp < 0;
                return 1;
            case HOPTok_GT:
                out->kind = HOPCTFEValue_BOOL;
                out->b = cmp > 0;
                return 1;
            case HOPTok_LTE:
                out->kind = HOPCTFEValue_BOOL;
                out->b = cmp <= 0;
                return 1;
            case HOPTok_GTE:
                out->kind = HOPCTFEValue_BOOL;
                out->b = cmp >= 0;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == HOPCTFEValue_TYPE && rhs->kind == HOPCTFEValue_TYPE && r != NULL
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

    if (lhs->kind == HOPCTFEValue_TYPE && rhs->kind == HOPCTFEValue_TYPE) {
        switch (op) {
            case HOPTok_EQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->typeTag == rhs->typeTag;
                return 1;
            case HOPTok_NEQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lhs->typeTag != rhs->typeTag;
                return 1;
            default: break;
        }
    }

    if (HOPCTFEValueToF64(lhs, &lf) && HOPCTFEValueToF64(rhs, &rf)) {
        switch (op) {
            case HOPTok_ADD:
                out->kind = HOPCTFEValue_FLOAT;
                out->f64 = lf + rf;
                return 1;
            case HOPTok_SUB:
                out->kind = HOPCTFEValue_FLOAT;
                out->f64 = lf - rf;
                return 1;
            case HOPTok_MUL:
                out->kind = HOPCTFEValue_FLOAT;
                out->f64 = lf * rf;
                return 1;
            case HOPTok_DIV:
                out->kind = HOPCTFEValue_FLOAT;
                out->f64 = lf / rf;
                return 1;
            case HOPTok_EQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lf == rf;
                return 1;
            case HOPTok_NEQ:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lf != rf;
                return 1;
            case HOPTok_LT:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lf < rf;
                return 1;
            case HOPTok_GT:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lf > rf;
                return 1;
            case HOPTok_LTE:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lf <= rf;
                return 1;
            case HOPTok_GTE:
                out->kind = HOPCTFEValue_BOOL;
                out->b = lf >= rf;
                return 1;
            default: return 0;
        }
    }

    if (op == HOPTok_EQ || op == HOPTok_NEQ) {
        int eq = 0;
        if (lhs->kind == HOPCTFEValue_NULL && rhs->kind == HOPCTFEValue_NULL) {
            eq = 1;
        } else if (lhs->kind == HOPCTFEValue_REFERENCE && rhs->kind == HOPCTFEValue_NULL) {
            eq = lhs->s.bytes == NULL;
        } else if (lhs->kind == HOPCTFEValue_NULL && rhs->kind == HOPCTFEValue_REFERENCE) {
            eq = rhs->s.bytes == NULL;
        } else if (lhs->kind == HOPCTFEValue_STRING && rhs->kind == HOPCTFEValue_NULL) {
            eq = lhs->s.bytes == NULL;
        } else if (lhs->kind == HOPCTFEValue_NULL && rhs->kind == HOPCTFEValue_STRING) {
            eq = rhs->s.bytes == NULL;
        } else if (lhs->kind == HOPCTFEValue_OPTIONAL && rhs->kind == HOPCTFEValue_NULL) {
            eq = lhs->b == 0u;
        } else if (lhs->kind == HOPCTFEValue_NULL && rhs->kind == HOPCTFEValue_OPTIONAL) {
            eq = rhs->b == 0u;
        } else if (lhs->kind == HOPCTFEValue_OPTIONAL && rhs->kind == HOPCTFEValue_OPTIONAL) {
            eq = HOPCTFEValueEqRec(lhs, rhs, 0);
        }
        if (lhs->kind == HOPCTFEValue_NULL || rhs->kind == HOPCTFEValue_NULL
            || (lhs->kind == HOPCTFEValue_OPTIONAL && rhs->kind == HOPCTFEValue_OPTIONAL))
        {
            out->kind = HOPCTFEValue_BOOL;
            out->b = (op == HOPTok_EQ) ? (eq ? 1u : 0u) : (eq ? 0u : 1u);
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

static int HOPCTFEEvalCast(HOPMirCastTarget target, const HOPCTFEValue* in, HOPCTFEValue* out) {
    HOPCTFEValueInvalid(out);
    switch (target) {
        case HOPMirCastTarget_INT: {
            int64_t asInt = 0;
            if (in->kind == HOPCTFEValue_INT) {
                asInt = in->i64;
            } else if (in->kind == HOPCTFEValue_BOOL) {
                asInt = in->b ? 1 : 0;
            } else if (in->kind == HOPCTFEValue_FLOAT) {
                if (in->f64 != in->f64 || in->f64 > (double)INT64_MAX
                    || in->f64 < (double)INT64_MIN)
                {
                    return 0;
                }
                asInt = (int64_t)in->f64;
            } else if (in->kind == HOPCTFEValue_NULL) {
                asInt = 0;
            } else {
                return 0;
            }
            out->kind = HOPCTFEValue_INT;
            out->i64 = asInt;
            return 1;
        }
        case HOPMirCastTarget_FLOAT: {
            double asFloat = 0.0;
            if (in->kind == HOPCTFEValue_FLOAT) {
                asFloat = in->f64;
            } else if (in->kind == HOPCTFEValue_INT) {
                asFloat = (double)in->i64;
            } else if (in->kind == HOPCTFEValue_BOOL) {
                asFloat = in->b ? 1.0 : 0.0;
            } else if (in->kind == HOPCTFEValue_NULL) {
                asFloat = 0.0;
            } else {
                return 0;
            }
            out->kind = HOPCTFEValue_FLOAT;
            out->f64 = asFloat;
            return 1;
        }
        case HOPMirCastTarget_BOOL: {
            uint8_t asBool = 0;
            if (in->kind == HOPCTFEValue_BOOL) {
                asBool = in->b ? 1u : 0u;
            } else if (in->kind == HOPCTFEValue_INT) {
                asBool = in->i64 != 0 ? 1u : 0u;
            } else if (in->kind == HOPCTFEValue_FLOAT) {
                asBool = in->f64 != 0.0 ? 1u : 0u;
            } else if (in->kind == HOPCTFEValue_OPTIONAL) {
                asBool = in->b != 0u ? 1u : 0u;
            } else if (in->kind == HOPCTFEValue_STRING) {
                asBool = 1u;
            } else if (in->kind == HOPCTFEValue_NULL) {
                asBool = 0u;
            } else {
                return 0;
            }
            out->kind = HOPCTFEValue_BOOL;
            out->b = asBool;
            return 1;
        }
        case HOPMirCastTarget_PTR_LIKE:
            if (in->kind == HOPCTFEValue_REFERENCE || in->kind == HOPCTFEValue_NULL
                || in->kind == HOPCTFEValue_STRING)
            {
                *out = *in;
                return 1;
            }
            return 0;
        case HOPMirCastTarget_STR_VIEW: *out = *in; return 1;
        default:                        return 0;
    }
}

int HOPMirEvalChunk(
    HOPArena* _Nonnull arena,
    HOPMirChunk chunk,
    const HOPMirExecEnv* _Nullable env,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    HOPMirExecRun run;
    if (HOPMirInitRun(&run, arena, chunk, NULL, NULL, 0u, NULL, 0u, env, 1, outValue, outIsConst)
        != 0)
    {
        return -1;
    }
    return HOPMirRunLoop(&run, outValue, outIsConst);
}

static int HOPMirEvalFunctionInternal(
    HOPArena* _Nonnull arena,
    const HOPMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const HOPMirExecValue* _Nullable args,
    uint32_t argCount,
    uint16_t callFlags,
    const HOPMirExecEnv* _Nullable env,
    int validateProgram,
    int clearDiag,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    const HOPMirFunction* fn;
    HOPMirChunk           chunk;
    HOPMirExecEnv         frameEnv;
    HOPMirExecRun         run;
    HOPMirExecValue       variadicPackValue;
    int                   enteredFunction = 0;
    int                   boundFrame = 0;
    int                   rc = 0;
    if (program == NULL || functionIndex >= program->funcLen) {
        if (env != NULL) {
            HOPCTFESetDiag(env->diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }
    if (validateProgram && HOPMirValidateProgram(program, env != NULL ? env->diag : NULL) != 0) {
        return -1;
    }
    fn = &program->funcs[functionIndex];
    if (fn->instStart > program->instLen || fn->instLen > program->instLen - fn->instStart) {
        if (env != NULL) {
            HOPCTFESetDiag(env->diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        }
        return -1;
    }
    if (fn->localCount < fn->paramCount) {
        return 0;
    }
    if ((fn->flags & HOPMirFunctionFlag_VARIADIC) != 0u) {
        uint32_t           fixedCount = fn->paramCount > 0u ? fn->paramCount - 1u : 0u;
        const HOPMirLocal* variadicLocal = NULL;
        int                packIsConst = 0;
        if (argCount < fixedCount || fn->paramCount == 0u || env == NULL
            || env->makeVariadicPack == NULL)
        {
            return 0;
        }
        variadicLocal = &program->locals[fn->localStart + fn->paramCount - 1u];
        HOPCTFEValueInvalid(&variadicPackValue);
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
    if (HOPMirInitRun(
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
    if ((fn->flags & HOPMirFunctionFlag_VARIADIC) != 0u) {
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
    rc = HOPMirRunLoop(&run, outValue, outIsConst);
end:
    if (boundFrame && frameEnv.unbindFrame != NULL) {
        frameEnv.unbindFrame(frameEnv.frameCtx);
    }
    if (enteredFunction && frameEnv.leaveFunction != NULL) {
        frameEnv.leaveFunction(frameEnv.functionCtx);
    }
    return rc;
}

int HOPMirEvalFunction(
    HOPArena* _Nonnull arena,
    const HOPMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const HOPMirExecValue* _Nullable args,
    uint32_t argCount,
    const HOPMirExecEnv* _Nullable env,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst) {
    return HOPMirEvalFunctionInternal(
        arena, program, functionIndex, args, argCount, 0u, env, 1, 1, outValue, outIsConst);
}
HOP_API_END
