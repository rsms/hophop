#include "internal.h"

SL_API_BEGIN

int SLTCBlockTerminates(SLTypeCheckCtx* c, int32_t blockNode) {
    int32_t child = SLAstFirstChild(c->ast, blockNode);
    int32_t last = -1;
    while (child >= 0) {
        last = child;
        child = SLAstNextSibling(c->ast, child);
    }
    if (last < 0) {
        return 0;
    }
    switch (c->ast->nodes[last].kind) {
        case SLAst_RETURN:
        case SLAst_BREAK:
        case SLAst_CONTINUE: return 1;
        default:             return 0;
    }
}

int SLTCTypeBlock(
    SLTypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t       savedLocalLen = c->localLen;
    uint32_t       savedVariantNarrowLen = c->variantNarrowLen;
    int32_t        child = SLAstFirstChild(c->ast, blockNode);
    SLTCNarrowSave narrows[8]; /* saved narrowings applied during this block */
    int            narrowLen = 0;
    int            i;

    while (child >= 0) {
        int32_t next = SLAstNextSibling(c->ast, child);
        if (SLTCTypeStmt(c, child, returnType, loopDepth, switchDepth) != 0) {
            for (i = 0; i < narrowLen; i++) {
                c->locals[narrows[i].localIdx].typeId = narrows[i].savedType;
            }
            c->localLen = savedLocalLen;
            c->variantNarrowLen = savedVariantNarrowLen;
            return -1;
        }
        /*
         * Guard-pattern continuation narrowing:
         *   if x == null { <terminates> }   ->  x narrows to T for the rest of the
         * block if x != null { <terminates> }   ->  x narrows to null for the rest
         * of the block Only fires when there is more code after the if (next >= 0)
         * and no else clause.
         */
        if (next >= 0 && c->ast->nodes[child].kind == SLAst_IF && narrowLen < 8) {
            int32_t        condNode = SLAstFirstChild(c->ast, child);
            int32_t        thenNode = condNode >= 0 ? SLAstNextSibling(c->ast, condNode) : -1;
            int32_t        elseNode = thenNode >= 0 ? SLAstNextSibling(c->ast, thenNode) : -1;
            SLTCNullNarrow narrow;
            int            isEq;
            if (elseNode < 0 && thenNode >= 0 && condNode >= 0 && SLTCBlockTerminates(c, thenNode)
                && SLTCGetNullNarrow(c, condNode, &isEq, &narrow))
            {
                int32_t contType = isEq ? narrow.innerType : c->typeNull;
                narrows[narrowLen].localIdx = narrow.localIdx;
                narrows[narrowLen].savedType = c->locals[narrow.localIdx].typeId;
                narrowLen++;
                c->locals[narrow.localIdx].typeId = contType;
            }
        }
        child = next;
    }
    for (i = 0; i < narrowLen; i++) {
        c->locals[narrows[i].localIdx].typeId = narrows[i].savedType;
    }
    c->localLen = savedLocalLen;
    c->variantNarrowLen = savedVariantNarrowLen;
    return 0;
}

int SLTCTypeForStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t savedLocalLen = c->localLen;
    int32_t  child = SLAstFirstChild(c->ast, nodeId);
    int32_t  nodes[4];
    int      count = 0;
    int      i;

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = SLAstNextSibling(c->ast, child);
    }

    if (count == 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }

    for (i = 0; i < count - 1; i++) {
        const SLAstNode* n = &c->ast->nodes[nodes[i]];
        if (n->kind == SLAst_VAR || n->kind == SLAst_CONST) {
            if (SLTCTypeVarLike(c, nodes[i]) != 0) {
                return -1;
            }
        } else {
            int32_t t;
            if (SLTCTypeExpr(c, nodes[i], &t) != 0) {
                return -1;
            }
            if (i == 1 && count == 4 && !SLTCIsBoolType(c, t)) {
                return SLTCFailNode(c, nodes[i], SLDiag_EXPECTED_BOOL);
            }
            if (i == 0 && count == 2 && !SLTCIsBoolType(c, t)) {
                return SLTCFailNode(c, nodes[i], SLDiag_EXPECTED_BOOL);
            }
        }
    }

    if (c->ast->nodes[nodes[count - 1]].kind != SLAst_BLOCK) {
        return SLTCFailNode(c, nodes[count - 1], SLDiag_UNEXPECTED_TOKEN);
    }

    if (SLTCTypeBlock(c, nodes[count - 1], returnType, loopDepth + 1, switchDepth) != 0) {
        return -1;
    }

    c->localLen = savedLocalLen;
    return 0;
}

int SLTCTypeSwitchStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const SLAstNode* sw = &c->ast->nodes[nodeId];
    int32_t          child = SLAstFirstChild(c->ast, nodeId);
    int32_t          subjectNode = -1;
    int32_t          subjectType = -1;
    int32_t          subjectEnumType = -1;
    int32_t          subjectLocalIdx = -1;
    uint32_t         enumVariantCount = 0;
    uint32_t*        enumVariantStarts = NULL;
    uint32_t*        enumVariantEnds = NULL;
    uint8_t*         enumCovered = NULL;
    int              boolCoveredTrue = 0;
    int              boolCoveredFalse = 0;
    int              hasDefault = 0;
    int              i;

    if (sw->flags == 1) {
        if (child < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        subjectNode = child;
        if (SLTCTypeExpr(c, child, &subjectType) != 0) {
            return -1;
        }
        if (c->ast->nodes[subjectNode].kind == SLAst_IDENT) {
            subjectLocalIdx = SLTCLocalFind(
                c, c->ast->nodes[subjectNode].dataStart, c->ast->nodes[subjectNode].dataEnd);
        }
        if (SLTCIsNamedDeclKind(c, subjectType, SLAst_ENUM)) {
            int32_t declNode = c->types[SLTCResolveAliasBaseType(c, subjectType)].declNode;
            int32_t variant = SLTCEnumDeclFirstVariantNode(c, declNode);
            while (variant >= 0) {
                if (c->ast->nodes[variant].kind == SLAst_FIELD) {
                    enumVariantCount++;
                }
                variant = SLAstNextSibling(c->ast, variant);
            }
            if (enumVariantCount > 0) {
                uint32_t idx = 0;
                variant = SLTCEnumDeclFirstVariantNode(c, declNode);
                enumVariantStarts = (uint32_t*)SLArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
                enumVariantEnds = (uint32_t*)SLArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
                enumCovered = (uint8_t*)SLArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
                if (enumVariantStarts == NULL || enumVariantEnds == NULL || enumCovered == NULL) {
                    return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
                }
                while (variant >= 0 && idx < enumVariantCount) {
                    if (c->ast->nodes[variant].kind == SLAst_FIELD) {
                        enumVariantStarts[idx] = c->ast->nodes[variant].dataStart;
                        enumVariantEnds[idx] = c->ast->nodes[variant].dataEnd;
                        enumCovered[idx] = 0;
                        idx++;
                    }
                    variant = SLAstNextSibling(c->ast, variant);
                }
                subjectEnumType = SLTCResolveAliasBaseType(c, subjectType);
            }
        }
        child = SLAstNextSibling(c->ast, child);
    }

    while (child >= 0) {
        const SLAstNode* clause = &c->ast->nodes[child];
        if (clause->kind == SLAst_CASE) {
            uint32_t savedLocalLen = c->localLen;
            uint32_t savedVariantNarrowLen = c->variantNarrowLen;
            int32_t  caseChild = SLAstFirstChild(c->ast, child);
            int32_t  bodyNode = -1;
            int      labelCount = 0;
            int      singleVariantLabel = 0;
            uint32_t singleVariantStart = 0;
            uint32_t singleVariantEnd = 0;
            while (caseChild >= 0) {
                int32_t next = SLAstNextSibling(c->ast, caseChild);
                int32_t labelExprNode;
                int32_t aliasNode;
                if (next < 0) {
                    bodyNode = caseChild;
                    break;
                }
                if (SLTCCasePatternParts(c, caseChild, &labelExprNode, &aliasNode) != 0) {
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
                if (sw->flags == 1) {
                    int32_t  labelEnumType = -1;
                    uint32_t labelVariantStart = 0;
                    uint32_t labelVariantEnd = 0;
                    int      variantRc = SLTCDecodeVariantPatternExpr(
                        c, labelExprNode, &labelEnumType, &labelVariantStart, &labelVariantEnd);
                    int32_t labelType;
                    if (variantRc < 0) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return -1;
                    }
                    if (variantRc == 1) {
                        if (subjectEnumType < 0 || labelEnumType != subjectEnumType) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return SLTCFailNode(c, labelExprNode, SLDiag_TYPE_MISMATCH);
                        }
                        for (i = 0; i < (int)enumVariantCount; i++) {
                            if (SLNameEqSlice(
                                    c->src,
                                    enumVariantStarts[i],
                                    enumVariantEnds[i],
                                    labelVariantStart,
                                    labelVariantEnd))
                            {
                                enumCovered[i] = 1;
                                break;
                            }
                        }
                        if (labelCount == 0) {
                            singleVariantLabel = 1;
                            singleVariantStart = labelVariantStart;
                            singleVariantEnd = labelVariantEnd;
                        } else {
                            singleVariantLabel = 0;
                        }
                    } else {
                        if (SLTCTypeExpr(c, labelExprNode, &labelType) != 0) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return -1;
                        }
                        if (!SLTCCanAssign(c, subjectType, labelType)) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return SLTCFailNode(c, labelExprNode, SLDiag_TYPE_MISMATCH);
                        }
                        if (SLTCIsBoolType(c, subjectType)
                            && c->ast->nodes[labelExprNode].kind == SLAst_BOOL)
                        {
                            if (SLNameEqLiteral(
                                    c->src,
                                    c->ast->nodes[labelExprNode].dataStart,
                                    c->ast->nodes[labelExprNode].dataEnd,
                                    "true"))
                            {
                                boolCoveredTrue = 1;
                            } else {
                                boolCoveredFalse = 1;
                            }
                        }
                        singleVariantLabel = 0;
                    }
                    if (aliasNode >= 0) {
                        if (variantRc != 1 || subjectEnumType < 0) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return SLTCFailNode(c, aliasNode, SLDiag_TYPE_MISMATCH);
                        }
                        if (SLTCLocalAdd(
                                c,
                                c->ast->nodes[aliasNode].dataStart,
                                c->ast->nodes[aliasNode].dataEnd,
                                subjectType,
                                0)
                            != 0)
                        {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return -1;
                        }
                        if (SLTCVariantNarrowPush(
                                c,
                                (int32_t)c->localLen - 1,
                                subjectEnumType,
                                labelVariantStart,
                                labelVariantEnd)
                            != 0)
                        {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return -1;
                        }
                    }
                } else {
                    int32_t condType;
                    if (aliasNode >= 0) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return SLTCFailNode(c, aliasNode, SLDiag_UNEXPECTED_TOKEN);
                    }
                    if (SLTCTypeExpr(c, labelExprNode, &condType) != 0) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return -1;
                    }
                    if (!SLTCIsBoolType(c, condType)) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return SLTCFailNode(c, labelExprNode, SLDiag_EXPECTED_BOOL);
                    }
                }
                labelCount++;
                caseChild = next;
            }
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
                c->localLen = savedLocalLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
            }
            if (sw->flags == 1 && subjectEnumType >= 0 && subjectLocalIdx >= 0 && labelCount == 1
                && singleVariantLabel)
            {
                if (SLTCVariantNarrowPush(
                        c, subjectLocalIdx, subjectEnumType, singleVariantStart, singleVariantEnd)
                    != 0)
                {
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
            }
            if (SLTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                c->localLen = savedLocalLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                return -1;
            }
            c->localLen = savedLocalLen;
            c->variantNarrowLen = savedVariantNarrowLen;
        } else if (clause->kind == SLAst_DEFAULT) {
            int32_t bodyNode = SLAstFirstChild(c->ast, child);
            hasDefault = 1;
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
                return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                return -1;
            }
        } else {
            return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
        }
        child = SLAstNextSibling(c->ast, child);
    }

    if (sw->flags == 1 && !hasDefault) {
        if (subjectEnumType >= 0) {
            int hasMissing = 0;
            for (i = 0; i < (int)enumVariantCount; i++) {
                if (!enumCovered[i]) {
                    hasMissing = 1;
                    break;
                }
            }
            if (hasMissing) {
                return SLTCFailSwitchMissingCases(
                    c,
                    nodeId,
                    subjectType,
                    subjectEnumType,
                    enumVariantCount,
                    enumVariantStarts,
                    enumVariantEnds,
                    enumCovered,
                    boolCoveredTrue,
                    boolCoveredFalse);
            }
        } else if (SLTCIsBoolType(c, subjectType)) {
            if (!boolCoveredTrue || !boolCoveredFalse) {
                return SLTCFailSwitchMissingCases(
                    c,
                    nodeId,
                    subjectType,
                    subjectEnumType,
                    enumVariantCount,
                    enumVariantStarts,
                    enumVariantEnds,
                    enumCovered,
                    boolCoveredTrue,
                    boolCoveredFalse);
            }
        }
    }

    return 0;
}

