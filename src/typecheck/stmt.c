#include "internal.h"

H2_API_BEGIN

static int H2TCMarkTemplateRootUsesVisitNode(H2TypeCheckCtx* c, int32_t nodeId) {
    const H2AstNode* n;
    int32_t          child;
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == H2Ast_CALL) {
        int32_t calleeNode = H2AstFirstChild(c->ast, nodeId);
        if (calleeNode >= 0 && (uint32_t)calleeNode < c->ast->len) {
            const H2AstNode* callee = &c->ast->nodes[calleeNode];
            if (callee->kind == H2Ast_IDENT) {
                int32_t fnIndex = H2TCFindFunctionIndex(c, callee->dataStart, callee->dataEnd);
                if (fnIndex >= 0) {
                    H2TCMarkFunctionUsed(c, fnIndex);
                }
            }
        }
    }
    child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        if (H2TCMarkTemplateRootUsesVisitNode(c, child) != 0) {
            return -1;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCMarkTemplateRootFunctionUses(H2TypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL || c->ast == NULL) {
        return -1;
    }
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        int32_t             child;
        if ((fn->flags & H2TCFunctionFlag_TEMPLATE) == 0
            || (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) != 0 || fn->defNode < 0)
        {
            continue;
        }
        child = H2AstFirstChild(c->ast, fn->defNode);
        while (child >= 0) {
            if (c->ast->nodes[child].kind == H2Ast_BLOCK) {
                if (H2TCMarkTemplateRootUsesVisitNode(c, child) != 0) {
                    return -1;
                }
                break;
            }
            child = H2AstNextSibling(c->ast, child);
        }
    }
    return 0;
}

static void H2TCMarkConstBlockLocalReadsRec(
    H2TypeCheckCtx* c, int32_t nodeId, int skipDirectIdent) {
    const H2AstNode* n;
    int32_t          child;
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (!skipDirectIdent && n->kind == H2Ast_IDENT) {
        int32_t localIdx = H2TCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            H2TCMarkLocalRead(c, localIdx);
        }
        return;
    }
    if (n->kind == H2Ast_VAR || n->kind == H2Ast_CONST) {
        child = H2AstFirstChild(c->ast, nodeId);
        if (child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == H2Ast_NAME_LIST)
        {
            child = H2AstNextSibling(c->ast, child);
        }
        while (child >= 0) {
            H2TCMarkConstBlockLocalReadsRec(c, child, 0);
            child = H2AstNextSibling(c->ast, child);
        }
        return;
    }
    if (n->kind == H2Ast_BINARY && n->op == H2Tok_ASSIGN) {
        int32_t lhsNode = H2AstFirstChild(c->ast, nodeId);
        int32_t rhsNode = lhsNode >= 0 ? H2AstNextSibling(c->ast, lhsNode) : -1;
        if (lhsNode >= 0 && (uint32_t)lhsNode < c->ast->len
            && c->ast->nodes[lhsNode].kind != H2Ast_IDENT)
        {
            H2TCMarkConstBlockLocalReadsRec(c, lhsNode, 0);
        }
        if (rhsNode >= 0) {
            H2TCMarkConstBlockLocalReadsRec(c, rhsNode, 0);
        }
        return;
    }
    child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        H2TCMarkConstBlockLocalReadsRec(c, child, 0);
        child = H2AstNextSibling(c->ast, child);
    }
}

static void H2TCMarkConstBlockLocalReads(H2TypeCheckCtx* c, int32_t blockNode) {
    int32_t child;
    if (c == NULL || c->ast == NULL || blockNode < 0 || (uint32_t)blockNode >= c->ast->len
        || c->ast->nodes[blockNode].kind != H2Ast_BLOCK)
    {
        return;
    }
    child = H2AstFirstChild(c->ast, blockNode);
    while (child >= 0) {
        H2TCMarkConstBlockLocalReadsRec(c, child, 0);
        child = H2AstNextSibling(c->ast, child);
    }
}

int H2TCBlockTerminates(H2TypeCheckCtx* c, int32_t blockNode) {
    int32_t child = H2AstFirstChild(c->ast, blockNode);
    int32_t last = -1;
    while (child >= 0) {
        last = child;
        child = H2AstNextSibling(c->ast, child);
    }
    if (last < 0) {
        return 0;
    }
    switch (c->ast->nodes[last].kind) {
        case H2Ast_RETURN:
        case H2Ast_BREAK:
        case H2Ast_CONTINUE: return 1;
        default:             return 0;
    }
}

static int H2TCSnapshotLocalInitStates(H2TypeCheckCtx* c, uint8_t** outStates) {
    uint32_t i;
    uint8_t* states;
    *outStates = NULL;
    if (c->localLen == 0) {
        return 0;
    }
    states = (uint8_t*)H2ArenaAlloc(c->arena, c->localLen * sizeof(uint8_t), 1);
    if (states == NULL) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, 0, 0);
    }
    for (i = 0; i < c->localLen; i++) {
        states[i] = c->locals[i].initState;
    }
    *outStates = states;
    return 0;
}

static void H2TCRestoreLocalInitStates(
    H2TypeCheckCtx* c, const uint8_t* states, uint32_t stateLen) {
    uint32_t i;
    uint32_t len = c->localLen < stateLen ? c->localLen : stateLen;
    if (states == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        c->locals[i].initState = states[i];
    }
}

static uint8_t H2TCMergeLocalInitState(uint8_t a, uint8_t b) {
    if (a == H2TCLocalInit_UNTRACKED || b == H2TCLocalInit_UNTRACKED) {
        return H2TCLocalInit_UNTRACKED;
    }
    if (a == b) {
        return a;
    }
    return H2TCLocalInit_MAYBE;
}

static void H2TCMergeLocalInitStates(
    H2TypeCheckCtx* c, const uint8_t* a, const uint8_t* b, uint32_t stateLen) {
    uint32_t i;
    uint32_t len = c->localLen < stateLen ? c->localLen : stateLen;
    if (a == NULL || b == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        c->locals[i].initState = H2TCMergeLocalInitState(a[i], b[i]);
    }
}

static void H2TCMergeLocalInitStateBuffers(uint8_t* dst, const uint8_t* src, uint32_t stateLen) {
    uint32_t i;
    if (dst == NULL || src == NULL) {
        return;
    }
    for (i = 0; i < stateLen; i++) {
        dst[i] = H2TCMergeLocalInitState(dst[i], src[i]);
    }
}

static int H2TCStmtTerminates(H2TypeCheckCtx* c, int32_t nodeId) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case H2Ast_RETURN:
        case H2Ast_BREAK:
        case H2Ast_CONTINUE: return 1;
        case H2Ast_BLOCK:    return H2TCBlockTerminates(c, nodeId);
        default:             return 0;
    }
}

static int H2TCStmtExitsSwitchWithoutContinuing(H2TypeCheckCtx* c, int32_t nodeId) {
    int32_t child;
    int32_t last = -1;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    switch (c->ast->nodes[nodeId].kind) {
        case H2Ast_RETURN:
        case H2Ast_CONTINUE: return 1;
        case H2Ast_BLOCK:    break;
        default:             return 0;
    }
    child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        last = child;
        child = H2AstNextSibling(c->ast, child);
    }
    if (last < 0) {
        return 0;
    }
    return H2TCStmtExitsSwitchWithoutContinuing(c, last);
}

