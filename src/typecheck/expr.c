#include "internal.h"

H2_API_BEGIN

static int H2TCFailUnaryOpTypeMismatch(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t exprNode, const char* opName, int32_t typeId) {
    char        typeBuf[H2TC_DIAG_TEXT_CAP];
    char        detailBuf[256];
    H2TCTextBuf typeText;
    H2TCTextBuf detailText;
    int         rc = H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    H2TCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    H2TCFormatTypeRec(c, typeId, &typeText, 0);
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "cannot ");
    H2TCTextBufAppendCStr(&detailText, opName);
    H2TCTextBufAppendCStr(&detailText, " expression of type ");
    H2TCTextBufAppendCStr(&detailText, typeBuf);
    c->diag->detail = H2TCAllocDiagText(c, detailBuf);
    if (exprNode >= 0 && (uint32_t)exprNode < c->ast->len) {
        c->diag->argStart = c->ast->nodes[exprNode].start;
        c->diag->argEnd = c->ast->nodes[exprNode].end;
        c->diag->argText = NULL;
        c->diag->argTextLen = 0;
        c->diag->arg2Start = 0;
        c->diag->arg2End = 0;
        c->diag->arg2Text = NULL;
        c->diag->arg2TextLen = 0;
    }
    return rc;
}

int H2TCValidateMemAllocatorArg(H2TypeCheckCtx* c, int32_t nodeId, int32_t allocBaseType) {
    const H2AstNode* n;
    int32_t          allocType;
    int32_t          allocRefType;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    n = &c->ast->nodes[nodeId];

    if (H2TCTypeExpr(c, nodeId, &allocType) != 0) {
        return -1;
    }
    if (H2TCCanAssign(c, allocBaseType, allocType)) {
        if (!H2TCExprIsAssignable(c, nodeId)) {
            return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
        }
        return 0;
    }

    allocRefType = H2TCInternRefType(c, allocBaseType, 1, n->start, n->end);
    if (allocRefType < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    }
    if (!H2TCCanAssign(c, allocRefType, allocType)) {
        return H2TCFailTypeMismatchDetail(c, nodeId, nodeId, allocType, allocRefType);
    }
    return 0;
}

static uint32_t H2TCArrayLitElementCount(H2TypeCheckCtx* c, int32_t nodeId) {
    uint32_t count = 0;
    int32_t  child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        count++;
        child = H2AstNextSibling(c->ast, child);
    }
    return count;
}

static int H2TCArrayLitValidateConstElements(H2TypeCheckCtx* c, int32_t nodeId) {
    int32_t child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        H2TCConstEvalCtx evalCtx;
        H2CTFEValue      value;
        int              isConst = 0;
        memset(&evalCtx, 0, sizeof(evalCtx));
        evalCtx.tc = c;
        evalCtx.rootCallOwnerFnIndex = -1;
        H2TCClearLastConstEvalReason(c);
        if (H2TCEvalConstExprNode(&evalCtx, child, &value, &isConst) != 0) {
            return -1;
        }
        H2TCStoreLastConstEvalReason(c, &evalCtx);
        if (!isConst) {
            int rc;
            if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd
                && c->lastConstEvalReasonEnd <= c->src.len)
            {
                rc = H2TCFailSpan(
                    c,
                    H2Diag_ARRAY_LITERAL_READONLY_CONST_REQUIRED,
                    c->lastConstEvalReasonStart,
                    c->lastConstEvalReasonEnd);
            } else {
                rc = H2TCFailNode(c, child, H2Diag_ARRAY_LITERAL_READONLY_CONST_REQUIRED);
            }
            H2TCAttachConstEvalReason(c);
            return rc;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCTypeArrayLit(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType) {
    const H2AstNode* n;
    int32_t          expectedBase = -1;
    int32_t          targetType = -1;
    int32_t          elemType = -1;
    uint32_t         targetLen = 0;
    uint32_t         elemCount;
    int              expectedReadonlyRef = 0;
    int              hasTargetLen = 0;
    int32_t          child;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailSpan(c, H2Diag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];
    elemCount = H2TCArrayLitElementCount(c, nodeId);

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen) {
        expectedBase = H2TCResolveAliasBaseType(c, expectedType);
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
            && c->types[expectedBase].kind == H2TCType_REF)
        {
            if (H2TCTypeIsMutable(&c->types[expectedBase])) {
                return H2TCFailNode(c, nodeId, H2Diag_ARRAY_LITERAL_MUT_REF_FORBIDDEN);
            }
            expectedReadonlyRef = 1;
            targetType = c->types[expectedBase].baseType;
            expectedBase = H2TCResolveAliasBaseType(c, targetType);
        } else {
            if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
                && c->types[expectedBase].kind == H2TCType_PTR)
            {
                return H2TCFailNode(c, nodeId, H2Diag_ARRAY_LITERAL_MUT_REF_FORBIDDEN);
            }
            targetType = expectedType;
        }
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen) {
            const H2TCType* t = &c->types[expectedBase];
            if (t->kind == H2TCType_ARRAY) {
                elemType = t->baseType;
                targetLen = t->arrayLen;
                hasTargetLen = 1;
            } else if (t->kind == H2TCType_SLICE && !H2TCTypeIsMutable(t)) {
                elemType = t->baseType;
                targetLen = elemCount;
                hasTargetLen = 1;
            }
        }
    }

    if (elemType >= 0) {
        uint32_t i = 0;
        if (hasTargetLen && elemCount > targetLen) {
            return H2TCFailNode(c, nodeId, H2Diag_ARRAY_LITERAL_TOO_MANY_ELEMENTS);
        }
        child = H2AstFirstChild(c->ast, nodeId);
        while (child >= 0) {
            int32_t childType;
            if (H2TCTypeExprExpected(c, child, elemType, &childType) != 0) {
                return -1;
            }
            if (!H2TCCanAssign(c, elemType, childType)) {
                return H2TCFailTypeMismatchDetail(c, child, child, childType, elemType);
            }
            i++;
            child = H2AstNextSibling(c->ast, child);
        }
        (void)i;
        if (expectedReadonlyRef && H2TCArrayLitValidateConstElements(c, nodeId) != 0) {
            return -1;
        }
        if (expectedReadonlyRef) {
            *outType = expectedType;
            return 0;
        }
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
            && c->types[expectedBase].kind == H2TCType_SLICE)
        {
            int32_t arrayType = H2TCInternArrayType(c, elemType, elemCount, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            *outType = arrayType;
            return 0;
        }
        *outType = targetType;
        return 0;
    }

    if (elemCount == 0u) {
        return H2TCFailNode(c, nodeId, H2Diag_ARRAY_LITERAL_EMPTY_TYPE_UNKNOWN);
    }

    child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        int32_t childType;
        if (H2TCTypeExpr(c, child, &childType) != 0) {
            return -1;
        }
        if (childType == c->typeNull) {
            return H2TCFailNode(c, child, H2Diag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (childType == c->typeVoid) {
            return H2TCFailNode(c, child, H2Diag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (elemType < 0) {
            elemType = childType;
        } else if (H2TCCoerceForBinary(c, elemType, childType, &elemType) != 0) {
            return H2TCFailTypeMismatchDetail(c, child, child, childType, elemType);
        }
        child = H2AstNextSibling(c->ast, child);
    }
    if (H2TCConcretizeInferredType(c, elemType, &elemType) != 0) {
        return -1;
    }
    child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        int32_t childType;
        if (H2TCTypeExprExpected(c, child, elemType, &childType) != 0) {
            return -1;
        }
        if (!H2TCCanAssign(c, elemType, childType)) {
            return H2TCFailTypeMismatchDetail(c, child, child, childType, elemType);
        }
        child = H2AstNextSibling(c->ast, child);
    }
    *outType = H2TCInternArrayType(c, elemType, elemCount, n->start, n->end);
    return *outType < 0 ? -1 : 0;
}

int H2TCTypeNewExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const H2AstNode* n;
    int32_t          typeNode;
    int32_t          nextNode;
    int32_t          countArgNode = -1;
    int32_t          initArgNode = -1;
    int32_t          allocArgNode = -1;
    int32_t          allocBaseType;
    int32_t          elemType;
    int32_t          resultType;
    int32_t          countType;
    int32_t          ctxMemType;
    int64_t          countValue = 0;
    int              countIsConst = 0;
    int              hasCount;
    int              hasInit;
    int              hasAlloc;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailSpan(c, H2Diag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    hasCount = (n->flags & H2AstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & H2AstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & H2AstFlag_NEW_HAS_ALLOC) != 0;
    if ((n->flags & H2AstFlag_NEW_HAS_ARRAY_LIT) != 0) {
        int32_t litNode = H2AstFirstChild(c->ast, nodeId);
        int32_t expected = c->activeExpectedNewType;
        int32_t expectedBase = expected >= 0 ? H2TCResolveAliasBaseType(c, expected) : -1;
        int32_t litExpected = -1;
        int32_t litType;
        int32_t pointee;
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
            && c->types[expectedBase].kind == H2TCType_PTR)
        {
            pointee = c->types[expectedBase].baseType;
            if (pointee >= 0 && (uint32_t)pointee < c->typeLen) {
                int32_t pointeeBase = H2TCResolveAliasBaseType(c, pointee);
                if (pointeeBase >= 0 && (uint32_t)pointeeBase < c->typeLen
                    && c->types[pointeeBase].kind == H2TCType_SLICE)
                {
                    litExpected = pointee;
                } else {
                    litExpected = pointee;
                }
            }
        }
        if (litNode < 0 || (uint32_t)litNode >= c->ast->len
            || c->ast->nodes[litNode].kind != H2Ast_ARRAY_LIT)
        {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        if (H2TCTypeArrayLit(c, litNode, litExpected, &litType) != 0) {
            return -1;
        }
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
            && c->types[expectedBase].kind == H2TCType_PTR)
        {
            if (!H2TCCanAssign(c, c->types[expectedBase].baseType, litType)) {
                return H2TCFailTypeMismatchDetail(
                    c, litNode, litNode, litType, c->types[expectedBase].baseType);
            }
            *outType = expected;
            return 0;
        }
        resultType = H2TCInternPtrType(c, litType, n->start, n->end);
        if (resultType < 0) {
            return -1;
        }
        *outType = resultType;
        return 0;
    }

    typeNode = H2AstFirstChild(c->ast, nodeId);
    if (typeNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }
    nextNode = H2AstNextSibling(c->ast, typeNode);
    if (hasCount) {
        countArgNode = nextNode;
        if (countArgNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        nextNode = H2AstNextSibling(c->ast, countArgNode);
    }
    if (hasInit) {
        initArgNode = nextNode;
        if (initArgNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        nextNode = H2AstNextSibling(c->ast, initArgNode);
    }
    if (hasAlloc) {
        allocArgNode = nextNode;
        if (allocArgNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        nextNode = H2AstNextSibling(c->ast, allocArgNode);
    }
    if (nextNode >= 0) {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }

    allocBaseType = H2TCFindMemAllocatorType(c);
    if (allocBaseType < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    }

    if (allocArgNode >= 0) {
        if (H2TCValidateMemAllocatorArg(c, allocArgNode, allocBaseType) != 0) {
            return -1;
        }
    } else {
        if (H2TCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxMemType) != 0) {
            return -1;
        }
        if (!H2TCCanAssign(c, allocBaseType, ctxMemType)) {
            return H2TCFailNode(c, nodeId, H2Diag_CONTEXT_TYPE_MISMATCH);
        }
    }

    if (H2TCResolveTypeNode(c, typeNode, &elemType) != 0) {
        return -1;
    }

    if (countArgNode >= 0 && H2TCTypeContainsVarSizeByValue(c, elemType)) {
        return H2TCFailVarSizeByValue(c, typeNode, elemType, "array element position");
    }
    if (countArgNode < 0 && H2TCTypeContainsVarSizeByValue(c, elemType) && initArgNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_NEW_VARSIZE_INIT_REQUIRED);
    }

    if (initArgNode >= 0) {
        int32_t initType;
        if (H2TCTypeExprExpected(c, initArgNode, elemType, &initType) != 0) {
            return -1;
        }
        if (!H2TCCanAssign(c, elemType, initType)) {
            return H2TCFailNode(c, initArgNode, H2Diag_TYPE_MISMATCH);
        }
    }

    if (countArgNode >= 0) {
        if (H2TCTypeExpr(c, countArgNode, &countType) != 0) {
            return -1;
        }
        if (!H2TCIsIntegerType(c, countType)) {
            return H2TCFailNode(c, countArgNode, H2Diag_TYPE_MISMATCH);
        }
        if (H2TCConstIntExpr(c, countArgNode, &countValue, &countIsConst) != 0) {
            return H2TCFailNode(c, countArgNode, H2Diag_TYPE_MISMATCH);
        }
        if (countIsConst && countValue < 0) {
            return H2TCFailNode(c, countArgNode, H2Diag_TYPE_MISMATCH);
        }
    }

    if (countArgNode >= 0) {
        if (countIsConst && countValue > 0) {
            int32_t arrayType = H2TCInternArrayType(
                c, elemType, (uint32_t)countValue, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            resultType = H2TCInternPtrType(c, arrayType, n->start, n->end);
        } else {
            int32_t sliceType = H2TCInternSliceType(c, elemType, 1, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            resultType = H2TCInternPtrType(c, sliceType, n->start, n->end);
        }
    } else {
        resultType = H2TCInternPtrType(c, elemType, n->start, n->end);
    }
    if (resultType < 0) {
        return -1;
    }
    *outType = resultType;
    return 0;
}

int H2TCExprIsCompoundTemporary(H2TypeCheckCtx* c, int32_t exprNode) {
    const H2AstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, exprNode);
        return H2TCExprIsCompoundTemporary(c, inner);
    }
    if (n->kind == H2Ast_COMPOUND_LIT) {
        return 1;
    }
    if (n->kind == H2Ast_ARRAY_LIT) {
        return 1;
    }
    if (n->kind == H2Ast_UNARY && n->op == H2Tok_AND) {
        int32_t rhsNode = H2AstFirstChild(c->ast, exprNode);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == H2Ast_COMPOUND_LIT)
        {
            return 1;
        }
    }
    return 0;
}

static int H2TCMatchAnyPackIndexExpr(
    H2TypeCheckCtx* c,
    int32_t         exprNode,
    int32_t* _Nullable outPackType,
    int32_t* _Nullable outIdxNode) {
    const H2AstNode* n;
    int32_t          baseNode;
    int32_t          idxNode;
    int32_t          localIdx;
    int32_t          packType;
    int32_t          resolvedPackType;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            return 0;
        }
        return H2TCMatchAnyPackIndexExpr(c, inner, outPackType, outIdxNode);
    }
    if (n->kind != H2Ast_INDEX || (n->flags & H2AstFlag_INDEX_SLICE) != 0) {
        return 0;
    }
    baseNode = H2AstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? H2AstNextSibling(c->ast, baseNode) : -1;
    if (baseNode < 0 || idxNode < 0 || H2AstNextSibling(c->ast, idxNode) >= 0) {
        return 0;
    }
    if ((uint32_t)baseNode >= c->ast->len || c->ast->nodes[baseNode].kind != H2Ast_IDENT) {
        return 0;
    }
    localIdx = H2TCLocalFind(c, c->ast->nodes[baseNode].dataStart, c->ast->nodes[baseNode].dataEnd);
    if (localIdx < 0) {
        return 0;
    }
    packType = c->locals[localIdx].typeId;
    resolvedPackType = H2TCResolveAliasBaseType(c, packType);
    if (resolvedPackType < 0 || (uint32_t)resolvedPackType >= c->typeLen
        || c->types[resolvedPackType].kind != H2TCType_PACK)
    {
        return 0;
    }
    H2TCMarkLocalRead(c, localIdx);
    if (outPackType != NULL) {
        *outPackType = packType;
    }
    if (outIdxNode != NULL) {
        *outIdxNode = idxNode;
    }
    return 1;
}

int H2TCExprNeedsExpectedType(H2TypeCheckCtx* c, int32_t exprNode) {
    const H2AstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, exprNode);
        return H2TCExprNeedsExpectedType(c, inner);
    }
    if (n->kind == H2Ast_COMPOUND_LIT) {
        return 1;
    }
    if (H2TCMatchAnyPackIndexExpr(c, exprNode, NULL, NULL)) {
        return 1;
    }
    if (n->kind == H2Ast_UNARY && n->op == H2Tok_AND) {
        int32_t rhsNode = H2AstFirstChild(c->ast, exprNode);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == H2Ast_COMPOUND_LIT)
        {
            int32_t rhsChild = H2AstFirstChild(c->ast, rhsNode);
            return !(rhsChild >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[rhsChild].kind));
        }
    }
    return 0;
}

