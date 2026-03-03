#include "internal.h"
#include "../fmt_parse.h"

SL_API_BEGIN
int IsTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_ANON_STRUCT || kind == SLAst_TYPE_ANON_UNION
        || kind == SLAst_TYPE_TUPLE;
}

int DecodeNewExprNodes(
    SLCBackendC* c,
    int32_t      nodeId,
    int32_t*     outTypeNode,
    int32_t*     outCountNode,
    int32_t*     outInitNode,
    int32_t*     outAllocNode) {
    const SLAstNode* n = NodeAt(c, nodeId);
    int32_t          typeNode;
    int32_t          nextNode;
    int              hasCount;
    int              hasInit;
    int              hasAlloc;
    if (n == NULL || n->kind != SLAst_NEW) {
        return -1;
    }
    hasCount = (n->flags & SLAstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & SLAstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & SLAstFlag_NEW_HAS_ALLOC) != 0;

    typeNode = AstFirstChild(&c->ast, nodeId);
    if (typeNode < 0) {
        return -1;
    }
    nextNode = AstNextSibling(&c->ast, typeNode);
    *outTypeNode = typeNode;
    *outCountNode = -1;
    *outInitNode = -1;
    *outAllocNode = -1;

    if (hasCount) {
        *outCountNode = nextNode;
        if (*outCountNode < 0) {
            return -1;
        }
        nextNode = AstNextSibling(&c->ast, *outCountNode);
    }
    if (hasInit) {
        *outInitNode = nextNode;
        if (*outInitNode < 0) {
            return -1;
        }
        nextNode = AstNextSibling(&c->ast, *outInitNode);
    }
    if (hasAlloc) {
        *outAllocNode = nextNode;
        if (*outAllocNode < 0) {
            return -1;
        }
        nextNode = AstNextSibling(&c->ast, *outAllocNode);
    }
    if (nextNode >= 0) {
        return -1;
    }
    return 0;
}

uint32_t ListCount(const SLAst* ast, int32_t listNode) {
    uint32_t count = 0;
    int32_t  child;
    if (listNode < 0 || (uint32_t)listNode >= ast->len) {
        return 0;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        count++;
        child = ast->nodes[child].nextSibling;
    }
    return count;
}

int32_t ListItemAt(const SLAst* ast, int32_t listNode, uint32_t index) {
    int32_t  child;
    uint32_t i = 0;
    if (listNode < 0 || (uint32_t)listNode >= ast->len) {
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

int ResolveVarLikeParts(SLCBackendC* c, int32_t nodeId, SLCCGVarLikeParts* out) {
    int32_t          firstChild = AstFirstChild(&c->ast, nodeId);
    const SLAstNode* firstNode;
    out->nameListNode = -1;
    out->typeNode = -1;
    out->initNode = -1;
    out->nameCount = 0;
    out->grouped = 0;
    if (firstChild < 0) {
        return 0;
    }
    firstNode = NodeAt(c, firstChild);
    if (firstNode != NULL && firstNode->kind == SLAst_NAME_LIST) {
        int32_t afterNames = AstNextSibling(&c->ast, firstChild);
        out->grouped = 1;
        out->nameListNode = firstChild;
        out->nameCount = ListCount(&c->ast, firstChild);
        if (afterNames >= 0 && NodeAt(c, afterNames) != NULL
            && IsTypeNodeKind(NodeAt(c, afterNames)->kind))
        {
            out->typeNode = afterNames;
            out->initNode = AstNextSibling(&c->ast, afterNames);
        } else {
            out->initNode = afterNames;
        }
        return 0;
    }
    out->nameCount = 1;
    if (firstNode != NULL && IsTypeNodeKind(firstNode->kind)) {
        out->typeNode = firstChild;
        out->initNode = AstNextSibling(&c->ast, firstChild);
    } else {
        out->initNode = firstChild;
    }
    return 0;
}

int InferVarLikeDeclType(SLCBackendC* c, int32_t initNode, SLTypeRef* outType) {
    if (initNode < 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (InferExprType(c, initNode, outType) != 0 || !outType->valid) {
        return -1;
    }
    if (outType->containerKind == SLTypeContainer_SCALAR && outType->containerPtrDepth == 0
        && outType->ptrDepth == 0 && outType->baseName != NULL
        && StrEq(outType->baseName, "__sl_i32"))
    {
        outType->baseName = "__sl_int";
    }
    return 0;
}

int FindTopLevelVarLikeNodeBySliceEx(
    const SLCBackendC* c,
    uint32_t           start,
    uint32_t           end,
    int32_t*           outNodeId,
    int32_t* _Nullable outNameIndex) {
    uint32_t i;
    if (outNodeId == NULL) {
        return -1;
    }
    if (outNameIndex != NULL) {
        *outNameIndex = -1;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n != NULL && (n->kind == SLAst_VAR || n->kind == SLAst_CONST)) {
            if (SliceSpanEq(c->unit->source, n->dataStart, n->dataEnd, start, end)) {
                *outNodeId = nodeId;
                if (outNameIndex != NULL) {
                    *outNameIndex = 0;
                }
                return 0;
            }
            {
                int32_t          firstChild = AstFirstChild(&c->ast, nodeId);
                const SLAstNode* firstNode = NodeAt(c, firstChild);
                if (firstNode != NULL && firstNode->kind == SLAst_NAME_LIST) {
                    uint32_t j;
                    uint32_t nameCount = ListCount(&c->ast, firstChild);
                    for (j = 0; j < nameCount; j++) {
                        int32_t          nameNode = ListItemAt(&c->ast, firstChild, j);
                        const SLAstNode* name = NodeAt(c, nameNode);
                        if (name != NULL
                            && SliceSpanEq(
                                c->unit->source, name->dataStart, name->dataEnd, start, end))
                        {
                            *outNodeId = nodeId;
                            if (outNameIndex != NULL) {
                                *outNameIndex = (int32_t)j;
                            }
                            return 0;
                        }
                    }
                }
            }
        }
    }
    return -1;
}

int FindTopLevelVarLikeNodeBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId) {
    return FindTopLevelVarLikeNodeBySliceEx(c, start, end, outNodeId, NULL);
}

int InferTopLevelVarLikeType(
    SLCBackendC* c, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd, SLTypeRef* outType) {
    SLCCGVarLikeParts parts;
    if (ResolveVarLikeParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (parts.typeNode >= 0) {
        return ParseTypeRef(c, parts.typeNode, outType);
    }
    if (!parts.grouped) {
        return InferVarLikeDeclType(c, parts.initNode, outType);
    }
    if (parts.initNode >= 0 && NodeAt(c, parts.initNode) != NULL
        && NodeAt(c, parts.initNode)->kind == SLAst_EXPR_LIST)
    {
        uint32_t i;
        uint32_t initCount = ListCount(&c->ast, parts.initNode);
        for (i = 0; i < parts.nameCount && i < initCount; i++) {
            int32_t          nameNode = ListItemAt(&c->ast, parts.nameListNode, i);
            const SLAstNode* name = NodeAt(c, nameNode);
            if (name != NULL
                && SliceSpanEq(c->unit->source, name->dataStart, name->dataEnd, nameStart, nameEnd))
            {
                int32_t initNode = ListItemAt(&c->ast, parts.initNode, i);
                return InferVarLikeDeclType(c, initNode, outType);
            }
        }
    }
    TypeRefSetInvalid(outType);
    return -1;
}

int ResolveTopLevelConstTypeValueBySlice(
    SLCBackendC* c, uint32_t start, uint32_t end, SLTypeRef* outType) {
    int32_t           nodeId = -1;
    int32_t           nameIndex = -1;
    const SLAstNode*  n;
    SLCCGVarLikeParts parts;
    int32_t           initNode = -1;
    SLCTFEValue       value;
    int               isConst = 0;
    if (outType == NULL) {
        return -1;
    }
    TypeRefSetInvalid(outType);
    if (c == NULL || c->constEval == NULL
        || FindTopLevelVarLikeNodeBySliceEx(c, start, end, &nodeId, &nameIndex) != 0)
    {
        return 0;
    }
    n = NodeAt(c, nodeId);
    if (n == NULL || n->kind != SLAst_CONST) {
        return 0;
    }
    if (ResolveVarLikeParts(c, nodeId, &parts) != 0 || parts.nameCount == 0 || nameIndex < 0
        || (uint32_t)nameIndex >= parts.nameCount)
    {
        return 0;
    }
    if (!parts.grouped) {
        initNode = parts.initNode;
    } else if (
        parts.initNode >= 0 && NodeAt(c, parts.initNode) != NULL
        && NodeAt(c, parts.initNode)->kind == SLAst_EXPR_LIST
        && (uint32_t)nameIndex < ListCount(&c->ast, parts.initNode))
    {
        initNode = ListItemAt(&c->ast, parts.initNode, (uint32_t)nameIndex);
    }
    if (initNode < 0) {
        return 0;
    }
    if (ResolveReflectedTypeValueExprTypeRef(c, initNode, outType)) {
        if (outType->valid) {
            return 1;
        }
        TypeRefSetInvalid(outType);
    }
    if (SLConstEvalSessionEvalExpr(c->constEval, initNode, &value, &isConst) != 0) {
        return 0;
    }
    if (!isConst || value.kind != SLCTFEValue_TYPE) {
        return 0;
    }
    if (ParseTypeRefFromConstEvalTypeTag(c, value.typeTag, outType) != 0 || !outType->valid) {
        TypeRefSetInvalid(outType);
        return 0;
    }
    return 1;
}

int TypeRefEqual(const SLTypeRef* a, const SLTypeRef* b) {
    if (a->valid != b->valid || a->ptrDepth != b->ptrDepth || a->containerKind != b->containerKind
        || a->containerPtrDepth != b->containerPtrDepth || a->hasArrayLen != b->hasArrayLen
        || a->arrayLen != b->arrayLen || a->readOnly != b->readOnly
        || a->isOptional != b->isOptional)
    {
        return 0;
    }
    if (a->baseName == NULL || b->baseName == NULL) {
        return a->baseName == b->baseName;
    }
    return StrEq(a->baseName, b->baseName);
}

int ExpandAliasSourceType(const SLCBackendC* c, const SLTypeRef* src, SLTypeRef* outExpanded) {
    const SLTypeAliasInfo* alias;
    if (!src->valid || src->baseName == NULL) {
        return 0;
    }
    alias = FindTypeAliasInfoByAliasName(c, src->baseName);
    if (alias == NULL) {
        return 0;
    }

    /* Wrapped alias: preserve wrappers only when alias target is scalar. */
    if ((src->ptrDepth > 0 || src->containerPtrDepth > 0 || src->isOptional)
        && alias->targetType.containerKind == SLTypeContainer_SCALAR
        && alias->targetType.ptrDepth == 0 && alias->targetType.containerPtrDepth == 0
        && alias->targetType.baseName != NULL)
    {
        *outExpanded = *src;
        outExpanded->baseName = alias->targetType.baseName;
        return 1;
    }

    /* Unwrapped alias can expand to any target type form. */
    if (src->containerKind == SLTypeContainer_SCALAR && src->ptrDepth == 0
        && src->containerPtrDepth == 0 && !src->isOptional)
    {
        *outExpanded = alias->targetType;
        return 1;
    }
    return 0;
}

int TypeRefAssignableCost(
    SLCBackendC* c, const SLTypeRef* dst, const SLTypeRef* src, uint8_t* outCost) {
    const SLFieldInfo* path[64];
    uint32_t           pathLen = 0;
    SLTypeRef          expandedSrc;
    uint8_t            expandedCost = 0;
    if (!dst->valid || !src->valid) {
        return -1;
    }
    if (TypeRefEqual(dst, src)) {
        *outCost = 0;
        return 0;
    }
    if (TypeRefIsFmtValueType(c, dst)) {
        *outCost = 5;
        return 0;
    }
    if (ExpandAliasSourceType(c, src, &expandedSrc)) {
        if (TypeRefAssignableCost(c, dst, &expandedSrc, &expandedCost) == 0) {
            if (expandedCost < 255u) {
                expandedCost = (uint8_t)(expandedCost + 1u);
            }
            *outCost = expandedCost;
            return 0;
        }
    }
    if (dst->isOptional && !src->isOptional) {
        SLTypeRef inner = *dst;
        inner.isOptional = 0;
        if (TypeRefAssignableCost(c, &inner, src, outCost) == 0) {
            *outCost = 4;
            return 0;
        }
        return -1;
    }
    if (dst->isOptional && src->isOptional) {
        SLTypeRef d = *dst;
        SLTypeRef s = *src;
        d.isOptional = 0;
        s.isOptional = 0;
        return TypeRefAssignableCost(c, &d, &s, outCost);
    }
    if (!dst->isOptional && src->isOptional) {
        return -1;
    }

    if (dst->containerKind == SLTypeContainer_SLICE_RO
        || dst->containerKind == SLTypeContainer_SLICE_MUT)
    {
        const char* srcBase = ResolveScalarAliasBaseName(c, src->baseName);
        if (srcBase == NULL) {
            srcBase = src->baseName;
        }
        if (src->containerKind == SLTypeContainer_SCALAR && src->containerPtrDepth == 0
            && src->ptrDepth > 0 && IsStrBaseName(srcBase) && dst->baseName != NULL
            && StrEq(dst->baseName, "__sl_u8"))
        {
            if (dst->containerKind == SLTypeContainer_SLICE_MUT && src->readOnly) {
                return -1;
            }
            *outCost = 1;
            return 0;
        }
        if ((src->containerKind == SLTypeContainer_SLICE_RO
             || src->containerKind == SLTypeContainer_SLICE_MUT)
            && dst->containerPtrDepth == src->containerPtrDepth && dst->ptrDepth == src->ptrDepth
            && dst->baseName != NULL && src->baseName != NULL
            && StrEq(dst->baseName, src->baseName))
        {
            if (dst->containerKind == SLTypeContainer_SLICE_RO
                && src->containerKind == SLTypeContainer_SLICE_MUT)
            {
                *outCost = 1;
            } else if (dst->containerKind == src->containerKind) {
                *outCost = 0;
            } else {
                return -1;
            }
            return 0;
        }
        if (src->containerKind == SLTypeContainer_ARRAY && dst->ptrDepth == src->ptrDepth
            && (dst->containerPtrDepth == src->containerPtrDepth
                || dst->containerPtrDepth == src->containerPtrDepth + 1)
            && dst->baseName != NULL && src->baseName != NULL
            && StrEq(dst->baseName, src->baseName))
        {
            *outCost = 1;
            return 0;
        }
        return -1;
    }

    if (dst->containerKind != src->containerKind || dst->containerPtrDepth != src->containerPtrDepth
        || dst->ptrDepth != src->ptrDepth)
    {
        return -1;
    }
    if (dst->baseName == NULL || src->baseName == NULL) {
        return -1;
    }
    if (StrEq(dst->baseName, src->baseName)) {
        if (dst->readOnly && !src->readOnly) {
            *outCost = 1;
        } else if (dst->readOnly == src->readOnly) {
            *outCost = 0;
        } else {
            return -1;
        }
        return 0;
    }

    if (ResolveEmbeddedPathByNames(
            c,
            src->baseName,
            dst->baseName,
            path,
            (uint32_t)(sizeof(path) / sizeof(path[0])),
            &pathLen)
            == 0
        && pathLen > 0)
    {
        *outCost = (uint8_t)(2u + (pathLen > 0 ? (pathLen - 1u) : 0u));
        return 0;
    }
    return -1;
}

int CostVecCmp(const uint8_t* a, const uint8_t* b, uint32_t len) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (a[i] < b[i]) {
            return -1;
        }
        if (a[i] > b[i]) {
            return 1;
        }
    }
    return 0;
}

int ExprNeedsExpectedType(const SLCBackendC* c, int32_t exprNode) {
    const SLAstNode* n = NodeAt(c, exprNode);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = AstFirstChild(&c->ast, exprNode);
        return ExprNeedsExpectedType(c, inner);
    }
    if (n->kind == SLAst_COMPOUND_LIT) {
        return 1;
    }
    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, exprNode);
        const SLAstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == SLAst_COMPOUND_LIT) {
            int32_t          rhsChild = AstFirstChild(&c->ast, rhsNode);
            const SLAstNode* rhsTypeNode = NodeAt(c, rhsChild);
            return !(rhsTypeNode != NULL && IsTypeNodeKind(rhsTypeNode->kind));
        }
    }
    return 0;
}

int32_t UnwrapCallArgExprNode(const SLCBackendC* c, int32_t argNode) {
    const SLAstNode* n = NodeAt(c, argNode);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == SLAst_CALL_ARG) {
        return AstFirstChild(&c->ast, argNode);
    }
    return argNode;
}

int CollectCallArgInfo(
    SLCBackendC*    c,
    int32_t         callNode,
    int32_t         calleeNode,
    int             includeReceiver,
    int32_t         receiverNode,
    SLCCallArgInfo* outArgs,
    SLTypeRef*      outArgTypes,
    uint32_t*       outArgCount) {
    int32_t  argNode = AstNextSibling(&c->ast, calleeNode);
    uint32_t argCount = 0;
    (void)callNode;
    if (includeReceiver) {
        if (argCount >= SLCCG_MAX_CALL_ARGS) {
            return -1;
        }
        outArgs[argCount].argNode = receiverNode;
        outArgs[argCount].exprNode = receiverNode;
        outArgs[argCount].explicitNameStart = 0;
        outArgs[argCount].explicitNameEnd = 0;
        outArgs[argCount].implicitNameStart = 0;
        outArgs[argCount].implicitNameEnd = 0;
        outArgs[argCount].spread = 0;
        outArgs[argCount]._reserved[0] = 0;
        outArgs[argCount]._reserved[1] = 0;
        outArgs[argCount]._reserved[2] = 0;
        if (!ExprNeedsExpectedType(c, receiverNode)
            && InferExprType(c, receiverNode, &outArgTypes[argCount]) != 0)
        {
            return -1;
        } else if (ExprNeedsExpectedType(c, receiverNode)) {
            TypeRefSetInvalid(&outArgTypes[argCount]);
        }
        argCount++;
    }
    while (argNode >= 0) {
        const SLAstNode* arg = NodeAt(c, argNode);
        int32_t          exprNode = UnwrapCallArgExprNode(c, argNode);
        if (argCount >= SLCCG_MAX_CALL_ARGS) {
            return -1;
        }
        if (arg == NULL || exprNode < 0) {
            return -1;
        }
        outArgs[argCount].argNode = argNode;
        outArgs[argCount].exprNode = exprNode;
        outArgs[argCount].explicitNameStart = 0;
        outArgs[argCount].explicitNameEnd = 0;
        outArgs[argCount].implicitNameStart = 0;
        outArgs[argCount].implicitNameEnd = 0;
        outArgs[argCount].spread =
            (uint8_t)(((arg->flags & SLAstFlag_CALL_ARG_SPREAD) != 0) ? 1 : 0);
        outArgs[argCount]._reserved[0] = 0;
        outArgs[argCount]._reserved[1] = 0;
        outArgs[argCount]._reserved[2] = 0;
        if (arg->dataEnd > arg->dataStart) {
            outArgs[argCount].explicitNameStart = arg->dataStart;
            outArgs[argCount].explicitNameEnd = arg->dataEnd;
        } else {
            const SLAstNode* expr = NodeAt(c, exprNode);
            if (expr != NULL && expr->kind == SLAst_IDENT) {
                outArgs[argCount].implicitNameStart = expr->dataStart;
                outArgs[argCount].implicitNameEnd = expr->dataEnd;
            }
        }
        if (!ExprNeedsExpectedType(c, exprNode)
            && InferExprType(c, exprNode, &outArgTypes[argCount]) != 0)
        {
            return -1;
        } else if (ExprNeedsExpectedType(c, exprNode)) {
            TypeRefSetInvalid(&outArgTypes[argCount]);
        }
        argCount++;
        argNode = AstNextSibling(&c->ast, argNode);
    }
    *outArgCount = argCount;
    return 0;
}

