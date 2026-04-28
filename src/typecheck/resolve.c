#include "internal.h"
#include "../mir_exec.h"
#include "../mir_lower_stmt.h"

H2_API_BEGIN

static int H2TCFailGenericTypeArgArity(
    H2TypeCheckCtx* c,
    int32_t         nodeId,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    uint16_t        expected,
    uint16_t        got) {
    char        detailBuf[256];
    char        expectedBuf[16];
    char        gotBuf[16];
    H2TCTextBuf detailText;
    H2TCTextBuf expectedText;
    H2TCTextBuf gotText;
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufInit(&expectedText, expectedBuf, (uint32_t)sizeof(expectedBuf));
    H2TCTextBufInit(&gotText, gotBuf, (uint32_t)sizeof(gotBuf));
    H2TCTextBufAppendU32(&expectedText, expected);
    H2TCTextBufAppendU32(&gotText, got);
    H2TCTextBufAppendCStr(&detailText, "generic type '");
    H2TCTextBufAppendSlice(&detailText, c->src, nameStart, nameEnd);
    H2TCTextBufAppendCStr(&detailText, "' expects ");
    H2TCTextBufAppendCStr(&detailText, expectedBuf);
    H2TCTextBufAppendCStr(&detailText, " type arguments, got ");
    H2TCTextBufAppendCStr(&detailText, gotBuf);
    return H2TCFailDiagText(c, nodeId, H2Diag_GENERIC_TYPE_ARITY_MISMATCH, detailBuf);
}

static int H2TCFailGenericTypeArgsRequired(
    H2TypeCheckCtx* c, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd) {
    char        detailBuf[256];
    H2TCTextBuf detailText;
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "generic type '");
    H2TCTextBufAppendSlice(&detailText, c->src, nameStart, nameEnd);
    H2TCTextBufAppendCStr(&detailText, "' requires explicit type arguments");
    return H2TCFailDiagText(c, nodeId, H2Diag_GENERIC_TYPE_ARGS_REQUIRED, detailBuf);
}

static void H2TCResolveMirSetReasonCb(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* reason) {
    H2TCConstSetReason((H2TCConstEvalCtx*)ctx, start, end, reason);
}

static int H2TCTryMirConstBlock(
    H2TCConstEvalCtx* evalCtx,
    int32_t           blockNode,
    H2CTFEValue*      outValue,
    int*              outDidReturn,
    int*              outIsConst,
    int*              outSupported) {
    H2TypeCheckCtx*      c;
    H2MirProgram         program = { 0 };
    H2MirExecEnv         env = { 0 };
    H2MirLowerOptions    options = { 0 };
    H2TCMirConstLowerCtx lowerCtx;
    uint32_t             mirFnIndex = UINT32_MAX;
    int                  supported = 0;
    int                  mirIsConst = 0;
    int                  rewriteRc;
    if (outDidReturn != NULL) {
        *outDidReturn = 0;
    }
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (evalCtx == NULL || outValue == NULL || outDidReturn == NULL || outIsConst == NULL
        || outSupported == NULL)
    {
        return -1;
    }
    c = evalCtx->tc;
    if (c == NULL) {
        return -1;
    }
    if (H2TCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    options.lowerConstExpr = H2TCMirConstLowerConstExpr;
    options.lowerConstExprCtx = evalCtx;
    if (H2MirLowerAppendSimpleFunctionWithOptions(
            &lowerCtx.builder,
            c->arena,
            c->ast,
            c->src,
            -1,
            blockNode,
            &options,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        return -1;
    }
    if (!supported || mirFnIndex == UINT32_MAX) {
        H2TCMirConstAdoptLowerDiagReason(evalCtx, c->diag);
        return 0;
    }
    rewriteRc = H2TCMirConstRewriteDirectCalls(&lowerCtx, mirFnIndex, blockNode);
    if (rewriteRc < 0) {
        return -1;
    }
    if (rewriteRc == 0) {
        return 0;
    }
    H2MirProgramBuilderFinish(&lowerCtx.builder, &program);
    env.src = c->src;
    env.resolveIdent = H2TCResolveConstIdent;
    env.resolveCallPre = H2TCResolveConstCallMirPre;
    env.resolveCall = H2TCResolveConstCallMir;
    env.resolveCtx = evalCtx;
    env.zeroInitLocal = H2TCMirConstZeroInitLocal;
    env.zeroInitCtx = evalCtx;
    env.coerceValueForType = H2TCMirConstCoerceValueForType;
    env.coerceValueCtx = evalCtx;
    env.indexValue = H2TCMirConstIndexValue;
    env.indexValueCtx = evalCtx;
    env.sequenceLen = H2TCMirConstSequenceLen;
    env.sequenceLenCtx = evalCtx;
    env.iterInit = H2TCMirConstIterInit;
    env.iterInitCtx = evalCtx;
    env.iterNext = H2TCMirConstIterNext;
    env.iterNextCtx = evalCtx;
    env.aggGetField = H2TCMirConstAggGetField;
    env.aggGetFieldCtx = evalCtx;
    env.aggAddrField = H2TCMirConstAggAddrField;
    env.aggAddrFieldCtx = evalCtx;
    env.makeTuple = H2TCMirConstMakeTuple;
    env.makeTupleCtx = evalCtx;
    env.bindFrame = H2TCMirConstBindFrame;
    env.unbindFrame = H2TCMirConstUnbindFrame;
    env.frameCtx = evalCtx;
    env.setReason = H2TCResolveMirSetReasonCb;
    env.setReasonCtx = evalCtx;
    env.backwardJumpLimit = H2TC_CONST_FOR_MAX_ITERS;
    env.diag = c->diag;
    if (!H2MirProgramNeedsDynamicResolution(&program)) {
        H2MirExecEnvDisableDynamicResolution(&env);
    }
    if (H2MirEvalFunction(c->arena, &program, mirFnIndex, NULL, 0, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    *outIsConst = mirIsConst;
    if (mirIsConst) {
        *outDidReturn = outValue->kind != H2CTFEValue_INVALID;
    }
    return 0;
}

static void H2TCInitConstEvalCtxFromParent(
    H2TypeCheckCtx* c, const H2TCConstEvalCtx* _Nullable parent, H2TCConstEvalCtx* outCtx) {
    if (outCtx == NULL) {
        return;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    outCtx->tc = c;
    outCtx->rootCallOwnerFnIndex = -1;
    outCtx->callFnIndex = -1;
    if (parent == NULL) {
        return;
    }
    outCtx->execCtx = parent->execCtx;
    outCtx->mirProgram = parent->mirProgram;
    outCtx->mirFunction = parent->mirFunction;
    outCtx->mirLocals = parent->mirLocals;
    outCtx->mirLocalCount = parent->mirLocalCount;
    outCtx->callArgs = parent->callArgs;
    outCtx->callArgCount = parent->callArgCount;
    outCtx->callBinding = parent->callBinding;
    outCtx->callFnIndex = parent->callFnIndex;
    outCtx->callPackParamNameStart = parent->callPackParamNameStart;
    outCtx->callPackParamNameEnd = parent->callPackParamNameEnd;
    memcpy(outCtx->callFrameArgs, parent->callFrameArgs, sizeof(outCtx->callFrameArgs));
    memcpy(
        outCtx->callFrameArgCounts, parent->callFrameArgCounts, sizeof(outCtx->callFrameArgCounts));
    memcpy(outCtx->callFrameBindings, parent->callFrameBindings, sizeof(outCtx->callFrameBindings));
    memcpy(
        outCtx->callFrameFnIndices, parent->callFrameFnIndices, sizeof(outCtx->callFrameFnIndices));
    memcpy(
        outCtx->callFramePackParamNameStarts,
        parent->callFramePackParamNameStarts,
        sizeof(outCtx->callFramePackParamNameStarts));
    memcpy(
        outCtx->callFramePackParamNameEnds,
        parent->callFramePackParamNameEnds,
        sizeof(outCtx->callFramePackParamNameEnds));
    outCtx->callFrameDepth = parent->callFrameDepth;
    outCtx->fnDepth = parent->fnDepth;
    memcpy(outCtx->fnStack, parent->fnStack, sizeof(outCtx->fnStack));
}

static int H2TCFindForwardedConstParamCallSpan(
    H2TypeCheckCtx* c, int32_t argExprNode, uint32_t* outStart, uint32_t* outEnd) {
    const H2AstNode*    n;
    int32_t             localIdx;
    const H2TCLocal*    local;
    const H2TCFunction* fn;
    uint32_t            p;
    if (outStart != NULL) {
        *outStart = 0;
    }
    if (outEnd != NULL) {
        *outEnd = 0;
    }
    if (c == NULL || c->ast == NULL || outStart == NULL || outEnd == NULL || argExprNode < 0
        || (uint32_t)argExprNode >= c->ast->len || c->currentFunctionIndex < 0
        || (uint32_t)c->currentFunctionIndex >= c->funcLen)
    {
        return 0;
    }
    n = &c->ast->nodes[argExprNode];
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = H2AstFirstChild(c->ast, argExprNode);
        if (inner < 0 || (uint32_t)inner >= c->ast->len) {
            return 0;
        }
        n = &c->ast->nodes[inner];
    }
    if (n->kind != H2Ast_IDENT) {
        return 0;
    }
    localIdx = H2TCLocalFind(c, n->dataStart, n->dataEnd);
    if (localIdx < 0) {
        return 0;
    }
    local = &c->locals[localIdx];
    if ((local->flags & H2TCLocalFlag_CONST) == 0) {
        return 0;
    }
    fn = &c->funcs[(uint32_t)c->currentFunctionIndex];
    for (p = 0; p < fn->paramCount; p++) {
        uint32_t paramSlot = fn->paramTypeStart + p;
        if (paramSlot >= c->funcParamLen || c->funcParamCallArgEnds[paramSlot] <= 0) {
            continue;
        }
        if (H2NameEqSlice(
                c->src,
                c->funcParamNameStarts[paramSlot],
                c->funcParamNameEnds[paramSlot],
                local->nameStart,
                local->nameEnd))
        {
            if (c->funcParamCallArgEnds[paramSlot] > c->funcParamCallArgStarts[paramSlot]) {
                *outStart = c->funcParamCallArgStarts[paramSlot];
                *outEnd = c->funcParamCallArgEnds[paramSlot];
                return 1;
            }
            return 0;
        }
    }
    return 0;
}

int H2TCResolveAnonAggregateTypeNode(
    H2TypeCheckCtx* c, int32_t nodeId, int isUnion, int32_t* outType) {
    int32_t          fieldNode = H2AstFirstChild(c->ast, nodeId);
    H2TCAnonFieldSig fieldSigs[H2TC_MAX_ANON_FIELDS];
    uint32_t         fieldCount = 0;

    while (fieldNode >= 0) {
        const H2AstNode* field = &c->ast->nodes[fieldNode];
        int32_t          typeNode;
        int32_t          typeId;
        uint32_t         i;
        if (field->kind != H2Ast_FIELD) {
            return H2TCFailNode(c, fieldNode, H2Diag_EXPECTED_TYPE);
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
                return H2TCFailDuplicateDefinition(
                    c,
                    field->dataStart,
                    field->dataEnd,
                    fieldSigs[i].nameStart,
                    fieldSigs[i].nameEnd);
            }
        }
        typeNode = H2AstFirstChild(c->ast, fieldNode);
        if (typeNode < 0) {
            return H2TCFailNode(c, fieldNode, H2Diag_EXPECTED_TYPE);
        }
        if (c->ast->nodes[typeNode].kind == H2Ast_TYPE_VARRAY) {
            return H2TCFailNode(c, typeNode, H2Diag_TYPE_MISMATCH);
        }
        if (H2TCResolveTypeNode(c, typeNode, &typeId) != 0) {
            return -1;
        }
        fieldSigs[fieldCount].nameStart = field->dataStart;
        fieldSigs[fieldCount].nameEnd = field->dataEnd;
        fieldSigs[fieldCount].typeId = typeId;
        fieldCount++;
        fieldNode = H2AstNextSibling(c->ast, fieldNode);
    }

    {
        int32_t typeId = H2TCInternAnonAggregateType(
            c,
            isUnion,
            fieldSigs,
            fieldCount,
            nodeId,
            c->ast->nodes[nodeId].start,
            c->ast->nodes[nodeId].end);
        if (typeId < 0) {
            return -1;
        }
        *outType = typeId;
        return 0;
    }
}

int H2TCResolveAliasTypeId(H2TypeCheckCtx* c, int32_t typeId) {
    H2TCType*        t;
    int32_t          targetNode;
    int32_t          targetType = -1;
    const H2AstNode* decl;

    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return H2TCFailSpan(c, H2Diag_UNKNOWN_TYPE, 0, 0);
    }
    t = &c->types[typeId];
    if (t->kind != H2TCType_ALIAS) {
        return 0;
    }
    if ((t->flags & H2TCTypeFlag_ALIAS_RESOLVED) != 0) {
        return 0;
    }
    if ((t->flags & H2TCTypeFlag_ALIAS_RESOLVING) != 0) {
        return H2TCFailNode(c, t->declNode, H2Diag_TYPE_MISMATCH);
    }
    if (t->declNode < 0 || (uint32_t)t->declNode >= c->ast->len) {
        return H2TCFailSpan(c, H2Diag_UNKNOWN_TYPE, 0, 0);
    }

    decl = &c->ast->nodes[t->declNode];
    targetNode = H2AstFirstChild(c->ast, t->declNode);
    if (targetNode < 0) {
        return H2TCFailNode(c, t->declNode, H2Diag_EXPECTED_TYPE);
    }

    t->flags |= H2TCTypeFlag_ALIAS_RESOLVING;
    if (H2TCResolveTypeNode(c, targetNode, &targetType) != 0) {
        t->flags &= (uint16_t)~H2TCTypeFlag_ALIAS_RESOLVING;
        return -1;
    }
    if (targetType < 0 || (uint32_t)targetType >= c->typeLen || targetType == typeId) {
        t->flags &= (uint16_t)~H2TCTypeFlag_ALIAS_RESOLVING;
        return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, decl->start, decl->end);
    }
    if (c->types[targetType].kind == H2TCType_ALIAS && H2TCResolveAliasTypeId(c, targetType) != 0) {
        t->flags &= (uint16_t)~H2TCTypeFlag_ALIAS_RESOLVING;
        return -1;
    }

    t->baseType = targetType;
    t->flags &= (uint16_t)~H2TCTypeFlag_ALIAS_RESOLVING;
    t->flags |= H2TCTypeFlag_ALIAS_RESOLVED;
    return 0;
}

int32_t H2TCResolveAliasBaseType(H2TypeCheckCtx* c, int32_t typeId) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == H2TCType_ALIAS
           && depth++ <= c->typeLen)
    {
        if (H2TCResolveAliasTypeId(c, typeId) != 0) {
            return -1;
        }
        typeId = c->types[typeId].baseType;
    }
    return typeId;
}

