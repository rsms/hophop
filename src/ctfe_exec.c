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
        case SLCTFEValue_NULL:     *outEq = 1; return 1;
        case SLCTFEValue_OPTIONAL: {
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
        *outIsConst = 0;
        return 0;
    }

    b = SLCTFEExecEnvFindBinding(c, lhs->dataStart, lhs->dataEnd);
    if (b == NULL || !b->mutable) {
        SLCTFEExecSetReasonNode(
            c, lhsNode, "assignment target is not mutable during const evaluation");
        *outIsConst = 0;
        return 0;
    }

    if (SLCTFEExecEvalExpr(c, rhsNode, &rhsValue, &rhsIsConst) != 0) {
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
    SLCTFEExecCtx*        c,
    int32_t               blockNode,
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

int SLCTFEExecEvalBlock(
    SLCTFEExecCtx* c,
    int32_t        blockNode,
    SLCTFEValue*   outValue,
    int*           outDidReturn,
    int*           outIsConst) {
    SLCTFEExecLoopAction loopAction = SLCTFEExecLoopAction_NONE;
    if (SLCTFEExecEvalBlockImpl(c, blockNode, outValue, outDidReturn, outIsConst, &loopAction) != 0)
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
    SLCTFEExecCtx*        c,
    int32_t               blockNode,
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
    frame.parent = savedEnv;
    frame.bindings = bindings;
    frame.bindingLen = 0;
    c->env = &frame;

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
        if (c->pendingReturnExprNode < 0 || (uint32_t)c->pendingReturnExprNode >= c->ast->len) {
            SLCTFEExecSetReasonNode(
                c, blockNode, "return expression is not available for const evaluation");
            didReturn = 0;
            loopAction = SLCTFEExecLoopAction_NONE;
            isConst = 0;
        } else if (SLCTFEExecEvalExpr(c, c->pendingReturnExprNode, &retValue, &retIsConst) != 0) {
            c->env = savedEnv;
            return -1;
        } else if (!retIsConst) {
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
        } else {
            *outValue = retValue;
        }
    }

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
        int32_t     initNode = SLCTFEExecVarLikeInitExprNode(c->ast, stmtNode);
        int32_t     declTypeNode = c->ast->nodes[stmtNode].firstChild;
        int32_t     declTypeId = -1;
        SLCTFEValue v;
        int         isConst = 0;
        if (declTypeNode >= 0 && c->ast->nodes[declTypeNode].kind == SLAst_NAME_LIST) {
            SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        if (initNode < 0 || frame->bindingLen >= bindingCap || frame->bindings == NULL) {
            SLCTFEExecSetReasonNode(c, stmtNode, "declaration is not const-evaluable");
            *outIsConst = 0;
            return 0;
        }
        if (declTypeNode >= 0 && SLCTFEExecIsTypeNodeKind(c->ast->nodes[declTypeNode].kind)
            && c->resolveType != NULL)
        {
            if (c->resolveType(c->resolveTypeCtx, declTypeNode, &declTypeId) != 0) {
                return -1;
            }
        }
        if (SLCTFEExecEvalExpr(c, initNode, &v, &isConst) != 0) {
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
        frame->bindings[frame->bindingLen].mutable = s->kind == SLAst_VAR;
        frame->bindings[frame->bindingLen]._reserved[0] = 0;
        frame->bindings[frame->bindingLen]._reserved[1] = 0;
        frame->bindings[frame->bindingLen]._reserved[2] = 0;
        frame->bindings[frame->bindingLen].value = v;
        frame->bindingLen++;
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
        if (SLCTFEExecEvalBlockImpl(c, stmtNode, outValue, &didReturn, &isConst, &loopAction) != 0)
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
        if (blockNode < 0 || c->ast->nodes[blockNode].kind != SLAst_BLOCK) {
            SLCTFEExecSetReasonNode(c, stmtNode, "const block is malformed for const evaluation");
            *outIsConst = 0;
            return 0;
        }
        if (SLCTFEExecEvalBlockImpl(c, blockNode, outValue, &didReturn, &isConst, &loopAction) != 0)
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
            if (SLCTFEExecEvalBlockImpl(c, branchNode, outValue, &didReturn, &isConst, &loopAction)
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
                        if (SLCTFEExecEvalExpr(c, labelExprNode, &labelValue, &labelIsConst) != 0) {
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
                                "switch case label is not comparable to const-evaluated subject");
                            *outIsConst = 0;
                            return 0;
                        }
                        if (labelMatch) {
                            matched = 1;
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
                    if (SLCTFEExecEvalBlockImpl(
                            c, bodyNode, outValue, outDidReturn, outIsConst, &bodyAction)
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
                    c, defaultBodyNode, outValue, outDidReturn, outIsConst, &bodyAction)
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
            SLCTFEExecSetReasonNode(
                c, stmtNode, "for-in loop is not supported in const evaluation");
            *outIsConst = 0;
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
            if (SLCTFEExecEvalBlockImpl(c, bodyNode, outValue, &didReturn, &isConst, &bodyAction)
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
