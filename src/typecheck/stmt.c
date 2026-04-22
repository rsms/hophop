#include "internal.h"

HOP_API_BEGIN

static int HOPTCMarkTemplateRootUsesVisitNode(HOPTypeCheckCtx* c, int32_t nodeId) {
    const HOPAstNode* n;
    int32_t           child;
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == HOPAst_CALL) {
        int32_t calleeNode = HOPAstFirstChild(c->ast, nodeId);
        if (calleeNode >= 0 && (uint32_t)calleeNode < c->ast->len) {
            const HOPAstNode* callee = &c->ast->nodes[calleeNode];
            if (callee->kind == HOPAst_IDENT) {
                int32_t fnIndex = HOPTCFindFunctionIndex(c, callee->dataStart, callee->dataEnd);
                if (fnIndex >= 0) {
                    HOPTCMarkFunctionUsed(c, fnIndex);
                }
            }
        }
    }
    child = HOPAstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        if (HOPTCMarkTemplateRootUsesVisitNode(c, child) != 0) {
            return -1;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

int HOPTCMarkTemplateRootFunctionUses(HOPTypeCheckCtx* c) {
    uint32_t i;
    if (c == NULL || c->ast == NULL) {
        return -1;
    }
    for (i = 0; i < c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        int32_t              child;
        if ((fn->flags & HOPTCFunctionFlag_TEMPLATE) == 0
            || (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) != 0 || fn->defNode < 0)
        {
            continue;
        }
        child = HOPAstFirstChild(c->ast, fn->defNode);
        while (child >= 0) {
            if (c->ast->nodes[child].kind == HOPAst_BLOCK) {
                if (HOPTCMarkTemplateRootUsesVisitNode(c, child) != 0) {
                    return -1;
                }
                break;
            }
            child = HOPAstNextSibling(c->ast, child);
        }
    }
    return 0;
}

static void HOPTCMarkConstBlockLocalReadsRec(
    HOPTypeCheckCtx* c, int32_t nodeId, int skipDirectIdent) {
    const HOPAstNode* n;
    int32_t           child;
    if (c == NULL || c->ast == NULL || nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    n = &c->ast->nodes[nodeId];
    if (!skipDirectIdent && n->kind == HOPAst_IDENT) {
        int32_t localIdx = HOPTCLocalFind(c, n->dataStart, n->dataEnd);
        if (localIdx >= 0) {
            HOPTCMarkLocalRead(c, localIdx);
        }
        return;
    }
    if (n->kind == HOPAst_VAR || n->kind == HOPAst_CONST) {
        child = HOPAstFirstChild(c->ast, nodeId);
        if (child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == HOPAst_NAME_LIST)
        {
            child = HOPAstNextSibling(c->ast, child);
        }
        while (child >= 0) {
            HOPTCMarkConstBlockLocalReadsRec(c, child, 0);
            child = HOPAstNextSibling(c->ast, child);
        }
        return;
    }
    if (n->kind == HOPAst_BINARY && n->op == HOPTok_ASSIGN) {
        int32_t lhsNode = HOPAstFirstChild(c->ast, nodeId);
        int32_t rhsNode = lhsNode >= 0 ? HOPAstNextSibling(c->ast, lhsNode) : -1;
        if (lhsNode >= 0 && (uint32_t)lhsNode < c->ast->len
            && c->ast->nodes[lhsNode].kind != HOPAst_IDENT)
        {
            HOPTCMarkConstBlockLocalReadsRec(c, lhsNode, 0);
        }
        if (rhsNode >= 0) {
            HOPTCMarkConstBlockLocalReadsRec(c, rhsNode, 0);
        }
        return;
    }
    child = HOPAstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        HOPTCMarkConstBlockLocalReadsRec(c, child, 0);
        child = HOPAstNextSibling(c->ast, child);
    }
}

static void HOPTCMarkConstBlockLocalReads(HOPTypeCheckCtx* c, int32_t blockNode) {
    int32_t child;
    if (c == NULL || c->ast == NULL || blockNode < 0 || (uint32_t)blockNode >= c->ast->len
        || c->ast->nodes[blockNode].kind != HOPAst_BLOCK)
    {
        return;
    }
    child = HOPAstFirstChild(c->ast, blockNode);
    while (child >= 0) {
        HOPTCMarkConstBlockLocalReadsRec(c, child, 0);
        child = HOPAstNextSibling(c->ast, child);
    }
}

int HOPTCBlockTerminates(HOPTypeCheckCtx* c, int32_t blockNode) {
    int32_t child = HOPAstFirstChild(c->ast, blockNode);
    int32_t last = -1;
    while (child >= 0) {
        last = child;
        child = HOPAstNextSibling(c->ast, child);
    }
    if (last < 0) {
        return 0;
    }
    switch (c->ast->nodes[last].kind) {
        case HOPAst_RETURN:
        case HOPAst_BREAK:
        case HOPAst_CONTINUE: return 1;
        default:              return 0;
    }
}

static int HOPTCSnapshotLocalInitStates(HOPTypeCheckCtx* c, uint8_t** outStates) {
    uint32_t i;
    uint8_t* states;
    *outStates = NULL;
    if (c->localLen == 0) {
        return 0;
    }
    states = (uint8_t*)HOPArenaAlloc(c->arena, c->localLen * sizeof(uint8_t), 1);
    if (states == NULL) {
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, 0, 0);
    }
    for (i = 0; i < c->localLen; i++) {
        states[i] = c->locals[i].initState;
    }
    *outStates = states;
    return 0;
}

static void HOPTCRestoreLocalInitStates(
    HOPTypeCheckCtx* c, const uint8_t* states, uint32_t stateLen) {
    uint32_t i;
    uint32_t len = c->localLen < stateLen ? c->localLen : stateLen;
    if (states == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        c->locals[i].initState = states[i];
    }
}

static uint8_t HOPTCMergeLocalInitState(uint8_t a, uint8_t b) {
    if (a == HOPTCLocalInit_UNTRACKED || b == HOPTCLocalInit_UNTRACKED) {
        return HOPTCLocalInit_UNTRACKED;
    }
    if (a == b) {
        return a;
    }
    return HOPTCLocalInit_MAYBE;
}

static void HOPTCMergeLocalInitStates(
    HOPTypeCheckCtx* c, const uint8_t* a, const uint8_t* b, uint32_t stateLen) {
    uint32_t i;
    uint32_t len = c->localLen < stateLen ? c->localLen : stateLen;
    if (a == NULL || b == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        c->locals[i].initState = HOPTCMergeLocalInitState(a[i], b[i]);
    }
}

static void HOPTCMergeLocalInitStateBuffers(uint8_t* dst, const uint8_t* src, uint32_t stateLen) {
    uint32_t i;
    if (dst == NULL || src == NULL) {
        return;
    }
    for (i = 0; i < stateLen; i++) {
        dst[i] = HOPTCMergeLocalInitState(dst[i], src[i]);
    }
}

static int HOPTCStmtTerminates(HOPTypeCheckCtx* c, int32_t nodeId) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case HOPAst_RETURN:
        case HOPAst_BREAK:
        case HOPAst_CONTINUE: return 1;
        case HOPAst_BLOCK:    return HOPTCBlockTerminates(c, nodeId);
        default:              return 0;
    }
}

static int HOPTCStmtExitsSwitchWithoutContinuing(HOPTypeCheckCtx* c, int32_t nodeId) {
    int32_t child;
    int32_t last = -1;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return 0;
    }
    switch (c->ast->nodes[nodeId].kind) {
        case HOPAst_RETURN:
        case HOPAst_CONTINUE: return 1;
        case HOPAst_BLOCK:    break;
        default:              return 0;
    }
    child = HOPAstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        last = child;
        child = HOPAstNextSibling(c->ast, child);
    }
    if (last < 0) {
        return 0;
    }
    return HOPTCStmtExitsSwitchWithoutContinuing(c, last);
}