uint16_t H2TCDeclTypeParamCount(H2TypeCheckCtx* c, int32_t declNode) {
    int32_t  child;
    uint16_t count = 0;
    if (c == NULL || declNode < 0 || (uint32_t)declNode >= c->ast->len) {
        return 0;
    }
    child = H2AstFirstChild(c->ast, declNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
            count++;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return count;
}

int32_t H2TCDeclTypeParamIndex(
    H2TypeCheckCtx* c, int32_t declNode, uint32_t nameStart, uint32_t nameEnd) {
    int32_t  child;
    uint16_t index = 0;
    if (c == NULL || declNode < 0 || (uint32_t)declNode >= c->ast->len) {
        return -1;
    }
    child = H2AstFirstChild(c->ast, declNode);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_TYPE_PARAM) {
            if (H2NameEqSlice(c->src, n->dataStart, n->dataEnd, nameStart, nameEnd)) {
                return (int32_t)index;
            }
            index++;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return -1;
}

int H2TCAppendDeclTypeParamPlaceholders(
    H2TypeCheckCtx* c, int32_t declNode, uint32_t* outStart, uint16_t* outCount) {
    int32_t  child;
    uint16_t count = H2TCDeclTypeParamCount(c, declNode);
    if (outStart != NULL) {
        *outStart = c != NULL ? c->genericArgLen : 0;
    }
    if (outCount != NULL) {
        *outCount = count;
    }
    if (c == NULL || count == 0) {
        return 0;
    }
    if (c->genericArgLen + count > c->genericArgCap || c->genericArgTypes == NULL) {
        return H2TCFailNode(c, declNode, H2Diag_ARENA_OOM);
    }
    child = H2AstFirstChild(c->ast, declNode);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_TYPE_PARAM) {
            H2TCType t;
            int32_t  typeId;
            memset(&t, 0, sizeof(t));
            t.kind = H2TCType_TYPE_PARAM;
            t.baseType = -1;
            t.declNode = declNode;
            t.funcIndex = -1;
            t.nameStart = n->dataStart;
            t.nameEnd = n->dataEnd;
            typeId = H2TCAddType(c, &t, n->start, n->end);
            if (typeId < 0) {
                return -1;
            }
            c->genericArgTypes[c->genericArgLen++] = typeId;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

static int H2TCResolveActiveDeclTypeParamType(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    int32_t idx;
    if (c == NULL || outType == NULL || c->activeGenericDeclNode < 0) {
        return 0;
    }
    idx = H2TCDeclTypeParamIndex(c, c->activeGenericDeclNode, nameStart, nameEnd);
    if (idx < 0 || (uint32_t)idx >= c->activeGenericArgCount) {
        return 0;
    }
    *outType = c->genericArgTypes[c->activeGenericArgStart + (uint32_t)idx];
    return 1;
}

int H2TCFnNodeHasTypeParamName(
    H2TypeCheckCtx* c, int32_t fnNode, uint32_t nameStart, uint32_t nameEnd) {
    int32_t child;
    if (c == NULL || fnNode < 0 || (uint32_t)fnNode >= c->ast->len) {
        return 0;
    }
    if (c->ast->nodes[fnNode].kind != H2Ast_FN) {
        return 0;
    }
    child = H2AstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
            int32_t          typeNode = H2AstFirstChild(c->ast, child);
            const H2AstNode* t = typeNode >= 0 ? &c->ast->nodes[typeNode] : NULL;
            if (t != NULL && t->kind == H2Ast_TYPE_NAME
                && H2NameEqLiteral(c->src, t->dataStart, t->dataEnd, "type")
                && H2NameEqSlice(c->src, n->dataStart, n->dataEnd, nameStart, nameEnd))
            {
                return 1;
            }
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return 0;
}

int H2TCResolveActiveTypeParamType(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    if (c == NULL || outType == NULL) {
        return 0;
    }
    if (H2TCResolveActiveDeclTypeParamType(c, nameStart, nameEnd, outType)) {
        return 1;
    }
    if (c->currentFunctionIndex >= 0 && (uint32_t)c->currentFunctionIndex < c->funcLen) {
        const H2TCFunction* fn = &c->funcs[c->currentFunctionIndex];
        uint32_t            p;
        for (p = 0; p < fn->paramCount; p++) {
            uint32_t paramIndex = fn->paramTypeStart + p;
            if (c->funcParamTypes[paramIndex] == c->typeType
                && H2NameEqSlice(
                    c->src,
                    c->funcParamNameStarts[paramIndex],
                    c->funcParamNameEnds[paramIndex],
                    nameStart,
                    nameEnd))
            {
                *outType = c->typeType;
                return 1;
            }
        }
    }
    if (c->activeTypeParamFnNode >= 0
        && H2TCFnNodeHasTypeParamName(c, c->activeTypeParamFnNode, nameStart, nameEnd))
    {
        *outType = c->typeType;
        return 1;
    }
    return 0;
}

int H2TCResolveTypeNode(H2TypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const H2AstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailSpan(c, H2Diag_EXPECTED_TYPE, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case H2Ast_TYPE_NAME: {
            int32_t  firstArgNode = H2AstFirstChild(c->ast, nodeId);
            uint16_t argCount = 0;
            if (H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "anytype")) {
                if (!c->allowAnytypeParamType) {
                    return H2TCFailSpan(
                        c, H2Diag_ANYTYPE_INVALID_POSITION, n->dataStart, n->dataEnd);
                }
                if (firstArgNode >= 0) {
                    return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, n->start, n->end);
                }
                *outType = c->typeAnytype;
                return 0;
            }
            int32_t typeId = H2TCFindBuiltinType(c, n->dataStart, n->dataEnd);
            if (typeId >= 0) {
                if ((typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat)
                    && !c->allowConstNumericTypeName)
                {
                    return H2TCFailSpan(
                        c, H2Diag_CONST_NUMERIC_INVALID_POSITION, n->dataStart, n->dataEnd);
                }
                if (firstArgNode >= 0) {
                    return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, n->start, n->end);
                }
                *outType = typeId;
                return 0;
            }
            {
                int32_t resolvedType = H2TCResolveTypeNamePath(
                    c, n->dataStart, n->dataEnd, c->currentTypeOwnerTypeId);
                if (resolvedType >= 0) {
                    int32_t  namedIndex = -1;
                    uint32_t i;
                    for (i = 0; i < c->namedTypeLen; i++) {
                        if (c->namedTypes[i].typeId == resolvedType) {
                            namedIndex = (int32_t)i;
                            break;
                        }
                    }
                    while (firstArgNode >= 0) {
                        argCount++;
                        firstArgNode = H2AstNextSibling(c->ast, firstArgNode);
                    }
                    if (namedIndex >= 0) {
                        const H2TCNamedType* nt = &c->namedTypes[(uint32_t)namedIndex];
                        if (nt->templateRootNamedIndex >= 0) {
                            namedIndex = nt->templateRootNamedIndex;
                            resolvedType = c->namedTypes[(uint32_t)namedIndex].typeId;
                            nt = &c->namedTypes[(uint32_t)namedIndex];
                        }
                        if (nt->templateArgCount > 0 || argCount > 0) {
                            int32_t  args[64];
                            int32_t  argNode = H2AstFirstChild(c->ast, nodeId);
                            uint16_t ai = 0;
                            if (argCount == 0 && nt->templateArgCount > 0) {
                                return H2TCFailGenericTypeArgsRequired(
                                    c, nodeId, n->dataStart, n->dataEnd);
                            }
                            if (nt->templateArgCount != argCount
                                || argCount > (uint16_t)(sizeof(args) / sizeof(args[0])))
                            {
                                return H2TCFailGenericTypeArgArity(
                                    c,
                                    nodeId,
                                    n->dataStart,
                                    n->dataEnd,
                                    nt->templateArgCount,
                                    argCount);
                            }
                            while (argNode >= 0) {
                                if (H2TCResolveTypeNode(c, argNode, &args[ai]) != 0) {
                                    return -1;
                                }
                                ai++;
                                argNode = H2AstNextSibling(c->ast, argNode);
                            }
                            resolvedType = H2TCInstantiateNamedType(
                                c, resolvedType, args, argCount);
                            if (resolvedType < 0) {
                                return -1;
                            }
                        }
                    } else if (argCount > 0) {
                        return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, n->start, n->end);
                    }
                    *outType = resolvedType;
                    if (*outType >= 0 && (uint32_t)*outType < c->typeLen
                        && c->types[*outType].kind == H2TCType_ALIAS
                        && H2TCResolveAliasTypeId(c, *outType) != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
            }
            {
                int32_t activeTypeParamType = -1;
                if (H2TCResolveActiveTypeParamType(
                        c, n->dataStart, n->dataEnd, &activeTypeParamType))
                {
                    if (H2AstFirstChild(c->ast, nodeId) >= 0) {
                        return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, n->start, n->end);
                    }
                    *outType = activeTypeParamType;
                    return 0;
                }
            }
            {
                int32_t resolvedType = H2TCFindBuiltinQualifiedNamedType(
                    c, n->dataStart, n->dataEnd);
                if (resolvedType >= 0) {
                    if (H2AstFirstChild(c->ast, nodeId) >= 0) {
                        return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, n->start, n->end);
                    }
                    *outType = resolvedType;
                    if (*outType >= 0 && (uint32_t)*outType < c->typeLen
                        && c->types[*outType].kind == H2TCType_ALIAS
                        && H2TCResolveAliasTypeId(c, *outType) != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
            }
            {
                int32_t varLikeNameIndex = -1;
                int32_t varLikeNode = H2TCFindTopLevelVarLikeNode(
                    c, n->dataStart, n->dataEnd, &varLikeNameIndex);
                if (varLikeNode >= 0 && c->ast->nodes[varLikeNode].kind == H2Ast_CONST) {
                    int32_t          resolvedConstType = -1;
                    H2TCConstEvalCtx evalCtx;
                    H2CTFEValue      value;
                    int              isConst = 0;
                    int32_t          reflectedType = -1;
                    if (H2TCTypeTopLevelVarLikeNode(
                            c, varLikeNode, varLikeNameIndex, &resolvedConstType)
                        != 0)
                    {
                        return -1;
                    }
                    if (resolvedConstType == c->typeType) {
                        memset(&evalCtx, 0, sizeof(evalCtx));
                        evalCtx.tc = c;
                        evalCtx.rootCallOwnerFnIndex = -1;
                        if (H2TCEvalTopLevelConstNodeAt(
                                c, &evalCtx, varLikeNode, varLikeNameIndex, &value, &isConst)
                            != 0)
                        {
                            return -1;
                        }
                        if (isConst && value.kind == H2CTFEValue_TYPE
                            && H2TCDecodeTypeTag(c, value.typeTag, &reflectedType) == 0)
                        {
                            if (H2AstFirstChild(c->ast, nodeId) >= 0) {
                                return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, n->start, n->end);
                            }
                            *outType = reflectedType;
                            return 0;
                        }
                    }
                }
            }
            return H2TCFailSpan(c, H2Diag_UNKNOWN_TYPE, n->dataStart, n->dataEnd);
        }
        case H2Ast_TYPE_PTR: {
            int32_t          child = H2AstFirstChild(c->ast, nodeId);
            int32_t          baseType;
            int32_t          ptrType;
            const H2AstNode* childNode;
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
            }
            childNode = &c->ast->nodes[child];
            if (childNode->kind == H2Ast_TYPE_SLICE || childNode->kind == H2Ast_TYPE_MUTSLICE) {
                int32_t elemNode = H2AstFirstChild(c->ast, child);
                int32_t elemType;
                int32_t sliceType;
                if (elemNode < 0) {
                    return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
                }
                if (H2TCResolveTypeNode(c, elemNode, &elemType) != 0) {
                    return -1;
                }
                sliceType = H2TCInternSliceType(c, elemType, 1, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                ptrType = H2TCInternPtrType(c, sliceType, n->start, n->end);
                if (ptrType < 0) {
                    return -1;
                }
                *outType = ptrType;
                return 0;
            }
            if (H2TCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            if (baseType >= 0 && (uint32_t)baseType < c->typeLen
                && c->types[baseType].kind == H2TCType_SLICE
                && !H2TCTypeIsMutable(&c->types[baseType]))
            {
                int32_t mutableSliceType = H2TCInternSliceType(
                    c, c->types[baseType].baseType, 1, n->start, n->end);
                if (mutableSliceType < 0) {
                    return -1;
                }
                baseType = mutableSliceType;
            }
            ptrType = H2TCInternPtrType(c, baseType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        case H2Ast_TYPE_REF:
        case H2Ast_TYPE_MUTREF: {
            int32_t          child = H2AstFirstChild(c->ast, nodeId);
            int32_t          baseType;
            int32_t          refType;
            const H2AstNode* childNode;
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
            }
            childNode = &c->ast->nodes[child];
            if (childNode->kind == H2Ast_TYPE_SLICE || childNode->kind == H2Ast_TYPE_MUTSLICE) {
                int32_t elemNode = H2AstFirstChild(c->ast, child);
                int32_t elemType;
                int32_t sliceType;
                if (elemNode < 0) {
                    return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
                }
                if (H2TCResolveTypeNode(c, elemNode, &elemType) != 0) {
                    return -1;
                }
                sliceType = H2TCInternSliceType(c, elemType, 0, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                refType = H2TCInternRefType(
                    c, sliceType, n->kind == H2Ast_TYPE_MUTREF, n->start, n->end);
                if (refType < 0) {
                    return -1;
                }
                *outType = refType;
                return 0;
            }
            if (H2TCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            if (baseType >= 0 && (uint32_t)baseType < c->typeLen
                && c->types[baseType].kind == H2TCType_SLICE
                && H2TCTypeIsMutable(&c->types[baseType]))
            {
                int32_t readOnlySliceType = H2TCInternSliceType(
                    c, c->types[baseType].baseType, 0, n->start, n->end);
                if (readOnlySliceType < 0) {
                    return -1;
                }
                baseType = readOnlySliceType;
            }
            refType = H2TCInternRefType(
                c, baseType, n->kind == H2Ast_TYPE_MUTREF, n->start, n->end);
            if (refType < 0) {
                return -1;
            }
            *outType = refType;
            return 0;
        }
        case H2Ast_TYPE_ARRAY: {
            int32_t  child = H2AstFirstChild(c->ast, nodeId);
            int32_t  lenNode = child >= 0 ? H2AstNextSibling(c->ast, child) : -1;
            int32_t  baseType;
            int32_t  arrayType;
            int64_t  lenValue = 0;
            int      lenIsConst = 0;
            uint32_t arrayLen;
            if (child < 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
            }
            if (lenNode >= 0) {
                int32_t lenType;
                if (H2AstNextSibling(c->ast, lenNode) >= 0) {
                    return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
                }
                if (H2TCTypeExpr(c, lenNode, &lenType) != 0) {
                    return -1;
                }
                if (!H2TCIsIntegerType(c, lenType)) {
                    return H2TCFailNode(c, lenNode, H2Diag_TYPE_MISMATCH);
                }
                if (H2TCConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0) {
                    return -1;
                }
                if (!lenIsConst) {
                    int rc = H2TCFailNode(c, lenNode, H2Diag_ARRAY_LEN_CONST_REQUIRED);
                    H2TCAttachConstEvalReason(c);
                    return rc;
                }
                if (lenValue < 0 || lenValue > (int64_t)UINT32_MAX) {
                    return H2TCFailNode(c, lenNode, H2Diag_ARRAY_LEN_CONST_REQUIRED);
                }
                arrayLen = (uint32_t)lenValue;
            } else if (H2TCParseArrayLen(c, n, &arrayLen) != 0) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
            }
            if (H2TCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            arrayType = H2TCInternArrayType(c, baseType, arrayLen, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            *outType = arrayType;
            return 0;
        }
        case H2Ast_TYPE_SLICE:
        case H2Ast_TYPE_MUTSLICE: {
            int32_t child = H2AstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t sliceType;
            if (H2TCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            sliceType = H2TCInternSliceType(
                c, baseType, n->kind == H2Ast_TYPE_MUTSLICE, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            *outType = sliceType;
            return 0;
        }
        case H2Ast_TYPE_OPTIONAL: {
            int32_t child = H2AstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t optType;
            if (H2TCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            optType = H2TCInternOptionalType(c, baseType, n->start, n->end);
            if (optType < 0) {
                return -1;
            }
            *outType = optType;
            return 0;
        }
        case H2Ast_TYPE_FN: {
            int32_t  child = H2AstFirstChild(c->ast, nodeId);
            int32_t  returnType = c->typeVoid;
            uint32_t paramCount = 0;
            int      isVariadic = 0;
            int      sawReturnType = 0;
            int32_t  savedParamTypes[H2TC_MAX_CALL_ARGS];
            uint8_t  savedParamFlags[H2TC_MAX_CALL_ARGS];
            while (child >= 0) {
                const H2AstNode* ch = &c->ast->nodes[child];
                if (ch->flags == 1) {
                    uint32_t i;
                    if (sawReturnType) {
                        return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
                    }
                    if (paramCount > H2TC_MAX_CALL_ARGS) {
                        return H2TCFailNode(c, child, H2Diag_ARENA_OOM);
                    }
                    for (i = 0; i < paramCount; i++) {
                        savedParamTypes[i] = c->scratchParamTypes[i];
                        savedParamFlags[i] = c->scratchParamFlags[i];
                    }
                    c->allowConstNumericTypeName = 1;
                    if (H2TCResolveTypeNode(c, child, &returnType) != 0) {
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowConstNumericTypeName = 0;
                    for (i = 0; i < paramCount; i++) {
                        c->scratchParamTypes[i] = savedParamTypes[i];
                        c->scratchParamFlags[i] = savedParamFlags[i];
                    }
                    if (H2TCTypeContainsVarSizeByValue(c, returnType)) {
                        return H2TCFailVarSizeByValue(
                            c, child, returnType, "function-type return position");
                    }
                    sawReturnType = 1;
                } else {
                    int32_t paramType;
                    if (paramCount >= c->scratchParamCap) {
                        return H2TCFailNode(c, child, H2Diag_ARENA_OOM);
                    }
                    c->allowAnytypeParamType = 1;
                    c->allowConstNumericTypeName = 1;
                    if (H2TCResolveTypeNode(c, child, &paramType) != 0) {
                        c->allowAnytypeParamType = 0;
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowAnytypeParamType = 0;
                    c->allowConstNumericTypeName = 0;
                    if ((paramType == c->typeUntypedInt || paramType == c->typeUntypedFloat)
                        && (ch->flags & H2AstFlag_PARAM_CONST) == 0)
                    {
                        return H2TCFailSpan(
                            c,
                            H2Diag_CONST_NUMERIC_PARAM_REQUIRES_CONST,
                            ch->dataStart,
                            ch->dataEnd);
                    }
                    if (H2TCTypeContainsVarSizeByValue(c, paramType)) {
                        return H2TCFailVarSizeByValue(
                            c, child, paramType, "function-type parameter position");
                    }
                    if ((ch->flags & H2AstFlag_PARAM_VARIADIC) != 0) {
                        int32_t sliceType;
                        if (isVariadic) {
                            return H2TCFailNode(c, child, H2Diag_VARIADIC_PARAM_NOT_LAST);
                        }
                        if (paramType != c->typeAnytype) {
                            sliceType = H2TCInternSliceType(c, paramType, 0, ch->start, ch->end);
                            if (sliceType < 0) {
                                return -1;
                            }
                            paramType = sliceType;
                        }
                        isVariadic = 1;
                    } else if (isVariadic) {
                        return H2TCFailNode(c, child, H2Diag_VARIADIC_PARAM_NOT_LAST);
                    }
                    c->scratchParamTypes[paramCount++] = paramType;
                    c->scratchParamFlags[paramCount - 1u] =
                        (ch->flags & H2AstFlag_PARAM_CONST) != 0 ? H2TCFuncParamFlag_CONST : 0u;
                }
                child = H2AstNextSibling(c->ast, child);
            }
            {
                int32_t fnType = H2TCInternFunctionType(
                    c,
                    returnType,
                    c->scratchParamTypes,
                    c->scratchParamFlags,
                    paramCount,
                    isVariadic,
                    -1,
                    n->start,
                    n->end);
                if (fnType < 0) {
                    return -1;
                }
                *outType = fnType;
                return 0;
            }
        }
        case H2Ast_TYPE_TUPLE: {
            int32_t  child = H2AstFirstChild(c->ast, nodeId);
            uint32_t elemCount = 0;
            while (child >= 0) {
                int32_t elemType;
                if (elemCount >= c->scratchParamCap) {
                    return H2TCFailNode(c, child, H2Diag_ARENA_OOM);
                }
                if (H2TCResolveTypeNode(c, child, &elemType) != 0) {
                    return -1;
                }
                if (H2TCTypeContainsVarSizeByValue(c, elemType)) {
                    return H2TCFailVarSizeByValue(c, child, elemType, "tuple element position");
                }
                c->scratchParamTypes[elemCount++] = elemType;
                child = H2AstNextSibling(c->ast, child);
            }
            if (elemCount < 2u) {
                return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
            }
            {
                int32_t tupleType = H2TCInternTupleType(
                    c, c->scratchParamTypes, elemCount, n->start, n->end);
                if (tupleType < 0) {
                    return -1;
                }
                *outType = tupleType;
                return 0;
            }
        }
        case H2Ast_TYPE_ANON_STRUCT: return H2TCResolveAnonAggregateTypeNode(c, nodeId, 0, outType);
        case H2Ast_TYPE_ANON_UNION:  return H2TCResolveAnonAggregateTypeNode(c, nodeId, 1, outType);
        case H2Ast_TYPE_VARRAY:      return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
        default:                     return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }
}

int H2TCAddNamedType(H2TypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId, int32_t* outTypeId) {
    const H2AstNode* node = &c->ast->nodes[nodeId];
    H2TCType         t;
    int32_t          typeId;
    int32_t          dupNamedIndex;
    uint32_t         idx;
    uint32_t         templateArgStart = 0;
    uint16_t         templateArgCount = 0;

    if (node->dataEnd <= node->dataStart) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }

    dupNamedIndex = H2TCFindNamedTypeIndexOwned(c, ownerTypeId, node->dataStart, node->dataEnd);
    if (dupNamedIndex >= 0) {
        return H2TCFailDuplicateDefinition(
            c,
            node->dataStart,
            node->dataEnd,
            c->namedTypes[(uint32_t)dupNamedIndex].nameStart,
            c->namedTypes[(uint32_t)dupNamedIndex].nameEnd);
    }

    t.kind = node->kind == H2Ast_TYPE_ALIAS ? H2TCType_ALIAS : H2TCType_NAMED;
    t.builtin = H2Builtin_INVALID;
    t.baseType = -1;
    t.declNode = nodeId;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = node->dataStart;
    t.nameEnd = node->dataEnd;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    typeId = H2TCAddType(c, &t, node->start, node->end);
    if (typeId < 0) {
        return -1;
    }

    if (c->namedTypeLen >= c->namedTypeCap) {
        return H2TCFailNode(c, nodeId, H2Diag_ARENA_OOM);
    }

    idx = c->namedTypeLen++;
    c->namedTypes[idx].nameStart = node->dataStart;
    c->namedTypes[idx].nameEnd = node->dataEnd;
    c->namedTypes[idx].typeId = typeId;
    c->namedTypes[idx].declNode = nodeId;
    c->namedTypes[idx].ownerTypeId = ownerTypeId;
    c->namedTypes[idx].templateArgStart = 0;
    c->namedTypes[idx].templateArgCount = 0;
    c->namedTypes[idx].templateRootNamedIndex = -1;
    if (H2TCAppendDeclTypeParamPlaceholders(c, nodeId, &templateArgStart, &templateArgCount) != 0) {
        return -1;
    }
    c->namedTypes[idx].templateArgStart = templateArgStart;
    c->namedTypes[idx].templateArgCount = templateArgCount;
    if (outTypeId != NULL) {
        *outTypeId = typeId;
    }
    return 0;
}

int32_t H2TCInstantiateNamedType(
    H2TypeCheckCtx* c, int32_t rootTypeId, const int32_t* argTypes, uint16_t argCount) {
    int32_t         rootNamedIndex = -1;
    uint32_t        i;
    const H2TCType* rootType;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (c->namedTypes[i].typeId == rootTypeId) {
            rootNamedIndex = (int32_t)i;
            break;
        }
    }
    if (rootNamedIndex < 0) {
        return -1;
    }
    if (c->namedTypes[(uint32_t)rootNamedIndex].templateRootNamedIndex >= 0) {
        rootNamedIndex = c->namedTypes[(uint32_t)rootNamedIndex].templateRootNamedIndex;
        rootTypeId = c->namedTypes[(uint32_t)rootNamedIndex].typeId;
    }
    if (c->namedTypes[(uint32_t)rootNamedIndex].templateArgCount != argCount) {
        return -1;
    }
    if (argCount == 0) {
        return rootTypeId;
    }
    for (i = 0; i < c->namedTypeLen; i++) {
        const H2TCNamedType* nt = &c->namedTypes[i];
        uint16_t             j;
        if (nt->templateRootNamedIndex != rootNamedIndex || nt->templateArgCount != argCount) {
            continue;
        }
        for (j = 0; j < argCount; j++) {
            if (c->genericArgTypes[nt->templateArgStart + j] != argTypes[j]) {
                break;
            }
        }
        if (j == argCount) {
            return nt->typeId;
        }
    }
    if (c->typeLen >= c->typeCap || c->namedTypeLen >= c->namedTypeCap
        || c->genericArgLen + argCount > c->genericArgCap)
    {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, 0, 0);
    }
    rootType = &c->types[rootTypeId];
    {
        H2TCType       t = *rootType;
        H2TCNamedType* nt;
        int32_t        typeId;
        t.fieldStart = 0;
        t.fieldCount = 0;
        t.flags &= (uint16_t)~(H2TCTypeFlag_ALIAS_RESOLVING | H2TCTypeFlag_ALIAS_RESOLVED);
        typeId = H2TCAddType(c, &t, rootType->nameStart, rootType->nameEnd);
        if (typeId < 0) {
            return -1;
        }
        nt = &c->namedTypes[c->namedTypeLen++];
        nt->nameStart = rootType->nameStart;
        nt->nameEnd = rootType->nameEnd;
        nt->typeId = typeId;
        nt->declNode = rootType->declNode;
        nt->ownerTypeId = c->namedTypes[(uint32_t)rootNamedIndex].ownerTypeId;
        nt->templateArgStart = c->genericArgLen;
        nt->templateArgCount = argCount;
        nt->templateRootNamedIndex = (int16_t)rootNamedIndex;
        for (i = 0; i < argCount; i++) {
            c->genericArgTypes[c->genericArgLen++] = argTypes[i];
        }
        if (t.kind == H2TCType_ALIAS) {
            int32_t baseType = H2TCSubstituteType(
                c,
                rootType->baseType,
                &c->genericArgTypes[c->namedTypes[(uint32_t)rootNamedIndex].templateArgStart],
                &c->genericArgTypes[nt->templateArgStart],
                argCount,
                rootType->nameStart,
                rootType->nameEnd);
            if (baseType < 0) {
                return -1;
            }
            c->types[typeId].baseType = baseType;
            c->types[typeId].flags |= H2TCTypeFlag_ALIAS_RESOLVED;
        }
        return typeId;
    }
}

static int H2TCIsNamedTypeDeclKind(H2AstKind kind) {
    return kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM
        || kind == H2Ast_TYPE_ALIAS;
}

static int H2TCCollectTypeDeclsFromNodeWithOwner(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId) {
    H2AstKind kind = c->ast->nodes[nodeId].kind;
    if (kind == H2Ast_PUB) {
        int32_t ch = H2AstFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (H2TCCollectTypeDeclsFromNodeWithOwner(c, ch, ownerTypeId) != 0) {
                return -1;
            }
            ch = H2AstNextSibling(c->ast, ch);
        }
        return 0;
    }
    if (H2TCIsNamedTypeDeclKind(kind)) {
        int32_t declaredTypeId = -1;
        if (H2TCAddNamedType(c, nodeId, ownerTypeId, &declaredTypeId) != 0) {
            return -1;
        }
        if (kind == H2Ast_STRUCT || kind == H2Ast_UNION) {
            int32_t child = H2AstFirstChild(c->ast, nodeId);
            while (child >= 0) {
                H2AstKind childKind = c->ast->nodes[child].kind;
                if (H2TCIsNamedTypeDeclKind(childKind)
                    && H2TCCollectTypeDeclsFromNodeWithOwner(c, child, declaredTypeId) != 0)
                {
                    return -1;
                }
                child = H2AstNextSibling(c->ast, child);
            }
        }
        return 0;
    }
    return 0;
}

int H2TCCollectTypeDeclsFromNode(H2TypeCheckCtx* c, int32_t nodeId) {
    return H2TCCollectTypeDeclsFromNodeWithOwner(c, nodeId, -1);
}

int H2TCIsIntegerType(H2TypeCheckCtx* c, int32_t typeId) {
    H2BuiltinKind b;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedInt) {
        return 1;
    }
    if (c->types[typeId].kind != H2TCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == H2Builtin_U8 || b == H2Builtin_U16 || b == H2Builtin_U32 || b == H2Builtin_U64
        || b == H2Builtin_I8 || b == H2Builtin_I16 || b == H2Builtin_I32 || b == H2Builtin_I64
        || b == H2Builtin_USIZE || b == H2Builtin_ISIZE;
}

int H2TCIsConstNumericType(H2TypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

int H2TCTypeIsRuneLike(H2TypeCheckCtx* c, int32_t typeId) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && depth++ <= c->typeLen) {
        if (typeId == c->typeRune) {
            return 1;
        }
        if (c->types[typeId].kind != H2TCType_ALIAS) {
            break;
        }
        if (H2TCResolveAliasTypeId(c, typeId) != 0) {
            return 0;
        }
        typeId = c->types[typeId].baseType;
    }
    return 0;
}

uint32_t H2TCU64BitLen(uint64_t v) {
    uint32_t bits = 0;
    while (v != 0u) {
        v >>= 1u;
        bits++;
    }
    return bits;
}

int H2TCConstIntFitsType(H2TypeCheckCtx* c, int64_t value, int32_t typeId) {
    H2BuiltinKind b;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedInt) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != H2TCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    switch (b) {
        case H2Builtin_U8:    return value >= 0 && value <= (int64_t)UINT8_MAX;
        case H2Builtin_U16:   return value >= 0 && value <= (int64_t)UINT16_MAX;
        case H2Builtin_U32:   return value >= 0 && value <= (int64_t)UINT32_MAX;
        case H2Builtin_U64:
        case H2Builtin_USIZE: return value >= 0;
        case H2Builtin_I8:    return value >= (int64_t)INT8_MIN && value <= (int64_t)INT8_MAX;
        case H2Builtin_I16:   return value >= (int64_t)INT16_MIN && value <= (int64_t)INT16_MAX;
        case H2Builtin_I32:   return value >= (int64_t)INT32_MIN && value <= (int64_t)INT32_MAX;
        case H2Builtin_I64:
        case H2Builtin_ISIZE: return 1;
        default:              return 0;
    }
}

int H2TCConstIntFitsFloatType(H2TypeCheckCtx* c, int64_t value, int32_t typeId) {
    H2BuiltinKind b;
    uint32_t      precisionBits;
    uint64_t      magnitude;
    uint32_t      bits;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != H2TCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    if (b == H2Builtin_F32) {
        precisionBits = 23u;
    } else if (b == H2Builtin_F64) {
        precisionBits = 53u;
    } else {
        return 0;
    }
    if (value < 0) {
        magnitude = (uint64_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint64_t)value;
    }
    bits = H2TCU64BitLen(magnitude);
    return bits <= precisionBits;
}

int H2TCConstFloatFitsType(H2TypeCheckCtx* c, double value, int32_t typeId) {
    H2BuiltinKind b;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != H2TCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    if (b == H2Builtin_F64) {
        return 1;
    }
    if (b != H2Builtin_F32) {
        return 0;
    }
    if (value != value) {
        return 1;
    }
    return (double)(float)value == value;
}

int H2TCFailConstIntRange(H2TypeCheckCtx* c, int32_t nodeId, int64_t value, int32_t expectedType) {
    char        dstTypeBuf[H2TC_DIAG_TEXT_CAP];
    char        detailBuf[256];
    H2TCTextBuf dstTypeText;
    H2TCTextBuf detailText;
    H2TCSetDiag(
        c->diag, H2Diag_TYPE_MISMATCH, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
    if (c->diag == NULL) {
        return -1;
    }
    H2TCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    H2TCFormatTypeRec(c, expectedType, &dstTypeText, 0);
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "constant value 0x");
    H2TCTextBufAppendHexU64(&detailText, (uint64_t)value);
    H2TCTextBufAppendCStr(&detailText, " is out of range for ");
    H2TCTextBufAppendCStr(&detailText, dstTypeBuf);
    c->diag->detail = H2TCAllocDiagText(c, detailBuf);
    return -1;
}

int H2TCFailConstFloatRange(H2TypeCheckCtx* c, int32_t nodeId, int32_t expectedType) {
    char        dstTypeBuf[H2TC_DIAG_TEXT_CAP];
    char        detailBuf[256];
    H2TCTextBuf dstTypeText;
    H2TCTextBuf detailText;
    H2TCSetDiag(
        c->diag, H2Diag_TYPE_MISMATCH, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
    if (c->diag == NULL) {
        return -1;
    }
    H2TCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    H2TCFormatTypeRec(c, expectedType, &dstTypeText, 0);
    H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    H2TCTextBufAppendCStr(&detailText, "constant value is not representable for ");
    H2TCTextBufAppendCStr(&detailText, dstTypeBuf);
    c->diag->detail = H2TCAllocDiagText(c, detailBuf);
    return -1;
}

int H2TCIsFloatType(H2TypeCheckCtx* c, int32_t typeId) {
    H2BuiltinKind b;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (c->types[typeId].kind != H2TCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == H2Builtin_F32 || b == H2Builtin_F64;
}

int H2TCIsNumericType(H2TypeCheckCtx* c, int32_t typeId) {
    return H2TCIsIntegerType(c, typeId) || H2TCIsFloatType(c, typeId);
}

int H2TCIsBoolType(H2TypeCheckCtx* c, int32_t typeId) {
    typeId = H2TCResolveAliasBaseType(c, typeId);
    return typeId == c->typeBool;
}

int H2TCIsRawptrType(H2TypeCheckCtx* c, int32_t typeId) {
    typeId = H2TCResolveAliasBaseType(c, typeId);
    return c != NULL && typeId >= 0 && typeId == c->typeRawptr;
}

int H2TCIsNamedDeclKind(H2TypeCheckCtx* c, int32_t typeId, H2AstKind kind) {
    int32_t declNode;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != H2TCType_NAMED) {
        return 0;
    }
    declNode = c->types[typeId].declNode;
    return declNode >= 0 && (uint32_t)declNode < c->ast->len
        && c->ast->nodes[declNode].kind == kind;
}

int H2TCIsStringLikeType(H2TypeCheckCtx* c, int32_t typeId) {
    int32_t baseType;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind != H2TCType_PTR && c->types[typeId].kind != H2TCType_REF) {
        return 0;
    }
    baseType = H2TCResolveAliasBaseType(c, c->types[typeId].baseType);
    return baseType == c->typeStr;
}

int H2TCTypeSupportsFmtReflectRec(H2TypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    uint32_t i;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (H2TCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case H2TCType_BUILTIN:
        case H2TCType_UNTYPED_INT:
        case H2TCType_UNTYPED_FLOAT:
            return H2TCIsBoolType(c, typeId) || H2TCIsNumericType(c, typeId)
                || typeId == c->typeType || H2TCIsRawptrType(c, typeId);
        case H2TCType_ARRAY:
        case H2TCType_SLICE:
            return H2TCTypeSupportsFmtReflectRec(c, c->types[typeId].baseType, depth + 1u);
        case H2TCType_PTR:
        case H2TCType_REF: return 1;
        case H2TCType_OPTIONAL:
            return H2TCTypeSupportsFmtReflectRec(c, c->types[typeId].baseType, depth + 1u);
        case H2TCType_NULL: return 1;
        case H2TCType_NAMED:
            if (H2TCIsNamedDeclKind(c, typeId, H2Ast_ENUM)) {
                return 1;
            }
            if (H2TCIsNamedDeclKind(c, typeId, H2Ast_UNION)) {
                return 0;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!H2TCTypeSupportsFmtReflectRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case H2TCType_ANON_STRUCT:
        case H2TCType_TUPLE:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!H2TCTypeSupportsFmtReflectRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case H2TCType_ANON_UNION: return 0;
        default:                  return 0;
    }
}

int H2TCIsComparableTypeRec(H2TypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    uint32_t i;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (H2TCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case H2TCType_BUILTIN:
        case H2TCType_UNTYPED_INT:
        case H2TCType_UNTYPED_FLOAT:
            return H2TCIsBoolType(c, typeId) || H2TCIsNumericType(c, typeId)
                || typeId == c->typeType || H2TCIsRawptrType(c, typeId);
        case H2TCType_ARRAY:
        case H2TCType_SLICE:
            return H2TCIsComparableTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case H2TCType_PTR:
        case H2TCType_REF: return 1;
        case H2TCType_NAMED:
            if (H2TCIsNamedDeclKind(c, typeId, H2Ast_ENUM)) {
                return 1;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!H2TCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case H2TCType_ANON_STRUCT:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!H2TCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case H2TCType_ANON_UNION:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!H2TCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case H2TCType_OPTIONAL:
            return H2TCIsComparableTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case H2TCType_NULL: return 1;
        default:            return 0;
    }
}

int H2TCIsOrderedTypeRec(H2TypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (H2TCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case H2TCType_BUILTIN:       return H2TCIsNumericType(c, typeId) || H2TCIsRawptrType(c, typeId);
        case H2TCType_UNTYPED_INT:
        case H2TCType_UNTYPED_FLOAT: return H2TCIsNumericType(c, typeId);
        case H2TCType_ARRAY:
        case H2TCType_SLICE:         return H2TCIsOrderedTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case H2TCType_PTR:
        case H2TCType_REF:           return 1;
        case H2TCType_NAMED:         return H2TCIsNamedDeclKind(c, typeId, H2Ast_ENUM);
        case H2TCType_OPTIONAL:
            return H2TCIsOrderedTypeRec(c, c->types[typeId].baseType, depth + 1u);
        default: return 0;
    }
}

int H2TCIsComparableType(H2TypeCheckCtx* c, int32_t typeId) {
    return H2TCIsComparableTypeRec(c, typeId, 0u);
}

int H2TCIsOrderedType(H2TypeCheckCtx* c, int32_t typeId) {
    return H2TCIsOrderedTypeRec(c, typeId, 0u);
}

int H2TCTypeSupportsLen(H2TypeCheckCtx* c, int32_t typeId) {
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind == H2TCType_PACK) {
        return 1;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind == H2TCType_ARRAY) {
        return 1;
    }
    if (c->types[typeId].kind == H2TCType_SLICE) {
        return 1;
    }
    if (c->types[typeId].kind == H2TCType_PTR || c->types[typeId].kind == H2TCType_REF) {
        int32_t baseType = H2TCResolveAliasBaseType(c, c->types[typeId].baseType);
        if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
            return 0;
        }
        if (baseType == c->typeStr) {
            return 1;
        }
        return c->types[baseType].kind == H2TCType_ARRAY
            || c->types[baseType].kind == H2TCType_SLICE;
    }
    return 0;
}

int H2TCIsUntyped(H2TypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

int H2TCIsTypeNodeKind(H2AstKind kind) {
    return kind == H2Ast_TYPE_NAME || kind == H2Ast_TYPE_PTR || kind == H2Ast_TYPE_REF
        || kind == H2Ast_TYPE_MUTREF || kind == H2Ast_TYPE_ARRAY || kind == H2Ast_TYPE_VARRAY
        || kind == H2Ast_TYPE_SLICE || kind == H2Ast_TYPE_MUTSLICE || kind == H2Ast_TYPE_OPTIONAL
        || kind == H2Ast_TYPE_FN || kind == H2Ast_TYPE_ANON_STRUCT || kind == H2Ast_TYPE_ANON_UNION
        || kind == H2Ast_TYPE_TUPLE;
}

int H2TCConcretizeInferredType(H2TypeCheckCtx* c, int32_t typeId, int32_t* outType) {
    const H2TCType* t;
    uint32_t        i;
    if (typeId == c->typeUntypedInt) {
        int32_t t = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
        if (t < 0) {
            return H2TCFailSpan(c, H2Diag_UNKNOWN_TYPE, 0, 0);
        }
        *outType = t;
        return 0;
    }
    if (typeId == c->typeUntypedFloat) {
        int32_t t = H2TCFindBuiltinByKind(c, H2Builtin_F64);
        if (t < 0) {
            return H2TCFailSpan(c, H2Diag_UNKNOWN_TYPE, 0, 0);
        }
        *outType = t;
        return 0;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        *outType = typeId;
        return 0;
    }
    t = &c->types[typeId];
    if (t->kind == H2TCType_TUPLE) {
        int32_t elems[256];
        int     changed = 0;
        if (t->fieldCount > (uint16_t)(sizeof(elems) / sizeof(elems[0]))) {
            return H2TCFailSpan(c, H2Diag_ARENA_OOM, 0, 0);
        }
        for (i = 0; i < t->fieldCount; i++) {
            int32_t elem = c->funcParamTypes[t->fieldStart + i];
            int32_t concreteElem = elem;
            if (H2TCConcretizeInferredType(c, elem, &concreteElem) != 0) {
                return -1;
            }
            if (concreteElem != elem) {
                changed = 1;
            }
            elems[i] = concreteElem;
        }
        if (!changed) {
            *outType = typeId;
            return 0;
        }
        *outType = H2TCInternTupleType(c, elems, t->fieldCount, 0, 0);
        return *outType < 0 ? -1 : 0;
    }
    *outType = typeId;
    return 0;
}

static int32_t H2TCLookupSubstitutedType(
    const int32_t* paramTypes, const int32_t* argTypes, uint16_t argCount, int32_t typeId) {
    uint16_t i;
    for (i = 0; i < argCount; i++) {
        if (paramTypes[i] == typeId) {
            return argTypes[i];
        }
    }
    return typeId;
}

int32_t H2TCSubstituteType(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    const int32_t*  paramTypes,
    const int32_t*  argTypes,
    uint16_t        argCount,
    uint32_t        errStart,
    uint32_t        errEnd) {
    const H2TCType* t;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || argCount == 0) {
        return typeId;
    }
    t = &c->types[typeId];
    if (t->kind == H2TCType_TYPE_PARAM) {
        return H2TCLookupSubstitutedType(paramTypes, argTypes, argCount, typeId);
    }
    switch (t->kind) {
        case H2TCType_PTR: {
            int32_t baseType = H2TCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : H2TCInternPtrType(c, baseType, errStart, errEnd);
        }
        case H2TCType_REF: {
            int32_t baseType = H2TCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : H2TCInternRefType(
                           c, baseType, (t->flags & H2TCTypeFlag_MUTABLE) != 0, errStart, errEnd);
        }
        case H2TCType_ARRAY: {
            int32_t baseType = H2TCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : H2TCInternArrayType(c, baseType, t->arrayLen, errStart, errEnd);
        }
        case H2TCType_SLICE: {
            int32_t baseType = H2TCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : H2TCInternSliceType(
                           c, baseType, (t->flags & H2TCTypeFlag_MUTABLE) != 0, errStart, errEnd);
        }
        case H2TCType_OPTIONAL: {
            int32_t baseType = H2TCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : H2TCInternOptionalType(c, baseType, errStart, errEnd);
        }
        case H2TCType_TUPLE:
        case H2TCType_PACK:  {
            int32_t  elems[256];
            uint32_t i;
            int      changed = 0;
            if (t->fieldCount > (uint16_t)(sizeof(elems) / sizeof(elems[0]))) {
                return -1;
            }
            for (i = 0; i < t->fieldCount; i++) {
                int32_t elem = c->funcParamTypes[t->fieldStart + i];
                elems[i] = H2TCSubstituteType(
                    c, elem, paramTypes, argTypes, argCount, errStart, errEnd);
                if (elems[i] != elem) {
                    changed = 1;
                }
            }
            if (!changed) {
                return typeId;
            }
            return t->kind == H2TCType_TUPLE
                     ? H2TCInternTupleType(c, elems, t->fieldCount, errStart, errEnd)
                     : H2TCInternPackType(c, elems, t->fieldCount, errStart, errEnd);
        }
        case H2TCType_FUNCTION: {
            int32_t  params[256];
            uint8_t  flags[256];
            int32_t  returnType;
            uint32_t i;
            int      changed = 0;
            if (t->fieldCount > (uint16_t)(sizeof(params) / sizeof(params[0]))) {
                return -1;
            }
            returnType = H2TCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            changed = returnType != t->baseType;
            for (i = 0; i < t->fieldCount; i++) {
                int32_t paramType = c->funcParamTypes[t->fieldStart + i];
                params[i] = H2TCSubstituteType(
                    c, paramType, paramTypes, argTypes, argCount, errStart, errEnd);
                flags[i] = c->funcParamFlags[t->fieldStart + i];
                if (params[i] != paramType) {
                    changed = 1;
                }
            }
            if (!changed) {
                return typeId;
            }
            return H2TCInternFunctionType(
                c,
                returnType,
                params,
                flags,
                t->fieldCount,
                (t->flags & H2TCTypeFlag_FUNCTION_VARIADIC) != 0,
                t->funcIndex,
                errStart,
                errEnd);
        }
        case H2TCType_NAMED:
        case H2TCType_ALIAS: {
            int32_t  namedIndex = -1;
            uint32_t ni;
            for (ni = 0; ni < c->namedTypeLen; ni++) {
                if (c->namedTypes[ni].typeId == typeId) {
                    namedIndex = (int32_t)ni;
                    break;
                }
            }
            if (namedIndex >= 0) {
                const H2TCNamedType* nt = &c->namedTypes[(uint32_t)namedIndex];
                if (nt->templateRootNamedIndex >= 0 || nt->templateArgCount > 0) {
                    int32_t  args[64];
                    uint16_t i;
                    int      changed = 0;
                    if (nt->templateArgCount > (uint16_t)(sizeof(args) / sizeof(args[0]))) {
                        return -1;
                    }
                    for (i = 0; i < nt->templateArgCount; i++) {
                        int32_t argType = c->genericArgTypes[nt->templateArgStart + i];
                        args[i] = H2TCSubstituteType(
                            c, argType, paramTypes, argTypes, argCount, errStart, errEnd);
                        if (args[i] != argType) {
                            changed = 1;
                        }
                    }
                    if (!changed && nt->templateRootNamedIndex >= 0) {
                        return typeId;
                    }
                    if (nt->templateRootNamedIndex >= 0) {
                        return H2TCInstantiateNamedType(
                            c,
                            c->namedTypes[(uint32_t)nt->templateRootNamedIndex].typeId,
                            args,
                            nt->templateArgCount);
                    }
                }
            }
            return typeId;
        }
        default: return typeId;
    }
}

int H2TCTypeIsVarSize(H2TypeCheckCtx* c, int32_t typeId) {
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    return (c->types[typeId].flags & H2TCTypeFlag_VARSIZE) != 0;
}

int H2TCTypeContainsVarSizeByValue(H2TypeCheckCtx* c, int32_t typeId) {
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind == H2TCType_PTR || c->types[typeId].kind == H2TCType_REF) {
        return 0;
    }
    if (c->types[typeId].kind == H2TCType_SLICE) {
        return 1;
    }
    if (c->types[typeId].kind == H2TCType_OPTIONAL) {
        return H2TCTypeContainsVarSizeByValue(c, c->types[typeId].baseType);
    }
    if (c->types[typeId].kind == H2TCType_ARRAY) {
        return H2TCTypeContainsVarSizeByValue(c, c->types[typeId].baseType);
    }
    if (c->types[typeId].kind == H2TCType_TUPLE) {
        uint32_t i;
        for (i = 0; i < c->types[typeId].fieldCount; i++) {
            if (H2TCTypeContainsVarSizeByValue(
                    c, c->funcParamTypes[c->types[typeId].fieldStart + i]))
            {
                return 1;
            }
        }
        return 0;
    }
    return H2TCTypeIsVarSize(c, typeId);
}

int H2TCIsComparisonHookName(
    H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int* outIsEqualHook) {
    if (H2NameEqLiteral(c->src, nameStart, nameEnd, "__equal")) {
        *outIsEqualHook = 1;
        return 1;
    }
    if (H2NameEqLiteral(c->src, nameStart, nameEnd, "__order")) {
        *outIsEqualHook = 0;
        return 1;
    }
    return 0;
}

int H2TCTypeIsU8Slice(H2TypeCheckCtx* c, int32_t typeId, int requireMutable) {
    int32_t u8Type;
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind != H2TCType_SLICE) {
        return 0;
    }
    u8Type = H2TCFindBuiltinByKind(c, H2Builtin_U8);
    if (u8Type < 0 || c->types[typeId].baseType != u8Type) {
        return 0;
    }
    if (requireMutable && !H2TCTypeIsMutable(&c->types[typeId])) {
        return 0;
    }
    return 1;
}

int H2TCTypeIsFreeablePointer(H2TypeCheckCtx* c, int32_t typeId) {
    typeId = H2TCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind == H2TCType_OPTIONAL) {
        return H2TCTypeIsFreeablePointer(c, c->types[typeId].baseType);
    }
    return c->types[typeId].kind == H2TCType_PTR;
}