int H2TCResolveIdentifierExprType(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        spanStart,
    uint32_t        spanEnd,
    int32_t*        outType) {
    int32_t localIdx = H2TCLocalFind(c, nameStart, nameEnd);
    if (localIdx >= 0) {
        if (H2TCCheckLocalInitialized(c, localIdx, spanStart, spanEnd) != 0) {
            return -1;
        }
        H2TCMarkLocalRead(c, localIdx);
        *outType = c->locals[localIdx].typeId;
        return 0;
    }
    if (H2NameEqLiteral(c->src, nameStart, nameEnd, "context")) {
        int32_t contextTypeId = -1;
        if (c->currentContextType >= 0) {
            contextTypeId = c->currentContextType;
        } else if (c->hasImplicitMainRootContext && c->implicitMainContextType >= 0) {
            contextTypeId = c->implicitMainContextType;
        }
        if (contextTypeId >= 0) {
            int32_t contextRefType = H2TCInternRefType(c, contextTypeId, 1, spanStart, spanEnd);
            if (contextRefType < 0) {
                return -1;
            }
            *outType = contextRefType;
            return 0;
        }
    }
    {
        int32_t fnIdx = H2TCFindFunctionIndex(c, nameStart, nameEnd);
        if (fnIdx < 0) {
            fnIdx = H2TCFindBuiltinQualifiedFunctionIndex(c, nameStart, nameEnd);
        }
        if (fnIdx >= 0) {
            H2TCMarkFunctionUsed(c, fnIdx);
            *outType = c->funcs[fnIdx].funcTypeId;
            return 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = H2TCFindTopLevelVarLikeNode(c, nameStart, nameEnd, &nameIndex);
        if (varLikeNode >= 0) {
            if (c->currentFunctionIsCompareHook) {
                return H2TCFailSpan(c, H2Diag_COMPARISON_HOOK_IMPURE, spanStart, spanEnd);
            }
            return H2TCTypeTopLevelVarLikeNode(c, varLikeNode, nameIndex, outType);
        }
    }
    return H2TCFailSpan(c, H2Diag_UNKNOWN_SYMBOL, nameStart, nameEnd);
}

int H2TCInferAnonStructTypeFromCompound(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t firstField, int32_t* outType) {
    H2TCAnonFieldSig fieldSigs[H2TC_MAX_ANON_FIELDS];
    uint32_t         fieldCount = 0;
    int32_t          fieldNode = firstField;
    while (fieldNode >= 0) {
        const H2AstNode* field = &c->ast->nodes[fieldNode];
        int32_t          exprNode;
        int32_t          fieldType = -1;
        uint32_t         i;
        if (field->kind != H2Ast_COMPOUND_FIELD) {
            return H2TCFailNode(c, fieldNode, H2Diag_UNEXPECTED_TOKEN);
        }
        if (fieldCount >= H2TC_MAX_ANON_FIELDS) {
            return H2TCFailNode(c, fieldNode, H2Diag_ARENA_OOM);
        }
        for (i = 0; i < fieldCount; i++) {
            if (H2NameEqSlice(
                    c->src,
                    fieldSigs[i].nameStart,
                    fieldSigs[i].nameEnd,
                    field->dataStart,
                    field->dataEnd))
            {
                H2TCSetDiagWithArg(
                    c->diag,
                    H2Diag_COMPOUND_FIELD_DUPLICATE,
                    field->start,
                    field->end,
                    field->dataStart,
                    field->dataEnd);
                return -1;
            }
        }
        exprNode = H2AstFirstChild(c->ast, fieldNode);
        if (exprNode < 0) {
            if ((field->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                return H2TCFailNode(c, fieldNode, H2Diag_EXPECTED_EXPR);
            }
            if (H2TCResolveIdentifierExprType(
                    c, field->dataStart, field->dataEnd, field->start, field->end, &fieldType)
                != 0)
            {
                return -1;
            }
        } else {
            if (H2TCTypeExpr(c, exprNode, &fieldType) != 0) {
                return -1;
            }
        }
        if (fieldType == c->typeNull) {
            return H2TCFailNode(c, exprNode, H2Diag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (fieldType == c->typeVoid) {
            return H2TCFailNode(c, exprNode, H2Diag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (H2TCConcretizeInferredType(c, fieldType, &fieldType) != 0) {
            return -1;
        }
        fieldSigs[fieldCount].nameStart = field->dataStart;
        fieldSigs[fieldCount].nameEnd = field->dataEnd;
        fieldSigs[fieldCount].typeId = fieldType;
        fieldCount++;
        fieldNode = H2AstNextSibling(c->ast, fieldNode);
    }

    {
        int32_t typeId = H2TCInternAnonAggregateType(
            c,
            0,
            fieldSigs,
            fieldCount,
            -1,
            c->ast->nodes[nodeId].start,
            c->ast->nodes[nodeId].end);
        if (typeId < 0) {
            return -1;
        }
        *outType = typeId;
        return 0;
    }
}

int H2TCTypeCompoundLit(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType) {
    int32_t  child = H2AstFirstChild(c->ast, nodeId);
    int32_t  firstField = child;
    int32_t  resolvedType = -1;
    int32_t  targetType = -1;
    int32_t  targetAggregateType = -1;
    int32_t  expectedBaseType = -1;
    int32_t  enumVariantType = -1;
    uint32_t enumVariantStart = 0;
    uint32_t enumVariantEnd = 0;
    int      expectedReadonlyRef = 0;
    int      isUnion = 0;
    int      isEnumVariantLiteral = 0;
    uint32_t explicitFieldCount = 0;

    if (child >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        if (H2TCResolveTypeNode(c, child, &resolvedType) != 0) {
            int variantRc = H2TCResolveEnumVariantTypeName(
                c, child, &enumVariantType, &enumVariantStart, &enumVariantEnd);
            if (variantRc < 0) {
                return -1;
            }
            if (variantRc == 0) {
                return -1;
            }
            resolvedType = enumVariantType;
            isEnumVariantLiteral = 1;
        }
        targetType = resolvedType;
        firstField = H2AstNextSibling(c->ast, child);
    } else {
        if (expectedType < 0) {
            if (H2TCInferAnonStructTypeFromCompound(c, nodeId, firstField, &resolvedType) != 0) {
                return -1;
            }
            targetType = resolvedType;
        } else {
            resolvedType = expectedType;
            targetType = expectedType;
        }
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && c->types[expectedType].kind == H2TCType_REF)
    {
        expectedReadonlyRef = !H2TCTypeIsMutable(&c->types[expectedType]);
        expectedBaseType = c->types[expectedType].baseType;
        if (!expectedReadonlyRef) {
            return H2TCFailNode(c, nodeId, H2Diag_COMPOUND_MUT_REF_TEMPORARY);
        }
        if (expectedBaseType < 0 || (uint32_t)expectedBaseType >= c->typeLen) {
            return H2TCFailNode(c, nodeId, H2Diag_COMPOUND_INFER_NON_AGGREGATE);
        }
        if (child < 0 || !H2TCIsTypeNodeKind(c->ast->nodes[child].kind)) {
            resolvedType = expectedType;
            targetType = expectedBaseType;
        } else {
            if (!H2TCCanAssign(c, expectedBaseType, targetType)) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            resolvedType = expectedType;
        }
    }

    targetAggregateType = H2TCResolveAliasBaseType(c, targetType);
    if (targetAggregateType < 0 || (uint32_t)targetAggregateType >= c->typeLen) {
        return H2TCFailNode(
            c,
            nodeId,
            child >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[child].kind)
                ? H2Diag_COMPOUND_TYPE_REQUIRED
                : H2Diag_COMPOUND_INFER_NON_AGGREGATE);
    }
    if (c->types[targetAggregateType].kind != H2TCType_NAMED
        && c->types[targetAggregateType].kind != H2TCType_ANON_STRUCT
        && c->types[targetAggregateType].kind != H2TCType_ANON_UNION)
    {
        return H2TCFailNode(
            c,
            nodeId,
            child >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[child].kind)
                ? H2Diag_COMPOUND_TYPE_REQUIRED
                : H2Diag_COMPOUND_INFER_NON_AGGREGATE);
    }
    if (c->types[targetAggregateType].kind == H2TCType_NAMED
        && H2TCEnsureNamedTypeFieldsResolved(c, targetAggregateType) != 0)
    {
        return -1;
    }
    if (isEnumVariantLiteral) {
        int32_t payloadType = -1;
        int32_t payloadBaseType = -1;
        if (!H2TCIsNamedDeclKind(c, targetAggregateType, H2Ast_ENUM)) {
            return H2TCFailNode(c, nodeId, H2Diag_COMPOUND_TYPE_REQUIRED);
        }
        if (H2TCEnumVariantPayloadType(
                c, targetAggregateType, enumVariantStart, enumVariantEnd, &payloadType)
            != 0)
        {
            return H2TCFailNode(c, nodeId, H2Diag_COMPOUND_TYPE_REQUIRED);
        }
        payloadBaseType = H2TCResolveAliasBaseType(c, payloadType);
        if (payloadBaseType < 0 || (uint32_t)payloadBaseType >= c->typeLen
            || !(
                (c->types[payloadBaseType].kind == H2TCType_ANON_STRUCT)
                || (c->types[payloadBaseType].kind == H2TCType_NAMED
                    && H2TCIsNamedDeclKind(c, payloadBaseType, H2Ast_STRUCT))))
        {
            return H2TCFailNode(c, nodeId, H2Diag_COMPOUND_TYPE_REQUIRED);
        }
        isUnion = 0;
    } else if (c->types[targetAggregateType].kind == H2TCType_NAMED) {
        int32_t declNode = c->types[targetAggregateType].declNode;
        if (declNode < 0 || (uint32_t)declNode >= c->ast->len
            || (c->ast->nodes[declNode].kind != H2Ast_STRUCT
                && c->ast->nodes[declNode].kind != H2Ast_UNION))
        {
            return H2TCFailNode(
                c,
                nodeId,
                child >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[child].kind)
                    ? H2Diag_COMPOUND_TYPE_REQUIRED
                    : H2Diag_COMPOUND_INFER_NON_AGGREGATE);
        }
        isUnion = c->ast->nodes[declNode].kind == H2Ast_UNION;
    } else {
        isUnion = c->types[targetAggregateType].kind == H2TCType_ANON_UNION;
    }

    while (firstField >= 0) {
        const H2AstNode* fieldNode = &c->ast->nodes[firstField];
        int32_t          fieldType = -1;
        int32_t          exprNode;
        int32_t          exprType = -1;
        int32_t          scan;

        if (fieldNode->kind != H2Ast_COMPOUND_FIELD) {
            return H2TCFailNode(c, firstField, H2Diag_UNEXPECTED_TOKEN);
        }

        scan = H2AstFirstChild(c->ast, nodeId);
        if (scan >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[scan].kind)) {
            scan = H2AstNextSibling(c->ast, scan);
        }
        while (scan >= 0 && scan != firstField) {
            const H2AstNode* prevField = &c->ast->nodes[scan];
            if (prevField->kind == H2Ast_COMPOUND_FIELD
                && H2NameEqSlice(
                    c->src,
                    prevField->dataStart,
                    prevField->dataEnd,
                    fieldNode->dataStart,
                    fieldNode->dataEnd))
            {
                H2TCSetDiagWithArg(
                    c->diag,
                    H2Diag_COMPOUND_FIELD_DUPLICATE,
                    fieldNode->start,
                    fieldNode->end,
                    fieldNode->dataStart,
                    fieldNode->dataEnd);
                return -1;
            }
            scan = H2AstNextSibling(c->ast, scan);
        }

        if ((!isEnumVariantLiteral
             && H2TCFieldLookupPath(
                    c, targetAggregateType, fieldNode->dataStart, fieldNode->dataEnd, &fieldType)
                    != 0)
            || (isEnumVariantLiteral
                && H2TCEnumVariantPayloadFieldType(
                       c,
                       targetAggregateType,
                       enumVariantStart,
                       enumVariantEnd,
                       fieldNode->dataStart,
                       fieldNode->dataEnd,
                       &fieldType)
                       != 0))
        {
            H2TCSetDiagWithArg(
                c->diag,
                H2Diag_COMPOUND_FIELD_UNKNOWN,
                fieldNode->start,
                fieldNode->end,
                fieldNode->dataStart,
                fieldNode->dataEnd);
            return -1;
        }

        exprNode = H2AstFirstChild(c->ast, firstField);
        if (exprNode < 0) {
            if ((fieldNode->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                return H2TCFailNode(c, firstField, H2Diag_EXPECTED_EXPR);
            }
            if (H2TCResolveIdentifierExprType(
                    c,
                    fieldNode->dataStart,
                    fieldNode->dataEnd,
                    fieldNode->start,
                    fieldNode->end,
                    &exprType)
                != 0)
            {
                return -1;
            }
        } else if (H2TCTypeExprExpected(c, exprNode, fieldType, &exprType) != 0) {
            return -1;
        }
        if (!H2TCCanAssign(c, fieldType, exprType)) {
            uint32_t errStart =
                exprNode >= 0 ? c->ast->nodes[exprNode].start : c->ast->nodes[firstField].start;
            uint32_t errEnd =
                exprNode >= 0 ? c->ast->nodes[exprNode].end : c->ast->nodes[firstField].end;
            H2TCSetDiagWithArg(
                c->diag,
                H2Diag_COMPOUND_FIELD_TYPE_MISMATCH,
                errStart,
                errEnd,
                fieldNode->dataStart,
                fieldNode->dataEnd);
            return -1;
        }
        explicitFieldCount++;
        firstField = H2AstNextSibling(c->ast, firstField);
    }

    if (isUnion && explicitFieldCount > 1u) {
        return H2TCFailNode(c, nodeId, H2Diag_COMPOUND_UNION_MULTI_FIELD);
    }

    *outType = resolvedType;
    return 0;
}

int H2TCTypeExprExpected(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailSpan(c, H2Diag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, nodeId);
        if (inner < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        return H2TCTypeExprExpected(c, inner, expectedType, outType);
    }

    if (n->kind == H2Ast_CALL || n->kind == H2Ast_CALL_WITH_CONTEXT) {
        int32_t savedExpectedCallType = c->activeExpectedCallType;
        int     rc;
        c->activeExpectedCallType = expectedType;
        rc = H2TCTypeExpr(c, nodeId, outType);
        c->activeExpectedCallType = savedExpectedCallType;
        return rc;
    }

    if (n->kind == H2Ast_COMPOUND_LIT) {
        return H2TCTypeCompoundLit(c, nodeId, expectedType, outType);
    }
    if (n->kind == H2Ast_ARRAY_LIT) {
        return H2TCTypeArrayLit(c, nodeId, expectedType, outType);
    }
    if (n->kind == H2Ast_NEW && (n->flags & H2AstFlag_NEW_HAS_ARRAY_LIT) != 0) {
        int32_t savedExpectedNewType = c->activeExpectedNewType;
        int     rc;
        c->activeExpectedNewType = expectedType;
        rc = H2TCTypeExpr(c, nodeId, outType);
        c->activeExpectedNewType = savedExpectedNewType;
        return rc;
    }
    if (n->kind == H2Ast_INDEX) {
        int32_t packType = -1;
        int32_t idxNode = -1;
        if (H2TCMatchAnyPackIndexExpr(c, nodeId, &packType, &idxNode)) {
            int32_t resolvedPackType = H2TCResolveAliasBaseType(c, packType);
            int32_t idxType;
            int64_t idxValue = 0;
            int     idxIsConst = 0;
            if (resolvedPackType < 0 || (uint32_t)resolvedPackType >= c->typeLen
                || c->types[resolvedPackType].kind != H2TCType_PACK)
            {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCTypeExpr(c, idxNode, &idxType) != 0) {
                return -1;
            }
            if (!H2TCIsIntegerType(c, idxType)) {
                return H2TCFailNode(c, idxNode, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
                return H2TCFailNode(c, idxNode, H2Diag_TYPE_MISMATCH);
            }
            if (!idxIsConst) {
                int32_t resolvedExpectedType = H2TCResolveAliasBaseType(c, expectedType);
                if (expectedType < 0 || resolvedExpectedType < 0) {
                    return H2TCFailNode(c, idxNode, H2Diag_ANYTYPE_PACK_INDEX_NOT_CONST);
                }
                if (resolvedExpectedType == c->typeAnytype) {
                    *outType = c->typeAnytype;
                    return 0;
                }
                *outType = expectedType;
                return 0;
            }
            if (idxValue < 0 || (uint64_t)idxValue >= c->types[resolvedPackType].fieldCount) {
                return H2TCFailNode(c, idxNode, H2Diag_ANYTYPE_PACK_INDEX_OOB);
            }
            *outType =
                c->funcParamTypes[c->types[resolvedPackType].fieldStart + (uint32_t)idxValue];
            return 0;
        }
    }

    if (n->kind == H2Ast_STRING
        || (n->kind == H2Ast_BINARY && (H2TokenKind)n->op == H2Tok_ADD
            && H2IsStringLiteralConcatChain(c->ast, nodeId)))
    {
        int32_t defaultType = H2TCGetStrRefType(c, n->start, n->end);
        if (defaultType < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
        }
        if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen) {
            int32_t expectedResolved = H2TCResolveAliasBaseType(c, expectedType);
            if (expectedResolved >= 0 && (uint32_t)expectedResolved < c->typeLen) {
                const H2TCType* t = &c->types[expectedResolved];
                int32_t         baseType = t->baseType;
                if (baseType >= 0) {
                    baseType = H2TCResolveAliasBaseType(c, baseType);
                }
                if (t->kind == H2TCType_PTR && baseType == c->typeStr) {
                    *outType = expectedResolved;
                    return 0;
                }
                if (t->kind == H2TCType_REF && baseType == c->typeStr && !H2TCTypeIsMutable(t)) {
                    *outType = expectedResolved;
                    return 0;
                }
            }
        }
        *outType = defaultType;
        return 0;
    }

    if (n->kind == H2Ast_TUPLE_EXPR) {
        int32_t expectedBase = H2TCResolveAliasBaseType(c, expectedType);
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
            && c->types[expectedBase].kind == H2TCType_TUPLE)
        {
            int32_t  child = H2AstFirstChild(c->ast, nodeId);
            uint32_t idx = 0;
            while (child >= 0) {
                int32_t srcType;
                int32_t dstType;
                if (idx >= c->types[expectedBase].fieldCount) {
                    return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
                }
                dstType = c->funcParamTypes[c->types[expectedBase].fieldStart + idx];
                if (H2TCTypeExprExpected(c, child, dstType, &srcType) != 0) {
                    return -1;
                }
                if (!H2TCCanAssign(c, dstType, srcType)) {
                    return H2TCFailTypeMismatchDetail(c, child, child, srcType, dstType);
                }
                idx++;
                child = H2AstNextSibling(c->ast, child);
            }
            if (idx != c->types[expectedBase].fieldCount) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            *outType = expectedBase;
            return 0;
        }
        return H2TCTypeExpr(c, nodeId, outType);
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && H2TCIsIntegerType(c, expectedType))
    {
        int32_t srcType;
        if (H2TCTypeExpr(c, nodeId, &srcType) != 0) {
            return -1;
        }
        if (srcType == c->typeUntypedInt) {
            int64_t value = 0;
            int     isConst = 0;
            if (H2TCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!H2TCConstIntFitsType(c, value, expectedType)) {
                return H2TCFailConstIntRange(c, nodeId, value, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
        if (H2TCTypeIsRuneLike(c, srcType)) {
            int64_t value = 0;
            int     isConst = 0;
            if (H2TCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!H2TCConstIntFitsType(c, value, expectedType)) {
                return H2TCFailConstIntRange(c, nodeId, value, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && H2TCIsFloatType(c, expectedType))
    {
        int32_t srcType;
        if (H2TCTypeExpr(c, nodeId, &srcType) != 0) {
            return -1;
        }
        if (srcType == c->typeUntypedInt) {
            int64_t value = 0;
            int     isConst = 0;
            if (H2TCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!H2TCConstIntFitsFloatType(c, value, expectedType)) {
                return H2TCFailConstFloatRange(c, nodeId, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
        if (srcType == c->typeUntypedFloat) {
            double value = 0.0;
            int    isConst = 0;
            if (H2TCConstFloatExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!H2TCConstFloatFitsType(c, value, expectedType)) {
                return H2TCFailConstFloatRange(c, nodeId, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
    }

    if (n->kind == H2Ast_UNARY && n->op == H2Tok_AND) {
        int32_t rhsNode = H2AstFirstChild(c->ast, nodeId);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == H2Ast_COMPOUND_LIT)
        {
            int32_t rhsExpected = -1;
            int32_t rhsType;
            int32_t ptrType;
            if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
                && (c->types[expectedType].kind == H2TCType_REF
                    || c->types[expectedType].kind == H2TCType_PTR))
            {
                rhsExpected = c->types[expectedType].baseType;
            }
            if (H2TCTypeExprExpected(c, rhsNode, rhsExpected, &rhsType) != 0) {
                return -1;
            }
            ptrType = H2TCInternPtrType(c, rhsType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
    }

    return H2TCTypeExpr(c, nodeId, outType);
}

int H2TCExprIsAssignable(H2TypeCheckCtx* c, int32_t exprNode) {
    const H2AstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, exprNode);
        return H2TCExprIsAssignable(c, inner);
    }
    if (n->kind == H2Ast_IDENT) {
        return 1;
    }
    if (n->kind == H2Ast_INDEX) {
        H2TCIndexBaseInfo info;
        int32_t           baseNode = H2AstFirstChild(c->ast, exprNode);
        int32_t           baseType;
        if ((n->flags & H2AstFlag_INDEX_SLICE) != 0) {
            return 0;
        }
        if (baseNode < 0 || H2TCTypeExpr(c, baseNode, &baseType) != 0 || baseType < 0
            || (uint32_t)baseType >= c->typeLen)
        {
            return 0;
        }
        if (H2TCResolveIndexBaseInfo(c, baseType, &info) != 0) {
            return 0;
        }
        if (!info.indexable) {
            return 0;
        }
        if (c->types[baseType].kind == H2TCType_ARRAY || c->types[baseType].kind == H2TCType_PTR) {
            return 1;
        }
        return info.sliceMutable;
    }
    if (n->kind == H2Ast_FIELD_EXPR) {
        int32_t  recvNode = H2AstFirstChild(c->ast, exprNode);
        int32_t  recvType;
        int32_t  fieldType;
        uint32_t fieldIndex = 0;
        if (recvNode < 0) {
            return 0;
        }
        if (H2TCTypeExpr(c, recvNode, &recvType) != 0) {
            return 0;
        }
        if (H2TCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, &fieldIndex) != 0) {
            return 0;
        }
        if ((c->fields[fieldIndex].flags & H2TCFieldFlag_DEPENDENT) != 0) {
            return 0;
        }
        if (recvType >= 0 && (uint32_t)recvType < c->typeLen
            && c->types[recvType].kind == H2TCType_REF)
        {
            return H2TCTypeIsMutable(&c->types[recvType]);
        }
        return 1;
    }
    if (n->kind == H2Ast_UNARY && n->op == H2Tok_MUL) {
        int32_t rhsNode = H2AstFirstChild(c->ast, exprNode);
        int32_t rhsType;
        if (rhsNode < 0 || H2TCTypeExpr(c, rhsNode, &rhsType) != 0 || rhsType < 0
            || (uint32_t)rhsType >= c->typeLen)
        {
            return 0;
        }
        if (c->types[rhsType].kind == H2TCType_PTR) {
            return 1;
        }
        if (c->types[rhsType].kind == H2TCType_REF) {
            return H2TCTypeIsMutable(&c->types[rhsType]);
        }
        return 0;
    }
    return 0;
}

int H2TCExprIsConstAssignTarget(H2TypeCheckCtx* c, int32_t exprNode) {
    const H2AstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, exprNode);
        return H2TCExprIsConstAssignTarget(c, inner);
    }
    if (n->kind != H2Ast_IDENT) {
        return 0;
    }
    {
        int32_t localIdx = H2TCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            return (c->locals[localIdx].flags & H2TCLocalFlag_CONST) != 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = H2TCFindTopLevelVarLikeNode(c, n->dataStart, n->dataEnd, &nameIndex);
        if (varLikeNode >= 0) {
            return c->ast->nodes[varLikeNode].kind == H2Ast_CONST;
        }
    }
    return 0;
}

int H2TCTypeAssignTargetExpr(
    H2TypeCheckCtx* c, int32_t nodeId, int skipDirectIdentRead, int32_t* outType) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, nodeId);
        return H2TCTypeAssignTargetExpr(c, inner, skipDirectIdentRead, outType);
    }
    if (skipDirectIdentRead && n->kind == H2Ast_IDENT) {
        int32_t localIdx = H2TCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            *outType = c->locals[localIdx].typeId;
            return 0;
        }
    }
    return H2TCTypeExpr(c, nodeId, outType);
}

void H2TCMarkDirectIdentLocalWrite(H2TypeCheckCtx* c, int32_t nodeId, int markInitialized) {
    const H2AstNode* n;
    int32_t          localIdx;
    if (c == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == H2Ast_CALL_ARG) {
        H2TCMarkDirectIdentLocalWrite(c, H2AstFirstChild(c->ast, nodeId), markInitialized);
        return;
    }
    if (n->kind != H2Ast_IDENT) {
        return;
    }
    localIdx = H2TCLocalFind(c, n->dataStart, n->dataEnd);
    if (localIdx < 0) {
        return;
    }
    H2TCMarkLocalWrite(c, localIdx);
    if (markInitialized) {
        H2TCMarkLocalInitialized(c, localIdx);
    }
}

int H2TCTypeExpr_IDENT(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    uint32_t i;
    (void)nodeId;
    if (c->defaultFieldNodes != NULL && c->defaultFieldTypes != NULL
        && c->defaultFieldCurrentIndex < c->defaultFieldCount)
    {
        for (i = 0; i < c->defaultFieldCount; i++) {
            int32_t          fieldNode = c->defaultFieldNodes[i];
            const H2AstNode* f;
            if (fieldNode < 0 || (uint32_t)fieldNode >= c->ast->len) {
                continue;
            }
            f = &c->ast->nodes[fieldNode];
            if (f->kind != H2Ast_FIELD) {
                continue;
            }
            if (H2NameEqSlice(c->src, f->dataStart, f->dataEnd, n->dataStart, n->dataEnd)) {
                if (i < c->defaultFieldCurrentIndex && c->defaultFieldTypes[i] >= 0) {
                    *outType = c->defaultFieldTypes[i];
                    return 0;
                }
                H2TCSetDiagWithArg(
                    c->diag,
                    H2Diag_FIELD_DEFAULT_FORWARD_REF,
                    n->start,
                    n->end,
                    f->dataStart,
                    f->dataEnd);
                return -1;
            }
            if ((f->flags & H2AstFlag_FIELD_EMBEDDED) != 0 && c->defaultFieldTypes[i] >= 0) {
                int32_t promotedType = -1;
                if (H2TCFieldLookupPath(
                        c, c->defaultFieldTypes[i], n->dataStart, n->dataEnd, &promotedType)
                    == 0)
                {
                    if (i < c->defaultFieldCurrentIndex) {
                        *outType = promotedType;
                        return 0;
                    }
                    H2TCSetDiagWithArg(
                        c->diag,
                        H2Diag_FIELD_DEFAULT_FORWARD_REF,
                        n->start,
                        n->end,
                        n->dataStart,
                        n->dataEnd);
                    return -1;
                }
            }
        }
    }
    {
        int32_t localIdx = H2TCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            if (H2TCCheckLocalInitialized(c, localIdx, n->start, n->end) != 0) {
                return -1;
            }
            H2TCMarkLocalRead(c, localIdx);
            *outType = c->locals[localIdx].typeId;
            return 0;
        }
    }
    if (c->activeConstEvalCtx != NULL) {
        int32_t execType = -1;
        if (H2TCConstLookupExecBindingType(
                c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execType))
        {
            *outType = execType;
            return 0;
        }
        if (H2TCConstLookupMirLocalType(c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execType))
        {
            *outType = execType;
            return 0;
        }
        {
            H2CTFEValue execValue;
            int         execIsConst = 0;
            if (H2TCResolveConstIdent(
                    c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execValue, &execIsConst, NULL)
                    == 0
                && execIsConst
                && H2TCEvalConstExecInferValueTypeCb(c->activeConstEvalCtx, &execValue, &execType)
                       == 0)
            {
                *outType = execType;
                return 0;
            }
        }
    }
    if (H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "context")) {
        if (c->currentFunctionIsCompareHook) {
            return H2TCFailNode(c, nodeId, H2Diag_COMPARISON_HOOK_IMPURE);
        }
        int32_t contextTypeId = -1;
        if (c->currentContextType >= 0) {
            contextTypeId = c->currentContextType;
        } else if (c->hasImplicitMainRootContext && c->implicitMainContextType >= 0) {
            contextTypeId = c->implicitMainContextType;
        }
        if (contextTypeId >= 0) {
            int32_t contextRefType = H2TCInternRefType(
                c, contextTypeId, 1, n->dataStart, n->dataEnd);
            if (contextRefType < 0) {
                return -1;
            }
            *outType = contextRefType;
            return 0;
        }
    }
    {
        int32_t fnIdx = H2TCFindFunctionIndex(c, n->dataStart, n->dataEnd);
        if (fnIdx >= 0) {
            H2TCMarkFunctionUsed(c, fnIdx);
            *outType = c->funcs[fnIdx].funcTypeId;
            return 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = H2TCFindTopLevelVarLikeNode(c, n->dataStart, n->dataEnd, &nameIndex);
        if (varLikeNode >= 0) {
            if (c->currentFunctionIsCompareHook) {
                return H2TCFailNode(c, nodeId, H2Diag_COMPARISON_HOOK_IMPURE);
            }
            return H2TCTypeTopLevelVarLikeNode(c, varLikeNode, nameIndex, outType);
        }
    }
    if (H2TCResolveTypeValueName(c, n->dataStart, n->dataEnd) >= 0 && c->typeType >= 0) {
        *outType = c->typeType;
        return 0;
    }
    return H2TCFailSpan(c, H2Diag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
}

int H2TCTypeExpr_TYPE_VALUE(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t typeNode = H2AstFirstChild(c->ast, nodeId);
    int32_t ignoredType;
    (void)n;
    if (typeNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }
    if (H2TCResolveTypeNode(c, typeNode, &ignoredType) != 0) {
        return -1;
    }
    *outType = c->typeType;
    return 0;
}

int H2TCTypeExpr_INT(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedInt;
    return 0;
}

int H2TCTypeExpr_FLOAT(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedFloat;
    return 0;
}

int H2TCTypeExpr_STRING(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t strRefType = H2TCGetStrRefType(c, n->start, n->end);
    if (strRefType < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
    }
    *outType = strRefType;
    return 0;
}

int H2TCTypeExpr_RUNE(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedInt;
    return 0;
}

int H2TCTypeExpr_BOOL(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeBool;
    return 0;
}

int H2TCTypeExpr_COMPOUND_LIT(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    (void)n;
    return H2TCTypeCompoundLit(c, nodeId, -1, outType);
}

int H2TCTypeExpr_CALL_WITH_CONTEXT(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t savedActive = c->activeCallWithNode;
    int32_t callNode = H2AstFirstChild(c->ast, nodeId);
    (void)n;
    if (callNode < 0 || c->ast->nodes[callNode].kind != H2Ast_CALL) {
        return H2TCFailNode(c, nodeId, H2Diag_WITH_CONTEXT_ON_NON_CALL);
    }
    c->activeCallWithNode = nodeId;
    if (H2TCValidateCurrentCallOverlay(c) != 0) {
        c->activeCallWithNode = savedActive;
        return -1;
    }
    if (H2TCTypeExpr(c, callNode, outType) != 0) {
        c->activeCallWithNode = savedActive;
        return -1;
    }
    c->activeCallWithNode = savedActive;
    return 0;
}

int H2TCTypeExpr_NEW(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    (void)n;
    if (c->currentFunctionIsCompareHook) {
        return H2TCFailNode(c, nodeId, H2Diag_COMPARISON_HOOK_IMPURE);
    }
    return H2TCTypeNewExpr(c, nodeId, outType);
}

int H2TCTypeSourceLocationOfCall(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* callee, int32_t* outType) {
    int32_t argNode = H2AstNextSibling(c->ast, H2AstFirstChild(c->ast, nodeId));
    int32_t argType;
    int32_t nextArgNode;
    if (argNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
    if (H2TCTypeExpr(c, argNode, &argType) != 0) {
        return -1;
    }
    (void)argType;
    nextArgNode = H2AstNextSibling(c->ast, argNode);
    if (nextArgNode >= 0) {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
    if (c->typeSourceLocation < 0) {
        c->typeSourceLocation = H2TCFindSourceLocationType(c);
    }
    if (c->typeSourceLocation < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
    }
    *outType = c->typeSourceLocation;
    (void)callee;
    return 0;
}

static int H2TCTypeEmitCompilerDiagCall(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t spanNode, int32_t msgNode, H2TCCompilerDiagOp op) {
    H2TCConstEvalCtx evalCtx;
    H2CTFEValue      msgValue;
    int              msgIsConst = 0;
    uint32_t         diagStart = c->ast->nodes[nodeId].start;
    uint32_t         diagEnd = c->ast->nodes[nodeId].end;
    const char*      detail;
    H2Diag           emitted;
    int32_t          msgExprNode = msgNode;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    evalCtx.rootCallOwnerFnIndex = -1;
    if (msgNode >= 0 && (uint32_t)msgNode < c->ast->len
        && c->ast->nodes[msgNode].kind == H2Ast_CALL_ARG)
    {
        int32_t inner = H2AstFirstChild(c->ast, msgNode);
        if (inner >= 0) {
            msgExprNode = inner;
        }
    }
    if (op == H2TCCompilerDiagOp_ERROR_AT || op == H2TCCompilerDiagOp_WARN_AT) {
        int        spanIsConst = 0;
        H2CTFESpan span;
        uint32_t   spanStartOffset = 0;
        uint32_t   spanEndOffset = 0;
        if (H2TCConstEvalSourceLocationExpr(&evalCtx, spanNode, &span, &spanIsConst) != 0) {
            return -1;
        }
        if (!spanIsConst || span.startLine == 0 || span.startColumn == 0 || span.endLine == 0
            || span.endColumn == 0
            || H2TCLineColToOffset(
                   c->src.ptr, c->src.len, span.startLine, span.startColumn, &spanStartOffset)
                   != 0
            || H2TCLineColToOffset(
                   c->src.ptr, c->src.len, span.endLine, span.endColumn, &spanEndOffset)
                   != 0
            || spanEndOffset < spanStartOffset)
        {
            return H2TCFailNode(c, spanNode, H2Diag_CONSTEVAL_DIAG_INVALID_SPAN);
        }
        diagStart = spanStartOffset;
        diagEnd = spanEndOffset;
    }
    if (H2TCEvalConstExprNode(&evalCtx, msgExprNode, &msgValue, &msgIsConst) != 0) {
        return -1;
    }
    if (!msgIsConst || msgValue.kind != H2CTFEValue_STRING) {
        return H2TCFailNode(c, msgNode, H2Diag_CONSTEVAL_DIAG_MESSAGE_NOT_CONST_STRING);
    }
    detail = H2TCAllocCStringBytes(c, msgValue.s.bytes, msgValue.s.len);
    if (detail == NULL) {
        return H2TCFailNode(c, msgNode, H2Diag_ARENA_OOM);
    }
    emitted = (H2Diag){
        .code = (op == H2TCCompilerDiagOp_WARN || op == H2TCCompilerDiagOp_WARN_AT)
                  ? H2Diag_CONSTEVAL_DIAG_WARNING
                  : H2Diag_CONSTEVAL_DIAG_ERROR,
        .type = (op == H2TCCompilerDiagOp_WARN || op == H2TCCompilerDiagOp_WARN_AT)
                  ? H2DiagType_WARNING
                  : H2DiagType_ERROR,
        .start = diagStart,
        .end = diagEnd,
        .argStart = 0,
        .argEnd = 0,
        .detail = detail,
        .hintOverride = NULL,
    };
    if (emitted.type == H2DiagType_WARNING) {
        return H2TCEmitWarningDiag(c, &emitted);
    }
    if (c->diag != NULL) {
        *c->diag = emitted;
    }
    return -1;
}

int H2TCTypeCompilerDiagCall(
    H2TypeCheckCtx*    c,
    int32_t            nodeId,
    const H2AstNode*   callee,
    H2TCCompilerDiagOp op,
    int32_t*           outType) {
    int32_t arg1Node = H2AstNextSibling(c->ast, H2AstFirstChild(c->ast, nodeId));
    int32_t arg2Node = arg1Node >= 0 ? H2AstNextSibling(c->ast, arg1Node) : -1;
    int32_t arg3Node = arg2Node >= 0 ? H2AstNextSibling(c->ast, arg2Node) : -1;
    int32_t spanNode = -1;
    int32_t msgNode;
    int32_t msgType;
    int32_t wantStrType;
    if (op == H2TCCompilerDiagOp_ERROR || op == H2TCCompilerDiagOp_WARN) {
        if (arg1Node < 0 || arg2Node >= 0) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        msgNode = arg1Node;
    } else {
        int32_t spanType;
        if (arg1Node < 0 || arg2Node < 0 || arg3Node >= 0) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        spanNode = arg1Node;
        if (H2TCTypeExpr(c, arg1Node, &spanType) != 0) {
            return -1;
        }
        if (!H2TCTypeIsSourceLocation(c, spanType)) {
            return H2TCFailNode(c, arg1Node, H2Diag_TYPE_MISMATCH);
        }
        msgNode = arg2Node;
    }
    if (H2TCTypeExpr(c, msgNode, &msgType) != 0) {
        return -1;
    }
    wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
    if (wantStrType < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
    }
    if (!H2TCCanAssign(c, wantStrType, msgType)) {
        return H2TCFailNode(c, msgNode, H2Diag_TYPE_MISMATCH);
    }
    if (c->activeConstEvalCtx == NULL && c->compilerDiagPathProven != 0) {
        if (H2TCTypeEmitCompilerDiagCall(c, nodeId, spanNode, msgNode, op) != 0) {
            return -1;
        }
    }
    *outType = c->typeVoid;
    return 0;
}

typedef struct {
    int32_t elemType;
    int     isString;
    int     writable;
} H2TCCopySeqInfo;

static int32_t H2TCGetStringSliceExprType(
    H2TypeCheckCtx* c, int32_t baseType, uint32_t start, uint32_t end) {
    int32_t         resolvedType;
    const H2TCType* t;
    int32_t         resolvedBaseType;
    if (c == NULL) {
        return -1;
    }
    resolvedType = H2TCResolveAliasBaseType(c, baseType);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return -1;
    }
    if (resolvedType == c->typeStr) {
        return c->typeStr;
    }
    t = &c->types[resolvedType];
    if (t->kind != H2TCType_PTR && t->kind != H2TCType_REF) {
        return -1;
    }
    resolvedBaseType = H2TCResolveAliasBaseType(c, t->baseType);
    if (resolvedBaseType != c->typeStr) {
        return -1;
    }
    if (t->kind == H2TCType_PTR) {
        return H2TCGetStrPtrType(c, start, end);
    }
    return H2TCGetStrRefType(c, start, end);
}

static int H2TCFailUnsliceableExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t baseType) {
    char        typeBuf[128];
    char        detailBuf[160];
    H2TCTextBuf typeText;
    H2TCTextBuf detailText;
    int         rc = H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    H2TCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    H2TCFormatTypeRec(c, baseType, &typeText, 0);
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "cannot slice expression of type ");
    H2TCTextBufAppendCStr(&detailText, typeBuf);
    c->diag->detail = H2TCAllocDiagText(c, detailBuf);
    return rc;
}

static int H2TCFailInvalidCast(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t srcType, int32_t dstType) {
    char        srcBuf[128];
    char        dstBuf[128];
    char        detailBuf[256];
    H2TCTextBuf srcText;
    H2TCTextBuf dstText;
    H2TCTextBuf detailText;
    int         rc = H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    H2TCTextBufInit(&srcText, srcBuf, (uint32_t)sizeof(srcBuf));
    H2TCFormatTypeRec(c, srcType, &srcText, 0);
    H2TCTextBufInit(&dstText, dstBuf, (uint32_t)sizeof(dstBuf));
    H2TCFormatTypeRec(c, dstType, &dstText, 0);
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "cannot cast ");
    H2TCTextBufAppendCStr(&detailText, srcBuf);
    H2TCTextBufAppendCStr(&detailText, " to ");
    H2TCTextBufAppendCStr(&detailText, dstBuf);
    c->diag->detail = H2TCAllocDiagText(c, detailBuf);
    return rc;
}

static int H2TCResolveCopySeqInfo(H2TypeCheckCtx* c, int32_t typeId, H2TCCopySeqInfo* out) {
    int32_t         resolvedType;
    const H2TCType* t;
    int32_t         u8Type;
    if (c == NULL || out == NULL) {
        return -1;
    }
    out->elemType = -1;
    out->isString = 0;
    out->writable = 0;
    u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
    if (u8Type < 0) {
        return -1;
    }
    resolvedType = H2TCResolveAliasBaseType(c, typeId);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return 1;
    }
    if (resolvedType == c->typeStr) {
        out->elemType = u8Type;
        out->isString = 1;
        return 0;
    }
    t = &c->types[resolvedType];
    if (t->kind == H2TCType_ARRAY) {
        out->elemType = t->baseType;
        out->writable = 1;
        return 0;
    }
    if (t->kind == H2TCType_SLICE) {
        out->elemType = t->baseType;
        out->writable = H2TCTypeIsMutable(t);
        return 0;
    }
    if (t->kind == H2TCType_PTR || t->kind == H2TCType_REF) {
        int32_t         baseType = t->baseType;
        int32_t         resolvedBaseType;
        const H2TCType* base;
        if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
            return 1;
        }
        resolvedBaseType = H2TCResolveAliasBaseType(c, baseType);
        if (resolvedBaseType < 0 || (uint32_t)resolvedBaseType >= c->typeLen) {
            return 1;
        }
        if (resolvedBaseType == c->typeStr) {
            out->elemType = u8Type;
            out->isString = 1;
            out->writable = t->kind == H2TCType_PTR;
            return 0;
        }
        base = &c->types[resolvedBaseType];
        if (base->kind == H2TCType_ARRAY) {
            out->elemType = base->baseType;
            out->writable = t->kind == H2TCType_PTR;
            return 0;
        }
        if (base->kind == H2TCType_SLICE) {
            out->elemType = base->baseType;
            out->writable = t->kind == H2TCType_PTR && H2TCTypeIsMutable(base);
            return 0;
        }
    }
    return 1;
}

static int H2TCTypeEnumVariantCall(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t calleeNode, int32_t* outType) {
    int32_t         enumType = -1;
    uint32_t        variantStart = 0;
    uint32_t        variantEnd = 0;
    int             variantRc;
    int32_t         payloadType = -1;
    int32_t         payloadBase = -1;
    H2TCCallArgInfo callArgs[H2TC_MAX_CALL_ARGS];
    uint32_t        argCount = 0;
    uint32_t        i;

    variantRc = H2TCDecodeVariantPatternExpr(c, calleeNode, &enumType, &variantStart, &variantEnd);
    if (variantRc <= 0) {
        return variantRc;
    }
    if (H2TCEnumVariantPayloadType(c, enumType, variantStart, variantEnd, &payloadType) != 0) {
        return H2TCFailNode(c, nodeId, H2Diag_NOT_CALLABLE);
    }
    payloadBase = H2TCResolveAliasBaseType(c, payloadType);
    if (payloadBase < 0 || (uint32_t)payloadBase >= c->typeLen) {
        return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    }
    if ((c->types[payloadBase].kind == H2TCType_ANON_STRUCT)
        || (c->types[payloadBase].kind == H2TCType_NAMED
            && H2TCIsNamedDeclKind(c, payloadBase, H2Ast_STRUCT)))
    {
        return H2TCFailNode(c, nodeId, H2Diag_NOT_CALLABLE);
    }
    if (H2TCCollectCallArgInfo(c, nodeId, calleeNode, 0, -1, callArgs, NULL, &argCount) != 0) {
        return -1;
    }
    for (i = 0; i < argCount; i++) {
        if (callArgs[i].spread || callArgs[i].explicitNameEnd > callArgs[i].explicitNameStart) {
            return H2TCFailNode(c, callArgs[i].argNode, H2Diag_ARITY_MISMATCH);
        }
    }
    if (c->types[payloadBase].kind == H2TCType_TUPLE) {
        if (argCount != c->types[payloadBase].fieldCount) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        for (i = 0; i < argCount; i++) {
            int32_t argType;
            int32_t elemType = c->funcParamTypes[c->types[payloadBase].fieldStart + i];
            if (H2TCTypeExprExpected(c, callArgs[i].exprNode, elemType, &argType) != 0) {
                return -1;
            }
            if (!H2TCCanAssign(c, elemType, argType)) {
                return H2TCFailTypeMismatchDetail(
                    c, callArgs[i].exprNode, callArgs[i].exprNode, argType, elemType);
            }
        }
    } else {
        int32_t argType;
        if (argCount != 1u) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        if (H2TCTypeExprExpected(c, callArgs[0].exprNode, payloadType, &argType) != 0) {
            return -1;
        }
        if (!H2TCCanAssign(c, payloadType, argType)) {
            return H2TCFailTypeMismatchDetail(
                c, callArgs[0].exprNode, callArgs[0].exprNode, argType, payloadType);
        }
    }
    *outType = enumType;
    return 1;
}

int H2TCTypeExpr_CALL(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t calleeNode = H2AstFirstChild(c->ast, nodeId);
    int32_t calleeType;
    (void)n;
    if (calleeNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_NOT_CALLABLE);
    }
    if (c->currentFunctionIsCompareHook) {
        return H2TCFailNode(c, nodeId, H2Diag_COMPARISON_HOOK_IMPURE);
    }
    if (c->ast->nodes[calleeNode].kind == H2Ast_FIELD_EXPR) {
        int variantCallRc = H2TCTypeEnumVariantCall(c, nodeId, calleeNode, outType);
        if (variantCallRc != 0) {
            return variantCallRc < 0 ? -1 : 0;
        }
    }
    if (c->ast->nodes[calleeNode].kind == H2Ast_IDENT) {
        const H2AstNode*   callee = &c->ast->nodes[calleeNode];
        H2TCCompilerDiagOp diagOp = H2TCCompilerDiagOpFromName(
            c, callee->dataStart, callee->dataEnd);
        if (diagOp != H2TCCompilerDiagOp_NONE) {
            return H2TCTypeCompilerDiagCall(c, nodeId, callee, diagOp, outType);
        }
        if (H2TCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd)) {
            return H2TCTypeSourceLocationOfCall(c, nodeId, callee, outType);
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_const")) {
            int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t nextArgNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
            int32_t ignoredType;
            if (argNode < 0 || nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, argNode, &ignoredType) != 0) {
                return -1;
            }
            if (c->typeBool < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            *outType = c->typeBool;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t kindType;
            int32_t u8Type;
            if (argNode >= 0) {
                if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = H2AstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    kindType = H2TCFindReflectKindType(c);
                    if (kindType >= 0) {
                        *outType = kindType;
                        return 0;
                    }
                    u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
                    if (u8Type < 0) {
                        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                    }
                    *outType = u8Type;
                    return 0;
                }
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode >= 0) {
                if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = H2AstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    if (c->typeBool < 0) {
                        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                    }
                    *outType = c->typeBool;
                    return 0;
                }
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t strRefType;
            if (argNode >= 0) {
                if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = H2AstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    strRefType = H2TCGetStrRefType(c, callee->start, callee->end);
                    if (strRefType < 0) {
                        return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
                    }
                    *outType = strRefType;
                    return 0;
                }
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t reflectedTypeId;
            if (argNode >= 0) {
                if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = H2AstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    if (H2TCResolveReflectedTypeValueExpr(c, argNode, &reflectedTypeId) != 0) {
                        return H2TCFailNode(c, argNode, H2Diag_TYPE_MISMATCH);
                    }
                    if (c->types[reflectedTypeId].kind != H2TCType_ALIAS) {
                        return H2TCFailNode(c, argNode, H2Diag_REFLECTION_BASE_REQUIRES_ALIAS);
                    }
                    if (H2TCResolveAliasTypeId(c, reflectedTypeId) != 0) {
                        return -1;
                    }
                    if (c->typeType < 0) {
                        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                    }
                    *outType = c->typeType;
                    return 0;
                }
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof")) {
            int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                return -1;
            }
            (void)argType;
            nextArgNode = H2AstNextSibling(c->ast, argNode);
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (c->typeType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            *outType = c->typeType;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")
            || H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice"))
        {
            int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                return -1;
            }
            if (argType != c->typeType) {
                return H2TCFailNode(c, argNode, H2Diag_TYPE_MISMATCH);
            }
            nextArgNode = H2AstNextSibling(c->ast, argNode);
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            *outType = c->typeType;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            int32_t typeArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t lenArgNode = typeArgNode >= 0 ? H2AstNextSibling(c->ast, typeArgNode) : -1;
            int32_t nextArgNode = lenArgNode >= 0 ? H2AstNextSibling(c->ast, lenArgNode) : -1;
            int32_t typeArgType;
            int32_t lenArgType;
            if (typeArgNode < 0 || lenArgNode < 0 || nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, typeArgNode, &typeArgType) != 0
                || H2TCTypeExpr(c, lenArgNode, &lenArgType) != 0)
            {
                return -1;
            }
            if (typeArgType != c->typeType || !H2TCIsIntegerType(c, lenArgType)) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            *outType = c->typeType;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len")) {
            int32_t strArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t strArgType;
            int32_t nextArgNode;
            int32_t intType;
            if (strArgNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, strArgNode, &strArgType) != 0) {
                return -1;
            }
            if (!H2TCTypeSupportsLen(c, strArgType)) {
                return H2TCFailNode(c, strArgNode, H2Diag_TYPE_MISMATCH);
            }
            nextArgNode = H2AstNextSibling(c->ast, strArgNode);
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            intType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
            if (intType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "copy")) {
            int32_t         dstNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t         srcNode = dstNode >= 0 ? H2AstNextSibling(c->ast, dstNode) : -1;
            int32_t         extraNode = srcNode >= 0 ? H2AstNextSibling(c->ast, srcNode) : -1;
            int32_t         dstType;
            int32_t         srcType;
            int32_t         dstElemResolved;
            int32_t         u8Type;
            int32_t         intType;
            H2TCCopySeqInfo dstInfo;
            H2TCCopySeqInfo srcInfo;
            if (dstNode < 0 || srcNode < 0 || extraNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, dstNode, &dstType) != 0 || H2TCTypeExpr(c, srcNode, &srcType) != 0)
            {
                return -1;
            }
            if (H2TCResolveCopySeqInfo(c, dstType, &dstInfo) != 0 || !dstInfo.writable
                || dstInfo.elemType < 0)
            {
                return H2TCFailNode(c, dstNode, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCResolveCopySeqInfo(c, srcType, &srcInfo) != 0 || srcInfo.elemType < 0) {
                return H2TCFailNode(c, srcNode, H2Diag_TYPE_MISMATCH);
            }
            if (dstInfo.isString) {
                if (!srcInfo.isString) {
                    return H2TCFailTypeMismatchDetail(c, srcNode, srcNode, srcType, dstType);
                }
            } else if (srcInfo.isString) {
                u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
                if (u8Type < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                }
                dstElemResolved = H2TCResolveAliasBaseType(c, dstInfo.elemType);
                if (dstElemResolved < 0 || dstElemResolved != u8Type) {
                    return H2TCFailTypeMismatchDetail(c, srcNode, srcNode, srcType, dstType);
                }
            } else if (!H2TCCanAssign(c, dstInfo.elemType, srcInfo.elemType)) {
                return H2TCFailTypeMismatchDetail(c, srcNode, srcNode, srcType, dstType);
            }
            intType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
            if (intType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "cstr")) {
            int32_t strArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t strArgType;
            int32_t nextArgNode;
            int32_t u8Type;
            int32_t u8RefType;
            int32_t wantStrType;
            if (strArgNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, strArgNode, &strArgType) != 0) {
                return -1;
            }
            wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, strArgType)) {
                return H2TCFailTypeMismatchDetail(
                    c, strArgNode, strArgNode, strArgType, wantStrType);
            }
            nextArgNode = H2AstNextSibling(c->ast, strArgNode);
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
            if (u8Type < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            u8RefType = H2TCInternRefType(c, u8Type, 0, callee->start, callee->end);
            if (u8RefType < 0) {
                return -1;
            }
            *outType = u8RefType;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "concat")) {
            int32_t aNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t bNode = aNode >= 0 ? H2AstNextSibling(c->ast, aNode) : -1;
            int32_t nextNode = bNode >= 0 ? H2AstNextSibling(c->ast, bNode) : -1;
            int32_t aType;
            int32_t bType;
            int32_t wantStrType;
            int32_t strPtrType;
            int32_t ctxMemType;
            int32_t allocBaseType = H2TCFindMemAllocatorType(c);
            if (aNode < 0 || bNode < 0 || nextNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (allocBaseType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxMemType) != 0) {
                return -1;
            }
            if (!H2TCCanAssign(c, allocBaseType, ctxMemType)) {
                return H2TCFailNode(c, nodeId, H2Diag_CONTEXT_TYPE_MISMATCH);
            }
            if (H2TCTypeExpr(c, aNode, &aType) != 0 || H2TCTypeExpr(c, bNode, &bType) != 0) {
                return -1;
            }
            wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, aType) || !H2TCCanAssign(c, wantStrType, bType)) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            strPtrType = H2TCGetStrPtrType(c, callee->start, callee->end);
            if (strPtrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            *outType = strPtrType;
            return 0;
        }
        if (H2TCNameEqLiteralOrPkgBuiltin(c, callee->dataStart, callee->dataEnd, "fmt", "builtin"))
        {
            int32_t        outNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t        fmtNode = outNode >= 0 ? H2AstNextSibling(c->ast, outNode) : -1;
            int32_t        argNode;
            int32_t        outBufType;
            int32_t        fmtType;
            int32_t        wantStrType;
            int32_t        strPtrType;
            int32_t        intType;
            int32_t        argNodes[H2TC_MAX_CALL_ARGS];
            int32_t        argTypes[H2TC_MAX_CALL_ARGS];
            uint32_t       argCount = 0;
            uint32_t       i;
            const uint8_t* fmtBytes = NULL;
            uint32_t       fmtLen = 0;
            int            fmtIsConst = 0;
            if (outNode < 0 || fmtNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, outNode, &outBufType) != 0) {
                return -1;
            }
            strPtrType = H2TCGetStrPtrType(c, callee->start, callee->end);
            if (strPtrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, strPtrType, outBufType)) {
                return H2TCFailNode(c, outNode, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCTypeExpr(c, fmtNode, &fmtType) != 0) {
                return -1;
            }
            wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, fmtType)) {
                return H2TCFailNode(c, fmtNode, H2Diag_TYPE_MISMATCH);
            }
            argNode = H2AstNextSibling(c->ast, fmtNode);
            while (argNode >= 0) {
                int32_t argType;
                if (argCount >= H2TC_MAX_CALL_ARGS) {
                    return H2TCFailNode(c, argNode, H2Diag_ARITY_MISMATCH);
                }
                if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                argNodes[argCount] = argNode;
                argTypes[argCount] = argType;
                argCount++;
                argNode = H2AstNextSibling(c->ast, argNode);
            }
            if (H2TCConstStringExpr(c, fmtNode, &fmtBytes, &fmtLen, &fmtIsConst) != 0) {
                return -1;
            }
            if (fmtIsConst) {
                H2FmtToken      tokens[512];
                uint32_t        tokenLen = 0;
                uint32_t        placeholderCount = 0;
                H2FmtParseError parseErr = { 0 };
                if (H2FmtParseBytes(
                        fmtBytes,
                        fmtLen,
                        tokens,
                        (uint32_t)(sizeof(tokens) / sizeof(tokens[0])),
                        &tokenLen,
                        &parseErr)
                    != 0)
                {
                    uint32_t errStart = c->ast->nodes[fmtNode].start;
                    uint32_t errEnd = c->ast->nodes[fmtNode].end;
                    if (parseErr.code == H2FmtParseErr_TOKEN_OVERFLOW) {
                        return H2TCFailNode(c, fmtNode, H2Diag_ARENA_OOM);
                    }
                    if (parseErr.end > parseErr.start && errStart + parseErr.end <= errEnd) {
                        errStart += parseErr.start;
                        errEnd = c->ast->nodes[fmtNode].start + parseErr.end;
                    }
                    return H2TCFailSpan(c, H2Diag_FORMAT_INVALID, errStart, errEnd);
                }
                for (i = 0; i < tokenLen; i++) {
                    if (tokens[i].kind == H2FmtTok_PLACEHOLDER_F
                        || tokens[i].kind == H2FmtTok_PLACEHOLDER_S)
                    {
                        uint32_t errStart = c->ast->nodes[fmtNode].start;
                        uint32_t errEnd = c->ast->nodes[fmtNode].end;
                        if (tokens[i].end > tokens[i].start
                            && errStart + tokens[i].end <= c->ast->nodes[fmtNode].end)
                        {
                            errStart += tokens[i].start;
                            errEnd = c->ast->nodes[fmtNode].start + tokens[i].end;
                        }
                        return H2TCFailSpan(c, H2Diag_FORMAT_INVALID, errStart, errEnd);
                    }
                    if (tokens[i].kind == H2FmtTok_PLACEHOLDER_I
                        || tokens[i].kind == H2FmtTok_PLACEHOLDER_R)
                    {
                        placeholderCount++;
                    }
                }
                if (placeholderCount != argCount) {
                    return H2TCFailNode(c, nodeId, H2Diag_FORMAT_ARG_COUNT_MISMATCH);
                }
                {
                    uint32_t placeholderIndex = 0;
                    for (i = 0; i < tokenLen; i++) {
                        if (tokens[i].kind == H2FmtTok_PLACEHOLDER_I
                            || tokens[i].kind == H2FmtTok_PLACEHOLDER_R)
                        {
                            uint32_t idx = placeholderIndex++;
                            if (idx >= argCount) {
                                return H2TCFailNode(c, nodeId, H2Diag_FORMAT_ARG_COUNT_MISMATCH);
                            }
                            if (tokens[i].kind == H2FmtTok_PLACEHOLDER_I) {
                                if (!H2TCIsIntegerType(c, argTypes[idx])) {
                                    return H2TCFailNode(
                                        c, argNodes[idx], H2Diag_FORMAT_ARG_TYPE_MISMATCH);
                                }
                            } else if (!H2TCTypeSupportsFmtReflectRec(c, argTypes[idx], 0u)) {
                                return H2TCFailNode(
                                    c, argNodes[idx], H2Diag_FORMAT_UNSUPPORTED_TYPE);
                            }
                        }
                    }
                }
            } else {
                for (i = 0; i < argCount; i++) {
                    if (!H2TCTypeSupportsFmtReflectRec(c, argTypes[i], 0u)) {
                        return H2TCFailNode(c, argNodes[i], H2Diag_FORMAT_UNSUPPORTED_TYPE);
                    }
                }
            }
            intType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
            if (intType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (0 && H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "free")) {
            int32_t arg1Node = H2AstNextSibling(c->ast, calleeNode);
            int32_t arg2Node = arg1Node >= 0 ? H2AstNextSibling(c->ast, arg1Node) : -1;
            int32_t arg3Node = arg2Node >= 0 ? H2AstNextSibling(c->ast, arg2Node) : -1;
            int32_t allocArgNode = -1;
            int32_t valueArgNode = -1;
            int32_t valueType;
            int32_t allocType;
            int32_t ctxMemType;
            int32_t allocBaseType = H2TCFindMemAllocatorType(c);
            int32_t allocParamType =
                allocBaseType < 0
                    ? -1
                    : H2TCInternRefType(c, allocBaseType, 1, callee->start, callee->end);
            if (arg1Node < 0 || arg3Node >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (arg2Node >= 0) {
                allocArgNode = arg1Node;
                valueArgNode = arg2Node;
            } else {
                valueArgNode = arg1Node;
            }
            if (allocParamType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            if (allocArgNode >= 0) {
                if (H2TCTypeExpr(c, allocArgNode, &allocType) != 0) {
                    return -1;
                }
                if (!H2TCCanAssign(c, allocParamType, allocType)) {
                    return H2TCFailNode(c, allocArgNode, H2Diag_TYPE_MISMATCH);
                }
            } else {
                if (H2TCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxMemType) != 0) {
                    return -1;
                }
                if (!H2TCCanAssign(c, allocParamType, ctxMemType)) {
                    return H2TCFailNode(c, nodeId, H2Diag_CONTEXT_TYPE_MISMATCH);
                }
            }
            if (H2TCTypeExpr(c, valueArgNode, &valueType) != 0) {
                return -1;
            }
            if (!H2TCTypeIsFreeablePointer(c, valueType)) {
                return H2TCFailNode(c, valueArgNode, H2Diag_TYPE_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "panic")) {
            int32_t msgArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t msgArgType;
            int32_t nextArgNode;
            int32_t wantStrType;
            if (msgArgNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, msgArgNode, &msgArgType) != 0) {
                return -1;
            }
            wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, msgArgType)) {
                return H2TCFailTypeMismatchDetail(
                    c, msgArgNode, msgArgNode, msgArgType, wantStrType);
            }
            nextArgNode = H2AstNextSibling(c->ast, msgArgNode);
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "print")) {
            int32_t msgArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t msgArgType;
            int32_t nextArgNode;
            int32_t logType;
            int32_t wantStrType;
            if (msgArgNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCTypeExpr(c, msgArgNode, &msgArgType) != 0) {
                return -1;
            }
            wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, msgArgType)) {
                return H2TCFailTypeMismatchDetail(
                    c, msgArgNode, msgArgNode, msgArgType, wantStrType);
            }
            nextArgNode = H2AstNextSibling(c->ast, msgArgNode);
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCGetEffectiveContextFieldTypeByLiteral(c, "logger", &logType) != 0) {
                return -1;
            }
            *outType = c->typeVoid;
            return 0;
        }
        {
            H2TCCallArgInfo callArgs[H2TC_MAX_CALL_ARGS];
            uint32_t        argCount = 0;
            int32_t         resolvedFn = -1;
            int32_t         mutRefTempArgNode = -1;
            int             status;
            if (H2TCCollectCallArgInfo(c, nodeId, calleeNode, 0, -1, callArgs, NULL, &argCount)
                != 0)
            {
                return -1;
            }
            status = H2TCResolveCallByName(
                c,
                callee->dataStart,
                callee->dataEnd,
                callArgs,
                argCount,
                0,
                0,
                &resolvedFn,
                &mutRefTempArgNode);
            if (status < 0) {
                return -1;
            }
            if (status == 0) {
                int32_t dependentReturnType = -1;
                int     dependentStatus = 0;
                if (argCount > 0) {
                    dependentStatus = H2TCResolveDependentPtrReturnForCall(
                        c, resolvedFn, callArgs[0].exprNode, &dependentReturnType);
                }
                if (dependentStatus < 0) {
                    return -1;
                }
                if (H2TCValidateCallContextRequirements(c, c->funcs[resolvedFn].contextType) != 0) {
                    return -1;
                }
                if (H2TCRecordCallTarget(c, nodeId, resolvedFn) != 0) {
                    return -1;
                }
                H2TCMarkFunctionUsed(c, resolvedFn);
                *outType =
                    dependentStatus == 1 ? dependentReturnType : c->funcs[resolvedFn].returnType;
                return 0;
            }
            if (status == 2) {
                return H2TCFailSpan(
                    c, H2Diag_NO_MATCHING_OVERLOAD, callee->dataStart, callee->dataEnd);
            }
            if (status == 3) {
                return H2TCFailSpan(c, H2Diag_AMBIGUOUS_CALL, callee->dataStart, callee->dataEnd);
            }
            if (status == 4) {
                return H2TCFailNode(c, mutRefTempArgNode, H2Diag_COMPOUND_MUT_REF_TEMPORARY);
            }
            if (status == 5) {
                return H2TCFailSpan(
                    c, H2Diag_COMPOUND_INFER_AMBIGUOUS, callee->dataStart, callee->dataEnd);
            }
            if (status == 6) {
                return -1;
            }
        }
    } else if (c->ast->nodes[calleeNode].kind == H2Ast_FIELD_EXPR) {
        const H2AstNode* callee = &c->ast->nodes[calleeNode];
        int32_t          recvNode = H2AstFirstChild(c->ast, calleeNode);
        int32_t          recvType;
        int32_t          fieldType;
        if (recvNode >= 0 && (uint32_t)recvNode < c->ast->len
            && c->ast->nodes[recvNode].kind == H2Ast_IDENT)
        {
            const H2AstNode* recv = &c->ast->nodes[recvNode];
            if (H2NameEqLiteral(c->src, recv->dataStart, recv->dataEnd, "builtin")
                && H2TCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd))
            {
                return H2TCTypeSourceLocationOfCall(c, nodeId, callee, outType);
            }
            if (H2NameEqLiteral(c->src, recv->dataStart, recv->dataEnd, "compiler")) {
                H2TCCompilerDiagOp diagOp = H2TCCompilerDiagOpFromName(
                    c, callee->dataStart, callee->dataEnd);
                if (diagOp != H2TCCompilerDiagOp_NONE) {
                    return H2TCTypeCompilerDiagCall(c, nodeId, callee, diagOp, outType);
                }
            }
            if (H2NameEqLiteral(c->src, recv->dataStart, recv->dataEnd, "reflect")
                && H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_const"))
            {
                int32_t argNode = H2AstNextSibling(c->ast, calleeNode);
                int32_t nextArgNode = argNode >= 0 ? H2AstNextSibling(c->ast, argNode) : -1;
                int32_t ignoredType;
                if (argNode < 0 || nextArgNode >= 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
                }
                if (H2TCTypeExpr(c, argNode, &ignoredType) != 0) {
                    return -1;
                }
                if (c->typeBool < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                }
                *outType = c->typeBool;
                return 0;
            }
        }
        if (recvNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_NOT_CALLABLE);
        }
        if (H2TCTypeExpr(c, recvNode, &recvType) != 0) {
            return -1;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t kindType;
            int32_t u8Type;
            if (nextArgNode < 0 && recvType == c->typeType) {
                kindType = H2TCFindReflectKindType(c);
                if (kindType >= 0) {
                    *outType = kindType;
                    return 0;
                }
                u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
                if (u8Type < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                }
                *outType = u8Type;
                return 0;
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            if (nextArgNode < 0 && recvType == c->typeType) {
                if (c->typeBool < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                }
                *outType = c->typeBool;
                return 0;
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t strRefType;
            if (nextArgNode < 0 && recvType == c->typeType) {
                strRefType = H2TCGetStrRefType(c, callee->start, callee->end);
                if (strRefType < 0) {
                    return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
                }
                *outType = strRefType;
                return 0;
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t reflectedTypeId;
            if (nextArgNode < 0 && recvType == c->typeType) {
                if (H2TCResolveReflectedTypeValueExpr(c, recvNode, &reflectedTypeId) != 0) {
                    return H2TCFailNode(c, recvNode, H2Diag_TYPE_MISMATCH);
                }
                if (c->types[reflectedTypeId].kind != H2TCType_ALIAS) {
                    return H2TCFailNode(c, recvNode, H2Diag_REFLECTION_BASE_REQUIRES_ALIAS);
                }
                if (H2TCResolveAliasTypeId(c, reflectedTypeId) != 0) {
                    return -1;
                }
                if (c->typeType < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                }
                *outType = c->typeType;
                return 0;
            }
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len")
            && H2TCTypeSupportsLen(c, recvType))
        {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t intType;
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            intType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
            if (intType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (H2TCFieldLookup(c, recvType, callee->dataStart, callee->dataEnd, &fieldType, NULL) == 0)
        {
            calleeType = fieldType;
            goto typed_call_from_callee_type;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "cstr")) {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t u8Type;
            int32_t u8RefType;
            int32_t wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, recvType)) {
                return H2TCFailNode(c, recvNode, H2Diag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
            if (u8Type < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            u8RefType = H2TCInternRefType(c, u8Type, 0, callee->start, callee->end);
            if (u8RefType < 0) {
                return -1;
            }
            *outType = u8RefType;
            return 0;
        }
        if (0 && H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "free")) {
            int32_t valueArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t nextArgNode = valueArgNode >= 0 ? H2AstNextSibling(c->ast, valueArgNode) : -1;
            int32_t allocBaseType;
            int32_t allocParamType;
            int32_t valueType;
            if (valueArgNode < 0 || nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            allocBaseType = H2TCFindMemAllocatorType(c);
            if (allocBaseType < 0) {
                return H2TCFailNode(c, recvNode, H2Diag_TYPE_MISMATCH);
            }
            allocParamType = H2TCInternRefType(c, allocBaseType, 1, callee->start, callee->end);
            if (allocParamType < 0) {
                return -1;
            }
            if (!H2TCCanAssign(c, allocParamType, recvType)) {
                return H2TCFailNode(c, recvNode, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCTypeExpr(c, valueArgNode, &valueType) != 0) {
                return -1;
            }
            if (!H2TCTypeIsFreeablePointer(c, valueType)) {
                return H2TCFailNode(c, valueArgNode, H2Diag_TYPE_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "panic")) {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, recvType)) {
                return H2TCFailNode(c, recvNode, H2Diag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (H2NameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "print")) {
            int32_t nextArgNode = H2AstNextSibling(c->ast, calleeNode);
            int32_t logType;
            int32_t wantStrType = H2TCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return H2TCFailNode(c, calleeNode, H2Diag_UNKNOWN_TYPE);
            }
            if (!H2TCCanAssign(c, wantStrType, recvType)) {
                return H2TCFailNode(c, recvNode, H2Diag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            if (H2TCGetEffectiveContextFieldTypeByLiteral(c, "logger", &logType) != 0) {
                return -1;
            }
            *outType = c->typeVoid;
            return 0;
        }

        {
            H2TCCallArgInfo callArgs[H2TC_MAX_CALL_ARGS];
            uint32_t        argCount = 0;
            int32_t         resolvedFn = -1;
            int32_t         mutRefTempArgNode = -1;
            int             status;
            int             recvPkgStatus = 0;
            uint32_t        recvPkgStart = 0;
            uint32_t        recvPkgEnd = 0;
            if (H2TCCollectCallArgInfo(
                    c, nodeId, calleeNode, 1, recvNode, callArgs, NULL, &argCount)
                != 0)
            {
                return -1;
            }
            status = H2TCResolveCallByName(
                c,
                callee->dataStart,
                callee->dataEnd,
                callArgs,
                argCount,
                1,
                0,
                &resolvedFn,
                &mutRefTempArgNode);
            if (status == 2 || status == 6) {
                status = H2TCResolveCallByName(
                    c,
                    callee->dataStart,
                    callee->dataEnd,
                    callArgs,
                    argCount,
                    1,
                    1,
                    &resolvedFn,
                    &mutRefTempArgNode);
            }
            recvPkgStatus = H2TCResolveReceiverPkgPrefix(c, recvType, &recvPkgStart, &recvPkgEnd);
            if (recvPkgStatus < 0) {
                return -1;
            }
            if ((status == 1 || status == 2) && recvPkgStatus == 1) {
                int prefixedStatus = H2TCResolveCallByPkgMethod(
                    c,
                    recvPkgStart,
                    recvPkgEnd,
                    callee->dataStart,
                    callee->dataEnd,
                    callArgs,
                    argCount,
                    1,
                    0,
                    &resolvedFn,
                    &mutRefTempArgNode);
                if (prefixedStatus == 2 || prefixedStatus == 6) {
                    prefixedStatus = H2TCResolveCallByPkgMethod(
                        c,
                        recvPkgStart,
                        recvPkgEnd,
                        callee->dataStart,
                        callee->dataEnd,
                        callArgs,
                        argCount,
                        1,
                        1,
                        &resolvedFn,
                        &mutRefTempArgNode);
                }
                if (prefixedStatus == 0) {
                    status = 0;
                } else if (status == 2 && (prefixedStatus == 1 || prefixedStatus == 2)) {
                    status = 2;
                } else if (prefixedStatus != 1) {
                    status = prefixedStatus;
                }
            }
            if (status < 0) {
                return -1;
            }
            if (status == 0) {
                int32_t dependentReturnType = -1;
                int     dependentStatus = 0;
                if (argCount > 0) {
                    dependentStatus = H2TCResolveDependentPtrReturnForCall(
                        c, resolvedFn, callArgs[0].exprNode, &dependentReturnType);
                }
                if (dependentStatus < 0) {
                    return -1;
                }
                if (H2TCValidateCallContextRequirements(c, c->funcs[resolvedFn].contextType) != 0) {
                    return -1;
                }
                if (H2TCRecordCallTarget(c, nodeId, resolvedFn) != 0) {
                    return -1;
                }
                H2TCMarkFunctionUsed(c, resolvedFn);
                *outType =
                    dependentStatus == 1 ? dependentReturnType : c->funcs[resolvedFn].returnType;
                return 0;
            }
            if (status == 2) {
                return H2TCFailSpan(
                    c, H2Diag_NO_MATCHING_OVERLOAD, callee->dataStart, callee->dataEnd);
            }
            if (status == 3) {
                return H2TCFailSpan(c, H2Diag_AMBIGUOUS_CALL, callee->dataStart, callee->dataEnd);
            }
            if (status == 4) {
                return H2TCFailNode(c, mutRefTempArgNode, H2Diag_COMPOUND_MUT_REF_TEMPORARY);
            }
            if (status == 5) {
                return H2TCFailSpan(
                    c, H2Diag_COMPOUND_INFER_AMBIGUOUS, callee->dataStart, callee->dataEnd);
            }
            if (status == 6) {
                return -1;
            }
            return H2TCFailSpan(c, H2Diag_UNKNOWN_SYMBOL, callee->dataStart, callee->dataEnd);
        }
    }
    if (H2TCTypeExpr(c, calleeNode, &calleeType) != 0) {
        return -1;
    }
typed_call_from_callee_type: {
    int32_t          fnReturnType;
    uint32_t         fnParamStart = 0;
    uint32_t         fnParamCount = 0;
    int              fnIsVariadic = 0;
    int32_t          fnIndexForDependent = -1;
    H2TCCallArgInfo  callArgs[H2TC_MAX_CALL_ARGS];
    uint32_t         paramNameStarts[H2TC_MAX_CALL_ARGS];
    uint32_t         paramNameEnds[H2TC_MAX_CALL_ARGS];
    uint8_t          paramFlags[H2TC_MAX_CALL_ARGS];
    uint32_t         callArgCount = 0;
    uint32_t         p;
    int              hasParamNames = 1;
    int              prepStatus;
    H2TCCallMapError mapError;
    H2TCCallBinding  binding;
    if (H2TCGetFunctionTypeSignature(
            c, calleeType, &fnReturnType, &fnParamStart, &fnParamCount, &fnIsVariadic)
        != 0)
    {
        return H2TCFailNode(c, calleeNode, H2Diag_NOT_CALLABLE);
    }
    if (H2TCCollectCallArgInfo(c, nodeId, calleeNode, 0, -1, callArgs, NULL, &callArgCount) != 0) {
        return -1;
    }
    if (!fnIsVariadic && callArgCount != fnParamCount) {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
    if (fnIsVariadic && callArgCount < (fnParamCount > 0 ? (fnParamCount - 1u) : 0u)) {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
    for (p = 0; p < fnParamCount; p++) {
        paramNameStarts[p] = c->funcParamNameStarts[fnParamStart + p];
        paramNameEnds[p] = c->funcParamNameEnds[fnParamStart + p];
        paramFlags[p] = c->funcParamFlags[fnParamStart + p];
    }
    if (calleeType >= 0 && (uint32_t)calleeType < c->typeLen && c->types[calleeType].funcIndex >= 0
        && (uint32_t)c->types[calleeType].funcIndex < c->funcLen)
    {
        const H2TCFunction* fn = &c->funcs[c->types[calleeType].funcIndex];
        fnIndexForDependent = c->types[calleeType].funcIndex;
        if (H2TCRecordCallTarget(c, nodeId, fnIndexForDependent) != 0) {
            return -1;
        }
        if (fn->paramCount == fnParamCount
            && (((fn->flags & H2TCFunctionFlag_VARIADIC) != 0) == (fnIsVariadic != 0)))
        {
            for (p = 0; p < fnParamCount; p++) {
                paramNameStarts[p] = c->funcParamNameStarts[fn->paramTypeStart + p];
                paramNameEnds[p] = c->funcParamNameEnds[fn->paramTypeStart + p];
                paramFlags[p] = c->funcParamFlags[fn->paramTypeStart + p];
            }
        }
    }
    for (p = 1; p < fnParamCount; p++) {
        if (paramNameEnds[p] <= paramNameStarts[p]) {
            hasParamNames = 0;
            break;
        }
    }
    H2TCCallMapErrorClear(&mapError);
    prepStatus = H2TCPrepareCallBinding(
        c,
        callArgs,
        callArgCount,
        paramNameStarts,
        paramNameEnds,
        &c->funcParamTypes[fnParamStart],
        fnParamCount,
        fnIsVariadic,
        hasParamNames,
        0,
        &binding,
        &mapError);
    if (prepStatus != 0) {
        if (prepStatus == 2 && mapError.code != 0) {
            H2TCSetDiagWithArg(
                c->diag,
                mapError.code,
                mapError.start,
                mapError.end,
                mapError.argStart,
                mapError.argEnd);
            return -1;
        }
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
    for (p = 0; p < callArgCount; p++) {
        int32_t argType;
        int32_t paramType;
        int32_t argExprNode = callArgs[p].exprNode;
        paramType = binding.argExpectedTypes[p];
        if (paramType < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        if (H2TCTypeExprExpected(c, argExprNode, paramType, &argType) != 0) {
            return -1;
        }
        if (fnIsVariadic && p == binding.spreadArgIndex
            && binding.variadicParamType == c->typeAnytype)
        {
            int32_t spreadType = H2TCResolveAliasBaseType(c, argType);
            if (spreadType < 0 || (uint32_t)spreadType >= c->typeLen
                || c->types[spreadType].kind != H2TCType_PACK)
            {
                return H2TCFailNode(c, argExprNode, H2Diag_ANYTYPE_SPREAD_REQUIRES_PACK);
            }
        }
        if (H2TCIsMutableRefType(c, paramType) && H2TCExprIsCompoundTemporary(c, argExprNode)) {
            return H2TCFailNode(c, argExprNode, H2Diag_COMPOUND_MUT_REF_TEMPORARY);
        }
        if (!H2TCCanAssign(c, paramType, argType)) {
            if (fnIsVariadic && p >= binding.fixedInputCount) {
                return H2TCFailNode(
                    c,
                    argExprNode,
                    binding.spreadArgIndex == p
                        ? H2Diag_VARIADIC_SPREAD_NON_SLICE
                        : H2Diag_VARIADIC_ARG_TYPE_MISMATCH);
            }
            return H2TCFailTypeMismatchDetail(c, argExprNode, argExprNode, argType, paramType);
        }
    }
    H2TCCallMapErrorClear(&mapError);
    {
        int constStatus = H2TCCheckConstParamArgs(
            c,
            callArgs,
            callArgCount,
            &binding,
            paramNameStarts,
            paramNameEnds,
            paramFlags,
            fnParamCount,
            &mapError);
        if (constStatus < 0) {
            return -1;
        }
        if (constStatus != 0) {
            H2TCSetDiagWithArg(
                c->diag,
                mapError.code,
                mapError.start,
                mapError.end,
                mapError.argStart,
                mapError.argEnd);
            return -1;
        }
    }
    if (fnIndexForDependent >= 0 && binding.fixedCount > 0
        && binding.fixedMappedArgExprNodes[0] >= 0)
    {
        int32_t dependentReturnType = -1;
        int     dependentStatus = H2TCResolveDependentPtrReturnForCall(
            c, fnIndexForDependent, binding.fixedMappedArgExprNodes[0], &dependentReturnType);
        if (dependentStatus < 0) {
            return -1;
        }
        if (dependentStatus == 1) {
            *outType = dependentReturnType;
            return 0;
        }
    }
    *outType = fnReturnType;
    return 0;
}
}

int H2TCTypeExpr_CAST(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t         exprNode = H2AstFirstChild(c->ast, nodeId);
    int32_t         typeNode;
    int32_t         sourceType;
    int32_t         resolvedSourceType;
    int32_t         targetType;
    int32_t         resolvedTargetType;
    const H2TCType* src;
    const H2TCType* dst;
    (void)n;
    if (exprNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    typeNode = H2AstNextSibling(c->ast, exprNode);
    if (typeNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }
    if (H2TCTypeExpr(c, exprNode, &sourceType) != 0) {
        return -1;
    }
    c->allowConstNumericTypeName = 1;
    if (H2TCResolveTypeNode(c, typeNode, &targetType) != 0) {
        c->allowConstNumericTypeName = 0;
        return -1;
    }
    c->allowConstNumericTypeName = 0;
    resolvedSourceType = H2TCResolveAliasBaseType(c, sourceType);
    resolvedTargetType = H2TCResolveAliasBaseType(c, targetType);
    if (resolvedSourceType < 0 || (uint32_t)resolvedSourceType >= c->typeLen
        || resolvedTargetType < 0 || (uint32_t)resolvedTargetType >= c->typeLen)
    {
        return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    }
    src = &c->types[resolvedSourceType];
    dst = &c->types[resolvedTargetType];
    if (src->kind == H2TCType_NULL
        && !(dst->kind == H2TCType_OPTIONAL || H2TCIsRawptrType(c, resolvedTargetType)))
    {
        return H2TCFailInvalidCast(c, nodeId, sourceType, targetType);
    }
    if (H2TCIsRawptrType(c, resolvedTargetType)) {
        if (!(src->kind == H2TCType_NULL || H2TCIsRawptrType(c, resolvedSourceType)
              || src->kind == H2TCType_PTR || src->kind == H2TCType_REF))
        {
            return H2TCFailInvalidCast(c, nodeId, sourceType, targetType);
        }
    } else if (H2TCIsRawptrType(c, resolvedSourceType)) {
        if (!(dst->kind == H2TCType_PTR || dst->kind == H2TCType_REF
              || H2TCIsRawptrType(c, resolvedTargetType)))
        {
            return H2TCFailInvalidCast(c, nodeId, sourceType, targetType);
        }
    }
    *outType = targetType;
    return 0;
}

int H2TCTypeExpr_SIZEOF(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t innerNode = H2AstFirstChild(c->ast, nodeId);
    int32_t innerType;
    if (innerNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    if (n->flags == 1) {
        if (H2TCResolveTypeNode(c, innerNode, &innerType) == 0) {
            if (H2TCTypeContainsVarSizeByValue(c, innerType)) {
                return H2TCFailVarSizeByValue(c, innerNode, innerType, "sizeof(type) operand");
            }
            *outType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
            if (*outType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
            return 0;
        }
        if (c->ast->nodes[innerNode].kind == H2Ast_TYPE_NAME) {
            int32_t localIdx = H2TCLocalFind(
                c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
            if (localIdx >= 0) {
                if (c->diag != NULL) {
                    *c->diag = (H2Diag){ 0 };
                }
                *outType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
                if (*outType < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                }
                return 0;
            }
            {
                int32_t fnIdx = H2TCFindFunctionIndex(
                    c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                if (fnIdx >= 0) {
                    if (c->diag != NULL) {
                        *c->diag = (H2Diag){ 0 };
                    }
                    *outType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
                    if (*outType < 0) {
                        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
                    }
                    return 0;
                }
            }
        }
    } else {
        if (H2TCTypeExpr(c, innerNode, &innerType) != 0) {
            return -1;
        }
    }
    *outType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
    if (*outType < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
    }
    return 0;
}

int H2TCTypeExpr_FIELD_EXPR(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t          recvNode = H2AstFirstChild(c->ast, nodeId);
    int32_t          recvType = -1;
    int32_t          fieldType = -1;
    int32_t          fnIndex;
    int32_t          localIdx;
    const H2AstNode* recv;
    if (recvNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_SYMBOL);
    }
    recv = &c->ast->nodes[recvNode];
    if (((recv->kind == H2Ast_IDENT && H2TCLocalFind(c, recv->dataStart, recv->dataEnd) < 0
          && H2TCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) < 0)
         || recv->kind == H2Ast_FIELD_EXPR)
        && H2TCResolveEnumMemberType(c, recvNode, n->dataStart, n->dataEnd, &fieldType))
    {
        *outType = fieldType;
        return 0;
    }
    localIdx = recv->kind == H2Ast_IDENT ? H2TCLocalFind(c, recv->dataStart, recv->dataEnd) : -1;
    if (localIdx >= 0) {
        const H2TCVariantNarrow* narrow;
        if (H2TCVariantNarrowFind(c, localIdx, &narrow)
            && H2TCEnumVariantPayloadFieldType(
                   c,
                   narrow->enumTypeId,
                   narrow->variantStart,
                   narrow->variantEnd,
                   n->dataStart,
                   n->dataEnd,
                   &fieldType)
                   == 0)
        {
            *outType = fieldType;
            return 0;
        }
    }
    if (recv->kind == H2Ast_IDENT && localIdx < 0
        && H2TCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) < 0)
    {
        fnIndex = H2TCFindPkgQualifiedFunctionValueIndex(
            c, recv->dataStart, recv->dataEnd, n->dataStart, n->dataEnd);
        if (fnIndex >= 0) {
            H2TCMarkFunctionUsed(c, fnIndex);
            *outType = c->funcs[(uint32_t)fnIndex].funcTypeId;
            return 0;
        }
    }
    if (H2TCTypeExpr(c, recvNode, &recvType) != 0) {
        return -1;
    }
    if (H2TCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, NULL) != 0) {
        return H2TCFailSpan(c, H2Diag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
    }
    *outType = fieldType;
    return 0;
}

static int32_t H2TCFindParentNode(const H2Ast* ast, int32_t childNodeId) {
    uint32_t i;
    if (ast == NULL || childNodeId < 0 || (uint32_t)childNodeId >= ast->len) {
        return -1;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t child = ast->nodes[i].firstChild;
        while (child >= 0) {
            if (child == childNodeId) {
                return (int32_t)i;
            }
            child = ast->nodes[child].nextSibling;
        }
    }
    return -1;
}

static int H2TCAllowRuntimeAnyPackIndex(H2TypeCheckCtx* c, int32_t indexNodeId) {
    int32_t          parentNodeId;
    const H2AstNode* parent;
    if (c == NULL || c->ast == NULL) {
        return 0;
    }
    parentNodeId = H2TCFindParentNode(c->ast, indexNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    parent = &c->ast->nodes[parentNodeId];
    if (parent->kind == H2Ast_CAST) {
        return 1;
    }
    if (parent->kind == H2Ast_CALL_ARG) {
        int32_t          callNodeId = H2TCFindParentNode(c->ast, parentNodeId);
        const H2AstNode* callNode;
        int32_t          calleeNodeId;
        const H2AstNode* calleeNode;
        if (callNodeId < 0 || (uint32_t)callNodeId >= c->ast->len) {
            return 0;
        }
        callNode = &c->ast->nodes[callNodeId];
        if (callNode->kind != H2Ast_CALL) {
            return 0;
        }
        calleeNodeId = H2AstFirstChild(c->ast, callNodeId);
        if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= c->ast->len) {
            return 0;
        }
        calleeNode = &c->ast->nodes[calleeNodeId];
        if (calleeNode->kind == H2Ast_IDENT
            && H2NameEqLiteral(c->src, calleeNode->dataStart, calleeNode->dataEnd, "typeof"))
        {
            return 1;
        }
    }
    return 0;
}

int H2TCTypeExpr_INDEX(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t           baseNode = H2AstFirstChild(c->ast, nodeId);
    int32_t           baseType;
    int32_t           resolvedBaseType;
    H2TCIndexBaseInfo info;
    if (baseNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    if (H2TCTypeExpr(c, baseNode, &baseType) != 0) {
        return -1;
    }
    resolvedBaseType = H2TCResolveAliasBaseType(c, baseType);
    if (resolvedBaseType >= 0 && (uint32_t)resolvedBaseType < c->typeLen
        && c->types[resolvedBaseType].kind == H2TCType_PACK)
    {
        int32_t idxNode = H2AstNextSibling(c->ast, baseNode);
        int32_t idxType;
        int64_t idxValue = 0;
        int     idxIsConst = 0;
        if ((n->flags & H2AstFlag_INDEX_SLICE) != 0) {
            return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
        }
        if (idxNode < 0 || H2AstNextSibling(c->ast, idxNode) >= 0) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        if (H2TCTypeExpr(c, idxNode, &idxType) != 0) {
            return -1;
        }
        if (!H2TCIsIntegerType(c, idxType)) {
            return H2TCFailNode(c, idxNode, H2Diag_TYPE_MISMATCH);
        }
        if (H2TCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
            return H2TCFailNode(c, idxNode, H2Diag_TYPE_MISMATCH);
        }
        if (!idxIsConst) {
            if (!H2TCAllowRuntimeAnyPackIndex(c, nodeId)) {
                return H2TCFailNode(c, idxNode, H2Diag_ANYTYPE_PACK_INDEX_NOT_CONST);
            }
            *outType = c->typeAnytype;
            return 0;
        }
        if (idxValue < 0 || (uint64_t)idxValue >= c->types[resolvedBaseType].fieldCount) {
            return H2TCFailNode(c, idxNode, H2Diag_ANYTYPE_PACK_INDEX_OOB);
        }
        *outType = c->funcParamTypes[c->types[resolvedBaseType].fieldStart + (uint32_t)idxValue];
        return 0;
    }
    if (H2TCResolveIndexBaseInfo(c, baseType, &info) != 0 || !info.indexable || info.elemType < 0) {
        if ((n->flags & H2AstFlag_INDEX_SLICE) == 0 && resolvedBaseType >= 0
            && (uint32_t)resolvedBaseType < c->typeLen
            && c->types[resolvedBaseType].kind == H2TCType_FUNCTION)
        {
            return H2TCFailNode(c, nodeId, H2Diag_GENERIC_FN_TYPE_ARGS_FORBIDDEN);
        }
        return H2TCFailNode(c, baseNode, H2Diag_TYPE_MISMATCH);
    }

    if ((n->flags & H2AstFlag_INDEX_SLICE) != 0) {
        int     hasStart = (n->flags & H2AstFlag_INDEX_HAS_START) != 0;
        int     hasEnd = (n->flags & H2AstFlag_INDEX_HAS_END) != 0;
        int32_t child = H2AstNextSibling(c->ast, baseNode);
        int32_t startNode = -1;
        int32_t endNode = -1;
        int32_t sliceType;
        int64_t startValue = 0;
        int64_t endValue = 0;
        int     startIsConst = 0;
        int     endIsConst = 0;

        if (!info.sliceable) {
            return H2TCFailUnsliceableExpr(c, nodeId, baseType);
        }

        if (hasStart) {
            int32_t startType;
            startNode = child;
            if (startNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            if (H2TCTypeExpr(c, startNode, &startType) != 0) {
                return -1;
            }
            if (!H2TCIsIntegerType(c, startType)) {
                return H2TCFailNode(c, startNode, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCConstIntExpr(c, startNode, &startValue, &startIsConst) != 0) {
                return H2TCFailNode(c, startNode, H2Diag_TYPE_MISMATCH);
            }
            child = H2AstNextSibling(c->ast, child);
        }
        if (hasEnd) {
            int32_t endType;
            endNode = child;
            if (endNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            if (H2TCTypeExpr(c, endNode, &endType) != 0) {
                return -1;
            }
            if (!H2TCIsIntegerType(c, endType)) {
                return H2TCFailNode(c, endNode, H2Diag_TYPE_MISMATCH);
            }
            if (H2TCConstIntExpr(c, endNode, &endValue, &endIsConst) != 0) {
                return H2TCFailNode(c, endNode, H2Diag_TYPE_MISMATCH);
            }
            child = H2AstNextSibling(c->ast, child);
        }
        if (child >= 0) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }

        if ((startIsConst && startValue < 0) || (endIsConst && endValue < 0)) {
            return H2TCFailNode(c, nodeId, H2Diag_SLICE_RANGE_OUT_OF_BOUNDS);
        }

        if (info.hasKnownLen) {
            int     startKnown = !hasStart || startIsConst;
            int     endKnown = !hasEnd || endIsConst;
            int64_t startBound = hasStart ? startValue : 0;
            int64_t endBound = hasEnd ? endValue : (int64_t)info.knownLen;
            if (startKnown && endKnown) {
                if (startBound > (int64_t)info.knownLen || endBound > (int64_t)info.knownLen) {
                    return H2TCFailNode(c, nodeId, H2Diag_SLICE_RANGE_OUT_OF_BOUNDS);
                }
                if (startBound > endBound) {
                    return H2TCFailNode(c, nodeId, H2Diag_SLICE_RANGE_INVALID);
                }
            } else {
                H2TCMarkRuntimeBoundsCheck(c, nodeId);
            }
        } else {
            if (startIsConst && endIsConst && startValue > endValue) {
                return H2TCFailNode(c, nodeId, H2Diag_SLICE_RANGE_INVALID);
            }
            H2TCMarkRuntimeBoundsCheck(c, nodeId);
        }

        if (info.isStringLike) {
            *outType = H2TCGetStringSliceExprType(c, baseType, n->start, n->end);
            if (*outType < 0) {
                return -1;
            }
        } else {
            sliceType = H2TCInternSliceType(c, info.elemType, info.sliceMutable, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            if (info.sliceMutable) {
                *outType = H2TCInternPtrType(c, sliceType, n->start, n->end);
            } else {
                *outType = H2TCInternRefType(c, sliceType, 0, n->start, n->end);
            }
            if (*outType < 0) {
                return -1;
            }
        }
        return 0;
    }

    {
        int32_t idxNode = H2AstNextSibling(c->ast, baseNode);
        int32_t idxType;
        int64_t idxValue = 0;
        int     idxIsConst = 0;

        if (idxNode < 0 || H2AstNextSibling(c->ast, idxNode) >= 0) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        if (H2TCTypeExpr(c, idxNode, &idxType) != 0) {
            return -1;
        }
        if (!H2TCIsIntegerType(c, idxType)) {
            return H2TCFailNode(c, idxNode, H2Diag_TYPE_MISMATCH);
        }
        if (H2TCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
            return H2TCFailNode(c, idxNode, H2Diag_TYPE_MISMATCH);
        }
        if (idxIsConst && idxValue < 0) {
            return H2TCFailNode(c, idxNode, H2Diag_INDEX_OUT_OF_BOUNDS);
        }

        if (info.hasKnownLen) {
            if (idxIsConst) {
                if (idxValue >= (int64_t)info.knownLen) {
                    return H2TCFailNode(c, idxNode, H2Diag_INDEX_OUT_OF_BOUNDS);
                }
            } else {
                H2TCMarkRuntimeBoundsCheck(c, nodeId);
            }
        } else if (info.sliceable) {
            H2TCMarkRuntimeBoundsCheck(c, nodeId);
        }
    }

    *outType = info.elemType;
    return 0;
}

int H2TCTypeExpr_UNARY(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t rhsNode = H2AstFirstChild(c->ast, nodeId);
    int32_t rhsType;
    if (rhsNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    if (H2TCTypeExpr(c, rhsNode, &rhsType) != 0) {
        return -1;
    }
    switch ((H2TokenKind)n->op) {
        case H2Tok_ADD:
        case H2Tok_SUB:
            if (!H2TCIsNumericType(c, rhsType)) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            *outType = rhsType;
            return 0;
        case H2Tok_NOT:
            if (!H2TCIsBoolType(c, rhsType)) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_BOOL);
            }
            *outType = c->typeBool;
            return 0;
        case H2Tok_MUL:
            if (c->types[rhsType].kind != H2TCType_PTR && c->types[rhsType].kind != H2TCType_REF) {
                return H2TCFailUnaryOpTypeMismatch(c, nodeId, rhsNode, "dereference", rhsType);
            }
            *outType = c->types[rhsType].baseType;
            return 0;
        case H2Tok_AND: {
            int32_t ptrType = H2TCInternPtrType(c, rhsType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        default: return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    }
}

int H2TCTypeExpr_BINARY(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t     lhsNode = H2AstFirstChild(c->ast, nodeId);
    int32_t     rhsNode;
    int32_t     lhsType;
    int32_t     rhsType;
    int32_t     commonType;
    int32_t     hookFn = -1;
    H2TokenKind op = (H2TokenKind)n->op;
    if (lhsNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    rhsNode = H2AstNextSibling(c->ast, lhsNode);
    if (rhsNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    if (op == H2Tok_ASSIGN && c->ast->nodes[lhsNode].kind == H2Ast_IDENT
        && H2NameEqLiteral(
            c->src, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, "_"))
    {
        if (H2TCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
            if (H2TCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                return -1;
            }
        }
        if (rhsType == c->typeVoid) {
            return H2TCFailNode(c, rhsNode, H2Diag_TYPE_MISMATCH);
        }
        *outType = rhsType;
        return 0;
    }
    if (op == H2Tok_ADD && H2IsStringLiteralConcatChain(c->ast, nodeId)) {
        int32_t strRefType = H2TCGetStrRefType(c, n->start, n->end);
        if (strRefType < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
        }
        *outType = strRefType;
        return 0;
    }
    if (op == H2Tok_ASSIGN || op == H2Tok_ADD_ASSIGN || op == H2Tok_SUB_ASSIGN
        || op == H2Tok_MUL_ASSIGN || op == H2Tok_DIV_ASSIGN || op == H2Tok_MOD_ASSIGN
        || op == H2Tok_AND_ASSIGN || op == H2Tok_OR_ASSIGN || op == H2Tok_XOR_ASSIGN
        || op == H2Tok_LSHIFT_ASSIGN || op == H2Tok_RSHIFT_ASSIGN)
    {
        int skipDirectIdentRead = op == H2Tok_ASSIGN;
        if (H2TCTypeAssignTargetExpr(c, lhsNode, skipDirectIdentRead, &lhsType) != 0
            || H2TCTypeExprExpected(c, rhsNode, lhsType, &rhsType) != 0)
        {
            return -1;
        }
        if (!H2TCExprIsAssignable(c, lhsNode)) {
            return H2TCFailAssignTargetNotAssignable(c, lhsNode);
        }
        if (H2TCExprIsConstAssignTarget(c, lhsNode)) {
            return H2TCFailAssignToConst(c, lhsNode);
        }
        if (!H2TCCanAssign(c, lhsType, rhsType)) {
            return H2TCFailTypeMismatchDetail(c, lhsNode, rhsNode, rhsType, lhsType);
        }
        if (op != H2Tok_ASSIGN && !H2TCIsNumericType(c, lhsType)) {
            return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
        }
        H2TCMarkDirectIdentLocalWrite(c, lhsNode, op == H2Tok_ASSIGN);
        *outType = lhsType;
        return 0;
    }
    if (H2TCTypeExpr(c, lhsNode, &lhsType) != 0
        || H2TCTypeExprExpected(c, rhsNode, lhsType, &rhsType) != 0)
    {
        return -1;
    }

    if (op == H2Tok_LOGICAL_AND || op == H2Tok_LOGICAL_OR) {
        if (!H2TCIsBoolType(c, lhsType) || !H2TCIsBoolType(c, rhsType)) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_BOOL);
        }
        *outType = c->typeBool;
        return 0;
    }

    /* Allow ?T == null, null == ?T, rawptr == null, null == rawptr, and != variants. */
    if (op == H2Tok_EQ || op == H2Tok_NEQ) {
        int lhsIsOpt = c->types[lhsType].kind == H2TCType_OPTIONAL;
        int rhsIsOpt = c->types[rhsType].kind == H2TCType_OPTIONAL;
        int lhsIsNull = c->types[lhsType].kind == H2TCType_NULL;
        int rhsIsNull = c->types[rhsType].kind == H2TCType_NULL;
        int lhsIsRawptr = H2TCIsRawptrType(c, lhsType);
        int rhsIsRawptr = H2TCIsRawptrType(c, rhsType);
        if ((lhsIsOpt && rhsIsNull) || (lhsIsNull && rhsIsOpt) || (lhsIsRawptr && rhsIsNull)
            || (lhsIsNull && rhsIsRawptr))
        {
            *outType = c->typeBool;
            return 0;
        }
    }

    if (op == H2Tok_EQ || op == H2Tok_NEQ || op == H2Tok_LT || op == H2Tok_GT || op == H2Tok_LTE
        || op == H2Tok_GTE)
    {
        int hookStatus = H2TCResolveComparisonHook(
            c,
            (op == H2Tok_EQ || op == H2Tok_NEQ) ? "__equal" : "__order",
            lhsType,
            rhsType,
            &hookFn);
        if (hookStatus == 0) {
            H2TCMarkFunctionUsed(c, hookFn);
            *outType = c->typeBool;
            return 0;
        }
        if (hookStatus == 3) {
            return H2TCFailNode(c, nodeId, H2Diag_AMBIGUOUS_CALL);
        }
        if (H2TCCoerceForBinary(c, lhsType, rhsType, &commonType) != 0) {
            return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
        }
        if (op == H2Tok_EQ || op == H2Tok_NEQ) {
            if (!H2TCIsComparableType(c, commonType)) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_NOT_COMPARABLE);
            }
        } else {
            if (!H2TCIsOrderedType(c, commonType)) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_NOT_ORDERED);
            }
        }
        *outType = c->typeBool;
        return 0;
    }

    if (op == H2Tok_ADD && (H2TCIsStringLikeType(c, lhsType) || H2TCIsStringLikeType(c, rhsType))) {
        return H2TCFailNode(c, nodeId, H2Diag_STRING_CONCAT_LITERAL_ONLY);
    }

    if (H2TCCoerceForBinary(c, lhsType, rhsType, &commonType) != 0) {
        return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    }

    if (!H2TCIsNumericType(c, commonType)) {
        return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
    }
    *outType = commonType;
    return 0;
}

int H2TCTypeExpr_NULL(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeNull;
    return 0;
}

int H2TCTypeExpr_UNWRAP(H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t inner = H2AstFirstChild(c->ast, nodeId);
    int32_t innerType;
    (void)n;
    if (inner < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    if (H2TCTypeExpr(c, inner, &innerType) != 0) {
        return -1;
    }
    if (c->types[innerType].kind != H2TCType_OPTIONAL) {
        return H2TCFailUnaryOpTypeMismatch(c, nodeId, inner, "unwrap", innerType);
    }
    *outType = c->types[innerType].baseType;
    return 0;
}

int H2TCTypeExpr_TUPLE_EXPR(
    H2TypeCheckCtx* c, int32_t nodeId, const H2AstNode* n, int32_t* outType) {
    int32_t  child = H2AstFirstChild(c->ast, nodeId);
    uint32_t elemCount = 0;
    (void)n;
    while (child >= 0) {
        int32_t elemType;
        if (elemCount >= c->scratchParamCap) {
            return H2TCFailNode(c, child, H2Diag_ARENA_OOM);
        }
        if (H2TCTypeExpr(c, child, &elemType) != 0) {
            return -1;
        }
        if (elemType == c->typeNull) {
            return H2TCFailNode(c, child, H2Diag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (elemType == c->typeVoid) {
            return H2TCFailNode(c, child, H2Diag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (H2TCConcretizeInferredType(c, elemType, &elemType) != 0) {
            return -1;
        }
        c->scratchParamTypes[elemCount++] = elemType;
        child = H2AstNextSibling(c->ast, child);
    }
    if (elemCount < 2u) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    *outType = H2TCInternTupleType(c, c->scratchParamTypes, elemCount, n->start, n->end);
    return *outType < 0 ? -1 : 0;
}

int H2TCTypeExpr(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailSpan(c, H2Diag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case H2Ast_IDENT:             return H2TCTypeExpr_IDENT(c, nodeId, n, outType);
        case H2Ast_TYPE_VALUE:        return H2TCTypeExpr_TYPE_VALUE(c, nodeId, n, outType);
        case H2Ast_INT:               return H2TCTypeExpr_INT(c, nodeId, n, outType);
        case H2Ast_FLOAT:             return H2TCTypeExpr_FLOAT(c, nodeId, n, outType);
        case H2Ast_STRING:            return H2TCTypeExpr_STRING(c, nodeId, n, outType);
        case H2Ast_RUNE:              return H2TCTypeExpr_RUNE(c, nodeId, n, outType);
        case H2Ast_BOOL:              return H2TCTypeExpr_BOOL(c, nodeId, n, outType);
        case H2Ast_COMPOUND_LIT:      return H2TCTypeExpr_COMPOUND_LIT(c, nodeId, n, outType);
        case H2Ast_ARRAY_LIT:         return H2TCTypeArrayLit(c, nodeId, -1, outType);
        case H2Ast_CALL_WITH_CONTEXT: return H2TCTypeExpr_CALL_WITH_CONTEXT(c, nodeId, n, outType);
        case H2Ast_NEW:               return H2TCTypeExpr_NEW(c, nodeId, n, outType);
        case H2Ast_CALL:              return H2TCTypeExpr_CALL(c, nodeId, n, outType);
        case H2Ast_CALL_ARG:          {
            int32_t inner = H2AstFirstChild(c->ast, nodeId);
            if (inner < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            return H2TCTypeExpr(c, inner, outType);
        }
        case H2Ast_CAST:       return H2TCTypeExpr_CAST(c, nodeId, n, outType);
        case H2Ast_SIZEOF:     return H2TCTypeExpr_SIZEOF(c, nodeId, n, outType);
        case H2Ast_FIELD_EXPR: return H2TCTypeExpr_FIELD_EXPR(c, nodeId, n, outType);
        case H2Ast_INDEX:      return H2TCTypeExpr_INDEX(c, nodeId, n, outType);
        case H2Ast_UNARY:      return H2TCTypeExpr_UNARY(c, nodeId, n, outType);
        case H2Ast_BINARY:     return H2TCTypeExpr_BINARY(c, nodeId, n, outType);
        case H2Ast_NULL:       return H2TCTypeExpr_NULL(c, nodeId, n, outType);
        case H2Ast_UNWRAP:     return H2TCTypeExpr_UNWRAP(c, nodeId, n, outType);
        case H2Ast_TUPLE_EXPR: return H2TCTypeExpr_TUPLE_EXPR(c, nodeId, n, outType);
        default:               return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
}

int H2TCValidateConstInitializerExprNode(H2TypeCheckCtx* c, int32_t initNode) {
    H2TCConstEvalCtx evalCtx;
    H2CTFEValue      value;
    int              isConst = 0;
    int              rc;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    evalCtx.rootCallOwnerFnIndex = -1;
    H2TCClearLastConstEvalReason(c);
    if (H2TCEvalConstExprNode(&evalCtx, initNode, &value, &isConst) != 0) {
        return -1;
    }
    H2TCStoreLastConstEvalReason(c, &evalCtx);
    if (isConst) {
        return 0;
    }
    if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd
        && c->lastConstEvalReasonEnd <= c->src.len)
    {
        rc = H2TCFailSpan(
            c,
            H2Diag_CONST_INIT_CONST_REQUIRED,
            c->lastConstEvalReasonStart,
            c->lastConstEvalReasonEnd);
    } else {
        rc = H2TCFailNode(c, initNode, H2Diag_CONST_INIT_CONST_REQUIRED);
    }
    H2TCAttachConstEvalReason(c);
    return rc;
}

static int H2TCValidateLocalConstFunctionInitializerExprNode(H2TypeCheckCtx* c, int32_t initNode) {
    const H2AstNode* init;
    int32_t          initType = -1;
    if (c == NULL || initNode < 0 || (uint32_t)initNode >= c->ast->len) {
        return 0;
    }
    init = &c->ast->nodes[initNode];
    if (init->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, initNode);
        return H2TCValidateLocalConstFunctionInitializerExprNode(c, inner);
    }
    if (init->kind != H2Ast_IDENT && init->kind != H2Ast_FIELD_EXPR) {
        return 0;
    }
    if (H2TCTypeExpr(c, initNode, &initType) != 0) {
        return 0;
    }
    return initType >= 0 && (uint32_t)initType < c->typeLen
        && c->types[initType].kind == H2TCType_FUNCTION;
}

int H2TCValidateLocalConstVarLikeInitializers(
    H2TypeCheckCtx* c, int32_t nodeId, const H2TCVarLikeParts* parts) {
    uint32_t i;
    if (parts == NULL || parts->initNode < 0) {
        return 0;
    }
    if (!parts->grouped) {
        int32_t initNode = H2TCVarLikeInitExprNode(c, nodeId);
        if (initNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        if (H2TCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
            return 0;
        }
        return H2TCValidateConstInitializerExprNode(c, initNode);
    }
    if ((uint32_t)parts->initNode >= c->ast->len
        || c->ast->nodes[parts->initNode].kind != H2Ast_EXPR_LIST)
    {
        return H2TCFailNode(c, parts->initNode, H2Diag_EXPECTED_EXPR);
    }
    {
        uint32_t initCount = H2TCListCount(c->ast, parts->initNode);
        if (initCount == parts->nameCount) {
            for (i = 0; i < initCount; i++) {
                int32_t initNode = H2TCListItemAt(c->ast, parts->initNode, i);
                if (initNode < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
                }
                if (H2TCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
                    continue;
                }
                if (H2TCValidateConstInitializerExprNode(c, initNode) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        if (initCount == 1u) {
            int32_t initNode = H2TCListItemAt(c->ast, parts->initNode, 0);
            if (initNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            if (H2TCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
                return 0;
            }
            return H2TCValidateConstInitializerExprNode(c, initNode);
        }
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
}

int H2TCTypeVarLike(H2TypeCheckCtx* c, int32_t nodeId) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
    H2TCVarLikeParts parts;
    int32_t          declType;
    uint32_t         i;
    int              isConstBinding;

    if (H2TCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }
    isConstBinding = n->kind == H2Ast_CONST;
    if (isConstBinding && parts.initNode < 0) {
        return H2TCFailNode(c, nodeId, H2Diag_CONST_MISSING_INITIALIZER);
    }
    if (isConstBinding && H2TCValidateLocalConstVarLikeInitializers(c, nodeId, &parts) != 0) {
        return -1;
    }

    if (!parts.grouped) {
        if (parts.typeNode < 0) {
            int32_t initType;
            if (H2TCTypeExpr(c, parts.initNode, &initType) != 0) {
                return -1;
            }
            if (initType == c->typeNull) {
                return H2TCFailNode(c, parts.initNode, H2Diag_INFER_NULL_TYPE_UNKNOWN);
            }
            if (initType == c->typeVoid) {
                return H2TCFailNode(c, parts.initNode, H2Diag_INFER_VOID_TYPE_UNKNOWN);
            }
            if (isConstBinding) {
                declType = initType;
            } else {
                if (H2TCConcretizeInferredType(c, initType, &declType) != 0) {
                    return -1;
                }
            }
            if (H2TCTypeContainsVarSizeByValue(c, declType)) {
                return H2TCFailVarSizeByValue(c, parts.initNode, declType, "variable position");
            }
            if (H2TCLocalAdd(
                    c,
                    n->dataStart,
                    n->dataEnd,
                    declType,
                    n->kind == H2Ast_CONST,
                    n->kind == H2Ast_CONST ? parts.initNode : -1)
                != 0)
            {
                return -1;
            }
            H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
            return 0;
        }

        c->allowConstNumericTypeName = n->kind == H2Ast_CONST ? 1u : 0u;
        if (H2TCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
            c->allowConstNumericTypeName = 0;
            return -1;
        }
        c->allowConstNumericTypeName = 0;
        if (H2TCTypeContainsVarSizeByValue(c, declType)) {
            return H2TCFailVarSizeByValue(c, parts.typeNode, declType, "variable position");
        }

        if (n->kind == H2Ast_CONST && parts.initNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_CONST_MISSING_INITIALIZER);
        }
        if (n->kind == H2Ast_VAR && parts.initNode < 0 && !H2TCEnumTypeHasTagZero(c, declType)) {
            char        typeBuf[H2TC_DIAG_TEXT_CAP];
            char        detailBuf[256];
            H2TCTextBuf typeText;
            H2TCTextBuf detailText;
            H2TCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
            H2TCFormatTypeRec(c, declType, &typeText, 0);
            H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
            H2TCTextBufAppendCStr(&detailText, "enum type ");
            H2TCTextBufAppendCStr(&detailText, typeBuf);
            H2TCTextBufAppendCStr(&detailText, " has no zero-valued tag; initializer required");
            return H2TCFailDiagText(c, nodeId, H2Diag_ENUM_ZERO_INIT_REQUIRES_ZERO_TAG, detailBuf);
        }
        if (parts.initNode >= 0) {
            int32_t initType;
            if (H2TCTypeExprExpected(c, parts.initNode, declType, &initType) != 0) {
                return -1;
            }
            if (!H2TCCanAssign(c, declType, initType)) {
                return H2TCFailTypeMismatchDetail(
                    c, parts.initNode, parts.initNode, initType, declType);
            }
        }

        if (H2TCLocalAdd(
                c,
                n->dataStart,
                n->dataEnd,
                declType,
                n->kind == H2Ast_CONST,
                n->kind == H2Ast_CONST ? parts.initNode : -1)
            != 0)
        {
            return -1;
        }
        if (parts.initNode >= 0) {
            H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
        }
        return 0;
    }

    if (parts.typeNode >= 0) {
        c->allowConstNumericTypeName = n->kind == H2Ast_CONST ? 1u : 0u;
        if (H2TCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
            c->allowConstNumericTypeName = 0;
            return -1;
        }
        c->allowConstNumericTypeName = 0;
        if (H2TCTypeContainsVarSizeByValue(c, declType)) {
            return H2TCFailVarSizeByValue(c, parts.typeNode, declType, "variable position");
        }
        if (n->kind == H2Ast_CONST && parts.initNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_CONST_MISSING_INITIALIZER);
        }
        if (n->kind == H2Ast_VAR && parts.initNode < 0 && !H2TCEnumTypeHasTagZero(c, declType)) {
            char        typeBuf[H2TC_DIAG_TEXT_CAP];
            char        detailBuf[256];
            H2TCTextBuf typeText;
            H2TCTextBuf detailText;
            H2TCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
            H2TCFormatTypeRec(c, declType, &typeText, 0);
            H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
            H2TCTextBufAppendCStr(&detailText, "enum type ");
            H2TCTextBufAppendCStr(&detailText, typeBuf);
            H2TCTextBufAppendCStr(&detailText, " has no zero-valued tag; initializer required");
            return H2TCFailDiagText(c, nodeId, H2Diag_ENUM_ZERO_INIT_REQUIRES_ZERO_TAG, detailBuf);
        }
        if (parts.initNode >= 0) {
            uint32_t initCount;
            if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != H2Ast_EXPR_LIST) {
                return H2TCFailNode(c, parts.initNode, H2Diag_EXPECTED_EXPR);
            }
            initCount = H2TCListCount(c->ast, parts.initNode);
            if (initCount == parts.nameCount) {
                for (i = 0; i < initCount; i++) {
                    int32_t initNode = H2TCListItemAt(c->ast, parts.initNode, i);
                    int32_t initType;
                    if (initNode < 0) {
                        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
                    }
                    if (H2TCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                        return -1;
                    }
                    if (!H2TCCanAssign(c, declType, initType)) {
                        return H2TCFailTypeMismatchDetail(
                            c, initNode, initNode, initType, declType);
                    }
                }
            } else if (initCount == 1u) {
                int32_t         initNode = H2TCListItemAt(c->ast, parts.initNode, 0);
                int32_t         initType;
                const H2TCType* t;
                if (initNode < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
                }
                if (H2TCTypeExpr(c, initNode, &initType) != 0) {
                    return -1;
                }
                if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                    return H2TCFailNode(c, initNode, H2Diag_TYPE_MISMATCH);
                }
                t = &c->types[initType];
                if (t->kind != H2TCType_TUPLE || t->fieldCount != parts.nameCount) {
                    return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t elemType = c->funcParamTypes[t->fieldStart + i];
                    if (!H2TCCanAssign(c, declType, elemType)) {
                        return H2TCFailTypeMismatchDetail(
                            c, initNode, initNode, elemType, declType);
                    }
                }
            } else {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
        }
        for (i = 0; i < parts.nameCount; i++) {
            int32_t          nameNode = H2TCListItemAt(c->ast, parts.nameListNode, i);
            const H2AstNode* name;
            if (nameNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            name = &c->ast->nodes[nameNode];
            if (!H2NameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                if (H2TCLocalAdd(
                        c, name->dataStart, name->dataEnd, declType, n->kind == H2Ast_CONST, -1)
                    != 0)
                {
                    return -1;
                }
                if (parts.initNode >= 0) {
                    H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        }
        return 0;
    }

    if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != H2Ast_EXPR_LIST) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    {
        uint32_t initCount = H2TCListCount(c->ast, parts.initNode);
        if (initCount == parts.nameCount) {
            for (i = 0; i < parts.nameCount; i++) {
                int32_t          nameNode = H2TCListItemAt(c->ast, parts.nameListNode, i);
                const H2AstNode* name;
                int32_t          initNode = H2TCListItemAt(c->ast, parts.initNode, i);
                int32_t          initType;
                int32_t          inferredType;
                if (nameNode < 0 || initNode < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
                }
                if (H2TCTypeExpr(c, initNode, &initType) != 0) {
                    return -1;
                }
                if (initType == c->typeNull) {
                    return H2TCFailNode(c, initNode, H2Diag_INFER_NULL_TYPE_UNKNOWN);
                }
                if (initType == c->typeVoid) {
                    return H2TCFailNode(c, initNode, H2Diag_INFER_VOID_TYPE_UNKNOWN);
                }
                if (isConstBinding) {
                    inferredType = initType;
                } else {
                    if (H2TCConcretizeInferredType(c, initType, &inferredType) != 0) {
                        return -1;
                    }
                }
                if (H2TCTypeContainsVarSizeByValue(c, inferredType)) {
                    return H2TCFailVarSizeByValue(c, initNode, inferredType, "variable position");
                }
                name = &c->ast->nodes[nameNode];
                if (!H2NameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                    if (H2TCLocalAdd(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            inferredType,
                            n->kind == H2Ast_CONST,
                            n->kind == H2Ast_CONST ? initNode : -1)
                        != 0)
                    {
                        return -1;
                    }
                    H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        } else if (initCount == 1u) {
            int32_t         initNode = H2TCListItemAt(c->ast, parts.initNode, 0);
            int32_t         initType;
            const H2TCType* t;
            if (initNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            if (H2TCTypeExpr(c, initNode, &initType) != 0) {
                return -1;
            }
            if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                return H2TCFailNode(c, initNode, H2Diag_TYPE_MISMATCH);
            }
            t = &c->types[initType];
            if (t->kind != H2TCType_TUPLE || t->fieldCount != parts.nameCount) {
                return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
            }
            for (i = 0; i < parts.nameCount; i++) {
                int32_t          nameNode = H2TCListItemAt(c->ast, parts.nameListNode, i);
                const H2AstNode* name;
                int32_t          inferredType = c->funcParamTypes[t->fieldStart + i];
                if (nameNode < 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
                }
                if (!isConstBinding) {
                    if (H2TCConcretizeInferredType(c, inferredType, &inferredType) != 0) {
                        return -1;
                    }
                }
                if (H2TCTypeContainsVarSizeByValue(c, inferredType)) {
                    return H2TCFailVarSizeByValue(c, initNode, inferredType, "variable position");
                }
                name = &c->ast->nodes[nameNode];
                if (!H2NameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                    if (H2TCLocalAdd(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            inferredType,
                            n->kind == H2Ast_CONST,
                            -1)
                        != 0)
                    {
                        return -1;
                    }
                    H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        } else {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
    }
    return 0;
}

int H2TCTypeTopLevelVarLikes(H2TypeCheckCtx* c, H2AstKind wantKind) {
    int32_t child = H2AstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == wantKind) {
            H2TCVarLikeParts parts;
            if (H2TCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
            }
            if (!parts.grouped) {
                int32_t firstChild = H2AstFirstChild(c->ast, child);
                if (firstChild >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[firstChild].kind)) {
                    int32_t initNode = H2AstNextSibling(c->ast, firstChild);
                    int32_t declType;
                    int32_t initType;
                    if (wantKind == H2Ast_CONST && initNode < 0
                        && !H2TCHasForeignImportDirective(c->ast, c->src, child))
                    {
                        return H2TCFailNode(c, child, H2Diag_CONST_MISSING_INITIALIZER);
                    }
                    c->allowConstNumericTypeName = wantKind == H2Ast_CONST ? 1u : 0u;
                    if (H2TCResolveTypeNode(c, firstChild, &declType) != 0) {
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowConstNumericTypeName = 0;
                    if (H2TCTypeContainsVarSizeByValue(c, declType)) {
                        return H2TCFailVarSizeByValue(c, firstChild, declType, "variable position");
                    }
                    if (wantKind == H2Ast_VAR && initNode < 0
                        && H2TCTypeIsTrackedPtrRef(c, declType))
                    {
                        return H2TCFailTopLevelPtrRefMissingInitializer(
                            c, n->start, n->end, n->dataStart, n->dataEnd);
                    }
                    if (initNode >= 0) {
                        if (H2TCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                            return -1;
                        }
                        if (!H2TCCanAssign(c, declType, initType)) {
                            return H2TCFailTypeMismatchDetail(
                                c, initNode, initNode, initType, declType);
                        }
                    }
                } else if (firstChild >= 0) {
                    int32_t initType;
                    int32_t declType;
                    if (H2TCTypeExpr(c, firstChild, &initType) != 0) {
                        return -1;
                    }
                    if (initType == c->typeNull) {
                        return H2TCFailNode(c, firstChild, H2Diag_INFER_NULL_TYPE_UNKNOWN);
                    }
                    if (initType == c->typeVoid) {
                        return H2TCFailNode(c, firstChild, H2Diag_INFER_VOID_TYPE_UNKNOWN);
                    }
                    if (wantKind == H2Ast_CONST) {
                        declType = initType;
                    } else {
                        if (H2TCConcretizeInferredType(c, initType, &declType) != 0) {
                            return -1;
                        }
                    }
                    if (H2TCTypeContainsVarSizeByValue(c, declType)) {
                        return H2TCFailVarSizeByValue(c, firstChild, declType, "variable position");
                    }
                }
                child = H2AstNextSibling(c->ast, child);
                continue;
            }

            if (parts.typeNode >= 0) {
                int32_t declType;
                if (wantKind == H2Ast_CONST && parts.initNode < 0
                    && !H2TCHasForeignImportDirective(c->ast, c->src, child))
                {
                    return H2TCFailNode(c, child, H2Diag_CONST_MISSING_INITIALIZER);
                }
                c->allowConstNumericTypeName = wantKind == H2Ast_CONST ? 1u : 0u;
                if (H2TCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
                    c->allowConstNumericTypeName = 0;
                    return -1;
                }
                c->allowConstNumericTypeName = 0;
                if (H2TCTypeContainsVarSizeByValue(c, declType)) {
                    return H2TCFailVarSizeByValue(c, parts.typeNode, declType, "variable position");
                }
                if (wantKind == H2Ast_VAR && parts.initNode < 0
                    && H2TCTypeIsTrackedPtrRef(c, declType))
                {
                    int32_t  nameNode = H2TCListItemAt(c->ast, parts.nameListNode, 0);
                    uint32_t nameStart = c->ast->nodes[child].dataStart;
                    uint32_t nameEnd = c->ast->nodes[child].dataEnd;
                    if (nameNode >= 0) {
                        nameStart = c->ast->nodes[nameNode].dataStart;
                        nameEnd = c->ast->nodes[nameNode].dataEnd;
                    }
                    return H2TCFailTopLevelPtrRefMissingInitializer(
                        c,
                        c->ast->nodes[child].start,
                        c->ast->nodes[child].end,
                        nameStart,
                        nameEnd);
                }
                if (parts.initNode >= 0) {
                    uint32_t i;
                    uint32_t initCount;
                    if (c->ast->nodes[parts.initNode].kind != H2Ast_EXPR_LIST) {
                        return H2TCFailNode(c, parts.initNode, H2Diag_EXPECTED_EXPR);
                    }
                    initCount = H2TCListCount(c->ast, parts.initNode);
                    if (initCount == parts.nameCount) {
                        for (i = 0; i < initCount; i++) {
                            int32_t initNode = H2TCListItemAt(c->ast, parts.initNode, i);
                            int32_t initType;
                            if (initNode < 0) {
                                return H2TCFailNode(c, child, H2Diag_EXPECTED_EXPR);
                            }
                            if (H2TCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                                return -1;
                            }
                            if (!H2TCCanAssign(c, declType, initType)) {
                                return H2TCFailTypeMismatchDetail(
                                    c, initNode, initNode, initType, declType);
                            }
                        }
                    } else if (initCount == 1u) {
                        int32_t         initNode = H2TCListItemAt(c->ast, parts.initNode, 0);
                        int32_t         initType;
                        const H2TCType* t;
                        if (initNode < 0) {
                            return H2TCFailNode(c, child, H2Diag_EXPECTED_EXPR);
                        }
                        if (H2TCTypeExpr(c, initNode, &initType) != 0) {
                            return -1;
                        }
                        if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                            return H2TCFailNode(c, initNode, H2Diag_TYPE_MISMATCH);
                        }
                        t = &c->types[initType];
                        if (t->kind != H2TCType_TUPLE || t->fieldCount != parts.nameCount) {
                            return H2TCFailNode(c, child, H2Diag_ARITY_MISMATCH);
                        }
                        for (i = 0; i < parts.nameCount; i++) {
                            int32_t elemType = c->funcParamTypes[t->fieldStart + i];
                            if (!H2TCCanAssign(c, declType, elemType)) {
                                return H2TCFailTypeMismatchDetail(
                                    c, initNode, initNode, elemType, declType);
                            }
                        }
                    } else {
                        return H2TCFailNode(c, child, H2Diag_ARITY_MISMATCH);
                    }
                }
            } else {
                uint32_t i;
                if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != H2Ast_EXPR_LIST) {
                    return H2TCFailNode(c, child, H2Diag_EXPECTED_EXPR);
                }
                {
                    uint32_t initCount = H2TCListCount(c->ast, parts.initNode);
                    int      tupleDecompose = 0;
                    if (initCount == parts.nameCount) {
                        tupleDecompose = 1;
                    } else if (initCount == 1u) {
                        int32_t         initNode = H2TCListItemAt(c->ast, parts.initNode, 0);
                        int32_t         initType;
                        const H2TCType* t;
                        if (initNode < 0) {
                            return H2TCFailNode(c, child, H2Diag_EXPECTED_EXPR);
                        }
                        if (H2TCTypeExpr(c, initNode, &initType) != 0) {
                            return -1;
                        }
                        if (initType >= 0 && (uint32_t)initType < c->typeLen) {
                            t = &c->types[initType];
                            tupleDecompose =
                                t->kind == H2TCType_TUPLE && t->fieldCount == parts.nameCount;
                        }
                    }
                    if (!tupleDecompose) {
                        return H2TCFailNode(c, child, H2Diag_ARITY_MISMATCH);
                    }
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t inferredType;
                    if (H2TCTypeTopLevelVarLikeNode(c, child, (int32_t)i, &inferredType) != 0) {
                        return -1;
                    }
                    (void)inferredType;
                }
            }
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCTypeTopLevelConsts(H2TypeCheckCtx* c) {
    return H2TCTypeTopLevelVarLikes(c, H2Ast_CONST);
}

int H2TCTypeTopLevelVars(H2TypeCheckCtx* c) {
    return H2TCTypeTopLevelVarLikes(c, H2Ast_VAR);
}

int H2TCCheckTopLevelConstInitializers(H2TypeCheckCtx* c) {
    int32_t child = H2AstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_CONST) {
            H2TCVarLikeParts parts;
            if (H2TCHasForeignImportDirective(c->ast, c->src, child)) {
                child = H2AstNextSibling(c->ast, child);
                continue;
            }
            if (H2TCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
            }
            if (parts.typeNode >= 0 && parts.initNode < 0
                && !H2TCHasForeignImportDirective(c->ast, c->src, child))
            {
                return H2TCFailNode(c, child, H2Diag_CONST_MISSING_INITIALIZER);
            }
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCValidateTopLevelConstEvaluable(H2TypeCheckCtx* c) {
    H2TCConstEvalCtx evalCtx;
    int32_t          child;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    evalCtx.rootCallOwnerFnIndex = -1;
    child = H2AstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_CONST) {
            H2TCVarLikeParts parts;
            if (H2TCHasForeignImportDirective(c->ast, c->src, child)) {
                child = H2AstNextSibling(c->ast, child);
                continue;
            }
            if (H2TCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
            }
            if (!parts.grouped) {
                int32_t     initNode = H2TCVarLikeInitExprNode(c, child);
                H2CTFEValue value;
                int         isConst = 0;
                if (initNode >= 0 && H2TCValidateLocalConstFunctionInitializerExprNode(c, initNode))
                {
                    child = H2AstNextSibling(c->ast, child);
                    continue;
                }
                H2TCClearLastConstEvalReason(c);
                evalCtx.nonConstReason = NULL;
                evalCtx.nonConstStart = 0;
                evalCtx.nonConstEnd = 0;
                evalCtx.nonConstTraceDepth = 0;
                evalCtx.rootCallOwnerFnIndex = -1;
                evalCtx.rootCallStart = 0;
                evalCtx.fnDepth = 0;
                evalCtx.execCtx = NULL;
                if (H2TCEvalTopLevelConstNode(c, &evalCtx, child, &value, &isConst) != 0) {
                    return -1;
                }
                H2TCStoreLastConstEvalReason(c, &evalCtx);
                if (!isConst) {
                    int rc;
                    if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd
                        && c->lastConstEvalReasonEnd <= c->src.len)
                    {
                        rc = H2TCFailSpan(
                            c,
                            H2Diag_CONST_INIT_CONST_REQUIRED,
                            c->lastConstEvalReasonStart,
                            c->lastConstEvalReasonEnd);
                    } else if (initNode >= 0) {
                        rc = H2TCFailNode(c, initNode, H2Diag_CONST_INIT_CONST_REQUIRED);
                    } else {
                        rc = H2TCFailNode(c, child, H2Diag_CONST_INIT_CONST_REQUIRED);
                    }
                    H2TCAttachConstEvalReason(c);
                    return rc;
                }
            } else {
                uint32_t i;
                if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != H2Ast_EXPR_LIST) {
                    return H2TCFailNode(c, child, H2Diag_EXPECTED_EXPR);
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t     initNode = H2TCVarLikeInitExprNodeAt(c, child, (int32_t)i);
                    H2CTFEValue value;
                    int         isConst = 0;
                    if (initNode < 0) {
                        return H2TCFailNode(c, child, H2Diag_ARITY_MISMATCH);
                    }
                    H2TCClearLastConstEvalReason(c);
                    evalCtx.nonConstReason = NULL;
                    evalCtx.nonConstStart = 0;
                    evalCtx.nonConstEnd = 0;
                    evalCtx.nonConstTraceDepth = 0;
                    evalCtx.rootCallOwnerFnIndex = -1;
                    evalCtx.rootCallStart = 0;
                    evalCtx.fnDepth = 0;
                    evalCtx.execCtx = NULL;
                    if (H2TCEvalConstExprNode(&evalCtx, initNode, &value, &isConst) != 0) {
                        return -1;
                    }
                    H2TCStoreLastConstEvalReason(c, &evalCtx);
                    if (!isConst) {
                        int rc;
                        if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd
                            && c->lastConstEvalReasonEnd <= c->src.len)
                        {
                            rc = H2TCFailSpan(
                                c,
                                H2Diag_CONST_INIT_CONST_REQUIRED,
                                c->lastConstEvalReasonStart,
                                c->lastConstEvalReasonEnd);
                        } else {
                            rc = H2TCFailNode(c, initNode, H2Diag_CONST_INIT_CONST_REQUIRED);
                        }
                        H2TCAttachConstEvalReason(c);
                        return rc;
                    }
                }
            }
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCGetNullNarrow(H2TypeCheckCtx* c, int32_t condNode, int* outIsEq, H2TCNullNarrow* out) {
    const H2AstNode* n;
    int32_t          lhs, rhs, identNode;
    H2TokenKind      op;
    int32_t          localIdx;
    int32_t          typeId;

    if (condNode < 0 || (uint32_t)condNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[condNode];
    if (n->kind != H2Ast_BINARY) {
        return 0;
    }
    op = (H2TokenKind)n->op;
    if (op != H2Tok_EQ && op != H2Tok_NEQ) {
        return 0;
    }
    lhs = H2AstFirstChild(c->ast, condNode);
    rhs = lhs >= 0 ? H2AstNextSibling(c->ast, lhs) : -1;
    if (lhs < 0 || rhs < 0) {
        return 0;
    }
    /* Identify which side is the ident and which is null. */
    if (c->ast->nodes[lhs].kind == H2Ast_IDENT && c->ast->nodes[rhs].kind == H2Ast_NULL) {
        identNode = lhs;
    } else if (c->ast->nodes[rhs].kind == H2Ast_IDENT && c->ast->nodes[lhs].kind == H2Ast_NULL) {
        identNode = rhs;
    } else {
        return 0;
    }
    {
        const H2AstNode* id = &c->ast->nodes[identNode];
        localIdx = H2TCLocalFind(c, id->dataStart, id->dataEnd);
    }
    if (localIdx < 0) {
        return 0;
    }
    typeId = c->locals[localIdx].typeId;
    if (c->types[typeId].kind != H2TCType_OPTIONAL) {
        return 0;
    }
    *outIsEq = (op == H2Tok_EQ);
    out->localIdx = localIdx;
    out->innerType = c->types[typeId].baseType;
    return 1;
}

int H2TCGetOptionalCondNarrow(
    H2TypeCheckCtx* c, int32_t condNode, int* outThenIsSome, H2TCNullNarrow* out) {
    const H2AstNode* n;
    int32_t          localIdx;
    int32_t          typeId;
    int              isEq = 0;
    if (c == NULL || outThenIsSome == NULL || out == NULL || condNode < 0
        || (uint32_t)condNode >= c->ast->len)
    {
        return 0;
    }
    n = &c->ast->nodes[condNode];

    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_NOT) {
        int32_t inner = H2AstFirstChild(c->ast, condNode);
        if (inner < 0) {
            return 0;
        }
        if (!H2TCGetOptionalCondNarrow(c, inner, outThenIsSome, out)) {
            return 0;
        }
        *outThenIsSome = !*outThenIsSome;
        return 1;
    }

    if (n->kind == H2Ast_IDENT) {
        localIdx = H2TCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx < 0) {
            return 0;
        }
        typeId = c->locals[localIdx].typeId;
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen
            || c->types[typeId].kind != H2TCType_OPTIONAL)
        {
            return 0;
        }
        out->localIdx = localIdx;
        out->innerType = c->types[typeId].baseType;
        *outThenIsSome = 1;
        return 1;
    }

    if (H2TCGetNullNarrow(c, condNode, &isEq, out)) {
        *outThenIsSome = isEq ? 0 : 1;
        return 1;
    }

    return 0;
}

H2_API_END