void GatherCallCandidatesBySlice(
    const SLCBackendC* c,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLFnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound) {
    const SLFnSig* candidates[SLCCG_MAX_CALL_CANDIDATES];
    const SLFnSig* byName[SLCCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    int            nameFound = 0;
    uint32_t       i, j;

    i = FindFnSigCandidatesBySlice(
        c, nameStart, nameEnd, byName, (uint32_t)(sizeof(byName) / sizeof(byName[0])));
    if (i > 0) {
        nameFound = 1;
        if (i > (uint32_t)(sizeof(byName) / sizeof(byName[0]))) {
            i = (uint32_t)(sizeof(byName) / sizeof(byName[0]));
        }
        for (j = 0; j < i && candidateLen < SLCCG_MAX_CALL_CANDIDATES; j++) {
            candidates[candidateLen++] = byName[j];
        }
    }
    for (i = 0; i < candidateLen; i++) {
        outCandidates[i] = candidates[i];
    }
    *outCandidateLen = candidateLen;
    *outNameFound = nameFound;
}

void GatherCallCandidatesByPkgMethod(
    const SLCBackendC* c,
    uint32_t           pkgStart,
    uint32_t           pkgEnd,
    uint32_t           methodStart,
    uint32_t           methodEnd,
    const SLFnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound) {
    const SLFnSig* candidates[SLCCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    int            nameFound = 0;
    uint32_t       i;
    for (i = 0; i < c->fnSigLen && candidateLen < SLCCG_MAX_CALL_CANDIDATES; i++) {
        if (NameEqPkgPrefixedMethod(
                c->fnSigs[i].slName, c->unit->source, pkgStart, pkgEnd, methodStart, methodEnd))
        {
            candidates[candidateLen++] = &c->fnSigs[i];
            nameFound = 1;
        }
    }
    for (i = 0; i < candidateLen; i++) {
        outCandidates[i] = candidates[i];
    }
    *outCandidateLen = candidateLen;
    *outNameFound = nameFound;
}

int MapCallArgsToParams(
    const SLCBackendC*    c,
    const SLFnSig*        sig,
    const SLCCallArgInfo* callArgs,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int32_t*              outMappedArgNodes,
    SLTypeRef*            outMappedArgTypes,
    const SLTypeRef*      argTypes) {
    uint8_t  assigned[SLCCG_MAX_CALL_ARGS];
    uint32_t i;
    if (argCount > sig->paramLen || argCount > SLCCG_MAX_CALL_ARGS) {
        return -1;
    }
    memset(assigned, 0, sizeof(assigned));
    for (i = 0; i < argCount; i++) {
        outMappedArgNodes[i] = -1;
        TypeRefSetInvalid(&outMappedArgTypes[i]);
    }
    if (firstPositionalArgIndex < argCount) {
        outMappedArgNodes[firstPositionalArgIndex] = callArgs[firstPositionalArgIndex].exprNode;
        outMappedArgTypes[firstPositionalArgIndex] = argTypes[firstPositionalArgIndex];
        assigned[firstPositionalArgIndex] = 1;
    }
    for (i = 0; i < argCount; i++) {
        const SLCCallArgInfo* a = &callArgs[i];
        uint32_t              nameStart = 0;
        uint32_t              nameEnd = 0;
        uint32_t              p;
        if (i < firstPositionalArgIndex) {
            outMappedArgNodes[i] = a->exprNode;
            outMappedArgTypes[i] = argTypes[i];
            assigned[i] = 1;
            continue;
        }
        if (i == firstPositionalArgIndex) {
            continue;
        }
        if (a->explicitNameEnd > a->explicitNameStart) {
            nameStart = a->explicitNameStart;
            nameEnd = a->explicitNameEnd;
        } else if (a->implicitNameEnd > a->implicitNameStart) {
            nameStart = a->implicitNameStart;
            nameEnd = a->implicitNameEnd;
        }
        if (!(nameEnd > nameStart)) {
            return -1;
        }
        for (p = firstPositionalArgIndex + 1u; p < argCount; p++) {
            const char* pn = (sig->paramNames != NULL) ? sig->paramNames[p] : NULL;
            if (pn == NULL || pn[0] == '\0') {
                continue;
            }
            if (SliceEqName(c->unit->source, nameStart, nameEnd, pn)) {
                if (assigned[p]) {
                    return -1;
                }
                assigned[p] = 1;
                outMappedArgNodes[p] = a->exprNode;
                outMappedArgTypes[p] = argTypes[i];
                break;
            }
        }
        if (p == argCount) {
            return -1;
        }
    }
    for (i = 0; i < argCount; i++) {
        if (!assigned[i]) {
            return -1;
        }
    }
    return 0;
}

int EmitResolvedCall(
    SLCBackendC*          c,
    int32_t               callNode,
    const char*           calleeName,
    const SLFnSig*        sig,
    const SLCCallBinding* binding,
    int                   autoRefFirstArg);

int PrepareCallBinding(
    const SLCBackendC*    c,
    const SLFnSig*        sig,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   allowNamedMapping,
    SLCCallBinding*       out) {
    uint32_t i;
    uint32_t spreadArgIndex = UINT32_MAX;
    uint32_t fixedCount;
    uint32_t fixedInputCount;
    if (sig == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->isVariadic = sig->isVariadic != 0;
    out->spreadArgIndex = UINT32_MAX;
    for (i = 0; i < SLCCG_MAX_CALL_ARGS; i++) {
        out->fixedMappedArgNodes[i] = -1;
        out->explicitTailNodes[i] = -1;
        out->argParamIndices[i] = -1;
        TypeRefSetInvalid(&out->argExpectedTypes[i]);
    }
    if (argCount > SLCCG_MAX_CALL_ARGS || sig->paramLen > SLCCG_MAX_CALL_ARGS) {
        return -1;
    }
    for (i = 0; i < argCount; i++) {
        if (!callArgs[i].spread) {
            continue;
        }
        if (i + 1u < argCount) {
            return -1;
        }
        spreadArgIndex = i;
    }

    fixedCount = out->isVariadic ? (sig->paramLen > 0 ? sig->paramLen - 1u : 0u) : sig->paramLen;
    out->fixedCount = fixedCount;
    if (!out->isVariadic) {
        if (spreadArgIndex != UINT32_MAX || argCount != sig->paramLen) {
            return -1;
        }
    } else {
        if (sig->paramLen == 0) {
            return -1;
        }
        if (spreadArgIndex != UINT32_MAX) {
            if (argCount != fixedCount + 1u) {
                return -1;
            }
            if (callArgs[spreadArgIndex].explicitNameEnd
                > callArgs[spreadArgIndex].explicitNameStart)
            {
                return -1;
            }
        } else if (argCount < fixedCount) {
            return -1;
        }
    }
    fixedInputCount = out->isVariadic ? fixedCount : argCount;
    out->fixedInputCount = fixedInputCount;
    out->spreadArgIndex = spreadArgIndex;

    if (fixedCount > 0) {
        if (!allowNamedMapping) {
            if (fixedInputCount > fixedCount) {
                return -1;
            }
            for (i = 0; i < fixedInputCount; i++) {
                out->fixedMappedArgNodes[i] = argNodes[i];
            }
        } else {
            SLFnSig fixedSig = *sig;
            fixedSig.paramLen = fixedCount;
            if (MapCallArgsToParams(
                    c,
                    &fixedSig,
                    callArgs,
                    fixedInputCount,
                    firstPositionalArgIndex,
                    out->fixedMappedArgNodes,
                    out->argExpectedTypes,
                    argTypes)
                != 0)
            {
                return -1;
            }
        }
    }

    for (i = 0; i < fixedInputCount; i++) {
        uint32_t p = UINT32_MAX;
        uint32_t j;
        if (!allowNamedMapping && i < fixedCount) {
            p = i;
        } else if (i < firstPositionalArgIndex && i < fixedCount) {
            p = i;
        } else if (
            i == firstPositionalArgIndex && i < fixedCount
            && out->fixedMappedArgNodes[i] == argNodes[i])
        {
            p = i;
        } else {
            for (j = 0; j < fixedCount; j++) {
                if (out->fixedMappedArgNodes[j] == argNodes[i]) {
                    p = j;
                    break;
                }
            }
        }
        if (p == UINT32_MAX) {
            return -1;
        }
        out->argParamIndices[i] = (int32_t)p;
        out->argExpectedTypes[i] = sig->paramTypes[p];
    }

    if (!out->isVariadic) {
        return 0;
    }

    if (spreadArgIndex != UINT32_MAX) {
        out->fixedMappedArgNodes[fixedCount] = argNodes[spreadArgIndex];
        out->argParamIndices[spreadArgIndex] = (int32_t)fixedCount;
        out->argExpectedTypes[spreadArgIndex] = sig->paramTypes[fixedCount];
        return 0;
    }

    {
        SLTypeRef elemType = sig->paramTypes[fixedCount];
        elemType.containerKind = SLTypeContainer_SCALAR;
        elemType.containerPtrDepth = 0;
        elemType.hasArrayLen = 0;
        elemType.arrayLen = 0;
        for (i = fixedInputCount; i < argCount; i++) {
            if (callArgs[i].explicitNameEnd > callArgs[i].explicitNameStart) {
                return -1;
            }
            out->argParamIndices[i] = (int32_t)fixedCount;
            out->argExpectedTypes[i] = elemType;
            out->explicitTailNodes[out->explicitTailCount++] = argNodes[i];
        }
    }
    return 0;
}

static int ConstParamArgsViable(
    SLCBackendC*          c,
    const SLFnSig*        sig,
    const int32_t*        argNodes,
    uint32_t              argCount,
    const SLCCallBinding* binding) {
    uint32_t i;
    if (c == NULL || sig == NULL || argNodes == NULL || binding == NULL || c->constEval == NULL
        || sig->paramFlags == NULL)
    {
        return 1;
    }
    for (i = 0; i < argCount; i++) {
        int32_t     p = binding->argParamIndices[i];
        int         isConst = 0;
        SLCTFEValue ignoredValue = { 0 };
        if (p < 0 || (uint32_t)p >= sig->paramLen) {
            continue;
        }
        if ((sig->paramFlags[p] & SLCCGParamFlag_CONST) == 0) {
            continue;
        }
        if (SLConstEvalSessionEvalExpr(c->constEval, argNodes[i], &ignoredValue, &isConst) != 0) {
            return 0;
        }
        if (!isConst) {
            return 0;
        }
    }
    return 1;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
int ResolveCallTargetFromCandidates(
    SLCBackendC*          c,
    const SLFnSig**       candidates,
    uint32_t              candidateLen,
    int                   nameFound,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    SLCCallBinding* _Nullable outBinding,
    const SLFnSig** outSig,
    const char**    outCalleeName) {
    const SLFnSig* bestSig = NULL;
    const char*    bestName = NULL;
    uint8_t        bestCosts[SLCCG_MAX_CALL_ARGS];
    SLCCallBinding bestBinding;
    uint32_t       bestTotal = 0;
    int            ambiguous = 0;
    SLTypeRef      autoRefType;
    int            hasAutoRefType = 0;
    uint32_t       i;
    memset(&bestBinding, 0, sizeof(bestBinding));

    if (!nameFound) {
        return 1;
    }
    if (autoRefFirstArg && argCount > 0) {
        if (argTypes[0].valid) {
            autoRefType = argTypes[0];
        } else if (InferExprType(c, argNodes[0], &autoRefType) != 0 || !autoRefType.valid) {
            autoRefFirstArg = 0;
        }
        if (autoRefFirstArg) {
            if (autoRefType.containerKind == SLTypeContainer_SCALAR) {
                autoRefType.ptrDepth++;
            } else {
                autoRefType.containerPtrDepth++;
            }
            hasAutoRefType = 1;
        }
    }

    for (i = 0; i < candidateLen; i++) {
        const SLFnSig* sig = candidates[i];
        uint8_t        costs[SLCCG_MAX_CALL_ARGS];
        SLCCallBinding binding;
        uint32_t       total = 0;
        uint32_t       p;
        int            viable = 1;
        int            cmp;
        if (PrepareCallBinding(
                c,
                sig,
                callArgs,
                argNodes,
                argTypes,
                argCount,
                firstPositionalArgIndex,
                1,
                &binding)
            != 0)
        {
            continue;
        }
        for (p = 0; p < argCount; p++) {
            SLTypeRef argType;
            uint8_t   cost = 0;
            SLTypeRef paramType = binding.argExpectedTypes[p];
            if (!paramType.valid) {
                viable = 0;
                break;
            }
            if (hasAutoRefType && p == 0) {
                argType = autoRefType;
            } else if (argTypes[p].valid) {
                argType = argTypes[p];
            } else {
                if (TypeRefIsFmtValueType(c, &paramType)) {
                    if (InferExprType(c, argNodes[p], &argType) != 0 || !argType.valid) {
                        viable = 0;
                        break;
                    }
                } else {
                    if (InferExprTypeExpected(c, argNodes[p], &paramType, &argType) != 0
                        || !argType.valid)
                    {
                        viable = 0;
                        break;
                    }
                }
            }
            if (TypeRefAssignableCost(c, &paramType, &argType, &cost) != 0) {
                viable = 0;
                break;
            }
            costs[p] = cost;
            total += cost;
        }
        if (viable && !ConstParamArgsViable(c, sig, argNodes, argCount, &binding)) {
            viable = 0;
        }
        if (!viable) {
            continue;
        }
        if (bestSig == NULL) {
            uint32_t j;
            bestSig = sig;
            bestName = sig->cName;
            bestTotal = total;
            ambiguous = 0;
            for (j = 0; j < argCount; j++) {
                bestCosts[j] = costs[j];
            }
            bestBinding = binding;
            continue;
        }
        cmp = CostVecCmp(costs, bestCosts, argCount);
        if (cmp < 0 || (cmp == 0 && total < bestTotal)) {
            uint32_t j;
            bestSig = sig;
            bestName = sig->cName;
            bestTotal = total;
            ambiguous = 0;
            for (j = 0; j < argCount; j++) {
                bestCosts[j] = costs[j];
            }
            bestBinding = binding;
            continue;
        }
        if (cmp == 0 && total == bestTotal) {
            ambiguous = 1;
        }
    }

    if (bestSig == NULL) {
        return 2;
    }
    if (ambiguous) {
        return 3;
    }
    if (outBinding != NULL) {
        *outBinding = bestBinding;
    }
    *outSig = bestSig;
    *outCalleeName = bestName;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
int ResolveCallTarget(
    SLCBackendC*          c,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    SLCCallBinding* _Nullable outBinding,
    const SLFnSig** outSig,
    const char**    outCalleeName) {
    const SLFnSig* candidates[SLCCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    int            nameFound = 0;
    uint32_t       adjustedFirstPositionalArgIndex = firstPositionalArgIndex;

    if (adjustedFirstPositionalArgIndex > argCount) {
        adjustedFirstPositionalArgIndex = argCount;
    }
    while (adjustedFirstPositionalArgIndex < argCount) {
        if (!(callArgs[adjustedFirstPositionalArgIndex].explicitNameEnd
              > callArgs[adjustedFirstPositionalArgIndex].explicitNameStart))
        {
            break;
        }
        adjustedFirstPositionalArgIndex++;
    }
    if (adjustedFirstPositionalArgIndex >= argCount) {
        adjustedFirstPositionalArgIndex = firstPositionalArgIndex;
    }

    GatherCallCandidatesBySlice(c, nameStart, nameEnd, candidates, &candidateLen, &nameFound);
    return ResolveCallTargetFromCandidates(
        c,
        candidates,
        candidateLen,
        nameFound,
        callArgs,
        argNodes,
        argTypes,
        argCount,
        adjustedFirstPositionalArgIndex,
        autoRefFirstArg,
        outBinding,
        outSig,
        outCalleeName);
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
int ResolveCallTargetByPkgMethod(
    SLCBackendC*          c,
    uint32_t              pkgStart,
    uint32_t              pkgEnd,
    uint32_t              methodStart,
    uint32_t              methodEnd,
    const SLCCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const SLTypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    SLCCallBinding* _Nullable outBinding,
    const SLFnSig** outSig,
    const char**    outCalleeName) {
    const SLFnSig* candidates[SLCCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    int            nameFound = 0;
    uint32_t       adjustedFirstPositionalArgIndex = firstPositionalArgIndex;

    if (adjustedFirstPositionalArgIndex > argCount) {
        adjustedFirstPositionalArgIndex = argCount;
    }
    while (adjustedFirstPositionalArgIndex < argCount) {
        if (!(callArgs[adjustedFirstPositionalArgIndex].explicitNameEnd
              > callArgs[adjustedFirstPositionalArgIndex].explicitNameStart))
        {
            break;
        }
        adjustedFirstPositionalArgIndex++;
    }
    if (adjustedFirstPositionalArgIndex >= argCount) {
        adjustedFirstPositionalArgIndex = firstPositionalArgIndex;
    }

    GatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, methodStart, methodEnd, candidates, &candidateLen, &nameFound);
    return ResolveCallTargetFromCandidates(
        c,
        candidates,
        candidateLen,
        nameFound,
        callArgs,
        argNodes,
        argTypes,
        argCount,
        adjustedFirstPositionalArgIndex,
        autoRefFirstArg,
        outBinding,
        outSig,
        outCalleeName);
}

int InferExprType_IDENT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType);

int InferCompoundLiteralType(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType) {
    const SLAstNode*      litNode = NodeAt(c, nodeId);
    int32_t               child;
    int32_t               firstField;
    int                   hasExplicitType;
    SLTypeRef             explicitType;
    SLTypeRef             targetValueType;
    SLTypeRef             resultType;
    const char*           ownerType = NULL;
    const SLNameMap*      ownerMap;
    const SLAnonTypeInfo* anonOwner = NULL;
    int                   isUnion = 0;
    int                   isEnumVariantLiteral = 0;
    uint32_t              enumVariantStart = 0;
    uint32_t              enumVariantEnd = 0;
    uint32_t              explicitFieldCount = 0;
    uint8_t               cost = 0;

    if (litNode == NULL || litNode->kind != SLAst_COMPOUND_LIT) {
        TypeRefSetInvalid(outType);
        return -1;
    }

    child = AstFirstChild(&c->ast, nodeId);
    hasExplicitType =
        child >= 0 && NodeAt(c, child) != NULL && IsTypeNodeKind(NodeAt(c, child)->kind);
    firstField = hasExplicitType ? AstNextSibling(&c->ast, child) : child;

    TypeRefSetInvalid(&explicitType);
    TypeRefSetInvalid(&targetValueType);
    TypeRefSetInvalid(&resultType);

    if (hasExplicitType) {
        const char* enumTypeName = NULL;
        int         variantRc = ResolveEnumVariantTypeNameNode(
            c, child, &enumTypeName, &enumVariantStart, &enumVariantEnd);
        if (variantRc < 0) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        if (variantRc == 1) {
            if (enumTypeName == NULL) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            TypeRefSetScalar(&explicitType, enumTypeName);
            isEnumVariantLiteral = 1;
        } else if (ParseTypeRef(c, child, &explicitType) != 0 || !explicitType.valid) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        targetValueType = explicitType;
        resultType = explicitType;
    } else {
        if (expectedType == NULL || !expectedType->valid) {
            const char* fieldNames[256];
            SLTypeRef   fieldTypes[256];
            uint32_t    fieldCount = 0;
            int32_t     scan = firstField;
            const char* anonName;
            while (scan >= 0) {
                const SLAstNode* fieldNode = NodeAt(c, scan);
                SLTypeRef        exprType;
                int32_t          exprNode;
                uint32_t         i;
                if (fieldNode == NULL || fieldNode->kind != SLAst_COMPOUND_FIELD) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (fieldCount >= (uint32_t)(sizeof(fieldNames) / sizeof(fieldNames[0]))) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                for (i = 0; i < fieldCount; i++) {
                    if (SliceEqName(
                            c->unit->source,
                            fieldNode->dataStart,
                            fieldNode->dataEnd,
                            fieldNames[i]))
                    {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                }
                exprNode = AstFirstChild(&c->ast, scan);
                if (exprNode < 0) {
                    if ((fieldNode->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    {
                        SLAstNode identNode = { 0 };
                        identNode.kind = SLAst_IDENT;
                        identNode.start = fieldNode->start;
                        identNode.end = fieldNode->end;
                        identNode.dataStart = fieldNode->dataStart;
                        identNode.dataEnd = fieldNode->dataEnd;
                        if (InferExprType_IDENT(c, scan, &identNode, &exprType) != 0
                            || !exprType.valid)
                        {
                            TypeRefSetInvalid(outType);
                            return -1;
                        }
                    }
                } else if (InferExprType(c, exprNode, &exprType) != 0 || !exprType.valid) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                CanonicalizeTypeRefBaseName(c, &exprType);
                fieldNames[fieldCount] = DupSlice(
                    c, c->unit->source, fieldNode->dataStart, fieldNode->dataEnd);
                if (fieldNames[fieldCount] == NULL) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                fieldTypes[fieldCount++] = exprType;
                scan = AstNextSibling(&c->ast, scan);
            }
            if (EnsureAnonTypeByFields(c, 0, fieldNames, fieldTypes, fieldCount, &anonName) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            TypeRefSetScalar(&targetValueType, anonName);
            resultType = targetValueType;
        } else {
            targetValueType = *expectedType;
            if (targetValueType.containerKind != SLTypeContainer_SCALAR) {
                if (targetValueType.containerPtrDepth > 0) {
                    targetValueType.containerPtrDepth--;
                }
            } else if (targetValueType.ptrDepth > 0) {
                targetValueType.ptrDepth--;
            }
            resultType = *expectedType;
        }
    }

    if (!targetValueType.valid || targetValueType.containerKind != SLTypeContainer_SCALAR
        || targetValueType.containerPtrDepth != 0 || targetValueType.ptrDepth != 0
        || targetValueType.baseName == NULL || targetValueType.isOptional)
    {
        TypeRefSetInvalid(outType);
        return -1;
    }

    ownerType = ResolveScalarAliasBaseName(c, targetValueType.baseName);
    if (ownerType == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    ownerMap = FindNameByCName(c, ownerType);
    anonOwner = FindAnonTypeByCName(c, ownerType);
    if ((ownerMap == NULL
         || (ownerMap->kind != SLAst_STRUCT && ownerMap->kind != SLAst_UNION
             && ownerMap->kind != SLAst_ENUM))
        && anonOwner == NULL)
    {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (ownerMap != NULL && ownerMap->kind == SLAst_ENUM) {
        int32_t enumNodeId = -1;
        if (!isEnumVariantLiteral || FindEnumDeclNodeByCName(c, ownerType, &enumNodeId) != 0
            || !EnumDeclHasPayload(c, enumNodeId))
        {
            TypeRefSetInvalid(outType);
            return -1;
        }
        isUnion = 0;
    } else if (ownerMap != NULL) {
        isUnion = ownerMap->kind == SLAst_UNION;
    } else {
        isUnion = anonOwner->isUnion;
    }

    while (firstField >= 0) {
        const SLAstNode* fieldNode = NodeAt(c, firstField);
        int32_t          exprNode;
        int32_t          scan;
        SLTypeRef        exprType;
        SLTypeRef        fieldType;

        if (fieldNode == NULL || fieldNode->kind != SLAst_COMPOUND_FIELD) {
            TypeRefSetInvalid(outType);
            return -1;
        }

        scan = hasExplicitType ? AstNextSibling(&c->ast, child) : child;
        while (scan >= 0 && scan != firstField) {
            const SLAstNode* prevField = NodeAt(c, scan);
            if (prevField != NULL && prevField->kind == SLAst_COMPOUND_FIELD
                && SliceSpanEq(
                    c->unit->source,
                    prevField->dataStart,
                    prevField->dataEnd,
                    fieldNode->dataStart,
                    fieldNode->dataEnd))
            {
                TypeRefSetInvalid(outType);
                return -1;
            }
            scan = AstNextSibling(&c->ast, scan);
        }

        exprNode = AstFirstChild(&c->ast, firstField);
        if (exprNode < 0 && (fieldNode->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
            TypeRefSetInvalid(outType);
            return -1;
        }

        if (isEnumVariantLiteral) {
            if (ResolveEnumVariantPayloadFieldType(
                    c,
                    ownerType,
                    enumVariantStart,
                    enumVariantEnd,
                    fieldNode->dataStart,
                    fieldNode->dataEnd,
                    &fieldType)
                != 0)
            {
                TypeRefSetInvalid(outType);
                return -1;
            }
        } else {
            const SLFieldInfo* fieldPath[64];
            const SLFieldInfo* field = NULL;
            uint32_t           fieldPathLen = 0;
            if (ResolveFieldPathBySlice(
                    c,
                    ownerType,
                    fieldNode->dataStart,
                    fieldNode->dataEnd,
                    fieldPath,
                    (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                    &fieldPathLen,
                    &field)
                    != 0
                || fieldPathLen == 0)
            {
                TypeRefSetInvalid(outType);
                return -1;
            }
            field = fieldPath[fieldPathLen - 1u];
            fieldType = field->type;
        }
        if (exprNode < 0) {
            SLAstNode identNode = { 0 };
            identNode.kind = SLAst_IDENT;
            identNode.start = fieldNode->start;
            identNode.end = fieldNode->end;
            identNode.dataStart = fieldNode->dataStart;
            identNode.dataEnd = fieldNode->dataEnd;
            if (InferExprType_IDENT(c, firstField, &identNode, &exprType) != 0 || !exprType.valid) {
                TypeRefSetInvalid(outType);
                return -1;
            }
        } else if (
            InferExprTypeExpected(c, exprNode, &fieldType, &exprType) != 0 || !exprType.valid)
        {
            TypeRefSetInvalid(outType);
            return -1;
        }
        if (TypeRefAssignableCost(c, &fieldType, &exprType, &cost) != 0) {
            const SLAstNode* expr = NodeAt(c, exprNode);
            const char*      dstBase =
                fieldType.baseName != NULL
                         ? ResolveScalarAliasBaseName(c, fieldType.baseName)
                         : NULL;
            if (dstBase == NULL) {
                dstBase = fieldType.baseName;
            }
            if (!(expr != NULL && expr->kind == SLAst_INT && dstBase != NULL
                  && (IsIntegerCTypeName(dstBase) || IsFloatCTypeName(dstBase)))
                && !(
                    expr != NULL && expr->kind == SLAst_FLOAT && dstBase != NULL
                    && IsFloatCTypeName(dstBase)))
            {
                TypeRefSetInvalid(outType);
                return -1;
            }
        }
        explicitFieldCount++;
        firstField = AstNextSibling(&c->ast, firstField);
    }

    if (isUnion && explicitFieldCount > 1u) {
        TypeRefSetInvalid(outType);
        return -1;
    }

    if (hasExplicitType && expectedType != NULL && expectedType->valid) {
        SLTypeRef expectedValueType = *expectedType;
        if (expectedValueType.containerKind != SLTypeContainer_SCALAR) {
            if (expectedValueType.containerPtrDepth > 0) {
                expectedValueType.containerPtrDepth--;
            }
        } else if (expectedValueType.ptrDepth > 0) {
            expectedValueType.ptrDepth--;
        }
        if (expectedValueType.valid && expectedValueType.containerKind == SLTypeContainer_SCALAR
            && expectedValueType.containerPtrDepth == 0
            && TypeRefAssignableCost(c, &expectedValueType, &explicitType, &cost) == 0)
        {
            *outType = *expectedType;
            return 0;
        }
    }

    *outType = resultType;
    return 0;
}

int InferExprTypeExpected(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }

    if (n->kind == SLAst_COMPOUND_LIT) {
        return InferCompoundLiteralType(c, nodeId, expectedType, outType);
    }

    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, nodeId);
        const SLAstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == SLAst_COMPOUND_LIT) {
            SLTypeRef rhsExpected;
            SLTypeRef rhsType;
            int       haveExpected = 0;
            if (expectedType != NULL && expectedType->valid) {
                rhsExpected = *expectedType;
                if (rhsExpected.containerKind != SLTypeContainer_SCALAR) {
                    if (rhsExpected.containerPtrDepth > 0) {
                        rhsExpected.containerPtrDepth--;
                        haveExpected = 1;
                    }
                } else if (rhsExpected.ptrDepth > 0) {
                    rhsExpected.ptrDepth--;
                    haveExpected = 1;
                }
            }
            if (InferExprTypeExpected(c, rhsNode, haveExpected ? &rhsExpected : NULL, &rhsType)
                != 0)
            {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (!rhsType.valid) {
                TypeRefSetScalar(outType, "void");
                outType->ptrDepth = 1;
                return 0;
            }
            if (rhsType.containerKind == SLTypeContainer_SCALAR) {
                rhsType.ptrDepth++;
            } else {
                rhsType.containerPtrDepth++;
            }
            *outType = rhsType;
            return 0;
        }
    }

    if (expectedType != NULL && expectedType->valid
        && expectedType->containerKind == SLTypeContainer_SCALAR && expectedType->ptrDepth == 0
        && expectedType->containerPtrDepth == 0)
    {
        SLTypeRef   srcType;
        const char* dstBase = ResolveScalarAliasBaseName(c, expectedType->baseName);
        if (dstBase == NULL) {
            dstBase = expectedType->baseName;
        }
        if (dstBase != NULL && InferExprType(c, nodeId, &srcType) == 0 && srcType.valid
            && srcType.containerKind == SLTypeContainer_SCALAR && srcType.ptrDepth == 0
            && srcType.containerPtrDepth == 0 && !srcType.isOptional)
        {
            const char* srcBase = ResolveScalarAliasBaseName(c, srcType.baseName);
            int64_t     intValue = 0;
            double      floatValue = 0.0;
            int         isConst = 0;
            if (srcBase == NULL) {
                srcBase = srcType.baseName;
            }
            if (IsIntegerCTypeName(dstBase)) {
                if (srcBase != NULL && StrEq(srcBase, "__sl_int")) {
                    if (EvalConstIntExpr(c, nodeId, &intValue, &isConst) != 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    if (!isConst) {
                        *outType = srcType;
                        return 0;
                    }
                    if (!ConstIntFitsIntegerType(dstBase, intValue)) {
                        SetDiagNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                        return -1;
                    }
                    *outType = *expectedType;
                    return 0;
                }
                if (TypeRefIsRuneLike(c, &srcType)) {
                    if (EvalConstIntExpr(c, nodeId, &intValue, &isConst) != 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    if (!isConst) {
                        *outType = srcType;
                        return 0;
                    }
                    if (!ConstIntFitsIntegerType(dstBase, intValue)) {
                        SetDiagNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                        return -1;
                    }
                    *outType = *expectedType;
                    return 0;
                }
            }
            if (IsFloatCTypeName(dstBase)) {
                if (srcBase != NULL && StrEq(srcBase, "__sl_int")) {
                    if (EvalConstIntExpr(c, nodeId, &intValue, &isConst) != 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    if (!isConst) {
                        *outType = srcType;
                        return 0;
                    }
                    if (!ConstIntFitsFloatType(dstBase, intValue)) {
                        SetDiagNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                        return -1;
                    }
                    *outType = *expectedType;
                    return 0;
                }
                if (srcBase != NULL && StrEq(srcBase, "__sl_f64")) {
                    if (EvalConstFloatExpr(c, nodeId, &floatValue, &isConst) != 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    if (!isConst) {
                        *outType = srcType;
                        return 0;
                    }
                    if (!ConstFloatFitsFloatType(dstBase, floatValue)) {
                        SetDiagNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                        return -1;
                    }
                    *outType = *expectedType;
                    return 0;
                }
            }
        }
    }

    return InferExprType(c, nodeId, outType);
}

int InferExprType_IDENT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    const SLLocal* local = FindLocalBySlice(c, n->dataStart, n->dataEnd);
    (void)nodeId;
    if (local != NULL) {
        *outType = local->type;
        return 0;
    }
    {
        const SLFnSig* sig = FindFnSigBySlice(c, n->dataStart, n->dataEnd);
        if (sig != NULL) {
            const char* aliasName;
            if (EnsureFnTypeAlias(
                    c,
                    sig->returnType,
                    sig->paramTypes,
                    sig->paramLen,
                    sig->isVariadic != 0,
                    &aliasName)
                != 0)
            {
                return -1;
            }
            TypeRefSetScalar(outType, aliasName);
            return 0;
        }
    }
    {
        int32_t topVarLikeNode = -1;
        if (FindTopLevelVarLikeNodeBySlice(c, n->dataStart, n->dataEnd, &topVarLikeNode) == 0) {
            return InferTopLevelVarLikeType(c, topVarLikeNode, n->dataStart, n->dataEnd, outType);
        }
    }
    {
        SLTypeRef typeValue;
        if (ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, &typeValue)) {
            TypeRefSetScalar(outType, "__sl_type");
            return 0;
        }
    }
    TypeRefSetInvalid(outType);
    return 0;
}

int InferExprType_COMPOUND_LIT(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)n;
    return InferCompoundLiteralType(c, nodeId, NULL, outType);
}

int InferExprType_CALL_WITH_CONTEXT(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t savedActive = c->activeCallWithNode;
    int32_t callNode = AstFirstChild(&c->ast, nodeId);
    int     rc;
    (void)n;
    if (callNode < 0) {
        TypeRefSetInvalid(outType);
        return 0;
    }
    c->activeCallWithNode = nodeId;
    rc = InferExprType(c, callNode, outType);
    c->activeCallWithNode = savedActive;
    return rc;
}

int InferExprType_CALL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t          callee = AstFirstChild(&c->ast, nodeId);
    const SLAstNode* cn = NodeAt(c, callee);
    (void)n;
    if (cn != NULL && cn->kind == SLAst_IDENT) {
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "kind")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                const char* kindTypeName;
                SLTypeRef   argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    kindTypeName = FindReflectKindTypeName(c);
                    TypeRefSetScalar(outType, kindTypeName != NULL ? kindTypeName : "__sl_u8");
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "base")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                SLTypeRef argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    TypeRefSetScalar(outType, "__sl_type");
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "is_alias")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                SLTypeRef argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    TypeRefSetScalar(outType, "__sl_bool");
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "type_name")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                SLTypeRef argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    TypeRefSetScalar(outType, "__sl_str");
                    outType->ptrDepth = 1;
                    outType->readOnly = 1;
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "typeof")) {
            int32_t   argNode = AstNextSibling(&c->ast, callee);
            int32_t   extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            SLTypeRef argType;
            if (argNode < 0 || extraNode >= 0) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (InferExprType(c, argNode, &argType) != 0 || !argType.valid) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            TypeRefSetScalar(outType, "__sl_type");
            return 0;
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "ptr")
            || SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "slice"))
        {
            int32_t   argNode = AstNextSibling(&c->ast, callee);
            int32_t   extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            SLTypeRef argType;
            if (argNode < 0 || extraNode >= 0) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (InferExprType(c, argNode, &argType) != 0 || !argType.valid) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (TypeRefIsTypeValue(&argType)) {
                TypeRefSetScalar(outType, "__sl_type");
                return 0;
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "array")) {
            int32_t     typeArgNode = AstNextSibling(&c->ast, callee);
            int32_t     lenArgNode = typeArgNode >= 0 ? AstNextSibling(&c->ast, typeArgNode) : -1;
            int32_t     extraNode = lenArgNode >= 0 ? AstNextSibling(&c->ast, lenArgNode) : -1;
            SLTypeRef   typeArgType;
            SLTypeRef   lenArgType;
            const char* lenBaseName;
            if (typeArgNode < 0 || lenArgNode < 0 || extraNode >= 0) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (InferExprType(c, typeArgNode, &typeArgType) != 0 || !typeArgType.valid
                || InferExprType(c, lenArgNode, &lenArgType) != 0 || !lenArgType.valid)
            {
                TypeRefSetInvalid(outType);
                return 0;
            }
            lenBaseName = ResolveScalarAliasBaseName(c, lenArgType.baseName);
            if (lenBaseName == NULL) {
                lenBaseName = lenArgType.baseName;
            }
            if (TypeRefIsTypeValue(&typeArgType)
                && lenArgType.containerKind == SLTypeContainer_SCALAR && lenArgType.ptrDepth == 0
                && lenArgType.containerPtrDepth == 0 && !lenArgType.isOptional
                && lenBaseName != NULL && IsIntegerCTypeName(lenBaseName))
            {
                TypeRefSetScalar(outType, "__sl_type");
                return 0;
            }
        }
        SLCCallArgInfo callArgs[SLCCG_MAX_CALL_ARGS];
        int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
        SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
        uint32_t       argCount = 0;
        uint32_t       i;
        const SLFnSig* resolved = NULL;
        const char*    resolvedName = NULL;
        int            status;
        if (CollectCallArgInfo(c, nodeId, callee, 0, -1, callArgs, argTypes, &argCount) == 0) {
            for (i = 0; i < argCount; i++) {
                argNodes[i] = callArgs[i].exprNode;
            }
            status = ResolveCallTarget(
                c,
                cn->dataStart,
                cn->dataEnd,
                callArgs,
                argNodes,
                argTypes,
                argCount,
                0,
                0,
                NULL,
                &resolved,
                &resolvedName);
            if (status == 2) {
                status = ResolveCallTarget(
                    c,
                    cn->dataStart,
                    cn->dataEnd,
                    callArgs,
                    argNodes,
                    argTypes,
                    argCount,
                    0,
                    1,
                    NULL,
                    &resolved,
                    &resolvedName);
            }
            if (status == 0 && resolved != NULL) {
                (void)resolvedName;
                *outType = resolved->returnType;
                return 0;
            }
        }
    } else if (cn != NULL && cn->kind == SLAst_FIELD_EXPR) {
        int32_t            recvNode = AstFirstChild(&c->ast, callee);
        SLTypeRef          recvType;
        const SLFieldInfo* fieldPath[64];
        uint32_t           fieldPathLen = 0;
        const SLFieldInfo* field = NULL;
        if (recvNode >= 0 && InferExprType(c, recvNode, &recvType) == 0 && recvType.valid) {
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "kind")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    const char* kindTypeName = FindReflectKindTypeName(c);
                    TypeRefSetScalar(outType, kindTypeName != NULL ? kindTypeName : "__sl_u8");
                    return 0;
                }
            }
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "base")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    TypeRefSetScalar(outType, "__sl_type");
                    return 0;
                }
            }
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "is_alias")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    TypeRefSetScalar(outType, "__sl_bool");
                    return 0;
                }
            }
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "type_name")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    TypeRefSetScalar(outType, "__sl_str");
                    outType->ptrDepth = 1;
                    outType->readOnly = 1;
                    return 0;
                }
            }
            if (recvType.containerKind != SLTypeContainer_SCALAR && recvType.containerPtrDepth > 0)
            {
                recvType.containerPtrDepth--;
            } else if (recvType.ptrDepth > 0) {
                recvType.ptrDepth--;
            }
            if (recvType.baseName != NULL
                && ResolveFieldPathBySlice(
                       c,
                       recvType.baseName,
                       cn->dataStart,
                       cn->dataEnd,
                       fieldPath,
                       (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                       &fieldPathLen,
                       &field)
                       == 0
                && fieldPathLen > 0)
            {
                field = fieldPath[fieldPathLen - 1u];
            } else {
                field = NULL;
            }
        }
        if (field == NULL && recvNode >= 0) {
            if (recvType.valid && recvType.containerKind == SLTypeContainer_SCALAR
                && recvType.ptrDepth == 0 && recvType.containerPtrDepth == 0
                && recvType.baseName != NULL
                && SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "impl")
                && StrEq(recvType.baseName, "__sl_Allocator"))
            {
                TypeRefSetScalar(outType, "__sl_uint");
                return 0;
            }
            if (recvType.valid && recvType.containerKind == SLTypeContainer_SCALAR
                && recvType.ptrDepth == 0 && recvType.containerPtrDepth == 0
                && recvType.baseName != NULL
                && SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "handler")
                && StrEq(recvType.baseName, "__sl_Logger"))
            {
                TypeRefSetScalar(outType, "void");
                return 0;
            }
            SLCCallArgInfo callArgs[SLCCG_MAX_CALL_ARGS];
            int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
            SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
            uint32_t       argCount = 0;
            uint32_t       i;
            const SLFnSig* resolved = NULL;
            const char*    resolvedName = NULL;
            uint32_t       recvPkgLen = 0;
            int            recvHasPkgPrefix =
                recvType.baseName != NULL && TypeNamePkgPrefixLen(recvType.baseName, &recvPkgLen);
            if (CollectCallArgInfo(c, nodeId, callee, 1, recvNode, callArgs, argTypes, &argCount)
                == 0)
            {
                for (i = 0; i < argCount; i++) {
                    argNodes[i] = callArgs[i].exprNode;
                }
                int status = ResolveCallTarget(
                    c,
                    cn->dataStart,
                    cn->dataEnd,
                    callArgs,
                    argNodes,
                    argTypes,
                    argCount,
                    1,
                    0,
                    NULL,
                    &resolved,
                    &resolvedName);
                if (status == 2) {
                    status = ResolveCallTarget(
                        c,
                        cn->dataStart,
                        cn->dataEnd,
                        callArgs,
                        argNodes,
                        argTypes,
                        argCount,
                        1,
                        1,
                        NULL,
                        &resolved,
                        &resolvedName);
                }
                if ((status == 1 || status == 2) && recvHasPkgPrefix) {
                    int prefixedStatus = ResolveCallTargetByPkgMethod(
                        c,
                        0,
                        recvPkgLen,
                        cn->dataStart,
                        cn->dataEnd,
                        callArgs,
                        argNodes,
                        argTypes,
                        argCount,
                        1,
                        0,
                        NULL,
                        &resolved,
                        &resolvedName);
                    if (prefixedStatus == 2) {
                        prefixedStatus = ResolveCallTargetByPkgMethod(
                            c,
                            0,
                            recvPkgLen,
                            cn->dataStart,
                            cn->dataEnd,
                            callArgs,
                            argNodes,
                            argTypes,
                            argCount,
                            1,
                            1,
                            NULL,
                            &resolved,
                            &resolvedName);
                    }
                    if (prefixedStatus == 0) {
                        status = 0;
                    }
                }
                if (status == 0 && resolved != NULL) {
                    (void)resolvedName;
                    *outType = resolved->returnType;
                    return 0;
                }
            }
        } else if (
            field != NULL && field->type.valid
            && field->type.containerKind == SLTypeContainer_SCALAR && field->type.ptrDepth == 0
            && field->type.containerPtrDepth == 0 && field->type.baseName != NULL)
        {
            const SLFnTypeAlias* alias = FindFnTypeAliasByName(c, field->type.baseName);
            if (alias != NULL) {
                *outType = alias->returnType;
                return 0;
            }
        }
    }
    {
        SLTypeRef            calleeType;
        const SLFnTypeAlias* alias = NULL;
        if (InferExprType(c, callee, &calleeType) == 0 && calleeType.valid
            && calleeType.containerKind == SLTypeContainer_SCALAR && calleeType.ptrDepth == 0
            && calleeType.containerPtrDepth == 0 && calleeType.baseName != NULL
            && !calleeType.isOptional)
        {
            alias = FindFnTypeAliasByName(c, calleeType.baseName);
        }
        if (alias != NULL) {
            *outType = alias->returnType;
            return 0;
        }
    }
    TypeRefSetInvalid(outType);
    return 0;
}