int32_t H2TCFindEmbeddedFieldIndex(H2TypeCheckCtx* c, int32_t namedTypeId) {
    uint32_t i;
    if (namedTypeId < 0 || (uint32_t)namedTypeId >= c->typeLen
        || c->types[namedTypeId].kind != H2TCType_NAMED)
    {
        return -1;
    }
    if (H2TCEnsureNamedTypeFieldsResolved(c, namedTypeId) != 0) {
        return -1;
    }
    for (i = 0; i < c->types[namedTypeId].fieldCount; i++) {
        uint32_t idx = c->types[namedTypeId].fieldStart + i;
        if ((c->fields[idx].flags & H2TCFieldFlag_EMBEDDED) != 0) {
            return (int32_t)idx;
        }
    }
    return -1;
}

int H2TCEmbedDistanceToType(
    H2TypeCheckCtx* c, int32_t srcType, int32_t dstType, uint32_t* outDistance) {
    uint32_t depth = 0;
    int32_t  cur = srcType;
    if (srcType == dstType) {
        *outDistance = 0;
        return 0;
    }
    while (depth++ <= c->typeLen) {
        int32_t embedIdx;
        if (cur < 0 || (uint32_t)cur >= c->typeLen || c->types[cur].kind != H2TCType_NAMED) {
            return -1;
        }
        embedIdx = H2TCFindEmbeddedFieldIndex(c, cur);
        if (embedIdx < 0) {
            return -1;
        }
        cur = c->fields[embedIdx].typeId;
        if (cur >= 0 && (uint32_t)cur < c->typeLen && c->types[cur].kind == H2TCType_ALIAS) {
            cur = H2TCResolveAliasBaseType(c, cur);
            if (cur < 0) {
                return -1;
            }
        }
        if (cur == dstType) {
            *outDistance = depth;
            return 0;
        }
    }
    return -1;
}

