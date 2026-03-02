#include "internal.h"

SL_API_BEGIN

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

    if ((c->types[baseTarget].kind == SLTCType_PTR || c->types[baseTarget].kind == SLTCType_REF
         || c->types[baseTarget].kind == SLTCType_OPTIONAL
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
    SLTypeCheckCtx*  c;
    const SLAstNode* callee;
    int32_t          calleeNode;
    int32_t          argNode;
    int32_t          extraNode;
    int32_t          argType;
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
    if (SLTCTypeExpr(c, argNode, &argType) != 0) {
        return -1;
    }
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
            op = SLTCCompilerDiagOpFromName(c, callee->dataStart, callee->dataEnd);
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
    if (kind == SLAst_CALL) {
        int32_t          calleeNode = SLAstFirstChild(c->ast, exprNode);
        const SLAstNode* callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
        int              compilerDiagStatus;
        int              spanOfStatus;
        int              reflectStatus;
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
    }
    if (kind == SLAst_CAST) {
        return SLTCConstEvalCast(evalCtx, exprNode, outValue, outIsConst);
    }
    rc = SLCTFEEvalExpr(
        c->arena,
        c->ast,
        c->src,
        exprNode,
        SLTCResolveConstIdent,
        SLTCResolveConstCall,
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
    if (evalCtx == NULL || evalCtx->tc == NULL || outTypeId == NULL) {
        return -1;
    }
    return SLTCResolveTypeNode(evalCtx->tc, typeNode, outTypeId);
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
        case SLCTFEValue_INT:
            *outTypeId = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
            return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_FLOAT:
            *outTypeId = SLTCFindBuiltinByKind(c, SLBuiltin_F64);
            return *outTypeId >= 0 ? 0 : -1;
        case SLCTFEValue_BOOL: *outTypeId = c->typeBool; return *outTypeId >= 0 ? 0 : -1;
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
        default:               return -1;
    }
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

int SLTCResolveConstCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLTCConstEvalCtx*  evalCtx = (SLTCConstEvalCtx*)ctx;
    SLTypeCheckCtx*    c;
    int32_t            fnIndex;
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
    int                rc;

    (void)diag;
    if (evalCtx == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
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
            if (paramTypeNode < 0 || SLTCResolveTypeNode(c, paramTypeNode, &paramTypeId) != 0) {
                return -1;
            }
            paramBindings[paramCount].nameStart = n->dataStart;
            paramBindings[paramCount].nameEnd = n->dataEnd;
            paramBindings[paramCount].typeId = paramTypeId;
            paramBindings[paramCount].mutable = 1;
            paramBindings[paramCount]._reserved[0] = 0;
            paramBindings[paramCount]._reserved[1] = 0;
            paramBindings[paramCount]._reserved[2] = 0;
            paramBindings[paramCount].value = args[paramCount];
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
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = SLTC_CONST_FOR_MAX_ITERS;
    SLCTFEExecResetReason(&execCtx);
    evalCtx->execCtx = &execCtx;
    evalCtx->fnStack[evalCtx->fnDepth++] = fnIndex;

    rc = SLCTFEExecEvalBlock(&execCtx, bodyNode, &retValue, &didReturn, &isConst);
    evalCtx->fnDepth = savedDepth;
    evalCtx->execCtx = savedExecCtx;
    if (rc != 0) {
        return -1;
    }
    if (!isConst || !didReturn) {
        if (execCtx.nonConstReason != NULL) {
            evalCtx->nonConstReason = execCtx.nonConstReason;
            evalCtx->nonConstStart = execCtx.nonConstStart;
            evalCtx->nonConstEnd = execCtx.nonConstEnd;
        }
        if (!didReturn) {
            SLTCConstSetReasonNode(
                evalCtx, bodyNode, "const-evaluable function must produce a const return value");
        }
        *outIsConst = 0;
        return 0;
    }
    *outValue = retValue;
    *outIsConst = 1;
    return 0;
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

int SLTCConstIntExpr(SLTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst) {
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
    if (SLCTFEValueToInt64(&value, out) != 0) {
        c->lastConstEvalReason = "expression evaluated to a non-integer value";
        c->lastConstEvalReasonStart = c->ast->nodes[nodeId].start;
        c->lastConstEvalReasonEnd = c->ast->nodes[nodeId].end;
        return 0;
    }
    *isConst = 1;
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
