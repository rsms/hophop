#include "internal.h"

SL_API_BEGIN

int SLTCTypeNewExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLAstNode* n;
    int32_t          typeNode;
    int32_t          nextNode;
    int32_t          countArgNode = -1;
    int32_t          initArgNode = -1;
    int32_t          allocArgNode = -1;
    int32_t          allocArgType = -1;
    int32_t          allocBaseType;
    int32_t          allocParamType;
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
        return SLTCFailSpan(c, SLDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    hasCount = (n->flags & SLAstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & SLAstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & SLAstFlag_NEW_HAS_ALLOC) != 0;

    typeNode = SLAstFirstChild(c->ast, nodeId);
    if (typeNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    nextNode = SLAstNextSibling(c->ast, typeNode);
    if (hasCount) {
        countArgNode = nextNode;
        if (countArgNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        nextNode = SLAstNextSibling(c->ast, countArgNode);
    }
    if (hasInit) {
        initArgNode = nextNode;
        if (initArgNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        nextNode = SLAstNextSibling(c->ast, initArgNode);
    }
    if (hasAlloc) {
        allocArgNode = nextNode;
        if (allocArgNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        nextNode = SLAstNextSibling(c->ast, allocArgNode);
    }
    if (nextNode >= 0) {
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }

    allocBaseType = SLTCFindMemAllocatorType(c);
    allocParamType =
        allocBaseType < 0 ? -1 : SLTCInternRefType(c, allocBaseType, 1, n->start, n->end);
    if (allocBaseType < 0 || allocParamType < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    }

    if (allocArgNode >= 0) {
        if (SLTCTypeExpr(c, allocArgNode, &allocArgType) != 0) {
            return -1;
        }
        if (!SLTCCanAssign(c, allocParamType, allocArgType)) {
            return SLTCFailNode(c, allocArgNode, SLDiag_TYPE_MISMATCH);
        }
    } else {
        if (SLTCGetEffectiveContextFieldTypeByLiteral(c, "mem", &ctxMemType) != 0) {
            return -1;
        }
        if (!SLTCCanAssign(c, allocParamType, ctxMemType)) {
            return SLTCFailNode(c, nodeId, SLDiag_CONTEXT_TYPE_MISMATCH);
        }
    }

    if (SLTCResolveTypeNode(c, typeNode, &elemType) != 0) {
        return -1;
    }

    if (countArgNode >= 0 && SLTCTypeContainsVarSizeByValue(c, elemType)) {
        return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
    }
    if (countArgNode < 0 && SLTCTypeContainsVarSizeByValue(c, elemType) && initArgNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_NEW_VARSIZE_INIT_REQUIRED);
    }

    if (initArgNode >= 0) {
        int32_t initType;
        if (SLTCTypeExprExpected(c, initArgNode, elemType, &initType) != 0) {
            return -1;
        }
        if (!SLTCCanAssign(c, elemType, initType)) {
            return SLTCFailNode(c, initArgNode, SLDiag_TYPE_MISMATCH);
        }
    }

    if (countArgNode >= 0) {
        if (SLTCTypeExpr(c, countArgNode, &countType) != 0) {
            return -1;
        }
        if (!SLTCIsIntegerType(c, countType)) {
            return SLTCFailNode(c, countArgNode, SLDiag_TYPE_MISMATCH);
        }
        if (SLTCConstIntExpr(c, countArgNode, &countValue, &countIsConst) != 0) {
            return SLTCFailNode(c, countArgNode, SLDiag_TYPE_MISMATCH);
        }
        if (countIsConst && countValue < 0) {
            return SLTCFailNode(c, countArgNode, SLDiag_TYPE_MISMATCH);
        }
    }

    if (countArgNode >= 0) {
        if (countIsConst && countValue > 0) {
            int32_t arrayType = SLTCInternArrayType(
                c, elemType, (uint32_t)countValue, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            resultType = SLTCInternPtrType(c, arrayType, n->start, n->end);
        } else {
            int32_t sliceType = SLTCInternSliceType(c, elemType, 1, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            resultType = SLTCInternPtrType(c, sliceType, n->start, n->end);
        }
    } else {
        resultType = SLTCInternPtrType(c, elemType, n->start, n->end);
    }
    if (resultType < 0) {
        return -1;
    }
    *outType = resultType;
    return 0;
}

int SLTCExprIsCompoundTemporary(SLTypeCheckCtx* c, int32_t exprNode) {
    const SLAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, exprNode);
        return SLTCExprIsCompoundTemporary(c, inner);
    }
    if (n->kind == SLAst_COMPOUND_LIT) {
        return 1;
    }
    if (n->kind == SLAst_UNARY && n->op == SLTok_AND) {
        int32_t rhsNode = SLAstFirstChild(c->ast, exprNode);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == SLAst_COMPOUND_LIT)
        {
            return 1;
        }
    }
    return 0;
}

static int SLTCMatchAnyPackIndexExpr(
    SLTypeCheckCtx* c,
    int32_t         exprNode,
    int32_t* _Nullable outPackType,
    int32_t* _Nullable outIdxNode) {
    const SLAstNode* n;
    int32_t          baseNode;
    int32_t          idxNode;
    int32_t          localIdx;
    int32_t          packType;
    int32_t          resolvedPackType;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, exprNode);
        if (inner < 0) {
            return 0;
        }
        return SLTCMatchAnyPackIndexExpr(c, inner, outPackType, outIdxNode);
    }
    if (n->kind != SLAst_INDEX || (n->flags & SLAstFlag_INDEX_SLICE) != 0) {
        return 0;
    }
    baseNode = SLAstFirstChild(c->ast, exprNode);
    idxNode = baseNode >= 0 ? SLAstNextSibling(c->ast, baseNode) : -1;
    if (baseNode < 0 || idxNode < 0 || SLAstNextSibling(c->ast, idxNode) >= 0) {
        return 0;
    }
    if ((uint32_t)baseNode >= c->ast->len || c->ast->nodes[baseNode].kind != SLAst_IDENT) {
        return 0;
    }
    localIdx = SLTCLocalFind(c, c->ast->nodes[baseNode].dataStart, c->ast->nodes[baseNode].dataEnd);
    if (localIdx < 0) {
        return 0;
    }
    packType = c->locals[localIdx].typeId;
    resolvedPackType = SLTCResolveAliasBaseType(c, packType);
    if (resolvedPackType < 0 || (uint32_t)resolvedPackType >= c->typeLen
        || c->types[resolvedPackType].kind != SLTCType_PACK)
    {
        return 0;
    }
    SLTCMarkLocalRead(c, localIdx);
    if (outPackType != NULL) {
        *outPackType = packType;
    }
    if (outIdxNode != NULL) {
        *outIdxNode = idxNode;
    }
    return 1;
}

int SLTCExprNeedsExpectedType(SLTypeCheckCtx* c, int32_t exprNode) {
    const SLAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, exprNode);
        return SLTCExprNeedsExpectedType(c, inner);
    }
    if (n->kind == SLAst_COMPOUND_LIT) {
        return 1;
    }
    if (SLTCMatchAnyPackIndexExpr(c, exprNode, NULL, NULL)) {
        return 1;
    }
    if (n->kind == SLAst_UNARY && n->op == SLTok_AND) {
        int32_t rhsNode = SLAstFirstChild(c->ast, exprNode);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == SLAst_COMPOUND_LIT)
        {
            int32_t rhsChild = SLAstFirstChild(c->ast, rhsNode);
            return !(rhsChild >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[rhsChild].kind));
        }
    }
    return 0;
}

int SLTCResolveIdentifierExprType(
    SLTypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint32_t        spanStart,
    uint32_t        spanEnd,
    int32_t*        outType) {
    int32_t localIdx = SLTCLocalFind(c, nameStart, nameEnd);
    if (localIdx >= 0) {
        if (SLTCCheckLocalInitialized(c, localIdx, spanStart, spanEnd) != 0) {
            return -1;
        }
        SLTCMarkLocalRead(c, localIdx);
        *outType = c->locals[localIdx].typeId;
        return 0;
    }
    if (SLNameEqLiteral(c->src, nameStart, nameEnd, "context")) {
        int32_t contextTypeId = -1;
        if (c->currentContextType >= 0) {
            contextTypeId = c->currentContextType;
        } else if (c->hasImplicitMainRootContext && c->implicitMainContextType >= 0) {
            contextTypeId = c->implicitMainContextType;
        }
        if (contextTypeId >= 0) {
            int32_t contextRefType = SLTCInternRefType(c, contextTypeId, 1, spanStart, spanEnd);
            if (contextRefType < 0) {
                return -1;
            }
            *outType = contextRefType;
            return 0;
        }
    }
    {
        int32_t fnIdx = SLTCFindFunctionIndex(c, nameStart, nameEnd);
        if (fnIdx >= 0) {
            SLTCMarkFunctionUsed(c, fnIdx);
            *outType = c->funcs[fnIdx].funcTypeId;
            return 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = SLTCFindTopLevelVarLikeNode(c, nameStart, nameEnd, &nameIndex);
        if (varLikeNode >= 0) {
            if (c->currentFunctionIsCompareHook) {
                return SLTCFailSpan(c, SLDiag_COMPARISON_HOOK_IMPURE, spanStart, spanEnd);
            }
            return SLTCTypeTopLevelVarLikeNode(c, varLikeNode, nameIndex, outType);
        }
    }
    return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, nameStart, nameEnd);
}