int H2TCIsTypeDerivedFromEmbedded(H2TypeCheckCtx* c, int32_t srcType, int32_t dstType) {
    uint32_t distance = 0;
    return H2TCEmbedDistanceToType(c, srcType, dstType, &distance) == 0;
}

int H2TCCanAssign(H2TypeCheckCtx* c, int32_t dstType, int32_t srcType) {
    const H2TCType* dst;
    const H2TCType* src;

    if (dstType == srcType) {
        return 1;
    }
    if (H2TCTypeIsFmtValue(c, dstType)) {
        return H2TCTypeSupportsFmtReflectRec(c, srcType, 0u);
    }
    if (dstType == c->typeAnytype) {
        return srcType >= 0 && (uint32_t)srcType < c->typeLen;
    }
    if (dstType >= 0 && (uint32_t)dstType < c->typeLen && c->types[dstType].kind == H2TCType_ALIAS)
    {
        /* Alias types are nominal: only exact alias matches assign implicitly.
         * Widening is one-way (Alias -> Target), handled via srcType alias path. */
        return 0;
    }
    if (srcType == c->typeUntypedInt && H2TCIsIntegerType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedFloat && H2TCIsFloatType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedInt && H2TCIsFloatType(c, dstType)) {
        return 1;
    }

    if (dstType < 0 || (uint32_t)dstType >= c->typeLen || srcType < 0
        || (uint32_t)srcType >= c->typeLen)
    {
        return 0;
    }

    if (c->types[srcType].kind == H2TCType_ALIAS) {
        int32_t srcBaseType = H2TCResolveAliasBaseType(c, srcType);
        if (srcBaseType < 0 || srcBaseType == srcType) {
            return 0;
        }
        return H2TCCanAssign(c, dstType, srcBaseType);
    }

    dst = &c->types[dstType];
    src = &c->types[srcType];

    if (dst->kind == H2TCType_PACK) {
        uint32_t i;
        if (src->kind != H2TCType_PACK || dst->fieldCount != src->fieldCount) {
            return 0;
        }
        for (i = 0; i < dst->fieldCount; i++) {
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (!H2TCCanAssign(c, dstElem, srcElem)) {
                return 0;
            }
        }
        return 1;
    }

    if (dst->kind == H2TCType_TUPLE) {
        uint32_t i;
        if (src->kind != H2TCType_TUPLE || dst->fieldCount != src->fieldCount) {
            return 0;
        }
        for (i = 0; i < dst->fieldCount; i++) {
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (!H2TCCanAssign(c, dstElem, srcElem)) {
                return 0;
            }
        }
        return 1;
    }

    if (dst->kind == H2TCType_ARRAY) {
        return src->kind == H2TCType_ARRAY && dst->baseType == src->baseType
            && src->arrayLen <= dst->arrayLen;
    }

    if (src->kind == H2TCType_NAMED && H2TCIsTypeDerivedFromEmbedded(c, srcType, dstType)) {
        return 1;
    }

    if (dst->kind == H2TCType_REF) {
        if (src->kind == H2TCType_REF && src->baseType == c->typeStr
            && H2TCTypeIsU8Slice(c, dst->baseType, 0))
        {
            return 1;
        }
        if (src->kind == H2TCType_PTR && src->baseType == c->typeStr
            && H2TCTypeIsU8Slice(c, dst->baseType, 0))
        {
            return 1;
        }
        if (src->kind == H2TCType_REF && H2TCCanAssign(c, dst->baseType, src->baseType)) {
            return !H2TCTypeIsMutable(dst) || H2TCTypeIsMutable(src);
        }
        if (src->kind == H2TCType_PTR && H2TCCanAssign(c, dst->baseType, src->baseType)) {
            return 1;
        }
        if (src->kind == H2TCType_ARRAY && H2TCCanAssign(c, dst->baseType, srcType)) {
            return 1;
        }
        return 0;
    }

    if (dst->kind == H2TCType_PTR) {
        /* Owned pointers (*T) can only come from new; references (&T) cannot be
         * implicitly promoted to owned pointers. */
        if (src->kind == H2TCType_ARRAY && dst->baseType >= 0
            && (uint32_t)dst->baseType < c->typeLen)
        {
            const H2TCType* dstBase = &c->types[dst->baseType];
            if (dstBase->kind == H2TCType_SLICE && dstBase->baseType == src->baseType) {
                return 1;
            }
        }
        if (src->kind == H2TCType_PTR && dst->baseType >= 0 && src->baseType >= 0
            && (uint32_t)dst->baseType < c->typeLen && (uint32_t)src->baseType < c->typeLen)
        {
            const H2TCType* dstBase = &c->types[dst->baseType];
            const H2TCType* srcBase = &c->types[src->baseType];
            int32_t         dstBaseResolved = H2TCResolveAliasBaseType(c, dst->baseType);
            int32_t         srcBaseResolved = H2TCResolveAliasBaseType(c, src->baseType);
            if (dstBaseResolved >= 0 && srcBaseResolved >= 0 && dstBaseResolved == srcBaseResolved)
            {
                return 1;
            }
            if (dstBaseResolved >= 0 && srcBaseResolved >= 0
                && H2TCIsTypeDerivedFromEmbedded(c, srcBaseResolved, dstBaseResolved))
            {
                return 1;
            }
            if (src->baseType == c->typeStr && H2TCTypeIsU8Slice(c, dst->baseType, 1)) {
                return 1;
            }
            if (dstBase->kind == H2TCType_SLICE) {
                if (srcBase->kind == H2TCType_SLICE && dstBase->baseType == srcBase->baseType) {
                    return !H2TCTypeIsMutable(dstBase) || H2TCTypeIsMutable(srcBase);
                }
                if (srcBase->kind == H2TCType_ARRAY && dstBase->baseType == srcBase->baseType) {
                    return 1;
                }
            }
        }
        return 0;
    }

    if (dst->kind == H2TCType_SLICE) {
        if (srcType == c->typeStr && H2TCTypeIsU8Slice(c, dstType, H2TCTypeIsMutable(dst))) {
            return 1;
        }
        if (src->kind == H2TCType_SLICE && dst->baseType == src->baseType) {
            return !H2TCTypeIsMutable(dst) || H2TCTypeIsMutable(src);
        }
        if (src->kind == H2TCType_ARRAY && dst->baseType == src->baseType) {
            return 1;
        }
        if (src->kind == H2TCType_PTR) {
            int32_t pointee = src->baseType;
            if (pointee >= 0 && (uint32_t)pointee < c->typeLen) {
                const H2TCType* p = &c->types[pointee];
                if (p->kind == H2TCType_ARRAY && p->baseType == dst->baseType) {
                    return 1;
                }
            }
        }
        return 0;
    }

    if (dst->kind == H2TCType_OPTIONAL) {
        /* null can be assigned to ?T */
        if (src->kind == H2TCType_NULL) {
            return 1;
        }
        /* T can be assigned to ?T (implicit lift through base assignability) */
        if (src->kind != H2TCType_OPTIONAL && H2TCCanAssign(c, dst->baseType, srcType)) {
            return 1;
        }
        /* ?T can be assigned to ?T (also handles mutable sub-type coercions) */
        if (src->kind == H2TCType_OPTIONAL) {
            return H2TCCanAssign(c, dst->baseType, src->baseType);
        }
        return 0;
    }

    if (H2TCIsRawptrType(c, dstType)) {
        return src->kind == H2TCType_NULL || H2TCIsRawptrType(c, srcType);
    }

    /* null can only be assigned to ?T, not to plain types */
    if (src->kind == H2TCType_NULL) {
        return 0;
    }

    return 0;
}

int H2TCCoerceForBinary(H2TypeCheckCtx* c, int32_t leftType, int32_t rightType, int32_t* outType) {
    if (leftType == rightType) {
        *outType = leftType;
        return 0;
    }
    if (H2TCIsUntyped(c, leftType) && !H2TCIsUntyped(c, rightType)
        && H2TCCanAssign(c, rightType, leftType))
    {
        *outType = rightType;
        return 0;
    }
    if (H2TCIsUntyped(c, rightType) && !H2TCIsUntyped(c, leftType)
        && H2TCCanAssign(c, leftType, rightType))
    {
        *outType = leftType;
        return 0;
    }
    if (leftType == c->typeUntypedInt && rightType == c->typeUntypedFloat) {
        *outType = c->typeUntypedFloat;
        return 0;
    }
    if (leftType == c->typeUntypedFloat && rightType == c->typeUntypedInt) {
        *outType = c->typeUntypedFloat;
        return 0;
    }
    return -1;
}

int H2TCConversionCost(H2TypeCheckCtx* c, int32_t dstType, int32_t srcType, uint8_t* outCost) {
    const H2TCType* dst;
    const H2TCType* src;

    if (!H2TCCanAssign(c, dstType, srcType)) {
        return -1;
    }
    if (dstType == srcType) {
        *outCost = 0;
        return 0;
    }
    if (dstType == c->typeAnytype) {
        *outCost = 8;
        return 0;
    }
    if (H2TCTypeIsFmtValue(c, dstType)) {
        *outCost = 5;
        return 0;
    }
    if (srcType == c->typeUntypedInt || srcType == c->typeUntypedFloat) {
        *outCost = 3;
        return 0;
    }
    if (srcType >= 0 && (uint32_t)srcType < c->typeLen && c->types[srcType].kind == H2TCType_ALIAS)
    {
        int32_t srcBaseType = H2TCResolveAliasBaseType(c, srcType);
        if (srcBaseType < 0 || srcBaseType == srcType) {
            return -1;
        }
        if (dstType == srcBaseType) {
            *outCost = 1;
            return 0;
        }
        if (H2TCConversionCost(c, dstType, srcBaseType, outCost) == 0) {
            if (*outCost < 255u) {
                *outCost = (uint8_t)(*outCost + 1u);
            }
            return 0;
        }
        return -1;
    }
    if (dstType < 0 || (uint32_t)dstType >= c->typeLen || srcType < 0
        || (uint32_t)srcType >= c->typeLen)
    {
        *outCost = 1;
        return 0;
    }
    dst = &c->types[dstType];
    src = &c->types[srcType];

    if (dst->kind == H2TCType_TUPLE && src->kind == H2TCType_TUPLE
        && dst->fieldCount == src->fieldCount)
    {
        uint32_t total = 0;
        uint32_t i;
        for (i = 0; i < dst->fieldCount; i++) {
            uint8_t elemCost = 0;
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (H2TCConversionCost(c, dstElem, srcElem, &elemCost) != 0) {
                return -1;
            }
            total += elemCost;
        }
        *outCost = total > 255u ? 255u : (uint8_t)total;
        return 0;
    }

    if (dst->kind == H2TCType_PACK && src->kind == H2TCType_PACK
        && dst->fieldCount == src->fieldCount)
    {
        uint32_t total = 0;
        uint32_t i;
        for (i = 0; i < dst->fieldCount; i++) {
            uint8_t elemCost = 0;
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (H2TCConversionCost(c, dstElem, srcElem, &elemCost) != 0) {
                return -1;
            }
            total += elemCost;
        }
        *outCost = total > 255u ? 255u : (uint8_t)total;
        return 0;
    }

    if (dst->kind == H2TCType_OPTIONAL && src->kind != H2TCType_OPTIONAL) {
        *outCost = 4;
        return 0;
    }

    if (dst->kind == H2TCType_OPTIONAL && src->kind == H2TCType_OPTIONAL) {
        return H2TCConversionCost(c, dst->baseType, src->baseType, outCost);
    }

    if (H2TCIsRawptrType(c, dstType) && H2TCIsRawptrType(c, srcType)) {
        *outCost = 0;
        return 0;
    }

    if (dst->kind == H2TCType_REF && src->kind == H2TCType_REF) {
        int      sameBase = dst->baseType == src->baseType;
        uint32_t upcastDistance = 0;
        int      upcastBase =
            H2TCEmbedDistanceToType(c, src->baseType, dst->baseType, &upcastDistance) == 0;
        if (upcastBase && !sameBase) {
            *outCost = (uint8_t)(2u + (upcastDistance > 0 ? (upcastDistance - 1u) : 0u));
            return 0;
        }
        if (!H2TCTypeIsMutable(dst) && H2TCTypeIsMutable(src) && sameBase) {
            *outCost = 1;
            return 0;
        }
    }

    if (dst->kind == H2TCType_PTR && src->kind == H2TCType_PTR) {
        int      sameBase = dst->baseType == src->baseType;
        uint32_t upcastDistance = 0;
        int      upcastBase =
            H2TCEmbedDistanceToType(c, src->baseType, dst->baseType, &upcastDistance) == 0;
        if (upcastBase && !sameBase) {
            *outCost = (uint8_t)(2u + (upcastDistance > 0 ? (upcastDistance - 1u) : 0u));
            return 0;
        }
    }

    if (dst->kind == H2TCType_SLICE && src->kind == H2TCType_SLICE && dst->baseType == src->baseType
        && !H2TCTypeIsMutable(dst) && H2TCTypeIsMutable(src))
    {
        *outCost = 1;
        return 0;
    }

    if (src->kind == H2TCType_NAMED && H2TCIsTypeDerivedFromEmbedded(c, srcType, dstType)) {
        uint32_t distance = 0;
        if (H2TCEmbedDistanceToType(c, srcType, dstType, &distance) == 0) {
            *outCost = (uint8_t)(2u + (distance > 0 ? (distance - 1u) : 0u));
        } else {
            *outCost = 2;
        }
        return 0;
    }

    *outCost = 1;
    return 0;
}

int H2TCCostVectorCompare(const uint8_t* a, const uint8_t* b, uint32_t len) {
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

int32_t H2TCUnwrapCallArgExprNode(H2TypeCheckCtx* c, int32_t argNode) {
    const H2AstNode* n;
    if (argNode < 0 || (uint32_t)argNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[argNode];
    if (n->kind == H2Ast_CALL_ARG) {
        return H2AstFirstChild(c->ast, argNode);
    }
    return argNode;
}

int H2TCCollectCallArgInfo(
    H2TypeCheckCtx*  c,
    int32_t          callNode,
    int32_t          calleeNode,
    int              includeReceiver,
    int32_t          receiverNode,
    H2TCCallArgInfo* outArgs,
    int32_t* _Nullable outArgTypes,
    uint32_t* outArgCount) {
    uint32_t argCount = 0;
    int32_t  argNode = H2AstNextSibling(c->ast, calleeNode);
    if (includeReceiver) {
        const H2AstNode* recvNode;
        if (receiverNode < 0 || (uint32_t)receiverNode >= c->ast->len) {
            return H2TCFailNode(c, callNode, H2Diag_EXPECTED_EXPR);
        }
        if (argCount >= H2TC_MAX_CALL_ARGS) {
            return H2TCFailNode(c, callNode, H2Diag_ARENA_OOM);
        }
        recvNode = &c->ast->nodes[receiverNode];
        outArgs[argCount] = (H2TCCallArgInfo){
            .argNode = receiverNode,
            .exprNode = receiverNode,
            .start = recvNode->start,
            .end = recvNode->end,
            .explicitNameStart = 0,
            .explicitNameEnd = 0,
            .implicitNameStart = 0,
            .implicitNameEnd = 0,
            .spread = 0,
            ._reserved = { 0, 0, 0 },
        };
        if (outArgTypes != NULL && H2TCTypeExpr(c, receiverNode, &outArgTypes[argCount]) != 0) {
            return -1;
        }
        argCount++;
    }
    while (argNode >= 0) {
        const H2AstNode* arg = &c->ast->nodes[argNode];
        int32_t          exprNode = H2TCUnwrapCallArgExprNode(c, argNode);
        if (argCount >= H2TC_MAX_CALL_ARGS) {
            return H2TCFailNode(c, callNode, H2Diag_ARENA_OOM);
        }
        if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
            return H2TCFailNode(c, argNode, H2Diag_EXPECTED_EXPR);
        }
        outArgs[argCount] = (H2TCCallArgInfo){
            .argNode = argNode,
            .exprNode = exprNode,
            .start = arg->start,
            .end = arg->end,
            .explicitNameStart = arg->dataStart,
            .explicitNameEnd = arg->dataEnd,
            .implicitNameStart = 0,
            .implicitNameEnd = 0,
            .spread = (uint8_t)(((arg->flags & H2AstFlag_CALL_ARG_SPREAD) != 0) ? 1 : 0),
            ._reserved = { 0, 0, 0 },
        };
        if ((arg->kind == H2Ast_CALL_ARG || arg->kind == H2Ast_IDENT)
            && !(outArgs[argCount].explicitNameEnd > outArgs[argCount].explicitNameStart)
            && c->ast->nodes[exprNode].kind == H2Ast_IDENT)
        {
            uint32_t nameStart = c->ast->nodes[exprNode].dataStart;
            uint32_t nameEnd = c->ast->nodes[exprNode].dataEnd;
            if (H2TCResolveTypeValueName(c, nameStart, nameEnd) < 0) {
                outArgs[argCount].implicitNameStart = nameStart;
                outArgs[argCount].implicitNameEnd = nameEnd;
            }
        }
        if (outArgTypes != NULL && H2TCTypeExpr(c, exprNode, &outArgTypes[argCount]) != 0) {
            return -1;
        }
        argCount++;
        argNode = H2AstNextSibling(c->ast, argNode);
    }
    *outArgCount = argCount;
    return 0;
}

int H2TCIsMainFunction(H2TypeCheckCtx* c, const H2TCFunction* fn) {
    return H2NameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "main");
}

