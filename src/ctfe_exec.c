#include "libsl-impl.h"
#include "ctfe_exec.h"

SL_API_BEGIN

static int SLCTFEExecNameEqSlice(
    SLStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart || aEnd > src.len || bEnd > src.len) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != bEnd - bStart) {
        return 0;
    }
    if (len == 0) {
        return 1;
    }
    return memcmp(src.ptr + aStart, src.ptr + bStart, len) == 0;
}

static int SLCTFEExecStrEq(const char* a, const char* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static int SLCTFEExecStringEq(const SLCTFEString* a, const SLCTFEString* b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    if (a->len != b->len) {
        return 0;
    }
    if (a->len == 0) {
        return 1;
    }
    if (a->bytes == NULL || b->bytes == NULL) {
        return 0;
    }
    return memcmp(a->bytes, b->bytes, a->len) == 0;
}

static int SLCTFEExecOptionalPayload(const SLCTFEValue* opt, const SLCTFEValue** outPayload) {
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

static int SLCTFEExecValueEq(const SLCTFEValue* a, const SLCTFEValue* b, int* outEq) {
    if (a == NULL || b == NULL || outEq == NULL) {
        return 0;
    }
    if (a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
        case SLCTFEValue_INT:    *outEq = a->i64 == b->i64; return 1;
        case SLCTFEValue_FLOAT:  *outEq = a->f64 == b->f64; return 1;
        case SLCTFEValue_BOOL:   *outEq = a->b == b->b; return 1;
        case SLCTFEValue_STRING: *outEq = SLCTFEExecStringEq(&a->s, &b->s); return 1;
        case SLCTFEValue_TYPE:   *outEq = a->typeTag == b->typeTag; return 1;
        case SLCTFEValue_SPAN:
            *outEq =
                a->span.startLine == b->span.startLine && a->span.startColumn == b->span.startColumn
                && a->span.endLine == b->span.endLine && a->span.endColumn == b->span.endColumn
                && a->span.fileLen == b->span.fileLen
                && ((a->span.fileLen == 0)
                    || (a->span.fileBytes != NULL && b->span.fileBytes != NULL
                        && memcmp(a->span.fileBytes, b->span.fileBytes, a->span.fileLen) == 0));
            return 1;
        case SLCTFEValue_NULL:      *outEq = 1; return 1;
        case SLCTFEValue_AGGREGATE: return 0;
        case SLCTFEValue_OPTIONAL:  {
            const SLCTFEValue* pa = NULL;
            const SLCTFEValue* pb = NULL;
            if (!SLCTFEExecOptionalPayload(a, &pa) || !SLCTFEExecOptionalPayload(b, &pb)) {
                return 0;
            }
            if (a->b == 0u || b->b == 0u) {
                *outEq = a->b == b->b;
                return 1;
            }
            return SLCTFEExecValueEq(pa, pb, outEq);
        }
        default: return 0;
    }
}

static int SLCTFEExecIsTypeNodeKind(SLAstKind kind) {
    switch (kind) {
        case SLAst_TYPE_NAME:
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
        case SLAst_TYPE_ARRAY:
        case SLAst_TYPE_VARRAY:
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE:
        case SLAst_TYPE_OPTIONAL:
        case SLAst_TYPE_FN:
        case SLAst_TYPE_TUPLE:
        case SLAst_TYPE_ANON_STRUCT:
        case SLAst_TYPE_ANON_UNION:  return 1;
        default:                     return 0;
    }
}

static int32_t SLCTFEExecVarLikeInitExprNode(const SLAst* ast, int32_t nodeId) {
    int32_t firstChild;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0) {
        return -1;
    }
    if (ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
        int32_t afterNames = ast->nodes[firstChild].nextSibling;
        if (afterNames >= 0 && SLCTFEExecIsTypeNodeKind(ast->nodes[afterNames].kind)) {
            return ast->nodes[afterNames].nextSibling;
        }
        return afterNames;
    }
    if (SLCTFEExecIsTypeNodeKind(ast->nodes[firstChild].kind)) {
        return ast->nodes[firstChild].nextSibling;
    }
    return firstChild;
}

static int32_t SLCTFEExecListItemAt(const SLAst* ast, int32_t listNode, uint32_t index) {
    uint32_t i = 0;
    int32_t  child;
    if (ast == NULL || listNode < 0 || (uint32_t)listNode >= ast->len) {
        return -1;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        if (i == index) {
            return child;
        }
        i++;
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

void SLCTFEExecResetReason(SLCTFEExecCtx* c) {
    c->nonConstReason = NULL;
    c->nonConstStart = 0;
    c->nonConstEnd = 0;
}

void SLCTFEExecSetReason(SLCTFEExecCtx* c, uint32_t start, uint32_t end, const char* reason) {
    if (c == NULL || reason == NULL || reason[0] == '\0' || c->nonConstReason != NULL) {
        return;
    }
    c->nonConstReason = reason;
    c->nonConstStart = start;
    c->nonConstEnd = end;
}

void SLCTFEExecSetReasonNode(SLCTFEExecCtx* c, int32_t nodeId, const char* reason) {
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        SLCTFEExecSetReason(c, 0, 0, reason);
        return;
    }
    SLCTFEExecSetReason(c, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, reason);
}

int SLCTFEExecEnvLookup(
    const SLCTFEExecCtx* c, uint32_t nameStart, uint32_t nameEnd, SLCTFEValue* outValue) {
    const SLCTFEExecEnv* frame;
    if (c == NULL || outValue == NULL) {
        return 0;
    }
    frame = c->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            const SLCTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (SLCTFEExecNameEqSlice(c->src, b->nameStart, b->nameEnd, nameStart, nameEnd)) {
                *outValue = b->value;
                return 1;
            }
        }
        frame = frame->parent;
    }
    return 0;
}

static SLCTFEExecBinding* _Nullable SLCTFEExecEnvFindBinding(
    SLCTFEExecCtx* c, uint32_t nameStart, uint32_t nameEnd) {
    SLCTFEExecEnv* frame = c != NULL ? c->env : NULL;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            SLCTFEExecBinding* b;
            i--;
            b = &frame->bindings[i];
            if (SLCTFEExecNameEqSlice(c->src, b->nameStart, b->nameEnd, nameStart, nameEnd)) {
                return b;
            }
        }
        frame = frame->parent;
    }
    return NULL;
}

