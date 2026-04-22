#include "internal.h"

HOP_API_BEGIN

int HOPTCValidateMemAllocatorArg(HOPTypeCheckCtx* c, int32_t nodeId, int32_t allocBaseType) {
    const HOPAstNode* n;
    int32_t           allocType;
    int32_t           allocRefType;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    n = &c->ast->nodes[nodeId];

    if (HOPTCTypeExpr(c, nodeId, &allocType) != 0) {
        return -1;
    }
    if (HOPTCCanAssign(c, allocBaseType, allocType)) {
        if (!HOPTCExprIsAssignable(c, nodeId)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }
        return 0;
    }

    allocRefType = HOPTCInternRefType(c, allocBaseType, 1, n->start, n->end);
    if (allocRefType < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }
    if (!HOPTCCanAssign(c, allocRefType, allocType)) {
        return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }
    return 0;
}

int HOPTCTypeNewExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const HOPAstNode* n;
    int32_t           typeNode;
    int32_t           nextNode;
    int32_t           countArgNode = -1;
    int32_t           initArgNode = -1;
    int32_t           allocArgNode = -1;
    int32_t           allocBaseType;
    int32_t           elemType;
    int32_t           resultType;
    int32_t           countType;
    int32_t           ctxMemType;
    int64_t           countValue = 0;
    int               countIsConst = 0;
    int               hasCount;
    int               hasInit;
    int               hasAlloc;

    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return HOPTCFailSpan(c, HOPDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    hasCount = (n->flags & HOPAstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & HOPAstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & HOPAstFlag_NEW_HAS_ALLOC) != 0;

    typeNode = HOPAstFirstChild(c->ast, nodeId);
    if (typeNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
    }
    nextNode = HOPAstNextSibling(c->ast, typeNode);
    if (hasCount) {
        countArgNode = nextNode;
        if (countArgNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        nextNode = HOPAstNextSibling(c->ast, countArgNode);
    }
    if (hasInit) {
        initArgNode = nextNode;
        if (initArgNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        nextNode = HOPAstNextSibling(c->ast, initArgNode);
    }
    if (hasAlloc) {
        allocArgNode = nextNode;
        if (allocArgNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        nextNode = HOPAstNextSibling(c->ast, allocArgNode);
    }
    if (nextNode >= 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }

    allocBaseType = HOPTCFindMemAllocatorType(c);
    if (allocBaseType < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }

    if (allocArgNode >= 0) {
        if (HOPTCValidateMemAllocatorArg(c, allocArgNode, allocBaseType) != 0) {
            return -1;
        }
    } else {
        if (HOPTCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxMemType) != 0) {
            return -1;
        }
        if (!HOPTCCanAssign(c, allocBaseType, ctxMemType)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_CONTEXT_TYPE_MISMATCH);
        }
    }

    if (HOPTCResolveTypeNode(c, typeNode, &elemType) != 0) {
        return -1;
    }

    if (countArgNode >= 0 && HOPTCTypeContainsVarSizeByValue(c, elemType)) {
        return HOPTCFailNode(c, typeNode, HOPDiag_TYPE_MISMATCH);
    }
    if (countArgNode < 0 && HOPTCTypeContainsVarSizeByValue(c, elemType) && initArgNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_NEW_VARSIZE_INIT_REQUIRED);
    }

    if (initArgNode >= 0) {
        int32_t initType;
        if (HOPTCTypeExprExpected(c, initArgNode, elemType, &initType) != 0) {
            return -1;
        }
        if (!HOPTCCanAssign(c, elemType, initType)) {
            return HOPTCFailNode(c, initArgNode, HOPDiag_TYPE_MISMATCH);
        }
    }

    if (countArgNode >= 0) {
        if (HOPTCTypeExpr(c, countArgNode, &countType) != 0) {
            return -1;
        }
        if (!HOPTCIsIntegerType(c, countType)) {
            return HOPTCFailNode(c, countArgNode, HOPDiag_TYPE_MISMATCH);
        }
        if (HOPTCConstIntExpr(c, countArgNode, &countValue, &countIsConst) != 0) {
            return HOPTCFailNode(c, countArgNode, HOPDiag_TYPE_MISMATCH);
        }
        if (countIsConst && countValue < 0) {
            return HOPTCFailNode(c, countArgNode, HOPDiag_TYPE_MISMATCH);
        }
    }

    if (countArgNode >= 0) {
        if (countIsConst && countValue > 0) {
            int32_t arrayType = HOPTCInternArrayType(
                c, elemType, (uint32_t)countValue, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            resultType = HOPTCInternPtrType(c, arrayType, n->start, n->end);
        } else {
            int32_t sliceType = HOPTCInternSliceType(c, elemType, 1, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            resultType = HOPTCInternPtrType(c, sliceType, n->start, n->end);
        }
    } else {
        resultType = HOPTCInternPtrType(c, elemType, n->start, n->end);
    }
    if (resultType < 0) {
        return -1;
    }
    *outType = resultType;
    return 0;
}

int HOPTCExprIsCompoundTemporary(HOPTypeCheckCtx* c, int32_t exprNode) {
    const HOPAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, exprNode);
        return HOPTCExprIsCompoundTemporary(c, inner);
    }
    if (n->kind == HOPAst_COMPOUND_LIT) {
        return 1;
    }
    if (n->kind == HOPAst_UNARY && n->op == HOPTok_AND) {
        int32_t rhsNode = HOPAstFirstChild(c->ast, exprNode);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == HOPAst_COMPOUND_LIT)
        {
            return 1;
        }
    }
    return 0;
}

static int HOPTCMatchAnyPackIndexExpr(
    HOPTypeCheckCtx* c,
    int32_t          exprNode,
    int32_t* _Nullable outPackType,
    int32_t* _Nullable outIdxNode) {
    const HOPAstNode* n;
    int32_t           baseNode;
    int32_t           idxNode;
    int32_t           localIdx;
    int32_t           packType;
    int32_t           resolvedPackType;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            return 0;
        }
        return HOPTCMatchAnyPackIndexExpr(c, inner, outPackType, outIdxNode);
    }
    if (n->kind != HOPAst_INDEX || (n->flags & HOPAstFlag_INDEX_SLICE) != 0) {
        return 0;
    }
    baseNode = HOPAstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? HOPAstNextSibling(c->ast, baseNode) : -1;
    if (baseNode < 0 || idxNode < 0 || HOPAstNextSibling(c->ast, idxNode) >= 0) {
        return 0;
    }
    if ((uint32_t)baseNode >= c->ast->len || c->ast->nodes[baseNode].kind != HOPAst_IDENT) {
        return 0;
    }
    localIdx = HOPTCLocalFind(
        c, c->ast->nodes[baseNode].dataStart, c->ast->nodes[baseNode].dataEnd);
    if (localIdx < 0) {
        return 0;
    }
    packType = c->locals[localIdx].typeId;
    resolvedPackType = HOPTCResolveAliasBaseType(c, packType);
    if (resolvedPackType < 0 || (uint32_t)resolvedPackType >= c->typeLen
        || c->types[resolvedPackType].kind != HOPTCType_PACK)
    {
        return 0;
    }
    HOPTCMarkLocalRead(c, localIdx);
    if (outPackType != NULL) {
        *outPackType = packType;
    }
    if (outIdxNode != NULL) {
        *outIdxNode = idxNode;
    }
    return 1;
}

int HOPTCExprNeedsExpectedType(HOPTypeCheckCtx* c, int32_t exprNode) {
    const HOPAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, exprNode);
        return HOPTCExprNeedsExpectedType(c, inner);
    }
    if (n->kind == HOPAst_COMPOUND_LIT) {
        return 1;
    }
    if (HOPTCMatchAnyPackIndexExpr(c, exprNode, NULL, NULL)) {
        return 1;
    }
    if (n->kind == HOPAst_UNARY && n->op == HOPTok_AND) {
        int32_t rhsNode = HOPAstFirstChild(c->ast, exprNode);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == HOPAst_COMPOUND_LIT)
        {
            int32_t rhsChild = HOPAstFirstChild(c->ast, rhsNode);
            return !(rhsChild >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[rhsChild].kind));
        }
    }
    return 0;
}

int HOPTCResolveIdentifierExprType(
    HOPTypeCheckCtx* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    uint32_t         spanStart,
    uint32_t         spanEnd,
    int32_t*         outType) {
    int32_t localIdx = HOPTCLocalFind(c, nameStart, nameEnd);
    if (localIdx >= 0) {
        if (HOPTCCheckLocalInitialized(c, localIdx, spanStart, spanEnd) != 0) {
            return -1;
        }
        HOPTCMarkLocalRead(c, localIdx);
        *outType = c->locals[localIdx].typeId;
        return 0;
    }
    if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "context")) {
        int32_t contextTypeId = -1;
        if (c->currentContextType >= 0) {
            contextTypeId = c->currentContextType;
        } else if (c->hasImplicitMainRootContext && c->implicitMainContextType >= 0) {
            contextTypeId = c->implicitMainContextType;
        }
        if (contextTypeId >= 0) {
            int32_t contextRefType = HOPTCInternRefType(c, contextTypeId, 1, spanStart, spanEnd);
            if (contextRefType < 0) {
                return -1;
            }
            *outType = contextRefType;
            return 0;
        }
    }
    {
        int32_t fnIdx = HOPTCFindFunctionIndex(c, nameStart, nameEnd);
        if (fnIdx < 0) {
            fnIdx = HOPTCFindBuiltinQualifiedFunctionIndex(c, nameStart, nameEnd);
        }
        if (fnIdx >= 0) {
            HOPTCMarkFunctionUsed(c, fnIdx);
            *outType = c->funcs[fnIdx].funcTypeId;
            return 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = HOPTCFindTopLevelVarLikeNode(c, nameStart, nameEnd, &nameIndex);
        if (varLikeNode >= 0) {
            if (c->currentFunctionIsCompareHook) {
                return HOPTCFailSpan(c, HOPDiag_COMPARISON_HOOK_IMPURE, spanStart, spanEnd);
            }
            return HOPTCTypeTopLevelVarLikeNode(c, varLikeNode, nameIndex, outType);
        }
    }
    return HOPTCFailSpan(c, HOPDiag_UNKNOWN_SYMBOL, nameStart, nameEnd);
}