int32_t H2TCResolveImplicitMainContextType(H2TypeCheckCtx* c) {
    uint32_t i;
    int32_t  typeId = H2TCFindNamedTypeByLiteral(c, "builtin__Context");
    if (typeId >= 0) {
        return typeId;
    }
    for (i = 0; i < c->typeLen; i++) {
        const H2TCType* t = &c->types[i];
        if (t->kind != H2TCType_NAMED) {
            continue;
        }
        if (H2NameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            && H2NameHasSuffix(c->src, t->nameStart, t->nameEnd, "__Context"))
        {
            return (int32_t)i;
        }
    }
    typeId = H2TCFindNamedTypeByLiteral(c, "Context");
    if (typeId >= 0) {
        return typeId;
    }
    return -1;
}

int H2TCCurrentContextFieldType(
    H2TypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType) {
    if (c->currentContextType >= 0) {
        if (H2TCFieldLookup(c, c->currentContextType, fieldStart, fieldEnd, outType, NULL) == 0) {
            return 0;
        }
        return -1;
    }
    if (c->hasImplicitMainRootContext) {
        if (c->implicitMainContextType >= 0) {
            if (H2TCFieldLookup(c, c->implicitMainContextType, fieldStart, fieldEnd, outType, NULL)
                == 0)
            {
                return 0;
            }
            return -1;
        }
        if (H2NameEqLiteral(c->src, fieldStart, fieldEnd, "allocator")) {
            int32_t t = H2TCFindMemAllocatorType(c);
            if (t < 0) {
                return -1;
            }
            *outType = t;
            return 0;
        }
        if (H2NameEqLiteral(c->src, fieldStart, fieldEnd, "logger")) {
            int32_t t = H2TCFindNamedTypeByLiteral(c, "builtin__Logger");
            if (t < 0) {
                t = H2TCFindNamedTypeByLiteral(c, "Logger");
            }
            if (t < 0) {
                t = c->typeStr;
                if (t < 0) {
                    return -1;
                }
            }
            *outType = t;
            return 0;
        }
    }
    return -1;
}

int H2TCCurrentContextFieldTypeByLiteral(
    H2TypeCheckCtx* c, const char* fieldName, int32_t* outType) {
    if (c->currentContextType >= 0) {
        int32_t  typeId = c->currentContextType;
        uint32_t i;
        if (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == H2TCType_ALIAS)
        {
            if (H2TCResolveAliasTypeId(c, typeId) != 0) {
                return -1;
            }
            typeId = c->types[typeId].baseType;
        }
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen
            || (c->types[typeId].kind != H2TCType_NAMED
                && c->types[typeId].kind != H2TCType_ANON_STRUCT
                && c->types[typeId].kind != H2TCType_ANON_UNION))
        {
            return -1;
        }
        if (c->types[typeId].kind == H2TCType_NAMED
            && H2TCEnsureNamedTypeFieldsResolved(c, typeId) != 0)
        {
            return -1;
        }
        for (i = 0; i < c->types[typeId].fieldCount; i++) {
            uint32_t idx = c->types[typeId].fieldStart + i;
            if (H2NameEqLiteral(
                    c->src, c->fields[idx].nameStart, c->fields[idx].nameEnd, fieldName))
            {
                *outType = c->fields[idx].typeId;
                return 0;
            }
        }
        return -1;
    }
    if (c->hasImplicitMainRootContext) {
        if (c->implicitMainContextType >= 0) {
            int32_t  typeId = c->implicitMainContextType;
            uint32_t i;
            while (typeId >= 0 && (uint32_t)typeId < c->typeLen
                   && c->types[typeId].kind == H2TCType_ALIAS)
            {
                if (H2TCResolveAliasTypeId(c, typeId) != 0) {
                    return -1;
                }
                typeId = c->types[typeId].baseType;
            }
            if (typeId < 0 || (uint32_t)typeId >= c->typeLen
                || (c->types[typeId].kind != H2TCType_NAMED
                    && c->types[typeId].kind != H2TCType_ANON_STRUCT
                    && c->types[typeId].kind != H2TCType_ANON_UNION))
            {
                return -1;
            }
            if (c->types[typeId].kind == H2TCType_NAMED
                && H2TCEnsureNamedTypeFieldsResolved(c, typeId) != 0)
            {
                return -1;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t idx = c->types[typeId].fieldStart + i;
                if (H2NameEqLiteral(
                        c->src, c->fields[idx].nameStart, c->fields[idx].nameEnd, fieldName))
                {
                    *outType = c->fields[idx].typeId;
                    return 0;
                }
            }
            return -1;
        }
        if (H2TCStrEqNullable(fieldName, "allocator")) {
            int32_t t = H2TCFindMemAllocatorType(c);
            if (t < 0) {
                return -1;
            }
            *outType = t;
            return 0;
        }
        if (H2TCStrEqNullable(fieldName, "logger")) {
            int32_t t = H2TCFindNamedTypeByLiteral(c, "builtin__Logger");
            if (t < 0) {
                t = H2TCFindNamedTypeByLiteral(c, "Logger");
            }
            if (t < 0) {
                t = c->typeStr;
                if (t < 0) {
                    return -1;
                }
            }
            *outType = t;
            return 0;
        }
    }
    return -1;
}

int32_t H2TCContextFindOverlayNode(H2TypeCheckCtx* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast->len) {
        return -1;
    }
    {
        int32_t callNode = H2AstFirstChild(c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? H2AstNextSibling(c->ast, callNode) : -1;
        if (child >= 0 && c->ast->nodes[child].kind == H2Ast_CONTEXT_OVERLAY) {
            return child;
        }
    }
    return -1;
}

int32_t H2TCContextFindDirectNode(H2TypeCheckCtx* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast->len) {
        return -1;
    }
    {
        int32_t callNode = H2AstFirstChild(c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? H2AstNextSibling(c->ast, callNode) : -1;
        if (child >= 0 && c->ast->nodes[child].kind != H2Ast_CONTEXT_OVERLAY) {
            return child;
        }
    }
    return -1;
}

int32_t H2TCContextFindOverlayBindMatch(
    H2TypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, const char* _Nullable fieldName) {
    int32_t overlayNode = H2TCContextFindOverlayNode(c);
    int32_t child = overlayNode >= 0 ? H2AstFirstChild(c->ast, overlayNode) : -1;
    while (child >= 0) {
        const H2AstNode* bind = &c->ast->nodes[child];
        if (bind->kind == H2Ast_CONTEXT_BIND) {
            int matches =
                fieldName != NULL
                    ? H2NameEqLiteral(c->src, bind->dataStart, bind->dataEnd, fieldName)
                    : H2NameEqSlice(c->src, bind->dataStart, bind->dataEnd, fieldStart, fieldEnd);
            if (matches) {
                return child;
            }
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return -1;
}

int32_t H2TCContextFindOverlayBindByLiteral(H2TypeCheckCtx* c, const char* fieldName) {
    return H2TCContextFindOverlayBindMatch(c, 0, 0, fieldName);
}

int H2TCGetEffectiveContextFieldType(
    H2TypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType) {
    int32_t bindNode;
    if (c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return H2TCFailSpan(c, H2Diag_CONTEXT_REQUIRED, fieldStart, fieldEnd);
    }
    bindNode = H2TCContextFindOverlayBindMatch(c, fieldStart, fieldEnd, NULL);
    if (bindNode >= 0) {
        int32_t exprNode = H2AstFirstChild(c->ast, bindNode);
        if (exprNode >= 0) {
            return H2TCTypeExpr(c, exprNode, outType);
        }
    }
    if (H2TCCurrentContextFieldType(c, fieldStart, fieldEnd, outType) != 0) {
        return H2TCFailSpan(c, H2Diag_CONTEXT_MISSING_FIELD, fieldStart, fieldEnd);
    }
    return 0;
}

int H2TCGetEffectiveContextFieldTypeByLiteral(
    H2TypeCheckCtx* c, const char* fieldName, int32_t* outType) {
    int32_t bindNode;
    if (c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return H2TCFailSpan(c, H2Diag_CONTEXT_REQUIRED, 0, 0);
    }
    bindNode = H2TCContextFindOverlayBindByLiteral(c, fieldName);
    if (bindNode >= 0) {
        int32_t exprNode = H2AstFirstChild(c->ast, bindNode);
        if (exprNode >= 0) {
            return H2TCTypeExpr(c, exprNode, outType);
        }
    }
    if (H2TCCurrentContextFieldTypeByLiteral(c, fieldName, outType) != 0) {
        return H2TCFailSpan(c, H2Diag_CONTEXT_MISSING_FIELD, 0, 0);
    }
    return 0;
}

int H2TCValidateCurrentCallOverlay(H2TypeCheckCtx* c) {
    int32_t overlayNode = H2TCContextFindOverlayNode(c);
    int32_t directNode = H2TCContextFindDirectNode(c);
    int32_t bind = overlayNode >= 0 ? H2AstFirstChild(c->ast, overlayNode) : -1;
    if (directNode >= 0) {
        int32_t t;
        int32_t savedActive = c->activeCallWithNode;
        c->activeCallWithNode = -1;
        if (H2TCTypeExpr(c, directNode, &t) != 0) {
            c->activeCallWithNode = savedActive;
            return -1;
        }
        c->activeCallWithNode = savedActive;
        return 0;
    }
    if (overlayNode >= 0 && c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return H2TCFailSpan(
            c,
            H2Diag_CONTEXT_REQUIRED,
            c->ast->nodes[overlayNode].start,
            c->ast->nodes[overlayNode].end);
    }
    while (bind >= 0) {
        const H2AstNode* b = &c->ast->nodes[bind];
        int32_t          expectedType;
        int32_t          exprNode;
        int32_t          t;
        int32_t          scan;
        if (b->kind != H2Ast_CONTEXT_BIND) {
            return H2TCFailNode(c, bind, H2Diag_UNEXPECTED_TOKEN);
        }
        if (H2TCCurrentContextFieldType(c, b->dataStart, b->dataEnd, &expectedType) != 0) {
            return H2TCFailSpan(c, H2Diag_CONTEXT_UNKNOWN_FIELD, b->dataStart, b->dataEnd);
        }
        scan = H2AstFirstChild(c->ast, overlayNode);
        while (scan >= 0) {
            const H2AstNode* bs = &c->ast->nodes[scan];
            if (scan != bind && bs->kind == H2Ast_CONTEXT_BIND
                && H2NameEqSlice(c->src, bs->dataStart, bs->dataEnd, b->dataStart, b->dataEnd))
            {
                return H2TCFailSpan(c, H2Diag_CONTEXT_DUPLICATE_FIELD, b->dataStart, b->dataEnd);
            }
            scan = H2AstNextSibling(c->ast, scan);
        }
        exprNode = H2AstFirstChild(c->ast, bind);
        if (exprNode >= 0) {
            int32_t savedActive = c->activeCallWithNode;
            c->activeCallWithNode = -1;
            if (H2TCTypeExpr(c, exprNode, &t) != 0) {
                c->activeCallWithNode = savedActive;
                return -1;
            }
            c->activeCallWithNode = savedActive;
            if (!H2TCCanAssign(c, expectedType, t)) {
                return H2TCFailNode(c, exprNode, H2Diag_CONTEXT_TYPE_MISMATCH);
            }
        }
        bind = H2AstNextSibling(c->ast, bind);
    }
    return 0;
}

int H2TCValidateCallContextRequirements(H2TypeCheckCtx* c, int32_t requiredContextType) {
    int32_t  typeId = requiredContextType;
    int32_t  directNode = H2TCContextFindDirectNode(c);
    uint32_t i;
    if (requiredContextType < 0) {
        return 0;
    }
    if (directNode >= 0) {
        int32_t expectedContextRef = H2TCInternRefType(
            c,
            requiredContextType,
            1,
            c->ast->nodes[directNode].start,
            c->ast->nodes[directNode].end);
        int32_t gotType;
        int32_t savedActive = c->activeCallWithNode;
        if (expectedContextRef < 0) {
            return -1;
        }
        c->activeCallWithNode = -1;
        if (H2TCTypeExpr(c, directNode, &gotType) != 0) {
            c->activeCallWithNode = savedActive;
            return -1;
        }
        c->activeCallWithNode = savedActive;
        if (!H2TCCanAssign(c, expectedContextRef, gotType)) {
            return H2TCFailNode(c, directNode, H2Diag_CONTEXT_CLAUSE_MISMATCH);
        }
        return 0;
    }
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == H2TCType_ALIAS)
    {
        if (H2TCResolveAliasTypeId(c, typeId) != 0) {
            return -1;
        }
        typeId = c->types[typeId].baseType;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen
        || (c->types[typeId].kind != H2TCType_NAMED
            && c->types[typeId].kind != H2TCType_ANON_STRUCT))
    {
        return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, 0, 0);
    }
    if (c->types[typeId].kind == H2TCType_NAMED
        && H2TCEnsureNamedTypeFieldsResolved(c, typeId) != 0)
    {
        return -1;
    }
    for (i = 0; i < c->types[typeId].fieldCount; i++) {
        uint32_t        fieldIdx = c->types[typeId].fieldStart + i;
        const H2TCField field = c->fields[fieldIdx];
        int32_t         gotType = -1;
        if (field.nameEnd <= field.nameStart) {
            continue;
        }
        if (H2TCGetEffectiveContextFieldType(c, field.nameStart, field.nameEnd, &gotType) != 0) {
            return -1;
        }
        if (!H2TCCanAssign(c, field.typeId, gotType)) {
            return H2TCFailSpan(c, H2Diag_CONTEXT_TYPE_MISMATCH, field.nameStart, field.nameEnd);
        }
    }
    return 0;
}

int H2TCGetFunctionTypeSignature(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    int32_t*        outReturnType,
    uint32_t*       outParamStart,
    uint32_t*       outParamCount,
    int* _Nullable outIsVariadic) {
    const H2TCType* t;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return -1;
    }
    t = &c->types[typeId];
    if (t->kind != H2TCType_FUNCTION) {
        return -1;
    }
    *outReturnType = t->baseType;
    *outParamStart = t->fieldStart;
    *outParamCount = t->fieldCount;
    if (outIsVariadic != NULL) {
        *outIsVariadic = (t->flags & H2TCTypeFlag_FUNCTION_VARIADIC) != 0;
    }
    return 0;
}

void H2TCCallMapErrorClear(H2TCCallMapError* err) {
    err->code = 0;
    err->start = 0;
    err->end = 0;
    err->argStart = 0;
    err->argEnd = 0;
}

static int H2TCParamNameStartsWithUnderscore(
    H2TypeCheckCtx* c,
    const uint32_t* paramNameStarts,
    const uint32_t* paramNameEnds,
    uint32_t        paramIndex) {
    uint32_t start;
    uint32_t end;
    if (c == NULL || c->src.ptr == NULL || paramNameStarts == NULL || paramNameEnds == NULL) {
        return 0;
    }
    start = paramNameStarts[paramIndex];
    end = paramNameEnds[paramIndex];
    return end > start && c->src.ptr[start] == '_';
}

static uint32_t H2TCPositionalCallPrefixEnd(
    H2TypeCheckCtx* c,
    const uint32_t* paramNameStarts,
    const uint32_t* paramNameEnds,
    uint32_t        paramCount,
    uint32_t        firstPositionalArgIndex) {
    uint32_t prefixEnd;
    if (firstPositionalArgIndex >= paramCount) {
        return paramCount;
    }
    prefixEnd = firstPositionalArgIndex + 1u;
    while (prefixEnd < paramCount
           && H2TCParamNameStartsWithUnderscore(c, paramNameStarts, paramNameEnds, prefixEnd))
    {
        prefixEnd++;
    }
    return prefixEnd;
}

int H2TCMapCallArgsToParams(
    H2TypeCheckCtx*        c,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    uint32_t               paramCount,
    uint32_t               firstPositionalArgIndex,
    int32_t*               outMappedArgExprNodes,
    H2TCCallMapError* _Nullable outError) {
    uint8_t  paramAssigned[H2TC_MAX_CALL_ARGS];
    uint32_t positionalPrefixEnd;
    uint32_t i;
    if (paramCount > H2TC_MAX_CALL_ARGS || argCount > paramCount) {
        return -1;
    }
    memset(paramAssigned, 0, sizeof(paramAssigned));
    for (i = 0; i < paramCount; i++) {
        outMappedArgExprNodes[i] = -1;
    }
    positionalPrefixEnd = H2TCPositionalCallPrefixEnd(
        c, paramNameStarts, paramNameEnds, paramCount, firstPositionalArgIndex);

    if (firstPositionalArgIndex < argCount) {
        const H2TCCallArgInfo* a = &callArgs[firstPositionalArgIndex];
        outMappedArgExprNodes[firstPositionalArgIndex] = a->exprNode;
        paramAssigned[firstPositionalArgIndex] = 1;
        if (a->explicitNameEnd > a->explicitNameStart
            && !H2NameEqSlice(
                c->src,
                a->explicitNameStart,
                a->explicitNameEnd,
                paramNameStarts[firstPositionalArgIndex],
                paramNameEnds[firstPositionalArgIndex]))
        {
            if (outError != NULL) {
                outError->code = H2Diag_CALL_FIRST_ARG_NAME_MISMATCH;
                outError->start = a->start;
                outError->end = a->end;
                outError->argStart = paramNameStarts[firstPositionalArgIndex];
                outError->argEnd = paramNameEnds[firstPositionalArgIndex];
            }
            return 1;
        }
    }

    for (i = 0; i < argCount; i++) {
        const H2TCCallArgInfo* a = &callArgs[i];
        uint32_t               nameStart = 0;
        uint32_t               nameEnd = 0;
        uint32_t               p;
        int                    foundName = 0;
        if (i < firstPositionalArgIndex) {
            outMappedArgExprNodes[i] = a->exprNode;
            paramAssigned[i] = 1;
            continue;
        }
        if (i == firstPositionalArgIndex) {
            continue;
        }
        if (a->explicitNameEnd > a->explicitNameStart) {
            nameStart = a->explicitNameStart;
            nameEnd = a->explicitNameEnd;
            foundName = 1;
        } else if (i < positionalPrefixEnd) {
            outMappedArgExprNodes[i] = a->exprNode;
            paramAssigned[i] = 1;
            continue;
        } else if (a->implicitNameEnd > a->implicitNameStart) {
            nameStart = a->implicitNameStart;
            nameEnd = a->implicitNameEnd;
            foundName = 1;
        }
        if (!foundName) {
            if (outError != NULL) {
                outError->start = a->start;
                outError->end = a->end;
                if (positionalPrefixEnd == firstPositionalArgIndex + 1u) {
                    outError->code = H2Diag_CALL_ARG_NAME_REQUIRED;
                    outError->argStart = 0;
                    outError->argEnd = 0;
                } else if (positionalPrefixEnd < paramCount) {
                    outError->code = H2Diag_CALL_ARG_NAME_REQUIRED_AFTER_PARAM;
                    outError->argStart = paramNameStarts[positionalPrefixEnd];
                    outError->argEnd = paramNameEnds[positionalPrefixEnd];
                } else {
                    outError->code = H2Diag_CALL_ARG_NAME_REQUIRED_AFTER_PARAM;
                    outError->argStart = 0;
                    outError->argEnd = 0;
                }
            }
            return 1;
        }

        for (p = firstPositionalArgIndex + 1u; p < paramCount; p++) {
            if (H2NameEqSlice(c->src, nameStart, nameEnd, paramNameStarts[p], paramNameEnds[p])) {
                if (paramAssigned[p]) {
                    if (outError != NULL) {
                        outError->code = H2Diag_CALL_ARG_DUPLICATE;
                        outError->start = a->start;
                        outError->end = a->end;
                        outError->argStart = nameStart;
                        outError->argEnd = nameEnd;
                    }
                    return 1;
                }
                outMappedArgExprNodes[p] = a->exprNode;
                paramAssigned[p] = 1;
                break;
            }
        }
        if (p == paramCount) {
            if (outError != NULL) {
                outError->code = H2Diag_CALL_ARG_UNKNOWN_NAME;
                outError->start = a->start;
                outError->end = a->end;
                outError->argStart = nameStart;
                outError->argEnd = nameEnd;
            }
            return 1;
        }
    }

    for (i = 0; i < paramCount; i++) {
        if (!paramAssigned[i]) {
            if (outError != NULL) {
                outError->code = H2Diag_CALL_ARG_MISSING;
                if (argCount > 0) {
                    outError->start = callArgs[0].start;
                    outError->end = callArgs[argCount - 1u].end;
                } else {
                    outError->start = paramNameStarts[i];
                    outError->end = paramNameEnds[i];
                }
                outError->argStart = paramNameStarts[i];
                outError->argEnd = paramNameEnds[i];
            }
            return 1;
        }
    }
    return 0;
}