static int SLCTFEExecAddI64(int64_t a, int64_t b, int64_t* out) {
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

static int SLCTFEExecSubI64(int64_t a, int64_t b, int64_t* out) {
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

static int SLCTFEExecMulI64(int64_t a, int64_t b, int64_t* out) {
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

static int SLCTFEExecIsAssignToken(SLTokenKind op) {
    switch (op) {
        case SLTok_ASSIGN:
        case SLTok_ADD_ASSIGN:
        case SLTok_SUB_ASSIGN:
        case SLTok_MUL_ASSIGN:
        case SLTok_DIV_ASSIGN:
        case SLTok_MOD_ASSIGN:
        case SLTok_AND_ASSIGN:
        case SLTok_OR_ASSIGN:
        case SLTok_XOR_ASSIGN:
        case SLTok_LSHIFT_ASSIGN:
        case SLTok_RSHIFT_ASSIGN: return 1;
        default:                  return 0;
    }
}

static int SLCTFEExecValueToF64(const SLCTFEValue* value, double* out) {
    if (value == NULL || out == NULL) {
        return 0;
    }
    if (value->kind == SLCTFEValue_FLOAT) {
        *out = value->f64;
        return 1;
    }
    if (value->kind == SLCTFEValue_INT) {
        *out = (double)value->i64;
        return 1;
    }
    return 0;
}

static int SLCTFEExecTypeIsOptional(
    SLCTFEExecCtx* c, int32_t typeId, int32_t* outPayloadTypeId, int* outIsOptional) {
    if (outIsOptional == NULL) {
        return -1;
    }
    *outIsOptional = 0;
    if (outPayloadTypeId != NULL) {
        *outPayloadTypeId = -1;
    }
    if (c == NULL || c->isOptionalType == NULL) {
        return 0;
    }
    return c->isOptionalType(c->isOptionalTypeCtx, typeId, outPayloadTypeId, outIsOptional);
}

static int SLCTFEExecWrapValueForOptionalType(
    SLCTFEExecCtx* c, int32_t optionalTypeId, const SLCTFEValue* inValue, SLCTFEValue* outValue) {
    SLCTFEValue* payloadCopy;
    int32_t      payloadTypeId = -1;
    int          isOptional = 0;
    if (c == NULL || inValue == NULL || outValue == NULL) {
        return -1;
    }
    if (SLCTFEExecTypeIsOptional(c, optionalTypeId, &payloadTypeId, &isOptional) != 0) {
        return -1;
    }
    if (!isOptional) {
        *outValue = *inValue;
        return 0;
    }
    if (inValue->kind == SLCTFEValue_OPTIONAL) {
        if (inValue->typeTag > 0 && inValue->typeTag <= (uint64_t)INT32_MAX
            && (int32_t)inValue->typeTag == optionalTypeId)
        {
            *outValue = *inValue;
            return 0;
        }
        if (inValue->b == 0u) {
            *outValue = *inValue;
            outValue->kind = SLCTFEValue_OPTIONAL;
            outValue->b = 0u;
            outValue->typeTag = (uint64_t)(uint32_t)optionalTypeId;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            return 0;
        }
        if (inValue->s.bytes == NULL) {
            return -1;
        }
        payloadCopy = (SLCTFEValue*)SLArenaAlloc(
            c->arena, sizeof(SLCTFEValue), (uint32_t)_Alignof(SLCTFEValue));
        if (payloadCopy == NULL) {
            return -1;
        }
        *payloadCopy = *(const SLCTFEValue*)inValue->s.bytes;
        *outValue = *inValue;
        outValue->kind = SLCTFEValue_OPTIONAL;
        outValue->b = 1u;
        outValue->typeTag = (uint64_t)(uint32_t)optionalTypeId;
        outValue->s.bytes = (const uint8_t*)payloadCopy;
        outValue->s.len = 0;
        return 0;
    }
    if (inValue->kind == SLCTFEValue_NULL) {
        *outValue = *inValue;
        outValue->kind = SLCTFEValue_OPTIONAL;
        outValue->b = 0u;
        outValue->typeTag = (uint64_t)(uint32_t)optionalTypeId;
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        return 0;
    }
    payloadCopy = (SLCTFEValue*)SLArenaAlloc(
        c->arena, sizeof(SLCTFEValue), (uint32_t)_Alignof(SLCTFEValue));
    if (payloadCopy == NULL) {
        return -1;
    }
    *payloadCopy = *inValue;
    *outValue = *inValue;
    outValue->kind = SLCTFEValue_OPTIONAL;
    outValue->b = 1u;
    outValue->typeTag = (uint64_t)(uint32_t)optionalTypeId;
    outValue->s.bytes = (const uint8_t*)payloadCopy;
    outValue->s.len = 0;
    return 0;
}

static int SLCTFEExecEvalExpr(
    SLCTFEExecCtx* c, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    int rc;
    if (c == NULL || outValue == NULL || outIsConst == NULL || c->evalExpr == NULL) {
        return -1;
    }
    rc = c->evalExpr(c->evalExprCtx, exprNode, outValue, outIsConst);
    if (rc == 0 && !*outIsConst) {
        SLCTFEExecSetReasonNode(c, exprNode, "expression is not const-evaluable");
    }
    return rc;
}

static int SLCTFEExecEvalExprForType(
    SLCTFEExecCtx* c, int32_t exprNode, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst) {
    int rc;
    if (c == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (typeNode >= 0 && c->evalExprForType != NULL) {
        rc = c->evalExprForType(c->evalExprForTypeCtx, exprNode, typeNode, outValue, outIsConst);
        if (rc == 0 && !*outIsConst) {
            SLCTFEExecSetReasonNode(c, exprNode, "expression is not const-evaluable");
        }
        return rc;
    }
    return SLCTFEExecEvalExpr(c, exprNode, outValue, outIsConst);
}

static int SLCTFEExecEvalAssignExpr(
    SLCTFEExecCtx* c, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    const SLAstNode*   n;
    int32_t            lhsNode;
    int32_t            rhsNode;
    const SLAstNode*   lhs;
    SLCTFEExecBinding* b;
    SLCTFEValue        rhsValue;
    int                rhsIsConst = 0;

    if (c == NULL || outValue == NULL || outIsConst == NULL || c->ast == NULL || exprNode < 0
        || (uint32_t)exprNode >= c->ast->len)
    {
        *outIsConst = 0;
        return 0;
    }

    n = &c->ast->nodes[exprNode];
    if (n->kind != SLAst_BINARY || !SLCTFEExecIsAssignToken((SLTokenKind)n->op)) {
        *outIsConst = 0;
        return 0;
    }

    lhsNode = c->ast->nodes[exprNode].firstChild;
    rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
    if (lhsNode < 0 || rhsNode < 0 || c->ast->nodes[rhsNode].nextSibling >= 0) {
        *outIsConst = 0;
        return 0;
    }
    lhs = &c->ast->nodes[lhsNode];
    if (lhs->kind != SLAst_IDENT) {
        if (c->assignExpr != NULL) {
            return c->assignExpr(c->assignExprCtx, c, exprNode, outValue, outIsConst);
        }
        *outIsConst = 0;
        return 0;
    }

    b = SLCTFEExecEnvFindBinding(c, lhs->dataStart, lhs->dataEnd);
    if (b == NULL) {
        if (c->assignExpr != NULL) {
            return c->assignExpr(c->assignExprCtx, c, exprNode, outValue, outIsConst);
        }
        SLCTFEExecSetReasonNode(
            c, lhsNode, "assignment target is not mutable during const evaluation");
        *outIsConst = 0;
        return 0;
    }
    if (!b->mutable) {
        SLCTFEExecSetReasonNode(
            c, lhsNode, "assignment target is not mutable during const evaluation");
        *outIsConst = 0;
        return 0;
    }

    if ((SLTokenKind)n->op == SLTok_ASSIGN && b->typeNode >= 0) {
        if (SLCTFEExecEvalExprForType(c, rhsNode, b->typeNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
    } else if (SLCTFEExecEvalExpr(c, rhsNode, &rhsValue, &rhsIsConst) != 0) {
        return -1;
    }
    if (!rhsIsConst) {
        *outIsConst = 0;
        return 0;
    }

    if ((SLTokenKind)n->op == SLTok_ASSIGN) {
        SLCTFEValue wrappedValue;
        if (SLCTFEExecWrapValueForOptionalType(c, b->typeId, &rhsValue, &wrappedValue) != 0) {
            return -1;
        }
        b->value = wrappedValue;
        *outValue = wrappedValue;
        *outIsConst = 1;
        return 0;
    }

    if (b->value.kind == SLCTFEValue_INT && rhsValue.kind == SLCTFEValue_INT) {
        int64_t v = 0;
        switch ((SLTokenKind)n->op) {
            case SLTok_ADD_ASSIGN:
                if (SLCTFEExecAddI64(b->value.i64, rhsValue.i64, &v) != 0) {
                    *outIsConst = 0;
                    return 0;
                }
                break;
            case SLTok_SUB_ASSIGN:
                if (SLCTFEExecSubI64(b->value.i64, rhsValue.i64, &v) != 0) {
                    *outIsConst = 0;
                    return 0;
                }
                break;
            case SLTok_MUL_ASSIGN:
                if (SLCTFEExecMulI64(b->value.i64, rhsValue.i64, &v) != 0) {
                    *outIsConst = 0;
                    return 0;
                }
                break;
            case SLTok_DIV_ASSIGN:
                if (rhsValue.i64 == 0 || (b->value.i64 == INT64_MIN && rhsValue.i64 == -1)) {
                    *outIsConst = 0;
                    return 0;
                }
                v = b->value.i64 / rhsValue.i64;
                break;
            case SLTok_MOD_ASSIGN:
                if (rhsValue.i64 == 0 || (b->value.i64 == INT64_MIN && rhsValue.i64 == -1)) {
                    *outIsConst = 0;
                    return 0;
                }
                v = b->value.i64 % rhsValue.i64;
                break;
            case SLTok_AND_ASSIGN: v = b->value.i64 & rhsValue.i64; break;
            case SLTok_OR_ASSIGN:  v = b->value.i64 | rhsValue.i64; break;
            case SLTok_XOR_ASSIGN: v = b->value.i64 ^ rhsValue.i64; break;
            case SLTok_LSHIFT_ASSIGN:
                if (rhsValue.i64 < 0 || rhsValue.i64 > 63 || b->value.i64 < 0) {
                    *outIsConst = 0;
                    return 0;
                }
                v = (int64_t)((uint64_t)b->value.i64 << (uint32_t)rhsValue.i64);
                break;
            case SLTok_RSHIFT_ASSIGN:
                if (rhsValue.i64 < 0 || rhsValue.i64 > 63 || b->value.i64 < 0) {
                    *outIsConst = 0;
                    return 0;
                }
                v = (int64_t)((uint64_t)b->value.i64 >> (uint32_t)rhsValue.i64);
                break;
            default:
                SLCTFEExecSetReasonNode(
                    c, exprNode, "assignment operator is not supported in const evaluation");
                *outIsConst = 0;
                return 0;
        }
        b->value.kind = SLCTFEValue_INT;
        b->value.i64 = v;
        b->value.f64 = 0.0;
        b->value.b = 0;
        b->value.typeTag = 0;
        b->value.s.bytes = NULL;
        b->value.s.len = 0;
        *outValue = b->value;
        *outIsConst = 1;
        return 0;
    }

    if (b->value.kind == SLCTFEValue_FLOAT) {
        double lhs = 0.0;
        double rhs = 0.0;
        double out = 0.0;
        if (!SLCTFEExecValueToF64(&b->value, &lhs) || !SLCTFEExecValueToF64(&rhsValue, &rhs)) {
            SLCTFEExecSetReasonNode(c, exprNode, "assignment operands are not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        switch ((SLTokenKind)n->op) {
            case SLTok_ADD_ASSIGN: out = lhs + rhs; break;
            case SLTok_SUB_ASSIGN: out = lhs - rhs; break;
            case SLTok_MUL_ASSIGN: out = lhs * rhs; break;
            case SLTok_DIV_ASSIGN: out = lhs / rhs; break;
            default:
                SLCTFEExecSetReasonNode(
                    c, exprNode, "assignment operator is not supported in const evaluation");
                *outIsConst = 0;
                return 0;
        }
        b->value.kind = SLCTFEValue_FLOAT;
        b->value.i64 = 0;
        b->value.f64 = out;
        b->value.b = 0;
        b->value.typeTag = 0;
        b->value.s.bytes = NULL;
        b->value.s.len = 0;
        *outValue = b->value;
        *outIsConst = 1;
        return 0;
    }

    SLCTFEExecSetReasonNode(c, exprNode, "assignment operands are not const-evaluable");
    *outIsConst = 0;
    return 0;
}

static int SLCTFEExecEvalExprSideEffect(SLCTFEExecCtx* c, int32_t exprNode, int* outIsConst) {
    SLCTFEValue v;
    int         rc;
    int         isConst = 0;
    rc = SLCTFEExecEvalAssignExpr(c, exprNode, &v, &isConst);
    if (rc != 0) {
        return -1;
    }
    if (isConst) {
        *outIsConst = 1;
        return 0;
    }
    return SLCTFEExecEvalExpr(c, exprNode, &v, outIsConst);
}

static int SLCTFEExecAssignValueExpr(
    SLCTFEExecCtx*     c,
    int32_t            lhsExprNode,
    const SLCTFEValue* inValue,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    if (c == NULL || inValue == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (c->assignValueExpr != NULL) {
        return c->assignValueExpr(
            c->assignValueExprCtx, c, lhsExprNode, inValue, outValue, outIsConst);
    }
    *outIsConst = 0;
    return 0;
}

static uint32_t SLCTFEExecBlockStmtCount(const SLAst* ast, int32_t blockNode) {
    uint32_t count = 0;
    int32_t  stmt;
    if (ast == NULL || blockNode < 0 || (uint32_t)blockNode >= ast->len) {
        return 0;
    }
    stmt = ast->nodes[blockNode].firstChild;
    while (stmt >= 0) {
        if (count == UINT32_MAX) {
            return UINT32_MAX;
        }
        count++;
        stmt = ast->nodes[stmt].nextSibling;
    }
    return count;
}

typedef enum SLCTFEExecLoopAction {
    SLCTFEExecLoopAction_NONE = 0,
    SLCTFEExecLoopAction_BREAK,
    SLCTFEExecLoopAction_CONTINUE,
} SLCTFEExecLoopAction;

static int SLCTFEExecEvalBlockImpl(
    SLCTFEExecCtx* c,
    int32_t        blockNode,
    const SLCTFEExecBinding* _Nullable preBindings,
    uint32_t              preBindingLen,
    SLCTFEValue*          outValue,
    int*                  outDidReturn,
    int*                  outIsConst,
    SLCTFEExecLoopAction* outLoopAction);

static int SLCTFEExecEvalStmt(
    SLCTFEExecCtx*        c,
    SLCTFEExecEnv*        frame,
    uint32_t              bindingCap,
    int32_t               stmtNode,
    SLCTFEValue*          outValue,
    int*                  outDidReturn,
    int*                  outIsConst,
    SLCTFEExecLoopAction* outLoopAction);

static int SLCTFEExecRunDeferredStmts(
    SLCTFEExecCtx* c,
    SLCTFEExecEnv* frame,
    uint32_t       bindingCap,
    const int32_t* deferredStmtNodes,
    uint32_t       deferredStmtLen,
    SLCTFEValue*   outValue,
    int*           outIsConst) {
    uint32_t i;
    if (c == NULL || frame == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    for (i = deferredStmtLen; i > 0; i--) {
        int32_t              stmtNode = deferredStmtNodes[i - 1];
        int                  didReturn = 0;
        int                  isConst = 0;
        SLCTFEExecLoopAction loopAction = SLCTFEExecLoopAction_NONE;
        if (SLCTFEExecEvalStmt(
                c, frame, bindingCap, stmtNode, outValue, &didReturn, &isConst, &loopAction)
            != 0)
        {
            return -1;
        }
        if (!isConst) {
            *outIsConst = 0;
            return 0;
        }
        if (didReturn || loopAction != SLCTFEExecLoopAction_NONE) {
            SLCTFEExecSetReasonNode(
                c, stmtNode, "deferred statement cannot alter control flow in const evaluation");
            *outIsConst = 0;
            return 0;
        }
    }
    *outIsConst = 1;
    return 0;
}

static int SLCTFEExecTupleElementAt(
    SLCTFEExecCtx*     c,
    const SLCTFEValue* tupleValue,
    uint32_t           index,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (c == NULL || tupleValue == NULL || outValue == NULL || outIsConst == NULL
        || tupleValue->kind != SLCTFEValue_ARRAY || c->forInIndex == NULL
        || index >= tupleValue->s.len)
    {
        return 0;
    }
    return c->forInIndex(c->forInIndexCtx, c, tupleValue, index, 0, outValue, outIsConst) == 0;
}

int SLCTFEExecEvalBlock(
    SLCTFEExecCtx* c,
    int32_t        blockNode,
    SLCTFEValue*   outValue,
    int*           outDidReturn,
    int*           outIsConst) {
    SLCTFEExecLoopAction loopAction = SLCTFEExecLoopAction_NONE;
    if (SLCTFEExecEvalBlockImpl(
            c, blockNode, NULL, 0, outValue, outDidReturn, outIsConst, &loopAction)
        != 0)
    {
        return -1;
    }
    if (*outIsConst && loopAction != SLCTFEExecLoopAction_NONE) {
        SLCTFEExecSetReasonNode(
            c, blockNode, "loop control statement escaped block during const evaluation");
        *outDidReturn = 0;
        *outIsConst = 0;
    }
    return 0;
}

static int SLCTFEExecEvalBlockImpl(
    SLCTFEExecCtx* c,
    int32_t        blockNode,
    const SLCTFEExecBinding* _Nullable preBindings,
    uint32_t              preBindingLen,
    SLCTFEValue*          outValue,
    int*                  outDidReturn,
    int*                  outIsConst,
    SLCTFEExecLoopAction* outLoopAction) {
    SLCTFEExecBinding*   bindings = NULL;
    int32_t*             deferredStmtNodes = NULL;
    SLCTFEExecEnv        frame;
    SLCTFEExecEnv*       savedEnv;
    uint32_t             bindingCap;
    uint32_t             deferredStmtLen = 0;
    int32_t              stmt;
    int32_t              savedPendingReturnExprNode;
    int                  didReturn = 0;
    int                  isConst = 1;
    int                  runDefersIsConst = 1;
    SLCTFEExecLoopAction loopAction = SLCTFEExecLoopAction_NONE;
    int                  rc;

    if (c == NULL || c->ast == NULL || outValue == NULL || outDidReturn == NULL
        || outIsConst == NULL || outLoopAction == NULL)
    {
        return -1;
    }
    if (blockNode < 0 || (uint32_t)blockNode >= c->ast->len
        || c->ast->nodes[blockNode].kind != SLAst_BLOCK)
    {
        *outDidReturn = 0;
        *outIsConst = 0;
        *outLoopAction = SLCTFEExecLoopAction_NONE;
        return 0;
    }

    bindingCap = SLCTFEExecBlockStmtCount(c->ast, blockNode);
    if (bindingCap == UINT32_MAX) {
        return -1;
    }
    if (preBindingLen > UINT32_MAX - bindingCap) {
        return -1;
    }
    bindingCap += preBindingLen;
    if (bindingCap > 0) {
        bindings = (SLCTFEExecBinding*)SLArenaAlloc(
            c->arena,
            sizeof(SLCTFEExecBinding) * bindingCap,
            (uint32_t)_Alignof(SLCTFEExecBinding));
        if (bindings == NULL) {
            return -1;
        }
        deferredStmtNodes = (int32_t*)SLArenaAlloc(
            c->arena, sizeof(int32_t) * bindingCap, (uint32_t)_Alignof(int32_t));
        if (deferredStmtNodes == NULL) {
            return -1;
        }
    }

    savedEnv = c->env;
    savedPendingReturnExprNode = c->pendingReturnExprNode;
    frame.parent = savedEnv;
    frame.bindings = bindings;
    frame.bindingLen = 0;
    if (preBindingLen > 0) {
        if (bindings == NULL || preBindings == NULL) {
            c->env = savedEnv;
            return -1;
        }
        memcpy(bindings, preBindings, sizeof(SLCTFEExecBinding) * preBindingLen);
        frame.bindingLen = preBindingLen;
    }
    c->env = &frame;
    c->pendingReturnExprNode = -1;

    stmt = c->ast->nodes[blockNode].firstChild;
    while (stmt >= 0) {
        const SLAstNode* stmtAst = &c->ast->nodes[stmt];
        int32_t          nextStmt = stmtAst->nextSibling;
        if (stmtAst->kind == SLAst_DEFER) {
            int32_t deferredStmtNode = stmtAst->firstChild;
            if (deferredStmtNode < 0 || c->ast->nodes[deferredStmtNode].nextSibling >= 0
                || deferredStmtLen >= bindingCap || deferredStmtNodes == NULL)
            {
                SLCTFEExecSetReasonNode(
                    c, stmt, "defer statement is malformed for const evaluation");
                c->env = savedEnv;
                *outDidReturn = 0;
                *outIsConst = 0;
                *outLoopAction = SLCTFEExecLoopAction_NONE;
                return 0;
            }
            deferredStmtNodes[deferredStmtLen++] = deferredStmtNode;
            stmt = nextStmt;
            continue;
        }

        rc = SLCTFEExecEvalStmt(
            c, &frame, bindingCap, stmt, outValue, &didReturn, &isConst, &loopAction);
        if (rc != 0) {
            c->pendingReturnExprNode = savedPendingReturnExprNode;
            c->env = savedEnv;
            return -1;
        }
        if (!isConst || didReturn || loopAction != SLCTFEExecLoopAction_NONE) {
            break;
        }
        stmt = nextStmt;
    }

    if (isConst) {
        if (SLCTFEExecRunDeferredStmts(
                c,
                &frame,
                bindingCap,
                deferredStmtNodes,
                deferredStmtLen,
                outValue,
                &runDefersIsConst)
            != 0)
        {
            c->pendingReturnExprNode = savedPendingReturnExprNode;
            c->env = savedEnv;
            return -1;
        }
        if (!runDefersIsConst) {
            didReturn = 0;
            loopAction = SLCTFEExecLoopAction_NONE;
            isConst = 0;
        }
    }
    if (isConst && didReturn) {
        SLCTFEValue retValue;
        int         retIsConst = 0;
        const char* prevReason = c->nonConstReason;
        uint32_t    prevReasonStart = c->nonConstStart;
        uint32_t    prevReasonEnd = c->nonConstEnd;
        if (c->pendingReturnExprNode >= 0 && (uint32_t)c->pendingReturnExprNode >= c->ast->len) {
            c->pendingReturnExprNode = savedPendingReturnExprNode;
            c->env = savedEnv;
            return -1;
        } else if (
            c->pendingReturnExprNode >= 0
            && SLCTFEExecEvalExpr(c, c->pendingReturnExprNode, &retValue, &retIsConst) != 0)
        {
            c->pendingReturnExprNode = savedPendingReturnExprNode;
            c->env = savedEnv;
            return -1;
        } else if (c->pendingReturnExprNode >= 0 && !retIsConst) {
            if (outValue->kind != SLCTFEValue_INVALID && c->nonConstReason != NULL
                && SLCTFEExecStrEq(
                    c->nonConstReason, "identifier is not a const value in this context"))
            {
                c->nonConstReason = prevReason;
                c->nonConstStart = prevReasonStart;
                c->nonConstEnd = prevReasonEnd;
            } else {
                SLCTFEExecSetReasonNode(
                    c, c->pendingReturnExprNode, "return expression is not const-evaluable");
                didReturn = 0;
                loopAction = SLCTFEExecLoopAction_NONE;
                isConst = 0;
            }
        } else if (c->pendingReturnExprNode >= 0) {
            *outValue = retValue;
        }
    }

    c->pendingReturnExprNode = didReturn ? -1 : savedPendingReturnExprNode;
    c->env = savedEnv;
    *outDidReturn = isConst ? didReturn : 0;
    *outIsConst = isConst;
    *outLoopAction = isConst ? loopAction : SLCTFEExecLoopAction_NONE;
    return 0;
}

static int SLCTFEExecEvalStmt(
    SLCTFEExecCtx*        c,
    SLCTFEExecEnv*        frame,
    uint32_t              bindingCap,
    int32_t               stmtNode,
    SLCTFEValue*          outValue,
    int*                  outDidReturn,
    int*                  outIsConst,
    SLCTFEExecLoopAction* outLoopAction) {
    const SLAstNode* s;

    if (c == NULL || c->ast == NULL || frame == NULL || outValue == NULL || outDidReturn == NULL
        || outIsConst == NULL || outLoopAction == NULL)
    {
        return -1;
    }
    if (stmtNode < 0 || (uint32_t)stmtNode >= c->ast->len) {
        *outDidReturn = 0;
        *outIsConst = 0;
        *outLoopAction = SLCTFEExecLoopAction_NONE;
        return 0;
    }

    s = &c->ast->nodes[stmtNode];
    *outDidReturn = 0;
    *outIsConst = 1;
    *outLoopAction = SLCTFEExecLoopAction_NONE;

    if (s->kind == SLAst_CONST || s->kind == SLAst_VAR) {
        int32_t firstChild = c->ast->nodes[stmtNode].firstChild;
        int32_t initNode = SLCTFEExecVarLikeInitExprNode(c->ast, stmtNode);
        int32_t declTypeNode = firstChild;
        int     hasNameList = firstChild >= 0 && c->ast->nodes[firstChild].kind == SLAst_NAME_LIST;
        int32_t declTypeId = -1;
        int32_t bindingTypeNode = -1;
        if (hasNameList) {
            declTypeNode = c->ast->nodes[firstChild].nextSibling;
        }
        if (frame->bindings == NULL) {
            SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        if (declTypeNode >= 0 && SLCTFEExecIsTypeNodeKind(c->ast->nodes[declTypeNode].kind)) {
            bindingTypeNode = declTypeNode;
            if (c->resolveType != NULL
                && c->resolveType(c->resolveTypeCtx, declTypeNode, &declTypeId) != 0)
            {
                return -1;
            }
        }
        if (!hasNameList) {
            SLCTFEValue v;
            int         isConst = 0;
            if (frame->bindingLen >= bindingCap) {
                SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            if (initNode < 0) {
                if (bindingTypeNode >= 0 && c->zeroInit != NULL) {
                    if (c->zeroInit(c->zeroInitCtx, bindingTypeNode, &v, &isConst) != 0) {
                        return -1;
                    }
                } else {
                    SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
                    *outIsConst = 0;
                    return 0;
                }
            } else if (
                bindingTypeNode >= 0
                    ? SLCTFEExecEvalExprForType(c, initNode, bindingTypeNode, &v, &isConst) != 0
                    : SLCTFEExecEvalExpr(c, initNode, &v, &isConst) != 0)
            {
                return -1;
            }
            if (!isConst) {
                *outIsConst = 0;
                return 0;
            }
            if (declTypeId < 0 && c->inferValueType != NULL) {
                if (c->inferValueType(c->inferValueTypeCtx, &v, &declTypeId) != 0) {
                    return -1;
                }
            }
            if (declTypeId >= 0) {
                SLCTFEValue wrappedValue;
                if (SLCTFEExecWrapValueForOptionalType(c, declTypeId, &v, &wrappedValue) != 0) {
                    return -1;
                }
                v = wrappedValue;
            }
            frame->bindings[frame->bindingLen].nameStart = s->dataStart;
            frame->bindings[frame->bindingLen].nameEnd = s->dataEnd;
            frame->bindings[frame->bindingLen].typeId = declTypeId;
            frame->bindings[frame->bindingLen].typeNode = bindingTypeNode;
            frame->bindings[frame->bindingLen].mutable = s->kind == SLAst_VAR;
            frame->bindings[frame->bindingLen]._reserved[0] = 0;
            frame->bindings[frame->bindingLen]._reserved[1] = 0;
            frame->bindings[frame->bindingLen]._reserved[2] = 0;
            frame->bindings[frame->bindingLen].value = v;
            frame->bindingLen++;
            return 0;
        }
        {
            uint32_t    nameCount = SLCTFEExecBlockStmtCount(c->ast, firstChild);
            uint32_t    initCount = 0;
            uint32_t    i;
            int32_t     exprListNode = initNode;
            SLCTFEValue values[256];
            int         valueIsConst[256];
            int         expandedTuple = 0;
            int32_t     nameNode;
            if (nameCount == 0 || nameCount > 256u || frame->bindingLen + nameCount > bindingCap) {
                SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            if (exprListNode < 0) {
                if (bindingTypeNode < 0 || c->zeroInit == NULL) {
                    *outIsConst = 0;
                    SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
                    return 0;
                }
                for (i = 0; i < nameCount; i++) {
                    valueIsConst[i] = 0;
                    if (c->zeroInit(c->zeroInitCtx, bindingTypeNode, &values[i], &valueIsConst[i])
                        != 0)
                    {
                        return -1;
                    }
                    if (!valueIsConst[i]) {
                        *outIsConst = 0;
                        return 0;
                    }
                }
            } else {
                if (c->ast->nodes[exprListNode].kind != SLAst_EXPR_LIST) {
                    SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
                    *outIsConst = 0;
                    return 0;
                }
                initCount = SLCTFEExecBlockStmtCount(c->ast, exprListNode);
                if (initCount != nameCount) {
                    if (initCount == 1u) {
                        int32_t     expr = SLCTFEExecListItemAt(c->ast, exprListNode, 0);
                        SLCTFEValue tupleValue;
                        int         tupleIsConst = 0;
                        if (expr < 0
                            || SLCTFEExecEvalExpr(c, expr, &tupleValue, &tupleIsConst) != 0)
                        {
                            return expr < 0 ? 0 : -1;
                        }
                        if (!tupleIsConst || tupleValue.kind != SLCTFEValue_ARRAY
                            || tupleValue.s.len != nameCount)
                        {
                            SLCTFEExecSetReasonNode(
                                c, stmtNode, "declaration is not const-evaluable");
                            *outIsConst = 0;
                            return 0;
                        }
                        for (i = 0; i < nameCount; i++) {
                            valueIsConst[i] = 0;
                            if (!SLCTFEExecTupleElementAt(
                                    c, &tupleValue, i, &values[i], &valueIsConst[i]))
                            {
                                *outIsConst = 0;
                                return 0;
                            }
                            if (!valueIsConst[i]) {
                                *outIsConst = 0;
                                return 0;
                            }
                        }
                        expandedTuple = 1;
                    } else {
                        SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
                        *outIsConst = 0;
                        return 0;
                    }
                }
                if (!expandedTuple) {
                    for (i = 0; i < initCount; i++) {
                        int32_t expr = SLCTFEExecListItemAt(c->ast, exprListNode, i);
                        valueIsConst[i] = 0;
                        if (expr < 0) {
                            *outIsConst = 0;
                            return 0;
                        }
                        if ((bindingTypeNode >= 0
                                 ? SLCTFEExecEvalExprForType(
                                       c, expr, bindingTypeNode, &values[i], &valueIsConst[i])
                                 : SLCTFEExecEvalExpr(c, expr, &values[i], &valueIsConst[i]))
                            != 0)
                        {
                            return -1;
                        }
                        if (!valueIsConst[i]) {
                            *outIsConst = 0;
                            return 0;
                        }
                    }
                }
            }
            for (i = 0; i < nameCount; i++) {
                nameNode = SLCTFEExecListItemAt(c->ast, firstChild, i);
                if (nameNode < 0 || c->ast->nodes[nameNode].kind != SLAst_IDENT) {
                    *outIsConst = 0;
                    return 0;
                }
                frame->bindings[frame->bindingLen].nameStart = c->ast->nodes[nameNode].dataStart;
                frame->bindings[frame->bindingLen].nameEnd = c->ast->nodes[nameNode].dataEnd;
                frame->bindings[frame->bindingLen].typeId = declTypeId;
                frame->bindings[frame->bindingLen].typeNode = bindingTypeNode;
                frame->bindings[frame->bindingLen].mutable = s->kind == SLAst_VAR;
                frame->bindings[frame->bindingLen]._reserved[0] = 0;
                frame->bindings[frame->bindingLen]._reserved[1] = 0;
                frame->bindings[frame->bindingLen]._reserved[2] = 0;
                frame->bindings[frame->bindingLen].value = values[i];
                frame->bindingLen++;
            }
            return 0;
        }
    }

    if (s->kind == SLAst_MULTI_ASSIGN) {
        int32_t     lhsList = s->firstChild;
        int32_t     rhsList = lhsList >= 0 ? c->ast->nodes[lhsList].nextSibling : -1;
        uint32_t    lhsCount;
        uint32_t    rhsCount;
        SLCTFEValue rhsValues[256];
        uint32_t    i;
        if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != SLAst_EXPR_LIST
            || c->ast->nodes[rhsList].kind != SLAst_EXPR_LIST)
        {
            SLCTFEExecSetReasonNode(c, stmtNode, "multi-assign is malformed for const evaluation");
            *outIsConst = 0;
            return 0;
        }
        lhsCount = SLCTFEExecBlockStmtCount(c->ast, lhsList);
        rhsCount = SLCTFEExecBlockStmtCount(c->ast, rhsList);
        if (lhsCount == 0 || lhsCount > 256u) {
            SLCTFEExecSetReasonNode(c, stmtNode, "multi-assign is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        if (rhsCount == lhsCount) {
            for (i = 0; i < rhsCount; i++) {
                int32_t rhsExpr = SLCTFEExecListItemAt(c->ast, rhsList, i);
                int     rhsIsConst = 0;
                if (rhsExpr < 0 || SLCTFEExecEvalExpr(c, rhsExpr, &rhsValues[i], &rhsIsConst) != 0)
                {
                    return -1;
                }
                if (!rhsIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
            }
        } else if (rhsCount == 1u) {
            int32_t     rhsExpr = SLCTFEExecListItemAt(c->ast, rhsList, 0);
            SLCTFEValue tupleValue;
            int         tupleIsConst = 0;
            if (rhsExpr < 0 || SLCTFEExecEvalExpr(c, rhsExpr, &tupleValue, &tupleIsConst) != 0) {
                return rhsExpr < 0 ? 0 : -1;
            }
            if (!tupleIsConst || tupleValue.kind != SLCTFEValue_ARRAY
                || tupleValue.s.len != lhsCount)
            {
                SLCTFEExecSetReasonNode(c, stmtNode, "multi-assign is not const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            for (i = 0; i < lhsCount; i++) {
                int elemIsConst = 0;
                if (!SLCTFEExecTupleElementAt(c, &tupleValue, i, &rhsValues[i], &elemIsConst)) {
                    *outIsConst = 0;
                    return 0;
                }
                if (!elemIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
            }
        } else {
            SLCTFEExecSetReasonNode(c, stmtNode, "multi-assign is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        for (i = 0; i < lhsCount; i++) {
            int32_t lhsExpr = SLCTFEExecListItemAt(c->ast, lhsList, i);
            if (lhsExpr < 0) {
                *outIsConst = 0;
                return 0;
            }
            if (c->ast->nodes[lhsExpr].kind == SLAst_IDENT) {
                SLCTFEExecBinding* b = SLCTFEExecEnvFindBinding(
                    c, c->ast->nodes[lhsExpr].dataStart, c->ast->nodes[lhsExpr].dataEnd);
                if (b != NULL) {
                    if (!b->mutable) {
                        SLCTFEExecSetReasonNode(
                            c, lhsExpr, "assignment target is not mutable during const evaluation");
                        *outIsConst = 0;
                        return 0;
                    }
                    b->value = rhsValues[i];
                    continue;
                }
            }
            if (SLCTFEExecAssignValueExpr(c, lhsExpr, &rhsValues[i], outValue, outIsConst) != 0) {
                return -1;
            }
            if (!*outIsConst) {
                return 0;
            }
        }
        return 0;
    }

    if (s->kind == SLAst_RETURN) {
        int32_t exprNode = s->firstChild;
        if (exprNode < 0 || c->ast->nodes[exprNode].nextSibling >= 0) {
            SLCTFEExecSetReasonNode(
                c, stmtNode, "return statement form is not supported in const evaluation");
            *outIsConst = 0;
            return 0;
        }
        c->pendingReturnExprNode = exprNode;
        *outDidReturn = 1;
        return 0;
    }

    if (s->kind == SLAst_BLOCK) {
        int                  didReturn = 0;
        int                  isConst = 0;
        SLCTFEExecLoopAction loopAction = SLCTFEExecLoopAction_NONE;
        if (SLCTFEExecEvalBlockImpl(
                c, stmtNode, NULL, 0, outValue, &didReturn, &isConst, &loopAction)
            != 0)
        {
            return -1;
        }
        *outDidReturn = didReturn;
        *outIsConst = isConst;
        *outLoopAction = loopAction;
        return 0;
    }

    if (s->kind == SLAst_CONST_BLOCK) {
        int32_t              blockNode = s->firstChild;
        int                  didReturn = 0;
        int                  isConst = 0;
        SLCTFEExecLoopAction loopAction = SLCTFEExecLoopAction_NONE;
        if (c->skipConstBlocks) {
            return 0;
        }
        if (blockNode < 0 || c->ast->nodes[blockNode].kind != SLAst_BLOCK) {
            SLCTFEExecSetReasonNode(c, stmtNode, "const block is malformed for const evaluation");
            *outIsConst = 0;
            return 0;
        }
        if (SLCTFEExecEvalBlockImpl(
                c, blockNode, NULL, 0, outValue, &didReturn, &isConst, &loopAction)
            != 0)
        {
            return -1;
        }
        if (!isConst) {
            *outIsConst = 0;
            return 0;
        }
        if (didReturn || loopAction != SLCTFEExecLoopAction_NONE) {
            SLCTFEExecSetReasonNode(
                c, stmtNode, "const block cannot alter control flow in const evaluation");
            *outIsConst = 0;
            return 0;
        }
        return 0;
    }

    if (s->kind == SLAst_IF) {
        int32_t              condNode = s->firstChild;
        int32_t              thenNode = condNode >= 0 ? c->ast->nodes[condNode].nextSibling : -1;
        int32_t              elseNode = thenNode >= 0 ? c->ast->nodes[thenNode].nextSibling : -1;
        int32_t              branchNode = -1;
        SLCTFEValue          condValue;
        int                  condIsConst = 0;
        int                  condTruth = 0;
        int                  didReturn = 0;
        int                  isConst = 0;
        SLCTFEExecLoopAction loopAction = SLCTFEExecLoopAction_NONE;

        if (condNode < 0 || thenNode < 0) {
            SLCTFEExecSetReasonNode(c, stmtNode, "if statement is malformed for const evaluation");
            *outIsConst = 0;
            return 0;
        }
        if (SLCTFEExecEvalExpr(c, condNode, &condValue, &condIsConst) != 0) {
            return -1;
        }
        if (!condIsConst) {
            SLCTFEExecSetReasonNode(c, condNode, "if condition must be const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        if (condValue.kind == SLCTFEValue_BOOL) {
            condTruth = condValue.b ? 1 : 0;
        } else if (condValue.kind == SLCTFEValue_OPTIONAL) {
            condTruth = condValue.b != 0u;
        } else {
            int32_t condTypeId = -1;
            int32_t payloadTypeId = -1;
            int     isOptionalType = 0;
            if (c->inferExprType != NULL
                && c->inferExprType(c->inferExprTypeCtx, condNode, &condTypeId) == 0
                && SLCTFEExecTypeIsOptional(c, condTypeId, &payloadTypeId, &isOptionalType) == 0
                && isOptionalType)
            {
                condTruth = condValue.kind != SLCTFEValue_NULL;
            } else {
                condTruth = condValue.kind != SLCTFEValue_NULL;
            }
        }
        branchNode = condTruth ? thenNode : elseNode;
        if (branchNode < 0) {
            return 0;
        }
        if (c->ast->nodes[branchNode].kind == SLAst_BLOCK) {
            if (SLCTFEExecEvalBlockImpl(
                    c, branchNode, NULL, 0, outValue, &didReturn, &isConst, &loopAction)
                != 0)
            {
                return -1;
            }
        } else if (c->ast->nodes[branchNode].kind == SLAst_IF) {
            if (SLCTFEExecEvalStmt(
                    c, frame, bindingCap, branchNode, outValue, &didReturn, &isConst, &loopAction)
                != 0)
            {
                return -1;
            }
        } else {
            SLCTFEExecSetReasonNode(c, branchNode, "if branch statement is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        *outDidReturn = didReturn;
        *outIsConst = isConst;
        *outLoopAction = loopAction;
        return 0;
    }

    if (s->kind == SLAst_EXPR_STMT) {
        int32_t exprNode = s->firstChild;
        int     isConst = 0;
        if (exprNode < 0 || c->ast->nodes[exprNode].nextSibling >= 0) {
            SLCTFEExecSetReasonNode(
                c, stmtNode, "expression statement is malformed for const evaluation");
            *outIsConst = 0;
            return 0;
        }
        if (SLCTFEExecEvalExprSideEffect(c, exprNode, &isConst) != 0) {
            return -1;
        }
        if (!isConst) {
            *outIsConst = 0;
            return 0;
        }
        return 0;
    }

    if (s->kind == SLAst_ASSERT) {
        int32_t     condNode = s->firstChild;
        SLCTFEValue condValue;
        int         condIsConst = 0;
        if (condNode < 0) {
            SLCTFEExecSetReasonNode(
                c, stmtNode, "assert statement is malformed for const evaluation");
            *outIsConst = 0;
            return 0;
        }
        if (SLCTFEExecEvalExpr(c, condNode, &condValue, &condIsConst) != 0) {
            return -1;
        }
        if (!condIsConst || condValue.kind != SLCTFEValue_BOOL) {
            SLCTFEExecSetReasonNode(
                c, condNode, "assert condition must be a const bool expression");
            *outIsConst = 0;
            return 0;
        }
        if (!condValue.b) {
            SLCTFEExecSetReasonNode(
                c, condNode, "assert condition evaluated to false during const evaluation");
            *outIsConst = 0;
            return 0;
        }
        return 0;
    }

    if (s->kind == SLAst_BREAK) {
        *outLoopAction = SLCTFEExecLoopAction_BREAK;
        return 0;
    }

    if (s->kind == SLAst_CONTINUE) {
        *outLoopAction = SLCTFEExecLoopAction_CONTINUE;
        return 0;
    }

    if (s->kind == SLAst_SWITCH) {
        int32_t              clauseNode;
        int32_t              defaultBodyNode = -1;
        int                  hasSubject = s->flags == 1;
        SLCTFEValue          subjectValue;
        int                  subjectIsConst = 0;
        SLCTFEExecLoopAction bodyAction = SLCTFEExecLoopAction_NONE;

        clauseNode = s->firstChild;
        if (hasSubject) {
            if (clauseNode < 0) {
                SLCTFEExecSetReasonNode(
                    c, stmtNode, "switch statement is malformed for const evaluation");
                *outIsConst = 0;
                return 0;
            }
            if (SLCTFEExecEvalExpr(c, clauseNode, &subjectValue, &subjectIsConst) != 0) {
                return -1;
            }
            if (!subjectIsConst) {
                SLCTFEExecSetReasonNode(c, clauseNode, "switch subject must be const-evaluable");
                *outIsConst = 0;
                return 0;
            }
            clauseNode = c->ast->nodes[clauseNode].nextSibling;
        }

        while (clauseNode >= 0) {
            const SLAstNode* clause = &c->ast->nodes[clauseNode];
            if (clause->kind == SLAst_CASE) {
                int32_t caseChild = clause->firstChild;
                int32_t bodyNode = -1;
                int     matched = 0;
                int32_t matchedPatternNode = -1;

                while (caseChild >= 0) {
                    int32_t next;
                    int32_t labelExprNode = caseChild;
                    if (c->ast->nodes[caseChild].kind == SLAst_CASE_PATTERN) {
                        labelExprNode = c->ast->nodes[caseChild].firstChild;
                        if (labelExprNode < 0) {
                            SLCTFEExecSetReasonNode(
                                c,
                                caseChild,
                                "switch case pattern is malformed for const evaluation");
                            *outIsConst = 0;
                            return 0;
                        }
                    }
                    next = c->ast->nodes[caseChild].nextSibling;
                    if (next < 0) {
                        bodyNode = caseChild;
                        break;
                    }

                    if (hasSubject) {
                        SLCTFEValue labelValue;
                        int         labelIsConst = 0;
                        int         labelMatch = 0;
                        if (c->matchPattern != NULL) {
                            int handled = c->matchPattern(
                                c->matchPatternCtx, c, &subjectValue, labelExprNode, &labelMatch);
                            if (handled < 0) {
                                return -1;
                            }
                            if (handled == 0) {
                                if (SLCTFEExecEvalExpr(c, labelExprNode, &labelValue, &labelIsConst)
                                    != 0)
                                {
                                    return -1;
                                }
                                if (!labelIsConst) {
                                    SLCTFEExecSetReasonNode(
                                        c,
                                        labelExprNode,
                                        "switch case label must be const-evaluable");
                                    *outIsConst = 0;
                                    return 0;
                                }
                                if (!SLCTFEExecValueEq(&subjectValue, &labelValue, &labelMatch)) {
                                    SLCTFEExecSetReasonNode(
                                        c,
                                        labelExprNode,
                                        "switch case label is not comparable to const-evaluated "
                                        "subject");
                                    *outIsConst = 0;
                                    return 0;
                                }
                            }
                        } else {
                            if (SLCTFEExecEvalExpr(c, labelExprNode, &labelValue, &labelIsConst)
                                != 0)
                            {
                                return -1;
                            }
                            if (!labelIsConst) {
                                SLCTFEExecSetReasonNode(
                                    c, labelExprNode, "switch case label must be const-evaluable");
                                *outIsConst = 0;
                                return 0;
                            }
                            if (!SLCTFEExecValueEq(&subjectValue, &labelValue, &labelMatch)) {
                                SLCTFEExecSetReasonNode(
                                    c,
                                    labelExprNode,
                                    "switch case label is not comparable to const-evaluated "
                                    "subject");
                                *outIsConst = 0;
                                return 0;
                            }
                        }
                        if (labelMatch) {
                            matched = 1;
                            matchedPatternNode =
                                c->ast->nodes[caseChild].kind == SLAst_CASE_PATTERN
                                    ? caseChild
                                    : -1;
                        }
                    } else {
                        SLCTFEValue condValue;
                        int         condIsConst = 0;
                        if (SLCTFEExecEvalExpr(c, labelExprNode, &condValue, &condIsConst) != 0) {
                            return -1;
                        }
                        if (!condIsConst || condValue.kind != SLCTFEValue_BOOL) {
                            SLCTFEExecSetReasonNode(
                                c,
                                labelExprNode,
                                "switch case condition must be a const bool expression");
                            *outIsConst = 0;
                            return 0;
                        }
                        if (condValue.b) {
                            matched = 1;
                        }
                    }

                    caseChild = next;
                }

                if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
                    SLCTFEExecSetReasonNode(
                        c, clauseNode, "switch case is malformed for const evaluation");
                    *outIsConst = 0;
                    return 0;
                }
                if (matched) {
                    if (matchedPatternNode >= 0) {
                        int32_t aliasNode =
                            c->ast->nodes[c->ast->nodes[matchedPatternNode].firstChild].nextSibling;
                        if (aliasNode >= 0 && c->ast->nodes[aliasNode].kind == SLAst_IDENT) {
                            SLCTFEExecBinding aliasBinding;
                            memset(&aliasBinding, 0, sizeof(aliasBinding));
                            aliasBinding.nameStart = c->ast->nodes[aliasNode].dataStart;
                            aliasBinding.nameEnd = c->ast->nodes[aliasNode].dataEnd;
                            aliasBinding.mutable = 1u;
                            aliasBinding.value = subjectValue;
                            if (SLCTFEExecEvalBlockImpl(
                                    c,
                                    bodyNode,
                                    &aliasBinding,
                                    1,
                                    outValue,
                                    outDidReturn,
                                    outIsConst,
                                    &bodyAction)
                                != 0)
                            {
                                return -1;
                            }
                        } else if (
                            SLCTFEExecEvalBlockImpl(
                                c,
                                bodyNode,
                                NULL,
                                0,
                                outValue,
                                outDidReturn,
                                outIsConst,
                                &bodyAction)
                            != 0)
                        {
                            return -1;
                        }
                    } else if (
                        SLCTFEExecEvalBlockImpl(
                            c, bodyNode, NULL, 0, outValue, outDidReturn, outIsConst, &bodyAction)
                        != 0)
                    {
                        return -1;
                    }
                    if (!*outIsConst) {
                        return 0;
                    }
                    if (bodyAction == SLCTFEExecLoopAction_BREAK) {
                        *outLoopAction = SLCTFEExecLoopAction_NONE;
                        return 0;
                    }
                    *outLoopAction = bodyAction;
                    return 0;
                }
            } else if (clause->kind == SLAst_DEFAULT) {
                int32_t bodyNode = clause->firstChild;
                if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
                    SLCTFEExecSetReasonNode(
                        c, clauseNode, "switch default clause is malformed for const evaluation");
                    *outIsConst = 0;
                    return 0;
                }
                defaultBodyNode = bodyNode;
            } else {
                SLCTFEExecSetReasonNode(
                    c, clauseNode, "switch clause is not supported in const evaluation");
                *outIsConst = 0;
                return 0;
            }
            clauseNode = c->ast->nodes[clauseNode].nextSibling;
        }

        if (defaultBodyNode >= 0) {
            if (SLCTFEExecEvalBlockImpl(
                    c, defaultBodyNode, NULL, 0, outValue, outDidReturn, outIsConst, &bodyAction)
                != 0)
            {
                return -1;
            }
            if (!*outIsConst) {
                return 0;
            }
            if (bodyAction == SLCTFEExecLoopAction_BREAK) {
                *outLoopAction = SLCTFEExecLoopAction_NONE;
                return 0;
            }
            *outLoopAction = bodyAction;
            return 0;
        }
        return 0;
    }

    if (s->kind == SLAst_FOR) {
        int32_t            nodes[4];
        int                count = 0;
        int32_t            child = s->firstChild;
        int32_t            initNode = -1;
        int32_t            condNode = -1;
        int32_t            postNode = -1;
        int32_t            bodyNode = -1;
        SLCTFEExecBinding* loopBindings = NULL;
        SLCTFEExecEnv      loopFrame;
        SLCTFEExecEnv*     savedEnv = c->env;
        uint32_t           initBindingCap = 0;
        int                hasLoopEnv = 0;
        uint32_t           iter;
        uint32_t iterLimit = c->forIterLimit == 0 ? SLCTFE_EXEC_DEFAULT_FOR_LIMIT : c->forIterLimit;
        int      isConst = 0;
        int      didReturn = 0;
        SLCTFEValue condValue;
        int         condIsConst = 0;

        if ((s->flags & SLAstFlag_FOR_IN) != 0) {
            int                hasKey = (s->flags & SLAstFlag_FOR_IN_HAS_KEY) != 0;
            int                keyRef = (s->flags & SLAstFlag_FOR_IN_KEY_REF) != 0;
            int                valueRef = (s->flags & SLAstFlag_FOR_IN_VALUE_REF) != 0;
            int                valueDiscard = (s->flags & SLAstFlag_FOR_IN_VALUE_DISCARD) != 0;
            int32_t            keyNode = -1;
            int32_t            valueNode = -1;
            int32_t            sourceNode = -1;
            SLCTFEValue        sourceValue;
            const SLCTFEValue* sourceTarget = &sourceValue;
            int                sourceIsConst = 0;
            uint32_t           iterLen = 0;

            while (child >= 0 && count < 4) {
                nodes[count++] = child;
                child = c->ast->nodes[child].nextSibling;
            }
            if ((!hasKey && count != 3) || (hasKey && count != 4) || child >= 0) {
                SLCTFEExecSetReasonNode(
                    c, stmtNode, "for-in loop form is not supported in const evaluation");
                *outIsConst = 0;
                return 0;
            }

            if (hasKey) {
                keyNode = nodes[0];
                valueNode = nodes[1];
                sourceNode = nodes[2];
                bodyNode = nodes[3];
            } else {
                valueNode = nodes[0];
                sourceNode = nodes[1];
                bodyNode = nodes[2];
            }
            if (keyRef) {
                SLCTFEExecSetReasonNode(
                    c, stmtNode, "for-in key reference is not supported in const evaluation");
                *outIsConst = 0;
                return 0;
            }
            if (bodyNode < 0 || (uint32_t)bodyNode >= c->ast->len
                || c->ast->nodes[bodyNode].kind != SLAst_BLOCK)
            {
                SLCTFEExecSetReasonNode(
                    c, stmtNode, "for-in loop body must be a block in const evaluation");
                *outIsConst = 0;
                return 0;
            }
            if (sourceNode < 0 || (uint32_t)sourceNode >= c->ast->len) {
                SLCTFEExecSetReasonNode(
                    c, stmtNode, "for-in loop source is malformed in const evaluation");
                *outIsConst = 0;
                return 0;
            }
            if (SLCTFEExecEvalExpr(c, sourceNode, &sourceValue, &sourceIsConst) != 0) {
                return -1;
            }
            if (!sourceIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (sourceValue.kind == SLCTFEValue_REFERENCE && sourceValue.s.bytes != NULL) {
                sourceTarget = (const SLCTFEValue*)sourceValue.s.bytes;
            }
            if (sourceTarget->kind == SLCTFEValue_STRING || sourceTarget->kind == SLCTFEValue_ARRAY)
            {
                iterLen = sourceTarget->s.len;
            } else {
                SLCTFEExecSetReasonNode(
                    c, sourceNode, "for-in loop source is not supported in const evaluation");
                *outIsConst = 0;
                return 0;
            }

            for (iter = 0; iter < iterLen; iter++) {
                SLCTFEExecBinding    iterBindings[2];
                uint32_t             iterBindingLen = 0;
                SLCTFEExecLoopAction bodyAction = SLCTFEExecLoopAction_NONE;
                memset(iterBindings, 0, sizeof(iterBindings));

                if (hasKey
                    && (keyNode < 0 || (uint32_t)keyNode >= c->ast->len
                        || c->ast->nodes[keyNode].kind != SLAst_IDENT))
                {
                    SLCTFEExecSetReasonNode(
                        c, stmtNode, "for-in key binding is malformed in const evaluation");
                    *outIsConst = 0;
                    return 0;
                }
                if (hasKey && keyNode >= 0
                    && !(
                        c->ast->nodes[keyNode].dataEnd == c->ast->nodes[keyNode].dataStart + 1u
                        && c->src.ptr[c->ast->nodes[keyNode].dataStart] == '_'))
                {
                    iterBindings[iterBindingLen].nameStart = c->ast->nodes[keyNode].dataStart;
                    iterBindings[iterBindingLen].nameEnd = c->ast->nodes[keyNode].dataEnd;
                    iterBindings[iterBindingLen].typeId = -1;
                    iterBindings[iterBindingLen].typeNode = -1;
                    iterBindings[iterBindingLen].mutable = 0;
                    iterBindings[iterBindingLen].value.kind = SLCTFEValue_INT;
                    iterBindings[iterBindingLen].value.i64 = (int64_t)iter;
                    iterBindings[iterBindingLen].value.f64 = 0.0;
                    iterBindings[iterBindingLen].value.b = 0;
                    iterBindings[iterBindingLen].value.typeTag = 0;
                    iterBindings[iterBindingLen].value.s.bytes = NULL;
                    iterBindings[iterBindingLen].value.s.len = 0;
                    iterBindingLen++;
                }
                if (!valueDiscard) {
                    SLCTFEValue iterValue;
                    int         iterValueIsConst = 0;
                    if (valueNode < 0 || (uint32_t)valueNode >= c->ast->len
                        || c->ast->nodes[valueNode].kind != SLAst_IDENT)
                    {
                        SLCTFEExecSetReasonNode(
                            c, stmtNode, "for-in value binding is malformed in const evaluation");
                        *outIsConst = 0;
                        return 0;
                    }
                    if (c->forInIndex != NULL) {
                        if (c->forInIndex(
                                c->forInIndexCtx,
                                c,
                                sourceTarget,
                                iter,
                                valueRef,
                                &iterValue,
                                &iterValueIsConst)
                            != 0)
                        {
                            return -1;
                        }
                    } else if (!valueRef && sourceTarget->kind == SLCTFEValue_STRING) {
                        iterValue.kind = SLCTFEValue_INT;
                        iterValue.i64 = (int64_t)sourceTarget->s.bytes[iter];
                        iterValue.f64 = 0.0;
                        iterValue.b = 0;
                        iterValue.typeTag = 0;
                        iterValue.s.bytes = NULL;
                        iterValue.s.len = 0;
                        iterValueIsConst = 1;
                    } else {
                        SLCTFEExecSetReasonNode(
                            c,
                            sourceNode,
                            "for-in loop source is not supported in const evaluation");
                        *outIsConst = 0;
                        return 0;
                    }
                    if (!iterValueIsConst) {
                        *outIsConst = 0;
                        return 0;
                    }
                    if (!(c->ast->nodes[valueNode].dataEnd
                              == c->ast->nodes[valueNode].dataStart + 1u
                          && c->src.ptr[c->ast->nodes[valueNode].dataStart] == '_'))
                    {
                        iterBindings[iterBindingLen].nameStart = c->ast->nodes[valueNode].dataStart;
                        iterBindings[iterBindingLen].nameEnd = c->ast->nodes[valueNode].dataEnd;
                        iterBindings[iterBindingLen].typeId = -1;
                        iterBindings[iterBindingLen].typeNode = -1;
                        iterBindings[iterBindingLen].mutable = 0;
                        iterBindings[iterBindingLen].value = iterValue;
                        iterBindingLen++;
                    }
                }

                if (SLCTFEExecEvalBlockImpl(
                        c,
                        bodyNode,
                        iterBindings,
                        iterBindingLen,
                        outValue,
                        &didReturn,
                        &isConst,
                        &bodyAction)
                    != 0)
                {
                    return -1;
                }
                if (!isConst) {
                    *outIsConst = 0;
                    return 0;
                }
                if (didReturn) {
                    *outDidReturn = 1;
                    *outIsConst = 1;
                    return 0;
                }
                if (bodyAction == SLCTFEExecLoopAction_BREAK) {
                    *outIsConst = 1;
                    return 0;
                }
            }
            *outIsConst = 1;
            return 0;
        }

        while (child >= 0 && count < 4) {
            nodes[count++] = child;
            child = c->ast->nodes[child].nextSibling;
        }
        if (count <= 0 || child >= 0) {
            SLCTFEExecSetReasonNode(
                c, stmtNode, "for loop form is not supported in const evaluation");
            *outIsConst = 0;
            return 0;
        }

        bodyNode = nodes[count - 1];
        if (count == 1) {
            /* for { body } */
        } else if (
            count == 2 && c->ast->nodes[nodes[0]].kind != SLAst_VAR
            && c->ast->nodes[nodes[0]].kind != SLAst_CONST)
        {
            condNode = nodes[0];
        } else {
            initNode = nodes[0];
            if (count >= 3) {
                condNode = nodes[1];
            }
            if (count >= 4) {
                postNode = nodes[2];
            }
        }
        if (c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
            SLCTFEExecSetReasonNode(
                c, stmtNode, "for loop body must be a block in const evaluation");
            *outIsConst = 0;
            return 0;
        }

        if (initNode >= 0) {
            if (c->ast->nodes[initNode].kind == SLAst_VAR
                || c->ast->nodes[initNode].kind == SLAst_CONST)
            {
                initBindingCap = 1;
            }
            if (initBindingCap > 0) {
                loopBindings = (SLCTFEExecBinding*)SLArenaAlloc(
                    c->arena,
                    sizeof(SLCTFEExecBinding) * initBindingCap,
                    (uint32_t)_Alignof(SLCTFEExecBinding));
                if (loopBindings == NULL) {
                    return -1;
                }
            }
            loopFrame.parent = c->env;
            loopFrame.bindings = loopBindings;
            loopFrame.bindingLen = 0;
            c->env = &loopFrame;
            hasLoopEnv = 1;
        }

        if (initNode >= 0) {
            if (c->ast->nodes[initNode].kind == SLAst_VAR
                || c->ast->nodes[initNode].kind == SLAst_CONST)
            {
                SLCTFEExecLoopAction initAction = SLCTFEExecLoopAction_NONE;
                if (SLCTFEExecEvalStmt(
                        c,
                        &loopFrame,
                        initBindingCap,
                        initNode,
                        outValue,
                        &didReturn,
                        &isConst,
                        &initAction)
                    != 0)
                {
                    goto for_error;
                }
                if (initAction != SLCTFEExecLoopAction_NONE) {
                    SLCTFEExecSetReasonNode(
                        c, initNode, "for-loop initializer cannot use loop control statements");
                    goto for_nonconst;
                }
                if (!isConst || didReturn) {
                    goto for_nonconst;
                }
            } else {
                if (SLCTFEExecEvalExprSideEffect(c, initNode, &isConst) != 0) {
                    goto for_error;
                }
                if (!isConst) {
                    goto for_nonconst;
                }
            }
        }

        for (iter = 0; iter < iterLimit; iter++) {
            SLCTFEExecLoopAction bodyAction = SLCTFEExecLoopAction_NONE;
            if (condNode >= 0) {
                int condTruth = 0;
                if (SLCTFEExecEvalExpr(c, condNode, &condValue, &condIsConst) != 0) {
                    goto for_error;
                }
                if (!condIsConst) {
                    SLCTFEExecSetReasonNode(
                        c, condNode, "for-loop condition must be a const bool expression");
                    goto for_nonconst;
                }
                if (condValue.kind == SLCTFEValue_BOOL) {
                    condTruth = condValue.b ? 1 : 0;
                } else if (condValue.kind == SLCTFEValue_OPTIONAL) {
                    condTruth = condValue.b != 0u;
                } else {
                    SLCTFEExecSetReasonNode(
                        c, condNode, "for-loop condition must be a const bool expression");
                    goto for_nonconst;
                }
                if (!condTruth) {
                    if (hasLoopEnv) {
                        c->env = savedEnv;
                    }
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (SLCTFEExecEvalBlockImpl(
                    c, bodyNode, NULL, 0, outValue, &didReturn, &isConst, &bodyAction)
                != 0)
            {
                goto for_error;
            }
            if (!isConst) {
                goto for_nonconst;
            }
            if (didReturn) {
                if (hasLoopEnv) {
                    c->env = savedEnv;
                }
                *outDidReturn = 1;
                *outIsConst = 1;
                return 0;
            }
            if (bodyAction == SLCTFEExecLoopAction_BREAK) {
                if (hasLoopEnv) {
                    c->env = savedEnv;
                }
                *outIsConst = 1;
                return 0;
            }
            if (bodyAction == SLCTFEExecLoopAction_CONTINUE && postNode < 0) {
                continue;
            }
            if (postNode >= 0) {
                if (SLCTFEExecEvalExprSideEffect(c, postNode, &isConst) != 0) {
                    goto for_error;
                }
                if (!isConst) {
                    goto for_nonconst;
                }
            }
        }

        SLCTFEExecSetReasonNode(c, stmtNode, "for-loop exceeded const-eval iteration limit");

    for_nonconst:
        if (hasLoopEnv) {
            c->env = savedEnv;
        }
        *outIsConst = 0;
        return 0;
    for_error:
        if (hasLoopEnv) {
            c->env = savedEnv;
        }
        return -1;
    }

    SLCTFEExecSetReasonNode(c, stmtNode, "statement is not supported in const evaluation");
    *outIsConst = 0;
    return 0;
}

SL_API_END