int SLTCExprIsBlankIdent(SLTypeCheckCtx* c, int32_t exprNode) {
    const SLAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    return n->kind == SLAst_IDENT && SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_");
}

int SLTCTypeMultiAssignStmt(SLTypeCheckCtx* c, int32_t nodeId) {
    int32_t  lhsList = SLAstFirstChild(c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? SLAstNextSibling(c->ast, lhsList) : -1;
    uint32_t lhsCount;
    uint32_t rhsCount;
    int32_t  rhsTypes[256];
    uint32_t i;
    if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != SLAst_EXPR_LIST
        || c->ast->nodes[rhsList].kind != SLAst_EXPR_LIST)
    {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    lhsCount = SLTCListCount(c->ast, lhsList);
    rhsCount = SLTCListCount(c->ast, rhsList);
    if (lhsCount == 0 || lhsCount > (uint32_t)(sizeof(rhsTypes) / sizeof(rhsTypes[0]))) {
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
    if (rhsCount == lhsCount) {
        for (i = 0; i < rhsCount; i++) {
            int32_t rhsNode = SLTCListItemAt(c->ast, rhsList, i);
            if (rhsNode < 0 || SLTCTypeExpr(c, rhsNode, &rhsTypes[i]) != 0) {
                return -1;
            }
        }
    } else if (rhsCount == 1u) {
        int32_t         rhsNode = SLTCListItemAt(c->ast, rhsList, 0);
        int32_t         rhsType;
        const SLTCType* t;
        if (rhsNode < 0 || SLTCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType < 0 || (uint32_t)rhsType >= c->typeLen) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        t = &c->types[rhsType];
        if (t->kind != SLTCType_TUPLE || t->fieldCount != lhsCount) {
            return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
        }
        for (i = 0; i < lhsCount; i++) {
            rhsTypes[i] = c->funcParamTypes[t->fieldStart + i];
        }
    } else {
        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
    }
    for (i = 0; i < lhsCount; i++) {
        int32_t lhsNode = SLTCListItemAt(c->ast, lhsList, i);
        int32_t rhsType = rhsTypes[i];
        if (lhsNode < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        if (SLTCExprIsBlankIdent(c, lhsNode)) {
            if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
                if (SLTCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                    return -1;
                }
            }
            continue;
        }
        {
            int32_t lhsType;
            if (SLTCTypeExpr(c, lhsNode, &lhsType) != 0) {
                return -1;
            }
            if (!SLTCExprIsAssignable(c, lhsNode)) {
                return SLTCFailNode(c, lhsNode, SLDiag_TYPE_MISMATCH);
            }
            if (SLTCExprIsConstAssignTarget(c, lhsNode)) {
                return SLTCFailAssignToConst(c, lhsNode);
            }
            if (!SLTCCanAssign(c, lhsType, rhsType)) {
                return SLTCFailTypeMismatchDetail(c, lhsNode, lhsNode, rhsType, lhsType);
            }
        }
    }
    return 0;
}

int SLTCTypeStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAst_BLOCK:        return SLTCTypeBlock(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAst_VAR:
        case SLAst_CONST:        return SLTCTypeVarLike(c, nodeId);
        case SLAst_MULTI_ASSIGN: return SLTCTypeMultiAssignStmt(c, nodeId);
        case SLAst_EXPR_STMT:    {
            int32_t expr = SLAstFirstChild(c->ast, nodeId);
            int32_t t;
            if (expr < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            return SLTCTypeExpr(c, expr, &t);
        }
        case SLAst_RETURN: {
            int32_t expr = SLAstFirstChild(c->ast, nodeId);
            if (expr < 0) {
                if (returnType != c->typeVoid) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }
                return 0;
            }
            if (c->ast->nodes[expr].kind == SLAst_EXPR_LIST) {
                const SLTCType* rt;
                uint32_t        wantCount;
                uint32_t        i;
                if (returnType < 0 || (uint32_t)returnType >= c->typeLen) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }
                rt = &c->types[returnType];
                if (rt->kind != SLTCType_TUPLE) {
                    return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                }
                wantCount = rt->fieldCount;
                if (SLTCListCount(c->ast, expr) != wantCount) {
                    return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                }
                for (i = 0; i < wantCount; i++) {
                    int32_t itemNode = SLTCListItemAt(c->ast, expr, i);
                    int32_t itemType;
                    int32_t dstType;
                    if (itemNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                    }
                    dstType = c->funcParamTypes[rt->fieldStart + i];
                    if (SLTCTypeExprExpected(c, itemNode, dstType, &itemType) != 0) {
                        return -1;
                    }
                    if (!SLTCCanAssign(c, dstType, itemType)) {
                        return SLTCFailTypeMismatchDetail(c, itemNode, itemNode, itemType, dstType);
                    }
                }
                return 0;
            }
            {
                int32_t t;
                if (SLTCTypeExprExpected(c, expr, returnType, &t) != 0) {
                    return -1;
                }
                if (!SLTCCanAssign(c, returnType, t)) {
                    return SLTCFailNode(c, expr, SLDiag_TYPE_MISMATCH);
                }
                return 0;
            }
        }
        case SLAst_IF: {
            int32_t        cond = SLAstFirstChild(c->ast, nodeId);
            int32_t        thenNode;
            int32_t        elseNode;
            int32_t        condType;
            SLTCNullNarrow narrow;
            int            isEq;
            int            hasNarrow;
            if (cond < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
            }
            thenNode = SLAstNextSibling(c->ast, cond);
            if (thenNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            if (!SLTCIsBoolType(c, condType)) {
                return SLTCFailNode(c, cond, SLDiag_EXPECTED_BOOL);
            }
            elseNode = SLAstNextSibling(c->ast, thenNode);
            hasNarrow = SLTCGetNullNarrow(c, cond, &isEq, &narrow);
            if (hasNarrow) {
                /*
                 * Apply branch narrowing:
                 *   x == null  -> then: x is null;  else: x is T
                 *   x != null  -> then: x is T;     else: x is null
                 */
                int32_t origType = c->locals[narrow.localIdx].typeId;
                int32_t trueType = isEq ? c->typeNull : narrow.innerType;
                int32_t falseType = isEq ? narrow.innerType : c->typeNull;
                c->locals[narrow.localIdx].typeId = trueType;
                if (SLTCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                    c->locals[narrow.localIdx].typeId = origType;
                    return -1;
                }
                c->locals[narrow.localIdx].typeId = falseType;
                if (elseNode >= 0
                    && SLTCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                {
                    c->locals[narrow.localIdx].typeId = origType;
                    return -1;
                }
                c->locals[narrow.localIdx].typeId = origType;
            } else {
                if (SLTCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                    return -1;
                }
                if (elseNode >= 0
                    && SLTCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_FOR:    return SLTCTypeForStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAst_SWITCH: return SLTCTypeSwitchStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAst_BREAK:
            if (loopDepth <= 0 && switchDepth <= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case SLAst_CONTINUE:
            if (loopDepth <= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case SLAst_DEFER: {
            int32_t stmt = SLAstFirstChild(c->ast, nodeId);
            if (stmt < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return SLTCTypeStmt(c, stmt, returnType, loopDepth, switchDepth);
        }
        case SLAst_ASSERT: {
            int32_t cond = SLAstFirstChild(c->ast, nodeId);
            int32_t condType;
            int32_t fmtNode;
            if (cond < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
            }
            if (SLTCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            if (!SLTCIsBoolType(c, condType)) {
                return SLTCFailNode(c, cond, SLDiag_EXPECTED_BOOL);
            }
            fmtNode = SLAstNextSibling(c->ast, cond);
            if (fmtNode >= 0) {
                int32_t fmtType;
                int32_t argNode;
                int32_t wantStrType = SLTCGetStrRefType(c, n->start, n->end);
                if (SLTCTypeExpr(c, fmtNode, &fmtType) != 0) {
                    return -1;
                }
                if (wantStrType < 0) {
                    return SLTCFailNode(c, fmtNode, SLDiag_UNKNOWN_TYPE);
                }
                if (!SLTCCanAssign(c, wantStrType, fmtType)) {
                    return SLTCFailNode(c, fmtNode, SLDiag_TYPE_MISMATCH);
                }
                argNode = SLAstNextSibling(c->ast, fmtNode);
                while (argNode >= 0) {
                    int32_t argType;
                    if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                        return -1;
                    }
                    argNode = SLAstNextSibling(c->ast, argNode);
                }
            }
            return 0;
        }
        default: return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
    }
}

int SLTCTypeFunctionBody(SLTypeCheckCtx* c, int32_t funcIndex) {
    const SLTCFunction* fn = &c->funcs[funcIndex];
    int32_t             nodeId = fn->defNode;
    int32_t             child;
    uint32_t            paramIndex = 0;
    int32_t             bodyNode = -1;
    int32_t             savedContextType = c->currentContextType;
    int                 savedImplicitRoot = c->hasImplicitMainRootContext;
    int32_t             savedImplicitMainContextType = c->implicitMainContextType;
    int32_t             savedFunctionIndex = c->currentFunctionIndex;
    int                 savedFunctionIsCompareHook = c->currentFunctionIsCompareHook;
    int32_t             savedActiveTypeParamFnNode = c->activeTypeParamFnNode;
    int                 isEqualHook = 0;

    if (nodeId < 0) {
        return 0;
    }
    if ((fn->flags & SLTCFunctionFlag_TEMPLATE) != 0
        && (fn->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
    {
        return 0;
    }

    c->localLen = 0;
    c->currentFunctionIndex = funcIndex;
    c->currentFunctionIsCompareHook = SLTCIsComparisonHookName(
        c, fn->nameStart, fn->nameEnd, &isEqualHook);
    c->activeTypeParamFnNode = nodeId;

    child = SLAstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            int32_t paramType;
            int     addedLocal = 0;
            if (paramIndex >= fn->paramCount) {
                return SLTCFailNode(c, child, SLDiag_ARITY_MISMATCH);
            }
            paramType = c->funcParamTypes[fn->paramTypeStart + paramIndex];
            if (!SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")
                && SLTCLocalAdd(
                       c,
                       n->dataStart,
                       n->dataEnd,
                       paramType,
                       (c->funcParamFlags[fn->paramTypeStart + paramIndex]
                        & SLTCFuncParamFlag_CONST)
                           != 0)
                       != 0)
            {
                return -1;
            }
            if (!SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")) {
                addedLocal = 1;
            }
            if (addedLocal && (n->flags & SLAstFlag_PARAM_VARIADIC) != 0
                && (paramType == c->typeAnytype
                    || ((uint32_t)paramType < c->typeLen
                        && c->types[paramType].kind == SLTCType_PACK)))
            {
                c->locals[c->localLen - 1u].flags |= SLTCLocalFlag_ANYPACK;
            }
            paramIndex++;
        } else if (n->kind == SLAst_BLOCK) {
            bodyNode = child;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    if (bodyNode < 0) {
        c->currentFunctionIndex = savedFunctionIndex;
        c->currentFunctionIsCompareHook = savedFunctionIsCompareHook;
        c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
        return 0;
    }

    c->currentContextType = -1;
    c->hasImplicitMainRootContext = 0;
    c->implicitMainContextType = -1;
    if (fn->contextType >= 0) {
        int32_t contextLocalType = SLTCInternRefType(
            c, fn->contextType, 1, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
        int32_t  fnChild = SLAstFirstChild(c->ast, nodeId);
        uint32_t contextNameStart = 0;
        uint32_t contextNameEnd = 0;
        if (contextLocalType < 0) {
            c->currentFunctionIndex = savedFunctionIndex;
            c->currentFunctionIsCompareHook = savedFunctionIsCompareHook;
            c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
            c->currentContextType = savedContextType;
            c->hasImplicitMainRootContext = savedImplicitRoot;
            c->implicitMainContextType = savedImplicitMainContextType;
            return -1;
        }
        while (fnChild >= 0) {
            const SLAstNode* ch = &c->ast->nodes[fnChild];
            if (ch->kind == SLAst_CONTEXT_CLAUSE) {
                contextNameStart = ch->start;
                contextNameEnd = ch->start + 7u; /* "context" */
                break;
            }
            fnChild = SLAstNextSibling(c->ast, fnChild);
        }
        if (contextNameEnd <= contextNameStart
            || SLTCLocalAdd(c, contextNameStart, contextNameEnd, contextLocalType, 0) != 0)
        {
            c->currentFunctionIndex = savedFunctionIndex;
            c->currentFunctionIsCompareHook = savedFunctionIsCompareHook;
            c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
            c->currentContextType = savedContextType;
            c->hasImplicitMainRootContext = savedImplicitRoot;
            c->implicitMainContextType = savedImplicitMainContextType;
            return -1;
        }
        c->currentContextType = fn->contextType;
    } else if (SLTCIsMainFunction(c, fn)) {
        c->implicitMainContextType = SLTCResolveImplicitMainContextType(c);
        c->hasImplicitMainRootContext = 1;
    }

    {
        int rc = SLTCTypeBlock(c, bodyNode, fn->returnType, 0, 0);
        c->currentFunctionIndex = savedFunctionIndex;
        c->currentFunctionIsCompareHook = savedFunctionIsCompareHook;
        c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
        c->currentContextType = savedContextType;
        c->hasImplicitMainRootContext = savedImplicitRoot;
        c->implicitMainContextType = savedImplicitMainContextType;
        return rc;
    }
}

int SLTCCollectFunctionDecls(SLTypeCheckCtx* c) {
    int32_t child = SLAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (SLTCCollectFunctionFromNode(c, child) != 0) {
            return -1;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

int SLTCCollectTypeDecls(SLTypeCheckCtx* c) {
    int32_t child = SLAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (SLTCCollectTypeDeclsFromNode(c, child) != 0) {
            return -1;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

int SLTCBuildCheckedContext(
    SLArena*     arena,
    const SLAst* ast,
    SLStrView    src,
    const SLTypeCheckOptions* _Nullable options,
    SLDiag* diag,
    SLTypeCheckCtx* _Nullable outCtx) {
    SLTypeCheckCtx c;
    uint32_t       capBase;
    uint32_t       i;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }

    if (arena == NULL || ast == NULL || ast->nodes == NULL || ast->root < 0) {
        SLTCSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    capBase = ast->len < 32 ? 32u : ast->len;

    c.arena = arena;
    c.ast = ast;
    c.src = src;
    c.diag = diag;
    c.diagSink.ctx = options != NULL ? options->ctx : NULL;
    c.diagSink.onDiag = options != NULL ? options->onDiag : NULL;

    c.types = (SLTCType*)SLArenaAlloc(
        arena, sizeof(SLTCType) * capBase * 4u, (uint32_t)_Alignof(SLTCType));
    c.fields = (SLTCField*)SLArenaAlloc(
        arena, sizeof(SLTCField) * capBase * 4u, (uint32_t)_Alignof(SLTCField));
    c.namedTypes = (SLTCNamedType*)SLArenaAlloc(
        arena, sizeof(SLTCNamedType) * capBase, (uint32_t)_Alignof(SLTCNamedType));
    c.funcs = (SLTCFunction*)SLArenaAlloc(
        arena, sizeof(SLTCFunction) * capBase, (uint32_t)_Alignof(SLTCFunction));
    c.funcParamTypes = (int32_t*)SLArenaAlloc(
        arena, sizeof(int32_t) * capBase * 8u, (uint32_t)_Alignof(int32_t));
    c.funcParamNameStarts = (uint32_t*)SLArenaAlloc(
        arena, sizeof(uint32_t) * capBase * 8u, (uint32_t)_Alignof(uint32_t));
    c.funcParamNameEnds = (uint32_t*)SLArenaAlloc(
        arena, sizeof(uint32_t) * capBase * 8u, (uint32_t)_Alignof(uint32_t));
    c.funcParamFlags = (uint8_t*)SLArenaAlloc(
        arena, sizeof(uint8_t) * capBase * 8u, (uint32_t)_Alignof(uint8_t));
    c.scratchParamTypes = (int32_t*)SLArenaAlloc(
        arena, sizeof(int32_t) * capBase, (uint32_t)_Alignof(int32_t));
    c.scratchParamFlags = (uint8_t*)SLArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.locals = (SLTCLocal*)SLArenaAlloc(
        arena, sizeof(SLTCLocal) * capBase * 4u, (uint32_t)_Alignof(SLTCLocal));
    c.variantNarrows = (SLTCVariantNarrow*)SLArenaAlloc(
        arena, sizeof(SLTCVariantNarrow) * capBase * 4u, (uint32_t)_Alignof(SLTCVariantNarrow));
    c.warningDedup = (SLTCWarningDedup*)SLArenaAlloc(
        arena, sizeof(SLTCWarningDedup) * capBase, (uint32_t)_Alignof(SLTCWarningDedup));
    c.constDiagUses = (SLTCConstDiagUse*)SLArenaAlloc(
        arena, sizeof(SLTCConstDiagUse) * capBase, (uint32_t)_Alignof(SLTCConstDiagUse));
    c.constDiagFnInvoked = (uint8_t*)SLArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.constEvalValues = (SLCTFEValue*)SLArenaAlloc(
        arena, sizeof(SLCTFEValue) * ast->len, (uint32_t)_Alignof(SLCTFEValue));
    c.constEvalState = (uint8_t*)SLArenaAlloc(
        arena, sizeof(uint8_t) * ast->len, (uint32_t)_Alignof(uint8_t));
    c.topVarLikeTypes = (int32_t*)SLArenaAlloc(
        arena, sizeof(int32_t) * ast->len, (uint32_t)_Alignof(int32_t));
    c.topVarLikeTypeState = (uint8_t*)SLArenaAlloc(
        arena, sizeof(uint8_t) * ast->len, (uint32_t)_Alignof(uint8_t));

    if (c.types == NULL || c.fields == NULL || c.namedTypes == NULL || c.funcs == NULL
        || c.funcParamTypes == NULL || c.funcParamNameStarts == NULL || c.funcParamNameEnds == NULL
        || c.funcParamFlags == NULL || c.scratchParamTypes == NULL || c.scratchParamFlags == NULL
        || c.locals == NULL || c.constEvalValues == NULL || c.constEvalState == NULL
        || c.topVarLikeTypes == NULL || c.topVarLikeTypeState == NULL || c.variantNarrows == NULL
        || c.warningDedup == NULL || c.constDiagUses == NULL || c.constDiagFnInvoked == NULL)
    {
        SLTCSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    c.typeLen = 0;
    c.typeCap = capBase * 4u;
    c.fieldLen = 0;
    c.fieldCap = capBase * 4u;
    c.namedTypeLen = 0;
    c.namedTypeCap = capBase;
    c.funcLen = 0;
    c.funcCap = capBase;
    c.funcParamLen = 0;
    c.funcParamCap = capBase * 8u;
    c.scratchParamCap = capBase;
    c.localLen = 0;
    c.localCap = capBase * 4u;
    c.variantNarrowLen = 0;
    c.variantNarrowCap = capBase * 4u;
    c.warningDedupLen = 0;
    c.warningDedupCap = capBase;
    c.constDiagUseLen = 0;
    c.constDiagUseCap = capBase;
    c.constDiagFnInvokedCap = capBase;
    c.currentContextType = -1;
    c.hasImplicitMainRootContext = 0;
    c.implicitMainContextType = -1;
    c.activeCallWithNode = -1;
    c.currentFunctionIndex = -1;
    c.currentFunctionIsCompareHook = 0;
    c.activeTypeParamFnNode = -1;
    c.defaultFieldNodes = NULL;
    c.defaultFieldTypes = NULL;
    c.defaultFieldCount = 0;
    c.defaultFieldCurrentIndex = 0;
    c.lastConstEvalReason = NULL;
    c.lastConstEvalReasonStart = 0;
    c.lastConstEvalReasonEnd = 0;
    memset(c.funcParamFlags, 0, sizeof(uint8_t) * c.funcParamCap);
    memset(c.scratchParamFlags, 0, sizeof(uint8_t) * c.scratchParamCap);
    if (ast->len > 0) {
        memset(c.constEvalState, 0, sizeof(uint8_t) * ast->len);
        memset(c.topVarLikeTypeState, 0, sizeof(uint8_t) * ast->len);
        memset(c.constDiagFnInvoked, 0, sizeof(uint8_t) * c.constDiagFnInvokedCap);
        for (i = 0; i < ast->len; i++) {
            c.topVarLikeTypes[i] = -1;
            c.constEvalValues[i].kind = SLCTFEValue_INVALID;
            c.constEvalValues[i].i64 = 0;
            c.constEvalValues[i].f64 = 0.0;
            c.constEvalValues[i].b = 0;
            c.constEvalValues[i].typeTag = 0;
            c.constEvalValues[i].s.bytes = NULL;
            c.constEvalValues[i].s.len = 0;
            c.constEvalValues[i].span.fileBytes = NULL;
            c.constEvalValues[i].span.fileLen = 0;
            c.constEvalValues[i].span.startLine = 0;
            c.constEvalValues[i].span.startColumn = 0;
            c.constEvalValues[i].span.endLine = 0;
            c.constEvalValues[i].span.endColumn = 0;
        }
    }

    if (SLTCEnsureInitialized(&c) != 0) {
        return -1;
    }
    c.typeUsize = SLTCFindBuiltinByKind(&c, SLBuiltin_USIZE);
    if (c.typeUsize < 0) {
        return SLTCFailSpan(&c, SLDiag_UNKNOWN_TYPE, 0, 0);
    }

    if (SLTCCollectTypeDecls(&c) != 0) {
        return -1;
    }
    {
        int32_t namedStrType = SLTCFindNamedTypeByLiteral(&c, "core__str");
        if (namedStrType < 0) {
            namedStrType = SLTCFindCoreNamedTypeBySuffix(&c, "__str");
        }
        if (namedStrType < 0) {
            namedStrType = SLTCFindNamedTypeByLiteral(&c, "str");
        }
        if (namedStrType >= 0) {
            c.typeStr = namedStrType;
        }
    }
    {
        int32_t namedRuneType = SLTCFindNamedTypeByLiteral(&c, "core__rune");
        if (namedRuneType < 0) {
            namedRuneType = SLTCFindCoreNamedTypeBySuffix(&c, "__rune");
        }
        if (namedRuneType < 0) {
            namedRuneType = SLTCFindNamedTypeByLiteral(&c, "rune");
        }
        if (namedRuneType >= 0) {
            c.typeRune = namedRuneType;
        }
    }
    c.typeMemAllocator = SLTCFindNamedTypeByLiteral(&c, "core__Allocator");
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = SLTCFindCoreNamedTypeBySuffix(&c, "__Allocator");
    }
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = SLTCFindNamedTypeByLiteral(&c, "Allocator");
    }
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = SLTCFindNamedTypeBySuffix(&c, "__Allocator");
    }
    if (SLTCResolveAllTypeAliases(&c) != 0) {
        return -1;
    }
    if (SLTCCollectFunctionDecls(&c) != 0) {
        return -1;
    }
    if (SLTCFinalizeFunctionTypes(&c) != 0) {
        return -1;
    }
    if (SLTCResolveAllNamedTypeFields(&c) != 0) {
        return -1;
    }
    if (SLTCCheckEmbeddedCycles(&c) != 0) {
        return -1;
    }
    if (SLTCPropagateVarSizeNamedTypes(&c) != 0) {
        return -1;
    }
    if (SLTCCheckTopLevelConstInitializers(&c) != 0) {
        return -1;
    }
    if (SLTCTypeTopLevelConsts(&c) != 0) {
        return -1;
    }
    if (SLTCTypeTopLevelVars(&c) != 0) {
        return -1;
    }

    for (i = 0; i < c.funcLen; i++) {
        if (SLTCTypeFunctionBody(&c, (int32_t)i) != 0) {
            return -1;
        }
    }
    if (SLTCValidateTopLevelConstEvaluable(&c) != 0) {
        return -1;
    }
    if (SLTCValidateConstDiagUses(&c) != 0) {
        return -1;
    }

    if (outCtx != NULL) {
        *outCtx = c;
    }

    return 0;
}

SL_API_END