int H2TCPrepareCallBinding(
    H2TypeCheckCtx*        c,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const int32_t*         paramTypes,
    uint32_t               paramCount,
    int                    isVariadic,
    int                    allowNamedMapping,
    uint32_t               firstPositionalArgIndex,
    H2TCCallBinding*       outBinding,
    H2TCCallMapError*      outError) {
    uint32_t spreadArgIndex = UINT32_MAX;
    uint32_t fixedCount = isVariadic ? (paramCount > 0 ? (paramCount - 1u) : 0u) : paramCount;
    uint32_t fixedInputCount;
    uint32_t i;
    if (outBinding == NULL) {
        return 1;
    }
    memset(outBinding, 0, sizeof(*outBinding));
    outBinding->isVariadic = isVariadic;
    outBinding->fixedCount = fixedCount;
    outBinding->fixedInputCount = 0;
    outBinding->spreadArgIndex = UINT32_MAX;
    outBinding->variadicParamType = -1;
    outBinding->variadicElemType = -1;
    for (i = 0; i < H2TC_MAX_CALL_ARGS; i++) {
        outBinding->fixedMappedArgExprNodes[i] = -1;
        outBinding->argParamIndices[i] = -1;
        outBinding->argExpectedTypes[i] = -1;
    }
    if (paramCount > H2TC_MAX_CALL_ARGS || argCount > H2TC_MAX_CALL_ARGS) {
        return 1;
    }

    for (i = 0; i < argCount; i++) {
        if (!callArgs[i].spread) {
            continue;
        }
        if (i + 1u < argCount) {
            if (outError != NULL) {
                outError->code = H2Diag_VARIADIC_SPREAD_NOT_LAST;
                outError->start = callArgs[i].start;
                outError->end = callArgs[i].end;
                outError->argStart = 0;
                outError->argEnd = 0;
            }
            return 2;
        }
        spreadArgIndex = i;
    }

    if (!isVariadic) {
        if (spreadArgIndex != UINT32_MAX) {
            if (outError != NULL) {
                outError->code = H2Diag_VARIADIC_CALL_SHAPE_MISMATCH;
                outError->start = callArgs[spreadArgIndex].start;
                outError->end = callArgs[spreadArgIndex].end;
                outError->argStart = 0;
                outError->argEnd = 0;
            }
            return 2;
        }
        if (argCount != paramCount) {
            return 1;
        }
    } else {
        int      variadicIsPack = 0;
        uint32_t packElemCount = 0;
        if (paramCount == 0) {
            return 1;
        }
        outBinding->variadicParamType = paramTypes[fixedCount];
        if (outBinding->variadicParamType < 0
            || (uint32_t)outBinding->variadicParamType >= c->typeLen)
        {
            return 1;
        }
        if (c->types[outBinding->variadicParamType].kind == H2TCType_SLICE) {
            outBinding->variadicElemType = c->types[outBinding->variadicParamType].baseType;
        } else if (c->types[outBinding->variadicParamType].kind == H2TCType_PACK) {
            variadicIsPack = 1;
            packElemCount = c->types[outBinding->variadicParamType].fieldCount;
        } else if (outBinding->variadicParamType == c->typeAnytype) {
            outBinding->variadicElemType = c->typeAnytype;
        } else {
            return 1;
        }
        if (spreadArgIndex != UINT32_MAX) {
            if (argCount != fixedCount + 1u) {
                if (outError != NULL) {
                    outError->code = H2Diag_VARIADIC_CALL_SHAPE_MISMATCH;
                    outError->start = callArgs[spreadArgIndex].start;
                    outError->end = callArgs[spreadArgIndex].end;
                    outError->argStart = 0;
                    outError->argEnd = 0;
                }
                return 2;
            }
            if (callArgs[spreadArgIndex].explicitNameEnd
                > callArgs[spreadArgIndex].explicitNameStart)
            {
                if (outError != NULL) {
                    outError->code = H2Diag_VARIADIC_CALL_SHAPE_MISMATCH;
                    outError->start = callArgs[spreadArgIndex].start;
                    outError->end = callArgs[spreadArgIndex].end;
                    outError->argStart = 0;
                    outError->argEnd = 0;
                }
                return 2;
            }
        } else if (argCount < fixedCount) {
            return 1;
        } else if (variadicIsPack && (argCount - fixedCount) != packElemCount) {
            return 1;
        }
    }

    fixedInputCount = isVariadic ? fixedCount : argCount;
    outBinding->fixedInputCount = fixedInputCount;
    outBinding->spreadArgIndex = spreadArgIndex;
    if (fixedCount > 0) {
        if (!allowNamedMapping) {
            if (fixedInputCount > fixedCount) {
                return 1;
            }
            for (i = 0; i < fixedInputCount; i++) {
                outBinding->fixedMappedArgExprNodes[i] = callArgs[i].exprNode;
            }
        } else {
            H2TCCallMapError mapError;
            H2TCCallMapErrorClear(&mapError);
            if (H2TCMapCallArgsToParams(
                    c,
                    callArgs,
                    fixedInputCount,
                    paramNameStarts,
                    paramNameEnds,
                    fixedCount,
                    firstPositionalArgIndex,
                    outBinding->fixedMappedArgExprNodes,
                    &mapError)
                != 0)
            {
                if (mapError.code != 0) {
                    if (outError != NULL) {
                        *outError = mapError;
                    }
                    return 2;
                }
                return 1;
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
            && outBinding->fixedMappedArgExprNodes[i] == callArgs[i].exprNode)
        {
            p = i;
        } else {
            for (j = 0; j < fixedCount; j++) {
                if (outBinding->fixedMappedArgExprNodes[j] == callArgs[i].exprNode) {
                    p = j;
                    break;
                }
            }
        }
        if (p == UINT32_MAX) {
            return 1;
        }
        outBinding->argParamIndices[i] = (int32_t)p;
        outBinding->argExpectedTypes[i] = paramTypes[p];
    }

    if (!isVariadic) {
        return 0;
    }

    if (spreadArgIndex != UINT32_MAX) {
        outBinding->argParamIndices[spreadArgIndex] = (int32_t)fixedCount;
        outBinding->argExpectedTypes[spreadArgIndex] = outBinding->variadicParamType;
        return 0;
    }

    for (i = fixedInputCount; i < argCount; i++) {
        const H2TCType* variadicType = &c->types[outBinding->variadicParamType];
        if (callArgs[i].explicitNameEnd > callArgs[i].explicitNameStart) {
            if (outError != NULL) {
                outError->code = H2Diag_VARIADIC_CALL_SHAPE_MISMATCH;
                outError->start = callArgs[i].start;
                outError->end = callArgs[i].end;
                outError->argStart = 0;
                outError->argEnd = 0;
            }
            return 2;
        }
        outBinding->argParamIndices[i] = (int32_t)fixedCount;
        if (variadicType->kind == H2TCType_PACK) {
            uint32_t packIndex = i - fixedInputCount;
            if (packIndex >= variadicType->fieldCount) {
                return 1;
            }
            outBinding->argExpectedTypes[i] =
                c->funcParamTypes[variadicType->fieldStart + packIndex];
        } else {
            outBinding->argExpectedTypes[i] = outBinding->variadicElemType;
        }
    }

    return 0;
}

int H2TCCheckConstParamArgs(
    H2TypeCheckCtx*        c,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    const H2TCCallBinding* binding,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const uint8_t*         paramFlags,
    uint32_t               paramCount,
    H2TCCallMapError*      outError) {
    uint32_t i;
    if (outError != NULL) {
        H2TCCallMapErrorClear(outError);
    }
    if (binding == NULL || paramFlags == NULL) {
        return 0;
    }
    for (i = 0; i < argCount; i++) {
        int32_t          p = binding->argParamIndices[i];
        int              isConst = 0;
        int              evalIsConst = 0;
        H2CTFEValue      ignoredValue = { 0 };
        H2TCConstEvalCtx evalCtx;
        H2DiagCode       code = H2Diag_CONST_PARAM_ARG_NOT_CONST;
        H2TCInitConstEvalCtxFromParent(c, c != NULL ? c->activeConstEvalCtx : NULL, &evalCtx);
        if (p < 0 || (uint32_t)p >= paramCount) {
            continue;
        }
        isConst = (paramFlags[p] & H2TCFuncParamFlag_CONST) != 0;
        if (!isConst) {
            continue;
        }
        if (binding->isVariadic && i == binding->spreadArgIndex
            && (uint32_t)p == binding->fixedCount)
        {
            code = H2Diag_CONST_PARAM_SPREAD_NOT_CONST;
        }
        if (H2TCEvalConstExprNode(&evalCtx, callArgs[i].exprNode, &ignoredValue, &evalIsConst) != 0)
        {
            return -1;
        }
        if (evalIsConst) {
            continue;
        }
        if (outError != NULL) {
            uint32_t forwardedStart = 0;
            uint32_t forwardedEnd = 0;
            outError->code = code;
            outError->start = callArgs[i].start;
            outError->end = callArgs[i].end;
            if (H2TCFindForwardedConstParamCallSpan(
                    c, callArgs[i].exprNode, &forwardedStart, &forwardedEnd))
            {
                outError->start = forwardedStart;
                outError->end = forwardedEnd;
            }
            if (paramNameEnds[p] > paramNameStarts[p]) {
                outError->argStart = paramNameStarts[p];
                outError->argEnd = paramNameEnds[p];
            } else {
                outError->argStart = callArgs[i].start;
                outError->argEnd = callArgs[i].end;
            }
        }
        return 1;
    }
    return 0;
}

int H2TCCheckConstBlocksForCall(
    H2TypeCheckCtx*        c,
    int32_t                fnIndex,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    const H2TCCallBinding* binding,
    H2TCCallMapError*      outError) {
    const H2TCFunction* fn;
    int32_t             fnNode;
    int32_t             bodyNode = -1;
    int32_t             child;
    uint32_t            paramIndex = 0;
    uint32_t            variadicPackParamNameStart = 0;
    uint32_t            variadicPackParamNameEnd = 0;
    int                 hasConstBlock = 0;
    uint32_t            savedLocalLen;
    uint32_t            savedLocalUseLen;
    uint32_t            savedVariantNarrowLen;
    H2TCConstEvalCtx*   savedActiveConstEvalCtx;
    H2CTFEExecBinding*  paramBindings = NULL;
    uint32_t            paramBindingLen = 0;
    H2CTFEExecEnv       paramFrame;
    H2CTFEExecCtx       execCtx;
    H2TCConstEvalCtx    evalCtx;
    H2CTFEValue         retValue;
    int                 didReturn = 0;
    int                 isConst = 0;
    int                 rc;

    if (outError != NULL) {
        H2TCCallMapErrorClear(outError);
    }
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return 0;
    }
    fn = &c->funcs[fnIndex];
    fnNode = fn->defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != H2Ast_FN) {
        return 0;
    }

    child = H2AstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == H2Ast_BLOCK) {
            bodyNode = child;
            break;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    if (bodyNode < 0) {
        return 0;
    }

    child = H2AstFirstChild(c->ast, bodyNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == H2Ast_CONST_BLOCK) {
            hasConstBlock = 1;
            break;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    if (!hasConstBlock) {
        return 0;
    }

    savedLocalLen = c->localLen;
    savedLocalUseLen = c->localUseLen;
    savedVariantNarrowLen = c->variantNarrowLen;
    savedActiveConstEvalCtx = c->activeConstEvalCtx;

    if (fn->paramCount > 0) {
        paramBindings = (H2CTFEExecBinding*)H2ArenaAlloc(
            c->arena,
            sizeof(H2CTFEExecBinding) * fn->paramCount,
            (uint32_t)_Alignof(H2CTFEExecBinding));
        if (paramBindings == NULL) {
            c->localLen = savedLocalLen;
            c->localUseLen = savedLocalUseLen;
            c->variantNarrowLen = savedVariantNarrowLen;
            c->activeConstEvalCtx = savedActiveConstEvalCtx;
            return H2TCFailNode(c, fnNode, H2Diag_ARENA_OOM);
        }
    }

    child = H2AstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
            int32_t paramType;
            int     isConstParam;
            int     addedLocal = 0;
            if (paramIndex >= fn->paramCount) {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return 0;
            }
            paramType = c->funcParamTypes[fn->paramTypeStart + paramIndex];
            isConstParam =
                (c->funcParamFlags[fn->paramTypeStart + paramIndex] & H2TCFuncParamFlag_CONST) != 0;

            if (!H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")
                && H2TCLocalAdd(c, n->dataStart, n->dataEnd, paramType, isConstParam, -1) != 0)
            {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return -1;
            }
            if (!H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")) {
                addedLocal = 1;
            }
            if (addedLocal && (n->flags & H2AstFlag_PARAM_VARIADIC) != 0
                && (paramType == c->typeAnytype
                    || ((uint32_t)paramType < c->typeLen
                        && c->types[paramType].kind == H2TCType_PACK)))
            {
                c->locals[c->localLen - 1u].flags |= H2TCLocalFlag_ANYPACK;
                variadicPackParamNameStart = n->dataStart;
                variadicPackParamNameEnd = n->dataEnd;
            }

            if ((isConstParam || (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) != 0)
                && binding != NULL && paramBindings != NULL
                && !H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "_"))
            {
                int32_t          argIndex = -1;
                H2CTFEValue      value;
                int              evalIsConst = 0;
                H2TCConstEvalCtx evalArgCtx;
                uint32_t         i;
                H2TCInitConstEvalCtxFromParent(c, savedActiveConstEvalCtx, &evalArgCtx);
                if (binding->isVariadic && paramIndex == binding->fixedCount) {
                    if (binding->spreadArgIndex != UINT32_MAX) {
                        argIndex = (int32_t)binding->spreadArgIndex;
                    }
                } else {
                    for (i = 0; i < argCount; i++) {
                        if (binding->argParamIndices[i] == (int32_t)paramIndex) {
                            argIndex = (int32_t)i;
                            break;
                        }
                    }
                }
                if (argIndex >= 0) {
                    if (H2TCEvalConstExprNode(
                            &evalArgCtx, callArgs[argIndex].exprNode, &value, &evalIsConst)
                        != 0)
                    {
                        c->localLen = savedLocalLen;
                        c->localUseLen = savedLocalUseLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        c->activeConstEvalCtx = savedActiveConstEvalCtx;
                        return -1;
                    }
                    if (!evalIsConst) {
                        if (!isConstParam) {
                            paramIndex++;
                            child = H2AstNextSibling(c->ast, child);
                            continue;
                        }
                        if (outError != NULL) {
                            outError->code = H2Diag_CONST_BLOCK_EVAL_FAILED;
                            outError->start = callArgs[argIndex].start;
                            outError->end = callArgs[argIndex].end;
                            outError->argStart = 0;
                            outError->argEnd = 0;
                        }
                        c->localLen = savedLocalLen;
                        c->localUseLen = savedLocalUseLen;
                        c->variantNarrowLen = savedVariantNarrowLen;
                        c->activeConstEvalCtx = savedActiveConstEvalCtx;
                        return 1;
                    }
                    paramBindings[paramBindingLen].nameStart = n->dataStart;
                    paramBindings[paramBindingLen].nameEnd = n->dataEnd;
                    paramBindings[paramBindingLen].typeId = paramType;
                    paramBindings[paramBindingLen].mutable = 0;
                    paramBindings[paramBindingLen]._reserved[0] = 0;
                    paramBindings[paramBindingLen]._reserved[1] = 0;
                    paramBindings[paramBindingLen]._reserved[2] = 0;
                    paramBindings[paramBindingLen].value = value;
                    paramBindingLen++;
                }
            }
            paramIndex++;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    if (paramIndex != fn->paramCount) {
        c->localLen = savedLocalLen;
        c->localUseLen = savedLocalUseLen;
        c->variantNarrowLen = savedVariantNarrowLen;
        c->activeConstEvalCtx = savedActiveConstEvalCtx;
        return 0;
    }

    paramFrame.parent = NULL;
    paramFrame.bindings = paramBindings;
    paramFrame.bindingLen = paramBindingLen;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = c->arena;
    execCtx.ast = c->ast;
    execCtx.src = c->src;
    execCtx.diag = c->diag;
    execCtx.env = &paramFrame;
    execCtx.evalExpr = H2TCEvalConstExecExprCb;
    execCtx.evalExprCtx = &evalCtx;
    execCtx.resolveType = H2TCEvalConstExecResolveTypeCb;
    execCtx.resolveTypeCtx = &evalCtx;
    execCtx.inferValueType = H2TCEvalConstExecInferValueTypeCb;
    execCtx.inferValueTypeCtx = &evalCtx;
    execCtx.forInIndex = H2TCEvalConstForInIndexCb;
    execCtx.forInIndexCtx = &evalCtx;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = H2TC_CONST_FOR_MAX_ITERS;
    H2TCInitConstEvalCtxFromParent(c, savedActiveConstEvalCtx, &evalCtx);
    evalCtx.execCtx = &execCtx;
    if (savedActiveConstEvalCtx != NULL && evalCtx.callFrameDepth < H2TC_CONST_CALL_MAX_DEPTH) {
        uint32_t frameIndex = evalCtx.callFrameDepth++;
        evalCtx.callFrameArgs[frameIndex] = savedActiveConstEvalCtx->callArgs;
        evalCtx.callFrameArgCounts[frameIndex] = savedActiveConstEvalCtx->callArgCount;
        evalCtx.callFrameBindings[frameIndex] = savedActiveConstEvalCtx->callBinding;
        evalCtx.callFrameFnIndices[frameIndex] = savedActiveConstEvalCtx->callFnIndex;
        evalCtx.callFramePackParamNameStarts[frameIndex] =
            savedActiveConstEvalCtx->callPackParamNameStart;
        evalCtx.callFramePackParamNameEnds[frameIndex] =
            savedActiveConstEvalCtx->callPackParamNameEnd;
    } else if (
        c->currentFunctionIndex >= 0 && (uint32_t)c->currentFunctionIndex < c->funcLen
        && evalCtx.callFrameDepth < H2TC_CONST_CALL_MAX_DEPTH)
    {
        const H2TCFunction* currentFn = &c->funcs[(uint32_t)c->currentFunctionIndex];
        if (currentFn->templateCallArgs != NULL && currentFn->templateCallBinding != NULL) {
            uint32_t frameIndex = evalCtx.callFrameDepth++;
            evalCtx.callFrameArgs[frameIndex] = currentFn->templateCallArgs;
            evalCtx.callFrameArgCounts[frameIndex] = currentFn->templateCallArgCount;
            evalCtx.callFrameBindings[frameIndex] = currentFn->templateCallBinding;
            evalCtx.callFrameFnIndices[frameIndex] = c->currentFunctionIndex;
            if ((currentFn->flags & H2TCFunctionFlag_VARIADIC) != 0 && currentFn->paramCount > 0) {
                uint32_t paramSlot = currentFn->paramTypeStart + currentFn->paramCount - 1u;
                evalCtx.callFramePackParamNameStarts[frameIndex] =
                    c->funcParamNameStarts[paramSlot];
                evalCtx.callFramePackParamNameEnds[frameIndex] = c->funcParamNameEnds[paramSlot];
            }
        }
    }
    evalCtx.callFnIndex = fnIndex;
    evalCtx.callArgs = callArgs;
    evalCtx.callArgCount = argCount;
    evalCtx.callBinding = binding;
    evalCtx.callPackParamNameStart = variadicPackParamNameStart;
    evalCtx.callPackParamNameEnd = variadicPackParamNameEnd;
    c->activeConstEvalCtx = &evalCtx;

    child = H2AstFirstChild(c->ast, bodyNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == H2Ast_CONST_BLOCK) {
            int32_t blockNode = H2AstFirstChild(c->ast, child);
            int     mirSupported = 0;
            if (blockNode < 0 || c->ast->nodes[blockNode].kind != H2Ast_BLOCK) {
                if (outError != NULL) {
                    outError->code = H2Diag_CONST_BLOCK_EVAL_FAILED;
                    outError->start = argCount > 0 ? callArgs[0].start : c->ast->nodes[child].start;
                    outError->end =
                        argCount > 0 ? callArgs[argCount - 1u].end : c->ast->nodes[child].end;
                    outError->argStart = 0;
                    outError->argEnd = 0;
                }
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return 1;
            }
            evalCtx.nonConstReason = NULL;
            evalCtx.nonConstStart = 0;
            evalCtx.nonConstEnd = 0;
            evalCtx.nonConstTraceDepth = 0;
            evalCtx.rootCallOwnerFnIndex = -1;
            evalCtx.rootCallStart = 0;
            rc = H2TCTryMirConstBlock(
                &evalCtx, blockNode, &retValue, &didReturn, &isConst, &mirSupported);
            if (rc != 0) {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return -1;
            }
            if (mirSupported && isConst && !didReturn) {
                child = H2AstNextSibling(c->ast, child);
                continue;
            }
            if (didReturn) {
                H2TCConstSetReasonNode(&evalCtx, blockNode, "const block must not return a value");
            } else if (evalCtx.nonConstReason == NULL) {
                H2TCConstSetReasonNode(&evalCtx, blockNode, "const block is not const-evaluable");
            }
            H2TCStoreLastConstEvalReason(c, c->activeConstEvalCtx);
            if (outError != NULL) {
                outError->code = H2Diag_CONST_BLOCK_EVAL_FAILED;
                outError->start = argCount > 0 ? callArgs[0].start : c->ast->nodes[child].start;
                outError->end =
                    argCount > 0 ? callArgs[argCount - 1u].end : c->ast->nodes[child].end;
                outError->argStart = 0;
                outError->argEnd = 0;
            }
            c->localLen = savedLocalLen;
            c->localUseLen = savedLocalUseLen;
            c->variantNarrowLen = savedVariantNarrowLen;
            c->activeConstEvalCtx = savedActiveConstEvalCtx;
            return 1;
        }
        child = H2AstNextSibling(c->ast, child);
    }

    c->localLen = savedLocalLen;
    c->localUseLen = savedLocalUseLen;
    c->variantNarrowLen = savedVariantNarrowLen;
    c->activeConstEvalCtx = savedActiveConstEvalCtx;
    return 0;
}