int HOPTCInferAnonStructTypeFromCompound(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t firstField, int32_t* outType) {
    HOPTCAnonFieldSig fieldSigs[HOPTC_MAX_ANON_FIELDS];
    uint32_t          fieldCount = 0;
    int32_t           fieldNode = firstField;
    while (fieldNode >= 0) {
        const HOPAstNode* field = &c->ast->nodes[fieldNode];
        int32_t           exprNode;
        int32_t           fieldType = -1;
        uint32_t          i;
        if (field->kind != HOPAst_COMPOUND_FIELD) {
            return HOPTCFailNode(c, fieldNode, HOPDiag_UNEXPECTED_TOKEN);
        }
        if (fieldCount >= HOPTC_MAX_ANON_FIELDS) {
            return HOPTCFailNode(c, fieldNode, HOPDiag_ARENA_OOM);
        }
        for (i = 0; i < fieldCount; i++) {
            if (HOPNameEqSlice(
                    c->src,
                    fieldSigs[i].nameStart,
                    fieldSigs[i].nameEnd,
                    field->dataStart,
                    field->dataEnd))
            {
                HOPTCSetDiagWithArg(
                    c->diag,
                    HOPDiag_COMPOUND_FIELD_DUPLICATE,
                    field->start,
                    field->end,
                    field->dataStart,
                    field->dataEnd);
                return -1;
            }
        }
        exprNode = HOPAstFirstChild(c->ast, fieldNode);
        if (exprNode < 0) {
            if ((field->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                return HOPTCFailNode(c, fieldNode, HOPDiag_EXPECTED_EXPR);
            }
            if (HOPTCResolveIdentifierExprType(
                    c, field->dataStart, field->dataEnd, field->start, field->end, &fieldType)
                != 0)
            {
                return -1;
            }
        } else {
            if (HOPTCTypeExpr(c, exprNode, &fieldType) != 0) {
                return -1;
            }
        }
        if (fieldType == c->typeNull) {
            return HOPTCFailNode(c, exprNode, HOPDiag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (fieldType == c->typeVoid) {
            return HOPTCFailNode(c, exprNode, HOPDiag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (HOPTCConcretizeInferredType(c, fieldType, &fieldType) != 0) {
            return -1;
        }
        fieldSigs[fieldCount].nameStart = field->dataStart;
        fieldSigs[fieldCount].nameEnd = field->dataEnd;
        fieldSigs[fieldCount].typeId = fieldType;
        fieldCount++;
        fieldNode = HOPAstNextSibling(c->ast, fieldNode);
    }

    {
        int32_t typeId = HOPTCInternAnonAggregateType(
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

int HOPTCTypeCompoundLit(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType) {
    int32_t  child = HOPAstFirstChild(c->ast, nodeId);
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

    if (child >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        if (HOPTCResolveTypeNode(c, child, &resolvedType) != 0) {
            int variantRc = HOPTCResolveEnumVariantTypeName(
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
        firstField = HOPAstNextSibling(c->ast, child);
    } else {
        if (expectedType < 0) {
            if (HOPTCInferAnonStructTypeFromCompound(c, nodeId, firstField, &resolvedType) != 0) {
                return -1;
            }
            targetType = resolvedType;
        } else {
            resolvedType = expectedType;
            targetType = expectedType;
        }
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && c->types[expectedType].kind == HOPTCType_REF)
    {
        expectedReadonlyRef = !HOPTCTypeIsMutable(&c->types[expectedType]);
        expectedBaseType = c->types[expectedType].baseType;
        if (!expectedReadonlyRef) {
            return HOPTCFailNode(c, nodeId, HOPDiag_COMPOUND_MUT_REF_TEMPORARY);
        }
        if (expectedBaseType < 0 || (uint32_t)expectedBaseType >= c->typeLen) {
            return HOPTCFailNode(c, nodeId, HOPDiag_COMPOUND_INFER_NON_AGGREGATE);
        }
        if (child < 0 || !HOPTCIsTypeNodeKind(c->ast->nodes[child].kind)) {
            resolvedType = expectedType;
            targetType = expectedBaseType;
        } else {
            if (!HOPTCCanAssign(c, expectedBaseType, targetType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            resolvedType = expectedType;
        }
    }

    targetAggregateType = HOPTCResolveAliasBaseType(c, targetType);
    if (targetAggregateType < 0 || (uint32_t)targetAggregateType >= c->typeLen) {
        return HOPTCFailNode(
            c,
            nodeId,
            child >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[child].kind)
                ? HOPDiag_COMPOUND_TYPE_REQUIRED
                : HOPDiag_COMPOUND_INFER_NON_AGGREGATE);
    }
    if (c->types[targetAggregateType].kind != HOPTCType_NAMED
        && c->types[targetAggregateType].kind != HOPTCType_ANON_STRUCT
        && c->types[targetAggregateType].kind != HOPTCType_ANON_UNION)
    {
        return HOPTCFailNode(
            c,
            nodeId,
            child >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[child].kind)
                ? HOPDiag_COMPOUND_TYPE_REQUIRED
                : HOPDiag_COMPOUND_INFER_NON_AGGREGATE);
    }
    if (c->types[targetAggregateType].kind == HOPTCType_NAMED
        && HOPTCEnsureNamedTypeFieldsResolved(c, targetAggregateType) != 0)
    {
        return -1;
    }
    if (isEnumVariantLiteral) {
        if (!HOPTCIsNamedDeclKind(c, targetAggregateType, HOPAst_ENUM)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_COMPOUND_TYPE_REQUIRED);
        }
        isUnion = 0;
    } else if (c->types[targetAggregateType].kind == HOPTCType_NAMED) {
        int32_t declNode = c->types[targetAggregateType].declNode;
        if (declNode < 0 || (uint32_t)declNode >= c->ast->len
            || (c->ast->nodes[declNode].kind != HOPAst_STRUCT
                && c->ast->nodes[declNode].kind != HOPAst_UNION))
        {
            return HOPTCFailNode(
                c,
                nodeId,
                child >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[child].kind)
                    ? HOPDiag_COMPOUND_TYPE_REQUIRED
                    : HOPDiag_COMPOUND_INFER_NON_AGGREGATE);
        }
        isUnion = c->ast->nodes[declNode].kind == HOPAst_UNION;
    } else {
        isUnion = c->types[targetAggregateType].kind == HOPTCType_ANON_UNION;
    }

    while (firstField >= 0) {
        const HOPAstNode* fieldNode = &c->ast->nodes[firstField];
        int32_t           fieldType = -1;
        int32_t           exprNode;
        int32_t           exprType = -1;
        int32_t           scan;

        if (fieldNode->kind != HOPAst_COMPOUND_FIELD) {
            return HOPTCFailNode(c, firstField, HOPDiag_UNEXPECTED_TOKEN);
        }

        scan = HOPAstFirstChild(c->ast, nodeId);
        if (scan >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[scan].kind)) {
            scan = HOPAstNextSibling(c->ast, scan);
        }
        while (scan >= 0 && scan != firstField) {
            const HOPAstNode* prevField = &c->ast->nodes[scan];
            if (prevField->kind == HOPAst_COMPOUND_FIELD
                && HOPNameEqSlice(
                    c->src,
                    prevField->dataStart,
                    prevField->dataEnd,
                    fieldNode->dataStart,
                    fieldNode->dataEnd))
            {
                HOPTCSetDiagWithArg(
                    c->diag,
                    HOPDiag_COMPOUND_FIELD_DUPLICATE,
                    fieldNode->start,
                    fieldNode->end,
                    fieldNode->dataStart,
                    fieldNode->dataEnd);
                return -1;
            }
            scan = HOPAstNextSibling(c->ast, scan);
        }

        if ((!isEnumVariantLiteral
             && HOPTCFieldLookupPath(
                    c, targetAggregateType, fieldNode->dataStart, fieldNode->dataEnd, &fieldType)
                    != 0)
            || (isEnumVariantLiteral
                && HOPTCEnumVariantPayloadFieldType(
                       c,
                       targetAggregateType,
                       enumVariantStart,
                       enumVariantEnd,
                       fieldNode->dataStart,
                       fieldNode->dataEnd,
                       &fieldType)
                       != 0))
        {
            HOPTCSetDiagWithArg(
                c->diag,
                HOPDiag_COMPOUND_FIELD_UNKNOWN,
                fieldNode->start,
                fieldNode->end,
                fieldNode->dataStart,
                fieldNode->dataEnd);
            return -1;
        }

        exprNode = HOPAstFirstChild(c->ast, firstField);
        if (exprNode < 0) {
            if ((fieldNode->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                return HOPTCFailNode(c, firstField, HOPDiag_EXPECTED_EXPR);
            }
            if (HOPTCResolveIdentifierExprType(
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
        } else if (HOPTCTypeExprExpected(c, exprNode, fieldType, &exprType) != 0) {
            return -1;
        }
        if (!HOPTCCanAssign(c, fieldType, exprType)) {
            uint32_t errStart =
                exprNode >= 0 ? c->ast->nodes[exprNode].start : c->ast->nodes[firstField].start;
            uint32_t errEnd =
                exprNode >= 0 ? c->ast->nodes[exprNode].end : c->ast->nodes[firstField].end;
            HOPTCSetDiagWithArg(
                c->diag,
                HOPDiag_COMPOUND_FIELD_TYPE_MISMATCH,
                errStart,
                errEnd,
                fieldNode->dataStart,
                fieldNode->dataEnd);
            return -1;
        }
        explicitFieldCount++;
        firstField = HOPAstNextSibling(c->ast, firstField);
    }

    if (isUnion && explicitFieldCount > 1u) {
        return HOPTCFailNode(c, nodeId, HOPDiag_COMPOUND_UNION_MULTI_FIELD);
    }

    *outType = resolvedType;
    return 0;
}

int HOPTCTypeExprExpected(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return HOPTCFailSpan(c, HOPDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, nodeId);
        if (inner < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        return HOPTCTypeExprExpected(c, inner, expectedType, outType);
    }

    if (n->kind == HOPAst_CALL || n->kind == HOPAst_CALL_WITH_CONTEXT) {
        int32_t savedExpectedCallType = c->activeExpectedCallType;
        int     rc;
        c->activeExpectedCallType = expectedType;
        rc = HOPTCTypeExpr(c, nodeId, outType);
        c->activeExpectedCallType = savedExpectedCallType;
        return rc;
    }

    if (n->kind == HOPAst_COMPOUND_LIT) {
        return HOPTCTypeCompoundLit(c, nodeId, expectedType, outType);
    }
    if (n->kind == HOPAst_INDEX) {
        int32_t packType = -1;
        int32_t idxNode = -1;
        if (HOPTCMatchAnyPackIndexExpr(c, nodeId, &packType, &idxNode)) {
            int32_t resolvedPackType = HOPTCResolveAliasBaseType(c, packType);
            int32_t idxType;
            int64_t idxValue = 0;
            int     idxIsConst = 0;
            if (resolvedPackType < 0 || (uint32_t)resolvedPackType >= c->typeLen
                || c->types[resolvedPackType].kind != HOPTCType_PACK)
            {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCTypeExpr(c, idxNode, &idxType) != 0) {
                return -1;
            }
            if (!HOPTCIsIntegerType(c, idxType)) {
                return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
                return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
            }
            if (!idxIsConst) {
                int32_t resolvedExpectedType = HOPTCResolveAliasBaseType(c, expectedType);
                if (expectedType < 0 || resolvedExpectedType < 0) {
                    return HOPTCFailNode(c, idxNode, HOPDiag_ANYTYPE_PACK_INDEX_NOT_CONST);
                }
                if (resolvedExpectedType == c->typeAnytype) {
                    *outType = c->typeAnytype;
                    return 0;
                }
                *outType = expectedType;
                return 0;
            }
            if (idxValue < 0 || (uint64_t)idxValue >= c->types[resolvedPackType].fieldCount) {
                return HOPTCFailNode(c, idxNode, HOPDiag_ANYTYPE_PACK_INDEX_OOB);
            }
            *outType =
                c->funcParamTypes[c->types[resolvedPackType].fieldStart + (uint32_t)idxValue];
            return 0;
        }
    }

    if (n->kind == HOPAst_STRING
        || (n->kind == HOPAst_BINARY && (HOPTokenKind)n->op == HOPTok_ADD
            && HOPIsStringLiteralConcatChain(c->ast, nodeId)))
    {
        int32_t defaultType = HOPTCGetStrRefType(c, n->start, n->end);
        if (defaultType < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
        }
        if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen) {
            int32_t expectedResolved = HOPTCResolveAliasBaseType(c, expectedType);
            if (expectedResolved >= 0 && (uint32_t)expectedResolved < c->typeLen) {
                const HOPTCType* t = &c->types[expectedResolved];
                int32_t          baseType = t->baseType;
                if (baseType >= 0) {
                    baseType = HOPTCResolveAliasBaseType(c, baseType);
                }
                if (t->kind == HOPTCType_PTR && baseType == c->typeStr) {
                    *outType = expectedResolved;
                    return 0;
                }
                if (t->kind == HOPTCType_REF && baseType == c->typeStr && !HOPTCTypeIsMutable(t)) {
                    *outType = expectedResolved;
                    return 0;
                }
            }
        }
        *outType = defaultType;
        return 0;
    }

    if (n->kind == HOPAst_TUPLE_EXPR) {
        int32_t expectedBase = HOPTCResolveAliasBaseType(c, expectedType);
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
            && c->types[expectedBase].kind == HOPTCType_TUPLE)
        {
            int32_t  child = HOPAstFirstChild(c->ast, nodeId);
            uint32_t idx = 0;
            while (child >= 0) {
                int32_t srcType;
                int32_t dstType;
                if (idx >= c->types[expectedBase].fieldCount) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
                }
                dstType = c->funcParamTypes[c->types[expectedBase].fieldStart + idx];
                if (HOPTCTypeExprExpected(c, child, dstType, &srcType) != 0) {
                    return -1;
                }
                if (!HOPTCCanAssign(c, dstType, srcType)) {
                    return HOPTCFailTypeMismatchDetail(c, child, child, srcType, dstType);
                }
                idx++;
                child = HOPAstNextSibling(c->ast, child);
            }
            if (idx != c->types[expectedBase].fieldCount) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            *outType = expectedBase;
            return 0;
        }
        return HOPTCTypeExpr(c, nodeId, outType);
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && HOPTCIsIntegerType(c, expectedType))
    {
        int32_t srcType;
        if (HOPTCTypeExpr(c, nodeId, &srcType) != 0) {
            return -1;
        }
        if (srcType == c->typeUntypedInt) {
            int64_t value = 0;
            int     isConst = 0;
            if (HOPTCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!HOPTCConstIntFitsType(c, value, expectedType)) {
                return HOPTCFailConstIntRange(c, nodeId, value, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
        if (HOPTCTypeIsRuneLike(c, srcType)) {
            int64_t value = 0;
            int     isConst = 0;
            if (HOPTCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!HOPTCConstIntFitsType(c, value, expectedType)) {
                return HOPTCFailConstIntRange(c, nodeId, value, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && HOPTCIsFloatType(c, expectedType))
    {
        int32_t srcType;
        if (HOPTCTypeExpr(c, nodeId, &srcType) != 0) {
            return -1;
        }
        if (srcType == c->typeUntypedInt) {
            int64_t value = 0;
            int     isConst = 0;
            if (HOPTCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!HOPTCConstIntFitsFloatType(c, value, expectedType)) {
                return HOPTCFailConstFloatRange(c, nodeId, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
        if (srcType == c->typeUntypedFloat) {
            double value = 0.0;
            int    isConst = 0;
            if (HOPTCConstFloatExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!HOPTCConstFloatFitsType(c, value, expectedType)) {
                return HOPTCFailConstFloatRange(c, nodeId, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
    }

    if (n->kind == HOPAst_UNARY && n->op == HOPTok_AND) {
        int32_t rhsNode = HOPAstFirstChild(c->ast, nodeId);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == HOPAst_COMPOUND_LIT)
        {
            int32_t rhsExpected = -1;
            int32_t rhsType;
            int32_t ptrType;
            if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
                && (c->types[expectedType].kind == HOPTCType_REF
                    || c->types[expectedType].kind == HOPTCType_PTR))
            {
                rhsExpected = c->types[expectedType].baseType;
            }
            if (HOPTCTypeExprExpected(c, rhsNode, rhsExpected, &rhsType) != 0) {
                return -1;
            }
            ptrType = HOPTCInternPtrType(c, rhsType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
    }

    return HOPTCTypeExpr(c, nodeId, outType);
}

int HOPTCExprIsAssignable(HOPTypeCheckCtx* c, int32_t exprNode) {
    const HOPAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, exprNode);
        return HOPTCExprIsAssignable(c, inner);
    }
    if (n->kind == HOPAst_IDENT) {
        return 1;
    }
    if (n->kind == HOPAst_INDEX) {
        HOPTCIndexBaseInfo info;
        int32_t            baseNode = HOPAstFirstChild(c->ast, exprNode);
        int32_t            baseType;
        if ((n->flags & HOPAstFlag_INDEX_SLICE) != 0) {
            return 0;
        }
        if (baseNode < 0 || HOPTCTypeExpr(c, baseNode, &baseType) != 0 || baseType < 0
            || (uint32_t)baseType >= c->typeLen)
        {
            return 0;
        }
        if (HOPTCResolveIndexBaseInfo(c, baseType, &info) != 0) {
            return 0;
        }
        if (!info.indexable) {
            return 0;
        }
        if (c->types[baseType].kind == HOPTCType_ARRAY || c->types[baseType].kind == HOPTCType_PTR)
        {
            return 1;
        }
        return info.sliceMutable;
    }
    if (n->kind == HOPAst_FIELD_EXPR) {
        int32_t  recvNode = HOPAstFirstChild(c->ast, exprNode);
        int32_t  recvType;
        int32_t  fieldType;
        uint32_t fieldIndex = 0;
        if (recvNode < 0) {
            return 0;
        }
        if (HOPTCTypeExpr(c, recvNode, &recvType) != 0) {
            return 0;
        }
        if (HOPTCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, &fieldIndex) != 0) {
            return 0;
        }
        if ((c->fields[fieldIndex].flags & HOPTCFieldFlag_DEPENDENT) != 0) {
            return 0;
        }
        if (recvType >= 0 && (uint32_t)recvType < c->typeLen
            && c->types[recvType].kind == HOPTCType_REF)
        {
            return HOPTCTypeIsMutable(&c->types[recvType]);
        }
        return 1;
    }
    if (n->kind == HOPAst_UNARY && n->op == HOPTok_MUL) {
        int32_t rhsNode = HOPAstFirstChild(c->ast, exprNode);
        int32_t rhsType;
        if (rhsNode < 0 || HOPTCTypeExpr(c, rhsNode, &rhsType) != 0 || rhsType < 0
            || (uint32_t)rhsType >= c->typeLen)
        {
            return 0;
        }
        if (c->types[rhsType].kind == HOPTCType_PTR) {
            return 1;
        }
        if (c->types[rhsType].kind == HOPTCType_REF) {
            return HOPTCTypeIsMutable(&c->types[rhsType]);
        }
        return 0;
    }
    return 0;
}

int HOPTCExprIsConstAssignTarget(HOPTypeCheckCtx* c, int32_t exprNode) {
    const HOPAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, exprNode);
        return HOPTCExprIsConstAssignTarget(c, inner);
    }
    if (n->kind != HOPAst_IDENT) {
        return 0;
    }
    {
        int32_t localIdx = HOPTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            return (c->locals[localIdx].flags & HOPTCLocalFlag_CONST) != 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = HOPTCFindTopLevelVarLikeNode(c, n->dataStart, n->dataEnd, &nameIndex);
        if (varLikeNode >= 0) {
            return c->ast->nodes[varLikeNode].kind == HOPAst_CONST;
        }
    }
    return 0;
}

int HOPTCTypeAssignTargetExpr(
    HOPTypeCheckCtx* c, int32_t nodeId, int skipDirectIdentRead, int32_t* outType) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, nodeId);
        return HOPTCTypeAssignTargetExpr(c, inner, skipDirectIdentRead, outType);
    }
    if (skipDirectIdentRead && n->kind == HOPAst_IDENT) {
        int32_t localIdx = HOPTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            *outType = c->locals[localIdx].typeId;
            return 0;
        }
    }
    return HOPTCTypeExpr(c, nodeId, outType);
}

void HOPTCMarkDirectIdentLocalWrite(HOPTypeCheckCtx* c, int32_t nodeId, int markInitialized) {
    const HOPAstNode* n;
    int32_t           localIdx;
    if (c == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == HOPAst_CALL_ARG) {
        HOPTCMarkDirectIdentLocalWrite(c, HOPAstFirstChild(c->ast, nodeId), markInitialized);
        return;
    }
    if (n->kind != HOPAst_IDENT) {
        return;
    }
    localIdx = HOPTCLocalFind(c, n->dataStart, n->dataEnd);
    if (localIdx < 0) {
        return;
    }
    HOPTCMarkLocalWrite(c, localIdx);
    if (markInitialized) {
        HOPTCMarkLocalInitialized(c, localIdx);
    }
}

int HOPTCTypeExpr_IDENT(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    uint32_t i;
    (void)nodeId;
    if (c->defaultFieldNodes != NULL && c->defaultFieldTypes != NULL
        && c->defaultFieldCurrentIndex < c->defaultFieldCount)
    {
        for (i = 0; i < c->defaultFieldCount; i++) {
            int32_t           fieldNode = c->defaultFieldNodes[i];
            const HOPAstNode* f;
            if (fieldNode < 0 || (uint32_t)fieldNode >= c->ast->len) {
                continue;
            }
            f = &c->ast->nodes[fieldNode];
            if (f->kind != HOPAst_FIELD) {
                continue;
            }
            if (HOPNameEqSlice(c->src, f->dataStart, f->dataEnd, n->dataStart, n->dataEnd)) {
                if (i < c->defaultFieldCurrentIndex && c->defaultFieldTypes[i] >= 0) {
                    *outType = c->defaultFieldTypes[i];
                    return 0;
                }
                HOPTCSetDiagWithArg(
                    c->diag,
                    HOPDiag_FIELD_DEFAULT_FORWARD_REF,
                    n->start,
                    n->end,
                    f->dataStart,
                    f->dataEnd);
                return -1;
            }
            if ((f->flags & HOPAstFlag_FIELD_EMBEDDED) != 0 && c->defaultFieldTypes[i] >= 0) {
                int32_t promotedType = -1;
                if (HOPTCFieldLookupPath(
                        c, c->defaultFieldTypes[i], n->dataStart, n->dataEnd, &promotedType)
                    == 0)
                {
                    if (i < c->defaultFieldCurrentIndex) {
                        *outType = promotedType;
                        return 0;
                    }
                    HOPTCSetDiagWithArg(
                        c->diag,
                        HOPDiag_FIELD_DEFAULT_FORWARD_REF,
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
        int32_t localIdx = HOPTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            if (HOPTCCheckLocalInitialized(c, localIdx, n->start, n->end) != 0) {
                return -1;
            }
            HOPTCMarkLocalRead(c, localIdx);
            *outType = c->locals[localIdx].typeId;
            return 0;
        }
    }
    if (c->activeConstEvalCtx != NULL) {
        int32_t execType = -1;
        if (HOPTCConstLookupExecBindingType(
                c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execType))
        {
            *outType = execType;
            return 0;
        }
        if (HOPTCConstLookupMirLocalType(
                c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execType))
        {
            *outType = execType;
            return 0;
        }
        {
            HOPCTFEValue execValue;
            int          execIsConst = 0;
            if (HOPTCResolveConstIdent(
                    c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execValue, &execIsConst, NULL)
                    == 0
                && execIsConst
                && HOPTCEvalConstExecInferValueTypeCb(c->activeConstEvalCtx, &execValue, &execType)
                       == 0)
            {
                *outType = execType;
                return 0;
            }
        }
    }
    if (HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "context")) {
        if (c->currentFunctionIsCompareHook) {
            return HOPTCFailNode(c, nodeId, HOPDiag_COMPARISON_HOOK_IMPURE);
        }
        int32_t contextTypeId = -1;
        if (c->currentContextType >= 0) {
            contextTypeId = c->currentContextType;
        } else if (c->hasImplicitMainRootContext && c->implicitMainContextType >= 0) {
            contextTypeId = c->implicitMainContextType;
        }
        if (contextTypeId >= 0) {
            int32_t contextRefType = HOPTCInternRefType(
                c, contextTypeId, 1, n->dataStart, n->dataEnd);
            if (contextRefType < 0) {
                return -1;
            }
            *outType = contextRefType;
            return 0;
        }
    }
    {
        int32_t fnIdx = HOPTCFindFunctionIndex(c, n->dataStart, n->dataEnd);
        if (fnIdx >= 0) {
            HOPTCMarkFunctionUsed(c, fnIdx);
            *outType = c->funcs[fnIdx].funcTypeId;
            return 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = HOPTCFindTopLevelVarLikeNode(c, n->dataStart, n->dataEnd, &nameIndex);
        if (varLikeNode >= 0) {
            if (c->currentFunctionIsCompareHook) {
                return HOPTCFailNode(c, nodeId, HOPDiag_COMPARISON_HOOK_IMPURE);
            }
            return HOPTCTypeTopLevelVarLikeNode(c, varLikeNode, nameIndex, outType);
        }
    }
    if (HOPTCResolveTypeValueName(c, n->dataStart, n->dataEnd) >= 0 && c->typeType >= 0) {
        *outType = c->typeType;
        return 0;
    }
    return HOPTCFailSpan(c, HOPDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
}

int HOPTCTypeExpr_TYPE_VALUE(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t typeNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t ignoredType;
    (void)n;
    if (typeNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
    }
    if (HOPTCResolveTypeNode(c, typeNode, &ignoredType) != 0) {
        return -1;
    }
    *outType = c->typeType;
    return 0;
}

int HOPTCTypeExpr_INT(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedInt;
    return 0;
}

int HOPTCTypeExpr_FLOAT(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedFloat;
    return 0;
}

int HOPTCTypeExpr_STRING(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t strRefType = HOPTCGetStrRefType(c, n->start, n->end);
    if (strRefType < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
    }
    *outType = strRefType;
    return 0;
}

int HOPTCTypeExpr_RUNE(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedInt;
    return 0;
}

int HOPTCTypeExpr_BOOL(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeBool;
    return 0;
}

int HOPTCTypeExpr_COMPOUND_LIT(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    (void)n;
    return HOPTCTypeCompoundLit(c, nodeId, -1, outType);
}

int HOPTCTypeExpr_CALL_WITH_CONTEXT(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t savedActive = c->activeCallWithNode;
    int32_t callNode = HOPAstFirstChild(c->ast, nodeId);
    (void)n;
    if (callNode < 0 || c->ast->nodes[callNode].kind != HOPAst_CALL) {
        return HOPTCFailNode(c, nodeId, HOPDiag_WITH_CONTEXT_ON_NON_CALL);
    }
    c->activeCallWithNode = nodeId;
    if (HOPTCValidateCurrentCallOverlay(c) != 0) {
        c->activeCallWithNode = savedActive;
        return -1;
    }
    if (HOPTCTypeExpr(c, callNode, outType) != 0) {
        c->activeCallWithNode = savedActive;
        return -1;
    }
    c->activeCallWithNode = savedActive;
    return 0;
}

int HOPTCTypeExpr_NEW(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    (void)n;
    if (c->currentFunctionIsCompareHook) {
        return HOPTCFailNode(c, nodeId, HOPDiag_COMPARISON_HOOK_IMPURE);
    }
    return HOPTCTypeNewExpr(c, nodeId, outType);
}

int HOPTCTypeSourceLocationOfCall(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* callee, int32_t* outType) {
    int32_t argNode = HOPAstNextSibling(c->ast, HOPAstFirstChild(c->ast, nodeId));
    int32_t argType;
    int32_t nextArgNode;
    if (argNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
    if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
        return -1;
    }
    (void)argType;
    nextArgNode = HOPAstNextSibling(c->ast, argNode);
    if (nextArgNode >= 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
    if (c->typeSourceLocation < 0) {
        c->typeSourceLocation = HOPTCFindSourceLocationType(c);
    }
    if (c->typeSourceLocation < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
    }
    *outType = c->typeSourceLocation;
    (void)callee;
    return 0;
}

static int HOPTCTypeEmitCompilerDiagCall(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t spanNode, int32_t msgNode, HOPTCCompilerDiagOp op) {
    HOPTCConstEvalCtx evalCtx;
    HOPCTFEValue      msgValue;
    int               msgIsConst = 0;
    uint32_t          diagStart = c->ast->nodes[nodeId].start;
    uint32_t          diagEnd = c->ast->nodes[nodeId].end;
    const char*       detail;
    HOPDiag           emitted;
    int32_t           msgExprNode = msgNode;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (msgNode >= 0 && (uint32_t)msgNode < c->ast->len
        && c->ast->nodes[msgNode].kind == HOPAst_CALL_ARG)
    {
        int32_t inner = HOPAstFirstChild(c->ast, msgNode);
        if (inner >= 0) {
            msgExprNode = inner;
        }
    }
    if (op == HOPTCCompilerDiagOp_ERROR_AT || op == HOPTCCompilerDiagOp_WARN_AT) {
        int         spanIsConst = 0;
        HOPCTFESpan span;
        uint32_t    spanStartOffset = 0;
        uint32_t    spanEndOffset = 0;
        if (HOPTCConstEvalSourceLocationExpr(&evalCtx, spanNode, &span, &spanIsConst) != 0) {
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
    if (HOPTCEvalConstExprNode(&evalCtx, msgExprNode, &msgValue, &msgIsConst) != 0) {
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
    if (emitted.type == HOPDiagType_WARNING) {
        return HOPTCEmitWarningDiag(c, &emitted);
    }
    if (c->diag != NULL) {
        *c->diag = emitted;
    }
    return -1;
}

int HOPTCTypeCompilerDiagCall(
    HOPTypeCheckCtx*    c,
    int32_t             nodeId,
    const HOPAstNode*   callee,
    HOPTCCompilerDiagOp op,
    int32_t*            outType) {
    int32_t arg1Node = HOPAstNextSibling(c->ast, HOPAstFirstChild(c->ast, nodeId));
    int32_t arg2Node = arg1Node >= 0 ? HOPAstNextSibling(c->ast, arg1Node) : -1;
    int32_t arg3Node = arg2Node >= 0 ? HOPAstNextSibling(c->ast, arg2Node) : -1;
    int32_t spanNode = -1;
    int32_t msgNode;
    int32_t msgType;
    int32_t wantStrType;
    if (op == HOPTCCompilerDiagOp_ERROR || op == HOPTCCompilerDiagOp_WARN) {
        if (arg1Node < 0 || arg2Node >= 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        msgNode = arg1Node;
    } else {
        int32_t spanType;
        if (arg1Node < 0 || arg2Node < 0 || arg3Node >= 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        spanNode = arg1Node;
        if (HOPTCTypeExpr(c, arg1Node, &spanType) != 0) {
            return -1;
        }
        if (!HOPTCTypeIsSourceLocation(c, spanType)) {
            return HOPTCFailNode(c, arg1Node, HOPDiag_TYPE_MISMATCH);
        }
        msgNode = arg2Node;
    }
    if (HOPTCTypeExpr(c, msgNode, &msgType) != 0) {
        return -1;
    }
    wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
    if (wantStrType < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
    }
    if (!HOPTCCanAssign(c, wantStrType, msgType)) {
        return HOPTCFailNode(c, msgNode, HOPDiag_TYPE_MISMATCH);
    }
    if (c->activeConstEvalCtx == NULL && c->compilerDiagPathProven != 0) {
        if (HOPTCTypeEmitCompilerDiagCall(c, nodeId, spanNode, msgNode, op) != 0) {
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
} HOPTCCopySeqInfo;

static int32_t HOPTCGetStringSliceExprType(
    HOPTypeCheckCtx* c, int32_t baseType, uint32_t start, uint32_t end) {
    int32_t          resolvedType;
    const HOPTCType* t;
    int32_t          resolvedBaseType;
    if (c == NULL) {
        return -1;
    }
    resolvedType = HOPTCResolveAliasBaseType(c, baseType);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return -1;
    }
    if (resolvedType == c->typeStr) {
        return c->typeStr;
    }
    t = &c->types[resolvedType];
    if (t->kind != HOPTCType_PTR && t->kind != HOPTCType_REF) {
        return -1;
    }
    resolvedBaseType = HOPTCResolveAliasBaseType(c, t->baseType);
    if (resolvedBaseType != c->typeStr) {
        return -1;
    }
    if (t->kind == HOPTCType_PTR) {
        return HOPTCGetStrPtrType(c, start, end);
    }
    return HOPTCGetStrRefType(c, start, end);
}

static int HOPTCFailUnsliceableExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t baseType) {
    char         typeBuf[128];
    char         detailBuf[160];
    HOPTCTextBuf typeText;
    HOPTCTextBuf detailText;
    int          rc = HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    HOPTCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    HOPTCFormatTypeRec(c, baseType, &typeText, 0);
    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufAppendCStr(&detailText, "cannot slice expression of type ");
    HOPTCTextBufAppendCStr(&detailText, typeBuf);
    c->diag->detail = HOPTCAllocDiagText(c, detailBuf);
    return rc;
}

static int HOPTCFailInvalidCast(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t srcType, int32_t dstType) {
    char         srcBuf[128];
    char         dstBuf[128];
    char         detailBuf[256];
    HOPTCTextBuf srcText;
    HOPTCTextBuf dstText;
    HOPTCTextBuf detailText;
    int          rc = HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    HOPTCTextBufInit(&srcText, srcBuf, (uint32_t)sizeof(srcBuf));
    HOPTCFormatTypeRec(c, srcType, &srcText, 0);
    HOPTCTextBufInit(&dstText, dstBuf, (uint32_t)sizeof(dstBuf));
    HOPTCFormatTypeRec(c, dstType, &dstText, 0);
    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufAppendCStr(&detailText, "cannot cast ");
    HOPTCTextBufAppendCStr(&detailText, srcBuf);
    HOPTCTextBufAppendCStr(&detailText, " to ");
    HOPTCTextBufAppendCStr(&detailText, dstBuf);
    c->diag->detail = HOPTCAllocDiagText(c, detailBuf);
    return rc;
}

static int HOPTCResolveCopySeqInfo(HOPTypeCheckCtx* c, int32_t typeId, HOPTCCopySeqInfo* out) {
    int32_t          resolvedType;
    const HOPTCType* t;
    int32_t          u8Type;
    if (c == NULL || out == NULL) {
        return -1;
    }
    out->elemType = -1;
    out->isString = 0;
    out->writable = 0;
    u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
    if (u8Type < 0) {
        return -1;
    }
    resolvedType = HOPTCResolveAliasBaseType(c, typeId);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return 1;
    }
    if (resolvedType == c->typeStr) {
        out->elemType = u8Type;
        out->isString = 1;
        return 0;
    }
    t = &c->types[resolvedType];
    if (t->kind == HOPTCType_ARRAY) {
        out->elemType = t->baseType;
        out->writable = 1;
        return 0;
    }
    if (t->kind == HOPTCType_SLICE) {
        out->elemType = t->baseType;
        out->writable = HOPTCTypeIsMutable(t);
        return 0;
    }
    if (t->kind == HOPTCType_PTR || t->kind == HOPTCType_REF) {
        int32_t          baseType = t->baseType;
        int32_t          resolvedBaseType;
        const HOPTCType* base;
        if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
            return 1;
        }
        resolvedBaseType = HOPTCResolveAliasBaseType(c, baseType);
        if (resolvedBaseType < 0 || (uint32_t)resolvedBaseType >= c->typeLen) {
            return 1;
        }
        if (resolvedBaseType == c->typeStr) {
            out->elemType = u8Type;
            out->isString = 1;
            out->writable = t->kind == HOPTCType_PTR;
            return 0;
        }
        base = &c->types[resolvedBaseType];
        if (base->kind == HOPTCType_ARRAY) {
            out->elemType = base->baseType;
            out->writable = t->kind == HOPTCType_PTR;
            return 0;
        }
        if (base->kind == HOPTCType_SLICE) {
            out->elemType = base->baseType;
            out->writable = t->kind == HOPTCType_PTR && HOPTCTypeIsMutable(base);
            return 0;
        }
    }
    return 1;
}

int HOPTCTypeExpr_CALL(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t calleeNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t calleeType;
    (void)n;
    if (calleeNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_NOT_CALLABLE);
    }
    if (c->currentFunctionIsCompareHook) {
        return HOPTCFailNode(c, nodeId, HOPDiag_COMPARISON_HOOK_IMPURE);
    }
    if (c->ast->nodes[calleeNode].kind == HOPAst_IDENT) {
        const HOPAstNode*   callee = &c->ast->nodes[calleeNode];
        HOPTCCompilerDiagOp diagOp = HOPTCCompilerDiagOpFromName(
            c, callee->dataStart, callee->dataEnd);
        if (diagOp != HOPTCCompilerDiagOp_NONE) {
            return HOPTCTypeCompilerDiagCall(c, nodeId, callee, diagOp, outType);
        }
        if (HOPTCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd)) {
            return HOPTCTypeSourceLocationOfCall(c, nodeId, callee, outType);
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t kindType;
            int32_t u8Type;
            if (argNode >= 0) {
                if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = HOPAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    kindType = HOPTCFindReflectKindType(c);
                    if (kindType >= 0) {
                        *outType = kindType;
                        return 0;
                    }
                    u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
                    if (u8Type < 0) {
                        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                    }
                    *outType = u8Type;
                    return 0;
                }
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode >= 0) {
                if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = HOPAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    if (c->typeBool < 0) {
                        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                    }
                    *outType = c->typeBool;
                    return 0;
                }
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t strRefType;
            if (argNode >= 0) {
                if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = HOPAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    strRefType = HOPTCGetStrRefType(c, callee->start, callee->end);
                    if (strRefType < 0) {
                        return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
                    }
                    *outType = strRefType;
                    return 0;
                }
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t reflectedTypeId;
            if (argNode >= 0) {
                if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = HOPAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    if (HOPTCResolveReflectedTypeValueExpr(c, argNode, &reflectedTypeId) != 0) {
                        return HOPTCFailNode(c, argNode, HOPDiag_TYPE_MISMATCH);
                    }
                    if (c->types[reflectedTypeId].kind != HOPTCType_ALIAS) {
                        return HOPTCFailNode(c, argNode, HOPDiag_TYPE_MISMATCH);
                    }
                    if (HOPTCResolveAliasTypeId(c, reflectedTypeId) != 0) {
                        return -1;
                    }
                    if (c->typeType < 0) {
                        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                    }
                    *outType = c->typeType;
                    return 0;
                }
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof")) {
            int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                return -1;
            }
            (void)argType;
            nextArgNode = HOPAstNextSibling(c->ast, argNode);
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (c->typeType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            *outType = c->typeType;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")
            || HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice"))
        {
            int32_t argNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                return -1;
            }
            if (argType != c->typeType) {
                return HOPTCFailNode(c, argNode, HOPDiag_TYPE_MISMATCH);
            }
            nextArgNode = HOPAstNextSibling(c->ast, argNode);
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            *outType = c->typeType;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            int32_t typeArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t lenArgNode = typeArgNode >= 0 ? HOPAstNextSibling(c->ast, typeArgNode) : -1;
            int32_t nextArgNode = lenArgNode >= 0 ? HOPAstNextSibling(c->ast, lenArgNode) : -1;
            int32_t typeArgType;
            int32_t lenArgType;
            if (typeArgNode < 0 || lenArgNode < 0 || nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, typeArgNode, &typeArgType) != 0
                || HOPTCTypeExpr(c, lenArgNode, &lenArgType) != 0)
            {
                return -1;
            }
            if (typeArgType != c->typeType || !HOPTCIsIntegerType(c, lenArgType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            *outType = c->typeType;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len")) {
            int32_t strArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t strArgType;
            int32_t nextArgNode;
            int32_t intType;
            if (strArgNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, strArgNode, &strArgType) != 0) {
                return -1;
            }
            if (!HOPTCTypeSupportsLen(c, strArgType)) {
                return HOPTCFailNode(c, strArgNode, HOPDiag_TYPE_MISMATCH);
            }
            nextArgNode = HOPAstNextSibling(c->ast, strArgNode);
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            intType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
            if (intType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "copy")) {
            int32_t          dstNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t          srcNode = dstNode >= 0 ? HOPAstNextSibling(c->ast, dstNode) : -1;
            int32_t          extraNode = srcNode >= 0 ? HOPAstNextSibling(c->ast, srcNode) : -1;
            int32_t          dstType;
            int32_t          srcType;
            int32_t          dstElemResolved;
            int32_t          u8Type;
            int32_t          intType;
            HOPTCCopySeqInfo dstInfo;
            HOPTCCopySeqInfo srcInfo;
            if (dstNode < 0 || srcNode < 0 || extraNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, dstNode, &dstType) != 0
                || HOPTCTypeExpr(c, srcNode, &srcType) != 0)
            {
                return -1;
            }
            if (HOPTCResolveCopySeqInfo(c, dstType, &dstInfo) != 0 || !dstInfo.writable
                || dstInfo.elemType < 0)
            {
                return HOPTCFailNode(c, dstNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCResolveCopySeqInfo(c, srcType, &srcInfo) != 0 || srcInfo.elemType < 0) {
                return HOPTCFailNode(c, srcNode, HOPDiag_TYPE_MISMATCH);
            }
            if (dstInfo.isString) {
                if (!srcInfo.isString) {
                    return HOPTCFailNode(c, srcNode, HOPDiag_TYPE_MISMATCH);
                }
            } else if (srcInfo.isString) {
                u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
                if (u8Type < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                }
                dstElemResolved = HOPTCResolveAliasBaseType(c, dstInfo.elemType);
                if (dstElemResolved < 0 || dstElemResolved != u8Type) {
                    return HOPTCFailNode(c, srcNode, HOPDiag_TYPE_MISMATCH);
                }
            } else if (!HOPTCCanAssign(c, dstInfo.elemType, srcInfo.elemType)) {
                return HOPTCFailNode(c, srcNode, HOPDiag_TYPE_MISMATCH);
            }
            intType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
            if (intType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "cstr")) {
            int32_t strArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t strArgType;
            int32_t nextArgNode;
            int32_t u8Type;
            int32_t u8RefType;
            int32_t wantStrType;
            if (strArgNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, strArgNode, &strArgType) != 0) {
                return -1;
            }
            wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, strArgType)) {
                return HOPTCFailNode(c, strArgNode, HOPDiag_TYPE_MISMATCH);
            }
            nextArgNode = HOPAstNextSibling(c->ast, strArgNode);
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
            if (u8Type < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            u8RefType = HOPTCInternRefType(c, u8Type, 0, callee->start, callee->end);
            if (u8RefType < 0) {
                return -1;
            }
            *outType = u8RefType;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "concat")) {
            int32_t aNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t bNode = aNode >= 0 ? HOPAstNextSibling(c->ast, aNode) : -1;
            int32_t nextNode = bNode >= 0 ? HOPAstNextSibling(c->ast, bNode) : -1;
            int32_t aType;
            int32_t bType;
            int32_t wantStrType;
            int32_t strPtrType;
            int32_t ctxMemType;
            int32_t allocBaseType = HOPTCFindMemAllocatorType(c);
            if (aNode < 0 || bNode < 0 || nextNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (allocBaseType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxMemType) != 0) {
                return -1;
            }
            if (!HOPTCCanAssign(c, allocBaseType, ctxMemType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_CONTEXT_TYPE_MISMATCH);
            }
            if (HOPTCTypeExpr(c, aNode, &aType) != 0 || HOPTCTypeExpr(c, bNode, &bType) != 0) {
                return -1;
            }
            wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, aType) || !HOPTCCanAssign(c, wantStrType, bType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            strPtrType = HOPTCGetStrPtrType(c, callee->start, callee->end);
            if (strPtrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            *outType = strPtrType;
            return 0;
        }
        if (HOPTCNameEqLiteralOrPkgBuiltin(c, callee->dataStart, callee->dataEnd, "fmt", "builtin"))
        {
            int32_t        outNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t        fmtNode = outNode >= 0 ? HOPAstNextSibling(c->ast, outNode) : -1;
            int32_t        argNode;
            int32_t        outBufType;
            int32_t        fmtType;
            int32_t        wantStrType;
            int32_t        strPtrType;
            int32_t        intType;
            int32_t        argNodes[HOPTC_MAX_CALL_ARGS];
            int32_t        argTypes[HOPTC_MAX_CALL_ARGS];
            uint32_t       argCount = 0;
            uint32_t       i;
            const uint8_t* fmtBytes = NULL;
            uint32_t       fmtLen = 0;
            int            fmtIsConst = 0;
            if (outNode < 0 || fmtNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, outNode, &outBufType) != 0) {
                return -1;
            }
            strPtrType = HOPTCGetStrPtrType(c, callee->start, callee->end);
            if (strPtrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, strPtrType, outBufType)) {
                return HOPTCFailNode(c, outNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCTypeExpr(c, fmtNode, &fmtType) != 0) {
                return -1;
            }
            wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, fmtType)) {
                return HOPTCFailNode(c, fmtNode, HOPDiag_TYPE_MISMATCH);
            }
            argNode = HOPAstNextSibling(c->ast, fmtNode);
            while (argNode >= 0) {
                int32_t argType;
                if (argCount >= HOPTC_MAX_CALL_ARGS) {
                    return HOPTCFailNode(c, argNode, HOPDiag_ARITY_MISMATCH);
                }
                if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                argNodes[argCount] = argNode;
                argTypes[argCount] = argType;
                argCount++;
                argNode = HOPAstNextSibling(c->ast, argNode);
            }
            if (HOPTCConstStringExpr(c, fmtNode, &fmtBytes, &fmtLen, &fmtIsConst) != 0) {
                return -1;
            }
            if (fmtIsConst) {
                HOPFmtToken      tokens[512];
                uint32_t         tokenLen = 0;
                uint32_t         placeholderCount = 0;
                HOPFmtParseError parseErr = { 0 };
                if (HOPFmtParseBytes(
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
                    if (parseErr.code == HOPFmtParseErr_TOKEN_OVERFLOW) {
                        return HOPTCFailNode(c, fmtNode, HOPDiag_ARENA_OOM);
                    }
                    if (parseErr.end > parseErr.start && errStart + parseErr.end <= errEnd) {
                        errStart += parseErr.start;
                        errEnd = c->ast->nodes[fmtNode].start + parseErr.end;
                    }
                    return HOPTCFailSpan(c, HOPDiag_FORMAT_INVALID, errStart, errEnd);
                }
                for (i = 0; i < tokenLen; i++) {
                    if (tokens[i].kind == HOPFmtTok_PLACEHOLDER_F
                        || tokens[i].kind == HOPFmtTok_PLACEHOLDER_S)
                    {
                        uint32_t errStart = c->ast->nodes[fmtNode].start;
                        uint32_t errEnd = c->ast->nodes[fmtNode].end;
                        if (tokens[i].end > tokens[i].start
                            && errStart + tokens[i].end <= c->ast->nodes[fmtNode].end)
                        {
                            errStart += tokens[i].start;
                            errEnd = c->ast->nodes[fmtNode].start + tokens[i].end;
                        }
                        return HOPTCFailSpan(c, HOPDiag_FORMAT_INVALID, errStart, errEnd);
                    }
                    if (tokens[i].kind == HOPFmtTok_PLACEHOLDER_I
                        || tokens[i].kind == HOPFmtTok_PLACEHOLDER_R)
                    {
                        placeholderCount++;
                    }
                }
                if (placeholderCount != argCount) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_FORMAT_ARG_COUNT_MISMATCH);
                }
                {
                    uint32_t placeholderIndex = 0;
                    for (i = 0; i < tokenLen; i++) {
                        if (tokens[i].kind == HOPFmtTok_PLACEHOLDER_I
                            || tokens[i].kind == HOPFmtTok_PLACEHOLDER_R)
                        {
                            uint32_t idx = placeholderIndex++;
                            if (idx >= argCount) {
                                return HOPTCFailNode(c, nodeId, HOPDiag_FORMAT_ARG_COUNT_MISMATCH);
                            }
                            if (tokens[i].kind == HOPFmtTok_PLACEHOLDER_I) {
                                if (!HOPTCIsIntegerType(c, argTypes[idx])) {
                                    return HOPTCFailNode(
                                        c, argNodes[idx], HOPDiag_FORMAT_ARG_TYPE_MISMATCH);
                                }
                            } else if (!HOPTCTypeSupportsFmtReflectRec(c, argTypes[idx], 0u)) {
                                return HOPTCFailNode(
                                    c, argNodes[idx], HOPDiag_FORMAT_UNSUPPORTED_TYPE);
                            }
                        }
                    }
                }
            } else {
                for (i = 0; i < argCount; i++) {
                    if (!HOPTCTypeSupportsFmtReflectRec(c, argTypes[i], 0u)) {
                        return HOPTCFailNode(c, argNodes[i], HOPDiag_FORMAT_UNSUPPORTED_TYPE);
                    }
                }
            }
            intType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
            if (intType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (0 && HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "free")) {
            int32_t arg1Node = HOPAstNextSibling(c->ast, calleeNode);
            int32_t arg2Node = arg1Node >= 0 ? HOPAstNextSibling(c->ast, arg1Node) : -1;
            int32_t arg3Node = arg2Node >= 0 ? HOPAstNextSibling(c->ast, arg2Node) : -1;
            int32_t allocArgNode = -1;
            int32_t valueArgNode = -1;
            int32_t valueType;
            int32_t allocType;
            int32_t ctxMemType;
            int32_t allocBaseType = HOPTCFindMemAllocatorType(c);
            int32_t allocParamType =
                allocBaseType < 0
                    ? -1
                    : HOPTCInternRefType(c, allocBaseType, 1, callee->start, callee->end);
            if (arg1Node < 0 || arg3Node >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (arg2Node >= 0) {
                allocArgNode = arg1Node;
                valueArgNode = arg2Node;
            } else {
                valueArgNode = arg1Node;
            }
            if (allocParamType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            if (allocArgNode >= 0) {
                if (HOPTCTypeExpr(c, allocArgNode, &allocType) != 0) {
                    return -1;
                }
                if (!HOPTCCanAssign(c, allocParamType, allocType)) {
                    return HOPTCFailNode(c, allocArgNode, HOPDiag_TYPE_MISMATCH);
                }
            } else {
                if (HOPTCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxMemType) != 0) {
                    return -1;
                }
                if (!HOPTCCanAssign(c, allocParamType, ctxMemType)) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_CONTEXT_TYPE_MISMATCH);
                }
            }
            if (HOPTCTypeExpr(c, valueArgNode, &valueType) != 0) {
                return -1;
            }
            if (!HOPTCTypeIsFreeablePointer(c, valueType)) {
                return HOPTCFailNode(c, valueArgNode, HOPDiag_TYPE_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "panic")) {
            int32_t msgArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t msgArgType;
            int32_t nextArgNode;
            int32_t wantStrType;
            if (msgArgNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, msgArgNode, &msgArgType) != 0) {
                return -1;
            }
            wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, msgArgType)) {
                return HOPTCFailNode(c, msgArgNode, HOPDiag_TYPE_MISMATCH);
            }
            nextArgNode = HOPAstNextSibling(c->ast, msgArgNode);
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "print")) {
            int32_t msgArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t msgArgType;
            int32_t nextArgNode;
            int32_t logType;
            int32_t wantStrType;
            if (msgArgNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCTypeExpr(c, msgArgNode, &msgArgType) != 0) {
                return -1;
            }
            wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, msgArgType)) {
                return HOPTCFailNode(c, msgArgNode, HOPDiag_TYPE_MISMATCH);
            }
            nextArgNode = HOPAstNextSibling(c->ast, msgArgNode);
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCGetEffectiveContextFieldTypeByLiteral(c, "logger", &logType) != 0) {
                return -1;
            }
            *outType = c->typeVoid;
            return 0;
        }
        {
            HOPTCCallArgInfo callArgs[HOPTC_MAX_CALL_ARGS];
            uint32_t         argCount = 0;
            int32_t          resolvedFn = -1;
            int32_t          mutRefTempArgNode = -1;
            int              status;
            if (HOPTCCollectCallArgInfo(c, nodeId, calleeNode, 0, -1, callArgs, NULL, &argCount)
                != 0)
            {
                return -1;
            }
            status = HOPTCResolveCallByName(
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
                    dependentStatus = HOPTCResolveDependentPtrReturnForCall(
                        c, resolvedFn, callArgs[0].exprNode, &dependentReturnType);
                }
                if (dependentStatus < 0) {
                    return -1;
                }
                if (HOPTCValidateCallContextRequirements(c, c->funcs[resolvedFn].contextType) != 0)
                {
                    return -1;
                }
                if (HOPTCRecordCallTarget(c, nodeId, resolvedFn) != 0) {
                    return -1;
                }
                HOPTCMarkFunctionUsed(c, resolvedFn);
                *outType =
                    dependentStatus == 1 ? dependentReturnType : c->funcs[resolvedFn].returnType;
                return 0;
            }
            if (status == 2) {
                return HOPTCFailSpan(
                    c, HOPDiag_NO_MATCHING_OVERLOAD, callee->dataStart, callee->dataEnd);
            }
            if (status == 3) {
                return HOPTCFailSpan(c, HOPDiag_AMBIGUOUS_CALL, callee->dataStart, callee->dataEnd);
            }
            if (status == 4) {
                return HOPTCFailNode(c, mutRefTempArgNode, HOPDiag_COMPOUND_MUT_REF_TEMPORARY);
            }
            if (status == 5) {
                return HOPTCFailSpan(
                    c, HOPDiag_COMPOUND_INFER_AMBIGUOUS, callee->dataStart, callee->dataEnd);
            }
            if (status == 6) {
                return -1;
            }
        }
    } else if (c->ast->nodes[calleeNode].kind == HOPAst_FIELD_EXPR) {
        const HOPAstNode* callee = &c->ast->nodes[calleeNode];
        int32_t           recvNode = HOPAstFirstChild(c->ast, calleeNode);
        int32_t           recvType;
        int32_t           fieldType;
        if (recvNode >= 0 && (uint32_t)recvNode < c->ast->len
            && c->ast->nodes[recvNode].kind == HOPAst_IDENT)
        {
            const HOPAstNode* recv = &c->ast->nodes[recvNode];
            if (HOPNameEqLiteral(c->src, recv->dataStart, recv->dataEnd, "builtin")
                && HOPTCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd))
            {
                return HOPTCTypeSourceLocationOfCall(c, nodeId, callee, outType);
            }
            if (HOPNameEqLiteral(c->src, recv->dataStart, recv->dataEnd, "compiler")) {
                HOPTCCompilerDiagOp diagOp = HOPTCCompilerDiagOpFromName(
                    c, callee->dataStart, callee->dataEnd);
                if (diagOp != HOPTCCompilerDiagOp_NONE) {
                    return HOPTCTypeCompilerDiagCall(c, nodeId, callee, diagOp, outType);
                }
            }
        }
        if (recvNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_NOT_CALLABLE);
        }
        if (HOPTCTypeExpr(c, recvNode, &recvType) != 0) {
            return -1;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t kindType;
            int32_t u8Type;
            if (nextArgNode < 0 && recvType == c->typeType) {
                kindType = HOPTCFindReflectKindType(c);
                if (kindType >= 0) {
                    *outType = kindType;
                    return 0;
                }
                u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
                if (u8Type < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                }
                *outType = u8Type;
                return 0;
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            if (nextArgNode < 0 && recvType == c->typeType) {
                if (c->typeBool < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                }
                *outType = c->typeBool;
                return 0;
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t strRefType;
            if (nextArgNode < 0 && recvType == c->typeType) {
                strRefType = HOPTCGetStrRefType(c, callee->start, callee->end);
                if (strRefType < 0) {
                    return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
                }
                *outType = strRefType;
                return 0;
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t reflectedTypeId;
            if (nextArgNode < 0 && recvType == c->typeType) {
                if (HOPTCResolveReflectedTypeValueExpr(c, recvNode, &reflectedTypeId) != 0) {
                    return HOPTCFailNode(c, recvNode, HOPDiag_TYPE_MISMATCH);
                }
                if (c->types[reflectedTypeId].kind != HOPTCType_ALIAS) {
                    return HOPTCFailNode(c, recvNode, HOPDiag_TYPE_MISMATCH);
                }
                if (HOPTCResolveAliasTypeId(c, reflectedTypeId) != 0) {
                    return -1;
                }
                if (c->typeType < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                }
                *outType = c->typeType;
                return 0;
            }
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len")
            && HOPTCTypeSupportsLen(c, recvType))
        {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t intType;
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            intType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
            if (intType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (HOPTCFieldLookup(c, recvType, callee->dataStart, callee->dataEnd, &fieldType, NULL)
            == 0)
        {
            calleeType = fieldType;
            goto typed_call_from_callee_type;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "cstr")) {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t u8Type;
            int32_t u8RefType;
            int32_t wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, recvType)) {
                return HOPTCFailNode(c, recvNode, HOPDiag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
            if (u8Type < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            u8RefType = HOPTCInternRefType(c, u8Type, 0, callee->start, callee->end);
            if (u8RefType < 0) {
                return -1;
            }
            *outType = u8RefType;
            return 0;
        }
        if (0 && HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "free")) {
            int32_t valueArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t nextArgNode = valueArgNode >= 0 ? HOPAstNextSibling(c->ast, valueArgNode) : -1;
            int32_t allocBaseType;
            int32_t allocParamType;
            int32_t valueType;
            if (valueArgNode < 0 || nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            allocBaseType = HOPTCFindMemAllocatorType(c);
            if (allocBaseType < 0) {
                return HOPTCFailNode(c, recvNode, HOPDiag_TYPE_MISMATCH);
            }
            allocParamType = HOPTCInternRefType(c, allocBaseType, 1, callee->start, callee->end);
            if (allocParamType < 0) {
                return -1;
            }
            if (!HOPTCCanAssign(c, allocParamType, recvType)) {
                return HOPTCFailNode(c, recvNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCTypeExpr(c, valueArgNode, &valueType) != 0) {
                return -1;
            }
            if (!HOPTCTypeIsFreeablePointer(c, valueType)) {
                return HOPTCFailNode(c, valueArgNode, HOPDiag_TYPE_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "panic")) {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, recvType)) {
                return HOPTCFailNode(c, recvNode, HOPDiag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "print")) {
            int32_t nextArgNode = HOPAstNextSibling(c->ast, calleeNode);
            int32_t logType;
            int32_t wantStrType = HOPTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return HOPTCFailNode(c, calleeNode, HOPDiag_UNKNOWN_TYPE);
            }
            if (!HOPTCCanAssign(c, wantStrType, recvType)) {
                return HOPTCFailNode(c, recvNode, HOPDiag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            if (HOPTCGetEffectiveContextFieldTypeByLiteral(c, "logger", &logType) != 0) {
                return -1;
            }
            *outType = c->typeVoid;
            return 0;
        }

        {
            HOPTCCallArgInfo callArgs[HOPTC_MAX_CALL_ARGS];
            uint32_t         argCount = 0;
            int32_t          resolvedFn = -1;
            int32_t          mutRefTempArgNode = -1;
            int              status;
            int              recvPkgStatus = 0;
            uint32_t         recvPkgStart = 0;
            uint32_t         recvPkgEnd = 0;
            if (HOPTCCollectCallArgInfo(
                    c, nodeId, calleeNode, 1, recvNode, callArgs, NULL, &argCount)
                != 0)
            {
                return -1;
            }
            status = HOPTCResolveCallByName(
                c,
                callee->dataStart,
                callee->dataEnd,
                callArgs,
                argCount,
                1,
                0,
                &resolvedFn,
                &mutRefTempArgNode);
            if (status == 2) {
                status = HOPTCResolveCallByName(
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
            recvPkgStatus = HOPTCResolveReceiverPkgPrefix(c, recvType, &recvPkgStart, &recvPkgEnd);
            if (recvPkgStatus < 0) {
                return -1;
            }
            if ((status == 1 || status == 2) && recvPkgStatus == 1) {
                int prefixedStatus = HOPTCResolveCallByPkgMethod(
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
                if (prefixedStatus == 2) {
                    prefixedStatus = HOPTCResolveCallByPkgMethod(
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
                    dependentStatus = HOPTCResolveDependentPtrReturnForCall(
                        c, resolvedFn, callArgs[0].exprNode, &dependentReturnType);
                }
                if (dependentStatus < 0) {
                    return -1;
                }
                if (HOPTCValidateCallContextRequirements(c, c->funcs[resolvedFn].contextType) != 0)
                {
                    return -1;
                }
                if (HOPTCRecordCallTarget(c, nodeId, resolvedFn) != 0) {
                    return -1;
                }
                HOPTCMarkFunctionUsed(c, resolvedFn);
                *outType =
                    dependentStatus == 1 ? dependentReturnType : c->funcs[resolvedFn].returnType;
                return 0;
            }
            if (status == 2) {
                return HOPTCFailSpan(
                    c, HOPDiag_NO_MATCHING_OVERLOAD, callee->dataStart, callee->dataEnd);
            }
            if (status == 3) {
                return HOPTCFailSpan(c, HOPDiag_AMBIGUOUS_CALL, callee->dataStart, callee->dataEnd);
            }
            if (status == 4) {
                return HOPTCFailNode(c, mutRefTempArgNode, HOPDiag_COMPOUND_MUT_REF_TEMPORARY);
            }
            if (status == 5) {
                return HOPTCFailSpan(
                    c, HOPDiag_COMPOUND_INFER_AMBIGUOUS, callee->dataStart, callee->dataEnd);
            }
            if (status == 6) {
                return -1;
            }
            return HOPTCFailSpan(c, HOPDiag_UNKNOWN_SYMBOL, callee->dataStart, callee->dataEnd);
        }
    }
    if (HOPTCTypeExpr(c, calleeNode, &calleeType) != 0) {
        return -1;
    }
typed_call_from_callee_type: {
    int32_t           fnReturnType;
    uint32_t          fnParamStart = 0;
    uint32_t          fnParamCount = 0;
    int               fnIsVariadic = 0;
    int32_t           fnIndexForDependent = -1;
    HOPTCCallArgInfo  callArgs[HOPTC_MAX_CALL_ARGS];
    uint32_t          paramNameStarts[HOPTC_MAX_CALL_ARGS];
    uint32_t          paramNameEnds[HOPTC_MAX_CALL_ARGS];
    uint8_t           paramFlags[HOPTC_MAX_CALL_ARGS];
    uint32_t          callArgCount = 0;
    uint32_t          p;
    int               hasParamNames = 1;
    int               prepStatus;
    HOPTCCallMapError mapError;
    HOPTCCallBinding  binding;
    if (HOPTCGetFunctionTypeSignature(
            c, calleeType, &fnReturnType, &fnParamStart, &fnParamCount, &fnIsVariadic)
        != 0)
    {
        return HOPTCFailNode(c, calleeNode, HOPDiag_NOT_CALLABLE);
    }
    if (HOPTCCollectCallArgInfo(c, nodeId, calleeNode, 0, -1, callArgs, NULL, &callArgCount) != 0) {
        return -1;
    }
    if (!fnIsVariadic && callArgCount != fnParamCount) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
    if (fnIsVariadic && callArgCount < (fnParamCount > 0 ? (fnParamCount - 1u) : 0u)) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
    for (p = 0; p < fnParamCount; p++) {
        paramNameStarts[p] = c->funcParamNameStarts[fnParamStart + p];
        paramNameEnds[p] = c->funcParamNameEnds[fnParamStart + p];
        paramFlags[p] = c->funcParamFlags[fnParamStart + p];
    }
    if (calleeType >= 0 && (uint32_t)calleeType < c->typeLen && c->types[calleeType].funcIndex >= 0
        && (uint32_t)c->types[calleeType].funcIndex < c->funcLen)
    {
        const HOPTCFunction* fn = &c->funcs[c->types[calleeType].funcIndex];
        fnIndexForDependent = c->types[calleeType].funcIndex;
        if (HOPTCRecordCallTarget(c, nodeId, fnIndexForDependent) != 0) {
            return -1;
        }
        if (fn->paramCount == fnParamCount
            && (((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0) == (fnIsVariadic != 0)))
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
    HOPTCCallMapErrorClear(&mapError);
    prepStatus = HOPTCPrepareCallBinding(
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
            HOPTCSetDiagWithArg(
                c->diag,
                mapError.code,
                mapError.start,
                mapError.end,
                mapError.argStart,
                mapError.argEnd);
            return -1;
        }
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
    for (p = 0; p < callArgCount; p++) {
        int32_t argType;
        int32_t paramType;
        int32_t argExprNode = callArgs[p].exprNode;
        paramType = binding.argExpectedTypes[p];
        if (paramType < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        if (HOPTCTypeExprExpected(c, argExprNode, paramType, &argType) != 0) {
            return -1;
        }
        if (fnIsVariadic && p == binding.spreadArgIndex
            && binding.variadicParamType == c->typeAnytype)
        {
            int32_t spreadType = HOPTCResolveAliasBaseType(c, argType);
            if (spreadType < 0 || (uint32_t)spreadType >= c->typeLen
                || c->types[spreadType].kind != HOPTCType_PACK)
            {
                return HOPTCFailNode(c, argExprNode, HOPDiag_ANYTYPE_SPREAD_REQUIRES_PACK);
            }
        }
        if (HOPTCIsMutableRefType(c, paramType) && HOPTCExprIsCompoundTemporary(c, argExprNode)) {
            return HOPTCFailNode(c, argExprNode, HOPDiag_COMPOUND_MUT_REF_TEMPORARY);
        }
        if (!HOPTCCanAssign(c, paramType, argType)) {
            if (fnIsVariadic && p >= binding.fixedInputCount) {
                return HOPTCFailNode(
                    c,
                    argExprNode,
                    binding.spreadArgIndex == p
                        ? HOPDiag_VARIADIC_SPREAD_NON_SLICE
                        : HOPDiag_VARIADIC_ARG_TYPE_MISMATCH);
            }
            return HOPTCFailNode(c, argExprNode, HOPDiag_TYPE_MISMATCH);
        }
    }
    HOPTCCallMapErrorClear(&mapError);
    {
        int constStatus = HOPTCCheckConstParamArgs(
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
            HOPTCSetDiagWithArg(
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
        int     dependentStatus = HOPTCResolveDependentPtrReturnForCall(
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

int HOPTCTypeExpr_CAST(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t          exprNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t          typeNode;
    int32_t          sourceType;
    int32_t          resolvedSourceType;
    int32_t          targetType;
    int32_t          resolvedTargetType;
    const HOPTCType* src;
    const HOPTCType* dst;
    (void)n;
    if (exprNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    typeNode = HOPAstNextSibling(c->ast, exprNode);
    if (typeNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
    }
    if (HOPTCTypeExpr(c, exprNode, &sourceType) != 0) {
        return -1;
    }
    c->allowConstNumericTypeName = 1;
    if (HOPTCResolveTypeNode(c, typeNode, &targetType) != 0) {
        c->allowConstNumericTypeName = 0;
        return -1;
    }
    c->allowConstNumericTypeName = 0;
    resolvedSourceType = HOPTCResolveAliasBaseType(c, sourceType);
    resolvedTargetType = HOPTCResolveAliasBaseType(c, targetType);
    if (resolvedSourceType < 0 || (uint32_t)resolvedSourceType >= c->typeLen
        || resolvedTargetType < 0 || (uint32_t)resolvedTargetType >= c->typeLen)
    {
        return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }
    src = &c->types[resolvedSourceType];
    dst = &c->types[resolvedTargetType];
    if (src->kind == HOPTCType_NULL
        && !(dst->kind == HOPTCType_OPTIONAL || HOPTCIsRawptrType(c, resolvedTargetType)))
    {
        return HOPTCFailInvalidCast(c, nodeId, sourceType, targetType);
    }
    if (HOPTCIsRawptrType(c, resolvedTargetType)) {
        if (!(src->kind == HOPTCType_NULL || HOPTCIsRawptrType(c, resolvedSourceType)
              || src->kind == HOPTCType_PTR || src->kind == HOPTCType_REF))
        {
            return HOPTCFailInvalidCast(c, nodeId, sourceType, targetType);
        }
    } else if (HOPTCIsRawptrType(c, resolvedSourceType)) {
        if (!(dst->kind == HOPTCType_PTR || dst->kind == HOPTCType_REF
              || HOPTCIsRawptrType(c, resolvedTargetType)))
        {
            return HOPTCFailInvalidCast(c, nodeId, sourceType, targetType);
        }
    }
    *outType = targetType;
    return 0;
}

int HOPTCTypeExpr_SIZEOF(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t innerNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t innerType;
    if (innerNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    if (n->flags == 1) {
        if (HOPTCResolveTypeNode(c, innerNode, &innerType) == 0) {
            if (HOPTCTypeContainsVarSizeByValue(c, innerType)) {
                return HOPTCFailNode(c, innerNode, HOPDiag_TYPE_MISMATCH);
            }
            *outType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
            if (*outType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
            return 0;
        }
        if (c->ast->nodes[innerNode].kind == HOPAst_TYPE_NAME) {
            int32_t localIdx = HOPTCLocalFind(
                c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
            if (localIdx >= 0) {
                if (c->diag != NULL) {
                    *c->diag = (HOPDiag){ 0 };
                }
                *outType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
                if (*outType < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                }
                return 0;
            }
            {
                int32_t fnIdx = HOPTCFindFunctionIndex(
                    c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                if (fnIdx >= 0) {
                    if (c->diag != NULL) {
                        *c->diag = (HOPDiag){ 0 };
                    }
                    *outType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
                    if (*outType < 0) {
                        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
                    }
                    return 0;
                }
            }
        }
    } else {
        if (HOPTCTypeExpr(c, innerNode, &innerType) != 0) {
            return -1;
        }
    }
    *outType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
    if (*outType < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
    }
    return 0;
}

int HOPTCTypeExpr_FIELD_EXPR(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t           recvNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t           recvType = -1;
    int32_t           fieldType = -1;
    int32_t           fnIndex;
    int32_t           localIdx;
    const HOPAstNode* recv;
    if (recvNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_SYMBOL);
    }
    recv = &c->ast->nodes[recvNode];
    if (((recv->kind == HOPAst_IDENT && HOPTCLocalFind(c, recv->dataStart, recv->dataEnd) < 0
          && HOPTCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) < 0)
         || recv->kind == HOPAst_FIELD_EXPR)
        && HOPTCResolveEnumMemberType(c, recvNode, n->dataStart, n->dataEnd, &fieldType))
    {
        *outType = fieldType;
        return 0;
    }
    localIdx = recv->kind == HOPAst_IDENT ? HOPTCLocalFind(c, recv->dataStart, recv->dataEnd) : -1;
    if (localIdx >= 0) {
        const HOPTCVariantNarrow* narrow;
        if (HOPTCVariantNarrowFind(c, localIdx, &narrow)
            && HOPTCEnumVariantPayloadFieldType(
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
    if (recv->kind == HOPAst_IDENT && localIdx < 0
        && HOPTCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) < 0)
    {
        fnIndex = HOPTCFindPkgQualifiedFunctionValueIndex(
            c, recv->dataStart, recv->dataEnd, n->dataStart, n->dataEnd);
        if (fnIndex >= 0) {
            HOPTCMarkFunctionUsed(c, fnIndex);
            *outType = c->funcs[(uint32_t)fnIndex].funcTypeId;
            return 0;
        }
    }
    if (HOPTCTypeExpr(c, recvNode, &recvType) != 0) {
        return -1;
    }
    if (HOPTCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, NULL) != 0) {
        return HOPTCFailSpan(c, HOPDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
    }
    *outType = fieldType;
    return 0;
}

static int32_t HOPTCFindParentNode(const HOPAst* ast, int32_t childNodeId) {
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

static int HOPTCAllowRuntimeAnyPackIndex(HOPTypeCheckCtx* c, int32_t indexNodeId) {
    int32_t           parentNodeId;
    const HOPAstNode* parent;
    if (c == NULL || c->ast == NULL) {
        return 0;
    }
    parentNodeId = HOPTCFindParentNode(c->ast, indexNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    parent = &c->ast->nodes[parentNodeId];
    if (parent->kind == HOPAst_CAST) {
        return 1;
    }
    if (parent->kind == HOPAst_CALL_ARG) {
        int32_t           callNodeId = HOPTCFindParentNode(c->ast, parentNodeId);
        const HOPAstNode* callNode;
        int32_t           calleeNodeId;
        const HOPAstNode* calleeNode;
        if (callNodeId < 0 || (uint32_t)callNodeId >= c->ast->len) {
            return 0;
        }
        callNode = &c->ast->nodes[callNodeId];
        if (callNode->kind != HOPAst_CALL) {
            return 0;
        }
        calleeNodeId = HOPAstFirstChild(c->ast, callNodeId);
        if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= c->ast->len) {
            return 0;
        }
        calleeNode = &c->ast->nodes[calleeNodeId];
        if (calleeNode->kind == HOPAst_IDENT
            && HOPNameEqLiteral(c->src, calleeNode->dataStart, calleeNode->dataEnd, "typeof"))
        {
            return 1;
        }
    }
    return 0;
}

int HOPTCTypeExpr_INDEX(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t            baseNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t            baseType;
    int32_t            resolvedBaseType;
    HOPTCIndexBaseInfo info;
    if (baseNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    if (HOPTCTypeExpr(c, baseNode, &baseType) != 0) {
        return -1;
    }
    resolvedBaseType = HOPTCResolveAliasBaseType(c, baseType);
    if (resolvedBaseType >= 0 && (uint32_t)resolvedBaseType < c->typeLen
        && c->types[resolvedBaseType].kind == HOPTCType_PACK)
    {
        int32_t idxNode = HOPAstNextSibling(c->ast, baseNode);
        int32_t idxType;
        int64_t idxValue = 0;
        int     idxIsConst = 0;
        if ((n->flags & HOPAstFlag_INDEX_SLICE) != 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }
        if (idxNode < 0 || HOPAstNextSibling(c->ast, idxNode) >= 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        if (HOPTCTypeExpr(c, idxNode, &idxType) != 0) {
            return -1;
        }
        if (!HOPTCIsIntegerType(c, idxType)) {
            return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
        }
        if (HOPTCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
            return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
        }
        if (!idxIsConst) {
            if (!HOPTCAllowRuntimeAnyPackIndex(c, nodeId)) {
                return HOPTCFailNode(c, idxNode, HOPDiag_ANYTYPE_PACK_INDEX_NOT_CONST);
            }
            *outType = c->typeAnytype;
            return 0;
        }
        if (idxValue < 0 || (uint64_t)idxValue >= c->types[resolvedBaseType].fieldCount) {
            return HOPTCFailNode(c, idxNode, HOPDiag_ANYTYPE_PACK_INDEX_OOB);
        }
        *outType = c->funcParamTypes[c->types[resolvedBaseType].fieldStart + (uint32_t)idxValue];
        return 0;
    }
    if (HOPTCResolveIndexBaseInfo(c, baseType, &info) != 0 || !info.indexable || info.elemType < 0)
    {
        return HOPTCFailNode(c, baseNode, HOPDiag_TYPE_MISMATCH);
    }

    if ((n->flags & HOPAstFlag_INDEX_SLICE) != 0) {
        int     hasStart = (n->flags & HOPAstFlag_INDEX_HAS_START) != 0;
        int     hasEnd = (n->flags & HOPAstFlag_INDEX_HAS_END) != 0;
        int32_t child = HOPAstNextSibling(c->ast, baseNode);
        int32_t startNode = -1;
        int32_t endNode = -1;
        int32_t sliceType;
        int64_t startValue = 0;
        int64_t endValue = 0;
        int     startIsConst = 0;
        int     endIsConst = 0;

        if (!info.sliceable) {
            return HOPTCFailUnsliceableExpr(c, nodeId, baseType);
        }

        if (hasStart) {
            int32_t startType;
            startNode = child;
            if (startNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            if (HOPTCTypeExpr(c, startNode, &startType) != 0) {
                return -1;
            }
            if (!HOPTCIsIntegerType(c, startType)) {
                return HOPTCFailNode(c, startNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCConstIntExpr(c, startNode, &startValue, &startIsConst) != 0) {
                return HOPTCFailNode(c, startNode, HOPDiag_TYPE_MISMATCH);
            }
            child = HOPAstNextSibling(c->ast, child);
        }
        if (hasEnd) {
            int32_t endType;
            endNode = child;
            if (endNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            if (HOPTCTypeExpr(c, endNode, &endType) != 0) {
                return -1;
            }
            if (!HOPTCIsIntegerType(c, endType)) {
                return HOPTCFailNode(c, endNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCConstIntExpr(c, endNode, &endValue, &endIsConst) != 0) {
                return HOPTCFailNode(c, endNode, HOPDiag_TYPE_MISMATCH);
            }
            child = HOPAstNextSibling(c->ast, child);
        }
        if (child >= 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }

        if ((startIsConst && startValue < 0) || (endIsConst && endValue < 0)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }

        if (info.hasKnownLen) {
            int     startKnown = !hasStart || startIsConst;
            int     endKnown = !hasEnd || endIsConst;
            int64_t startBound = hasStart ? startValue : 0;
            int64_t endBound = hasEnd ? endValue : (int64_t)info.knownLen;
            if (startKnown && endKnown) {
                if (startBound > endBound || endBound > (int64_t)info.knownLen) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
                }
            } else {
                HOPTCMarkRuntimeBoundsCheck(c, nodeId);
            }
        } else {
            if (startIsConst && endIsConst && startValue > endValue) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            HOPTCMarkRuntimeBoundsCheck(c, nodeId);
        }

        if (info.isStringLike) {
            *outType = HOPTCGetStringSliceExprType(c, baseType, n->start, n->end);
            if (*outType < 0) {
                return -1;
            }
        } else {
            sliceType = HOPTCInternSliceType(c, info.elemType, info.sliceMutable, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            if (info.sliceMutable) {
                *outType = HOPTCInternPtrType(c, sliceType, n->start, n->end);
            } else {
                *outType = HOPTCInternRefType(c, sliceType, 0, n->start, n->end);
            }
            if (*outType < 0) {
                return -1;
            }
        }
        return 0;
    }

    {
        int32_t idxNode = HOPAstNextSibling(c->ast, baseNode);
        int32_t idxType;
        int64_t idxValue = 0;
        int     idxIsConst = 0;

        if (idxNode < 0 || HOPAstNextSibling(c->ast, idxNode) >= 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        if (HOPTCTypeExpr(c, idxNode, &idxType) != 0) {
            return -1;
        }
        if (!HOPTCIsIntegerType(c, idxType)) {
            return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
        }
        if (HOPTCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
            return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
        }
        if (idxIsConst && idxValue < 0) {
            return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
        }

        if (info.hasKnownLen) {
            if (idxIsConst) {
                if (idxValue >= (int64_t)info.knownLen) {
                    return HOPTCFailNode(c, idxNode, HOPDiag_TYPE_MISMATCH);
                }
            } else {
                HOPTCMarkRuntimeBoundsCheck(c, nodeId);
            }
        } else if (info.sliceable) {
            HOPTCMarkRuntimeBoundsCheck(c, nodeId);
        }
    }

    *outType = info.elemType;
    return 0;
}

int HOPTCTypeExpr_UNARY(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t rhsNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t rhsType;
    if (rhsNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    if (HOPTCTypeExpr(c, rhsNode, &rhsType) != 0) {
        return -1;
    }
    switch ((HOPTokenKind)n->op) {
        case HOPTok_ADD:
        case HOPTok_SUB:
            if (!HOPTCIsNumericType(c, rhsType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            *outType = rhsType;
            return 0;
        case HOPTok_NOT:
            if (!HOPTCIsBoolType(c, rhsType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_BOOL);
            }
            *outType = c->typeBool;
            return 0;
        case HOPTok_MUL:
            if (c->types[rhsType].kind != HOPTCType_PTR && c->types[rhsType].kind != HOPTCType_REF)
            {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            *outType = c->types[rhsType].baseType;
            return 0;
        case HOPTok_AND: {
            int32_t ptrType = HOPTCInternPtrType(c, rhsType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        default: return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }
}

int HOPTCTypeExpr_BINARY(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t      lhsNode = HOPAstFirstChild(c->ast, nodeId);
    int32_t      rhsNode;
    int32_t      lhsType;
    int32_t      rhsType;
    int32_t      commonType;
    int32_t      hookFn = -1;
    HOPTokenKind op = (HOPTokenKind)n->op;
    if (lhsNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    rhsNode = HOPAstNextSibling(c->ast, lhsNode);
    if (rhsNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    if (op == HOPTok_ASSIGN && c->ast->nodes[lhsNode].kind == HOPAst_IDENT
        && HOPNameEqLiteral(
            c->src, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, "_"))
    {
        if (HOPTCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
            if (HOPTCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                return -1;
            }
        }
        if (rhsType == c->typeVoid) {
            return HOPTCFailNode(c, rhsNode, HOPDiag_TYPE_MISMATCH);
        }
        *outType = rhsType;
        return 0;
    }
    if (op == HOPTok_ADD && HOPIsStringLiteralConcatChain(c->ast, nodeId)) {
        int32_t strRefType = HOPTCGetStrRefType(c, n->start, n->end);
        if (strRefType < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
        }
        *outType = strRefType;
        return 0;
    }
    if (op == HOPTok_ASSIGN || op == HOPTok_ADD_ASSIGN || op == HOPTok_SUB_ASSIGN
        || op == HOPTok_MUL_ASSIGN || op == HOPTok_DIV_ASSIGN || op == HOPTok_MOD_ASSIGN
        || op == HOPTok_AND_ASSIGN || op == HOPTok_OR_ASSIGN || op == HOPTok_XOR_ASSIGN
        || op == HOPTok_LSHIFT_ASSIGN || op == HOPTok_RSHIFT_ASSIGN)
    {
        int skipDirectIdentRead = op == HOPTok_ASSIGN;
        if (HOPTCTypeAssignTargetExpr(c, lhsNode, skipDirectIdentRead, &lhsType) != 0
            || HOPTCTypeExprExpected(c, rhsNode, lhsType, &rhsType) != 0)
        {
            return -1;
        }
        if (!HOPTCExprIsAssignable(c, lhsNode)) {
            return HOPTCFailNode(c, lhsNode, HOPDiag_TYPE_MISMATCH);
        }
        if (HOPTCExprIsConstAssignTarget(c, lhsNode)) {
            return HOPTCFailAssignToConst(c, lhsNode);
        }
        if (!HOPTCCanAssign(c, lhsType, rhsType)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }
        if (op != HOPTok_ASSIGN && !HOPTCIsNumericType(c, lhsType)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }
        HOPTCMarkDirectIdentLocalWrite(c, lhsNode, op == HOPTok_ASSIGN);
        *outType = lhsType;
        return 0;
    }
    if (HOPTCTypeExpr(c, lhsNode, &lhsType) != 0
        || HOPTCTypeExprExpected(c, rhsNode, lhsType, &rhsType) != 0)
    {
        return -1;
    }

    if (op == HOPTok_LOGICAL_AND || op == HOPTok_LOGICAL_OR) {
        if (!HOPTCIsBoolType(c, lhsType) || !HOPTCIsBoolType(c, rhsType)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_BOOL);
        }
        *outType = c->typeBool;
        return 0;
    }

    /* Allow ?T == null, null == ?T, rawptr == null, null == rawptr, and != variants. */
    if (op == HOPTok_EQ || op == HOPTok_NEQ) {
        int lhsIsOpt = c->types[lhsType].kind == HOPTCType_OPTIONAL;
        int rhsIsOpt = c->types[rhsType].kind == HOPTCType_OPTIONAL;
        int lhsIsNull = c->types[lhsType].kind == HOPTCType_NULL;
        int rhsIsNull = c->types[rhsType].kind == HOPTCType_NULL;
        int lhsIsRawptr = HOPTCIsRawptrType(c, lhsType);
        int rhsIsRawptr = HOPTCIsRawptrType(c, rhsType);
        if ((lhsIsOpt && rhsIsNull) || (lhsIsNull && rhsIsOpt) || (lhsIsRawptr && rhsIsNull)
            || (lhsIsNull && rhsIsRawptr))
        {
            *outType = c->typeBool;
            return 0;
        }
    }

    if (op == HOPTok_EQ || op == HOPTok_NEQ || op == HOPTok_LT || op == HOPTok_GT
        || op == HOPTok_LTE || op == HOPTok_GTE)
    {
        int hookStatus = HOPTCResolveComparisonHook(
            c,
            (op == HOPTok_EQ || op == HOPTok_NEQ) ? "__equal" : "__order",
            lhsType,
            rhsType,
            &hookFn);
        if (hookStatus == 0) {
            HOPTCMarkFunctionUsed(c, hookFn);
            *outType = c->typeBool;
            return 0;
        }
        if (hookStatus == 3) {
            return HOPTCFailNode(c, nodeId, HOPDiag_AMBIGUOUS_CALL);
        }
        if (HOPTCCoerceForBinary(c, lhsType, rhsType, &commonType) != 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }
        if (op == HOPTok_EQ || op == HOPTok_NEQ) {
            if (!HOPTCIsComparableType(c, commonType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_NOT_COMPARABLE);
            }
        } else {
            if (!HOPTCIsOrderedType(c, commonType)) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_NOT_ORDERED);
            }
        }
        *outType = c->typeBool;
        return 0;
    }

    if (op == HOPTok_ADD
        && (HOPTCIsStringLikeType(c, lhsType) || HOPTCIsStringLikeType(c, rhsType)))
    {
        return HOPTCFailNode(c, nodeId, HOPDiag_STRING_CONCAT_LITERAL_ONLY);
    }

    if (HOPTCCoerceForBinary(c, lhsType, rhsType, &commonType) != 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }

    if (!HOPTCIsNumericType(c, commonType)) {
        return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }
    *outType = commonType;
    return 0;
}

int HOPTCTypeExpr_NULL(HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeNull;
    return 0;
}

int HOPTCTypeExpr_UNWRAP(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t inner = HOPAstFirstChild(c->ast, nodeId);
    int32_t innerType;
    (void)n;
    if (inner < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    if (HOPTCTypeExpr(c, inner, &innerType) != 0) {
        return -1;
    }
    if (c->types[innerType].kind != HOPTCType_OPTIONAL) {
        return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
    }
    *outType = c->types[innerType].baseType;
    return 0;
}

int HOPTCTypeExpr_TUPLE_EXPR(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPAstNode* n, int32_t* outType) {
    int32_t  child = HOPAstFirstChild(c->ast, nodeId);
    uint32_t elemCount = 0;
    (void)n;
    while (child >= 0) {
        int32_t elemType;
        if (elemCount >= c->scratchParamCap) {
            return HOPTCFailNode(c, child, HOPDiag_ARENA_OOM);
        }
        if (HOPTCTypeExpr(c, child, &elemType) != 0) {
            return -1;
        }
        if (elemType == c->typeNull) {
            return HOPTCFailNode(c, child, HOPDiag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (elemType == c->typeVoid) {
            return HOPTCFailNode(c, child, HOPDiag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (HOPTCConcretizeInferredType(c, elemType, &elemType) != 0) {
            return -1;
        }
        c->scratchParamTypes[elemCount++] = elemType;
        child = HOPAstNextSibling(c->ast, child);
    }
    if (elemCount < 2u) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    *outType = HOPTCInternTupleType(c, c->scratchParamTypes, elemCount, n->start, n->end);
    return *outType < 0 ? -1 : 0;
}

int HOPTCTypeExpr(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return HOPTCFailSpan(c, HOPDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case HOPAst_IDENT:        return HOPTCTypeExpr_IDENT(c, nodeId, n, outType);
        case HOPAst_TYPE_VALUE:   return HOPTCTypeExpr_TYPE_VALUE(c, nodeId, n, outType);
        case HOPAst_INT:          return HOPTCTypeExpr_INT(c, nodeId, n, outType);
        case HOPAst_FLOAT:        return HOPTCTypeExpr_FLOAT(c, nodeId, n, outType);
        case HOPAst_STRING:       return HOPTCTypeExpr_STRING(c, nodeId, n, outType);
        case HOPAst_RUNE:         return HOPTCTypeExpr_RUNE(c, nodeId, n, outType);
        case HOPAst_BOOL:         return HOPTCTypeExpr_BOOL(c, nodeId, n, outType);
        case HOPAst_COMPOUND_LIT: return HOPTCTypeExpr_COMPOUND_LIT(c, nodeId, n, outType);
        case HOPAst_CALL_WITH_CONTEXT:
            return HOPTCTypeExpr_CALL_WITH_CONTEXT(c, nodeId, n, outType);
        case HOPAst_NEW:      return HOPTCTypeExpr_NEW(c, nodeId, n, outType);
        case HOPAst_CALL:     return HOPTCTypeExpr_CALL(c, nodeId, n, outType);
        case HOPAst_CALL_ARG: {
            int32_t inner = HOPAstFirstChild(c->ast, nodeId);
            if (inner < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            return HOPTCTypeExpr(c, inner, outType);
        }
        case HOPAst_CAST:       return HOPTCTypeExpr_CAST(c, nodeId, n, outType);
        case HOPAst_SIZEOF:     return HOPTCTypeExpr_SIZEOF(c, nodeId, n, outType);
        case HOPAst_FIELD_EXPR: return HOPTCTypeExpr_FIELD_EXPR(c, nodeId, n, outType);
        case HOPAst_INDEX:      return HOPTCTypeExpr_INDEX(c, nodeId, n, outType);
        case HOPAst_UNARY:      return HOPTCTypeExpr_UNARY(c, nodeId, n, outType);
        case HOPAst_BINARY:     return HOPTCTypeExpr_BINARY(c, nodeId, n, outType);
        case HOPAst_NULL:       return HOPTCTypeExpr_NULL(c, nodeId, n, outType);
        case HOPAst_UNWRAP:     return HOPTCTypeExpr_UNWRAP(c, nodeId, n, outType);
        case HOPAst_TUPLE_EXPR: return HOPTCTypeExpr_TUPLE_EXPR(c, nodeId, n, outType);
        default:                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
}

int HOPTCValidateConstInitializerExprNode(HOPTypeCheckCtx* c, int32_t initNode) {
    HOPTCConstEvalCtx evalCtx;
    HOPCTFEValue      value;
    int               isConst = 0;
    int               rc;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (HOPTCEvalConstExprNode(&evalCtx, initNode, &value, &isConst) != 0) {
        return -1;
    }
    c->lastConstEvalReason = evalCtx.nonConstReason;
    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
    if (isConst) {
        return 0;
    }
    if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd
        && c->lastConstEvalReasonEnd <= c->src.len)
    {
        rc = HOPTCFailSpan(
            c,
            HOPDiag_CONST_INIT_CONST_REQUIRED,
            c->lastConstEvalReasonStart,
            c->lastConstEvalReasonEnd);
    } else {
        rc = HOPTCFailNode(c, initNode, HOPDiag_CONST_INIT_CONST_REQUIRED);
    }
    HOPTCAttachConstEvalReason(c);
    return rc;
}

static int HOPTCValidateLocalConstFunctionInitializerExprNode(
    HOPTypeCheckCtx* c, int32_t initNode) {
    const HOPAstNode* init;
    int32_t           initType = -1;
    if (c == NULL || initNode < 0 || (uint32_t)initNode >= c->ast->len) {
        return 0;
    }
    init = &c->ast->nodes[initNode];
    if (init->kind == HOPAst_CALL_ARG) {
        int32_t inner = HOPAstFirstChild(c->ast, initNode);
        return HOPTCValidateLocalConstFunctionInitializerExprNode(c, inner);
    }
    if (init->kind != HOPAst_IDENT && init->kind != HOPAst_FIELD_EXPR) {
        return 0;
    }
    if (HOPTCTypeExpr(c, initNode, &initType) != 0) {
        return 0;
    }
    return initType >= 0 && (uint32_t)initType < c->typeLen
        && c->types[initType].kind == HOPTCType_FUNCTION;
}

int HOPTCValidateLocalConstVarLikeInitializers(
    HOPTypeCheckCtx* c, int32_t nodeId, const HOPTCVarLikeParts* parts) {
    uint32_t i;
    if (parts == NULL || parts->initNode < 0) {
        return 0;
    }
    if (!parts->grouped) {
        int32_t initNode = HOPTCVarLikeInitExprNode(c, nodeId);
        if (initNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        if (HOPTCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
            return 0;
        }
        return HOPTCValidateConstInitializerExprNode(c, initNode);
    }
    if ((uint32_t)parts->initNode >= c->ast->len
        || c->ast->nodes[parts->initNode].kind != HOPAst_EXPR_LIST)
    {
        return HOPTCFailNode(c, parts->initNode, HOPDiag_EXPECTED_EXPR);
    }
    {
        uint32_t initCount = HOPTCListCount(c->ast, parts->initNode);
        if (initCount == parts->nameCount) {
            for (i = 0; i < initCount; i++) {
                int32_t initNode = HOPTCListItemAt(c->ast, parts->initNode, i);
                if (initNode < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
                }
                if (HOPTCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
                    continue;
                }
                if (HOPTCValidateConstInitializerExprNode(c, initNode) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        if (initCount == 1u) {
            int32_t initNode = HOPTCListItemAt(c->ast, parts->initNode, 0);
            if (initNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            if (HOPTCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
                return 0;
            }
            return HOPTCValidateConstInitializerExprNode(c, initNode);
        }
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
}

int HOPTCTypeVarLike(HOPTypeCheckCtx* c, int32_t nodeId) {
    const HOPAstNode* n = &c->ast->nodes[nodeId];
    HOPTCVarLikeParts parts;
    int32_t           declType;
    uint32_t          i;
    int               isConstBinding;

    if (HOPTCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
    }
    isConstBinding = n->kind == HOPAst_CONST;
    if (isConstBinding && parts.initNode < 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_CONST_MISSING_INITIALIZER);
    }
    if (isConstBinding && HOPTCValidateLocalConstVarLikeInitializers(c, nodeId, &parts) != 0) {
        return -1;
    }

    if (!parts.grouped) {
        if (parts.typeNode < 0) {
            int32_t initType;
            if (HOPTCTypeExpr(c, parts.initNode, &initType) != 0) {
                return -1;
            }
            if (initType == c->typeNull) {
                return HOPTCFailNode(c, parts.initNode, HOPDiag_INFER_NULL_TYPE_UNKNOWN);
            }
            if (initType == c->typeVoid) {
                return HOPTCFailNode(c, parts.initNode, HOPDiag_INFER_VOID_TYPE_UNKNOWN);
            }
            if (isConstBinding) {
                declType = initType;
            } else {
                if (HOPTCConcretizeInferredType(c, initType, &declType) != 0) {
                    return -1;
                }
            }
            if (HOPTCTypeContainsVarSizeByValue(c, declType)) {
                return HOPTCFailNode(c, parts.initNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCLocalAdd(
                    c,
                    n->dataStart,
                    n->dataEnd,
                    declType,
                    n->kind == HOPAst_CONST,
                    n->kind == HOPAst_CONST ? parts.initNode : -1)
                != 0)
            {
                return -1;
            }
            HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
            return 0;
        }

        c->allowConstNumericTypeName = n->kind == HOPAst_CONST ? 1u : 0u;
        if (HOPTCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
            c->allowConstNumericTypeName = 0;
            return -1;
        }
        c->allowConstNumericTypeName = 0;
        if (HOPTCTypeContainsVarSizeByValue(c, declType)) {
            if (parts.initNode >= 0) {
                int32_t initType;
                if (HOPTCTypeExprExpected(c, parts.initNode, declType, &initType) != 0) {
                    return -1;
                }
                return HOPTCFailTypeMismatchDetail(
                    c, parts.initNode, parts.initNode, initType, declType);
            }
            return HOPTCFailNode(c, parts.typeNode, HOPDiag_TYPE_MISMATCH);
        }

        if (n->kind == HOPAst_CONST && parts.initNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_CONST_MISSING_INITIALIZER);
        }
        if (n->kind == HOPAst_VAR && parts.initNode < 0 && !HOPTCEnumTypeHasTagZero(c, declType)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }
        if (parts.initNode >= 0) {
            int32_t initType;
            if (HOPTCTypeExprExpected(c, parts.initNode, declType, &initType) != 0) {
                return -1;
            }
            if (!HOPTCCanAssign(c, declType, initType)) {
                return HOPTCFailTypeMismatchDetail(
                    c, parts.initNode, parts.initNode, initType, declType);
            }
        }

        if (HOPTCLocalAdd(
                c,
                n->dataStart,
                n->dataEnd,
                declType,
                n->kind == HOPAst_CONST,
                n->kind == HOPAst_CONST ? parts.initNode : -1)
            != 0)
        {
            return -1;
        }
        if (parts.initNode >= 0) {
            HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
        }
        return 0;
    }

    if (parts.typeNode >= 0) {
        c->allowConstNumericTypeName = n->kind == HOPAst_CONST ? 1u : 0u;
        if (HOPTCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
            c->allowConstNumericTypeName = 0;
            return -1;
        }
        c->allowConstNumericTypeName = 0;
        if (HOPTCTypeContainsVarSizeByValue(c, declType)) {
            return HOPTCFailNode(c, parts.typeNode, HOPDiag_TYPE_MISMATCH);
        }
        if (n->kind == HOPAst_CONST && parts.initNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_CONST_MISSING_INITIALIZER);
        }
        if (n->kind == HOPAst_VAR && parts.initNode < 0 && !HOPTCEnumTypeHasTagZero(c, declType)) {
            return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
        }
        if (parts.initNode >= 0) {
            uint32_t initCount;
            if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != HOPAst_EXPR_LIST) {
                return HOPTCFailNode(c, parts.initNode, HOPDiag_EXPECTED_EXPR);
            }
            initCount = HOPTCListCount(c->ast, parts.initNode);
            if (initCount == parts.nameCount) {
                for (i = 0; i < initCount; i++) {
                    int32_t initNode = HOPTCListItemAt(c->ast, parts.initNode, i);
                    int32_t initType;
                    if (initNode < 0) {
                        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
                    }
                    if (HOPTCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                        return -1;
                    }
                    if (!HOPTCCanAssign(c, declType, initType)) {
                        return HOPTCFailTypeMismatchDetail(
                            c, initNode, initNode, initType, declType);
                    }
                }
            } else if (initCount == 1u) {
                int32_t          initNode = HOPTCListItemAt(c->ast, parts.initNode, 0);
                int32_t          initType;
                const HOPTCType* t;
                if (initNode < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
                }
                if (HOPTCTypeExpr(c, initNode, &initType) != 0) {
                    return -1;
                }
                if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                    return HOPTCFailNode(c, initNode, HOPDiag_TYPE_MISMATCH);
                }
                t = &c->types[initType];
                if (t->kind != HOPTCType_TUPLE || t->fieldCount != parts.nameCount) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t elemType = c->funcParamTypes[t->fieldStart + i];
                    if (!HOPTCCanAssign(c, declType, elemType)) {
                        return HOPTCFailTypeMismatchDetail(
                            c, initNode, initNode, elemType, declType);
                    }
                }
            } else {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
        }
        for (i = 0; i < parts.nameCount; i++) {
            int32_t           nameNode = HOPTCListItemAt(c->ast, parts.nameListNode, i);
            const HOPAstNode* name;
            if (nameNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            name = &c->ast->nodes[nameNode];
            if (!HOPNameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                if (HOPTCLocalAdd(
                        c, name->dataStart, name->dataEnd, declType, n->kind == HOPAst_CONST, -1)
                    != 0)
                {
                    return -1;
                }
                if (parts.initNode >= 0) {
                    HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        }
        return 0;
    }

    if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != HOPAst_EXPR_LIST) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    {
        uint32_t initCount = HOPTCListCount(c->ast, parts.initNode);
        if (initCount == parts.nameCount) {
            for (i = 0; i < parts.nameCount; i++) {
                int32_t           nameNode = HOPTCListItemAt(c->ast, parts.nameListNode, i);
                const HOPAstNode* name;
                int32_t           initNode = HOPTCListItemAt(c->ast, parts.initNode, i);
                int32_t           initType;
                int32_t           inferredType;
                if (nameNode < 0 || initNode < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
                }
                if (HOPTCTypeExpr(c, initNode, &initType) != 0) {
                    return -1;
                }
                if (initType == c->typeNull) {
                    return HOPTCFailNode(c, initNode, HOPDiag_INFER_NULL_TYPE_UNKNOWN);
                }
                if (initType == c->typeVoid) {
                    return HOPTCFailNode(c, initNode, HOPDiag_INFER_VOID_TYPE_UNKNOWN);
                }
                if (isConstBinding) {
                    inferredType = initType;
                } else {
                    if (HOPTCConcretizeInferredType(c, initType, &inferredType) != 0) {
                        return -1;
                    }
                }
                if (HOPTCTypeContainsVarSizeByValue(c, inferredType)) {
                    return HOPTCFailNode(c, initNode, HOPDiag_TYPE_MISMATCH);
                }
                name = &c->ast->nodes[nameNode];
                if (!HOPNameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                    if (HOPTCLocalAdd(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            inferredType,
                            n->kind == HOPAst_CONST,
                            n->kind == HOPAst_CONST ? initNode : -1)
                        != 0)
                    {
                        return -1;
                    }
                    HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        } else if (initCount == 1u) {
            int32_t          initNode = HOPTCListItemAt(c->ast, parts.initNode, 0);
            int32_t          initType;
            const HOPTCType* t;
            if (initNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            if (HOPTCTypeExpr(c, initNode, &initType) != 0) {
                return -1;
            }
            if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                return HOPTCFailNode(c, initNode, HOPDiag_TYPE_MISMATCH);
            }
            t = &c->types[initType];
            if (t->kind != HOPTCType_TUPLE || t->fieldCount != parts.nameCount) {
                return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
            }
            for (i = 0; i < parts.nameCount; i++) {
                int32_t           nameNode = HOPTCListItemAt(c->ast, parts.nameListNode, i);
                const HOPAstNode* name;
                int32_t           inferredType = c->funcParamTypes[t->fieldStart + i];
                if (nameNode < 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
                }
                if (!isConstBinding) {
                    if (HOPTCConcretizeInferredType(c, inferredType, &inferredType) != 0) {
                        return -1;
                    }
                }
                if (HOPTCTypeContainsVarSizeByValue(c, inferredType)) {
                    return HOPTCFailNode(c, initNode, HOPDiag_TYPE_MISMATCH);
                }
                name = &c->ast->nodes[nameNode];
                if (!HOPNameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                    if (HOPTCLocalAdd(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            inferredType,
                            n->kind == HOPAst_CONST,
                            -1)
                        != 0)
                    {
                        return -1;
                    }
                    HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        } else {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
    }
    return 0;
}

int HOPTCTypeTopLevelVarLikes(HOPTypeCheckCtx* c, HOPAstKind wantKind) {
    int32_t child = HOPAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == wantKind) {
            HOPTCVarLikeParts parts;
            if (HOPTCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return HOPTCFailNode(c, child, HOPDiag_EXPECTED_TYPE);
            }
            if (!parts.grouped) {
                int32_t firstChild = HOPAstFirstChild(c->ast, child);
                if (firstChild >= 0 && HOPTCIsTypeNodeKind(c->ast->nodes[firstChild].kind)) {
                    int32_t initNode = HOPAstNextSibling(c->ast, firstChild);
                    int32_t declType;
                    int32_t initType;
                    if (wantKind == HOPAst_CONST && initNode < 0
                        && !HOPTCHasForeignImportDirective(c->ast, c->src, child))
                    {
                        return HOPTCFailNode(c, child, HOPDiag_CONST_MISSING_INITIALIZER);
                    }
                    c->allowConstNumericTypeName = wantKind == HOPAst_CONST ? 1u : 0u;
                    if (HOPTCResolveTypeNode(c, firstChild, &declType) != 0) {
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowConstNumericTypeName = 0;
                    if (HOPTCTypeContainsVarSizeByValue(c, declType)) {
                        return HOPTCFailNode(c, firstChild, HOPDiag_TYPE_MISMATCH);
                    }
                    if (wantKind == HOPAst_VAR && initNode < 0
                        && HOPTCTypeIsTrackedPtrRef(c, declType))
                    {
                        return HOPTCFailTopLevelPtrRefMissingInitializer(
                            c, n->start, n->end, n->dataStart, n->dataEnd);
                    }
                    if (initNode >= 0) {
                        if (HOPTCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                            return -1;
                        }
                        if (!HOPTCCanAssign(c, declType, initType)) {
                            return HOPTCFailTypeMismatchDetail(
                                c, initNode, initNode, initType, declType);
                        }
                    }
                } else if (firstChild >= 0) {
                    int32_t initType;
                    int32_t declType;
                    if (HOPTCTypeExpr(c, firstChild, &initType) != 0) {
                        return -1;
                    }
                    if (initType == c->typeNull) {
                        return HOPTCFailNode(c, firstChild, HOPDiag_INFER_NULL_TYPE_UNKNOWN);
                    }
                    if (initType == c->typeVoid) {
                        return HOPTCFailNode(c, firstChild, HOPDiag_INFER_VOID_TYPE_UNKNOWN);
                    }
                    if (wantKind == HOPAst_CONST) {
                        declType = initType;
                    } else {
                        if (HOPTCConcretizeInferredType(c, initType, &declType) != 0) {
                            return -1;
                        }
                    }
                    if (HOPTCTypeContainsVarSizeByValue(c, declType)) {
                        return HOPTCFailNode(c, firstChild, HOPDiag_TYPE_MISMATCH);
                    }
                }
                child = HOPAstNextSibling(c->ast, child);
                continue;
            }

            if (parts.typeNode >= 0) {
                int32_t declType;
                if (wantKind == HOPAst_CONST && parts.initNode < 0
                    && !HOPTCHasForeignImportDirective(c->ast, c->src, child))
                {
                    return HOPTCFailNode(c, child, HOPDiag_CONST_MISSING_INITIALIZER);
                }
                c->allowConstNumericTypeName = wantKind == HOPAst_CONST ? 1u : 0u;
                if (HOPTCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
                    c->allowConstNumericTypeName = 0;
                    return -1;
                }
                c->allowConstNumericTypeName = 0;
                if (HOPTCTypeContainsVarSizeByValue(c, declType)) {
                    return HOPTCFailNode(c, parts.typeNode, HOPDiag_TYPE_MISMATCH);
                }
                if (wantKind == HOPAst_VAR && parts.initNode < 0
                    && HOPTCTypeIsTrackedPtrRef(c, declType))
                {
                    int32_t  nameNode = HOPTCListItemAt(c->ast, parts.nameListNode, 0);
                    uint32_t nameStart = c->ast->nodes[child].dataStart;
                    uint32_t nameEnd = c->ast->nodes[child].dataEnd;
                    if (nameNode >= 0) {
                        nameStart = c->ast->nodes[nameNode].dataStart;
                        nameEnd = c->ast->nodes[nameNode].dataEnd;
                    }
                    return HOPTCFailTopLevelPtrRefMissingInitializer(
                        c,
                        c->ast->nodes[child].start,
                        c->ast->nodes[child].end,
                        nameStart,
                        nameEnd);
                }
                if (parts.initNode >= 0) {
                    uint32_t i;
                    uint32_t initCount;
                    if (c->ast->nodes[parts.initNode].kind != HOPAst_EXPR_LIST) {
                        return HOPTCFailNode(c, parts.initNode, HOPDiag_EXPECTED_EXPR);
                    }
                    initCount = HOPTCListCount(c->ast, parts.initNode);
                    if (initCount == parts.nameCount) {
                        for (i = 0; i < initCount; i++) {
                            int32_t initNode = HOPTCListItemAt(c->ast, parts.initNode, i);
                            int32_t initType;
                            if (initNode < 0) {
                                return HOPTCFailNode(c, child, HOPDiag_EXPECTED_EXPR);
                            }
                            if (HOPTCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                                return -1;
                            }
                            if (!HOPTCCanAssign(c, declType, initType)) {
                                return HOPTCFailTypeMismatchDetail(
                                    c, initNode, initNode, initType, declType);
                            }
                        }
                    } else if (initCount == 1u) {
                        int32_t          initNode = HOPTCListItemAt(c->ast, parts.initNode, 0);
                        int32_t          initType;
                        const HOPTCType* t;
                        if (initNode < 0) {
                            return HOPTCFailNode(c, child, HOPDiag_EXPECTED_EXPR);
                        }
                        if (HOPTCTypeExpr(c, initNode, &initType) != 0) {
                            return -1;
                        }
                        if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                            return HOPTCFailNode(c, initNode, HOPDiag_TYPE_MISMATCH);
                        }
                        t = &c->types[initType];
                        if (t->kind != HOPTCType_TUPLE || t->fieldCount != parts.nameCount) {
                            return HOPTCFailNode(c, child, HOPDiag_ARITY_MISMATCH);
                        }
                        for (i = 0; i < parts.nameCount; i++) {
                            int32_t elemType = c->funcParamTypes[t->fieldStart + i];
                            if (!HOPTCCanAssign(c, declType, elemType)) {
                                return HOPTCFailTypeMismatchDetail(
                                    c, initNode, initNode, elemType, declType);
                            }
                        }
                    } else {
                        return HOPTCFailNode(c, child, HOPDiag_ARITY_MISMATCH);
                    }
                }
            } else {
                uint32_t i;
                if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != HOPAst_EXPR_LIST) {
                    return HOPTCFailNode(c, child, HOPDiag_EXPECTED_EXPR);
                }
                {
                    uint32_t initCount = HOPTCListCount(c->ast, parts.initNode);
                    int      tupleDecompose = 0;
                    if (initCount == parts.nameCount) {
                        tupleDecompose = 1;
                    } else if (initCount == 1u) {
                        int32_t          initNode = HOPTCListItemAt(c->ast, parts.initNode, 0);
                        int32_t          initType;
                        const HOPTCType* t;
                        if (initNode < 0) {
                            return HOPTCFailNode(c, child, HOPDiag_EXPECTED_EXPR);
                        }
                        if (HOPTCTypeExpr(c, initNode, &initType) != 0) {
                            return -1;
                        }
                        if (initType >= 0 && (uint32_t)initType < c->typeLen) {
                            t = &c->types[initType];
                            tupleDecompose =
                                t->kind == HOPTCType_TUPLE && t->fieldCount == parts.nameCount;
                        }
                    }
                    if (!tupleDecompose) {
                        return HOPTCFailNode(c, child, HOPDiag_ARITY_MISMATCH);
                    }
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t inferredType;
                    if (HOPTCTypeTopLevelVarLikeNode(c, child, (int32_t)i, &inferredType) != 0) {
                        return -1;
                    }
                    (void)inferredType;
                }
            }
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

int HOPTCTypeTopLevelConsts(HOPTypeCheckCtx* c) {
    return HOPTCTypeTopLevelVarLikes(c, HOPAst_CONST);
}

int HOPTCTypeTopLevelVars(HOPTypeCheckCtx* c) {
    return HOPTCTypeTopLevelVarLikes(c, HOPAst_VAR);
}

int HOPTCCheckTopLevelConstInitializers(HOPTypeCheckCtx* c) {
    int32_t child = HOPAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_CONST) {
            HOPTCVarLikeParts parts;
            if (HOPTCHasForeignImportDirective(c->ast, c->src, child)) {
                child = HOPAstNextSibling(c->ast, child);
                continue;
            }
            if (HOPTCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return HOPTCFailNode(c, child, HOPDiag_EXPECTED_TYPE);
            }
            if (parts.typeNode >= 0 && parts.initNode < 0
                && !HOPTCHasForeignImportDirective(c->ast, c->src, child))
            {
                return HOPTCFailNode(c, child, HOPDiag_CONST_MISSING_INITIALIZER);
            }
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

int HOPTCValidateTopLevelConstEvaluable(HOPTypeCheckCtx* c) {
    HOPTCConstEvalCtx evalCtx;
    int32_t           child;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    child = HOPAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_CONST) {
            HOPTCVarLikeParts parts;
            if (HOPTCHasForeignImportDirective(c->ast, c->src, child)) {
                child = HOPAstNextSibling(c->ast, child);
                continue;
            }
            if (HOPTCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return HOPTCFailNode(c, child, HOPDiag_EXPECTED_TYPE);
            }
            if (!parts.grouped) {
                int32_t      initNode = HOPTCVarLikeInitExprNode(c, child);
                HOPCTFEValue value;
                int          isConst = 0;
                if (initNode >= 0
                    && HOPTCValidateLocalConstFunctionInitializerExprNode(c, initNode))
                {
                    child = HOPAstNextSibling(c->ast, child);
                    continue;
                }
                c->lastConstEvalReason = NULL;
                c->lastConstEvalReasonStart = 0;
                c->lastConstEvalReasonEnd = 0;
                evalCtx.nonConstReason = NULL;
                evalCtx.nonConstStart = 0;
                evalCtx.nonConstEnd = 0;
                evalCtx.fnDepth = 0;
                evalCtx.execCtx = NULL;
                if (HOPTCEvalTopLevelConstNode(c, &evalCtx, child, &value, &isConst) != 0) {
                    return -1;
                }
                c->lastConstEvalReason = evalCtx.nonConstReason;
                c->lastConstEvalReasonStart = evalCtx.nonConstStart;
                c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
                if (!isConst) {
                    int rc;
                    if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd
                        && c->lastConstEvalReasonEnd <= c->src.len)
                    {
                        rc = HOPTCFailSpan(
                            c,
                            HOPDiag_CONST_INIT_CONST_REQUIRED,
                            c->lastConstEvalReasonStart,
                            c->lastConstEvalReasonEnd);
                    } else if (initNode >= 0) {
                        rc = HOPTCFailNode(c, initNode, HOPDiag_CONST_INIT_CONST_REQUIRED);
                    } else {
                        rc = HOPTCFailNode(c, child, HOPDiag_CONST_INIT_CONST_REQUIRED);
                    }
                    HOPTCAttachConstEvalReason(c);
                    return rc;
                }
            } else {
                uint32_t i;
                if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != HOPAst_EXPR_LIST) {
                    return HOPTCFailNode(c, child, HOPDiag_EXPECTED_EXPR);
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t      initNode = HOPTCVarLikeInitExprNodeAt(c, child, (int32_t)i);
                    HOPCTFEValue value;
                    int          isConst = 0;
                    if (initNode < 0) {
                        return HOPTCFailNode(c, child, HOPDiag_ARITY_MISMATCH);
                    }
                    c->lastConstEvalReason = NULL;
                    c->lastConstEvalReasonStart = 0;
                    c->lastConstEvalReasonEnd = 0;
                    evalCtx.nonConstReason = NULL;
                    evalCtx.nonConstStart = 0;
                    evalCtx.nonConstEnd = 0;
                    evalCtx.fnDepth = 0;
                    evalCtx.execCtx = NULL;
                    if (HOPTCEvalConstExprNode(&evalCtx, initNode, &value, &isConst) != 0) {
                        return -1;
                    }
                    c->lastConstEvalReason = evalCtx.nonConstReason;
                    c->lastConstEvalReasonStart = evalCtx.nonConstStart;
                    c->lastConstEvalReasonEnd = evalCtx.nonConstEnd;
                    if (!isConst) {
                        int rc;
                        if (c->lastConstEvalReasonStart < c->lastConstEvalReasonEnd
                            && c->lastConstEvalReasonEnd <= c->src.len)
                        {
                            rc = HOPTCFailSpan(
                                c,
                                HOPDiag_CONST_INIT_CONST_REQUIRED,
                                c->lastConstEvalReasonStart,
                                c->lastConstEvalReasonEnd);
                        } else {
                            rc = HOPTCFailNode(c, initNode, HOPDiag_CONST_INIT_CONST_REQUIRED);
                        }
                        HOPTCAttachConstEvalReason(c);
                        return rc;
                    }
                }
            }
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

int HOPTCGetNullNarrow(HOPTypeCheckCtx* c, int32_t condNode, int* outIsEq, HOPTCNullNarrow* out) {
    const HOPAstNode* n;
    int32_t           lhs, rhs, identNode;
    HOPTokenKind      op;
    int32_t           localIdx;
    int32_t           typeId;

    if (condNode < 0 || (uint32_t)condNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[condNode];
    if (n->kind != HOPAst_BINARY) {
        return 0;
    }
    op = (HOPTokenKind)n->op;
    if (op != HOPTok_EQ && op != HOPTok_NEQ) {
        return 0;
    }
    lhs = HOPAstFirstChild(c->ast, condNode);
    rhs = lhs >= 0 ? HOPAstNextSibling(c->ast, lhs) : -1;
    if (lhs < 0 || rhs < 0) {
        return 0;
    }
    /* Identify which side is the ident and which is null. */
    if (c->ast->nodes[lhs].kind == HOPAst_IDENT && c->ast->nodes[rhs].kind == HOPAst_NULL) {
        identNode = lhs;
    } else if (c->ast->nodes[rhs].kind == HOPAst_IDENT && c->ast->nodes[lhs].kind == HOPAst_NULL) {
        identNode = rhs;
    } else {
        return 0;
    }
    {
        const HOPAstNode* id = &c->ast->nodes[identNode];
        localIdx = HOPTCLocalFind(c, id->dataStart, id->dataEnd);
    }
    if (localIdx < 0) {
        return 0;
    }
    typeId = c->locals[localIdx].typeId;
    if (c->types[typeId].kind != HOPTCType_OPTIONAL) {
        return 0;
    }
    *outIsEq = (op == HOPTok_EQ);
    out->localIdx = localIdx;
    out->innerType = c->types[typeId].baseType;
    return 1;
}

int HOPTCGetOptionalCondNarrow(
    HOPTypeCheckCtx* c, int32_t condNode, int* outThenIsSome, HOPTCNullNarrow* out) {
    const HOPAstNode* n;
    int32_t           localIdx;
    int32_t           typeId;
    int               isEq = 0;
    if (c == NULL || outThenIsSome == NULL || out == NULL || condNode < 0
        || (uint32_t)condNode >= c->ast->len)
    {
        return 0;
    }
    n = &c->ast->nodes[condNode];

    if (n->kind == HOPAst_UNARY && (HOPTokenKind)n->op == HOPTok_NOT) {
        int32_t inner = HOPAstFirstChild(c->ast, condNode);
        if (inner < 0) {
            return 0;
        }
        if (!HOPTCGetOptionalCondNarrow(c, inner, outThenIsSome, out)) {
            return 0;
        }
        *outThenIsSome = !*outThenIsSome;
        return 1;
    }

    if (n->kind == HOPAst_IDENT) {
        localIdx = HOPTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx < 0) {
            return 0;
        }
        typeId = c->locals[localIdx].typeId;
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen
            || c->types[typeId].kind != HOPTCType_OPTIONAL)
        {
            return 0;
        }
        out->localIdx = localIdx;
        out->innerType = c->types[typeId].baseType;
        *outThenIsSome = 1;
        return 1;
    }

    if (HOPTCGetNullNarrow(c, condNode, &isEq, out)) {
        *outThenIsSome = isEq ? 0 : 1;
        return 1;
    }

    return 0;
}

HOP_API_END
