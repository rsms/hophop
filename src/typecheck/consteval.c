#include "internal.h"
#include "../mir_exec.h"
#include "../mir_lower_pkg.h"
#include "../mir_lower_stmt.h"

SL_API_BEGIN

enum {
    SL_TC_MIR_TUPLE_TAG = 0x54434d4952545550ULL,
    SL_TC_MIR_ITER_TAG = 0x54434d4952495445ULL,
    SL_TC_MIR_IMPORT_ALIAS_TAG = 0x54434d49524d504bULL,
};

static int SLTCConstEvalResolveTrackedAnyPackArgIndex(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outCallArgIndex);
static int SLTCConstEvalDirectCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int SLTCConstEvalSetOptionalNoneValue(
    SLTypeCheckCtx* c, int32_t optionalTypeId, SLCTFEValue* outValue);
static int SLTCConstEvalSetOptionalSomeValue(
    SLTypeCheckCtx* c, int32_t optionalTypeId, const SLCTFEValue* payload, SLCTFEValue* outValue);
static int SLTCInvokeConstFunctionByIndex(
    SLTCConstEvalCtx*  evalCtx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    int32_t            fnIndex,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst);

void SLTCConstSetReason(
    SLTCConstEvalCtx* evalCtx, uint32_t start, uint32_t end, const char* reason) {
    if (evalCtx == NULL || reason == NULL || reason[0] == '\0' || evalCtx->nonConstReason != NULL) {
        return;
    }
    evalCtx->nonConstReason = reason;
    evalCtx->nonConstStart = start;
    evalCtx->nonConstEnd = end;
    if (evalCtx->execCtx != NULL) {
        SLCTFEExecSetReason(evalCtx->execCtx, start, end, reason);
    }
}

void SLTCConstSetReasonNode(SLTCConstEvalCtx* evalCtx, int32_t nodeId, const char* reason) {
    SLTypeCheckCtx* c;
    if (evalCtx == NULL) {
        return;
    }
    c = evalCtx->tc;
    if (c != NULL && nodeId >= 0 && (uint32_t)nodeId < c->ast->len) {
        SLTCConstSetReason(evalCtx, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, reason);
        return;
    }
    SLTCConstSetReason(evalCtx, 0, 0, reason);
}

void SLTCAttachConstEvalReason(SLTypeCheckCtx* c) {
    if (c == NULL || c->diag == NULL || c->lastConstEvalReason == NULL
        || c->lastConstEvalReason[0] == '\0')
    {
        return;
    }
    c->diag->detail = SLTCAllocDiagText(c, c->lastConstEvalReason);
}

