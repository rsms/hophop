#include "internal.h"
#include "../mir_exec.h"
#include "../mir_lower_pkg.h"
#include "../mir_lower_stmt.h"

HOP_API_BEGIN

static const uint64_t HOP_TC_MIR_TUPLE_TAG = 0x54434d4952545550ULL;
static const uint64_t HOP_TC_MIR_ITER_TAG = 0x54434d4952495445ULL;
static const uint64_t HOP_TC_MIR_IMPORT_ALIAS_TAG = 0x54434d49524d504bULL;

static int HOPTCConstEvalResolveTrackedAnyPackArgIndex(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outCallArgIndex);
static int HOPTCConstEvalGetConcreteCallArgType(
    HOPTCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outType);
static int HOPTCConstEvalGetConcreteCallArgPackType(
    HOPTCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outPackType);
static int HOPTCConstEvalDirectCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
int HOPTCResolveConstCallMir(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nonnull args,
    uint32_t argCount,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
int HOPTCResolveConstCallMirPre(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPTCConstEvalSetOptionalNoneValue(
    HOPTypeCheckCtx* c, int32_t optionalTypeId, HOPCTFEValue* outValue);
static int HOPTCConstEvalSetOptionalSomeValue(
    HOPTypeCheckCtx*    c,
    int32_t             optionalTypeId,
    const HOPCTFEValue* payload,
    HOPCTFEValue*       outValue);
static int HOPTCInvokeConstFunctionByIndex(
    HOPTCConstEvalCtx* evalCtx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    int32_t            fnIndex,
    const HOPCTFEValue* _Nullable args,
    uint32_t argCount,
    const HOPTCCallArgInfo* _Nullable callArgs,
    uint32_t callArgCount,
    const HOPTCCallBinding* _Nullable callBinding,
    uint32_t      callPackParamNameStart,
    uint32_t      callPackParamNameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst);
void HOPTCConstSetReason(
    HOPTCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason);
static int HOPTCConstLookupMirLocalValue(
    HOPTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, HOPCTFEValue* outValue);
int HOPTCMirConstBindFrame(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPCTFEValue* _Nullable locals,
    uint32_t localCount,
    HOPDiag* _Nullable diag);
void HOPTCMirConstUnbindFrame(void* _Nullable ctx);

static int HOPTCConstEvalIsTrackedAnyPackName(
    const HOPTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd) {
    HOPTypeCheckCtx* c;
    int32_t          localIdx;
    if (evalCtx == NULL) {
        return 0;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return 0;
    }
    if (evalCtx->callPackParamNameStart < evalCtx->callPackParamNameEnd
        && HOPNameEqSlice(
            c->src,
            nameStart,
            nameEnd,
            evalCtx->callPackParamNameStart,
            evalCtx->callPackParamNameEnd))
    {
        return 1;
    }
    localIdx = HOPTCLocalFind(c, nameStart, nameEnd);
    return localIdx >= 0 && (c->locals[localIdx].flags & HOPTCLocalFlag_ANYPACK) != 0;
}

static void HOPTCMirConstSetReasonCb(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* reason) {
    HOPTCConstSetReason((HOPTCConstEvalCtx*)ctx, start, end, reason);
}

void HOPTCConstSetReason(
    HOPTCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason) {
    if (evalCtx == NULL || reason == NULL || reason[0] == '\0' || evalCtx->nonConstReason != NULL) {
        return;
    }
    evalCtx->nonConstReason = reason;
    evalCtx->nonConstStart = start;
    evalCtx->nonConstEnd = end;
    if (evalCtx->execCtx != NULL) {
        HOPCTFEExecSetReason(evalCtx->execCtx, start, end, reason);
    }
}

static int HOPTCConstLookupMirLocalValue(
    HOPTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, HOPCTFEValue* outValue) {
    HOPTypeCheckCtx* c;
    uint32_t         i;
    if (evalCtx == NULL || outValue == NULL || evalCtx->mirProgram == NULL
        || evalCtx->mirFunction == NULL || evalCtx->mirLocals == NULL)
    {
        return 0;
    }
    c = evalCtx->tc;
    if (c == NULL || evalCtx->mirFunction->localStart > evalCtx->mirProgram->localLen
        || evalCtx->mirFunction->localCount
               > evalCtx->mirProgram->localLen - evalCtx->mirFunction->localStart
        || evalCtx->mirLocalCount < evalCtx->mirFunction->localCount)
    {
        return 0;
    }
    for (i = evalCtx->mirFunction->localCount; i > 0; i--) {
        const HOPMirLocal* local =
            &evalCtx->mirProgram->locals[evalCtx->mirFunction->localStart + i - 1u];
        const HOPCTFEValue* value = &evalCtx->mirLocals[i - 1u];
        if (local->nameEnd <= local->nameStart
            || !HOPNameEqSlice(c->src, local->nameStart, local->nameEnd, nameStart, nameEnd))
        {
            continue;
        }
        if (value->kind == HOPCTFEValue_INVALID) {
            continue;
        }
        *outValue = *value;
        return 1;
    }
    return 0;
}

int HOPTCMirConstBindFrame(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPCTFEValue* _Nullable locals,
    uint32_t localCount,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    if (evalCtx == NULL || evalCtx->mirFrameDepth >= HOPTC_CONST_CALL_MAX_DEPTH) {
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
    evalCtx->mirSavedPrograms[evalCtx->mirFrameDepth] = evalCtx->mirProgram;
    evalCtx->mirSavedFunctions[evalCtx->mirFrameDepth] = evalCtx->mirFunction;
    evalCtx->mirSavedLocals[evalCtx->mirFrameDepth] = evalCtx->mirLocals;
    evalCtx->mirSavedLocalCounts[evalCtx->mirFrameDepth] = evalCtx->mirLocalCount;
    evalCtx->mirFrameDepth++;
    evalCtx->mirProgram = program;
    evalCtx->mirFunction = function;
    evalCtx->mirLocals = locals;
    evalCtx->mirLocalCount = localCount;
    return 0;
}

void HOPTCMirConstUnbindFrame(void* _Nullable ctx) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    if (evalCtx == NULL || evalCtx->mirFrameDepth == 0) {
        return;
    }
    evalCtx->mirFrameDepth--;
    evalCtx->mirProgram = evalCtx->mirSavedPrograms[evalCtx->mirFrameDepth];
    evalCtx->mirFunction = evalCtx->mirSavedFunctions[evalCtx->mirFrameDepth];
    evalCtx->mirLocals = evalCtx->mirSavedLocals[evalCtx->mirFrameDepth];
    evalCtx->mirLocalCount = evalCtx->mirSavedLocalCounts[evalCtx->mirFrameDepth];
}

void HOPTCMirConstAdoptLowerDiagReason(HOPTCConstEvalCtx* evalCtx, const HOPDiag* _Nullable diag) {
    if (evalCtx == NULL || diag == NULL || diag->detail == NULL || diag->detail[0] == '\0') {
        return;
    }
    HOPTCConstSetReason(evalCtx, diag->start, diag->end, diag->detail);
}

int HOPTCMirConstLowerConstExpr(
    void* _Nullable ctx,
    int32_t exprNode,
    HOPMirConst* _Nonnull outValue,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTCConstEvalCtx  localEvalCtx;
    HOPTypeCheckCtx*   c;
    HOPCTFEValue       value;
    int32_t            reflectedTypeId = -1;
    int                isConst = 0;
    if (outValue == NULL || evalCtx == NULL || evalCtx->tc == NULL || exprNode < 0) {
        return 0;
    }
    c = evalCtx->tc;
    if ((uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    localEvalCtx = *evalCtx;
    localEvalCtx.nonConstReason = NULL;
    localEvalCtx.nonConstStart = 0;
    localEvalCtx.nonConstEnd = 0;
    if (c->ast->nodes[exprNode].kind == HOPAst_SIZEOF) {
        if (HOPTCConstEvalSizeOf(&localEvalCtx, exprNode, &value, &isConst) != 0) {
            return -1;
        }
        if (!isConst || value.kind != HOPCTFEValue_INT) {
            if (diag != NULL && diag->detail == NULL && localEvalCtx.nonConstReason != NULL) {
                diag->start = localEvalCtx.nonConstStart;
                diag->end = localEvalCtx.nonConstEnd;
                diag->detail = localEvalCtx.nonConstReason;
            }
            return 0;
        }
        outValue->kind = HOPMirConst_INT;
        outValue->bits = (uint64_t)value.i64;
        return 1;
    }
    if (HOPTCResolveReflectedTypeValueExpr(c, exprNode, &reflectedTypeId) < 0) {
        return -1;
    }
    if (reflectedTypeId >= 0) {
        outValue->kind = HOPMirConst_TYPE;
        outValue->bits = HOPTCEncodeTypeTag(c, reflectedTypeId);
        return 1;
    }
    if (c->ast->nodes[exprNode].kind == HOPAst_CALL) {
        int32_t calleeNode = HOPAstFirstChild(c->ast, exprNode);
        if (calleeNode < 0 || (uint32_t)calleeNode >= c->ast->len
            || c->ast->nodes[calleeNode].kind != HOPAst_IDENT
            || !HOPNameEqLiteral(
                c->src,
                c->ast->nodes[calleeNode].dataStart,
                c->ast->nodes[calleeNode].dataEnd,
                "typeof"))
        {
            return 0;
        }
        if (HOPTCConstEvalTypeOf(&localEvalCtx, exprNode, &value, &isConst) != 0) {
            return -1;
        }
        if (!isConst || value.kind != HOPCTFEValue_TYPE) {
            if (diag != NULL && diag->detail == NULL && localEvalCtx.nonConstReason != NULL) {
                diag->start = localEvalCtx.nonConstStart;
                diag->end = localEvalCtx.nonConstEnd;
                diag->detail = localEvalCtx.nonConstReason;
            }
            return 0;
        }
        outValue->kind = HOPMirConst_TYPE;
        outValue->bits = value.typeTag;
        return 1;
    }
    return 0;
}

void HOPTCConstSetReasonNode(HOPTCConstEvalCtx* evalCtx, int32_t nodeId, const char* reason) {
    HOPTypeCheckCtx* c;
    if (evalCtx == NULL) {
        return;
    }
    c = evalCtx->tc;
    if (c != NULL && nodeId >= 0 && (uint32_t)nodeId < c->ast->len) {
        HOPTCConstSetReason(
            evalCtx, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, reason);
        return;
    }
    HOPTCConstSetReason(evalCtx, 0, 0, reason);
}

void HOPTCAttachConstEvalReason(HOPTypeCheckCtx* c) {
    if (c == NULL || c->diag == NULL || c->lastConstEvalReason == NULL
        || c->lastConstEvalReason[0] == '\0')
    {
        return;
    }
    c->diag->detail = HOPTCAllocDiagText(c, c->lastConstEvalReason);
}

static int32_t HOPTCFindPkgQualifiedFunctionValueIndexBySlice(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    uint32_t pkgStart = 0;
    uint32_t pkgEnd = 0;
    if (c == NULL || nameEnd <= nameStart + 2u || nameEnd > c->src.len) {
        return -1;
    }
    for (i = nameStart + 1u; i + 1u < nameEnd; i++) {
        if (c->src.ptr[i] == '.') {
            return HOPTCFindPkgQualifiedFunctionValueIndex(c, nameStart, i, i + 1u, nameEnd);
        }
    }
    if (HOPTCExtractPkgPrefixFromTypeName(c, nameStart, nameEnd, &pkgStart, &pkgEnd)) {
        uint32_t methodStart = pkgEnd + 2u;
        if (methodStart < nameEnd) {
            return HOPTCFindPkgQualifiedFunctionValueIndex(
                c, pkgStart, pkgEnd, methodStart, nameEnd);
        }
    }
    return -1;
}

static void HOPTCConstEvalValueInvalid(HOPCTFEValue* v) {
    if (v == NULL) {
        return;
    }
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

static int HOPTCConstEvalValueToF64(const HOPCTFEValue* value, double* out) {
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

static int HOPTCConstEvalStringEq(const HOPCTFEString* a, const HOPCTFEString* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->len != b->len) {
        return 0;
    }
    if (a->len == 0) {
        return 1;
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

static int HOPTCConstEvalStringConcat(
    HOPTypeCheckCtx* c, const HOPCTFEString* a, const HOPCTFEString* b, HOPCTFEString* out) {
    uint64_t totalLen64;
    uint32_t totalLen;
    uint8_t* bytes;
    if (c == NULL || a == NULL || b == NULL || out == NULL) {
        return -1;
    }
    totalLen64 = (uint64_t)a->len + (uint64_t)b->len;
    if (totalLen64 > UINT32_MAX) {
        return 0;
    }
    totalLen = (uint32_t)totalLen64;
    if (totalLen == 0) {
        out->bytes = NULL;
        out->len = 0;
        return 1;
    }
    bytes = (uint8_t*)HOPArenaAlloc(c->arena, totalLen, (uint32_t)_Alignof(uint8_t));
    if (bytes == NULL) {
        return -1;
    }
    if (a->len > 0) {
        memcpy(bytes, a->bytes, a->len);
    }
    if (b->len > 0) {
        memcpy(bytes + a->len, b->bytes, b->len);
    }
    out->bytes = bytes;
    out->len = totalLen;
    return 1;
}

static int HOPTCConstEvalAddI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_add_overflow)
    return __builtin_add_overflow(a, b, out) ? -1 : 0;
#else
    if ((b > 0 && a > INT64_MAX - b) || (b < 0 && a < INT64_MIN - b)) {
        return -1;
    }
    *out = a + b;
    return 0;
#endif
}

static int HOPTCConstEvalSubI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_sub_overflow)
    return __builtin_sub_overflow(a, b, out) ? -1 : 0;
#else
    if ((b > 0 && a < INT64_MIN + b) || (b < 0 && a > INT64_MAX + b)) {
        return -1;
    }
    *out = a - b;
    return 0;
#endif
}

static int HOPTCConstEvalMulI64(int64_t a, int64_t b, int64_t* out) {
#if __has_builtin(__builtin_mul_overflow)
    return __builtin_mul_overflow(a, b, out) ? -1 : 0;
#else
    if (a == 0 || b == 0) {
        *out = 0;
        return 0;
    }
    if ((a == -1 && b == INT64_MIN) || (b == -1 && a == INT64_MIN)) {
        return -1;
    }
    if (a > 0) {
        if (b > 0) {
            if (a > INT64_MAX / b) {
                return -1;
            }
        } else if (b < INT64_MIN / a) {
            return -1;
        }
    } else if (b > 0) {
        if (a < INT64_MIN / b) {
            return -1;
        }
    } else if (a != 0 && b < INT64_MAX / a) {
        return -1;
    }
    *out = a * b;
    return 0;
#endif
}

static int HOPTCConstEvalApplyUnary(
    HOPTokenKind op, const HOPCTFEValue* inValue, HOPCTFEValue* outValue) {
    HOPTCConstEvalValueInvalid(outValue);
    if (inValue == NULL) {
        return 0;
    }
    if (op == HOPTok_ADD && inValue->kind == HOPCTFEValue_INT) {
        *outValue = *inValue;
        return 1;
    }
    if (op == HOPTok_ADD && inValue->kind == HOPCTFEValue_FLOAT) {
        *outValue = *inValue;
        return 1;
    }
    if (op == HOPTok_SUB && inValue->kind == HOPCTFEValue_INT) {
        if (inValue->i64 == INT64_MIN) {
            return 0;
        }
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = -inValue->i64;
        return 1;
    }
    if (op == HOPTok_SUB && inValue->kind == HOPCTFEValue_FLOAT) {
        outValue->kind = HOPCTFEValue_FLOAT;
        outValue->f64 = -inValue->f64;
        return 1;
    }
    if (op == HOPTok_NOT && inValue->kind == HOPCTFEValue_BOOL) {
        outValue->kind = HOPCTFEValue_BOOL;
        outValue->b = inValue->b ? 0u : 1u;
        return 1;
    }
    return 0;
}

static int HOPTCConstEvalApplyBinary(
    HOPTypeCheckCtx*    c,
    HOPTokenKind        op,
    const HOPCTFEValue* lhs,
    const HOPCTFEValue* rhs,
    HOPCTFEValue*       outValue) {
    int64_t i;
    double  lhsF64;
    double  rhsF64;
    HOPTCConstEvalValueInvalid(outValue);
    if (c == NULL || lhs == NULL || rhs == NULL) {
        return 0;
    }

    if (lhs->kind == HOPCTFEValue_INT && rhs->kind == HOPCTFEValue_INT) {
        switch (op) {
            case HOPTok_ADD:
                if (HOPTCConstEvalAddI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case HOPTok_SUB:
                if (HOPTCConstEvalSubI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case HOPTok_MUL:
                if (HOPTCConstEvalMulI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case HOPTok_DIV:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = lhs->i64 / rhs->i64;
                return 1;
            case HOPTok_MOD:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = lhs->i64 % rhs->i64;
                return 1;
            case HOPTok_AND:
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = lhs->i64 & rhs->i64;
                return 1;
            case HOPTok_OR:
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = lhs->i64 | rhs->i64;
                return 1;
            case HOPTok_XOR:
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = lhs->i64 ^ rhs->i64;
                return 1;
            case HOPTok_LSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = (int64_t)((uint64_t)lhs->i64 << (uint32_t)rhs->i64);
                return 1;
            case HOPTok_RSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = (int64_t)((uint64_t)lhs->i64 >> (uint32_t)rhs->i64);
                return 1;
            case HOPTok_EQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->i64 == rhs->i64;
                return 1;
            case HOPTok_NEQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->i64 != rhs->i64;
                return 1;
            case HOPTok_LT:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->i64 < rhs->i64;
                return 1;
            case HOPTok_GT:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->i64 > rhs->i64;
                return 1;
            case HOPTok_LTE:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->i64 <= rhs->i64;
                return 1;
            case HOPTok_GTE:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->i64 >= rhs->i64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == HOPCTFEValue_BOOL && rhs->kind == HOPCTFEValue_BOOL) {
        switch (op) {
            case HOPTok_LOGICAL_AND:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->b && rhs->b;
                return 1;
            case HOPTok_LOGICAL_OR:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->b || rhs->b;
                return 1;
            case HOPTok_EQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->b == rhs->b;
                return 1;
            case HOPTok_NEQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->b != rhs->b;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == HOPCTFEValue_STRING && rhs->kind == HOPCTFEValue_STRING) {
        switch (op) {
            case HOPTok_ADD:
                outValue->kind = HOPCTFEValue_STRING;
                return HOPTCConstEvalStringConcat(c, &lhs->s, &rhs->s, &outValue->s);
            case HOPTok_EQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = HOPTCConstEvalStringEq(&lhs->s, &rhs->s);
                return 1;
            case HOPTok_NEQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = !HOPTCConstEvalStringEq(&lhs->s, &rhs->s);
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == HOPCTFEValue_TYPE && rhs->kind == HOPCTFEValue_TYPE) {
        switch (op) {
            case HOPTok_EQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->typeTag == rhs->typeTag;
                return 1;
            case HOPTok_NEQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhs->typeTag != rhs->typeTag;
                return 1;
            default: return 0;
        }
    }

    if (HOPTCConstEvalValueToF64(lhs, &lhsF64) && HOPTCConstEvalValueToF64(rhs, &rhsF64)) {
        switch (op) {
            case HOPTok_ADD:
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->f64 = lhsF64 + rhsF64;
                return 1;
            case HOPTok_SUB:
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->f64 = lhsF64 - rhsF64;
                return 1;
            case HOPTok_MUL:
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->f64 = lhsF64 * rhsF64;
                return 1;
            case HOPTok_DIV:
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->f64 = lhsF64 / rhsF64;
                return 1;
            case HOPTok_EQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhsF64 == rhsF64;
                return 1;
            case HOPTok_NEQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhsF64 != rhsF64;
                return 1;
            case HOPTok_LT:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhsF64 < rhsF64;
                return 1;
            case HOPTok_GT:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhsF64 > rhsF64;
                return 1;
            case HOPTok_LTE:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhsF64 <= rhsF64;
                return 1;
            case HOPTok_GTE:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = lhsF64 >= rhsF64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == HOPCTFEValue_NULL && rhs->kind == HOPCTFEValue_NULL) {
        switch (op) {
            case HOPTok_EQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = 1;
                return 1;
            case HOPTok_NEQ:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = 0;
                return 1;
            default: return 0;
        }
    }

    return 0;
}

int HOPTCResolveConstIdent(
    void*         ctx,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*   c;
    int32_t            nodeId;
    int32_t            nameIndex = -1;
    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (evalCtx->execCtx != NULL
        && HOPCTFEExecEnvLookup(evalCtx->execCtx, nameStart, nameEnd, outValue))
    {
        *outIsConst = 1;
        return 0;
    }
    if (HOPTCConstLookupMirLocalValue(evalCtx, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t localIdx = HOPTCLocalFind(c, nameStart, nameEnd);
        if (localIdx >= 0) {
            HOPTCLocal* local = &c->locals[localIdx];
            int32_t     localType = local->typeId;
            int32_t     resolvedLocalType = HOPTCResolveAliasBaseType(c, localType);
            if ((local->flags & HOPTCLocalFlag_ANYPACK) != 0 && evalCtx->callBinding != NULL
                && HOPTCConstEvalIsTrackedAnyPackName(evalCtx, nameStart, nameEnd))
            {
                const HOPTCCallBinding* binding = (const HOPTCCallBinding*)evalCtx->callBinding;
                int32_t                 elemTypes[HOPTC_MAX_CALL_ARGS];
                uint32_t                elemCount = 0;
                uint32_t                i;
                int                     haveAllTypes = 1;
                if (binding->isVariadic && binding->spreadArgIndex != UINT32_MAX) {
                    uint32_t spreadArgIndex = binding->spreadArgIndex;
                    int32_t  spreadArgType = -1;
                    if (spreadArgIndex < evalCtx->callArgCount
                        && HOPTCConstEvalGetConcreteCallArgType(
                               evalCtx, spreadArgIndex, &spreadArgType)
                               == 0)
                    {
                        int32_t spreadType = HOPTCResolveAliasBaseType(c, spreadArgType);
                        if (spreadType >= 0 && (uint32_t)spreadType < c->typeLen
                            && c->types[spreadType].kind == HOPTCType_PACK)
                        {
                            localType = spreadArgType;
                            resolvedLocalType = spreadType;
                        }
                    }
                } else if (binding->isVariadic) {
                    for (i = 0; i < evalCtx->callArgCount; i++) {
                        int32_t elemType = -1;
                        if (binding->argParamIndices[i] != (int32_t)binding->fixedCount) {
                            continue;
                        }
                        if (elemCount >= HOPTC_MAX_CALL_ARGS
                            || HOPTCConstEvalGetConcreteCallArgType(evalCtx, i, &elemType) != 0)
                        {
                            haveAllTypes = 0;
                            break;
                        }
                        elemTypes[elemCount++] = elemType;
                    }
                    if (haveAllTypes) {
                        int32_t packTypeId = HOPTCInternPackType(
                            c, elemTypes, elemCount, nameStart, nameEnd);
                        if (packTypeId < 0) {
                            return -1;
                        }
                        localType = packTypeId;
                        resolvedLocalType = HOPTCResolveAliasBaseType(c, localType);
                    }
                }
            }
            if (resolvedLocalType >= 0 && (uint32_t)resolvedLocalType < c->typeLen
                && c->types[resolvedLocalType].kind == HOPTCType_PACK)
            {
                outValue->kind = HOPCTFEValue_TYPE;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = HOPTCEncodeTypeTag(c, localType);
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->span.fileBytes = NULL;
                outValue->span.fileLen = 0;
                outValue->span.startLine = 0;
                outValue->span.startColumn = 0;
                outValue->span.endLine = 0;
                outValue->span.endColumn = 0;
                *outIsConst = 1;
                return 0;
            }
            if ((local->flags & HOPTCLocalFlag_CONST) != 0 && local->initExprNode != -1) {
                int32_t initExprNode = local->initExprNode;
                int     evalIsConst = 0;
                int     rc;
                if (initExprNode == -2) {
                    HOPTCConstSetReason(
                        evalCtx, nameStart, nameEnd, "const local initializer is recursive");
                    *outIsConst = 0;
                    return 0;
                }
                local->initExprNode = -2;
                rc = HOPTCEvalConstExprNode(evalCtx, initExprNode, outValue, &evalIsConst);
                local->initExprNode = initExprNode;
                if (rc != 0) {
                    return -1;
                }
                *outIsConst = evalIsConst;
                return 0;
            }
        }
    }
    {
        int32_t fnIdx = HOPTCFindPlainFunctionValueIndex(c, nameStart, nameEnd);
        if (fnIdx >= 0) {
            HOPMirValueSetFunctionRef(outValue, (uint32_t)fnIdx);
            *outIsConst = 1;
            return 0;
        }
    }
    {
        int32_t fnIdx = HOPTCFindPkgQualifiedFunctionValueIndexBySlice(c, nameStart, nameEnd);
        if (fnIdx >= 0) {
            HOPMirValueSetFunctionRef(outValue, (uint32_t)fnIdx);
            *outIsConst = 1;
            return 0;
        }
    }
    if (HOPTCHasImportAlias(c, nameStart, nameEnd)) {
        HOPTCConstEvalValueInvalid(outValue);
        outValue->kind = HOPCTFEValue_SPAN;
        outValue->typeTag = HOP_TC_MIR_IMPORT_ALIAS_TAG;
        outValue->span.fileBytes = (const uint8_t*)c->src.ptr + nameStart;
        outValue->span.fileLen = nameEnd - nameStart;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t typeId = HOPTCResolveTypeValueName(c, nameStart, nameEnd);
        if (typeId >= 0) {
            outValue->kind = HOPCTFEValue_TYPE;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = 0;
            outValue->typeTag = HOPTCEncodeTypeTag(c, typeId);
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            outValue->span.fileBytes = NULL;
            outValue->span.fileLen = 0;
            outValue->span.startLine = 0;
            outValue->span.startColumn = 0;
            outValue->span.endLine = 0;
            outValue->span.endColumn = 0;
            *outIsConst = 1;
            return 0;
        }
    }
    nodeId = HOPTCFindTopLevelVarLikeNode(c, nameStart, nameEnd, &nameIndex);
    if (nodeId < 0 || c->ast->nodes[nodeId].kind != HOPAst_CONST) {
        HOPTCConstSetReason(
            evalCtx, nameStart, nameEnd, "identifier is not a const value in this context");
        *outIsConst = 0;
        return 0;
    }
    return HOPTCEvalTopLevelConstNodeAt(c, evalCtx, nodeId, nameIndex, outValue, outIsConst);
}

int HOPTCConstLookupExecBindingType(
    HOPTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    const HOPCTFEExecEnv* frame;
    if (evalCtx == NULL || evalCtx->execCtx == NULL || outType == NULL) {
        return 0;
    }
    frame = evalCtx->execCtx->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            const HOPCTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (HOPNameEqSlice(evalCtx->tc->src, b->nameStart, b->nameEnd, nameStart, nameEnd)) {
                if (b->typeId >= 0) {
                    *outType = b->typeId;
                    return 1;
                }
                if (HOPTCEvalConstExecInferValueTypeCb(evalCtx, &b->value, outType) == 0) {
                    return 1;
                }
            }
        }
        frame = frame->parent;
    }
    return 0;
}

int HOPTCConstLookupMirLocalType(
    HOPTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    HOPTypeCheckCtx* c;
    uint32_t         i;
    uint8_t          savedAllowConstNumericTypeName;
    uint8_t          savedAllowAnytypeParamType;
    if (outType != NULL) {
        *outType = -1;
    }
    if (evalCtx == NULL || outType == NULL || evalCtx->mirProgram == NULL
        || evalCtx->mirFunction == NULL)
    {
        return 0;
    }
    c = evalCtx->tc;
    if (c == NULL || evalCtx->mirFunction->localStart > evalCtx->mirProgram->localLen
        || evalCtx->mirFunction->localCount
               > evalCtx->mirProgram->localLen - evalCtx->mirFunction->localStart)
    {
        return 0;
    }
    savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
    savedAllowAnytypeParamType = c->allowAnytypeParamType;
    c->allowConstNumericTypeName = 1;
    c->allowAnytypeParamType = 1;
    for (i = evalCtx->mirFunction->localCount; i > 0; i--) {
        const HOPMirLocal* local =
            &evalCtx->mirProgram->locals[evalCtx->mirFunction->localStart + i - 1u];
        const HOPCTFEValue* value =
            i - 1u < evalCtx->mirLocalCount ? &evalCtx->mirLocals[i - 1u] : NULL;
        if (local->nameEnd <= local->nameStart
            || !HOPNameEqSlice(c->src, local->nameStart, local->nameEnd, nameStart, nameEnd))
        {
            continue;
        }
        if (local->typeRef < evalCtx->mirProgram->typeLen) {
            const HOPMirTypeRef* typeRef = &evalCtx->mirProgram->types[local->typeRef];
            if (typeRef->astNode > INT32_MAX || typeRef->astNode >= c->ast->len
                || HOPTCResolveTypeNode(c, (int32_t)typeRef->astNode, outType) != 0)
            {
                c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
                c->allowAnytypeParamType = savedAllowAnytypeParamType;
                return -1;
            }
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            c->allowAnytypeParamType = savedAllowAnytypeParamType;
            return 1;
        }
        if (value != NULL && value->kind != HOPCTFEValue_INVALID
            && HOPTCEvalConstExecInferValueTypeCb(evalCtx, value, outType) == 0)
        {
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            c->allowAnytypeParamType = savedAllowAnytypeParamType;
            return 1;
        }
    }
    c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    c->allowAnytypeParamType = savedAllowAnytypeParamType;
    return 0;
}

int HOPTCConstBuiltinSizeBytes(HOPBuiltinKind b, uint64_t* outBytes) {
    if (outBytes == NULL) {
        return 0;
    }
    switch (b) {
        case HOPBuiltin_BOOL:
        case HOPBuiltin_U8:
        case HOPBuiltin_I8:     *outBytes = 1u; return 1;
        case HOPBuiltin_TYPE:   *outBytes = 8u; return 1;
        case HOPBuiltin_U16:
        case HOPBuiltin_I16:    *outBytes = 2u; return 1;
        case HOPBuiltin_U32:
        case HOPBuiltin_I32:
        case HOPBuiltin_F32:    *outBytes = 4u; return 1;
        case HOPBuiltin_U64:
        case HOPBuiltin_I64:
        case HOPBuiltin_F64:    *outBytes = 8u; return 1;
        case HOPBuiltin_USIZE:
        case HOPBuiltin_ISIZE:
        case HOPBuiltin_RAWPTR: *outBytes = (uint64_t)sizeof(void*); return 1;
        default:                return 0;
    }
}

int HOPTCConstBuiltinAlignBytes(HOPBuiltinKind b, uint64_t* outAlign) {
    uint64_t size = 0;
    if (outAlign == NULL || !HOPTCConstBuiltinSizeBytes(b, &size)) {
        return 0;
    }
    *outAlign = size > (uint64_t)sizeof(void*) ? (uint64_t)sizeof(void*) : size;
    if (*outAlign == 0) {
        *outAlign = 1;
    }
    return 1;
}

uint64_t HOPTCConstAlignUpU64(uint64_t v, uint64_t align) {
    if (align == 0) {
        return v;
    }
    return (v + align - 1u) & ~(align - 1u);
}

int HOPTCConstTypeLayout(
    HOPTypeCheckCtx* c, int32_t typeId, uint64_t* outSize, uint64_t* outAlign, uint32_t depth) {
    const HOPTCType* t;
    uint64_t         ptrSize = (uint64_t)sizeof(void*);
    uint64_t         usizeSize = (uint64_t)sizeof(uintptr_t);
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || outSize == NULL || outAlign == NULL
        || depth > c->typeLen)
    {
        return 0;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case HOPTCType_BUILTIN:
            if (!HOPTCConstBuiltinSizeBytes(t->builtin, outSize)
                || !HOPTCConstBuiltinAlignBytes(t->builtin, outAlign))
            {
                if (typeId == c->typeStr) {
                    *outSize = HOPTCConstAlignUpU64(usizeSize, ptrSize) + ptrSize;
                    *outAlign = ptrSize;
                    return 1;
                }
                return 0;
            }
            return 1;
        case HOPTCType_UNTYPED_INT:
            *outSize = (uint64_t)sizeof(ptrdiff_t);
            *outAlign = *outSize;
            return 1;
        case HOPTCType_UNTYPED_FLOAT:
            *outSize = 8u;
            *outAlign = 8u;
            return 1;
        case HOPTCType_PTR:
        case HOPTCType_REF:
        case HOPTCType_FUNCTION:
            *outSize = ptrSize;
            *outAlign = ptrSize;
            return 1;
        case HOPTCType_ARRAY: {
            uint64_t elemSize = 0;
            uint64_t elemAlign = 0;
            if (!HOPTCConstTypeLayout(c, t->baseType, &elemSize, &elemAlign, depth + 1u)) {
                return 0;
            }
            if (t->arrayLen > 0 && elemSize > UINT64_MAX / (uint64_t)t->arrayLen) {
                return 0;
            }
            *outSize = elemSize * (uint64_t)t->arrayLen;
            *outAlign = elemAlign;
            return 1;
        }
        case HOPTCType_SLICE:
            *outSize = HOPTCConstAlignUpU64(usizeSize, ptrSize) + ptrSize;
            *outAlign = ptrSize;
            return 1;
        case HOPTCType_OPTIONAL:
            return HOPTCConstTypeLayout(c, t->baseType, outSize, outAlign, depth + 1u);
        case HOPTCType_NAMED:
        case HOPTCType_ANON_STRUCT: {
            uint64_t offset = 0;
            uint64_t maxAlign = 1;
            uint32_t i;
            for (i = 0; i < t->fieldCount; i++) {
                uint64_t fieldSize = 0;
                uint64_t fieldAlign = 0;
                uint32_t fieldIdx = t->fieldStart + i;
                if (fieldIdx >= c->fieldLen
                    || !HOPTCConstTypeLayout(
                        c, c->fields[fieldIdx].typeId, &fieldSize, &fieldAlign, depth + 1u))
                {
                    return 0;
                }
                if (fieldAlign > maxAlign) {
                    maxAlign = fieldAlign;
                }
                offset = HOPTCConstAlignUpU64(offset, fieldAlign);
                if (fieldSize > UINT64_MAX - offset) {
                    return 0;
                }
                offset += fieldSize;
            }
            *outSize = HOPTCConstAlignUpU64(offset, maxAlign);
            *outAlign = maxAlign;
            return 1;
        }
        case HOPTCType_ANON_UNION: {
            uint64_t maxSize = 0;
            uint64_t maxAlign = 1;
            uint32_t i;
            for (i = 0; i < t->fieldCount; i++) {
                uint64_t fieldSize = 0;
                uint64_t fieldAlign = 0;
                uint32_t fieldIdx = t->fieldStart + i;
                if (fieldIdx >= c->fieldLen
                    || !HOPTCConstTypeLayout(
                        c, c->fields[fieldIdx].typeId, &fieldSize, &fieldAlign, depth + 1u))
                {
                    return 0;
                }
                if (fieldSize > maxSize) {
                    maxSize = fieldSize;
                }
                if (fieldAlign > maxAlign) {
                    maxAlign = fieldAlign;
                }
            }
            *outSize = HOPTCConstAlignUpU64(maxSize, maxAlign);
            *outAlign = maxAlign;
            return 1;
        }
        case HOPTCType_NULL:
            *outSize = ptrSize;
            *outAlign = ptrSize;
            return 1;
        default: return 0;
    }
}

int HOPTCConstEvalSizeOf(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    const HOPAstNode* n;
    int32_t           innerNode;
    int32_t           innerType = -1;
    uint64_t          sizeBytes = 0;
    uint64_t          alignBytes = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    innerNode = HOPAstFirstChild(c->ast, exprNode);
    if (innerNode < 0) {
        HOPTCConstSetReasonNode(evalCtx, exprNode, "sizeof expression is malformed");
        *outIsConst = 0;
        return 0;
    }

    if (n->flags == 1) {
        if (HOPTCResolveTypeNode(c, innerNode, &innerType) != 0) {
            if (c->diag != NULL) {
                *c->diag = (HOPDiag){ 0 };
            }
            if (c->ast->nodes[innerNode].kind == HOPAst_TYPE_NAME) {
                int32_t localIdx = HOPTCLocalFind(
                    c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                if (localIdx >= 0) {
                    innerType = c->locals[localIdx].typeId;
                } else {
                    int32_t fnIdx = HOPTCFindFunctionIndex(
                        c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                    if (fnIdx >= 0) {
                        innerType = c->funcs[fnIdx].funcTypeId;
                    } else {
                        int32_t topNameIndex = -1;
                        int32_t topNode = HOPTCFindTopLevelVarLikeNode(
                            c,
                            c->ast->nodes[innerNode].dataStart,
                            c->ast->nodes[innerNode].dataEnd,
                            &topNameIndex);
                        if (topNode >= 0) {
                            if (HOPTCTypeTopLevelVarLikeNode(c, topNode, topNameIndex, &innerType)
                                != 0)
                            {
                                return -1;
                            }
                        }
                    }
                }
                if (innerType < 0
                    && HOPTCConstLookupExecBindingType(
                        evalCtx,
                        c->ast->nodes[innerNode].dataStart,
                        c->ast->nodes[innerNode].dataEnd,
                        &innerType))
                {
                    /* resolved from const-eval execution environment */
                }
            }
        }
    } else {
        if (HOPTCTypeExpr(c, innerNode, &innerType) != 0) {
            return -1;
        }
        if (HOPTCTypeContainsVarSizeByValue(c, innerType)) {
            HOPTCConstSetReasonNode(
                evalCtx, innerNode, "sizeof operand type is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
    }
    if (innerType < 0) {
        HOPTCConstSetReasonNode(evalCtx, innerNode, "sizeof operand type is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (!HOPTCConstTypeLayout(c, innerType, &sizeBytes, &alignBytes, 0)
        || sizeBytes > (uint64_t)INT64_MAX || alignBytes == 0)
    {
        HOPTCConstSetReasonNode(evalCtx, innerNode, "sizeof operand type is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    outValue->kind = HOPCTFEValue_INT;
    outValue->i64 = (int64_t)sizeBytes;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

int HOPTCConstEvalCast(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx* c;
    int32_t          valueNode;
    int32_t          typeNode;
    int32_t          targetType;
    int32_t          baseTarget;
    HOPCTFEValue     inValue;
    int              inIsConst = 0;

    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }

    valueNode = HOPAstFirstChild(c->ast, exprNode);
    typeNode = valueNode >= 0 ? HOPAstNextSibling(c->ast, valueNode) : -1;
    if (valueNode < 0 || typeNode < 0) {
        HOPTCConstSetReasonNode(evalCtx, exprNode, "cast expression is malformed");
        *outIsConst = 0;
        return 0;
    }

    if (HOPTCResolveTypeNode(c, typeNode, &targetType) != 0) {
        return -1;
    }
    if (HOPTCEvalConstExprNode(evalCtx, valueNode, &inValue, &inIsConst) != 0) {
        return -1;
    }
    if (!inIsConst) {
        *outIsConst = 0;
        return 0;
    }

    baseTarget = HOPTCResolveAliasBaseType(c, targetType);
    if (baseTarget < 0 || (uint32_t)baseTarget >= c->typeLen) {
        *outIsConst = 0;
        return 0;
    }

    if (HOPTCIsIntegerType(c, baseTarget)) {
        int64_t asInt = 0;
        if (inValue.kind == HOPCTFEValue_INT) {
            asInt = inValue.i64;
        } else if (inValue.kind == HOPCTFEValue_BOOL) {
            asInt = inValue.b ? 1 : 0;
        } else if (inValue.kind == HOPCTFEValue_FLOAT) {
            if (inValue.f64 != inValue.f64 || inValue.f64 > (double)INT64_MAX
                || inValue.f64 < (double)INT64_MIN)
            {
                HOPTCConstSetReasonNode(
                    evalCtx, valueNode, "cast result is out of range for const integer");
                *outIsConst = 0;
                return 0;
            }
            asInt = (int64_t)inValue.f64;
        } else if (inValue.kind == HOPCTFEValue_NULL) {
            asInt = 0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = asInt;
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (HOPTCIsFloatType(c, baseTarget)) {
        double asFloat = 0.0;
        if (inValue.kind == HOPCTFEValue_FLOAT) {
            asFloat = inValue.f64;
        } else if (inValue.kind == HOPCTFEValue_INT) {
            asFloat = (double)inValue.i64;
        } else if (inValue.kind == HOPCTFEValue_BOOL) {
            asFloat = inValue.b ? 1.0 : 0.0;
        } else if (inValue.kind == HOPCTFEValue_NULL) {
            asFloat = 0.0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = HOPCTFEValue_FLOAT;
        outValue->i64 = 0;
        outValue->f64 = asFloat;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (HOPTCIsBoolType(c, baseTarget)) {
        uint8_t asBool = 0;
        if (inValue.kind == HOPCTFEValue_BOOL) {
            asBool = inValue.b ? 1u : 0u;
        } else if (inValue.kind == HOPCTFEValue_INT) {
            asBool = inValue.i64 != 0 ? 1u : 0u;
        } else if (inValue.kind == HOPCTFEValue_FLOAT) {
            asBool = inValue.f64 != 0.0 ? 1u : 0u;
        } else if (inValue.kind == HOPCTFEValue_STRING) {
            asBool = 1u;
        } else if (inValue.kind == HOPCTFEValue_NULL) {
            asBool = 0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = HOPCTFEValue_BOOL;
        outValue->i64 = 0;
        outValue->f64 = 0.0;
        outValue->b = asBool;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (HOPTCIsRawptrType(c, baseTarget)) {
        if (inValue.kind == HOPCTFEValue_NULL || inValue.kind == HOPCTFEValue_REFERENCE
            || inValue.kind == HOPCTFEValue_STRING)
        {
            *outValue = inValue;
            outValue->typeTag = (uint64_t)(uint32_t)baseTarget;
            *outIsConst = 1;
            return 0;
        }
        *outIsConst = 0;
        return 0;
    }

    if (c->types[baseTarget].kind == HOPTCType_OPTIONAL) {
        if (inValue.kind == HOPCTFEValue_OPTIONAL) {
            if (inValue.typeTag > 0 && inValue.typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)inValue.typeTag < c->typeLen && (int32_t)inValue.typeTag == baseTarget)
            {
                *outValue = inValue;
                *outIsConst = 1;
                return 0;
            }
            if (inValue.b == 0u) {
                if (HOPTCConstEvalSetOptionalNoneValue(c, baseTarget, outValue) != 0) {
                    return -1;
                }
                *outIsConst = 1;
                return 0;
            }
            if (inValue.s.bytes == NULL) {
                *outIsConst = 0;
                return 0;
            }
            if (HOPTCConstEvalSetOptionalSomeValue(
                    c, baseTarget, (const HOPCTFEValue*)inValue.s.bytes, outValue)
                != 0)
            {
                return -1;
            }
            *outIsConst = 1;
            return 0;
        }
        if (inValue.kind == HOPCTFEValue_NULL) {
            if (HOPTCConstEvalSetOptionalNoneValue(c, baseTarget, outValue) != 0) {
                return -1;
            }
            *outIsConst = 1;
            return 0;
        }
        if (HOPTCConstEvalSetOptionalSomeValue(c, baseTarget, &inValue, outValue) != 0) {
            return -1;
        }
        *outIsConst = 1;
        return 0;
    }

    if ((c->types[baseTarget].kind == HOPTCType_PTR || c->types[baseTarget].kind == HOPTCType_REF
         || c->types[baseTarget].kind == HOPTCType_FUNCTION)
        && inValue.kind == HOPCTFEValue_NULL)
    {
        *outValue = inValue;
        *outIsConst = 1;
        return 0;
    }

    *outValue = inValue;
    *outIsConst = 1;
    return 0;
}

int HOPTCConstEvalTypeOf(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*   c;
    const HOPAstNode*  callee;
    int32_t            calleeNode;
    int32_t            argNode;
    int32_t            argExprNode;
    int32_t            extraNode;
    int32_t            argType;
    uint32_t           callArgIndex = 0;
    int                packStatus;
    HOPTCConstEvalCtx* savedActiveEvalCtx;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = HOPAstFirstChild(c->ast, exprNode);
    argNode = calleeNode >= 0 ? HOPAstNextSibling(c->ast, calleeNode) : -1;
    extraNode = argNode >= 0 ? HOPAstNextSibling(c->ast, argNode) : -1;
    if (calleeNode < 0 || argNode < 0 || extraNode >= 0) {
        HOPTCConstSetReasonNode(evalCtx, exprNode, "typeof call has invalid arity");
        *outIsConst = 0;
        return 0;
    }
    callee = &c->ast->nodes[calleeNode];
    if (callee->kind != HOPAst_IDENT
        || !HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof"))
    {
        return -1;
    }
    argExprNode = argNode;
    if (c->ast->nodes[argExprNode].kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, argExprNode);
        if (inner >= 0) {
            argExprNode = inner;
        }
    }
    packStatus = HOPTCConstEvalResolveTrackedAnyPackArgIndex(evalCtx, argExprNode, &callArgIndex);
    if (packStatus < 0) {
        return -1;
    }
    if (packStatus == 0) {
        if (HOPTCConstEvalGetConcreteCallArgType(evalCtx, callArgIndex, &argType) == 0) {
            goto done;
        }
        {
            const HOPTCCallBinding* binding = (const HOPTCCallBinding*)evalCtx->callBinding;
            if (binding != NULL && callArgIndex < evalCtx->callArgCount
                && binding->argExpectedTypes[callArgIndex] >= 0)
            {
                argType = binding->argExpectedTypes[callArgIndex];
                goto done;
            }
        }
    }
    if (packStatus == 2 || packStatus == 3) {
        *outIsConst = 0;
        return 0;
    }
    {
        HOPCTFEValue argValue;
        int          argIsConst = 0;
        if (HOPTCEvalConstExprNode(evalCtx, argExprNode, &argValue, &argIsConst) != 0) {
            return -1;
        }
        if (argIsConst && HOPTCEvalConstExecInferValueTypeCb(evalCtx, &argValue, &argType) == 0) {
            goto done;
        }
    }
    savedActiveEvalCtx = c->activeConstEvalCtx;
    c->activeConstEvalCtx = evalCtx;
    if (HOPTCTypeExpr(c, argExprNode, &argType) != 0) {
        c->activeConstEvalCtx = savedActiveEvalCtx;
        return -1;
    }
    c->activeConstEvalCtx = savedActiveEvalCtx;
done:
    outValue->kind = HOPCTFEValue_TYPE;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = HOPTCEncodeTypeTag(c, argType);
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

int HOPTCConstEvalLenCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*   c;
    const HOPAstNode*  callee;
    int32_t            calleeNode;
    int32_t            argNode;
    int32_t            argExprNode;
    int32_t            extraNode;
    int32_t            argType = -1;
    int32_t            resolvedArgType = -1;
    HOPCTFEValue       argValue;
    int                argIsConst = 0;
    HOPTCConstEvalCtx* savedActiveEvalCtx;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = HOPAstFirstChild(c->ast, exprNode);
    argNode = calleeNode >= 0 ? HOPAstNextSibling(c->ast, calleeNode) : -1;
    extraNode = argNode >= 0 ? HOPAstNextSibling(c->ast, argNode) : -1;
    if (calleeNode < 0 || argNode < 0 || extraNode >= 0) {
        return 1;
    }
    callee = &c->ast->nodes[calleeNode];
    if (callee->kind != HOPAst_IDENT
        || !HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len"))
    {
        return 1;
    }
    argExprNode = argNode;
    if (c->ast->nodes[argExprNode].kind == HOPAst_CALL_ARG) {
        argExprNode = HOPAstFirstChild(c->ast, argExprNode);
        if (argExprNode < 0) {
            return 1;
        }
    }

    if (HOPTCEvalConstExprNode(evalCtx, argExprNode, &argValue, &argIsConst) != 0) {
        return -1;
    }
    if (argIsConst && argValue.kind == HOPCTFEValue_STRING) {
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = (int64_t)argValue.s.len;
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    savedActiveEvalCtx = c->activeConstEvalCtx;
    c->activeConstEvalCtx = evalCtx;
    if (HOPTCTypeExpr(c, argExprNode, &argType) != 0) {
        c->activeConstEvalCtx = savedActiveEvalCtx;
        return -1;
    }
    c->activeConstEvalCtx = savedActiveEvalCtx;

    resolvedArgType = HOPTCResolveAliasBaseType(c, argType);
    if (resolvedArgType >= 0 && (uint32_t)resolvedArgType < c->typeLen) {
        const HOPTCType* t = &c->types[resolvedArgType];
        if (t->kind == HOPTCType_PACK) {
            outValue->kind = HOPCTFEValue_INT;
            outValue->i64 = (int64_t)t->fieldCount;
            outValue->f64 = 0.0;
            outValue->b = 0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            outValue->span.fileBytes = NULL;
            outValue->span.fileLen = 0;
            outValue->span.startLine = 0;
            outValue->span.startColumn = 0;
            outValue->span.endLine = 0;
            outValue->span.endColumn = 0;
            *outIsConst = 1;
            return 0;
        }
        if (t->kind == HOPTCType_ARRAY) {
            outValue->kind = HOPCTFEValue_INT;
            outValue->i64 = (int64_t)t->arrayLen;
            outValue->f64 = 0.0;
            outValue->b = 0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            outValue->span.fileBytes = NULL;
            outValue->span.fileLen = 0;
            outValue->span.startLine = 0;
            outValue->span.startColumn = 0;
            outValue->span.endLine = 0;
            outValue->span.endColumn = 0;
            *outIsConst = 1;
            return 0;
        }
    }

    HOPTCConstSetReasonNode(evalCtx, argExprNode, "len() operand is not const-evaluable");
    *outIsConst = 0;
    return 0;
}

static int HOPTCConstEvalIndexExpr(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    const HOPAstNode* n;
    int32_t           baseNode;
    int32_t           idxNode;
    int32_t           extraNode;
    HOPCTFEValue      baseValue;
    HOPCTFEValue      idxValue;
    int               baseIsConst = 0;
    int               idxIsConst = 0;
    int64_t           idxInt = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind != HOPAst_INDEX) {
        return 1;
    }
    if ((n->flags & HOPAstFlag_INDEX_SLICE) != 0) {
        return 1;
    }
    baseNode = HOPAstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? HOPAstNextSibling(c->ast, baseNode) : -1;
    extraNode = idxNode >= 0 ? HOPAstNextSibling(c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
        return 1;
    }

    if (HOPTCEvalConstExprNode(evalCtx, baseNode, &baseValue, &baseIsConst) != 0) {
        return -1;
    }
    if (!baseIsConst) {
        HOPTCConstSetReasonNode(evalCtx, baseNode, "index base is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (baseValue.kind != HOPCTFEValue_STRING) {
        HOPTCConstSetReasonNode(evalCtx, baseNode, "index base is not const-evaluable string data");
        *outIsConst = 0;
        return 0;
    }

    if (HOPTCEvalConstExprNode(evalCtx, idxNode, &idxValue, &idxIsConst) != 0) {
        return -1;
    }
    if (!idxIsConst) {
        HOPTCConstSetReasonNode(evalCtx, idxNode, "index is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (HOPCTFEValueToInt64(&idxValue, &idxInt) != 0) {
        HOPTCConstSetReasonNode(evalCtx, idxNode, "index expression did not evaluate to integer");
        *outIsConst = 0;
        return 0;
    }
    if (idxInt < 0) {
        HOPTCConstSetReasonNode(evalCtx, idxNode, "index is negative in const evaluation");
        *outIsConst = 0;
        return 0;
    }
    if ((uint64_t)idxInt >= (uint64_t)baseValue.s.len) {
        HOPTCConstSetReasonNode(evalCtx, idxNode, "index is out of bounds in const evaluation");
        *outIsConst = 0;
        return 0;
    }

    outValue->kind = HOPCTFEValue_INT;
    outValue->i64 = (int64_t)baseValue.s.bytes[(uint32_t)idxInt];
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

int HOPTCResolveReflectedTypeValueExpr(HOPTypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId) {
    const HOPAstNode* n;
    if (c == NULL || outTypeId == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            return 1;
        }
        return HOPTCResolveReflectedTypeValueExpr(c, inner, outTypeId);
    }
    if (n->kind == HOPAst_IDENT) {
        int32_t typeId = HOPTCResolveTypeValueName(c, n->dataStart, n->dataEnd);
        if (typeId < 0) {
            return 1;
        }
        *outTypeId = typeId;
        return 0;
    }
    if (n->kind == HOPAst_TYPE_VALUE) {
        int32_t typeNode = HOPAstFirstChild(c->ast, exprNode);
        if (typeNode < 0) {
            return 1;
        }
        if (HOPTCResolveTypeNode(c, typeNode, outTypeId) != 0) {
            return -1;
        }
        return 0;
    }
    if (HOPTCIsTypeNodeKind(n->kind)) {
        if (HOPTCResolveTypeNode(c, exprNode, outTypeId) != 0) {
            return -1;
        }
        return 0;
    }
    if (n->kind == HOPAst_CALL) {
        int32_t           calleeNode = HOPAstFirstChild(c->ast, exprNode);
        const HOPAstNode* callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
        int32_t           argNode;
        int32_t           extraNode;
        int32_t           elemTypeId;
        if (callee == NULL || callee->kind != HOPAst_IDENT) {
            return 1;
        }

        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof")) {
            argNode = HOPAstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? HOPAstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (HOPTCTypeExpr(c, argNode, outTypeId) != 0) {
                return -1;
            }
            return 0;
        }

        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")) {
            argNode = HOPAstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? HOPAstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (HOPTCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            *outTypeId = HOPTCInternPtrType(c, elemTypeId, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }

        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice")) {
            argNode = HOPAstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? HOPAstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (HOPTCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            *outTypeId = HOPTCInternSliceType(c, elemTypeId, 0, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }

        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            int32_t lenNode;
            int32_t lenType;
            int64_t lenValue = 0;
            int     lenIsConst = 0;
            argNode = HOPAstNextSibling(c->ast, calleeNode);
            lenNode = argNode >= 0 ? HOPAstNextSibling(c->ast, argNode) : -1;
            extraNode = lenNode >= 0 ? HOPAstNextSibling(c->ast, lenNode) : -1;
            if (argNode < 0 || lenNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (HOPTCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            if (HOPTCTypeExpr(c, lenNode, &lenType) != 0) {
                return -1;
            }
            if (!HOPTCIsIntegerType(c, lenType)) {
                return 1;
            }
            if (HOPTCConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0 || !lenIsConst
                || lenValue < 0 || lenValue > (int64_t)UINT32_MAX)
            {
                return 1;
            }
            *outTypeId = HOPTCInternArrayType(c, elemTypeId, (uint32_t)lenValue, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }
    }
    return 1;
}

int HOPTCConstEvalTypeNameValue(
    HOPTypeCheckCtx* c, int32_t typeId, HOPCTFEValue* outValue, int* outIsConst) {
    char         tmp[256];
    HOPTCTextBuf b;
    char*        storage;
    if (c == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    HOPTCTextBufInit(&b, tmp, (uint32_t)sizeof(tmp));
    HOPTCFormatTypeRec(c, typeId, &b, 0);
    storage = (char*)HOPArenaAlloc(c->arena, b.len + 1u, 1u);
    if (storage == NULL) {
        return -1;
    }
    if (b.len > 0u) {
        memcpy(storage, b.ptr, (size_t)b.len);
    }
    storage[b.len] = '\0';
    outValue->kind = HOPCTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = (const uint8_t*)storage;
    outValue->s.len = b.len;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
    *outIsConst = 1;
    return 0;
}

static void HOPTCConstEvalSetTypeValue(HOPTypeCheckCtx* c, int32_t typeId, HOPCTFEValue* outValue) {
    if (c == NULL || outValue == NULL) {
        return;
    }
    outValue->kind = HOPCTFEValue_TYPE;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = HOPTCEncodeTypeTag(c, typeId);
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
}

void HOPTCConstEvalSetNullValue(HOPCTFEValue* outValue) {
    if (outValue == NULL) {
        return;
    }
    outValue->kind = HOPCTFEValue_NULL;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = NULL;
    outValue->s.len = 0;
    outValue->span.fileBytes = NULL;
    outValue->span.fileLen = 0;
    outValue->span.startLine = 0;
    outValue->span.startColumn = 0;
    outValue->span.endLine = 0;
    outValue->span.endColumn = 0;
}

static int HOPTCConstEvalSetOptionalNoneValue(
    HOPTypeCheckCtx* c, int32_t optionalTypeId, HOPCTFEValue* outValue) {
    int32_t baseTypeId;
    if (c == NULL || outValue == NULL) {
        return -1;
    }
    baseTypeId = HOPTCResolveAliasBaseType(c, optionalTypeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
        || c->types[baseTypeId].kind != HOPTCType_OPTIONAL)
    {
        return -1;
    }
    HOPTCConstEvalSetNullValue(outValue);
    outValue->kind = HOPCTFEValue_OPTIONAL;
    outValue->b = 0;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    return 0;
}

static int HOPTCConstEvalSetOptionalSomeValue(
    HOPTypeCheckCtx*    c,
    int32_t             optionalTypeId,
    const HOPCTFEValue* payload,
    HOPCTFEValue*       outValue) {
    HOPCTFEValue* payloadCopy;
    int32_t       baseTypeId;
    if (c == NULL || payload == NULL || outValue == NULL) {
        return -1;
    }
    baseTypeId = HOPTCResolveAliasBaseType(c, optionalTypeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
        || c->types[baseTypeId].kind != HOPTCType_OPTIONAL)
    {
        return -1;
    }
    payloadCopy = (HOPCTFEValue*)HOPArenaAlloc(
        c->arena, sizeof(HOPCTFEValue), (uint32_t)_Alignof(HOPCTFEValue));
    if (payloadCopy == NULL) {
        return -1;
    }
    *payloadCopy = *payload;
    HOPTCConstEvalSetNullValue(outValue);
    outValue->kind = HOPCTFEValue_OPTIONAL;
    outValue->b = 1u;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    outValue->s.bytes = (const uint8_t*)payloadCopy;
    outValue->s.len = 0;
    return 0;
}

void HOPTCConstEvalSetSourceLocationFromOffsets(
    HOPTypeCheckCtx* c, uint32_t startOffset, uint32_t endOffset, HOPCTFEValue* outValue) {
    HOPTCConstEvalSetNullValue(outValue);
    outValue->kind = HOPCTFEValue_SPAN;
    outValue->span.fileBytes = (const uint8_t*)"";
    outValue->span.fileLen = 0;
    HOPTCOffsetToLineCol(
        c->src.ptr,
        c->src.len,
        startOffset,
        &outValue->span.startLine,
        &outValue->span.startColumn);
    HOPTCOffsetToLineCol(
        c->src.ptr, c->src.len, endOffset, &outValue->span.endLine, &outValue->span.endColumn);
}

/* Returns 0 when resolved, 1 when not a tracked anypack index expression, 2 on non-const index,
 * 3 on out-of-bounds index, and -1 on hard error. */
static int HOPTCConstEvalResolveTrackedAnyPackArgIndex(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outCallArgIndex) {
    HOPTypeCheckCtx*        c;
    const HOPTCCallBinding* binding;
    const HOPTCCallArgInfo* callArgs;
    int32_t                 baseNode;
    int32_t                 idxNode;
    int32_t                 extraNode;
    int64_t                 idxValue = 0;
    uint32_t                paramIndex;
    uint32_t                ordinal = 0;
    uint32_t                i;
    HOPCTFEValue            idxConstValue;
    int                     idxIsConst = 0;
    if (evalCtx == NULL || outCallArgIndex == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    binding = (const HOPTCCallBinding*)evalCtx->callBinding;
    callArgs = (const HOPTCCallArgInfo*)evalCtx->callArgs;
    if (c == NULL || binding == NULL || callArgs == NULL || evalCtx->callArgCount == 0) {
        return 1;
    }
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len
        || c->ast->nodes[exprNode].kind != HOPAst_INDEX
        || (c->ast->nodes[exprNode].flags & HOPAstFlag_INDEX_SLICE) != 0)
    {
        return 1;
    }
    baseNode = HOPAstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? HOPAstNextSibling(c->ast, baseNode) : -1;
    extraNode = idxNode >= 0 ? HOPAstNextSibling(c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
        return 1;
    }
    if (c->ast->nodes[baseNode].kind != HOPAst_IDENT
        || !HOPTCConstEvalIsTrackedAnyPackName(
            evalCtx, c->ast->nodes[baseNode].dataStart, c->ast->nodes[baseNode].dataEnd))
    {
        return 1;
    }
    if (HOPTCEvalConstExprNode(evalCtx, idxNode, &idxConstValue, &idxIsConst) != 0) {
        return -1;
    }
    if (!idxIsConst || HOPCTFEValueToInt64(&idxConstValue, &idxValue) != 0) {
        HOPTCConstSetReasonNode(
            evalCtx, idxNode, "anytype pack index must be const-evaluable integer");
        return 2;
    }
    if (idxValue < 0) {
        HOPTCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
        return 3;
    }
    if (!binding->isVariadic) {
        HOPTCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
        return 3;
    }
    if (binding->spreadArgIndex != UINT32_MAX) {
        uint32_t spreadArgIndex = binding->spreadArgIndex;
        int32_t  spreadArgType = -1;
        if (spreadArgIndex < evalCtx->callArgCount
            && HOPTCConstEvalGetConcreteCallArgType(evalCtx, spreadArgIndex, &spreadArgType) == 0)
        {
            int32_t spreadType = HOPTCResolveAliasBaseType(c, spreadArgType);
            if (spreadType >= 0 && (uint32_t)spreadType < c->typeLen
                && c->types[spreadType].kind == HOPTCType_PACK)
            {
                if ((uint64_t)idxValue >= (uint64_t)c->types[spreadType].fieldCount) {
                    HOPTCConstSetReasonNode(
                        evalCtx, idxNode, "anytype pack index is out of bounds");
                    return 3;
                }
                return 1;
            }
        }
        HOPTCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
        return 3;
    }
    paramIndex = binding->fixedCount;
    for (i = 0; i < evalCtx->callArgCount; i++) {
        if (binding->argParamIndices[i] != (int32_t)paramIndex) {
            continue;
        }
        if ((int64_t)ordinal == idxValue) {
            *outCallArgIndex = i;
            return 0;
        }
        ordinal++;
    }
    HOPTCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
    return 3;
}

static int HOPTCConstEvalGetConcreteCallArgType(
    HOPTCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outType) {
    HOPTypeCheckCtx*        c;
    const HOPTCCallBinding* binding;
    const HOPTCCallArgInfo* callArgs;
    HOPTCConstEvalCtx* _Nullable savedActiveEvalCtx;
    int32_t argType = -1;
    if (outType != NULL) {
        *outType = -1;
    }
    if (evalCtx == NULL || outType == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    binding = (const HOPTCCallBinding*)evalCtx->callBinding;
    callArgs = (const HOPTCCallArgInfo*)evalCtx->callArgs;
    if (c == NULL || binding == NULL || callArgs == NULL || callArgIndex >= evalCtx->callArgCount) {
        return 1;
    }
    if (callArgs[callArgIndex].exprNode >= 0
        && (uint32_t)callArgs[callArgIndex].exprNode < c->ast->len)
    {
        savedActiveEvalCtx = c->activeConstEvalCtx;
        c->activeConstEvalCtx = evalCtx;
        if (HOPTCTypeExpr(c, callArgs[callArgIndex].exprNode, &argType) != 0) {
            c->activeConstEvalCtx = savedActiveEvalCtx;
            return -1;
        }
        c->activeConstEvalCtx = savedActiveEvalCtx;
        if ((argType == c->typeUntypedInt || argType == c->typeUntypedFloat)
            && HOPTCConcretizeInferredType(c, argType, &argType) != 0)
        {
            return -1;
        }
        if (argType >= 0 && argType != c->typeAnytype) {
            *outType = argType;
            return 0;
        }
    }
    if (HOPTCConstEvalGetConcreteCallArgPackType(evalCtx, callArgIndex, &argType) == 0) {
        *outType = argType;
        return 0;
    }
    if (binding->argExpectedTypes[callArgIndex] >= 0) {
        *outType = binding->argExpectedTypes[callArgIndex];
        return 0;
    }
    if (argType >= 0) {
        *outType = argType;
        return 0;
    }
    return 1;
}

int HOPTCConstEvalSourceLocationOfCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    int32_t           calleeNode;
    const HOPAstNode* callee;
    int32_t           operandNode = -1;
    int32_t           nextNode = -1;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = HOPAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == HOPAst_IDENT) {
        if (!HOPTCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd)) {
            return 1;
        }
        operandNode = HOPAstNextSibling(c->ast, calleeNode);
        nextNode = operandNode >= 0 ? HOPAstNextSibling(c->ast, operandNode) : -1;
        if (operandNode < 0 || nextNode >= 0) {
            return 1;
        }
    } else if (callee->kind == HOPAst_FIELD_EXPR) {
        int32_t recvNode = HOPAstFirstChild(c->ast, calleeNode);
        if (recvNode < 0 || c->ast->nodes[recvNode].kind != HOPAst_IDENT
            || !HOPNameEqLiteral(
                c->src,
                c->ast->nodes[recvNode].dataStart,
                c->ast->nodes[recvNode].dataEnd,
                "builtin")
            || !HOPTCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd))
        {
            return 1;
        }
        operandNode = HOPAstNextSibling(c->ast, calleeNode);
        nextNode = operandNode >= 0 ? HOPAstNextSibling(c->ast, operandNode) : -1;
        if (operandNode < 0 || nextNode >= 0) {
            return 1;
        }
    } else {
        return 1;
    }
    if (c->ast->nodes[operandNode].kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, operandNode);
        if (inner < 0) {
            return 1;
        }
        operandNode = inner;
    }
    {
        uint32_t callArgIndex = 0;
        int      packStatus = HOPTCConstEvalResolveTrackedAnyPackArgIndex(
            evalCtx, operandNode, &callArgIndex);
        if (packStatus < 0) {
            return -1;
        }
        if (packStatus == 0) {
            const HOPTCCallArgInfo* callArgs = (const HOPTCCallArgInfo*)evalCtx->callArgs;
            HOPTCConstEvalSetSourceLocationFromOffsets(
                c, callArgs[callArgIndex].start, callArgs[callArgIndex].end, outValue);
            *outIsConst = 1;
            return 0;
        }
        if (packStatus == 2 || packStatus == 3) {
            *outIsConst = 0;
            return 0;
        }
    }
    if ((c->ast->nodes[operandNode].kind == HOPAst_IDENT
         || c->ast->nodes[operandNode].kind == HOPAst_TYPE_NAME)
        && HOPNameHasPrefix(
            c->src,
            c->ast->nodes[operandNode].dataStart,
            c->ast->nodes[operandNode].dataEnd,
            "__hop_"))
    {
        HOPTCConstSetReasonNode(
            evalCtx, operandNode, "source_location_of operand cannot reference __hop_ names");
        *outIsConst = 0;
        return 0;
    }
    HOPTCConstEvalSetSourceLocationFromOffsets(
        c, c->ast->nodes[operandNode].start, c->ast->nodes[operandNode].end, outValue);
    *outIsConst = 1;
    return 0;
}

int HOPTCConstEvalU32Arg(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outValue, int* outIsConst) {
    HOPCTFEValue v;
    int          isConst = 0;
    if (outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (HOPTCEvalConstExprNode(evalCtx, exprNode, &v, &isConst) != 0) {
        return -1;
    }
    if (!isConst || v.kind != HOPCTFEValue_INT || v.i64 < 0 || v.i64 > (int64_t)UINT32_MAX) {
        *outIsConst = 0;
        return 0;
    }
    *outValue = (uint32_t)v.i64;
    *outIsConst = 1;
    return 0;
}

int HOPTCConstEvalSourceLocationCompound(
    HOPTCConstEvalCtx* evalCtx,
    int32_t            exprNode,
    int                forceSourceLocation,
    HOPCTFEValue*      outValue,
    int*               outIsConst) {
    HOPTypeCheckCtx* c;
    int32_t          child;
    int32_t          fieldNode;
    int32_t          resolvedType = -1;
    uint32_t         startLine = 0;
    uint32_t         startColumn = 0;
    uint32_t         endLine = 0;
    uint32_t         endColumn = 0;
    const uint8_t*   fileBytes = (const uint8_t*)"";
    uint32_t         fileLen = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len
        || c->ast->nodes[exprNode].kind != HOPAst_COMPOUND_LIT)
    {
        return 0;
    }
    child = HOPAstFirstChild(c->ast, exprNode);
    fieldNode = child;
    if (child >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        if (HOPTCResolveTypeNode(c, child, &resolvedType) != 0) {
            return -1;
        }
        if (!HOPTCTypeIsSourceLocation(c, resolvedType)) {
            return 0;
        }
        fieldNode = HOPAstNextSibling(c->ast, child);
    } else if (!forceSourceLocation) {
        return 0;
    }

    HOPTCOffsetToLineCol(
        c->src.ptr, c->src.len, c->ast->nodes[exprNode].start, &startLine, &startColumn);
    HOPTCOffsetToLineCol(c->src.ptr, c->src.len, c->ast->nodes[exprNode].end, &endLine, &endColumn);

    while (fieldNode >= 0) {
        const HOPAstNode* field = &c->ast->nodes[fieldNode];
        int32_t           valueNode = HOPAstFirstChild(c->ast, fieldNode);
        if (field->kind != HOPAst_COMPOUND_FIELD || valueNode < 0) {
            goto non_const;
        }
        if (HOPNameEqLiteral(c->src, field->dataStart, field->dataEnd, "file")) {
            HOPCTFEValue fileValue;
            int          fileIsConst = 0;
            if (HOPTCEvalConstExprNode(evalCtx, valueNode, &fileValue, &fileIsConst) != 0) {
                return -1;
            }
            if (!fileIsConst || fileValue.kind != HOPCTFEValue_STRING) {
                goto non_const;
            }
            fileBytes = fileValue.s.bytes;
            fileLen = fileValue.s.len;
        } else if (HOPNameEqLiteral(c->src, field->dataStart, field->dataEnd, "start_line")) {
            int isConst = 0;
            if (HOPTCConstEvalU32Arg(evalCtx, valueNode, &startLine, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else if (HOPNameEqLiteral(c->src, field->dataStart, field->dataEnd, "start_column")) {
            int isConst = 0;
            if (HOPTCConstEvalU32Arg(evalCtx, valueNode, &startColumn, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else if (HOPNameEqLiteral(c->src, field->dataStart, field->dataEnd, "end_line")) {
            int isConst = 0;
            if (HOPTCConstEvalU32Arg(evalCtx, valueNode, &endLine, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else if (HOPNameEqLiteral(c->src, field->dataStart, field->dataEnd, "end_column")) {
            int isConst = 0;
            if (HOPTCConstEvalU32Arg(evalCtx, valueNode, &endColumn, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                goto non_const;
            }
        } else {
            goto non_const;
        }
        fieldNode = HOPAstNextSibling(c->ast, fieldNode);
    }

    HOPTCConstEvalSetNullValue(outValue);
    outValue->kind = HOPCTFEValue_SPAN;
    outValue->span.fileBytes = fileBytes;
    outValue->span.fileLen = fileLen;
    outValue->span.startLine = startLine;
    outValue->span.startColumn = startColumn;
    outValue->span.endLine = endLine;
    outValue->span.endColumn = endColumn;
    *outIsConst = 1;
    return 1;

non_const:
    HOPTCConstSetReasonNode(
        evalCtx, exprNode, "builtin.SourceLocation literal is not const-evaluable");
    HOPTCConstEvalSetNullValue(outValue);
    *outIsConst = 0;
    return 1;
}

int HOPTCConstEvalSourceLocationExpr(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFESpan* outSpan, int* outIsConst) {
    HOPTypeCheckCtx* c;
    HOPCTFEValue     v;
    int              isConst = 0;
    int              handled;
    if (evalCtx == NULL || outSpan == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        *outIsConst = 0;
        return 0;
    }
    if (c->ast->nodes[exprNode].kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            *outIsConst = 0;
            return 0;
        }
        exprNode = inner;
    }
    if (c->ast->nodes[exprNode].kind == HOPAst_COMPOUND_LIT) {
        handled = HOPTCConstEvalSourceLocationCompound(evalCtx, exprNode, 1, &v, &isConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            if (!isConst || v.kind != HOPCTFEValue_SPAN) {
                *outIsConst = 0;
                return 0;
            }
            *outSpan = v.span;
            *outIsConst = 1;
            return 0;
        }
    }
    if (HOPTCEvalConstExprNode(evalCtx, exprNode, &v, &isConst) != 0) {
        return -1;
    }
    if (!isConst || v.kind != HOPCTFEValue_SPAN) {
        *outIsConst = 0;
        return 0;
    }
    *outSpan = v.span;
    *outIsConst = 1;
    return 0;
}

static HOPTCCompilerDiagOp HOPTCConstEvalCompilerDiagOpFromFieldExpr(
    HOPTypeCheckCtx* c, const HOPAstNode* fieldExpr) {
    HOPTCCompilerDiagOp op;
    uint32_t            segStart;
    uint32_t            i;
    if (c == NULL || fieldExpr == NULL) {
        return HOPTCCompilerDiagOp_NONE;
    }
    op = HOPTCCompilerDiagOpFromName(c, fieldExpr->dataStart, fieldExpr->dataEnd);
    if (op != HOPTCCompilerDiagOp_NONE) {
        return op;
    }
    if (fieldExpr->dataEnd <= fieldExpr->dataStart || fieldExpr->dataEnd > c->src.len) {
        return HOPTCCompilerDiagOp_NONE;
    }
    segStart = fieldExpr->dataStart;
    for (i = fieldExpr->dataStart; i < fieldExpr->dataEnd; i++) {
        if (c->src.ptr[i] == '.') {
            segStart = i + 1u;
        }
    }
    if (HOPNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "error")) {
        return HOPTCCompilerDiagOp_ERROR;
    }
    if (HOPNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "error_at")) {
        return HOPTCCompilerDiagOp_ERROR_AT;
    }
    if (HOPNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "warn")) {
        return HOPTCCompilerDiagOp_WARN;
    }
    if (HOPNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "warn_at")) {
        return HOPTCCompilerDiagOp_WARN_AT;
    }
    return HOPTCCompilerDiagOp_NONE;
}

int HOPTCConstEvalCompilerDiagCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*    c;
    int32_t             calleeNode;
    const HOPAstNode*   callee;
    HOPTCCompilerDiagOp op = HOPTCCompilerDiagOp_NONE;
    int32_t             msgNode = -1;
    int32_t             spanNode = -1;
    int32_t             nextNode;
    HOPCTFEValue        msgValue;
    int                 msgIsConst = 0;
    uint32_t            diagStart = 0;
    uint32_t            diagEnd = 0;
    const char*         detail;
    HOPDiag             emitted;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = HOPAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == HOPAst_IDENT) {
        op = HOPTCCompilerDiagOpFromName(c, callee->dataStart, callee->dataEnd);
    } else if (callee->kind == HOPAst_FIELD_EXPR) {
        int32_t recvNode = HOPAstFirstChild(c->ast, calleeNode);
        if (recvNode >= 0 && c->ast->nodes[recvNode].kind == HOPAst_IDENT
            && HOPNameEqLiteral(
                c->src,
                c->ast->nodes[recvNode].dataStart,
                c->ast->nodes[recvNode].dataEnd,
                "compiler"))
        {
            op = HOPTCConstEvalCompilerDiagOpFromFieldExpr(c, callee);
        }
    }
    if (op == HOPTCCompilerDiagOp_NONE) {
        return 1;
    }

    if (op == HOPTCCompilerDiagOp_ERROR || op == HOPTCCompilerDiagOp_WARN) {
        msgNode = HOPAstNextSibling(c->ast, calleeNode);
        nextNode = msgNode >= 0 ? HOPAstNextSibling(c->ast, msgNode) : -1;
        if (msgNode < 0 || nextNode >= 0) {
            return 1;
        }
        diagStart = c->ast->nodes[exprNode].start;
        diagEnd = c->ast->nodes[exprNode].end;
    } else {
        int         spanIsConst = 0;
        HOPCTFESpan span;
        uint32_t    spanStartOffset = 0;
        uint32_t    spanEndOffset = 0;
        spanNode = HOPAstNextSibling(c->ast, calleeNode);
        msgNode = spanNode >= 0 ? HOPAstNextSibling(c->ast, spanNode) : -1;
        nextNode = msgNode >= 0 ? HOPAstNextSibling(c->ast, msgNode) : -1;
        if (spanNode < 0 || msgNode < 0 || nextNode >= 0) {
            return 1;
        }
        if (HOPTCConstEvalSourceLocationExpr(evalCtx, spanNode, &span, &spanIsConst) != 0) {
            return -1;
        }
        if (!spanIsConst || span.startLine == 0 || span.startColumn == 0 || span.endLine == 0
            || span.endColumn == 0
            || HOPTCLineColToOffset(
                   c->src.ptr, c->src.len, span.startLine, span.startColumn, &spanStartOffset)
                   != 0
            || HOPTCLineColToOffset(
                   c->src.ptr, c->src.len, span.endLine, span.endColumn, &spanEndOffset)
                   != 0
            || spanEndOffset < spanStartOffset)
        {
            return HOPTCFailNode(c, spanNode, HOPDiag_CONSTEVAL_DIAG_INVALID_SPAN);
        }
        diagStart = spanStartOffset;
        diagEnd = spanEndOffset;
    }
    if (msgNode >= 0 && (uint32_t)msgNode < c->ast->len
        && c->ast->nodes[msgNode].kind == HOPAst_CALL_ARG)
    {
        int32_t inner = HOPAstFirstChild(c->ast, msgNode);
        if (inner >= 0) {
            msgNode = inner;
        }
    }

    if (HOPTCEvalConstExprNode(evalCtx, msgNode, &msgValue, &msgIsConst) != 0) {
        return -1;
    }
    if (!msgIsConst || msgValue.kind != HOPCTFEValue_STRING) {
        return HOPTCFailNode(c, msgNode, HOPDiag_CONSTEVAL_DIAG_MESSAGE_NOT_CONST_STRING);
    }
    detail = HOPTCAllocCStringBytes(c, msgValue.s.bytes, msgValue.s.len);
    if (detail == NULL) {
        return HOPTCFailNode(c, msgNode, HOPDiag_ARENA_OOM);
    }

    emitted = (HOPDiag){
        .code = (op == HOPTCCompilerDiagOp_WARN || op == HOPTCCompilerDiagOp_WARN_AT)
                  ? HOPDiag_CONSTEVAL_DIAG_WARNING
                  : HOPDiag_CONSTEVAL_DIAG_ERROR,
        .type = (op == HOPTCCompilerDiagOp_WARN || op == HOPTCCompilerDiagOp_WARN_AT)
                  ? HOPDiagType_WARNING
                  : HOPDiagType_ERROR,
        .start = diagStart,
        .end = diagEnd,
        .argStart = 0,
        .argEnd = 0,
        .detail = detail,
        .hintOverride = NULL,
    };
    HOPTCMarkConstDiagUseExecuted(c, exprNode);
    if (emitted.type == HOPDiagType_WARNING) {
        if (HOPTCEmitWarningDiag(c, &emitted) != 0) {
            return -1;
        }
        HOPTCConstEvalSetNullValue(outValue);
        *outIsConst = 1;
        return 0;
    }

    if (c->diag != NULL) {
        *c->diag = emitted;
    }
    return -1;
}

int HOPTCConstEvalTypeReflectionCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    int32_t           calleeNode;
    const HOPAstNode* callee;
    int32_t           op = 0;
    int32_t           operandNode = -1;
    int32_t           operandNode2 = -1;
    HOPCTFEValue      operandValue;
    HOPCTFEValue      operandValue2;
    int               operandIsConst = 0;
    int               operandIsConst2 = 0;
    int32_t           reflectedTypeId;
    enum {
        HOPTCReflectKind_KIND = 1,
        HOPTCReflectKind_BASE = 2,
        HOPTCReflectKind_IS_ALIAS = 3,
        HOPTCReflectKind_TYPE_NAME = 4,
        HOPTCReflectKind_PTR = 5,
        HOPTCReflectKind_SLICE = 6,
        HOPTCReflectKind_ARRAY = 7,
    };
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = HOPAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == HOPAst_IDENT) {
        int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
        int32_t nextNode = argNode >= 0 ? HOPAstNextSibling(c->ast, argNode) : -1;
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            op = HOPTCReflectKind_KIND;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            op = HOPTCReflectKind_BASE;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            op = HOPTCReflectKind_IS_ALIAS;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            op = HOPTCReflectKind_TYPE_NAME;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")) {
            op = HOPTCReflectKind_PTR;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice")) {
            op = HOPTCReflectKind_SLICE;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            op = HOPTCReflectKind_ARRAY;
        } else {
            return 1;
        }
        if (op == HOPTCReflectKind_ARRAY) {
            int32_t extraNode = nextNode >= 0 ? HOPAstNextSibling(c->ast, nextNode) : -1;
            if (argNode < 0 || nextNode < 0 || extraNode >= 0) {
                return 1;
            }
            operandNode = argNode;
            operandNode2 = nextNode;
        } else {
            if (argNode < 0 || nextNode >= 0) {
                return 1;
            }
            operandNode = argNode;
        }
    } else if (callee->kind == HOPAst_FIELD_EXPR) {
        int32_t recvNode = HOPAstFirstChild(c->ast, calleeNode);
        int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            op = HOPTCReflectKind_KIND;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            op = HOPTCReflectKind_BASE;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            op = HOPTCReflectKind_IS_ALIAS;
        } else if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            op = HOPTCReflectKind_TYPE_NAME;
        } else {
            return 1;
        }
        if (recvNode < 0 || nextArgNode >= 0) {
            return 1;
        }
        operandNode = recvNode;
    } else {
        return 1;
    }

    if (c->ast->nodes[operandNode].kind == HOPAst_CALL_ARG) {
        int32_t innerNode = HOPAstFirstChild(c->ast, operandNode);
        if (innerNode < 0) {
            return 1;
        }
        operandNode = innerNode;
    }
    if (operandNode2 >= 0 && c->ast->nodes[operandNode2].kind == HOPAst_CALL_ARG) {
        int32_t innerNode = HOPAstFirstChild(c->ast, operandNode2);
        if (innerNode < 0) {
            return 1;
        }
        operandNode2 = innerNode;
    }

    if (HOPTCEvalConstExprNode(evalCtx, operandNode, &operandValue, &operandIsConst) != 0) {
        return -1;
    }
    if (!operandIsConst || operandValue.kind != HOPCTFEValue_TYPE) {
        return 1;
    }

    if (op == HOPTCReflectKind_KIND) {
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = (int64_t)((operandValue.typeTag >> 56u) & 0xffu);
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    if (op == HOPTCReflectKind_IS_ALIAS) {
        outValue->kind = HOPCTFEValue_BOOL;
        outValue->b = ((operandValue.typeTag >> 56u) & 0xffu) == (uint64_t)HOPTCTypeTagKind_ALIAS;
        outValue->i64 = 0;
        outValue->f64 = 0.0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }

    if (HOPTCDecodeTypeTag(c, operandValue.typeTag, &reflectedTypeId) != 0) {
        return 1;
    }
    if (op == HOPTCReflectKind_PTR || op == HOPTCReflectKind_SLICE || op == HOPTCReflectKind_ARRAY)
    {
        int32_t constructedTypeId = -1;
        if (op == HOPTCReflectKind_PTR) {
            constructedTypeId = HOPTCInternPtrType(c, reflectedTypeId, callee->start, callee->end);
        } else if (op == HOPTCReflectKind_SLICE) {
            constructedTypeId = HOPTCInternSliceType(
                c, reflectedTypeId, 0, callee->start, callee->end);
        } else {
            int64_t arrayLen = 0;
            if (operandNode2 < 0) {
                return 1;
            }
            if (HOPTCEvalConstExprNode(evalCtx, operandNode2, &operandValue2, &operandIsConst2)
                != 0)
            {
                return -1;
            }
            if (!operandIsConst2 || HOPCTFEValueToInt64(&operandValue2, &arrayLen) != 0
                || arrayLen < 0 || arrayLen > (int64_t)UINT32_MAX)
            {
                return 1;
            }
            constructedTypeId = HOPTCInternArrayType(
                c, reflectedTypeId, (uint32_t)arrayLen, callee->start, callee->end);
        }
        if (constructedTypeId < 0) {
            return -1;
        }
        HOPTCConstEvalSetTypeValue(c, constructedTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (op == HOPTCReflectKind_TYPE_NAME) {
        return HOPTCConstEvalTypeNameValue(c, reflectedTypeId, outValue, outIsConst);
    }
    if (reflectedTypeId < 0 || (uint32_t)reflectedTypeId >= c->typeLen
        || c->types[reflectedTypeId].kind != HOPTCType_ALIAS)
    {
        HOPTCConstSetReasonNode(evalCtx, operandNode, "base() requires an alias type");
        *outIsConst = 0;
        return 0;
    }
    if (HOPTCResolveAliasTypeId(c, reflectedTypeId) != 0) {
        return -1;
    }
    reflectedTypeId = c->types[reflectedTypeId].baseType;
    HOPTCConstEvalSetTypeValue(c, reflectedTypeId, outValue);
    *outIsConst = 1;
    return 0;
}

static int HOPTCConstEvalTypeReflectionByArgs(
    HOPTCConstEvalCtx*  evalCtx,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* args,
    uint32_t            argCount,
    HOPCTFEValue*       outValue,
    int*                outIsConst) {
    HOPTypeCheckCtx* c;
    int32_t          op = 0;
    int32_t          reflectedTypeId = -1;
    enum {
        HOPTCReflectArg_KIND = 1,
        HOPTCReflectArg_BASE = 2,
        HOPTCReflectArg_IS_ALIAS = 3,
        HOPTCReflectArg_TYPE_NAME = 4,
        HOPTCReflectArg_PTR = 5,
        HOPTCReflectArg_SLICE = 6,
        HOPTCReflectArg_ARRAY = 7,
    };
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "kind")) {
        op = HOPTCReflectArg_KIND;
    } else if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "base")) {
        op = HOPTCReflectArg_BASE;
    } else if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "is_alias")) {
        op = HOPTCReflectArg_IS_ALIAS;
    } else if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "type_name")) {
        op = HOPTCReflectArg_TYPE_NAME;
    } else if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "ptr")) {
        op = HOPTCReflectArg_PTR;
    } else if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "slice")) {
        op = HOPTCReflectArg_SLICE;
    } else if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "array")) {
        op = HOPTCReflectArg_ARRAY;
    } else {
        return 1;
    }
    if (op == HOPTCReflectArg_ARRAY) {
        int64_t arrayLen = 0;
        int32_t arrayTypeId;
        if (argCount != 2u || args == NULL || args[0].kind != HOPCTFEValue_TYPE
            || HOPCTFEValueToInt64(&args[1], &arrayLen) != 0 || arrayLen < 0
            || arrayLen > (int64_t)UINT32_MAX)
        {
            return 1;
        }
        if (HOPTCDecodeTypeTag(c, args[0].typeTag, &reflectedTypeId) != 0) {
            return -1;
        }
        arrayTypeId = HOPTCInternArrayType(c, reflectedTypeId, (uint32_t)arrayLen, 0, 0);
        if (arrayTypeId < 0) {
            return -1;
        }
        HOPTCConstEvalSetTypeValue(c, arrayTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (argCount != 1u || args == NULL || args[0].kind != HOPCTFEValue_TYPE) {
        return 1;
    }
    if (op == HOPTCReflectArg_KIND) {
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = (int64_t)((args[0].typeTag >> 56u) & 0xffu);
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    if (op == HOPTCReflectArg_IS_ALIAS) {
        outValue->kind = HOPCTFEValue_BOOL;
        outValue->b = ((args[0].typeTag >> 56u) & 0xffu) == (uint64_t)HOPTCTypeTagKind_ALIAS;
        outValue->i64 = 0;
        outValue->f64 = 0.0;
        outValue->typeTag = 0;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->span.fileBytes = NULL;
        outValue->span.fileLen = 0;
        outValue->span.startLine = 0;
        outValue->span.startColumn = 0;
        outValue->span.endLine = 0;
        outValue->span.endColumn = 0;
        *outIsConst = 1;
        return 0;
    }
    if (HOPTCDecodeTypeTag(c, args[0].typeTag, &reflectedTypeId) != 0) {
        return -1;
    }
    if (op == HOPTCReflectArg_PTR) {
        int32_t ptrTypeId = HOPTCInternPtrType(c, reflectedTypeId, 0, 0);
        if (ptrTypeId < 0) {
            return -1;
        }
        HOPTCConstEvalSetTypeValue(c, ptrTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (op == HOPTCReflectArg_SLICE) {
        int32_t sliceTypeId = HOPTCInternSliceType(c, reflectedTypeId, 0, 0, 0);
        if (sliceTypeId < 0) {
            return -1;
        }
        HOPTCConstEvalSetTypeValue(c, sliceTypeId, outValue);
        *outIsConst = 1;
        return 0;
    }
    if (op == HOPTCReflectArg_TYPE_NAME) {
        return HOPTCConstEvalTypeNameValue(c, reflectedTypeId, outValue, outIsConst);
    }
    if (reflectedTypeId < 0 || (uint32_t)reflectedTypeId >= c->typeLen
        || c->types[reflectedTypeId].kind != HOPTCType_ALIAS)
    {
        HOPTCConstSetReason(evalCtx, nameStart, nameEnd, "base() requires an alias type");
        *outIsConst = 0;
        return 0;
    }
    if (HOPTCResolveAliasTypeId(c, reflectedTypeId) != 0) {
        return -1;
    }
    HOPTCConstEvalSetTypeValue(c, c->types[reflectedTypeId].baseType, outValue);
    *outIsConst = 1;
    return 0;
}

static int HOPTCConstEvalPkgFunctionValueExpr(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    const HOPAstNode* n;
    int32_t           recvNode;
    const HOPAstNode* recv;
    int32_t           fnIndex;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind != HOPAst_FIELD_EXPR) {
        return 1;
    }
    recvNode = HOPAstFirstChild(c->ast, exprNode);
    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len) {
        return 1;
    }
    recv = &c->ast->nodes[recvNode];
    if (recv->kind != HOPAst_IDENT) {
        return 1;
    }
    if (evalCtx->execCtx != NULL) {
        int32_t execType = -1;
        if (HOPTCConstLookupExecBindingType(evalCtx, recv->dataStart, recv->dataEnd, &execType)) {
            return 1;
        }
    }
    if (HOPTCLocalFind(c, recv->dataStart, recv->dataEnd) >= 0
        || HOPTCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) >= 0)
    {
        return 1;
    }
    fnIndex = HOPTCFindPkgQualifiedFunctionValueIndex(
        c, recv->dataStart, recv->dataEnd, n->dataStart, n->dataEnd);
    if (fnIndex < 0) {
        return 1;
    }
    HOPMirValueSetFunctionRef(outValue, (uint32_t)fnIndex);
    *outIsConst = 1;
    return 0;
}

int HOPTCEvalConstExprNode(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx* c;
    HOPAstKind       kind;
    int              rc;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    kind = c->ast->nodes[exprNode].kind;
    if (kind == HOPAst_BINARY) {
        const HOPAstNode* n = &c->ast->nodes[exprNode];
        if ((HOPTokenKind)n->op == HOPTok_EQ || (HOPTokenKind)n->op == HOPTok_NEQ) {
            int32_t lhsNode = HOPAstFirstChild(c->ast, exprNode);
            int32_t rhsNode = lhsNode >= 0 ? HOPAstNextSibling(c->ast, lhsNode) : -1;
            int32_t extraNode = rhsNode >= 0 ? HOPAstNextSibling(c->ast, rhsNode) : -1;
            int32_t lhsTypeId = -1;
            int32_t rhsTypeId = -1;
            int     lhsStatus;
            int     rhsStatus;
            if (lhsNode >= 0 && rhsNode >= 0 && extraNode < 0) {
                lhsStatus = HOPTCResolveReflectedTypeValueExpr(c, lhsNode, &lhsTypeId);
                if (lhsStatus < 0) {
                    return -1;
                }
                rhsStatus = HOPTCResolveReflectedTypeValueExpr(c, rhsNode, &rhsTypeId);
                if (rhsStatus < 0) {
                    return -1;
                }
                if (lhsStatus == 0 && rhsStatus == 0) {
                    outValue->kind = HOPCTFEValue_BOOL;
                    outValue->i64 = 0;
                    outValue->f64 = 0.0;
                    outValue->b =
                        (((HOPTokenKind)n->op == HOPTok_EQ)
                             ? (lhsTypeId == rhsTypeId)
                             : (lhsTypeId != rhsTypeId))
                            ? 1
                            : 0;
                    outValue->typeTag = 0;
                    outValue->s.bytes = NULL;
                    outValue->s.len = 0;
                    outValue->span.fileBytes = NULL;
                    outValue->span.fileLen = 0;
                    outValue->span.startLine = 0;
                    outValue->span.startColumn = 0;
                    outValue->span.endLine = 0;
                    outValue->span.endColumn = 0;
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        {
            int32_t      lhsNode = HOPAstFirstChild(c->ast, exprNode);
            int32_t      rhsNode = lhsNode >= 0 ? HOPAstNextSibling(c->ast, lhsNode) : -1;
            int32_t      extraNode = rhsNode >= 0 ? HOPAstNextSibling(c->ast, rhsNode) : -1;
            HOPCTFEValue lhsValue;
            HOPCTFEValue rhsValue;
            int          lhsIsConst = 0;
            int          rhsIsConst = 0;
            if (lhsNode < 0 || rhsNode < 0 || extraNode >= 0) {
                return -1;
            }
            if (HOPTCEvalConstExprNode(evalCtx, lhsNode, &lhsValue, &lhsIsConst) != 0) {
                return -1;
            }
            if (!lhsIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (HOPTCEvalConstExprNode(evalCtx, rhsNode, &rhsValue, &rhsIsConst) != 0) {
                return -1;
            }
            if (!rhsIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (!HOPTCConstEvalApplyBinary(c, (HOPTokenKind)n->op, &lhsValue, &rhsValue, outValue))
            {
                HOPTCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            *outIsConst = 1;
            return 0;
        }
    }
    if (kind == HOPAst_UNARY) {
        const HOPAstNode* n = &c->ast->nodes[exprNode];
        int32_t           operandNode = HOPAstFirstChild(c->ast, exprNode);
        int32_t      extraNode = operandNode >= 0 ? HOPAstNextSibling(c->ast, operandNode) : -1;
        HOPCTFEValue operandValue;
        int          operandIsConst = 0;
        if (operandNode < 0 || extraNode >= 0) {
            return -1;
        }
        if (HOPTCEvalConstExprNode(evalCtx, operandNode, &operandValue, &operandIsConst) != 0) {
            return -1;
        }
        if (!operandIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (!HOPTCConstEvalApplyUnary((HOPTokenKind)n->op, &operandValue, outValue)) {
            HOPTCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        *outIsConst = 1;
        return 0;
    }
    if (kind == HOPAst_SIZEOF) {
        return HOPTCConstEvalSizeOf(evalCtx, exprNode, outValue, outIsConst);
    }
    if (kind == HOPAst_COMPOUND_LIT) {
        int locationStatus = HOPTCConstEvalSourceLocationCompound(
            evalCtx, exprNode, 0, outValue, outIsConst);
        if (locationStatus < 0) {
            return -1;
        }
        if (locationStatus > 0) {
            return 0;
        }
    }
    if (kind == HOPAst_INDEX) {
        int indexStatus = HOPTCConstEvalIndexExpr(evalCtx, exprNode, outValue, outIsConst);
        if (indexStatus == 0) {
            return 0;
        }
        if (indexStatus < 0) {
            return -1;
        }
    }
    if (kind == HOPAst_FIELD_EXPR) {
        int pkgFnStatus = HOPTCConstEvalPkgFunctionValueExpr(
            evalCtx, exprNode, outValue, outIsConst);
        if (pkgFnStatus == 0) {
            return 0;
        }
        if (pkgFnStatus < 0) {
            return -1;
        }
    }
    if (kind == HOPAst_CALL) {
        int32_t           calleeNode = HOPAstFirstChild(c->ast, exprNode);
        const HOPAstNode* callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
        int               compilerDiagStatus;
        int               directCallStatus;
        int               lenStatus;
        int               sourceLocationStatus;
        int               reflectStatus;
        lenStatus = HOPTCConstEvalLenCall(evalCtx, exprNode, outValue, outIsConst);
        if (lenStatus == 0) {
            return 0;
        }
        if (lenStatus < 0) {
            return -1;
        }
        compilerDiagStatus = HOPTCConstEvalCompilerDiagCall(
            evalCtx, exprNode, outValue, outIsConst);
        if (compilerDiagStatus == 0) {
            return 0;
        }
        if (compilerDiagStatus < 0) {
            return -1;
        }
        sourceLocationStatus = HOPTCConstEvalSourceLocationOfCall(
            evalCtx, exprNode, outValue, outIsConst);
        if (sourceLocationStatus == 0) {
            return 0;
        }
        if (sourceLocationStatus < 0) {
            return -1;
        }
        if (callee != NULL && callee->kind == HOPAst_IDENT
            && HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof"))
        {
            return HOPTCConstEvalTypeOf(evalCtx, exprNode, outValue, outIsConst);
        }
        reflectStatus = HOPTCConstEvalTypeReflectionCall(evalCtx, exprNode, outValue, outIsConst);
        if (reflectStatus == 0) {
            return 0;
        }
        if (reflectStatus < 0) {
            return -1;
        }
        directCallStatus = HOPTCConstEvalDirectCall(evalCtx, exprNode, outValue, outIsConst);
        if (directCallStatus == 0) {
            return 0;
        }
        if (directCallStatus < 0) {
            return -1;
        }
    }
    if (kind == HOPAst_CAST) {
        return HOPTCConstEvalCast(evalCtx, exprNode, outValue, outIsConst);
    }
    rc = HOPCTFEEvalExprEx(
        c->arena,
        c->ast,
        c->src,
        exprNode,
        HOPTCResolveConstIdent,
        HOPTCResolveConstCall,
        evalCtx,
        HOPTCMirConstMakeTuple,
        evalCtx,
        HOPTCMirConstIndexValue,
        evalCtx,
        HOPTCMirConstAggGetField,
        evalCtx,
        HOPTCMirConstAggAddrField,
        evalCtx,
        outValue,
        outIsConst,
        c->diag);
    if (rc == 0 && !*outIsConst) {
        HOPTCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
    }
    return rc;
}

int HOPTCEvalConstExecExprCb(void* ctx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    return HOPTCEvalConstExprNode((HOPTCConstEvalCtx*)ctx, exprNode, outValue, outIsConst);
}

int HOPTCEvalConstExecResolveTypeCb(void* ctx, int32_t typeNode, int32_t* outTypeId) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    uint8_t            savedAllowConstNumericTypeName;
    if (evalCtx == NULL || evalCtx->tc == NULL || outTypeId == NULL) {
        return -1;
    }
    savedAllowConstNumericTypeName = evalCtx->tc->allowConstNumericTypeName;
    evalCtx->tc->allowConstNumericTypeName = 1;
    if (HOPTCResolveTypeNode(evalCtx->tc, typeNode, outTypeId) != 0) {
        evalCtx->tc->allowConstNumericTypeName = savedAllowConstNumericTypeName;
        return -1;
    }
    evalCtx->tc->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    return 0;
}

int HOPTCEvalConstExecInferValueTypeCb(void* ctx, const HOPCTFEValue* value, int32_t* outTypeId) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*   c;
    if (evalCtx == NULL || value == NULL || outTypeId == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (value->kind != HOPCTFEValue_TYPE && value->typeTag > 0
        && value->typeTag <= (uint64_t)INT32_MAX && (uint32_t)value->typeTag < c->typeLen)
    {
        *outTypeId = (int32_t)value->typeTag;
        return 0;
    }
    switch (value->kind) {
        case HOPCTFEValue_INT:   *outTypeId = c->typeUntypedInt; return *outTypeId >= 0 ? 0 : -1;
        case HOPCTFEValue_FLOAT: *outTypeId = c->typeUntypedFloat; return *outTypeId >= 0 ? 0 : -1;
        case HOPCTFEValue_BOOL:  *outTypeId = c->typeBool; return *outTypeId >= 0 ? 0 : -1;
        case HOPCTFEValue_STRING:
            *outTypeId = HOPTCGetStrRefType(c, 0, 0);
            return *outTypeId >= 0 ? 0 : -1;
        case HOPCTFEValue_TYPE: *outTypeId = c->typeType; return *outTypeId >= 0 ? 0 : -1;
        case HOPCTFEValue_SPAN:
            if (c->typeSourceLocation < 0) {
                c->typeSourceLocation = HOPTCFindSourceLocationType(c);
            }
            *outTypeId = c->typeSourceLocation;
            return *outTypeId >= 0 ? 0 : -1;
        case HOPCTFEValue_NULL: *outTypeId = c->typeNull; return *outTypeId >= 0 ? 0 : -1;
        case HOPCTFEValue_OPTIONAL:
            if (value->typeTag > 0 && value->typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)value->typeTag < c->typeLen)
            {
                *outTypeId = (int32_t)value->typeTag;
                return 0;
            }
            return -1;
        default: return -1;
    }
}

int HOPTCEvalConstExecInferExprTypeCb(void* ctx, int32_t exprNode, int32_t* outTypeId) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    if (evalCtx == NULL || evalCtx->tc == NULL || outTypeId == NULL) {
        return -1;
    }
    return HOPTCTypeExpr(evalCtx->tc, exprNode, outTypeId);
}

int HOPTCEvalConstExecIsOptionalTypeCb(
    void* ctx, int32_t typeId, int32_t* outPayloadTypeId, int* outIsOptional) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*   c;
    int32_t            baseTypeId;
    if (evalCtx == NULL || evalCtx->tc == NULL || outIsOptional == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    baseTypeId = HOPTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return -1;
    }
    *outIsOptional = c->types[baseTypeId].kind == HOPTCType_OPTIONAL;
    if (outPayloadTypeId != NULL) {
        *outPayloadTypeId = *outIsOptional ? c->types[baseTypeId].baseType : -1;
    }
    return 0;
}

static int HOPTCMirConstResolveTypeRefTypeId(
    HOPTCConstEvalCtx* evalCtx, const HOPMirTypeRef* typeRef, int32_t* outTypeId) {
    HOPTypeCheckCtx* c;
    uint8_t          savedAllowConstNumericTypeName;
    uint8_t          savedAllowAnytypeParamType;
    if (outTypeId != NULL) {
        *outTypeId = -1;
    }
    if (evalCtx == NULL || typeRef == NULL || outTypeId == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || typeRef->astNode > INT32_MAX || typeRef->astNode >= c->ast->len) {
        return -1;
    }
    savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
    savedAllowAnytypeParamType = c->allowAnytypeParamType;
    c->allowConstNumericTypeName = 1;
    c->allowAnytypeParamType = 1;
    if (HOPTCResolveTypeNode(c, (int32_t)typeRef->astNode, outTypeId) != 0) {
        c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
        c->allowAnytypeParamType = savedAllowAnytypeParamType;
        return -1;
    }
    c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    c->allowAnytypeParamType = savedAllowAnytypeParamType;
    return 0;
}

typedef struct {
    uint32_t     len;
    uint32_t     _reserved;
    HOPCTFEValue elems[];
} HOPTCMirConstTuple;

typedef struct {
    int32_t      typeId;
    uint32_t     fieldCount;
    HOPCTFEValue fields[];
} HOPTCMirConstAggregate;

enum {
    HOP_TC_MIR_CONST_ITER_KIND_SEQUENCE = 1u,
    HOP_TC_MIR_CONST_ITER_KIND_PROTOCOL = 2u,
};

typedef struct {
    uint32_t     index;
    uint16_t     flags;
    uint8_t      kind;
    uint8_t      _reserved;
    int32_t      iterFnIndex;
    int32_t      nextFnIndex;
    uint8_t      usePair;
    uint8_t      _reserved2[3];
    HOPCTFEValue sourceValue;
    HOPCTFEValue iteratorValue;
    HOPCTFEValue currentValue;
} HOPTCMirConstIter;

static const HOPTCMirConstTuple* _Nullable HOPTCMirConstTupleFromValue(const HOPCTFEValue* value) {
    if (value == NULL || value->kind != HOPCTFEValue_ARRAY || value->typeTag != HOP_TC_MIR_TUPLE_TAG
        || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (const HOPTCMirConstTuple*)value->s.bytes;
}

static int HOPTCConstEvalGetConcreteCallArgPackType(
    HOPTCConstEvalCtx* evalCtx, uint32_t callArgIndex, int32_t* outPackType) {
    HOPTypeCheckCtx*          c;
    const HOPTCCallArgInfo*   callArgs;
    HOPCTFEValue              argValue;
    const HOPCTFEValue*       sourceValue;
    const HOPTCMirConstTuple* tuple;
    int32_t                   elemTypes[HOPTC_MAX_CALL_ARGS];
    uint32_t                  i;
    int                       argIsConst = 0;
    if (outPackType != NULL) {
        *outPackType = -1;
    }
    if (evalCtx == NULL || outPackType == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    callArgs = (const HOPTCCallArgInfo*)evalCtx->callArgs;
    if (c == NULL || callArgs == NULL || callArgIndex >= evalCtx->callArgCount
        || callArgs[callArgIndex].exprNode < 0
        || (uint32_t)callArgs[callArgIndex].exprNode >= c->ast->len)
    {
        return 1;
    }
    if (HOPTCEvalConstExprNode(evalCtx, callArgs[callArgIndex].exprNode, &argValue, &argIsConst)
        != 0)
    {
        return -1;
    }
    if (!argIsConst) {
        return 1;
    }
    sourceValue = &argValue;
    if (sourceValue->kind == HOPCTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        sourceValue = (const HOPCTFEValue*)sourceValue->s.bytes;
    }
    tuple = HOPTCMirConstTupleFromValue(sourceValue);
    if (tuple == NULL || tuple->len > HOPTC_MAX_CALL_ARGS) {
        return 1;
    }
    for (i = 0; i < tuple->len; i++) {
        if (HOPTCEvalConstExecInferValueTypeCb(evalCtx, &tuple->elems[i], &elemTypes[i]) != 0) {
            return 1;
        }
        if ((elemTypes[i] == c->typeUntypedInt || elemTypes[i] == c->typeUntypedFloat)
            && HOPTCConcretizeInferredType(c, elemTypes[i], &elemTypes[i]) != 0)
        {
            return -1;
        }
    }
    *outPackType = HOPTCInternPackType(c, elemTypes, tuple->len, 0, 0);
    return *outPackType >= 0 ? 0 : -1;
}

static HOPTCMirConstIter* _Nullable HOPTCMirConstIterFromValue(const HOPCTFEValue* value) {
    const HOPCTFEValue* target = value;
    if (value == NULL) {
        return NULL;
    }
    if (value->kind == HOPCTFEValue_REFERENCE && value->s.bytes != NULL) {
        target = (const HOPCTFEValue*)value->s.bytes;
    }
    if (target == NULL || target->kind != HOPCTFEValue_SPAN
        || target->typeTag != HOP_TC_MIR_ITER_TAG || target->s.bytes == NULL)
    {
        return NULL;
    }
    return (HOPTCMirConstIter*)target->s.bytes;
}

static const HOPTCMirConstAggregate* _Nullable HOPTCMirConstAggregateFromValue(
    const HOPCTFEValue* value) {
    const HOPCTFEValue* target = value;
    if (value == NULL) {
        return NULL;
    }
    if (value->kind == HOPCTFEValue_REFERENCE && value->s.bytes != NULL) {
        target = (const HOPCTFEValue*)value->s.bytes;
    }
    if (target == NULL || target->kind != HOPCTFEValue_AGGREGATE || target->s.bytes == NULL) {
        return NULL;
    }
    return (const HOPTCMirConstAggregate*)target->s.bytes;
}

static HOPCTFEValue* _Nullable HOPTCMirConstAggregateFieldValuePtr(
    const HOPTCMirConstAggregate* agg, uint32_t fieldIndex) {
    if (agg == NULL || fieldIndex >= agg->fieldCount) {
        return NULL;
    }
    return (HOPCTFEValue*)&agg->fields[fieldIndex];
}

static const HOPCTFEValue* HOPTCMirConstValueTargetOrSelf(const HOPCTFEValue* value) {
    if (value != NULL && value->kind == HOPCTFEValue_REFERENCE && value->s.bytes != NULL) {
        return (const HOPCTFEValue*)value->s.bytes;
    }
    return value;
}

static void HOPTCMirConstSetReference(HOPCTFEValue* outValue, HOPCTFEValue* target) {
    if (outValue == NULL) {
        return;
    }
    HOPTCConstEvalValueInvalid(outValue);
    if (target == NULL) {
        return;
    }
    outValue->kind = HOPCTFEValue_REFERENCE;
    outValue->s.bytes = (const uint8_t*)target;
    outValue->s.len = 0;
}

static int HOPTCMirConstOptionalPayload(
    const HOPCTFEValue* value, const HOPCTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (value == NULL || value->kind != HOPCTFEValue_OPTIONAL) {
        return 0;
    }
    if (value->b == 0u || value->s.bytes == NULL) {
        return 1;
    }
    if (outPayload != NULL) {
        *outPayload = (const HOPCTFEValue*)value->s.bytes;
    }
    return 1;
}

static void HOPTCMirConstAdaptForInValueBinding(
    const HOPCTFEValue* inValue, int valueRef, HOPCTFEValue* outValue) {
    const HOPCTFEValue* target;
    if (outValue == NULL) {
        return;
    }
    HOPTCConstEvalValueInvalid(outValue);
    if (inValue == NULL) {
        return;
    }
    if (valueRef) {
        *outValue = *inValue;
        return;
    }
    target = HOPTCMirConstValueTargetOrSelf(inValue);
    *outValue = target != NULL ? *target : *inValue;
}

static int HOPTCMirConstFindExecBindingTypeId(
    HOPTCConstEvalCtx* evalCtx, int32_t sourceNode, int32_t* outTypeId) {
    HOPTypeCheckCtx*  c;
    const HOPAstNode* node;
    HOPCTFEExecCtx*   execCtx;
    HOPCTFEExecEnv*   env;
    uint8_t           savedAllowConstNumericTypeName;
    if (outTypeId != NULL) {
        *outTypeId = -1;
    }
    if (evalCtx == NULL || outTypeId == NULL || evalCtx->tc == NULL || evalCtx->execCtx == NULL) {
        return 0;
    }
    c = evalCtx->tc;
    execCtx = evalCtx->execCtx;
    if (sourceNode < 0 || (uint32_t)sourceNode >= c->ast->len) {
        return 0;
    }
    node = &c->ast->nodes[sourceNode];
    if (node->kind != HOPAst_IDENT) {
        return 0;
    }
    for (env = execCtx->env; env != NULL; env = env->parent) {
        uint32_t i;
        for (i = 0; i < env->bindingLen; i++) {
            HOPCTFEExecBinding* binding = &env->bindings[i];
            if (binding->nameStart != node->dataStart || binding->nameEnd != node->dataEnd) {
                continue;
            }
            if (binding->typeId >= 0) {
                *outTypeId = binding->typeId;
                return 1;
            }
            if (binding->typeNode < 0) {
                return 0;
            }
            savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
            c->allowConstNumericTypeName = 1;
            if (HOPTCResolveTypeNode(c, binding->typeNode, outTypeId) != 0) {
                c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
                return -1;
            }
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            return 1;
        }
    }
    return 0;
}

int HOPTCMirConstMakeTuple(
    void* _Nullable ctx,
    const HOPCTFEValue* elems,
    uint32_t            elemCount,
    uint32_t            typeNodeHint,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx*  evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*    c;
    HOPTCMirConstTuple* tuple;
    size_t              bytes;
    (void)typeNodeHint;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        HOPTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    bytes = sizeof(*tuple) + sizeof(HOPCTFEValue) * (size_t)elemCount;
    tuple = (HOPTCMirConstTuple*)HOPArenaAlloc(
        c->arena, (uint32_t)bytes, (uint32_t)_Alignof(HOPTCMirConstTuple));
    if (tuple == NULL) {
        return -1;
    }
    tuple->len = elemCount;
    tuple->_reserved = 0u;
    if (elemCount != 0u && elems != NULL) {
        memcpy(tuple->elems, elems, sizeof(HOPCTFEValue) * elemCount);
    }
    outValue->kind = HOPCTFEValue_ARRAY;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0u;
    outValue->typeTag = HOP_TC_MIR_TUPLE_TAG;
    outValue->s.bytes = (const uint8_t*)tuple;
    outValue->s.len = elemCount;
    outValue->span = (HOPCTFESpan){ 0 };
    *outIsConst = 1;
    return 0;
}

int HOPTCMirConstIndexValue(
    void* _Nullable ctx,
    const HOPCTFEValue* base,
    const HOPCTFEValue* index,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    const HOPTCMirConstTuple* tuple;
    int64_t                   indexValue = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    tuple = HOPTCMirConstTupleFromValue(base);
    if (tuple == NULL || HOPCTFEValueToInt64(index, &indexValue) != 0 || indexValue < 0
        || (uint64_t)indexValue >= (uint64_t)tuple->len)
    {
        HOPTCConstEvalCtx*      evalCtx = (HOPTCConstEvalCtx*)ctx;
        HOPTypeCheckCtx*        c;
        const HOPTCCallBinding* binding;
        const HOPTCCallArgInfo* callArgs;
        int32_t                 typeId = -1;
        int32_t                 baseTypeId;
        uint32_t                paramIndex;
        uint32_t                ordinal = 0;
        uint32_t                callArgIndex = UINT32_MAX;
        uint32_t                i;
        if (evalCtx == NULL || base->kind != HOPCTFEValue_TYPE) {
            return 0;
        }
        c = evalCtx->tc;
        binding = (const HOPTCCallBinding*)evalCtx->callBinding;
        callArgs = (const HOPTCCallArgInfo*)evalCtx->callArgs;
        if (c == NULL || binding == NULL || callArgs == NULL || evalCtx->callArgCount == 0
            || evalCtx->callPackParamNameStart >= evalCtx->callPackParamNameEnd
            || !binding->isVariadic || binding->spreadArgIndex != UINT32_MAX
            || HOPTCDecodeTypeTag(c, base->typeTag, &typeId) != 0)
        {
            return 0;
        }
        baseTypeId = HOPTCResolveAliasBaseType(c, typeId);
        if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
            || c->types[baseTypeId].kind != HOPTCType_PACK)
        {
            return 0;
        }
        paramIndex = binding->fixedCount;
        for (i = 0; i < evalCtx->callArgCount; i++) {
            if (binding->argParamIndices[i] != (int32_t)paramIndex) {
                continue;
            }
            if ((int64_t)ordinal == indexValue) {
                callArgIndex = i;
                break;
            }
            ordinal++;
        }
        if (callArgIndex == UINT32_MAX) {
            return 0;
        }
        if (HOPTCEvalConstExprNode(evalCtx, callArgs[callArgIndex].exprNode, outValue, outIsConst)
            != 0)
        {
            return -1;
        }
        if (*outIsConst) {
            int32_t elemType = -1;
            if (HOPTCConstEvalGetConcreteCallArgType(evalCtx, callArgIndex, &elemType) == 0) {
                switch (outValue->kind) {
                    case HOPCTFEValue_INT:
                    case HOPCTFEValue_FLOAT:
                    case HOPCTFEValue_BOOL:
                    case HOPCTFEValue_STRING:
                        outValue->typeTag = (uint64_t)(uint32_t)elemType;
                        break;
                    default: break;
                }
            }
        }
        return 0;
    }
    *outValue = tuple->elems[(uint32_t)indexValue];
    *outIsConst = 1;
    return 0;
}

int HOPTCMirConstSequenceLen(
    void* _Nullable ctx,
    const HOPCTFEValue* base,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx*        evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*          c;
    const HOPCTFEValue*       value = base;
    const HOPTCMirConstTuple* tuple;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx != NULL ? evalCtx->tc : NULL;
    if (base->kind == HOPCTFEValue_REFERENCE && base->s.bytes != NULL) {
        value = (const HOPCTFEValue*)base->s.bytes;
    }
    if (value->kind == HOPCTFEValue_STRING) {
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = (int64_t)value->s.len;
        *outIsConst = 1;
        return 0;
    }
    tuple = HOPTCMirConstTupleFromValue(value);
    if (tuple != NULL) {
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = (int64_t)tuple->len;
        *outIsConst = 1;
        return 0;
    }
    if (c != NULL && value->kind == HOPCTFEValue_TYPE) {
        int32_t typeId = -1;
        int32_t baseTypeId;
        if (HOPTCDecodeTypeTag(c, value->typeTag, &typeId) == 0) {
            baseTypeId = HOPTCResolveAliasBaseType(c, typeId);
            if (baseTypeId >= 0 && (uint32_t)baseTypeId < c->typeLen) {
                const HOPTCType* t = &c->types[baseTypeId];
                if (t->kind == HOPTCType_PACK) {
                    outValue->kind = HOPCTFEValue_INT;
                    outValue->i64 = (int64_t)t->fieldCount;
                    *outIsConst = 1;
                    return 0;
                }
                if (t->kind == HOPTCType_ARRAY) {
                    outValue->kind = HOPCTFEValue_INT;
                    outValue->i64 = (int64_t)t->arrayLen;
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
    }
    return 0;
}

int HOPTCMirConstIterInit(
    void* _Nullable ctx,
    uint32_t            sourceNode,
    const HOPCTFEValue* source,
    uint16_t            flags,
    HOPCTFEValue*       outIter,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx*        evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*          c;
    const HOPCTFEValue*       sourceValue = source;
    const HOPTCMirConstTuple* tuple;
    HOPTCMirConstIter*        iter;
    HOPCTFEValue*             target;
    int32_t                   sourceType = -1;
    int32_t                   iterType = -1;
    int32_t                   iterPtrType = -1;
    int32_t                   iterFn = -1;
    int32_t                   nextValueFn = -1;
    int32_t                   nextKeyFn = -1;
    int32_t                   nextPairFn = -1;
    int32_t                   valueType = -1;
    int32_t                   keyType = -1;
    const HOPAstNode*         sourceAstNode = NULL;
    int                       hasKey;
    int                       valueDiscard;
    int                       valueRef;
    int                       rc;
    int                       iterIsConst = 0;
    HOPCTFEValue              iterValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || source == NULL || outIter == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    c = evalCtx->tc;
    if ((flags & HOPMirIterFlag_KEY_REF) != 0u || (flags & HOPMirIterFlag_VALUE_REF) != 0u) {
        if ((flags & HOPMirIterFlag_VALUE_REF) == 0u) {
            return 0;
        }
    }
    if (source->kind == HOPCTFEValue_REFERENCE && source->s.bytes != NULL) {
        sourceValue = (const HOPCTFEValue*)source->s.bytes;
    }
    tuple = HOPTCMirConstTupleFromValue(sourceValue);
    iter = (HOPTCMirConstIter*)HOPArenaAlloc(
        c->arena, sizeof(*iter), (uint32_t)_Alignof(HOPTCMirConstIter));
    target = (HOPCTFEValue*)HOPArenaAlloc(
        c->arena, sizeof(*target), (uint32_t)_Alignof(HOPCTFEValue));
    if (iter == NULL || target == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->flags = flags;
    iter->sourceValue = *source;
    iter->iterFnIndex = -1;
    iter->nextFnIndex = -1;
    hasKey = (flags & HOPMirIterFlag_HAS_KEY) != 0u;
    valueDiscard = (flags & HOPMirIterFlag_VALUE_DISCARD) != 0u;
    valueRef = (flags & HOPMirIterFlag_VALUE_REF) != 0u;
    if (sourceValue->kind == HOPCTFEValue_STRING || tuple != NULL) {
        iter->kind = HOP_TC_MIR_CONST_ITER_KIND_SEQUENCE;
    } else {
        if (sourceNode < c->ast->len) {
            sourceAstNode = &c->ast->nodes[sourceNode];
        }
        rc = HOPTCMirConstFindExecBindingTypeId(evalCtx, (int32_t)sourceNode, &sourceType);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0 && HOPTCTypeExpr(c, (int32_t)sourceNode, &sourceType) != 0) {
            return -1;
        }
        rc = HOPTCResolveForInIterator(c, (int32_t)sourceNode, sourceType, &iterFn, &iterType);
        if (rc != 0) {
            return 0;
        }
        iterPtrType = HOPTCInternPtrType(
            c,
            iterType,
            sourceAstNode != NULL ? sourceAstNode->start : 0u,
            sourceAstNode != NULL ? sourceAstNode->end : 0u);
        if (iterPtrType < 0) {
            return -1;
        }
        if (hasKey && valueDiscard) {
            rc = HOPTCResolveForInNextKey(c, iterPtrType, &keyType, &nextKeyFn);
            if (rc == 1 || rc == 2) {
                rc = HOPTCResolveForInNextKeyAndValue(
                    c, iterPtrType, HOPTCForInValueMode_ANY, &keyType, &valueType, &nextPairFn);
            }
            if (rc != 0) {
                return 0;
            }
            iter->usePair = nextPairFn >= 0 ? 1u : 0u;
            iter->nextFnIndex = nextPairFn >= 0 ? nextPairFn : nextKeyFn;
        } else if (hasKey) {
            rc = HOPTCResolveForInNextKeyAndValue(
                c,
                iterPtrType,
                valueDiscard ? HOPTCForInValueMode_ANY
                             : (valueRef ? HOPTCForInValueMode_REF : HOPTCForInValueMode_VALUE),
                &keyType,
                &valueType,
                &nextPairFn);
            if (rc != 0) {
                return 0;
            }
            iter->usePair = 1u;
            iter->nextFnIndex = nextPairFn;
        } else {
            rc = HOPTCResolveForInNextValue(
                c,
                iterPtrType,
                valueDiscard ? HOPTCForInValueMode_ANY
                             : (valueRef ? HOPTCForInValueMode_REF : HOPTCForInValueMode_VALUE),
                &valueType,
                &nextValueFn);
            if (rc == 1 || rc == 2) {
                rc = HOPTCResolveForInNextKeyAndValue(
                    c,
                    iterPtrType,
                    valueDiscard ? HOPTCForInValueMode_ANY
                                 : (valueRef ? HOPTCForInValueMode_REF : HOPTCForInValueMode_VALUE),
                    &keyType,
                    &valueType,
                    &nextPairFn);
            }
            if (rc != 0) {
                return 0;
            }
            iter->usePair = nextPairFn >= 0 ? 1u : 0u;
            iter->nextFnIndex = nextPairFn >= 0 ? nextPairFn : nextValueFn;
        }
        if (iter->nextFnIndex < 0) {
            return 0;
        }
        HOPTCConstEvalValueInvalid(&iterValue);
        if (HOPTCInvokeConstFunctionByIndex(
                evalCtx,
                c->funcs[iterFn].nameStart,
                c->funcs[iterFn].nameEnd,
                iterFn,
                source,
                1u,
                NULL,
                0u,
                NULL,
                0u,
                0u,
                &iterValue,
                &iterIsConst)
            != 0)
        {
            return -1;
        }
        if (!iterIsConst) {
            return 0;
        }
        iter->kind = HOP_TC_MIR_CONST_ITER_KIND_PROTOCOL;
        iter->iterFnIndex = iterFn;
        iter->iteratorValue = iterValue;
    }
    target->kind = HOPCTFEValue_SPAN;
    target->typeTag = HOP_TC_MIR_ITER_TAG;
    target->s.bytes = (const uint8_t*)iter;
    target->s.len = 0;
    target->span = (HOPCTFESpan){ 0 };
    outIter->kind = HOPCTFEValue_REFERENCE;
    outIter->s.bytes = (const uint8_t*)target;
    outIter->s.len = 0;
    outIter->typeTag = 0;
    outIter->span = (HOPCTFESpan){ 0 };
    *outIsConst = 1;
    return 0;
}

int HOPTCMirConstIterNext(
    void* _Nullable ctx,
    const HOPCTFEValue* iterValue,
    uint16_t            flags,
    int*                outHasItem,
    HOPCTFEValue*       outKey,
    int*                outKeyIsConst,
    HOPCTFEValue*       outValue,
    int*                outValueIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx*        evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*          c;
    HOPTCMirConstIter*        iter;
    const HOPCTFEValue*       sourceValue;
    const HOPTCMirConstTuple* tuple;
    const HOPCTFEValue*       payload = NULL;
    HOPCTFEValue              callResult;
    HOPCTFEValue              iterRef;
    const HOPCTFEValue*       pairValue;
    const HOPTCMirConstTuple* pairTuple;
    int                       isConst = 0;
    (void)diag;
    if (outHasItem != NULL) {
        *outHasItem = 0;
    }
    if (outKeyIsConst != NULL) {
        *outKeyIsConst = 0;
    }
    if (outValueIsConst != NULL) {
        *outValueIsConst = 0;
    }
    if (evalCtx == NULL || iterValue == NULL || outHasItem == NULL || outKey == NULL
        || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    c = evalCtx->tc;
    if ((flags & HOPMirIterFlag_KEY_REF) != 0u) {
        return 0;
    }
    iter = HOPTCMirConstIterFromValue(iterValue);
    if (iter == NULL) {
        return 0;
    }
    if (iter->kind == HOP_TC_MIR_CONST_ITER_KIND_PROTOCOL) {
        if (c == NULL || iter->nextFnIndex < 0 || (uint32_t)iter->nextFnIndex >= c->funcLen) {
            return 0;
        }
        HOPTCConstEvalValueInvalid(&callResult);
        HOPTCMirConstSetReference(&iterRef, &iter->iteratorValue);
        if (HOPTCInvokeConstFunctionByIndex(
                evalCtx,
                c->funcs[iter->nextFnIndex].nameStart,
                c->funcs[iter->nextFnIndex].nameEnd,
                iter->nextFnIndex,
                &iterRef,
                1u,
                NULL,
                0u,
                NULL,
                0u,
                0u,
                &callResult,
                &isConst)
            != 0)
        {
            return -1;
        }
        if (!isConst) {
            return 0;
        }
        if (callResult.kind == HOPCTFEValue_NULL) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if (callResult.kind == HOPCTFEValue_OPTIONAL) {
            if (!HOPTCMirConstOptionalPayload(&callResult, &payload)) {
                return 0;
            }
            if (callResult.b == 0u || payload == NULL) {
                *outKeyIsConst = 1;
                *outValueIsConst = 1;
                return 0;
            }
        } else {
            payload = &callResult;
        }
        if (iter->usePair) {
            pairValue = HOPTCMirConstValueTargetOrSelf(payload);
            pairTuple = HOPTCMirConstTupleFromValue(pairValue);
            if (pairTuple == NULL || pairTuple->len != 2u) {
                return 0;
            }
            if ((flags & HOPMirIterFlag_HAS_KEY) != 0u) {
                *outKey = pairTuple->elems[0];
                *outKeyIsConst = 1;
            } else {
                *outKeyIsConst = 1;
            }
            if ((flags & HOPMirIterFlag_VALUE_DISCARD) == 0u) {
                HOPTCMirConstAdaptForInValueBinding(
                    &pairTuple->elems[1], (flags & HOPMirIterFlag_VALUE_REF) != 0u, outValue);
                *outValueIsConst = 1;
            } else {
                *outValueIsConst = 1;
            }
        } else if ((flags & HOPMirIterFlag_HAS_KEY) != 0u) {
            *outKey = *payload;
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
        } else {
            HOPTCMirConstAdaptForInValueBinding(
                payload, (flags & HOPMirIterFlag_VALUE_REF) != 0u, outValue);
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        return 0;
    }
    sourceValue = &iter->sourceValue;
    if (sourceValue->kind == HOPCTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        sourceValue = (const HOPCTFEValue*)sourceValue->s.bytes;
    }
    tuple = HOPTCMirConstTupleFromValue(sourceValue);
    if (sourceValue->kind == HOPCTFEValue_STRING) {
        if (iter->index >= sourceValue->s.len) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if ((flags & HOPMirIterFlag_HAS_KEY) != 0u) {
            outKey->kind = HOPCTFEValue_INT;
            outKey->i64 = (int64_t)iter->index;
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if ((flags & HOPMirIterFlag_VALUE_DISCARD) == 0u) {
            iter->currentValue.kind = HOPCTFEValue_INT;
            iter->currentValue.i64 = (int64_t)sourceValue->s.bytes[iter->index];
            if ((flags & HOPMirIterFlag_VALUE_REF) != 0u) {
                HOPTCMirConstSetReference(outValue, &iter->currentValue);
            } else {
                *outValue = iter->currentValue;
            }
            *outValueIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        iter->index++;
        return 0;
    }
    if (tuple != NULL) {
        if (iter->index >= tuple->len) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if ((flags & HOPMirIterFlag_HAS_KEY) != 0u) {
            outKey->kind = HOPCTFEValue_INT;
            outKey->i64 = (int64_t)iter->index;
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if ((flags & HOPMirIterFlag_VALUE_DISCARD) == 0u) {
            if ((flags & HOPMirIterFlag_VALUE_REF) != 0u) {
                HOPTCMirConstSetReference(outValue, (HOPCTFEValue*)&tuple->elems[iter->index]);
            } else {
                *outValue = tuple->elems[iter->index];
            }
            *outValueIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        iter->index++;
        return 0;
    }
    return 0;
}

int HOPTCEvalConstForInIndexCb(
    void* _Nullable ctx,
    HOPCTFEExecCtx*     execCtx,
    const HOPCTFEValue* sourceValue,
    uint32_t            index,
    int                 byRef,
    HOPCTFEValue*       outValue,
    int*                outIsConst) {
    const HOPCTFEValue*       value = sourceValue;
    const HOPTCMirConstTuple* tuple;
    (void)ctx;
    (void)execCtx;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (sourceValue == NULL || outValue == NULL || outIsConst == NULL || byRef) {
        return 0;
    }
    if (sourceValue->kind == HOPCTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        value = (const HOPCTFEValue*)sourceValue->s.bytes;
    }
    tuple = HOPTCMirConstTupleFromValue(value);
    if (tuple == NULL || index >= tuple->len) {
        return 0;
    }
    *outValue = tuple->elems[index];
    *outIsConst = 1;
    return 0;
}

static int HOPTCMirConstResolveAggregateType(
    HOPTypeCheckCtx* c, int32_t typeId, int32_t* outBaseTypeId) {
    int32_t          baseTypeId = -1;
    const HOPTCType* t;
    if (outBaseTypeId != NULL) {
        *outBaseTypeId = -1;
    }
    if (c == NULL) {
        return 0;
    }
    baseTypeId = HOPTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    t = &c->types[baseTypeId];
    if (t->kind == HOPTCType_NAMED && t->fieldCount == 0u) {
        int32_t  namedIndex = -1;
        uint32_t i;
        for (i = 0; i < c->namedTypeLen; i++) {
            if (c->namedTypes[i].typeId == baseTypeId) {
                namedIndex = (int32_t)i;
                break;
            }
        }
        if (namedIndex >= 0 && HOPTCResolveNamedTypeFields(c, (uint32_t)namedIndex) != 0) {
            return 0;
        }
        t = &c->types[baseTypeId];
    }
    if (t->kind != HOPTCType_NAMED && t->kind != HOPTCType_ANON_STRUCT) {
        return 0;
    }
    if (outBaseTypeId != NULL) {
        *outBaseTypeId = baseTypeId;
    }
    return 1;
}

static int HOPTCMirConstAggregateLookupFieldIndex(
    HOPTypeCheckCtx* c,
    int32_t          typeId,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    uint32_t*        outFieldIndex) {
    int32_t  baseTypeId = -1;
    int32_t  fieldType = -1;
    uint32_t absFieldIndex = UINT32_MAX;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (!HOPTCMirConstResolveAggregateType(c, typeId, &baseTypeId)) {
        return 0;
    }
    if (HOPTCFieldLookup(c, baseTypeId, nameStart, nameEnd, &fieldType, &absFieldIndex) != 0) {
        return 0;
    }
    if (absFieldIndex < c->types[baseTypeId].fieldStart) {
        return 0;
    }
    absFieldIndex -= c->types[baseTypeId].fieldStart;
    if (absFieldIndex >= c->types[baseTypeId].fieldCount) {
        return 0;
    }
    if (outFieldIndex != NULL) {
        *outFieldIndex = absFieldIndex;
    }
    return 1;
}

static int HOPTCMirConstZeroInitTypeId(
    HOPTCConstEvalCtx* evalCtx, int32_t typeId, HOPCTFEValue* outValue, int* outIsConst);

static int HOPTCMirConstMakeAggregateValue(
    HOPTCConstEvalCtx* evalCtx, int32_t typeId, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*        c;
    int32_t                 baseTypeId = -1;
    uint32_t                fieldCount = 0;
    uint32_t                i;
    size_t                  bytes;
    HOPTCMirConstAggregate* agg;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        HOPTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (!HOPTCMirConstResolveAggregateType(c, typeId, &baseTypeId)) {
        return 0;
    }
    fieldCount = c->types[baseTypeId].fieldCount;
    bytes = sizeof(*agg) + sizeof(HOPCTFEValue) * (size_t)fieldCount;
    agg = (HOPTCMirConstAggregate*)HOPArenaAlloc(
        c->arena, (uint32_t)bytes, (uint32_t)_Alignof(HOPTCMirConstAggregate));
    if (agg == NULL) {
        return -1;
    }
    agg->typeId = baseTypeId;
    agg->fieldCount = fieldCount;
    memset(agg->fields, 0, sizeof(HOPCTFEValue) * fieldCount);
    for (i = 0; i < fieldCount; i++) {
        uint32_t fieldIndex = c->types[baseTypeId].fieldStart + i;
        if (fieldIndex >= c->fieldLen
            || HOPTCMirConstZeroInitTypeId(
                   evalCtx, c->fields[fieldIndex].typeId, &agg->fields[i], outIsConst)
                   != 0)
        {
            return -1;
        }
        if (!*outIsConst) {
            return 0;
        }
    }
    outValue->kind = HOPCTFEValue_AGGREGATE;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    outValue->s.bytes = (const uint8_t*)agg;
    outValue->s.len = fieldCount;
    *outIsConst = 1;
    return 0;
}

int HOPTCMirConstAggGetField(
    void* _Nullable ctx,
    const HOPCTFEValue* base,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx*            evalCtx = (HOPTCConstEvalCtx*)ctx;
    const HOPTCMirConstAggregate* agg;
    uint32_t                      fieldIndex = UINT32_MAX;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        HOPTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (base != NULL && base->kind == HOPCTFEValue_SPAN
        && base->typeTag == HOP_TC_MIR_IMPORT_ALIAS_TAG)
    {
        HOPTypeCheckCtx* c = evalCtx->tc;
        const uint8_t*   srcBytes = c != NULL ? (const uint8_t*)c->src.ptr : NULL;
        const uint8_t*   aliasBytes = base->span.fileBytes;
        uint32_t         aliasLen = base->span.fileLen;
        uint32_t         aliasStart;
        uint32_t         aliasEnd;
        int32_t          fnIndex;
        if (c == NULL || srcBytes == NULL || aliasBytes == NULL || aliasLen == 0u
            || aliasBytes < srcBytes || (uint64_t)(aliasBytes - srcBytes) > UINT32_MAX
            || (uint64_t)(aliasBytes - srcBytes) + aliasLen > c->src.len)
        {
            return 0;
        }
        aliasStart = (uint32_t)(aliasBytes - srcBytes);
        aliasEnd = aliasStart + aliasLen;
        fnIndex = HOPTCFindPkgQualifiedFunctionValueIndex(
            c, aliasStart, aliasEnd, nameStart, nameEnd);
        if (fnIndex < 0) {
            return 0;
        }
        HOPMirValueSetFunctionRef(outValue, (uint32_t)fnIndex);
        *outIsConst = 1;
        return 0;
    }
    agg = HOPTCMirConstAggregateFromValue(base);
    if (agg == NULL
        || !HOPTCMirConstAggregateLookupFieldIndex(
            evalCtx->tc, agg->typeId, nameStart, nameEnd, &fieldIndex))
    {
        return 0;
    }
    {
        HOPCTFEValue* fieldValue = HOPTCMirConstAggregateFieldValuePtr(agg, fieldIndex);
        if (fieldValue == NULL) {
            return 0;
        }
        *outValue = *fieldValue;
    }
    *outIsConst = 1;
    return 0;
}

int HOPTCMirConstAggAddrField(
    void* _Nullable ctx,
    const HOPCTFEValue* base,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx*            evalCtx = (HOPTCConstEvalCtx*)ctx;
    const HOPTCMirConstAggregate* agg;
    uint32_t                      fieldIndex = UINT32_MAX;
    HOPCTFEValue*                 fieldValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        HOPTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    agg = HOPTCMirConstAggregateFromValue(base);
    if (agg == NULL
        || !HOPTCMirConstAggregateLookupFieldIndex(
            evalCtx->tc, agg->typeId, nameStart, nameEnd, &fieldIndex))
    {
        return 0;
    }
    fieldValue = HOPTCMirConstAggregateFieldValuePtr(agg, fieldIndex);
    if (fieldValue == NULL) {
        return 0;
    }
    HOPTCMirConstSetReference(outValue, fieldValue);
    *outIsConst = 1;
    return 0;
}

static int HOPTCMirConstZeroInitTypeId(
    HOPTCConstEvalCtx* evalCtx, int32_t typeId, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx* c;
    int32_t          baseTypeId;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        HOPTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    baseTypeId = HOPTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    if (HOPTCIsIntegerType(c, baseTypeId)) {
        outValue->kind = HOPCTFEValue_INT;
        outValue->i64 = 0;
        *outIsConst = 1;
        return 0;
    }
    if (HOPTCIsFloatType(c, baseTypeId)) {
        outValue->kind = HOPCTFEValue_FLOAT;
        outValue->f64 = 0.0;
        *outIsConst = 1;
        return 0;
    }
    if (HOPTCIsBoolType(c, baseTypeId)) {
        outValue->kind = HOPCTFEValue_BOOL;
        outValue->b = 0u;
        *outIsConst = 1;
        return 0;
    }
    if (HOPTCIsRawptrType(c, baseTypeId)) {
        HOPTCConstEvalSetNullValue(outValue);
        outValue->typeTag = (uint64_t)(uint32_t)typeId;
        *outIsConst = 1;
        return 0;
    }
    if (HOPTCIsStringLikeType(c, baseTypeId)) {
        outValue->kind = HOPCTFEValue_STRING;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        outValue->typeTag = (uint64_t)(uint32_t)typeId;
        *outIsConst = 1;
        return 0;
    }
    if (c->types[baseTypeId].kind == HOPTCType_OPTIONAL) {
        if (HOPTCConstEvalSetOptionalNoneValue(c, baseTypeId, outValue) != 0) {
            return -1;
        }
        *outIsConst = 1;
        return 0;
    }
    if (c->types[baseTypeId].kind == HOPTCType_PTR || c->types[baseTypeId].kind == HOPTCType_REF
        || c->types[baseTypeId].kind == HOPTCType_FUNCTION
        || c->types[baseTypeId].kind == HOPTCType_NULL)
    {
        HOPTCConstEvalSetNullValue(outValue);
        *outIsConst = 1;
        return 0;
    }
    return HOPTCMirConstMakeAggregateValue(evalCtx, baseTypeId, outValue, outIsConst);
}

int HOPTCMirConstZeroInitLocal(
    void* _Nullable ctx,
    const HOPMirTypeRef* typeRef,
    HOPCTFEValue*        outValue,
    int*                 outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    int32_t            typeId = -1;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        HOPTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || typeRef == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (evalCtx->tc == NULL || HOPTCMirConstResolveTypeRefTypeId(evalCtx, typeRef, &typeId) != 0) {
        return -1;
    }
    return HOPTCMirConstZeroInitTypeId(evalCtx, typeId, outValue, outIsConst);
}

int HOPTCMirConstCoerceValueForType(
    void* _Nullable ctx,
    const HOPMirTypeRef* typeRef,
    HOPCTFEValue*        inOutValue,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*   c;
    int32_t            typeId = -1;
    int32_t            baseTypeId;
    (void)diag;
    if (evalCtx == NULL || typeRef == NULL || inOutValue == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || HOPTCMirConstResolveTypeRefTypeId(evalCtx, typeRef, &typeId) != 0) {
        return -1;
    }
    baseTypeId = HOPTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    if ((c->types[baseTypeId].kind == HOPTCType_NAMED
         || c->types[baseTypeId].kind == HOPTCType_ANON_STRUCT)
        && inOutValue->kind == HOPCTFEValue_AGGREGATE)
    {
        if (inOutValue->typeTag == (uint64_t)(uint32_t)baseTypeId) {
            return 0;
        }
        return 0;
    }
    if (c->types[baseTypeId].kind == HOPTCType_OPTIONAL) {
        HOPCTFEValue wrapped;
        if (inOutValue->kind == HOPCTFEValue_OPTIONAL) {
            if (inOutValue->typeTag > 0 && inOutValue->typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)inOutValue->typeTag < c->typeLen
                && (int32_t)inOutValue->typeTag == baseTypeId)
            {
                return 0;
            }
            if (inOutValue->b == 0u) {
                if (HOPTCConstEvalSetOptionalNoneValue(c, baseTypeId, &wrapped) != 0) {
                    return -1;
                }
            } else if (inOutValue->s.bytes == NULL) {
                return 0;
            } else if (
                HOPTCConstEvalSetOptionalSomeValue(
                    c, baseTypeId, (const HOPCTFEValue*)inOutValue->s.bytes, &wrapped)
                != 0)
            {
                return -1;
            }
            *inOutValue = wrapped;
            return 0;
        }
        if (inOutValue->kind == HOPCTFEValue_NULL) {
            if (HOPTCConstEvalSetOptionalNoneValue(c, baseTypeId, &wrapped) != 0) {
                return -1;
            }
            *inOutValue = wrapped;
            return 0;
        }
        if (HOPTCConstEvalSetOptionalSomeValue(c, baseTypeId, inOutValue, &wrapped) != 0) {
            return -1;
        }
        *inOutValue = wrapped;
        return 0;
    }
    if (HOPTCIsIntegerType(c, baseTypeId)) {
        if (inOutValue->kind == HOPCTFEValue_BOOL) {
            inOutValue->kind = HOPCTFEValue_INT;
            inOutValue->i64 = inOutValue->b ? 1 : 0;
        } else if (inOutValue->kind == HOPCTFEValue_FLOAT) {
            inOutValue->kind = HOPCTFEValue_INT;
            inOutValue->i64 = (int64_t)inOutValue->f64;
            inOutValue->f64 = 0.0;
        } else if (inOutValue->kind == HOPCTFEValue_NULL) {
            inOutValue->kind = HOPCTFEValue_INT;
            inOutValue->i64 = 0;
        }
        return 0;
    }
    if (HOPTCIsFloatType(c, baseTypeId)) {
        if (inOutValue->kind == HOPCTFEValue_INT) {
            inOutValue->kind = HOPCTFEValue_FLOAT;
            inOutValue->f64 = (double)inOutValue->i64;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == HOPCTFEValue_BOOL) {
            inOutValue->kind = HOPCTFEValue_FLOAT;
            inOutValue->f64 = inOutValue->b ? 1.0 : 0.0;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == HOPCTFEValue_NULL) {
            inOutValue->kind = HOPCTFEValue_FLOAT;
            inOutValue->f64 = 0.0;
        }
        return 0;
    }
    if (HOPTCIsBoolType(c, baseTypeId)) {
        if (inOutValue->kind == HOPCTFEValue_INT) {
            inOutValue->kind = HOPCTFEValue_BOOL;
            inOutValue->b = inOutValue->i64 != 0 ? 1u : 0u;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == HOPCTFEValue_FLOAT) {
            inOutValue->kind = HOPCTFEValue_BOOL;
            inOutValue->b = inOutValue->f64 != 0.0 ? 1u : 0u;
            inOutValue->f64 = 0.0;
        } else if (inOutValue->kind == HOPCTFEValue_NULL) {
            inOutValue->kind = HOPCTFEValue_BOOL;
            inOutValue->b = 0u;
        }
        return 0;
    }
    if (HOPTCIsStringLikeType(c, typeId)) {
        if (inOutValue->kind == HOPCTFEValue_STRING || inOutValue->kind == HOPCTFEValue_NULL) {
            inOutValue->typeTag = (uint64_t)(uint32_t)typeId;
        }
        return 0;
    }
    return 0;
}

static const uint32_t HOP_TC_MIR_CONST_FN_NONE = UINT32_MAX;

int HOPTCMirConstLowerFunction(
    HOPTCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex);

int HOPTCMirConstInitLowerCtx(HOPTCConstEvalCtx* evalCtx, HOPTCMirConstLowerCtx* _Nonnull outCtx) {
    HOPTypeCheckCtx* c;
    uint32_t*        tcToMir;
    uint8_t*         loweringFns;
    uint32_t*        topConstToMir;
    uint8_t*         loweringTopConsts;
    uint32_t         i;
    if (outCtx == NULL) {
        return -1;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    if (evalCtx == NULL || evalCtx->tc == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    tcToMir = (uint32_t*)HOPArenaAlloc(
        c->arena, sizeof(uint32_t) * c->funcLen, (uint32_t)_Alignof(uint32_t));
    loweringFns = (uint8_t*)HOPArenaAlloc(
        c->arena, sizeof(uint8_t) * c->funcLen, (uint32_t)_Alignof(uint8_t));
    topConstToMir = (uint32_t*)HOPArenaAlloc(
        c->arena, sizeof(uint32_t) * c->ast->len, (uint32_t)_Alignof(uint32_t));
    loweringTopConsts = (uint8_t*)HOPArenaAlloc(
        c->arena, sizeof(uint8_t) * c->ast->len, (uint32_t)_Alignof(uint8_t));
    if (tcToMir == NULL || loweringFns == NULL || topConstToMir == NULL
        || loweringTopConsts == NULL)
    {
        return -1;
    }
    for (i = 0; i < c->funcLen; i++) {
        tcToMir[i] = HOP_TC_MIR_CONST_FN_NONE;
        loweringFns[i] = 0u;
    }
    for (i = 0; i < c->ast->len; i++) {
        topConstToMir[i] = HOP_TC_MIR_CONST_FN_NONE;
        loweringTopConsts[i] = 0u;
    }
    HOPMirProgramBuilderInit(&outCtx->builder, c->arena);
    outCtx->evalCtx = evalCtx;
    outCtx->tcToMir = tcToMir;
    outCtx->loweringFns = loweringFns;
    outCtx->topConstToMir = topConstToMir;
    outCtx->loweringTopConsts = loweringTopConsts;
    outCtx->diag = c->diag;
    return 0;
}

static int HOPTCMirConstGetFunctionBody(
    HOPTypeCheckCtx* c, int32_t fnIndex, int32_t* outFnNode, int32_t* outBodyNode) {
    int32_t child;
    int32_t fnNode;
    int32_t bodyNode = -1;
    if (outFnNode != NULL) {
        *outFnNode = -1;
    }
    if (outBodyNode != NULL) {
        *outBodyNode = -1;
    }
    if (c == NULL || outFnNode == NULL || outBodyNode == NULL || fnIndex < 0
        || (uint32_t)fnIndex >= c->funcLen)
    {
        return 0;
    }
    fnNode = c->funcs[(uint32_t)fnIndex].defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != HOPAst_FN) {
        return 0;
    }
    child = HOPAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == HOPAst_BLOCK) {
            if (bodyNode >= 0) {
                return 0;
            }
            bodyNode = child;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    if (bodyNode < 0) {
        return 0;
    }
    *outFnNode = fnNode;
    *outBodyNode = bodyNode;
    return 1;
}

static int HOPTCMirConstMatchPlainCallNode(
    const HOPTypeCheckCtx* tc,
    const HOPMirSymbolRef* symbol,
    int32_t                rootNode,
    uint32_t               encodedArgCount,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (tc == NULL || symbol == NULL || outCallNode == NULL || outCalleeNode == NULL || rootNode < 0
        || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t           nodeId = stack[--stackLen];
        const HOPAstNode* node = &tc->ast->nodes[nodeId];
        int32_t           child;
        if (node->kind == HOPAst_CALL) {
            int32_t           calleeNode = node->firstChild;
            const HOPAstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            if (callee != NULL && callee->kind == HOPAst_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && HOPTCListCount(tc->ast, nodeId)
                       == HOPMirCallArgCountFromTok((uint16_t)encodedArgCount))
            {
                if (found) {
                    return 0;
                }
                *outCallNode = nodeId;
                *outCalleeNode = calleeNode;
                found = 1;
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int HOPTCMirConstMatchAliasCallNode(
    const HOPTypeCheckCtx* tc,
    const HOPMirSymbolRef* symbol,
    int32_t                rootNode,
    uint32_t               encodedArgCount,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (tc == NULL || symbol == NULL || outCallNode == NULL || outCalleeNode == NULL || rootNode < 0
        || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t           nodeId = stack[--stackLen];
        const HOPAstNode* node = &tc->ast->nodes[nodeId];
        int32_t           child;
        if (node->kind == HOPAst_CALL) {
            int32_t           calleeNode = node->firstChild;
            const HOPAstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            if (callee != NULL && callee->kind == HOPAst_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && HOPTCListCount(tc->ast, nodeId)
                       == HOPMirCallArgCountFromTok((uint16_t)encodedArgCount) + 1u)
            {
                if (found) {
                    return 0;
                }
                *outCallNode = nodeId;
                *outCalleeNode = calleeNode;
                found = 1;
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int HOPTCMirConstResolveDirectCallTarget(
    const HOPTCMirConstLowerCtx* c, int32_t rootNode, const HOPMirInst* ins, int32_t* outFnIndex) {
    const HOPMirSymbolRef* symbol;
    HOPTypeCheckCtx*       tc;
    HOPTCCallArgInfo       callArgs[HOPTC_MAX_CALL_ARGS];
    int32_t                callNode = -1;
    int32_t                calleeNode = -1;
    int32_t                fnIndex = -1;
    int32_t                mutRefTempArgNode = -1;
    uint32_t               argCount = 0;
    int                    status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != HOPMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != HOPMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL
        || !HOPTCMirConstMatchPlainCallNode(
            tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode))
    {
        return 0;
    }
    if (HOPTCCollectCallArgInfo(tc, callNode, calleeNode, 0, -1, callArgs, NULL, &argCount) != 0) {
        return -1;
    }
    status = HOPTCResolveCallByName(
        tc,
        symbol->nameStart,
        symbol->nameEnd,
        callArgs,
        argCount,
        0,
        0,
        &fnIndex,
        &mutRefTempArgNode);
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int HOPTCMirConstHasImportAlias(
    const HOPTCMirConstLowerCtx* c, uint32_t aliasStart, uint32_t aliasEnd) {
    HOPTypeCheckCtx* tc;
    if (c == NULL || c->evalCtx == NULL) {
        return 0;
    }
    tc = c->evalCtx->tc;
    return tc != NULL && HOPTCHasImportAlias(tc, aliasStart, aliasEnd);
}

static int HOPTCMirConstMatchQualifiedCallNode(
    const HOPTypeCheckCtx* tc,
    const HOPMirSymbolRef* symbol,
    int32_t                rootNode,
    uint32_t               encodedArgCount,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode,
    int32_t* _Nonnull outRecvNode,
    uint32_t* _Nonnull outBaseStart,
    uint32_t* _Nonnull outBaseEnd) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (outRecvNode != NULL) {
        *outRecvNode = -1;
    }
    if (outBaseStart != NULL) {
        *outBaseStart = 0;
    }
    if (outBaseEnd != NULL) {
        *outBaseEnd = 0;
    }
    if (tc == NULL || symbol == NULL || outCallNode == NULL || outCalleeNode == NULL
        || outRecvNode == NULL || outBaseStart == NULL || outBaseEnd == NULL || rootNode < 0
        || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t           nodeId = stack[--stackLen];
        const HOPAstNode* node = &tc->ast->nodes[nodeId];
        int32_t           child;
        if (node->kind == HOPAst_CALL) {
            int32_t           calleeNode = node->firstChild;
            const HOPAstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            int32_t recvNode = calleeNode >= 0 ? tc->ast->nodes[calleeNode].firstChild : -1;
            if (callee != NULL && callee->kind == HOPAst_FIELD_EXPR && recvNode >= 0
                && (uint32_t)recvNode < tc->ast->len
                && tc->ast->nodes[recvNode].kind == HOPAst_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && HOPTCListCount(tc->ast, nodeId)
                       == HOPMirCallArgCountFromTok((uint16_t)encodedArgCount))
            {
                uint32_t baseStart = tc->ast->nodes[recvNode].dataStart;
                uint32_t baseEnd = tc->ast->nodes[recvNode].dataEnd;
                if (!found) {
                    *outCallNode = nodeId;
                    *outCalleeNode = calleeNode;
                    *outRecvNode = recvNode;
                    *outBaseStart = baseStart;
                    *outBaseEnd = baseEnd;
                    found = 1;
                } else if (*outBaseStart != baseStart || *outBaseEnd != baseEnd) {
                    return 0;
                }
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int HOPTCMirConstMatchPkgPrefixedQualifiedCallNode(
    const HOPTypeCheckCtx* tc,
    int32_t                rootNode,
    uint32_t               encodedArgCount,
    uint32_t               pkgStart,
    uint32_t               pkgEnd,
    uint32_t               methodStart,
    uint32_t               methodEnd,
    int32_t* _Nonnull outCallNode,
    int32_t* _Nonnull outCalleeNode,
    int32_t* _Nonnull outRecvNode) {
    int32_t  stack[256];
    uint32_t stackLen = 0;
    int      found = 0;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (outCalleeNode != NULL) {
        *outCalleeNode = -1;
    }
    if (outRecvNode != NULL) {
        *outRecvNode = -1;
    }
    if (tc == NULL || outCallNode == NULL || outCalleeNode == NULL || outRecvNode == NULL
        || rootNode < 0 || (uint32_t)rootNode >= tc->ast->len)
    {
        return 0;
    }
    stack[stackLen++] = rootNode;
    while (stackLen > 0) {
        int32_t           nodeId = stack[--stackLen];
        const HOPAstNode* node = &tc->ast->nodes[nodeId];
        int32_t           child;
        if (node->kind == HOPAst_CALL) {
            int32_t           calleeNode = node->firstChild;
            const HOPAstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            int32_t recvNode = calleeNode >= 0 ? tc->ast->nodes[calleeNode].firstChild : -1;
            if (callee != NULL && callee->kind == HOPAst_FIELD_EXPR && recvNode >= 0
                && (uint32_t)recvNode < tc->ast->len
                && tc->ast->nodes[recvNode].kind == HOPAst_IDENT
                && HOPNameEqSlice(
                    tc->src, callee->dataStart, callee->dataEnd, methodStart, methodEnd)
                && HOPNameEqSlice(
                    tc->src,
                    tc->ast->nodes[recvNode].dataStart,
                    tc->ast->nodes[recvNode].dataEnd,
                    pkgStart,
                    pkgEnd)
                && HOPTCListCount(tc->ast, nodeId)
                       == HOPMirCallArgCountFromTok((uint16_t)encodedArgCount) + 1u)
            {
                if (!found) {
                    *outCallNode = nodeId;
                    *outCalleeNode = calleeNode;
                    *outRecvNode = recvNode;
                    found = 1;
                }
            }
        }
        child = node->firstChild;
        while (child >= 0) {
            if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                return 0;
            }
            stack[stackLen++] = child;
            child = tc->ast->nodes[child].nextSibling;
        }
    }
    return found;
}

static int HOPTCMirConstResolveQualifiedCallTarget(
    const HOPTCMirConstLowerCtx* c, int32_t rootNode, const HOPMirInst* ins, int32_t* outFnIndex) {
    HOPTypeCheckCtx*       tc;
    const HOPMirSymbolRef* symbol;
    HOPTCCallArgInfo       callArgs[HOPTC_MAX_CALL_ARGS];
    int32_t                callNode = -1;
    int32_t                calleeNode = -1;
    int32_t                recvNode = -1;
    int32_t                fnIndex = -1;
    int32_t                mutRefTempArgNode = -1;
    uint32_t               argCount = 0;
    uint32_t               baseStart = 0;
    uint32_t               baseEnd = 0;
    int                    status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != HOPMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != HOPMirSymbol_CALL
        || !HOPTCMirConstMatchQualifiedCallNode(
            tc,
            symbol,
            rootNode,
            (uint32_t)ins->tok,
            &callNode,
            &calleeNode,
            &recvNode,
            &baseStart,
            &baseEnd))
    {
        return 0;
    }
    if (!HOPTCMirConstHasImportAlias(c, baseStart, baseEnd)) {
        return 0;
    }
    if (HOPTCCollectCallArgInfo(tc, callNode, calleeNode, 1, recvNode, callArgs, NULL, &argCount)
        != 0)
    {
        return -1;
    }
    status = HOPTCResolveCallByPkgMethod(
        tc,
        baseStart,
        baseEnd,
        symbol->nameStart,
        symbol->nameEnd,
        callArgs,
        argCount,
        1,
        0,
        &fnIndex,
        &mutRefTempArgNode);
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int HOPTCMirConstResolveFunctionIdentTarget(
    const HOPTCMirConstLowerCtx* c, const HOPMirInst* ins, int32_t* outFnIndex) {
    HOPTypeCheckCtx* tc;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != HOPMirOp_LOAD_IDENT)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    *outFnIndex = HOPTCFindPlainFunctionValueIndex(tc, ins->start, ins->end);
    if (*outFnIndex < 0) {
        return 0;
    }
    return 1;
}

static int HOPTCMirConstResolveQualifiedFunctionValueTarget(
    const HOPTCMirConstLowerCtx* c,
    const HOPMirInst*            loadIns,
    const HOPMirInst* _Nullable fieldIns,
    int32_t* outFnIndex) {
    HOPTypeCheckCtx* tc;
    uint32_t         fieldStart;
    uint32_t         fieldEnd;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || loadIns == NULL || fieldIns == NULL || outFnIndex == NULL
        || loadIns->op != HOPMirOp_LOAD_IDENT || fieldIns->op != HOPMirOp_AGG_GET)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || !HOPTCHasImportAlias(tc, loadIns->start, loadIns->end)) {
        return 0;
    }
    if (c->builder.fields != NULL && fieldIns->aux < c->builder.fieldLen) {
        fieldStart = c->builder.fields[fieldIns->aux].nameStart;
        fieldEnd = c->builder.fields[fieldIns->aux].nameEnd;
    } else {
        fieldStart = fieldIns->start;
        fieldEnd = fieldIns->end;
    }
    *outFnIndex = HOPTCFindPkgQualifiedFunctionValueIndex(
        tc, loadIns->start, loadIns->end, fieldStart, fieldEnd);
    return *outFnIndex >= 0;
}

static int HOPTCMirConstRewriteQualifiedFunctionValueLoad(
    HOPTCMirConstLowerCtx* c,
    uint32_t               ownerMirFnIndex,
    uint32_t               loadInstIndex,
    uint32_t               targetMirFnIndex) {
    HOPMirInst* loadIns;
    HOPMirInst* fieldIns;
    HOPMirInst  inserted = { 0 };
    HOPMirConst value = { 0 };
    uint32_t    constIndex = UINT32_MAX;
    if (c == NULL || ownerMirFnIndex >= c->builder.funcLen || loadInstIndex >= c->builder.instLen
        || loadInstIndex + 1u >= c->builder.instLen)
    {
        return -1;
    }
    fieldIns = &c->builder.insts[loadInstIndex + 1u];
    value.kind = HOPMirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (HOPMirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
        return -1;
    }
    inserted.op = HOPMirOp_PUSH_CONST;
    inserted.aux = constIndex;
    inserted.start = fieldIns->start;
    inserted.end = fieldIns->end;
    if (HOPMirProgramBuilderInsertInst(
            &c->builder,
            ownerMirFnIndex,
            loadInstIndex + 2u - c->builder.funcs[ownerMirFnIndex].instStart,
            &inserted)
        != 0)
    {
        return -1;
    }
    loadIns = &c->builder.insts[loadInstIndex];
    fieldIns = &c->builder.insts[loadInstIndex + 1u];
    loadIns->op = HOPMirOp_PUSH_NULL;
    loadIns->tok = 0u;
    loadIns->aux = 0u;
    fieldIns->op = HOPMirOp_DROP;
    fieldIns->tok = 0u;
    fieldIns->aux = 0u;
    return 0;
}

static int HOPTCMirConstResolveTopConstIdentTarget(
    const HOPTCMirConstLowerCtx* c, int32_t rootNode, const HOPMirInst* ins, int32_t* outNodeId) {
    HOPTypeCheckCtx*  tc;
    int32_t           nodeId = -1;
    int32_t           nameIndex = -1;
    HOPTCVarLikeParts parts;
    const HOPAstNode* n;
    if (outNodeId != NULL) {
        *outNodeId = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outNodeId == NULL
        || ins->op != HOPMirOp_LOAD_IDENT)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    nodeId = HOPTCFindTopLevelVarLikeNode(tc, ins->start, ins->end, &nameIndex);
    if (nodeId < 0 || (uint32_t)nodeId >= tc->ast->len || nameIndex != 0) {
        return 0;
    }
    n = &tc->ast->nodes[nodeId];
    if (n->kind != HOPAst_CONST || HOPTCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped
        || parts.nameCount != 1u)
    {
        return 0;
    }
    if (rootNode >= 0 && HOPTCVarLikeInitExprNodeAt(tc, nodeId, 0) == rootNode) {
        return 0;
    }
    *outNodeId = nodeId;
    return 1;
}

static int HOPTCMirConstResolvePkgPrefixedDirectCallTarget(
    const HOPTCMirConstLowerCtx* c, int32_t rootNode, const HOPMirInst* ins, int32_t* outFnIndex) {
    HOPTypeCheckCtx*       tc;
    const HOPMirSymbolRef* symbol;
    HOPTCCallArgInfo       callArgs[HOPTC_MAX_CALL_ARGS];
    int32_t                callNode = -1;
    int32_t                calleeNode = -1;
    int32_t                recvNode = -1;
    int32_t                fnIndex = -1;
    int32_t                mutRefTempArgNode = -1;
    uint32_t               argCount = 0;
    uint32_t               pkgStart = 0;
    uint32_t               pkgEnd = 0;
    uint32_t               methodStart;
    uint32_t               firstPositionalArgIndex = 0;
    int                    collectReceiver = 0;
    int                    status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != HOPMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != HOPMirSymbol_CALL || symbol->flags != 0u
        || !HOPTCExtractPkgPrefixFromTypeName(
            tc, symbol->nameStart, symbol->nameEnd, &pkgStart, &pkgEnd))
    {
        return 0;
    }
    methodStart = pkgEnd + 2u;
    if (methodStart >= symbol->nameEnd) {
        return 0;
    }
    if (HOPTCMirConstMatchPkgPrefixedQualifiedCallNode(
            tc,
            rootNode,
            (uint32_t)ins->tok,
            pkgStart,
            pkgEnd,
            methodStart,
            symbol->nameEnd,
            &callNode,
            &calleeNode,
            &recvNode))
    {
        collectReceiver = 1;
        firstPositionalArgIndex = 1u;
    } else if (!HOPTCMirConstMatchPlainCallNode(
                   tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode))
    {
        return 0;
    }
    if (HOPTCCollectCallArgInfo(
            tc, callNode, calleeNode, collectReceiver, recvNode, callArgs, NULL, &argCount)
        != 0)
    {
        return -1;
    }
    status = HOPTCResolveCallByPkgMethod(
        tc,
        pkgStart,
        pkgEnd,
        methodStart,
        symbol->nameEnd,
        callArgs,
        argCount,
        firstPositionalArgIndex,
        0,
        &fnIndex,
        &mutRefTempArgNode);
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int HOPTCMirConstResolveSimpleTopConstAliasCallTarget(
    const HOPTCMirConstLowerCtx* c, int32_t rootNode, const HOPMirInst* ins, int32_t* outFnIndex) {
    HOPTypeCheckCtx*       tc;
    const HOPMirSymbolRef* symbol;
    HOPTCVarLikeParts      parts;
    HOPTCCallArgInfo       callArgs[HOPTC_MAX_CALL_ARGS];
    int32_t                nodeId = -1;
    int32_t                nameIndex = -1;
    int32_t                initNode;
    int32_t                callNode = -1;
    int32_t                calleeNode = -1;
    int32_t                fnIndex = -1;
    int32_t                mutRefTempArgNode = -1;
    uint32_t               argCount = 0;
    int                    status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != HOPMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != HOPMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    nodeId = HOPTCFindTopLevelVarLikeNode(tc, symbol->nameStart, symbol->nameEnd, &nameIndex);
    if (nodeId < 0 || nameIndex != 0) {
        return 0;
    }
    if (!HOPTCMirConstMatchPlainCallNode(
            tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode)
        && !HOPTCMirConstMatchAliasCallNode(
            tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode))
    {
        return 0;
    }
    if (HOPTCCollectCallArgInfo(tc, callNode, calleeNode, 0, -1, callArgs, NULL, &argCount) != 0) {
        return -1;
    }
    if (HOPTCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped || parts.nameCount != 1u
        || tc->ast->nodes[nodeId].kind != HOPAst_CONST)
    {
        return 0;
    }
    initNode = HOPTCVarLikeInitExprNodeAt(tc, nodeId, 0);
    if (initNode < 0 || (uint32_t)initNode >= tc->ast->len) {
        return 0;
    }
    if (tc->ast->nodes[initNode].kind == HOPAst_IDENT) {
        const HOPAstNode* initExpr = &tc->ast->nodes[initNode];
        uint32_t          pkgStart = 0;
        uint32_t          pkgEnd = 0;
        if (HOPTCExtractPkgPrefixFromTypeName(
                tc, initExpr->dataStart, initExpr->dataEnd, &pkgStart, &pkgEnd))
        {
            uint32_t methodStart = pkgEnd + 2u;
            if (methodStart >= initExpr->dataEnd) {
                return 0;
            }
            status = HOPTCResolveCallByPkgMethod(
                tc,
                pkgStart,
                pkgEnd,
                methodStart,
                initExpr->dataEnd,
                callArgs,
                argCount,
                0,
                0,
                &fnIndex,
                &mutRefTempArgNode);
        } else {
            status = HOPTCResolveCallByName(
                tc,
                initExpr->dataStart,
                initExpr->dataEnd,
                callArgs,
                argCount,
                0,
                0,
                &fnIndex,
                &mutRefTempArgNode);
        }
    } else if (tc->ast->nodes[initNode].kind == HOPAst_FIELD_EXPR) {
        const HOPAstNode* initExpr = &tc->ast->nodes[initNode];
        int32_t           baseNode = initExpr->firstChild;
        const HOPAstNode* baseExpr;
        if (baseNode < 0 || (uint32_t)baseNode >= tc->ast->len) {
            return 0;
        }
        baseExpr = &tc->ast->nodes[baseNode];
        if (baseExpr->kind != HOPAst_IDENT
            || !HOPTCHasImportAlias(tc, baseExpr->dataStart, baseExpr->dataEnd))
        {
            return 0;
        }
        status = HOPTCResolveCallByPkgMethod(
            tc,
            baseExpr->dataStart,
            baseExpr->dataEnd,
            initExpr->dataStart,
            initExpr->dataEnd,
            callArgs,
            argCount,
            0,
            0,
            &fnIndex,
            &mutRefTempArgNode);
    } else {
        return 0;
    }
    if (status != 0 || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = fnIndex;
    return 1;
}

static int HOPTCMirConstFinalizeLoweredFunction(
    HOPTCMirConstLowerCtx* c, uint32_t* mirMapSlot, uint32_t mirFnIndex, int32_t rootNode);
static int HOPTCMirConstRunFunction(
    HOPTCConstEvalCtx*     evalCtx,
    HOPTCMirConstLowerCtx* lowerCtx,
    const HOPMirProgram*   program,
    uint32_t               mirFnIndex,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int* _Nullable outDidReturn,
    int* outIsConst);

static int HOPTCMirConstLowerTopConstNode(
    HOPTCMirConstLowerCtx* c, int32_t nodeId, uint32_t* _Nullable outMirFnIndex) {
    HOPTypeCheckCtx*  tc;
    HOPTCVarLikeParts parts;
    uint32_t          mirFnIndex = UINT32_MAX;
    int32_t           initNode;
    int               supported = 0;
    int               rewriteRc;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->evalCtx == NULL || c->topConstToMir == NULL || c->loweringTopConsts == NULL)
    {
        return -1;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || nodeId < 0 || (uint32_t)nodeId >= tc->ast->len) {
        return 0;
    }
    if (c->topConstToMir[(uint32_t)nodeId] != HOP_TC_MIR_CONST_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->topConstToMir[(uint32_t)nodeId];
        }
        return 1;
    }
    if (c->loweringTopConsts[(uint32_t)nodeId] != 0u) {
        return 0;
    }
    if (HOPTCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped || parts.nameCount != 1u
        || tc->ast->nodes[nodeId].kind != HOPAst_CONST)
    {
        return 0;
    }
    initNode = HOPTCVarLikeInitExprNodeAt(tc, nodeId, 0);
    if (initNode < 0) {
        return 0;
    }
    c->loweringTopConsts[(uint32_t)nodeId] = 1u;
    if (HOPMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
            &c->builder,
            tc->arena,
            tc->ast,
            tc->src,
            nodeId,
            tc->ast->nodes[nodeId].dataStart,
            tc->ast->nodes[nodeId].dataEnd,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        c->loweringTopConsts[(uint32_t)nodeId] = 0u;
        return -1;
    }
    if (!supported || mirFnIndex == UINT32_MAX) {
        c->loweringTopConsts[(uint32_t)nodeId] = 0u;
        return 0;
    }
    rewriteRc = HOPTCMirConstFinalizeLoweredFunction(
        c, &c->topConstToMir[(uint32_t)nodeId], mirFnIndex, initNode);
    c->loweringTopConsts[(uint32_t)nodeId] = 0u;
    if (rewriteRc <= 0) {
        return rewriteRc;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int HOPTCMirConstRewriteLoadIdentToFunctionConst(
    HOPTCMirConstLowerCtx* c, HOPMirInst* ins, uint32_t targetMirFnIndex) {
    HOPMirConst value = { 0 };
    uint32_t    constIndex = UINT32_MAX;
    if (c == NULL || ins == NULL) {
        return -1;
    }
    value.kind = HOPMirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (HOPMirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
        return -1;
    }
    ins->op = HOPMirOp_PUSH_CONST;
    ins->tok = 0u;
    ins->aux = constIndex;
    return 0;
}

static int HOPTCMirConstFinalizeLoweredFunction(
    HOPTCMirConstLowerCtx* c, uint32_t* mirMapSlot, uint32_t mirFnIndex, int32_t rootNode) {
    int rewriteRc;
    if (c == NULL || mirMapSlot == NULL || mirFnIndex == UINT32_MAX) {
        return -1;
    }
    *mirMapSlot = mirFnIndex;
    rewriteRc = HOPTCMirConstRewriteDirectCalls(c, mirFnIndex, rootNode);
    if (rewriteRc < 0) {
        return -1;
    }
    if (rewriteRc == 0) {
        *mirMapSlot = HOP_TC_MIR_CONST_FN_NONE;
        return 0;
    }
    return 1;
}

static int HOPTCMirConstFindTcFunctionIndexForMir(
    const HOPTCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t* outFnIndex) {
    uint32_t i;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->tcToMir == NULL || c->evalCtx == NULL || c->evalCtx->tc == NULL
        || outFnIndex == NULL)
    {
        return 0;
    }
    for (i = 0; i < c->evalCtx->tc->funcLen; i++) {
        if (c->tcToMir[i] == mirFnIndex) {
            *outFnIndex = (int32_t)i;
            return 1;
        }
    }
    return 0;
}

static int HOPTCMirConstExportValue(const HOPTCMirConstLowerCtx* c, HOPCTFEValue* _Nonnull value) {
    uint32_t mirFnIndex = UINT32_MAX;
    int32_t  tcFnIndex = -1;
    if (c == NULL || value == NULL || !HOPMirValueAsFunctionRef(value, &mirFnIndex)) {
        return 1;
    }
    if (!HOPTCMirConstFindTcFunctionIndexForMir(c, mirFnIndex, &tcFnIndex) || tcFnIndex < 0) {
        return 0;
    }
    HOPMirValueSetFunctionRef(value, (uint32_t)tcFnIndex);
    return 1;
}

static int HOPTCMirConstCallNodeIsSpecialBuiltin(HOPTypeCheckCtx* tc, int32_t callNode) {
    const HOPAstNode* call;
    const HOPAstNode* callee;
    int32_t           calleeNode;
    int32_t           recvNode;
    if (tc == NULL || callNode < 0 || (uint32_t)callNode >= tc->ast->len) {
        return 0;
    }
    call = &tc->ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
    if (call->kind != HOPAst_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == HOPAst_IDENT) {
        return HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "typeof")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "kind")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "base")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "is_alias")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "type_name")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "ptr")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "slice")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "array")
            || HOPTCIsSourceLocationOfName(tc, callee->dataStart, callee->dataEnd)
            || HOPTCCompilerDiagOpFromName(tc, callee->dataStart, callee->dataEnd)
                   != HOPTCCompilerDiagOp_NONE;
    }
    if (callee->kind != HOPAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= tc->ast->len
        || tc->ast->nodes[recvNode].kind != HOPAst_IDENT)
    {
        return 0;
    }
    if (HOPNameEqLiteral(
            tc->src,
            tc->ast->nodes[recvNode].dataStart,
            tc->ast->nodes[recvNode].dataEnd,
            "builtin")
        && HOPTCIsSourceLocationOfName(tc, callee->dataStart, callee->dataEnd))
    {
        return 1;
    }
    if (HOPNameEqLiteral(
            tc->src,
            tc->ast->nodes[recvNode].dataStart,
            tc->ast->nodes[recvNode].dataEnd,
            "reflect")
        && (HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "kind")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "base")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "is_alias")
            || HOPNameEqLiteral(tc->src, callee->dataStart, callee->dataEnd, "type_name")))
    {
        return 1;
    }
    if (HOPNameEqLiteral(
            tc->src,
            tc->ast->nodes[recvNode].dataStart,
            tc->ast->nodes[recvNode].dataEnd,
            "compiler")
        && HOPTCConstEvalCompilerDiagOpFromFieldExpr(tc, callee) != HOPTCCompilerDiagOp_NONE)
    {
        return 1;
    }
    return 0;
}

static int HOPTCMirConstResolveCallNode(
    HOPTCConstEvalCtx* evalCtx,
    const HOPMirProgram* _Nullable program,
    const HOPMirInst* _Nullable inst,
    int32_t* outCallNode) {
    const HOPMirSymbolRef* symbol;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || program == NULL || inst == NULL
        || outCallNode == NULL || inst->op != HOPMirOp_CALL || inst->aux >= program->symbolLen)
    {
        return 0;
    }
    symbol = &program->symbols[inst->aux];
    if (symbol->kind != HOPMirSymbol_CALL || symbol->target >= evalCtx->tc->ast->len) {
        return 0;
    }
    *outCallNode = (int32_t)symbol->target;
    return 1;
}

static int HOPTCMirConstRunFunction(
    HOPTCConstEvalCtx*     evalCtx,
    HOPTCMirConstLowerCtx* lowerCtx,
    const HOPMirProgram*   program,
    uint32_t               mirFnIndex,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int* _Nullable outDidReturn,
    int* outIsConst) {
    HOPTypeCheckCtx* c;
    HOPMirExecEnv    env = { 0 };
    int              mirIsConst = 0;
    if (outDidReturn != NULL) {
        *outDidReturn = 0;
    }
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (evalCtx == NULL || program == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    env.src = c->src;
    env.resolveIdent = HOPTCResolveConstIdent;
    env.resolveCallPre = HOPTCResolveConstCallMirPre;
    env.resolveCall = HOPTCResolveConstCallMir;
    env.resolveCtx = evalCtx;
    env.zeroInitLocal = HOPTCMirConstZeroInitLocal;
    env.zeroInitCtx = evalCtx;
    env.coerceValueForType = HOPTCMirConstCoerceValueForType;
    env.coerceValueCtx = evalCtx;
    env.indexValue = HOPTCMirConstIndexValue;
    env.indexValueCtx = evalCtx;
    env.sequenceLen = HOPTCMirConstSequenceLen;
    env.sequenceLenCtx = evalCtx;
    env.iterInit = HOPTCMirConstIterInit;
    env.iterInitCtx = evalCtx;
    env.iterNext = HOPTCMirConstIterNext;
    env.iterNextCtx = evalCtx;
    env.aggGetField = HOPTCMirConstAggGetField;
    env.aggGetFieldCtx = evalCtx;
    env.aggAddrField = HOPTCMirConstAggAddrField;
    env.aggAddrFieldCtx = evalCtx;
    env.makeTuple = HOPTCMirConstMakeTuple;
    env.makeTupleCtx = evalCtx;
    env.bindFrame = HOPTCMirConstBindFrame;
    env.unbindFrame = HOPTCMirConstUnbindFrame;
    env.frameCtx = evalCtx;
    env.setReason = HOPTCMirConstSetReasonCb;
    env.setReasonCtx = evalCtx;
    env.backwardJumpLimit = HOPTC_CONST_FOR_MAX_ITERS;
    env.diag = c->diag;
    if (!HOPMirProgramNeedsDynamicResolution(program)) {
        HOPMirExecEnvDisableDynamicResolution(&env);
    }
    if (HOPMirEvalFunction(
            c->arena, program, mirFnIndex, args, argCount, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    if (mirIsConst && !HOPTCMirConstExportValue(lowerCtx, outValue)) {
        return 0;
    }
    *outIsConst = mirIsConst;
    if (outDidReturn != NULL && mirIsConst) {
        *outDidReturn = outValue->kind != HOPCTFEValue_INVALID;
    }
    return 0;
}

int HOPTCResolveConstCallMirPre(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    int32_t            callNode = -1;
    int                status;

    (void)function;
    (void)nameStart;
    (void)nameEnd;
    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!HOPTCMirConstResolveCallNode(evalCtx, program, inst, &callNode) || callNode < 0) {
        return 0;
    }

    status = HOPTCConstEvalCompilerDiagCall(evalCtx, callNode, outValue, outIsConst);
    if (status < 0) {
        return -1;
    }
    if (status == 0) {
        return 1;
    }

    status = HOPTCConstEvalSourceLocationOfCall(evalCtx, callNode, outValue, outIsConst);
    if (status < 0) {
        return -1;
    }
    if (status == 0) {
        return 1;
    }

    return 0;
}

int HOPTCMirConstRewriteDirectCalls(
    HOPTCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t rootNode) {
    HOPTypeCheckCtx* tc;
    uint32_t         instIndex;
    if (c == NULL || c->evalCtx == NULL || c->tcToMir == NULL || c->loweringFns == NULL) {
        return -1;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || mirFnIndex >= c->builder.funcLen) {
        return 0;
    }
    for (instIndex = c->builder.funcs[mirFnIndex].instStart;
         instIndex < c->builder.funcs[mirFnIndex].instStart + c->builder.funcs[mirFnIndex].instLen;
         instIndex++)
    {
        HOPMirInst* ins = &c->builder.insts[instIndex];
        HOPMirInst* nextIns =
            instIndex + 1u < c->builder.instLen ? &c->builder.insts[instIndex + 1u] : NULL;
        int32_t  targetFnIndex = -1;
        int32_t  targetTopConstNode = -1;
        uint32_t targetMirFnIndex = UINT32_MAX;
        int      lowerRc;
        if (ins->op == HOPMirOp_CALL && ins->aux < c->builder.symbolLen) {
            const HOPMirSymbolRef* symbol = &c->builder.symbols[ins->aux];
            int32_t                callNode = (int32_t)symbol->target;
            if (symbol->kind == HOPMirSymbol_CALL && callNode >= 0
                && (uint32_t)callNode < tc->ast->len
                && HOPTCMirConstCallNodeIsSpecialBuiltin(tc, callNode))
            {
                continue;
            }
        }
        lowerRc = HOPTCMirConstResolveQualifiedFunctionValueTarget(c, ins, nextIns, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = HOPTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            if (HOPTCMirConstRewriteQualifiedFunctionValueLoad(
                    c, mirFnIndex, instIndex, targetMirFnIndex)
                != 0)
            {
                return -1;
            }
            continue;
        }
        if (HOPTCMirConstResolveTopConstIdentTarget(c, rootNode, ins, &targetTopConstNode)) {
            lowerRc = HOPTCMirConstLowerTopConstNode(c, targetTopConstNode, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = HOPMirOp_CALL_FN;
            ins->tok = 0u;
            ins->aux = targetMirFnIndex;
            continue;
        }
        if (HOPTCMirConstResolveFunctionIdentTarget(c, ins, &targetFnIndex)) {
            lowerRc = HOPTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            if (HOPTCMirConstRewriteLoadIdentToFunctionConst(c, ins, targetMirFnIndex) != 0) {
                return -1;
            }
            continue;
        }
        lowerRc = HOPTCMirConstResolveSimpleTopConstAliasCallTarget(
            c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = HOPTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = HOPMirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = HOPTCMirConstResolveQualifiedCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = HOPTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = HOPMirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = HOPTCMirConstResolvePkgPrefixedDirectCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = HOPTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins = &c->builder.insts[instIndex];
            ins->op = HOPMirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = HOPTCMirConstResolveDirectCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            continue;
        }
        lowerRc = HOPTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            return 0;
        }
        ins = &c->builder.insts[instIndex];
        ins->op = HOPMirOp_CALL_FN;
        ins->aux = targetMirFnIndex;
    }
    return 1;
}

int HOPTCMirConstLowerFunction(
    HOPTCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex) {
    HOPTypeCheckCtx*   tc;
    HOPMirLowerOptions options = { 0 };
    uint32_t           mirFnIndex = UINT32_MAX;
    int32_t            fnNode = -1;
    int32_t            bodyNode = -1;
    int                supported = 0;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = UINT32_MAX;
    }
    if (c == NULL || c->evalCtx == NULL || c->tcToMir == NULL || c->loweringFns == NULL) {
        return -1;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || fnIndex < 0 || (uint32_t)fnIndex >= tc->funcLen) {
        return 0;
    }
    if (c->loweringFns[(uint32_t)fnIndex] != 0u) {
        return 0;
    }
    if (c->tcToMir[(uint32_t)fnIndex] != HOP_TC_MIR_CONST_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->tcToMir[(uint32_t)fnIndex];
        }
        return 1;
    }
    if (!HOPTCMirConstGetFunctionBody(tc, fnIndex, &fnNode, &bodyNode)) {
        return 0;
    }
    {
        int32_t  stack[256];
        uint32_t stackLen = 0;
        stack[stackLen++] = bodyNode;
        while (stackLen > 0) {
            int32_t           nodeId = stack[--stackLen];
            const HOPAstNode* node = &tc->ast->nodes[nodeId];
            int32_t           child;
            child = node->firstChild;
            while (child >= 0) {
                if (stackLen >= (uint32_t)(sizeof(stack) / sizeof(stack[0]))) {
                    return 0;
                }
                stack[stackLen++] = child;
                child = tc->ast->nodes[child].nextSibling;
            }
        }
    }
    c->loweringFns[(uint32_t)fnIndex] = 1u;
    options.lowerConstExpr = HOPTCMirConstLowerConstExpr;
    options.lowerConstExprCtx = c->evalCtx;
    if (HOPMirLowerAppendSimpleFunctionWithOptions(
            &c->builder,
            tc->arena,
            tc->ast,
            tc->src,
            fnNode,
            bodyNode,
            &options,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        c->loweringFns[(uint32_t)fnIndex] = 0u;
        return -1;
    }
    if (!supported) {
        c->loweringFns[(uint32_t)fnIndex] = 0u;
        return 0;
    }
    {
        int rewriteRc = HOPTCMirConstFinalizeLoweredFunction(
            c, &c->tcToMir[(uint32_t)fnIndex], mirFnIndex, bodyNode);
        if (rewriteRc <= 0) {
            c->loweringFns[(uint32_t)fnIndex] = 0u;
            return rewriteRc;
        }
    }
    c->loweringFns[(uint32_t)fnIndex] = 0u;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int HOPTCTryMirConstCall(
    HOPTCConstEvalCtx*  evalCtx,
    int32_t             fnIndex,
    const HOPCTFEValue* args,
    uint32_t            argCount,
    HOPCTFEValue*       outValue,
    int*                outDidReturn,
    int*                outIsConst,
    int*                outSupported) {
    HOPTypeCheckCtx*      c;
    HOPMirProgram         program = { 0 };
    HOPTCMirConstLowerCtx lowerCtx;
    uint32_t              mirFnIndex = UINT32_MAX;
    int                   lowerRc;
    if (outDidReturn != NULL) {
        *outDidReturn = 0;
    }
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (evalCtx == NULL || outValue == NULL || outDidReturn == NULL || outIsConst == NULL
        || outSupported == NULL)
    {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return -1;
    }
    if (HOPTCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    lowerRc = HOPTCMirConstLowerFunction(&lowerCtx, fnIndex, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        HOPTCMirConstAdoptLowerDiagReason(evalCtx, lowerCtx.diag);
        return 0;
    }
    HOPMirProgramBuilderFinish(&lowerCtx.builder, &program);
    if (HOPTCMirConstRunFunction(
            evalCtx,
            &lowerCtx,
            &program,
            mirFnIndex,
            args,
            argCount,
            outValue,
            outDidReturn,
            outIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    return 0;
}

static int HOPTCConstEvalTryDirectReturnFunction(
    HOPTCConstEvalCtx* evalCtx, int32_t bodyNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx* c;
    int32_t          stmtNode;
    int32_t          exprNode;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || bodyNode < 0 || (uint32_t)bodyNode >= c->ast->len
        || c->ast->nodes[bodyNode].kind != HOPAst_BLOCK)
    {
        return 1;
    }
    stmtNode = HOPAstFirstChild(c->ast, bodyNode);
    if (stmtNode < 0 || HOPAstNextSibling(c->ast, stmtNode) >= 0
        || c->ast->nodes[stmtNode].kind != HOPAst_RETURN)
    {
        return 1;
    }
    exprNode = HOPAstFirstChild(c->ast, stmtNode);
    if (exprNode < 0 || HOPAstNextSibling(c->ast, exprNode) >= 0) {
        return 1;
    }
    return HOPTCEvalConstExprNode(evalCtx, exprNode, outValue, outIsConst);
}

static int HOPTCTryMirTopLevelConst(
    HOPTCConstEvalCtx* evalCtx,
    int32_t            nodeId,
    int32_t            nameIndex,
    HOPCTFEValue*      outValue,
    int*               outIsConst,
    int*               outSupported) {
    HOPMirProgram         program = { 0 };
    HOPTCMirConstLowerCtx lowerCtx;
    uint32_t              mirFnIndex = UINT32_MAX;
    int                   lowerRc;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL || outSupported == NULL) {
        return -1;
    }
    if (evalCtx->tc != NULL) {
        HOPTypeCheckCtx*  c = evalCtx->tc;
        HOPTCVarLikeParts parts;
        int32_t           initNode = -1;
        int32_t           initType = -1;
        HOPDiag           savedDiag = { 0 };
        int               haveSavedDiag = c->diag != NULL;
        if (haveSavedDiag) {
            savedDiag = *c->diag;
        }
        if (HOPTCVarLikeGetParts(c, nodeId, &parts) == 0 && !parts.grouped && nameIndex >= 0
            && (uint32_t)nameIndex < parts.nameCount)
        {
            initNode = HOPTCVarLikeInitExprNodeAt(c, nodeId, nameIndex);
            if (initNode >= 0 && HOPTCTypeExpr(c, initNode, &initType) == 0
                && initType == c->typeType)
            {
                if (haveSavedDiag) {
                    *c->diag = savedDiag;
                }
                return 0;
            }
        }
        if (haveSavedDiag) {
            *c->diag = savedDiag;
        }
    }
    if (HOPTCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    lowerRc = HOPTCMirConstLowerTopConstNode(&lowerCtx, nodeId, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        HOPTCMirConstAdoptLowerDiagReason(evalCtx, lowerCtx.diag);
        return 0;
    }
    HOPMirProgramBuilderFinish(&lowerCtx.builder, &program);
    if (HOPTCMirConstRunFunction(
            evalCtx, &lowerCtx, &program, mirFnIndex, NULL, 0, outValue, NULL, outIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    return 0;
}

int32_t HOPTCFindConstCallableFunction(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, uint32_t argCount) {
    uint32_t i;
    int32_t  found = -1;
    for (i = 0; i < c->funcLen; i++) {
        const HOPTCFunction* f = &c->funcs[i];
        if (!HOPNameEqSlice(c->src, f->nameStart, f->nameEnd, nameStart, nameEnd)) {
            continue;
        }
        if (f->contextType >= 0 || (f->flags & HOPTCFunctionFlag_VARIADIC) != 0
            || f->paramCount != argCount || f->defNode < 0)
        {
            continue;
        }
        if (found >= 0) {
            return -2;
        }
        found = (int32_t)i;
    }
    return found;
}

static int32_t HOPTCFindPkgConstCallableFunction(
    HOPTypeCheckCtx* c,
    uint32_t         pkgStart,
    uint32_t         pkgEnd,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    uint32_t         argCount) {
    int32_t  candidates[HOPTC_MAX_CALL_CANDIDATES];
    uint32_t candidateCount = 0;
    int      nameFound = 0;
    uint32_t i;
    int32_t  found = -1;
    HOPTCGatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, nameStart, nameEnd, candidates, &candidateCount, &nameFound);
    if (!nameFound) {
        return -1;
    }
    for (i = 0; i < candidateCount; i++) {
        const HOPTCFunction* f;
        int32_t              fnIndex = candidates[i];
        if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
            continue;
        }
        f = &c->funcs[(uint32_t)fnIndex];
        if (f->contextType >= 0 || (f->flags & HOPTCFunctionFlag_VARIADIC) != 0
            || f->paramCount != argCount || f->defNode < 0)
        {
            continue;
        }
        if (found >= 0) {
            return -2;
        }
        found = fnIndex;
    }
    return found;
}

static int HOPTCConstEvalPrepareInvokeCallContext(
    HOPTCConstEvalCtx* evalCtx,
    int32_t            callNode,
    int32_t            calleeNode,
    int32_t            fnIndex,
    HOPTCCallArgInfo*  outCallArgs,
    uint32_t*          outCallArgCount,
    HOPTCCallBinding*  outBinding,
    uint32_t*          outPackParamNameStart,
    uint32_t*          outPackParamNameEnd) {
    HOPTypeCheckCtx*     c;
    const HOPTCFunction* fn;
    HOPTCCallMapError    mapError;
    uint32_t             paramStart;
    uint32_t             argCount = 0;
    if (outCallArgCount != NULL) {
        *outCallArgCount = 0;
    }
    if (outPackParamNameStart != NULL) {
        *outPackParamNameStart = 0;
    }
    if (outPackParamNameEnd != NULL) {
        *outPackParamNameEnd = 0;
    }
    if (evalCtx == NULL || outCallArgs == NULL || outCallArgCount == NULL || outBinding == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen || callNode < 0
        || (uint32_t)callNode >= c->ast->len || calleeNode < 0
        || (uint32_t)calleeNode >= c->ast->len)
    {
        return 1;
    }
    fn = &c->funcs[(uint32_t)fnIndex];
    paramStart = fn->paramTypeStart;
    if (HOPTCCollectCallArgInfo(c, callNode, calleeNode, 0, -1, outCallArgs, NULL, &argCount) != 0)
    {
        return -1;
    }
    HOPTCCallMapErrorClear(&mapError);
    if (HOPTCPrepareCallBinding(
            c,
            outCallArgs,
            argCount,
            &c->funcParamNameStarts[paramStart],
            &c->funcParamNameEnds[paramStart],
            &c->funcParamTypes[paramStart],
            fn->paramCount,
            (fn->flags & HOPTCFunctionFlag_VARIADIC) != 0,
            1,
            0,
            outBinding,
            &mapError)
        != 0)
    {
        return 1;
    }
    *outCallArgCount = argCount;
    if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0 && fn->paramCount > 0u) {
        if (outPackParamNameStart != NULL) {
            *outPackParamNameStart = c->funcParamNameStarts[paramStart + fn->paramCount - 1u];
        }
        if (outPackParamNameEnd != NULL) {
            *outPackParamNameEnd = c->funcParamNameEnds[paramStart + fn->paramCount - 1u];
        }
    }
    return 0;
}

static int HOPTCInvokeConstFunctionByIndex(
    HOPTCConstEvalCtx* evalCtx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    int32_t            fnIndex,
    const HOPCTFEValue* _Nullable args,
    uint32_t argCount,
    const HOPTCCallArgInfo* _Nullable callArgs,
    uint32_t callArgCount,
    const HOPTCCallBinding* _Nullable callBinding,
    uint32_t      callPackParamNameStart,
    uint32_t      callPackParamNameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst) {
    HOPTypeCheckCtx*    c;
    int32_t             fnNode;
    int32_t             bodyNode = -1;
    int32_t             child;
    uint32_t            paramCount = 0;
    HOPCTFEExecBinding* paramBindings = NULL;
    HOPCTFEExecEnv      paramFrame;
    HOPCTFEExecCtx      execCtx;
    HOPCTFEExecCtx*     savedExecCtx;
    HOPTCConstEvalCtx*  savedActiveConstEvalCtx;
    const void*         savedCallArgs;
    uint32_t            savedCallArgCount;
    const void*         savedCallBinding;
    uint32_t            savedCallPackParamNameStart;
    uint32_t            savedCallPackParamNameEnd;
    const HOPCTFEValue* invokeArgs = args;
    HOPCTFEValue        reorderedArgs[HOPTC_MAX_CALL_ARGS];
    uint32_t            savedDepth;
    HOPCTFEValue        retValue;
    int                 didReturn = 0;
    int                 isConst = 0;
    int                 mirSupported = 0;
    int                 rc;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return -1;
    }

    HOPTCMarkConstDiagFnInvoked(c, fnIndex);

    for (savedDepth = 0; savedDepth < evalCtx->fnDepth; savedDepth++) {
        if (evalCtx->fnStack[savedDepth] == fnIndex) {
            HOPTCConstSetReason(
                evalCtx, nameStart, nameEnd, "recursive const function calls are not supported");
            *outIsConst = 0;
            return 0;
        }
    }
    if (evalCtx->fnDepth >= HOPTC_CONST_CALL_MAX_DEPTH) {
        HOPTCConstSetReason(evalCtx, nameStart, nameEnd, "const-eval call depth limit exceeded");
        *outIsConst = 0;
        return 0;
    }

    fnNode = c->funcs[fnIndex].defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != HOPAst_FN) {
        HOPTCConstSetReason(evalCtx, nameStart, nameEnd, "call target has no const-evaluable body");
        *outIsConst = 0;
        return 0;
    }

    child = HOPAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
            if (paramCount >= argCount) {
                HOPTCConstSetReasonNode(
                    evalCtx, fnNode, "function signature does not match const-eval call arguments");
                *outIsConst = 0;
                return 0;
            }
            paramCount++;
        } else if (n->kind == HOPAst_BLOCK) {
            if (bodyNode >= 0) {
                HOPTCConstSetReasonNode(
                    evalCtx, fnNode, "function body shape is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            bodyNode = child;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    if (paramCount != argCount || bodyNode < 0) {
        HOPTCConstSetReasonNode(evalCtx, fnNode, "function body shape is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (argCount > 0 && args == NULL) {
        return -1;
    }
    if (callBinding != NULL && argCount > 0) {
        uint8_t  assigned[HOPTC_MAX_CALL_ARGS];
        uint32_t i;
        if (argCount > HOPTC_MAX_CALL_ARGS || callBinding->fixedCount != argCount
            || callBinding->fixedInputCount != argCount
            || callBinding->spreadArgIndex != UINT32_MAX)
        {
            return -1;
        }
        memset(assigned, 0, sizeof(assigned));
        for (i = 0; i < argCount; i++) {
            int32_t paramIndex = callBinding->argParamIndices[i];
            if (paramIndex < 0 || (uint32_t)paramIndex >= argCount
                || assigned[(uint32_t)paramIndex])
            {
                return -1;
            }
            reorderedArgs[(uint32_t)paramIndex] = args[i];
            assigned[(uint32_t)paramIndex] = 1;
        }
        invokeArgs = reorderedArgs;
    }

    if (argCount > 0) {
        paramBindings = (HOPCTFEExecBinding*)HOPArenaAlloc(
            c->arena,
            sizeof(HOPCTFEExecBinding) * argCount,
            (uint32_t)_Alignof(HOPCTFEExecBinding));
        if (paramBindings == NULL) {
            return HOPTCFailNode(c, fnNode, HOPDiag_ARENA_OOM);
        }
    }
    paramCount = 0;
    child = HOPAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
            if (paramBindings == NULL) {
                return -1;
            }
            int32_t paramTypeNode = HOPAstFirstChild(c->ast, child);
            int32_t paramTypeId = -1;
            uint8_t savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
            c->allowConstNumericTypeName = 1;
            if (paramTypeNode < 0 || HOPTCResolveTypeNode(c, paramTypeNode, &paramTypeId) != 0) {
                c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
                return -1;
            }
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            paramBindings[paramCount].nameStart = n->dataStart;
            paramBindings[paramCount].nameEnd = n->dataEnd;
            paramBindings[paramCount].typeId = paramTypeId;
            paramBindings[paramCount].mutable = 1;
            paramBindings[paramCount]._reserved[0] = 0;
            paramBindings[paramCount]._reserved[1] = 0;
            paramBindings[paramCount]._reserved[2] = 0;
            if (c->types[paramTypeId].kind == HOPTCType_OPTIONAL) {
                HOPCTFEValue wrapped;
                if (invokeArgs[paramCount].kind == HOPCTFEValue_OPTIONAL) {
                    if (invokeArgs[paramCount].typeTag > 0
                        && invokeArgs[paramCount].typeTag <= (uint64_t)INT32_MAX
                        && (uint32_t)invokeArgs[paramCount].typeTag < c->typeLen
                        && (int32_t)invokeArgs[paramCount].typeTag == paramTypeId)
                    {
                        wrapped = invokeArgs[paramCount];
                    } else if (invokeArgs[paramCount].b == 0u) {
                        if (HOPTCConstEvalSetOptionalNoneValue(c, paramTypeId, &wrapped) != 0) {
                            return -1;
                        }
                    } else if (invokeArgs[paramCount].s.bytes == NULL) {
                        return -1;
                    } else if (
                        HOPTCConstEvalSetOptionalSomeValue(
                            c,
                            paramTypeId,
                            (const HOPCTFEValue*)invokeArgs[paramCount].s.bytes,
                            &wrapped)
                        != 0)
                    {
                        return -1;
                    }
                } else if (invokeArgs[paramCount].kind == HOPCTFEValue_NULL) {
                    if (HOPTCConstEvalSetOptionalNoneValue(c, paramTypeId, &wrapped) != 0) {
                        return -1;
                    }
                } else if (
                    HOPTCConstEvalSetOptionalSomeValue(
                        c, paramTypeId, &invokeArgs[paramCount], &wrapped)
                    != 0)
                {
                    return -1;
                }
                paramBindings[paramCount].value = wrapped;
            } else {
                paramBindings[paramCount].value = invokeArgs[paramCount];
            }
            paramCount++;
        }
        child = HOPAstNextSibling(c->ast, child);
    }

    savedExecCtx = evalCtx->execCtx;
    savedActiveConstEvalCtx = c->activeConstEvalCtx;
    savedDepth = evalCtx->fnDepth;
    savedCallArgs = evalCtx->callArgs;
    savedCallArgCount = evalCtx->callArgCount;
    savedCallBinding = evalCtx->callBinding;
    savedCallPackParamNameStart = evalCtx->callPackParamNameStart;
    savedCallPackParamNameEnd = evalCtx->callPackParamNameEnd;

    paramFrame.parent = NULL;
    paramFrame.bindings = paramBindings;
    paramFrame.bindingLen = argCount;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = c->arena;
    execCtx.ast = c->ast;
    execCtx.src = c->src;
    execCtx.diag = c->diag;
    execCtx.env = &paramFrame;
    execCtx.evalExpr = HOPTCEvalConstExecExprCb;
    execCtx.evalExprCtx = evalCtx;
    execCtx.resolveType = HOPTCEvalConstExecResolveTypeCb;
    execCtx.resolveTypeCtx = evalCtx;
    execCtx.inferValueType = HOPTCEvalConstExecInferValueTypeCb;
    execCtx.inferValueTypeCtx = evalCtx;
    execCtx.inferExprType = HOPTCEvalConstExecInferExprTypeCb;
    execCtx.inferExprTypeCtx = evalCtx;
    execCtx.isOptionalType = HOPTCEvalConstExecIsOptionalTypeCb;
    execCtx.isOptionalTypeCtx = evalCtx;
    execCtx.forInIndex = HOPTCEvalConstForInIndexCb;
    execCtx.forInIndexCtx = evalCtx;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = HOPTC_CONST_FOR_MAX_ITERS;
    HOPCTFEExecResetReason(&execCtx);
    evalCtx->execCtx = &execCtx;
    evalCtx->fnStack[evalCtx->fnDepth++] = fnIndex;
    evalCtx->callArgs = callArgs;
    evalCtx->callArgCount = callArgCount;
    evalCtx->callBinding = callBinding;
    evalCtx->callPackParamNameStart = callPackParamNameStart;
    evalCtx->callPackParamNameEnd = callPackParamNameEnd;
    evalCtx->nonConstReason = NULL;
    evalCtx->nonConstStart = 0;
    evalCtx->nonConstEnd = 0;
    c->activeConstEvalCtx = evalCtx;

    if (c->funcs[fnIndex].returnType == c->typeType) {
        rc = HOPTCConstEvalTryDirectReturnFunction(evalCtx, bodyNode, &retValue, &isConst);
        if (rc <= 0) {
            evalCtx->fnDepth = savedDepth;
            evalCtx->execCtx = savedExecCtx;
            evalCtx->callArgs = savedCallArgs;
            evalCtx->callArgCount = savedCallArgCount;
            evalCtx->callBinding = savedCallBinding;
            evalCtx->callPackParamNameStart = savedCallPackParamNameStart;
            evalCtx->callPackParamNameEnd = savedCallPackParamNameEnd;
            c->activeConstEvalCtx = savedActiveConstEvalCtx;
            if (rc != 0) {
                return -1;
            }
            *outValue = retValue;
            *outIsConst = isConst;
            return 0;
        }
    }

    rc = HOPTCTryMirConstCall(
        evalCtx, fnIndex, invokeArgs, argCount, &retValue, &didReturn, &isConst, &mirSupported);
    evalCtx->fnDepth = savedDepth;
    evalCtx->execCtx = savedExecCtx;
    evalCtx->callArgs = savedCallArgs;
    evalCtx->callArgCount = savedCallArgCount;
    evalCtx->callBinding = savedCallBinding;
    evalCtx->callPackParamNameStart = savedCallPackParamNameStart;
    evalCtx->callPackParamNameEnd = savedCallPackParamNameEnd;
    c->activeConstEvalCtx = savedActiveConstEvalCtx;
    if (rc != 0) {
        return -1;
    }
    if (!mirSupported || !isConst) {
        if (evalCtx->nonConstReason == NULL) {
            HOPTCConstSetReasonNode(evalCtx, bodyNode, "function body is not const-evaluable");
        }
        *outIsConst = 0;
        return 0;
    }
    if (!didReturn) {
        if (c->funcs[fnIndex].returnType == c->typeVoid) {
            HOPTCConstEvalSetNullValue(outValue);
            *outIsConst = 1;
            return 0;
        }
        if (execCtx.nonConstReason != NULL) {
            evalCtx->nonConstReason = execCtx.nonConstReason;
            evalCtx->nonConstStart = execCtx.nonConstStart;
            evalCtx->nonConstEnd = execCtx.nonConstEnd;
        }
        HOPTCConstSetReasonNode(
            evalCtx, bodyNode, "const-evaluable function must produce a const return value");
        *outIsConst = 0;
        return 0;
    }
    if (fnIndex >= 0 && (uint32_t)fnIndex < c->funcLen) {
        int32_t returnTypeId = c->funcs[fnIndex].returnType;
        int32_t returnBaseTypeId = HOPTCResolveAliasBaseType(c, returnTypeId);
        if (returnBaseTypeId >= 0 && (uint32_t)returnBaseTypeId < c->typeLen
            && c->types[returnBaseTypeId].kind == HOPTCType_OPTIONAL)
        {
            HOPCTFEValue wrapped;
            if (retValue.kind == HOPCTFEValue_OPTIONAL) {
                if (retValue.typeTag > 0 && retValue.typeTag <= (uint64_t)INT32_MAX
                    && (uint32_t)retValue.typeTag < c->typeLen
                    && (int32_t)retValue.typeTag == returnBaseTypeId)
                {
                    wrapped = retValue;
                } else if (retValue.b == 0u) {
                    if (HOPTCConstEvalSetOptionalNoneValue(c, returnBaseTypeId, &wrapped) != 0) {
                        return -1;
                    }
                } else if (retValue.s.bytes == NULL) {
                    return -1;
                } else if (
                    HOPTCConstEvalSetOptionalSomeValue(
                        c, returnBaseTypeId, (const HOPCTFEValue*)retValue.s.bytes, &wrapped)
                    != 0)
                {
                    return -1;
                }
            } else if (retValue.kind == HOPCTFEValue_NULL) {
                if (HOPTCConstEvalSetOptionalNoneValue(c, returnBaseTypeId, &wrapped) != 0) {
                    return -1;
                }
            } else if (
                HOPTCConstEvalSetOptionalSomeValue(c, returnBaseTypeId, &retValue, &wrapped) != 0)
            {
                return -1;
            }
            retValue = wrapped;
        }
    }
    *outValue = retValue;
    *outIsConst = 1;
    return 0;
}

int HOPTCResolveConstCallMir(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* args,
    uint32_t            argCount,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPTCConstEvalCtx* evalCtx = (HOPTCConstEvalCtx*)ctx;
    HOPTypeCheckCtx*   c;
    int32_t            callNode = -1;
    int32_t            callCalleeNode = -1;
    int32_t            fnIndex;
    HOPTCCallArgInfo   invokeCallArgs[HOPTC_MAX_CALL_ARGS];
    HOPTCCallBinding   invokeBinding;
    uint32_t           invokeCallArgCount = 0;
    uint32_t           invokePackParamNameStart = 0;
    uint32_t           invokePackParamNameEnd = 0;
    int                invokeHasCallContext = 0;

    (void)function;
    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (HOPTCMirConstResolveCallNode(evalCtx, program, inst, &callNode)) {
        if (callNode >= 0 && (uint32_t)callNode < c->ast->len) {
            callCalleeNode = HOPAstFirstChild(c->ast, callNode);
        }
        int status = HOPTCConstEvalLenCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        status = HOPTCConstEvalCompilerDiagCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        status = HOPTCConstEvalSourceLocationOfCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        status = HOPTCConstEvalTypeReflectionCall(evalCtx, callNode, outValue, outIsConst);
        if (status == 0) {
            return 0;
        }
        if (status < 0) {
            return -1;
        }
        if (callNode >= 0) {
            int32_t calleeNode = HOPAstFirstChild(c->ast, callNode);
            if (calleeNode >= 0 && c->ast->nodes[calleeNode].kind == HOPAst_IDENT
                && HOPNameEqLiteral(
                    c->src,
                    c->ast->nodes[calleeNode].dataStart,
                    c->ast->nodes[calleeNode].dataEnd,
                    "typeof"))
            {
                int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
                int32_t argExprNode = argNode;
                if (argCount == 1u) {
                    int      isCurrentPackIndexExpr = 0;
                    uint32_t trackedAnyPackArgIndex = 0;
                    int32_t  argTypeId = -1;
                    int      packStatus = -1;
                    if (argExprNode >= 0 && (uint32_t)argExprNode < c->ast->len
                        && c->ast->nodes[argExprNode].kind == HOPAst_CALL_ARG)
                    {
                        int32_t innerArgExprNode = HOPAstFirstChild(c->ast, argExprNode);
                        if (innerArgExprNode >= 0) {
                            argExprNode = innerArgExprNode;
                        }
                    }
                    if (argExprNode >= 0 && (uint32_t)argExprNode < c->ast->len
                        && c->ast->nodes[argExprNode].kind == HOPAst_INDEX
                        && (c->ast->nodes[argExprNode].flags & HOPAstFlag_INDEX_SLICE) == 0u)
                    {
                        int32_t baseNode = HOPAstFirstChild(c->ast, argExprNode);
                        if (baseNode >= 0 && (uint32_t)baseNode < c->ast->len
                            && c->ast->nodes[baseNode].kind == HOPAst_IDENT
                            && HOPTCConstEvalIsTrackedAnyPackName(
                                evalCtx,
                                c->ast->nodes[baseNode].dataStart,
                                c->ast->nodes[baseNode].dataEnd))
                        {
                            isCurrentPackIndexExpr = 1;
                        }
                    }
                    if (isCurrentPackIndexExpr) {
                        return HOPTCConstEvalTypeOf(evalCtx, callNode, outValue, outIsConst);
                    }
                    packStatus = HOPTCConstEvalResolveTrackedAnyPackArgIndex(
                        evalCtx, argExprNode, &trackedAnyPackArgIndex);
                    if (packStatus < 0) {
                        return -1;
                    }
                    if (packStatus == 0 || packStatus == 2 || packStatus == 3) {
                        return HOPTCConstEvalTypeOf(evalCtx, callNode, outValue, outIsConst);
                    }
                    if (HOPTCEvalConstExecInferValueTypeCb(evalCtx, &args[0], &argTypeId) == 0) {
                        outValue->kind = HOPCTFEValue_TYPE;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->b = 0;
                        outValue->typeTag = HOPTCEncodeTypeTag(c, argTypeId);
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->span.fileBytes = NULL;
                        outValue->span.fileLen = 0;
                        outValue->span.startLine = 0;
                        outValue->span.startColumn = 0;
                        outValue->span.endLine = 0;
                        outValue->span.endColumn = 0;
                        *outIsConst = 1;
                        return 0;
                    }
                }
                return HOPTCConstEvalTypeOf(evalCtx, callNode, outValue, outIsConst);
            }
        }
    }

    if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "typeof")) {
        if (argCount == 1u) {
            int32_t argTypeId = -1;
            if (HOPTCEvalConstExecInferValueTypeCb(evalCtx, &args[0], &argTypeId) == 0) {
                if ((argTypeId == c->typeUntypedInt || argTypeId == c->typeUntypedFloat)
                    && HOPTCConcretizeInferredType(c, argTypeId, &argTypeId) != 0)
                {
                    return -1;
                }
                outValue->kind = HOPCTFEValue_TYPE;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = HOPTCEncodeTypeTag(c, argTypeId);
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->span.fileBytes = NULL;
                outValue->span.fileLen = 0;
                outValue->span.startLine = 0;
                outValue->span.startColumn = 0;
                outValue->span.endLine = 0;
                outValue->span.endColumn = 0;
                *outIsConst = 1;
                return 0;
            }
        }
        HOPTCConstSetReason(evalCtx, nameStart, nameEnd, "typeof() operand is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    {
        int reflectArgStatus = HOPTCConstEvalTypeReflectionByArgs(
            evalCtx, nameStart, nameEnd, args, argCount, outValue, outIsConst);
        if (reflectArgStatus == 0) {
            return 0;
        }
        if (reflectArgStatus < 0) {
            return -1;
        }
    }

    if (args != NULL && argCount > 0 && args[0].kind == HOPCTFEValue_SPAN
        && args[0].typeTag == HOP_TC_MIR_IMPORT_ALIAS_TAG && args[0].span.fileBytes != NULL
        && args[0].span.fileLen > 0)
    {
        const uint8_t* srcBytes = (const uint8_t*)c->src.ptr;
        const uint8_t* aliasPtr = args[0].span.fileBytes;
        uint32_t       aliasStart;
        uint32_t       aliasEnd;
        if (aliasPtr >= srcBytes && (uint64_t)(aliasPtr - srcBytes) <= UINT32_MAX) {
            aliasStart = (uint32_t)(aliasPtr - srcBytes);
            aliasEnd = aliasStart + args[0].span.fileLen;
            if (aliasEnd <= c->src.len) {
                fnIndex = HOPTCFindPkgConstCallableFunction(
                    c, aliasStart, aliasEnd, nameStart, nameEnd, argCount - 1u);
                if (fnIndex >= 0) {
                    return HOPTCInvokeConstFunctionByIndex(
                        evalCtx,
                        nameStart,
                        nameEnd,
                        fnIndex,
                        args + 1u,
                        argCount - 1u,
                        NULL,
                        0u,
                        NULL,
                        0u,
                        0u,
                        outValue,
                        outIsConst);
                }
            }
        }
    }

    {
        HOPCTFEValue calleeValue;
        int          calleeIsConst = 0;
        uint32_t     calleeFnIndex = UINT32_MAX;
        const char*  savedReason = evalCtx->nonConstReason;
        uint32_t     savedStart = evalCtx->nonConstStart;
        uint32_t     savedEnd = evalCtx->nonConstEnd;
        if (HOPTCResolveConstIdent(
                evalCtx, nameStart, nameEnd, &calleeValue, &calleeIsConst, c->diag)
            != 0)
        {
            return -1;
        }
        if (calleeIsConst && HOPMirValueAsFunctionRef(&calleeValue, &calleeFnIndex)
            && calleeFnIndex < c->funcLen)
        {
            const HOPTCFunction* fn = &c->funcs[calleeFnIndex];
            if (callNode >= 0 && callCalleeNode >= 0
                && HOPTCConstEvalPrepareInvokeCallContext(
                       evalCtx,
                       callNode,
                       callCalleeNode,
                       (int32_t)calleeFnIndex,
                       invokeCallArgs,
                       &invokeCallArgCount,
                       &invokeBinding,
                       &invokePackParamNameStart,
                       &invokePackParamNameEnd)
                       == 0)
            {
                invokeHasCallContext = 1;
            }
            return HOPTCInvokeConstFunctionByIndex(
                evalCtx,
                fn->nameStart,
                fn->nameEnd,
                (int32_t)calleeFnIndex,
                args,
                argCount,
                invokeHasCallContext ? invokeCallArgs : NULL,
                invokeHasCallContext ? invokeCallArgCount : 0u,
                invokeHasCallContext ? &invokeBinding : NULL,
                invokeHasCallContext ? invokePackParamNameStart : 0u,
                invokeHasCallContext ? invokePackParamNameEnd : 0u,
                outValue,
                outIsConst);
        }
        evalCtx->nonConstReason = savedReason;
        evalCtx->nonConstStart = savedStart;
        evalCtx->nonConstEnd = savedEnd;
    }

    if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "len")) {
        if (argCount == 1u) {
            if (args[0].kind == HOPCTFEValue_STRING) {
                outValue->kind = HOPCTFEValue_INT;
                outValue->i64 = (int64_t)args[0].s.len;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->span.fileBytes = NULL;
                outValue->span.fileLen = 0;
                outValue->span.startLine = 0;
                outValue->span.startColumn = 0;
                outValue->span.endLine = 0;
                outValue->span.endColumn = 0;
                *outIsConst = 1;
                return 0;
            }
            if (args[0].kind == HOPCTFEValue_TYPE) {
                int32_t typeId = -1;
                int32_t baseType;
                if (HOPTCDecodeTypeTag(c, args[0].typeTag, &typeId) == 0) {
                    baseType = HOPTCResolveAliasBaseType(c, typeId);
                    if (baseType >= 0 && (uint32_t)baseType < c->typeLen) {
                        const HOPTCType* t = &c->types[baseType];
                        if (t->kind == HOPTCType_PACK) {
                            outValue->kind = HOPCTFEValue_INT;
                            outValue->i64 = (int64_t)t->fieldCount;
                            outValue->f64 = 0.0;
                            outValue->b = 0;
                            outValue->typeTag = 0;
                            outValue->s.bytes = NULL;
                            outValue->s.len = 0;
                            outValue->span.fileBytes = NULL;
                            outValue->span.fileLen = 0;
                            outValue->span.startLine = 0;
                            outValue->span.startColumn = 0;
                            outValue->span.endLine = 0;
                            outValue->span.endColumn = 0;
                            *outIsConst = 1;
                            return 0;
                        }
                        if (t->kind == HOPTCType_ARRAY) {
                            outValue->kind = HOPCTFEValue_INT;
                            outValue->i64 = (int64_t)t->arrayLen;
                            outValue->f64 = 0.0;
                            outValue->b = 0;
                            outValue->typeTag = 0;
                            outValue->s.bytes = NULL;
                            outValue->s.len = 0;
                            outValue->span.fileBytes = NULL;
                            outValue->span.fileLen = 0;
                            outValue->span.startLine = 0;
                            outValue->span.startColumn = 0;
                            outValue->span.endLine = 0;
                            outValue->span.endColumn = 0;
                            *outIsConst = 1;
                            return 0;
                        }
                    }
                }
            }
        }
        HOPTCConstSetReason(evalCtx, nameStart, nameEnd, "len() operand is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    fnIndex = HOPTCFindConstCallableFunction(c, nameStart, nameEnd, argCount);
    if (fnIndex < 0) {
        HOPTCConstSetReason(
            evalCtx,
            nameStart,
            nameEnd,
            "call target is not a const-evaluable function for these arguments");
        *outIsConst = 0;
        return 0;
    }
    if (callNode >= 0 && callCalleeNode >= 0
        && HOPTCConstEvalPrepareInvokeCallContext(
               evalCtx,
               callNode,
               callCalleeNode,
               fnIndex,
               invokeCallArgs,
               &invokeCallArgCount,
               &invokeBinding,
               &invokePackParamNameStart,
               &invokePackParamNameEnd)
               == 0)
    {
        invokeHasCallContext = 1;
    }
    return HOPTCInvokeConstFunctionByIndex(
        evalCtx,
        nameStart,
        nameEnd,
        fnIndex,
        args,
        argCount,
        invokeHasCallContext ? invokeCallArgs : NULL,
        invokeHasCallContext ? invokeCallArgCount : 0u,
        invokeHasCallContext ? &invokeBinding : NULL,
        invokeHasCallContext ? invokePackParamNameStart : 0u,
        invokeHasCallContext ? invokePackParamNameEnd : 0u,
        outValue,
        outIsConst);
}

int HOPTCResolveConstCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    return HOPTCResolveConstCallMir(
        ctx, NULL, NULL, NULL, nameStart, nameEnd, args, argCount, outValue, outIsConst, diag);
}

static int HOPTCConstEvalDirectCall(
    HOPTCConstEvalCtx* evalCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPTypeCheckCtx*  c;
    int32_t           calleeNode;
    const HOPAstNode* callee;
    HOPCTFEValue      calleeValue;
    int               calleeIsConst = 0;
    uint32_t          calleeFnIndex = UINT32_MAX;
    int32_t           argNode;
    uint32_t          argCount = 0;
    uint32_t          argIndex = 0;
    HOPCTFEValue*     argValues = NULL;
    HOPTCCallArgInfo  invokeCallArgs[HOPTC_MAX_CALL_ARGS];
    HOPTCCallBinding  invokeBinding;
    uint32_t          invokeCallArgCount = 0;
    uint32_t          invokePackParamNameStart = 0;
    uint32_t          invokePackParamNameEnd = 0;
    int               invokeHasCallContext = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = HOPAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL || callee->kind != HOPAst_IDENT) {
        return 1;
    }

    argNode = HOPAstNextSibling(c->ast, calleeNode);
    while (argNode >= 0) {
        argCount++;
        argNode = HOPAstNextSibling(c->ast, argNode);
    }
    if (argCount > 0) {
        argValues = (HOPCTFEValue*)HOPArenaAlloc(
            c->arena, sizeof(HOPCTFEValue) * argCount, (uint32_t)_Alignof(HOPCTFEValue));
        if (argValues == NULL) {
            return HOPTCFailNode(c, exprNode, HOPDiag_ARENA_OOM);
        }
    }

    argNode = HOPAstNextSibling(c->ast, calleeNode);
    while (argNode >= 0) {
        int32_t exprArgNode = argNode;
        int     argIsConst = 0;
        if (argIndex >= argCount) {
            return -1;
        }
        if (c->ast->nodes[argNode].kind == HOPAst_CALL_ARG) {
            exprArgNode = HOPAstFirstChild(c->ast, argNode);
            if (exprArgNode < 0) {
                return -1;
            }
        }
        if (HOPTCEvalConstExprNode(evalCtx, exprArgNode, &argValues[argIndex], &argIsConst) != 0) {
            return -1;
        }
        if (!argIsConst) {
            *outIsConst = 0;
            return 0;
        }
        argIndex++;
        argNode = HOPAstNextSibling(c->ast, argNode);
    }

    if (HOPTCResolveConstIdent(
            evalCtx, callee->dataStart, callee->dataEnd, &calleeValue, &calleeIsConst, c->diag)
        != 0)
    {
        return -1;
    }
    if (calleeIsConst && HOPMirValueAsFunctionRef(&calleeValue, &calleeFnIndex)
        && calleeFnIndex < c->funcLen)
    {
        const HOPTCFunction* fn = &c->funcs[calleeFnIndex];
        if (HOPTCConstEvalPrepareInvokeCallContext(
                evalCtx,
                exprNode,
                calleeNode,
                (int32_t)calleeFnIndex,
                invokeCallArgs,
                &invokeCallArgCount,
                &invokeBinding,
                &invokePackParamNameStart,
                &invokePackParamNameEnd)
            == 0)
        {
            invokeHasCallContext = 1;
        }
        return HOPTCInvokeConstFunctionByIndex(
            evalCtx,
            fn->nameStart,
            fn->nameEnd,
            (int32_t)calleeFnIndex,
            argValues,
            argCount,
            invokeHasCallContext ? invokeCallArgs : NULL,
            invokeHasCallContext ? invokeCallArgCount : 0u,
            invokeHasCallContext ? &invokeBinding : NULL,
            invokeHasCallContext ? invokePackParamNameStart : 0u,
            invokeHasCallContext ? invokePackParamNameEnd : 0u,
            outValue,
            outIsConst);
    }

    return HOPTCResolveConstCall(
        evalCtx,
        callee->dataStart,
        callee->dataEnd,
        argValues,
        argCount,
        outValue,
        outIsConst,
        c->diag);
}

int HOPTCEvalTopLevelConstNodeAt(
    HOPTypeCheckCtx*   c,
    HOPTCConstEvalCtx* evalCtx,
    int32_t            nodeId,
    int32_t            nameIndex,
    HOPCTFEValue*      outValue,
    int*               outIsConst) {
    uint8_t           state;
    int32_t           initNode;
    int               isConst = 0;
    int               mirSupported = 0;
    HOPTCVarLikeParts parts;
    if (c == NULL || evalCtx == NULL || outValue == NULL || outIsConst == NULL || nodeId < 0
        || (uint32_t)nodeId >= c->ast->len)
    {
        return -1;
    }
    if (c->constEvalState == NULL || c->constEvalValues == NULL) {
        *outIsConst = 0;
        return 0;
    }
    if (HOPTCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0 || nameIndex < 0
        || (uint32_t)nameIndex >= parts.nameCount)
    {
        *outIsConst = 0;
        return 0;
    }

    state = c->constEvalState[nodeId];
    if (state == HOPTCConstEval_READY) {
        *outValue = c->constEvalValues[nodeId];
        *outIsConst = 1;
        return 0;
    }
    if (state == HOPTCConstEval_NONCONST || state == HOPTCConstEval_VISITING) {
        if (state == HOPTCConstEval_VISITING) {
            HOPTCConstSetReasonNode(
                evalCtx, nodeId, "cyclic const dependency is not supported in const evaluation");
        }
        *outIsConst = 0;
        return 0;
    }

    c->constEvalState[nodeId] = HOPTCConstEval_VISITING;
    initNode = HOPTCVarLikeInitExprNodeAt(c, nodeId, nameIndex);
    if (initNode < 0) {
        if (HOPTCHasForeignImportDirective(c->ast, c->src, nodeId)) {
            c->constEvalState[nodeId] = HOPTCConstEval_NONCONST;
            *outIsConst = 0;
            return 0;
        }
        HOPTCConstSetReasonNode(evalCtx, nodeId, "const declaration is missing an initializer");
        c->constEvalState[nodeId] = HOPTCConstEval_NONCONST;
        *outIsConst = 0;
        return 0;
    }
    evalCtx->nonConstReason = NULL;
    evalCtx->nonConstStart = 0;
    evalCtx->nonConstEnd = 0;
    if (HOPTCTryMirTopLevelConst(evalCtx, nodeId, nameIndex, outValue, &isConst, &mirSupported)
        != 0)
    {
        c->constEvalState[nodeId] = HOPTCConstEval_UNSEEN;
        return -1;
    }
    if (mirSupported) {
        if (!isConst) {
            if (evalCtx->nonConstReason == NULL) {
                HOPTCConstSetReasonNode(
                    evalCtx, initNode, "const initializer is not const-evaluable");
            }
            c->constEvalState[nodeId] = HOPTCConstEval_NONCONST;
            *outIsConst = 0;
            return 0;
        }
        if (!parts.grouped) {
            c->constEvalValues[nodeId] = *outValue;
            c->constEvalState[nodeId] = HOPTCConstEval_READY;
        } else {
            c->constEvalState[nodeId] = HOPTCConstEval_UNSEEN;
        }
        *outIsConst = 1;
        return 0;
    }
    if (HOPTCEvalConstExprNode(evalCtx, initNode, outValue, &isConst) != 0) {
        c->constEvalState[nodeId] = HOPTCConstEval_UNSEEN;
        return -1;
    }
    if (!isConst) {
        HOPTCConstSetReasonNode(evalCtx, initNode, "const initializer is not const-evaluable");
        c->constEvalState[nodeId] = HOPTCConstEval_NONCONST;
        *outIsConst = 0;
        return 0;
    }
    if (!parts.grouped) {
        c->constEvalValues[nodeId] = *outValue;
        c->constEvalState[nodeId] = HOPTCConstEval_READY;
    } else {
        c->constEvalState[nodeId] = HOPTCConstEval_UNSEEN;
    }
    *outIsConst = 1;
    return 0;
}

int HOPTCEvalTopLevelConstNode(
    HOPTypeCheckCtx*   c,
    HOPTCConstEvalCtx* evalCtx,
    int32_t            nodeId,
    HOPCTFEValue*      outValue,
    int*               outIsConst) {
    return HOPTCEvalTopLevelConstNodeAt(c, evalCtx, nodeId, 0, outValue, outIsConst);
}

int HOPTCConstBoolExpr(HOPTypeCheckCtx* c, int32_t nodeId, int* out, int* isConst) {
    HOPTCConstEvalCtx  evalCtxStorage;
    HOPTCConstEvalCtx* evalCtx = c->activeConstEvalCtx;
    HOPCTFEValue       value;
    int                valueIsConst = 0;
    const HOPAstNode*  n;
    *isConst = 0;
    *out = 0;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == HOPAst_UNARY && (HOPTokenKind)n->op == HOPTok_NOT) {
        int32_t rhsNode = HOPAstFirstChild(c->ast, nodeId);
        int     rhsValue = 0;
        int     rhsIsConst = 0;
        if (rhsNode < 0) {
            return -1;
        }
        if (HOPTCConstBoolExpr(c, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (rhsIsConst) {
            *out = rhsValue ? 0 : 1;
            *isConst = 1;
        }
        return 0;
    }
    if (n->kind == HOPAst_BINARY
        && ((HOPTokenKind)n->op == HOPTok_EQ || (HOPTokenKind)n->op == HOPTok_NEQ))
    {
        int32_t lhsNode = HOPAstFirstChild(c->ast, nodeId);
        int32_t rhsNode = lhsNode >= 0 ? HOPAstNextSibling(c->ast, lhsNode) : -1;
        int32_t extraNode = rhsNode >= 0 ? HOPAstNextSibling(c->ast, rhsNode) : -1;
        int32_t lhsTypeId = -1;
        int32_t rhsTypeId = -1;
        int     lhsStatus;
        int     rhsStatus;
        if (lhsNode < 0 || rhsNode < 0 || extraNode >= 0) {
            return -1;
        }
        lhsStatus = HOPTCResolveReflectedTypeValueExpr(c, lhsNode, &lhsTypeId);
        if (lhsStatus < 0) {
            return -1;
        }
        rhsStatus = HOPTCResolveReflectedTypeValueExpr(c, rhsNode, &rhsTypeId);
        if (rhsStatus < 0) {
            return -1;
        }
        if (lhsStatus == 0 && rhsStatus == 0) {
            *out = (((HOPTokenKind)n->op == HOPTok_EQ)
                        ? (lhsTypeId == rhsTypeId)
                        : (lhsTypeId != rhsTypeId))
                     ? 1
                     : 0;
            *isConst = 1;
            return 0;
        }
    }
    if (evalCtx != NULL) {
        evalCtxStorage = *evalCtx;
        evalCtxStorage.tc = c;
        evalCtxStorage.nonConstReason = NULL;
        evalCtxStorage.nonConstStart = 0;
        evalCtxStorage.nonConstEnd = 0;
        evalCtx = &evalCtxStorage;
    } else {
        memset(&evalCtxStorage, 0, sizeof(evalCtxStorage));
        evalCtxStorage.tc = c;
        evalCtx = &evalCtxStorage;
    }
    if (HOPTCEvalConstExprNode(evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx->nonConstReason;
    c->lastConstEvalReasonStart = evalCtx->nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx->nonConstEnd;
    if (!valueIsConst) {
        return 0;
    }
    if (value.kind == HOPCTFEValue_OPTIONAL) {
        *out = value.b != 0u ? 1 : 0;
        *isConst = 1;
        return 0;
    }
    if (value.kind != HOPCTFEValue_BOOL) {
        c->lastConstEvalReason = "expression evaluated to a non-boolean value";
        c->lastConstEvalReasonStart = c->ast->nodes[nodeId].start;
        c->lastConstEvalReasonEnd = c->ast->nodes[nodeId].end;
        return 0;
    }
    *out = value.b ? 1 : 0;
    *isConst = 1;
    return 0;
}

int HOPTCConstIntExpr(HOPTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst) {
    HOPTCConstEvalCtx  evalCtxStorage;
    HOPTCConstEvalCtx* evalCtx = c->activeConstEvalCtx;
    HOPCTFEValue       value;
    int                valueIsConst = 0;
    *isConst = 0;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    if (evalCtx != NULL) {
        evalCtxStorage = *evalCtx;
        evalCtxStorage.tc = c;
        evalCtxStorage.nonConstReason = NULL;
        evalCtxStorage.nonConstStart = 0;
        evalCtxStorage.nonConstEnd = 0;
        evalCtx = &evalCtxStorage;
    } else {
        memset(&evalCtxStorage, 0, sizeof(evalCtxStorage));
        evalCtxStorage.tc = c;
        evalCtx = &evalCtxStorage;
    }
    if (HOPTCEvalConstExprNode(evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx->nonConstReason;
    c->lastConstEvalReasonStart = evalCtx->nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx->nonConstEnd;
    if (!valueIsConst) {
        return 0;
    }
    if (HOPCTFEValueToInt64(&value, out) != 0) {
        c->lastConstEvalReason = "expression evaluated to a non-integer value";
        c->lastConstEvalReasonStart = c->ast->nodes[nodeId].start;
        c->lastConstEvalReasonEnd = c->ast->nodes[nodeId].end;
        return 0;
    }
    *isConst = 1;
    return 0;
}

int HOPTCConstFloatExpr(HOPTypeCheckCtx* c, int32_t nodeId, double* out, int* isConst) {
    HOPTCConstEvalCtx evalCtx;
    HOPCTFEValue      value;
    int               valueIsConst = 0;
    *isConst = 0;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (HOPTCEvalConstExprNode(&evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    if (!valueIsConst) {
        return 0;
    }
    if (value.kind == HOPCTFEValue_FLOAT) {
        *out = value.f64;
        *isConst = 1;
        return 0;
    }
    if (value.kind == HOPCTFEValue_INT) {
        *out = (double)value.i64;
        *isConst = 1;
        return 0;
    }
    c->lastConstEvalReason = "expression evaluated to a non-float value";
    c->lastConstEvalReasonStart = c->ast->nodes[nodeId].start;
    c->lastConstEvalReasonEnd = c->ast->nodes[nodeId].end;
    return 0;
}

int HOPTCConstStringExpr(
    HOPTypeCheckCtx* c,
    int32_t          nodeId,
    const uint8_t**  outBytes,
    uint32_t*        outLen,
    int*             outIsConst) {
    const HOPAstNode* node;
    HOPTCConstEvalCtx evalCtx;
    HOPCTFEValue      value;
    int               valueIsConst = 0;
    if (outBytes == NULL || outLen == NULL || outIsConst == NULL) {
        return -1;
    }
    *outBytes = NULL;
    *outLen = 0;
    *outIsConst = 0;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    node = &c->ast->nodes[nodeId];
    while (node->kind == HOPAst_CALL_ARG) {
        nodeId = HOPAstFirstChild(c->ast, nodeId);
        if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
            return -1;
        }
        node = &c->ast->nodes[nodeId];
    }
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (HOPTCEvalConstExprNode(&evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    if (!valueIsConst || value.kind != HOPCTFEValue_STRING) {
        return 0;
    }
    *outBytes = value.s.bytes;
    *outLen = value.s.len;
    *outIsConst = 1;
    return 0;
}

void HOPTCMarkRuntimeBoundsCheck(HOPTypeCheckCtx* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    ((HOPAstNode*)&c->ast->nodes[nodeId])->flags |= HOPAstFlag_INDEX_RUNTIME_BOUNDS;
}

HOP_API_END