int H2TCTypeBlock(
    H2TypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t       savedLocalLen = c->localLen;
    uint32_t       savedVariantNarrowLen = c->variantNarrowLen;
    int32_t        child = H2AstFirstChild(c->ast, blockNode);
    H2TCNarrowSave narrows[8]; /* saved narrowings applied during this block */
    int            narrowLen = 0;
    int            i;

    while (child >= 0) {
        int32_t next = H2AstNextSibling(c->ast, child);
        if (H2TCTypeStmt(c, child, returnType, loopDepth, switchDepth) != 0) {
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
         * block
         *   if x != null { <terminates> }   ->  x narrows to null for the rest of
         * the block
         *   if x { <terminates> }           ->  x narrows to null for the rest of
         * the block
         *   if !x { <terminates> }          ->  x narrows to T for the rest of the
         * block
         * Only fires when there is more code after the if (next >= 0) and no else
         * clause.
         */
        if (next >= 0 && c->ast->nodes[child].kind == H2Ast_IF && narrowLen < 8) {
            int32_t        condNode = H2AstFirstChild(c->ast, child);
            int32_t        thenNode = condNode >= 0 ? H2AstNextSibling(c->ast, condNode) : -1;
            int32_t        elseNode = thenNode >= 0 ? H2AstNextSibling(c->ast, thenNode) : -1;
            H2TCNullNarrow narrow;
            int            thenIsSome;
            if (elseNode < 0 && thenNode >= 0 && condNode >= 0 && H2TCBlockTerminates(c, thenNode)
                && H2TCGetOptionalCondNarrow(c, condNode, &thenIsSome, &narrow))
            {
                int32_t contType = thenIsSome ? c->typeNull : narrow.innerType;
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

static void H2TCAttachForInNextHookNoMatchingDetail(
    H2TypeCheckCtx* c, int hasKey, int valueDiscard, int32_t iterType) {
    char        detailBuf[512];
    char        iterTypeBuf[H2TC_DIAG_TEXT_CAP];
    H2TCTextBuf b;
    H2TCTextBuf iterTypeText;
    const char* iterTypeName = "Iter";
    if (c == NULL || c->diag == NULL) {
        return;
    }
    H2TCTextBufInit(&iterTypeText, iterTypeBuf, (uint32_t)sizeof(iterTypeBuf));
    if (iterType >= 0 && (uint32_t)iterType < c->typeLen) {
        H2TCFormatTypeRec(c, iterType, &iterTypeText, 0);
        iterTypeName = iterTypeBuf;
    }
    H2TCTextBufInit(&b, detailBuf, (uint32_t)sizeof(detailBuf));
    if (!hasKey) {
        H2TCTextBufAppendCStr(&b, "loop form requires ");
        H2TCTextBufAppendCStr(&b, "next_value(it *");
        H2TCTextBufAppendCStr(&b, iterTypeName);
        H2TCTextBufAppendCStr(&b, ")");
        H2TCTextBufAppendCStr(&b, " or ");
        H2TCTextBufAppendCStr(&b, "next_key_and_value(it *");
        H2TCTextBufAppendCStr(&b, iterTypeName);
        H2TCTextBufAppendCStr(&b, ")");
    } else if (valueDiscard) {
        H2TCTextBufAppendCStr(&b, "loop form requires ");
        H2TCTextBufAppendCStr(&b, "next_key(it *");
        H2TCTextBufAppendCStr(&b, iterTypeName);
        H2TCTextBufAppendCStr(&b, ")");
        H2TCTextBufAppendCStr(&b, " or ");
        H2TCTextBufAppendCStr(&b, "next_key_and_value(it *");
        H2TCTextBufAppendCStr(&b, iterTypeName);
        H2TCTextBufAppendCStr(&b, ")");
    } else {
        H2TCTextBufAppendCStr(&b, "loop form requires ");
        H2TCTextBufAppendCStr(&b, "next_key_and_value(it *");
        H2TCTextBufAppendCStr(&b, iterTypeName);
        H2TCTextBufAppendCStr(&b, ")");
    }
    c->diag->detail = H2TCAllocDiagText(c, detailBuf);
}

static int H2TCForInPayloadTypeFromOptional(
    H2TypeCheckCtx* c, int32_t optionalType, int32_t* outPayloadType) {
    const H2TCType* t;
    int32_t         payloadType;
    if (optionalType < 0 || (uint32_t)optionalType >= c->typeLen) {
        return 0;
    }
    t = &c->types[optionalType];
    if (t->kind != H2TCType_OPTIONAL || t->baseType < 0) {
        return 0;
    }
    payloadType = t->baseType;
    if ((uint32_t)payloadType >= c->typeLen) {
        return 0;
    }
    *outPayloadType = payloadType;
    return 1;
}

static int H2TCForInValueLocalTypeFromPayload(
    H2TypeCheckCtx* c, int32_t payloadType, H2TCForInValueMode mode, int32_t* outLocalType) {
    const H2TCType* payload;
    if (payloadType < 0 || (uint32_t)payloadType >= c->typeLen) {
        return 0;
    }
    payload = &c->types[payloadType];
    if (mode == H2TCForInValueMode_REF) {
        if (payload->kind != H2TCType_PTR && payload->kind != H2TCType_REF) {
            return 0;
        }
        *outLocalType = payloadType;
        return 1;
    }
    if (mode == H2TCForInValueMode_VALUE) {
        if (payload->kind == H2TCType_PTR || payload->kind == H2TCType_REF) {
            if (payload->baseType < 0 || (uint32_t)payload->baseType >= c->typeLen) {
                return 0;
            }
            *outLocalType = payload->baseType;
            return 1;
        }
        *outLocalType = payloadType;
        return 1;
    }
    if (mode == H2TCForInValueMode_ANY) {
        *outLocalType = payloadType;
        return 1;
    }
    return 0;
}

static int H2TCForInValueLocalTypeFromDirect(
    H2TypeCheckCtx* c, int32_t valueType, H2TCForInValueMode mode, int32_t* outLocalType) {
    const H2TCType* value;
    if (valueType < 0 || (uint32_t)valueType >= c->typeLen) {
        return 0;
    }
    value = &c->types[valueType];
    if (mode == H2TCForInValueMode_ANY) {
        *outLocalType = valueType;
        return 1;
    }
    if (mode == H2TCForInValueMode_REF) {
        if (value->kind != H2TCType_PTR && value->kind != H2TCType_REF) {
            return 0;
        }
        *outLocalType = valueType;
        return 1;
    }
    if (mode == H2TCForInValueMode_VALUE) {
        if (value->kind == H2TCType_PTR || value->kind == H2TCType_REF) {
            if (value->baseType < 0 || (uint32_t)value->baseType >= c->typeLen) {
                return 0;
            }
            *outLocalType = value->baseType;
            return 1;
        }
        *outLocalType = valueType;
        return 1;
    }
    return 0;
}

static int H2TCForInTuple2ValueTypesFromPayload(
    H2TypeCheckCtx* c, int32_t payloadType, int32_t* outKeyType, int32_t* outValueType) {
    const H2TCType* payload;
    int32_t         pairType = payloadType;
    if (payloadType < 0 || (uint32_t)payloadType >= c->typeLen) {
        return 0;
    }
    payload = &c->types[payloadType];
    if ((payload->kind == H2TCType_PTR || payload->kind == H2TCType_REF) && payload->baseType >= 0)
    {
        pairType = payload->baseType;
    }
    if (pairType < 0 || (uint32_t)pairType >= c->typeLen) {
        return 0;
    }
    pairType = H2TCResolveAliasBaseType(c, pairType);
    if (pairType < 0 || (uint32_t)pairType >= c->typeLen) {
        return 0;
    }
    if (c->types[pairType].kind != H2TCType_TUPLE || c->types[pairType].fieldCount != 2
        || c->types[pairType].fieldStart + 2 > c->funcParamLen)
    {
        return 0;
    }
    *outKeyType = c->funcParamTypes[c->types[pairType].fieldStart];
    *outValueType = c->funcParamTypes[c->types[pairType].fieldStart + 1];
    return 1;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
static int H2TCResolveForInIteratorFromType(
    H2TypeCheckCtx* c, int32_t sourceType, int32_t* outFnIndex, int32_t* outIterType) {
    int32_t bestFn = -1;
    int32_t bestIterType = -1;
    uint8_t bestCost = 0;
    int     nameFound = 0;
    int     ambiguous = 0;
    int     i;

    for (i = 0; i < (int)c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        int32_t             paramType;
        uint8_t             cost = 0;
        if (!H2NameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "__iterator")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & H2TCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen)
        {
            continue;
        }
        paramType = c->funcParamTypes[fn->paramTypeStart];
        if (H2TCConversionCost(c, paramType, sourceType, &cost) != 0) {
            continue;
        }
        if (bestFn < 0) {
            bestFn = i;
            bestIterType = fn->returnType;
            bestCost = cost;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestFn = i;
            bestIterType = fn->returnType;
            bestCost = cost;
            ambiguous = 0;
        } else if (cost == bestCost && i != bestFn) {
            ambiguous = 1;
        }
    }
    if (!nameFound) {
        return 1;
    }
    if (bestFn < 0) {
        return 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outFnIndex = bestFn;
    *outIterType = bestIterType;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
int H2TCResolveForInIterator(
    H2TypeCheckCtx* c,
    int32_t         sourceNode,
    int32_t         sourceType,
    int32_t*        outFnIndex,
    int32_t*        outIterType) {
    int rc = H2TCResolveForInIteratorFromType(c, sourceType, outFnIndex, outIterType);
    if (rc == 2 && sourceNode >= 0 && (uint32_t)sourceNode < c->ast->len
        && H2TCExprIsAssignable(c, sourceNode))
    {
        const H2AstNode* srcNode = &c->ast->nodes[sourceNode];
        int32_t autoRefType = H2TCInternPtrType(c, sourceType, srcNode->start, srcNode->end);
        if (autoRefType < 0) {
            return -1;
        }
        rc = H2TCResolveForInIteratorFromType(c, autoRefType, outFnIndex, outIterType);
    }
    return rc;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous, 4 bad return type */
int H2TCResolveForInNextValue(
    H2TypeCheckCtx*    c,
    int32_t            iterPtrType,
    H2TCForInValueMode valueMode,
    int32_t*           outValueType,
    int32_t*           outFn) {
    int32_t bestFn = -1;
    uint8_t bestCost = 0;
    int32_t bestValueType = -1;
    int     nameFound = 0;
    int     badReturnType = 0;
    int     ambiguous = 0;
    int     i;
    for (i = 0; i < (int)c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        int32_t             p0Type;
        uint8_t             cost = 0;
        int32_t             payloadType = -1;
        int32_t             valueType = -1;
        if (!H2NameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "next_value")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & H2TCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen
            || fn->paramTypeStart + fn->paramCount > c->funcParamLen)
        {
            continue;
        }
        p0Type = c->funcParamTypes[fn->paramTypeStart];
        if (H2TCConversionCost(c, p0Type, iterPtrType, &cost) != 0) {
            continue;
        }
        if (!H2TCForInPayloadTypeFromOptional(c, fn->returnType, &payloadType)
            || !H2TCForInValueLocalTypeFromPayload(c, payloadType, valueMode, &valueType))
        {
            badReturnType = 1;
            continue;
        }
        if (bestFn < 0) {
            bestFn = i;
            bestCost = cost;
            bestValueType = valueType;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestFn = i;
            bestCost = cost;
            bestValueType = valueType;
            ambiguous = 0;
        } else if (cost == bestCost && i != bestFn) {
            ambiguous = 1;
        }
    }
    if (!nameFound) {
        return 1;
    }
    if (bestFn < 0) {
        return badReturnType ? 4 : 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outFn = bestFn;
    *outValueType = bestValueType;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous, 4 bad return type */
int H2TCResolveForInNextKey(
    H2TypeCheckCtx* c, int32_t iterPtrType, int32_t* outKeyType, int32_t* outFn) {
    int32_t bestFn = -1;
    uint8_t bestCost = 0;
    int32_t bestKeyType = -1;
    int     nameFound = 0;
    int     badReturnType = 0;
    int     ambiguous = 0;
    int     i;
    for (i = 0; i < (int)c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        int32_t             p0Type;
        uint8_t             cost = 0;
        int32_t             payloadType = -1;
        if (!H2NameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "next_key")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & H2TCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen
            || fn->paramTypeStart + fn->paramCount > c->funcParamLen)
        {
            continue;
        }
        p0Type = c->funcParamTypes[fn->paramTypeStart];
        if (H2TCConversionCost(c, p0Type, iterPtrType, &cost) != 0) {
            continue;
        }
        if (!H2TCForInPayloadTypeFromOptional(c, fn->returnType, &payloadType)) {
            badReturnType = 1;
            continue;
        }
        if (bestFn < 0) {
            bestFn = i;
            bestCost = cost;
            bestKeyType = payloadType;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestFn = i;
            bestCost = cost;
            bestKeyType = payloadType;
            ambiguous = 0;
        } else if (cost == bestCost && i != bestFn) {
            ambiguous = 1;
        }
    }
    if (!nameFound) {
        return 1;
    }
    if (bestFn < 0) {
        return badReturnType ? 4 : 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outFn = bestFn;
    *outKeyType = bestKeyType;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous, 4 bad return type */
int H2TCResolveForInNextKeyAndValue(
    H2TypeCheckCtx*    c,
    int32_t            iterPtrType,
    H2TCForInValueMode valueMode,
    int32_t*           outKeyType,
    int32_t*           outValueType,
    int32_t*           outFn) {
    int32_t bestFn = -1;
    uint8_t bestCost = 0;
    int32_t bestKeyType = -1;
    int32_t bestValueType = -1;
    int     nameFound = 0;
    int     badReturnType = 0;
    int     ambiguous = 0;
    int     i;
    for (i = 0; i < (int)c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        int32_t             p0Type;
        uint8_t             cost = 0;
        int32_t             payloadType = -1;
        int32_t             keyFieldType = -1;
        int32_t             valueFieldType = -1;
        int32_t             valueType = -1;
        if (!H2NameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "next_key_and_value")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & H2TCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen
            || fn->paramTypeStart + fn->paramCount > c->funcParamLen)
        {
            continue;
        }
        p0Type = c->funcParamTypes[fn->paramTypeStart];
        if (H2TCConversionCost(c, p0Type, iterPtrType, &cost) != 0) {
            continue;
        }
        if (!H2TCForInPayloadTypeFromOptional(c, fn->returnType, &payloadType)
            || !H2TCForInTuple2ValueTypesFromPayload(c, payloadType, &keyFieldType, &valueFieldType)
            || !H2TCForInValueLocalTypeFromDirect(c, valueFieldType, valueMode, &valueType))
        {
            badReturnType = 1;
            continue;
        }
        if (bestFn < 0) {
            bestFn = i;
            bestCost = cost;
            bestKeyType = keyFieldType;
            bestValueType = valueType;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestFn = i;
            bestCost = cost;
            bestKeyType = keyFieldType;
            bestValueType = valueType;
            ambiguous = 0;
        } else if (cost == bestCost && i != bestFn) {
            ambiguous = 1;
        }
    }
    if (!nameFound) {
        return 1;
    }
    if (bestFn < 0) {
        return badReturnType ? 4 : 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outFn = bestFn;
    *outKeyType = bestKeyType;
    *outValueType = bestValueType;
    return 0;
}

static int H2TCTypeForInStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const H2AstNode*   forNode = &c->ast->nodes[nodeId];
    uint32_t           savedLocalLen = c->localLen;
    int                hasKey = (forNode->flags & H2AstFlag_FOR_IN_HAS_KEY) != 0;
    int                keyRef = (forNode->flags & H2AstFlag_FOR_IN_KEY_REF) != 0;
    int                valueRef = (forNode->flags & H2AstFlag_FOR_IN_VALUE_REF) != 0;
    int                valueDiscard = (forNode->flags & H2AstFlag_FOR_IN_VALUE_DISCARD) != 0;
    int32_t            nodes[4];
    int                count = 0;
    int32_t            child = H2AstFirstChild(c->ast, nodeId);
    int32_t            keyNode = -1;
    int32_t            valueNode = -1;
    int32_t            sourceNode = -1;
    int32_t            bodyNode = -1;
    int32_t            sourceType = -1;
    H2TCIndexBaseInfo  sourceInfo;
    int                useSequencePath = 0;
    int32_t            keyLocalType = -1;
    int32_t            valueLocalType = -1;
    int32_t            iterTypeForDiag = -1;
    H2TCForInValueMode requestedValueMode = H2TCForInValueMode_VALUE;

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = H2AstNextSibling(c->ast, child);
    }
    if (count == 0 || child >= 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    if (hasKey) {
        if (count != 4) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        keyNode = nodes[0];
        valueNode = nodes[1];
        sourceNode = nodes[2];
        bodyNode = nodes[3];
    } else {
        if (count != 3) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        valueNode = nodes[0];
        sourceNode = nodes[1];
        bodyNode = nodes[2];
    }

    if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != H2Ast_BLOCK) {
        return H2TCFailNode(c, nodeId, H2Diag_UNEXPECTED_TOKEN);
    }
    if (valueDiscard) {
        const H2AstNode* valueIdent = &c->ast->nodes[valueNode];
        if (valueIdent->kind != H2Ast_IDENT
            || !H2NameEqLiteral(c->src, valueIdent->dataStart, valueIdent->dataEnd, "_"))
        {
            return H2TCFailNode(c, valueNode, H2Diag_FOR_IN_VALUE_BINDING_INVALID);
        }
    }

    if (H2TCTypeExpr(c, sourceNode, &sourceType) != 0) {
        return -1;
    }
    if (valueRef) {
        requestedValueMode = H2TCForInValueMode_REF;
    }
    if (H2TCTypeSupportsLen(c, sourceType)
        && H2TCResolveIndexBaseInfo(c, sourceType, &sourceInfo) == 0 && sourceInfo.indexable
        && sourceInfo.elemType >= 0)
    {
        useSequencePath = 1;
    }

    if (useSequencePath) {
        if (keyRef) {
            return H2TCFailNode(c, keyNode, H2Diag_FOR_IN_KEY_REF_INVALID);
        }

        if (hasKey) {
            keyLocalType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
            if (keyLocalType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_TYPE);
            }
        }
        if (!valueDiscard) {
            const H2AstNode* valueName = &c->ast->nodes[valueNode];
            valueLocalType = sourceInfo.elemType;
            if (valueRef) {
                if (sourceInfo.sliceMutable) {
                    valueLocalType = H2TCInternPtrType(
                        c, sourceInfo.elemType, valueName->start, valueName->end);
                } else {
                    valueLocalType = H2TCInternRefType(
                        c, sourceInfo.elemType, 0, valueName->start, valueName->end);
                }
            }
            if (valueLocalType < 0) {
                return -1;
            }
        }
    } else {
        int32_t iterType = -1;
        int32_t iterPtrType = -1;
        int32_t iterFn = -1;
        int32_t nextValueFn = -1;
        int32_t nextKeyFn = -1;
        int32_t nextPairFn = -1;
        int     useNextPair = 0;
        int     rc;
        if (keyRef) {
            return H2TCFailNode(c, keyNode, H2Diag_FOR_IN_KEY_REF_INVALID);
        }

        rc = H2TCResolveForInIterator(c, sourceNode, sourceType, &iterFn, &iterType);
        if (rc == 1 || rc == 2) {
            return H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_INVALID_SOURCE);
        }
        if (rc == 3) {
            return H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ITERATOR_AMBIGUOUS);
        }
        if (rc != 0) {
            return -1;
        }
        if (H2TCValidateCallContextRequirements(c, c->funcs[iterFn].contextType) != 0) {
            return -1;
        }
        H2TCMarkFunctionUsed(c, iterFn);
        iterTypeForDiag = iterType;
        iterPtrType = H2TCInternPtrType(
            c, iterType, c->ast->nodes[sourceNode].start, c->ast->nodes[sourceNode].end);
        if (iterPtrType < 0) {
            return -1;
        }

        if (hasKey && valueDiscard) {
            rc = H2TCResolveForInNextKey(c, iterPtrType, &keyLocalType, &nextKeyFn);
            if (rc == 1 || rc == 2) {
                rc = H2TCResolveForInNextKeyAndValue(
                    c,
                    iterPtrType,
                    H2TCForInValueMode_ANY,
                    &keyLocalType,
                    &valueLocalType,
                    &nextPairFn);
                if (rc == 0) {
                    useNextPair = 1;
                }
            }
            if (rc == 4) {
                return H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NON_BOOL);
            }
            if (rc == 1 || rc == 2 || rc == 3) {
                int err = H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                H2TCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
                return err;
            }
            if (rc != 0) {
                return -1;
            }
        } else if (hasKey) {
            rc = H2TCResolveForInNextKeyAndValue(
                c,
                iterPtrType,
                valueDiscard ? H2TCForInValueMode_ANY : requestedValueMode,
                &keyLocalType,
                &valueLocalType,
                &nextPairFn);
            if (rc == 4) {
                return H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NON_BOOL);
            }
            if (rc == 1 || rc == 2 || rc == 3) {
                int err = H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                H2TCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
                return err;
            }
            if (rc != 0) {
                return -1;
            }
            useNextPair = 1;
        } else {
            rc = H2TCResolveForInNextValue(
                c,
                iterPtrType,
                valueDiscard ? H2TCForInValueMode_ANY : requestedValueMode,
                &valueLocalType,
                &nextValueFn);
            if (rc == 1 || rc == 2) {
                rc = H2TCResolveForInNextKeyAndValue(
                    c,
                    iterPtrType,
                    valueDiscard ? H2TCForInValueMode_ANY : requestedValueMode,
                    &keyLocalType,
                    &valueLocalType,
                    &nextPairFn);
                if (rc == 0) {
                    useNextPair = 1;
                }
            }
            if (rc == 4) {
                return H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NON_BOOL);
            }
            if (rc == 1 || rc == 2 || rc == 3) {
                int err = H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                H2TCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
                return err;
            }
            if (rc != 0) {
                return -1;
            }
        }

        if (nextValueFn >= 0 && !useNextPair) {
            if (H2TCValidateCallContextRequirements(c, c->funcs[nextValueFn].contextType) != 0) {
                return -1;
            }
            H2TCMarkFunctionUsed(c, nextValueFn);
        }
        if (nextKeyFn >= 0 && !useNextPair) {
            if (H2TCValidateCallContextRequirements(c, c->funcs[nextKeyFn].contextType) != 0) {
                return -1;
            }
            H2TCMarkFunctionUsed(c, nextKeyFn);
        }
        if (nextPairFn >= 0 && useNextPair) {
            if (H2TCValidateCallContextRequirements(c, c->funcs[nextPairFn].contextType) != 0) {
                return -1;
            }
            H2TCMarkFunctionUsed(c, nextPairFn);
        }
    }

    if (hasKey) {
        const H2AstNode* keyName = &c->ast->nodes[keyNode];
        if (keyLocalType < 0) {
            int err = H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
            H2TCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
            return err;
        }
        if (H2TCLocalAdd(c, keyName->dataStart, keyName->dataEnd, keyLocalType, 0, -1) != 0) {
            return -1;
        }
        H2TCMarkLocalWrite(c, (int32_t)c->localLen - 1);
        H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
    }
    if (!valueDiscard) {
        const H2AstNode* valueName = &c->ast->nodes[valueNode];
        if (valueLocalType < 0) {
            int err = H2TCFailNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
            H2TCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
            return err;
        }
        if (H2TCLocalAdd(c, valueName->dataStart, valueName->dataEnd, valueLocalType, 0, -1) != 0) {
            return -1;
        }
        H2TCMarkLocalWrite(c, (int32_t)c->localLen - 1);
        H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
    }

    {
        uint8_t* beforeBodyStates;
        uint32_t beforeBodyStateLen = c->localLen;
        uint8_t  savedDiagPath = c->compilerDiagPathProven;
        if (H2TCSnapshotLocalInitStates(c, &beforeBodyStates) != 0) {
            c->localLen = savedLocalLen;
            return -1;
        }
        c->compilerDiagPathProven = 0;
        if (H2TCTypeBlock(c, bodyNode, returnType, loopDepth + 1, switchDepth) != 0) {
            c->compilerDiagPathProven = savedDiagPath;
            H2TCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
            return -1;
        }
        c->compilerDiagPathProven = savedDiagPath;
        H2TCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
    }
    c->localLen = savedLocalLen;
    return 0;
}

int H2TCTypeShortAssignStmt(H2TypeCheckCtx* c, int32_t nodeId);

int H2TCTypeForStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const H2AstNode* forNode = &c->ast->nodes[nodeId];
    uint32_t         savedLocalLen = c->localLen;
    int32_t          child = H2AstFirstChild(c->ast, nodeId);
    int32_t          nodes[4];
    int              count = 0;
    int              i;

    if ((forNode->flags & H2AstFlag_FOR_IN) != 0) {
        return H2TCTypeForInStmt(c, nodeId, returnType, loopDepth, switchDepth);
    }

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = H2AstNextSibling(c->ast, child);
    }

    if (count == 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }

    if (c->ast->nodes[nodes[count - 1]].kind != H2Ast_BLOCK) {
        return H2TCFailNode(c, nodes[count - 1], H2Diag_UNEXPECTED_TOKEN);
    }

    if (count == 2 || count == 4) {
        int     condIndex = count == 2 ? 0 : 1;
        int32_t condType;
        if (count == 4) {
            const H2AstNode* initNode = &c->ast->nodes[nodes[0]];
            if (initNode->kind == H2Ast_VAR || initNode->kind == H2Ast_CONST) {
                if (H2TCTypeVarLike(c, nodes[0]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else if (initNode->kind == H2Ast_SHORT_ASSIGN) {
                if (H2TCTypeShortAssignStmt(c, nodes[0]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else {
                int32_t initType;
                if (H2TCTypeExpr(c, nodes[0], &initType) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            }
        }
        if (H2TCTypeExpr(c, nodes[condIndex], &condType) != 0) {
            c->localLen = savedLocalLen;
            return -1;
        }
        if (!H2TCIsBoolType(c, condType)) {
            c->localLen = savedLocalLen;
            return H2TCFailNode(c, nodes[condIndex], H2Diag_EXPECTED_BOOL);
        }
    } else {
        for (i = 0; i < count - 1; i++) {
            const H2AstNode* n = &c->ast->nodes[nodes[i]];
            if (n->kind == H2Ast_VAR || n->kind == H2Ast_CONST) {
                if (H2TCTypeVarLike(c, nodes[i]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else if (n->kind == H2Ast_SHORT_ASSIGN) {
                if (H2TCTypeShortAssignStmt(c, nodes[i]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else {
                int32_t t;
                if (H2TCTypeExpr(c, nodes[i], &t) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            }
        }
    }

    {
        uint8_t* beforeBodyStates;
        uint32_t beforeBodyStateLen = c->localLen;
        uint8_t  savedDiagPath = c->compilerDiagPathProven;
        if (H2TCSnapshotLocalInitStates(c, &beforeBodyStates) != 0) {
            c->localLen = savedLocalLen;
            return -1;
        }
        c->compilerDiagPathProven = 0;
        if (H2TCTypeBlock(c, nodes[count - 1], returnType, loopDepth + 1, switchDepth) != 0) {
            c->compilerDiagPathProven = savedDiagPath;
            H2TCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
            c->localLen = savedLocalLen;
            return -1;
        }
        c->compilerDiagPathProven = savedDiagPath;
        H2TCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
        if (count == 4) {
            int32_t postType;
            if (H2TCTypeExpr(c, nodes[2], &postType) != 0) {
                c->localLen = savedLocalLen;
                return -1;
            }
            H2TCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
        }
    }

    c->localLen = savedLocalLen;
    return 0;
}

int H2TCTypeSwitchStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const H2AstNode* sw = &c->ast->nodes[nodeId];
    int32_t          child = H2AstFirstChild(c->ast, nodeId);
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
    uint8_t*         preSwitchStates = NULL;
    uint8_t*         mergedSwitchStates = NULL;
    uint32_t         switchStateLen = 0;
    int              hasMergedSwitchState = 0;
    int              i;

    if (sw->flags == 1) {
        if (child < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        subjectNode = child;
        if (H2TCTypeExpr(c, child, &subjectType) != 0) {
            return -1;
        }
        if (c->ast->nodes[subjectNode].kind == H2Ast_IDENT) {
            subjectLocalIdx = H2TCLocalFind(
                c, c->ast->nodes[subjectNode].dataStart, c->ast->nodes[subjectNode].dataEnd);
        }
        if (H2TCIsNamedDeclKind(c, subjectType, H2Ast_ENUM)) {
            int32_t declNode = c->types[H2TCResolveAliasBaseType(c, subjectType)].declNode;
            int32_t variant = H2TCEnumDeclFirstVariantNode(c, declNode);
            while (variant >= 0) {
                if (c->ast->nodes[variant].kind == H2Ast_FIELD) {
                    enumVariantCount++;
                }
                variant = H2AstNextSibling(c->ast, variant);
            }
            if (enumVariantCount > 0) {
                uint32_t idx = 0;
                variant = H2TCEnumDeclFirstVariantNode(c, declNode);
                enumVariantStarts = (uint32_t*)H2ArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
                enumVariantEnds = (uint32_t*)H2ArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
                enumCovered = (uint8_t*)H2ArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
                if (enumVariantStarts == NULL || enumVariantEnds == NULL || enumCovered == NULL) {
                    return H2TCFailNode(c, nodeId, H2Diag_ARENA_OOM);
                }
                while (variant >= 0 && idx < enumVariantCount) {
                    if (c->ast->nodes[variant].kind == H2Ast_FIELD) {
                        enumVariantStarts[idx] = c->ast->nodes[variant].dataStart;
                        enumVariantEnds[idx] = c->ast->nodes[variant].dataEnd;
                        enumCovered[idx] = 0;
                        idx++;
                    }
                    variant = H2AstNextSibling(c->ast, variant);
                }
                subjectEnumType = H2TCResolveAliasBaseType(c, subjectType);
            }
        }
        child = H2AstNextSibling(c->ast, child);
    }

    switchStateLen = c->localLen;
    if (H2TCSnapshotLocalInitStates(c, &preSwitchStates) != 0) {
        return -1;
    }

    while (child >= 0) {
        const H2AstNode* clause = &c->ast->nodes[child];
        if (clause->kind == H2Ast_CASE) {
            uint32_t savedLocalLen = c->localLen;
            uint32_t savedVariantNarrowLen = c->variantNarrowLen;
            int32_t  caseChild = H2AstFirstChild(c->ast, child);
            int32_t  bodyNode = -1;
            int      labelCount = 0;
            int      singleVariantLabel = 0;
            uint32_t singleVariantStart = 0;
            uint32_t singleVariantEnd = 0;
            H2TCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
            while (caseChild >= 0) {
                int32_t next = H2AstNextSibling(c->ast, caseChild);
                int32_t labelExprNode;
                int32_t aliasNode;
                if (next < 0) {
                    bodyNode = caseChild;
                    break;
                }
                if (H2TCCasePatternParts(c, caseChild, &labelExprNode, &aliasNode) != 0) {
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
                if (sw->flags == 1) {
                    int32_t  labelEnumType = -1;
                    uint32_t labelVariantStart = 0;
                    uint32_t labelVariantEnd = 0;
                    int      variantRc = H2TCDecodeVariantPatternExpr(
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
                            return H2TCFailNode(c, labelExprNode, H2Diag_TYPE_MISMATCH);
                        }
                        for (i = 0; i < (int)enumVariantCount; i++) {
                            if (H2NameEqSlice(
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
                        if (H2TCTypeExpr(c, labelExprNode, &labelType) != 0) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return -1;
                        }
                        if (!H2TCCanAssign(c, subjectType, labelType)) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return H2TCFailTypeMismatchDetail(
                                c, labelExprNode, labelExprNode, labelType, subjectType);
                        }
                        if (H2TCIsBoolType(c, subjectType)
                            && c->ast->nodes[labelExprNode].kind == H2Ast_BOOL)
                        {
                            if (H2NameEqLiteral(
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
                            return H2TCFailNode(c, aliasNode, H2Diag_TYPE_MISMATCH);
                        }
                        if (H2TCLocalAdd(
                                c,
                                c->ast->nodes[aliasNode].dataStart,
                                c->ast->nodes[aliasNode].dataEnd,
                                subjectType,
                                0,
                                -1)
                            != 0)
                        {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return -1;
                        }
                        H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                        if (H2TCVariantNarrowPush(
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
                        return H2TCFailNode(c, aliasNode, H2Diag_UNEXPECTED_TOKEN);
                    }
                    if (H2TCTypeExpr(c, labelExprNode, &condType) != 0) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return -1;
                    }
                    if (!H2TCIsBoolType(c, condType)) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return H2TCFailNode(c, labelExprNode, H2Diag_EXPECTED_BOOL);
                    }
                }
                labelCount++;
                caseChild = next;
            }
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != H2Ast_BLOCK) {
                c->localLen = savedLocalLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                return H2TCFailNode(c, child, H2Diag_UNEXPECTED_TOKEN);
            }
            H2TCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
            if (sw->flags == 1 && subjectEnumType >= 0 && subjectLocalIdx >= 0 && labelCount == 1
                && singleVariantLabel)
            {
                if (H2TCVariantNarrowPush(
                        c, subjectLocalIdx, subjectEnumType, singleVariantStart, singleVariantEnd)
                    != 0)
                {
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
            }
            {
                uint8_t savedDiagPath = c->compilerDiagPathProven;
                c->compilerDiagPathProven = 0;
                if (H2TCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                    c->compilerDiagPathProven = savedDiagPath;
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
                c->compilerDiagPathProven = savedDiagPath;
            }
            if (!H2TCStmtExitsSwitchWithoutContinuing(c, bodyNode)) {
                uint8_t* caseStates;
                if (H2TCSnapshotLocalInitStates(c, &caseStates) != 0) {
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
                if (!hasMergedSwitchState) {
                    mergedSwitchStates = caseStates;
                    hasMergedSwitchState = 1;
                } else {
                    H2TCMergeLocalInitStateBuffers(mergedSwitchStates, caseStates, switchStateLen);
                }
            }
            c->localLen = savedLocalLen;
            c->variantNarrowLen = savedVariantNarrowLen;
        } else if (clause->kind == H2Ast_DEFAULT) {
            int32_t bodyNode = H2AstFirstChild(c->ast, child);
            hasDefault = 1;
            H2TCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != H2Ast_BLOCK) {
                return H2TCFailNode(c, child, H2Diag_UNEXPECTED_TOKEN);
            }
            {
                uint8_t savedDiagPath = c->compilerDiagPathProven;
                c->compilerDiagPathProven = 0;
                if (H2TCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                    c->compilerDiagPathProven = savedDiagPath;
                    return -1;
                }
                c->compilerDiagPathProven = savedDiagPath;
            }
            if (!H2TCStmtExitsSwitchWithoutContinuing(c, bodyNode)) {
                uint8_t* caseStates;
                if (H2TCSnapshotLocalInitStates(c, &caseStates) != 0) {
                    return -1;
                }
                if (!hasMergedSwitchState) {
                    mergedSwitchStates = caseStates;
                    hasMergedSwitchState = 1;
                } else {
                    H2TCMergeLocalInitStateBuffers(mergedSwitchStates, caseStates, switchStateLen);
                }
            }
        } else {
            return H2TCFailNode(c, child, H2Diag_UNEXPECTED_TOKEN);
        }
        child = H2AstNextSibling(c->ast, child);
    }

    {
        int switchIsExhaustive = hasDefault;
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
                    return H2TCFailSwitchMissingCases(
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
                switchIsExhaustive = 1;
            } else if (H2TCIsBoolType(c, subjectType)) {
                if (!boolCoveredTrue || !boolCoveredFalse) {
                    return H2TCFailSwitchMissingCases(
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
                switchIsExhaustive = 1;
            }
        }

        if (!switchIsExhaustive) {
            if (hasMergedSwitchState) {
                H2TCMergeLocalInitStateBuffers(mergedSwitchStates, preSwitchStates, switchStateLen);
            } else {
                mergedSwitchStates = preSwitchStates;
                hasMergedSwitchState = 1;
            }
        }
        if (hasMergedSwitchState) {
            H2TCRestoreLocalInitStates(c, mergedSwitchStates, switchStateLen);
        } else {
            H2TCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
        }
    }

    return 0;
}

int H2TCExprIsBlankIdent(H2TypeCheckCtx* c, int32_t exprNode) {
    const H2AstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    return n->kind == H2Ast_IDENT && H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "_");
}

int H2TCTypeMultiAssignStmt(H2TypeCheckCtx* c, int32_t nodeId) {
    int32_t  lhsList = H2AstFirstChild(c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? H2AstNextSibling(c->ast, lhsList) : -1;
    uint32_t lhsCount;
    uint32_t rhsCount;
    int32_t  rhsTypes[256];
    uint32_t i;
    if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != H2Ast_EXPR_LIST
        || c->ast->nodes[rhsList].kind != H2Ast_EXPR_LIST)
    {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    lhsCount = H2TCListCount(c->ast, lhsList);
    rhsCount = H2TCListCount(c->ast, rhsList);
    if (lhsCount == 0 || lhsCount > (uint32_t)(sizeof(rhsTypes) / sizeof(rhsTypes[0]))) {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
    if (rhsCount == lhsCount) {
        for (i = 0; i < rhsCount; i++) {
            int32_t rhsNode = H2TCListItemAt(c->ast, rhsList, i);
            if (rhsNode < 0 || H2TCTypeExpr(c, rhsNode, &rhsTypes[i]) != 0) {
                return -1;
            }
        }
    } else if (rhsCount == 1u) {
        int32_t         rhsNode = H2TCListItemAt(c->ast, rhsList, 0);
        int32_t         rhsType;
        const H2TCType* t;
        if (rhsNode < 0 || H2TCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType < 0 || (uint32_t)rhsType >= c->typeLen) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        t = &c->types[rhsType];
        if (t->kind != H2TCType_TUPLE || t->fieldCount != lhsCount) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        for (i = 0; i < lhsCount; i++) {
            rhsTypes[i] = c->funcParamTypes[t->fieldStart + i];
        }
    } else {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }
    for (i = 0; i < lhsCount; i++) {
        int32_t lhsNode = H2TCListItemAt(c->ast, lhsList, i);
        int32_t rhsType = rhsTypes[i];
        if (lhsNode < 0) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        if (H2TCExprIsBlankIdent(c, lhsNode)) {
            if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
                if (H2TCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                    return -1;
                }
            }
            continue;
        }
        {
            int32_t lhsType;
            if (H2TCTypeAssignTargetExpr(c, lhsNode, 1, &lhsType) != 0) {
                return -1;
            }
            if (!H2TCExprIsAssignable(c, lhsNode)) {
                return H2TCFailAssignTargetNotAssignable(c, lhsNode);
            }
            if (H2TCExprIsConstAssignTarget(c, lhsNode)) {
                return H2TCFailAssignToConst(c, lhsNode);
            }
            if (!H2TCCanAssign(c, lhsType, rhsType)) {
                return H2TCFailTypeMismatchDetail(c, lhsNode, lhsNode, rhsType, lhsType);
            }
            H2TCMarkDirectIdentLocalWrite(c, lhsNode, 1);
        }
    }
    return 0;
}

int H2TCTypeShortAssignStmt(H2TypeCheckCtx* c, int32_t nodeId) {
    int32_t  nameList = H2AstFirstChild(c->ast, nodeId);
    int32_t  rhsList = nameList >= 0 ? H2AstNextSibling(c->ast, nameList) : -1;
    uint32_t lhsCount;
    uint32_t rhsCount;
    int32_t  rhsTypes[256];
    int32_t  rhsNodes[256];
    int32_t  localIdxs[256];
    int32_t  declTypes[256];
    uint8_t  isBlank[256];
    uint32_t i;
    if (nameList < 0 || rhsList < 0 || c->ast->nodes[nameList].kind != H2Ast_NAME_LIST
        || c->ast->nodes[rhsList].kind != H2Ast_EXPR_LIST)
    {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    lhsCount = H2TCListCount(c->ast, nameList);
    rhsCount = H2TCListCount(c->ast, rhsList);
    if (lhsCount == 0 || lhsCount > (uint32_t)(sizeof(rhsTypes) / sizeof(rhsTypes[0]))) {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }

    for (i = 0; i < lhsCount; i++) {
        int32_t          nameNode = H2TCListItemAt(c->ast, nameList, i);
        const H2AstNode* name;
        uint32_t         j;
        localIdxs[i] = -1;
        declTypes[i] = -1;
        isBlank[i] = 0;
        rhsNodes[i] = -1;
        if (nameNode < 0 || c->ast->nodes[nameNode].kind != H2Ast_IDENT) {
            return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
        }
        name = &c->ast->nodes[nameNode];
        if (H2NameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
            isBlank[i] = 1;
            continue;
        }
        for (j = 0; j < i; j++) {
            int32_t          prevNameNode;
            const H2AstNode* prevName;
            if (isBlank[j]) {
                continue;
            }
            prevNameNode = H2TCListItemAt(c->ast, nameList, j);
            prevName = prevNameNode >= 0 ? &c->ast->nodes[prevNameNode] : NULL;
            if (prevName != NULL
                && H2NameEqSlice(
                    c->src, prevName->dataStart, prevName->dataEnd, name->dataStart, name->dataEnd))
            {
                return H2TCFailDuplicateDefinition(
                    c, name->dataStart, name->dataEnd, prevName->dataStart, prevName->dataEnd);
            }
        }
        localIdxs[i] = H2TCLocalFind(c, name->dataStart, name->dataEnd);
    }

    if (rhsCount == lhsCount) {
        for (i = 0; i < rhsCount; i++) {
            int32_t rhsNode = H2TCListItemAt(c->ast, rhsList, i);
            rhsNodes[i] = rhsNode;
            if (rhsNode < 0 || H2TCTypeExpr(c, rhsNode, &rhsTypes[i]) != 0) {
                return -1;
            }
        }
    } else if (rhsCount == 1u) {
        int32_t         rhsNode = H2TCListItemAt(c->ast, rhsList, 0);
        int32_t         rhsType;
        const H2TCType* t;
        if (rhsNode < 0 || H2TCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType < 0 || (uint32_t)rhsType >= c->typeLen) {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        t = &c->types[rhsType];
        if (t->kind != H2TCType_TUPLE || t->fieldCount != lhsCount
            || t->fieldStart + lhsCount > c->funcParamLen)
        {
            return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
        }
        for (i = 0; i < lhsCount; i++) {
            rhsTypes[i] = c->funcParamTypes[t->fieldStart + i];
            rhsNodes[i] = rhsNode;
        }
    } else {
        return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
    }

    for (i = 0; i < lhsCount; i++) {
        int32_t nameNode = H2TCListItemAt(c->ast, nameList, i);
        int32_t rhsType = rhsTypes[i];
        if (isBlank[i]) {
            if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
                if (H2TCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                    return -1;
                }
            }
            continue;
        }
        if (localIdxs[i] >= 0) {
            int32_t localType = c->locals[localIdxs[i]].typeId;
            if ((c->locals[localIdxs[i]].flags & H2TCLocalFlag_CONST) != 0) {
                return H2TCFailAssignToConst(c, nameNode);
            }
            if (!H2TCCanAssign(c, localType, rhsType)) {
                return H2TCFailTypeMismatchDetail(c, nameNode, rhsNodes[i], rhsType, localType);
            }
            continue;
        }
        if (rhsType == c->typeNull) {
            return H2TCFailNode(c, rhsNodes[i], H2Diag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (rhsType == c->typeVoid) {
            return H2TCFailNode(c, rhsNodes[i], H2Diag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (H2TCConcretizeInferredType(c, rhsType, &declTypes[i]) != 0) {
            return -1;
        }
        if (H2TCTypeContainsVarSizeByValue(c, declTypes[i])) {
            return H2TCFailVarSizeByValue(c, rhsNodes[i], declTypes[i], "variable position");
        }
    }

    for (i = 0; i < lhsCount; i++) {
        int32_t          nameNode = H2TCListItemAt(c->ast, nameList, i);
        const H2AstNode* name;
        if (isBlank[i]) {
            continue;
        }
        name = &c->ast->nodes[nameNode];
        if (localIdxs[i] >= 0) {
            H2TCMarkLocalWrite(c, localIdxs[i]);
            H2TCMarkLocalInitialized(c, localIdxs[i]);
            continue;
        }
        if (H2TCLocalAdd(c, name->dataStart, name->dataEnd, declTypes[i], 0, -1) != 0) {
            return -1;
        }
        H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
    }
    return 0;
}

int H2TCTypeStmt(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case H2Ast_BLOCK:       return H2TCTypeBlock(c, nodeId, returnType, loopDepth, switchDepth);
        case H2Ast_VAR:
        case H2Ast_CONST:       return H2TCTypeVarLike(c, nodeId);
        case H2Ast_CONST_BLOCK: {
            int32_t blockNode = H2AstFirstChild(c->ast, nodeId);
            if (blockNode < 0 || c->ast->nodes[blockNode].kind != H2Ast_BLOCK) {
                return H2TCFailNode(c, nodeId, H2Diag_UNEXPECTED_TOKEN);
            }
            H2TCMarkConstBlockLocalReads(c, blockNode);
            return 0;
        }
        case H2Ast_MULTI_ASSIGN: return H2TCTypeMultiAssignStmt(c, nodeId);
        case H2Ast_SHORT_ASSIGN: return H2TCTypeShortAssignStmt(c, nodeId);
        case H2Ast_EXPR_STMT:    {
            int32_t expr = H2AstFirstChild(c->ast, nodeId);
            int32_t t;
            if (expr < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            return H2TCTypeExpr(c, expr, &t);
        }
        case H2Ast_RETURN: {
            int32_t expr = H2AstFirstChild(c->ast, nodeId);
            if (expr < 0) {
                if (returnType != c->typeVoid) {
                    return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
                }
                return 0;
            }
            if (c->ast->nodes[expr].kind == H2Ast_EXPR_LIST) {
                const H2TCType* rt;
                const H2TCType* payload = NULL;
                uint32_t        wantCount;
                uint32_t        i;
                if (returnType < 0 || (uint32_t)returnType >= c->typeLen) {
                    return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
                }
                rt = &c->types[returnType];
                if (rt->kind == H2TCType_OPTIONAL && rt->baseType >= 0
                    && (uint32_t)rt->baseType < c->typeLen)
                {
                    payload = &c->types[rt->baseType];
                }
                if (rt->kind != H2TCType_TUPLE
                    && !(payload != NULL && payload->kind == H2TCType_TUPLE))
                {
                    return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
                }
                if (payload != NULL && payload->kind == H2TCType_TUPLE) {
                    rt = payload;
                }
                wantCount = rt->fieldCount;
                if (H2TCListCount(c->ast, expr) != wantCount) {
                    return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
                }
                for (i = 0; i < wantCount; i++) {
                    int32_t itemNode = H2TCListItemAt(c->ast, expr, i);
                    int32_t itemType;
                    int32_t dstType;
                    if (itemNode < 0) {
                        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
                    }
                    dstType = c->funcParamTypes[rt->fieldStart + i];
                    if (H2TCTypeExprExpected(c, itemNode, dstType, &itemType) != 0) {
                        return -1;
                    }
                    if (!H2TCCanAssign(c, dstType, itemType)) {
                        return H2TCFailTypeMismatchDetail(c, itemNode, itemNode, itemType, dstType);
                    }
                }
                return 0;
            }
            {
                int32_t         t;
                const H2TCType* rt =
                    returnType >= 0 && (uint32_t)returnType < c->typeLen
                        ? &c->types[returnType]
                        : NULL;
                if (rt != NULL && rt->kind == H2TCType_TUPLE) {
                    return H2TCFailNode(c, nodeId, H2Diag_ARITY_MISMATCH);
                }
                if (H2TCTypeExprExpected(c, expr, returnType, &t) != 0) {
                    return -1;
                }
                if (!H2TCCanAssign(c, returnType, t)) {
                    return H2TCFailTypeMismatchDetail(c, expr, expr, t, returnType);
                }
                return 0;
            }
        }
        case H2Ast_IF: {
            int32_t        cond = H2AstFirstChild(c->ast, nodeId);
            int32_t        thenNode;
            int32_t        elseNode;
            int32_t        condType;
            int            condIsOptional = 0;
            int            canSpecializeByConstCond = 0;
            int            condConstValue = 0;
            int            condIsConst = 0;
            int            diagCondConstValue = 0;
            int            diagCondIsConst = 0;
            uint8_t        savedDiagPath = c->compilerDiagPathProven;
            uint8_t        thenDiagPath = 0;
            uint8_t        elseDiagPath = 0;
            H2TCNullNarrow narrow;
            int            thenIsSome = 0;
            int            hasNarrow;
            if (cond < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_BOOL);
            }
            thenNode = H2AstNextSibling(c->ast, cond);
            if (thenNode < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNEXPECTED_TOKEN);
            }
            if (H2TCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            condIsOptional =
                (condType >= 0 && (uint32_t)condType < c->typeLen
                 && c->types[condType].kind == H2TCType_OPTIONAL);
            if (!H2TCIsBoolType(c, condType) && !condIsOptional) {
                return H2TCFailNode(c, cond, H2Diag_EXPECTED_BOOL);
            }
            if (H2TCConstBoolExpr(c, cond, &diagCondConstValue, &diagCondIsConst) != 0) {
                return -1;
            }
            if (diagCondIsConst) {
                thenDiagPath = diagCondConstValue ? savedDiagPath : 0;
                elseDiagPath = diagCondConstValue ? 0 : savedDiagPath;
            }
            elseNode = H2AstNextSibling(c->ast, thenNode);
            canSpecializeByConstCond = c->activeConstEvalCtx != NULL;
            if (!canSpecializeByConstCond && c->currentFunctionIndex >= 0
                && (uint32_t)c->currentFunctionIndex < c->funcLen
                && (c->funcs[c->currentFunctionIndex].flags & H2TCFunctionFlag_TEMPLATE_INSTANCE)
                       != 0)
            {
                canSpecializeByConstCond = 1;
            }
            if (canSpecializeByConstCond) {
                condConstValue = diagCondConstValue;
                condIsConst = diagCondIsConst;
            }
            hasNarrow = H2TCGetOptionalCondNarrow(c, cond, &thenIsSome, &narrow);
            {
                uint8_t* preInitStates;
                uint32_t initStateLen = c->localLen;
                if (H2TCSnapshotLocalInitStates(c, &preInitStates) != 0) {
                    return -1;
                }
                if (condIsConst) {
                    int32_t branchNode = condConstValue ? thenNode : elseNode;
                    uint8_t branchDiagPath = condConstValue ? thenDiagPath : elseDiagPath;
                    c->compilerDiagPathProven = branchDiagPath;
                    if (hasNarrow) {
                        int32_t origType = c->locals[narrow.localIdx].typeId;
                        int32_t trueType = thenIsSome ? narrow.innerType : c->typeNull;
                        int32_t falseType = thenIsSome ? c->typeNull : narrow.innerType;
                        c->locals[narrow.localIdx].typeId = condConstValue ? trueType : falseType;
                        if (branchNode >= 0
                            && H2TCTypeStmt(c, branchNode, returnType, loopDepth, switchDepth) != 0)
                        {
                            c->locals[narrow.localIdx].typeId = origType;
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        c->locals[narrow.localIdx].typeId = origType;
                    } else if (
                        branchNode >= 0
                        && H2TCTypeStmt(c, branchNode, returnType, loopDepth, switchDepth) != 0)
                    {
                        H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                        c->compilerDiagPathProven = savedDiagPath;
                        return -1;
                    }
                    c->compilerDiagPathProven = savedDiagPath;
                    return 0;
                }
                {
                    uint8_t* thenInitStates;
                    uint8_t* elseInitStates = preInitStates;
                    int      thenContinues;
                    int      elseContinues = 1;
                    if (hasNarrow) {
                        /*
                         * Apply branch narrowing:
                         *   x == null / !x  -> then: x is null;  else: x is T
                         *   x != null / x   -> then: x is T;     else: x is null
                         */
                        int32_t origType = c->locals[narrow.localIdx].typeId;
                        int32_t trueType = thenIsSome ? narrow.innerType : c->typeNull;
                        int32_t falseType = thenIsSome ? c->typeNull : narrow.innerType;
                        c->locals[narrow.localIdx].typeId = trueType;
                        c->compilerDiagPathProven = thenDiagPath;
                        if (H2TCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                            c->locals[narrow.localIdx].typeId = origType;
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        c->locals[narrow.localIdx].typeId = origType;
                        thenContinues = !H2TCStmtTerminates(c, thenNode);
                        if (H2TCSnapshotLocalInitStates(c, &thenInitStates) != 0) {
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                        c->locals[narrow.localIdx].typeId = falseType;
                        c->compilerDiagPathProven = elseDiagPath;
                        if (elseNode >= 0
                            && H2TCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                        {
                            c->locals[narrow.localIdx].typeId = origType;
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        c->locals[narrow.localIdx].typeId = origType;
                    } else {
                        c->compilerDiagPathProven = thenDiagPath;
                        if (H2TCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        thenContinues = !H2TCStmtTerminates(c, thenNode);
                        if (H2TCSnapshotLocalInitStates(c, &thenInitStates) != 0) {
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                        c->compilerDiagPathProven = elseDiagPath;
                        if (elseNode >= 0
                            && H2TCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                        {
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                    }
                    if (elseNode >= 0) {
                        elseContinues = !H2TCStmtTerminates(c, elseNode);
                        if (H2TCSnapshotLocalInitStates(c, &elseInitStates) != 0) {
                            H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                    }
                    if (thenContinues && elseContinues) {
                        H2TCMergeLocalInitStates(c, thenInitStates, elseInitStates, initStateLen);
                    } else if (thenContinues) {
                        H2TCRestoreLocalInitStates(c, thenInitStates, initStateLen);
                    } else if (elseContinues) {
                        H2TCRestoreLocalInitStates(c, elseInitStates, initStateLen);
                    } else {
                        H2TCRestoreLocalInitStates(c, preInitStates, initStateLen);
                    }
                }
                c->compilerDiagPathProven = savedDiagPath;
                return 0;
            }
        }
        case H2Ast_FOR:    return H2TCTypeForStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case H2Ast_SWITCH: return H2TCTypeSwitchStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case H2Ast_BREAK:
            if (loopDepth <= 0 && switchDepth <= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNEXPECTED_TOKEN);
            }
            return 0;
        case H2Ast_CONTINUE:
            if (loopDepth <= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNEXPECTED_TOKEN);
            }
            return 0;
        case H2Ast_DEFER: {
            int32_t stmt = H2AstFirstChild(c->ast, nodeId);
            if (stmt < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_UNEXPECTED_TOKEN);
            }
            return H2TCTypeStmt(c, stmt, returnType, loopDepth, switchDepth);
        }
        case H2Ast_ASSERT: {
            int32_t cond = H2AstFirstChild(c->ast, nodeId);
            int32_t condType;
            int32_t fmtNode;
            if (cond < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_BOOL);
            }
            if (H2TCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            if (!H2TCIsBoolType(c, condType)) {
                return H2TCFailNode(c, cond, H2Diag_EXPECTED_BOOL);
            }
            fmtNode = H2AstNextSibling(c->ast, cond);
            if (fmtNode >= 0) {
                int32_t fmtType;
                int32_t argNode;
                int32_t wantStrType = H2TCGetStrRefType(c, n->start, n->end);
                if (H2TCTypeExpr(c, fmtNode, &fmtType) != 0) {
                    return -1;
                }
                if (wantStrType < 0) {
                    return H2TCFailNode(c, fmtNode, H2Diag_UNKNOWN_TYPE);
                }
                if (!H2TCCanAssign(c, wantStrType, fmtType)) {
                    return H2TCFailNode(c, fmtNode, H2Diag_TYPE_MISMATCH);
                }
                argNode = H2AstNextSibling(c->ast, fmtNode);
                while (argNode >= 0) {
                    int32_t argType;
                    if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                        return -1;
                    }
                    argNode = H2AstNextSibling(c->ast, argNode);
                }
            }
            return 0;
        }
        case H2Ast_DEL: {
            int32_t expr = H2AstFirstChild(c->ast, nodeId);
            int32_t allocArgNode = -1;
            int32_t allocType = H2TCFindMemAllocatorType(c);
            int32_t ctxAllocType = -1;
            if (allocType < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
            }
            if ((n->flags & H2AstFlag_DEL_HAS_ALLOC) != 0) {
                int32_t scan = expr;
                while (scan >= 0) {
                    int32_t next = H2AstNextSibling(c->ast, scan);
                    if (next < 0) {
                        allocArgNode = scan;
                        break;
                    }
                    scan = next;
                }
                if (allocArgNode < 0
                    || H2TCValidateMemAllocatorArg(c, allocArgNode, allocType) != 0)
                {
                    return -1;
                }
            } else {
                if (H2TCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxAllocType) != 0) {
                    return -1;
                }
                if (!H2TCCanAssign(c, allocType, ctxAllocType)) {
                    return H2TCFailNode(c, nodeId, H2Diag_CONTEXT_TYPE_MISMATCH);
                }
            }
            if (expr < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
            }
            while (expr >= 0 && expr != allocArgNode) {
                int32_t t;
                int32_t resolved;
                if (H2TCTypeExpr(c, expr, &t) != 0) {
                    return -1;
                }
                resolved = H2TCResolveAliasBaseType(c, t);
                if (resolved < 0 || (uint32_t)resolved >= c->typeLen
                    || c->types[resolved].kind != H2TCType_PTR)
                {
                    return H2TCFailNode(c, expr, H2Diag_TYPE_MISMATCH);
                }
                expr = H2AstNextSibling(c->ast, expr);
            }
            return 0;
        }
        default: return H2TCFailNode(c, nodeId, H2Diag_UNEXPECTED_TOKEN);
    }
}

int H2TCTypeFunctionBody(H2TypeCheckCtx* c, int32_t funcIndex) {
    const H2TCFunction* fn = &c->funcs[funcIndex];
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
    uint32_t            savedActiveGenericArgStart = c->activeGenericArgStart;
    uint16_t            savedActiveGenericArgCount = c->activeGenericArgCount;
    int32_t             savedActiveGenericDeclNode = c->activeGenericDeclNode;
    int                 isEqualHook = 0;

    if (nodeId < 0) {
        return 0;
    }
    if ((fn->flags & H2TCFunctionFlag_TEMPLATE) != 0
        && (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0)
    {
        return 0;
    }

    c->localLen = 0;
    c->currentFunctionIndex = funcIndex;
    c->currentFunctionIsCompareHook = H2TCIsComparisonHookName(
        c, fn->nameStart, fn->nameEnd, &isEqualHook);
    c->activeTypeParamFnNode = nodeId;
    c->activeGenericArgStart = fn->templateArgStart;
    c->activeGenericArgCount = fn->templateArgCount;
    c->activeGenericDeclNode = fn->templateArgCount > 0 ? fn->declNode : -1;

    child = H2AstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
            int32_t paramType;
            int     addedLocal = 0;
            if (paramIndex >= fn->paramCount) {
                return H2TCFailNode(c, child, H2Diag_ARITY_MISMATCH);
            }
            paramType = c->funcParamTypes[fn->paramTypeStart + paramIndex];
            if (!H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")
                && H2TCLocalAdd(
                       c,
                       n->dataStart,
                       n->dataEnd,
                       paramType,
                       (c->funcParamFlags[fn->paramTypeStart + paramIndex]
                        & H2TCFuncParamFlag_CONST)
                           != 0,
                       -1)
                       != 0)
            {
                return -1;
            }
            if (!H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")) {
                addedLocal = 1;
                H2TCSetLocalUsageKind(c, (int32_t)c->localLen - 1, H2TCLocalUseKind_PARAM);
                H2TCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
            }
            if (addedLocal && (n->flags & H2AstFlag_PARAM_VARIADIC) != 0
                && (paramType == c->typeAnytype
                    || ((uint32_t)paramType < c->typeLen
                        && c->types[paramType].kind == H2TCType_PACK)))
            {
                c->locals[c->localLen - 1u].flags |= H2TCLocalFlag_ANYPACK;
            }
            paramIndex++;
        } else if (n->kind == H2Ast_BLOCK) {
            bodyNode = child;
        }
        child = H2AstNextSibling(c->ast, child);
    }

    if (bodyNode < 0) {
        c->currentFunctionIndex = savedFunctionIndex;
        c->currentFunctionIsCompareHook = savedFunctionIsCompareHook;
        c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
        c->activeGenericArgStart = savedActiveGenericArgStart;
        c->activeGenericArgCount = savedActiveGenericArgCount;
        c->activeGenericDeclNode = savedActiveGenericDeclNode;
        return 0;
    }

    c->currentContextType = H2TCResolveImplicitMainContextType(c);
    c->hasImplicitMainRootContext = c->currentContextType < 0;
    c->implicitMainContextType = c->currentContextType;

    {
        int rc = H2TCTypeBlock(c, bodyNode, fn->returnType, 0, 0);
        c->currentFunctionIndex = savedFunctionIndex;
        c->currentFunctionIsCompareHook = savedFunctionIsCompareHook;
        c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
        c->activeGenericArgStart = savedActiveGenericArgStart;
        c->activeGenericArgCount = savedActiveGenericArgCount;
        c->activeGenericDeclNode = savedActiveGenericDeclNode;
        c->currentContextType = savedContextType;
        c->hasImplicitMainRootContext = savedImplicitRoot;
        c->implicitMainContextType = savedImplicitMainContextType;
        return rc;
    }
}

int H2TCCollectFunctionDecls(H2TypeCheckCtx* c) {
    int32_t child = H2AstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (H2TCCollectFunctionFromNode(c, child) != 0) {
            return -1;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCCollectTypeDecls(H2TypeCheckCtx* c) {
    int32_t child = H2AstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (H2TCCollectTypeDeclsFromNode(c, child) != 0) {
            return -1;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCBuildCheckedContext(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    const H2TypeCheckOptions* _Nullable options,
    H2Diag* _Nullable diag,
    H2TypeCheckCtx* _Nullable outCtx) {
    H2TypeCheckCtx c;
    uint32_t       capBase;
    uint32_t       i;

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }

    if (arena == NULL || ast == NULL || ast->nodes == NULL || ast->root < 0) {
        H2TCSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    capBase = ast->len < 32 ? 32u : ast->len;

    c.arena = arena;
    c.ast = ast;
    c.src = src;
    c.filePath = options != NULL ? options->filePath : NULL;
    c.diag = diag;
    c.diagSink.ctx = options != NULL ? options->ctx : NULL;
    c.diagSink.onDiag = options != NULL ? options->onDiag : NULL;

    c.types = (H2TCType*)H2ArenaAlloc(
        arena, sizeof(H2TCType) * capBase * 4u, (uint32_t)_Alignof(H2TCType));
    c.fields = (H2TCField*)H2ArenaAlloc(
        arena, sizeof(H2TCField) * capBase * 4u, (uint32_t)_Alignof(H2TCField));
    c.namedTypes = (H2TCNamedType*)H2ArenaAlloc(
        arena, sizeof(H2TCNamedType) * capBase, (uint32_t)_Alignof(H2TCNamedType));
    c.funcs = (H2TCFunction*)H2ArenaAlloc(
        arena, sizeof(H2TCFunction) * capBase, (uint32_t)_Alignof(H2TCFunction));
    c.funcUsed = (uint8_t*)H2ArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.funcParamTypes = (int32_t*)H2ArenaAlloc(
        arena, sizeof(int32_t) * capBase * 8u, (uint32_t)_Alignof(int32_t));
    c.funcParamNameStarts = (uint32_t*)H2ArenaAlloc(
        arena, sizeof(uint32_t) * capBase * 8u, (uint32_t)_Alignof(uint32_t));
    c.funcParamNameEnds = (uint32_t*)H2ArenaAlloc(
        arena, sizeof(uint32_t) * capBase * 8u, (uint32_t)_Alignof(uint32_t));
    c.funcParamFlags = (uint8_t*)H2ArenaAlloc(
        arena, sizeof(uint8_t) * capBase * 8u, (uint32_t)_Alignof(uint8_t));
    c.genericArgTypes = (int32_t*)H2ArenaAlloc(
        arena, sizeof(int32_t) * capBase * 16u, (uint32_t)_Alignof(int32_t));
    c.scratchParamTypes = (int32_t*)H2ArenaAlloc(
        arena, sizeof(int32_t) * capBase, (uint32_t)_Alignof(int32_t));
    c.scratchParamFlags = (uint8_t*)H2ArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.locals = (H2TCLocal*)H2ArenaAlloc(
        arena, sizeof(H2TCLocal) * capBase * 4u, (uint32_t)_Alignof(H2TCLocal));
    c.localUses = (H2TCLocalUse*)H2ArenaAlloc(
        arena, sizeof(H2TCLocalUse) * capBase * 8u, (uint32_t)_Alignof(H2TCLocalUse));
    c.variantNarrows = (H2TCVariantNarrow*)H2ArenaAlloc(
        arena, sizeof(H2TCVariantNarrow) * capBase * 4u, (uint32_t)_Alignof(H2TCVariantNarrow));
    c.warningDedup = (H2TCWarningDedup*)H2ArenaAlloc(
        arena, sizeof(H2TCWarningDedup) * capBase, (uint32_t)_Alignof(H2TCWarningDedup));
    c.constDiagUses = (H2TCConstDiagUse*)H2ArenaAlloc(
        arena, sizeof(H2TCConstDiagUse) * capBase, (uint32_t)_Alignof(H2TCConstDiagUse));
    c.constDiagFnInvoked = (uint8_t*)H2ArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.callTargets = (H2TCCallTarget*)H2ArenaAlloc(
        arena, sizeof(H2TCCallTarget) * capBase * 8u, (uint32_t)_Alignof(H2TCCallTarget));
    c.constEvalValues = (H2CTFEValue*)H2ArenaAlloc(
        arena, sizeof(H2CTFEValue) * ast->len, (uint32_t)_Alignof(H2CTFEValue));
    c.constEvalState = (uint8_t*)H2ArenaAlloc(
        arena, sizeof(uint8_t) * ast->len, (uint32_t)_Alignof(uint8_t));
    c.topVarLikeTypes = (int32_t*)H2ArenaAlloc(
        arena, sizeof(int32_t) * ast->len, (uint32_t)_Alignof(int32_t));
    c.topVarLikeTypeState = (uint8_t*)H2ArenaAlloc(
        arena, sizeof(uint8_t) * ast->len, (uint32_t)_Alignof(uint8_t));

    if (c.types == NULL || c.fields == NULL || c.namedTypes == NULL || c.funcs == NULL
        || c.funcUsed == NULL || c.funcParamTypes == NULL || c.funcParamNameStarts == NULL
        || c.funcParamNameEnds == NULL || c.funcParamFlags == NULL || c.genericArgTypes == NULL
        || c.scratchParamTypes == NULL || c.scratchParamFlags == NULL || c.locals == NULL
        || c.localUses == NULL || c.constEvalValues == NULL || c.constEvalState == NULL
        || c.topVarLikeTypes == NULL || c.topVarLikeTypeState == NULL || c.variantNarrows == NULL
        || c.warningDedup == NULL || c.constDiagUses == NULL || c.constDiagFnInvoked == NULL
        || c.callTargets == NULL)
    {
        H2TCSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
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
    c.funcUsedCap = capBase;
    c.funcParamLen = 0;
    c.funcParamCap = capBase * 8u;
    c.genericArgLen = 0;
    c.genericArgCap = capBase * 16u;
    c.scratchParamCap = capBase;
    c.localLen = 0;
    c.localCap = capBase * 4u;
    c.localUseLen = 0;
    c.localUseCap = capBase * 8u;
    c.variantNarrowLen = 0;
    c.variantNarrowCap = capBase * 4u;
    c.warningDedupLen = 0;
    c.warningDedupCap = capBase;
    c.constDiagUseLen = 0;
    c.constDiagUseCap = capBase;
    c.constDiagFnInvokedCap = capBase;
    c.callTargetLen = 0;
    c.callTargetCap = capBase * 8u;
    c.currentContextType = -1;
    c.hasImplicitMainRootContext = 0;
    c.implicitMainContextType = -1;
    c.activeExpectedCallType = -1;
    c.activeCallWithNode = -1;
    c.currentFunctionIndex = -1;
    c.currentFunctionIsCompareHook = 0;
    c.activeTypeParamFnNode = -1;
    c.currentTypeOwnerTypeId = -1;
    c.activeGenericArgStart = 0;
    c.activeGenericArgCount = 0;
    c.activeGenericDeclNode = -1;
    c.activeConstEvalCtx = NULL;
    c.compilerDiagPathProven = 1;
    c.allowAnytypeParamType = 0;
    c.allowConstNumericTypeName = 0;
    c.defaultFieldNodes = NULL;
    c.defaultFieldTypes = NULL;
    c.defaultFieldCount = 0;
    c.defaultFieldCurrentIndex = 0;
    c.lastConstEvalReason = NULL;
    c.lastConstEvalReasonStart = 0;
    c.lastConstEvalReasonEnd = 0;
    c.lastConstEvalTraceDepth = 0;
    c.lastConstEvalRootFnIndex = -1;
    c.lastConstEvalRootCallStart = 0;
    memset(c.lastConstEvalTrace, 0, sizeof(c.lastConstEvalTrace));
    memset(c.funcParamFlags, 0, sizeof(uint8_t) * c.funcParamCap);
    memset(c.scratchParamFlags, 0, sizeof(uint8_t) * c.scratchParamCap);
    memset(c.funcUsed, 0, sizeof(uint8_t) * c.funcUsedCap);
    if (ast->len > 0) {
        memset(c.constEvalState, 0, sizeof(uint8_t) * ast->len);
        memset(c.topVarLikeTypeState, 0, sizeof(uint8_t) * ast->len);
        memset(c.constDiagFnInvoked, 0, sizeof(uint8_t) * c.constDiagFnInvokedCap);
        for (i = 0; i < ast->len; i++) {
            c.topVarLikeTypes[i] = -1;
            c.constEvalValues[i].kind = H2CTFEValue_INVALID;
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

    if (H2TCEnsureInitialized(&c) != 0) {
        return -1;
    }
    c.typeUsize = H2TCFindBuiltinByKind(&c, H2Builtin_USIZE);
    if (c.typeUsize < 0) {
        return H2TCFailSpan(&c, H2Diag_UNKNOWN_TYPE, 0, 0);
    }

    if (H2TCCollectTypeDecls(&c) != 0) {
        return -1;
    }
    {
        int32_t namedStrType = H2TCFindNamedTypeByLiteral(&c, "builtin__str");
        if (namedStrType < 0) {
            namedStrType = H2TCFindBuiltinNamedTypeBySuffix(&c, "__str");
        }
        if (namedStrType < 0) {
            namedStrType = H2TCFindNamedTypeByLiteral(&c, "str");
        }
        if (namedStrType >= 0) {
            c.typeStr = namedStrType;
        }
    }
    {
        int32_t namedRuneType = H2TCFindNamedTypeByLiteral(&c, "builtin__rune");
        if (namedRuneType < 0) {
            namedRuneType = H2TCFindBuiltinNamedTypeBySuffix(&c, "__rune");
        }
        if (namedRuneType < 0) {
            namedRuneType = H2TCFindNamedTypeByLiteral(&c, "rune");
        }
        if (namedRuneType >= 0) {
            c.typeRune = namedRuneType;
        }
    }
    c.typeMemAllocator = H2TCFindNamedTypeByLiteral(&c, "builtin__MemAllocator");
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = H2TCFindBuiltinNamedTypeBySuffix(&c, "__MemAllocator");
    }
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = H2TCFindNamedTypeByLiteral(&c, "MemAllocator");
    }
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = H2TCFindNamedTypeBySuffix(&c, "__MemAllocator");
    }
    if (H2TCResolveAllTypeAliases(&c) != 0) {
        return -1;
    }
    if (H2TCCollectFunctionDecls(&c) != 0) {
        return -1;
    }
    if (H2TCFinalizeFunctionTypes(&c) != 0) {
        return -1;
    }
    if (H2TCResolveAllNamedTypeFields(&c) != 0) {
        return -1;
    }
    if (H2TCCheckEmbeddedCycles(&c) != 0) {
        return -1;
    }
    if (H2TCPropagateVarSizeNamedTypes(&c) != 0) {
        return -1;
    }
    if (H2TCCheckTopLevelConstInitializers(&c) != 0) {
        return -1;
    }
    if (H2TCTypeTopLevelConsts(&c) != 0) {
        return -1;
    }
    if (H2TCTypeTopLevelVars(&c) != 0) {
        return -1;
    }

    for (i = 0; i < c.funcLen; i++) {
        if (H2TCTypeFunctionBody(&c, (int32_t)i) != 0) {
            return -1;
        }
    }
    if (H2TCValidateTopLevelConstEvaluable(&c) != 0) {
        return -1;
    }
    if (H2TCValidateConstDiagUses(&c) != 0) {
        return -1;
    }
    if (H2TCMarkTemplateRootFunctionUses(&c) != 0) {
        return -1;
    }
    if (H2TCEmitUnusedSymbolWarnings(&c) != 0) {
        return -1;
    }

    if (outCtx != NULL) {
        *outCtx = c;
    }

    return 0;
}

H2_API_END