int HOPTCTypeBlock(
    HOPTypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t        savedLocalLen = c->localLen;
    uint32_t        savedVariantNarrowLen = c->variantNarrowLen;
    int32_t         child = HOPAstFirstChild(c->ast, blockNode);
    HOPTCNarrowSave narrows[8]; /* saved narrowings applied during this block */
    int             narrowLen = 0;
    int             i;

    while (child >= 0) {
        int32_t next = HOPAstNextSibling(c->ast, child);
        if (HOPTCTypeStmt(c, child, returnType, loopDepth, switchDepth) != 0) {
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
        if (next >= 0 && c->ast->nodes[child].kind == HOPAst_IF && narrowLen < 8) {
            int32_t         condNode = HOPAstFirstChild(c->ast, child);
            int32_t         thenNode = condNode >= 0 ? HOPAstNextSibling(c->ast, condNode) : -1;
            int32_t         elseNode = thenNode >= 0 ? HOPAstNextSibling(c->ast, thenNode) : -1;
            HOPTCNullNarrow narrow;
            int             thenIsSome;
            if (elseNode < 0 && thenNode >= 0 && condNode >= 0 && HOPTCBlockTerminates(c, thenNode)
                && HOPTCGetOptionalCondNarrow(c, condNode, &thenIsSome, &narrow))
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

static void HOPTCAttachForInNextHookNoMatchingDetail(
    HOPTypeCheckCtx* c, int hasKey, int valueDiscard, int32_t iterType) {
    char         detailBuf[512];
    char         iterTypeBuf[HOPTC_DIAG_TEXT_CAP];
    HOPTCTextBuf b;
    HOPTCTextBuf iterTypeText;
    const char*  iterTypeName = "Iter";
    if (c == NULL || c->diag == NULL) {
        return;
    }
    HOPTCTextBufInit(&iterTypeText, iterTypeBuf, (uint32_t)sizeof(iterTypeBuf));
    if (iterType >= 0 && (uint32_t)iterType < c->typeLen) {
        HOPTCFormatTypeRec(c, iterType, &iterTypeText, 0);
        iterTypeName = iterTypeBuf;
    }
    HOPTCTextBufInit(&b, detailBuf, (uint32_t)sizeof(detailBuf));
    if (!hasKey) {
        HOPTCTextBufAppendCStr(&b, "loop form requires ");
        HOPTCTextBufAppendCStr(&b, "next_value(it *");
        HOPTCTextBufAppendCStr(&b, iterTypeName);
        HOPTCTextBufAppendCStr(&b, ")");
        HOPTCTextBufAppendCStr(&b, " or ");
        HOPTCTextBufAppendCStr(&b, "next_key_and_value(it *");
        HOPTCTextBufAppendCStr(&b, iterTypeName);
        HOPTCTextBufAppendCStr(&b, ")");
    } else if (valueDiscard) {
        HOPTCTextBufAppendCStr(&b, "loop form requires ");
        HOPTCTextBufAppendCStr(&b, "next_key(it *");
        HOPTCTextBufAppendCStr(&b, iterTypeName);
        HOPTCTextBufAppendCStr(&b, ")");
        HOPTCTextBufAppendCStr(&b, " or ");
        HOPTCTextBufAppendCStr(&b, "next_key_and_value(it *");
        HOPTCTextBufAppendCStr(&b, iterTypeName);
        HOPTCTextBufAppendCStr(&b, ")");
    } else {
        HOPTCTextBufAppendCStr(&b, "loop form requires ");
        HOPTCTextBufAppendCStr(&b, "next_key_and_value(it *");
        HOPTCTextBufAppendCStr(&b, iterTypeName);
        HOPTCTextBufAppendCStr(&b, ")");
    }
    c->diag->detail = HOPTCAllocDiagText(c, detailBuf);
}

static int HOPTCForInPayloadTypeFromOptional(
    HOPTypeCheckCtx* c, int32_t optionalType, int32_t* outPayloadType) {
    const HOPTCType* t;
    int32_t          payloadType;
    if (optionalType < 0 || (uint32_t)optionalType >= c->typeLen) {
        return 0;
    }
    t = &c->types[optionalType];
    if (t->kind != HOPTCType_OPTIONAL || t->baseType < 0) {
        return 0;
    }
    payloadType = t->baseType;
    if ((uint32_t)payloadType >= c->typeLen) {
        return 0;
    }
    *outPayloadType = payloadType;
    return 1;
}

static int HOPTCForInValueLocalTypeFromPayload(
    HOPTypeCheckCtx* c, int32_t payloadType, HOPTCForInValueMode mode, int32_t* outLocalType) {
    const HOPTCType* payload;
    if (payloadType < 0 || (uint32_t)payloadType >= c->typeLen) {
        return 0;
    }
    payload = &c->types[payloadType];
    if (mode == HOPTCForInValueMode_REF) {
        if (payload->kind != HOPTCType_PTR && payload->kind != HOPTCType_REF) {
            return 0;
        }
        *outLocalType = payloadType;
        return 1;
    }
    if (mode == HOPTCForInValueMode_VALUE) {
        if (payload->kind == HOPTCType_PTR || payload->kind == HOPTCType_REF) {
            if (payload->baseType < 0 || (uint32_t)payload->baseType >= c->typeLen) {
                return 0;
            }
            *outLocalType = payload->baseType;
            return 1;
        }
        *outLocalType = payloadType;
        return 1;
    }
    if (mode == HOPTCForInValueMode_ANY) {
        *outLocalType = payloadType;
        return 1;
    }
    return 0;
}

static int HOPTCForInValueLocalTypeFromDirect(
    HOPTypeCheckCtx* c, int32_t valueType, HOPTCForInValueMode mode, int32_t* outLocalType) {
    const HOPTCType* value;
    if (valueType < 0 || (uint32_t)valueType >= c->typeLen) {
        return 0;
    }
    value = &c->types[valueType];
    if (mode == HOPTCForInValueMode_ANY) {
        *outLocalType = valueType;
        return 1;
    }
    if (mode == HOPTCForInValueMode_REF) {
        if (value->kind != HOPTCType_PTR && value->kind != HOPTCType_REF) {
            return 0;
        }
        *outLocalType = valueType;
        return 1;
    }
    if (mode == HOPTCForInValueMode_VALUE) {
        if (value->kind == HOPTCType_PTR || value->kind == HOPTCType_REF) {
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

static int HOPTCForInTuple2ValueTypesFromPayload(
    HOPTypeCheckCtx* c, int32_t payloadType, int32_t* outKeyType, int32_t* outValueType) {
    const HOPTCType* payload;
    int32_t          pairType = payloadType;
    if (payloadType < 0 || (uint32_t)payloadType >= c->typeLen) {
        return 0;
    }
    payload = &c->types[payloadType];
    if ((payload->kind == HOPTCType_PTR || payload->kind == HOPTCType_REF)
        && payload->baseType >= 0)
    {
        pairType = payload->baseType;
    }
    if (pairType < 0 || (uint32_t)pairType >= c->typeLen) {
        return 0;
    }
    pairType = HOPTCResolveAliasBaseType(c, pairType);
    if (pairType < 0 || (uint32_t)pairType >= c->typeLen) {
        return 0;
    }
    if (c->types[pairType].kind != HOPTCType_TUPLE || c->types[pairType].fieldCount != 2
        || c->types[pairType].fieldStart + 2 > c->funcParamLen)
    {
        return 0;
    }
    *outKeyType = c->funcParamTypes[c->types[pairType].fieldStart];
    *outValueType = c->funcParamTypes[c->types[pairType].fieldStart + 1];
    return 1;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
static int HOPTCResolveForInIteratorFromType(
    HOPTypeCheckCtx* c, int32_t sourceType, int32_t* outFnIndex, int32_t* outIterType) {
    int32_t bestFn = -1;
    int32_t bestIterType = -1;
    uint8_t bestCost = 0;
    int     nameFound = 0;
    int     ambiguous = 0;
    int     i;

    for (i = 0; i < (int)c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        int32_t              paramType;
        uint8_t              cost = 0;
        if (!HOPNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "__iterator")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & HOPTCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen)
        {
            continue;
        }
        paramType = c->funcParamTypes[fn->paramTypeStart];
        if (HOPTCConversionCost(c, paramType, sourceType, &cost) != 0) {
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
int HOPTCResolveForInIterator(
    HOPTypeCheckCtx* c,
    int32_t          sourceNode,
    int32_t          sourceType,
    int32_t*         outFnIndex,
    int32_t*         outIterType) {
    int rc = HOPTCResolveForInIteratorFromType(c, sourceType, outFnIndex, outIterType);
    if (rc == 2 && sourceNode >= 0 && (uint32_t)sourceNode < c->ast->len
        && HOPTCExprIsAssignable(c, sourceNode))
    {
        const HOPAstNode* srcNode = &c->ast->nodes[sourceNode];
        int32_t autoRefType = HOPTCInternPtrType(c, sourceType, srcNode->start, srcNode->end);
        if (autoRefType < 0) {
            return -1;
        }
        rc = HOPTCResolveForInIteratorFromType(c, autoRefType, outFnIndex, outIterType);
    }
    return rc;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous, 4 bad return type */
int HOPTCResolveForInNextValue(
    HOPTypeCheckCtx*    c,
    int32_t             iterPtrType,
    HOPTCForInValueMode valueMode,
    int32_t*            outValueType,
    int32_t*            outFn) {
    int32_t bestFn = -1;
    uint8_t bestCost = 0;
    int32_t bestValueType = -1;
    int     nameFound = 0;
    int     badReturnType = 0;
    int     ambiguous = 0;
    int     i;
    for (i = 0; i < (int)c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        int32_t              p0Type;
        uint8_t              cost = 0;
        int32_t              payloadType = -1;
        int32_t              valueType = -1;
        if (!HOPNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "next_value")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & HOPTCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen
            || fn->paramTypeStart + fn->paramCount > c->funcParamLen)
        {
            continue;
        }
        p0Type = c->funcParamTypes[fn->paramTypeStart];
        if (HOPTCConversionCost(c, p0Type, iterPtrType, &cost) != 0) {
            continue;
        }
        if (!HOPTCForInPayloadTypeFromOptional(c, fn->returnType, &payloadType)
            || !HOPTCForInValueLocalTypeFromPayload(c, payloadType, valueMode, &valueType))
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
int HOPTCResolveForInNextKey(
    HOPTypeCheckCtx* c, int32_t iterPtrType, int32_t* outKeyType, int32_t* outFn) {
    int32_t bestFn = -1;
    uint8_t bestCost = 0;
    int32_t bestKeyType = -1;
    int     nameFound = 0;
    int     badReturnType = 0;
    int     ambiguous = 0;
    int     i;
    for (i = 0; i < (int)c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        int32_t              p0Type;
        uint8_t              cost = 0;
        int32_t              payloadType = -1;
        if (!HOPNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "next_key")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & HOPTCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen
            || fn->paramTypeStart + fn->paramCount > c->funcParamLen)
        {
            continue;
        }
        p0Type = c->funcParamTypes[fn->paramTypeStart];
        if (HOPTCConversionCost(c, p0Type, iterPtrType, &cost) != 0) {
            continue;
        }
        if (!HOPTCForInPayloadTypeFromOptional(c, fn->returnType, &payloadType)) {
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
int HOPTCResolveForInNextKeyAndValue(
    HOPTypeCheckCtx*    c,
    int32_t             iterPtrType,
    HOPTCForInValueMode valueMode,
    int32_t*            outKeyType,
    int32_t*            outValueType,
    int32_t*            outFn) {
    int32_t bestFn = -1;
    uint8_t bestCost = 0;
    int32_t bestKeyType = -1;
    int32_t bestValueType = -1;
    int     nameFound = 0;
    int     badReturnType = 0;
    int     ambiguous = 0;
    int     i;
    for (i = 0; i < (int)c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        int32_t              p0Type;
        uint8_t              cost = 0;
        int32_t              payloadType = -1;
        int32_t              keyFieldType = -1;
        int32_t              valueFieldType = -1;
        int32_t              valueType = -1;
        if (!HOPNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "next_key_and_value")) {
            continue;
        }
        nameFound = 1;
        if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0
            || ((fn->flags & HOPTCFunctionFlag_TEMPLATE) != 0
                && (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
            || fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen
            || fn->paramTypeStart + fn->paramCount > c->funcParamLen)
        {
            continue;
        }
        p0Type = c->funcParamTypes[fn->paramTypeStart];
        if (HOPTCConversionCost(c, p0Type, iterPtrType, &cost) != 0) {
            continue;
        }
        if (!HOPTCForInPayloadTypeFromOptional(c, fn->returnType, &payloadType)
            || !HOPTCForInTuple2ValueTypesFromPayload(
                c, payloadType, &keyFieldType, &valueFieldType)
            || !HOPTCForInValueLocalTypeFromDirect(c, valueFieldType, valueMode, &valueType))
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

static int HOPTCTypeForInStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const HOPAstNode*   forNode = &c->ast->nodes[nodeId];
    uint32_t            savedLocalLen = c->localLen;
    int                 hasKey = (forNode->flags & HOPAstFlag_FOR_IN_HAS_KEY) != 0;
    int                 keyRef = (forNode->flags & HOPAstFlag_FOR_IN_KEY_REF) != 0;
    int                 valueRef = (forNode->flags & HOPAstFlag_FOR_IN_VALUE_REF) != 0;
    int                 valueDiscard = (forNode->flags & HOPAstFlag_FOR_IN_VALUE_DISCARD) != 0;
    int32_t             nodes[4];
    int                 count = 0;
    int32_t             child = HOPAstFirstChild(c->ast, nodeId);
    int32_t             keyNode = -1;
    int32_t             valueNode = -1;
    int32_t             sourceNode = -1;
    int32_t             bodyNode = -1;
    int32_t             sourceType = -1;
    HOPTCIndexBaseInfo  sourceInfo;
    int                 useSequencePath = 0;
    int32_t             keyLocalType = -1;
    int32_t             valueLocalType = -1;
    int32_t             iterTypeForDiag = -1;
    HOPTCForInValueMode requestedValueMode = HOPTCForInValueMode_VALUE;

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = HOPAstNextSibling(c->ast, child);
    }
    if (count == 0 || child >= 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    if (hasKey) {
        if (count != 4) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        keyNode = nodes[0];
        valueNode = nodes[1];
        sourceNode = nodes[2];
        bodyNode = nodes[3];
    } else {
        if (count != 3) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        valueNode = nodes[0];
        sourceNode = nodes[1];
        bodyNode = nodes[2];
    }

    if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != HOPAst_BLOCK) {
        return HOPTCFailNode(c, nodeId, HOPDiag_UNEXPECTED_TOKEN);
    }
    if (valueDiscard) {
        const HOPAstNode* valueIdent = &c->ast->nodes[valueNode];
        if (valueIdent->kind != HOPAst_IDENT
            || !HOPNameEqLiteral(c->src, valueIdent->dataStart, valueIdent->dataEnd, "_"))
        {
            return HOPTCFailNode(c, valueNode, HOPDiag_FOR_IN_VALUE_BINDING_INVALID);
        }
    }

    if (HOPTCTypeExpr(c, sourceNode, &sourceType) != 0) {
        return -1;
    }
    if (valueRef) {
        requestedValueMode = HOPTCForInValueMode_REF;
    }
    if (HOPTCTypeSupportsLen(c, sourceType)
        && HOPTCResolveIndexBaseInfo(c, sourceType, &sourceInfo) == 0 && sourceInfo.indexable
        && sourceInfo.elemType >= 0)
    {
        useSequencePath = 1;
    }

    if (useSequencePath) {
        if (keyRef) {
            return HOPTCFailNode(c, keyNode, HOPDiag_FOR_IN_KEY_REF_INVALID);
        }

        if (hasKey) {
            keyLocalType = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
            if (keyLocalType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNKNOWN_TYPE);
            }
        }
        if (!valueDiscard) {
            const HOPAstNode* valueName = &c->ast->nodes[valueNode];
            valueLocalType = sourceInfo.elemType;
            if (valueRef) {
                if (sourceInfo.sliceMutable) {
                    valueLocalType = HOPTCInternPtrType(
                        c, sourceInfo.elemType, valueName->start, valueName->end);
                } else {
                    valueLocalType = HOPTCInternRefType(
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
            return HOPTCFailNode(c, keyNode, HOPDiag_FOR_IN_KEY_REF_INVALID);
        }

        rc = HOPTCResolveForInIterator(c, sourceNode, sourceType, &iterFn, &iterType);
        if (rc == 1 || rc == 2) {
            return HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_INVALID_SOURCE);
        }
        if (rc == 3) {
            return HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ITERATOR_AMBIGUOUS);
        }
        if (rc != 0) {
            return -1;
        }
        if (HOPTCValidateCallContextRequirements(c, c->funcs[iterFn].contextType) != 0) {
            return -1;
        }
        HOPTCMarkFunctionUsed(c, iterFn);
        iterTypeForDiag = iterType;
        iterPtrType = HOPTCInternPtrType(
            c, iterType, c->ast->nodes[sourceNode].start, c->ast->nodes[sourceNode].end);
        if (iterPtrType < 0) {
            return -1;
        }

        if (hasKey && valueDiscard) {
            rc = HOPTCResolveForInNextKey(c, iterPtrType, &keyLocalType, &nextKeyFn);
            if (rc == 1 || rc == 2) {
                rc = HOPTCResolveForInNextKeyAndValue(
                    c,
                    iterPtrType,
                    HOPTCForInValueMode_ANY,
                    &keyLocalType,
                    &valueLocalType,
                    &nextPairFn);
                if (rc == 0) {
                    useNextPair = 1;
                }
            }
            if (rc == 4) {
                return HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NON_BOOL);
            }
            if (rc == 1 || rc == 2 || rc == 3) {
                int err = HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                HOPTCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
                return err;
            }
            if (rc != 0) {
                return -1;
            }
        } else if (hasKey) {
            rc = HOPTCResolveForInNextKeyAndValue(
                c,
                iterPtrType,
                valueDiscard ? HOPTCForInValueMode_ANY : requestedValueMode,
                &keyLocalType,
                &valueLocalType,
                &nextPairFn);
            if (rc == 4) {
                return HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NON_BOOL);
            }
            if (rc == 1 || rc == 2 || rc == 3) {
                int err = HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                HOPTCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
                return err;
            }
            if (rc != 0) {
                return -1;
            }
            useNextPair = 1;
        } else {
            rc = HOPTCResolveForInNextValue(
                c,
                iterPtrType,
                valueDiscard ? HOPTCForInValueMode_ANY : requestedValueMode,
                &valueLocalType,
                &nextValueFn);
            if (rc == 1 || rc == 2) {
                rc = HOPTCResolveForInNextKeyAndValue(
                    c,
                    iterPtrType,
                    valueDiscard ? HOPTCForInValueMode_ANY : requestedValueMode,
                    &keyLocalType,
                    &valueLocalType,
                    &nextPairFn);
                if (rc == 0) {
                    useNextPair = 1;
                }
            }
            if (rc == 4) {
                return HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NON_BOOL);
            }
            if (rc == 1 || rc == 2 || rc == 3) {
                int err = HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                HOPTCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
                return err;
            }
            if (rc != 0) {
                return -1;
            }
        }

        if (nextValueFn >= 0 && !useNextPair) {
            if (HOPTCValidateCallContextRequirements(c, c->funcs[nextValueFn].contextType) != 0) {
                return -1;
            }
            HOPTCMarkFunctionUsed(c, nextValueFn);
        }
        if (nextKeyFn >= 0 && !useNextPair) {
            if (HOPTCValidateCallContextRequirements(c, c->funcs[nextKeyFn].contextType) != 0) {
                return -1;
            }
            HOPTCMarkFunctionUsed(c, nextKeyFn);
        }
        if (nextPairFn >= 0 && useNextPair) {
            if (HOPTCValidateCallContextRequirements(c, c->funcs[nextPairFn].contextType) != 0) {
                return -1;
            }
            HOPTCMarkFunctionUsed(c, nextPairFn);
        }
    }

    if (hasKey) {
        const HOPAstNode* keyName = &c->ast->nodes[keyNode];
        if (keyLocalType < 0) {
            int err = HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
            HOPTCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
            return err;
        }
        if (HOPTCLocalAdd(c, keyName->dataStart, keyName->dataEnd, keyLocalType, 0, -1) != 0) {
            return -1;
        }
        HOPTCMarkLocalWrite(c, (int32_t)c->localLen - 1);
        HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
    }
    if (!valueDiscard) {
        const HOPAstNode* valueName = &c->ast->nodes[valueNode];
        if (valueLocalType < 0) {
            int err = HOPTCFailNode(c, sourceNode, HOPDiag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
            HOPTCAttachForInNextHookNoMatchingDetail(c, hasKey, valueDiscard, iterTypeForDiag);
            return err;
        }
        if (HOPTCLocalAdd(c, valueName->dataStart, valueName->dataEnd, valueLocalType, 0, -1) != 0)
        {
            return -1;
        }
        HOPTCMarkLocalWrite(c, (int32_t)c->localLen - 1);
        HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
    }

    {
        uint8_t* beforeBodyStates;
        uint32_t beforeBodyStateLen = c->localLen;
        uint8_t  savedDiagPath = c->compilerDiagPathProven;
        if (HOPTCSnapshotLocalInitStates(c, &beforeBodyStates) != 0) {
            c->localLen = savedLocalLen;
            return -1;
        }
        c->compilerDiagPathProven = 0;
        if (HOPTCTypeBlock(c, bodyNode, returnType, loopDepth + 1, switchDepth) != 0) {
            c->compilerDiagPathProven = savedDiagPath;
            HOPTCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
            return -1;
        }
        c->compilerDiagPathProven = savedDiagPath;
        HOPTCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
    }
    c->localLen = savedLocalLen;
    return 0;
}

int HOPTCTypeShortAssignStmt(HOPTypeCheckCtx* c, int32_t nodeId);

int HOPTCTypeForStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const HOPAstNode* forNode = &c->ast->nodes[nodeId];
    uint32_t          savedLocalLen = c->localLen;
    int32_t           child = HOPAstFirstChild(c->ast, nodeId);
    int32_t           nodes[4];
    int               count = 0;
    int               i;

    if ((forNode->flags & HOPAstFlag_FOR_IN) != 0) {
        return HOPTCTypeForInStmt(c, nodeId, returnType, loopDepth, switchDepth);
    }

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = HOPAstNextSibling(c->ast, child);
    }

    if (count == 0) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }

    if (c->ast->nodes[nodes[count - 1]].kind != HOPAst_BLOCK) {
        return HOPTCFailNode(c, nodes[count - 1], HOPDiag_UNEXPECTED_TOKEN);
    }

    if (count == 2 || count == 4) {
        int     condIndex = count == 2 ? 0 : 1;
        int32_t condType;
        if (count == 4) {
            const HOPAstNode* initNode = &c->ast->nodes[nodes[0]];
            if (initNode->kind == HOPAst_VAR || initNode->kind == HOPAst_CONST) {
                if (HOPTCTypeVarLike(c, nodes[0]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else if (initNode->kind == HOPAst_SHORT_ASSIGN) {
                if (HOPTCTypeShortAssignStmt(c, nodes[0]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else {
                int32_t initType;
                if (HOPTCTypeExpr(c, nodes[0], &initType) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            }
        }
        if (HOPTCTypeExpr(c, nodes[condIndex], &condType) != 0) {
            c->localLen = savedLocalLen;
            return -1;
        }
        if (!HOPTCIsBoolType(c, condType)) {
            c->localLen = savedLocalLen;
            return HOPTCFailNode(c, nodes[condIndex], HOPDiag_EXPECTED_BOOL);
        }
    } else {
        for (i = 0; i < count - 1; i++) {
            const HOPAstNode* n = &c->ast->nodes[nodes[i]];
            if (n->kind == HOPAst_VAR || n->kind == HOPAst_CONST) {
                if (HOPTCTypeVarLike(c, nodes[i]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else if (n->kind == HOPAst_SHORT_ASSIGN) {
                if (HOPTCTypeShortAssignStmt(c, nodes[i]) != 0) {
                    c->localLen = savedLocalLen;
                    return -1;
                }
            } else {
                int32_t t;
                if (HOPTCTypeExpr(c, nodes[i], &t) != 0) {
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
        if (HOPTCSnapshotLocalInitStates(c, &beforeBodyStates) != 0) {
            c->localLen = savedLocalLen;
            return -1;
        }
        c->compilerDiagPathProven = 0;
        if (HOPTCTypeBlock(c, nodes[count - 1], returnType, loopDepth + 1, switchDepth) != 0) {
            c->compilerDiagPathProven = savedDiagPath;
            HOPTCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
            c->localLen = savedLocalLen;
            return -1;
        }
        c->compilerDiagPathProven = savedDiagPath;
        HOPTCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
        if (count == 4) {
            int32_t postType;
            if (HOPTCTypeExpr(c, nodes[2], &postType) != 0) {
                c->localLen = savedLocalLen;
                return -1;
            }
            HOPTCRestoreLocalInitStates(c, beforeBodyStates, beforeBodyStateLen);
        }
    }

    c->localLen = savedLocalLen;
    return 0;
}

int HOPTCTypeSwitchStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const HOPAstNode* sw = &c->ast->nodes[nodeId];
    int32_t           child = HOPAstFirstChild(c->ast, nodeId);
    int32_t           subjectNode = -1;
    int32_t           subjectType = -1;
    int32_t           subjectEnumType = -1;
    int32_t           subjectLocalIdx = -1;
    uint32_t          enumVariantCount = 0;
    uint32_t*         enumVariantStarts = NULL;
    uint32_t*         enumVariantEnds = NULL;
    uint8_t*          enumCovered = NULL;
    int               boolCoveredTrue = 0;
    int               boolCoveredFalse = 0;
    int               hasDefault = 0;
    uint8_t*          preSwitchStates = NULL;
    uint8_t*          mergedSwitchStates = NULL;
    uint32_t          switchStateLen = 0;
    int               hasMergedSwitchState = 0;
    int               i;

    if (sw->flags == 1) {
        if (child < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        subjectNode = child;
        if (HOPTCTypeExpr(c, child, &subjectType) != 0) {
            return -1;
        }
        if (c->ast->nodes[subjectNode].kind == HOPAst_IDENT) {
            subjectLocalIdx = HOPTCLocalFind(
                c, c->ast->nodes[subjectNode].dataStart, c->ast->nodes[subjectNode].dataEnd);
        }
        if (HOPTCIsNamedDeclKind(c, subjectType, HOPAst_ENUM)) {
            int32_t declNode = c->types[HOPTCResolveAliasBaseType(c, subjectType)].declNode;
            int32_t variant = HOPTCEnumDeclFirstVariantNode(c, declNode);
            while (variant >= 0) {
                if (c->ast->nodes[variant].kind == HOPAst_FIELD) {
                    enumVariantCount++;
                }
                variant = HOPAstNextSibling(c->ast, variant);
            }
            if (enumVariantCount > 0) {
                uint32_t idx = 0;
                variant = HOPTCEnumDeclFirstVariantNode(c, declNode);
                enumVariantStarts = (uint32_t*)HOPArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
                enumVariantEnds = (uint32_t*)HOPArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint32_t), (uint32_t)_Alignof(uint32_t));
                enumCovered = (uint8_t*)HOPArenaAlloc(
                    c->arena, enumVariantCount * sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
                if (enumVariantStarts == NULL || enumVariantEnds == NULL || enumCovered == NULL) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_ARENA_OOM);
                }
                while (variant >= 0 && idx < enumVariantCount) {
                    if (c->ast->nodes[variant].kind == HOPAst_FIELD) {
                        enumVariantStarts[idx] = c->ast->nodes[variant].dataStart;
                        enumVariantEnds[idx] = c->ast->nodes[variant].dataEnd;
                        enumCovered[idx] = 0;
                        idx++;
                    }
                    variant = HOPAstNextSibling(c->ast, variant);
                }
                subjectEnumType = HOPTCResolveAliasBaseType(c, subjectType);
            }
        }
        child = HOPAstNextSibling(c->ast, child);
    }

    switchStateLen = c->localLen;
    if (HOPTCSnapshotLocalInitStates(c, &preSwitchStates) != 0) {
        return -1;
    }

    while (child >= 0) {
        const HOPAstNode* clause = &c->ast->nodes[child];
        if (clause->kind == HOPAst_CASE) {
            uint32_t savedLocalLen = c->localLen;
            uint32_t savedVariantNarrowLen = c->variantNarrowLen;
            int32_t  caseChild = HOPAstFirstChild(c->ast, child);
            int32_t  bodyNode = -1;
            int      labelCount = 0;
            int      singleVariantLabel = 0;
            uint32_t singleVariantStart = 0;
            uint32_t singleVariantEnd = 0;
            HOPTCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
            while (caseChild >= 0) {
                int32_t next = HOPAstNextSibling(c->ast, caseChild);
                int32_t labelExprNode;
                int32_t aliasNode;
                if (next < 0) {
                    bodyNode = caseChild;
                    break;
                }
                if (HOPTCCasePatternParts(c, caseChild, &labelExprNode, &aliasNode) != 0) {
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
                if (sw->flags == 1) {
                    int32_t  labelEnumType = -1;
                    uint32_t labelVariantStart = 0;
                    uint32_t labelVariantEnd = 0;
                    int      variantRc = HOPTCDecodeVariantPatternExpr(
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
                            return HOPTCFailNode(c, labelExprNode, HOPDiag_TYPE_MISMATCH);
                        }
                        for (i = 0; i < (int)enumVariantCount; i++) {
                            if (HOPNameEqSlice(
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
                        if (HOPTCTypeExpr(c, labelExprNode, &labelType) != 0) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return -1;
                        }
                        if (!HOPTCCanAssign(c, subjectType, labelType)) {
                            c->localLen = savedLocalLen;
                            c->variantNarrowLen = savedVariantNarrowLen;
                            return HOPTCFailNode(c, labelExprNode, HOPDiag_TYPE_MISMATCH);
                        }
                        if (HOPTCIsBoolType(c, subjectType)
                            && c->ast->nodes[labelExprNode].kind == HOPAst_BOOL)
                        {
                            if (HOPNameEqLiteral(
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
                            return HOPTCFailNode(c, aliasNode, HOPDiag_TYPE_MISMATCH);
                        }
                        if (HOPTCLocalAdd(
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
                        HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
                        if (HOPTCVariantNarrowPush(
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
                        return HOPTCFailNode(c, aliasNode, HOPDiag_UNEXPECTED_TOKEN);
                    }
                    if (HOPTCTypeExpr(c, labelExprNode, &condType) != 0) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return -1;
                    }
                    if (!HOPTCIsBoolType(c, condType)) {
                        c->localLen = savedLocalLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        return HOPTCFailNode(c, labelExprNode, HOPDiag_EXPECTED_BOOL);
                    }
                }
                labelCount++;
                caseChild = next;
            }
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != HOPAst_BLOCK) {
                c->localLen = savedLocalLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                return HOPTCFailNode(c, child, HOPDiag_UNEXPECTED_TOKEN);
            }
            HOPTCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
            if (sw->flags == 1 && subjectEnumType >= 0 && subjectLocalIdx >= 0 && labelCount == 1
                && singleVariantLabel)
            {
                if (HOPTCVariantNarrowPush(
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
                if (HOPTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                    c->compilerDiagPathProven = savedDiagPath;
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
                c->compilerDiagPathProven = savedDiagPath;
            }
            if (!HOPTCStmtExitsSwitchWithoutContinuing(c, bodyNode)) {
                uint8_t* caseStates;
                if (HOPTCSnapshotLocalInitStates(c, &caseStates) != 0) {
                    c->localLen = savedLocalLen;
                    c->variantNarrowLen = savedVariantNarrowLen;
                    return -1;
                }
                if (!hasMergedSwitchState) {
                    mergedSwitchStates = caseStates;
                    hasMergedSwitchState = 1;
                } else {
                    HOPTCMergeLocalInitStateBuffers(mergedSwitchStates, caseStates, switchStateLen);
                }
            }
            c->localLen = savedLocalLen;
            c->variantNarrowLen = savedVariantNarrowLen;
        } else if (clause->kind == HOPAst_DEFAULT) {
            int32_t bodyNode = HOPAstFirstChild(c->ast, child);
            hasDefault = 1;
            HOPTCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != HOPAst_BLOCK) {
                return HOPTCFailNode(c, child, HOPDiag_UNEXPECTED_TOKEN);
            }
            {
                uint8_t savedDiagPath = c->compilerDiagPathProven;
                c->compilerDiagPathProven = 0;
                if (HOPTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                    c->compilerDiagPathProven = savedDiagPath;
                    return -1;
                }
                c->compilerDiagPathProven = savedDiagPath;
            }
            if (!HOPTCStmtExitsSwitchWithoutContinuing(c, bodyNode)) {
                uint8_t* caseStates;
                if (HOPTCSnapshotLocalInitStates(c, &caseStates) != 0) {
                    return -1;
                }
                if (!hasMergedSwitchState) {
                    mergedSwitchStates = caseStates;
                    hasMergedSwitchState = 1;
                } else {
                    HOPTCMergeLocalInitStateBuffers(mergedSwitchStates, caseStates, switchStateLen);
                }
            }
        } else {
            return HOPTCFailNode(c, child, HOPDiag_UNEXPECTED_TOKEN);
        }
        child = HOPAstNextSibling(c->ast, child);
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
                    return HOPTCFailSwitchMissingCases(
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
            } else if (HOPTCIsBoolType(c, subjectType)) {
                if (!boolCoveredTrue || !boolCoveredFalse) {
                    return HOPTCFailSwitchMissingCases(
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
                HOPTCMergeLocalInitStateBuffers(
                    mergedSwitchStates, preSwitchStates, switchStateLen);
            } else {
                mergedSwitchStates = preSwitchStates;
                hasMergedSwitchState = 1;
            }
        }
        if (hasMergedSwitchState) {
            HOPTCRestoreLocalInitStates(c, mergedSwitchStates, switchStateLen);
        } else {
            HOPTCRestoreLocalInitStates(c, preSwitchStates, switchStateLen);
        }
    }

    return 0;
}

int HOPTCExprIsBlankIdent(HOPTypeCheckCtx* c, int32_t exprNode) {
    const HOPAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    return n->kind == HOPAst_IDENT && HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_");
}

int HOPTCTypeMultiAssignStmt(HOPTypeCheckCtx* c, int32_t nodeId) {
    int32_t  lhsList = HOPAstFirstChild(c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? HOPAstNextSibling(c->ast, lhsList) : -1;
    uint32_t lhsCount;
    uint32_t rhsCount;
    int32_t  rhsTypes[256];
    uint32_t i;
    if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != HOPAst_EXPR_LIST
        || c->ast->nodes[rhsList].kind != HOPAst_EXPR_LIST)
    {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    lhsCount = HOPTCListCount(c->ast, lhsList);
    rhsCount = HOPTCListCount(c->ast, rhsList);
    if (lhsCount == 0 || lhsCount > (uint32_t)(sizeof(rhsTypes) / sizeof(rhsTypes[0]))) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
    if (rhsCount == lhsCount) {
        for (i = 0; i < rhsCount; i++) {
            int32_t rhsNode = HOPTCListItemAt(c->ast, rhsList, i);
            if (rhsNode < 0 || HOPTCTypeExpr(c, rhsNode, &rhsTypes[i]) != 0) {
                return -1;
            }
        }
    } else if (rhsCount == 1u) {
        int32_t          rhsNode = HOPTCListItemAt(c->ast, rhsList, 0);
        int32_t          rhsType;
        const HOPTCType* t;
        if (rhsNode < 0 || HOPTCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType < 0 || (uint32_t)rhsType >= c->typeLen) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        t = &c->types[rhsType];
        if (t->kind != HOPTCType_TUPLE || t->fieldCount != lhsCount) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        for (i = 0; i < lhsCount; i++) {
            rhsTypes[i] = c->funcParamTypes[t->fieldStart + i];
        }
    } else {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }
    for (i = 0; i < lhsCount; i++) {
        int32_t lhsNode = HOPTCListItemAt(c->ast, lhsList, i);
        int32_t rhsType = rhsTypes[i];
        if (lhsNode < 0) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        if (HOPTCExprIsBlankIdent(c, lhsNode)) {
            if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
                if (HOPTCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                    return -1;
                }
            }
            continue;
        }
        {
            int32_t lhsType;
            if (HOPTCTypeAssignTargetExpr(c, lhsNode, 1, &lhsType) != 0) {
                return -1;
            }
            if (!HOPTCExprIsAssignable(c, lhsNode)) {
                return HOPTCFailNode(c, lhsNode, HOPDiag_TYPE_MISMATCH);
            }
            if (HOPTCExprIsConstAssignTarget(c, lhsNode)) {
                return HOPTCFailAssignToConst(c, lhsNode);
            }
            if (!HOPTCCanAssign(c, lhsType, rhsType)) {
                return HOPTCFailTypeMismatchDetail(c, lhsNode, lhsNode, rhsType, lhsType);
            }
            HOPTCMarkDirectIdentLocalWrite(c, lhsNode, 1);
        }
    }
    return 0;
}

int HOPTCTypeShortAssignStmt(HOPTypeCheckCtx* c, int32_t nodeId) {
    int32_t  nameList = HOPAstFirstChild(c->ast, nodeId);
    int32_t  rhsList = nameList >= 0 ? HOPAstNextSibling(c->ast, nameList) : -1;
    uint32_t lhsCount;
    uint32_t rhsCount;
    int32_t  rhsTypes[256];
    int32_t  rhsNodes[256];
    int32_t  localIdxs[256];
    int32_t  declTypes[256];
    uint8_t  isBlank[256];
    uint32_t i;
    if (nameList < 0 || rhsList < 0 || c->ast->nodes[nameList].kind != HOPAst_NAME_LIST
        || c->ast->nodes[rhsList].kind != HOPAst_EXPR_LIST)
    {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
    }
    lhsCount = HOPTCListCount(c->ast, nameList);
    rhsCount = HOPTCListCount(c->ast, rhsList);
    if (lhsCount == 0 || lhsCount > (uint32_t)(sizeof(rhsTypes) / sizeof(rhsTypes[0]))) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }

    for (i = 0; i < lhsCount; i++) {
        int32_t           nameNode = HOPTCListItemAt(c->ast, nameList, i);
        const HOPAstNode* name;
        uint32_t          j;
        localIdxs[i] = -1;
        declTypes[i] = -1;
        isBlank[i] = 0;
        rhsNodes[i] = -1;
        if (nameNode < 0 || c->ast->nodes[nameNode].kind != HOPAst_IDENT) {
            return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
        }
        name = &c->ast->nodes[nameNode];
        if (HOPNameEqLiteral(c->src, name->dataStart, name->dataEnd, "_")) {
            isBlank[i] = 1;
            continue;
        }
        for (j = 0; j < i; j++) {
            int32_t           prevNameNode;
            const HOPAstNode* prevName;
            if (isBlank[j]) {
                continue;
            }
            prevNameNode = HOPTCListItemAt(c->ast, nameList, j);
            prevName = prevNameNode >= 0 ? &c->ast->nodes[prevNameNode] : NULL;
            if (prevName != NULL
                && HOPNameEqSlice(
                    c->src, prevName->dataStart, prevName->dataEnd, name->dataStart, name->dataEnd))
            {
                return HOPTCFailDuplicateDefinition(
                    c, name->dataStart, name->dataEnd, prevName->dataStart, prevName->dataEnd);
            }
        }
        localIdxs[i] = HOPTCLocalFind(c, name->dataStart, name->dataEnd);
    }

    if (rhsCount == lhsCount) {
        for (i = 0; i < rhsCount; i++) {
            int32_t rhsNode = HOPTCListItemAt(c->ast, rhsList, i);
            rhsNodes[i] = rhsNode;
            if (rhsNode < 0 || HOPTCTypeExpr(c, rhsNode, &rhsTypes[i]) != 0) {
                return -1;
            }
        }
    } else if (rhsCount == 1u) {
        int32_t          rhsNode = HOPTCListItemAt(c->ast, rhsList, 0);
        int32_t          rhsType;
        const HOPTCType* t;
        if (rhsNode < 0 || HOPTCTypeExpr(c, rhsNode, &rhsType) != 0) {
            return -1;
        }
        if (rhsType < 0 || (uint32_t)rhsType >= c->typeLen) {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        t = &c->types[rhsType];
        if (t->kind != HOPTCType_TUPLE || t->fieldCount != lhsCount
            || t->fieldStart + lhsCount > c->funcParamLen)
        {
            return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
        }
        for (i = 0; i < lhsCount; i++) {
            rhsTypes[i] = c->funcParamTypes[t->fieldStart + i];
            rhsNodes[i] = rhsNode;
        }
    } else {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
    }

    for (i = 0; i < lhsCount; i++) {
        int32_t nameNode = HOPTCListItemAt(c->ast, nameList, i);
        int32_t rhsType = rhsTypes[i];
        if (isBlank[i]) {
            if (rhsType == c->typeUntypedInt || rhsType == c->typeUntypedFloat) {
                if (HOPTCConcretizeInferredType(c, rhsType, &rhsType) != 0) {
                    return -1;
                }
            }
            continue;
        }
        if (localIdxs[i] >= 0) {
            int32_t localType = c->locals[localIdxs[i]].typeId;
            if ((c->locals[localIdxs[i]].flags & HOPTCLocalFlag_CONST) != 0) {
                return HOPTCFailAssignToConst(c, nameNode);
            }
            if (!HOPTCCanAssign(c, localType, rhsType)) {
                return HOPTCFailTypeMismatchDetail(c, nameNode, rhsNodes[i], rhsType, localType);
            }
            continue;
        }
        if (rhsType == c->typeNull) {
            return HOPTCFailNode(c, rhsNodes[i], HOPDiag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (rhsType == c->typeVoid) {
            return HOPTCFailNode(c, rhsNodes[i], HOPDiag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (HOPTCConcretizeInferredType(c, rhsType, &declTypes[i]) != 0) {
            return -1;
        }
        if (HOPTCTypeContainsVarSizeByValue(c, declTypes[i])) {
            return HOPTCFailNode(c, rhsNodes[i], HOPDiag_TYPE_MISMATCH);
        }
    }

    for (i = 0; i < lhsCount; i++) {
        int32_t           nameNode = HOPTCListItemAt(c->ast, nameList, i);
        const HOPAstNode* name;
        if (isBlank[i]) {
            continue;
        }
        name = &c->ast->nodes[nameNode];
        if (localIdxs[i] >= 0) {
            HOPTCMarkLocalWrite(c, localIdxs[i]);
            HOPTCMarkLocalInitialized(c, localIdxs[i]);
            continue;
        }
        if (HOPTCLocalAdd(c, name->dataStart, name->dataEnd, declTypes[i], 0, -1) != 0) {
            return -1;
        }
        HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
    }
    return 0;
}

int HOPTCTypeStmt(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const HOPAstNode* n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case HOPAst_BLOCK:       return HOPTCTypeBlock(c, nodeId, returnType, loopDepth, switchDepth);
        case HOPAst_VAR:
        case HOPAst_CONST:       return HOPTCTypeVarLike(c, nodeId);
        case HOPAst_CONST_BLOCK: {
            int32_t blockNode = HOPAstFirstChild(c->ast, nodeId);
            if (blockNode < 0 || c->ast->nodes[blockNode].kind != HOPAst_BLOCK) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNEXPECTED_TOKEN);
            }
            HOPTCMarkConstBlockLocalReads(c, blockNode);
            return 0;
        }
        case HOPAst_MULTI_ASSIGN: return HOPTCTypeMultiAssignStmt(c, nodeId);
        case HOPAst_SHORT_ASSIGN: return HOPTCTypeShortAssignStmt(c, nodeId);
        case HOPAst_EXPR_STMT:    {
            int32_t expr = HOPAstFirstChild(c->ast, nodeId);
            int32_t t;
            if (expr < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            return HOPTCTypeExpr(c, expr, &t);
        }
        case HOPAst_RETURN: {
            int32_t expr = HOPAstFirstChild(c->ast, nodeId);
            if (expr < 0) {
                if (returnType != c->typeVoid) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
                }
                return 0;
            }
            if (c->ast->nodes[expr].kind == HOPAst_EXPR_LIST) {
                const HOPTCType* rt;
                const HOPTCType* payload = NULL;
                uint32_t         wantCount;
                uint32_t         i;
                if (returnType < 0 || (uint32_t)returnType >= c->typeLen) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
                }
                rt = &c->types[returnType];
                if (rt->kind == HOPTCType_OPTIONAL && rt->baseType >= 0
                    && (uint32_t)rt->baseType < c->typeLen)
                {
                    payload = &c->types[rt->baseType];
                }
                if (rt->kind != HOPTCType_TUPLE
                    && !(payload != NULL && payload->kind == HOPTCType_TUPLE))
                {
                    return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
                }
                if (payload != NULL && payload->kind == HOPTCType_TUPLE) {
                    rt = payload;
                }
                wantCount = rt->fieldCount;
                if (HOPTCListCount(c->ast, expr) != wantCount) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_ARITY_MISMATCH);
                }
                for (i = 0; i < wantCount; i++) {
                    int32_t itemNode = HOPTCListItemAt(c->ast, expr, i);
                    int32_t itemType;
                    int32_t dstType;
                    if (itemNode < 0) {
                        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
                    }
                    dstType = c->funcParamTypes[rt->fieldStart + i];
                    if (HOPTCTypeExprExpected(c, itemNode, dstType, &itemType) != 0) {
                        return -1;
                    }
                    if (!HOPTCCanAssign(c, dstType, itemType)) {
                        return HOPTCFailTypeMismatchDetail(
                            c, itemNode, itemNode, itemType, dstType);
                    }
                }
                return 0;
            }
            {
                int32_t t;
                if (HOPTCTypeExprExpected(c, expr, returnType, &t) != 0) {
                    return -1;
                }
                if (!HOPTCCanAssign(c, returnType, t)) {
                    return HOPTCFailNode(c, expr, HOPDiag_TYPE_MISMATCH);
                }
                return 0;
            }
        }
        case HOPAst_IF: {
            int32_t         cond = HOPAstFirstChild(c->ast, nodeId);
            int32_t         thenNode;
            int32_t         elseNode;
            int32_t         condType;
            int             condIsOptional = 0;
            int             canSpecializeByConstCond = 0;
            int             condConstValue = 0;
            int             condIsConst = 0;
            int             diagCondConstValue = 0;
            int             diagCondIsConst = 0;
            uint8_t         savedDiagPath = c->compilerDiagPathProven;
            uint8_t         thenDiagPath = 0;
            uint8_t         elseDiagPath = 0;
            HOPTCNullNarrow narrow;
            int             thenIsSome = 0;
            int             hasNarrow;
            if (cond < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_BOOL);
            }
            thenNode = HOPAstNextSibling(c->ast, cond);
            if (thenNode < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNEXPECTED_TOKEN);
            }
            if (HOPTCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            condIsOptional =
                (condType >= 0 && (uint32_t)condType < c->typeLen
                 && c->types[condType].kind == HOPTCType_OPTIONAL);
            if (!HOPTCIsBoolType(c, condType) && !condIsOptional) {
                return HOPTCFailNode(c, cond, HOPDiag_EXPECTED_BOOL);
            }
            if (HOPTCConstBoolExpr(c, cond, &diagCondConstValue, &diagCondIsConst) != 0) {
                return -1;
            }
            if (diagCondIsConst) {
                thenDiagPath = diagCondConstValue ? savedDiagPath : 0;
                elseDiagPath = diagCondConstValue ? 0 : savedDiagPath;
            }
            elseNode = HOPAstNextSibling(c->ast, thenNode);
            canSpecializeByConstCond = c->activeConstEvalCtx != NULL;
            if (!canSpecializeByConstCond && c->currentFunctionIndex >= 0
                && (uint32_t)c->currentFunctionIndex < c->funcLen
                && (c->funcs[c->currentFunctionIndex].flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE)
                       != 0)
            {
                canSpecializeByConstCond = 1;
            }
            if (canSpecializeByConstCond) {
                condConstValue = diagCondConstValue;
                condIsConst = diagCondIsConst;
            }
            hasNarrow = HOPTCGetOptionalCondNarrow(c, cond, &thenIsSome, &narrow);
            {
                uint8_t* preInitStates;
                uint32_t initStateLen = c->localLen;
                if (HOPTCSnapshotLocalInitStates(c, &preInitStates) != 0) {
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
                            && HOPTCTypeStmt(c, branchNode, returnType, loopDepth, switchDepth)
                                   != 0)
                        {
                            c->locals[narrow.localIdx].typeId = origType;
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        c->locals[narrow.localIdx].typeId = origType;
                    } else if (
                        branchNode >= 0
                        && HOPTCTypeStmt(c, branchNode, returnType, loopDepth, switchDepth) != 0)
                    {
                        HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
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
                        if (HOPTCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                            c->locals[narrow.localIdx].typeId = origType;
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        c->locals[narrow.localIdx].typeId = origType;
                        thenContinues = !HOPTCStmtTerminates(c, thenNode);
                        if (HOPTCSnapshotLocalInitStates(c, &thenInitStates) != 0) {
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                        c->locals[narrow.localIdx].typeId = falseType;
                        c->compilerDiagPathProven = elseDiagPath;
                        if (elseNode >= 0
                            && HOPTCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                        {
                            c->locals[narrow.localIdx].typeId = origType;
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        c->locals[narrow.localIdx].typeId = origType;
                    } else {
                        c->compilerDiagPathProven = thenDiagPath;
                        if (HOPTCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        thenContinues = !HOPTCStmtTerminates(c, thenNode);
                        if (HOPTCSnapshotLocalInitStates(c, &thenInitStates) != 0) {
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                        HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                        c->compilerDiagPathProven = elseDiagPath;
                        if (elseNode >= 0
                            && HOPTCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                        {
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                    }
                    if (elseNode >= 0) {
                        elseContinues = !HOPTCStmtTerminates(c, elseNode);
                        if (HOPTCSnapshotLocalInitStates(c, &elseInitStates) != 0) {
                            HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                            c->compilerDiagPathProven = savedDiagPath;
                            return -1;
                        }
                    }
                    if (thenContinues && elseContinues) {
                        HOPTCMergeLocalInitStates(c, thenInitStates, elseInitStates, initStateLen);
                    } else if (thenContinues) {
                        HOPTCRestoreLocalInitStates(c, thenInitStates, initStateLen);
                    } else if (elseContinues) {
                        HOPTCRestoreLocalInitStates(c, elseInitStates, initStateLen);
                    } else {
                        HOPTCRestoreLocalInitStates(c, preInitStates, initStateLen);
                    }
                }
                c->compilerDiagPathProven = savedDiagPath;
                return 0;
            }
        }
        case HOPAst_FOR: return HOPTCTypeForStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case HOPAst_SWITCH:
            return HOPTCTypeSwitchStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case HOPAst_BREAK:
            if (loopDepth <= 0 && switchDepth <= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case HOPAst_CONTINUE:
            if (loopDepth <= 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case HOPAst_DEFER: {
            int32_t stmt = HOPAstFirstChild(c->ast, nodeId);
            if (stmt < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_UNEXPECTED_TOKEN);
            }
            return HOPTCTypeStmt(c, stmt, returnType, loopDepth, switchDepth);
        }
        case HOPAst_ASSERT: {
            int32_t cond = HOPAstFirstChild(c->ast, nodeId);
            int32_t condType;
            int32_t fmtNode;
            if (cond < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_BOOL);
            }
            if (HOPTCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            if (!HOPTCIsBoolType(c, condType)) {
                return HOPTCFailNode(c, cond, HOPDiag_EXPECTED_BOOL);
            }
            fmtNode = HOPAstNextSibling(c->ast, cond);
            if (fmtNode >= 0) {
                int32_t fmtType;
                int32_t argNode;
                int32_t wantStrType = HOPTCGetStrRefType(c, n->start, n->end);
                if (HOPTCTypeExpr(c, fmtNode, &fmtType) != 0) {
                    return -1;
                }
                if (wantStrType < 0) {
                    return HOPTCFailNode(c, fmtNode, HOPDiag_UNKNOWN_TYPE);
                }
                if (!HOPTCCanAssign(c, wantStrType, fmtType)) {
                    return HOPTCFailNode(c, fmtNode, HOPDiag_TYPE_MISMATCH);
                }
                argNode = HOPAstNextSibling(c->ast, fmtNode);
                while (argNode >= 0) {
                    int32_t argType;
                    if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                        return -1;
                    }
                    argNode = HOPAstNextSibling(c->ast, argNode);
                }
            }
            return 0;
        }
        case HOPAst_DEL: {
            int32_t expr = HOPAstFirstChild(c->ast, nodeId);
            int32_t allocArgNode = -1;
            int32_t allocType = HOPTCFindMemAllocatorType(c);
            int32_t ctxAllocType = -1;
            if (allocType < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_TYPE_MISMATCH);
            }
            if ((n->flags & HOPAstFlag_DEL_HAS_ALLOC) != 0) {
                int32_t scan = expr;
                while (scan >= 0) {
                    int32_t next = HOPAstNextSibling(c->ast, scan);
                    if (next < 0) {
                        allocArgNode = scan;
                        break;
                    }
                    scan = next;
                }
                if (allocArgNode < 0
                    || HOPTCValidateMemAllocatorArg(c, allocArgNode, allocType) != 0)
                {
                    return -1;
                }
            } else {
                if (HOPTCGetEffectiveContextFieldTypeByLiteral(c, "allocator", &ctxAllocType) != 0)
                {
                    return -1;
                }
                if (!HOPTCCanAssign(c, allocType, ctxAllocType)) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_CONTEXT_TYPE_MISMATCH);
                }
            }
            if (expr < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_EXPR);
            }
            while (expr >= 0 && expr != allocArgNode) {
                int32_t t;
                int32_t resolved;
                if (HOPTCTypeExpr(c, expr, &t) != 0) {
                    return -1;
                }
                resolved = HOPTCResolveAliasBaseType(c, t);
                if (resolved < 0 || (uint32_t)resolved >= c->typeLen
                    || c->types[resolved].kind != HOPTCType_PTR)
                {
                    return HOPTCFailNode(c, expr, HOPDiag_TYPE_MISMATCH);
                }
                expr = HOPAstNextSibling(c->ast, expr);
            }
            return 0;
        }
        default: return HOPTCFailNode(c, nodeId, HOPDiag_UNEXPECTED_TOKEN);
    }
}

int HOPTCTypeFunctionBody(HOPTypeCheckCtx* c, int32_t funcIndex) {
    const HOPTCFunction* fn = &c->funcs[funcIndex];
    int32_t              nodeId = fn->defNode;
    int32_t              child;
    uint32_t             paramIndex = 0;
    int32_t              bodyNode = -1;
    int32_t              savedContextType = c->currentContextType;
    int                  savedImplicitRoot = c->hasImplicitMainRootContext;
    int32_t              savedImplicitMainContextType = c->implicitMainContextType;
    int32_t              savedFunctionIndex = c->currentFunctionIndex;
    int                  savedFunctionIsCompareHook = c->currentFunctionIsCompareHook;
    int32_t              savedActiveTypeParamFnNode = c->activeTypeParamFnNode;
    uint32_t             savedActiveGenericArgStart = c->activeGenericArgStart;
    uint16_t             savedActiveGenericArgCount = c->activeGenericArgCount;
    int32_t              savedActiveGenericDeclNode = c->activeGenericDeclNode;
    int                  isEqualHook = 0;

    if (nodeId < 0) {
        return 0;
    }
    if ((fn->flags & HOPTCFunctionFlag_TEMPLATE) != 0
        && (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
    {
        return 0;
    }

    c->localLen = 0;
    c->currentFunctionIndex = funcIndex;
    c->currentFunctionIsCompareHook = HOPTCIsComparisonHookName(
        c, fn->nameStart, fn->nameEnd, &isEqualHook);
    c->activeTypeParamFnNode = nodeId;
    c->activeGenericArgStart = fn->templateArgStart;
    c->activeGenericArgCount = fn->templateArgCount;
    c->activeGenericDeclNode = fn->templateArgCount > 0 ? fn->declNode : -1;

    child = HOPAstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
            int32_t paramType;
            int     addedLocal = 0;
            if (paramIndex >= fn->paramCount) {
                return HOPTCFailNode(c, child, HOPDiag_ARITY_MISMATCH);
            }
            paramType = c->funcParamTypes[fn->paramTypeStart + paramIndex];
            if (!HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")
                && HOPTCLocalAdd(
                       c,
                       n->dataStart,
                       n->dataEnd,
                       paramType,
                       (c->funcParamFlags[fn->paramTypeStart + paramIndex]
                        & HOPTCFuncParamFlag_CONST)
                           != 0,
                       -1)
                       != 0)
            {
                return -1;
            }
            if (!HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")) {
                addedLocal = 1;
                HOPTCSetLocalUsageKind(c, (int32_t)c->localLen - 1, HOPTCLocalUseKind_PARAM);
                HOPTCMarkLocalInitialized(c, (int32_t)c->localLen - 1);
            }
            if (addedLocal && (n->flags & HOPAstFlag_PARAM_VARIADIC) != 0
                && (paramType == c->typeAnytype
                    || ((uint32_t)paramType < c->typeLen
                        && c->types[paramType].kind == HOPTCType_PACK)))
            {
                c->locals[c->localLen - 1u].flags |= HOPTCLocalFlag_ANYPACK;
            }
            paramIndex++;
        } else if (n->kind == HOPAst_BLOCK) {
            bodyNode = child;
        }
        child = HOPAstNextSibling(c->ast, child);
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

    c->currentContextType = HOPTCResolveImplicitMainContextType(c);
    c->hasImplicitMainRootContext = c->currentContextType < 0;
    c->implicitMainContextType = c->currentContextType;

    {
        int rc = HOPTCTypeBlock(c, bodyNode, fn->returnType, 0, 0);
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

int HOPTCCollectFunctionDecls(HOPTypeCheckCtx* c) {
    int32_t child = HOPAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (HOPTCCollectFunctionFromNode(c, child) != 0) {
            return -1;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

int HOPTCCollectTypeDecls(HOPTypeCheckCtx* c) {
    int32_t child = HOPAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (HOPTCCollectTypeDeclsFromNode(c, child) != 0) {
            return -1;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

int HOPTCBuildCheckedContext(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    const HOPTypeCheckOptions* _Nullable options,
    HOPDiag* _Nullable diag,
    HOPTypeCheckCtx* _Nullable outCtx) {
    HOPTypeCheckCtx c;
    uint32_t        capBase;
    uint32_t        i;

    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }

    if (arena == NULL || ast == NULL || ast->nodes == NULL || ast->root < 0) {
        HOPTCSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    capBase = ast->len < 32 ? 32u : ast->len;

    c.arena = arena;
    c.ast = ast;
    c.src = src;
    c.diag = diag;
    c.diagSink.ctx = options != NULL ? options->ctx : NULL;
    c.diagSink.onDiag = options != NULL ? options->onDiag : NULL;

    c.types = (HOPTCType*)HOPArenaAlloc(
        arena, sizeof(HOPTCType) * capBase * 4u, (uint32_t)_Alignof(HOPTCType));
    c.fields = (HOPTCField*)HOPArenaAlloc(
        arena, sizeof(HOPTCField) * capBase * 4u, (uint32_t)_Alignof(HOPTCField));
    c.namedTypes = (HOPTCNamedType*)HOPArenaAlloc(
        arena, sizeof(HOPTCNamedType) * capBase, (uint32_t)_Alignof(HOPTCNamedType));
    c.funcs = (HOPTCFunction*)HOPArenaAlloc(
        arena, sizeof(HOPTCFunction) * capBase, (uint32_t)_Alignof(HOPTCFunction));
    c.funcUsed = (uint8_t*)HOPArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.funcParamTypes = (int32_t*)HOPArenaAlloc(
        arena, sizeof(int32_t) * capBase * 8u, (uint32_t)_Alignof(int32_t));
    c.funcParamNameStarts = (uint32_t*)HOPArenaAlloc(
        arena, sizeof(uint32_t) * capBase * 8u, (uint32_t)_Alignof(uint32_t));
    c.funcParamNameEnds = (uint32_t*)HOPArenaAlloc(
        arena, sizeof(uint32_t) * capBase * 8u, (uint32_t)_Alignof(uint32_t));
    c.funcParamFlags = (uint8_t*)HOPArenaAlloc(
        arena, sizeof(uint8_t) * capBase * 8u, (uint32_t)_Alignof(uint8_t));
    c.genericArgTypes = (int32_t*)HOPArenaAlloc(
        arena, sizeof(int32_t) * capBase * 16u, (uint32_t)_Alignof(int32_t));
    c.scratchParamTypes = (int32_t*)HOPArenaAlloc(
        arena, sizeof(int32_t) * capBase, (uint32_t)_Alignof(int32_t));
    c.scratchParamFlags = (uint8_t*)HOPArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.locals = (HOPTCLocal*)HOPArenaAlloc(
        arena, sizeof(HOPTCLocal) * capBase * 4u, (uint32_t)_Alignof(HOPTCLocal));
    c.localUses = (HOPTCLocalUse*)HOPArenaAlloc(
        arena, sizeof(HOPTCLocalUse) * capBase * 8u, (uint32_t)_Alignof(HOPTCLocalUse));
    c.variantNarrows = (HOPTCVariantNarrow*)HOPArenaAlloc(
        arena, sizeof(HOPTCVariantNarrow) * capBase * 4u, (uint32_t)_Alignof(HOPTCVariantNarrow));
    c.warningDedup = (HOPTCWarningDedup*)HOPArenaAlloc(
        arena, sizeof(HOPTCWarningDedup) * capBase, (uint32_t)_Alignof(HOPTCWarningDedup));
    c.constDiagUses = (HOPTCConstDiagUse*)HOPArenaAlloc(
        arena, sizeof(HOPTCConstDiagUse) * capBase, (uint32_t)_Alignof(HOPTCConstDiagUse));
    c.constDiagFnInvoked = (uint8_t*)HOPArenaAlloc(
        arena, sizeof(uint8_t) * capBase, (uint32_t)_Alignof(uint8_t));
    c.callTargets = (HOPTCCallTarget*)HOPArenaAlloc(
        arena, sizeof(HOPTCCallTarget) * capBase * 8u, (uint32_t)_Alignof(HOPTCCallTarget));
    c.constEvalValues = (HOPCTFEValue*)HOPArenaAlloc(
        arena, sizeof(HOPCTFEValue) * ast->len, (uint32_t)_Alignof(HOPCTFEValue));
    c.constEvalState = (uint8_t*)HOPArenaAlloc(
        arena, sizeof(uint8_t) * ast->len, (uint32_t)_Alignof(uint8_t));
    c.topVarLikeTypes = (int32_t*)HOPArenaAlloc(
        arena, sizeof(int32_t) * ast->len, (uint32_t)_Alignof(int32_t));
    c.topVarLikeTypeState = (uint8_t*)HOPArenaAlloc(
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
        HOPTCSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
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
    memset(c.funcParamFlags, 0, sizeof(uint8_t) * c.funcParamCap);
    memset(c.scratchParamFlags, 0, sizeof(uint8_t) * c.scratchParamCap);
    memset(c.funcUsed, 0, sizeof(uint8_t) * c.funcUsedCap);
    if (ast->len > 0) {
        memset(c.constEvalState, 0, sizeof(uint8_t) * ast->len);
        memset(c.topVarLikeTypeState, 0, sizeof(uint8_t) * ast->len);
        memset(c.constDiagFnInvoked, 0, sizeof(uint8_t) * c.constDiagFnInvokedCap);
        for (i = 0; i < ast->len; i++) {
            c.topVarLikeTypes[i] = -1;
            c.constEvalValues[i].kind = HOPCTFEValue_INVALID;
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

    if (HOPTCEnsureInitialized(&c) != 0) {
        return -1;
    }
    c.typeUsize = HOPTCFindBuiltinByKind(&c, HOPBuiltin_USIZE);
    if (c.typeUsize < 0) {
        return HOPTCFailSpan(&c, HOPDiag_UNKNOWN_TYPE, 0, 0);
    }

    if (HOPTCCollectTypeDecls(&c) != 0) {
        return -1;
    }
    {
        int32_t namedStrType = HOPTCFindNamedTypeByLiteral(&c, "builtin__str");
        if (namedStrType < 0) {
            namedStrType = HOPTCFindBuiltinNamedTypeBySuffix(&c, "__str");
        }
        if (namedStrType < 0) {
            namedStrType = HOPTCFindNamedTypeByLiteral(&c, "str");
        }
        if (namedStrType >= 0) {
            c.typeStr = namedStrType;
        }
    }
    {
        int32_t namedRuneType = HOPTCFindNamedTypeByLiteral(&c, "builtin__rune");
        if (namedRuneType < 0) {
            namedRuneType = HOPTCFindBuiltinNamedTypeBySuffix(&c, "__rune");
        }
        if (namedRuneType < 0) {
            namedRuneType = HOPTCFindNamedTypeByLiteral(&c, "rune");
        }
        if (namedRuneType >= 0) {
            c.typeRune = namedRuneType;
        }
    }
    c.typeMemAllocator = HOPTCFindNamedTypeByLiteral(&c, "builtin__MemAllocator");
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = HOPTCFindBuiltinNamedTypeBySuffix(&c, "__MemAllocator");
    }
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = HOPTCFindNamedTypeByLiteral(&c, "MemAllocator");
    }
    if (c.typeMemAllocator < 0) {
        c.typeMemAllocator = HOPTCFindNamedTypeBySuffix(&c, "__MemAllocator");
    }
    if (HOPTCResolveAllTypeAliases(&c) != 0) {
        return -1;
    }
    if (HOPTCCollectFunctionDecls(&c) != 0) {
        return -1;
    }
    if (HOPTCFinalizeFunctionTypes(&c) != 0) {
        return -1;
    }
    if (HOPTCResolveAllNamedTypeFields(&c) != 0) {
        return -1;
    }
    if (HOPTCCheckEmbeddedCycles(&c) != 0) {
        return -1;
    }
    if (HOPTCPropagateVarSizeNamedTypes(&c) != 0) {
        return -1;
    }
    if (HOPTCCheckTopLevelConstInitializers(&c) != 0) {
        return -1;
    }
    if (HOPTCTypeTopLevelConsts(&c) != 0) {
        return -1;
    }
    if (HOPTCTypeTopLevelVars(&c) != 0) {
        return -1;
    }

    for (i = 0; i < c.funcLen; i++) {
        if (HOPTCTypeFunctionBody(&c, (int32_t)i) != 0) {
            return -1;
        }
    }
    if (HOPTCValidateTopLevelConstEvaluable(&c) != 0) {
        return -1;
    }
    if (HOPTCValidateConstDiagUses(&c) != 0) {
        return -1;
    }
    if (HOPTCMarkTemplateRootFunctionUses(&c) != 0) {
        return -1;
    }
    if (HOPTCEmitUnusedSymbolWarnings(&c) != 0) {
        return -1;
    }

    if (outCtx != NULL) {
        *outCtx = c;
    }

    return 0;
}

HOP_API_END