int H2TCResolveComparisonHookArgCost(
    H2TypeCheckCtx* c, int32_t paramType, int32_t argType, uint8_t* outCost) {
    int32_t resolvedParam = H2TCResolveAliasBaseType(c, paramType);
    uint8_t baseCost = 0;
    if (H2TCConversionCost(c, paramType, argType, outCost) == 0) {
        return 0;
    }
    if (resolvedParam < 0 || (uint32_t)resolvedParam >= c->typeLen) {
        return -1;
    }
    if (c->types[resolvedParam].kind == H2TCType_REF && !H2TCTypeIsMutable(&c->types[resolvedParam])
        && H2TCConversionCost(c, c->types[resolvedParam].baseType, argType, &baseCost) == 0)
    {
        *outCost = (uint8_t)(baseCost < 254u ? baseCost + 1u : 255u);
        return 0;
    }
    return -1;
}

int H2TCResolveComparisonHook(
    H2TypeCheckCtx* c,
    const char*     hookName,
    int32_t         lhsType,
    int32_t         rhsType,
    int32_t*        outFuncIndex) {
    uint8_t  bestCosts[2];
    int      haveBest = 0;
    int      ambiguous = 0;
    int      nameFound = 0;
    uint32_t bestTotal = 0;
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        uint8_t             curCosts[2];
        uint32_t            curTotal = 0;
        int                 cmp;
        if (!H2NameEqLiteral(c->src, fn->nameStart, fn->nameEnd, hookName)) {
            continue;
        }
        nameFound = 1;
        if (fn->paramCount != 2) {
            continue;
        }
        if (H2TCResolveComparisonHookArgCost(
                c, c->funcParamTypes[fn->paramTypeStart], lhsType, &curCosts[0])
                != 0
            || H2TCResolveComparisonHookArgCost(
                   c, c->funcParamTypes[fn->paramTypeStart + 1u], rhsType, &curCosts[1])
                   != 0)
        {
            continue;
        }
        curTotal = (uint32_t)curCosts[0] + (uint32_t)curCosts[1];
        if (!haveBest) {
            *outFuncIndex = (int32_t)i;
            haveBest = 1;
            ambiguous = 0;
            bestTotal = curTotal;
            bestCosts[0] = curCosts[0];
            bestCosts[1] = curCosts[1];
            continue;
        }
        cmp = H2TCCostVectorCompare(curCosts, bestCosts, 2u);
        if (cmp < 0 || (cmp == 0 && curTotal < bestTotal)) {
            *outFuncIndex = (int32_t)i;
            ambiguous = 0;
            bestTotal = curTotal;
            bestCosts[0] = curCosts[0];
            bestCosts[1] = curCosts[1];
            continue;
        }
        if (cmp == 0 && curTotal == bestTotal) {
            ambiguous = 1;
        }
    }
    if (!nameFound) {
        return 1;
    }
    if (!haveBest) {
        return 2;
    }
    return ambiguous ? 3 : 0;
}

static int H2TCFunctionIsBuiltinQualifiedSlice(
    H2TypeCheckCtx* c, const H2TCFunction* fn, uint32_t start, uint32_t end) {
    uint32_t nameLen;
    uint32_t candLen;
    if (end <= start || end > c->src.len || fn->nameEnd <= fn->nameStart) {
        return 0;
    }
    nameLen = end - start;
    candLen = fn->nameEnd - fn->nameStart;
    if (candLen != 9u + nameLen) {
        return 0;
    }
    if (memcmp(c->src.ptr + fn->nameStart, "builtin__", 9u) != 0) {
        return 0;
    }
    return memcmp(c->src.ptr + fn->nameStart + 9u, c->src.ptr + start, nameLen) == 0;
}

static int H2TCFunctionIdentityEq(H2TypeCheckCtx* c, const H2TCFunction* a, const H2TCFunction* b) {
    uint32_t p;
    if (a->paramCount != b->paramCount
        || ((a->flags & H2TCFunctionFlag_VARIADIC) != (b->flags & H2TCFunctionFlag_VARIADIC)))
    {
        return 0;
    }
    if (a->returnType < 0 || b->returnType < 0) {
        if (a->returnType != b->returnType) {
            return 0;
        }
    } else if (
        !H2TCCanAssign(c, a->returnType, b->returnType)
        || !H2TCCanAssign(c, b->returnType, a->returnType))
    {
        return 0;
    }
    if ((a->contextType < 0) != (b->contextType < 0)) {
        return 0;
    }
    if (a->contextType >= 0
        && (!H2TCCanAssign(c, a->contextType, b->contextType)
            || !H2TCCanAssign(c, b->contextType, a->contextType)))
    {
        return 0;
    }
    for (p = 0; p < a->paramCount; p++) {
        uint32_t aIndex = a->paramTypeStart + p;
        uint32_t bIndex = b->paramTypeStart + p;
        if (!H2TCCanAssign(c, c->funcParamTypes[aIndex], c->funcParamTypes[bIndex])
            || !H2TCCanAssign(c, c->funcParamTypes[bIndex], c->funcParamTypes[aIndex]))
        {
            return 0;
        }
        if ((c->funcParamFlags[aIndex] & H2TCFuncParamFlag_CONST)
            != (c->funcParamFlags[bIndex] & H2TCFuncParamFlag_CONST))
        {
            return 0;
        }
    }
    return 1;
}

static int H2TCHasUnqualifiedFunctionIdentity(
    H2TypeCheckCtx* c, const H2TCFunction* builtinFn, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        if (!H2NameEqSlice(c->src, fn->nameStart, fn->nameEnd, nameStart, nameEnd)) {
            continue;
        }
        if (H2TCFunctionIdentityEq(c, fn, builtinFn)) {
            return 1;
        }
    }
    return 0;
}

void H2TCGatherCallCandidates(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    int32_t*        outCandidates,
    uint32_t*       outCandidateCount,
    int*            outNameFound) {
    uint32_t count = 0;
    uint32_t i;
    *outNameFound = 0;
    for (i = 0; i < c->funcLen && count < H2TC_MAX_CALL_CANDIDATES; i++) {
        if (H2TCFunctionNameEq(c, i, nameStart, nameEnd)) {
            outCandidates[count++] = (int32_t)i;
            *outNameFound = 1;
        }
    }
    for (i = 0; i < c->funcLen && count < H2TC_MAX_CALL_CANDIDATES; i++) {
        const H2TCFunction* fn = &c->funcs[i];
        if (!H2TCFunctionIsBuiltinQualifiedSlice(c, fn, nameStart, nameEnd)) {
            continue;
        }
        if (H2TCHasUnqualifiedFunctionIdentity(c, fn, nameStart, nameEnd)) {
            continue;
        }
        outCandidates[count++] = (int32_t)i;
        *outNameFound = 1;
    }
    *outCandidateCount = count;
}

void H2TCGatherCallCandidatesByPkgMethod(
    H2TypeCheckCtx* c,
    uint32_t        pkgStart,
    uint32_t        pkgEnd,
    uint32_t        methodStart,
    uint32_t        methodEnd,
    int32_t*        outCandidates,
    uint32_t*       outCandidateCount,
    int*            outNameFound) {
    uint32_t count = 0;
    uint32_t i;
    *outNameFound = 0;
    for (i = 0; i < c->funcLen && count < H2TC_MAX_CALL_CANDIDATES; i++) {
        if (H2TCNameEqPkgPrefixedMethod(
                c,
                c->funcs[i].nameStart,
                c->funcs[i].nameEnd,
                pkgStart,
                pkgEnd,
                methodStart,
                methodEnd))
        {
            outCandidates[count++] = (int32_t)i;
            *outNameFound = 1;
        }
    }
    *outCandidateCount = count;
}

int H2TCFunctionHasAnytypeParam(H2TypeCheckCtx* c, int32_t fnIndex) {
    const H2TCFunction* fn;
    uint32_t            p;
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return 0;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & H2TCFunctionFlag_TEMPLATE) != 0) {
        return 1;
    }
    for (p = 0; p < fn->paramCount; p++) {
        if (c->funcParamTypes[fn->paramTypeStart + p] == c->typeAnytype) {
            return 1;
        }
    }
    return 0;
}

static int H2TCInferTemplateArgBind(
    H2TypeCheckCtx* c, int32_t* boundType, int32_t argType, uint32_t errStart, uint32_t errEnd) {
    if (*boundType < 0) {
        *boundType = argType;
        return 0;
    }
    if (*boundType == argType) {
        return 0;
    }
    if (H2TCIsUntyped(c, *boundType) && !H2TCIsUntyped(c, argType)
        && H2TCCanAssign(c, argType, *boundType))
    {
        *boundType = argType;
        return 0;
    }
    if (!H2TCIsUntyped(c, *boundType) && H2TCIsUntyped(c, argType)
        && H2TCCanAssign(c, *boundType, argType))
    {
        return 0;
    }
    (void)errStart;
    (void)errEnd;
    return 1;
}

static int H2TCInferTemplateArgsFromTypes(
    H2TypeCheckCtx* c,
    int32_t         paramType,
    int32_t         argType,
    const int32_t*  templateParamTypes,
    int32_t*        templateArgTypes,
    uint16_t        templateArgCount,
    uint32_t        errStart,
    uint32_t        errEnd) {
    uint16_t i;
    int32_t  resolvedParam = H2TCResolveAliasBaseType(c, paramType);
    int32_t  resolvedArg = H2TCResolveAliasBaseType(c, argType);
    if (resolvedParam < 0) {
        resolvedParam = paramType;
    }
    if (resolvedArg < 0) {
        resolvedArg = argType;
    }
    for (i = 0; i < templateArgCount; i++) {
        if (templateParamTypes[i] == paramType || templateParamTypes[i] == resolvedParam) {
            return H2TCInferTemplateArgBind(c, &templateArgTypes[i], argType, errStart, errEnd);
        }
    }
    if (resolvedParam < 0 || (uint32_t)resolvedParam >= c->typeLen || resolvedArg < 0
        || (uint32_t)resolvedArg >= c->typeLen)
    {
        return 0;
    }
    if (resolvedParam == resolvedArg) {
        return 0;
    }
    if (c->types[resolvedParam].kind != c->types[resolvedArg].kind) {
        return 0;
    }
    switch (c->types[resolvedParam].kind) {
        case H2TCType_PTR:
        case H2TCType_REF:
        case H2TCType_ARRAY:
        case H2TCType_SLICE:
        case H2TCType_OPTIONAL:
            return H2TCInferTemplateArgsFromTypes(
                c,
                c->types[resolvedParam].baseType,
                c->types[resolvedArg].baseType,
                templateParamTypes,
                templateArgTypes,
                templateArgCount,
                errStart,
                errEnd);
        case H2TCType_TUPLE:
        case H2TCType_PACK:
        case H2TCType_FUNCTION: {
            uint32_t j;
            if (c->types[resolvedParam].fieldCount != c->types[resolvedArg].fieldCount) {
                return 0;
            }
            if (c->types[resolvedParam].kind == H2TCType_FUNCTION
                && H2TCInferTemplateArgsFromTypes(
                       c,
                       c->types[resolvedParam].baseType,
                       c->types[resolvedArg].baseType,
                       templateParamTypes,
                       templateArgTypes,
                       templateArgCount,
                       errStart,
                       errEnd)
                       != 0)
            {
                return 1;
            }
            for (j = 0; j < c->types[resolvedParam].fieldCount; j++) {
                if (H2TCInferTemplateArgsFromTypes(
                        c,
                        c->funcParamTypes[c->types[resolvedParam].fieldStart + j],
                        c->funcParamTypes[c->types[resolvedArg].fieldStart + j],
                        templateParamTypes,
                        templateArgTypes,
                        templateArgCount,
                        errStart,
                        errEnd)
                    != 0)
                {
                    return 1;
                }
            }
            return 0;
        }
        case H2TCType_NAMED:
        case H2TCType_ALIAS: {
            const H2TCNamedType* paramNt = NULL;
            const H2TCNamedType* argNt = NULL;
            uint32_t             ni;
            for (ni = 0; ni < c->namedTypeLen; ni++) {
                if (c->namedTypes[ni].typeId == resolvedParam) {
                    paramNt = &c->namedTypes[ni];
                }
                if (c->namedTypes[ni].typeId == resolvedArg) {
                    argNt = &c->namedTypes[ni];
                }
            }
            if (paramNt != NULL && argNt != NULL) {
                int32_t paramRoot =
                    paramNt->templateRootNamedIndex >= 0
                        ? c->namedTypes[(uint32_t)paramNt->templateRootNamedIndex].typeId
                        : resolvedParam;
                int32_t argRoot =
                    argNt->templateRootNamedIndex >= 0
                        ? c->namedTypes[(uint32_t)argNt->templateRootNamedIndex].typeId
                        : resolvedArg;
                uint16_t j;
                if (paramRoot != argRoot || paramNt->templateArgCount != argNt->templateArgCount) {
                    return 0;
                }
                for (j = 0; j < paramNt->templateArgCount; j++) {
                    if (H2TCInferTemplateArgsFromTypes(
                            c,
                            c->genericArgTypes[paramNt->templateArgStart + j],
                            c->genericArgTypes[argNt->templateArgStart + j],
                            templateParamTypes,
                            templateArgTypes,
                            templateArgCount,
                            errStart,
                            errEnd)
                        != 0)
                    {
                        return 1;
                    }
                }
            }
            return 0;
        }
        default: return 0;
    }
}