static void SLTCConstEvalValueInvalid(SLCTFEValue* v) {
    if (v == NULL) {
        return;
    }
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

static int SLTCConstEvalValueToF64(const SLCTFEValue* value, double* out) {
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

static int SLTCConstEvalStringEq(const SLCTFEString* a, const SLCTFEString* b) {
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

static int SLTCConstEvalStringConcat(
    SLTypeCheckCtx* c, const SLCTFEString* a, const SLCTFEString* b, SLCTFEString* out) {
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
    bytes = (uint8_t*)SLArenaAlloc(c->arena, totalLen, (uint32_t)_Alignof(uint8_t));
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

static int SLTCConstEvalAddI64(int64_t a, int64_t b, int64_t* out) {
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

static int SLTCConstEvalSubI64(int64_t a, int64_t b, int64_t* out) {
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

static int SLTCConstEvalMulI64(int64_t a, int64_t b, int64_t* out) {
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

static int SLTCConstEvalApplyUnary(
    SLTokenKind op, const SLCTFEValue* inValue, SLCTFEValue* outValue) {
    SLTCConstEvalValueInvalid(outValue);
    if (inValue == NULL) {
        return 0;
    }
    if (op == SLTok_ADD && inValue->kind == SLCTFEValue_INT) {
        *outValue = *inValue;
        return 1;
    }
    if (op == SLTok_ADD && inValue->kind == SLCTFEValue_FLOAT) {
        *outValue = *inValue;
        return 1;
    }
    if (op == SLTok_SUB && inValue->kind == SLCTFEValue_INT) {
        if (inValue->i64 == INT64_MIN) {
            return 0;
        }
        outValue->kind = SLCTFEValue_INT;
        outValue->i64 = -inValue->i64;
        return 1;
    }
    if (op == SLTok_SUB && inValue->kind == SLCTFEValue_FLOAT) {
        outValue->kind = SLCTFEValue_FLOAT;
        outValue->f64 = -inValue->f64;
        return 1;
    }
    if (op == SLTok_NOT && inValue->kind == SLCTFEValue_BOOL) {
        outValue->kind = SLCTFEValue_BOOL;
        outValue->b = inValue->b ? 0u : 1u;
        return 1;
    }
    return 0;
}

static int SLTCConstEvalApplyBinary(
    SLTypeCheckCtx*    c,
    SLTokenKind        op,
    const SLCTFEValue* lhs,
    const SLCTFEValue* rhs,
    SLCTFEValue*       outValue) {
    int64_t i;
    double  lhsF64;
    double  rhsF64;
    SLTCConstEvalValueInvalid(outValue);
    if (c == NULL || lhs == NULL || rhs == NULL) {
        return 0;
    }

    if (lhs->kind == SLCTFEValue_INT && rhs->kind == SLCTFEValue_INT) {
        switch (op) {
            case SLTok_ADD:
                if (SLTCConstEvalAddI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case SLTok_SUB:
                if (SLTCConstEvalSubI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case SLTok_MUL:
                if (SLTCConstEvalMulI64(lhs->i64, rhs->i64, &i) != 0) {
                    return 0;
                }
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = i;
                return 1;
            case SLTok_DIV:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = lhs->i64 / rhs->i64;
                return 1;
            case SLTok_MOD:
                if (rhs->i64 == 0 || (lhs->i64 == INT64_MIN && rhs->i64 == -1)) {
                    return 0;
                }
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = lhs->i64 % rhs->i64;
                return 1;
            case SLTok_AND:
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = lhs->i64 & rhs->i64;
                return 1;
            case SLTok_OR:
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = lhs->i64 | rhs->i64;
                return 1;
            case SLTok_XOR:
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = lhs->i64 ^ rhs->i64;
                return 1;
            case SLTok_LSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = (int64_t)((uint64_t)lhs->i64 << (uint32_t)rhs->i64);
                return 1;
            case SLTok_RSHIFT:
                if (rhs->i64 < 0 || rhs->i64 > 63 || lhs->i64 < 0) {
                    return 0;
                }
                outValue->kind = SLCTFEValue_INT;
                outValue->i64 = (int64_t)((uint64_t)lhs->i64 >> (uint32_t)rhs->i64);
                return 1;
            case SLTok_EQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->i64 == rhs->i64;
                return 1;
            case SLTok_NEQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->i64 != rhs->i64;
                return 1;
            case SLTok_LT:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->i64 < rhs->i64;
                return 1;
            case SLTok_GT:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->i64 > rhs->i64;
                return 1;
            case SLTok_LTE:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->i64 <= rhs->i64;
                return 1;
            case SLTok_GTE:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->i64 >= rhs->i64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == SLCTFEValue_BOOL && rhs->kind == SLCTFEValue_BOOL) {
        switch (op) {
            case SLTok_LOGICAL_AND:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->b && rhs->b;
                return 1;
            case SLTok_LOGICAL_OR:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->b || rhs->b;
                return 1;
            case SLTok_EQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->b == rhs->b;
                return 1;
            case SLTok_NEQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->b != rhs->b;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == SLCTFEValue_STRING && rhs->kind == SLCTFEValue_STRING) {
        switch (op) {
            case SLTok_ADD:
                outValue->kind = SLCTFEValue_STRING;
                return SLTCConstEvalStringConcat(c, &lhs->s, &rhs->s, &outValue->s);
            case SLTok_EQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = SLTCConstEvalStringEq(&lhs->s, &rhs->s);
                return 1;
            case SLTok_NEQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = !SLTCConstEvalStringEq(&lhs->s, &rhs->s);
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == SLCTFEValue_TYPE && rhs->kind == SLCTFEValue_TYPE) {
        switch (op) {
            case SLTok_EQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->typeTag == rhs->typeTag;
                return 1;
            case SLTok_NEQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhs->typeTag != rhs->typeTag;
                return 1;
            default: return 0;
        }
    }

    if (SLTCConstEvalValueToF64(lhs, &lhsF64) && SLTCConstEvalValueToF64(rhs, &rhsF64)) {
        switch (op) {
            case SLTok_ADD:
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->f64 = lhsF64 + rhsF64;
                return 1;
            case SLTok_SUB:
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->f64 = lhsF64 - rhsF64;
                return 1;
            case SLTok_MUL:
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->f64 = lhsF64 * rhsF64;
                return 1;
            case SLTok_DIV:
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->f64 = lhsF64 / rhsF64;
                return 1;
            case SLTok_EQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhsF64 == rhsF64;
                return 1;
            case SLTok_NEQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhsF64 != rhsF64;
                return 1;
            case SLTok_LT:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhsF64 < rhsF64;
                return 1;
            case SLTok_GT:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhsF64 > rhsF64;
                return 1;
            case SLTok_LTE:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhsF64 <= rhsF64;
                return 1;
            case SLTok_GTE:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = lhsF64 >= rhsF64;
                return 1;
            default: return 0;
        }
    }

    if (lhs->kind == SLCTFEValue_NULL && rhs->kind == SLCTFEValue_NULL) {
        switch (op) {
            case SLTok_EQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = 1;
                return 1;
            case SLTok_NEQ:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = 0;
                return 1;
            default: return 0;
        }
    }

    return 0;
}

int SLTCResolveConstIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*   c;
    int32_t           nodeId;
    int32_t           nameIndex = -1;
    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (evalCtx->execCtx != NULL
        && SLCTFEExecEnvLookup(evalCtx->execCtx, nameStart, nameEnd, outValue))
    {
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t localIdx = SLTCLocalFind(c, nameStart, nameEnd);
        if (localIdx >= 0) {
            SLTCLocal* local = &c->locals[localIdx];
            int32_t    localType = local->typeId;
            int32_t    resolvedLocalType = SLTCResolveAliasBaseType(c, localType);
            if (resolvedLocalType >= 0 && (uint32_t)resolvedLocalType < c->typeLen
                && c->types[resolvedLocalType].kind == SLTCType_PACK)
            {
                outValue->kind = SLCTFEValue_TYPE;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = SLTCEncodeTypeTag(c, localType);
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
            if ((local->flags & SLTCLocalFlag_CONST) != 0 && local->initExprNode != -1) {
                int32_t initExprNode = local->initExprNode;
                int     evalIsConst = 0;
                int     rc;
                if (initExprNode == -2) {
                    SLTCConstSetReason(
                        evalCtx, nameStart, nameEnd, "const local initializer is recursive");
                    *outIsConst = 0;
                    return 0;
                }
                local->initExprNode = -2;
                rc = SLTCEvalConstExprNode(evalCtx, initExprNode, outValue, &evalIsConst);
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
        int32_t fnIdx = SLTCFindPlainFunctionValueIndex(c, nameStart, nameEnd);
        if (fnIdx >= 0) {
            SLMirValueSetFunctionRef(outValue, (uint32_t)fnIdx);
            *outIsConst = 1;
            return 0;
        }
    }
    if (SLTCHasImportAlias(c, nameStart, nameEnd)) {
        SLTCConstEvalValueInvalid(outValue);
        outValue->kind = SLCTFEValue_SPAN;
        outValue->typeTag = SL_TC_MIR_IMPORT_ALIAS_TAG;
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
        int32_t typeId = SLTCResolveTypeValueName(c, nameStart, nameEnd);
        if (typeId >= 0) {
            outValue->kind = SLCTFEValue_TYPE;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = 0;
            outValue->typeTag = SLTCEncodeTypeTag(c, typeId);
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
    nodeId = SLTCFindTopLevelVarLikeNode(c, nameStart, nameEnd, &nameIndex);
    if (nodeId < 0 || c->ast->nodes[nodeId].kind != SLAst_CONST) {
        SLTCConstSetReason(
            evalCtx, nameStart, nameEnd, "identifier is not a const value in this context");
        *outIsConst = 0;
        return 0;
    }
    return SLTCEvalTopLevelConstNodeAt(c, evalCtx, nodeId, nameIndex, outValue, outIsConst);
}

int SLTCConstLookupExecBindingType(
    SLTCConstEvalCtx* evalCtx, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    const SLCTFEExecEnv* frame;
    if (evalCtx == NULL || evalCtx->execCtx == NULL || outType == NULL) {
        return 0;
    }
    frame = evalCtx->execCtx->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            const SLCTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (SLNameEqSlice(evalCtx->tc->src, b->nameStart, b->nameEnd, nameStart, nameEnd)
                && b->typeId >= 0)
            {
                *outType = b->typeId;
                return 1;
            }
        }
        frame = frame->parent;
    }
    return 0;
}

int SLTCConstBuiltinSizeBytes(SLBuiltinKind b, uint64_t* outBytes) {
    if (outBytes == NULL) {
        return 0;
    }
    switch (b) {
        case SLBuiltin_BOOL:
        case SLBuiltin_U8:
        case SLBuiltin_I8:    *outBytes = 1u; return 1;
        case SLBuiltin_TYPE:  *outBytes = 8u; return 1;
        case SLBuiltin_U16:
        case SLBuiltin_I16:   *outBytes = 2u; return 1;
        case SLBuiltin_U32:
        case SLBuiltin_I32:
        case SLBuiltin_F32:   *outBytes = 4u; return 1;
        case SLBuiltin_U64:
        case SLBuiltin_I64:
        case SLBuiltin_F64:   *outBytes = 8u; return 1;
        case SLBuiltin_USIZE:
        case SLBuiltin_ISIZE: *outBytes = (uint64_t)sizeof(void*); return 1;
        default:              return 0;
    }
}

int SLTCConstBuiltinAlignBytes(SLBuiltinKind b, uint64_t* outAlign) {
    uint64_t size = 0;
    if (outAlign == NULL || !SLTCConstBuiltinSizeBytes(b, &size)) {
        return 0;
    }
    *outAlign = size > (uint64_t)sizeof(void*) ? (uint64_t)sizeof(void*) : size;
    if (*outAlign == 0) {
        *outAlign = 1;
    }
    return 1;
}

uint64_t SLTCConstAlignUpU64(uint64_t v, uint64_t align) {
    if (align == 0) {
        return v;
    }
    return (v + align - 1u) & ~(align - 1u);
}

int SLTCConstTypeLayout(
    SLTypeCheckCtx* c, int32_t typeId, uint64_t* outSize, uint64_t* outAlign, uint32_t depth) {
    const SLTCType* t;
    uint64_t        ptrSize = (uint64_t)sizeof(void*);
    uint64_t        usizeSize = (uint64_t)sizeof(uintptr_t);
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || outSize == NULL || outAlign == NULL
        || depth > c->typeLen)
    {
        return 0;
    }
    t = &c->types[typeId];
    switch (t->kind) {
        case SLTCType_BUILTIN:
            if (!SLTCConstBuiltinSizeBytes(t->builtin, outSize)
                || !SLTCConstBuiltinAlignBytes(t->builtin, outAlign))
            {
                if (typeId == c->typeStr) {
                    *outSize = SLTCConstAlignUpU64(usizeSize, ptrSize) + ptrSize;
                    *outAlign = ptrSize;
                    return 1;
                }
                return 0;
            }
            return 1;
        case SLTCType_UNTYPED_INT:
            *outSize = (uint64_t)sizeof(ptrdiff_t);
            *outAlign = *outSize;
            return 1;
        case SLTCType_UNTYPED_FLOAT:
            *outSize = 8u;
            *outAlign = 8u;
            return 1;
        case SLTCType_PTR:
        case SLTCType_REF:
        case SLTCType_FUNCTION:
            *outSize = ptrSize;
            *outAlign = ptrSize;
            return 1;
        case SLTCType_ARRAY: {
            uint64_t elemSize = 0;
            uint64_t elemAlign = 0;
            if (!SLTCConstTypeLayout(c, t->baseType, &elemSize, &elemAlign, depth + 1u)) {
                return 0;
            }
            if (t->arrayLen > 0 && elemSize > UINT64_MAX / (uint64_t)t->arrayLen) {
                return 0;
            }
            *outSize = elemSize * (uint64_t)t->arrayLen;
            *outAlign = elemAlign;
            return 1;
        }
        case SLTCType_SLICE:
            *outSize = SLTCConstAlignUpU64(usizeSize, ptrSize) + ptrSize;
            *outAlign = ptrSize;
            return 1;
        case SLTCType_OPTIONAL:
            return SLTCConstTypeLayout(c, t->baseType, outSize, outAlign, depth + 1u);
        case SLTCType_NAMED:
        case SLTCType_ANON_STRUCT: {
            uint64_t offset = 0;
            uint64_t maxAlign = 1;
            uint32_t i;
            for (i = 0; i < t->fieldCount; i++) {
                uint64_t fieldSize = 0;
                uint64_t fieldAlign = 0;
                uint32_t fieldIdx = t->fieldStart + i;
                if (fieldIdx >= c->fieldLen
                    || !SLTCConstTypeLayout(
                        c, c->fields[fieldIdx].typeId, &fieldSize, &fieldAlign, depth + 1u))
                {
                    return 0;
                }
                if (fieldAlign > maxAlign) {
                    maxAlign = fieldAlign;
                }
                offset = SLTCConstAlignUpU64(offset, fieldAlign);
                if (fieldSize > UINT64_MAX - offset) {
                    return 0;
                }
                offset += fieldSize;
            }
            *outSize = SLTCConstAlignUpU64(offset, maxAlign);
            *outAlign = maxAlign;
            return 1;
        }
        case SLTCType_ANON_UNION: {
            uint64_t maxSize = 0;
            uint64_t maxAlign = 1;
            uint32_t i;
            for (i = 0; i < t->fieldCount; i++) {
                uint64_t fieldSize = 0;
                uint64_t fieldAlign = 0;
                uint32_t fieldIdx = t->fieldStart + i;
                if (fieldIdx >= c->fieldLen
                    || !SLTCConstTypeLayout(
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
            *outSize = SLTCConstAlignUpU64(maxSize, maxAlign);
            *outAlign = maxAlign;
            return 1;
        }
        case SLTCType_NULL:
            *outSize = ptrSize;
            *outAlign = ptrSize;
            return 1;
        default: return 0;
    }
}

int SLTCConstEvalSizeOf(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    const SLAstNode* n;
    int32_t          innerNode;
    int32_t          innerType = -1;
    uint64_t         sizeBytes = 0;
    uint64_t         alignBytes = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    innerNode = SLAstFirstChild(c->ast, exprNode);
    if (innerNode < 0) {
        SLTCConstSetReasonNode(evalCtx, exprNode, "sizeof expression is malformed");
        *outIsConst = 0;
        return 0;
    }

    if (n->flags == 1) {
        if (SLTCResolveTypeNode(c, innerNode, &innerType) != 0) {
            if (c->diag != NULL) {
                *c->diag = (SLDiag){ 0 };
            }
            if (c->ast->nodes[innerNode].kind == SLAst_TYPE_NAME) {
                int32_t localIdx = SLTCLocalFind(
                    c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                if (localIdx >= 0) {
                    innerType = c->locals[localIdx].typeId;
                } else {
                    int32_t fnIdx = SLTCFindFunctionIndex(
                        c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                    if (fnIdx >= 0) {
                        innerType = c->funcs[fnIdx].funcTypeId;
                    } else {
                        int32_t topNameIndex = -1;
                        int32_t topNode = SLTCFindTopLevelVarLikeNode(
                            c,
                            c->ast->nodes[innerNode].dataStart,
                            c->ast->nodes[innerNode].dataEnd,
                            &topNameIndex);
                        if (topNode >= 0) {
                            if (SLTCTypeTopLevelVarLikeNode(c, topNode, topNameIndex, &innerType)
                                != 0)
                            {
                                return -1;
                            }
                        }
                    }
                }
                if (innerType < 0
                    && SLTCConstLookupExecBindingType(
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
        if (SLTCTypeExpr(c, innerNode, &innerType) != 0) {
            return -1;
        }
        if (SLTCTypeContainsVarSizeByValue(c, innerType)) {
            SLTCConstSetReasonNode(
                evalCtx, innerNode, "sizeof operand type is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
    }
    if (innerType < 0) {
        SLTCConstSetReasonNode(evalCtx, innerNode, "sizeof operand type is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (!SLTCConstTypeLayout(c, innerType, &sizeBytes, &alignBytes, 0)
        || sizeBytes > (uint64_t)INT64_MAX || alignBytes == 0)
    {
        SLTCConstSetReasonNode(evalCtx, innerNode, "sizeof operand type is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    outValue->kind = SLCTFEValue_INT;
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

int SLTCConstEvalCast(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx* c;
    int32_t         valueNode;
    int32_t         typeNode;
    int32_t         targetType;
    int32_t         baseTarget;
    SLCTFEValue     inValue;
    int             inIsConst = 0;

    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }

    valueNode = SLAstFirstChild(c->ast, exprNode);
    typeNode = valueNode >= 0 ? SLAstNextSibling(c->ast, valueNode) : -1;
    if (valueNode < 0 || typeNode < 0) {
        SLTCConstSetReasonNode(evalCtx, exprNode, "cast expression is malformed");
        *outIsConst = 0;
        return 0;
    }

    if (SLTCResolveTypeNode(c, typeNode, &targetType) != 0) {
        return -1;
    }
    if (SLTCEvalConstExprNode(evalCtx, valueNode, &inValue, &inIsConst) != 0) {
        return -1;
    }
    if (!inIsConst) {
        *outIsConst = 0;
        return 0;
    }

    baseTarget = SLTCResolveAliasBaseType(c, targetType);
    if (baseTarget < 0 || (uint32_t)baseTarget >= c->typeLen) {
        *outIsConst = 0;
        return 0;
    }

    if (SLTCIsIntegerType(c, baseTarget)) {
        int64_t asInt = 0;
        if (inValue.kind == SLCTFEValue_INT) {
            asInt = inValue.i64;
        } else if (inValue.kind == SLCTFEValue_BOOL) {
            asInt = inValue.b ? 1 : 0;
        } else if (inValue.kind == SLCTFEValue_FLOAT) {
            if (inValue.f64 != inValue.f64 || inValue.f64 > (double)INT64_MAX
                || inValue.f64 < (double)INT64_MIN)
            {
                SLTCConstSetReasonNode(
                    evalCtx, valueNode, "cast result is out of range for const integer");
                *outIsConst = 0;
                return 0;
            }
            asInt = (int64_t)inValue.f64;
        } else if (inValue.kind == SLCTFEValue_NULL) {
            asInt = 0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = SLCTFEValue_INT;
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

    if (SLTCIsFloatType(c, baseTarget)) {
        double asFloat = 0.0;
        if (inValue.kind == SLCTFEValue_FLOAT) {
            asFloat = inValue.f64;
        } else if (inValue.kind == SLCTFEValue_INT) {
            asFloat = (double)inValue.i64;
        } else if (inValue.kind == SLCTFEValue_BOOL) {
            asFloat = inValue.b ? 1.0 : 0.0;
        } else if (inValue.kind == SLCTFEValue_NULL) {
            asFloat = 0.0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = SLCTFEValue_FLOAT;
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

    if (SLTCIsBoolType(c, baseTarget)) {
        uint8_t asBool = 0;
        if (inValue.kind == SLCTFEValue_BOOL) {
            asBool = inValue.b ? 1u : 0u;
        } else if (inValue.kind == SLCTFEValue_INT) {
            asBool = inValue.i64 != 0 ? 1u : 0u;
        } else if (inValue.kind == SLCTFEValue_FLOAT) {
            asBool = inValue.f64 != 0.0 ? 1u : 0u;
        } else if (inValue.kind == SLCTFEValue_STRING) {
            asBool = 1u;
        } else if (inValue.kind == SLCTFEValue_NULL) {
            asBool = 0;
        } else {
            *outIsConst = 0;
            return 0;
        }
        outValue->kind = SLCTFEValue_BOOL;
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

    if (c->types[baseTarget].kind == SLTCType_OPTIONAL) {
        if (inValue.kind == SLCTFEValue_OPTIONAL) {
            if (inValue.typeTag > 0 && inValue.typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)inValue.typeTag < c->typeLen && (int32_t)inValue.typeTag == baseTarget)
            {
                *outValue = inValue;
                *outIsConst = 1;
                return 0;
            }
            if (inValue.b == 0u) {
                if (SLTCConstEvalSetOptionalNoneValue(c, baseTarget, outValue) != 0) {
                    return -1;
                }
                *outIsConst = 1;
                return 0;
            }
            if (inValue.s.bytes == NULL) {
                *outIsConst = 0;
                return 0;
            }
            if (SLTCConstEvalSetOptionalSomeValue(
                    c, baseTarget, (const SLCTFEValue*)inValue.s.bytes, outValue)
                != 0)
            {
                return -1;
            }
            *outIsConst = 1;
            return 0;
        }
        if (inValue.kind == SLCTFEValue_NULL) {
            if (SLTCConstEvalSetOptionalNoneValue(c, baseTarget, outValue) != 0) {
                return -1;
            }
            *outIsConst = 1;
            return 0;
        }
        if (SLTCConstEvalSetOptionalSomeValue(c, baseTarget, &inValue, outValue) != 0) {
            return -1;
        }
        *outIsConst = 1;
        return 0;
    }

    if ((c->types[baseTarget].kind == SLTCType_PTR || c->types[baseTarget].kind == SLTCType_REF
         || c->types[baseTarget].kind == SLTCType_FUNCTION)
        && inValue.kind == SLCTFEValue_NULL)
    {
        *outValue = inValue;
        *outIsConst = 1;
        return 0;
    }

    *outValue = inValue;
    *outIsConst = 1;
    return 0;
}

int SLTCConstEvalTypeOf(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*   c;
    const SLAstNode*  callee;
    int32_t           calleeNode;
    int32_t           argNode;
    int32_t           argExprNode;
    int32_t           extraNode;
    int32_t           argType;
    uint32_t          callArgIndex = 0;
    int               packStatus;
    SLTCConstEvalCtx* savedActiveEvalCtx;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = SLAstFirstChild(c->ast, exprNode);
    argNode = calleeNode >= 0 ? SLAstNextSibling(c->ast, calleeNode) : -1;
    extraNode = argNode >= 0 ? SLAstNextSibling(c->ast, argNode) : -1;
    if (calleeNode < 0 || argNode < 0 || extraNode >= 0) {
        SLTCConstSetReasonNode(evalCtx, exprNode, "typeof call has invalid arity");
        *outIsConst = 0;
        return 0;
    }
    callee = &c->ast->nodes[calleeNode];
    if (callee->kind != SLAst_IDENT
        || !SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof"))
    {
        return -1;
    }
    argExprNode = argNode;
    if (c->ast->nodes[argExprNode].kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, argExprNode);
        if (inner >= 0) {
            argExprNode = inner;
        }
    }
    packStatus = SLTCConstEvalResolveTrackedAnyPackArgIndex(evalCtx, argExprNode, &callArgIndex);
    if (packStatus < 0) {
        return -1;
    }
    if (packStatus == 0) {
        const SLTCCallBinding* binding = (const SLTCCallBinding*)evalCtx->callBinding;
        if (binding != NULL && callArgIndex < evalCtx->callArgCount
            && binding->argExpectedTypes[callArgIndex] >= 0)
        {
            argType = binding->argExpectedTypes[callArgIndex];
            goto done;
        }
    }
    if (packStatus == 2 || packStatus == 3) {
        *outIsConst = 0;
        return 0;
    }
    savedActiveEvalCtx = c->activeConstEvalCtx;
    c->activeConstEvalCtx = evalCtx;
    if (SLTCTypeExpr(c, argExprNode, &argType) != 0) {
        c->activeConstEvalCtx = savedActiveEvalCtx;
        return -1;
    }
    c->activeConstEvalCtx = savedActiveEvalCtx;
done:
    outValue->kind = SLCTFEValue_TYPE;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = SLTCEncodeTypeTag(c, argType);
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

int SLTCConstEvalLenCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*   c;
    const SLAstNode*  callee;
    int32_t           calleeNode;
    int32_t           argNode;
    int32_t           argExprNode;
    int32_t           extraNode;
    int32_t           argType = -1;
    int32_t           resolvedArgType = -1;
    SLCTFEValue       argValue;
    int               argIsConst = 0;
    SLTCConstEvalCtx* savedActiveEvalCtx;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = SLAstFirstChild(c->ast, exprNode);
    argNode = calleeNode >= 0 ? SLAstNextSibling(c->ast, calleeNode) : -1;
    extraNode = argNode >= 0 ? SLAstNextSibling(c->ast, argNode) : -1;
    if (calleeNode < 0 || argNode < 0 || extraNode >= 0) {
        return 1;
    }
    callee = &c->ast->nodes[calleeNode];
    if (callee->kind != SLAst_IDENT
        || !SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len"))
    {
        return 1;
    }
    argExprNode = argNode;
    if (c->ast->nodes[argExprNode].kind == SLAst_CALL_ARG) {
        argExprNode = SLAstFirstChild(c->ast, argExprNode);
        if (argExprNode < 0) {
            return 1;
        }
    }

    if (SLTCEvalConstExprNode(evalCtx, argExprNode, &argValue, &argIsConst) != 0) {
        return -1;
    }
    if (argIsConst && argValue.kind == SLCTFEValue_STRING) {
        outValue->kind = SLCTFEValue_INT;
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
    if (SLTCTypeExpr(c, argExprNode, &argType) != 0) {
        c->activeConstEvalCtx = savedActiveEvalCtx;
        return -1;
    }
    c->activeConstEvalCtx = savedActiveEvalCtx;

    resolvedArgType = SLTCResolveAliasBaseType(c, argType);
    if (resolvedArgType >= 0 && (uint32_t)resolvedArgType < c->typeLen) {
        const SLTCType* t = &c->types[resolvedArgType];
        if (t->kind == SLTCType_PACK) {
            outValue->kind = SLCTFEValue_INT;
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
        if (t->kind == SLTCType_ARRAY) {
            outValue->kind = SLCTFEValue_INT;
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

    SLTCConstSetReasonNode(evalCtx, argExprNode, "len() operand is not const-evaluable");
    *outIsConst = 0;
    return 0;
}

static int SLTCConstEvalIndexExpr(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    const SLAstNode* n;
    int32_t          baseNode;
    int32_t          idxNode;
    int32_t          extraNode;
    SLCTFEValue      baseValue;
    SLCTFEValue      idxValue;
    int              baseIsConst = 0;
    int              idxIsConst = 0;
    int64_t          idxInt = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind != SLAst_INDEX) {
        return 1;
    }
    if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
        return 1;
    }
    baseNode = SLAstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? SLAstNextSibling(c->ast, baseNode) : -1;
    extraNode = idxNode >= 0 ? SLAstNextSibling(c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
        return 1;
    }

    if (SLTCEvalConstExprNode(evalCtx, baseNode, &baseValue, &baseIsConst) != 0) {
        return -1;
    }
    if (!baseIsConst) {
        SLTCConstSetReasonNode(evalCtx, baseNode, "index base is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (baseValue.kind != SLCTFEValue_STRING) {
        SLTCConstSetReasonNode(evalCtx, baseNode, "index base is not const-evaluable string data");
        *outIsConst = 0;
        return 0;
    }

    if (SLTCEvalConstExprNode(evalCtx, idxNode, &idxValue, &idxIsConst) != 0) {
        return -1;
    }
    if (!idxIsConst) {
        SLTCConstSetReasonNode(evalCtx, idxNode, "index is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }
    if (SLCTFEValueToInt64(&idxValue, &idxInt) != 0) {
        SLTCConstSetReasonNode(evalCtx, idxNode, "index expression did not evaluate to integer");
        *outIsConst = 0;
        return 0;
    }
    if (idxInt < 0) {
        SLTCConstSetReasonNode(evalCtx, idxNode, "index is negative in const evaluation");
        *outIsConst = 0;
        return 0;
    }
    if ((uint64_t)idxInt >= (uint64_t)baseValue.s.len) {
        SLTCConstSetReasonNode(evalCtx, idxNode, "index is out of bounds in const evaluation");
        *outIsConst = 0;
        return 0;
    }

    outValue->kind = SLCTFEValue_INT;
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

int SLTCResolveReflectedTypeValueExpr(SLTypeCheckCtx* c, int32_t exprNode, int32_t* outTypeId) {
    const SLAstNode* n;
    if (c == NULL || outTypeId == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            return 1;
        }
        return SLTCResolveReflectedTypeValueExpr(c, inner, outTypeId);
    }
    if (n->kind == SLAst_IDENT) {
        int32_t typeId = SLTCResolveTypeValueName(c, n->dataStart, n->dataEnd);
        if (typeId < 0) {
            return 1;
        }
        *outTypeId = typeId;
        return 0;
    }
    if (SLTCIsTypeNodeKind(n->kind)) {
        if (SLTCResolveTypeNode(c, exprNode, outTypeId) != 0) {
            return -1;
        }
        return 0;
    }
    if (n->kind == SLAst_CALL) {
        int32_t          calleeNode = SLAstFirstChild(c->ast, exprNode);
        const SLAstNode* callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
        int32_t          argNode;
        int32_t          extraNode;
        int32_t          elemTypeId;
        if (callee == NULL || callee->kind != SLAst_IDENT) {
            return 1;
        }

        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof")) {
            argNode = SLAstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? SLAstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (SLTCTypeExpr(c, argNode, outTypeId) != 0) {
                return -1;
            }
            return 0;
        }

        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")) {
            argNode = SLAstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? SLAstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (SLTCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            *outTypeId = SLTCInternPtrType(c, elemTypeId, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }

        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice")) {
            argNode = SLAstNextSibling(c->ast, calleeNode);
            extraNode = argNode >= 0 ? SLAstNextSibling(c->ast, argNode) : -1;
            if (argNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (SLTCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            *outTypeId = SLTCInternSliceType(c, elemTypeId, 0, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }

        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            int32_t lenNode;
            int32_t lenType;
            int64_t lenValue = 0;
            int     lenIsConst = 0;
            argNode = SLAstNextSibling(c->ast, calleeNode);
            lenNode = argNode >= 0 ? SLAstNextSibling(c->ast, argNode) : -1;
            extraNode = lenNode >= 0 ? SLAstNextSibling(c->ast, lenNode) : -1;
            if (argNode < 0 || lenNode < 0 || extraNode >= 0) {
                return 1;
            }
            if (SLTCResolveReflectedTypeValueExpr(c, argNode, &elemTypeId) != 0) {
                return 1;
            }
            if (SLTCTypeExpr(c, lenNode, &lenType) != 0) {
                return -1;
            }
            if (!SLTCIsIntegerType(c, lenType)) {
                return 1;
            }
            if (SLTCConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0 || !lenIsConst
                || lenValue < 0 || lenValue > (int64_t)UINT32_MAX)
            {
                return 1;
            }
            *outTypeId = SLTCInternArrayType(c, elemTypeId, (uint32_t)lenValue, n->start, n->end);
            return *outTypeId >= 0 ? 0 : -1;
        }
    }
    return 1;
}

int SLTCConstEvalTypeNameValue(
    SLTypeCheckCtx* c, int32_t typeId, SLCTFEValue* outValue, int* outIsConst) {
    char        tmp[256];
    SLTCTextBuf b;
    char*       storage;
    if (c == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    SLTCTextBufInit(&b, tmp, (uint32_t)sizeof(tmp));
    SLTCFormatTypeRec(c, typeId, &b, 0);
    storage = (char*)SLArenaAlloc(c->arena, b.len + 1u, 1u);
    if (storage == NULL) {
        return -1;
    }
    if (b.len > 0u) {
        memcpy(storage, b.ptr, (size_t)b.len);
    }
    storage[b.len] = '\0';
    outValue->kind = SLCTFEValue_STRING;
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

void SLTCConstEvalSetNullValue(SLCTFEValue* outValue) {
    if (outValue == NULL) {
        return;
    }
    outValue->kind = SLCTFEValue_NULL;
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

static int SLTCConstEvalSetOptionalNoneValue(
    SLTypeCheckCtx* c, int32_t optionalTypeId, SLCTFEValue* outValue) {
    int32_t baseTypeId;
    if (c == NULL || outValue == NULL) {
        return -1;
    }
    baseTypeId = SLTCResolveAliasBaseType(c, optionalTypeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
        || c->types[baseTypeId].kind != SLTCType_OPTIONAL)
    {
        return -1;
    }
    SLTCConstEvalSetNullValue(outValue);
    outValue->kind = SLCTFEValue_OPTIONAL;
    outValue->b = 0;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    return 0;
}

static int SLTCConstEvalSetOptionalSomeValue(
    SLTypeCheckCtx* c, int32_t optionalTypeId, const SLCTFEValue* payload, SLCTFEValue* outValue) {
    SLCTFEValue* payloadCopy;
    int32_t      baseTypeId;
    if (c == NULL || payload == NULL || outValue == NULL) {
        return -1;
    }
    baseTypeId = SLTCResolveAliasBaseType(c, optionalTypeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen
        || c->types[baseTypeId].kind != SLTCType_OPTIONAL)
    {
        return -1;
    }
    payloadCopy = (SLCTFEValue*)SLArenaAlloc(
        c->arena, sizeof(SLCTFEValue), (uint32_t)_Alignof(SLCTFEValue));
    if (payloadCopy == NULL) {
        return -1;
    }
    *payloadCopy = *payload;
    SLTCConstEvalSetNullValue(outValue);
    outValue->kind = SLCTFEValue_OPTIONAL;
    outValue->b = 1u;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    outValue->s.bytes = (const uint8_t*)payloadCopy;
    outValue->s.len = 0;
    return 0;
}

void SLTCConstEvalSetSpanFromOffsets(
    SLTypeCheckCtx* c, uint32_t startOffset, uint32_t endOffset, SLCTFEValue* outValue) {
    SLTCConstEvalSetNullValue(outValue);
    outValue->kind = SLCTFEValue_SPAN;
    outValue->span.fileBytes = (const uint8_t*)"";
    outValue->span.fileLen = 0;
    SLTCOffsetToLineCol(
        c->src.ptr,
        c->src.len,
        startOffset,
        &outValue->span.startLine,
        &outValue->span.startColumn);
    SLTCOffsetToLineCol(
        c->src.ptr, c->src.len, endOffset, &outValue->span.endLine, &outValue->span.endColumn);
}

/* Returns 0 when resolved, 1 when not a tracked anypack index expression, 2 on non-const index,
 * 3 on out-of-bounds index, and -1 on hard error. */
static int SLTCConstEvalResolveTrackedAnyPackArgIndex(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outCallArgIndex) {
    SLTypeCheckCtx*        c;
    const SLTCCallBinding* binding;
    const SLTCCallArgInfo* callArgs;
    int32_t                baseNode;
    int32_t                idxNode;
    int32_t                extraNode;
    int64_t                idxValue = 0;
    uint32_t               paramIndex;
    uint32_t               ordinal = 0;
    uint32_t               i;
    SLCTFEValue            idxConstValue;
    int                    idxIsConst = 0;
    if (evalCtx == NULL || outCallArgIndex == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    binding = (const SLTCCallBinding*)evalCtx->callBinding;
    callArgs = (const SLTCCallArgInfo*)evalCtx->callArgs;
    if (c == NULL || binding == NULL || callArgs == NULL || evalCtx->callArgCount == 0
        || evalCtx->callPackParamNameStart >= evalCtx->callPackParamNameEnd)
    {
        return 1;
    }
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len
        || c->ast->nodes[exprNode].kind != SLAst_INDEX
        || (c->ast->nodes[exprNode].flags & SLAstFlag_INDEX_SLICE) != 0)
    {
        return 1;
    }
    baseNode = SLAstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? SLAstNextSibling(c->ast, baseNode) : -1;
    extraNode = idxNode >= 0 ? SLAstNextSibling(c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
        return 1;
    }
    if (c->ast->nodes[baseNode].kind != SLAst_IDENT
        || !SLNameEqSlice(
            c->src,
            c->ast->nodes[baseNode].dataStart,
            c->ast->nodes[baseNode].dataEnd,
            evalCtx->callPackParamNameStart,
            evalCtx->callPackParamNameEnd))
    {
        return 1;
    }
    if (!binding->isVariadic || binding->spreadArgIndex != UINT32_MAX) {
        SLTCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
        return 3;
    }
    if (SLTCEvalConstExprNode(evalCtx, idxNode, &idxConstValue, &idxIsConst) != 0) {
        return -1;
    }
    if (!idxIsConst || SLCTFEValueToInt64(&idxConstValue, &idxValue) != 0) {
        SLTCConstSetReasonNode(
            evalCtx, idxNode, "anytype pack index must be const-evaluable integer");
        return 2;
    }
    if (idxValue < 0) {
        SLTCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
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
    SLTCConstSetReasonNode(evalCtx, idxNode, "anytype pack index is out of bounds");
    return 3;
}

int SLTCConstEvalSpanOfCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    int32_t          calleeNode;
    const SLAstNode* callee;
    int32_t          operandNode = -1;
    int32_t          nextNode = -1;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = SLAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == SLAst_IDENT) {
        if (!SLTCIsSpanOfName(c, callee->dataStart, callee->dataEnd)) {
            return 1;
        }
        operandNode = SLAstNextSibling(c->ast, calleeNode);
        nextNode = operandNode >= 0 ? SLAstNextSibling(c->ast, operandNode) : -1;
        if (operandNode < 0 || nextNode >= 0) {
            return 1;
        }
    } else if (callee->kind == SLAst_FIELD_EXPR) {
        int32_t recvNode = SLAstFirstChild(c->ast, calleeNode);
        if (recvNode < 0 || c->ast->nodes[recvNode].kind != SLAst_IDENT
            || !SLNameEqLiteral(
                c->src,
                c->ast->nodes[recvNode].dataStart,
                c->ast->nodes[recvNode].dataEnd,
                "reflect")
            || !SLTCIsSpanOfName(c, callee->dataStart, callee->dataEnd))
        {
            return 1;
        }
        operandNode = SLAstNextSibling(c->ast, calleeNode);
        nextNode = operandNode >= 0 ? SLAstNextSibling(c->ast, operandNode) : -1;
        if (operandNode < 0 || nextNode >= 0) {
            return 1;
        }
    } else {
        return 1;
    }
    if (c->ast->nodes[operandNode].kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, operandNode);
        if (inner < 0) {
            return 1;
        }
        operandNode = inner;
    }
    {
        uint32_t callArgIndex = 0;
        int      packStatus = SLTCConstEvalResolveTrackedAnyPackArgIndex(
            evalCtx, operandNode, &callArgIndex);
        if (packStatus < 0) {
            return -1;
        }
        if (packStatus == 0) {
            const SLTCCallArgInfo* callArgs = (const SLTCCallArgInfo*)evalCtx->callArgs;
            SLTCConstEvalSetSpanFromOffsets(
                c, callArgs[callArgIndex].start, callArgs[callArgIndex].end, outValue);
            *outIsConst = 1;
            return 0;
        }
        if (packStatus == 2 || packStatus == 3) {
            *outIsConst = 0;
            return 0;
        }
    }
    if ((c->ast->nodes[operandNode].kind == SLAst_IDENT
         || c->ast->nodes[operandNode].kind == SLAst_TYPE_NAME)
        && SLNameHasPrefix(
            c->src,
            c->ast->nodes[operandNode].dataStart,
            c->ast->nodes[operandNode].dataEnd,
            "__sl_"))
    {
        SLTCConstSetReasonNode(
            evalCtx, operandNode, "span_of operand cannot reference __sl_ names");
        *outIsConst = 0;
        return 0;
    }
    SLTCConstEvalSetSpanFromOffsets(
        c, c->ast->nodes[operandNode].start, c->ast->nodes[operandNode].end, outValue);
    *outIsConst = 1;
    return 0;
}

int SLTCConstEvalU32Arg(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, uint32_t* outValue, int* outIsConst) {
    SLCTFEValue v;
    int         isConst = 0;
    if (outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (SLTCEvalConstExprNode(evalCtx, exprNode, &v, &isConst) != 0) {
        return -1;
    }
    if (!isConst || v.kind != SLCTFEValue_INT || v.i64 < 0 || v.i64 > (int64_t)UINT32_MAX) {
        *outIsConst = 0;
        return 0;
    }
    *outValue = (uint32_t)v.i64;
    *outIsConst = 1;
    return 0;
}

int SLTCConstEvalPosCompound(
    SLTCConstEvalCtx* evalCtx, int32_t nodeId, uint32_t* ioLine, uint32_t* ioColumn) {
    SLTypeCheckCtx* c;
    int32_t         fieldNode;
    if (evalCtx == NULL || ioLine == NULL || ioColumn == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c != NULL && nodeId >= 0 && (uint32_t)nodeId < c->ast->len
        && c->ast->nodes[nodeId].kind == SLAst_CALL_ARG)
    {
        int32_t inner = SLAstFirstChild(c->ast, nodeId);
        if (inner >= 0) {
            nodeId = inner;
        }
    }
    if (c == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len
        || c->ast->nodes[nodeId].kind != SLAst_COMPOUND_LIT)
    {
        return 1;
    }
    fieldNode = SLAstFirstChild(c->ast, nodeId);
    if (fieldNode >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[fieldNode].kind)) {
        fieldNode = SLAstNextSibling(c->ast, fieldNode);
    }
    while (fieldNode >= 0) {
        const SLAstNode* field = &c->ast->nodes[fieldNode];
        int32_t          valueNode = SLAstFirstChild(c->ast, fieldNode);
        uint32_t         v = 0;
        int              isConst = 0;
        if (field->kind != SLAst_COMPOUND_FIELD || valueNode < 0) {
            return 1;
        }
        if (SLTCConstEvalU32Arg(evalCtx, valueNode, &v, &isConst) != 0) {
            return -1;
        }
        if (!isConst) {
            return 1;
        }
        if (SLNameEqLiteral(c->src, field->dataStart, field->dataEnd, "line")) {
            *ioLine = v;
        } else if (SLNameEqLiteral(c->src, field->dataStart, field->dataEnd, "column")) {
            *ioColumn = v;
        } else {
            return 1;
        }
        fieldNode = SLAstNextSibling(c->ast, fieldNode);
    }
    return 0;
}

int SLTCConstEvalSpanCompound(
    SLTCConstEvalCtx* evalCtx,
    int32_t           exprNode,
    int               forceSpan,
    SLCTFEValue*      outValue,
    int*              outIsConst) {
    SLTypeCheckCtx* c;
    int32_t         child;
    int32_t         fieldNode;
    int32_t         resolvedType = -1;
    uint32_t        startLine = 0;
    uint32_t        startColumn = 0;
    uint32_t        endLine = 0;
    uint32_t        endColumn = 0;
    const uint8_t*  fileBytes = (const uint8_t*)"";
    uint32_t        fileLen = 0;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len
        || c->ast->nodes[exprNode].kind != SLAst_COMPOUND_LIT)
    {
        return 0;
    }
    child = SLAstFirstChild(c->ast, exprNode);
    fieldNode = child;
    if (child >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        if (SLTCResolveTypeNode(c, child, &resolvedType) != 0) {
            return -1;
        }
        if (!SLTCTypeIsReflectSpan(c, resolvedType)) {
            return 0;
        }
        fieldNode = SLAstNextSibling(c->ast, child);
    } else if (!forceSpan) {
        return 0;
    }

    SLTCOffsetToLineCol(
        c->src.ptr, c->src.len, c->ast->nodes[exprNode].start, &startLine, &startColumn);
    SLTCOffsetToLineCol(c->src.ptr, c->src.len, c->ast->nodes[exprNode].end, &endLine, &endColumn);

    while (fieldNode >= 0) {
        const SLAstNode* field = &c->ast->nodes[fieldNode];
        int32_t          valueNode = SLAstFirstChild(c->ast, fieldNode);
        if (field->kind != SLAst_COMPOUND_FIELD || valueNode < 0) {
            goto non_const;
        }
        if (SLNameEqLiteral(c->src, field->dataStart, field->dataEnd, "file")) {
            SLCTFEValue fileValue;
            int         fileIsConst = 0;
            if (SLTCEvalConstExprNode(evalCtx, valueNode, &fileValue, &fileIsConst) != 0) {
                return -1;
            }
            if (!fileIsConst || fileValue.kind != SLCTFEValue_STRING) {
                goto non_const;
            }
            fileBytes = fileValue.s.bytes;
            fileLen = fileValue.s.len;
        } else if (SLNameEqLiteral(c->src, field->dataStart, field->dataEnd, "start")) {
            if (SLTCConstEvalPosCompound(evalCtx, valueNode, &startLine, &startColumn) != 0) {
                goto non_const;
            }
        } else if (SLNameEqLiteral(c->src, field->dataStart, field->dataEnd, "end")) {
            if (SLTCConstEvalPosCompound(evalCtx, valueNode, &endLine, &endColumn) != 0) {
                goto non_const;
            }
        } else {
            goto non_const;
        }
        fieldNode = SLAstNextSibling(c->ast, fieldNode);
    }

    SLTCConstEvalSetNullValue(outValue);
    outValue->kind = SLCTFEValue_SPAN;
    outValue->span.fileBytes = fileBytes;
    outValue->span.fileLen = fileLen;
    outValue->span.startLine = startLine;
    outValue->span.startColumn = startColumn;
    outValue->span.endLine = endLine;
    outValue->span.endColumn = endColumn;
    *outIsConst = 1;
    return 1;

non_const:
    SLTCConstSetReasonNode(evalCtx, exprNode, "reflect.Span literal is not const-evaluable");
    SLTCConstEvalSetNullValue(outValue);
    *outIsConst = 0;
    return 1;
}

int SLTCConstEvalSpanExpr(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFESpan* outSpan, int* outIsConst) {
    SLTypeCheckCtx* c;
    SLCTFEValue     v;
    int             isConst = 0;
    int             handled;
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
    if (c->ast->nodes[exprNode].kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            *outIsConst = 0;
            return 0;
        }
        exprNode = inner;
    }
    if (c->ast->nodes[exprNode].kind == SLAst_COMPOUND_LIT) {
        handled = SLTCConstEvalSpanCompound(evalCtx, exprNode, 1, &v, &isConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            if (!isConst || v.kind != SLCTFEValue_SPAN) {
                *outIsConst = 0;
                return 0;
            }
            *outSpan = v.span;
            *outIsConst = 1;
            return 0;
        }
    }
    if (SLTCEvalConstExprNode(evalCtx, exprNode, &v, &isConst) != 0) {
        return -1;
    }
    if (!isConst || v.kind != SLCTFEValue_SPAN) {
        *outIsConst = 0;
        return 0;
    }
    *outSpan = v.span;
    *outIsConst = 1;
    return 0;
}

static SLTCCompilerDiagOp SLTCConstEvalCompilerDiagOpFromFieldExpr(
    SLTypeCheckCtx* c, const SLAstNode* fieldExpr) {
    SLTCCompilerDiagOp op;
    uint32_t           segStart;
    uint32_t           i;
    if (c == NULL || fieldExpr == NULL) {
        return SLTCCompilerDiagOp_NONE;
    }
    op = SLTCCompilerDiagOpFromName(c, fieldExpr->dataStart, fieldExpr->dataEnd);
    if (op != SLTCCompilerDiagOp_NONE) {
        return op;
    }
    if (fieldExpr->dataEnd <= fieldExpr->dataStart || fieldExpr->dataEnd > c->src.len) {
        return SLTCCompilerDiagOp_NONE;
    }
    segStart = fieldExpr->dataStart;
    for (i = fieldExpr->dataStart; i < fieldExpr->dataEnd; i++) {
        if (c->src.ptr[i] == '.') {
            segStart = i + 1u;
        }
    }
    if (SLNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "error")) {
        return SLTCCompilerDiagOp_ERROR;
    }
    if (SLNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "error_at")) {
        return SLTCCompilerDiagOp_ERROR_AT;
    }
    if (SLNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "warn")) {
        return SLTCCompilerDiagOp_WARN;
    }
    if (SLNameEqLiteral(c->src, segStart, fieldExpr->dataEnd, "warn_at")) {
        return SLTCCompilerDiagOp_WARN_AT;
    }
    return SLTCCompilerDiagOp_NONE;
}

int SLTCConstEvalCompilerDiagCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*    c;
    int32_t            calleeNode;
    const SLAstNode*   callee;
    SLTCCompilerDiagOp op = SLTCCompilerDiagOp_NONE;
    int32_t            msgNode = -1;
    int32_t            spanNode = -1;
    int32_t            nextNode;
    SLCTFEValue        msgValue;
    int                msgIsConst = 0;
    uint32_t           diagStart = 0;
    uint32_t           diagEnd = 0;
    const char*        detail;
    SLDiag             emitted;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = SLAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == SLAst_IDENT) {
        op = SLTCCompilerDiagOpFromName(c, callee->dataStart, callee->dataEnd);
    } else if (callee->kind == SLAst_FIELD_EXPR) {
        int32_t recvNode = SLAstFirstChild(c->ast, calleeNode);
        if (recvNode >= 0 && c->ast->nodes[recvNode].kind == SLAst_IDENT
            && SLNameEqLiteral(
                c->src,
                c->ast->nodes[recvNode].dataStart,
                c->ast->nodes[recvNode].dataEnd,
                "compiler"))
        {
            op = SLTCConstEvalCompilerDiagOpFromFieldExpr(c, callee);
        }
    }
    if (op == SLTCCompilerDiagOp_NONE) {
        return 1;
    }

    if (op == SLTCCompilerDiagOp_ERROR || op == SLTCCompilerDiagOp_WARN) {
        msgNode = SLAstNextSibling(c->ast, calleeNode);
        nextNode = msgNode >= 0 ? SLAstNextSibling(c->ast, msgNode) : -1;
        if (msgNode < 0 || nextNode >= 0) {
            return 1;
        }
        diagStart = c->ast->nodes[exprNode].start;
        diagEnd = c->ast->nodes[exprNode].end;
    } else {
        int        spanIsConst = 0;
        SLCTFESpan span;
        uint32_t   spanStartOffset = 0;
        uint32_t   spanEndOffset = 0;
        spanNode = SLAstNextSibling(c->ast, calleeNode);
        msgNode = spanNode >= 0 ? SLAstNextSibling(c->ast, spanNode) : -1;
        nextNode = msgNode >= 0 ? SLAstNextSibling(c->ast, msgNode) : -1;
        if (spanNode < 0 || msgNode < 0 || nextNode >= 0) {
            return 1;
        }
        if (SLTCConstEvalSpanExpr(evalCtx, spanNode, &span, &spanIsConst) != 0) {
            return -1;
        }
        if (!spanIsConst || span.startLine == 0 || span.startColumn == 0 || span.endLine == 0
            || span.endColumn == 0
            || SLTCLineColToOffset(
                   c->src.ptr, c->src.len, span.startLine, span.startColumn, &spanStartOffset)
                   != 0
            || SLTCLineColToOffset(
                   c->src.ptr, c->src.len, span.endLine, span.endColumn, &spanEndOffset)
                   != 0
            || spanEndOffset < spanStartOffset)
        {
            return SLTCFailNode(c, spanNode, SLDiag_CONSTEVAL_DIAG_INVALID_SPAN);
        }
        diagStart = spanStartOffset;
        diagEnd = spanEndOffset;
    }
    if (msgNode >= 0 && (uint32_t)msgNode < c->ast->len
        && c->ast->nodes[msgNode].kind == SLAst_CALL_ARG)
    {
        int32_t inner = SLAstFirstChild(c->ast, msgNode);
        if (inner >= 0) {
            msgNode = inner;
        }
    }

    if (SLTCEvalConstExprNode(evalCtx, msgNode, &msgValue, &msgIsConst) != 0) {
        return -1;
    }
    if (!msgIsConst || msgValue.kind != SLCTFEValue_STRING) {
        return SLTCFailNode(c, msgNode, SLDiag_CONSTEVAL_DIAG_MESSAGE_NOT_CONST_STRING);
    }
    detail = SLTCAllocCStringBytes(c, msgValue.s.bytes, msgValue.s.len);
    if (detail == NULL) {
        return SLTCFailNode(c, msgNode, SLDiag_ARENA_OOM);
    }

    emitted = (SLDiag){
        .code = (op == SLTCCompilerDiagOp_WARN || op == SLTCCompilerDiagOp_WARN_AT)
                  ? SLDiag_CONSTEVAL_DIAG_WARNING
                  : SLDiag_CONSTEVAL_DIAG_ERROR,
        .type = (op == SLTCCompilerDiagOp_WARN || op == SLTCCompilerDiagOp_WARN_AT)
                  ? SLDiagType_WARNING
                  : SLDiagType_ERROR,
        .start = diagStart,
        .end = diagEnd,
        .argStart = 0,
        .argEnd = 0,
        .detail = detail,
        .hintOverride = NULL,
    };
    SLTCMarkConstDiagUseExecuted(c, exprNode);
    if (emitted.type == SLDiagType_WARNING) {
        if (SLTCEmitWarningDiag(c, &emitted) != 0) {
            return -1;
        }
        SLTCConstEvalSetNullValue(outValue);
        *outIsConst = 1;
        return 0;
    }

    if (c->diag != NULL) {
        *c->diag = emitted;
    }
    return -1;
}

int SLTCConstEvalTypeReflectionCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    int32_t          calleeNode;
    const SLAstNode* callee;
    int32_t          op = 0;
    int32_t          operandNode = -1;
    int32_t          operandNode2 = -1;
    SLCTFEValue      operandValue;
    SLCTFEValue      operandValue2;
    int              operandIsConst = 0;
    int              operandIsConst2 = 0;
    int32_t          reflectedTypeId;
    enum {
        SLTCReflectKind_KIND = 1,
        SLTCReflectKind_BASE = 2,
        SLTCReflectKind_IS_ALIAS = 3,
        SLTCReflectKind_TYPE_NAME = 4,
        SLTCReflectKind_PTR = 5,
        SLTCReflectKind_SLICE = 6,
        SLTCReflectKind_ARRAY = 7,
    };
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = SLAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL) {
        return 1;
    }
    if (callee->kind == SLAst_IDENT) {
        int32_t argNode = SLAstNextSibling(c->ast, calleeNode);
        int32_t nextNode = argNode >= 0 ? SLAstNextSibling(c->ast, argNode) : -1;
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            op = SLTCReflectKind_KIND;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            op = SLTCReflectKind_BASE;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            op = SLTCReflectKind_IS_ALIAS;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            op = SLTCReflectKind_TYPE_NAME;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")) {
            op = SLTCReflectKind_PTR;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice")) {
            op = SLTCReflectKind_SLICE;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            op = SLTCReflectKind_ARRAY;
        } else {
            return 1;
        }
        if (op == SLTCReflectKind_ARRAY) {
            int32_t extraNode = nextNode >= 0 ? SLAstNextSibling(c->ast, nextNode) : -1;
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
    } else if (callee->kind == SLAst_FIELD_EXPR) {
        int32_t recvNode = SLAstFirstChild(c->ast, calleeNode);
        int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            op = SLTCReflectKind_KIND;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            op = SLTCReflectKind_BASE;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            op = SLTCReflectKind_IS_ALIAS;
        } else if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            op = SLTCReflectKind_TYPE_NAME;
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

    if (c->ast->nodes[operandNode].kind == SLAst_CALL_ARG) {
        int32_t innerNode = SLAstFirstChild(c->ast, operandNode);
        if (innerNode < 0) {
            return 1;
        }
        operandNode = innerNode;
    }
    if (operandNode2 >= 0 && c->ast->nodes[operandNode2].kind == SLAst_CALL_ARG) {
        int32_t innerNode = SLAstFirstChild(c->ast, operandNode2);
        if (innerNode < 0) {
            return 1;
        }
        operandNode2 = innerNode;
    }

    if (SLTCEvalConstExprNode(evalCtx, operandNode, &operandValue, &operandIsConst) != 0) {
        return -1;
    }
    if (!operandIsConst || operandValue.kind != SLCTFEValue_TYPE) {
        return 1;
    }

    if (op == SLTCReflectKind_KIND) {
        outValue->kind = SLCTFEValue_INT;
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
    if (op == SLTCReflectKind_IS_ALIAS) {
        outValue->kind = SLCTFEValue_BOOL;
        outValue->b = ((operandValue.typeTag >> 56u) & 0xffu) == (uint64_t)SLTCTypeTagKind_ALIAS;
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

    if (SLTCDecodeTypeTag(c, operandValue.typeTag, &reflectedTypeId) != 0) {
        return 1;
    }
    if (op == SLTCReflectKind_PTR || op == SLTCReflectKind_SLICE || op == SLTCReflectKind_ARRAY) {
        int32_t constructedTypeId = -1;
        if (op == SLTCReflectKind_PTR) {
            constructedTypeId = SLTCInternPtrType(c, reflectedTypeId, callee->start, callee->end);
        } else if (op == SLTCReflectKind_SLICE) {
            constructedTypeId = SLTCInternSliceType(
                c, reflectedTypeId, 0, callee->start, callee->end);
        } else {
            int64_t arrayLen = 0;
            if (operandNode2 < 0) {
                return 1;
            }
            if (SLTCEvalConstExprNode(evalCtx, operandNode2, &operandValue2, &operandIsConst2) != 0)
            {
                return -1;
            }
            if (!operandIsConst2 || SLCTFEValueToInt64(&operandValue2, &arrayLen) != 0
                || arrayLen < 0 || arrayLen > (int64_t)UINT32_MAX)
            {
                return 1;
            }
            constructedTypeId = SLTCInternArrayType(
                c, reflectedTypeId, (uint32_t)arrayLen, callee->start, callee->end);
        }
        if (constructedTypeId < 0) {
            return -1;
        }
        outValue->kind = SLCTFEValue_TYPE;
        outValue->i64 = 0;
        outValue->f64 = 0.0;
        outValue->b = 0;
        outValue->typeTag = SLTCEncodeTypeTag(c, constructedTypeId);
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
    if (op == SLTCReflectKind_TYPE_NAME) {
        return SLTCConstEvalTypeNameValue(c, reflectedTypeId, outValue, outIsConst);
    }
    if (reflectedTypeId < 0 || (uint32_t)reflectedTypeId >= c->typeLen
        || c->types[reflectedTypeId].kind != SLTCType_ALIAS)
    {
        SLTCConstSetReasonNode(evalCtx, operandNode, "base() requires an alias type");
        *outIsConst = 0;
        return 0;
    }
    if (SLTCResolveAliasTypeId(c, reflectedTypeId) != 0) {
        return -1;
    }
    reflectedTypeId = c->types[reflectedTypeId].baseType;
    outValue->kind = SLCTFEValue_TYPE;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = SLTCEncodeTypeTag(c, reflectedTypeId);
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

static int SLTCConstEvalPkgFunctionValueExpr(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    const SLAstNode* n;
    int32_t          recvNode;
    const SLAstNode* recv;
    int32_t          fnIndex;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind != SLAst_FIELD_EXPR) {
        return 1;
    }
    recvNode = SLAstFirstChild(c->ast, exprNode);
    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len) {
        return 1;
    }
    recv = &c->ast->nodes[recvNode];
    if (recv->kind != SLAst_IDENT) {
        return 1;
    }
    if (evalCtx->execCtx != NULL) {
        int32_t execType = -1;
        if (SLTCConstLookupExecBindingType(evalCtx, recv->dataStart, recv->dataEnd, &execType)) {
            return 1;
        }
    }
    if (SLTCLocalFind(c, recv->dataStart, recv->dataEnd) >= 0
        || SLTCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) >= 0)
    {
        return 1;
    }
    fnIndex = SLTCFindPkgQualifiedFunctionValueIndex(
        c, recv->dataStart, recv->dataEnd, n->dataStart, n->dataEnd);
    if (fnIndex < 0) {
        return 1;
    }
    SLMirValueSetFunctionRef(outValue, (uint32_t)fnIndex);
    *outIsConst = 1;
    return 0;
}

int SLTCEvalConstExprNode(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx* c;
    SLAstKind       kind;
    int             rc;
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
    if (kind == SLAst_BINARY) {
        const SLAstNode* n = &c->ast->nodes[exprNode];
        if ((SLTokenKind)n->op == SLTok_EQ || (SLTokenKind)n->op == SLTok_NEQ) {
            int32_t lhsNode = SLAstFirstChild(c->ast, exprNode);
            int32_t rhsNode = lhsNode >= 0 ? SLAstNextSibling(c->ast, lhsNode) : -1;
            int32_t extraNode = rhsNode >= 0 ? SLAstNextSibling(c->ast, rhsNode) : -1;
            int32_t lhsTypeId = -1;
            int32_t rhsTypeId = -1;
            int     lhsStatus;
            int     rhsStatus;
            if (lhsNode >= 0 && rhsNode >= 0 && extraNode < 0) {
                lhsStatus = SLTCResolveReflectedTypeValueExpr(c, lhsNode, &lhsTypeId);
                if (lhsStatus < 0) {
                    return -1;
                }
                rhsStatus = SLTCResolveReflectedTypeValueExpr(c, rhsNode, &rhsTypeId);
                if (rhsStatus < 0) {
                    return -1;
                }
                if (lhsStatus == 0 && rhsStatus == 0) {
                    outValue->kind = SLCTFEValue_BOOL;
                    outValue->i64 = 0;
                    outValue->f64 = 0.0;
                    outValue->b =
                        (((SLTokenKind)n->op == SLTok_EQ)
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
            int32_t     lhsNode = SLAstFirstChild(c->ast, exprNode);
            int32_t     rhsNode = lhsNode >= 0 ? SLAstNextSibling(c->ast, lhsNode) : -1;
            int32_t     extraNode = rhsNode >= 0 ? SLAstNextSibling(c->ast, rhsNode) : -1;
            SLCTFEValue lhsValue;
            SLCTFEValue rhsValue;
            int         lhsIsConst = 0;
            int         rhsIsConst = 0;
            if (lhsNode < 0 || rhsNode < 0 || extraNode >= 0) {
                return -1;
            }
            if (SLTCEvalConstExprNode(evalCtx, lhsNode, &lhsValue, &lhsIsConst) != 0) {
                return -1;
            }
            if (!lhsIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (SLTCEvalConstExprNode(evalCtx, rhsNode, &rhsValue, &rhsIsConst) != 0) {
                return -1;
            }
            if (!rhsIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (!SLTCConstEvalApplyBinary(c, (SLTokenKind)n->op, &lhsValue, &rhsValue, outValue)) {
                SLTCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            *outIsConst = 1;
            return 0;
        }
    }
    if (kind == SLAst_UNARY) {
        const SLAstNode* n = &c->ast->nodes[exprNode];
        int32_t          operandNode = SLAstFirstChild(c->ast, exprNode);
        int32_t          extraNode = operandNode >= 0 ? SLAstNextSibling(c->ast, operandNode) : -1;
        SLCTFEValue      operandValue;
        int              operandIsConst = 0;
        if (operandNode < 0 || extraNode >= 0) {
            return -1;
        }
        if (SLTCEvalConstExprNode(evalCtx, operandNode, &operandValue, &operandIsConst) != 0) {
            return -1;
        }
        if (!operandIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (!SLTCConstEvalApplyUnary((SLTokenKind)n->op, &operandValue, outValue)) {
            SLTCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        *outIsConst = 1;
        return 0;
    }
    if (kind == SLAst_SIZEOF) {
        return SLTCConstEvalSizeOf(evalCtx, exprNode, outValue, outIsConst);
    }
    if (kind == SLAst_COMPOUND_LIT) {
        int spanStatus = SLTCConstEvalSpanCompound(evalCtx, exprNode, 0, outValue, outIsConst);
        if (spanStatus < 0) {
            return -1;
        }
        if (spanStatus > 0) {
            return 0;
        }
    }
    if (kind == SLAst_INDEX) {
        int indexStatus = SLTCConstEvalIndexExpr(evalCtx, exprNode, outValue, outIsConst);
        if (indexStatus == 0) {
            return 0;
        }
        if (indexStatus < 0) {
            return -1;
        }
    }
    if (kind == SLAst_FIELD_EXPR) {
        int pkgFnStatus = SLTCConstEvalPkgFunctionValueExpr(
            evalCtx, exprNode, outValue, outIsConst);
        if (pkgFnStatus == 0) {
            return 0;
        }
        if (pkgFnStatus < 0) {
            return -1;
        }
    }
    if (kind == SLAst_CALL) {
        int32_t          calleeNode = SLAstFirstChild(c->ast, exprNode);
        const SLAstNode* callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
        int              compilerDiagStatus;
        int              directCallStatus;
        int              lenStatus;
        int              spanOfStatus;
        int              reflectStatus;
        lenStatus = SLTCConstEvalLenCall(evalCtx, exprNode, outValue, outIsConst);
        if (lenStatus == 0) {
            return 0;
        }
        if (lenStatus < 0) {
            return -1;
        }
        compilerDiagStatus = SLTCConstEvalCompilerDiagCall(evalCtx, exprNode, outValue, outIsConst);
        if (compilerDiagStatus == 0) {
            return 0;
        }
        if (compilerDiagStatus < 0) {
            return -1;
        }
        spanOfStatus = SLTCConstEvalSpanOfCall(evalCtx, exprNode, outValue, outIsConst);
        if (spanOfStatus == 0) {
            return 0;
        }
        if (spanOfStatus < 0) {
            return -1;
        }
        if (callee != NULL && callee->kind == SLAst_IDENT
            && SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof"))
        {
            return SLTCConstEvalTypeOf(evalCtx, exprNode, outValue, outIsConst);
        }
        reflectStatus = SLTCConstEvalTypeReflectionCall(evalCtx, exprNode, outValue, outIsConst);
        if (reflectStatus == 0) {
            return 0;
        }
        if (reflectStatus < 0) {
            return -1;
        }
        directCallStatus = SLTCConstEvalDirectCall(evalCtx, exprNode, outValue, outIsConst);
        if (directCallStatus == 0) {
            return 0;
        }
        if (directCallStatus < 0) {
            return -1;
        }
    }
    if (kind == SLAst_CAST) {
        return SLTCConstEvalCast(evalCtx, exprNode, outValue, outIsConst);
    }
    rc = SLCTFEEvalExprEx(
        c->arena,
        c->ast,
        c->src,
        exprNode,
        SLTCResolveConstIdent,
        SLTCResolveConstCall,
        evalCtx,
        SLTCMirConstMakeTuple,
        evalCtx,
        SLTCMirConstIndexValue,
        evalCtx,
        SLTCMirConstAggGetField,
        evalCtx,
        SLTCMirConstAggAddrField,
        evalCtx,
        outValue,
        outIsConst,
        c->diag);
    if (rc == 0 && !*outIsConst) {
        SLTCConstSetReasonNode(evalCtx, exprNode, "expression is not const-evaluable");
    }
    return rc;
}

int SLTCEvalConstExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    return SLTCEvalConstExprNode((SLTCConstEvalCtx*)ctx, exprNode, outValue, outIsConst);
}

int SLTCEvalConstExecResolveTypeCb(void* ctx, int32_t typeNode, int32_t* outTypeId) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    uint8_t           savedAllowConstNumericTypeName;
    if (evalCtx == NULL || evalCtx->tc == NULL || outTypeId == NULL) {
        return -1;
    }
    savedAllowConstNumericTypeName = evalCtx->tc->allowConstNumericTypeName;
    evalCtx->tc->allowConstNumericTypeName = 1;
    if (SLTCResolveTypeNode(evalCtx->tc, typeNode, outTypeId) != 0) {
        evalCtx->tc->allowConstNumericTypeName = savedAllowConstNumericTypeName;
        return -1;
    }
    evalCtx->tc->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    return 0;
}

int SLTCEvalConstExecInferValueTypeCb(void* ctx, const SLCTFEValue* value, int32_t* outTypeId) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*   c;
    if (evalCtx == NULL || value == NULL || outTypeId == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    switch (value->kind) {
        case SLCTFEValue_INT:   *outTypeId = c->typeUntypedInt; return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_FLOAT: *outTypeId = c->typeUntypedFloat; return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_BOOL:  *outTypeId = c->typeBool; return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_STRING:
            *outTypeId = SLTCGetStrRefType(c, 0, 0);
            return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_TYPE: *outTypeId = c->typeType; return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_SPAN:
            if (c->typeReflectSpan < 0) {
                c->typeReflectSpan = SLTCFindReflectSpanType(c);
            }
            *outTypeId = c->typeReflectSpan;
            return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_NULL: *outTypeId = c->typeNull; return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_OPTIONAL:
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

int SLTCEvalConstExecInferExprTypeCb(void* ctx, int32_t exprNode, int32_t* outTypeId) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    if (evalCtx == NULL || evalCtx->tc == NULL || outTypeId == NULL) {
        return -1;
    }
    return SLTCTypeExpr(evalCtx->tc, exprNode, outTypeId);
}

int SLTCEvalConstExecIsOptionalTypeCb(
    void* ctx, int32_t typeId, int32_t* outPayloadTypeId, int* outIsOptional) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*   c;
    int32_t           baseTypeId;
    if (evalCtx == NULL || evalCtx->tc == NULL || outIsOptional == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    baseTypeId = SLTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return -1;
    }
    *outIsOptional = c->types[baseTypeId].kind == SLTCType_OPTIONAL;
    if (outPayloadTypeId != NULL) {
        *outPayloadTypeId = *outIsOptional ? c->types[baseTypeId].baseType : -1;
    }
    return 0;
}

static int SLTCMirConstResolveTypeRefTypeId(
    SLTCConstEvalCtx* evalCtx, const SLMirTypeRef* typeRef, int32_t* outTypeId) {
    SLTypeCheckCtx* c;
    uint8_t         savedAllowConstNumericTypeName;
    uint8_t         savedAllowAnytypeParamType;
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
    if (SLTCResolveTypeNode(c, (int32_t)typeRef->astNode, outTypeId) != 0) {
        c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
        c->allowAnytypeParamType = savedAllowAnytypeParamType;
        return -1;
    }
    c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
    c->allowAnytypeParamType = savedAllowAnytypeParamType;
    return 0;
}

typedef struct {
    uint32_t    len;
    uint32_t    _reserved;
    SLCTFEValue elems[];
} SLTCMirConstTuple;

typedef struct {
    int32_t     typeId;
    uint32_t    fieldCount;
    SLCTFEValue fields[];
} SLTCMirConstAggregate;

enum {
    SL_TC_MIR_CONST_ITER_KIND_SEQUENCE = 1u,
    SL_TC_MIR_CONST_ITER_KIND_PROTOCOL = 2u,
};

typedef struct {
    uint32_t    index;
    uint16_t    flags;
    uint8_t     kind;
    uint8_t     _reserved;
    int32_t     iterFnIndex;
    int32_t     nextFnIndex;
    uint8_t     usePair;
    uint8_t     _reserved2[3];
    SLCTFEValue sourceValue;
    SLCTFEValue iteratorValue;
    SLCTFEValue currentValue;
} SLTCMirConstIter;

static const SLTCMirConstTuple* _Nullable SLTCMirConstTupleFromValue(const SLCTFEValue* value) {
    if (value == NULL || value->kind != SLCTFEValue_ARRAY || value->typeTag != SL_TC_MIR_TUPLE_TAG
        || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (const SLTCMirConstTuple*)value->s.bytes;
}

static SLTCMirConstIter* _Nullable SLTCMirConstIterFromValue(const SLCTFEValue* value) {
    const SLCTFEValue* target = value;
    if (value == NULL) {
        return NULL;
    }
    if (value->kind == SLCTFEValue_REFERENCE && value->s.bytes != NULL) {
        target = (const SLCTFEValue*)value->s.bytes;
    }
    if (target == NULL || target->kind != SLCTFEValue_SPAN || target->typeTag != SL_TC_MIR_ITER_TAG
        || target->s.bytes == NULL)
    {
        return NULL;
    }
    return (SLTCMirConstIter*)target->s.bytes;
}

static const SLTCMirConstAggregate* _Nullable SLTCMirConstAggregateFromValue(
    const SLCTFEValue* value) {
    const SLCTFEValue* target = value;
    if (value == NULL) {
        return NULL;
    }
    if (value->kind == SLCTFEValue_REFERENCE && value->s.bytes != NULL) {
        target = (const SLCTFEValue*)value->s.bytes;
    }
    if (target == NULL || target->kind != SLCTFEValue_AGGREGATE || target->s.bytes == NULL) {
        return NULL;
    }
    return (const SLTCMirConstAggregate*)target->s.bytes;
}

static SLCTFEValue* _Nullable SLTCMirConstAggregateFieldValuePtr(
    const SLTCMirConstAggregate* agg, uint32_t fieldIndex) {
    if (agg == NULL || fieldIndex >= agg->fieldCount) {
        return NULL;
    }
    return (SLCTFEValue*)&agg->fields[fieldIndex];
}

static const SLCTFEValue* SLTCMirConstValueTargetOrSelf(const SLCTFEValue* value) {
    if (value != NULL && value->kind == SLCTFEValue_REFERENCE && value->s.bytes != NULL) {
        return (const SLCTFEValue*)value->s.bytes;
    }
    return value;
}

static void SLTCMirConstSetReference(SLCTFEValue* outValue, SLCTFEValue* target) {
    if (outValue == NULL) {
        return;
    }
    SLTCConstEvalValueInvalid(outValue);
    if (target == NULL) {
        return;
    }
    outValue->kind = SLCTFEValue_REFERENCE;
    outValue->s.bytes = (const uint8_t*)target;
    outValue->s.len = 0;
}

static int SLTCMirConstOptionalPayload(const SLCTFEValue* value, const SLCTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (value == NULL || value->kind != SLCTFEValue_OPTIONAL) {
        return 0;
    }
    if (value->b == 0u || value->s.bytes == NULL) {
        return 1;
    }
    if (outPayload != NULL) {
        *outPayload = (const SLCTFEValue*)value->s.bytes;
    }
    return 1;
}

static void SLTCMirConstAdaptForInValueBinding(
    const SLCTFEValue* inValue, int valueRef, SLCTFEValue* outValue) {
    const SLCTFEValue* target;
    if (outValue == NULL) {
        return;
    }
    SLTCConstEvalValueInvalid(outValue);
    if (inValue == NULL) {
        return;
    }
    if (valueRef) {
        *outValue = *inValue;
        return;
    }
    target = SLTCMirConstValueTargetOrSelf(inValue);
    *outValue = target != NULL ? *target : *inValue;
}

static int SLTCMirConstFindExecBindingTypeId(
    SLTCConstEvalCtx* evalCtx, int32_t sourceNode, int32_t* outTypeId) {
    SLTypeCheckCtx*  c;
    const SLAstNode* node;
    SLCTFEExecCtx*   execCtx;
    SLCTFEExecEnv*   env;
    uint8_t          savedAllowConstNumericTypeName;
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
    if (node->kind != SLAst_IDENT) {
        return 0;
    }
    for (env = execCtx->env; env != NULL; env = env->parent) {
        uint32_t i;
        for (i = 0; i < env->bindingLen; i++) {
            SLCTFEExecBinding* binding = &env->bindings[i];
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
            if (SLTCResolveTypeNode(c, binding->typeNode, outTypeId) != 0) {
                c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
                return -1;
            }
            c->allowConstNumericTypeName = savedAllowConstNumericTypeName;
            return 1;
        }
    }
    return 0;
}

int SLTCMirConstMakeTuple(
    void* _Nullable ctx,
    const SLCTFEValue* elems,
    uint32_t           elemCount,
    uint32_t           typeNodeHint,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx*  evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*    c;
    SLTCMirConstTuple* tuple;
    size_t             bytes;
    (void)typeNodeHint;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        SLTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    bytes = sizeof(*tuple) + sizeof(SLCTFEValue) * (size_t)elemCount;
    tuple = (SLTCMirConstTuple*)SLArenaAlloc(
        c->arena, (uint32_t)bytes, (uint32_t)_Alignof(SLTCMirConstTuple));
    if (tuple == NULL) {
        return -1;
    }
    tuple->len = elemCount;
    tuple->_reserved = 0u;
    if (elemCount != 0u && elems != NULL) {
        memcpy(tuple->elems, elems, sizeof(SLCTFEValue) * elemCount);
    }
    outValue->kind = SLCTFEValue_ARRAY;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0u;
    outValue->typeTag = SL_TC_MIR_TUPLE_TAG;
    outValue->s.bytes = (const uint8_t*)tuple;
    outValue->s.len = elemCount;
    outValue->span = (SLCTFESpan){ 0 };
    *outIsConst = 1;
    return 0;
}

int SLTCMirConstIndexValue(
    void* _Nullable ctx,
    const SLCTFEValue* base,
    const SLCTFEValue* index,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    const SLTCMirConstTuple* tuple;
    int64_t                  indexValue = 0;
    (void)ctx;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    tuple = SLTCMirConstTupleFromValue(base);
    if (tuple == NULL || SLCTFEValueToInt64(index, &indexValue) != 0 || indexValue < 0
        || (uint64_t)indexValue >= (uint64_t)tuple->len)
    {
        return 0;
    }
    *outValue = tuple->elems[(uint32_t)indexValue];
    *outIsConst = 1;
    return 0;
}

int SLTCMirConstSequenceLen(
    void* _Nullable ctx,
    const SLCTFEValue* base,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    const SLCTFEValue*       value = base;
    const SLTCMirConstTuple* tuple;
    (void)ctx;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (base->kind == SLCTFEValue_REFERENCE && base->s.bytes != NULL) {
        value = (const SLCTFEValue*)base->s.bytes;
    }
    if (value->kind == SLCTFEValue_STRING) {
        outValue->kind = SLCTFEValue_INT;
        outValue->i64 = (int64_t)value->s.len;
        *outIsConst = 1;
        return 0;
    }
    tuple = SLTCMirConstTupleFromValue(value);
    if (tuple != NULL) {
        outValue->kind = SLCTFEValue_INT;
        outValue->i64 = (int64_t)tuple->len;
        *outIsConst = 1;
        return 0;
    }
    return 0;
}

int SLTCMirConstIterInit(
    void* _Nullable ctx,
    uint32_t           sourceNode,
    const SLCTFEValue* source,
    uint16_t           flags,
    SLCTFEValue*       outIter,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx*        evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*          c;
    const SLCTFEValue*       sourceValue = source;
    const SLTCMirConstTuple* tuple;
    SLTCMirConstIter*        iter;
    SLCTFEValue*             target;
    int32_t                  sourceType = -1;
    int32_t                  iterType = -1;
    int32_t                  iterPtrType = -1;
    int32_t                  iterFn = -1;
    int32_t                  nextValueFn = -1;
    int32_t                  nextKeyFn = -1;
    int32_t                  nextPairFn = -1;
    int32_t                  valueType = -1;
    int32_t                  keyType = -1;
    const SLAstNode*         sourceAstNode = NULL;
    int                      hasKey;
    int                      valueDiscard;
    int                      valueRef;
    int                      rc;
    int                      iterIsConst = 0;
    SLCTFEValue              iterValue;
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
    if ((flags & SLMirIterFlag_KEY_REF) != 0u || (flags & SLMirIterFlag_VALUE_REF) != 0u) {
        if ((flags & SLMirIterFlag_VALUE_REF) == 0u) {
            return 0;
        }
    }
    if (source->kind == SLCTFEValue_REFERENCE && source->s.bytes != NULL) {
        sourceValue = (const SLCTFEValue*)source->s.bytes;
    }
    tuple = SLTCMirConstTupleFromValue(sourceValue);
    iter = (SLTCMirConstIter*)SLArenaAlloc(
        c->arena, sizeof(*iter), (uint32_t)_Alignof(SLTCMirConstIter));
    target = (SLCTFEValue*)SLArenaAlloc(c->arena, sizeof(*target), (uint32_t)_Alignof(SLCTFEValue));
    if (iter == NULL || target == NULL) {
        return -1;
    }
    memset(iter, 0, sizeof(*iter));
    iter->flags = flags;
    iter->sourceValue = *source;
    iter->iterFnIndex = -1;
    iter->nextFnIndex = -1;
    hasKey = (flags & SLMirIterFlag_HAS_KEY) != 0u;
    valueDiscard = (flags & SLMirIterFlag_VALUE_DISCARD) != 0u;
    valueRef = (flags & SLMirIterFlag_VALUE_REF) != 0u;
    if (sourceValue->kind == SLCTFEValue_STRING || tuple != NULL) {
        iter->kind = SL_TC_MIR_CONST_ITER_KIND_SEQUENCE;
    } else {
        if (sourceNode < c->ast->len) {
            sourceAstNode = &c->ast->nodes[sourceNode];
        }
        rc = SLTCMirConstFindExecBindingTypeId(evalCtx, (int32_t)sourceNode, &sourceType);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0 && SLTCTypeExpr(c, (int32_t)sourceNode, &sourceType) != 0) {
            return -1;
        }
        rc = SLTCResolveForInIterator(c, (int32_t)sourceNode, sourceType, &iterFn, &iterType);
        if (rc != 0) {
            return 0;
        }
        iterPtrType = SLTCInternPtrType(
            c,
            iterType,
            sourceAstNode != NULL ? sourceAstNode->start : 0u,
            sourceAstNode != NULL ? sourceAstNode->end : 0u);
        if (iterPtrType < 0) {
            return -1;
        }
        if (hasKey && valueDiscard) {
            rc = SLTCResolveForInNextKey(c, iterPtrType, &keyType, &nextKeyFn);
            if (rc == 1 || rc == 2) {
                rc = SLTCResolveForInNextKeyAndValue(
                    c, iterPtrType, SLTCForInValueMode_ANY, &keyType, &valueType, &nextPairFn);
            }
            if (rc != 0) {
                return 0;
            }
            iter->usePair = nextPairFn >= 0 ? 1u : 0u;
            iter->nextFnIndex = nextPairFn >= 0 ? nextPairFn : nextKeyFn;
        } else if (hasKey) {
            rc = SLTCResolveForInNextKeyAndValue(
                c,
                iterPtrType,
                valueDiscard ? SLTCForInValueMode_ANY
                             : (valueRef ? SLTCForInValueMode_REF : SLTCForInValueMode_VALUE),
                &keyType,
                &valueType,
                &nextPairFn);
            if (rc != 0) {
                return 0;
            }
            iter->usePair = 1u;
            iter->nextFnIndex = nextPairFn;
        } else {
            rc = SLTCResolveForInNextValue(
                c,
                iterPtrType,
                valueDiscard ? SLTCForInValueMode_ANY
                             : (valueRef ? SLTCForInValueMode_REF : SLTCForInValueMode_VALUE),
                &valueType,
                &nextValueFn);
            if (rc == 1 || rc == 2) {
                rc = SLTCResolveForInNextKeyAndValue(
                    c,
                    iterPtrType,
                    valueDiscard ? SLTCForInValueMode_ANY
                                 : (valueRef ? SLTCForInValueMode_REF : SLTCForInValueMode_VALUE),
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
        SLTCConstEvalValueInvalid(&iterValue);
        if (SLTCInvokeConstFunctionByIndex(
                evalCtx,
                c->funcs[iterFn].nameStart,
                c->funcs[iterFn].nameEnd,
                iterFn,
                source,
                1u,
                &iterValue,
                &iterIsConst)
            != 0)
        {
            return -1;
        }
        if (!iterIsConst) {
            return 0;
        }
        iter->kind = SL_TC_MIR_CONST_ITER_KIND_PROTOCOL;
        iter->iterFnIndex = iterFn;
        iter->iteratorValue = iterValue;
    }
    target->kind = SLCTFEValue_SPAN;
    target->typeTag = SL_TC_MIR_ITER_TAG;
    target->s.bytes = (const uint8_t*)iter;
    target->s.len = 0;
    target->span = (SLCTFESpan){ 0 };
    outIter->kind = SLCTFEValue_REFERENCE;
    outIter->s.bytes = (const uint8_t*)target;
    outIter->s.len = 0;
    outIter->typeTag = 0;
    outIter->span = (SLCTFESpan){ 0 };
    *outIsConst = 1;
    return 0;
}

int SLTCMirConstIterNext(
    void* _Nullable ctx,
    const SLCTFEValue* iterValue,
    uint16_t           flags,
    int*               outHasItem,
    SLCTFEValue*       outKey,
    int*               outKeyIsConst,
    SLCTFEValue*       outValue,
    int*               outValueIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx*        evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*          c;
    SLTCMirConstIter*        iter;
    const SLCTFEValue*       sourceValue;
    const SLTCMirConstTuple* tuple;
    const SLCTFEValue*       payload = NULL;
    SLCTFEValue              callResult;
    SLCTFEValue              iterRef;
    const SLCTFEValue*       pairValue;
    const SLTCMirConstTuple* pairTuple;
    int                      isConst = 0;
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
    if ((flags & SLMirIterFlag_KEY_REF) != 0u) {
        return 0;
    }
    iter = SLTCMirConstIterFromValue(iterValue);
    if (iter == NULL) {
        return 0;
    }
    if (iter->kind == SL_TC_MIR_CONST_ITER_KIND_PROTOCOL) {
        if (c == NULL || iter->nextFnIndex < 0 || (uint32_t)iter->nextFnIndex >= c->funcLen) {
            return 0;
        }
        SLTCConstEvalValueInvalid(&callResult);
        SLTCMirConstSetReference(&iterRef, &iter->iteratorValue);
        if (SLTCInvokeConstFunctionByIndex(
                evalCtx,
                c->funcs[iter->nextFnIndex].nameStart,
                c->funcs[iter->nextFnIndex].nameEnd,
                iter->nextFnIndex,
                &iterRef,
                1u,
                &callResult,
                &isConst)
            != 0)
        {
            return -1;
        }
        if (!isConst) {
            return 0;
        }
        if (callResult.kind == SLCTFEValue_NULL) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if (callResult.kind == SLCTFEValue_OPTIONAL) {
            if (!SLTCMirConstOptionalPayload(&callResult, &payload)) {
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
            pairValue = SLTCMirConstValueTargetOrSelf(payload);
            pairTuple = SLTCMirConstTupleFromValue(pairValue);
            if (pairTuple == NULL || pairTuple->len != 2u) {
                return 0;
            }
            if ((flags & SLMirIterFlag_HAS_KEY) != 0u) {
                *outKey = pairTuple->elems[0];
                *outKeyIsConst = 1;
            } else {
                *outKeyIsConst = 1;
            }
            if ((flags & SLMirIterFlag_VALUE_DISCARD) == 0u) {
                SLTCMirConstAdaptForInValueBinding(
                    &pairTuple->elems[1], (flags & SLMirIterFlag_VALUE_REF) != 0u, outValue);
                *outValueIsConst = 1;
            } else {
                *outValueIsConst = 1;
            }
        } else if ((flags & SLMirIterFlag_HAS_KEY) != 0u) {
            *outKey = *payload;
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
        } else {
            SLTCMirConstAdaptForInValueBinding(
                payload, (flags & SLMirIterFlag_VALUE_REF) != 0u, outValue);
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        return 0;
    }
    sourceValue = &iter->sourceValue;
    if (sourceValue->kind == SLCTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        sourceValue = (const SLCTFEValue*)sourceValue->s.bytes;
    }
    tuple = SLTCMirConstTupleFromValue(sourceValue);
    if (sourceValue->kind == SLCTFEValue_STRING) {
        if (iter->index >= sourceValue->s.len) {
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if ((flags & SLMirIterFlag_HAS_KEY) != 0u) {
            outKey->kind = SLCTFEValue_INT;
            outKey->i64 = (int64_t)iter->index;
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if ((flags & SLMirIterFlag_VALUE_DISCARD) == 0u) {
            iter->currentValue.kind = SLCTFEValue_INT;
            iter->currentValue.i64 = (int64_t)sourceValue->s.bytes[iter->index];
            if ((flags & SLMirIterFlag_VALUE_REF) != 0u) {
                SLTCMirConstSetReference(outValue, &iter->currentValue);
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
        if ((flags & SLMirIterFlag_HAS_KEY) != 0u) {
            outKey->kind = SLCTFEValue_INT;
            outKey->i64 = (int64_t)iter->index;
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if ((flags & SLMirIterFlag_VALUE_DISCARD) == 0u) {
            if ((flags & SLMirIterFlag_VALUE_REF) != 0u) {
                SLTCMirConstSetReference(outValue, (SLCTFEValue*)&tuple->elems[iter->index]);
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

int SLTCEvalConstForInIndexCb(
    void* _Nullable ctx,
    SLCTFEExecCtx*     execCtx,
    const SLCTFEValue* sourceValue,
    uint32_t           index,
    int                byRef,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    const SLCTFEValue*       value = sourceValue;
    const SLTCMirConstTuple* tuple;
    (void)ctx;
    (void)execCtx;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (sourceValue == NULL || outValue == NULL || outIsConst == NULL || byRef) {
        return 0;
    }
    if (sourceValue->kind == SLCTFEValue_REFERENCE && sourceValue->s.bytes != NULL) {
        value = (const SLCTFEValue*)sourceValue->s.bytes;
    }
    tuple = SLTCMirConstTupleFromValue(value);
    if (tuple == NULL || index >= tuple->len) {
        return 0;
    }
    *outValue = tuple->elems[index];
    *outIsConst = 1;
    return 0;
}

static int SLTCMirConstResolveAggregateType(
    SLTypeCheckCtx* c, int32_t typeId, int32_t* outBaseTypeId) {
    int32_t         baseTypeId = -1;
    const SLTCType* t;
    if (outBaseTypeId != NULL) {
        *outBaseTypeId = -1;
    }
    if (c == NULL) {
        return 0;
    }
    baseTypeId = SLTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    t = &c->types[baseTypeId];
    if (t->kind == SLTCType_NAMED && t->fieldCount == 0u) {
        int32_t  namedIndex = -1;
        uint32_t i;
        for (i = 0; i < c->namedTypeLen; i++) {
            if (c->namedTypes[i].typeId == baseTypeId) {
                namedIndex = (int32_t)i;
                break;
            }
        }
        if (namedIndex >= 0 && SLTCResolveNamedTypeFields(c, (uint32_t)namedIndex) != 0) {
            return 0;
        }
        t = &c->types[baseTypeId];
    }
    if (t->kind != SLTCType_NAMED && t->kind != SLTCType_ANON_STRUCT) {
        return 0;
    }
    if (outBaseTypeId != NULL) {
        *outBaseTypeId = baseTypeId;
    }
    return 1;
}

static int SLTCMirConstAggregateLookupFieldIndex(
    SLTypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t*       outFieldIndex) {
    int32_t  baseTypeId = -1;
    int32_t  fieldType = -1;
    uint32_t absFieldIndex = UINT32_MAX;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (!SLTCMirConstResolveAggregateType(c, typeId, &baseTypeId)) {
        return 0;
    }
    if (SLTCFieldLookup(c, baseTypeId, nameStart, nameEnd, &fieldType, &absFieldIndex) != 0) {
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

static int SLTCMirConstZeroInitTypeId(
    SLTCConstEvalCtx* evalCtx, int32_t typeId, SLCTFEValue* outValue, int* outIsConst);

static int SLTCMirConstMakeAggregateValue(
    SLTCConstEvalCtx* evalCtx, int32_t typeId, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*        c;
    int32_t                baseTypeId = -1;
    uint32_t               fieldCount = 0;
    uint32_t               i;
    size_t                 bytes;
    SLTCMirConstAggregate* agg;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        SLTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (!SLTCMirConstResolveAggregateType(c, typeId, &baseTypeId)) {
        return 0;
    }
    fieldCount = c->types[baseTypeId].fieldCount;
    bytes = sizeof(*agg) + sizeof(SLCTFEValue) * (size_t)fieldCount;
    agg = (SLTCMirConstAggregate*)SLArenaAlloc(
        c->arena, (uint32_t)bytes, (uint32_t)_Alignof(SLTCMirConstAggregate));
    if (agg == NULL) {
        return -1;
    }
    agg->typeId = baseTypeId;
    agg->fieldCount = fieldCount;
    memset(agg->fields, 0, sizeof(SLCTFEValue) * fieldCount);
    for (i = 0; i < fieldCount; i++) {
        uint32_t fieldIndex = c->types[baseTypeId].fieldStart + i;
        if (fieldIndex >= c->fieldLen
            || SLTCMirConstZeroInitTypeId(
                   evalCtx, c->fields[fieldIndex].typeId, &agg->fields[i], outIsConst)
                   != 0)
        {
            return -1;
        }
        if (!*outIsConst) {
            return 0;
        }
    }
    outValue->kind = SLCTFEValue_AGGREGATE;
    outValue->typeTag = (uint64_t)(uint32_t)baseTypeId;
    outValue->s.bytes = (const uint8_t*)agg;
    outValue->s.len = fieldCount;
    *outIsConst = 1;
    return 0;
}

int SLTCMirConstAggGetField(
    void* _Nullable ctx,
    const SLCTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx*            evalCtx = (SLTCConstEvalCtx*)ctx;
    const SLTCMirConstAggregate* agg;
    uint32_t                     fieldIndex = UINT32_MAX;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        SLTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (base != NULL && base->kind == SLCTFEValue_SPAN
        && base->typeTag == SL_TC_MIR_IMPORT_ALIAS_TAG)
    {
        SLTypeCheckCtx* c = evalCtx->tc;
        const uint8_t*  srcBytes = c != NULL ? (const uint8_t*)c->src.ptr : NULL;
        const uint8_t*  aliasBytes = base->span.fileBytes;
        uint32_t        aliasLen = base->span.fileLen;
        uint32_t        aliasStart;
        uint32_t        aliasEnd;
        int32_t         fnIndex;
        if (c == NULL || srcBytes == NULL || aliasBytes == NULL || aliasLen == 0u
            || aliasBytes < srcBytes || (uint64_t)(aliasBytes - srcBytes) > UINT32_MAX
            || (uint64_t)(aliasBytes - srcBytes) + aliasLen > c->src.len)
        {
            return 0;
        }
        aliasStart = (uint32_t)(aliasBytes - srcBytes);
        aliasEnd = aliasStart + aliasLen;
        fnIndex = SLTCFindPkgQualifiedFunctionValueIndex(
            c, aliasStart, aliasEnd, nameStart, nameEnd);
        if (fnIndex < 0) {
            return 0;
        }
        SLMirValueSetFunctionRef(outValue, (uint32_t)fnIndex);
        *outIsConst = 1;
        return 0;
    }
    agg = SLTCMirConstAggregateFromValue(base);
    if (agg == NULL
        || !SLTCMirConstAggregateLookupFieldIndex(
            evalCtx->tc, agg->typeId, nameStart, nameEnd, &fieldIndex))
    {
        return 0;
    }
    {
        SLCTFEValue* fieldValue = SLTCMirConstAggregateFieldValuePtr(agg, fieldIndex);
        if (fieldValue == NULL) {
            return 0;
        }
        *outValue = *fieldValue;
    }
    *outIsConst = 1;
    return 0;
}

int SLTCMirConstAggAddrField(
    void* _Nullable ctx,
    const SLCTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx*            evalCtx = (SLTCConstEvalCtx*)ctx;
    const SLTCMirConstAggregate* agg;
    uint32_t                     fieldIndex = UINT32_MAX;
    SLCTFEValue*                 fieldValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        SLTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || evalCtx->tc == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    agg = SLTCMirConstAggregateFromValue(base);
    if (agg == NULL
        || !SLTCMirConstAggregateLookupFieldIndex(
            evalCtx->tc, agg->typeId, nameStart, nameEnd, &fieldIndex))
    {
        return 0;
    }
    fieldValue = SLTCMirConstAggregateFieldValuePtr(agg, fieldIndex);
    if (fieldValue == NULL) {
        return 0;
    }
    SLTCMirConstSetReference(outValue, fieldValue);
    *outIsConst = 1;
    return 0;
}

static int SLTCMirConstZeroInitTypeId(
    SLTCConstEvalCtx* evalCtx, int32_t typeId, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx* c;
    int32_t         baseTypeId;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        SLTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    baseTypeId = SLTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    if (SLTCIsIntegerType(c, baseTypeId)) {
        outValue->kind = SLCTFEValue_INT;
        outValue->i64 = 0;
        *outIsConst = 1;
        return 0;
    }
    if (SLTCIsFloatType(c, baseTypeId)) {
        outValue->kind = SLCTFEValue_FLOAT;
        outValue->f64 = 0.0;
        *outIsConst = 1;
        return 0;
    }
    if (SLTCIsBoolType(c, baseTypeId)) {
        outValue->kind = SLCTFEValue_BOOL;
        outValue->b = 0u;
        *outIsConst = 1;
        return 0;
    }
    if (SLTCIsStringLikeType(c, baseTypeId)) {
        outValue->kind = SLCTFEValue_STRING;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        *outIsConst = 1;
        return 0;
    }
    if (c->types[baseTypeId].kind == SLTCType_OPTIONAL) {
        if (SLTCConstEvalSetOptionalNoneValue(c, baseTypeId, outValue) != 0) {
            return -1;
        }
        *outIsConst = 1;
        return 0;
    }
    if (c->types[baseTypeId].kind == SLTCType_PTR || c->types[baseTypeId].kind == SLTCType_REF
        || c->types[baseTypeId].kind == SLTCType_FUNCTION
        || c->types[baseTypeId].kind == SLTCType_NULL)
    {
        SLTCConstEvalSetNullValue(outValue);
        *outIsConst = 1;
        return 0;
    }
    return SLTCMirConstMakeAggregateValue(evalCtx, baseTypeId, outValue, outIsConst);
}

int SLTCMirConstZeroInitLocal(
    void* _Nullable ctx,
    const SLMirTypeRef* typeRef,
    SLCTFEValue*        outValue,
    int*                outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    int32_t           typeId = -1;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue != NULL) {
        SLTCConstEvalValueInvalid(outValue);
    }
    if (evalCtx == NULL || typeRef == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (evalCtx->tc == NULL || SLTCMirConstResolveTypeRefTypeId(evalCtx, typeRef, &typeId) != 0) {
        return -1;
    }
    return SLTCMirConstZeroInitTypeId(evalCtx, typeId, outValue, outIsConst);
}

int SLTCMirConstCoerceValueForType(
    void* _Nullable ctx,
    const SLMirTypeRef* typeRef,
    SLCTFEValue*        inOutValue,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*   c;
    int32_t           typeId = -1;
    int32_t           baseTypeId;
    (void)diag;
    if (evalCtx == NULL || typeRef == NULL || inOutValue == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || SLTCMirConstResolveTypeRefTypeId(evalCtx, typeRef, &typeId) != 0) {
        return -1;
    }
    baseTypeId = SLTCResolveAliasBaseType(c, typeId);
    if (baseTypeId < 0 || (uint32_t)baseTypeId >= c->typeLen) {
        return 0;
    }
    if ((c->types[baseTypeId].kind == SLTCType_NAMED
         || c->types[baseTypeId].kind == SLTCType_ANON_STRUCT)
        && inOutValue->kind == SLCTFEValue_AGGREGATE)
    {
        if (inOutValue->typeTag == (uint64_t)(uint32_t)baseTypeId) {
            return 0;
        }
        return 0;
    }
    if (c->types[baseTypeId].kind == SLTCType_OPTIONAL) {
        SLCTFEValue wrapped;
        if (inOutValue->kind == SLCTFEValue_OPTIONAL) {
            if (inOutValue->typeTag > 0 && inOutValue->typeTag <= (uint64_t)INT32_MAX
                && (uint32_t)inOutValue->typeTag < c->typeLen
                && (int32_t)inOutValue->typeTag == baseTypeId)
            {
                return 0;
            }
            if (inOutValue->b == 0u) {
                if (SLTCConstEvalSetOptionalNoneValue(c, baseTypeId, &wrapped) != 0) {
                    return -1;
                }
            } else if (inOutValue->s.bytes == NULL) {
                return 0;
            } else if (
                SLTCConstEvalSetOptionalSomeValue(
                    c, baseTypeId, (const SLCTFEValue*)inOutValue->s.bytes, &wrapped)
                != 0)
            {
                return -1;
            }
            *inOutValue = wrapped;
            return 0;
        }
        if (inOutValue->kind == SLCTFEValue_NULL) {
            if (SLTCConstEvalSetOptionalNoneValue(c, baseTypeId, &wrapped) != 0) {
                return -1;
            }
            *inOutValue = wrapped;
            return 0;
        }
        if (SLTCConstEvalSetOptionalSomeValue(c, baseTypeId, inOutValue, &wrapped) != 0) {
            return -1;
        }
        *inOutValue = wrapped;
        return 0;
    }
    if (SLTCIsIntegerType(c, baseTypeId)) {
        if (inOutValue->kind == SLCTFEValue_BOOL) {
            inOutValue->kind = SLCTFEValue_INT;
            inOutValue->i64 = inOutValue->b ? 1 : 0;
        } else if (inOutValue->kind == SLCTFEValue_FLOAT) {
            inOutValue->kind = SLCTFEValue_INT;
            inOutValue->i64 = (int64_t)inOutValue->f64;
            inOutValue->f64 = 0.0;
        } else if (inOutValue->kind == SLCTFEValue_NULL) {
            inOutValue->kind = SLCTFEValue_INT;
            inOutValue->i64 = 0;
        }
        return 0;
    }
    if (SLTCIsFloatType(c, baseTypeId)) {
        if (inOutValue->kind == SLCTFEValue_INT) {
            inOutValue->kind = SLCTFEValue_FLOAT;
            inOutValue->f64 = (double)inOutValue->i64;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == SLCTFEValue_BOOL) {
            inOutValue->kind = SLCTFEValue_FLOAT;
            inOutValue->f64 = inOutValue->b ? 1.0 : 0.0;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == SLCTFEValue_NULL) {
            inOutValue->kind = SLCTFEValue_FLOAT;
            inOutValue->f64 = 0.0;
        }
        return 0;
    }
    if (SLTCIsBoolType(c, baseTypeId)) {
        if (inOutValue->kind == SLCTFEValue_INT) {
            inOutValue->kind = SLCTFEValue_BOOL;
            inOutValue->b = inOutValue->i64 != 0 ? 1u : 0u;
            inOutValue->i64 = 0;
        } else if (inOutValue->kind == SLCTFEValue_FLOAT) {
            inOutValue->kind = SLCTFEValue_BOOL;
            inOutValue->b = inOutValue->f64 != 0.0 ? 1u : 0u;
            inOutValue->f64 = 0.0;
        } else if (inOutValue->kind == SLCTFEValue_NULL) {
            inOutValue->kind = SLCTFEValue_BOOL;
            inOutValue->b = 0u;
        }
        return 0;
    }
    return 0;
}

enum {
    SL_TC_MIR_CONST_FN_NONE = UINT32_MAX,
};

int SLTCMirConstLowerFunction(
    SLTCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex);

int SLTCMirConstInitLowerCtx(SLTCConstEvalCtx* evalCtx, SLTCMirConstLowerCtx* _Nonnull outCtx) {
    SLTypeCheckCtx* c;
    uint32_t*       tcToMir;
    uint8_t*        loweringFns;
    uint32_t*       topConstToMir;
    uint8_t*        loweringTopConsts;
    uint32_t        i;
    if (outCtx == NULL) {
        return -1;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    if (evalCtx == NULL || evalCtx->tc == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    tcToMir = (uint32_t*)SLArenaAlloc(
        c->arena, sizeof(uint32_t) * c->funcLen, (uint32_t)_Alignof(uint32_t));
    loweringFns = (uint8_t*)SLArenaAlloc(
        c->arena, sizeof(uint8_t) * c->funcLen, (uint32_t)_Alignof(uint8_t));
    topConstToMir = (uint32_t*)SLArenaAlloc(
        c->arena, sizeof(uint32_t) * c->ast->len, (uint32_t)_Alignof(uint32_t));
    loweringTopConsts = (uint8_t*)SLArenaAlloc(
        c->arena, sizeof(uint8_t) * c->ast->len, (uint32_t)_Alignof(uint8_t));
    if (tcToMir == NULL || loweringFns == NULL || topConstToMir == NULL
        || loweringTopConsts == NULL)
    {
        return -1;
    }
    for (i = 0; i < c->funcLen; i++) {
        tcToMir[i] = SL_TC_MIR_CONST_FN_NONE;
        loweringFns[i] = 0u;
    }
    for (i = 0; i < c->ast->len; i++) {
        topConstToMir[i] = SL_TC_MIR_CONST_FN_NONE;
        loweringTopConsts[i] = 0u;
    }
    SLMirProgramBuilderInit(&outCtx->builder, c->arena);
    outCtx->evalCtx = evalCtx;
    outCtx->tcToMir = tcToMir;
    outCtx->loweringFns = loweringFns;
    outCtx->topConstToMir = topConstToMir;
    outCtx->loweringTopConsts = loweringTopConsts;
    outCtx->diag = c->diag;
    return 0;
}

static int SLTCMirConstGetFunctionBody(
    SLTypeCheckCtx* c, int32_t fnIndex, int32_t* outFnNode, int32_t* outBodyNode) {
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
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != SLAst_FN) {
        return 0;
    }
    child = SLAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == SLAst_BLOCK) {
            if (bodyNode >= 0) {
                return 0;
            }
            bodyNode = child;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    if (bodyNode < 0) {
        return 0;
    }
    *outFnNode = fnNode;
    *outBodyNode = bodyNode;
    return 1;
}

static int SLTCMirConstMatchPlainCallNode(
    const SLTypeCheckCtx* tc,
    const SLMirSymbolRef* symbol,
    int32_t               rootNode,
    uint32_t              encodedArgCount,
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
        int32_t          nodeId = stack[--stackLen];
        const SLAstNode* node = &tc->ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == SLAst_CALL) {
            int32_t          calleeNode = node->firstChild;
            const SLAstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            if (callee != NULL && callee->kind == SLAst_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && SLTCListCount(tc->ast, nodeId) == encodedArgCount)
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

static int SLTCMirConstResolveDirectCallTarget(
    const SLTCMirConstLowerCtx* c, int32_t rootNode, const SLMirInst* ins, int32_t* outFnIndex) {
    const SLMirSymbolRef* symbol;
    SLTypeCheckCtx*       tc;
    SLTCCallArgInfo       callArgs[SLTC_MAX_CALL_ARGS];
    int32_t               callNode = -1;
    int32_t               calleeNode = -1;
    int32_t               fnIndex = -1;
    int32_t               mutRefTempArgNode = -1;
    uint32_t              argCount = 0;
    int                   status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL
        || !SLTCMirConstMatchPlainCallNode(
            tc, symbol, rootNode, (uint32_t)ins->tok, &callNode, &calleeNode))
    {
        return 0;
    }
    if (SLTCCollectCallArgInfo(tc, callNode, calleeNode, 0, -1, callArgs, NULL, &argCount) != 0) {
        return -1;
    }
    status = SLTCResolveCallByName(
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

static int SLTCMirConstHasImportAlias(
    const SLTCMirConstLowerCtx* c, uint32_t aliasStart, uint32_t aliasEnd) {
    SLTypeCheckCtx* tc;
    if (c == NULL || c->evalCtx == NULL) {
        return 0;
    }
    tc = c->evalCtx->tc;
    return tc != NULL && SLTCHasImportAlias(tc, aliasStart, aliasEnd);
}

static int SLTCMirConstMatchQualifiedCallNode(
    const SLTypeCheckCtx* tc,
    const SLMirSymbolRef* symbol,
    int32_t               rootNode,
    uint32_t              encodedArgCount,
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
        int32_t          nodeId = stack[--stackLen];
        const SLAstNode* node = &tc->ast->nodes[nodeId];
        int32_t          child;
        if (node->kind == SLAst_CALL) {
            int32_t          calleeNode = node->firstChild;
            const SLAstNode* callee = calleeNode >= 0 ? &tc->ast->nodes[calleeNode] : NULL;
            int32_t recvNode = calleeNode >= 0 ? tc->ast->nodes[calleeNode].firstChild : -1;
            if (callee != NULL && callee->kind == SLAst_FIELD_EXPR && recvNode >= 0
                && (uint32_t)recvNode < tc->ast->len && tc->ast->nodes[recvNode].kind == SLAst_IDENT
                && callee->dataStart == symbol->nameStart && callee->dataEnd == symbol->nameEnd
                && SLTCListCount(tc->ast, nodeId)
                       == (encodedArgCount & ~SLMirCallArgFlag_RECEIVER_ARG0))
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

static int SLTCMirConstResolveQualifiedCallTarget(
    const SLTCMirConstLowerCtx* c, int32_t rootNode, const SLMirInst* ins, int32_t* outFnIndex) {
    SLTypeCheckCtx*       tc;
    const SLMirSymbolRef* symbol;
    SLTCCallArgInfo       callArgs[SLTC_MAX_CALL_ARGS];
    int32_t               callNode = -1;
    int32_t               calleeNode = -1;
    int32_t               recvNode = -1;
    int32_t               fnIndex = -1;
    int32_t               mutRefTempArgNode = -1;
    uint32_t              argCount = 0;
    uint32_t              baseStart = 0;
    uint32_t              baseEnd = 0;
    int                   status;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL
        || !SLTCMirConstMatchQualifiedCallNode(
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
    if (!SLTCMirConstHasImportAlias(c, baseStart, baseEnd)) {
        return 0;
    }
    if (SLTCCollectCallArgInfo(tc, callNode, calleeNode, 1, recvNode, callArgs, NULL, &argCount)
        != 0)
    {
        return -1;
    }
    status = SLTCResolveCallByPkgMethod(
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

static int SLTCMirConstResolveFunctionIdentTarget(
    const SLTCMirConstLowerCtx* c, const SLMirInst* ins, int32_t* outFnIndex) {
    SLTypeCheckCtx* tc;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != SLMirOp_LOAD_IDENT)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    *outFnIndex = SLTCFindPlainFunctionValueIndex(tc, ins->start, ins->end);
    if (*outFnIndex < 0) {
        return 0;
    }
    return 1;
}

static int SLTCMirConstResolveQualifiedFunctionValueTarget(
    const SLTCMirConstLowerCtx* c,
    const SLMirInst*            loadIns,
    const SLMirInst*            fieldIns,
    int32_t*                    outFnIndex) {
    SLTypeCheckCtx* tc;
    uint32_t        fieldStart;
    uint32_t        fieldEnd;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || loadIns == NULL || fieldIns == NULL || outFnIndex == NULL
        || loadIns->op != SLMirOp_LOAD_IDENT || fieldIns->op != SLMirOp_AGG_GET)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || !SLTCHasImportAlias(tc, loadIns->start, loadIns->end)) {
        return 0;
    }
    if (c->builder.fields != NULL && fieldIns->aux < c->builder.fieldLen) {
        fieldStart = c->builder.fields[fieldIns->aux].nameStart;
        fieldEnd = c->builder.fields[fieldIns->aux].nameEnd;
    } else {
        fieldStart = fieldIns->start;
        fieldEnd = fieldIns->end;
    }
    *outFnIndex = SLTCFindPkgQualifiedFunctionValueIndex(
        tc, loadIns->start, loadIns->end, fieldStart, fieldEnd);
    return *outFnIndex >= 0;
}

static int SLTCMirConstRewriteQualifiedFunctionValueLoad(
    SLTCMirConstLowerCtx* c,
    uint32_t              ownerMirFnIndex,
    uint32_t              loadInstIndex,
    uint32_t              targetMirFnIndex) {
    SLMirInst* loadIns;
    SLMirInst* fieldIns;
    SLMirInst  inserted = { 0 };
    SLMirConst value = { 0 };
    uint32_t   constIndex = UINT32_MAX;
    if (c == NULL || ownerMirFnIndex >= c->builder.funcLen || loadInstIndex >= c->builder.instLen
        || loadInstIndex + 1u >= c->builder.instLen)
    {
        return -1;
    }
    loadIns = &c->builder.insts[loadInstIndex];
    fieldIns = &c->builder.insts[loadInstIndex + 1u];
    value.kind = SLMirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (SLMirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
        return -1;
    }
    inserted.op = SLMirOp_PUSH_CONST;
    inserted.aux = constIndex;
    inserted.start = fieldIns->start;
    inserted.end = fieldIns->end;
    if (SLMirProgramBuilderInsertInst(
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
    loadIns->op = SLMirOp_PUSH_NULL;
    loadIns->tok = 0u;
    loadIns->aux = 0u;
    fieldIns->op = SLMirOp_DROP;
    fieldIns->tok = 0u;
    fieldIns->aux = 0u;
    return 0;
}

static int SLTCMirConstResolveTopConstIdentTarget(
    const SLTCMirConstLowerCtx* c, const SLMirInst* ins, int32_t* outNodeId) {
    SLTypeCheckCtx*  tc;
    int32_t          nodeId = -1;
    int32_t          nameIndex = -1;
    SLTCVarLikeParts parts;
    const SLAstNode* n;
    if (outNodeId != NULL) {
        *outNodeId = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outNodeId == NULL
        || ins->op != SLMirOp_LOAD_IDENT)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    nodeId = SLTCFindTopLevelVarLikeNode(tc, ins->start, ins->end, &nameIndex);
    if (nodeId < 0 || (uint32_t)nodeId >= tc->ast->len || nameIndex != 0) {
        return 0;
    }
    n = &tc->ast->nodes[nodeId];
    if (n->kind != SLAst_CONST || SLTCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped
        || parts.nameCount != 1u)
    {
        return 0;
    }
    *outNodeId = nodeId;
    return 1;
}

static int SLTCMirConstResolveSimpleTopConstFunctionValueTarget(
    const SLTCMirConstLowerCtx* c, int32_t nodeId, int32_t* outFnIndex) {
    SLTypeCheckCtx*  tc;
    SLTCVarLikeParts parts;
    int32_t          initNode;
    const SLAstNode* initExpr;
    int32_t          targetFnIndex;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || outFnIndex == NULL) {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL || nodeId < 0 || (uint32_t)nodeId >= tc->ast->len) {
        return 0;
    }
    if (SLTCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped || parts.nameCount != 1u
        || tc->ast->nodes[nodeId].kind != SLAst_CONST)
    {
        return 0;
    }
    initNode = SLTCVarLikeInitExprNodeAt(tc, nodeId, 0);
    if (initNode < 0 || (uint32_t)initNode >= tc->ast->len) {
        return 0;
    }
    initExpr = &tc->ast->nodes[initNode];
    if (initExpr->kind != SLAst_IDENT) {
        return 0;
    }
    targetFnIndex = SLTCFindPlainFunctionValueIndex(tc, initExpr->dataStart, initExpr->dataEnd);
    if (targetFnIndex < 0 || (uint32_t)targetFnIndex >= tc->funcLen) {
        return 0;
    }
    *outFnIndex = targetFnIndex;
    return 1;
}

static int SLTCMirConstResolveSimpleFunctionValueAliasCallTarget(
    const SLTCMirConstLowerCtx* c, const SLMirInst* ins, int32_t* outFnIndex) {
    SLTypeCheckCtx*       tc;
    const SLMirSymbolRef* symbol;
    int32_t               nodeId = -1;
    int32_t               nameIndex = -1;
    if (outFnIndex != NULL) {
        *outFnIndex = -1;
    }
    if (c == NULL || c->evalCtx == NULL || ins == NULL || outFnIndex == NULL
        || ins->op != SLMirOp_CALL || c->builder.symbols == NULL
        || ins->aux >= c->builder.symbolLen)
    {
        return 0;
    }
    tc = c->evalCtx->tc;
    if (tc == NULL) {
        return 0;
    }
    symbol = &c->builder.symbols[ins->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->flags != 0u) {
        return 0;
    }
    nodeId = SLTCFindTopLevelVarLikeNode(tc, symbol->nameStart, symbol->nameEnd, &nameIndex);
    if (nodeId < 0 || nameIndex != 0) {
        return 0;
    }
    return SLTCMirConstResolveSimpleTopConstFunctionValueTarget(c, nodeId, outFnIndex);
}

static int SLTCMirConstLowerTopConstNode(
    SLTCMirConstLowerCtx* c, int32_t nodeId, uint32_t* _Nullable outMirFnIndex) {
    SLTypeCheckCtx*  tc;
    SLTCVarLikeParts parts;
    uint32_t         mirFnIndex = UINT32_MAX;
    int32_t          initNode;
    int              supported = 0;
    int              rewriteRc;
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
    if (c->topConstToMir[(uint32_t)nodeId] != SL_TC_MIR_CONST_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->topConstToMir[(uint32_t)nodeId];
        }
        return 1;
    }
    if (c->loweringTopConsts[(uint32_t)nodeId] != 0u) {
        return 0;
    }
    if (SLTCVarLikeGetParts(tc, nodeId, &parts) != 0 || parts.grouped || parts.nameCount != 1u
        || tc->ast->nodes[nodeId].kind != SLAst_CONST)
    {
        return 0;
    }
    initNode = SLTCVarLikeInitExprNodeAt(tc, nodeId, 0);
    if (initNode < 0) {
        return 0;
    }
    c->loweringTopConsts[(uint32_t)nodeId] = 1u;
    if (SLMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
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
    c->topConstToMir[(uint32_t)nodeId] = mirFnIndex;
    rewriteRc = SLTCMirConstRewriteDirectCalls(c, mirFnIndex, initNode);
    c->loweringTopConsts[(uint32_t)nodeId] = 0u;
    if (rewriteRc < 0) {
        return -1;
    }
    if (rewriteRc == 0) {
        c->topConstToMir[(uint32_t)nodeId] = SL_TC_MIR_CONST_FN_NONE;
        return 0;
    }
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int SLTCMirConstRewriteLoadIdentToFunctionConst(
    SLTCMirConstLowerCtx* c, SLMirInst* ins, uint32_t targetMirFnIndex) {
    SLMirConst value = { 0 };
    uint32_t   constIndex = UINT32_MAX;
    if (c == NULL || ins == NULL) {
        return -1;
    }
    value.kind = SLMirConst_FUNCTION;
    value.bits = targetMirFnIndex;
    if (SLMirProgramBuilderAddConst(&c->builder, &value, &constIndex) != 0) {
        return -1;
    }
    ins->op = SLMirOp_PUSH_CONST;
    ins->tok = 0u;
    ins->aux = constIndex;
    return 0;
}

int SLTCMirConstRewriteDirectCalls(SLTCMirConstLowerCtx* c, uint32_t mirFnIndex, int32_t rootNode) {
    SLTypeCheckCtx* tc;
    uint32_t        instIndex;
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
        SLMirInst* ins = &c->builder.insts[instIndex];
        SLMirInst* nextIns =
            instIndex + 1u < c->builder.instLen ? &c->builder.insts[instIndex + 1u] : NULL;
        int32_t  targetFnIndex = -1;
        int32_t  targetTopConstNode = -1;
        uint32_t targetMirFnIndex = UINT32_MAX;
        int      lowerRc;
        lowerRc = SLTCMirConstResolveQualifiedFunctionValueTarget(c, ins, nextIns, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = SLTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            if (SLTCMirConstRewriteQualifiedFunctionValueLoad(
                    c, mirFnIndex, instIndex, targetMirFnIndex)
                != 0)
            {
                return -1;
            }
            continue;
        }
        if (SLTCMirConstResolveTopConstIdentTarget(c, ins, &targetTopConstNode)) {
            lowerRc = SLTCMirConstLowerTopConstNode(c, targetTopConstNode, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins->op = SLMirOp_CALL_FN;
            ins->tok = 0u;
            ins->aux = targetMirFnIndex;
            continue;
        }
        if (SLTCMirConstResolveFunctionIdentTarget(c, ins, &targetFnIndex)) {
            lowerRc = SLTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            if (SLTCMirConstRewriteLoadIdentToFunctionConst(c, ins, targetMirFnIndex) != 0) {
                return -1;
            }
            continue;
        }
        if (SLTCMirConstResolveSimpleFunctionValueAliasCallTarget(c, ins, &targetFnIndex)) {
            lowerRc = SLTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins->op = SLMirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = SLTCMirConstResolveQualifiedCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            lowerRc = SLTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
            if (lowerRc < 0) {
                return -1;
            }
            if (lowerRc == 0) {
                return 0;
            }
            ins->op = SLMirOp_CALL_FN;
            ins->aux = targetMirFnIndex;
            continue;
        }
        lowerRc = SLTCMirConstResolveDirectCallTarget(c, rootNode, ins, &targetFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            continue;
        }
        lowerRc = SLTCMirConstLowerFunction(c, targetFnIndex, &targetMirFnIndex);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc == 0) {
            return 0;
        }
        ins->op = SLMirOp_CALL_FN;
        ins->aux = targetMirFnIndex;
    }
    return 1;
}

int SLTCMirConstLowerFunction(
    SLTCMirConstLowerCtx* c, int32_t fnIndex, uint32_t* _Nullable outMirFnIndex) {
    SLTypeCheckCtx* tc;
    uint32_t        mirFnIndex = UINT32_MAX;
    int32_t         fnNode = -1;
    int32_t         bodyNode = -1;
    int             supported = 0;
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
    if (c->tcToMir[(uint32_t)fnIndex] != SL_TC_MIR_CONST_FN_NONE) {
        if (outMirFnIndex != NULL) {
            *outMirFnIndex = c->tcToMir[(uint32_t)fnIndex];
        }
        return 1;
    }
    if (!SLTCMirConstGetFunctionBody(tc, fnIndex, &fnNode, &bodyNode)) {
        return 0;
    }
    {
        int32_t  stack[256];
        uint32_t stackLen = 0;
        stack[stackLen++] = bodyNode;
        while (stackLen > 0) {
            int32_t          nodeId = stack[--stackLen];
            const SLAstNode* node = &tc->ast->nodes[nodeId];
            int32_t          child;
            if (node->kind == SLAst_CONST) {
                int32_t initNode = SLTCVarLikeInitExprNode(tc, nodeId);
                if (initNode >= 0 && (uint32_t)initNode < tc->ast->len
                    && tc->ast->nodes[initNode].kind == SLAst_FIELD_EXPR)
                {
                    int32_t          recvNode = SLAstFirstChild(tc->ast, initNode);
                    const SLAstNode* recv = recvNode >= 0 ? &tc->ast->nodes[recvNode] : NULL;
                    if (recv != NULL && recv->kind == SLAst_IDENT
                        && SLTCHasImportAlias(tc, recv->dataStart, recv->dataEnd))
                    {
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
    }
    c->loweringFns[(uint32_t)fnIndex] = 1u;
    if (SLMirLowerAppendSimpleFunction(
            &c->builder,
            tc->arena,
            tc->ast,
            tc->src,
            fnNode,
            bodyNode,
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
    c->tcToMir[(uint32_t)fnIndex] = mirFnIndex;
    {
        int rewriteRc = SLTCMirConstRewriteDirectCalls(c, mirFnIndex, bodyNode);
        if (rewriteRc < 0) {
            c->loweringFns[(uint32_t)fnIndex] = 0u;
            return -1;
        }
        if (rewriteRc == 0) {
            c->tcToMir[(uint32_t)fnIndex] = SL_TC_MIR_CONST_FN_NONE;
            c->loweringFns[(uint32_t)fnIndex] = 0u;
            return 0;
        }
    }
    c->loweringFns[(uint32_t)fnIndex] = 0u;
    if (outMirFnIndex != NULL) {
        *outMirFnIndex = mirFnIndex;
    }
    return 1;
}

static int SLTCTryMirConstCall(
    SLTCConstEvalCtx*  evalCtx,
    int32_t            fnIndex,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outDidReturn,
    int*               outIsConst,
    int*               outSupported) {
    SLTypeCheckCtx*      c;
    SLMirProgram         program = { 0 };
    SLMirExecEnv         env = { 0 };
    SLTCMirConstLowerCtx lowerCtx;
    uint32_t             mirFnIndex = UINT32_MAX;
    int                  lowerRc;
    int                  mirIsConst = 0;
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
    if (SLTCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    lowerRc = SLTCMirConstLowerFunction(&lowerCtx, fnIndex, &mirFnIndex);
    if (lowerRc < 0) {
        return -1;
    }
    if (lowerRc == 0 || mirFnIndex == UINT32_MAX) {
        return 0;
    }
    SLMirProgramBuilderFinish(&lowerCtx.builder, &program);
    env.src = c->src;
    env.resolveIdent = SLTCResolveConstIdent;
    env.resolveCall = SLTCResolveConstCall;
    env.resolveCtx = evalCtx;
    env.zeroInitLocal = SLTCMirConstZeroInitLocal;
    env.zeroInitCtx = evalCtx;
    env.coerceValueForType = SLTCMirConstCoerceValueForType;
    env.coerceValueCtx = evalCtx;
    env.indexValue = SLTCMirConstIndexValue;
    env.indexValueCtx = evalCtx;
    env.sequenceLen = SLTCMirConstSequenceLen;
    env.sequenceLenCtx = evalCtx;
    env.iterInit = SLTCMirConstIterInit;
    env.iterInitCtx = evalCtx;
    env.iterNext = SLTCMirConstIterNext;
    env.iterNextCtx = evalCtx;
    env.aggGetField = SLTCMirConstAggGetField;
    env.aggGetFieldCtx = evalCtx;
    env.aggAddrField = SLTCMirConstAggAddrField;
    env.aggAddrFieldCtx = evalCtx;
    env.makeTuple = SLTCMirConstMakeTuple;
    env.makeTupleCtx = evalCtx;
    env.backwardJumpLimit = SLTC_CONST_FOR_MAX_ITERS;
    env.diag = c->diag;
    if (!SLMirProgramNeedsDynamicResolution(&program)) {
        env.resolveIdent = NULL;
        env.resolveCall = NULL;
        env.resolveCtx = NULL;
    }
    if (SLMirEvalFunction(
            c->arena, &program, mirFnIndex, args, argCount, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    *outIsConst = mirIsConst;
    if (mirIsConst) {
        *outDidReturn = outValue->kind != SLCTFEValue_INVALID;
    }
    return 0;
}

int32_t SLTCFindConstCallableFunction(
    SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, uint32_t argCount) {
    uint32_t i;
    int32_t  found = -1;
    for (i = 0; i < c->funcLen; i++) {
        const SLTCFunction* f = &c->funcs[i];
        if (!SLNameEqSlice(c->src, f->nameStart, f->nameEnd, nameStart, nameEnd)) {
            continue;
        }
        if (f->contextType >= 0 || (f->flags & SLTCFunctionFlag_VARIADIC) != 0
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

static int SLTCInvokeConstFunctionByIndex(
    SLTCConstEvalCtx*  evalCtx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    int32_t            fnIndex,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    SLTypeCheckCtx*    c;
    int32_t            fnNode;
    int32_t            bodyNode = -1;
    int32_t            child;
    uint32_t           paramCount = 0;
    SLCTFEExecBinding* paramBindings = NULL;
    SLCTFEExecEnv      paramFrame;
    SLCTFEExecCtx      execCtx;
    SLCTFEExecCtx*     savedExecCtx;
    uint32_t           savedDepth;
    SLCTFEValue        retValue;
    int                didReturn = 0;
    int                isConst = 0;
    int                mirSupported = 0;
    int                rc;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return -1;
    }

    SLTCMarkConstDiagFnInvoked(c, fnIndex);

    for (savedDepth = 0; savedDepth < evalCtx->fnDepth; savedDepth++) {
        if (evalCtx->fnStack[savedDepth] == fnIndex) {
            SLTCConstSetReason(
                evalCtx, nameStart, nameEnd, "recursive const function calls are not supported");
            *outIsConst = 0;
            return 0;
        }
    }
    if (evalCtx->fnDepth >= SLTC_CONST_CALL_MAX_DEPTH) {
        SLTCConstSetReason(evalCtx, nameStart, nameEnd, "const-eval call depth limit exceeded");
        *outIsConst = 0;
        return 0;
    }

    fnNode = c->funcs[fnIndex].defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != SLAst_FN) {
        SLTCConstSetReason(evalCtx, nameStart, nameEnd, "call target has no const-evaluable body");
        *outIsConst = 0;
        return 0;
    }

    child = SLAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            if (paramCount >= argCount) {
                SLTCConstSetReasonNode(
                    evalCtx, fnNode, "function signature does not match const-eval call arguments");
                *outIsConst = 0;
                return 0;
            }
            paramCount++;
        } else if (n->kind == SLAst_BLOCK) {
            if (bodyNode >= 0) {
                SLTCConstSetReasonNode(
                    evalCtx, fnNode, "function body shape is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            bodyNode = child;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    if (paramCount != argCount || bodyNode < 0) {
        SLTCConstSetReasonNode(evalCtx, fnNode, "function body shape is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    if (argCount > 0) {
        paramBindings = (SLCTFEExecBinding*)SLArenaAlloc(
            c->arena, sizeof(SLCTFEExecBinding) * argCount, (uint32_t)_Alignof(SLCTFEExecBinding));
        if (paramBindings == NULL) {
            return SLTCFailNode(c, fnNode, SLDiag_ARENA_OOM);
        }
    }
    paramCount = 0;
    child = SLAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            int32_t paramTypeNode = SLAstFirstChild(c->ast, child);
            int32_t paramTypeId = -1;
            uint8_t savedAllowConstNumericTypeName = c->allowConstNumericTypeName;
            c->allowConstNumericTypeName = 1;
            if (paramTypeNode < 0 || SLTCResolveTypeNode(c, paramTypeNode, &paramTypeId) != 0) {
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
            if (c->types[paramTypeId].kind == SLTCType_OPTIONAL) {
                SLCTFEValue wrapped;
                if (args[paramCount].kind == SLCTFEValue_OPTIONAL) {
                    if (args[paramCount].typeTag > 0
                        && args[paramCount].typeTag <= (uint64_t)INT32_MAX
                        && (uint32_t)args[paramCount].typeTag < c->typeLen
                        && (int32_t)args[paramCount].typeTag == paramTypeId)
                    {
                        wrapped = args[paramCount];
                    } else if (args[paramCount].b == 0u) {
                        if (SLTCConstEvalSetOptionalNoneValue(c, paramTypeId, &wrapped) != 0) {
                            return -1;
                        }
                    } else if (args[paramCount].s.bytes == NULL) {
                        return -1;
                    } else if (
                        SLTCConstEvalSetOptionalSomeValue(
                            c, paramTypeId, (const SLCTFEValue*)args[paramCount].s.bytes, &wrapped)
                        != 0)
                    {
                        return -1;
                    }
                } else if (args[paramCount].kind == SLCTFEValue_NULL) {
                    if (SLTCConstEvalSetOptionalNoneValue(c, paramTypeId, &wrapped) != 0) {
                        return -1;
                    }
                } else if (
                    SLTCConstEvalSetOptionalSomeValue(c, paramTypeId, &args[paramCount], &wrapped)
                    != 0)
                {
                    return -1;
                }
                paramBindings[paramCount].value = wrapped;
            } else {
                paramBindings[paramCount].value = args[paramCount];
            }
            paramCount++;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    savedExecCtx = evalCtx->execCtx;
    savedDepth = evalCtx->fnDepth;

    paramFrame.parent = NULL;
    paramFrame.bindings = paramBindings;
    paramFrame.bindingLen = argCount;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = c->arena;
    execCtx.ast = c->ast;
    execCtx.src = c->src;
    execCtx.diag = c->diag;
    execCtx.env = &paramFrame;
    execCtx.evalExpr = SLTCEvalConstExecExprCb;
    execCtx.evalExprCtx = evalCtx;
    execCtx.resolveType = SLTCEvalConstExecResolveTypeCb;
    execCtx.resolveTypeCtx = evalCtx;
    execCtx.inferValueType = SLTCEvalConstExecInferValueTypeCb;
    execCtx.inferValueTypeCtx = evalCtx;
    execCtx.inferExprType = SLTCEvalConstExecInferExprTypeCb;
    execCtx.inferExprTypeCtx = evalCtx;
    execCtx.isOptionalType = SLTCEvalConstExecIsOptionalTypeCb;
    execCtx.isOptionalTypeCtx = evalCtx;
    execCtx.forInIndex = SLTCEvalConstForInIndexCb;
    execCtx.forInIndexCtx = evalCtx;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = SLTC_CONST_FOR_MAX_ITERS;
    SLCTFEExecResetReason(&execCtx);
    evalCtx->execCtx = &execCtx;
    evalCtx->fnStack[evalCtx->fnDepth++] = fnIndex;

    rc = SLTCTryMirConstCall(
        evalCtx, fnIndex, args, argCount, &retValue, &didReturn, &isConst, &mirSupported);
    if (rc != 0) {
        evalCtx->fnDepth = savedDepth;
        evalCtx->execCtx = savedExecCtx;
        return -1;
    }
    if (mirSupported && isConst) {
        evalCtx->fnDepth = savedDepth;
        evalCtx->execCtx = savedExecCtx;
        if (!didReturn) {
            if (c->funcs[fnIndex].returnType == c->typeVoid) {
                SLTCConstEvalSetNullValue(outValue);
                *outIsConst = 1;
                return 0;
            }
            SLTCConstSetReasonNode(
                evalCtx, bodyNode, "const-evaluable function must produce a const return value");
            *outIsConst = 0;
            return 0;
        }
        *outValue = retValue;
        *outIsConst = 1;
        return 0;
    }
    evalCtx->nonConstReason = NULL;
    evalCtx->nonConstStart = 0;
    evalCtx->nonConstEnd = 0;

    rc = SLCTFEExecEvalBlock(&execCtx, bodyNode, &retValue, &didReturn, &isConst);
    evalCtx->fnDepth = savedDepth;
    evalCtx->execCtx = savedExecCtx;
    if (rc != 0) {
        return -1;
    }
    if (!isConst) {
        if (execCtx.nonConstReason != NULL) {
            evalCtx->nonConstReason = execCtx.nonConstReason;
            evalCtx->nonConstStart = execCtx.nonConstStart;
            evalCtx->nonConstEnd = execCtx.nonConstEnd;
        }
        *outIsConst = 0;
        return 0;
    }
    if (!didReturn) {
        if (c->funcs[fnIndex].returnType == c->typeVoid) {
            SLTCConstEvalSetNullValue(outValue);
            *outIsConst = 1;
            return 0;
        }
        if (execCtx.nonConstReason != NULL) {
            evalCtx->nonConstReason = execCtx.nonConstReason;
            evalCtx->nonConstStart = execCtx.nonConstStart;
            evalCtx->nonConstEnd = execCtx.nonConstEnd;
        }
        SLTCConstSetReasonNode(
            evalCtx, bodyNode, "const-evaluable function must produce a const return value");
        *outIsConst = 0;
        return 0;
    }
    if (fnIndex >= 0 && (uint32_t)fnIndex < c->funcLen) {
        int32_t returnTypeId = c->funcs[fnIndex].returnType;
        int32_t returnBaseTypeId = SLTCResolveAliasBaseType(c, returnTypeId);
        if (returnBaseTypeId >= 0 && (uint32_t)returnBaseTypeId < c->typeLen
            && c->types[returnBaseTypeId].kind == SLTCType_OPTIONAL)
        {
            SLCTFEValue wrapped;
            if (retValue.kind == SLCTFEValue_OPTIONAL) {
                if (retValue.typeTag > 0 && retValue.typeTag <= (uint64_t)INT32_MAX
                    && (uint32_t)retValue.typeTag < c->typeLen
                    && (int32_t)retValue.typeTag == returnBaseTypeId)
                {
                    wrapped = retValue;
                } else if (retValue.b == 0u) {
                    if (SLTCConstEvalSetOptionalNoneValue(c, returnBaseTypeId, &wrapped) != 0) {
                        return -1;
                    }
                } else if (retValue.s.bytes == NULL) {
                    return -1;
                } else if (
                    SLTCConstEvalSetOptionalSomeValue(
                        c, returnBaseTypeId, (const SLCTFEValue*)retValue.s.bytes, &wrapped)
                    != 0)
                {
                    return -1;
                }
            } else if (retValue.kind == SLCTFEValue_NULL) {
                if (SLTCConstEvalSetOptionalNoneValue(c, returnBaseTypeId, &wrapped) != 0) {
                    return -1;
                }
            } else if (
                SLTCConstEvalSetOptionalSomeValue(c, returnBaseTypeId, &retValue, &wrapped) != 0)
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

int SLTCResolveConstCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx* evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*   c;
    int32_t           fnIndex;

    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }

    if (SLNameEqLiteral(c->src, nameStart, nameEnd, "len")) {
        if (argCount == 1u) {
            if (args[0].kind == SLCTFEValue_STRING) {
                outValue->kind = SLCTFEValue_INT;
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
            if (args[0].kind == SLCTFEValue_TYPE) {
                int32_t typeId = -1;
                int32_t baseType;
                if (SLTCDecodeTypeTag(c, args[0].typeTag, &typeId) == 0) {
                    baseType = SLTCResolveAliasBaseType(c, typeId);
                    if (baseType >= 0 && (uint32_t)baseType < c->typeLen) {
                        const SLTCType* t = &c->types[baseType];
                        if (t->kind == SLTCType_PACK) {
                            outValue->kind = SLCTFEValue_INT;
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
                        if (t->kind == SLTCType_ARRAY) {
                            outValue->kind = SLCTFEValue_INT;
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
        SLTCConstSetReason(evalCtx, nameStart, nameEnd, "len() operand is not const-evaluable");
        *outIsConst = 0;
        return 0;
    }

    fnIndex = SLTCFindConstCallableFunction(c, nameStart, nameEnd, argCount);
    if (fnIndex < 0) {
        SLTCConstSetReason(
            evalCtx,
            nameStart,
            nameEnd,
            "call target is not a const-evaluable function for these arguments");
        *outIsConst = 0;
        return 0;
    }
    return SLTCInvokeConstFunctionByIndex(
        evalCtx, nameStart, nameEnd, fnIndex, args, argCount, outValue, outIsConst);
}

static int SLTCConstEvalDirectCall(
    SLTCConstEvalCtx* evalCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLTypeCheckCtx*  c;
    int32_t          calleeNode;
    const SLAstNode* callee;
    SLCTFEValue      calleeValue;
    int              calleeIsConst = 0;
    uint32_t         calleeFnIndex = UINT32_MAX;
    int32_t          argNode;
    uint32_t         argCount = 0;
    uint32_t         argIndex = 0;
    SLCTFEValue*     argValues = NULL;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return -1;
    }
    calleeNode = SLAstFirstChild(c->ast, exprNode);
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (callee == NULL || callee->kind != SLAst_IDENT) {
        return 1;
    }

    argNode = SLAstNextSibling(c->ast, calleeNode);
    while (argNode >= 0) {
        argCount++;
        argNode = SLAstNextSibling(c->ast, argNode);
    }
    if (argCount > 0) {
        argValues = (SLCTFEValue*)SLArenaAlloc(
            c->arena, sizeof(SLCTFEValue) * argCount, (uint32_t)_Alignof(SLCTFEValue));
        if (argValues == NULL) {
            return SLTCFailNode(c, exprNode, SLDiag_ARENA_OOM);
        }
    }

    argNode = SLAstNextSibling(c->ast, calleeNode);
    while (argNode >= 0) {
        int32_t exprArgNode = argNode;
        int     argIsConst = 0;
        if (argIndex >= argCount) {
            return -1;
        }
        if (c->ast->nodes[argNode].kind == SLAst_CALL_ARG) {
            exprArgNode = SLAstFirstChild(c->ast, argNode);
            if (exprArgNode < 0) {
                return -1;
            }
        }
        if (SLTCEvalConstExprNode(evalCtx, exprArgNode, &argValues[argIndex], &argIsConst) != 0) {
            return -1;
        }
        if (!argIsConst) {
            *outIsConst = 0;
            return 0;
        }
        argIndex++;
        argNode = SLAstNextSibling(c->ast, argNode);
    }

    if (SLTCResolveConstIdent(
            evalCtx, callee->dataStart, callee->dataEnd, &calleeValue, &calleeIsConst, c->diag)
        != 0)
    {
        return -1;
    }
    if (calleeIsConst && SLMirValueAsFunctionRef(&calleeValue, &calleeFnIndex)
        && calleeFnIndex < c->funcLen)
    {
        const SLTCFunction* fn = &c->funcs[calleeFnIndex];
        return SLTCInvokeConstFunctionByIndex(
            evalCtx,
            fn->nameStart,
            fn->nameEnd,
            (int32_t)calleeFnIndex,
            argValues,
            argCount,
            outValue,
            outIsConst);
    }

    return SLTCResolveConstCall(
        evalCtx,
        callee->dataStart,
        callee->dataEnd,
        argValues,
        argCount,
        outValue,
        outIsConst,
        c->diag);
}

int SLTCEvalTopLevelConstNodeAt(
    SLTypeCheckCtx*   c,
    SLTCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    int32_t           nameIndex,
    SLCTFEValue*      outValue,
    int*              outIsConst) {
    uint8_t          state;
    int32_t          initNode;
    int              isConst = 0;
    SLTCVarLikeParts parts;
    if (c == NULL || evalCtx == NULL || outValue == NULL || outIsConst == NULL || nodeId < 0
        || (uint32_t)nodeId >= c->ast->len)
    {
        return -1;
    }
    if (c->constEvalState == NULL || c->constEvalValues == NULL) {
        *outIsConst = 0;
        return 0;
    }
    if (SLTCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0 || nameIndex < 0
        || (uint32_t)nameIndex >= parts.nameCount)
    {
        *outIsConst = 0;
        return 0;
    }

    state = c->constEvalState[nodeId];
    if (state == SLTCConstEval_READY) {
        *outValue = c->constEvalValues[nodeId];
        *outIsConst = 1;
        return 0;
    }
    if (state == SLTCConstEval_NONCONST || state == SLTCConstEval_VISITING) {
        if (state == SLTCConstEval_VISITING) {
            SLTCConstSetReasonNode(
                evalCtx, nodeId, "cyclic const dependency is not supported in const evaluation");
        }
        *outIsConst = 0;
        return 0;
    }

    c->constEvalState[nodeId] = SLTCConstEval_VISITING;
    initNode = SLTCVarLikeInitExprNodeAt(c, nodeId, nameIndex);
    if (initNode < 0) {
        SLTCConstSetReasonNode(evalCtx, nodeId, "const declaration is missing an initializer");
        c->constEvalState[nodeId] = SLTCConstEval_NONCONST;
        *outIsConst = 0;
        return 0;
    }
    if (SLTCEvalConstExprNode(evalCtx, initNode, outValue, &isConst) != 0) {
        c->constEvalState[nodeId] = SLTCConstEval_UNSEEN;
        return -1;
    }
    if (!isConst) {
        SLTCConstSetReasonNode(evalCtx, initNode, "const initializer is not const-evaluable");
        c->constEvalState[nodeId] = SLTCConstEval_NONCONST;
        *outIsConst = 0;
        return 0;
    }
    if (!parts.grouped) {
        c->constEvalValues[nodeId] = *outValue;
        c->constEvalState[nodeId] = SLTCConstEval_READY;
    } else {
        c->constEvalState[nodeId] = SLTCConstEval_UNSEEN;
    }
    *outIsConst = 1;
    return 0;
}

int SLTCEvalTopLevelConstNode(
    SLTypeCheckCtx*   c,
    SLTCConstEvalCtx* evalCtx,
    int32_t           nodeId,
    SLCTFEValue*      outValue,
    int*              outIsConst) {
    return SLTCEvalTopLevelConstNodeAt(c, evalCtx, nodeId, 0, outValue, outIsConst);
}

int SLTCConstBoolExpr(SLTypeCheckCtx* c, int32_t nodeId, int* out, int* isConst) {
    SLTCConstEvalCtx  evalCtxStorage;
    SLTCConstEvalCtx* evalCtx = c->activeConstEvalCtx;
    SLCTFEValue       value;
    int               valueIsConst = 0;
    const SLAstNode*  n;
    *isConst = 0;
    *out = 0;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_NOT) {
        int32_t rhsNode = SLAstFirstChild(c->ast, nodeId);
        int     rhsValue = 0;
        int     rhsIsConst = 0;
        if (rhsNode < 0) {
            return -1;
        }
        if (SLTCConstBoolExpr(c, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (rhsIsConst) {
            *out = rhsValue ? 0 : 1;
            *isConst = 1;
        }
        return 0;
    }
    if (n->kind == SLAst_BINARY
        && ((SLTokenKind)n->op == SLTok_EQ || (SLTokenKind)n->op == SLTok_NEQ))
    {
        int32_t lhsNode = SLAstFirstChild(c->ast, nodeId);
        int32_t rhsNode = lhsNode >= 0 ? SLAstNextSibling(c->ast, lhsNode) : -1;
        int32_t extraNode = rhsNode >= 0 ? SLAstNextSibling(c->ast, rhsNode) : -1;
        int32_t lhsTypeId = -1;
        int32_t rhsTypeId = -1;
        int     lhsStatus;
        int     rhsStatus;
        if (lhsNode < 0 || rhsNode < 0 || extraNode >= 0) {
            return -1;
        }
        lhsStatus = SLTCResolveReflectedTypeValueExpr(c, lhsNode, &lhsTypeId);
        if (lhsStatus < 0) {
            return -1;
        }
        rhsStatus = SLTCResolveReflectedTypeValueExpr(c, rhsNode, &rhsTypeId);
        if (rhsStatus < 0) {
            return -1;
        }
        if (lhsStatus == 0 && rhsStatus == 0) {
            *out = (((SLTokenKind)n->op == SLTok_EQ)
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
    if (SLTCEvalConstExprNode(evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx->nonConstReason;
    c->lastConstEvalReasonStart = evalCtx->nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx->nonConstEnd;
    if (!valueIsConst) {
        return 0;
    }
    if (value.kind == SLCTFEValue_OPTIONAL) {
        *out = value.b != 0u ? 1 : 0;
        *isConst = 1;
        return 0;
    }
    if (value.kind != SLCTFEValue_BOOL) {
        c->lastConstEvalReason = "expression evaluated to a non-boolean value";
        c->lastConstEvalReasonStart = c->ast->nodes[nodeId].start;
        c->lastConstEvalReasonEnd = c->ast->nodes[nodeId].end;
        return 0;
    }
    *out = value.b ? 1 : 0;
    *isConst = 1;
    return 0;
}

int SLTCConstIntExpr(SLTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst) {
    SLTCConstEvalCtx  evalCtxStorage;
    SLTCConstEvalCtx* evalCtx = c->activeConstEvalCtx;
    SLCTFEValue       value;
    int               valueIsConst = 0;
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
    if (SLTCEvalConstExprNode(evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx->nonConstReason;
    c->lastConstEvalReasonStart = evalCtx->nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx->nonConstEnd;
    if (!valueIsConst) {
        return 0;
    }
    if (SLCTFEValueToInt64(&value, out) != 0) {
        c->lastConstEvalReason = "expression evaluated to a non-integer value";
        c->lastConstEvalReasonStart = c->ast->nodes[nodeId].start;
        c->lastConstEvalReasonEnd = c->ast->nodes[nodeId].end;
        return 0;
    }
    *isConst = 1;
    return 0;
}

int SLTCConstFloatExpr(SLTypeCheckCtx* c, int32_t nodeId, double* out, int* isConst) {
    SLTCConstEvalCtx evalCtx;
    SLCTFEValue      value;
    int              valueIsConst = 0;
    *isConst = 0;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (SLTCEvalConstExprNode(&evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    if (!valueIsConst) {
        return 0;
    }
    if (value.kind == SLCTFEValue_FLOAT) {
        *out = value.f64;
        *isConst = 1;
        return 0;
    }
    if (value.kind == SLCTFEValue_INT) {
        *out = (double)value.i64;
        *isConst = 1;
        return 0;
    }
    c->lastConstEvalReason = "expression evaluated to a non-float value";
    c->lastConstEvalReasonStart = c->ast->nodes[nodeId].start;
    c->lastConstEvalReasonEnd = c->ast->nodes[nodeId].end;
    return 0;
}

int SLTCConstStringExpr(
    SLTypeCheckCtx* c,
    int32_t         nodeId,
    const uint8_t** outBytes,
    uint32_t*       outLen,
    int*            outIsConst) {
    const SLAstNode* node;
    SLTCConstEvalCtx evalCtx;
    SLCTFEValue      value;
    int              valueIsConst = 0;
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
    while (node->kind == SLAst_CALL_ARG) {
        nodeId = SLAstFirstChild(c->ast, nodeId);
        if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
            return -1;
        }
        node = &c->ast->nodes[nodeId];
    }
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (SLTCEvalConstExprNode(&evalCtx, nodeId, &value, &valueIsConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    if (!valueIsConst || value.kind != SLCTFEValue_STRING) {
        return 0;
    }
    *outBytes = value.s.bytes;
    *outLen = value.s.len;
    *outIsConst = 1;
    return 0;
}

void SLTCMarkRuntimeBoundsCheck(SLTypeCheckCtx* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    ((SLAstNode*)&c->ast->nodes[nodeId])->flags |= SLAstFlag_INDEX_RUNTIME_BOUNDS;
}

SL_API_END