int SLTCInferAnonStructTypeFromCompound(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t firstField, int32_t* outType) {
    SLTCAnonFieldSig fieldSigs[SLTC_MAX_ANON_FIELDS];
    uint32_t         fieldCount = 0;
    int32_t          fieldNode = firstField;
    while (fieldNode >= 0) {
        const SLAstNode* field = &c->ast->nodes[fieldNode];
        int32_t          exprNode;
        int32_t          fieldType = -1;
        uint32_t         i;
        if (field->kind != SLAst_COMPOUND_FIELD) {
            return SLTCFailNode(c, fieldNode, SLDiag_UNEXPECTED_TOKEN);
        }
        if (fieldCount >= SLTC_MAX_ANON_FIELDS) {
            return SLTCFailNode(c, fieldNode, SLDiag_ARENA_OOM);
        }
        for (i = 0; i < fieldCount; i++) {
            if (SLNameEqSlice(
                    c->src,
                    fieldSigs[i].nameStart,
                    fieldSigs[i].nameEnd,
                    field->dataStart,
                    field->dataEnd))
            {
                SLTCSetDiagWithArg(
                    c->diag,
                    SLDiag_COMPOUND_FIELD_DUPLICATE,
                    field->start,
                    field->end,
                    field->dataStart,
                    field->dataEnd);
                return -1;
            }
        }
        exprNode = SLAstFirstChild(c->ast, fieldNode);
        if (exprNode < 0) {
            if ((field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                return SLTCFailNode(c, fieldNode, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCResolveIdentifierExprType(
                    c, field->dataStart, field->dataEnd, field->start, field->end, &fieldType)
                != 0)
            {
                return -1;
            }
        } else {
            if (SLTCTypeExpr(c, exprNode, &fieldType) != 0) {
                return -1;
            }
        }
        if (fieldType == c->typeNull) {
            return SLTCFailNode(c, exprNode, SLDiag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (fieldType == c->typeVoid) {
            return SLTCFailNode(c, exprNode, SLDiag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (SLTCConcretizeInferredType(c, fieldType, &fieldType) != 0) {
            return -1;
        }
        fieldSigs[fieldCount].nameStart = field->dataStart;
        fieldSigs[fieldCount].nameEnd = field->dataEnd;
        fieldSigs[fieldCount].typeId = fieldType;
        fieldCount++;
        fieldNode = SLAstNextSibling(c->ast, fieldNode);
    }

    {
        int32_t typeId = SLTCInternAnonAggregateType(
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

int SLTCTypeCompoundLit(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType) {
    int32_t  child = SLAstFirstChild(c->ast, nodeId);
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

    if (child >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        if (SLTCResolveTypeNode(c, child, &resolvedType) != 0) {
            int variantRc = SLTCResolveEnumVariantTypeName(
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
        firstField = SLAstNextSibling(c->ast, child);
    } else {
        if (expectedType < 0) {
            if (SLTCInferAnonStructTypeFromCompound(c, nodeId, firstField, &resolvedType) != 0) {
                return -1;
            }
            targetType = resolvedType;
        } else {
            resolvedType = expectedType;
            targetType = expectedType;
        }
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && c->types[expectedType].kind == SLTCType_REF)
    {
        expectedReadonlyRef = !SLTCTypeIsMutable(&c->types[expectedType]);
        expectedBaseType = c->types[expectedType].baseType;
        if (!expectedReadonlyRef) {
            return SLTCFailNode(c, nodeId, SLDiag_COMPOUND_MUT_REF_TEMPORARY);
        }
        if (expectedBaseType < 0 || (uint32_t)expectedBaseType >= c->typeLen) {
            return SLTCFailNode(c, nodeId, SLDiag_COMPOUND_INFER_NON_AGGREGATE);
        }
        if (child < 0 || !SLTCIsTypeNodeKind(c->ast->nodes[child].kind)) {
            resolvedType = expectedType;
            targetType = expectedBaseType;
        } else {
            if (!SLTCCanAssign(c, expectedBaseType, targetType)) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            resolvedType = expectedType;
        }
    }

    targetAggregateType = SLTCResolveAliasBaseType(c, targetType);
    if (targetAggregateType < 0 || (uint32_t)targetAggregateType >= c->typeLen) {
        return SLTCFailNode(
            c,
            nodeId,
            child >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[child].kind)
                ? SLDiag_COMPOUND_TYPE_REQUIRED
                : SLDiag_COMPOUND_INFER_NON_AGGREGATE);
    }
    if (c->types[targetAggregateType].kind != SLTCType_NAMED
        && c->types[targetAggregateType].kind != SLTCType_ANON_STRUCT
        && c->types[targetAggregateType].kind != SLTCType_ANON_UNION)
    {
        return SLTCFailNode(
            c,
            nodeId,
            child >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[child].kind)
                ? SLDiag_COMPOUND_TYPE_REQUIRED
                : SLDiag_COMPOUND_INFER_NON_AGGREGATE);
    }
    if (c->types[targetAggregateType].kind == SLTCType_NAMED
        && SLTCEnsureNamedTypeFieldsResolved(c, targetAggregateType) != 0)
    {
        return -1;
    }
    if (isEnumVariantLiteral) {
        if (!SLTCIsNamedDeclKind(c, targetAggregateType, SLAst_ENUM)) {
            return SLTCFailNode(c, nodeId, SLDiag_COMPOUND_TYPE_REQUIRED);
        }
        isUnion = 0;
    } else if (c->types[targetAggregateType].kind == SLTCType_NAMED) {
        int32_t declNode = c->types[targetAggregateType].declNode;
        if (declNode < 0 || (uint32_t)declNode >= c->ast->len
            || (c->ast->nodes[declNode].kind != SLAst_STRUCT
                && c->ast->nodes[declNode].kind != SLAst_UNION))
        {
            return SLTCFailNode(
                c,
                nodeId,
                child >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[child].kind)
                    ? SLDiag_COMPOUND_TYPE_REQUIRED
                    : SLDiag_COMPOUND_INFER_NON_AGGREGATE);
        }
        isUnion = c->ast->nodes[declNode].kind == SLAst_UNION;
    } else {
        isUnion = c->types[targetAggregateType].kind == SLTCType_ANON_UNION;
    }

    while (firstField >= 0) {
        const SLAstNode* fieldNode = &c->ast->nodes[firstField];
        int32_t          fieldType = -1;
        int32_t          exprNode;
        int32_t          exprType = -1;
        int32_t          scan;

        if (fieldNode->kind != SLAst_COMPOUND_FIELD) {
            return SLTCFailNode(c, firstField, SLDiag_UNEXPECTED_TOKEN);
        }

        scan = SLAstFirstChild(c->ast, nodeId);
        if (scan >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[scan].kind)) {
            scan = SLAstNextSibling(c->ast, scan);
        }
        while (scan >= 0 && scan != firstField) {
            const SLAstNode* prevField = &c->ast->nodes[scan];
            if (prevField->kind == SLAst_COMPOUND_FIELD
                && SLNameEqSlice(
                    c->src,
                    prevField->dataStart,
                    prevField->dataEnd,
                    fieldNode->dataStart,
                    fieldNode->dataEnd))
            {
                SLTCSetDiagWithArg(
                    c->diag,
                    SLDiag_COMPOUND_FIELD_DUPLICATE,
                    fieldNode->start,
                    fieldNode->end,
                    fieldNode->dataStart,
                    fieldNode->dataEnd);
                return -1;
            }
            scan = SLAstNextSibling(c->ast, scan);
        }

        if ((!isEnumVariantLiteral
             && SLTCFieldLookupPath(
                    c, targetAggregateType, fieldNode->dataStart, fieldNode->dataEnd, &fieldType)
                    != 0)
            || (isEnumVariantLiteral
                && SLTCEnumVariantPayloadFieldType(
                       c,
                       targetAggregateType,
                       enumVariantStart,
                       enumVariantEnd,
                       fieldNode->dataStart,
                       fieldNode->dataEnd,
                       &fieldType)
                       != 0))
        {
            SLTCSetDiagWithArg(
                c->diag,
                SLDiag_COMPOUND_FIELD_UNKNOWN,
                fieldNode->start,
                fieldNode->end,
                fieldNode->dataStart,
                fieldNode->dataEnd);
            return -1;
        }

        exprNode = SLAstFirstChild(c->ast, firstField);
        if (exprNode < 0) {
            if ((fieldNode->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                return SLTCFailNode(c, firstField, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCResolveIdentifierExprType(
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
        } else if (SLTCTypeExprExpected(c, exprNode, fieldType, &exprType) != 0) {
            return -1;
        }
        if (!SLTCCanAssign(c, fieldType, exprType)) {
            uint32_t errStart =
                exprNode >= 0 ? c->ast->nodes[exprNode].start : c->ast->nodes[firstField].start;
            uint32_t errEnd =
                exprNode >= 0 ? c->ast->nodes[exprNode].end : c->ast->nodes[firstField].end;
            SLTCSetDiagWithArg(
                c->diag,
                SLDiag_COMPOUND_FIELD_TYPE_MISMATCH,
                errStart,
                errEnd,
                fieldNode->dataStart,
                fieldNode->dataEnd);
            return -1;
        }
        explicitFieldCount++;
        firstField = SLAstNextSibling(c->ast, firstField);
    }

    if (isUnion && explicitFieldCount > 1u) {
        return SLTCFailNode(c, nodeId, SLDiag_COMPOUND_UNION_MULTI_FIELD);
    }

    *outType = resolvedType;
    return 0;
}

int SLTCTypeExprExpected(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType, int32_t* outType) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, nodeId);
        if (inner < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        return SLTCTypeExprExpected(c, inner, expectedType, outType);
    }

    if (n->kind == SLAst_CALL || n->kind == SLAst_CALL_WITH_CONTEXT) {
        int32_t savedExpectedCallType = c->activeExpectedCallType;
        int     rc;
        c->activeExpectedCallType = expectedType;
        rc = SLTCTypeExpr(c, nodeId, outType);
        c->activeExpectedCallType = savedExpectedCallType;
        return rc;
    }

    if (n->kind == SLAst_COMPOUND_LIT) {
        return SLTCTypeCompoundLit(c, nodeId, expectedType, outType);
    }
    if (n->kind == SLAst_INDEX) {
        int32_t packType = -1;
        int32_t idxNode = -1;
        if (SLTCMatchAnyPackIndexExpr(c, nodeId, &packType, &idxNode)) {
            int32_t resolvedPackType = SLTCResolveAliasBaseType(c, packType);
            int32_t idxType;
            int64_t idxValue = 0;
            int     idxIsConst = 0;
            if (resolvedPackType < 0 || (uint32_t)resolvedPackType >= c->typeLen
                || c->types[resolvedPackType].kind != SLTCType_PACK)
            {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCTypeExpr(c, idxNode, &idxType) != 0) {
                return -1;
            }
            if (!SLTCIsIntegerType(c, idxType)) {
                return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
                return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
            }
            if (!idxIsConst) {
                int32_t resolvedExpectedType = SLTCResolveAliasBaseType(c, expectedType);
                if (expectedType < 0 || resolvedExpectedType < 0) {
                    return SLTCFailNode(c, idxNode, SLDiag_ANYTYPE_PACK_INDEX_NOT_CONST);
                }
                if (resolvedExpectedType == c->typeAnytype) {
                    *outType = c->typeAnytype;
                    return 0;
                }
                *outType = expectedType;
                return 0;
            }
            if (idxValue < 0 || (uint64_t)idxValue >= c->types[resolvedPackType].fieldCount) {
                return SLTCFailNode(c, idxNode, SLDiag_ANYTYPE_PACK_INDEX_OOB);
            }
            *outType =
                c->funcParamTypes[c->types[resolvedPackType].fieldStart + (uint32_t)idxValue];
            return 0;
        }
    }

    if (n->kind == SLAst_STRING
        || (n->kind == SLAst_BINARY && (SLTokenKind)n->op == SLTok_ADD
            && SLIsStringLiteralConcatChain(c->ast, nodeId)))
    {
        int32_t defaultType = SLTCGetStrRefType(c, n->start, n->end);
        if (defaultType < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
        }
        if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen) {
            int32_t expectedResolved = SLTCResolveAliasBaseType(c, expectedType);
            if (expectedResolved >= 0 && (uint32_t)expectedResolved < c->typeLen) {
                const SLTCType* t = &c->types[expectedResolved];
                int32_t         baseType = t->baseType;
                if (baseType >= 0) {
                    baseType = SLTCResolveAliasBaseType(c, baseType);
                }
                if (t->kind == SLTCType_PTR && baseType == c->typeStr) {
                    *outType = expectedResolved;
                    return 0;
                }
                if (t->kind == SLTCType_REF && baseType == c->typeStr && !SLTCTypeIsMutable(t)) {
                    *outType = expectedResolved;
                    return 0;
                }
            }
        }
        *outType = defaultType;
        return 0;
    }

    if (n->kind == SLAst_TUPLE_EXPR) {
        int32_t expectedBase = SLTCResolveAliasBaseType(c, expectedType);
        if (expectedBase >= 0 && (uint32_t)expectedBase < c->typeLen
            && c->types[expectedBase].kind == SLTCType_TUPLE)
        {
            int32_t  child = SLAstFirstChild(c->ast, nodeId);
            uint32_t idx = 0;
            while (child >= 0) {
                int32_t srcType;
                int32_t dstType;
                if (idx >= c->types[expectedBase].fieldCount) {
                    return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                }
                dstType = c->funcParamTypes[c->types[expectedBase].fieldStart + idx];
                if (SLTCTypeExprExpected(c, child, dstType, &srcType) != 0) {
                    return -1;
                }
                if (!SLTCCanAssign(c, dstType, srcType)) {
                    return SLTCFailTypeMismatchDetail(c, child, child, srcType, dstType);
                }
                idx++;
                child = SLAstNextSibling(c->ast, child);
            }
            if (idx != c->types[expectedBase].fieldCount) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            *outType = expectedBase;
            return 0;
        }
        return SLTCTypeExpr(c, nodeId, outType);
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && SLTCIsIntegerType(c, expectedType))
    {
        int32_t srcType;
        if (SLTCTypeExpr(c, nodeId, &srcType) != 0) {
            return -1;
        }
        if (srcType == c->typeUntypedInt) {
            int64_t value = 0;
            int     isConst = 0;
            if (SLTCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!SLTCConstIntFitsType(c, value, expectedType)) {
                return SLTCFailConstIntRange(c, nodeId, value, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
        if (SLTCTypeIsRuneLike(c, srcType)) {
            int64_t value = 0;
            int     isConst = 0;
            if (SLTCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!SLTCConstIntFitsType(c, value, expectedType)) {
                return SLTCFailConstIntRange(c, nodeId, value, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
    }

    if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
        && SLTCIsFloatType(c, expectedType))
    {
        int32_t srcType;
        if (SLTCTypeExpr(c, nodeId, &srcType) != 0) {
            return -1;
        }
        if (srcType == c->typeUntypedInt) {
            int64_t value = 0;
            int     isConst = 0;
            if (SLTCConstIntExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!SLTCConstIntFitsFloatType(c, value, expectedType)) {
                return SLTCFailConstFloatRange(c, nodeId, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
        if (srcType == c->typeUntypedFloat) {
            double value = 0.0;
            int    isConst = 0;
            if (SLTCConstFloatExpr(c, nodeId, &value, &isConst) != 0) {
                return -1;
            }
            if (!isConst) {
                *outType = srcType;
                return 0;
            }
            if (!SLTCConstFloatFitsType(c, value, expectedType)) {
                return SLTCFailConstFloatRange(c, nodeId, expectedType);
            }
            *outType = expectedType;
            return 0;
        }
    }

    if (n->kind == SLAst_UNARY && n->op == SLTok_AND) {
        int32_t rhsNode = SLAstFirstChild(c->ast, nodeId);
        if (rhsNode >= 0 && (uint32_t)rhsNode < c->ast->len
            && c->ast->nodes[rhsNode].kind == SLAst_COMPOUND_LIT)
        {
            int32_t rhsExpected = -1;
            int32_t rhsType;
            int32_t ptrType;
            if (expectedType >= 0 && (uint32_t)expectedType < c->typeLen
                && (c->types[expectedType].kind == SLTCType_REF
                    || c->types[expectedType].kind == SLTCType_PTR))
            {
                rhsExpected = c->types[expectedType].baseType;
            }
            if (SLTCTypeExprExpected(c, rhsNode, rhsExpected, &rhsType) != 0) {
                return -1;
            }
            ptrType = SLTCInternPtrType(c, rhsType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
    }

    return SLTCTypeExpr(c, nodeId, outType);
}

int SLTCExprIsAssignable(SLTypeCheckCtx* c, int32_t exprNode) {
    const SLAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, exprNode);
        return SLTCExprIsAssignable(c, inner);
    }
    if (n->kind == SLAst_IDENT) {
        return 1;
    }
    if (n->kind == SLAst_INDEX) {
        SLTCIndexBaseInfo info;
        int32_t           baseNode = SLAstFirstChild(c->ast, exprNode);
        int32_t           baseType;
        if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
            return 0;
        }
        if (baseNode < 0 || SLTCTypeExpr(c, baseNode, &baseType) != 0 || baseType < 0
            || (uint32_t)baseType >= c->typeLen)
        {
            return 0;
        }
        if (SLTCResolveIndexBaseInfo(c, baseType, &info) != 0) {
            return 0;
        }
        if (!info.indexable) {
            return 0;
        }
        if (c->types[baseType].kind == SLTCType_ARRAY || c->types[baseType].kind == SLTCType_PTR) {
            return 1;
        }
        return info.sliceMutable;
    }
    if (n->kind == SLAst_FIELD_EXPR) {
        int32_t  recvNode = SLAstFirstChild(c->ast, exprNode);
        int32_t  recvType;
        int32_t  fieldType;
        uint32_t fieldIndex = 0;
        if (recvNode < 0) {
            return 0;
        }
        if (SLTCTypeExpr(c, recvNode, &recvType) != 0) {
            return 0;
        }
        if (SLTCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, &fieldIndex) != 0) {
            return 0;
        }
        if ((c->fields[fieldIndex].flags & SLTCFieldFlag_DEPENDENT) != 0) {
            return 0;
        }
        if (recvType >= 0 && (uint32_t)recvType < c->typeLen
            && c->types[recvType].kind == SLTCType_REF)
        {
            return SLTCTypeIsMutable(&c->types[recvType]);
        }
        return 1;
    }
    if (n->kind == SLAst_UNARY && n->op == SLTok_MUL) {
        int32_t rhsNode = SLAstFirstChild(c->ast, exprNode);
        int32_t rhsType;
        if (rhsNode < 0 || SLTCTypeExpr(c, rhsNode, &rhsType) != 0 || rhsType < 0
            || (uint32_t)rhsType >= c->typeLen)
        {
            return 0;
        }
        if (c->types[rhsType].kind == SLTCType_PTR) {
            return 1;
        }
        if (c->types[rhsType].kind == SLTCType_REF) {
            return SLTCTypeIsMutable(&c->types[rhsType]);
        }
        return 0;
    }
    return 0;
}

int SLTCExprIsConstAssignTarget(SLTypeCheckCtx* c, int32_t exprNode) {
    const SLAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, exprNode);
        return SLTCExprIsConstAssignTarget(c, inner);
    }
    if (n->kind != SLAst_IDENT) {
        return 0;
    }
    {
        int32_t localIdx = SLTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            return (c->locals[localIdx].flags & SLTCLocalFlag_CONST) != 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = SLTCFindTopLevelVarLikeNode(c, n->dataStart, n->dataEnd, &nameIndex);
        if (varLikeNode >= 0) {
            return c->ast->nodes[varLikeNode].kind == SLAst_CONST;
        }
    }
    return 0;
}

int SLTCTypeAssignTargetExpr(
    SLTypeCheckCtx* c, int32_t nodeId, int skipDirectIdentRead, int32_t* outType) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, nodeId);
        return SLTCTypeAssignTargetExpr(c, inner, skipDirectIdentRead, outType);
    }
    if (skipDirectIdentRead && n->kind == SLAst_IDENT) {
        int32_t localIdx = SLTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            *outType = c->locals[localIdx].typeId;
            return 0;
        }
    }
    return SLTCTypeExpr(c, nodeId, outType);
}

void SLTCMarkDirectIdentLocalWrite(SLTypeCheckCtx* c, int32_t nodeId, int markInitialized) {
    const SLAstNode* n;
    int32_t          localIdx;
    if (c == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == SLAst_CALL_ARG) {
        SLTCMarkDirectIdentLocalWrite(c, SLAstFirstChild(c->ast, nodeId), markInitialized);
        return;
    }
    if (n->kind != SLAst_IDENT) {
        return;
    }
    localIdx = SLTCLocalFind(c, n->dataStart, n->dataEnd);
    if (localIdx < 0) {
        return;
    }
    SLTCMarkLocalWrite(c, localIdx);
    if (markInitialized) {
        SLTCMarkLocalInitialized(c, localIdx);
    }
}

int SLTCTypeExpr_IDENT(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    uint32_t i;
    (void)nodeId;
    if (c->defaultFieldNodes != NULL && c->defaultFieldTypes != NULL
        && c->defaultFieldCurrentIndex < c->defaultFieldCount)
    {
        for (i = 0; i < c->defaultFieldCount; i++) {
            int32_t          fieldNode = c->defaultFieldNodes[i];
            const SLAstNode* f;
            if (fieldNode < 0 || (uint32_t)fieldNode >= c->ast->len) {
                continue;
            }
            f = &c->ast->nodes[fieldNode];
            if (f->kind != SLAst_FIELD) {
                continue;
            }
            if (SLNameEqSlice(c->src, f->dataStart, f->dataEnd, n->dataStart, n->dataEnd)) {
                if (i < c->defaultFieldCurrentIndex && c->defaultFieldTypes[i] >= 0) {
                    *outType = c->defaultFieldTypes[i];
                    return 0;
                }
                SLTCSetDiagWithArg(
                    c->diag,
                    SLDiag_FIELD_DEFAULT_FORWARD_REF,
                    n->start,
                    n->end,
                    f->dataStart,
                    f->dataEnd);
                return -1;
            }
            if ((f->flags & SLAstFlag_FIELD_EMBEDDED) != 0 && c->defaultFieldTypes[i] >= 0) {
                int32_t promotedType = -1;
                if (SLTCFieldLookupPath(
                        c, c->defaultFieldTypes[i], n->dataStart, n->dataEnd, &promotedType)
                    == 0)
                {
                    if (i < c->defaultFieldCurrentIndex) {
                        *outType = promotedType;
                        return 0;
                    }
                    SLTCSetDiagWithArg(
                        c->diag,
                        SLDiag_FIELD_DEFAULT_FORWARD_REF,
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
        int32_t localIdx = SLTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            if (SLTCCheckLocalInitialized(c, localIdx, n->start, n->end) != 0) {
                return -1;
            }
            SLTCMarkLocalRead(c, localIdx);
            *outType = c->locals[localIdx].typeId;
            return 0;
        }
    }
    if (c->activeConstEvalCtx != NULL) {
        int32_t execType = -1;
        if (SLTCConstLookupExecBindingType(
                c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execType))
        {
            *outType = execType;
            return 0;
        }
        if (SLTCConstLookupMirLocalType(c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execType))
        {
            *outType = execType;
            return 0;
        }
        {
            SLCTFEValue execValue;
            int         execIsConst = 0;
            if (SLTCResolveConstIdent(
                    c->activeConstEvalCtx, n->dataStart, n->dataEnd, &execValue, &execIsConst, NULL)
                    == 0
                && execIsConst
                && SLTCEvalConstExecInferValueTypeCb(c->activeConstEvalCtx, &execValue, &execType)
                       == 0)
            {
                *outType = execType;
                return 0;
            }
        }
    }
    if (SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "context")) {
        if (c->currentFunctionIsCompareHook) {
            return SLTCFailNode(c, nodeId, SLDiag_COMPARISON_HOOK_IMPURE);
        }
        int32_t contextTypeId = -1;
        if (c->currentContextType >= 0) {
            contextTypeId = c->currentContextType;
        } else if (c->hasImplicitMainRootContext && c->implicitMainContextType >= 0) {
            contextTypeId = c->implicitMainContextType;
        }
        if (contextTypeId >= 0) {
            int32_t contextRefType = SLTCInternRefType(
                c, contextTypeId, 1, n->dataStart, n->dataEnd);
            if (contextRefType < 0) {
                return -1;
            }
            *outType = contextRefType;
            return 0;
        }
    }
    {
        int32_t fnIdx = SLTCFindFunctionIndex(c, n->dataStart, n->dataEnd);
        if (fnIdx >= 0) {
            SLTCMarkFunctionUsed(c, fnIdx);
            *outType = c->funcs[fnIdx].funcTypeId;
            return 0;
        }
    }
    {
        int32_t nameIndex = -1;
        int32_t varLikeNode = SLTCFindTopLevelVarLikeNode(c, n->dataStart, n->dataEnd, &nameIndex);
        if (varLikeNode >= 0) {
            if (c->currentFunctionIsCompareHook) {
                return SLTCFailNode(c, nodeId, SLDiag_COMPARISON_HOOK_IMPURE);
            }
            return SLTCTypeTopLevelVarLikeNode(c, varLikeNode, nameIndex, outType);
        }
    }
    if (SLTCResolveTypeValueName(c, n->dataStart, n->dataEnd) >= 0 && c->typeType >= 0) {
        *outType = c->typeType;
        return 0;
    }
    return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
}

int SLTCTypeExpr_TYPE_VALUE(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t typeNode = SLAstFirstChild(c->ast, nodeId);
    int32_t ignoredType;
    (void)n;
    if (typeNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    if (SLTCResolveTypeNode(c, typeNode, &ignoredType) != 0) {
        return -1;
    }
    *outType = c->typeType;
    return 0;
}

int SLTCTypeExpr_INT(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedInt;
    return 0;
}

int SLTCTypeExpr_FLOAT(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedFloat;
    return 0;
}

int SLTCTypeExpr_STRING(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t strRefType = SLTCGetStrRefType(c, n->start, n->end);
    if (strRefType < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
    }
    *outType = strRefType;
    return 0;
}

int SLTCTypeExpr_RUNE(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeUntypedInt;
    return 0;
}

int SLTCTypeExpr_BOOL(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeBool;
    return 0;
}

int SLTCTypeExpr_COMPOUND_LIT(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    (void)n;
    return SLTCTypeCompoundLit(c, nodeId, -1, outType);
}

int SLTCTypeExpr_CALL_WITH_CONTEXT(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t savedActive = c->activeCallWithNode;
    int32_t callNode = SLAstFirstChild(c->ast, nodeId);
    (void)n;
    if (callNode < 0 || c->ast->nodes[callNode].kind != SLAst_CALL) {
        return SLTCFailNode(c, nodeId, SLDiag_WITH_CONTEXT_ON_NON_CALL);
    }
    c->activeCallWithNode = nodeId;
    if (SLTCValidateCurrentCallOverlay(c) != 0) {
        c->activeCallWithNode = savedActive;
        return -1;
    }
    if (SLTCTypeExpr(c, callNode, outType) != 0) {
        c->activeCallWithNode = savedActive;
        return -1;
    }
    c->activeCallWithNode = savedActive;
    return 0;
}

int SLTCTypeExpr_NEW(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    (void)n;
    if (c->currentFunctionIsCompareHook) {
        return SLTCFailNode(c, nodeId, SLDiag_COMPARISON_HOOK_IMPURE);
    }
    return SLTCTypeNewExpr(c, nodeId, outType);
}

int SLTCTypeSourceLocationOfCall(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* callee, int32_t* outType) {
    int32_t argNode = SLAstNextSibling(c->ast, SLAstFirstChild(c->ast, nodeId));
    int32_t argType;
    int32_t nextArgNode;
    if (argNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
    if (SLTCTypeExpr(c, argNode, &argType) != 0) {
        return -1;
    }
    (void)argType;
    nextArgNode = SLAstNextSibling(c->ast, argNode);
    if (nextArgNode >= 0) {
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
    if (c->typeSourceLocation < 0) {
        c->typeSourceLocation = SLTCFindSourceLocationType(c);
    }
    if (c->typeSourceLocation < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
    }
    *outType = c->typeSourceLocation;
    (void)callee;
    return 0;
}

static int SLTCTypeEmitCompilerDiagCall(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t spanNode, int32_t msgNode, SLTCCompilerDiagOp op) {
    SLTCConstEvalCtx evalCtx;
    SLCTFEValue      msgValue;
    int              msgIsConst = 0;
    uint32_t         diagStart = c->ast->nodes[nodeId].start;
    uint32_t         diagEnd = c->ast->nodes[nodeId].end;
    const char*      detail;
    SLDiag           emitted;
    int32_t          msgExprNode = msgNode;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    if (msgNode >= 0 && (uint32_t)msgNode < c->ast->len
        && c->ast->nodes[msgNode].kind == SLAst_CALL_ARG)
    {
        int32_t inner = SLAstFirstChild(c->ast, msgNode);
        if (inner >= 0) {
            msgExprNode = inner;
        }
    }
    if (op == SLTCCompilerDiagOp_ERROR_AT || op == SLTCCompilerDiagOp_WARN_AT) {
        int        spanIsConst = 0;
        SLCTFESpan span;
        uint32_t   spanStartOffset = 0;
        uint32_t   spanEndOffset = 0;
        if (SLTCConstEvalSourceLocationExpr(&evalCtx, spanNode, &span, &spanIsConst) != 0) {
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
    if (SLTCEvalConstExprNode(&evalCtx, msgExprNode, &msgValue, &msgIsConst) != 0) {
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
    if (emitted.type == SLDiagType_WARNING) {
        return SLTCEmitWarningDiag(c, &emitted);
    }
    if (c->diag != NULL) {
        *c->diag = emitted;
    }
    return -1;
}

int SLTCTypeCompilerDiagCall(
    SLTypeCheckCtx*    c,
    int32_t            nodeId,
    const SLAstNode*   callee,
    SLTCCompilerDiagOp op,
    int32_t*           outType) {
    int32_t arg1Node = SLAstNextSibling(c->ast, SLAstFirstChild(c->ast, nodeId));
    int32_t arg2Node = arg1Node >= 0 ? SLAstNextSibling(c->ast, arg1Node) : -1;
    int32_t arg3Node = arg2Node >= 0 ? SLAstNextSibling(c->ast, arg2Node) : -1;
    int32_t spanNode = -1;
    int32_t msgNode;
    int32_t msgType;
    int32_t wantStrType;
    if (op == SLTCCompilerDiagOp_ERROR || op == SLTCCompilerDiagOp_WARN) {
        if (arg1Node < 0 || arg2Node >= 0) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        msgNode = arg1Node;
    } else {
        int32_t spanType;
        if (arg1Node < 0 || arg2Node < 0 || arg3Node >= 0) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        spanNode = arg1Node;
        if (SLTCTypeExpr(c, arg1Node, &spanType) != 0) {
            return -1;
        }
        if (!SLTCTypeIsSourceLocation(c, spanType)) {
            return SLTCFailNode(c, arg1Node, SLDiag_TYPE_MISMATCH);
        }
        msgNode = arg2Node;
    }
    if (SLTCTypeExpr(c, msgNode, &msgType) != 0) {
        return -1;
    }
    wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
    if (wantStrType < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
    }
    if (!SLTCCanAssign(c, wantStrType, msgType)) {
        return SLTCFailNode(c, msgNode, SLDiag_TYPE_MISMATCH);
    }
    if (c->activeConstEvalCtx == NULL && c->compilerDiagPathProven != 0) {
        if (SLTCTypeEmitCompilerDiagCall(c, nodeId, spanNode, msgNode, op) != 0) {
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
} SLTCCopySeqInfo;

static int32_t SLTCGetStringSliceExprType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t start, uint32_t end) {
    int32_t         resolvedType;
    const SLTCType* t;
    int32_t         resolvedBaseType;
    if (c == NULL) {
        return -1;
    }
    resolvedType = SLTCResolveAliasBaseType(c, baseType);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return -1;
    }
    if (resolvedType == c->typeStr) {
        return c->typeStr;
    }
    t = &c->types[resolvedType];
    if (t->kind != SLTCType_PTR && t->kind != SLTCType_REF) {
        return -1;
    }
    resolvedBaseType = SLTCResolveAliasBaseType(c, t->baseType);
    if (resolvedBaseType != c->typeStr) {
        return -1;
    }
    if (t->kind == SLTCType_PTR) {
        return SLTCGetStrPtrType(c, start, end);
    }
    return SLTCGetStrRefType(c, start, end);
}

static int SLTCFailUnsliceableExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t baseType) {
    char        typeBuf[128];
    char        detailBuf[160];
    SLTCTextBuf typeText;
    SLTCTextBuf detailText;
    int         rc = SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    SLTCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
    SLTCFormatTypeRec(c, baseType, &typeText, 0);
    SLTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    SLTCTextBufAppendCStr(&detailText, "cannot slice expression of type ");
    SLTCTextBufAppendCStr(&detailText, typeBuf);
    c->diag->detail = SLTCAllocDiagText(c, detailBuf);
    return rc;
}

static int SLTCFailInvalidCast(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t srcType, int32_t dstType) {
    char        srcBuf[128];
    char        dstBuf[128];
    char        detailBuf[256];
    SLTCTextBuf srcText;
    SLTCTextBuf dstText;
    SLTCTextBuf detailText;
    int         rc = SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    if (rc != -1 || c == NULL || c->diag == NULL) {
        return rc;
    }
    SLTCTextBufInit(&srcText, srcBuf, (uint32_t)sizeof(srcBuf));
    SLTCFormatTypeRec(c, srcType, &srcText, 0);
    SLTCTextBufInit(&dstText, dstBuf, (uint32_t)sizeof(dstBuf));
    SLTCFormatTypeRec(c, dstType, &dstText, 0);
    SLTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    SLTCTextBufAppendCStr(&detailText, "cannot cast ");
    SLTCTextBufAppendCStr(&detailText, srcBuf);
    SLTCTextBufAppendCStr(&detailText, " to ");
    SLTCTextBufAppendCStr(&detailText, dstBuf);
    c->diag->detail = SLTCAllocDiagText(c, detailBuf);
    return rc;
}

static int SLTCResolveCopySeqInfo(SLTypeCheckCtx* c, int32_t typeId, SLTCCopySeqInfo* out) {
    int32_t         resolvedType;
    const SLTCType* t;
    int32_t         u8Type;
    if (c == NULL || out == NULL) {
        return -1;
    }
    out->elemType = -1;
    out->isString = 0;
    out->writable = 0;
    u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
    if (u8Type < 0) {
        return -1;
    }
    resolvedType = SLTCResolveAliasBaseType(c, typeId);
    if (resolvedType < 0 || (uint32_t)resolvedType >= c->typeLen) {
        return 1;
    }
    if (resolvedType == c->typeStr) {
        out->elemType = u8Type;
        out->isString = 1;
        return 0;
    }
    t = &c->types[resolvedType];
    if (t->kind == SLTCType_ARRAY) {
        out->elemType = t->baseType;
        out->writable = 1;
        return 0;
    }
    if (t->kind == SLTCType_SLICE) {
        out->elemType = t->baseType;
        out->writable = SLTCTypeIsMutable(t);
        return 0;
    }
    if (t->kind == SLTCType_PTR || t->kind == SLTCType_REF) {
        int32_t         baseType = t->baseType;
        int32_t         resolvedBaseType;
        const SLTCType* base;
        if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
            return 1;
        }
        resolvedBaseType = SLTCResolveAliasBaseType(c, baseType);
        if (resolvedBaseType < 0 || (uint32_t)resolvedBaseType >= c->typeLen) {
            return 1;
        }
        if (resolvedBaseType == c->typeStr) {
            out->elemType = u8Type;
            out->isString = 1;
            out->writable = t->kind == SLTCType_PTR;
            return 0;
        }
        base = &c->types[resolvedBaseType];
        if (base->kind == SLTCType_ARRAY) {
            out->elemType = base->baseType;
            out->writable = t->kind == SLTCType_PTR;
            return 0;
        }
        if (base->kind == SLTCType_SLICE) {
            out->elemType = base->baseType;
            out->writable = t->kind == SLTCType_PTR && SLTCTypeIsMutable(base);
            return 0;
        }
    }
    return 1;
}

int SLTCTypeExpr_CALL(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t calleeNode = SLAstFirstChild(c->ast, nodeId);
    int32_t calleeType;
    (void)n;
    if (calleeNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_NOT_CALLABLE);
    }
    if (c->currentFunctionIsCompareHook) {
        return SLTCFailNode(c, nodeId, SLDiag_COMPARISON_HOOK_IMPURE);
    }
    if (c->ast->nodes[calleeNode].kind == SLAst_IDENT) {
        const SLAstNode*   callee = &c->ast->nodes[calleeNode];
        SLTCCompilerDiagOp diagOp = SLTCCompilerDiagOpFromName(
            c, callee->dataStart, callee->dataEnd);
        if (diagOp != SLTCCompilerDiagOp_NONE) {
            return SLTCTypeCompilerDiagCall(c, nodeId, callee, diagOp, outType);
        }
        if (SLTCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd)) {
            return SLTCTypeSourceLocationOfCall(c, nodeId, callee, outType);
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            int32_t argNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t kindType;
            int32_t u8Type;
            if (argNode >= 0) {
                if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = SLAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    kindType = SLTCFindReflectKindType(c);
                    if (kindType >= 0) {
                        *outType = kindType;
                        return 0;
                    }
                    u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
                    if (u8Type < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                    }
                    *outType = u8Type;
                    return 0;
                }
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            int32_t argNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode >= 0) {
                if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = SLAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    if (c->typeBool < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                    }
                    *outType = c->typeBool;
                    return 0;
                }
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            int32_t argNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t strRefType;
            if (argNode >= 0) {
                if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = SLAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    strRefType = SLTCGetStrRefType(c, callee->start, callee->end);
                    if (strRefType < 0) {
                        return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
                    }
                    *outType = strRefType;
                    return 0;
                }
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            int32_t argNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            int32_t reflectedTypeId;
            if (argNode >= 0) {
                if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                nextArgNode = SLAstNextSibling(c->ast, argNode);
                if (nextArgNode < 0 && argType == c->typeType) {
                    if (SLTCResolveReflectedTypeValueExpr(c, argNode, &reflectedTypeId) != 0) {
                        return SLTCFailNode(c, argNode, SLDiag_TYPE_MISMATCH);
                    }
                    if (c->types[reflectedTypeId].kind != SLTCType_ALIAS) {
                        return SLTCFailNode(c, argNode, SLDiag_TYPE_MISMATCH);
                    }
                    if (SLTCResolveAliasTypeId(c, reflectedTypeId) != 0) {
                        return -1;
                    }
                    if (c->typeType < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                    }
                    *outType = c->typeType;
                    return 0;
                }
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "typeof")) {
            int32_t argNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                return -1;
            }
            (void)argType;
            nextArgNode = SLAstNextSibling(c->ast, argNode);
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (c->typeType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            *outType = c->typeType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "ptr")
            || SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "slice"))
        {
            int32_t argNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t argType;
            int32_t nextArgNode;
            if (argNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                return -1;
            }
            if (argType != c->typeType) {
                return SLTCFailNode(c, argNode, SLDiag_TYPE_MISMATCH);
            }
            nextArgNode = SLAstNextSibling(c->ast, argNode);
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            *outType = c->typeType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "array")) {
            int32_t typeArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t lenArgNode = typeArgNode >= 0 ? SLAstNextSibling(c->ast, typeArgNode) : -1;
            int32_t nextArgNode = lenArgNode >= 0 ? SLAstNextSibling(c->ast, lenArgNode) : -1;
            int32_t typeArgType;
            int32_t lenArgType;
            if (typeArgNode < 0 || lenArgNode < 0 || nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, typeArgNode, &typeArgType) != 0
                || SLTCTypeExpr(c, lenArgNode, &lenArgType) != 0)
            {
                return -1;
            }
            if (typeArgType != c->typeType || !SLTCIsIntegerType(c, lenArgType)) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            *outType = c->typeType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len")) {
            int32_t strArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t strArgType;
            int32_t nextArgNode;
            int32_t intType;
            if (strArgNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, strArgNode, &strArgType) != 0) {
                return -1;
            }
            if (!SLTCTypeSupportsLen(c, strArgType)) {
                return SLTCFailNode(c, strArgNode, SLDiag_TYPE_MISMATCH);
            }
            nextArgNode = SLAstNextSibling(c->ast, strArgNode);
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            intType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
            if (intType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "copy")) {
            int32_t         dstNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t         srcNode = dstNode >= 0 ? SLAstNextSibling(c->ast, dstNode) : -1;
            int32_t         extraNode = srcNode >= 0 ? SLAstNextSibling(c->ast, srcNode) : -1;
            int32_t         dstType;
            int32_t         srcType;
            int32_t         dstElemResolved;
            int32_t         u8Type;
            int32_t         intType;
            SLTCCopySeqInfo dstInfo;
            SLTCCopySeqInfo srcInfo;
            if (dstNode < 0 || srcNode < 0 || extraNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, dstNode, &dstType) != 0 || SLTCTypeExpr(c, srcNode, &srcType) != 0)
            {
                return -1;
            }
            if (SLTCResolveCopySeqInfo(c, dstType, &dstInfo) != 0 || !dstInfo.writable
                || dstInfo.elemType < 0)
            {
                return SLTCFailNode(c, dstNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCResolveCopySeqInfo(c, srcType, &srcInfo) != 0 || srcInfo.elemType < 0) {
                return SLTCFailNode(c, srcNode, SLDiag_TYPE_MISMATCH);
            }
            if (dstInfo.isString) {
                if (!srcInfo.isString) {
                    return SLTCFailNode(c, srcNode, SLDiag_TYPE_MISMATCH);
                }
            } else if (srcInfo.isString) {
                u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
                if (u8Type < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                }
                dstElemResolved = SLTCResolveAliasBaseType(c, dstInfo.elemType);
                if (dstElemResolved < 0 || dstElemResolved != u8Type) {
                    return SLTCFailNode(c, srcNode, SLDiag_TYPE_MISMATCH);
                }
            } else if (!SLTCCanAssign(c, dstInfo.elemType, srcInfo.elemType)) {
                return SLTCFailNode(c, srcNode, SLDiag_TYPE_MISMATCH);
            }
            intType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
            if (intType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "cstr")) {
            int32_t strArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t strArgType;
            int32_t nextArgNode;
            int32_t u8Type;
            int32_t u8RefType;
            int32_t wantStrType;
            if (strArgNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, strArgNode, &strArgType) != 0) {
                return -1;
            }
            wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, strArgType)) {
                return SLTCFailNode(c, strArgNode, SLDiag_TYPE_MISMATCH);
            }
            nextArgNode = SLAstNextSibling(c->ast, strArgNode);
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
            if (u8Type < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            u8RefType = SLTCInternRefType(c, u8Type, 0, callee->start, callee->end);
            if (u8RefType < 0) {
                return -1;
            }
            *outType = u8RefType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "concat")) {
            int32_t aNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t bNode = aNode >= 0 ? SLAstNextSibling(c->ast, aNode) : -1;
            int32_t nextNode = bNode >= 0 ? SLAstNextSibling(c->ast, bNode) : -1;
            int32_t aType;
            int32_t bType;
            int32_t wantStrType;
            int32_t strPtrType;
            int32_t ctxMemType;
            int32_t allocBaseType = SLTCFindMemAllocatorType(c);
            int32_t allocParamType =
                allocBaseType < 0
                    ? -1
                    : SLTCInternRefType(c, allocBaseType, 1, callee->start, callee->end);
            if (aNode < 0 || bNode < 0 || nextNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (allocParamType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCGetEffectiveContextFieldTypeByLiteral(c, "mem", &ctxMemType) != 0) {
                return -1;
            }
            if (!SLTCCanAssign(c, allocParamType, ctxMemType)) {
                return SLTCFailNode(c, nodeId, SLDiag_CONTEXT_TYPE_MISMATCH);
            }
            if (SLTCTypeExpr(c, aNode, &aType) != 0 || SLTCTypeExpr(c, bNode, &bType) != 0) {
                return -1;
            }
            wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, aType) || !SLTCCanAssign(c, wantStrType, bType)) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            strPtrType = SLTCGetStrPtrType(c, callee->start, callee->end);
            if (strPtrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            *outType = strPtrType;
            return 0;
        }
        if (SLTCNameEqLiteralOrPkgBuiltin(c, callee->dataStart, callee->dataEnd, "fmt", "builtin"))
        {
            int32_t        outNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t        fmtNode = outNode >= 0 ? SLAstNextSibling(c->ast, outNode) : -1;
            int32_t        argNode;
            int32_t        outBufType;
            int32_t        fmtType;
            int32_t        wantStrType;
            int32_t        strPtrType;
            int32_t        intType;
            int32_t        argNodes[SLTC_MAX_CALL_ARGS];
            int32_t        argTypes[SLTC_MAX_CALL_ARGS];
            uint32_t       argCount = 0;
            uint32_t       i;
            const uint8_t* fmtBytes = NULL;
            uint32_t       fmtLen = 0;
            int            fmtIsConst = 0;
            if (outNode < 0 || fmtNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, outNode, &outBufType) != 0) {
                return -1;
            }
            strPtrType = SLTCGetStrPtrType(c, callee->start, callee->end);
            if (strPtrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, strPtrType, outBufType)) {
                return SLTCFailNode(c, outNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCTypeExpr(c, fmtNode, &fmtType) != 0) {
                return -1;
            }
            wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, fmtType)) {
                return SLTCFailNode(c, fmtNode, SLDiag_TYPE_MISMATCH);
            }
            argNode = SLAstNextSibling(c->ast, fmtNode);
            while (argNode >= 0) {
                int32_t argType;
                if (argCount >= SLTC_MAX_CALL_ARGS) {
                    return SLTCFailNode(c, argNode, SLDiag_ARITY_MISMATCH);
                }
                if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                argNodes[argCount] = argNode;
                argTypes[argCount] = argType;
                argCount++;
                argNode = SLAstNextSibling(c->ast, argNode);
            }
            if (SLTCConstStringExpr(c, fmtNode, &fmtBytes, &fmtLen, &fmtIsConst) != 0) {
                return -1;
            }
            if (fmtIsConst) {
                SLFmtToken      tokens[512];
                uint32_t        tokenLen = 0;
                uint32_t        placeholderCount = 0;
                SLFmtParseError parseErr = { 0 };
                if (SLFmtParseBytes(
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
                    if (parseErr.code == SLFmtParseErr_TOKEN_OVERFLOW) {
                        return SLTCFailNode(c, fmtNode, SLDiag_ARENA_OOM);
                    }
                    if (parseErr.end > parseErr.start && errStart + parseErr.end <= errEnd) {
                        errStart += parseErr.start;
                        errEnd = c->ast->nodes[fmtNode].start + parseErr.end;
                    }
                    return SLTCFailSpan(c, SLDiag_FORMAT_INVALID, errStart, errEnd);
                }
                for (i = 0; i < tokenLen; i++) {
                    if (tokens[i].kind == SLFmtTok_PLACEHOLDER_F
                        || tokens[i].kind == SLFmtTok_PLACEHOLDER_S)
                    {
                        uint32_t errStart = c->ast->nodes[fmtNode].start;
                        uint32_t errEnd = c->ast->nodes[fmtNode].end;
                        if (tokens[i].end > tokens[i].start
                            && errStart + tokens[i].end <= c->ast->nodes[fmtNode].end)
                        {
                            errStart += tokens[i].start;
                            errEnd = c->ast->nodes[fmtNode].start + tokens[i].end;
                        }
                        return SLTCFailSpan(c, SLDiag_FORMAT_INVALID, errStart, errEnd);
                    }
                    if (tokens[i].kind == SLFmtTok_PLACEHOLDER_I
                        || tokens[i].kind == SLFmtTok_PLACEHOLDER_R)
                    {
                        placeholderCount++;
                    }
                }
                if (placeholderCount != argCount) {
                    return SLTCFailNode(c, nodeId, SLDiag_FORMAT_ARG_COUNT_MISMATCH);
                }
                {
                    uint32_t placeholderIndex = 0;
                    for (i = 0; i < tokenLen; i++) {
                        if (tokens[i].kind == SLFmtTok_PLACEHOLDER_I
                            || tokens[i].kind == SLFmtTok_PLACEHOLDER_R)
                        {
                            uint32_t idx = placeholderIndex++;
                            if (idx >= argCount) {
                                return SLTCFailNode(c, nodeId, SLDiag_FORMAT_ARG_COUNT_MISMATCH);
                            }
                            if (tokens[i].kind == SLFmtTok_PLACEHOLDER_I) {
                                if (!SLTCIsIntegerType(c, argTypes[idx])) {
                                    return SLTCFailNode(
                                        c, argNodes[idx], SLDiag_FORMAT_ARG_TYPE_MISMATCH);
                                }
                            } else if (!SLTCTypeSupportsFmtReflectRec(c, argTypes[idx], 0u)) {
                                return SLTCFailNode(
                                    c, argNodes[idx], SLDiag_FORMAT_UNSUPPORTED_TYPE);
                            }
                        }
                    }
                }
            } else {
                for (i = 0; i < argCount; i++) {
                    if (!SLTCTypeSupportsFmtReflectRec(c, argTypes[i], 0u)) {
                        return SLTCFailNode(c, argNodes[i], SLDiag_FORMAT_UNSUPPORTED_TYPE);
                    }
                }
            }
            intType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
            if (intType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "free")) {
            int32_t arg1Node = SLAstNextSibling(c->ast, calleeNode);
            int32_t arg2Node = arg1Node >= 0 ? SLAstNextSibling(c->ast, arg1Node) : -1;
            int32_t arg3Node = arg2Node >= 0 ? SLAstNextSibling(c->ast, arg2Node) : -1;
            int32_t allocArgNode = -1;
            int32_t valueArgNode = -1;
            int32_t valueType;
            int32_t allocType;
            int32_t ctxMemType;
            int32_t allocBaseType = SLTCFindMemAllocatorType(c);
            int32_t allocParamType =
                allocBaseType < 0
                    ? -1
                    : SLTCInternRefType(c, allocBaseType, 1, callee->start, callee->end);
            if (arg1Node < 0 || arg3Node >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (arg2Node >= 0) {
                allocArgNode = arg1Node;
                valueArgNode = arg2Node;
            } else {
                valueArgNode = arg1Node;
            }
            if (allocParamType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            if (allocArgNode >= 0) {
                if (SLTCTypeExpr(c, allocArgNode, &allocType) != 0) {
                    return -1;
                }
                if (!SLTCCanAssign(c, allocParamType, allocType)) {
                    return SLTCFailNode(c, allocArgNode, SLDiag_TYPE_MISMATCH);
                }
            } else {
                if (SLTCGetEffectiveContextFieldTypeByLiteral(c, "mem", &ctxMemType) != 0) {
                    return -1;
                }
                if (!SLTCCanAssign(c, allocParamType, ctxMemType)) {
                    return SLTCFailNode(c, nodeId, SLDiag_CONTEXT_TYPE_MISMATCH);
                }
            }
            if (SLTCTypeExpr(c, valueArgNode, &valueType) != 0) {
                return -1;
            }
            if (!SLTCTypeIsFreeablePointer(c, valueType)) {
                return SLTCFailNode(c, valueArgNode, SLDiag_TYPE_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "panic")) {
            int32_t msgArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t msgArgType;
            int32_t nextArgNode;
            int32_t wantStrType;
            if (msgArgNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, msgArgNode, &msgArgType) != 0) {
                return -1;
            }
            wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, msgArgType)) {
                return SLTCFailNode(c, msgArgNode, SLDiag_TYPE_MISMATCH);
            }
            nextArgNode = SLAstNextSibling(c->ast, msgArgNode);
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "print")) {
            int32_t msgArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t msgArgType;
            int32_t nextArgNode;
            int32_t logType;
            int32_t wantStrType;
            if (msgArgNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCTypeExpr(c, msgArgNode, &msgArgType) != 0) {
                return -1;
            }
            wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, msgArgType)) {
                return SLTCFailNode(c, msgArgNode, SLDiag_TYPE_MISMATCH);
            }
            nextArgNode = SLAstNextSibling(c->ast, msgArgNode);
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCGetEffectiveContextFieldTypeByLiteral(c, "log", &logType) != 0) {
                return -1;
            }
            *outType = c->typeVoid;
            return 0;
        }
        {
            SLTCCallArgInfo callArgs[SLTC_MAX_CALL_ARGS];
            uint32_t        argCount = 0;
            int32_t         resolvedFn = -1;
            int32_t         mutRefTempArgNode = -1;
            int             status;
            if (SLTCCollectCallArgInfo(c, nodeId, calleeNode, 0, -1, callArgs, NULL, &argCount)
                != 0)
            {
                return -1;
            }
            status = SLTCResolveCallByName(
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
                    dependentStatus = SLTCResolveDependentPtrReturnForCall(
                        c, resolvedFn, callArgs[0].exprNode, &dependentReturnType);
                }
                if (dependentStatus < 0) {
                    return -1;
                }
                if (SLTCValidateCallContextRequirements(c, c->funcs[resolvedFn].contextType) != 0) {
                    return -1;
                }
                if (SLTCRecordCallTarget(c, nodeId, resolvedFn) != 0) {
                    return -1;
                }
                SLTCMarkFunctionUsed(c, resolvedFn);
                *outType =
                    dependentStatus == 1 ? dependentReturnType : c->funcs[resolvedFn].returnType;
                return 0;
            }
            if (status == 2) {
                return SLTCFailSpan(
                    c, SLDiag_NO_MATCHING_OVERLOAD, callee->dataStart, callee->dataEnd);
            }
            if (status == 3) {
                return SLTCFailSpan(c, SLDiag_AMBIGUOUS_CALL, callee->dataStart, callee->dataEnd);
            }
            if (status == 4) {
                return SLTCFailNode(c, mutRefTempArgNode, SLDiag_COMPOUND_MUT_REF_TEMPORARY);
            }
            if (status == 5) {
                return SLTCFailSpan(
                    c, SLDiag_COMPOUND_INFER_AMBIGUOUS, callee->dataStart, callee->dataEnd);
            }
            if (status == 6) {
                return -1;
            }
        }
    } else if (c->ast->nodes[calleeNode].kind == SLAst_FIELD_EXPR) {
        const SLAstNode* callee = &c->ast->nodes[calleeNode];
        int32_t          recvNode = SLAstFirstChild(c->ast, calleeNode);
        int32_t          recvType;
        int32_t          fieldType;
        if (recvNode >= 0 && (uint32_t)recvNode < c->ast->len
            && c->ast->nodes[recvNode].kind == SLAst_IDENT)
        {
            const SLAstNode* recv = &c->ast->nodes[recvNode];
            if (SLNameEqLiteral(c->src, recv->dataStart, recv->dataEnd, "builtin")
                && SLTCIsSourceLocationOfName(c, callee->dataStart, callee->dataEnd))
            {
                return SLTCTypeSourceLocationOfCall(c, nodeId, callee, outType);
            }
            if (SLNameEqLiteral(c->src, recv->dataStart, recv->dataEnd, "compiler")) {
                SLTCCompilerDiagOp diagOp = SLTCCompilerDiagOpFromName(
                    c, callee->dataStart, callee->dataEnd);
                if (diagOp != SLTCCompilerDiagOp_NONE) {
                    return SLTCTypeCompilerDiagCall(c, nodeId, callee, diagOp, outType);
                }
            }
        }
        if (recvNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_NOT_CALLABLE);
        }
        if (SLTCTypeExpr(c, recvNode, &recvType) != 0) {
            return -1;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "kind")) {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t kindType;
            int32_t u8Type;
            if (nextArgNode < 0 && recvType == c->typeType) {
                kindType = SLTCFindReflectKindType(c);
                if (kindType >= 0) {
                    *outType = kindType;
                    return 0;
                }
                u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
                if (u8Type < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                }
                *outType = u8Type;
                return 0;
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "is_alias")) {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            if (nextArgNode < 0 && recvType == c->typeType) {
                if (c->typeBool < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                }
                *outType = c->typeBool;
                return 0;
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "type_name")) {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t strRefType;
            if (nextArgNode < 0 && recvType == c->typeType) {
                strRefType = SLTCGetStrRefType(c, callee->start, callee->end);
                if (strRefType < 0) {
                    return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
                }
                *outType = strRefType;
                return 0;
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "base")) {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t reflectedTypeId;
            if (nextArgNode < 0 && recvType == c->typeType) {
                if (SLTCResolveReflectedTypeValueExpr(c, recvNode, &reflectedTypeId) != 0) {
                    return SLTCFailNode(c, recvNode, SLDiag_TYPE_MISMATCH);
                }
                if (c->types[reflectedTypeId].kind != SLTCType_ALIAS) {
                    return SLTCFailNode(c, recvNode, SLDiag_TYPE_MISMATCH);
                }
                if (SLTCResolveAliasTypeId(c, reflectedTypeId) != 0) {
                    return -1;
                }
                if (c->typeType < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                }
                *outType = c->typeType;
                return 0;
            }
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len")
            && SLTCTypeSupportsLen(c, recvType))
        {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t intType;
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            intType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
            if (intType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            *outType = intType;
            return 0;
        }
        if (SLTCFieldLookup(c, recvType, callee->dataStart, callee->dataEnd, &fieldType, NULL) == 0)
        {
            calleeType = fieldType;
            goto typed_call_from_callee_type;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "cstr")) {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t u8Type;
            int32_t u8RefType;
            int32_t wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, recvType)) {
                return SLTCFailNode(c, recvNode, SLDiag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
            if (u8Type < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            u8RefType = SLTCInternRefType(c, u8Type, 0, callee->start, callee->end);
            if (u8RefType < 0) {
                return -1;
            }
            *outType = u8RefType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "free")) {
            int32_t valueArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t nextArgNode = valueArgNode >= 0 ? SLAstNextSibling(c->ast, valueArgNode) : -1;
            int32_t allocBaseType;
            int32_t allocParamType;
            int32_t valueType;
            if (valueArgNode < 0 || nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            allocBaseType = SLTCFindMemAllocatorType(c);
            if (allocBaseType < 0) {
                return SLTCFailNode(c, recvNode, SLDiag_TYPE_MISMATCH);
            }
            allocParamType = SLTCInternRefType(c, allocBaseType, 1, callee->start, callee->end);
            if (allocParamType < 0) {
                return -1;
            }
            if (!SLTCCanAssign(c, allocParamType, recvType)) {
                return SLTCFailNode(c, recvNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCTypeExpr(c, valueArgNode, &valueType) != 0) {
                return -1;
            }
            if (!SLTCTypeIsFreeablePointer(c, valueType)) {
                return SLTCFailNode(c, valueArgNode, SLDiag_TYPE_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "panic")) {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, recvType)) {
                return SLTCFailNode(c, recvNode, SLDiag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            *outType = c->typeVoid;
            return 0;
        }
        if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "print")) {
            int32_t nextArgNode = SLAstNextSibling(c->ast, calleeNode);
            int32_t logType;
            int32_t wantStrType = SLTCGetStrRefType(c, callee->start, callee->end);
            if (wantStrType < 0) {
                return SLTCFailNode(c, calleeNode, SLDiag_UNKNOWN_TYPE);
            }
            if (!SLTCCanAssign(c, wantStrType, recvType)) {
                return SLTCFailNode(c, recvNode, SLDiag_TYPE_MISMATCH);
            }
            if (nextArgNode >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCGetEffectiveContextFieldTypeByLiteral(c, "log", &logType) != 0) {
                return -1;
            }
            *outType = c->typeVoid;
            return 0;
        }

        {
            SLTCCallArgInfo callArgs[SLTC_MAX_CALL_ARGS];
            uint32_t        argCount = 0;
            int32_t         resolvedFn = -1;
            int32_t         mutRefTempArgNode = -1;
            int             status;
            int             recvPkgStatus = 0;
            uint32_t        recvPkgStart = 0;
            uint32_t        recvPkgEnd = 0;
            if (SLTCCollectCallArgInfo(
                    c, nodeId, calleeNode, 1, recvNode, callArgs, NULL, &argCount)
                != 0)
            {
                return -1;
            }
            status = SLTCResolveCallByName(
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
                status = SLTCResolveCallByName(
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
            recvPkgStatus = SLTCResolveReceiverPkgPrefix(c, recvType, &recvPkgStart, &recvPkgEnd);
            if (recvPkgStatus < 0) {
                return -1;
            }
            if ((status == 1 || status == 2) && recvPkgStatus == 1) {
                int prefixedStatus = SLTCResolveCallByPkgMethod(
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
                    prefixedStatus = SLTCResolveCallByPkgMethod(
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
                    dependentStatus = SLTCResolveDependentPtrReturnForCall(
                        c, resolvedFn, callArgs[0].exprNode, &dependentReturnType);
                }
                if (dependentStatus < 0) {
                    return -1;
                }
                if (SLTCValidateCallContextRequirements(c, c->funcs[resolvedFn].contextType) != 0) {
                    return -1;
                }
                if (SLTCRecordCallTarget(c, nodeId, resolvedFn) != 0) {
                    return -1;
                }
                SLTCMarkFunctionUsed(c, resolvedFn);
                *outType =
                    dependentStatus == 1 ? dependentReturnType : c->funcs[resolvedFn].returnType;
                return 0;
            }
            if (status == 2) {
                return SLTCFailSpan(
                    c, SLDiag_NO_MATCHING_OVERLOAD, callee->dataStart, callee->dataEnd);
            }
            if (status == 3) {
                return SLTCFailSpan(c, SLDiag_AMBIGUOUS_CALL, callee->dataStart, callee->dataEnd);
            }
            if (status == 4) {
                return SLTCFailNode(c, mutRefTempArgNode, SLDiag_COMPOUND_MUT_REF_TEMPORARY);
            }
            if (status == 5) {
                return SLTCFailSpan(
                    c, SLDiag_COMPOUND_INFER_AMBIGUOUS, callee->dataStart, callee->dataEnd);
            }
            if (status == 6) {
                return -1;
            }
            return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, callee->dataStart, callee->dataEnd);
        }
    }
    if (SLTCTypeExpr(c, calleeNode, &calleeType) != 0) {
        return -1;
    }
typed_call_from_callee_type: {
    int32_t          fnReturnType;
    uint32_t         fnParamStart = 0;
    uint32_t         fnParamCount = 0;
    int              fnIsVariadic = 0;
    int32_t          fnIndexForDependent = -1;
    SLTCCallArgInfo  callArgs[SLTC_MAX_CALL_ARGS];
    uint32_t         paramNameStarts[SLTC_MAX_CALL_ARGS];
    uint32_t         paramNameEnds[SLTC_MAX_CALL_ARGS];
    uint8_t          paramFlags[SLTC_MAX_CALL_ARGS];
    uint32_t         callArgCount = 0;
    uint32_t         p;
    int              hasParamNames = 1;
    int              prepStatus;
    SLTCCallMapError mapError;
    SLTCCallBinding  binding;
    if (SLTCGetFunctionTypeSignature(
            c, calleeType, &fnReturnType, &fnParamStart, &fnParamCount, &fnIsVariadic)
        != 0)
    {
        return SLTCFailNode(c, calleeNode, SLDiag_NOT_CALLABLE);
    }
    if (SLTCCollectCallArgInfo(c, nodeId, calleeNode, 0, -1, callArgs, NULL, &callArgCount) != 0) {
        return -1;
    }
    if (!fnIsVariadic && callArgCount != fnParamCount) {
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
    if (fnIsVariadic && callArgCount < (fnParamCount > 0 ? (fnParamCount - 1u) : 0u)) {
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
    for (p = 0; p < fnParamCount; p++) {
        paramNameStarts[p] = c->funcParamNameStarts[fnParamStart + p];
        paramNameEnds[p] = c->funcParamNameEnds[fnParamStart + p];
        paramFlags[p] = c->funcParamFlags[fnParamStart + p];
    }
    if (calleeType >= 0 && (uint32_t)calleeType < c->typeLen && c->types[calleeType].funcIndex >= 0
        && (uint32_t)c->types[calleeType].funcIndex < c->funcLen)
    {
        const SLTCFunction* fn = &c->funcs[c->types[calleeType].funcIndex];
        fnIndexForDependent = c->types[calleeType].funcIndex;
        if (SLTCRecordCallTarget(c, nodeId, fnIndexForDependent) != 0) {
            return -1;
        }
        if (fn->paramCount == fnParamCount
            && (((fn->flags & SLTCFunctionFlag_VARIADIC) != 0) == (fnIsVariadic != 0)))
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
    SLTCCallMapErrorClear(&mapError);
    prepStatus = SLTCPrepareCallBinding(
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
            SLTCSetDiagWithArg(
                c->diag,
                mapError.code,
                mapError.start,
                mapError.end,
                mapError.argStart,
                mapError.argEnd);
            return -1;
        }
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
    for (p = 0; p < callArgCount; p++) {
        int32_t argType;
        int32_t paramType;
        int32_t argExprNode = callArgs[p].exprNode;
        paramType = binding.argExpectedTypes[p];
        if (paramType < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        if (SLTCTypeExprExpected(c, argExprNode, paramType, &argType) != 0) {
            return -1;
        }
        if (fnIsVariadic && p == binding.spreadArgIndex
            && binding.variadicParamType == c->typeAnytype)
        {
            int32_t spreadType = SLTCResolveAliasBaseType(c, argType);
            if (spreadType < 0 || (uint32_t)spreadType >= c->typeLen
                || c->types[spreadType].kind != SLTCType_PACK)
            {
                return SLTCFailNode(c, argExprNode, SLDiag_ANYTYPE_SPREAD_REQUIRES_PACK);
            }
        }
        if (SLTCIsMutableRefType(c, paramType) && SLTCExprIsCompoundTemporary(c, argExprNode)) {
            return SLTCFailNode(c, argExprNode, SLDiag_COMPOUND_MUT_REF_TEMPORARY);
        }
        if (!SLTCCanAssign(c, paramType, argType)) {
            if (fnIsVariadic && p >= binding.fixedInputCount) {
                return SLTCFailNode(
                    c,
                    argExprNode,
                    binding.spreadArgIndex == p
                        ? SLDiag_VARIADIC_SPREAD_NON_SLICE
                        : SLDiag_VARIADIC_ARG_TYPE_MISMATCH);
            }
            return SLTCFailNode(c, argExprNode, SLDiag_TYPE_MISMATCH);
        }
    }
    SLTCCallMapErrorClear(&mapError);
    {
        int constStatus = SLTCCheckConstParamArgs(
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
            SLTCSetDiagWithArg(
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
        int     dependentStatus = SLTCResolveDependentPtrReturnForCall(
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

int SLTCTypeExpr_CAST(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t         exprNode = SLAstFirstChild(c->ast, nodeId);
    int32_t         typeNode;
    int32_t         sourceType;
    int32_t         resolvedSourceType;
    int32_t         targetType;
    int32_t         resolvedTargetType;
    const SLTCType* src;
    const SLTCType* dst;
    (void)n;
    if (exprNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    typeNode = SLAstNextSibling(c->ast, exprNode);
    if (typeNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    if (SLTCTypeExpr(c, exprNode, &sourceType) != 0) {
        return -1;
    }
    c->allowConstNumericTypeName = 1;
    if (SLTCResolveTypeNode(c, typeNode, &targetType) != 0) {
        c->allowConstNumericTypeName = 0;
        return -1;
    }
    c->allowConstNumericTypeName = 0;
    resolvedSourceType = SLTCResolveAliasBaseType(c, sourceType);
    resolvedTargetType = SLTCResolveAliasBaseType(c, targetType);
    if (resolvedSourceType < 0 || (uint32_t)resolvedSourceType >= c->typeLen
        || resolvedTargetType < 0 || (uint32_t)resolvedTargetType >= c->typeLen)
    {
        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    }
    src = &c->types[resolvedSourceType];
    dst = &c->types[resolvedTargetType];
    if (src->kind == SLTCType_NULL
        && !(dst->kind == SLTCType_OPTIONAL || SLTCIsRawptrType(c, resolvedTargetType)))
    {
        return SLTCFailInvalidCast(c, nodeId, sourceType, targetType);
    }
    if (SLTCIsRawptrType(c, resolvedTargetType)) {
        if (!(src->kind == SLTCType_NULL || SLTCIsRawptrType(c, resolvedSourceType)
              || src->kind == SLTCType_PTR || src->kind == SLTCType_REF))
        {
            return SLTCFailInvalidCast(c, nodeId, sourceType, targetType);
        }
    } else if (SLTCIsRawptrType(c, resolvedSourceType)) {
        if (!(dst->kind == SLTCType_PTR || dst->kind == SLTCType_REF
              || SLTCIsRawptrType(c, resolvedTargetType)))
        {
            return SLTCFailInvalidCast(c, nodeId, sourceType, targetType);
        }
    }
    *outType = targetType;
    return 0;
}

int SLTCTypeExpr_SIZEOF(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t innerNode = SLAstFirstChild(c->ast, nodeId);
    int32_t innerType;
    if (innerNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    if (n->flags == 1) {
        if (SLTCResolveTypeNode(c, innerNode, &innerType) == 0) {
            if (SLTCTypeContainsVarSizeByValue(c, innerType)) {
                return SLTCFailNode(c, innerNode, SLDiag_TYPE_MISMATCH);
            }
            *outType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
            if (*outType < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
            }
            return 0;
        }
        if (c->ast->nodes[innerNode].kind == SLAst_TYPE_NAME) {
            int32_t localIdx = SLTCLocalFind(
                c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
            if (localIdx >= 0) {
                if (c->diag != NULL) {
                    *c->diag = (SLDiag){ 0 };
                }
                *outType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
                if (*outType < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                }
                return 0;
            }
            {
                int32_t fnIdx = SLTCFindFunctionIndex(
                    c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                if (fnIdx >= 0) {
                    if (c->diag != NULL) {
                        *c->diag = (SLDiag){ 0 };
                    }
                    *outType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
                    if (*outType < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                    }
                    return 0;
                }
            }
        }
    } else {
        if (SLTCTypeExpr(c, innerNode, &innerType) != 0) {
            return -1;
        }
    }
    *outType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
    if (*outType < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
    }
    return 0;
}

int SLTCTypeExpr_FIELD_EXPR(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t          recvNode = SLAstFirstChild(c->ast, nodeId);
    int32_t          recvType = -1;
    int32_t          fieldType = -1;
    int32_t          fnIndex;
    int32_t          localIdx;
    const SLAstNode* recv;
    if (recvNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_SYMBOL);
    }
    recv = &c->ast->nodes[recvNode];
    if (((recv->kind == SLAst_IDENT && SLTCLocalFind(c, recv->dataStart, recv->dataEnd) < 0
          && SLTCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) < 0)
         || recv->kind == SLAst_FIELD_EXPR)
        && SLTCResolveEnumMemberType(c, recvNode, n->dataStart, n->dataEnd, &fieldType))
    {
        *outType = fieldType;
        return 0;
    }
    localIdx = recv->kind == SLAst_IDENT ? SLTCLocalFind(c, recv->dataStart, recv->dataEnd) : -1;
    if (localIdx >= 0) {
        const SLTCVariantNarrow* narrow;
        if (SLTCVariantNarrowFind(c, localIdx, &narrow)
            && SLTCEnumVariantPayloadFieldType(
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
    if (recv->kind == SLAst_IDENT && localIdx < 0
        && SLTCFindFunctionIndex(c, recv->dataStart, recv->dataEnd) < 0)
    {
        fnIndex = SLTCFindPkgQualifiedFunctionValueIndex(
            c, recv->dataStart, recv->dataEnd, n->dataStart, n->dataEnd);
        if (fnIndex >= 0) {
            SLTCMarkFunctionUsed(c, fnIndex);
            *outType = c->funcs[(uint32_t)fnIndex].funcTypeId;
            return 0;
        }
    }
    if (SLTCTypeExpr(c, recvNode, &recvType) != 0) {
        return -1;
    }
    if (SLTCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, NULL) != 0) {
        return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
    }
    *outType = fieldType;
    return 0;
}

static int32_t SLTCFindParentNode(const SLAst* ast, int32_t childNodeId) {
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

static int SLTCAllowRuntimeAnyPackIndex(SLTypeCheckCtx* c, int32_t indexNodeId) {
    int32_t          parentNodeId;
    const SLAstNode* parent;
    if (c == NULL || c->ast == NULL) {
        return 0;
    }
    parentNodeId = SLTCFindParentNode(c->ast, indexNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    parent = &c->ast->nodes[parentNodeId];
    if (parent->kind == SLAst_CAST) {
        return 1;
    }
    if (parent->kind == SLAst_CALL_ARG) {
        int32_t          callNodeId = SLTCFindParentNode(c->ast, parentNodeId);
        const SLAstNode* callNode;
        int32_t          calleeNodeId;
        const SLAstNode* calleeNode;
        if (callNodeId < 0 || (uint32_t)callNodeId >= c->ast->len) {
            return 0;
        }
        callNode = &c->ast->nodes[callNodeId];
        if (callNode->kind != SLAst_CALL) {
            return 0;
        }
        calleeNodeId = SLAstFirstChild(c->ast, callNodeId);
        if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= c->ast->len) {
            return 0;
        }
        calleeNode = &c->ast->nodes[calleeNodeId];
        if (calleeNode->kind == SLAst_IDENT
            && SLNameEqLiteral(c->src, calleeNode->dataStart, calleeNode->dataEnd, "typeof"))
        {
            return 1;
        }
    }
    return 0;
}

int SLTCTypeExpr_INDEX(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t           baseNode = SLAstFirstChild(c->ast, nodeId);
    int32_t           baseType;
    int32_t           resolvedBaseType;
    SLTCIndexBaseInfo info;
    if (baseNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    if (SLTCTypeExpr(c, baseNode, &baseType) != 0) {
        return -1;
    }
    resolvedBaseType = SLTCResolveAliasBaseType(c, baseType);
    if (resolvedBaseType >= 0 && (uint32_t)resolvedBaseType < c->typeLen
        && c->types[resolvedBaseType].kind == SLTCType_PACK)
    {
        int32_t idxNode = SLAstNextSibling(c->ast, baseNode);
        int32_t idxType;
        int64_t idxValue = 0;
        int     idxIsConst = 0;
        if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }
        if (idxNode < 0 || SLAstNextSibling(c->ast, idxNode) >= 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        if (SLTCTypeExpr(c, idxNode, &idxType) != 0) {
            return -1;
        }
        if (!SLTCIsIntegerType(c, idxType)) {
            return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
        }
        if (SLTCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
            return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
        }
        if (!idxIsConst) {
            if (!SLTCAllowRuntimeAnyPackIndex(c, nodeId)) {
                return SLTCFailNode(c, idxNode, SLDiag_ANYTYPE_PACK_INDEX_NOT_CONST);
            }
            *outType = c->typeAnytype;
            return 0;
        }
        if (idxValue < 0 || (uint64_t)idxValue >= c->types[resolvedBaseType].fieldCount) {
            return SLTCFailNode(c, idxNode, SLDiag_ANYTYPE_PACK_INDEX_OOB);
        }
        *outType = c->funcParamTypes[c->types[resolvedBaseType].fieldStart + (uint32_t)idxValue];
        return 0;
    }
    if (SLTCResolveIndexBaseInfo(c, baseType, &info) != 0 || !info.indexable || info.elemType < 0) {
        return SLTCFailNode(c, baseNode, SLDiag_TYPE_MISMATCH);
    }

    if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
        int     hasStart = (n->flags & SLAstFlag_INDEX_HAS_START) != 0;
        int     hasEnd = (n->flags & SLAstFlag_INDEX_HAS_END) != 0;
        int32_t child = SLAstNextSibling(c->ast, baseNode);
        int32_t startNode = -1;
        int32_t endNode = -1;
        int32_t sliceType;
        int64_t startValue = 0;
        int64_t endValue = 0;
        int     startIsConst = 0;
        int     endIsConst = 0;

        if (!info.sliceable) {
            return SLTCFailUnsliceableExpr(c, nodeId, baseType);
        }

        if (hasStart) {
            int32_t startType;
            startNode = child;
            if (startNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, startNode, &startType) != 0) {
                return -1;
            }
            if (!SLTCIsIntegerType(c, startType)) {
                return SLTCFailNode(c, startNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCConstIntExpr(c, startNode, &startValue, &startIsConst) != 0) {
                return SLTCFailNode(c, startNode, SLDiag_TYPE_MISMATCH);
            }
            child = SLAstNextSibling(c->ast, child);
        }
        if (hasEnd) {
            int32_t endType;
            endNode = child;
            if (endNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, endNode, &endType) != 0) {
                return -1;
            }
            if (!SLTCIsIntegerType(c, endType)) {
                return SLTCFailNode(c, endNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCConstIntExpr(c, endNode, &endValue, &endIsConst) != 0) {
                return SLTCFailNode(c, endNode, SLDiag_TYPE_MISMATCH);
            }
            child = SLAstNextSibling(c->ast, child);
        }
        if (child >= 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }

        if ((startIsConst && startValue < 0) || (endIsConst && endValue < 0)) {
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }

        if (info.hasKnownLen) {
            int     startKnown = !hasStart || startIsConst;
            int     endKnown = !hasEnd || endIsConst;
            int64_t startBound = hasStart ? startValue : 0;
            int64_t endBound = hasEnd ? endValue : (int64_t)info.knownLen;
            if (startKnown && endKnown) {
                if (startBound > endBound || endBound > (int64_t)info.knownLen) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }
            } else {
                SLTCMarkRuntimeBoundsCheck(c, nodeId);
            }
        } else {
            if (startIsConst && endIsConst && startValue > endValue) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            SLTCMarkRuntimeBoundsCheck(c, nodeId);
        }

        if (info.isStringLike) {
            *outType = SLTCGetStringSliceExprType(c, baseType, n->start, n->end);
            if (*outType < 0) {
                return -1;
            }
        } else {
            sliceType = SLTCInternSliceType(c, info.elemType, info.sliceMutable, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            if (info.sliceMutable) {
                *outType = SLTCInternPtrType(c, sliceType, n->start, n->end);
            } else {
                *outType = SLTCInternRefType(c, sliceType, 0, n->start, n->end);
            }
            if (*outType < 0) {
                return -1;
            }
        }
        return 0;
    }

    {
        int32_t idxNode = SLAstNextSibling(c->ast, baseNode);
        int32_t idxType;
        int64_t idxValue = 0;
        int     idxIsConst = 0;

        if (idxNode < 0 || SLAstNextSibling(c->ast, idxNode) >= 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        if (SLTCTypeExpr(c, idxNode, &idxType) != 0) {
            return -1;
        }
        if (!SLTCIsIntegerType(c, idxType)) {
            return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
        }
        if (SLTCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
            return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
        }
        if (idxIsConst && idxValue < 0) {
            return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
        }

        if (info.hasKnownLen) {
            if (idxIsConst) {
                if (idxValue >= (int64_t)info.knownLen) {
                    return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
                }
            } else {
                SLTCMarkRuntimeBoundsCheck(c, nodeId);
            }
        } else if (info.sliceable) {
            SLTCMarkRuntimeBoundsCheck(c, nodeId);
        }
    }

    *outType = info.elemType;
    return 0;
}

int SLTCTypeExpr_UNARY(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t rhsNode = SLAstFirstChild(c->ast, nodeId);
    int32_t rhsType;
    if (rhsNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    if (SLTCTypeExpr(c, rhsNode, &rhsType) != 0) {
        return -1;
    }
    switch ((SLTokenKind)n->op) {
        case SLTok_ADD:
        case SLTok_SUB:
            if (!SLTCIsNumericType(c, rhsType)) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            *outType = rhsType;
            return 0;
        case SLTok_NOT:
            if (!SLTCIsBoolType(c, rhsType)) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
            }
            *outType = c->typeBool;
            return 0;
        case SLTok_MUL:
            if (c->types[rhsType].kind != SLTCType_PTR && c->types[rhsType].kind != SLTCType_REF) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            *outType = c->types[rhsType].baseType;
            return 0;
        case SLTok_AND: {
            int32_t ptrType = SLTCInternPtrType(c, rhsType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        default: return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    }
}

int SLTCTypeExpr_BINARY(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t     lhsNode = SLAstFirstChild(c->ast, nodeId);
    int32_t     rhsNode;
    int32_t     lhsType;
    int32_t     rhsType;
    int32_t     commonType;
    int32_t     hookFn = -1;
    SLTokenKind op = (SLTokenKind)n->op;
    if (lhsNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    rhsNode = SLAstNextSibling(c->ast, lhsNode);
    if (rhsNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    if (op == SLTok_ASSIGN && c->ast->nodes[lhsNode].kind == SLAst_IDENT
        && SLNameEqLiteral(
            c->src, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, "_"))
    {
        if (SLTCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
            if (SLTCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                return -1;
            }
        }
        if (rhsType == c->typeVoid) {
            return SLTCFailNode(c, rhsNode, SLDiag_TYPE_MISMATCH);
        }
        *outType = rhsType;
        return 0;
    }
    if (op == SLTok_ADD && SLIsStringLiteralConcatChain(c->ast, nodeId)) {
        int32_t strRefType = SLTCGetStrRefType(c, n->start, n->end);
        if (strRefType < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
        }
        *outType = strRefType;
        return 0;
    }
    if (op == SLTok_ASSIGN || op == SLTok_ADD_ASSIGN || op == SLTok_SUB_ASSIGN
        || op == SLTok_MUL_ASSIGN || op == SLTok_DIV_ASSIGN || op == SLTok_MOD_ASSIGN
        || op == SLTok_AND_ASSIGN || op == SLTok_OR_ASSIGN || op == SLTok_XOR_ASSIGN
        || op == SLTok_LSHIFT_ASSIGN || op == SLTok_RSHIFT_ASSIGN)
    {
        int skipDirectIdentRead = op == SLTok_ASSIGN;
        if (SLTCTypeAssignTargetExpr(c, lhsNode, skipDirectIdentRead, &lhsType) != 0
            || SLTCTypeExprExpected(c, rhsNode, lhsType, &rhsType) != 0)
        {
            return -1;
        }
        if (!SLTCExprIsAssignable(c, lhsNode)) {
            return SLTCFailNode(c, lhsNode, SLDiag_TYPE_MISMATCH);
        }
        if (SLTCExprIsConstAssignTarget(c, lhsNode)) {
            return SLTCFailAssignToConst(c, lhsNode);
        }
        if (!SLTCCanAssign(c, lhsType, rhsType)) {
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }
        if (op != SLTok_ASSIGN && !SLTCIsNumericType(c, lhsType)) {
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }
        SLTCMarkDirectIdentLocalWrite(c, lhsNode, op == SLTok_ASSIGN);
        *outType = lhsType;
        return 0;
    }
    if (SLTCTypeExpr(c, lhsNode, &lhsType) != 0
        || SLTCTypeExprExpected(c, rhsNode, lhsType, &rhsType) != 0)
    {
        return -1;
    }

    if (op == SLTok_LOGICAL_AND || op == SLTok_LOGICAL_OR) {
        if (!SLTCIsBoolType(c, lhsType) || !SLTCIsBoolType(c, rhsType)) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
        }
        *outType = c->typeBool;
        return 0;
    }

    /* Allow ?T == null, null == ?T, rawptr == null, null == rawptr, and != variants. */
    if (op == SLTok_EQ || op == SLTok_NEQ) {
        int lhsIsOpt = c->types[lhsType].kind == SLTCType_OPTIONAL;
        int rhsIsOpt = c->types[rhsType].kind == SLTCType_OPTIONAL;
        int lhsIsNull = c->types[lhsType].kind == SLTCType_NULL;
        int rhsIsNull = c->types[rhsType].kind == SLTCType_NULL;
        int lhsIsRawptr = SLTCIsRawptrType(c, lhsType);
        int rhsIsRawptr = SLTCIsRawptrType(c, rhsType);
        if ((lhsIsOpt && rhsIsNull) || (lhsIsNull && rhsIsOpt) || (lhsIsRawptr && rhsIsNull)
            || (lhsIsNull && rhsIsRawptr))
        {
            *outType = c->typeBool;
            return 0;
        }
    }

    if (op == SLTok_EQ || op == SLTok_NEQ || op == SLTok_LT || op == SLTok_GT || op == SLTok_LTE
        || op == SLTok_GTE)
    {
        int hookStatus = SLTCResolveComparisonHook(
            c,
            (op == SLTok_EQ || op == SLTok_NEQ) ? "__equal" : "__order",
            lhsType,
            rhsType,
            &hookFn);
        if (hookStatus == 0) {
            SLTCMarkFunctionUsed(c, hookFn);
            *outType = c->typeBool;
            return 0;
        }
        if (hookStatus == 3) {
            return SLTCFailNode(c, nodeId, SLDiag_AMBIGUOUS_CALL);
        }
        if (SLTCCoerceForBinary(c, lhsType, rhsType, &commonType) != 0) {
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }
        if (op == SLTok_EQ || op == SLTok_NEQ) {
            if (!SLTCIsComparableType(c, commonType)) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_NOT_COMPARABLE);
            }
        } else {
            if (!SLTCIsOrderedType(c, commonType)) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_NOT_ORDERED);
            }
        }
        *outType = c->typeBool;
        return 0;
    }

    if (op == SLTok_ADD && (SLTCIsStringLikeType(c, lhsType) || SLTCIsStringLikeType(c, rhsType))) {
        return SLTCFailNode(c, nodeId, SLDiag_STRING_CONCAT_LITERAL_ONLY);
    }

    if (SLTCCoerceForBinary(c, lhsType, rhsType, &commonType) != 0) {
        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    }

    if (!SLTCIsNumericType(c, commonType)) {
        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    }
    *outType = commonType;
    return 0;
}

int SLTCTypeExpr_NULL(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    (void)nodeId;
    (void)n;
    *outType = c->typeNull;
    return 0;
}

int SLTCTypeExpr_UNWRAP(SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t inner = SLAstFirstChild(c->ast, nodeId);
    int32_t innerType;
    (void)n;
    if (inner < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    if (SLTCTypeExpr(c, inner, &innerType) != 0) {
        return -1;
    }
    if (c->types[innerType].kind != SLTCType_OPTIONAL) {
        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
    }
    *outType = c->types[innerType].baseType;
    return 0;
}

int SLTCTypeExpr_TUPLE_EXPR(
    SLTypeCheckCtx* c, int32_t nodeId, const SLAstNode* n, int32_t* outType) {
    int32_t  child = SLAstFirstChild(c->ast, nodeId);
    uint32_t elemCount = 0;
    (void)n;
    while (child >= 0) {
        int32_t elemType;
        if (elemCount >= c->scratchParamCap) {
            return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
        }
        if (SLTCTypeExpr(c, child, &elemType) != 0) {
            return -1;
        }
        if (elemType == c->typeNull) {
            return SLTCFailNode(c, child, SLDiag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (elemType == c->typeVoid) {
            return SLTCFailNode(c, child, SLDiag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (SLTCConcretizeInferredType(c, elemType, &elemType) != 0) {
            return -1;
        }
        c->scratchParamTypes[elemCount++] = elemType;
        child = SLAstNextSibling(c->ast, child);
    }
    if (elemCount < 2u) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    *outType = SLTCInternTupleType(c, c->scratchParamTypes, elemCount, n->start, n->end);
    return *outType < 0 ? -1 : 0;
}

int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case SLAst_IDENT:             return SLTCTypeExpr_IDENT(c, nodeId, n, outType);
        case SLAst_TYPE_VALUE:        return SLTCTypeExpr_TYPE_VALUE(c, nodeId, n, outType);
        case SLAst_INT:               return SLTCTypeExpr_INT(c, nodeId, n, outType);
        case SLAst_FLOAT:             return SLTCTypeExpr_FLOAT(c, nodeId, n, outType);
        case SLAst_STRING:            return SLTCTypeExpr_STRING(c, nodeId, n, outType);
        case SLAst_RUNE:              return SLTCTypeExpr_RUNE(c, nodeId, n, outType);
        case SLAst_BOOL:              return SLTCTypeExpr_BOOL(c, nodeId, n, outType);
        case SLAst_COMPOUND_LIT:      return SLTCTypeExpr_COMPOUND_LIT(c, nodeId, n, outType);
        case SLAst_CALL_WITH_CONTEXT: return SLTCTypeExpr_CALL_WITH_CONTEXT(c, nodeId, n, outType);
        case SLAst_NEW:               return SLTCTypeExpr_NEW(c, nodeId, n, outType);
        case SLAst_CALL:              return SLTCTypeExpr_CALL(c, nodeId, n, outType);
        case SLAst_CALL_ARG:          {
            int32_t inner = SLAstFirstChild(c->ast, nodeId);
            if (inner < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            return SLTCTypeExpr(c, inner, outType);
        }
        case SLAst_CAST:       return SLTCTypeExpr_CAST(c, nodeId, n, outType);
        case SLAst_SIZEOF:     return SLTCTypeExpr_SIZEOF(c, nodeId, n, outType);
        case SLAst_FIELD_EXPR: return SLTCTypeExpr_FIELD_EXPR(c, nodeId, n, outType);
        case SLAst_INDEX:      return SLTCTypeExpr_INDEX(c, nodeId, n, outType);
        case SLAst_UNARY:      return SLTCTypeExpr_UNARY(c, nodeId, n, outType);
        case SLAst_BINARY:     return SLTCTypeExpr_BINARY(c, nodeId, n, outType);
        case SLAst_NULL:       return SLTCTypeExpr_NULL(c, nodeId, n, outType);
        case SLAst_UNWRAP:     return SLTCTypeExpr_UNWRAP(c, nodeId, n, outType);
        case SLAst_TUPLE_EXPR: return SLTCTypeExpr_TUPLE_EXPR(c, nodeId, n, outType);
        default:               return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
}

int SLTCValidateConstInitializerExprNode(SLTypeCheckCtx* c, int32_t initNode) {
    SLTCConstEvalCtx evalCtx;
    SLCTFEValue      value;
    int              isConst = 0;
    int              rc;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    c->lastConstEvalReason = NULL;
    c->lastConstEvalReasonStart = 0;
    c->lastConstEvalReasonEnd = 0;
    if (SLTCEvalConstExprNode(&evalCtx, initNode, &value, &isConst) != 0) {
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
        rc = SLTCFailSpan(
            c,
            SLDiag_CONST_INIT_CONST_REQUIRED,
            c->lastConstEvalReasonStart,
            c->lastConstEvalReasonEnd);
    } else {
        rc = SLTCFailNode(c, initNode, SLDiag_CONST_INIT_CONST_REQUIRED);
    }
    SLTCAttachConstEvalReason(c);
    return rc;
}

static int SLTCValidateLocalConstFunctionInitializerExprNode(SLTypeCheckCtx* c, int32_t initNode) {
    const SLAstNode* init;
    int32_t          initType = -1;
    if (c == NULL || initNode < 0 || (uint32_t)initNode >= c->ast->len) {
        return 0;
    }
    init = &c->ast->nodes[initNode];
    if (init->kind == SLAst_CALL_ARG) {
        int32_t inner = SLAstFirstChild(c->ast, initNode);
        return SLTCValidateLocalConstFunctionInitializerExprNode(c, inner);
    }
    if (init->kind != SLAst_IDENT && init->kind != SLAst_FIELD_EXPR) {
        return 0;
    }
    if (SLTCTypeExpr(c, initNode, &initType) != 0) {
        return 0;
    }
    return initType >= 0 && (uint32_t)initType < c->typeLen
        && c->types[initType].kind == SLTCType_FUNCTION;
}

int SLTCValidateLocalConstVarLikeInitializers(
    SLTypeCheckCtx* c, int32_t nodeId, const SLTCVarLikeParts* parts) {
    uint32_t i;
    if (parts == NULL || parts->initNode < 0) {
        return 0;
    }
    if (!parts->grouped) {
        int32_t initNode = SLTCVarLikeInitExprNode(c, nodeId);
        if (initNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        if (SLTCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
            return 0;
        }
        return SLTCValidateConstInitializerExprNode(c, initNode);
    }
    if ((uint32_t)parts->initNode >= c->ast->len
        || c->ast->nodes[parts->initNode].kind != SLAst_EXPR_LIST)
    {
        return SLTCFailNode(c, parts->initNode, SLDiag_EXPECTED_EXPR);
    }
    {
        uint32_t initCount = SLTCListCount(c->ast, parts->initNode);
        if (initCount == parts->nameCount) {
            for (i = 0; i < initCount; i++) {
                int32_t initNode = SLTCListItemAt(c->ast, parts->initNode, i);
                if (initNode < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                }
                if (SLTCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
                    continue;
                }
                if (SLTCValidateConstInitializerExprNode(c, initNode) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        if (initCount == 1u) {
            int32_t initNode = SLTCListItemAt(c->ast, parts->initNode, 0);
            if (initNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCValidateLocalConstFunctionInitializerExprNode(c, initNode)) {
                return 0;
            }
            return SLTCValidateConstInitializerExprNode(c, initNode);
        }
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
}

int SLTCTypeVarLike(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    SLTCVarLikeParts parts;
    int32_t          declType;
    uint32_t         i;
    int              isConstBinding;

    if (SLTCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    isConstBinding = n->kind == SLAst_CONST;
    if (isConstBinding && parts.initNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_CONST_MISSING_INITIALIZER);
    }
    if (isConstBinding && SLTCValidateLocalConstVarLikeInitializers(c, nodeId, &parts) != 0) {
        return -1;
    }

    if (!parts.grouped) {
        if (parts.typeNode < 0) {
            int32_t initType;
            if (SLTCTypeExpr(c, parts.initNode, &initType) != 0) {
                return -1;
            }
            if (initType == c->typeNull) {
                return SLTCFailNode(c, parts.initNode, SLDiag_INFER_NULL_TYPE_UNKNOWN);
            }
            if (initType == c->typeVoid) {
                return SLTCFailNode(c, parts.initNode, SLDiag_INFER_VOID_TYPE_UNKNOWN);
            }
            if (isConstBinding) {
                declType = initType;
            } else {
                if (SLTCConcretizeInferredType(c, initType, &declType) != 0) {
                    return -1;
                }
            }
            if (SLTCTypeContainsVarSizeByValue(c, declType)) {
                return SLTCFailNode(c, parts.initNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCLocalAdd(
                    c,
                    n->dataStart,
                    n->dataEnd,
                    declType,
                    n->kind == SLAst_CONST,
                    n->kind == SLAst_CONST ? parts.initNode : -1)
                != 0)
            {
                return -1;
            }
            SLTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
            return 0;
        }

        c->allowConstNumericTypeName = n->kind == SLAst_CONST ? 1u : 0u;
        if (SLTCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
            c->allowConstNumericTypeName = 0;
            return -1;
        }
        c->allowConstNumericTypeName = 0;
        if (SLTCTypeContainsVarSizeByValue(c, declType)) {
            if (parts.initNode >= 0) {
                int32_t initType;
                if (SLTCTypeExprExpected(c, parts.initNode, declType, &initType) != 0) {
                    return -1;
                }
                return SLTCFailTypeMismatchDetail(
                    c, parts.initNode, parts.initNode, initType, declType);
            }
            return SLTCFailNode(c, parts.typeNode, SLDiag_TYPE_MISMATCH);
        }

        if (n->kind == SLAst_CONST && parts.initNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_CONST_MISSING_INITIALIZER);
        }
        if (n->kind == SLAst_VAR && parts.initNode < 0 && !SLTCEnumTypeHasTagZero(c, declType)) {
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }
        if (parts.initNode >= 0) {
            int32_t initType;
            if (SLTCTypeExprExpected(c, parts.initNode, declType, &initType) != 0) {
                return -1;
            }
            if (!SLTCCanAssign(c, declType, initType)) {
                return SLTCFailTypeMismatchDetail(
                    c, parts.initNode, parts.initNode, initType, declType);
            }
        }

        if (SLTCLocalAdd(
                c,
                n->dataStart,
                n->dataEnd,
                declType,
                n->kind == SLAst_CONST,
                n->kind == SLAst_CONST ? parts.initNode : -1)
            != 0)
        {
            return -1;
        }
        if (parts.initNode >= 0) {
            SLTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
        }
        return 0;
    }

    if (parts.typeNode >= 0) {
        c->allowConstNumericTypeName = n->kind == SLAst_CONST ? 1u : 0u;
        if (SLTCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
            c->allowConstNumericTypeName = 0;
            return -1;
        }
        c->allowConstNumericTypeName = 0;
        if (SLTCTypeContainsVarSizeByValue(c, declType)) {
            return SLTCFailNode(c, parts.typeNode, SLDiag_TYPE_MISMATCH);
        }
        if (n->kind == SLAst_CONST && parts.initNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_CONST_MISSING_INITIALIZER);
        }
        if (n->kind == SLAst_VAR && parts.initNode < 0 && !SLTCEnumTypeHasTagZero(c, declType)) {
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }
        if (parts.initNode >= 0) {
            uint32_t initCount;
            if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != SLAst_EXPR_LIST) {
                return SLTCFailNode(c, parts.initNode, SLDiag_EXPECTED_EXPR);
            }
            initCount = SLTCListCount(c->ast, parts.initNode);
            if (initCount == parts.nameCount) {
                for (i = 0; i < initCount; i++) {
                    int32_t initNode = SLTCListItemAt(c->ast, parts.initNode, i);
                    int32_t initType;
                    if (initNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                    }
                    if (SLTCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                        return -1;
                    }
                    if (!SLTCCanAssign(c, declType, initType)) {
                        return SLTCFailTypeMismatchDetail(
                            c, initNode, initNode, initType, declType);
                    }
                }
            } else if (initCount == 1u) {
                int32_t         initNode = SLTCListItemAt(c->ast, parts.initNode, 0);
                int32_t         initType;
                const SLTCType* t;
                if (initNode < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                }
                if (SLTCTypeExpr(c, initNode, &initType) != 0) {
                    return -1;
                }
                if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                    return SLTCFailNode(c, initNode, SLDiag_TYPE_MISMATCH);
                }
                t = &c->types[initType];
                if (t->kind != SLTCType_TUPLE || t->fieldCount != parts.nameCount) {
                    return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t elemType = c->funcParamTypes[t->fieldStart + i];
                    if (!SLTCCanAssign(c, declType, elemType)) {
                        return SLTCFailTypeMismatchDetail(
                            c, initNode, initNode, elemType, declType);
                    }
                }
            } else {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
        }
        for (i = 0; i < parts.nameCount; i++) {
            int32_t          nameNode = SLTCListItemAt(c->ast, parts.nameListNode, i);
            const SLAstNode* name;
            if (nameNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            name = &c->ast->nodes[nameNode];
            if (!SLNameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                if (SLTCLocalAdd(
                        c, name->dataStart, name->dataEnd, declType, n->kind == SLAst_CONST, -1)
                    != 0)
                {
                    return -1;
                }
                if (parts.initNode >= 0) {
                    SLTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        }
        return 0;
    }

    if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != SLAst_EXPR_LIST) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    {
        uint32_t initCount = SLTCListCount(c->ast, parts.initNode);
        if (initCount == parts.nameCount) {
            for (i = 0; i < parts.nameCount; i++) {
                int32_t          nameNode = SLTCListItemAt(c->ast, parts.nameListNode, i);
                const SLAstNode* name;
                int32_t          initNode = SLTCListItemAt(c->ast, parts.initNode, i);
                int32_t          initType;
                int32_t          inferredType;
                if (nameNode < 0 || initNode < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                }
                if (SLTCTypeExpr(c, initNode, &initType) != 0) {
                    return -1;
                }
                if (initType == c->typeNull) {
                    return SLTCFailNode(c, initNode, SLDiag_INFER_NULL_TYPE_UNKNOWN);
                }
                if (initType == c->typeVoid) {
                    return SLTCFailNode(c, initNode, SLDiag_INFER_VOID_TYPE_UNKNOWN);
                }
                if (isConstBinding) {
                    inferredType = initType;
                } else {
                    if (SLTCConcretizeInferredType(c, initType, &inferredType) != 0) {
                        return -1;
                    }
                }
                if (SLTCTypeContainsVarSizeByValue(c, inferredType)) {
                    return SLTCFailNode(c, initNode, SLDiag_TYPE_MISMATCH);
                }
                name = &c->ast->nodes[nameNode];
                if (!SLNameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                    if (SLTCLocalAdd(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            inferredType,
                            n->kind == SLAst_CONST,
                            n->kind == SLAst_CONST ? initNode : -1)
                        != 0)
                    {
                        return -1;
                    }
                    SLTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        } else if (initCount == 1u) {
            int32_t         initNode = SLTCListItemAt(c->ast, parts.initNode, 0);
            int32_t         initType;
            const SLTCType* t;
            if (initNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, initNode, &initType) != 0) {
                return -1;
            }
            if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                return SLTCFailNode(c, initNode, SLDiag_TYPE_MISMATCH);
            }
            t = &c->types[initType];
            if (t->kind != SLTCType_TUPLE || t->fieldCount != parts.nameCount) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            for (i = 0; i < parts.nameCount; i++) {
                int32_t          nameNode = SLTCListItemAt(c->ast, parts.nameListNode, i);
                const SLAstNode* name;
                int32_t          inferredType = c->funcParamTypes[t->fieldStart + i];
                if (nameNode < 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                }
                if (!isConstBinding) {
                    if (SLTCConcretizeInferredType(c, inferredType, &inferredType) != 0) {
                        return -1;
                    }
                }
                if (SLTCTypeContainsVarSizeByValue(c, inferredType)) {
                    return SLTCFailNode(c, initNode, SLDiag_TYPE_MISMATCH);
                }
                name = &c->ast->nodes[nameNode];
                if (!SLNameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
                    if (SLTCLocalAdd(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            inferredType,
                            n->kind == SLAst_CONST,
                            -1)
                        != 0)
                    {
                        return -1;
                    }
                    SLTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                }
            }
        } else {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
    }
    return 0;
}

int SLTCTypeTopLevelVarLikes(SLTypeCheckCtx* c, SLAstKind wantKind) {
    int32_t child = SLAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == wantKind) {
            SLTCVarLikeParts parts;
            if (SLTCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
            }
            if (!parts.grouped) {
                int32_t firstChild = SLAstFirstChild(c->ast, child);
                if (firstChild >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[firstChild].kind)) {
                    int32_t initNode = SLAstNextSibling(c->ast, firstChild);
                    int32_t declType;
                    int32_t initType;
                    if (wantKind == SLAst_CONST && initNode < 0
                        && !SLTCHasForeignImportDirective(c->ast, c->src, child))
                    {
                        return SLTCFailNode(c, child, SLDiag_CONST_MISSING_INITIALIZER);
                    }
                    c->allowConstNumericTypeName = wantKind == SLAst_CONST ? 1u : 0u;
                    if (SLTCResolveTypeNode(c, firstChild, &declType) != 0) {
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowConstNumericTypeName = 0;
                    if (SLTCTypeContainsVarSizeByValue(c, declType)) {
                        return SLTCFailNode(c, firstChild, SLDiag_TYPE_MISMATCH);
                    }
                    if (wantKind == SLAst_VAR && initNode < 0
                        && SLTCTypeIsTrackedPtrRef(c, declType))
                    {
                        return SLTCFailTopLevelPtrRefMissingInitializer(
                            c, n->start, n->end, n->dataStart, n->dataEnd);
                    }
                    if (initNode >= 0) {
                        if (SLTCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                            return -1;
                        }
                        if (!SLTCCanAssign(c, declType, initType)) {
                            return SLTCFailTypeMismatchDetail(
                                c, initNode, initNode, initType, declType);
                        }
                    }
                } else if (firstChild >= 0) {
                    int32_t initType;
                    int32_t declType;
                    if (SLTCTypeExpr(c, firstChild, &initType) != 0) {
                        return -1;
                    }
                    if (initType == c->typeNull) {
                        return SLTCFailNode(c, firstChild, SLDiag_INFER_NULL_TYPE_UNKNOWN);
                    }
                    if (initType == c->typeVoid) {
                        return SLTCFailNode(c, firstChild, SLDiag_INFER_VOID_TYPE_UNKNOWN);
                    }
                    if (wantKind == SLAst_CONST) {
                        declType = initType;
                    } else {
                        if (SLTCConcretizeInferredType(c, initType, &declType) != 0) {
                            return -1;
                        }
                    }
                    if (SLTCTypeContainsVarSizeByValue(c, declType)) {
                        return SLTCFailNode(c, firstChild, SLDiag_TYPE_MISMATCH);
                    }
                }
                child = SLAstNextSibling(c->ast, child);
                continue;
            }

            if (parts.typeNode >= 0) {
                int32_t declType;
                if (wantKind == SLAst_CONST && parts.initNode < 0
                    && !SLTCHasForeignImportDirective(c->ast, c->src, child))
                {
                    return SLTCFailNode(c, child, SLDiag_CONST_MISSING_INITIALIZER);
                }
                c->allowConstNumericTypeName = wantKind == SLAst_CONST ? 1u : 0u;
                if (SLTCResolveTypeNode(c, parts.typeNode, &declType) != 0) {
                    c->allowConstNumericTypeName = 0;
                    return -1;
                }
                c->allowConstNumericTypeName = 0;
                if (SLTCTypeContainsVarSizeByValue(c, declType)) {
                    return SLTCFailNode(c, parts.typeNode, SLDiag_TYPE_MISMATCH);
                }
                if (wantKind == SLAst_VAR && parts.initNode < 0
                    && SLTCTypeIsTrackedPtrRef(c, declType))
                {
                    int32_t  nameNode = SLTCListItemAt(c->ast, parts.nameListNode, 0);
                    uint32_t nameStart = c->ast->nodes[child].dataStart;
                    uint32_t nameEnd = c->ast->nodes[child].dataEnd;
                    if (nameNode >= 0) {
                        nameStart = c->ast->nodes[nameNode].dataStart;
                        nameEnd = c->ast->nodes[nameNode].dataEnd;
                    }
                    return SLTCFailTopLevelPtrRefMissingInitializer(
                        c,
                        c->ast->nodes[child].start,
                        c->ast->nodes[child].end,
                        nameStart,
                        nameEnd);
                }
                if (parts.initNode >= 0) {
                    uint32_t i;
                    uint32_t initCount;
                    if (c->ast->nodes[parts.initNode].kind != SLAst_EXPR_LIST) {
                        return SLTCFailNode(c, parts.initNode, SLDiag_EXPECTED_EXPR);
                    }
                    initCount = SLTCListCount(c->ast, parts.initNode);
                    if (initCount == parts.nameCount) {
                        for (i = 0; i < initCount; i++) {
                            int32_t initNode = SLTCListItemAt(c->ast, parts.initNode, i);
                            int32_t initType;
                            if (initNode < 0) {
                                return SLTCFailNode(c, child, SLDiag_EXPECTED_EXPR);
                            }
                            if (SLTCTypeExprExpected(c, initNode, declType, &initType) != 0) {
                                return -1;
                            }
                            if (!SLTCCanAssign(c, declType, initType)) {
                                return SLTCFailTypeMismatchDetail(
                                    c, initNode, initNode, initType, declType);
                            }
                        }
                    } else if (initCount == 1u) {
                        int32_t         initNode = SLTCListItemAt(c->ast, parts.initNode, 0);
                        int32_t         initType;
                        const SLTCType* t;
                        if (initNode < 0) {
                            return SLTCFailNode(c, child, SLDiag_EXPECTED_EXPR);
                        }
                        if (SLTCTypeExpr(c, initNode, &initType) != 0) {
                            return -1;
                        }
                        if (initType < 0 || (uint32_t)initType >= c->typeLen) {
                            return SLTCFailNode(c, initNode, SLDiag_TYPE_MISMATCH);
                        }
                        t = &c->types[initType];
                        if (t->kind != SLTCType_TUPLE || t->fieldCount != parts.nameCount) {
                            return SLTCFailNode(c, child, SLDiag_ARITY_MISMATCH);
                        }
                        for (i = 0; i < parts.nameCount; i++) {
                            int32_t elemType = c->funcParamTypes[t->fieldStart + i];
                            if (!SLTCCanAssign(c, declType, elemType)) {
                                return SLTCFailTypeMismatchDetail(
                                    c, initNode, initNode, elemType, declType);
                            }
                        }
                    } else {
                        return SLTCFailNode(c, child, SLDiag_ARITY_MISMATCH);
                    }
                }
            } else {
                uint32_t i;
                if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != SLAst_EXPR_LIST) {
                    return SLTCFailNode(c, child, SLDiag_EXPECTED_EXPR);
                }
                {
                    uint32_t initCount = SLTCListCount(c->ast, parts.initNode);
                    int      tupleDecompose = 0;
                    if (initCount == parts.nameCount) {
                        tupleDecompose = 1;
                    } else if (initCount == 1u) {
                        int32_t         initNode = SLTCListItemAt(c->ast, parts.initNode, 0);
                        int32_t         initType;
                        const SLTCType* t;
                        if (initNode < 0) {
                            return SLTCFailNode(c, child, SLDiag_EXPECTED_EXPR);
                        }
                        if (SLTCTypeExpr(c, initNode, &initType) != 0) {
                            return -1;
                        }
                        if (initType >= 0 && (uint32_t)initType < c->typeLen) {
                            t = &c->types[initType];
                            tupleDecompose =
                                t->kind == SLTCType_TUPLE && t->fieldCount == parts.nameCount;
                        }
                    }
                    if (!tupleDecompose) {
                        return SLTCFailNode(c, child, SLDiag_ARITY_MISMATCH);
                    }
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t inferredType;
                    if (SLTCTypeTopLevelVarLikeNode(c, child, (int32_t)i, &inferredType) != 0) {
                        return -1;
                    }
                    (void)inferredType;
                }
            }
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

int SLTCTypeTopLevelConsts(SLTypeCheckCtx* c) {
    return SLTCTypeTopLevelVarLikes(c, SLAst_CONST);
}

int SLTCTypeTopLevelVars(SLTypeCheckCtx* c) {
    return SLTCTypeTopLevelVarLikes(c, SLAst_VAR);
}

int SLTCCheckTopLevelConstInitializers(SLTypeCheckCtx* c) {
    int32_t child = SLAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_CONST) {
            SLTCVarLikeParts parts;
            if (SLTCHasForeignImportDirective(c->ast, c->src, child)) {
                child = SLAstNextSibling(c->ast, child);
                continue;
            }
            if (SLTCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
            }
            if (parts.typeNode >= 0 && parts.initNode < 0
                && !SLTCHasForeignImportDirective(c->ast, c->src, child))
            {
                return SLTCFailNode(c, child, SLDiag_CONST_MISSING_INITIALIZER);
            }
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

int SLTCValidateTopLevelConstEvaluable(SLTypeCheckCtx* c) {
    SLTCConstEvalCtx evalCtx;
    int32_t          child;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    child = SLAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_CONST) {
            SLTCVarLikeParts parts;
            if (SLTCHasForeignImportDirective(c->ast, c->src, child)) {
                child = SLAstNextSibling(c->ast, child);
                continue;
            }
            if (SLTCVarLikeGetParts(c, child, &parts) != 0 || parts.nameCount == 0) {
                return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
            }
            if (!parts.grouped) {
                int32_t     initNode = SLTCVarLikeInitExprNode(c, child);
                SLCTFEValue value;
                int         isConst = 0;
                if (initNode >= 0 && SLTCValidateLocalConstFunctionInitializerExprNode(c, initNode))
                {
                    child = SLAstNextSibling(c->ast, child);
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
                if (SLTCEvalTopLevelConstNode(c, &evalCtx, child, &value, &isConst) != 0) {
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
                        rc = SLTCFailSpan(
                            c,
                            SLDiag_CONST_INIT_CONST_REQUIRED,
                            c->lastConstEvalReasonStart,
                            c->lastConstEvalReasonEnd);
                    } else if (initNode >= 0) {
                        rc = SLTCFailNode(c, initNode, SLDiag_CONST_INIT_CONST_REQUIRED);
                    } else {
                        rc = SLTCFailNode(c, child, SLDiag_CONST_INIT_CONST_REQUIRED);
                    }
                    SLTCAttachConstEvalReason(c);
                    return rc;
                }
            } else {
                uint32_t i;
                if (parts.initNode < 0 || c->ast->nodes[parts.initNode].kind != SLAst_EXPR_LIST) {
                    return SLTCFailNode(c, child, SLDiag_EXPECTED_EXPR);
                }
                for (i = 0; i < parts.nameCount; i++) {
                    int32_t     initNode = SLTCVarLikeInitExprNodeAt(c, child, (int32_t)i);
                    SLCTFEValue value;
                    int         isConst = 0;
                    if (initNode < 0) {
                        return SLTCFailNode(c, child, SLDiag_ARITY_MISMATCH);
                    }
                    c->lastConstEvalReason = NULL;
                    c->lastConstEvalReasonStart = 0;
                    c->lastConstEvalReasonEnd = 0;
                    evalCtx.nonConstReason = NULL;
                    evalCtx.nonConstStart = 0;
                    evalCtx.nonConstEnd = 0;
                    evalCtx.fnDepth = 0;
                    evalCtx.execCtx = NULL;
                    if (SLTCEvalConstExprNode(&evalCtx, initNode, &value, &isConst) != 0) {
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
                            rc = SLTCFailSpan(
                                c,
                                SLDiag_CONST_INIT_CONST_REQUIRED,
                                c->lastConstEvalReasonStart,
                                c->lastConstEvalReasonEnd);
                        } else {
                            rc = SLTCFailNode(c, initNode, SLDiag_CONST_INIT_CONST_REQUIRED);
                        }
                        SLTCAttachConstEvalReason(c);
                        return rc;
                    }
                }
            }
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

int SLTCGetNullNarrow(SLTypeCheckCtx* c, int32_t condNode, int* outIsEq, SLTCNullNarrow* out) {
    const SLAstNode* n;
    int32_t          lhs, rhs, identNode;
    SLTokenKind      op;
    int32_t          localIdx;
    int32_t          typeId;

    if (condNode < 0 || (uint32_t)condNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[condNode];
    if (n->kind != SLAst_BINARY) {
        return 0;
    }
    op = (SLTokenKind)n->op;
    if (op != SLTok_EQ && op != SLTok_NEQ) {
        return 0;
    }
    lhs = SLAstFirstChild(c->ast, condNode);
    rhs = lhs >= 0 ? SLAstNextSibling(c->ast, lhs) : -1;
    if (lhs < 0 || rhs < 0) {
        return 0;
    }
    /* Identify which side is the ident and which is null. */
    if (c->ast->nodes[lhs].kind == SLAst_IDENT && c->ast->nodes[rhs].kind == SLAst_NULL) {
        identNode = lhs;
    } else if (c->ast->nodes[rhs].kind == SLAst_IDENT && c->ast->nodes[lhs].kind == SLAst_NULL) {
        identNode = rhs;
    } else {
        return 0;
    }
    {
        const SLAstNode* id = &c->ast->nodes[identNode];
        localIdx = SLTCLocalFind(c, id->dataStart, id->dataEnd);
    }
    if (localIdx < 0) {
        return 0;
    }
    typeId = c->locals[localIdx].typeId;
    if (c->types[typeId].kind != SLTCType_OPTIONAL) {
        return 0;
    }
    *outIsEq = (op == SLTok_EQ);
    out->localIdx = localIdx;
    out->innerType = c->types[typeId].baseType;
    return 1;
}

int SLTCGetOptionalCondNarrow(
    SLTypeCheckCtx* c, int32_t condNode, int* outThenIsSome, SLTCNullNarrow* out) {
    const SLAstNode* n;
    int32_t          localIdx;
    int32_t          typeId;
    int              isEq = 0;
    if (c == NULL || outThenIsSome == NULL || out == NULL || condNode < 0
        || (uint32_t)condNode >= c->ast->len)
    {
        return 0;
    }
    n = &c->ast->nodes[condNode];

    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_NOT) {
        int32_t inner = SLAstFirstChild(c->ast, condNode);
        if (inner < 0) {
            return 0;
        }
        if (!SLTCGetOptionalCondNarrow(c, inner, outThenIsSome, out)) {
            return 0;
        }
        *outThenIsSome = !*outThenIsSome;
        return 1;
    }

    if (n->kind == SLAst_IDENT) {
        localIdx = SLTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx < 0) {
            return 0;
        }
        typeId = c->locals[localIdx].typeId;
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen
            || c->types[typeId].kind != SLTCType_OPTIONAL)
        {
            return 0;
        }
        out->localIdx = localIdx;
        out->innerType = c->types[typeId].baseType;
        *outThenIsSome = 1;
        return 1;
    }

    if (SLTCGetNullNarrow(c, condNode, &isEq, out)) {
        *outThenIsSome = isEq ? 0 : 1;
        return 1;
    }

    return 0;
}

SL_API_END