int H2TCInstantiateAnytypeFunctionForCall(
    H2TypeCheckCtx*        c,
    int32_t                fnIndex,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int32_t                autoRefFirstArgType,
    int32_t*               outFuncIndex,
    H2TCCallMapError*      outError) {
    const H2TCFunction* fn;
    int32_t             resolvedParamTypes[H2TC_MAX_CALL_ARGS];
    uint32_t            p;
    H2TCCallBinding     binding;
    H2TCCallMapError    mapError;
    uint8_t             hasAnytypeParam = 0;
    uint8_t             hasAnyPack = 0;
    int32_t             templateArgTypes[64];
    int32_t             expectedReturnType = c->activeExpectedCallType;
    int32_t             packElems[H2TC_MAX_CALL_ARGS];
    uint32_t            packElemCount = 0;

    if (outError != NULL) {
        H2TCCallMapErrorClear(outError);
    }
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen || outFuncIndex == NULL) {
        return -1;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & H2TCFunctionFlag_TEMPLATE) == 0
        || (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) != 0)
    {
        *outFuncIndex = fnIndex;
        return 0;
    }
    if (fn->paramCount > H2TC_MAX_CALL_ARGS) {
        return -1;
    }
    if (fn->templateArgCount > (uint16_t)(sizeof(templateArgTypes) / sizeof(templateArgTypes[0]))) {
        return -1;
    }
    for (p = 0; p < fn->templateArgCount; p++) {
        templateArgTypes[p] = -1;
    }

    for (p = 0; p < fn->paramCount; p++) {
        resolvedParamTypes[p] = c->funcParamTypes[fn->paramTypeStart + p];
        if (resolvedParamTypes[p] == c->typeAnytype) {
            hasAnytypeParam = 1;
            if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0 && p + 1u == fn->paramCount) {
                hasAnyPack = 1;
            }
        }
    }
    if (!hasAnytypeParam && fn->templateArgCount == 0) {
        *outFuncIndex = fnIndex;
        return 0;
    }

    H2TCCallMapErrorClear(&mapError);
    {
        int prepStatus = H2TCPrepareCallBinding(
            c,
            callArgs,
            argCount,
            &c->funcParamNameStarts[fn->paramTypeStart],
            &c->funcParamNameEnds[fn->paramTypeStart],
            &c->funcParamTypes[fn->paramTypeStart],
            fn->paramCount,
            (fn->flags & H2TCFunctionFlag_VARIADIC) != 0,
            1,
            firstPositionalArgIndex,
            &binding,
            &mapError);
        if (prepStatus != 0) {
            if (prepStatus == 2 && outError != NULL && mapError.code != 0) {
                *outError = mapError;
                return 2;
            }
            return 1;
        }
    }

    for (p = 0; p < argCount; p++) {
        int32_t argType = -1;
        int32_t inferredArgType;
        int32_t argExprNode = callArgs[p].exprNode;
        int32_t paramIndex = binding.argParamIndices[p];
        int32_t expectedType = binding.argExpectedTypes[p];
        int32_t savedExpectedCallType = c->activeExpectedCallType;
        if (paramIndex < 0 || (uint32_t)paramIndex >= fn->paramCount) {
            return 1;
        }
        c->activeExpectedCallType = -1;
        if (p == 0 && autoRefFirstArgType >= 0) {
            argType = autoRefFirstArgType;
        } else if (!H2TCExprNeedsExpectedType(c, argExprNode)) {
            if (H2TCTypeExpr(c, argExprNode, &argType) != 0) {
                c->activeExpectedCallType = savedExpectedCallType;
                return -1;
            }
        } else {
            H2Diag savedDiag = { 0 };
            if (c->diag != NULL) {
                savedDiag = *c->diag;
            }
            if (H2TCTypeExprExpected(c, argExprNode, expectedType, &argType) != 0) {
                c->activeExpectedCallType = savedExpectedCallType;
                if (c->diag != NULL) {
                    *c->diag = savedDiag;
                }
                return 1;
            }
        }
        c->activeExpectedCallType = savedExpectedCallType;
        inferredArgType = argType;
        if (fn->templateArgCount > 0
            && H2TCInferTemplateArgsFromTypes(
                   c,
                   c->funcParamTypes[fn->paramTypeStart + (uint32_t)paramIndex],
                   inferredArgType,
                   &c->genericArgTypes[fn->templateArgStart],
                   templateArgTypes,
                   fn->templateArgCount,
                   callArgs[p].start,
                   callArgs[p].end)
                   != 0)
        {
            return 1;
        }
        if (H2TCConcretizeInferredType(c, argType, &argType) != 0) {
            return -1;
        }

        if (expectedType == c->typeAnytype) {
            if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0
                && (uint32_t)paramIndex + 1u == fn->paramCount)
            {
                if (binding.spreadArgIndex == p) {
                    int32_t spreadType = H2TCResolveAliasBaseType(c, argType);
                    if (spreadType < 0 || (uint32_t)spreadType >= c->typeLen
                        || c->types[spreadType].kind != H2TCType_PACK)
                    {
                        if (outError != NULL) {
                            outError->code = H2Diag_ANYTYPE_SPREAD_REQUIRES_PACK;
                            outError->start = callArgs[p].start;
                            outError->end = callArgs[p].end;
                            outError->argStart = 0;
                            outError->argEnd = 0;
                        }
                        return 2;
                    }
                    resolvedParamTypes[paramIndex] = spreadType;
                } else {
                    if (packElemCount >= H2TC_MAX_CALL_ARGS) {
                        return -1;
                    }
                    packElems[packElemCount++] = argType;
                }
            } else {
                resolvedParamTypes[paramIndex] = argType;
            }
        }
    }
    if (fn->templateArgCount > 0 && expectedReturnType >= 0) {
        if (H2TCInferTemplateArgsFromTypes(
                c,
                fn->returnType,
                expectedReturnType,
                &c->genericArgTypes[fn->templateArgStart],
                templateArgTypes,
                fn->templateArgCount,
                fn->nameStart,
                fn->nameEnd)
            != 0)
        {
            return 1;
        }
    }

    if (hasAnyPack) {
        int32_t lastParamIndex = (int32_t)fn->paramCount - 1;
        if (lastParamIndex < 0) {
            return 1;
        }
        if (binding.spreadArgIndex == UINT32_MAX) {
            int32_t packType = H2TCInternPackType(
                c,
                packElems,
                packElemCount,
                c->ast->nodes[fn->declNode].start,
                c->ast->nodes[fn->declNode].end);
            if (packType < 0) {
                return -1;
            }
            resolvedParamTypes[lastParamIndex] = packType;
        } else {
            int32_t spreadType = resolvedParamTypes[lastParamIndex];
            if (spreadType < 0 || (uint32_t)spreadType >= c->typeLen
                || c->types[spreadType].kind != H2TCType_PACK)
            {
                return 1;
            }
        }
    }

    for (p = 0; p < fn->templateArgCount; p++) {
        if (templateArgTypes[p] < 0) {
            return 1;
        }
        if (H2TCConcretizeInferredType(c, templateArgTypes[p], &templateArgTypes[p]) != 0) {
            return -1;
        }
    }
    if (fn->templateArgCount > 0) {
        for (p = 0; p < fn->paramCount; p++) {
            resolvedParamTypes[p] = H2TCSubstituteType(
                c,
                c->funcParamTypes[fn->paramTypeStart + p],
                &c->genericArgTypes[fn->templateArgStart],
                templateArgTypes,
                fn->templateArgCount,
                c->ast->nodes[fn->declNode].start,
                c->ast->nodes[fn->declNode].end);
            if (resolvedParamTypes[p] < 0) {
                return -1;
            }
        }
    }

    for (p = 0; p < c->funcLen; p++) {
        const H2TCFunction* cur = &c->funcs[p];
        uint32_t            j;
        if (cur->declNode != fn->declNode || (cur->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0
            || cur->paramCount != fn->paramCount || cur->contextType != fn->contextType
            || ((cur->flags & H2TCFunctionFlag_VARIADIC)
                != (fn->flags & H2TCFunctionFlag_VARIADIC)))
        {
            continue;
        }
        if (cur->templateArgCount != fn->templateArgCount) {
            continue;
        }
        for (j = 0; j < fn->templateArgCount; j++) {
            if (c->genericArgTypes[cur->templateArgStart + j] != templateArgTypes[j]) {
                break;
            }
        }
        if (j != fn->templateArgCount) {
            continue;
        }
        for (j = 0; j < fn->paramCount; j++) {
            if (c->funcParamTypes[cur->paramTypeStart + j] != resolvedParamTypes[j]
                || (c->funcParamFlags[cur->paramTypeStart + j] & H2TCFuncParamFlag_CONST)
                       != (c->funcParamFlags[fn->paramTypeStart + j] & H2TCFuncParamFlag_CONST))
            {
                break;
            }
        }
        if (j == fn->paramCount) {
            *outFuncIndex = (int32_t)p;
            return 0;
        }
    }

    if (c->funcLen >= c->funcCap || c->funcParamLen + fn->paramCount > c->funcParamCap) {
        return H2TCFailNode(c, fn->declNode, H2Diag_ARENA_OOM);
    }

    {
        uint32_t      idx = c->funcLen++;
        H2TCFunction* f = &c->funcs[idx];
        int32_t       typeId;
        for (p = 0; p < fn->paramCount; p++) {
            uint32_t argIndex;
            c->funcParamTypes[c->funcParamLen + p] = resolvedParamTypes[p];
            c->funcParamNameStarts[c->funcParamLen + p] =
                c->funcParamNameStarts[fn->paramTypeStart + p];
            c->funcParamNameEnds[c->funcParamLen + p] =
                c->funcParamNameEnds[fn->paramTypeStart + p];
            c->funcParamFlags[c->funcParamLen + p] =
                c->funcParamFlags[fn->paramTypeStart + p] & H2TCFuncParamFlag_CONST;
            c->funcParamCallArgStarts[c->funcParamLen + p] = 0;
            c->funcParamCallArgEnds[c->funcParamLen + p] = 0;
            c->funcParamCallArgExprNodes[c->funcParamLen + p] = -1;
            if ((c->funcParamFlags[c->funcParamLen + p] & H2TCFuncParamFlag_CONST) != 0) {
                int32_t mappedExprNode =
                    p < H2TC_MAX_CALL_ARGS ? binding.fixedMappedArgExprNodes[p] : -1;
                if (mappedExprNode >= 0 && (uint32_t)mappedExprNode < c->ast->len) {
                    c->funcParamCallArgStarts[c->funcParamLen + p] =
                        c->ast->nodes[mappedExprNode].start;
                    c->funcParamCallArgEnds[c->funcParamLen + p] =
                        c->ast->nodes[mappedExprNode].end;
                    c->funcParamCallArgExprNodes[c->funcParamLen + p] = mappedExprNode;
                } else {
                    for (argIndex = 0; argIndex < argCount; argIndex++) {
                        if (binding.argParamIndices[argIndex] == (int32_t)p) {
                            c->funcParamCallArgStarts[c->funcParamLen + p] =
                                callArgs[argIndex].start;
                            c->funcParamCallArgEnds[c->funcParamLen + p] = callArgs[argIndex].end;
                            c->funcParamCallArgExprNodes[c->funcParamLen + p] =
                                callArgs[argIndex].exprNode;
                            break;
                        }
                    }
                }
            }
        }
        f->nameStart = fn->nameStart;
        f->nameEnd = fn->nameEnd;
        f->returnType =
            fn->templateArgCount > 0
                ? H2TCSubstituteType(
                      c,
                      fn->returnType,
                      &c->genericArgTypes[fn->templateArgStart],
                      templateArgTypes,
                      fn->templateArgCount,
                      fn->nameStart,
                      fn->nameEnd)
                : fn->returnType;
        f->paramTypeStart = c->funcParamLen;
        f->paramCount = fn->paramCount;
        f->contextType =
            fn->templateArgCount > 0
                ? H2TCSubstituteType(
                      c,
                      fn->contextType,
                      &c->genericArgTypes[fn->templateArgStart],
                      templateArgTypes,
                      fn->templateArgCount,
                      fn->nameStart,
                      fn->nameEnd)
                : fn->contextType;
        f->declNode = fn->declNode;
        f->defNode = fn->defNode;
        f->funcTypeId = -1;
        f->templateArgStart = c->genericArgLen;
        f->templateArgCount = fn->templateArgCount;
        f->templateRootFuncIndex = (int16_t)fnIndex;
        f->flags = (fn->flags & H2TCFunctionFlag_VARIADIC) | H2TCFunctionFlag_TEMPLATE
                 | H2TCFunctionFlag_TEMPLATE_INSTANCE;
        f->templateCallArgs = NULL;
        f->templateCallArgCount = 0;
        f->templateCallBinding = NULL;
        if (argCount > 0) {
            H2TCCallArgInfo* callArgCopy = (H2TCCallArgInfo*)H2ArenaAlloc(
                c->arena, sizeof(H2TCCallArgInfo) * argCount, (uint32_t)_Alignof(H2TCCallArgInfo));
            if (callArgCopy == NULL) {
                return H2TCFailNode(c, fn->declNode, H2Diag_ARENA_OOM);
            }
            memcpy(callArgCopy, callArgs, sizeof(H2TCCallArgInfo) * argCount);
            f->templateCallArgs = callArgCopy;
            f->templateCallArgCount = argCount;
        }
        {
            H2TCCallBinding* bindingCopy = (H2TCCallBinding*)H2ArenaAlloc(
                c->arena, sizeof(H2TCCallBinding), (uint32_t)_Alignof(H2TCCallBinding));
            if (bindingCopy == NULL) {
                return H2TCFailNode(c, fn->declNode, H2Diag_ARENA_OOM);
            }
            *bindingCopy = binding;
            f->templateCallBinding = bindingCopy;
        }
        for (p = 0; p < fn->templateArgCount; p++) {
            c->genericArgTypes[c->genericArgLen++] = templateArgTypes[p];
        }
        if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0 && fn->paramCount > 0) {
            int32_t variadicType = resolvedParamTypes[fn->paramCount - 1u];
            if (variadicType >= 0 && (uint32_t)variadicType < c->typeLen
                && c->types[variadicType].kind == H2TCType_PACK)
            {
                f->flags |= H2TCFunctionFlag_TEMPLATE_HAS_ANYPACK;
            }
        }
        c->funcParamLen += fn->paramCount;
        typeId = H2TCInternFunctionType(
            c,
            f->returnType,
            &c->funcParamTypes[f->paramTypeStart],
            &c->funcParamFlags[f->paramTypeStart],
            f->paramCount,
            (f->flags & H2TCFunctionFlag_VARIADIC) != 0,
            (int32_t)idx,
            f->nameStart,
            f->nameEnd);
        if (typeId < 0) {
            return -1;
        }
        f->funcTypeId = typeId;
        *outFuncIndex = (int32_t)idx;
        return 0;
    }
}

int H2TCResolveCallFromCandidates(
    H2TypeCheckCtx*        c,
    const int32_t*         candidates,
    uint32_t               candidateCount,
    int                    nameFound,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode) {
    uint8_t          bestCosts[H2TC_MAX_CALL_ARGS];
    int              haveBest = 0;
    int              ambiguous = 0;
    int              hasExpectedDependentArg = 0;
    int32_t          mutRefTempArgNode = -1;
    int32_t          autoRefType = -1;
    int              hasAutoRefType = 0;
    H2TCCallMapError firstMapError;
    int              hasMapError = 0;
    uint32_t         firstMapErrorCost = UINT32_MAX;
    uint32_t         bestTotal = 0;
    uint32_t         i;
    uint32_t         p;
    H2TCCallMapErrorClear(&firstMapError);
    if (!nameFound) {
        return 1;
    }

    if (argCount > H2TC_MAX_CALL_ARGS) {
        return -1;
    }
    if (autoRefFirstArg && argCount > 0 && H2TCExprIsAssignable(c, callArgs[0].exprNode)) {
        int32_t          argType;
        const H2AstNode* argNode = &c->ast->nodes[callArgs[0].exprNode];
        if (H2TCTypeExpr(c, callArgs[0].exprNode, &argType) != 0) {
            return -1;
        }
        autoRefType = H2TCInternPtrType(c, argType, argNode->start, argNode->end);
        if (autoRefType < 0) {
            return -1;
        }
        hasAutoRefType = 1;
    }
    for (p = 0; p < argCount; p++) {
        hasExpectedDependentArg =
            hasExpectedDependentArg || H2TCExprNeedsExpectedType(c, callArgs[p].exprNode) != 0;
    }

    for (i = 0; i < candidateCount; i++) {
        int32_t             fnIdx = candidates[i];
        int32_t             candidateFnIdx = fnIdx;
        const H2TCFunction* fn = &c->funcs[candidateFnIdx];
        uint8_t             curCosts[H2TC_MAX_CALL_ARGS];
        H2TCCallBinding     binding;
        uint32_t            curTotal = 0;
        int                 viable = 1;
        int                 cmp;
        H2TCCallMapError    mapError;
        H2TCCallMapErrorClear(&mapError);
        if ((fn->flags & H2TCFunctionFlag_TEMPLATE) != 0
            && (fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0)
        {
            int instantiateStatus = H2TCInstantiateAnytypeFunctionForCall(
                c,
                fnIdx,
                callArgs,
                argCount,
                firstPositionalArgIndex,
                hasAutoRefType ? autoRefType : -1,
                &candidateFnIdx,
                &mapError);
            if (instantiateStatus < 0) {
                return -1;
            }
            if (instantiateStatus != 0) {
                if (instantiateStatus == 2 && !hasMapError && mapError.code != 0) {
                    hasMapError = 1;
                    firstMapError = mapError;
                    firstMapErrorCost = UINT32_MAX;
                }
                continue;
            }
            fn = &c->funcs[candidateFnIdx];
        }
        {
            int prepStatus = H2TCPrepareCallBinding(
                c,
                callArgs,
                argCount,
                &c->funcParamNameStarts[fn->paramTypeStart],
                &c->funcParamNameEnds[fn->paramTypeStart],
                &c->funcParamTypes[fn->paramTypeStart],
                fn->paramCount,
                (fn->flags & H2TCFunctionFlag_VARIADIC) != 0,
                1,
                firstPositionalArgIndex,
                &binding,
                &mapError);
            if (prepStatus != 0) {
                if (prepStatus == 2 && !hasMapError && mapError.code != 0) {
                    hasMapError = 1;
                    firstMapError = mapError;
                    firstMapErrorCost = UINT32_MAX;
                }
                continue;
            }
        }
        for (p = 0; p < argCount; p++) {
            int32_t paramType = binding.argExpectedTypes[p];
            int32_t argNode = callArgs[p].exprNode;
            int32_t argType;
            uint8_t cost = 0;
            if (paramType < 0) {
                viable = 0;
                break;
            }
            if (hasAutoRefType && p == 0) {
                argType = autoRefType;
            } else {
                if (H2TCIsMutableRefType(c, paramType) && H2TCExprIsCompoundTemporary(c, argNode)) {
                    if (mutRefTempArgNode < 0) {
                        mutRefTempArgNode = argNode;
                    }
                    viable = 0;
                    break;
                }
                if (!H2TCExprNeedsExpectedType(c, argNode)) {
                    if (H2TCTypeExpr(c, argNode, &argType) != 0) {
                        return -1;
                    }
                } else {
                    H2Diag savedDiag = { 0 };
                    if (c->diag != NULL) {
                        savedDiag = *c->diag;
                    }
                    if (H2TCTypeExprExpected(c, argNode, paramType, &argType) != 0) {
                        if (c->diag != NULL) {
                            *c->diag = savedDiag;
                        }
                        if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0
                            && p >= binding.fixedInputCount)
                        {
                            mapError.code =
                                (binding.spreadArgIndex == p)
                                    ? H2Diag_VARIADIC_SPREAD_NON_SLICE
                                    : H2Diag_VARIADIC_ARG_TYPE_MISMATCH;
                            mapError.start = callArgs[p].start;
                            mapError.end = callArgs[p].end;
                            mapError.argStart = 0;
                            mapError.argEnd = 0;
                        }
                        viable = 0;
                        break;
                    }
                }
            }
            if (H2TCConversionCost(c, paramType, argType, &cost) != 0) {
                if ((fn->flags & H2TCFunctionFlag_VARIADIC) != 0 && p >= binding.fixedInputCount) {
                    mapError.code =
                        (binding.spreadArgIndex == p)
                            ? H2Diag_VARIADIC_SPREAD_NON_SLICE
                            : H2Diag_VARIADIC_ARG_TYPE_MISMATCH;
                    mapError.start = callArgs[p].start;
                    mapError.end = callArgs[p].end;
                    mapError.argStart = 0;
                    mapError.argEnd = 0;
                }
                viable = 0;
                break;
            }
            curCosts[p] = cost;
            curTotal += cost;
        }
        if (viable) {
            int constStatus = H2TCCheckConstParamArgs(
                c,
                callArgs,
                argCount,
                &binding,
                &c->funcParamNameStarts[fn->paramTypeStart],
                &c->funcParamNameEnds[fn->paramTypeStart],
                &c->funcParamFlags[fn->paramTypeStart],
                fn->paramCount,
                &mapError);
            if (constStatus < 0) {
                return -1;
            }
            if (constStatus != 0) {
                viable = 0;
            }
        }
        if (viable) {
            int constBlockStatus = H2TCCheckConstBlocksForCall(
                c, candidateFnIdx, callArgs, argCount, &binding, &mapError);
            if (constBlockStatus < 0) {
                return -1;
            }
            if (constBlockStatus != 0) {
                viable = 0;
            }
        }
        if (!viable) {
            if (mapError.code != 0
                && (!hasMapError
                    || ((mapError.code == H2Diag_CONST_PARAM_ARG_NOT_CONST
                         || mapError.code == H2Diag_CONST_PARAM_SPREAD_NOT_CONST
                         || mapError.code == H2Diag_CONST_BLOCK_EVAL_FAILED)
                        && curTotal < firstMapErrorCost)))
            {
                hasMapError = 1;
                firstMapError = mapError;
                firstMapErrorCost = curTotal;
            }
            continue;
        }
        if (!haveBest) {
            uint32_t j;
            haveBest = 1;
            ambiguous = 0;
            *outFuncIndex = candidateFnIdx;
            bestTotal = curTotal;
            for (j = 0; j < argCount; j++) {
                bestCosts[j] = curCosts[j];
            }
            continue;
        }
        cmp = H2TCCostVectorCompare(curCosts, bestCosts, argCount);
        if (cmp < 0 || (cmp == 0 && curTotal < bestTotal)) {
            uint32_t j;
            *outFuncIndex = candidateFnIdx;
            bestTotal = curTotal;
            ambiguous = 0;
            for (j = 0; j < argCount; j++) {
                bestCosts[j] = curCosts[j];
            }
            continue;
        }
        if (cmp == 0 && curTotal == bestTotal && candidateFnIdx != *outFuncIndex) {
            ambiguous = 1;
        }
    }

    if (!haveBest) {
        if (hasMapError && c->diag != NULL) {
            H2TCSetDiagWithArg(
                c->diag,
                firstMapError.code,
                firstMapError.start,
                firstMapError.end,
                firstMapError.argStart,
                firstMapError.argEnd);
            if (firstMapError.code == H2Diag_CONST_BLOCK_EVAL_FAILED) {
                H2TCAttachConstEvalReason(c);
            }
            return 6;
        }
        if (outMutRefTempArgNode != NULL && mutRefTempArgNode >= 0) {
            *outMutRefTempArgNode = mutRefTempArgNode;
        }
        if (mutRefTempArgNode >= 0) {
            return 4;
        }
        return 2;
    }
    if (ambiguous) {
        if (hasExpectedDependentArg) {
            return 5;
        }
        return 3;
    }
    if (outMutRefTempArgNode != NULL) {
        *outMutRefTempArgNode = -1;
    }
    return 0;
}

int H2TCResolveCallByName(
    H2TypeCheckCtx*        c,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode) {
    int32_t  candidates[H2TC_MAX_CALL_CANDIDATES];
    uint32_t candidateCount = 0;
    int      nameFound = 0;
    uint32_t adjustedFirstPositionalArgIndex = firstPositionalArgIndex;

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

    H2TCGatherCallCandidates(c, nameStart, nameEnd, candidates, &candidateCount, &nameFound);
    return H2TCResolveCallFromCandidates(
        c,
        candidates,
        candidateCount,
        nameFound,
        callArgs,
        argCount,
        adjustedFirstPositionalArgIndex,
        autoRefFirstArg,
        outFuncIndex,
        outMutRefTempArgNode);
}

int H2TCResolveCallByPkgMethod(
    H2TypeCheckCtx*        c,
    uint32_t               pkgStart,
    uint32_t               pkgEnd,
    uint32_t               methodStart,
    uint32_t               methodEnd,
    const H2TCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode) {
    int32_t  candidates[H2TC_MAX_CALL_CANDIDATES];
    uint32_t candidateCount = 0;
    int      nameFound = 0;
    uint32_t adjustedFirstPositionalArgIndex = firstPositionalArgIndex;

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

    H2TCGatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, methodStart, methodEnd, candidates, &candidateCount, &nameFound);
    return H2TCResolveCallFromCandidates(
        c,
        candidates,
        candidateCount,
        nameFound,
        callArgs,
        argCount,
        adjustedFirstPositionalArgIndex,
        autoRefFirstArg,
        outFuncIndex,
        outMutRefTempArgNode);
}

int H2TCResolveDependentPtrReturnForCall(
    H2TypeCheckCtx* c, int32_t fnIndex, int32_t argNode, int32_t* outType) {
    const H2TCFunction* fn;
    int32_t             returnType;
    int32_t             reflectedType = -1;
    if (c == NULL || outType == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return -1;
    }
    fn = &c->funcs[fnIndex];
    if (fn->paramCount != 1 || fn->paramTypeStart >= c->funcParamLen
        || c->funcParamTypes[fn->paramTypeStart] != c->typeType)
    {
        return 0;
    }
    returnType = fn->returnType;
    if (returnType < 0 || (uint32_t)returnType >= c->typeLen
        || c->types[returnType].kind != H2TCType_PTR
        || c->types[returnType].baseType != c->typeType)
    {
        return 0;
    }
    if (H2TCResolveReflectedTypeValueExpr(c, argNode, &reflectedType) != 0) {
        return 0;
    }
    reflectedType = H2TCInternPtrType(
        c, reflectedType, c->ast->nodes[argNode].start, c->ast->nodes[argNode].end);
    if (reflectedType < 0) {
        return -1;
    }
    *outType = reflectedType;
    return 1;
}

H2_API_END