int InferExprType_NEW(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)n;
    return InferNewExprType(c, nodeId, outType);
}

int InferExprType_UNARY(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    if (InferExprType(c, child, outType) != 0) {
        return -1;
    }
    if ((SLTokenKind)n->op == SLTok_AND) {
        if (!outType->valid) {
            TypeRefSetScalar(outType, "void");
            outType->ptrDepth = 1;
            return 0;
        }
        if (outType->containerKind == SLTypeContainer_SCALAR) {
            outType->ptrDepth++;
        } else {
            outType->containerPtrDepth++;
        }
    } else if ((SLTokenKind)n->op == SLTok_MUL) {
        if (outType->valid && outType->containerKind != SLTypeContainer_SCALAR
            && outType->containerPtrDepth > 0)
        {
            outType->containerPtrDepth--;
        } else if (outType->valid && outType->ptrDepth > 0) {
            outType->ptrDepth--;
        } else {
            TypeRefSetInvalid(outType);
        }
    }
    return 0;
}

int InferExprType_FIELD_EXPR(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    const SLNameMap*       enumMap = NULL;
    int32_t                recv = AstFirstChild(&c->ast, nodeId);
    const SLAstNode*       recvNode = NodeAt(c, recv);
    const SLVariantNarrow* narrow = NULL;
    int32_t                recvLocalIdx = -1;
    SLTypeRef              recvType;
    const SLFieldInfo*     fieldPath[64];
    uint32_t               fieldPathLen = 0;
    const SLFieldInfo*     field = NULL;
    SLTypeRef              narrowFieldType;
    if (ResolveEnumSelectorByFieldExpr(c, nodeId, &enumMap, NULL, NULL, NULL, NULL) != 0
        && enumMap != NULL)
    {
        TypeRefSetScalar(outType, enumMap->cName);
        return 0;
    }
    if (recvNode != NULL && recvNode->kind == SLAst_IDENT) {
        recvLocalIdx = FindLocalIndexBySlice(c, recvNode->dataStart, recvNode->dataEnd);
        if (recvLocalIdx >= 0) {
            narrow = FindVariantNarrowByLocalIdx(c, recvLocalIdx);
        }
    }
    if (narrow != NULL
        && ResolveEnumVariantPayloadFieldType(
               c,
               narrow->enumTypeName,
               narrow->variantStart,
               narrow->variantEnd,
               n->dataStart,
               n->dataEnd,
               &narrowFieldType)
               == 0)
    {
        *outType = narrowFieldType;
        return 0;
    }
    if (InferExprType(c, recv, &recvType) != 0 || !recvType.valid) {
        TypeRefSetInvalid(outType);
        return 0;
    }
    if (recvType.containerKind != SLTypeContainer_SCALAR && recvType.containerPtrDepth > 0) {
        recvType.containerPtrDepth--;
    } else if (recvType.ptrDepth > 0) {
        recvType.ptrDepth--;
    }
    if (recvType.baseName != NULL
        && ResolveFieldPathBySlice(
               c,
               recvType.baseName,
               n->dataStart,
               n->dataEnd,
               fieldPath,
               (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
               &fieldPathLen,
               &field)
               == 0
        && fieldPathLen > 0)
    {
        field = fieldPath[fieldPathLen - 1u];
    } else {
        field = NULL;
    }
    if (field != NULL) {
        *outType = field->type;
        return 0;
    }
    TypeRefSetInvalid(outType);
    return 0;
}

int InferExprType_INDEX(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t base = AstFirstChild(&c->ast, nodeId);
    if (InferExprType(c, base, outType) != 0) {
        return -1;
    }
    if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
        if (!outType->valid) {
            TypeRefSetInvalid(outType);
            return 0;
        }
        if (outType->containerKind == SLTypeContainer_ARRAY) {
            outType->containerKind =
                outType->readOnly ? SLTypeContainer_SLICE_RO : SLTypeContainer_SLICE_MUT;
            outType->containerPtrDepth = 1;
            outType->readOnly = outType->containerKind == SLTypeContainer_SLICE_RO;
        } else if (
            outType->containerKind == SLTypeContainer_SLICE_RO
            || outType->containerKind == SLTypeContainer_SLICE_MUT)
        {
            outType->containerPtrDepth = 1;
            outType->readOnly = outType->containerKind == SLTypeContainer_SLICE_RO;
        } else {
            TypeRefSetInvalid(outType);
        }
        outType->hasArrayLen = 0;
        outType->arrayLen = 0;
        return 0;
    }
    if (!outType->valid) {
        return 0;
    }
    if (outType->containerKind == SLTypeContainer_ARRAY
        || outType->containerKind == SLTypeContainer_SLICE_RO
        || outType->containerKind == SLTypeContainer_SLICE_MUT)
    {
        outType->containerKind = SLTypeContainer_SCALAR;
        outType->containerPtrDepth = 0;
        outType->hasArrayLen = 0;
        outType->arrayLen = 0;
        outType->readOnly = 0;
    } else if (outType->ptrDepth > 0) {
        outType->ptrDepth--;
    }
    return 0;
}

int InferExprType_CAST(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t expr = AstFirstChild(&c->ast, nodeId);
    int32_t typeNode = AstNextSibling(&c->ast, expr);
    (void)n;
    return ParseTypeRef(c, typeNode, outType);
}

int InferExprType_SIZEOF(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__sl_uint");
    return 0;
}

int InferExprType_STRING(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__sl_str");
    outType->ptrDepth = 1;
    outType->readOnly = 1;
    return 0;
}

int InferExprType_BOOL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__sl_bool");
    return 0;
}

int InferExprType_INT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__sl_int");
    return 0;
}

int InferExprType_RUNE(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__sl_int");
    return 0;
}

int InferExprType_FLOAT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__sl_f64");
    return 0;
}

int InferExprType_NULL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetInvalid(outType);
    return 0;
}

int InferExprType_UNWRAP(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t inner = AstFirstChild(&c->ast, nodeId);
    (void)n;
    if (InferExprType(c, inner, outType) != 0) {
        return -1;
    }
    outType->isOptional = 0;
    return 0;
}

int InferExprType_TUPLE_EXPR(
    SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t     child = AstFirstChild(&c->ast, nodeId);
    const char* fieldNames[256];
    SLTypeRef   fieldTypes[256];
    uint32_t    fieldCount = 0;
    const char* anonName = NULL;
    (void)n;
    while (child >= 0) {
        if (fieldCount >= (uint32_t)(sizeof(fieldNames) / sizeof(fieldNames[0]))) {
            TypeRefSetInvalid(outType);
            return 0;
        }
        if (InferExprType(c, child, &fieldTypes[fieldCount]) != 0 || !fieldTypes[fieldCount].valid)
        {
            TypeRefSetInvalid(outType);
            return 0;
        }
        CanonicalizeTypeRefBaseName(c, &fieldTypes[fieldCount]);
        fieldNames[fieldCount] = TupleFieldName(c, fieldCount);
        if (fieldNames[fieldCount] == NULL) {
            TypeRefSetInvalid(outType);
            return 0;
        }
        fieldCount++;
        child = AstNextSibling(&c->ast, child);
    }
    if (fieldCount < 2u
        || EnsureAnonTypeByFields(c, 0, fieldNames, fieldTypes, fieldCount, &anonName) != 0)
    {
        TypeRefSetInvalid(outType);
        return 0;
    }
    TypeRefSetScalar(outType, anonName);
    return 0;
}

int InferExprType_CALL_ARG(SLCBackendC* c, int32_t nodeId, const SLAstNode* n, SLTypeRef* outType) {
    int32_t inner = AstFirstChild(&c->ast, nodeId);
    (void)n;
    if (inner < 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    return InferExprType(c, inner, outType);
}

int InferExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }

    switch (n->kind) {
        case SLAst_IDENT:             return InferExprType_IDENT(c, nodeId, n, outType);
        case SLAst_COMPOUND_LIT:      return InferExprType_COMPOUND_LIT(c, nodeId, n, outType);
        case SLAst_CALL_WITH_CONTEXT: return InferExprType_CALL_WITH_CONTEXT(c, nodeId, n, outType);
        case SLAst_CALL:              return InferExprType_CALL(c, nodeId, n, outType);
        case SLAst_NEW:               return InferExprType_NEW(c, nodeId, n, outType);
        case SLAst_UNARY:             return InferExprType_UNARY(c, nodeId, n, outType);
        case SLAst_FIELD_EXPR:        return InferExprType_FIELD_EXPR(c, nodeId, n, outType);
        case SLAst_INDEX:             return InferExprType_INDEX(c, nodeId, n, outType);
        case SLAst_CAST:              return InferExprType_CAST(c, nodeId, n, outType);
        case SLAst_SIZEOF:            return InferExprType_SIZEOF(c, nodeId, n, outType);
        case SLAst_STRING:            return InferExprType_STRING(c, nodeId, n, outType);
        case SLAst_BOOL:              return InferExprType_BOOL(c, nodeId, n, outType);
        case SLAst_INT:               return InferExprType_INT(c, nodeId, n, outType);
        case SLAst_RUNE:              return InferExprType_RUNE(c, nodeId, n, outType);
        case SLAst_FLOAT:             return InferExprType_FLOAT(c, nodeId, n, outType);
        case SLAst_NULL:              return InferExprType_NULL(c, nodeId, n, outType);
        case SLAst_UNWRAP:            return InferExprType_UNWRAP(c, nodeId, n, outType);
        case SLAst_TUPLE_EXPR:        return InferExprType_TUPLE_EXPR(c, nodeId, n, outType);
        case SLAst_CALL_ARG:          return InferExprType_CALL_ARG(c, nodeId, n, outType);
        default:                      TypeRefSetInvalid(outType); return 0;
    }
}

int InferNewExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType) {
    int32_t  typeNode = -1;
    int32_t  countNode = -1;
    int32_t  initNode = -1;
    int32_t  allocNode = -1;
    int64_t  countValue = 0;
    int      countIsConst = 0;
    uint32_t arrayLen;

    TypeRefSetInvalid(outType);
    if (DecodeNewExprNodes(c, nodeId, &typeNode, &countNode, &initNode, &allocNode) != 0) {
        return 0;
    }
    (void)initNode;
    (void)allocNode;

    if (ParseTypeRef(c, typeNode, outType) != 0 || !outType->valid) {
        TypeRefSetInvalid(outType);
        return 0;
    }
    if (countNode >= 0) {
        if (EvalConstIntExpr(c, countNode, &countValue, &countIsConst) == 0 && countIsConst
            && countValue > 0 && countValue <= (int64_t)UINT32_MAX)
        {
            arrayLen = (uint32_t)countValue;
            outType->containerKind = SLTypeContainer_ARRAY;
            outType->containerPtrDepth = 1;
            outType->hasArrayLen = 1;
            outType->arrayLen = arrayLen;
            outType->ptrDepth = 0;
        } else {
            outType->ptrDepth = 1;
        }
    } else {
        outType->ptrDepth = 1;
    }
    return 0;
}

const char* UnaryOpString(SLTokenKind op) {
    switch (op) {
        case SLTok_ADD: return "+";
        case SLTok_SUB: return "-";
        case SLTok_NOT: return "!";
        case SLTok_MUL: return "*";
        case SLTok_AND: return "&";
        default:        return "";
    }
}

const char* BinaryOpString(SLTokenKind op) {
    switch (op) {
        case SLTok_ASSIGN:        return "=";
        case SLTok_ADD:           return "+";
        case SLTok_SUB:           return "-";
        case SLTok_MUL:           return "*";
        case SLTok_DIV:           return "/";
        case SLTok_MOD:           return "%";
        case SLTok_AND:           return "&";
        case SLTok_OR:            return "|";
        case SLTok_XOR:           return "^";
        case SLTok_LSHIFT:        return "<<";
        case SLTok_RSHIFT:        return ">>";
        case SLTok_EQ:            return "==";
        case SLTok_NEQ:           return "!=";
        case SLTok_LT:            return "<";
        case SLTok_GT:            return ">";
        case SLTok_LTE:           return "<=";
        case SLTok_GTE:           return ">=";
        case SLTok_LOGICAL_AND:   return "&&";
        case SLTok_LOGICAL_OR:    return "||";
        case SLTok_ADD_ASSIGN:    return "+=";
        case SLTok_SUB_ASSIGN:    return "-=";
        case SLTok_MUL_ASSIGN:    return "*=";
        case SLTok_DIV_ASSIGN:    return "/=";
        case SLTok_MOD_ASSIGN:    return "%=";
        case SLTok_AND_ASSIGN:    return "&=";
        case SLTok_OR_ASSIGN:     return "|=";
        case SLTok_XOR_ASSIGN:    return "^=";
        case SLTok_LSHIFT_ASSIGN: return "<<=";
        case SLTok_RSHIFT_ASSIGN: return ">>=";
        default:                  return "";
    }
}

int EmitHexByte(SLBuf* b, uint8_t value) {
    static const char kHex[] = "0123456789ABCDEF";
    if (BufAppendCStr(b, "0x") != 0) {
        return -1;
    }
    if (BufAppendChar(b, kHex[(value >> 4u) & 0xFu]) != 0) {
        return -1;
    }
    if (BufAppendChar(b, kHex[value & 0xFu]) != 0) {
        return -1;
    }
    return 0;
}

int BufAppendHexU64Literal(SLBuf* b, uint64_t value) {
    static const char kHex[] = "0123456789abcdef";
    uint32_t          i;
    if (BufAppendCStr(b, "0x") != 0) {
        return -1;
    }
    for (i = 0; i < 16u; i++) {
        uint32_t shift = (15u - i) * 4u;
        uint8_t  nibble = (uint8_t)((value >> shift) & 0xfu);
        if (BufAppendChar(b, kHex[nibble]) != 0) {
            return -1;
        }
    }
    return BufAppendCStr(b, "ULL");
}

int EmitStringLiteralRef(SLCBackendC* c, int32_t literalId, int writable) {
    if (BufAppendCStr(&c->out, "((__sl_str*)(void*)&sl_lit_") != 0
        || BufAppendCStr(&c->out, writable ? "rw_" : "ro_") != 0
        || BufAppendU32(&c->out, (uint32_t)literalId) != 0 || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitStringLiteralPool(SLCBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->stringLitLen; i++) {
        uint32_t               j;
        const SLStringLiteral* lit = &c->stringLits[i];
        if (BufAppendCStr(&c->out, "static const __sl_u8 sl_lit_ro_bytes_") != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendChar(&c->out, '[') != 0
            || BufAppendU32(&c->out, lit->len + 1u) != 0 || BufAppendCStr(&c->out, "] = { ") != 0)
        {
            return -1;
        }
        for (j = 0; j < lit->len; j++) {
            if (EmitHexByte(&c->out, lit->bytes[j]) != 0 || BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
        }
        if (EmitHexByte(&c->out, 0u) != 0 || BufAppendCStr(&c->out, " };\n") != 0
            || BufAppendCStr(&c->out, "static const __sl_str sl_lit_ro_") != 0
            || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(
                   &c->out,
                   " = { (__sl_u8*)(uintptr_t)"
                   "(const void*)sl_lit_ro_bytes_")
                   != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, ", ") != 0
            || BufAppendU32(&c->out, lit->len) != 0 || BufAppendCStr(&c->out, "u };\n") != 0)
        {
            return -1;
        }

        if (BufAppendCStr(&c->out, "static __sl_u8 sl_lit_rw_bytes_") != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendChar(&c->out, '[') != 0
            || BufAppendU32(&c->out, lit->len + 1u) != 0 || BufAppendCStr(&c->out, "] = { ") != 0)
        {
            return -1;
        }
        for (j = 0; j < lit->len; j++) {
            if (EmitHexByte(&c->out, lit->bytes[j]) != 0 || BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
        }
        if (EmitHexByte(&c->out, 0u) != 0 || BufAppendCStr(&c->out, " };\n") != 0
            || BufAppendCStr(&c->out, "static __sl_str sl_lit_rw_") != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, " = { sl_lit_rw_bytes_") != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, ", ") != 0
            || BufAppendU32(&c->out, lit->len) != 0 || BufAppendCStr(&c->out, "u };\n") != 0)
        {
            return -1;
        }
    }
    if (c->stringLitLen > 0 && BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }
    return 0;
}

int EmitExpr(SLCBackendC* c, int32_t nodeId);
int EmitAssertFormatArg(SLCBackendC* c, int32_t nodeId);

int IsStrBaseName(const char* _Nullable s) {
    return s != NULL && (StrEq(s, "__sl_str") || StrEq(s, "core__str"));
}

int TypeRefIsStr(const SLTypeRef* t) {
    return t->valid && t->containerKind == SLTypeContainer_SCALAR && IsStrBaseName(t->baseName);
}

int TypeRefContainerWritable(const SLTypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if (t->containerKind == SLTypeContainer_ARRAY || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        return t->readOnly == 0;
    }
    /* For slice-pointer forms, readOnly distinguishes &[...] from *[...]. */
    if (t->containerKind == SLTypeContainer_SLICE_RO && t->containerPtrDepth > 0) {
        return t->readOnly == 0;
    }
    return 0;
}

int EmitElementTypeName(SLCBackendC* c, const SLTypeRef* t, int asConst) {
    int i;
    if (!t->valid || t->baseName == NULL) {
        return -1;
    }
    if (asConst && BufAppendCStr(&c->out, "const ") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, t->baseName) != 0) {
        return -1;
    }
    for (i = 0; i < t->ptrDepth; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitLenExprFromType(SLCBackendC* c, int32_t exprNode, const SLTypeRef* t) {
    if (TypeRefIsStr(t)) {
        if (t->ptrDepth > 0) {
            if (BufAppendCStr(&c->out, "(__sl_uint)(((__sl_str*)(") != 0
                || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, "))->len)") != 0)
            {
                return -1;
            }
            return 0;
        } else {
            if (BufAppendCStr(&c->out, "(__sl_uint)((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").len)") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (t->containerKind == SLTypeContainer_ARRAY && t->hasArrayLen) {
        if (t->containerPtrDepth > 0) {
            if (BufAppendCStr(&c->out, "((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ") == 0 ? 0u : ") != 0
                || BufAppendU32(&c->out, t->arrayLen) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        if (BufAppendU32(&c->out, t->arrayLen) != 0) {
            return -1;
        }
        return 0;
    }
    if (t->containerKind == SLTypeContainer_SLICE_RO
        || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (t->containerPtrDepth > 0) {
            int stars = SliceStructPtrDepth(t);
            if (stars > 0) {
                if (BufAppendCStr(&c->out, "((") != 0 || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, ") == 0 ? 0u : (__sl_uint)((") != 0
                    || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, ")->len))") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "(__sl_uint)((") != 0 || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, ").len)") != 0)
                {
                    return -1;
                }
            }
        } else {
            if (BufAppendCStr(&c->out, "(__sl_uint)((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").len)") != 0)
            {
                return -1;
            }
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "__sl_len(") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitElemPtrExpr(
    SLCBackendC* c, int32_t baseNode, const SLTypeRef* baseType, int wantWritableElem) {
    int elemConst = !wantWritableElem;
    if (BufAppendCStr(&c->out, "((") != 0 || EmitElementTypeName(c, baseType, elemConst) != 0
        || BufAppendCStr(&c->out, "*)(") != 0)
    {
        return -1;
    }
    if (baseType->containerKind == SLTypeContainer_ARRAY) {
        if (EmitExpr(c, baseNode) != 0) {
            return -1;
        }
    } else if (
        baseType->containerKind == SLTypeContainer_SLICE_RO
        || baseType->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (baseType->containerPtrDepth > 0) {
            int stars = SliceStructPtrDepth(baseType);
            if (stars > 0) {
                if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, baseNode) != 0
                    || BufAppendCStr(&c->out, ")->ptr") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, baseNode) != 0
                    || BufAppendCStr(&c->out, ").ptr") != 0)
                {
                    return -1;
                }
            }
        } else {
            if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, baseNode) != 0
                || BufAppendCStr(&c->out, ").ptr") != 0)
            {
                return -1;
            }
        }
    } else {
        return -1;
    }
    return BufAppendCStr(&c->out, "))");
}

int EmitSliceExpr(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    int32_t          baseNode = AstFirstChild(&c->ast, nodeId);
    int32_t          child = AstNextSibling(&c->ast, baseNode);
    int              hasStart = (n->flags & SLAstFlag_INDEX_HAS_START) != 0;
    int              hasEnd = (n->flags & SLAstFlag_INDEX_HAS_END) != 0;
    int32_t          startNode = -1;
    int32_t          endNode = -1;
    SLTypeRef        baseType;
    int              outMut = 0;

    if (baseNode < 0 || InferExprType(c, baseNode, &baseType) != 0 || !baseType.valid) {
        return -1;
    }
    if (hasStart) {
        startNode = child;
        child = AstNextSibling(&c->ast, child);
    }
    if (hasEnd) {
        endNode = child;
        child = AstNextSibling(&c->ast, child);
    }
    if (child >= 0) {
        return -1;
    }
    outMut = TypeRefContainerWritable(&baseType);
    if (BufAppendCStr(&c->out, "((") != 0
        || BufAppendCStr(&c->out, outMut ? "__sl_slice_mut" : "__sl_slice_ro") != 0
        || BufAppendCStr(&c->out, "){ ") != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, outMut ? "(void*)(" : "(const void*)(") != 0
        || EmitElemPtrExpr(c, baseNode, &baseType, outMut) != 0
        || BufAppendCStr(&c->out, " + (__sl_uint)(") != 0)
    {
        return -1;
    }
    if (startNode >= 0) {
        if (EmitExpr(c, startNode) != 0) {
            return -1;
        }
    } else if (BufAppendChar(&c->out, '0') != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, ")), (__sl_uint)((") != 0) {
        return -1;
    }
    if (endNode >= 0) {
        if (EmitExpr(c, endNode) != 0) {
            return -1;
        }
    } else if (EmitLenExprFromType(c, baseNode, &baseType) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, ") - (") != 0) {
        return -1;
    }
    if (startNode >= 0) {
        if (EmitExpr(c, startNode) != 0) {
            return -1;
        }
    } else if (BufAppendChar(&c->out, '0') != 0) {
        return -1;
    }
    if (BufAppendChar(&c->out, ')') != 0) {
        return -1;
    }
    if (BufAppendChar(&c->out, ')') != 0) {
        return -1;
    }
    return BufAppendCStr(&c->out, " })");
}

int TypeRefIsPointerLike(const SLTypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if (t->containerKind == SLTypeContainer_SCALAR) {
        return t->ptrDepth > 0;
    }
    if (t->containerKind == SLTypeContainer_ARRAY) {
        return t->ptrDepth > 0 || t->containerPtrDepth > 0;
    }
    if (t->containerKind == SLTypeContainer_SLICE_RO
        || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        return t->ptrDepth > 0 || t->containerPtrDepth > 0;
    }
    return 0;
}

int TypeRefIsOwnedRuntimeArrayStruct(const SLTypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if ((t->containerKind != SLTypeContainer_SLICE_RO
         && t->containerKind != SLTypeContainer_SLICE_MUT)
        || t->containerPtrDepth == 0)
    {
        return 0;
    }
    return SliceStructPtrDepth(t) == 0;
}

int TypeRefIsNamedDeclKind(const SLCBackendC* c, const SLTypeRef* t, SLAstKind wantKind) {
    const char*           baseName;
    const SLNameMap*      map;
    const SLAnonTypeInfo* anon;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR
        || t->containerPtrDepth != 0 || t->ptrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    baseName = ResolveScalarAliasBaseName(c, t->baseName);
    if (baseName == NULL) {
        baseName = t->baseName;
    }
    map = FindNameByCName(c, baseName);
    if (map != NULL) {
        return map->kind == wantKind;
    }
    anon = FindAnonTypeByCName(c, baseName);
    if (anon != NULL) {
        if (wantKind == SLAst_STRUCT) {
            return anon->isUnion == 0;
        }
        if (wantKind == SLAst_UNION) {
            return anon->isUnion != 0;
        }
    }
    return 0;
}

int TypeRefDerefReadonlyRefLike(const SLTypeRef* in, SLTypeRef* outBase) {
    if (in == NULL || outBase == NULL || !in->valid || !in->readOnly) {
        return -1;
    }
    *outBase = *in;
    if (outBase->containerKind == SLTypeContainer_SCALAR) {
        if (outBase->ptrDepth == 0) {
            return -1;
        }
        outBase->ptrDepth--;
        outBase->readOnly = 0;
        return 0;
    }
    if (outBase->containerKind == SLTypeContainer_ARRAY
        || outBase->containerKind == SLTypeContainer_SLICE_RO
        || outBase->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (outBase->containerPtrDepth == 0) {
            return -1;
        }
        outBase->containerPtrDepth--;
        outBase->readOnly = 0;
        return 0;
    }
    return -1;
}

int ResolveComparisonHookArgCost(
    SLCBackendC*     c,
    const SLTypeRef* paramType,
    const SLTypeRef* argType,
    uint8_t*         outCost,
    int*             outAutoRef) {
    uint8_t   baseCost = 0;
    SLTypeRef byValueType;
    if (TypeRefAssignableCost(c, paramType, argType, outCost) == 0) {
        *outAutoRef = 0;
        return 0;
    }
    if (TypeRefDerefReadonlyRefLike(paramType, &byValueType) == 0
        && TypeRefAssignableCost(c, &byValueType, argType, &baseCost) == 0)
    {
        *outCost = (uint8_t)(baseCost < 254u ? baseCost + 1u : 255u);
        *outAutoRef = 1;
        return 0;
    }
    return -1;
}

/* Returns 0 success, 1 no hook name, 2 no viable hook, 3 ambiguous */
int ResolveComparisonHook(
    SLCBackendC*     c,
    const char*      hookName,
    const SLTypeRef* lhsType,
    const SLTypeRef* rhsType,
    const SLFnSig**  outSig,
    const char**     outCalleeName,
    int              outAutoRef[2]) {
    const SLFnSig* candidates[SLCCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    const SLFnSig* bestSig = NULL;
    const char*    bestCalleeName = NULL;
    uint8_t        bestCosts[2];
    uint32_t       bestTotal = 0;
    int            ambiguous = 0;
    uint32_t       i;
    candidateLen = FindFnSigCandidatesByName(
        c, hookName, candidates, (uint32_t)(sizeof(candidates) / sizeof(candidates[0])));
    if (candidateLen == 0) {
        return 1;
    }
    if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
        candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
    }
    for (i = 0; i < candidateLen; i++) {
        const SLFnSig* sig = candidates[i];
        uint8_t        costs[2];
        int            autoRef[2] = { 0, 0 };
        uint32_t       total = 0;
        int            cmp;
        if (sig->paramLen != 2) {
            continue;
        }
        if (ResolveComparisonHookArgCost(c, &sig->paramTypes[0], lhsType, &costs[0], &autoRef[0])
                != 0
            || ResolveComparisonHookArgCost(c, &sig->paramTypes[1], rhsType, &costs[1], &autoRef[1])
                   != 0)
        {
            continue;
        }
        total = (uint32_t)costs[0] + (uint32_t)costs[1];
        if (bestSig == NULL) {
            bestSig = sig;
            bestCalleeName = sig->cName;
            bestCosts[0] = costs[0];
            bestCosts[1] = costs[1];
            bestTotal = total;
            ambiguous = 0;
            outAutoRef[0] = autoRef[0];
            outAutoRef[1] = autoRef[1];
            continue;
        }
        cmp = CostVecCmp(costs, bestCosts, 2u);
        if (cmp < 0 || (cmp == 0 && total < bestTotal)) {
            bestSig = sig;
            bestCalleeName = sig->cName;
            bestCosts[0] = costs[0];
            bestCosts[1] = costs[1];
            bestTotal = total;
            ambiguous = 0;
            outAutoRef[0] = autoRef[0];
            outAutoRef[1] = autoRef[1];
            continue;
        }
        if (cmp == 0 && total == bestTotal) {
            ambiguous = 1;
        }
    }
    if (bestSig == NULL) {
        return 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outSig = bestSig;
    *outCalleeName = bestCalleeName;
    return 0;
}

int EmitExprAutoRefCoerced(SLCBackendC* c, int32_t argNode, const SLTypeRef* paramType) {
    SLTypeRef byValueType;
    if (TypeRefDerefReadonlyRefLike(paramType, &byValueType) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(__extension__({ ") != 0
        || EmitTypeNameWithDepth(c, &byValueType) != 0
        || BufAppendCStr(&c->out, " __sl_cmp_arg = ") != 0
        || EmitExprCoerced(c, argNode, &byValueType) != 0 || BufAppendCStr(&c->out, "; ((") != 0
        || EmitTypeNameWithDepth(c, paramType) != 0
        || BufAppendCStr(&c->out, ")(&__sl_cmp_arg)); }))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExprAsSliceRO(SLCBackendC* c, int32_t exprNode, const SLTypeRef* exprType) {
    if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)(") != 0
        || EmitElemPtrExpr(c, exprNode, exprType, 0) != 0
        || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
        || EmitLenExprFromType(c, exprNode, exprType) != 0 || BufAppendCStr(&c->out, ") })") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitComparisonHookCall(
    SLCBackendC*   c,
    const SLFnSig* sig,
    const char*    calleeName,
    int32_t        lhsNode,
    int32_t        rhsNode,
    const int      autoRef[2]) {
    int32_t  argNodes[2];
    uint32_t i;
    argNodes[0] = lhsNode;
    argNodes[1] = rhsNode;
    if (BufAppendCStr(&c->out, calleeName) != 0 || BufAppendChar(&c->out, '(') != 0) {
        return -1;
    }
    if (sig != NULL && sig->hasContext) {
        if (EmitContextArgForSig(c, sig) != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
    }
    for (i = 0; i < 2u; i++) {
        if (i > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (autoRef[i]) {
            if (EmitExprAutoRefCoerced(c, argNodes[i], &sig->paramTypes[i]) != 0) {
                return -1;
            }
        } else if (EmitExprCoerced(c, argNodes[i], &sig->paramTypes[i]) != 0) {
            return -1;
        }
    }
    if (BufAppendChar(&c->out, ')') != 0) {
        return -1;
    }
    return 0;
}

int EmitPointerIdentityExpr(SLCBackendC* c, int32_t exprNode, const SLTypeRef* exprType) {
    if (TypeRefIsOwnedRuntimeArrayStruct(exprType)) {
        if (BufAppendCStr(&c->out, "((const void*)((") != 0 || EmitExpr(c, exprNode) != 0
            || BufAppendCStr(&c->out, ").ptr))") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "((const void*)(") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendCStr(&c->out, "))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitNewAllocArgExpr(SLCBackendC* c, int32_t allocArg) {
    if (allocArg >= 0) {
        return EmitExpr(c, allocArg);
    }
    {
        SLTypeRef want;
        SetPreferredAllocatorPtrType(&want);
        return EmitEffectiveContextFieldValue(c, "mem", &want);
    }
}

const char* _Nullable ResolveVarSizeValueBaseName(SLCBackendC* c, const SLTypeRef* valueType) {
    const char* baseName;
    if (valueType == NULL || !valueType->valid || valueType->containerKind != SLTypeContainer_SCALAR
        || valueType->containerPtrDepth != 0 || valueType->ptrDepth != 0
        || valueType->baseName == NULL || valueType->isOptional)
    {
        return NULL;
    }
    baseName = ResolveScalarAliasBaseName(c, valueType->baseName);
    if (baseName == NULL) {
        baseName = valueType->baseName;
    }
    if (IsStrBaseName(baseName) || IsVarSizeTypeName(c, baseName)) {
        return baseName;
    }
    return NULL;
}

int EmitConcatCallExpr(SLCBackendC* c, int32_t calleeNode) {
    int32_t aNode = AstNextSibling(&c->ast, calleeNode);
    int32_t bNode = aNode >= 0 ? AstNextSibling(&c->ast, aNode) : -1;
    int32_t extra = bNode >= 0 ? AstNextSibling(&c->ast, bNode) : -1;
    if (aNode < 0 || bNode < 0 || extra >= 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "__sl_concat((__sl_Allocator*)(") != 0
        || EmitNewAllocArgExpr(c, -1) != 0 || BufAppendCStr(&c->out, "), ") != 0
        || EmitExpr(c, aNode) != 0 || BufAppendCStr(&c->out, ", ") != 0 || EmitExpr(c, bNode) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

uint32_t FmtNextTempId(SLCBackendC* c) {
    c->fmtTempCounter++;
    if (c->fmtTempCounter == 0) {
        c->fmtTempCounter = 1;
    }
    return c->fmtTempCounter;
}

int TypeRefIsSignedIntegerLike(const SLCBackendC* c, const SLTypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    return StrEq(base, "__sl_i8") || StrEq(base, "__sl_i16") || StrEq(base, "__sl_i32")
        || StrEq(base, "__sl_i64") || StrEq(base, "__sl_int");
}

int TypeRefIsIntegerLike(const SLCBackendC* c, const SLTypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    return IsIntegerCTypeName(base);
}

int TypeRefIsFloatLike(const SLCBackendC* c, const SLTypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    return IsFloatCTypeName(base);
}

int TypeRefIsBoolLike(const SLCBackendC* c, const SLTypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    return StrEq(base, "__sl_bool");
}

int TypeRefIsNamedEnumLike(const SLCBackendC* c, const SLTypeRef* t) {
    return TypeRefIsNamedDeclKind(c, t, SLAst_ENUM);
}

int TypeRefIsFmtStringLike(const SLCBackendC* c, const SLTypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR || t->baseName == NULL
        || t->containerPtrDepth != 0 || t->ptrDepth <= 0)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    (void)c;
    return IsStrBaseName(base);
}

int TypeRefIsFmtValueType(const SLCBackendC* c, const SLTypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL || t->isOptional)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    if (StrEq(base, "core__FmtValue")) {
        return 1;
    }
    return StrHasPrefix(base, "core") && StrHasSuffix(base, "__FmtValue");
}

int EmitFmtAppendLiteralBytes(
    SLCBackendC* c, const char* builderName, const uint8_t* bytes, uint32_t len) {
    uint32_t i;
    if (bytes == NULL || len == 0) {
        return 0;
    }
    if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_bytes(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ") != 0
        || BufAppendCStr(&c->out, "(const __sl_u8*)\"") != 0)
    {
        return -1;
    }
    for (i = 0; i < len; i++) {
        uint8_t ch = bytes[i];
        if (ch == (uint8_t)'\\') {
            if (BufAppendCStr(&c->out, "\\\\") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'"') {
            if (BufAppendCStr(&c->out, "\\\"") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'\n') {
            if (BufAppendCStr(&c->out, "\\n") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'\r') {
            if (BufAppendCStr(&c->out, "\\r") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'\t') {
            if (BufAppendCStr(&c->out, "\\t") != 0) {
                return -1;
            }
            continue;
        }
        if (ch >= 0x20u && ch <= 0x7eu) {
            if (BufAppendChar(&c->out, (char)ch) != 0) {
                return -1;
            }
            continue;
        }
        if (BufAppendChar(&c->out, '\\') != 0
            || BufAppendChar(&c->out, (char)('0' + ((ch >> 6u) & 0x7u))) != 0
            || BufAppendChar(&c->out, (char)('0' + ((ch >> 3u) & 0x7u))) != 0
            || BufAppendChar(&c->out, (char)('0' + (ch & 0x7u))) != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, "\", ") != 0 || BufAppendU32(&c->out, len) != 0
        || BufAppendCStr(&c->out, "u);\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFormatBufAppendLiteralBytes(
    SLCBackendC*   c,
    const char*    dstBufName,
    const char*    writeCapName,
    const char*    ioLenName,
    const uint8_t* bytes,
    uint32_t       len) {
    uint32_t i;
    if (bytes == NULL || len == 0) {
        return 0;
    }
    if (BufAppendCStr(&c->out, "    __sl_fmt_buf_append_bytes(") != 0
        || BufAppendCStr(&c->out, dstBufName) != 0 || BufAppendCStr(&c->out, ", ") != 0
        || BufAppendCStr(&c->out, writeCapName) != 0 || BufAppendCStr(&c->out, ", &") != 0
        || BufAppendCStr(&c->out, ioLenName) != 0
        || BufAppendCStr(&c->out, ", (const __sl_u8*)\"") != 0)
    {
        return -1;
    }
    for (i = 0; i < len; i++) {
        uint8_t ch = bytes[i];
        if (ch == (uint8_t)'\\') {
            if (BufAppendCStr(&c->out, "\\\\") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'"') {
            if (BufAppendCStr(&c->out, "\\\"") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'\n') {
            if (BufAppendCStr(&c->out, "\\n") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'\r') {
            if (BufAppendCStr(&c->out, "\\r") != 0) {
                return -1;
            }
            continue;
        }
        if (ch == (uint8_t)'\t') {
            if (BufAppendCStr(&c->out, "\\t") != 0) {
                return -1;
            }
            continue;
        }
        if (ch >= 0x20u && ch <= 0x7eu) {
            if (BufAppendChar(&c->out, (char)ch) != 0) {
                return -1;
            }
            continue;
        }
        if (BufAppendChar(&c->out, '\\') != 0
            || BufAppendChar(&c->out, (char)('0' + ((ch >> 6u) & 0x7u))) != 0
            || BufAppendChar(&c->out, (char)('0' + ((ch >> 3u) & 0x7u))) != 0
            || BufAppendChar(&c->out, (char)('0' + (ch & 0x7u))) != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, "\", ") != 0 || BufAppendU32(&c->out, len) != 0
        || BufAppendCStr(&c->out, "u);\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFormatBufAppendLiteralToken(
    SLCBackendC*   c,
    const char*    dstBufName,
    const char*    writeCapName,
    const char*    ioLenName,
    const uint8_t* bytes,
    uint32_t       len) {
    uint32_t i = 0;
    uint32_t start = 0;
    if (bytes == NULL || len == 0u) {
        return 0;
    }
    while (i < len) {
        if (i + 1u < len
            && ((bytes[i] == (uint8_t)'{' && bytes[i + 1u] == (uint8_t)'{')
                || (bytes[i] == (uint8_t)'}' && bytes[i + 1u] == (uint8_t)'}')))
        {
            if (i > start
                && EmitFormatBufAppendLiteralBytes(
                       c, dstBufName, writeCapName, ioLenName, bytes + start, i - start)
                       != 0)
            {
                return -1;
            }
            {
                uint8_t ch = bytes[i];
                if (EmitFormatBufAppendLiteralBytes(c, dstBufName, writeCapName, ioLenName, &ch, 1u)
                    != 0)
                {
                    return -1;
                }
            }
            i += 2u;
            start = i;
            continue;
        }
        i++;
    }
    if (len > start) {
        return EmitFormatBufAppendLiteralBytes(
            c, dstBufName, writeCapName, ioLenName, bytes + start, len - start);
    }
    return 0;
}

int EvalConstStringExpr(
    SLCBackendC*      c,
    int32_t           nodeId,
    const uint8_t**   outBytes,
    uint32_t*         outLen,
    const SLAstNode** outNode) {
    const SLAstNode* n;
    SLCTFEValue      value = { 0 };
    int              isConst = 0;
    if (c == NULL || outBytes == NULL || outLen == NULL || outNode == NULL) {
        return -1;
    }
    *outBytes = NULL;
    *outLen = 0;
    *outNode = NULL;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast.len) {
        return -1;
    }
    n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }
    while (n->kind == SLAst_CALL_ARG) {
        nodeId = AstFirstChild(&c->ast, nodeId);
        if (nodeId < 0 || (uint32_t)nodeId >= c->ast.len) {
            return -1;
        }
        n = NodeAt(c, nodeId);
        if (n == NULL) {
            return -1;
        }
    }
    if (c->constEval == NULL) {
        return -1;
    }
    if (SLConstEvalSessionEvalExpr(c->constEval, nodeId, &value, &isConst) != 0) {
        return -1;
    }
    if (!isConst || value.kind != SLCTFEValue_STRING) {
        return -1;
    }
    *outBytes = value.s.bytes;
    *outLen = value.s.len;
    *outNode = n;
    return 0;
}

int EmitFmtAppendLiteralText(
    SLCBackendC* c, const char* builderName, const char* text, uint32_t len) {
    return EmitFmtAppendLiteralBytes(c, builderName, (const uint8_t*)text, len);
}

int EmitFmtBuilderInitStmt(SLCBackendC* c, const char* builderName, int32_t allocArgNode) {
    if (BufAppendCStr(&c->out, "    __sl_fmt_builder ") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ";\n    __sl_fmt_builder_init(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", (__sl_Allocator*)(") != 0
        || EmitNewAllocArgExpr(c, allocArgNode) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
    {
        return -1;
    }
    return 0;
}

char* _Nullable FmtMakeExprField(SLCBackendC* c, const char* baseExpr, const char* fieldName) {
    SLBuf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, "(") != 0 || BufAppendCStr(&b, baseExpr) != 0
        || BufAppendCStr(&b, ").") != 0 || BufAppendCStr(&b, fieldName) != 0)
    {
        return NULL;
    }
    return BufFinish(&b);
}

char* _Nullable FmtMakeExprIndex(SLCBackendC* c, const char* baseExpr, const char* indexExpr) {
    SLBuf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, "(") != 0 || BufAppendCStr(&b, baseExpr) != 0
        || BufAppendCStr(&b, ")[") != 0 || BufAppendCStr(&b, indexExpr) != 0
        || BufAppendChar(&b, ']') != 0)
    {
        return NULL;
    }
    return BufFinish(&b);
}

int EmitFmtAppendReflectExpr(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectArray(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth) {
    uint32_t i;
    if (type == NULL || !type->valid || type->containerKind != SLTypeContainer_ARRAY
        || type->containerPtrDepth != 0 || !type->hasArrayLen)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", '[');\n") != 0)
    {
        return -1;
    }
    for (i = 0; i < type->arrayLen; i++) {
        SLTypeRef elemType = *type;
        char      idxBuf[24];
        char*     idxExpr;
        if (i > 0 && EmitFmtAppendLiteralText(c, builderName, ", ", 2u) != 0) {
            return -1;
        }
        idxBuf[0] = '\0';
        {
            SLBuf b = { 0 };
            b.arena = &c->arena;
            if (BufAppendU32(&b, i) != 0) {
                return -1;
            }
            idxExpr = BufFinish(&b);
        }
        if (idxExpr == NULL) {
            return -1;
        }
        elemType.containerKind = SLTypeContainer_SCALAR;
        elemType.containerPtrDepth = 0;
        elemType.hasArrayLen = 0;
        elemType.arrayLen = 0;
        {
            char* elemExpr = FmtMakeExprIndex(c, expr, idxExpr);
            if (elemExpr == NULL
                || EmitFmtAppendReflectExpr(c, builderName, elemExpr, &elemType, depth + 1u) != 0)
            {
                return -1;
            }
        }
    }
    if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ']');\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFmtAppendReflectSlice(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth) {
    SLTypeRef elemType;
    uint32_t  loopId;
    SLBuf     idxNameBuf = { 0 };
    SLBuf     elemExprBuf = { 0 };
    char*     idxName;
    char*     elemExpr;
    if (type == NULL || !type->valid
        || (type->containerKind != SLTypeContainer_SLICE_RO
            && type->containerKind != SLTypeContainer_SLICE_MUT)
        || type->containerPtrDepth != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", '[');\n") != 0)
    {
        return -1;
    }
    loopId = FmtNextTempId(c);
    idxNameBuf.arena = &c->arena;
    elemExprBuf.arena = &c->arena;
    if (BufAppendCStr(&idxNameBuf, "__sl_fmt_i") != 0 || BufAppendU32(&idxNameBuf, loopId) != 0) {
        return -1;
    }
    idxName = BufFinish(&idxNameBuf);
    if (idxName == NULL) {
        return -1;
    }
    elemType = *type;
    elemType.containerKind = SLTypeContainer_SCALAR;
    elemType.containerPtrDepth = 0;
    elemType.hasArrayLen = 0;
    elemType.arrayLen = 0;
    if (BufAppendCStr(&c->out, "    for (__sl_uint ") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, " = 0u; ") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, " < (__sl_uint)((") != 0 || BufAppendCStr(&c->out, expr) != 0
        || BufAppendCStr(&c->out, ").len); ") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, "++) {\n") != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "        if (") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, " > 0u) { __sl_fmt_builder_append_str(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", __sl_strlit(\", \")); }\n") != 0)
    {
        return -1;
    }
    {
        SLBuf typeNameBuf = { 0 };
        char* elemTypeName;
        typeNameBuf.arena = &c->arena;
        if (BufAppendCStr(&typeNameBuf, elemType.baseName) != 0) {
            return -1;
        }
        {
            int i;
            for (i = 0; i < elemType.ptrDepth; i++) {
                if (BufAppendChar(&typeNameBuf, '*') != 0) {
                    return -1;
                }
            }
        }
        elemTypeName = BufFinish(&typeNameBuf);
        if (elemTypeName == NULL) {
            return -1;
        }
        if (BufAppendCStr(&elemExprBuf, "(((") != 0
            || BufAppendCStr(&elemExprBuf, elemTypeName) != 0
            || BufAppendCStr(&elemExprBuf, "*)(") != 0 || BufAppendCStr(&elemExprBuf, "(") != 0
            || BufAppendCStr(&elemExprBuf, expr) != 0
            || BufAppendCStr(&elemExprBuf, ").ptr))[") != 0
            || BufAppendCStr(&elemExprBuf, idxName) != 0 || BufAppendChar(&elemExprBuf, ']') != 0)
        {
            return -1;
        }
    }
    elemExpr = BufFinish(&elemExprBuf);
    if (elemExpr == NULL) {
        return -1;
    }
    if (EmitFmtAppendReflectExpr(c, builderName, elemExpr, &elemType, depth + 1u) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "    }\n") != 0
        || BufAppendCStr(&c->out, "    __sl_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ']');\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFmtAppendReflectStruct(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth) {
    const char* baseName;
    uint32_t    i;
    uint32_t    fieldCount = 0;
    if (type == NULL || !type->valid || type->containerKind != SLTypeContainer_SCALAR
        || type->containerPtrDepth != 0 || type->ptrDepth != 0 || type->baseName == NULL)
    {
        return -1;
    }
    baseName = ResolveScalarAliasBaseName(c, type->baseName);
    if (baseName == NULL) {
        baseName = type->baseName;
    }
    if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_str(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", __sl_strlit(\"{ \"));\n") != 0)
    {
        return -1;
    }
    for (i = 0; i < c->fieldInfoLen; i++) {
        const SLFieldInfo* f = &c->fieldInfos[i];
        if (!StrEq(f->ownerType, baseName)) {
            continue;
        }
        if (fieldCount > 0 && EmitFmtAppendLiteralText(c, builderName, ", ", 2u) != 0) {
            return -1;
        }
        if (EmitFmtAppendLiteralText(c, builderName, f->fieldName, (uint32_t)StrLen(f->fieldName))
                != 0
            || EmitFmtAppendLiteralText(c, builderName, ": ", 2u) != 0)
        {
            return -1;
        }
        {
            char* fieldExpr = FmtMakeExprField(c, expr, f->fieldName);
            if (fieldExpr == NULL
                || EmitFmtAppendReflectExpr(c, builderName, fieldExpr, &f->type, depth + 1u) != 0)
            {
                return -1;
            }
        }
        fieldCount++;
    }
    if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_str(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", __sl_strlit(\" }\"));\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFmtAppendReflectExpr(
    SLCBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const SLTypeRef* type,
    uint32_t         depth) {
    if (depth > 8u) {
        return EmitFmtAppendLiteralText(c, builderName, "...", 3u);
    }
    if (type == NULL || !type->valid) {
        return EmitFmtAppendLiteralText(c, builderName, "<invalid>", 9u);
    }
    if (TypeRefIsFmtStringLike(c, type)) {
        if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_escaped_str(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ") != 0
            || BufAppendCStr(&c->out, expr) != 0 || BufAppendCStr(&c->out, ");\n") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (TypeRefIsBoolLike(c, type)) {
        if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_str(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", (") != 0
            || BufAppendCStr(&c->out, expr) != 0
            || BufAppendCStr(&c->out, ") ? __sl_strlit(\"true\") : __sl_strlit(\"false\"));\n")
                   != 0)
        {
            return -1;
        }
        return 0;
    }
    if (TypeRefIsIntegerLike(c, type) || TypeRefIsNamedEnumLike(c, type)) {
        if (TypeRefIsSignedIntegerLike(c, type)) {
            if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_i64(&") != 0
                || BufAppendCStr(&c->out, builderName) != 0
                || BufAppendCStr(&c->out, ", (__sl_i64)(") != 0 || BufAppendCStr(&c->out, expr) != 0
                || BufAppendCStr(&c->out, "));\n") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_u64(&") != 0
                || BufAppendCStr(&c->out, builderName) != 0
                || BufAppendCStr(&c->out, ", (__sl_u64)(") != 0 || BufAppendCStr(&c->out, expr) != 0
                || BufAppendCStr(&c->out, "));\n") != 0)
            {
                return -1;
            }
        }
        return 0;
    }
    if (type->containerKind == SLTypeContainer_ARRAY && type->containerPtrDepth == 0) {
        return EmitFmtAppendReflectArray(c, builderName, expr, type, depth);
    }
    if ((type->containerKind == SLTypeContainer_SLICE_RO
         || type->containerKind == SLTypeContainer_SLICE_MUT)
        && type->containerPtrDepth == 0)
    {
        return EmitFmtAppendReflectSlice(c, builderName, expr, type, depth);
    }
    if (TypeRefIsNamedDeclKind(c, type, SLAst_STRUCT) || TypeRefTupleInfo(c, type, NULL)) {
        return EmitFmtAppendReflectStruct(c, builderName, expr, type, depth);
    }
    if (TypeRefIsPointerLike(type) || type->isOptional) {
        if (BufAppendCStr(&c->out, "    __sl_fmt_builder_append_str(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ((") != 0
            || BufAppendCStr(&c->out, expr) != 0
            || BufAppendCStr(
                   &c->out, ") == 0) ? __sl_strlit(\"null\") : __sl_strlit(\"<ptr>\"));\n")
                   != 0)
        {
            return -1;
        }
        return 0;
    }
    return EmitFmtAppendLiteralText(c, builderName, "<unsupported>", 13u);
}

int EmitExprCoerceFmtValue(
    SLCBackendC* c, int32_t exprNode, const SLTypeRef* srcType, const SLTypeRef* dstType) {
    uint32_t    tempId;
    SLBuf       valueNameBuf = { 0 };
    SLBuf       builderNameBuf = { 0 };
    SLBuf       reprNameBuf = { 0 };
    char*       valueName;
    char*       builderName;
    char*       reprName;
    const char* kindExpr = "((__sl_u8)3u)";
    if (c == NULL || srcType == NULL || dstType == NULL) {
        return -1;
    }
    tempId = FmtNextTempId(c);
    valueNameBuf.arena = &c->arena;
    builderNameBuf.arena = &c->arena;
    reprNameBuf.arena = &c->arena;
    if (BufAppendCStr(&valueNameBuf, "__sl_refv_v") != 0 || BufAppendU32(&valueNameBuf, tempId) != 0
        || BufAppendCStr(&builderNameBuf, "__sl_refv_b") != 0
        || BufAppendU32(&builderNameBuf, tempId) != 0
        || BufAppendCStr(&reprNameBuf, "__sl_refv_r") != 0
        || BufAppendU32(&reprNameBuf, tempId) != 0)
    {
        return -1;
    }
    valueName = BufFinish(&valueNameBuf);
    builderName = BufFinish(&builderNameBuf);
    reprName = BufFinish(&reprNameBuf);
    if (valueName == NULL || builderName == NULL || reprName == NULL) {
        return -1;
    }
    if (TypeRefIsIntegerLike(c, srcType)) {
        if (TypeRefIsSignedIntegerLike(c, srcType)) {
            kindExpr = "((__sl_u8)1u)";
        } else {
            kindExpr = "((__sl_u8)2u)";
        }
    }

    if (BufAppendCStr(&c->out, "(__extension__({\n    __auto_type ") != 0
        || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, " = ") != 0
        || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, ";\n") != 0
        || EmitFmtBuilderInitStmt(c, builderName, -1) != 0)
    {
        return -1;
    }
    if (EmitFmtAppendReflectExpr(c, builderName, valueName, srcType, 0u) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "    __sl_str* ") != 0 || BufAppendCStr(&c->out, reprName) != 0
        || BufAppendCStr(&c->out, " = __sl_fmt_builder_finish(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ");\n    ((") != 0
        || EmitTypeNameWithDepth(c, dstType) != 0 || BufAppendCStr(&c->out, "){ .kind = ") != 0
        || BufAppendCStr(&c->out, kindExpr) != 0 || BufAppendCStr(&c->out, ", .repr = ") != 0
        || BufAppendCStr(&c->out, reprName) != 0 || BufAppendCStr(&c->out, " });\n}))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFreeCallExpr(SLCBackendC* c, int32_t allocArgNode, int32_t valueNode) {
    SLTypeRef valueType;
    if (InferExprType(c, valueNode, &valueType) != 0 || !valueType.valid) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "__sl_free((__sl_Allocator*)(") != 0
        || EmitNewAllocArgExpr(c, allocArgNode) != 0 || BufAppendCStr(&c->out, "), ") != 0)
    {
        return -1;
    }

    if (valueType.containerKind == SLTypeContainer_SCALAR && valueType.containerPtrDepth == 0
        && valueType.ptrDepth > 0)
    {
        SLTypeRef pointeeType = valueType;
        pointeeType.ptrDepth--;
        if (BufAppendCStr(&c->out, "(void*)(") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, "), ") != 0)
        {
            return -1;
        }
        if (pointeeType.ptrDepth == 0 && pointeeType.containerKind == SLTypeContainer_SCALAR
            && IsStrBaseName(pointeeType.baseName))
        {
            if (BufAppendCStr(&c->out, "__sl_str_sizeof((__sl_str*)(") != 0
                || EmitExpr(c, valueNode) != 0
                || BufAppendCStr(&c->out, ")), _Alignof(__sl_str))") != 0)
            {
                return -1;
            }
            return 0;
        }
        if (BufAppendCStr(&c->out, "sizeof(") != 0 || EmitTypeNameWithDepth(c, &pointeeType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &pointeeType) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }

    if ((valueType.containerKind == SLTypeContainer_SLICE_RO
         || valueType.containerKind == SLTypeContainer_SLICE_MUT)
        && valueType.containerPtrDepth > 0 && SliceStructPtrDepth(&valueType) == 0)
    {
        if (BufAppendCStr(&c->out, "(void*)((") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, ").ptr), ((__sl_uint)((") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, ").len) * sizeof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0
            || BufAppendCStr(&c->out, ")), _Alignof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }

    if (valueType.containerKind == SLTypeContainer_ARRAY && valueType.containerPtrDepth > 0
        && valueType.hasArrayLen)
    {
        if (BufAppendCStr(&c->out, "(void*)(") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, "), ((__sl_uint)") != 0
            || BufAppendU32(&c->out, valueType.arrayLen) != 0
            || BufAppendCStr(&c->out, " * sizeof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0
            || BufAppendCStr(&c->out, ")), _Alignof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }

    return -1;
}

int EmitNewExpr(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable dstType, int requireNonNull) {
    int32_t          typeNode = -1;
    int32_t          countArg = -1;
    int32_t          initArg = -1;
    int32_t          allocArg = -1;
    SLTypeRef        elemType;
    const char*      varSizeBaseName = NULL;
    const char*      ownerTypeName = NULL;
    const SLNameMap* ownerMap = NULL;
    int              needsImplicitInit = 0;
    int              dstIsRuntimeArray = 0;
    int              dstIsRuntimeArrayMut = 0;

    if (DecodeNewExprNodes(c, nodeId, &typeNode, &countArg, &initArg, &allocArg) != 0) {
        return -1;
    }
    if (ParseTypeRef(c, typeNode, &elemType) != 0 || !elemType.valid) {
        return -1;
    }
    if (dstType != NULL
        && (dstType->containerKind == SLTypeContainer_SLICE_RO
            || dstType->containerKind == SLTypeContainer_SLICE_MUT)
        && dstType->containerPtrDepth > 0)
    {
        dstIsRuntimeArray = 1;
        dstIsRuntimeArrayMut = dstType->containerKind == SLTypeContainer_SLICE_MUT;
    }
    if (countArg < 0) {
        varSizeBaseName = ResolveVarSizeValueBaseName(c, &elemType);
    }
    if (countArg < 0 && initArg < 0 && varSizeBaseName == NULL
        && elemType.containerKind == SLTypeContainer_SCALAR && elemType.containerPtrDepth == 0
        && elemType.ptrDepth == 0 && elemType.baseName != NULL && !elemType.isOptional)
    {
        ownerTypeName = ResolveScalarAliasBaseName(c, elemType.baseName);
        if (ownerTypeName != NULL) {
            ownerMap = FindNameByCName(c, ownerTypeName);
            if (ownerMap != NULL && ownerMap->kind == SLAst_STRUCT
                && StructHasFieldDefaults(c, ownerTypeName))
            {
                needsImplicitInit = 1;
            }
        }
    }

    if (countArg >= 0 && dstIsRuntimeArray) {
        if (requireNonNull) {
            if (BufAppendCStr(
                    &c->out,
                    dstIsRuntimeArrayMut
                        ? "((__sl_slice_mut){ (void*)__sl_unwrap((const void*)("
                        : "((__sl_slice_ro){ (const void*)__sl_unwrap((const "
                          "void*)(")
                != 0)
            {
                return -1;
            }
            if (BufAppendCStr(&c->out, "__sl_new_array((__sl_Allocator*)(") != 0
                || EmitNewAllocArgExpr(c, allocArg) != 0
                || BufAppendCStr(&c->out, "), sizeof(") != 0
                || EmitTypeNameWithDepth(c, &elemType) != 0
                || BufAppendCStr(&c->out, "), _Alignof(") != 0
                || EmitTypeNameWithDepth(c, &elemType) != 0
                || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
                || BufAppendCStr(&c->out, ")))), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            if (BufAppendCStr(&c->out, " })") != 0) {
                return -1;
            }
            return 0;
        }

        if (BufAppendCStr(
                &c->out,
                dstIsRuntimeArrayMut ? "__sl_new_array_slice_mut((__sl_Allocator*)("
                                     : "__sl_new_array_slice_ro((__sl_Allocator*)(")
                != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }

    if (countArg >= 0) {
        if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "*)") != 0)
        {
            return -1;
        }
        if (requireNonNull && BufAppendCStr(&c->out, "__sl_unwrap((const void*)(") != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "__sl_new_array((__sl_Allocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        if (requireNonNull && BufAppendCStr(&c->out, "))") != 0) {
            return -1;
        }
        return BufAppendChar(&c->out, ')');
    }

    if (countArg < 0 && initArg < 0 && !needsImplicitInit && varSizeBaseName == NULL) {
        if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "*)") != 0)
        {
            return -1;
        }
        if (requireNonNull) {
            if (BufAppendCStr(&c->out, "__sl_unwrap((const void*)(") != 0) {
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, "__sl_new((__sl_Allocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        if (requireNonNull && BufAppendCStr(&c->out, "))") != 0) {
            return -1;
        }
        return BufAppendChar(&c->out, ')');
    }

    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, &elemType) != 0
        || BufAppendCStr(&c->out, "*)") != 0)
    {
        return -1;
    }
    if (requireNonNull && BufAppendCStr(&c->out, "__sl_unwrap((const void*)(") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(__extension__({\n    ") != 0
        || EmitTypeNameWithDepth(c, &elemType) != 0 || BufAppendCStr(&c->out, "* __sl_p;\n") != 0)
    {
        return -1;
    }

    if (varSizeBaseName != NULL) {
        const SLAstNode* initNode;
        const char*      initOwnerType;
        int32_t          initOwnerNodeId = -1;
        int32_t          fieldNode;
        if (initArg < 0) {
            return -1;
        }
        initNode = NodeAt(c, initArg);
        initOwnerType = ResolveScalarAliasBaseName(c, elemType.baseName);
        if (initOwnerType == NULL || initNode == NULL || initNode->kind != SLAst_COMPOUND_LIT) {
            return -1;
        }
        {
            uint32_t i;
            for (i = 0; i < c->topDeclLen; i++) {
                int32_t          topNodeId = c->topDecls[i].nodeId;
                const SLAstNode* topNode = NodeAt(c, topNodeId);
                const SLNameMap* topMap;
                if (topNode == NULL || topNode->kind != SLAst_STRUCT) {
                    continue;
                }
                topMap = FindNameBySlice(c, topNode->dataStart, topNode->dataEnd);
                if (topMap != NULL && topMap->cName != NULL && StrEq(topMap->cName, initOwnerType))
                {
                    initOwnerNodeId = topNodeId;
                    break;
                }
            }
        }
        if (BufAppendCStr(&c->out, "    ") != 0 || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, " __sl_init = {0};\n") != 0)
        {
            return -1;
        }
        fieldNode = AstFirstChild(&c->ast, initArg);
        if (fieldNode >= 0 && IsTypeNodeKind(NodeAt(c, fieldNode)->kind)) {
            fieldNode = AstNextSibling(&c->ast, fieldNode);
        }
        while (fieldNode >= 0) {
            const SLAstNode*   field = NodeAt(c, fieldNode);
            const SLFieldInfo* fieldPath[64];
            const SLFieldInfo* resolvedField = NULL;
            uint32_t           fieldPathLen = 0;
            int32_t            exprNode;
            uint32_t           i;
            if (field == NULL || field->kind != SLAst_COMPOUND_FIELD) {
                return -1;
            }
            exprNode = AstFirstChild(&c->ast, fieldNode);
            if (exprNode < 0 && (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                return -1;
            }
            if (ResolveFieldPathBySlice(
                    c,
                    initOwnerType,
                    field->dataStart,
                    field->dataEnd,
                    fieldPath,
                    (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                    &fieldPathLen,
                    &resolvedField)
                    != 0
                || fieldPathLen == 0)
            {
                return -1;
            }
            resolvedField = fieldPath[fieldPathLen - 1u];
            if (BufAppendCStr(&c->out, "    __sl_init") != 0) {
                return -1;
            }
            for (i = 0; i < fieldPathLen; i++) {
                if (BufAppendChar(&c->out, '.') != 0
                    || BufAppendCStr(&c->out, fieldPath[i]->fieldName) != 0)
                {
                    return -1;
                }
            }
            if (BufAppendCStr(&c->out, " = ") != 0
                || EmitCompoundFieldValueCoerced(c, field, exprNode, &resolvedField->type) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
            fieldNode = AstNextSibling(&c->ast, fieldNode);
        }
        if (BufAppendCStr(&c->out, "    __sl_uint __sl_size = ") != 0) {
            return -1;
        }
        if (IsStrBaseName(varSizeBaseName)) {
            if (BufAppendCStr(&c->out, "__sl_str_sizeof((__sl_str*)&__sl_init)") != 0) {
                return -1;
            }
        } else if (
            BufAppendCStr(&c->out, varSizeBaseName) != 0
            || BufAppendCStr(&c->out, "__sizeof(&__sl_init)") != 0)
        {
            return -1;
        }
        if (BufAppendCStr(&c->out, ";\n") != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "    __sl_p = (") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "*)__sl_new((__sl_Allocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0
            || BufAppendCStr(&c->out, "), __sl_size, _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
        {
            return -1;
        }
        if (BufAppendCStr(&c->out, "    if (__sl_p != NULL) {\n        *__sl_p = __sl_init;\n")
            != 0)
        {
            return -1;
        }
        if (IsStrBaseName(varSizeBaseName)) {
            if (BufAppendCStr(
                    &c->out,
                    "        if (__sl_p->ptr == (__sl_u8*)0 && __sl_p->len > 0u) {\n"
                    "            __sl_p->ptr = (__sl_u8*)(void*)(__sl_p + 1);\n"
                    "            __sl_p->ptr[__sl_p->len] = 0;\n"
                    "        }\n")
                != 0)
            {
                return -1;
            }
        } else if (initOwnerNodeId >= 0) {
            int32_t declField = AstFirstChild(&c->ast, initOwnerNodeId);
            if (BufAppendCStr(&c->out, "        __sl_uint __sl_off = sizeof(") != 0
                || BufAppendCStr(&c->out, varSizeBaseName) != 0
                || BufAppendCStr(&c->out, "__hdr);\n") != 0)
            {
                return -1;
            }
            while (declField >= 0) {
                const SLAstNode* df = NodeAt(c, declField);
                if (df != NULL && df->kind == SLAst_FIELD) {
                    int32_t          wt = AstFirstChild(&c->ast, declField);
                    const SLAstNode* wtn = NodeAt(c, wt);
                    if (wtn != NULL && wtn->kind == SLAst_TYPE_VARRAY) {
                        int32_t welem = AstFirstChild(&c->ast, wt);
                        if (BufAppendCStr(
                                &c->out, "        __sl_off = __sl_align_up(__sl_off, _Alignof(")
                                != 0
                            || EmitTypeForCast(c, welem) != 0
                            || BufAppendCStr(
                                   &c->out, "));\n        __sl_off += (__sl_uint)__sl_p->")
                                   != 0
                            || BufAppendSlice(
                                   &c->out, c->unit->source, wtn->dataStart, wtn->dataEnd)
                                   != 0
                            || BufAppendCStr(&c->out, " * sizeof(") != 0
                            || EmitTypeForCast(c, welem) != 0
                            || BufAppendCStr(&c->out, ");\n") != 0)
                        {
                            return -1;
                        }
                    } else if (wt >= 0) {
                        SLTypeRef   wFieldType;
                        const char* wVarSizeBaseName = NULL;
                        if (ParseTypeRef(c, wt, &wFieldType) != 0) {
                            return -1;
                        }
                        wVarSizeBaseName = ResolveVarSizeValueBaseName(c, &wFieldType);
                        if (wVarSizeBaseName != NULL) {
                            if (IsStrBaseName(wVarSizeBaseName)) {
                                if (BufAppendCStr(&c->out, "        if (__sl_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".ptr == (__sl_u8*)0 && __sl_p->")
                                           != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".len > 0u) {\n        __sl_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(
                                           &c->out,
                                           ".ptr = (__sl_u8*)((__sl_u8*)__sl_p + __sl_off);\n")
                                           != 0
                                    || BufAppendCStr(&c->out, "        __sl_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".ptr[__sl_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".len] = 0;\n        }\n") != 0)
                                {
                                    return -1;
                                }
                            }
                            if (BufAppendCStr(&c->out, "        __sl_off += ") != 0) {
                                return -1;
                            }
                            if (IsStrBaseName(wVarSizeBaseName)) {
                                if (BufAppendCStr(&c->out, "__sl_str_sizeof((__sl_str*)&__sl_p->")
                                        != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendChar(&c->out, ')') != 0)
                                {
                                    return -1;
                                }
                            } else if (
                                BufAppendCStr(&c->out, wVarSizeBaseName) != 0
                                || BufAppendCStr(&c->out, "__sizeof(&__sl_p->") != 0
                                || BufAppendSlice(
                                       &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                       != 0
                                || BufAppendChar(&c->out, ')') != 0)
                            {
                                return -1;
                            }
                            if (BufAppendCStr(&c->out, " - sizeof(") != 0
                                || EmitTypeForCast(c, wt) != 0
                                || BufAppendCStr(&c->out, ");\n") != 0)
                            {
                                return -1;
                            }
                        }
                    }
                }
                declField = AstNextSibling(&c->ast, declField);
            }
        }
        if (BufAppendCStr(&c->out, "    }\n") != 0) {
            return -1;
        }
    } else {
        if (BufAppendCStr(&c->out, "    __sl_p = (") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "*)__sl_new((__sl_Allocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
        {
            return -1;
        }
        if (initArg >= 0 || needsImplicitInit) {
            if (BufAppendCStr(&c->out, "    if (__sl_p != NULL) {\n        *__sl_p = ") != 0) {
                return -1;
            }
            if (initArg >= 0) {
                if (EmitExprCoerced(c, initArg, &elemType) != 0) {
                    return -1;
                }
            } else {
                if (ownerTypeName == NULL
                    || EmitCompoundLiteralOrderedStruct(c, -1, ownerTypeName, &elemType) != 0)
                {
                    return -1;
                }
            }
            if (BufAppendCStr(&c->out, ";\n    }\n") != 0) {
                return -1;
            }
        }
    }

    if (BufAppendCStr(&c->out, "    __sl_p;\n}))") != 0) {
        return -1;
    }
    if (requireNonNull && BufAppendCStr(&c->out, "))") != 0) {
        return -1;
    }
    return BufAppendChar(&c->out, ')');
}

int EmitExprCoerced(SLCBackendC* c, int32_t exprNode, const SLTypeRef* dstType) {
    const SLAstNode*   expr = NodeAt(c, exprNode);
    SLTypeRef          srcType;
    const SLFieldInfo* embedPath[64];
    uint32_t           embedPathLen = 0;
    if (dstType == NULL || !dstType->valid) {
        return EmitExpr(c, exprNode);
    }
    if (expr != NULL && expr->kind == SLAst_NEW) {
        int requireNonNull = TypeRefIsPointerLike(dstType) && !dstType->isOptional;
        return EmitNewExpr(c, exprNode, dstType, requireNonNull);
    }
    if (expr != NULL
        && (expr->kind == SLAst_STRING
            || (expr->kind == SLAst_BINARY && (SLTokenKind)expr->op == SLTok_ADD))
        && dstType->containerKind == SLTypeContainer_SCALAR && dstType->containerPtrDepth == 0
        && dstType->ptrDepth > 0 && IsStrBaseName(dstType->baseName))
    {
        int32_t literalId = -1;
        if ((uint32_t)exprNode < c->stringLitByNodeLen) {
            literalId = c->stringLitByNode[exprNode];
        }
        if (literalId < 0) {
            return -1;
        }
        return EmitStringLiteralRef(c, literalId, dstType->readOnly ? 0 : 1);
    }
    if (TypeRefIsFmtValueType(c, dstType)) {
        if (InferExprType(c, exprNode, &srcType) == 0 && srcType.valid
            && !TypeRefIsFmtValueType(c, &srcType))
        {
            return EmitExprCoerceFmtValue(c, exprNode, &srcType, dstType);
        }
    }
    if (expr != NULL && expr->kind == SLAst_COMPOUND_LIT) {
        if (TypeRefIsPointerLike(dstType)) {
            SLTypeRef        targetType = *dstType;
            const SLTypeRef* literalExpected = NULL;
            if (targetType.containerKind != SLTypeContainer_SCALAR) {
                if (targetType.containerPtrDepth > 0) {
                    targetType.containerPtrDepth--;
                    literalExpected = &targetType;
                } else if (targetType.ptrDepth > 0) {
                    targetType.ptrDepth--;
                    literalExpected = &targetType;
                }
            } else if (targetType.ptrDepth > 0) {
                targetType.ptrDepth--;
                literalExpected = &targetType;
            }
            if (literalExpected != NULL) {
                if (BufAppendCStr(&c->out, "(&") != 0
                    || EmitCompoundLiteral(c, exprNode, literalExpected) != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                return 0;
            }
        }
        return EmitCompoundLiteral(c, exprNode, dstType);
    }
    if (expr != NULL && expr->kind == SLAst_UNARY && (SLTokenKind)expr->op == SLTok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, exprNode);
        const SLAstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == SLAst_COMPOUND_LIT) {
            SLTypeRef        targetType = *dstType;
            const SLTypeRef* literalExpected = NULL;
            if (targetType.containerKind != SLTypeContainer_SCALAR) {
                if (targetType.containerPtrDepth > 0) {
                    targetType.containerPtrDepth--;
                    literalExpected = &targetType;
                }
            } else if (targetType.ptrDepth > 0) {
                targetType.ptrDepth--;
                literalExpected = &targetType;
            }
            if (BufAppendCStr(&c->out, "(&") != 0
                || EmitCompoundLiteral(c, rhsNode, literalExpected) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (InferExprType(c, exprNode, &srcType) != 0 || !srcType.valid) {
        return EmitExpr(c, exprNode);
    }
    if (TypeRefIsFmtValueType(c, dstType) && !TypeRefIsFmtValueType(c, &srcType)) {
        return EmitExprCoerceFmtValue(c, exprNode, &srcType, dstType);
    }
    if (srcType.containerKind == SLTypeContainer_SCALAR
        && dstType->containerKind == SLTypeContainer_SCALAR && srcType.baseName != NULL
        && dstType->baseName != NULL)
    {
        if (srcType.ptrDepth == 0 && srcType.containerPtrDepth == 0 && dstType->ptrDepth == 0
            && dstType->containerPtrDepth == 0
            && ResolveEmbeddedPathByNames(
                   c,
                   srcType.baseName,
                   dstType->baseName,
                   embedPath,
                   (uint32_t)(sizeof(embedPath) / sizeof(embedPath[0])),
                   &embedPathLen)
                   == 0
            && embedPathLen > 0)
        {
            uint32_t i;
            if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, exprNode) != 0) {
                return -1;
            }
            for (i = 0; i < embedPathLen; i++) {
                if (BufAppendChar(&c->out, '.') != 0
                    || BufAppendCStr(&c->out, embedPath[i]->fieldName) != 0)
                {
                    return -1;
                }
            }
            return BufAppendChar(&c->out, ')');
        }
        if (srcType.ptrDepth > 0 && srcType.ptrDepth == dstType->ptrDepth
            && srcType.containerPtrDepth == 0 && dstType->containerPtrDepth == 0
            && ResolveEmbeddedPathByNames(
                   c,
                   srcType.baseName,
                   dstType->baseName,
                   embedPath,
                   (uint32_t)(sizeof(embedPath) / sizeof(embedPath[0])),
                   &embedPathLen)
                   == 0
            && embedPathLen > 0)
        {
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if ((dstType->containerKind == SLTypeContainer_SLICE_RO
         || dstType->containerKind == SLTypeContainer_SLICE_MUT)
        && dstType->containerPtrDepth > 0 && SliceStructPtrDepth(dstType) == 0)
    {
        const char* srcBase = ResolveScalarAliasBaseName(c, srcType.baseName);
        if (srcBase == NULL) {
            srcBase = srcType.baseName;
        }
        if (srcType.containerKind == SLTypeContainer_SCALAR && srcType.containerPtrDepth == 0
            && srcType.ptrDepth > 0 && IsStrBaseName(srcBase) && dstType->baseName != NULL
            && StrEq(dstType->baseName, "__sl_u8"))
        {
            if (dstType->containerKind == SLTypeContainer_SLICE_MUT && srcType.readOnly) {
                return EmitExpr(c, exprNode);
            }
            if (dstType->containerKind == SLTypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__sl_slice_mut){ (void*)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, "))->ptr), (__sl_uint)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, "))->len) })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, "))->ptr), (__sl_uint)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, "))->len) })") != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
        if (dstType->containerKind == SLTypeContainer_SLICE_RO
            && srcType.containerKind == SLTypeContainer_SLICE_MUT && srcType.containerPtrDepth > 0
            && SliceStructPtrDepth(&srcType) == 0 && srcType.ptrDepth == dstType->ptrDepth
            && srcType.containerPtrDepth == dstType->containerPtrDepth && srcType.baseName != NULL
            && dstType->baseName != NULL && StrEq(srcType.baseName, dstType->baseName))
        {
            if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)((") != 0
                || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").ptr), (__sl_uint)((") != 0
                || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, ").len) })") != 0)
            {
                return -1;
            }
            return 0;
        }
        if (srcType.containerKind == dstType->containerKind && srcType.containerPtrDepth > 0
            && srcType.ptrDepth == dstType->ptrDepth
            && srcType.containerPtrDepth == dstType->containerPtrDepth && srcType.baseName != NULL
            && dstType->baseName != NULL && StrEq(srcType.baseName, dstType->baseName))
        {
            return EmitExpr(c, exprNode);
        }
        if (srcType.containerKind == SLTypeContainer_ARRAY && srcType.ptrDepth == dstType->ptrDepth
            && (srcType.containerPtrDepth == dstType->containerPtrDepth
                || srcType.containerPtrDepth + 1 == dstType->containerPtrDepth)
            && srcType.baseName != NULL && dstType->baseName != NULL
            && StrEq(srcType.baseName, dstType->baseName))
        {
            if (dstType->containerKind == SLTypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__sl_slice_mut){ (void*)(") != 0
                    || EmitElemPtrExpr(c, exprNode, &srcType, 1) != 0
                    || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                    || EmitLenExprFromType(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)(") != 0
                    || EmitElemPtrExpr(c, exprNode, &srcType, 0) != 0
                    || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                    || EmitLenExprFromType(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
    }
    if (dstType->containerKind == SLTypeContainer_SLICE_RO && dstType->containerPtrDepth == 0) {
        if (srcType.containerKind == SLTypeContainer_SLICE_RO && srcType.containerPtrDepth == 0) {
            return EmitExpr(c, exprNode);
        }
        if ((srcType.containerKind == SLTypeContainer_SLICE_MUT && srcType.containerPtrDepth == 0)
            || srcType.containerKind == SLTypeContainer_ARRAY)
        {
            if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)(") != 0
                || EmitElemPtrExpr(c, exprNode, &srcType, 0) != 0
                || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                || EmitLenExprFromType(c, exprNode, &srcType) != 0
                || BufAppendCStr(&c->out, ") })") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (dstType->containerKind == SLTypeContainer_SLICE_MUT && dstType->containerPtrDepth == 0) {
        if (srcType.containerKind == SLTypeContainer_SLICE_MUT && srcType.containerPtrDepth == 0) {
            return EmitExpr(c, exprNode);
        }
        if (srcType.containerKind == SLTypeContainer_ARRAY) {
            if (BufAppendCStr(&c->out, "((__sl_slice_mut){ (void*)(") != 0
                || EmitElemPtrExpr(c, exprNode, &srcType, 1) != 0
                || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                || EmitLenExprFromType(c, exprNode, &srcType) != 0
                || BufAppendCStr(&c->out, ") })") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    return EmitExpr(c, exprNode);
}

int32_t ActiveCallOverlayNode(const SLCBackendC* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast.len) {
        return -1;
    }
    {
        int32_t callNode = AstFirstChild(&c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? AstNextSibling(&c->ast, callNode) : -1;
        if (child >= 0) {
            const SLAstNode* n = NodeAt(c, child);
            if (n != NULL && n->kind == SLAst_CONTEXT_OVERLAY) {
                return child;
            }
        }
    }
    return -1;
}

int32_t FindActiveOverlayBindByName(const SLCBackendC* c, const char* fieldName) {
    int32_t overlayNode = ActiveCallOverlayNode(c);
    int32_t child = overlayNode >= 0 ? AstFirstChild(&c->ast, overlayNode) : -1;
    while (child >= 0) {
        const SLAstNode* b = NodeAt(c, child);
        if (b != NULL && b->kind == SLAst_CONTEXT_BIND
            && SliceEqName(c->unit->source, b->dataStart, b->dataEnd, fieldName))
        {
            return child;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return -1;
}

int EmitCurrentContextFieldRaw(SLCBackendC* c, const char* fieldName) {
    if (c->hasCurrentContext) {
        if (BufAppendCStr(&c->out, "(context->") != 0 || BufAppendCStr(&c->out, fieldName) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    return -1;
}

int EmitCurrentContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType) {
    if (c->hasCurrentContext) {
        const SLFieldInfo* srcField = NULL;
        if (c->currentContextType.valid && c->currentContextType.baseName != NULL) {
            srcField = FindFieldInfoByName(c, c->currentContextType.baseName, fieldName);
        }
        if (requiredType != NULL && requiredType->valid && srcField != NULL && srcField->type.valid
            && !TypeRefEqual(&srcField->type, requiredType))
        {
            const SLFieldInfo* embedPath[64];
            uint32_t           embedPathLen = 0;
            uint8_t            cost = 0;
            if (srcField->type.containerKind == SLTypeContainer_SCALAR
                && requiredType->containerKind == SLTypeContainer_SCALAR
                && srcField->type.baseName != NULL && requiredType->baseName != NULL)
            {
                if (srcField->type.ptrDepth == 0 && srcField->type.containerPtrDepth == 0
                    && requiredType->ptrDepth == 0 && requiredType->containerPtrDepth == 0
                    && ResolveEmbeddedPathByNames(
                           c,
                           srcField->type.baseName,
                           requiredType->baseName,
                           embedPath,
                           (uint32_t)(sizeof(embedPath) / sizeof(embedPath[0])),
                           &embedPathLen)
                           == 0
                    && embedPathLen > 0)
                {
                    uint32_t i;
                    if (EmitCurrentContextFieldRaw(c, fieldName) != 0) {
                        return -1;
                    }
                    for (i = 0; i < embedPathLen; i++) {
                        if (BufAppendChar(&c->out, '.') != 0
                            || BufAppendCStr(&c->out, embedPath[i]->fieldName) != 0)
                        {
                            return -1;
                        }
                    }
                    return 0;
                }
                if (srcField->type.ptrDepth > 0 && srcField->type.ptrDepth == requiredType->ptrDepth
                    && srcField->type.containerPtrDepth == 0 && requiredType->containerPtrDepth == 0
                    && ResolveEmbeddedPathByNames(
                           c,
                           srcField->type.baseName,
                           requiredType->baseName,
                           embedPath,
                           (uint32_t)(sizeof(embedPath) / sizeof(embedPath[0])),
                           &embedPathLen)
                           == 0
                    && embedPathLen > 0)
                {
                    if (BufAppendCStr(&c->out, "((") != 0
                        || EmitTypeNameWithDepth(c, requiredType) != 0
                        || BufAppendCStr(&c->out, ")(") != 0
                        || EmitCurrentContextFieldRaw(c, fieldName) != 0
                        || BufAppendCStr(&c->out, "))") != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
            }
            if (TypeRefAssignableCost(c, requiredType, &srcField->type, &cost) == 0 && cost > 0) {
                if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, requiredType) != 0
                    || BufAppendCStr(&c->out, ")(") != 0
                    || EmitCurrentContextFieldRaw(c, fieldName) != 0
                    || BufAppendCStr(&c->out, "))") != 0)
                {
                    return -1;
                }
                return 0;
            }

            if (StrEq(fieldName, "log") && srcField->type.containerKind == SLTypeContainer_SCALAR
                && requiredType->containerKind == SLTypeContainer_SCALAR
                && srcField->type.baseName != NULL && requiredType->baseName != NULL
                && srcField->type.ptrDepth == 0 && requiredType->ptrDepth == 0
                && srcField->type.containerPtrDepth == 0 && requiredType->containerPtrDepth == 0)
            {
                if (BufAppendCStr(&c->out, "(*((") != 0
                    || EmitTypeNameWithDepth(c, requiredType) != 0
                    || BufAppendCStr(&c->out, "*)(void*)&(") != 0
                    || EmitCurrentContextFieldRaw(c, fieldName) != 0
                    || BufAppendCStr(&c->out, ")))") != 0)
                {
                    return -1;
                }
                return 0;
            }
        }
    }
    return EmitCurrentContextFieldRaw(c, fieldName);
}

int EmitEffectiveContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType) {
    int32_t bindNode = FindActiveOverlayBindByName(c, fieldName);
    int32_t bindExpr = bindNode >= 0 ? AstFirstChild(&c->ast, bindNode) : -1;
    if (bindExpr >= 0) {
        int32_t savedActive = c->activeCallWithNode;
        int     rc;
        c->activeCallWithNode = -1;
        rc = EmitExprCoerced(c, bindExpr, requiredType);
        c->activeCallWithNode = savedActive;
        return rc;
    }
    return EmitCurrentContextFieldValue(c, fieldName, requiredType);
}

int EmitContextArgForSig(SLCBackendC* c, const SLFnSig* sig) {
    uint32_t i;
    uint32_t fieldCount = 0;
    if (sig == NULL || !sig->hasContext) {
        return 0;
    }
    if (!sig->contextType.valid || sig->contextType.baseName == NULL) {
        return -1;
    }

    for (i = 0; i < c->fieldInfoLen; i++) {
        if (StrEq(c->fieldInfos[i].ownerType, sig->contextType.baseName)) {
            fieldCount++;
        }
    }

    if (fieldCount == 0) {
        if (c->hasCurrentContext && TypeRefEqual(&c->currentContextType, &sig->contextType)) {
            return BufAppendCStr(&c->out, "context");
        }
        return -1;
    }

    if (BufAppendCStr(&c->out, "(&((") != 0) {
        return -1;
    }
    if (EmitTypeNameWithDepth(c, &sig->contextType) != 0 || BufAppendCStr(&c->out, "){") != 0) {
        return -1;
    }
    {
        int first = 1;
        for (i = 0; i < c->fieldInfoLen; i++) {
            const SLFieldInfo* f = &c->fieldInfos[i];
            if (!StrEq(f->ownerType, sig->contextType.baseName)) {
                continue;
            }
            if (!first && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            first = 0;
            if (BufAppendChar(&c->out, '.') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
                || BufAppendCStr(&c->out, " = ") != 0)
            {
                return -1;
            }
            if (EmitEffectiveContextFieldValue(c, f->fieldName, &f->type) != 0) {
                return -1;
            }
        }
    }
    return BufAppendCStr(&c->out, "}))");
}

int EmitResolvedCall(
    SLCBackendC*          c,
    int32_t               callNode,
    const char*           calleeName,
    const SLFnSig*        sig,
    const SLCCallBinding* binding,
    int                   autoRefFirstArg) {
    uint32_t i;
    int      isStrFormatCall = 0;
    (void)callNode;
    if (sig == NULL || binding == NULL) {
        return -1;
    }
    if ((StrEq(calleeName, "format") || StrEq(calleeName, "str__format")
         || StrHasSuffix(calleeName, "__format"))
        && sig->isVariadic && sig->paramLen >= 3
        && sig->paramTypes[0].containerKind == SLTypeContainer_SLICE_MUT
        && sig->paramTypes[0].containerPtrDepth > 0
        && sig->paramTypes[1].containerKind == SLTypeContainer_SCALAR
        && sig->paramTypes[1].containerPtrDepth == 0 && sig->paramTypes[1].ptrDepth > 0
        && sig->paramTypes[1].baseName != NULL && IsStrBaseName(sig->paramTypes[1].baseName))
    {
        isStrFormatCall = 1;
    }
    if (isStrFormatCall) {
        int32_t          outNode = binding->fixedMappedArgNodes[0];
        int32_t          fmtNode = binding->fixedMappedArgNodes[1];
        int32_t          argNodes[SLCCG_MAX_CALL_ARGS];
        uint32_t         argCount = 0;
        const uint8_t*   fmtBytes = NULL;
        uint32_t         fmtLen = 0;
        uint32_t         fmtStart = 0;
        uint32_t         fmtEnd = 0;
        const SLAstNode* fmtAstNode = NULL;
        SLFmtToken       tokens[512];
        uint32_t         tokenLen = 0;
        uint32_t         placeholderCount = 0;
        SLFmtParseError  parseErr = { 0 };
        uint32_t         placeholderIndex = 0;
        if (outNode < 0 || fmtNode < 0 || binding->spreadArgIndex != UINT32_MAX) {
            return -1;
        }
        fmtAstNode = NodeAt(c, fmtNode);
        if (fmtAstNode != NULL) {
            fmtStart = fmtAstNode->start;
            fmtEnd = fmtAstNode->end;
        }
        if (EvalConstStringExpr(c, fmtNode, &fmtBytes, &fmtLen, &fmtAstNode) != 0) {
            if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, fmtStart, fmtEnd);
            }
            return -1;
        }
        if (fmtAstNode != NULL) {
            fmtStart = fmtAstNode->start;
            fmtEnd = fmtAstNode->end;
        }
        if (SLFmtParseBytes(
                fmtBytes,
                fmtLen,
                tokens,
                (uint32_t)(sizeof(tokens) / sizeof(tokens[0])),
                &tokenLen,
                &parseErr)
            != 0)
        {
            if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, fmtStart, fmtEnd);
            }
            return -1;
        }
        for (i = 0; i < binding->explicitTailCount; i++) {
            argNodes[argCount++] = binding->explicitTailNodes[i];
        }
        for (i = 0; i < tokenLen; i++) {
            if (tokens[i].kind == SLFmtTok_PLACEHOLDER_I || tokens[i].kind == SLFmtTok_PLACEHOLDER_F
                || tokens[i].kind == SLFmtTok_PLACEHOLDER_S)
            {
                placeholderCount++;
            } else if (tokens[i].kind == SLFmtTok_PLACEHOLDER_R) {
                if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                    SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, fmtStart, fmtEnd);
                }
                return -1;
            }
        }
        if (placeholderCount != argCount) {
            if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, fmtStart, fmtEnd);
            }
            return -1;
        }
        if (BufAppendCStr(&c->out, "(__extension__({\n    __auto_type __sl_fmt_out = ") != 0
            || EmitExprCoerced(c, outNode, &sig->paramTypes[0]) != 0
            || BufAppendCStr(
                   &c->out,
                   ";\n"
                   "    __sl_u8* __sl_fmt_buf = (__sl_u8*)(__sl_fmt_out.ptr);\n"
                   "    __sl_uint __sl_fmt_cap = (__sl_uint)(__sl_fmt_out.len);\n"
                   "    __sl_uint __sl_fmt_write_cap = __sl_fmt_cap > 0u ? (__sl_fmt_cap - 1u) : "
                   "0u;\n"
                   "    __sl_uint __sl_fmt_len = 0u;\n")
                   != 0)
        {
            return -1;
        }
        for (i = 0; i < tokenLen; i++) {
            const SLFmtToken* tok = &tokens[i];
            if (tok->kind == SLFmtTok_LITERAL) {
                if (tok->end > tok->start
                    && EmitFormatBufAppendLiteralToken(
                           c,
                           "__sl_fmt_buf",
                           "__sl_fmt_write_cap",
                           "__sl_fmt_len",
                           fmtBytes + tok->start,
                           tok->end - tok->start)
                           != 0)
                {
                    return -1;
                }
                continue;
            }
            if (tok->kind == SLFmtTok_PLACEHOLDER_I) {
                SLTypeRef argType;
                if (placeholderIndex >= argCount
                    || InferExprType(c, argNodes[placeholderIndex], &argType) != 0
                    || !argType.valid)
                {
                    return -1;
                }
                if (TypeRefIsSignedIntegerLike(c, &argType)) {
                    if (BufAppendCStr(
                            &c->out,
                            "    __sl_fmt_buf_append_i64(__sl_fmt_buf, __sl_fmt_write_cap, "
                            "&__sl_fmt_len, (__sl_i64)(")
                            != 0
                        || EmitExpr(c, argNodes[placeholderIndex]) != 0
                        || BufAppendCStr(&c->out, "));\n") != 0)
                    {
                        return -1;
                    }
                } else {
                    if (BufAppendCStr(
                            &c->out,
                            "    __sl_fmt_buf_append_u64(__sl_fmt_buf, __sl_fmt_write_cap, "
                            "&__sl_fmt_len, (__sl_u64)(")
                            != 0
                        || EmitExpr(c, argNodes[placeholderIndex]) != 0
                        || BufAppendCStr(&c->out, "));\n") != 0)
                    {
                        return -1;
                    }
                }
                placeholderIndex++;
                continue;
            }
            if (tok->kind == SLFmtTok_PLACEHOLDER_F) {
                if (placeholderIndex >= argCount) {
                    return -1;
                }
                if (BufAppendCStr(
                        &c->out,
                        "    __sl_fmt_buf_append_f64g(__sl_fmt_buf, __sl_fmt_write_cap, "
                        "&__sl_fmt_len, (__sl_f64)(")
                        != 0
                    || EmitExpr(c, argNodes[placeholderIndex]) != 0
                    || BufAppendCStr(&c->out, "));\n") != 0)
                {
                    return -1;
                }
                placeholderIndex++;
                continue;
            }
            if (tok->kind == SLFmtTok_PLACEHOLDER_S) {
                uint32_t tempId;
                SLBuf    nameBuf = { 0 };
                char*    name;
                if (placeholderIndex >= argCount) {
                    return -1;
                }
                tempId = FmtNextTempId(c);
                nameBuf.arena = &c->arena;
                if (BufAppendCStr(&nameBuf, "__sl_fmt_s") != 0
                    || BufAppendU32(&nameBuf, tempId) != 0)
                {
                    return -1;
                }
                name = BufFinish(&nameBuf);
                if (name == NULL) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "    __auto_type ") != 0
                    || BufAppendCStr(&c->out, name) != 0 || BufAppendCStr(&c->out, " = ") != 0
                    || EmitExprCoerced(c, argNodes[placeholderIndex], &sig->paramTypes[1]) != 0
                    || BufAppendCStr(
                           &c->out,
                           ";\n"
                           "    __sl_fmt_buf_append_bytes(__sl_fmt_buf, __sl_fmt_write_cap, "
                           "&__sl_fmt_len, __sl_cstr(")
                           != 0
                    || BufAppendCStr(&c->out, name) != 0
                    || BufAppendCStr(&c->out, "), __sl_len(") != 0
                    || BufAppendCStr(&c->out, name) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
                {
                    return -1;
                }
                placeholderIndex++;
                continue;
            }
        }
        if (BufAppendCStr(
                &c->out,
                "    if (__sl_fmt_cap > 0u) {\n"
                "        __sl_uint __sl_fmt_nul = "
                "__sl_fmt_len < __sl_fmt_cap ? __sl_fmt_len : (__sl_fmt_cap - 1u);\n"
                "        __sl_fmt_buf[__sl_fmt_nul] = (__sl_u8)0;\n"
                "    }\n"
                "    (__sl_uint)__sl_fmt_len;\n"
                "}))")
            != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, calleeName) != 0 || BufAppendChar(&c->out, '(') != 0) {
        return -1;
    }
    if (sig != NULL && sig->hasContext) {
        if (EmitContextArgForSig(c, sig) != 0) {
            return -1;
        }
        if (sig->paramLen > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
    }
    for (i = 0; i < sig->paramLen; i++) {
        int32_t argNode = -1;
        if (i != 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (!binding->isVariadic || i < binding->fixedCount) {
            argNode = binding->fixedMappedArgNodes[i];
            if (argNode < 0) {
                return -1;
            }
            if (autoRefFirstArg && i == 0) {
                if (BufAppendCStr(&c->out, "((") != 0
                    || EmitTypeNameWithDepth(c, &sig->paramTypes[i]) != 0
                    || BufAppendCStr(&c->out, ")(&(") != 0 || EmitExpr(c, argNode) != 0
                    || BufAppendCStr(&c->out, ")))") != 0)
                {
                    return -1;
                }
            } else if (EmitExprCoerced(c, argNode, &sig->paramTypes[i]) != 0) {
                return -1;
            }
            continue;
        }

        if (!binding->isVariadic || i != binding->fixedCount) {
            return -1;
        }

        if (binding->spreadArgIndex != UINT32_MAX) {
            argNode = binding->fixedMappedArgNodes[i];
            if (argNode < 0 || EmitExprCoerced(c, argNode, &sig->paramTypes[i]) != 0) {
                return -1;
            }
            continue;
        }

        if (binding->explicitTailCount == 0) {
            if (sig->paramTypes[i].containerKind == SLTypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__sl_slice_mut){ (void*)NULL, (__sl_uint)0u })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)NULL, (__sl_uint)0u })")
                    != 0)
                {
                    return -1;
                }
            }
            continue;
        }

        {
            SLTypeRef elemType = sig->paramTypes[i];
            uint32_t  j;
            elemType.containerKind = SLTypeContainer_SCALAR;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            if (BufAppendCStr(&c->out, "(__extension__({ ") != 0
                || EmitTypeNameWithDepth(c, &elemType) != 0
                || BufAppendCStr(&c->out, " __sl_va[") != 0
                || BufAppendU32(&c->out, binding->explicitTailCount) != 0
                || BufAppendCStr(&c->out, "] = { ") != 0)
            {
                return -1;
            }
            for (j = 0; j < binding->explicitTailCount; j++) {
                if (j > 0 && BufAppendCStr(&c->out, ", ") != 0) {
                    return -1;
                }
                if (EmitExprCoerced(c, binding->explicitTailNodes[j], &elemType) != 0) {
                    return -1;
                }
            }
            if (sig->paramTypes[i].containerKind == SLTypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, " }; ((__sl_slice_mut){ (void*)(__sl_va), (__sl_uint)(")
                        != 0
                    || BufAppendU32(&c->out, binding->explicitTailCount) != 0
                    || BufAppendCStr(&c->out, "u) }); }))") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(
                        &c->out, " }; ((__sl_slice_ro){ (const void*)(__sl_va), (__sl_uint)(")
                        != 0
                    || BufAppendU32(&c->out, binding->explicitTailCount) != 0
                    || BufAppendCStr(&c->out, "u) }); }))") != 0)
                {
                    return -1;
                }
            }
        }
    }
    return BufAppendChar(&c->out, ')');
}

int EmitFieldPathLValue(
    SLCBackendC* c, const char* base, const SLFieldInfo* const* path, uint32_t pathLen) {
    uint32_t i;
    if (BufAppendCStr(&c->out, base) != 0) {
        return -1;
    }
    for (i = 0; i < pathLen; i++) {
        if (BufAppendChar(&c->out, '.') != 0 || BufAppendCStr(&c->out, path[i]->fieldName) != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitCompoundFieldValueCoerced(
    SLCBackendC* c, const SLAstNode* field, int32_t exprNode, const SLTypeRef* _Nullable dstType) {
    if (exprNode >= 0) {
        return EmitExprCoerced(c, exprNode, dstType);
    }
    if (field == NULL || (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
        return -1;
    }
    return BufAppendSlice(&c->out, c->unit->source, field->dataStart, field->dataEnd);
}

int EmitEnumVariantCompoundLiteral(
    SLCBackendC*     c,
    int32_t          nodeId,
    int32_t          firstField,
    const char*      enumTypeName,
    uint32_t         variantStart,
    uint32_t         variantEnd,
    const SLTypeRef* valueType) {
    int32_t fieldNode = firstField;
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, valueType) != 0
        || BufAppendCStr(&c->out, "){ .tag = ") != 0 || BufAppendCStr(&c->out, enumTypeName) != 0
        || BufAppendCStr(&c->out, "__") != 0
        || BufAppendSlice(&c->out, c->unit->source, variantStart, variantEnd) != 0)
    {
        return -1;
    }
    while (fieldNode >= 0) {
        const SLAstNode* field = NodeAt(c, fieldNode);
        int32_t          exprNode;
        SLTypeRef        fieldType;
        if (field == NULL || field->kind != SLAst_COMPOUND_FIELD) {
            return -1;
        }
        exprNode = AstFirstChild(&c->ast, fieldNode);
        if (exprNode < 0 && (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
            return -1;
        }
        if (ResolveEnumVariantPayloadFieldType(
                c,
                enumTypeName,
                variantStart,
                variantEnd,
                field->dataStart,
                field->dataEnd,
                &fieldType)
            != 0)
        {
            return -1;
        }
        if (BufAppendCStr(&c->out, ", .payload.") != 0
            || BufAppendSlice(&c->out, c->unit->source, variantStart, variantEnd) != 0
            || BufAppendChar(&c->out, '.') != 0
            || BufAppendSlice(&c->out, c->unit->source, field->dataStart, field->dataEnd) != 0
            || BufAppendCStr(&c->out, " = ") != 0
            || EmitCompoundFieldValueCoerced(c, field, exprNode, &fieldType) != 0)
        {
            return -1;
        }
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }
    (void)nodeId;
    return BufAppendCStr(&c->out, "})");
}

int EmitCompoundLiteralDesignated(
    SLCBackendC* c, int32_t firstField, const char* ownerType, const SLTypeRef* valueType) {
    int32_t fieldNode = firstField;
    int     first = 1;

    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, valueType) != 0
        || BufAppendCStr(&c->out, "){") != 0)
    {
        return -1;
    }

    while (fieldNode >= 0) {
        const SLAstNode*   field = NodeAt(c, fieldNode);
        const SLFieldInfo* fieldPath[64];
        const SLFieldInfo* resolvedField = NULL;
        uint32_t           fieldPathLen = 0;
        int32_t            exprNode;
        if (field == NULL || field->kind != SLAst_COMPOUND_FIELD) {
            return -1;
        }
        exprNode = AstFirstChild(&c->ast, fieldNode);
        if (exprNode < 0 && (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
            return -1;
        }
        if (ResolveFieldPathBySlice(
                c,
                ownerType,
                field->dataStart,
                field->dataEnd,
                fieldPath,
                (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                &fieldPathLen,
                &resolvedField)
                != 0
            || fieldPathLen == 0)
        {
            return -1;
        }
        resolvedField = fieldPath[fieldPathLen - 1u];
        if (!first && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        first = 0;
        if (BufAppendChar(&c->out, '.') != 0
            || BufAppendCStr(&c->out, resolvedField->fieldName) != 0
            || BufAppendCStr(&c->out, " = ") != 0
            || EmitCompoundFieldValueCoerced(c, field, exprNode, &resolvedField->type) != 0)
        {
            return -1;
        }
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }

    return BufAppendCStr(&c->out, "})");
}

int EmitCompoundLiteralOrderedStruct(
    SLCBackendC* c, int32_t firstField, const char* ownerType, const SLTypeRef* valueType) {
    const SLFieldInfo* directFields[256];
    uint8_t            directExplicit[256];
    uint32_t           directCount = 0;
    uint32_t           i;
    uint32_t           tempIndex = 0;
    int32_t            fieldNode = firstField;

    for (i = 0; i < c->fieldInfoLen; i++) {
        const SLFieldInfo* f = &c->fieldInfos[i];
        if (!StrEq(f->ownerType, ownerType) || f->isEmbedded) {
            continue;
        }
        if (directCount >= (uint32_t)(sizeof(directFields) / sizeof(directFields[0]))) {
            return -1;
        }
        directFields[directCount] = f;
        directExplicit[directCount] = 0;
        directCount++;
    }

    if (PushScope(c) != 0) {
        return -1;
    }

    if (BufAppendCStr(&c->out, "(__extension__({\n    ") != 0
        || EmitTypeNameWithDepth(c, valueType) != 0
        || BufAppendCStr(&c->out, " __sl_tmp = {0};\n") != 0)
    {
        PopScope(c);
        return -1;
    }

    while (fieldNode >= 0) {
        const SLAstNode*   field = NodeAt(c, fieldNode);
        const SLFieldInfo* fieldPath[64];
        const SLFieldInfo* resolvedField = NULL;
        uint32_t           fieldPathLen = 0;
        int32_t            exprNode;

        if (field == NULL || field->kind != SLAst_COMPOUND_FIELD) {
            PopScope(c);
            return -1;
        }
        exprNode = AstFirstChild(&c->ast, fieldNode);
        if (exprNode < 0 && (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
            PopScope(c);
            return -1;
        }
        if (ResolveFieldPathBySlice(
                c,
                ownerType,
                field->dataStart,
                field->dataEnd,
                fieldPath,
                (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                &fieldPathLen,
                &resolvedField)
                != 0
            || fieldPathLen == 0)
        {
            PopScope(c);
            return -1;
        }
        resolvedField = fieldPath[fieldPathLen - 1u];

        if (BufAppendCStr(&c->out, "    ") != 0
            || EmitTypeNameWithDepth(c, &resolvedField->type) != 0
            || BufAppendCStr(&c->out, " __sl_exp_") != 0 || BufAppendU32(&c->out, tempIndex) != 0
            || BufAppendCStr(&c->out, " = ") != 0
            || EmitCompoundFieldValueCoerced(c, field, exprNode, &resolvedField->type) != 0
            || BufAppendCStr(&c->out, ";\n    ") != 0
            || EmitFieldPathLValue(c, "__sl_tmp", fieldPath, fieldPathLen) != 0
            || BufAppendCStr(&c->out, " = __sl_exp_") != 0 || BufAppendU32(&c->out, tempIndex) != 0
            || BufAppendCStr(&c->out, ";\n") != 0)
        {
            PopScope(c);
            return -1;
        }

        if (fieldPathLen == 1u) {
            for (i = 0; i < directCount; i++) {
                if (StrEq(directFields[i]->fieldName, resolvedField->fieldName)) {
                    directExplicit[i] = 1;
                    break;
                }
            }
        }
        tempIndex++;
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }

    for (i = 0; i < directCount; i++) {
        const SLFieldInfo* f = directFields[i];
        if (!directExplicit[i] && f->defaultExprNode >= 0) {
            if (BufAppendCStr(&c->out, "    __sl_tmp.") != 0
                || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, " = ") != 0
                || EmitExprCoerced(c, f->defaultExprNode, &f->type) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                PopScope(c);
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, "    ") != 0 || EmitTypeNameWithDepth(c, &f->type) != 0
            || BufAppendChar(&c->out, ' ') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
            || BufAppendCStr(&c->out, " = __sl_tmp.") != 0
            || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            PopScope(c);
            return -1;
        }
        if (AddLocal(c, f->fieldName, f->type) != 0) {
            PopScope(c);
            return -1;
        }
    }

    if (BufAppendCStr(&c->out, "    __sl_tmp;\n}))") != 0) {
        PopScope(c);
        return -1;
    }

    PopScope(c);
    return 0;
}

int StructHasFieldDefaults(const SLCBackendC* c, const char* ownerType) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        const SLFieldInfo* f = &c->fieldInfos[i];
        if (StrEq(f->ownerType, ownerType) && !f->isEmbedded && f->defaultExprNode >= 0) {
            return 1;
        }
    }
    return 0;
}

int EmitCompoundLiteral(SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType) {
    const SLAstNode* litNode = NodeAt(c, nodeId);
    SLTypeRef        litType;
    SLTypeRef        valueType;
    const char*      ownerType;
    const SLNameMap* ownerMap;
    int32_t          fieldNode;
    int32_t          typeNode = -1;

    if (litNode == NULL || litNode->kind != SLAst_COMPOUND_LIT) {
        return -1;
    }
    if (InferCompoundLiteralType(c, nodeId, expectedType, &litType) != 0 || !litType.valid) {
        return -1;
    }

    valueType = litType;
    if (valueType.containerKind != SLTypeContainer_SCALAR) {
        if (valueType.containerPtrDepth <= 0) {
            return -1;
        }
        valueType.containerPtrDepth--;
    } else if (valueType.ptrDepth > 0) {
        valueType.ptrDepth--;
    }
    if (!valueType.valid || valueType.containerKind != SLTypeContainer_SCALAR
        || valueType.containerPtrDepth != 0 || valueType.ptrDepth != 0
        || valueType.baseName == NULL)
    {
        return -1;
    }
    ownerType = ResolveScalarAliasBaseName(c, valueType.baseName);
    if (ownerType == NULL) {
        return -1;
    }
    valueType.baseName = ownerType;
    ownerMap = FindNameByCName(c, ownerType);

    fieldNode = AstFirstChild(&c->ast, nodeId);
    if (fieldNode >= 0 && NodeAt(c, fieldNode) != NULL
        && IsTypeNodeKind(NodeAt(c, fieldNode)->kind))
    {
        typeNode = fieldNode;
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }

    if (ownerMap != NULL && ownerMap->kind == SLAst_ENUM) {
        const char* enumTypeName = NULL;
        uint32_t    variantStart = 0;
        uint32_t    variantEnd = 0;
        int         variantRc;
        if (typeNode < 0) {
            return -1;
        }
        variantRc = ResolveEnumVariantTypeNameNode(
            c, typeNode, &enumTypeName, &variantStart, &variantEnd);
        if (variantRc != 1 || enumTypeName == NULL) {
            return -1;
        }
        return EmitEnumVariantCompoundLiteral(
            c, nodeId, fieldNode, enumTypeName, variantStart, variantEnd, &valueType);
    }

    if (ownerMap != NULL && ownerMap->kind == SLAst_STRUCT && StructHasFieldDefaults(c, ownerType))
    {
        return EmitCompoundLiteralOrderedStruct(c, fieldNode, ownerType, &valueType);
    }
    return EmitCompoundLiteralDesignated(c, fieldNode, ownerType, &valueType);
}

int EmitExpr_IDENT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t topVarLikeNode = -1;
    (void)nodeId;
    if (FindLocalBySlice(c, n->dataStart, n->dataEnd) != NULL
        || FindFnSigBySlice(c, n->dataStart, n->dataEnd) != NULL
        || FindTopLevelVarLikeNodeBySlice(c, n->dataStart, n->dataEnd, &topVarLikeNode) == 0)
    {
        return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
    }
    {
        SLTypeRef typeValue;
        if (ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, &typeValue)) {
            return EmitTypeTagLiteralFromTypeRef(c, &typeValue);
        }
    }
    return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
}

int EmitExpr_INT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    (void)nodeId;
    return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
}

int EmitExpr_RUNE(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    uint32_t     rune = 0;
    SLRuneLitErr runeErr = { 0 };
    (void)nodeId;
    if (SLDecodeRuneLiteralValidate(c->unit->source, n->dataStart, n->dataEnd, &rune, &runeErr)
        != 0)
    {
        SetDiag(c->diag, SLRuneLitErrDiagCode(runeErr.kind), runeErr.start, runeErr.end);
        return -1;
    }
    return BufAppendU32(&c->out, rune);
}

int EmitExpr_FLOAT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    (void)nodeId;
    return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
}

int EmitExpr_BOOL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    (void)nodeId;
    if (n->dataEnd > n->dataStart && c->unit->source[n->dataStart] == 't') {
        return BufAppendCStr(&c->out, "1");
    }
    return BufAppendCStr(&c->out, "0");
}

int EmitExpr_COMPOUND_LIT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    (void)n;
    return EmitCompoundLiteral(c, nodeId, NULL);
}

int EmitExpr_STRING(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t literalId = -1;
    (void)n;
    if (nodeId >= 0 && (uint32_t)nodeId < c->stringLitByNodeLen) {
        literalId = c->stringLitByNode[nodeId];
    }
    if (literalId < 0) {
        return -1;
    }
    return EmitStringLiteralRef(c, literalId, 0);
}

int EmitExpr_UNARY(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    if ((SLTokenKind)n->op == SLTok_AND && child >= 0) {
        const SLAstNode* cn = NodeAt(c, child);
        if (cn != NULL && cn->kind == SLAst_FIELD_EXPR) {
            int32_t            recv = AstFirstChild(&c->ast, child);
            SLTypeRef          recvType;
            SLTypeRef          ownerType;
            const SLFieldInfo* fieldPath[64];
            uint32_t           fieldPathLen = 0;
            const SLFieldInfo* field = NULL;
            SLTypeRef          childType;
            if (InferExprType(c, recv, &recvType) == 0 && recvType.valid) {
                ownerType = recvType;
                if (ownerType.ptrDepth > 0) {
                    ownerType.ptrDepth--;
                }
                if (ownerType.baseName != NULL) {
                    if (ResolveFieldPathBySlice(
                            c,
                            ownerType.baseName,
                            cn->dataStart,
                            cn->dataEnd,
                            fieldPath,
                            (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                            &fieldPathLen,
                            &field)
                            == 0
                        && fieldPathLen > 0)
                    {
                        field = fieldPath[fieldPathLen - 1u];
                    } else {
                        field = NULL;
                    }
                }
            }
            if (field != NULL && field->isDependent && InferExprType(c, child, &childType) == 0
                && childType.valid)
            {
                if (BufAppendCStr(&c->out, "(&(") != 0 || EmitTypeNameWithDepth(c, &childType) != 0
                    || BufAppendCStr(&c->out, "){") != 0 || EmitExpr(c, child) != 0
                    || BufAppendCStr(&c->out, "})") != 0)
                {
                    return -1;
                }
                return 0;
            }
        }
        {
            SLTypeRef childType;
            if (InferExprType(c, child, &childType) == 0 && childType.valid
                && childType.containerKind == SLTypeContainer_ARRAY
                && childType.containerPtrDepth == 0)
            {
                return EmitElemPtrExpr(c, child, &childType, TypeRefContainerWritable(&childType));
            }
        }
    }
    if ((SLTokenKind)n->op == SLTok_MUL && child >= 0) {
        SLTypeRef childType;
        if (InferExprType(c, child, &childType) == 0 && childType.valid
            && TypeRefIsOwnedRuntimeArrayStruct(&childType))
        {
            if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, child) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (BufAppendChar(&c->out, '(') != 0
        || BufAppendCStr(&c->out, UnaryOpString((SLTokenKind)n->op)) != 0 || EmitExpr(c, child) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_BINARY(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t     lhs = AstFirstChild(&c->ast, nodeId);
    int32_t     rhs = AstNextSibling(&c->ast, lhs);
    SLTokenKind op = (SLTokenKind)n->op;
    if (op == SLTok_ADD) {
        int32_t literalId = -1;
        if (nodeId >= 0 && (uint32_t)nodeId < c->stringLitByNodeLen) {
            literalId = c->stringLitByNode[nodeId];
        }
        if (literalId >= 0) {
            return EmitStringLiteralRef(c, literalId, 0);
        }
    }
    if (op == SLTok_ASSIGN) {
        SLTypeRef lhsType;
        if (lhs < 0 || rhs < 0) {
            return -1;
        }
        if (InferExprType(c, lhs, &lhsType) != 0 || !lhsType.valid) {
            goto emit_raw_binary;
        }
        if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, lhs) != 0
            || BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, rhs, &lhsType) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (op == SLTok_EQ || op == SLTok_NEQ || op == SLTok_LT || op == SLTok_LTE || op == SLTok_GT
        || op == SLTok_GTE)
    {
        SLTypeRef        lhsType;
        SLTypeRef        rhsType;
        const SLFnSig*   hookSig = NULL;
        const char*      hookCalleeName = NULL;
        int              hookAutoRef[2] = { 0, 0 };
        int              hookStatus;
        int              lhsNull;
        int              rhsNull;
        int              isEqOp = (op == SLTok_EQ || op == SLTok_NEQ);
        const SLTypeRef* seqType = NULL;
        lhsNull = lhs >= 0 && NodeAt(c, lhs) != NULL && NodeAt(c, lhs)->kind == SLAst_NULL;
        rhsNull = rhs >= 0 && NodeAt(c, rhs) != NULL && NodeAt(c, rhs)->kind == SLAst_NULL;
        if (lhs < 0 || rhs < 0 || InferExprType(c, lhs, &lhsType) != 0
            || InferExprType(c, rhs, &rhsType) != 0)
        {
            goto emit_raw_binary;
        }
        if ((!lhsType.valid && !lhsNull) || (!rhsType.valid && !rhsNull)) {
            goto emit_raw_binary;
        }

        {
            const char* lhsPayloadEnum = NULL;
            const char* rhsPayloadEnum = NULL;
            if (ResolvePayloadEnumType(c, &lhsType, &lhsPayloadEnum)
                && ResolvePayloadEnumType(c, &rhsType, &rhsPayloadEnum) && lhsPayloadEnum != NULL
                && rhsPayloadEnum != NULL && StrEq(lhsPayloadEnum, rhsPayloadEnum))
            {
                if (isEqOp) {
                    if (op == SLTok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                        return -1;
                    }
                    if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __sl_cmp_a = ") != 0
                        || EmitExprCoerced(c, lhs, &lhsType) != 0
                        || BufAppendCStr(&c->out, "; __auto_type __sl_cmp_b = ") != 0
                        || EmitExprCoerced(c, rhs, &lhsType) != 0
                        || BufAppendCStr(
                               &c->out,
                               "; __sl_mem_equal((const void*)&__sl_cmp_a, (const "
                               "void*)&__sl_cmp_b, (__sl_uint)sizeof(__sl_cmp_a)); }))")
                               != 0)
                    {
                        return -1;
                    }
                    if (op == SLTok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                        return -1;
                    }
                    return 0;
                }
                if (BufAppendCStr(&c->out, "((") != 0 || EmitExprCoerced(c, lhs, &lhsType) != 0
                    || BufAppendCStr(&c->out, ").tag ") != 0
                    || BufAppendCStr(&c->out, BinaryOpString(op)) != 0
                    || BufAppendCStr(&c->out, " (") != 0 || EmitExprCoerced(c, rhs, &lhsType) != 0
                    || BufAppendCStr(&c->out, ").tag)") != 0)
                {
                    return -1;
                }
                return 0;
            }
        }

        if (isEqOp) {
            if (TypeRefIsOwnedRuntimeArrayStruct(&lhsType) && rhsNull) {
                if (BufAppendCStr(&c->out, "(((") != 0 || EmitExpr(c, lhs) != 0
                    || BufAppendCStr(&c->out, ").ptr) ") != 0
                    || BufAppendCStr(&c->out, BinaryOpString(op)) != 0
                    || BufAppendCStr(&c->out, " NULL)") != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (TypeRefIsOwnedRuntimeArrayStruct(&rhsType) && lhsNull) {
                if (BufAppendCStr(&c->out, "(NULL ") != 0
                    || BufAppendCStr(&c->out, BinaryOpString(op)) != 0
                    || BufAppendCStr(&c->out, " ((") != 0 || EmitExpr(c, rhs) != 0
                    || BufAppendCStr(&c->out, ").ptr))") != 0)
                {
                    return -1;
                }
                return 0;
            }
        }

        hookStatus = ResolveComparisonHook(
            c,
            isEqOp ? "__equal" : "__order",
            &lhsType,
            &rhsType,
            &hookSig,
            &hookCalleeName,
            hookAutoRef);
        if (hookStatus == 0) {
            if (isEqOp) {
                if (op == SLTok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                    return -1;
                }
                if (BufAppendChar(&c->out, '(') != 0
                    || EmitComparisonHookCall(c, hookSig, hookCalleeName, lhs, rhs, hookAutoRef)
                           != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                if (op == SLTok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                    return -1;
                }
                return 0;
            }
            if (BufAppendChar(&c->out, '(') != 0
                || EmitComparisonHookCall(c, hookSig, hookCalleeName, lhs, rhs, hookAutoRef) != 0
                || BufAppendCStr(&c->out, " ") != 0)
            {
                return -1;
            }
            switch (op) {
                case SLTok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GTE:
                    if (BufAppendCStr(&c->out, ">= 0)") != 0) {
                        return -1;
                    }
                    break;
                default: return -1;
            }
            return 0;
        }

        if (TypeRefIsStr(&lhsType) || TypeRefIsStr(&rhsType)) {
            if (isEqOp) {
                if (op == SLTok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_str_equal((const __sl_str*)(") != 0
                    || EmitExprCoerced(c, lhs, &lhsType) != 0
                    || BufAppendCStr(&c->out, "), (const __sl_str*)(") != 0
                    || EmitExprCoerced(c, rhs, &lhsType) != 0 || BufAppendCStr(&c->out, "))") != 0)
                {
                    return -1;
                }
                if (op == SLTok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                    return -1;
                }
                return 0;
            }
            if (BufAppendCStr(&c->out, "(__sl_str_order((const __sl_str*)(") != 0
                || EmitExprCoerced(c, lhs, &lhsType) != 0
                || BufAppendCStr(&c->out, "), (const __sl_str*)(") != 0
                || EmitExprCoerced(c, rhs, &lhsType) != 0 || BufAppendCStr(&c->out, ")) ") != 0)
            {
                return -1;
            }
            switch (op) {
                case SLTok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GTE:
                    if (BufAppendCStr(&c->out, ">= 0)") != 0) {
                        return -1;
                    }
                    break;
                default: return -1;
            }
            return 0;
        }

        if ((TypeRefIsPointerLike(&lhsType) && TypeRefIsPointerLike(&rhsType))
            || (TypeRefIsPointerLike(&lhsType) && rhsNull)
            || (TypeRefIsPointerLike(&rhsType) && lhsNull))
        {
            if (isEqOp) {
                if (BufAppendChar(&c->out, '(') != 0) {
                    return -1;
                }
                if (lhsNull) {
                    if (BufAppendCStr(&c->out, "NULL") != 0) {
                        return -1;
                    }
                } else if (EmitPointerIdentityExpr(c, lhs, &lhsType) != 0) {
                    return -1;
                }
                if (BufAppendChar(&c->out, ' ') != 0
                    || BufAppendCStr(&c->out, BinaryOpString(op)) != 0
                    || BufAppendChar(&c->out, ' ') != 0)
                {
                    return -1;
                }
                if (rhsNull) {
                    if (BufAppendCStr(&c->out, "NULL") != 0) {
                        return -1;
                    }
                } else if (EmitPointerIdentityExpr(c, rhs, &rhsType) != 0) {
                    return -1;
                }
                if (BufAppendChar(&c->out, ')') != 0) {
                    return -1;
                }
                return 0;
            }
            if (BufAppendCStr(&c->out, "(__sl_ptr_order(") != 0) {
                return -1;
            }
            if (lhsNull) {
                if (BufAppendCStr(&c->out, "NULL") != 0) {
                    return -1;
                }
            } else if (EmitPointerIdentityExpr(c, lhs, &lhsType) != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (rhsNull) {
                if (BufAppendCStr(&c->out, "NULL") != 0) {
                    return -1;
                }
            } else if (EmitPointerIdentityExpr(c, rhs, &rhsType) != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, ") ") != 0) {
                return -1;
            }
            switch (op) {
                case SLTok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GTE:
                    if (BufAppendCStr(&c->out, ">= 0)") != 0) {
                        return -1;
                    }
                    break;
                default: return -1;
            }
            return 0;
        }

        if ((lhsType.containerKind == SLTypeContainer_ARRAY
             || lhsType.containerKind == SLTypeContainer_SLICE_RO
             || lhsType.containerKind == SLTypeContainer_SLICE_MUT)
            && !TypeRefIsPointerLike(&lhsType))
        {
            seqType = &lhsType;
            if (isEqOp) {
                if (op == SLTok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_slice_equal_ro(") != 0
                    || EmitExprAsSliceRO(c, lhs, &lhsType) != 0 || BufAppendCStr(&c->out, ", ") != 0
                    || EmitExprAsSliceRO(c, rhs, &rhsType) != 0
                    || BufAppendCStr(&c->out, ", (__sl_uint)sizeof(") != 0
                    || EmitElementTypeName(c, seqType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
                {
                    return -1;
                }
                if (op == SLTok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                    return -1;
                }
                return 0;
            }
            if (BufAppendCStr(&c->out, "(__sl_slice_order_ro(") != 0
                || EmitExprAsSliceRO(c, lhs, &lhsType) != 0 || BufAppendCStr(&c->out, ", ") != 0
                || EmitExprAsSliceRO(c, rhs, &rhsType) != 0
                || BufAppendCStr(&c->out, ", (__sl_uint)sizeof(") != 0
                || EmitElementTypeName(c, seqType, 0) != 0 || BufAppendCStr(&c->out, ")) ") != 0)
            {
                return -1;
            }
            switch (op) {
                case SLTok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case SLTok_GTE:
                    if (BufAppendCStr(&c->out, ">= 0)") != 0) {
                        return -1;
                    }
                    break;
                default: return -1;
            }
            return 0;
        }

        if (isEqOp
            && (TypeRefIsNamedDeclKind(c, &lhsType, SLAst_STRUCT)
                || TypeRefIsNamedDeclKind(c, &lhsType, SLAst_UNION)))
        {
            if (op == SLTok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __sl_cmp_a = ") != 0
                || EmitExprCoerced(c, lhs, &lhsType) != 0
                || BufAppendCStr(&c->out, "; __auto_type __sl_cmp_b = ") != 0
                || EmitExprCoerced(c, rhs, &lhsType) != 0
                || BufAppendCStr(
                       &c->out,
                       "; __sl_mem_equal((const void*)&__sl_cmp_a, (const "
                       "void*)&__sl_cmp_b, (__sl_uint)sizeof(__sl_cmp_a)); }))")
                       != 0)
            {
                return -1;
            }
            if (op == SLTok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                return -1;
            }
            return 0;
        }
    }
emit_raw_binary:
    if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, lhs) != 0
        || BufAppendChar(&c->out, ' ') != 0
        || BufAppendCStr(&c->out, BinaryOpString((SLTokenKind)n->op)) != 0
        || BufAppendChar(&c->out, ' ') != 0 || EmitExpr(c, rhs) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_CALL_WITH_CONTEXT(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t savedActive = c->activeCallWithNode;
    int32_t callNode = AstFirstChild(&c->ast, nodeId);
    int     rc;
    (void)n;
    if (callNode < 0 || NodeAt(c, callNode) == NULL || NodeAt(c, callNode)->kind != SLAst_CALL) {
        return -1;
    }
    c->activeCallWithNode = nodeId;
    rc = EmitExpr(c, callNode);
    c->activeCallWithNode = savedActive;
    return rc;
}

int EmitExpr_CALL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    const SLAstNode* callee = NodeAt(c, child);
    (void)n;
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "kind"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        SLTypeRef argType;
        SLTypeRef reflectedType;
        if (arg >= 0 && extra < 0) {
            if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
                return -1;
            }
            if (TypeRefIsTypeValue(&argType)) {
                if (ResolveReflectedTypeValueExprTypeRef(c, arg, &reflectedType)) {
                    return EmitTypeTagKindLiteralFromTypeRef(c, &reflectedType);
                }
                return EmitRuntimeTypeTagKindFromExpr(c, arg);
            }
        }
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "base"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        SLTypeRef argType;
        SLTypeRef reflectedType;
        if (arg >= 0 && extra < 0) {
            if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
                return -1;
            }
            if (TypeRefIsTypeValue(&argType)) {
                if (!ResolveReflectedTypeValueExprTypeRef(c, arg, &reflectedType)) {
                    return -1;
                }
                return EmitTypeTagBaseLiteralFromTypeRef(c, &reflectedType);
            }
        }
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "is_alias"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        SLTypeRef argType;
        SLTypeRef reflectedType;
        if (arg >= 0 && extra < 0) {
            if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
                return -1;
            }
            if (TypeRefIsTypeValue(&argType)) {
                if (ResolveReflectedTypeValueExprTypeRef(c, arg, &reflectedType)) {
                    return EmitTypeTagIsAliasLiteralFromTypeRef(c, &reflectedType);
                }
                return EmitRuntimeTypeTagIsAliasFromExpr(c, arg);
            }
        }
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "type_name"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        SLTypeRef argType;
        SLTypeRef reflectedType;
        if (arg >= 0 && extra < 0) {
            if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
                return -1;
            }
            if (TypeRefIsTypeValue(&argType)) {
                if (ResolveReflectedTypeValueExprTypeRef(c, arg, &reflectedType)) {
                    return EmitTypeNameStringLiteralFromTypeRef(c, &reflectedType);
                }
                return EmitTypeNameStringLiteralFromTypeRef(c, NULL);
            }
        }
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "typeof"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        SLTypeRef argType;
        if (arg < 0 || extra >= 0) {
            return -1;
        }
        if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
            return -1;
        }
        return EmitTypeTagLiteralFromTypeRef(c, &argType);
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "ptr")
            || SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "slice")
            || SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "array")))
    {
        int32_t   arg0 = AstNextSibling(&c->ast, child);
        int32_t   arg1 = arg0 >= 0 ? AstNextSibling(&c->ast, arg0) : -1;
        int32_t   arg2 = arg1 >= 0 ? AstNextSibling(&c->ast, arg1) : -1;
        SLTypeRef arg0Type;
        SLTypeRef arg1Type;
        int       isArray = SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "array");
        int       isPtr = SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "ptr");
        int       isSlice = SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "slice");
        int       builtinShape = 0;

        if (arg0 >= 0 && InferExprType(c, arg0, &arg0Type) == 0 && arg0Type.valid
            && TypeRefIsTypeValue(&arg0Type))
        {
            if (isArray) {
                const char* lenBase;
                if (arg1 >= 0 && arg2 < 0 && InferExprType(c, arg1, &arg1Type) == 0
                    && arg1Type.valid)
                {
                    lenBase = ResolveScalarAliasBaseName(c, arg1Type.baseName);
                    if (lenBase == NULL) {
                        lenBase = arg1Type.baseName;
                    }
                    builtinShape =
                        arg1Type.containerKind == SLTypeContainer_SCALAR && arg1Type.ptrDepth == 0
                        && arg1Type.containerPtrDepth == 0 && !arg1Type.isOptional
                        && lenBase != NULL && IsIntegerCTypeName(lenBase);
                }
            } else {
                builtinShape = arg1 < 0;
            }
        }
        if (builtinShape) {
            SLTypeRef reflectedType;
            if (ResolveReflectedTypeValueExprTypeRef(c, nodeId, &reflectedType)) {
                return EmitTypeTagLiteralFromTypeRef(c, &reflectedType);
            }
            if (isPtr) {
                return EmitRuntimeTypeTagCtorUnary(c, 6u, 0x9e3779b97f4a7c15ULL, arg0);
            }
            if (isSlice) {
                return EmitRuntimeTypeTagCtorUnary(c, 8u, 0x3243f6a8885a308dULL, arg0);
            }
            return EmitRuntimeTypeTagCtorArray(c, arg0, arg1);
        }
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "len"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        SLTypeRef argType;
        if (arg < 0 || extra >= 0) {
            return -1;
        }
        if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
            return -1;
        }
        return EmitLenExprFromType(c, arg, &argType);
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "cstr"))
    {
        int32_t arg = AstNextSibling(&c->ast, child);
        int32_t extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        if (arg < 0 || extra >= 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "__sl_cstr(") != 0 || EmitExpr(c, arg) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "concat"))
    {
        return EmitConcatCallExpr(c, child);
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "free"))
    {
        int32_t arg1 = AstNextSibling(&c->ast, child);
        int32_t arg2 = arg1 >= 0 ? AstNextSibling(&c->ast, arg1) : -1;
        int32_t arg3 = arg2 >= 0 ? AstNextSibling(&c->ast, arg2) : -1;
        if (arg1 < 0 || arg3 >= 0) {
            return -1;
        }
        if (arg2 >= 0) {
            return EmitFreeCallExpr(c, arg1, arg2);
        }
        return EmitFreeCallExpr(c, -1, arg1);
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "panic"))
    {
        int32_t msgArg = AstNextSibling(&c->ast, child);
        if (msgArg < 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "__sl_panic(") != 0 || EmitExpr(c, msgArg) != 0
            || BufAppendCStr(&c->out, ", __FILE__, __LINE__)") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "platform__exit"))
    {
        int32_t statusArg = AstNextSibling(&c->ast, child);
        int32_t extra = statusArg >= 0 ? AstNextSibling(&c->ast, statusArg) : -1;
        if (statusArg < 0 || extra >= 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "platform__exit((__sl_i32)(") != 0 || EmitExpr(c, statusArg) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (callee != NULL && callee->kind == SLAst_FIELD_EXPR) {
        int32_t            recvNode = AstFirstChild(&c->ast, child);
        SLTypeRef          recvType;
        SLTypeRef          ownerType;
        const SLFieldInfo* fieldPath[64];
        uint32_t           fieldPathLen = 0;
        const SLFieldInfo* field = NULL;
        int                hasField = 0;
        if (recvNode >= 0 && InferExprType(c, recvNode, &recvType) == 0 && recvType.valid) {
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "kind")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                SLTypeRef reflectedType;
                if (extra < 0 && TypeRefIsTypeValue(&recvType)) {
                    if (ResolveReflectedTypeValueExprTypeRef(c, recvNode, &reflectedType)) {
                        return EmitTypeTagKindLiteralFromTypeRef(c, &reflectedType);
                    }
                    return EmitRuntimeTypeTagKindFromExpr(c, recvNode);
                }
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "base")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                SLTypeRef reflectedType;
                if (extra < 0 && TypeRefIsTypeValue(&recvType)) {
                    if (!ResolveReflectedTypeValueExprTypeRef(c, recvNode, &reflectedType)) {
                        return -1;
                    }
                    return EmitTypeTagBaseLiteralFromTypeRef(c, &reflectedType);
                }
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "is_alias")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                SLTypeRef reflectedType;
                if (extra < 0 && TypeRefIsTypeValue(&recvType)) {
                    if (ResolveReflectedTypeValueExprTypeRef(c, recvNode, &reflectedType)) {
                        return EmitTypeTagIsAliasLiteralFromTypeRef(c, &reflectedType);
                    }
                    return EmitRuntimeTypeTagIsAliasFromExpr(c, recvNode);
                }
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "type_name")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                SLTypeRef reflectedType;
                if (extra < 0 && TypeRefIsTypeValue(&recvType)) {
                    if (ResolveReflectedTypeValueExprTypeRef(c, recvNode, &reflectedType)) {
                        return EmitTypeNameStringLiteralFromTypeRef(c, &reflectedType);
                    }
                    return EmitTypeNameStringLiteralFromTypeRef(c, NULL);
                }
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "len")) {
                int32_t extra = AstNextSibling(&c->ast, child);
                if (extra >= 0) {
                    return -1;
                }
                if (EmitLenExprFromType(c, recvNode, &recvType) == 0) {
                    return 0;
                }
            }
            ownerType = recvType;
            if (ownerType.containerKind != SLTypeContainer_SCALAR
                && ownerType.containerPtrDepth > 0)
            {
                ownerType.containerPtrDepth--;
            } else if (ownerType.ptrDepth > 0) {
                ownerType.ptrDepth--;
            }
            if (ownerType.baseName != NULL) {
                if (ResolveFieldPathBySlice(
                        c,
                        ownerType.baseName,
                        callee->dataStart,
                        callee->dataEnd,
                        fieldPath,
                        (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                        &fieldPathLen,
                        &field)
                        == 0
                    && fieldPathLen > 0)
                {
                    hasField = 1;
                }
            }
        }
        if (!hasField) {
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "len")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                SLTypeRef recvExprType;
                if (recvNode < 0 || extra >= 0) {
                    return -1;
                }
                if (InferExprType(c, recvNode, &recvExprType) != 0 || !recvExprType.valid) {
                    return -1;
                }
                return EmitLenExprFromType(c, recvNode, &recvExprType);
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "cstr")) {
                int32_t extra = AstNextSibling(&c->ast, child);
                if (recvNode < 0 || extra >= 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_cstr(") != 0 || EmitExpr(c, recvNode) != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "free")) {
                int32_t valueNode = AstNextSibling(&c->ast, child);
                int32_t extra = valueNode >= 0 ? AstNextSibling(&c->ast, valueNode) : -1;
                if (recvNode < 0 || valueNode < 0 || extra >= 0) {
                    return -1;
                }
                return EmitFreeCallExpr(c, recvNode, valueNode);
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "panic")) {
                int32_t extra = AstNextSibling(&c->ast, child);
                if (recvNode < 0 || extra >= 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_panic(") != 0 || EmitExpr(c, recvNode) != 0
                    || BufAppendCStr(&c->out, ", __FILE__, __LINE__)") != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (recvNode >= 0) {
                SLCCallArgInfo callArgs[SLCCG_MAX_CALL_ARGS];
                int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
                SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
                SLCCallBinding binding;
                uint32_t       argCount = 0;
                uint32_t       i;
                const SLFnSig* resolvedSig = NULL;
                const char*    resolvedName = NULL;
                uint32_t       recvPkgLen = 0;
                int            recvHasPkgPrefix =
                    recvType.baseName != NULL
                    && TypeNamePkgPrefixLen(recvType.baseName, &recvPkgLen);
                if (CollectCallArgInfo(c, nodeId, child, 1, recvNode, callArgs, argTypes, &argCount)
                    == 0)
                {
                    for (i = 0; i < argCount; i++) {
                        argNodes[i] = callArgs[i].exprNode;
                    }
                    int status = ResolveCallTarget(
                        c,
                        callee->dataStart,
                        callee->dataEnd,
                        callArgs,
                        argNodes,
                        argTypes,
                        argCount,
                        1,
                        0,
                        &binding,
                        &resolvedSig,
                        &resolvedName);
                    if (status == 0 && resolvedName != NULL) {
                        return EmitResolvedCall(c, nodeId, resolvedName, resolvedSig, &binding, 0);
                    }
                    if (status == 2) {
                        status = ResolveCallTarget(
                            c,
                            callee->dataStart,
                            callee->dataEnd,
                            callArgs,
                            argNodes,
                            argTypes,
                            argCount,
                            1,
                            1,
                            &binding,
                            &resolvedSig,
                            &resolvedName);
                        if (status == 0 && resolvedName != NULL) {
                            return EmitResolvedCall(
                                c, nodeId, resolvedName, resolvedSig, &binding, 1);
                        }
                    }
                    if ((status == 1 || status == 2) && recvHasPkgPrefix) {
                        int prefixedStatus = ResolveCallTargetByPkgMethod(
                            c,
                            0,
                            recvPkgLen,
                            callee->dataStart,
                            callee->dataEnd,
                            callArgs,
                            argNodes,
                            argTypes,
                            argCount,
                            1,
                            0,
                            &binding,
                            &resolvedSig,
                            &resolvedName);
                        if (prefixedStatus == 0 && resolvedName != NULL) {
                            return EmitResolvedCall(
                                c, nodeId, resolvedName, resolvedSig, &binding, 0);
                        }
                        if (prefixedStatus == 2) {
                            prefixedStatus = ResolveCallTargetByPkgMethod(
                                c,
                                0,
                                recvPkgLen,
                                callee->dataStart,
                                callee->dataEnd,
                                callArgs,
                                argNodes,
                                argTypes,
                                argCount,
                                1,
                                1,
                                &binding,
                                &resolvedSig,
                                &resolvedName);
                            if (prefixedStatus == 0 && resolvedName != NULL) {
                                return EmitResolvedCall(
                                    c, nodeId, resolvedName, resolvedSig, &binding, 1);
                            }
                        }
                    }
                }
            }
        }
    }
    if (callee != NULL && callee->kind == SLAst_IDENT) {
        SLCCallArgInfo callArgs[SLCCG_MAX_CALL_ARGS];
        int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
        SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
        SLCCallBinding binding;
        uint32_t       argCount = 0;
        uint32_t       i;
        const SLFnSig* resolvedSig = NULL;
        const char*    resolvedName = NULL;
        int            status;
        if (CollectCallArgInfo(c, nodeId, child, 0, -1, callArgs, argTypes, &argCount) == 0) {
            for (i = 0; i < argCount; i++) {
                argNodes[i] = callArgs[i].exprNode;
            }
            status = ResolveCallTarget(
                c,
                callee->dataStart,
                callee->dataEnd,
                callArgs,
                argNodes,
                argTypes,
                argCount,
                0,
                0,
                &binding,
                &resolvedSig,
                &resolvedName);
            if (status == 0 && resolvedName != NULL) {
                return EmitResolvedCall(c, nodeId, resolvedName, resolvedSig, &binding, 0);
            }
            if (status == 2) {
                status = ResolveCallTarget(
                    c,
                    callee->dataStart,
                    callee->dataEnd,
                    callArgs,
                    argNodes,
                    argTypes,
                    argCount,
                    0,
                    1,
                    &binding,
                    &resolvedSig,
                    &resolvedName);
                if (status == 0 && resolvedName != NULL) {
                    return EmitResolvedCall(c, nodeId, resolvedName, resolvedSig, &binding, 1);
                }
            }
        }
    }
    {
        const SLFnSig*       sig = NULL;
        const SLFnTypeAlias* typeAlias = NULL;
        uint32_t             argIndex = 0;
        int                  first = 1;
        if (callee != NULL && callee->kind == SLAst_IDENT) {
            sig = FindFnSigBySlice(c, callee->dataStart, callee->dataEnd);
        }
        if (sig == NULL && callee != NULL) {
            SLTypeRef calleeType;
            if (InferExprType(c, AstFirstChild(&c->ast, nodeId), &calleeType) == 0
                && calleeType.valid && calleeType.containerKind == SLTypeContainer_SCALAR
                && calleeType.ptrDepth == 0 && calleeType.containerPtrDepth == 0
                && calleeType.baseName != NULL && !calleeType.isOptional)
            {
                typeAlias = FindFnTypeAliasByName(c, calleeType.baseName);
            }
        }
        if (sig != NULL && sig->isVariadic) {
            SLCCallArgInfo callArgs[SLCCG_MAX_CALL_ARGS];
            int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
            SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
            SLCCallBinding binding;
            uint32_t       argCount = 0;
            uint32_t       i;
            if (CollectCallArgInfo(c, nodeId, child, 0, -1, callArgs, argTypes, &argCount) == 0) {
                for (i = 0; i < argCount; i++) {
                    argNodes[i] = callArgs[i].exprNode;
                }
                if (PrepareCallBinding(
                        c, sig, callArgs, argNodes, argTypes, argCount, 0, 0, &binding)
                        == 0
                    || PrepareCallBinding(
                           c, sig, callArgs, argNodes, argTypes, argCount, 0, 1, &binding)
                           == 0)
                {
                    return EmitResolvedCall(c, nodeId, sig->cName, sig, &binding, 0);
                }
            }
        }
        if (EmitExpr(c, child) != 0 || BufAppendChar(&c->out, '(') != 0) {
            return -1;
        }
        if (sig != NULL && sig->hasContext) {
            if (EmitContextArgForSig(c, sig) != 0) {
                return -1;
            }
            first = 0;
        }
        child = AstNextSibling(&c->ast, child);
        while (child >= 0) {
            if (!first && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (sig != NULL && argIndex < sig->paramLen) {
                if (EmitExprCoerced(c, child, &sig->paramTypes[argIndex]) != 0) {
                    return -1;
                }
            } else if (typeAlias != NULL && argIndex < typeAlias->paramLen) {
                if (EmitExprCoerced(c, child, &typeAlias->paramTypes[argIndex]) != 0) {
                    return -1;
                }
            } else if (EmitExpr(c, child) != 0) {
                return -1;
            }
            first = 0;
            argIndex++;
            child = AstNextSibling(&c->ast, child);
        }
        return BufAppendChar(&c->out, ')');
    }
}

int EmitExpr_NEW(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    (void)n;
    return EmitNewExpr(c, nodeId, NULL, 0);
}

int EmitExpr_INDEX(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t base = AstFirstChild(&c->ast, nodeId);
    int32_t idx = AstNextSibling(&c->ast, base);
    if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
        return EmitSliceExpr(c, nodeId);
    }
    if (base < 0 || idx < 0) {
        return -1;
    }
    {
        SLTypeRef baseType;
        if (InferExprType(c, base, &baseType) != 0 || !baseType.valid) {
            return -1;
        }
        if (baseType.containerKind == SLTypeContainer_ARRAY
            || baseType.containerKind == SLTypeContainer_SLICE_RO
            || baseType.containerKind == SLTypeContainer_SLICE_MUT)
        {
            if (BufAppendChar(&c->out, '(') != 0
                || EmitElemPtrExpr(c, base, &baseType, TypeRefContainerWritable(&baseType)) != 0
                || BufAppendChar(&c->out, '[') != 0 || EmitExpr(c, idx) != 0
                || BufAppendCStr(&c->out, "])") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (EmitExpr(c, base) != 0 || BufAppendChar(&c->out, '[') != 0 || EmitExpr(c, idx) != 0
        || BufAppendChar(&c->out, ']') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_FIELD_EXPR(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    const SLNameMap*       enumMap = NULL;
    int32_t                enumDeclNode = -1;
    int                    enumHasPayload = 0;
    uint32_t               enumVariantStart = 0;
    uint32_t               enumVariantEnd = 0;
    int32_t                recv = AstFirstChild(&c->ast, nodeId);
    const SLAstNode*       recvNode = NodeAt(c, recv);
    int32_t                recvLocalIdx = -1;
    const SLVariantNarrow* narrow = NULL;
    SLTypeRef              recvType;
    SLTypeRef              ownerType;
    SLTypeRef              narrowFieldType;
    const SLFieldInfo*     fieldPath[64];
    uint32_t               fieldPathLen = 0;
    const SLFieldInfo*     field = NULL;
    int                    useArrow = 0;
    uint32_t               i;

    if (ResolveEnumSelectorByFieldExpr(
            c, nodeId, &enumMap, &enumDeclNode, &enumHasPayload, &enumVariantStart, &enumVariantEnd)
            != 0
        && enumMap != NULL)
    {
        if (!enumHasPayload) {
            if (BufAppendCStr(&c->out, enumMap->cName) != 0 || BufAppendCStr(&c->out, "__") != 0
                || BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd) != 0)
            {
                return -1;
            }
            return 0;
        }
        if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, enumMap->cName) != 0
            || BufAppendCStr(&c->out, "){ .tag = ") != 0
            || BufAppendCStr(&c->out, enumMap->cName) != 0 || BufAppendCStr(&c->out, "__") != 0
            || BufAppendSlice(&c->out, c->unit->source, enumVariantStart, enumVariantEnd) != 0
            || BufAppendCStr(&c->out, " })") != 0)
        {
            return -1;
        }
        (void)enumDeclNode;
        return 0;
    }

    if (InferExprType(c, recv, &recvType) == 0 && recvType.valid) {
        ownerType = recvType;
        if (ownerType.ptrDepth > 0) {
            ownerType.ptrDepth--;
            useArrow = 1;
        }
        if (ownerType.baseName != NULL) {
            if (ResolveFieldPathBySlice(
                    c,
                    ownerType.baseName,
                    n->dataStart,
                    n->dataEnd,
                    fieldPath,
                    (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                    &fieldPathLen,
                    &field)
                != 0)
            {
                fieldPathLen = 0;
                field = NULL;
            } else if (fieldPathLen > 0) {
                field = fieldPath[fieldPathLen - 1u];
            }
        }
    }

    if (recvNode != NULL && recvNode->kind == SLAst_IDENT) {
        recvLocalIdx = FindLocalIndexBySlice(c, recvNode->dataStart, recvNode->dataEnd);
        if (recvLocalIdx >= 0) {
            narrow = FindVariantNarrowByLocalIdx(c, recvLocalIdx);
        }
    }
    if (narrow != NULL
        && ResolveEnumVariantPayloadFieldType(
               c,
               narrow->enumTypeName,
               narrow->variantStart,
               narrow->variantEnd,
               n->dataStart,
               n->dataEnd,
               &narrowFieldType)
               == 0)
    {
        (void)narrowFieldType;
        if (EmitExpr(c, recv) != 0
            || BufAppendCStr(&c->out, useArrow ? "->payload." : ".payload.") != 0
            || BufAppendSlice(&c->out, c->unit->source, narrow->variantStart, narrow->variantEnd)
                   != 0
            || BufAppendChar(&c->out, '.') != 0
            || BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd) != 0)
        {
            return -1;
        }
        return 0;
    }

    if (field != NULL && field->isDependent && fieldPathLen > 0) {
        if (BufAppendCStr(&c->out, field->ownerType) != 0 || BufAppendCStr(&c->out, "__") != 0
            || BufAppendCStr(&c->out, field->fieldName) != 0 || BufAppendChar(&c->out, '(') != 0)
        {
            return -1;
        }
        if (!useArrow && fieldPathLen == 1u) {
            if (BufAppendCStr(&c->out, "&(") != 0 || EmitExpr(c, recv) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
        } else if (useArrow && fieldPathLen == 1u) {
            if (EmitExpr(c, recv) != 0) {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "&(") != 0 || EmitExpr(c, recv) != 0) {
                return -1;
            }
            for (i = 0; i + 1u < fieldPathLen; i++) {
                if (BufAppendCStr(&c->out, (i == 0u && useArrow) ? "->" : ".") != 0
                    || BufAppendCStr(&c->out, fieldPath[i]->fieldName) != 0)
                {
                    return -1;
                }
            }
            if (BufAppendChar(&c->out, ')') != 0) {
                return -1;
            }
        }
        return BufAppendChar(&c->out, ')');
    }

    if (fieldPathLen > 0u) {
        if (EmitExpr(c, recv) != 0) {
            return -1;
        }
        for (i = 0; i < fieldPathLen; i++) {
            if (BufAppendCStr(&c->out, (i == 0u && useArrow) ? "->" : ".") != 0
                || BufAppendCStr(&c->out, fieldPath[i]->fieldName) != 0)
            {
                return -1;
            }
        }
        return 0;
    }

    if (EmitExpr(c, recv) != 0 || BufAppendCStr(&c->out, useArrow ? "->" : ".") != 0
        || BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd) != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_CAST(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t expr = AstFirstChild(&c->ast, nodeId);
    int32_t typeNode = AstNextSibling(&c->ast, expr);
    (void)n;
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeForCast(c, typeNode) != 0
        || BufAppendCStr(&c->out, ")(") != 0 || EmitExpr(c, expr) != 0
        || BufAppendCStr(&c->out, "))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_SIZEOF(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t   inner = AstFirstChild(&c->ast, nodeId);
    SLTypeRef innerType;
    if (inner < 0) {
        return -1;
    }
    if (n->flags == 1) {
        if (BufAppendCStr(&c->out, "sizeof(") != 0 || EmitTypeForCast(c, inner) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (InferExprType(c, inner, &innerType) == 0 && innerType.valid) {
        if (innerType.containerKind == SLTypeContainer_SCALAR && innerType.ptrDepth == 1
            && innerType.baseName != NULL && IsVarSizeTypeName(c, innerType.baseName))
        {
            if (BufAppendCStr(&c->out, innerType.baseName) != 0
                || BufAppendCStr(&c->out, "__sizeof(") != 0 || EmitExpr(c, inner) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        if (innerType.containerKind == SLTypeContainer_SCALAR && innerType.ptrDepth > 0) {
            SLTypeRef pointeeType = innerType;
            pointeeType.ptrDepth--;
            if (BufAppendCStr(&c->out, "sizeof(") != 0
                || EmitTypeNameWithDepth(c, &pointeeType) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        if (innerType.containerKind == SLTypeContainer_ARRAY && innerType.hasArrayLen
            && innerType.containerPtrDepth == 1 && SliceStructPtrDepth(&innerType) == 0)
        {
            if (BufAppendCStr(&c->out, "((__sl_uint)(") != 0
                || BufAppendU32(&c->out, innerType.arrayLen) != 0
                || BufAppendCStr(&c->out, "u) * sizeof(") != 0
                || EmitElementTypeName(c, &innerType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
        if ((innerType.containerKind == SLTypeContainer_SLICE_RO
             || innerType.containerKind == SLTypeContainer_SLICE_MUT)
            && innerType.containerPtrDepth == 1 && SliceStructPtrDepth(&innerType) == 0)
        {
            if (BufAppendCStr(&c->out, "((__sl_uint)(") != 0
                || EmitLenExprFromType(c, inner, &innerType) != 0
                || BufAppendCStr(&c->out, ") * sizeof(") != 0
                || EmitElementTypeName(c, &innerType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (BufAppendCStr(&c->out, "sizeof(") != 0 || EmitExpr(c, inner) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_NULL(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    (void)nodeId;
    (void)n;
    return BufAppendCStr(&c->out, "NULL");
}

int EmitExpr_UNWRAP(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t inner = AstFirstChild(&c->ast, nodeId);
    (void)n;
    if (inner < 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "__sl_unwrap(") != 0 || EmitExpr(c, inner) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_CALL_ARG(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    int32_t inner = AstFirstChild(&c->ast, nodeId);
    (void)n;
    if (inner < 0) {
        return -1;
    }
    return EmitExpr(c, inner);
}

int EmitExpr_TUPLE_EXPR(SLCBackendC* c, int32_t nodeId, const SLAstNode* n) {
    SLTypeRef             tupleType;
    const SLAnonTypeInfo* tupleInfo = NULL;
    uint32_t              i;
    int32_t               child;
    (void)n;
    if (InferExprType(c, nodeId, &tupleType) != 0 || !tupleType.valid
        || !TypeRefTupleInfo(c, &tupleType, &tupleInfo))
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, &tupleType) != 0
        || BufAppendCStr(&c->out, "){") != 0)
    {
        return -1;
    }
    child = AstFirstChild(&c->ast, nodeId);
    for (i = 0; i < tupleInfo->fieldCount; i++) {
        const SLFieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
        if (child < 0) {
            return -1;
        }
        if (i > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (BufAppendChar(&c->out, '.') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
            || BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, child, &f->type) != 0)
        {
            return -1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    if (child >= 0) {
        return -1;
    }
    return BufAppendCStr(&c->out, "})");
}

int EmitExpr(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAst_IDENT:             return EmitExpr_IDENT(c, nodeId, n);
        case SLAst_INT:               return EmitExpr_INT(c, nodeId, n);
        case SLAst_RUNE:              return EmitExpr_RUNE(c, nodeId, n);
        case SLAst_FLOAT:             return EmitExpr_FLOAT(c, nodeId, n);
        case SLAst_BOOL:              return EmitExpr_BOOL(c, nodeId, n);
        case SLAst_COMPOUND_LIT:      return EmitExpr_COMPOUND_LIT(c, nodeId, n);
        case SLAst_STRING:            return EmitExpr_STRING(c, nodeId, n);
        case SLAst_UNARY:             return EmitExpr_UNARY(c, nodeId, n);
        case SLAst_BINARY:            return EmitExpr_BINARY(c, nodeId, n);
        case SLAst_CALL_WITH_CONTEXT: return EmitExpr_CALL_WITH_CONTEXT(c, nodeId, n);
        case SLAst_CALL:              return EmitExpr_CALL(c, nodeId, n);
        case SLAst_NEW:               return EmitExpr_NEW(c, nodeId, n);
        case SLAst_INDEX:             return EmitExpr_INDEX(c, nodeId, n);
        case SLAst_FIELD_EXPR:        return EmitExpr_FIELD_EXPR(c, nodeId, n);
        case SLAst_CAST:              return EmitExpr_CAST(c, nodeId, n);
        case SLAst_SIZEOF:            return EmitExpr_SIZEOF(c, nodeId, n);
        case SLAst_NULL:              return EmitExpr_NULL(c, nodeId, n);
        case SLAst_UNWRAP:            return EmitExpr_UNWRAP(c, nodeId, n);
        case SLAst_TUPLE_EXPR:        return EmitExpr_TUPLE_EXPR(c, nodeId, n);
        case SLAst_CALL_ARG:          return EmitExpr_CALL_ARG(c, nodeId, n);
        default:
            /* Unsupported AST kind in expression context. */
            return -1;
    }
}

int EmitStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth);

int EmitDeferredRange(SLCBackendC* c, uint32_t start, uint32_t depth) {
    uint32_t i = c->deferredStmtLen;
    while (i > start) {
        int32_t stmtNodeId;
        i--;
        stmtNodeId = c->deferredStmtNodes[i];
        if (EmitStmt(c, stmtNodeId, depth) != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitBlockImpl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int inlineOpen) {
    int32_t  child = AstFirstChild(&c->ast, nodeId);
    uint32_t deferMark;
    if (PushScope(c) != 0) {
        return -1;
    }
    if (PushDeferScope(c) != 0) {
        PopScope(c);
        return -1;
    }
    deferMark = c->deferScopeMarks[c->deferScopeLen - 1u];
    if (!inlineOpen) {
        EmitIndent(c, depth);
    }
    if (BufAppendCStr(&c->out, "{\n") != 0) {
        PopDeferScope(c);
        PopScope(c);
        return -1;
    }
    while (child >= 0) {
        if (EmitStmt(c, child, depth + 1u) != 0) {
            PopDeferScope(c);
            PopScope(c);
            return -1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    if (EmitDeferredRange(c, deferMark, depth + 1u) != 0) {
        PopDeferScope(c);
        PopScope(c);
        return -1;
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "}\n") != 0) {
        PopDeferScope(c);
        PopScope(c);
        return -1;
    }
    PopDeferScope(c);
    PopScope(c);
    return 0;
}

int EmitBlock(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    return EmitBlockImpl(c, nodeId, depth, 0);
}

int EmitBlockInline(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    return EmitBlockImpl(c, nodeId, depth, 1);
}

int EmitVarLikeStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isConst) {
    const SLAstNode*  n = NodeAt(c, nodeId);
    SLCCGVarLikeParts parts;
    SLTypeRef         sharedType;
    uint32_t          i;

    if (n == NULL) {
        return -1;
    }
    if (ResolveVarLikeParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }

    if (!parts.grouped) {
        int32_t   typeNode = parts.typeNode;
        int32_t   initNode = parts.initNode;
        char*     name = DupSlice(c, c->unit->source, n->dataStart, n->dataEnd);
        SLTypeRef type;
        if (name == NULL) {
            return -1;
        }
        if (typeNode >= 0) {
            if (ParseTypeRef(c, typeNode, &type) != 0) {
                return -1;
            }
        } else {
            if (InferVarLikeDeclType(c, initNode, &type) != 0) {
                return -1;
            }
        }
        if (EnsureAnonTypeVisible(c, &type, depth) != 0) {
            return -1;
        }

        EmitIndent(c, depth);
        if (isConst && BufAppendCStr(&c->out, "const ") != 0) {
            return -1;
        }
        if ((typeNode >= 0 && EmitTypeWithName(c, typeNode, name) != 0)
            || (typeNode < 0 && EmitTypeRefWithName(c, &type, name) != 0))
        {
            return -1;
        }
        if (initNode >= 0) {
            if (BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, initNode, &type) != 0) {
                if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                    SetDiagNode(c, initNode, SLDiag_CODEGEN_INTERNAL);
                }
                return -1;
            }
        } else if (!isConst && BufAppendCStr(&c->out, " = {0}") != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, ";\n") != 0) {
            return -1;
        }

        if (AddLocal(c, name, type) != 0) {
            return -1;
        }
        return 0;
    }

    if (parts.typeNode >= 0) {
        if (ParseTypeRef(c, parts.typeNode, &sharedType) != 0) {
            return -1;
        }
        if (EnsureAnonTypeVisible(c, &sharedType, depth) != 0) {
            return -1;
        }
    } else {
        TypeRefSetInvalid(&sharedType);
    }

    {
        int                   useTupleDecompose = 0;
        int32_t               tupleInitNode = -1;
        SLTypeRef             tupleInitType;
        const SLAnonTypeInfo* tupleInfo = NULL;
        if (parts.initNode >= 0) {
            if (NodeAt(c, parts.initNode) == NULL
                || NodeAt(c, parts.initNode)->kind != SLAst_EXPR_LIST)
            {
                return -1;
            }
            {
                uint32_t initCount = ListCount(&c->ast, parts.initNode);
                if (initCount == parts.nameCount) {
                } else if (initCount == 1u) {
                    tupleInitNode = ListItemAt(&c->ast, parts.initNode, 0);
                    if (tupleInitNode < 0 || InferExprType(c, tupleInitNode, &tupleInitType) != 0
                        || !tupleInitType.valid || !TypeRefTupleInfo(c, &tupleInitType, &tupleInfo)
                        || tupleInfo->fieldCount != parts.nameCount)
                    {
                        return -1;
                    }
                    useTupleDecompose = 1;
                } else {
                    return -1;
                }
            }
        }

        if (useTupleDecompose) {
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "__auto_type __sl_tmp_tuple = ") != 0
                || EmitExprCoerced(c, tupleInitNode, &tupleInitType) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
            for (i = 0; i < parts.nameCount; i++) {
                int32_t            nameNode = ListItemAt(&c->ast, parts.nameListNode, i);
                const SLAstNode*   nameAst = NodeAt(c, nameNode);
                const SLFieldInfo* tf = &c->fieldInfos[tupleInfo->fieldStart + i];
                char*              name;
                int                isHole;
                SLTypeRef          type;
                if (nameAst == NULL) {
                    return -1;
                }
                name = DupSlice(c, c->unit->source, nameAst->dataStart, nameAst->dataEnd);
                if (name == NULL) {
                    return -1;
                }
                isHole = StrEq(name, "_");
                type = parts.typeNode >= 0 ? sharedType : tf->type;
                if (EnsureAnonTypeVisible(c, &type, depth) != 0) {
                    return -1;
                }
                EmitIndent(c, depth);
                if (isHole) {
                    if (BufAppendCStr(&c->out, "(void)(__sl_tmp_tuple.") != 0
                        || BufAppendCStr(&c->out, tf->fieldName) != 0
                        || BufAppendCStr(&c->out, ");\n") != 0)
                    {
                        return -1;
                    }
                    continue;
                }
                if (isConst && BufAppendCStr(&c->out, "const ") != 0) {
                    return -1;
                }
                if ((parts.typeNode >= 0 && EmitTypeWithName(c, parts.typeNode, name) != 0)
                    || (parts.typeNode < 0 && EmitTypeRefWithName(c, &type, name) != 0))
                {
                    return -1;
                }
                if (BufAppendCStr(&c->out, " = __sl_tmp_tuple.") != 0
                    || BufAppendCStr(&c->out, tf->fieldName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
                if (AddLocal(c, name, type) != 0) {
                    return -1;
                }
            }
            return 0;
        }

        for (i = 0; i < parts.nameCount; i++) {
            int32_t          nameNode = ListItemAt(&c->ast, parts.nameListNode, i);
            const SLAstNode* nameAst = NodeAt(c, nameNode);
            int32_t          initNode = -1;
            char*            name;
            int              isHole;
            SLTypeRef        type;
            if (nameAst == NULL) {
                return -1;
            }
            name = DupSlice(c, c->unit->source, nameAst->dataStart, nameAst->dataEnd);
            if (name == NULL) {
                return -1;
            }
            isHole = StrEq(name, "_");
            if (parts.initNode >= 0 && NodeAt(c, parts.initNode) != NULL
                && NodeAt(c, parts.initNode)->kind == SLAst_EXPR_LIST)
            {
                initNode = ListItemAt(&c->ast, parts.initNode, i);
            }
            if (parts.typeNode >= 0) {
                type = sharedType;
            } else {
                if (initNode < 0 || InferVarLikeDeclType(c, initNode, &type) != 0) {
                    return -1;
                }
                if (EnsureAnonTypeVisible(c, &type, depth) != 0) {
                    return -1;
                }
            }

            if (isHole) {
                if (initNode >= 0) {
                    EmitIndent(c, depth);
                    if (BufAppendCStr(&c->out, "(void)(") != 0) {
                        return -1;
                    }
                    if (parts.typeNode >= 0) {
                        if (EmitExprCoerced(c, initNode, &type) != 0) {
                            if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                                SetDiagNode(c, initNode, SLDiag_CODEGEN_INTERNAL);
                            }
                            return -1;
                        }
                    } else if (EmitExpr(c, initNode) != 0) {
                        return -1;
                    }
                    if (BufAppendCStr(&c->out, ");\n") != 0) {
                        return -1;
                    }
                }
                continue;
            }

            EmitIndent(c, depth);
            if (isConst && BufAppendCStr(&c->out, "const ") != 0) {
                return -1;
            }
            if ((parts.typeNode >= 0 && EmitTypeWithName(c, parts.typeNode, name) != 0)
                || (parts.typeNode < 0 && EmitTypeRefWithName(c, &type, name) != 0))
            {
                return -1;
            }
            if (initNode >= 0) {
                if (BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, initNode, &type) != 0)
                {
                    if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                        SetDiagNode(c, initNode, SLDiag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
            } else if (!isConst && BufAppendCStr(&c->out, " = {0}") != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, ";\n") != 0) {
                return -1;
            }
            if (AddLocal(c, name, type) != 0) {
                return -1;
            }
        }
        return 0;
    }
}

int EmitMultiAssignStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    int32_t  lhsList = AstFirstChild(&c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? AstNextSibling(&c->ast, lhsList) : -1;
    uint32_t lhsCount;
    uint32_t rhsCount;
    uint32_t i;
    if (lhsList < 0 || rhsList < 0 || NodeAt(c, lhsList) == NULL || NodeAt(c, rhsList) == NULL
        || NodeAt(c, lhsList)->kind != SLAst_EXPR_LIST
        || NodeAt(c, rhsList)->kind != SLAst_EXPR_LIST)
    {
        return -1;
    }
    lhsCount = ListCount(&c->ast, lhsList);
    rhsCount = ListCount(&c->ast, rhsList);
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "{\n") != 0) {
        return -1;
    }
    if (rhsCount == lhsCount) {
        for (i = 0; i < lhsCount; i++) {
            int32_t   rhsNode = ListItemAt(&c->ast, rhsList, i);
            SLTypeRef rhsType;
            if (rhsNode < 0 || InferExprType(c, rhsNode, &rhsType) != 0 || !rhsType.valid) {
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "__auto_type __sl_tmp_") != 0
                || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, " = ") != 0
                || EmitExprCoerced(c, rhsNode, &rhsType) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
    } else if (rhsCount == 1u) {
        int32_t               rhsNode = ListItemAt(&c->ast, rhsList, 0);
        SLTypeRef             rhsType;
        const SLAnonTypeInfo* tupleInfo = NULL;
        if (rhsNode < 0 || InferExprType(c, rhsNode, &rhsType) != 0 || !rhsType.valid
            || !TypeRefTupleInfo(c, &rhsType, &tupleInfo) || tupleInfo->fieldCount != lhsCount)
        {
            return -1;
        }
        EmitIndent(c, depth + 1u);
        if (BufAppendCStr(&c->out, "__auto_type __sl_tmp_tuple = ") != 0
            || EmitExprCoerced(c, rhsNode, &rhsType) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
        for (i = 0; i < lhsCount; i++) {
            const SLFieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "__auto_type __sl_tmp_") != 0
                || BufAppendU32(&c->out, i) != 0
                || BufAppendCStr(&c->out, " = __sl_tmp_tuple.") != 0
                || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
    } else {
        return -1;
    }
    for (i = 0; i < lhsCount; i++) {
        int32_t          lhsNode = ListItemAt(&c->ast, lhsList, i);
        const SLAstNode* lhs = NodeAt(c, lhsNode);
        if (lhs == NULL) {
            return -1;
        }
        if (lhs->kind == SLAst_IDENT
            && SliceEqName(c->unit->source, lhs->dataStart, lhs->dataEnd, "_"))
        {
            continue;
        }
        EmitIndent(c, depth + 1u);
        if (EmitExpr(c, lhsNode) != 0 || BufAppendCStr(&c->out, " = __sl_tmp_") != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
    }
    EmitIndent(c, depth);
    return BufAppendCStr(&c->out, "}\n");
}

int EmitForStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    int32_t          nodes[4];
    int              count = 0;
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          body;
    const SLAstNode* bodyNode;
    int32_t          init = -1;
    int32_t          cond = -1;
    int32_t          post = -1;

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = AstNextSibling(&c->ast, child);
    }
    if (count <= 0) {
        return -1;
    }

    body = nodes[count - 1];
    bodyNode = NodeAt(c, body);
    if (count == 1) {
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "for (;;)") != 0) {
            return -1;
        }
        if (bodyNode != NULL && bodyNode->kind == SLAst_BLOCK) {
            if (BufAppendChar(&c->out, ' ') != 0) {
                return -1;
            }
            return EmitBlockInline(c, body, depth);
        }
        if (BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
        return EmitStmt(c, body, depth);
    }

    if (count == 2 && NodeAt(c, nodes[0])->kind != SLAst_VAR
        && NodeAt(c, nodes[0])->kind != SLAst_CONST)
    {
        cond = nodes[0];
    } else {
        init = nodes[0];
        if (count >= 3) {
            cond = nodes[1];
        }
        if (count >= 4) {
            post = nodes[2];
        }
    }

    if (init >= 0) {
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "{\n") != 0) {
            return -1;
        }
        if (PushScope(c) != 0) {
            return -1;
        }
        if (NodeAt(c, init)->kind == SLAst_VAR) {
            if (EmitVarLikeStmt(c, init, depth + 1u, 0) != 0) {
                return -1;
            }
        } else if (NodeAt(c, init)->kind == SLAst_CONST) {
            if (EmitVarLikeStmt(c, init, depth + 1u, 1) != 0) {
                return -1;
            }
        } else {
            EmitIndent(c, depth + 1u);
            if (EmitExpr(c, init) != 0 || BufAppendCStr(&c->out, ";\n") != 0) {
                return -1;
            }
        }
        EmitIndent(c, depth + 1u);
    } else {
        EmitIndent(c, depth);
    }

    if (BufAppendCStr(&c->out, "for (; ") != 0) {
        return -1;
    }
    if (cond >= 0) {
        if (EmitExpr(c, cond) != 0) {
            return -1;
        }
    } else if (BufAppendChar(&c->out, '1') != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "; ") != 0) {
        return -1;
    }
    if (post >= 0) {
        if (EmitExpr(c, post) != 0) {
            return -1;
        }
    }
    if (bodyNode != NULL && bodyNode->kind == SLAst_BLOCK) {
        if (BufAppendChar(&c->out, ')') != 0 || BufAppendChar(&c->out, ' ') != 0) {
            return -1;
        }
        if (EmitBlockInline(c, body, init >= 0 ? depth + 1u : depth) != 0) {
            return -1;
        }
    } else {
        if (BufAppendCStr(&c->out, ")\n") != 0) {
            return -1;
        }
        if (EmitStmt(c, body, init >= 0 ? depth + 1u : depth) != 0) {
            return -1;
        }
    }

    if (init >= 0) {
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "}\n") != 0) {
            return -1;
        }
        PopScope(c);
    }

    return 0;
}

int EmitSwitchStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLAstNode* sw = NodeAt(c, nodeId);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          subjectNode = -1;
    int32_t          subjectLocalIdx = -1;
    SLTypeRef        subjectType;
    int              haveSubjectType = 0;
    int              subjectEnumHasPayload = 0;
    int              firstClause = 1;
    const char*      switchSubjectTemp = "__sl_sw_subject";

    TypeRefSetInvalid(&subjectType);
    if (sw == NULL) {
        return -1;
    }

    if (sw->flags == 1) {
        const SLAstNode* subjectAst;
        subjectNode = child;
        child = AstNextSibling(&c->ast, child);
        subjectAst = NodeAt(c, subjectNode);
        if (subjectAst != NULL && subjectAst->kind == SLAst_IDENT) {
            subjectLocalIdx = FindLocalIndexBySlice(c, subjectAst->dataStart, subjectAst->dataEnd);
        }
        if (InferExprType(c, subjectNode, &subjectType) == 0 && subjectType.valid) {
            haveSubjectType = 1;
            if (subjectType.containerKind == SLTypeContainer_SCALAR
                && subjectType.containerPtrDepth == 0 && subjectType.ptrDepth == 0
                && subjectType.baseName != NULL)
            {
                const char*      baseName = ResolveScalarAliasBaseName(c, subjectType.baseName);
                const SLNameMap* map;
                int32_t          enumNodeId;
                if (baseName == NULL) {
                    baseName = subjectType.baseName;
                }
                map = FindNameByCName(c, baseName);
                if (map != NULL && map->kind == SLAst_ENUM
                    && FindEnumDeclNodeByCName(c, baseName, &enumNodeId) == 0)
                {
                    subjectEnumHasPayload = EnumDeclHasPayload(c, enumNodeId);
                }
            }
        }
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "do {\n") != 0) {
        return -1;
    }

    if (sw->flags == 1) {
        EmitIndent(c, depth + 1u);
        if (BufAppendCStr(&c->out, "__auto_type ") != 0
            || BufAppendCStr(&c->out, switchSubjectTemp) != 0 || BufAppendCStr(&c->out, " = ") != 0
            || EmitExpr(c, subjectNode) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
    }

    while (child >= 0) {
        const SLAstNode* clause = NodeAt(c, child);
        if (clause != NULL && clause->kind == SLAst_CASE) {
            int32_t          caseChild = AstFirstChild(&c->ast, child);
            int32_t          bodyNode = -1;
            int              labelCount = 0;
            int              aliasCount = 0;
            int              singleVariant = 0;
            uint32_t         singleVariantStart = 0;
            uint32_t         singleVariantEnd = 0;
            const char*      singleVariantEnumTypeName = NULL;
            int32_t          aliasNodes[64];
            uint32_t         aliasVariantStarts[64];
            uint32_t         aliasVariantEnds[64];
            const char*      aliasEnumTypeNames[64];
            const SLAstNode* bodyStmt;

            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, firstClause ? "if (" : "else if (") != 0) {
                return -1;
            }
            firstClause = 0;

            while (caseChild >= 0) {
                int32_t next = AstNextSibling(&c->ast, caseChild);
                int32_t labelExprNode = -1;
                int32_t aliasNode = -1;
                if (next < 0) {
                    bodyNode = caseChild;
                    break;
                }
                if (CasePatternParts(c, caseChild, &labelExprNode, &aliasNode) != 0) {
                    return -1;
                }
                if (labelCount > 0 && BufAppendCStr(&c->out, " || ") != 0) {
                    return -1;
                }
                if (sw->flags == 1) {
                    const SLNameMap* labelEnumMap = NULL;
                    int32_t          labelEnumDeclNode = -1;
                    int              labelEnumHasPayload = 0;
                    uint32_t         labelVariantStart = 0;
                    uint32_t         labelVariantEnd = 0;
                    int              variantRc = DecodeEnumVariantPatternExpr(
                        c,
                        labelExprNode,
                        &labelEnumMap,
                        &labelEnumDeclNode,
                        &labelEnumHasPayload,
                        &labelVariantStart,
                        &labelVariantEnd);
                    if (variantRc < 0) {
                        return -1;
                    }
                    if (variantRc == 1 && labelEnumMap != NULL) {
                        if (labelEnumHasPayload) {
                            if (BufAppendCStr(&c->out, "((") != 0
                                || BufAppendCStr(&c->out, switchSubjectTemp) != 0
                                || BufAppendCStr(&c->out, ").tag == ") != 0
                                || BufAppendCStr(&c->out, labelEnumMap->cName) != 0
                                || BufAppendCStr(&c->out, "__") != 0
                                || BufAppendSlice(
                                       &c->out, c->unit->source, labelVariantStart, labelVariantEnd)
                                       != 0
                                || BufAppendChar(&c->out, ')') != 0)
                            {
                                return -1;
                            }
                        } else {
                            if (BufAppendCStr(&c->out, "((") != 0
                                || BufAppendCStr(&c->out, switchSubjectTemp) != 0
                                || BufAppendCStr(&c->out, ") == ") != 0
                                || BufAppendCStr(&c->out, labelEnumMap->cName) != 0
                                || BufAppendCStr(&c->out, "__") != 0
                                || BufAppendSlice(
                                       &c->out, c->unit->source, labelVariantStart, labelVariantEnd)
                                       != 0
                                || BufAppendChar(&c->out, ')') != 0)
                            {
                                return -1;
                            }
                        }
                        if (labelCount == 0) {
                            singleVariant = 1;
                            singleVariantStart = labelVariantStart;
                            singleVariantEnd = labelVariantEnd;
                            singleVariantEnumTypeName = labelEnumMap->cName;
                        } else {
                            singleVariant = 0;
                        }
                        if (aliasNode >= 0) {
                            if (aliasCount >= (int)(sizeof(aliasNodes) / sizeof(aliasNodes[0]))) {
                                return -1;
                            }
                            aliasNodes[aliasCount] = aliasNode;
                            aliasVariantStarts[aliasCount] = labelVariantStart;
                            aliasVariantEnds[aliasCount] = labelVariantEnd;
                            aliasEnumTypeNames[aliasCount] = labelEnumMap->cName;
                            aliasCount++;
                        }
                    } else {
                        singleVariant = 0;
                        if (aliasNode >= 0) {
                            return -1;
                        }
                        if (subjectEnumHasPayload) {
                            if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __sl_cmp_b = ")
                                    != 0
                                || EmitExpr(c, labelExprNode) != 0
                                || BufAppendCStr(&c->out, "; __sl_mem_equal((const void*)&") != 0
                                || BufAppendCStr(&c->out, switchSubjectTemp) != 0
                                || BufAppendCStr(
                                       &c->out, ", (const void*)&__sl_cmp_b, (__sl_uint)sizeof(")
                                       != 0
                                || BufAppendCStr(&c->out, switchSubjectTemp) != 0
                                || BufAppendCStr(&c->out, ")); }))") != 0)
                            {
                                return -1;
                            }
                        } else {
                            if (BufAppendCStr(&c->out, "((") != 0
                                || BufAppendCStr(&c->out, switchSubjectTemp) != 0
                                || BufAppendCStr(&c->out, ") == (") != 0
                                || EmitExpr(c, labelExprNode) != 0
                                || BufAppendCStr(&c->out, "))") != 0)
                            {
                                return -1;
                            }
                        }
                    }
                    (void)labelEnumDeclNode;
                } else {
                    if (aliasNode >= 0) {
                        return -1;
                    }
                    if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, labelExprNode) != 0
                        || BufAppendChar(&c->out, ')') != 0)
                    {
                        return -1;
                    }
                }
                labelCount++;
                caseChild = next;
            }

            if (bodyNode < 0) {
                return -1;
            }
            bodyStmt = NodeAt(c, bodyNode);
            if (BufAppendCStr(&c->out, ") {\n") != 0) {
                return -1;
            }
            if (PushScope(c) != 0) {
                return -1;
            }
            if (sw->flags == 1) {
                int a;
                for (a = 0; a < aliasCount; a++) {
                    char* aliasName;
                    if (!haveSubjectType) {
                        PopScope(c);
                        return -1;
                    }
                    aliasName = DupSlice(
                        c,
                        c->unit->source,
                        NodeAt(c, aliasNodes[a])->dataStart,
                        NodeAt(c, aliasNodes[a])->dataEnd);
                    if (aliasName == NULL) {
                        PopScope(c);
                        return -1;
                    }
                    EmitIndent(c, depth + 2u);
                    if (BufAppendCStr(&c->out, "__auto_type ") != 0
                        || BufAppendCStr(&c->out, aliasName) != 0
                        || BufAppendCStr(&c->out, " = ") != 0
                        || BufAppendCStr(&c->out, switchSubjectTemp) != 0
                        || BufAppendCStr(&c->out, ";\n") != 0)
                    {
                        PopScope(c);
                        return -1;
                    }
                    if (AddLocal(c, aliasName, subjectType) != 0
                        || AddVariantNarrow(
                               c,
                               (int32_t)c->localLen - 1,
                               aliasEnumTypeNames[a],
                               aliasVariantStarts[a],
                               aliasVariantEnds[a])
                               != 0)
                    {
                        PopScope(c);
                        return -1;
                    }
                }
                if (subjectLocalIdx >= 0 && labelCount == 1 && singleVariant
                    && singleVariantEnumTypeName != NULL)
                {
                    if (AddVariantNarrow(
                            c,
                            subjectLocalIdx,
                            singleVariantEnumTypeName,
                            singleVariantStart,
                            singleVariantEnd)
                        != 0)
                    {
                        PopScope(c);
                        return -1;
                    }
                }
            }
            if (bodyStmt != NULL && bodyStmt->kind == SLAst_BLOCK) {
                if (EmitBlockInline(c, bodyNode, depth + 2u) != 0) {
                    PopScope(c);
                    return -1;
                }
            } else {
                if (EmitStmt(c, bodyNode, depth + 2u) != 0) {
                    PopScope(c);
                    return -1;
                }
            }
            PopScope(c);
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "}\n") != 0) {
                return -1;
            }
        } else if (clause != NULL && clause->kind == SLAst_DEFAULT) {
            int32_t          bodyNode = AstFirstChild(&c->ast, child);
            const SLAstNode* bodyStmt = NodeAt(c, bodyNode);
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, firstClause ? "if (1) {\n" : "else {\n") != 0) {
                return -1;
            }
            firstClause = 0;
            if (PushScope(c) != 0) {
                return -1;
            }
            if (bodyStmt != NULL && bodyStmt->kind == SLAst_BLOCK) {
                if (EmitBlockInline(c, bodyNode, depth + 2u) != 0) {
                    PopScope(c);
                    return -1;
                }
            } else {
                if (EmitStmt(c, bodyNode, depth + 2u) != 0) {
                    PopScope(c);
                    return -1;
                }
            }
            PopScope(c);
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "}\n") != 0) {
                return -1;
            }
        } else {
            return -1;
        }
        child = AstNextSibling(&c->ast, child);
    }

    EmitIndent(c, depth);
    return BufAppendCStr(&c->out, "} while (0);\n");
}

int EmitAssertFormatArg(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == SLAst_STRING) {
        return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
    }
    if (BufAppendCStr(&c->out, "(const char*)(const void*)__sl_cstr(") != 0
        || EmitExpr(c, nodeId) != 0 || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAst_BLOCK:        return EmitBlock(c, nodeId, depth);
        case SLAst_VAR:          return EmitVarLikeStmt(c, nodeId, depth, 0);
        case SLAst_CONST:        return EmitVarLikeStmt(c, nodeId, depth, 1);
        case SLAst_MULTI_ASSIGN: return EmitMultiAssignStmt(c, nodeId, depth);
        case SLAst_EXPR_STMT:    {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            EmitIndent(c, depth);
            if (EmitExpr(c, expr) != 0 || BufAppendCStr(&c->out, ";\n") != 0) {
                if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                    SetDiagNode(c, expr >= 0 ? expr : nodeId, SLDiag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            return 0;
        }
        case SLAst_RETURN: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            if (EmitDeferredRange(c, 0, depth) != 0) {
                return -1;
            }
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "return") != 0) {
                return -1;
            }
            if (expr >= 0) {
                if (NodeAt(c, expr) != NULL && NodeAt(c, expr)->kind == SLAst_EXPR_LIST) {
                    const SLAnonTypeInfo* tupleInfo = NULL;
                    uint32_t              i;
                    int32_t               itemNode;
                    if (!c->hasCurrentReturnType
                        || !TypeRefTupleInfo(c, &c->currentReturnType, &tupleInfo))
                    {
                        return -1;
                    }
                    if (ListCount(&c->ast, expr) != tupleInfo->fieldCount) {
                        return -1;
                    }
                    if (BufAppendCStr(&c->out, " ((") != 0
                        || EmitTypeNameWithDepth(c, &c->currentReturnType) != 0
                        || BufAppendCStr(&c->out, "){") != 0)
                    {
                        return -1;
                    }
                    itemNode = AstFirstChild(&c->ast, expr);
                    for (i = 0; i < tupleInfo->fieldCount; i++) {
                        const SLFieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
                        if (itemNode < 0) {
                            return -1;
                        }
                        if (i > 0 && BufAppendCStr(&c->out, ", ") != 0) {
                            return -1;
                        }
                        if (BufAppendChar(&c->out, '.') != 0
                            || BufAppendCStr(&c->out, f->fieldName) != 0
                            || BufAppendCStr(&c->out, " = ") != 0
                            || EmitExprCoerced(c, itemNode, &f->type) != 0)
                        {
                            return -1;
                        }
                        itemNode = AstNextSibling(&c->ast, itemNode);
                    }
                    if (itemNode >= 0 || BufAppendCStr(&c->out, "})") != 0) {
                        return -1;
                    }
                } else {
                    if (BufAppendChar(&c->out, ' ') != 0
                        || EmitExprCoerced(
                               c, expr, c->hasCurrentReturnType ? &c->currentReturnType : NULL)
                               != 0)
                    {
                        return -1;
                    }
                }
            }
            return BufAppendCStr(&c->out, ";\n");
        }
        case SLAst_ASSERT: {
            int32_t cond = AstFirstChild(&c->ast, nodeId);
            int32_t fmtNode;
            if (cond < 0) {
                return -1;
            }
            EmitIndent(c, depth);
            fmtNode = AstNextSibling(&c->ast, cond);
            if (fmtNode < 0) {
                if (BufAppendCStr(&c->out, "__sl_assert(") != 0 || EmitExpr(c, cond) != 0
                    || BufAppendCStr(&c->out, ");\n") != 0)
                {
                    return -1;
                }
            } else {
                int32_t argNode;
                if (BufAppendCStr(&c->out, "__sl_assertf(") != 0 || EmitExpr(c, cond) != 0
                    || BufAppendCStr(&c->out, ", ") != 0 || EmitAssertFormatArg(c, fmtNode) != 0)
                {
                    return -1;
                }
                argNode = AstNextSibling(&c->ast, fmtNode);
                while (argNode >= 0) {
                    if (BufAppendCStr(&c->out, ", ") != 0 || EmitExpr(c, argNode) != 0) {
                        return -1;
                    }
                    argNode = AstNextSibling(&c->ast, argNode);
                }
                if (BufAppendCStr(&c->out, ");\n") != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_IF: {
            int32_t          cond = AstFirstChild(&c->ast, nodeId);
            int32_t          thenNode = AstNextSibling(&c->ast, cond);
            int32_t          elseNode = AstNextSibling(&c->ast, thenNode);
            const SLAstNode* thenStmt = NodeAt(c, thenNode);
            const SLAstNode* elseStmt = NodeAt(c, elseNode);
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "if (") != 0 || EmitExpr(c, cond) != 0) {
                return -1;
            }
            if (thenStmt != NULL && thenStmt->kind == SLAst_BLOCK) {
                if (BufAppendCStr(&c->out, ") ") != 0 || EmitBlockInline(c, thenNode, depth) != 0) {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, ")\n") != 0 || EmitStmt(c, thenNode, depth) != 0) {
                    return -1;
                }
            }
            if (elseNode >= 0) {
                EmitIndent(c, depth);
                if (elseStmt != NULL && elseStmt->kind == SLAst_BLOCK) {
                    if (BufAppendCStr(&c->out, "else ") != 0
                        || EmitBlockInline(c, elseNode, depth) != 0)
                    {
                        return -1;
                    }
                } else {
                    if (BufAppendCStr(&c->out, "else\n") != 0 || EmitStmt(c, elseNode, depth) != 0)
                    {
                        return -1;
                    }
                }
            }
            return 0;
        }
        case SLAst_FOR:    return EmitForStmt(c, nodeId, depth);
        case SLAst_SWITCH: return EmitSwitchStmt(c, nodeId, depth);
        case SLAst_BREAK:
            if (c->deferScopeLen > 0
                && EmitDeferredRange(c, c->deferScopeMarks[c->deferScopeLen - 1u], depth) != 0)
            {
                return -1;
            }
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "break;\n");
        case SLAst_CONTINUE:
            if (c->deferScopeLen > 0
                && EmitDeferredRange(c, c->deferScopeMarks[c->deferScopeLen - 1u], depth) != 0)
            {
                return -1;
            }
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "continue;\n");
        case SLAst_DEFER: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (child < 0) {
                return -1;
            }
            return AddDeferredStmt(c, child);
        }
        default: SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, n->start, n->end); return -1;
    }
}

SL_API_END
