#include "internal.h"
#include "../mir_exec.h"
#include "../mir_lower_stmt.h"

HOP_API_BEGIN

static int HOPTCFailGenericTypeArgArity(
    HOPTypeCheckCtx* c,
    int32_t          nodeId,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    uint16_t         expected,
    uint16_t         got) {
    char         detailBuf[256];
    char         expectedBuf[16];
    char         gotBuf[16];
    HOPTCTextBuf detailText;
    HOPTCTextBuf expectedText;
    HOPTCTextBuf gotText;
    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufInit(&expectedText, expectedBuf, (uint32_t)sizeof(expectedBuf));
    HOPTCTextBufInit(&gotText, gotBuf, (uint32_t)sizeof(gotBuf));
    HOPTCTextBufAppendU32(&expectedText, expected);
    HOPTCTextBufAppendU32(&gotText, got);
    HOPTCTextBufAppendCStr(&detailText, "generic type '");
    HOPTCTextBufAppendSlice(&detailText, c->src, nameStart, nameEnd);
    HOPTCTextBufAppendCStr(&detailText, "' expects ");
    HOPTCTextBufAppendCStr(&detailText, expectedBuf);
    HOPTCTextBufAppendCStr(&detailText, " type arguments, got ");
    HOPTCTextBufAppendCStr(&detailText, gotBuf);
    return HOPTCFailDiagText(c, nodeId, HOPDiag_GENERIC_TYPE_ARITY_MISMATCH, detailBuf);
}

static int HOPTCFailGenericTypeArgsRequired(
    HOPTypeCheckCtx* c, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd) {
    char         detailBuf[256];
    HOPTCTextBuf detailText;
    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufAppendCStr(&detailText, "generic type '");
    HOPTCTextBufAppendSlice(&detailText, c->src, nameStart, nameEnd);
    HOPTCTextBufAppendCStr(&detailText, "' requires explicit type arguments");
    return HOPTCFailDiagText(c, nodeId, HOPDiag_GENERIC_TYPE_ARGS_REQUIRED, detailBuf);
}

static void HOPTCResolveMirSetReasonCb(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* reason) {
    HOPTCConstSetReason((HOPTCConstEvalCtx*)ctx, start, end, reason);
}

static int HOPTCTryMirConstBlock(
    HOPTCConstEvalCtx* evalCtx,
    int32_t            blockNode,
    HOPCTFEValue*      outValue,
    int*               outDidReturn,
    int*               outIsConst,
    int*               outSupported) {
    HOPTypeCheckCtx*      c;
    HOPMirProgram         program = { 0 };
    HOPMirExecEnv         env = { 0 };
    HOPMirLowerOptions    options = { 0 };
    HOPTCMirConstLowerCtx lowerCtx;
    uint32_t              mirFnIndex = UINT32_MAX;
    int                   supported = 0;
    int                   mirIsConst = 0;
    int                   rewriteRc;
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
    if (HOPTCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    options.lowerConstExpr = HOPTCMirConstLowerConstExpr;
    options.lowerConstExprCtx = evalCtx;
    if (HOPMirLowerAppendSimpleFunctionWithOptions(
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
        HOPTCMirConstAdoptLowerDiagReason(evalCtx, c->diag);
        return 0;
    }
    rewriteRc = HOPTCMirConstRewriteDirectCalls(&lowerCtx, mirFnIndex, blockNode);
    if (rewriteRc < 0) {
        return -1;
    }
    if (rewriteRc == 0) {
        return 0;
    }
    HOPMirProgramBuilderFinish(&lowerCtx.builder, &program);
    env.src = c->src;
    env.resolveIdent = HOPTCResolveConstIdent;
    env.resolveCallPre = HOPTCResolveConstCallMirPre;
    env.resolveCall = HOPTCResolveConstCallMir;
    env.resolveCtx = evalCtx;
    env.zeroInitLocal = HOPTCMirConstZeroInitLocal;
    env.zeroInitCtx = evalCtx;
    env.coerceValueForType = HOPTCMirConstCoerceValueForType;
    env.coerceValueCtx = evalCtx;
    env.indexValue = HOPTCMirConstIndexValue;
    env.indexValueCtx = evalCtx;
    env.sequenceLen = HOPTCMirConstSequenceLen;
    env.sequenceLenCtx = evalCtx;
    env.iterInit = HOPTCMirConstIterInit;
    env.iterInitCtx = evalCtx;
    env.iterNext = HOPTCMirConstIterNext;
    env.iterNextCtx = evalCtx;
    env.aggGetField = HOPTCMirConstAggGetField;
    env.aggGetFieldCtx = evalCtx;
    env.aggAddrField = HOPTCMirConstAggAddrField;
    env.aggAddrFieldCtx = evalCtx;
    env.makeTuple = HOPTCMirConstMakeTuple;
    env.makeTupleCtx = evalCtx;
    env.bindFrame = HOPTCMirConstBindFrame;
    env.unbindFrame = HOPTCMirConstUnbindFrame;
    env.frameCtx = evalCtx;
    env.setReason = HOPTCResolveMirSetReasonCb;
    env.setReasonCtx = evalCtx;
    env.backwardJumpLimit = HOPTC_CONST_FOR_MAX_ITERS;
    env.diag = c->diag;
    if (!HOPMirProgramNeedsDynamicResolution(&program)) {
        HOPMirExecEnvDisableDynamicResolution(&env);
    }
    if (HOPMirEvalFunction(c->arena, &program, mirFnIndex, NULL, 0, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    *outIsConst = mirIsConst;
    if (mirIsConst) {
        *outDidReturn = outValue->kind != HOPCTFEValue_INVALID;
    }
    return 0;
}

static void HOPTCInitConstEvalCtxFromParent(
    HOPTypeCheckCtx* c, const HOPTCConstEvalCtx* _Nullable parent, HOPTCConstEvalCtx* outCtx) {
    if (outCtx == NULL) {
        return;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    outCtx->tc = c;
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
    outCtx->callPackParamNameStart = parent->callPackParamNameStart;
    outCtx->callPackParamNameEnd = parent->callPackParamNameEnd;
    outCtx->fnDepth = parent->fnDepth;
    memcpy(outCtx->fnStack, parent->fnStack, sizeof(outCtx->fnStack));
}

int HOPTCResolveAnonAggregateTypeNode(
    HOPTypeCheckCtx* c, int32_t nodeId, int isUnion, int32_t* outType) {
    int32_t           fieldNode = HOPAstFirstChild(c->ast, nodeId);
    HOPTCAnonFieldSig fieldSigs[HOPTC_MAX_ANON_FIELDS];
    uint32_t          fieldCount = 0;

    while (fieldNode >= 0) {
        const HOPAstNode* field = &c->ast->nodes[fieldNode];
        int32_t           typeNode;
        int32_t           typeId;
        uint32_t          i;
        if (field->kind != HOPAst_FIELD) {
            return HOPTCFailNode(c, fieldNode, HOPDiag_EXPECTED_TYPE);
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
                return HOPTCFailDuplicateDefinition(
                    c,
                    field->dataStart,
                    field->dataEnd,
                    fieldSigs[i].nameStart,
                    fieldSigs[i].nameEnd);
            }
        }
        typeNode = HOPAstFirstChild(c->ast, fieldNode);
        if (typeNode < 0) {
            return HOPTCFailNode(c, fieldNode, HOPDiag_EXPECTED_TYPE);
        }
        if (c->ast->nodes[typeNode].kind == HOPAst_TYPE_VARRAY) {
            return HOPTCFailNode(c, typeNode, HOPDiag_TYPE_MISMATCH);
        }
        if (HOPTCResolveTypeNode(c, typeNode, &typeId) != 0) {
            return -1;
        }
        fieldSigs[fieldCount].nameStart = field->dataStart;
        fieldSigs[fieldCount].nameEnd = field->dataEnd;
        fieldSigs[fieldCount].typeId = typeId;
        fieldCount++;
        fieldNode = HOPAstNextSibling(c->ast, fieldNode);
    }

    {
        int32_t typeId = HOPTCInternAnonAggregateType(
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

int HOPTCResolveAliasTypeId(HOPTypeCheckCtx* c, int32_t typeId) {
    HOPTCType*        t;
    int32_t           targetNode;
    int32_t           targetType = -1;
    const HOPAstNode* decl;

    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return HOPTCFailSpan(c, HOPDiag_UNKNOWN_TYPE, 0, 0);
    }
    t = &c->types[typeId];
    if (t->kind != HOPTCType_ALIAS) {
        return 0;
    }
    if ((t->flags & HOPTCTypeFlag_ALIAS_RESOLVED) != 0) {
        return 0;
    }
    if ((t->flags & HOPTCTypeFlag_ALIAS_RESOLVING) != 0) {
        return HOPTCFailNode(c, t->declNode, HOPDiag_TYPE_MISMATCH);
    }
    if (t->declNode < 0 || (uint32_t)t->declNode >= c->ast->len) {
        return HOPTCFailSpan(c, HOPDiag_UNKNOWN_TYPE, 0, 0);
    }

    decl = &c->ast->nodes[t->declNode];
    targetNode = HOPAstFirstChild(c->ast, t->declNode);
    if (targetNode < 0) {
        return HOPTCFailNode(c, t->declNode, HOPDiag_EXPECTED_TYPE);
    }

    t->flags |= HOPTCTypeFlag_ALIAS_RESOLVING;
    if (HOPTCResolveTypeNode(c, targetNode, &targetType) != 0) {
        t->flags &= (uint16_t)~HOPTCTypeFlag_ALIAS_RESOLVING;
        return -1;
    }
    if (targetType < 0 || (uint32_t)targetType >= c->typeLen || targetType == typeId) {
        t->flags &= (uint16_t)~HOPTCTypeFlag_ALIAS_RESOLVING;
        return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, decl->start, decl->end);
    }
    if (c->types[targetType].kind == HOPTCType_ALIAS && HOPTCResolveAliasTypeId(c, targetType) != 0)
    {
        t->flags &= (uint16_t)~HOPTCTypeFlag_ALIAS_RESOLVING;
        return -1;
    }

    t->baseType = targetType;
    t->flags &= (uint16_t)~HOPTCTypeFlag_ALIAS_RESOLVING;
    t->flags |= HOPTCTypeFlag_ALIAS_RESOLVED;
    return 0;
}

int32_t HOPTCResolveAliasBaseType(HOPTypeCheckCtx* c, int32_t typeId) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == HOPTCType_ALIAS
           && depth++ <= c->typeLen)
    {
        if (HOPTCResolveAliasTypeId(c, typeId) != 0) {
            return -1;
        }
        typeId = c->types[typeId].baseType;
    }
    return typeId;
}

uint16_t HOPTCDeclTypeParamCount(HOPTypeCheckCtx* c, int32_t declNode) {
    int32_t  child;
    uint16_t count = 0;
    if (c == NULL || declNode < 0 || (uint32_t)declNode >= c->ast->len) {
        return 0;
    }
    child = HOPAstFirstChild(c->ast, declNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
            count++;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return count;
}

int32_t HOPTCDeclTypeParamIndex(
    HOPTypeCheckCtx* c, int32_t declNode, uint32_t nameStart, uint32_t nameEnd) {
    int32_t  child;
    uint16_t index = 0;
    if (c == NULL || declNode < 0 || (uint32_t)declNode >= c->ast->len) {
        return -1;
    }
    child = HOPAstFirstChild(c->ast, declNode);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_TYPE_PARAM) {
            if (HOPNameEqSlice(c->src, n->dataStart, n->dataEnd, nameStart, nameEnd)) {
                return (int32_t)index;
            }
            index++;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return -1;
}

int HOPTCAppendDeclTypeParamPlaceholders(
    HOPTypeCheckCtx* c, int32_t declNode, uint32_t* outStart, uint16_t* outCount) {
    int32_t  child;
    uint16_t count = HOPTCDeclTypeParamCount(c, declNode);
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
        return HOPTCFailNode(c, declNode, HOPDiag_ARENA_OOM);
    }
    child = HOPAstFirstChild(c->ast, declNode);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_TYPE_PARAM) {
            HOPTCType t;
            int32_t   typeId;
            memset(&t, 0, sizeof(t));
            t.kind = HOPTCType_TYPE_PARAM;
            t.baseType = -1;
            t.declNode = declNode;
            t.funcIndex = -1;
            t.nameStart = n->dataStart;
            t.nameEnd = n->dataEnd;
            typeId = HOPTCAddType(c, &t, n->start, n->end);
            if (typeId < 0) {
                return -1;
            }
            c->genericArgTypes[c->genericArgLen++] = typeId;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

static int HOPTCResolveActiveDeclTypeParamType(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    int32_t idx;
    if (c == NULL || outType == NULL || c->activeGenericDeclNode < 0) {
        return 0;
    }
    idx = HOPTCDeclTypeParamIndex(c, c->activeGenericDeclNode, nameStart, nameEnd);
    if (idx < 0 || (uint32_t)idx >= c->activeGenericArgCount) {
        return 0;
    }
    *outType = c->genericArgTypes[c->activeGenericArgStart + (uint32_t)idx];
    return 1;
}

int HOPTCFnNodeHasTypeParamName(
    HOPTypeCheckCtx* c, int32_t fnNode, uint32_t nameStart, uint32_t nameEnd) {
    int32_t child;
    if (c == NULL || fnNode < 0 || (uint32_t)fnNode >= c->ast->len) {
        return 0;
    }
    if (c->ast->nodes[fnNode].kind != HOPAst_FN) {
        return 0;
    }
    child = HOPAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
            int32_t           typeNode = HOPAstFirstChild(c->ast, child);
            const HOPAstNode* t = typeNode >= 0 ? &c->ast->nodes[typeNode] : NULL;
            if (t != NULL && t->kind == HOPAst_TYPE_NAME
                && HOPNameEqLiteral(c->src, t->dataStart, t->dataEnd, "type")
                && HOPNameEqSlice(c->src, n->dataStart, n->dataEnd, nameStart, nameEnd))
            {
                return 1;
            }
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return 0;
}

int HOPTCResolveActiveTypeParamType(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    if (c == NULL || outType == NULL) {
        return 0;
    }
    if (HOPTCResolveActiveDeclTypeParamType(c, nameStart, nameEnd, outType)) {
        return 1;
    }
    if (c->currentFunctionIndex >= 0 && (uint32_t)c->currentFunctionIndex < c->funcLen) {
        const HOPTCFunction* fn = &c->funcs[c->currentFunctionIndex];
        uint32_t             p;
        for (p = 0; p < fn->paramCount; p++) {
            uint32_t paramIndex = fn->paramTypeStart + p;
            if (c->funcParamTypes[paramIndex] == c->typeType
                && HOPNameEqSlice(
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
        && HOPTCFnNodeHasTypeParamName(c, c->activeTypeParamFnNode, nameStart, nameEnd))
    {
        *outType = c->typeType;
        return 1;
    }
    return 0;
}

int HOPTCResolveTypeNode(HOPTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const HOPAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return HOPTCFailSpan(c, HOPDiag_EXPECTED_TYPE, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case HOPAst_TYPE_NAME: {
            int32_t  firstArgNode = HOPAstFirstChild(c->ast, nodeId);
            uint16_t argCount = 0;
            if (HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "anytype")) {
                if (!c->allowAnytypeParamType) {
                    return HOPTCFailSpan(
                        c, HOPDiag_ANYTYPE_INVALID_POSITION, n->dataStart, n->dataEnd);
                }
                if (firstArgNode >= 0) {
                    return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, n->start, n->end);
                }
                *outType = c->typeAnytype;
                return 0;
            }
            int32_t typeId = HOPTCFindBuiltinType(c, n->dataStart, n->dataEnd);
            if (typeId >= 0) {
                if ((typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat)
                    && !c->allowConstNumericTypeName)
                {
                    return HOPTCFailSpan(
                        c, HOPDiag_CONST_NUMERIC_INVALID_POSITION, n->dataStart, n->dataEnd);
                }
                if (firstArgNode >= 0) {
                    return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, n->start, n->end);
                }
                *outType = typeId;
                return 0;
            }
            {
                int32_t resolvedType = HOPTCResolveTypeNamePath(
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
                        firstArgNode = HOPAstNextSibling(c->ast, firstArgNode);
                    }
                    if (namedIndex >= 0) {
                        const HOPTCNamedType* nt = &c->namedTypes[(uint32_t)namedIndex];
                        if (nt->templateRootNamedIndex >= 0) {
                            namedIndex = nt->templateRootNamedIndex;
                            resolvedType = c->namedTypes[(uint32_t)namedIndex].typeId;
                            nt = &c->namedTypes[(uint32_t)namedIndex];
                        }
                        if (nt->templateArgCount > 0 || argCount > 0) {
                            int32_t  args[64];
                            int32_t  argNode = HOPAstFirstChild(c->ast, nodeId);
                            uint16_t ai = 0;
                            if (argCount == 0 && nt->templateArgCount > 0) {
                                return HOPTCFailGenericTypeArgsRequired(
                                    c, nodeId, n->dataStart, n->dataEnd);
                            }
                            if (nt->templateArgCount != argCount
                                || argCount > (uint16_t)(sizeof(args) / sizeof(args[0])))
                            {
                                return HOPTCFailGenericTypeArgArity(
                                    c,
                                    nodeId,
                                    n->dataStart,
                                    n->dataEnd,
                                    nt->templateArgCount,
                                    argCount);
                            }
                            while (argNode >= 0) {
                                if (HOPTCResolveTypeNode(c, argNode, &args[ai]) != 0) {
                                    return -1;
                                }
                                ai++;
                                argNode = HOPAstNextSibling(c->ast, argNode);
                            }
                            resolvedType = HOPTCInstantiateNamedType(
                                c, resolvedType, args, argCount);
                            if (resolvedType < 0) {
                                return -1;
                            }
                        }
                    } else if (argCount > 0) {
                        return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, n->start, n->end);
                    }
                    *outType = resolvedType;
                    if (*outType >= 0 && (uint32_t)*outType < c->typeLen
                        && c->types[*outType].kind == HOPTCType_ALIAS
                        && HOPTCResolveAliasTypeId(c, *outType) != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
            }
            {
                int32_t activeTypeParamType = -1;
                if (HOPTCResolveActiveTypeParamType(
                        c, n->dataStart, n->dataEnd, &activeTypeParamType))
                {
                    if (HOPAstFirstChild(c->ast, nodeId) >= 0) {
                        return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, n->start, n->end);
                    }
                    *outType = activeTypeParamType;
                    return 0;
                }
            }
            {
                int32_t resolvedType = HOPTCFindBuiltinQualifiedNamedType(
                    c, n->dataStart, n->dataEnd);
                if (resolvedType >= 0) {
                    if (HOPAstFirstChild(c->ast, nodeId) >= 0) {
                        return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, n->start, n->end);
                    }
                    *outType = resolvedType;
                    if (*outType >= 0 && (uint32_t)*outType < c->typeLen
                        && c->types[*outType].kind == HOPTCType_ALIAS
                        && HOPTCResolveAliasTypeId(c, *outType) != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
            }
            {
                int32_t varLikeNameIndex = -1;
                int32_t varLikeNode = HOPTCFindTopLevelVarLikeNode(
                    c, n->dataStart, n->dataEnd, &varLikeNameIndex);
                if (varLikeNode >= 0 && c->ast->nodes[varLikeNode].kind == HOPAst_CONST) {
                    int32_t           resolvedConstType = -1;
                    HOPTCConstEvalCtx evalCtx;
                    HOPCTFEValue      value;
                    int               isConst = 0;
                    int32_t           reflectedType = -1;
                    if (HOPTCTypeTopLevelVarLikeNode(
                            c, varLikeNode, varLikeNameIndex, &resolvedConstType)
                        != 0)
                    {
                        return -1;
                    }
                    if (resolvedConstType == c->typeType) {
                        memset(&evalCtx, 0, sizeof(evalCtx));
                        evalCtx.tc = c;
                        if (HOPTCEvalTopLevelConstNodeAt(
                                c, &evalCtx, varLikeNode, varLikeNameIndex, &value, &isConst)
                            != 0)
                        {
                            return -1;
                        }
                        if (isConst && value.kind == HOPCTFEValue_TYPE
                            && HOPTCDecodeTypeTag(c, value.typeTag, &reflectedType) == 0)
                        {
                            if (HOPAstFirstChild(c->ast, nodeId) >= 0) {
                                return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, n->start, n->end);
                            }
                            *outType = reflectedType;
                            return 0;
                        }
                    }
                }
            }
            return HOPTCFailSpan(c, HOPDiag_UNKNOWN_TYPE, n->dataStart, n->dataEnd);
        }
        case HOPAst_TYPE_PTR: {
            int32_t           child = HOPAstFirstChild(c->ast, nodeId);
            int32_t           baseType;
            int32_t           ptrType;
            const HOPAstNode* childNode;
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
            }
            childNode = &c->ast->nodes[child];
            if (childNode->kind == HOPAst_TYPE_SLICE || childNode->kind == HOPAst_TYPE_MUTSLICE) {
                int32_t elemNode = HOPAstFirstChild(c->ast, child);
                int32_t elemType;
                int32_t sliceType;
                if (elemNode < 0) {
                    return HOPTCFailNode(c, child, HOPDiag_EXPECTED_TYPE);
                }
                if (HOPTCResolveTypeNode(c, elemNode, &elemType) != 0) {
                    return -1;
                }
                sliceType = HOPTCInternSliceType(c, elemType, 1, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                ptrType = HOPTCInternPtrType(c, sliceType, n->start, n->end);
                if (ptrType < 0) {
                    return -1;
                }
                *outType = ptrType;
                return 0;
            }
            if (HOPTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            if (baseType >= 0 && (uint32_t)baseType < c->typeLen
                && c->types[baseType].kind == HOPTCType_SLICE
                && !HOPTCTypeIsMutable(&c->types[baseType]))
            {
                int32_t mutableSliceType = HOPTCInternSliceType(
                    c, c->types[baseType].baseType, 1, n->start, n->end);
                if (mutableSliceType < 0) {
                    return -1;
                }
                baseType = mutableSliceType;
            }
            ptrType = HOPTCInternPtrType(c, baseType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF: {
            int32_t           child = HOPAstFirstChild(c->ast, nodeId);
            int32_t           baseType;
            int32_t           refType;
            const HOPAstNode* childNode;
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
            }
            childNode = &c->ast->nodes[child];
            if (childNode->kind == HOPAst_TYPE_SLICE || childNode->kind == HOPAst_TYPE_MUTSLICE) {
                int32_t elemNode = HOPAstFirstChild(c->ast, child);
                int32_t elemType;
                int32_t sliceType;
                if (elemNode < 0) {
                    return HOPTCFailNode(c, child, HOPDiag_EXPECTED_TYPE);
                }
                if (HOPTCResolveTypeNode(c, elemNode, &elemType) != 0) {
                    return -1;
                }
                sliceType = HOPTCInternSliceType(c, elemType, 0, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                refType = HOPTCInternRefType(
                    c, sliceType, n->kind == HOPAst_TYPE_MUTREF, n->start, n->end);
                if (refType < 0) {
                    return -1;
                }
                *outType = refType;
                return 0;
            }
            if (HOPTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            if (baseType >= 0 && (uint32_t)baseType < c->typeLen
                && c->types[baseType].kind == HOPTCType_SLICE
                && HOPTCTypeIsMutable(&c->types[baseType]))
            {
                int32_t readOnlySliceType = HOPTCInternSliceType(
                    c, c->types[baseType].baseType, 0, n->start, n->end);
                if (readOnlySliceType < 0) {
                    return -1;
                }
                baseType = readOnlySliceType;
            }
            refType = HOPTCInternRefType(
                c, baseType, n->kind == HOPAst_TYPE_MUTREF, n->start, n->end);
            if (refType < 0) {
                return -1;
            }
            *outType = refType;
            return 0;
        }
        case HOPAst_TYPE_ARRAY: {
            int32_t  child = HOPAstFirstChild(c->ast, nodeId);
            int32_t  lenNode = child >= 0 ? HOPAstNextSibling(c->ast, child) : -1;
            int32_t  baseType;
            int32_t  arrayType;
            int64_t  lenValue = 0;
            int      lenIsConst = 0;
            uint32_t arrayLen;
            if (child < 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
            }
            if (lenNode >= 0) {
                int32_t lenType;
                if (HOPAstNextSibling(c->ast, lenNode) >= 0) {
                    return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
                }
                if (HOPTCTypeExpr(c, lenNode, &lenType) != 0) {
                    return -1;
                }
                if (!HOPTCIsIntegerType(c, lenType)) {
                    return HOPTCFailNode(c, lenNode, HOPDiag_TYPE_MISMATCH);
                }
                if (HOPTCConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0) {
                    return -1;
                }
                if (!lenIsConst) {
                    int rc = HOPTCFailNode(c, lenNode, HOPDiag_ARRAY_LEN_CONST_REQUIRED);
                    HOPTCAttachConstEvalReason(c);
                    return rc;
                }
                if (lenValue < 0 || lenValue > (int64_t)UINT32_MAX) {
                    return HOPTCFailNode(c, lenNode, HOPDiag_ARRAY_LEN_CONST_REQUIRED);
                }
                arrayLen = (uint32_t)lenValue;
            } else if (HOPTCParseArrayLen(c, n, &arrayLen) != 0) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
            }
            if (HOPTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            arrayType = HOPTCInternArrayType(c, baseType, arrayLen, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            *outType = arrayType;
            return 0;
        }
        case HOPAst_TYPE_SLICE:
        case HOPAst_TYPE_MUTSLICE: {
            int32_t child = HOPAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t sliceType;
            if (HOPTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            sliceType = HOPTCInternSliceType(
                c, baseType, n->kind == HOPAst_TYPE_MUTSLICE, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            *outType = sliceType;
            return 0;
        }
        case HOPAst_TYPE_OPTIONAL: {
            int32_t child = HOPAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t optType;
            if (HOPTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            optType = HOPTCInternOptionalType(c, baseType, n->start, n->end);
            if (optType < 0) {
                return -1;
            }
            *outType = optType;
            return 0;
        }
        case HOPAst_TYPE_FN: {
            int32_t  child = HOPAstFirstChild(c->ast, nodeId);
            int32_t  returnType = c->typeVoid;
            uint32_t paramCount = 0;
            int      isVariadic = 0;
            int      sawReturnType = 0;
            int32_t  savedParamTypes[HOPTC_MAX_CALL_ARGS];
            uint8_t  savedParamFlags[HOPTC_MAX_CALL_ARGS];
            while (child >= 0) {
                const HOPAstNode* ch = &c->ast->nodes[child];
                if (ch->flags == 1) {
                    uint32_t i;
                    if (sawReturnType) {
                        return HOPTCFailNode(c, child, HOPDiag_EXPECTED_TYPE);
                    }
                    if (paramCount > HOPTC_MAX_CALL_ARGS) {
                        return HOPTCFailNode(c, child, HOPDiag_ARENA_OOM);
                    }
                    for (i = 0; i < paramCount; i++) {
                        savedParamTypes[i] = c->scratchParamTypes[i];
                        savedParamFlags[i] = c->scratchParamFlags[i];
                    }
                    c->allowConstNumericTypeName = 1;
                    if (HOPTCResolveTypeNode(c, child, &returnType) != 0) {
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowConstNumericTypeName = 0;
                    for (i = 0; i < paramCount; i++) {
                        c->scratchParamTypes[i] = savedParamTypes[i];
                        c->scratchParamFlags[i] = savedParamFlags[i];
                    }
                    if (HOPTCTypeContainsVarSizeByValue(c, returnType)) {
                        return HOPTCFailVarSizeByValue(
                            c, child, returnType, "function-type return position");
                    }
                    sawReturnType = 1;
                } else {
                    int32_t paramType;
                    if (paramCount >= c->scratchParamCap) {
                        return HOPTCFailNode(c, child, HOPDiag_ARENA_OOM);
                    }
                    c->allowAnytypeParamType = 1;
                    c->allowConstNumericTypeName = 1;
                    if (HOPTCResolveTypeNode(c, child, &paramType) != 0) {
                        c->allowAnytypeParamType = 0;
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowAnytypeParamType = 0;
                    c->allowConstNumericTypeName = 0;
                    if ((paramType == c->typeUntypedInt || paramType == c->typeUntypedFloat)
                        && (ch->flags & HOPAstFlag_PARAM_CONST) == 0)
                    {
                        return HOPTCFailSpan(
                            c,
                            HOPDiag_CONST_NUMERIC_PARAM_REQUIRES_CONST,
                            ch->dataStart,
                            ch->dataEnd);
                    }
                    if (HOPTCTypeContainsVarSizeByValue(c, paramType)) {
                        return HOPTCFailVarSizeByValue(
                            c, child, paramType, "function-type parameter position");
                    }
                    if ((ch->flags & HOPAstFlag_PARAM_VARIADIC) != 0) {
                        int32_t sliceType;
                        if (isVariadic) {
                            return HOPTCFailNode(c, child, HOPDiag_VARIADIC_PARAM_NOT_LAST);
                        }
                        if (paramType != c->typeAnytype) {
                            sliceType = HOPTCInternSliceType(c, paramType, 0, ch->start, ch->end);
                            if (sliceType < 0) {
                                return -1;
                            }
                            paramType = sliceType;
                        }
                        isVariadic = 1;
                    } else if (isVariadic) {
                        return HOPTCFailNode(c, child, HOPDiag_VARIADIC_PARAM_NOT_LAST);
                    }
                    c->scratchParamTypes[paramCount++] = paramType;
                    c->scratchParamFlags[paramCount - 1u] =
                        (ch->flags & HOPAstFlag_PARAM_CONST) != 0 ? HOPTCFuncParamFlag_CONST : 0u;
                }
                child = HOPAstNextSibling(c->ast, child);
            }
            {
                int32_t fnType = HOPTCInternFunctionType(
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
        case HOPAst_TYPE_TUPLE: {
            int32_t  child = HOPAstFirstChild(c->ast, nodeId);
            uint32_t elemCount = 0;
            while (child >= 0) {
                int32_t elemType;
                if (elemCount >= c->scratchParamCap) {
                    return HOPTCFailNode(c, child, HOPDiag_ARENA_OOM);
                }
                if (HOPTCResolveTypeNode(c, child, &elemType) != 0) {
                    return -1;
                }
                if (HOPTCTypeContainsVarSizeByValue(c, elemType)) {
                    return HOPTCFailVarSizeByValue(c, child, elemType, "tuple element position");
                }
                c->scratchParamTypes[elemCount++] = elemType;
                child = HOPAstNextSibling(c->ast, child);
            }
            if (elemCount < 2u) {
                return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
            }
            {
                int32_t tupleType = HOPTCInternTupleType(
                    c, c->scratchParamTypes, elemCount, n->start, n->end);
                if (tupleType < 0) {
                    return -1;
                }
                *outType = tupleType;
                return 0;
            }
        }
        case HOPAst_TYPE_ANON_STRUCT:
            return HOPTCResolveAnonAggregateTypeNode(c, nodeId, 0, outType);
        case HOPAst_TYPE_ANON_UNION:
            return HOPTCResolveAnonAggregateTypeNode(c, nodeId, 1, outType);
        case HOPAst_TYPE_VARRAY: return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
        default:                 return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
    }
}

int HOPTCAddNamedType(HOPTypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId, int32_t* outTypeId) {
    const HOPAstNode* node = &c->ast->nodes[nodeId];
    HOPTCType         t;
    int32_t           typeId;
    int32_t           dupNamedIndex;
    uint32_t          idx;
    uint32_t          templateArgStart = 0;
    uint16_t          templateArgCount = 0;

    if (node->dataEnd <= node->dataStart) {
        return HOPTCFailNode(c, nodeId, HOPDiag_EXPECTED_TYPE);
    }

    dupNamedIndex = HOPTCFindNamedTypeIndexOwned(c, ownerTypeId, node->dataStart, node->dataEnd);
    if (dupNamedIndex >= 0) {
        return HOPTCFailDuplicateDefinition(
            c,
            node->dataStart,
            node->dataEnd,
            c->namedTypes[(uint32_t)dupNamedIndex].nameStart,
            c->namedTypes[(uint32_t)dupNamedIndex].nameEnd);
    }

    t.kind = node->kind == HOPAst_TYPE_ALIAS ? HOPTCType_ALIAS : HOPTCType_NAMED;
    t.builtin = HOPBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = nodeId;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = node->dataStart;
    t.nameEnd = node->dataEnd;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    typeId = HOPTCAddType(c, &t, node->start, node->end);
    if (typeId < 0) {
        return -1;
    }

    if (c->namedTypeLen >= c->namedTypeCap) {
        return HOPTCFailNode(c, nodeId, HOPDiag_ARENA_OOM);
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
    if (HOPTCAppendDeclTypeParamPlaceholders(c, nodeId, &templateArgStart, &templateArgCount) != 0)
    {
        return -1;
    }
    c->namedTypes[idx].templateArgStart = templateArgStart;
    c->namedTypes[idx].templateArgCount = templateArgCount;
    if (outTypeId != NULL) {
        *outTypeId = typeId;
    }
    return 0;
}

int32_t HOPTCInstantiateNamedType(
    HOPTypeCheckCtx* c, int32_t rootTypeId, const int32_t* argTypes, uint16_t argCount) {
    int32_t          rootNamedIndex = -1;
    uint32_t         i;
    const HOPTCType* rootType;
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
        const HOPTCNamedType* nt = &c->namedTypes[i];
        uint16_t              j;
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
        return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, 0, 0);
    }
    rootType = &c->types[rootTypeId];
    {
        HOPTCType       t = *rootType;
        HOPTCNamedType* nt;
        int32_t         typeId;
        t.fieldStart = 0;
        t.fieldCount = 0;
        t.flags &= (uint16_t)~(HOPTCTypeFlag_ALIAS_RESOLVING | HOPTCTypeFlag_ALIAS_RESOLVED);
        typeId = HOPTCAddType(c, &t, rootType->nameStart, rootType->nameEnd);
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
        if (t.kind == HOPTCType_ALIAS) {
            int32_t baseType = HOPTCSubstituteType(
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
            c->types[typeId].flags |= HOPTCTypeFlag_ALIAS_RESOLVED;
        }
        return typeId;
    }
}

static int HOPTCIsNamedTypeDeclKind(HOPAstKind kind) {
    return kind == HOPAst_STRUCT || kind == HOPAst_UNION || kind == HOPAst_ENUM
        || kind == HOPAst_TYPE_ALIAS;
}

static int HOPTCCollectTypeDeclsFromNodeWithOwner(
    HOPTypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId) {
    HOPAstKind kind = c->ast->nodes[nodeId].kind;
    if (kind == HOPAst_PUB) {
        int32_t ch = HOPAstFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (HOPTCCollectTypeDeclsFromNodeWithOwner(c, ch, ownerTypeId) != 0) {
                return -1;
            }
            ch = HOPAstNextSibling(c->ast, ch);
        }
        return 0;
    }
    if (HOPTCIsNamedTypeDeclKind(kind)) {
        int32_t declaredTypeId = -1;
        if (HOPTCAddNamedType(c, nodeId, ownerTypeId, &declaredTypeId) != 0) {
            return -1;
        }
        if (kind == HOPAst_STRUCT || kind == HOPAst_UNION) {
            int32_t child = HOPAstFirstChild(c->ast, nodeId);
            while (child >= 0) {
                HOPAstKind childKind = c->ast->nodes[child].kind;
                if (HOPTCIsNamedTypeDeclKind(childKind)
                    && HOPTCCollectTypeDeclsFromNodeWithOwner(c, child, declaredTypeId) != 0)
                {
                    return -1;
                }
                child = HOPAstNextSibling(c->ast, child);
            }
        }
        return 0;
    }
    return 0;
}

int HOPTCCollectTypeDeclsFromNode(HOPTypeCheckCtx* c, int32_t nodeId) {
    return HOPTCCollectTypeDeclsFromNodeWithOwner(c, nodeId, -1);
}

int HOPTCIsIntegerType(HOPTypeCheckCtx* c, int32_t typeId) {
    HOPBuiltinKind b;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedInt) {
        return 1;
    }
    if (c->types[typeId].kind != HOPTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == HOPBuiltin_U8 || b == HOPBuiltin_U16 || b == HOPBuiltin_U32 || b == HOPBuiltin_U64
        || b == HOPBuiltin_I8 || b == HOPBuiltin_I16 || b == HOPBuiltin_I32 || b == HOPBuiltin_I64
        || b == HOPBuiltin_USIZE || b == HOPBuiltin_ISIZE;
}

int HOPTCIsConstNumericType(HOPTypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

int HOPTCTypeIsRuneLike(HOPTypeCheckCtx* c, int32_t typeId) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && depth++ <= c->typeLen) {
        if (typeId == c->typeRune) {
            return 1;
        }
        if (c->types[typeId].kind != HOPTCType_ALIAS) {
            break;
        }
        if (HOPTCResolveAliasTypeId(c, typeId) != 0) {
            return 0;
        }
        typeId = c->types[typeId].baseType;
    }
    return 0;
}

uint32_t HOPTCU64BitLen(uint64_t v) {
    uint32_t bits = 0;
    while (v != 0u) {
        v >>= 1u;
        bits++;
    }
    return bits;
}

int HOPTCConstIntFitsType(HOPTypeCheckCtx* c, int64_t value, int32_t typeId) {
    HOPBuiltinKind b;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedInt) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != HOPTCType_BUILTIN)
    {
        return 0;
    }
    b = c->types[typeId].builtin;
    switch (b) {
        case HOPBuiltin_U8:    return value >= 0 && value <= (int64_t)UINT8_MAX;
        case HOPBuiltin_U16:   return value >= 0 && value <= (int64_t)UINT16_MAX;
        case HOPBuiltin_U32:   return value >= 0 && value <= (int64_t)UINT32_MAX;
        case HOPBuiltin_U64:
        case HOPBuiltin_USIZE: return value >= 0;
        case HOPBuiltin_I8:    return value >= (int64_t)INT8_MIN && value <= (int64_t)INT8_MAX;
        case HOPBuiltin_I16:   return value >= (int64_t)INT16_MIN && value <= (int64_t)INT16_MAX;
        case HOPBuiltin_I32:   return value >= (int64_t)INT32_MIN && value <= (int64_t)INT32_MAX;
        case HOPBuiltin_I64:
        case HOPBuiltin_ISIZE: return 1;
        default:               return 0;
    }
}

int HOPTCConstIntFitsFloatType(HOPTypeCheckCtx* c, int64_t value, int32_t typeId) {
    HOPBuiltinKind b;
    uint32_t       precisionBits;
    uint64_t       magnitude;
    uint32_t       bits;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != HOPTCType_BUILTIN)
    {
        return 0;
    }
    b = c->types[typeId].builtin;
    if (b == HOPBuiltin_F32) {
        precisionBits = 23u;
    } else if (b == HOPBuiltin_F64) {
        precisionBits = 53u;
    } else {
        return 0;
    }
    if (value < 0) {
        magnitude = (uint64_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint64_t)value;
    }
    bits = HOPTCU64BitLen(magnitude);
    return bits <= precisionBits;
}

int HOPTCConstFloatFitsType(HOPTypeCheckCtx* c, double value, int32_t typeId) {
    HOPBuiltinKind b;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != HOPTCType_BUILTIN)
    {
        return 0;
    }
    b = c->types[typeId].builtin;
    if (b == HOPBuiltin_F64) {
        return 1;
    }
    if (b != HOPBuiltin_F32) {
        return 0;
    }
    if (value != value) {
        return 1;
    }
    return (double)(float)value == value;
}

int HOPTCFailConstIntRange(
    HOPTypeCheckCtx* c, int32_t nodeId, int64_t value, int32_t expectedType) {
    char         dstTypeBuf[HOPTC_DIAG_TEXT_CAP];
    char         detailBuf[256];
    HOPTCTextBuf dstTypeText;
    HOPTCTextBuf detailText;
    HOPTCSetDiag(
        c->diag, HOPDiag_TYPE_MISMATCH, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
    if (c->diag == NULL) {
        return -1;
    }
    HOPTCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    HOPTCFormatTypeRec(c, expectedType, &dstTypeText, 0);
    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufAppendCStr(&detailText, "constant value 0x");
    HOPTCTextBufAppendHexU64(&detailText, (uint64_t)value);
    HOPTCTextBufAppendCStr(&detailText, " is out of range for ");
    HOPTCTextBufAppendCStr(&detailText, dstTypeBuf);
    c->diag->detail = HOPTCAllocDiagText(c, detailBuf);
    return -1;
}

int HOPTCFailConstFloatRange(HOPTypeCheckCtx* c, int32_t nodeId, int32_t expectedType) {
    char         dstTypeBuf[HOPTC_DIAG_TEXT_CAP];
    char         detailBuf[256];
    HOPTCTextBuf dstTypeText;
    HOPTCTextBuf detailText;
    HOPTCSetDiag(
        c->diag, HOPDiag_TYPE_MISMATCH, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
    if (c->diag == NULL) {
        return -1;
    }
    HOPTCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    HOPTCFormatTypeRec(c, expectedType, &dstTypeText, 0);
    HOPTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    HOPTCTextBufAppendCStr(&detailText, "constant value is not representable for ");
    HOPTCTextBufAppendCStr(&detailText, dstTypeBuf);
    c->diag->detail = HOPTCAllocDiagText(c, detailBuf);
    return -1;
}

int HOPTCIsFloatType(HOPTypeCheckCtx* c, int32_t typeId) {
    HOPBuiltinKind b;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (c->types[typeId].kind != HOPTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == HOPBuiltin_F32 || b == HOPBuiltin_F64;
}

int HOPTCIsNumericType(HOPTypeCheckCtx* c, int32_t typeId) {
    return HOPTCIsIntegerType(c, typeId) || HOPTCIsFloatType(c, typeId);
}

int HOPTCIsBoolType(HOPTypeCheckCtx* c, int32_t typeId) {
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    return typeId == c->typeBool;
}

int HOPTCIsRawptrType(HOPTypeCheckCtx* c, int32_t typeId) {
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    return c != NULL && typeId >= 0 && typeId == c->typeRawptr;
}

int HOPTCIsNamedDeclKind(HOPTypeCheckCtx* c, int32_t typeId, HOPAstKind kind) {
    int32_t declNode;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != HOPTCType_NAMED) {
        return 0;
    }
    declNode = c->types[typeId].declNode;
    return declNode >= 0 && (uint32_t)declNode < c->ast->len
        && c->ast->nodes[declNode].kind == kind;
}

int HOPTCIsStringLikeType(HOPTypeCheckCtx* c, int32_t typeId) {
    int32_t baseType;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind != HOPTCType_PTR && c->types[typeId].kind != HOPTCType_REF) {
        return 0;
    }
    baseType = HOPTCResolveAliasBaseType(c, c->types[typeId].baseType);
    return baseType == c->typeStr;
}

int HOPTCTypeSupportsFmtReflectRec(HOPTypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    uint32_t i;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (HOPTCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case HOPTCType_BUILTIN:
        case HOPTCType_UNTYPED_INT:
        case HOPTCType_UNTYPED_FLOAT:
            return HOPTCIsBoolType(c, typeId) || HOPTCIsNumericType(c, typeId)
                || typeId == c->typeType || HOPTCIsRawptrType(c, typeId);
        case HOPTCType_ARRAY:
        case HOPTCType_SLICE:
            return HOPTCTypeSupportsFmtReflectRec(c, c->types[typeId].baseType, depth + 1u);
        case HOPTCType_PTR:
        case HOPTCType_REF: return 1;
        case HOPTCType_OPTIONAL:
            return HOPTCTypeSupportsFmtReflectRec(c, c->types[typeId].baseType, depth + 1u);
        case HOPTCType_NULL: return 1;
        case HOPTCType_NAMED:
            if (HOPTCIsNamedDeclKind(c, typeId, HOPAst_ENUM)) {
                return 1;
            }
            if (HOPTCIsNamedDeclKind(c, typeId, HOPAst_UNION)) {
                return 0;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!HOPTCTypeSupportsFmtReflectRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case HOPTCType_ANON_STRUCT:
        case HOPTCType_TUPLE:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!HOPTCTypeSupportsFmtReflectRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case HOPTCType_ANON_UNION: return 0;
        default:                   return 0;
    }
}

int HOPTCIsComparableTypeRec(HOPTypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    uint32_t i;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (HOPTCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case HOPTCType_BUILTIN:
        case HOPTCType_UNTYPED_INT:
        case HOPTCType_UNTYPED_FLOAT:
            return HOPTCIsBoolType(c, typeId) || HOPTCIsNumericType(c, typeId)
                || typeId == c->typeType || HOPTCIsRawptrType(c, typeId);
        case HOPTCType_ARRAY:
        case HOPTCType_SLICE:
            return HOPTCIsComparableTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case HOPTCType_PTR:
        case HOPTCType_REF: return 1;
        case HOPTCType_NAMED:
            if (HOPTCIsNamedDeclKind(c, typeId, HOPAst_ENUM)) {
                return 1;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!HOPTCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case HOPTCType_ANON_STRUCT:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!HOPTCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case HOPTCType_ANON_UNION:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!HOPTCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case HOPTCType_OPTIONAL:
            return HOPTCIsComparableTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case HOPTCType_NULL: return 1;
        default:             return 0;
    }
}

int HOPTCIsOrderedTypeRec(HOPTypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (HOPTCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case HOPTCType_BUILTIN:
            return HOPTCIsNumericType(c, typeId) || HOPTCIsRawptrType(c, typeId);
        case HOPTCType_UNTYPED_INT:
        case HOPTCType_UNTYPED_FLOAT: return HOPTCIsNumericType(c, typeId);
        case HOPTCType_ARRAY:
        case HOPTCType_SLICE:
            return HOPTCIsOrderedTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case HOPTCType_PTR:
        case HOPTCType_REF:   return 1;
        case HOPTCType_NAMED: return HOPTCIsNamedDeclKind(c, typeId, HOPAst_ENUM);
        case HOPTCType_OPTIONAL:
            return HOPTCIsOrderedTypeRec(c, c->types[typeId].baseType, depth + 1u);
        default: return 0;
    }
}

int HOPTCIsComparableType(HOPTypeCheckCtx* c, int32_t typeId) {
    return HOPTCIsComparableTypeRec(c, typeId, 0u);
}

int HOPTCIsOrderedType(HOPTypeCheckCtx* c, int32_t typeId) {
    return HOPTCIsOrderedTypeRec(c, typeId, 0u);
}

int HOPTCTypeSupportsLen(HOPTypeCheckCtx* c, int32_t typeId) {
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind == HOPTCType_PACK) {
        return 1;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind == HOPTCType_ARRAY) {
        return 1;
    }
    if (c->types[typeId].kind == HOPTCType_SLICE) {
        return 1;
    }
    if (c->types[typeId].kind == HOPTCType_PTR || c->types[typeId].kind == HOPTCType_REF) {
        int32_t baseType = HOPTCResolveAliasBaseType(c, c->types[typeId].baseType);
        if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
            return 0;
        }
        if (baseType == c->typeStr) {
            return 1;
        }
        return c->types[baseType].kind == HOPTCType_ARRAY
            || c->types[baseType].kind == HOPTCType_SLICE;
    }
    return 0;
}

int HOPTCIsUntyped(HOPTypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

int HOPTCIsTypeNodeKind(HOPAstKind kind) {
    return kind == HOPAst_TYPE_NAME || kind == HOPAst_TYPE_PTR || kind == HOPAst_TYPE_REF
        || kind == HOPAst_TYPE_MUTREF || kind == HOPAst_TYPE_ARRAY || kind == HOPAst_TYPE_VARRAY
        || kind == HOPAst_TYPE_SLICE || kind == HOPAst_TYPE_MUTSLICE || kind == HOPAst_TYPE_OPTIONAL
        || kind == HOPAst_TYPE_FN || kind == HOPAst_TYPE_ANON_STRUCT
        || kind == HOPAst_TYPE_ANON_UNION || kind == HOPAst_TYPE_TUPLE;
}

int HOPTCConcretizeInferredType(HOPTypeCheckCtx* c, int32_t typeId, int32_t* outType) {
    const HOPTCType* t;
    uint32_t         i;
    if (typeId == c->typeUntypedInt) {
        int32_t t = HOPTCFindBuiltinByKind(c, HOPBuiltin_ISIZE);
        if (t < 0) {
            return HOPTCFailSpan(c, HOPDiag_UNKNOWN_TYPE, 0, 0);
        }
        *outType = t;
        return 0;
    }
    if (typeId == c->typeUntypedFloat) {
        int32_t t = HOPTCFindBuiltinByKind(c, HOPBuiltin_F64);
        if (t < 0) {
            return HOPTCFailSpan(c, HOPDiag_UNKNOWN_TYPE, 0, 0);
        }
        *outType = t;
        return 0;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        *outType = typeId;
        return 0;
    }
    t = &c->types[typeId];
    if (t->kind == HOPTCType_TUPLE) {
        int32_t elems[256];
        int     changed = 0;
        if (t->fieldCount > (uint16_t)(sizeof(elems) / sizeof(elems[0]))) {
            return HOPTCFailSpan(c, HOPDiag_ARENA_OOM, 0, 0);
        }
        for (i = 0; i < t->fieldCount; i++) {
            int32_t elem = c->funcParamTypes[t->fieldStart + i];
            int32_t concreteElem = elem;
            if (HOPTCConcretizeInferredType(c, elem, &concreteElem) != 0) {
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
        *outType = HOPTCInternTupleType(c, elems, t->fieldCount, 0, 0);
        return *outType < 0 ? -1 : 0;
    }
    *outType = typeId;
    return 0;
}

static int32_t HOPTCLookupSubstitutedType(
    const int32_t* paramTypes, const int32_t* argTypes, uint16_t argCount, int32_t typeId) {
    uint16_t i;
    for (i = 0; i < argCount; i++) {
        if (paramTypes[i] == typeId) {
            return argTypes[i];
        }
    }
    return typeId;
}

int32_t HOPTCSubstituteType(
    HOPTypeCheckCtx* c,
    int32_t          typeId,
    const int32_t*   paramTypes,
    const int32_t*   argTypes,
    uint16_t         argCount,
    uint32_t         errStart,
    uint32_t         errEnd) {
    const HOPTCType* t;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || argCount == 0) {
        return typeId;
    }
    t = &c->types[typeId];
    if (t->kind == HOPTCType_TYPE_PARAM) {
        return HOPTCLookupSubstitutedType(paramTypes, argTypes, argCount, typeId);
    }
    switch (t->kind) {
        case HOPTCType_PTR: {
            int32_t baseType = HOPTCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : HOPTCInternPtrType(c, baseType, errStart, errEnd);
        }
        case HOPTCType_REF: {
            int32_t baseType = HOPTCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : HOPTCInternRefType(
                           c, baseType, (t->flags & HOPTCTypeFlag_MUTABLE) != 0, errStart, errEnd);
        }
        case HOPTCType_ARRAY: {
            int32_t baseType = HOPTCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : HOPTCInternArrayType(c, baseType, t->arrayLen, errStart, errEnd);
        }
        case HOPTCType_SLICE: {
            int32_t baseType = HOPTCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : HOPTCInternSliceType(
                           c, baseType, (t->flags & HOPTCTypeFlag_MUTABLE) != 0, errStart, errEnd);
        }
        case HOPTCType_OPTIONAL: {
            int32_t baseType = HOPTCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            return baseType == t->baseType
                     ? typeId
                     : HOPTCInternOptionalType(c, baseType, errStart, errEnd);
        }
        case HOPTCType_TUPLE:
        case HOPTCType_PACK:  {
            int32_t  elems[256];
            uint32_t i;
            int      changed = 0;
            if (t->fieldCount > (uint16_t)(sizeof(elems) / sizeof(elems[0]))) {
                return -1;
            }
            for (i = 0; i < t->fieldCount; i++) {
                int32_t elem = c->funcParamTypes[t->fieldStart + i];
                elems[i] = HOPTCSubstituteType(
                    c, elem, paramTypes, argTypes, argCount, errStart, errEnd);
                if (elems[i] != elem) {
                    changed = 1;
                }
            }
            if (!changed) {
                return typeId;
            }
            return t->kind == HOPTCType_TUPLE
                     ? HOPTCInternTupleType(c, elems, t->fieldCount, errStart, errEnd)
                     : HOPTCInternPackType(c, elems, t->fieldCount, errStart, errEnd);
        }
        case HOPTCType_FUNCTION: {
            int32_t  params[256];
            uint8_t  flags[256];
            int32_t  returnType;
            uint32_t i;
            int      changed = 0;
            if (t->fieldCount > (uint16_t)(sizeof(params) / sizeof(params[0]))) {
                return -1;
            }
            returnType = HOPTCSubstituteType(
                c, t->baseType, paramTypes, argTypes, argCount, errStart, errEnd);
            changed = returnType != t->baseType;
            for (i = 0; i < t->fieldCount; i++) {
                int32_t paramType = c->funcParamTypes[t->fieldStart + i];
                params[i] = HOPTCSubstituteType(
                    c, paramType, paramTypes, argTypes, argCount, errStart, errEnd);
                flags[i] = c->funcParamFlags[t->fieldStart + i];
                if (params[i] != paramType) {
                    changed = 1;
                }
            }
            if (!changed) {
                return typeId;
            }
            return HOPTCInternFunctionType(
                c,
                returnType,
                params,
                flags,
                t->fieldCount,
                (t->flags & HOPTCTypeFlag_FUNCTION_VARIADIC) != 0,
                t->funcIndex,
                errStart,
                errEnd);
        }
        case HOPTCType_NAMED:
        case HOPTCType_ALIAS: {
            int32_t  namedIndex = -1;
            uint32_t ni;
            for (ni = 0; ni < c->namedTypeLen; ni++) {
                if (c->namedTypes[ni].typeId == typeId) {
                    namedIndex = (int32_t)ni;
                    break;
                }
            }
            if (namedIndex >= 0) {
                const HOPTCNamedType* nt = &c->namedTypes[(uint32_t)namedIndex];
                if (nt->templateRootNamedIndex >= 0 || nt->templateArgCount > 0) {
                    int32_t  args[64];
                    uint16_t i;
                    int      changed = 0;
                    if (nt->templateArgCount > (uint16_t)(sizeof(args) / sizeof(args[0]))) {
                        return -1;
                    }
                    for (i = 0; i < nt->templateArgCount; i++) {
                        int32_t argType = c->genericArgTypes[nt->templateArgStart + i];
                        args[i] = HOPTCSubstituteType(
                            c, argType, paramTypes, argTypes, argCount, errStart, errEnd);
                        if (args[i] != argType) {
                            changed = 1;
                        }
                    }
                    if (!changed && nt->templateRootNamedIndex >= 0) {
                        return typeId;
                    }
                    if (nt->templateRootNamedIndex >= 0) {
                        return HOPTCInstantiateNamedType(
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

int HOPTCTypeIsVarSize(HOPTypeCheckCtx* c, int32_t typeId) {
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    return (c->types[typeId].flags & HOPTCTypeFlag_VARSIZE) != 0;
}

int HOPTCTypeContainsVarSizeByValue(HOPTypeCheckCtx* c, int32_t typeId) {
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind == HOPTCType_PTR || c->types[typeId].kind == HOPTCType_REF) {
        return 0;
    }
    if (c->types[typeId].kind == HOPTCType_SLICE) {
        return 1;
    }
    if (c->types[typeId].kind == HOPTCType_OPTIONAL) {
        return HOPTCTypeContainsVarSizeByValue(c, c->types[typeId].baseType);
    }
    if (c->types[typeId].kind == HOPTCType_ARRAY) {
        return HOPTCTypeContainsVarSizeByValue(c, c->types[typeId].baseType);
    }
    if (c->types[typeId].kind == HOPTCType_TUPLE) {
        uint32_t i;
        for (i = 0; i < c->types[typeId].fieldCount; i++) {
            if (HOPTCTypeContainsVarSizeByValue(
                    c, c->funcParamTypes[c->types[typeId].fieldStart + i]))
            {
                return 1;
            }
        }
        return 0;
    }
    return HOPTCTypeIsVarSize(c, typeId);
}

int HOPTCIsComparisonHookName(
    HOPTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int* outIsEqualHook) {
    if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "__equal")) {
        *outIsEqualHook = 1;
        return 1;
    }
    if (HOPNameEqLiteral(c->src, nameStart, nameEnd, "__order")) {
        *outIsEqualHook = 0;
        return 1;
    }
    return 0;
}

int HOPTCTypeIsU8Slice(HOPTypeCheckCtx* c, int32_t typeId, int requireMutable) {
    int32_t u8Type;
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind != HOPTCType_SLICE) {
        return 0;
    }
    u8Type = HOPTCFindBuiltinByKind(c, HOPBuiltin_U8);
    if (u8Type < 0 || c->types[typeId].baseType != u8Type) {
        return 0;
    }
    if (requireMutable && !HOPTCTypeIsMutable(&c->types[typeId])) {
        return 0;
    }
    return 1;
}

int HOPTCTypeIsFreeablePointer(HOPTypeCheckCtx* c, int32_t typeId) {
    typeId = HOPTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind == HOPTCType_OPTIONAL) {
        return HOPTCTypeIsFreeablePointer(c, c->types[typeId].baseType);
    }
    return c->types[typeId].kind == HOPTCType_PTR;
}

int32_t HOPTCFindEmbeddedFieldIndex(HOPTypeCheckCtx* c, int32_t namedTypeId) {
    uint32_t i;
    if (namedTypeId < 0 || (uint32_t)namedTypeId >= c->typeLen
        || c->types[namedTypeId].kind != HOPTCType_NAMED)
    {
        return -1;
    }
    if (HOPTCEnsureNamedTypeFieldsResolved(c, namedTypeId) != 0) {
        return -1;
    }
    for (i = 0; i < c->types[namedTypeId].fieldCount; i++) {
        uint32_t idx = c->types[namedTypeId].fieldStart + i;
        if ((c->fields[idx].flags & HOPTCFieldFlag_EMBEDDED) != 0) {
            return (int32_t)idx;
        }
    }
    return -1;
}

int HOPTCEmbedDistanceToType(
    HOPTypeCheckCtx* c, int32_t srcType, int32_t dstType, uint32_t* outDistance) {
    uint32_t depth = 0;
    int32_t  cur = srcType;
    if (srcType == dstType) {
        *outDistance = 0;
        return 0;
    }
    while (depth++ <= c->typeLen) {
        int32_t embedIdx;
        if (cur < 0 || (uint32_t)cur >= c->typeLen || c->types[cur].kind != HOPTCType_NAMED) {
            return -1;
        }
        embedIdx = HOPTCFindEmbeddedFieldIndex(c, cur);
        if (embedIdx < 0) {
            return -1;
        }
        cur = c->fields[embedIdx].typeId;
        if (cur >= 0 && (uint32_t)cur < c->typeLen && c->types[cur].kind == HOPTCType_ALIAS) {
            cur = HOPTCResolveAliasBaseType(c, cur);
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

int HOPTCIsTypeDerivedFromEmbedded(HOPTypeCheckCtx* c, int32_t srcType, int32_t dstType) {
    uint32_t distance = 0;
    return HOPTCEmbedDistanceToType(c, srcType, dstType, &distance) == 0;
}

int HOPTCCanAssign(HOPTypeCheckCtx* c, int32_t dstType, int32_t srcType) {
    const HOPTCType* dst;
    const HOPTCType* src;

    if (dstType == srcType) {
        return 1;
    }
    if (HOPTCTypeIsFmtValue(c, dstType)) {
        return HOPTCTypeSupportsFmtReflectRec(c, srcType, 0u);
    }
    if (dstType == c->typeAnytype) {
        return srcType >= 0 && (uint32_t)srcType < c->typeLen;
    }
    if (dstType >= 0 && (uint32_t)dstType < c->typeLen && c->types[dstType].kind == HOPTCType_ALIAS)
    {
        /* Alias types are nominal: only exact alias matches assign implicitly.
         * Widening is one-way (Alias -> Target), handled via srcType alias path. */
        return 0;
    }
    if (srcType == c->typeUntypedInt && HOPTCIsIntegerType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedFloat && HOPTCIsFloatType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedInt && HOPTCIsFloatType(c, dstType)) {
        return 1;
    }

    if (dstType < 0 || (uint32_t)dstType >= c->typeLen || srcType < 0
        || (uint32_t)srcType >= c->typeLen)
    {
        return 0;
    }

    if (c->types[srcType].kind == HOPTCType_ALIAS) {
        int32_t srcBaseType = HOPTCResolveAliasBaseType(c, srcType);
        if (srcBaseType < 0 || srcBaseType == srcType) {
            return 0;
        }
        return HOPTCCanAssign(c, dstType, srcBaseType);
    }

    dst = &c->types[dstType];
    src = &c->types[srcType];

    if (dst->kind == HOPTCType_PACK) {
        uint32_t i;
        if (src->kind != HOPTCType_PACK || dst->fieldCount != src->fieldCount) {
            return 0;
        }
        for (i = 0; i < dst->fieldCount; i++) {
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (!HOPTCCanAssign(c, dstElem, srcElem)) {
                return 0;
            }
        }
        return 1;
    }

    if (dst->kind == HOPTCType_TUPLE) {
        uint32_t i;
        if (src->kind != HOPTCType_TUPLE || dst->fieldCount != src->fieldCount) {
            return 0;
        }
        for (i = 0; i < dst->fieldCount; i++) {
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (!HOPTCCanAssign(c, dstElem, srcElem)) {
                return 0;
            }
        }
        return 1;
    }

    if (src->kind == HOPTCType_NAMED && HOPTCIsTypeDerivedFromEmbedded(c, srcType, dstType)) {
        return 1;
    }

    if (dst->kind == HOPTCType_REF) {
        if (src->kind == HOPTCType_REF && src->baseType == c->typeStr
            && HOPTCTypeIsU8Slice(c, dst->baseType, 0))
        {
            return 1;
        }
        if (src->kind == HOPTCType_PTR && src->baseType == c->typeStr
            && HOPTCTypeIsU8Slice(c, dst->baseType, 0))
        {
            return 1;
        }
        if (src->kind == HOPTCType_REF && HOPTCCanAssign(c, dst->baseType, src->baseType)) {
            return !HOPTCTypeIsMutable(dst) || HOPTCTypeIsMutable(src);
        }
        if (src->kind == HOPTCType_PTR && HOPTCCanAssign(c, dst->baseType, src->baseType)) {
            return 1;
        }
        if (src->kind == HOPTCType_ARRAY && HOPTCCanAssign(c, dst->baseType, srcType)) {
            return 1;
        }
        return 0;
    }

    if (dst->kind == HOPTCType_PTR) {
        /* Owned pointers (*T) can only come from new; references (&T) cannot be
         * implicitly promoted to owned pointers. */
        if (src->kind == HOPTCType_ARRAY && dst->baseType >= 0
            && (uint32_t)dst->baseType < c->typeLen)
        {
            const HOPTCType* dstBase = &c->types[dst->baseType];
            if (dstBase->kind == HOPTCType_SLICE && dstBase->baseType == src->baseType) {
                return 1;
            }
        }
        if (src->kind == HOPTCType_PTR && dst->baseType >= 0 && src->baseType >= 0
            && (uint32_t)dst->baseType < c->typeLen && (uint32_t)src->baseType < c->typeLen)
        {
            const HOPTCType* dstBase = &c->types[dst->baseType];
            const HOPTCType* srcBase = &c->types[src->baseType];
            int32_t          dstBaseResolved = HOPTCResolveAliasBaseType(c, dst->baseType);
            int32_t          srcBaseResolved = HOPTCResolveAliasBaseType(c, src->baseType);
            if (dstBaseResolved >= 0 && srcBaseResolved >= 0 && dstBaseResolved == srcBaseResolved)
            {
                return 1;
            }
            if (dstBaseResolved >= 0 && srcBaseResolved >= 0
                && HOPTCIsTypeDerivedFromEmbedded(c, srcBaseResolved, dstBaseResolved))
            {
                return 1;
            }
            if (src->baseType == c->typeStr && HOPTCTypeIsU8Slice(c, dst->baseType, 1)) {
                return 1;
            }
            if (dstBase->kind == HOPTCType_SLICE) {
                if (srcBase->kind == HOPTCType_SLICE && dstBase->baseType == srcBase->baseType) {
                    return !HOPTCTypeIsMutable(dstBase) || HOPTCTypeIsMutable(srcBase);
                }
                if (srcBase->kind == HOPTCType_ARRAY && dstBase->baseType == srcBase->baseType) {
                    return 1;
                }
            }
        }
        return 0;
    }

    if (dst->kind == HOPTCType_SLICE) {
        if (srcType == c->typeStr && HOPTCTypeIsU8Slice(c, dstType, HOPTCTypeIsMutable(dst))) {
            return 1;
        }
        if (src->kind == HOPTCType_SLICE && dst->baseType == src->baseType) {
            return !HOPTCTypeIsMutable(dst) || HOPTCTypeIsMutable(src);
        }
        if (src->kind == HOPTCType_ARRAY && dst->baseType == src->baseType) {
            return 1;
        }
        if (src->kind == HOPTCType_PTR) {
            int32_t pointee = src->baseType;
            if (pointee >= 0 && (uint32_t)pointee < c->typeLen) {
                const HOPTCType* p = &c->types[pointee];
                if (p->kind == HOPTCType_ARRAY && p->baseType == dst->baseType) {
                    return 1;
                }
            }
        }
        return 0;
    }

    if (dst->kind == HOPTCType_OPTIONAL) {
        /* null can be assigned to ?T */
        if (src->kind == HOPTCType_NULL) {
            return 1;
        }
        /* T can be assigned to ?T (implicit lift through base assignability) */
        if (src->kind != HOPTCType_OPTIONAL && HOPTCCanAssign(c, dst->baseType, srcType)) {
            return 1;
        }
        /* ?T can be assigned to ?T (also handles mutable sub-type coercions) */
        if (src->kind == HOPTCType_OPTIONAL) {
            return HOPTCCanAssign(c, dst->baseType, src->baseType);
        }
        return 0;
    }

    if (HOPTCIsRawptrType(c, dstType)) {
        return src->kind == HOPTCType_NULL || HOPTCIsRawptrType(c, srcType);
    }

    /* null can only be assigned to ?T, not to plain types */
    if (src->kind == HOPTCType_NULL) {
        return 0;
    }

    return 0;
}

int HOPTCCoerceForBinary(
    HOPTypeCheckCtx* c, int32_t leftType, int32_t rightType, int32_t* outType) {
    if (leftType == rightType) {
        *outType = leftType;
        return 0;
    }
    if (HOPTCIsUntyped(c, leftType) && !HOPTCIsUntyped(c, rightType)
        && HOPTCCanAssign(c, rightType, leftType))
    {
        *outType = rightType;
        return 0;
    }
    if (HOPTCIsUntyped(c, rightType) && !HOPTCIsUntyped(c, leftType)
        && HOPTCCanAssign(c, leftType, rightType))
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

int HOPTCConversionCost(HOPTypeCheckCtx* c, int32_t dstType, int32_t srcType, uint8_t* outCost) {
    const HOPTCType* dst;
    const HOPTCType* src;

    if (!HOPTCCanAssign(c, dstType, srcType)) {
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
    if (HOPTCTypeIsFmtValue(c, dstType)) {
        *outCost = 5;
        return 0;
    }
    if (srcType == c->typeUntypedInt || srcType == c->typeUntypedFloat) {
        *outCost = 3;
        return 0;
    }
    if (srcType >= 0 && (uint32_t)srcType < c->typeLen && c->types[srcType].kind == HOPTCType_ALIAS)
    {
        int32_t srcBaseType = HOPTCResolveAliasBaseType(c, srcType);
        if (srcBaseType < 0 || srcBaseType == srcType) {
            return -1;
        }
        if (dstType == srcBaseType) {
            *outCost = 1;
            return 0;
        }
        if (HOPTCConversionCost(c, dstType, srcBaseType, outCost) == 0) {
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

    if (dst->kind == HOPTCType_TUPLE && src->kind == HOPTCType_TUPLE
        && dst->fieldCount == src->fieldCount)
    {
        uint32_t total = 0;
        uint32_t i;
        for (i = 0; i < dst->fieldCount; i++) {
            uint8_t elemCost = 0;
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (HOPTCConversionCost(c, dstElem, srcElem, &elemCost) != 0) {
                return -1;
            }
            total += elemCost;
        }
        *outCost = total > 255u ? 255u : (uint8_t)total;
        return 0;
    }

    if (dst->kind == HOPTCType_PACK && src->kind == HOPTCType_PACK
        && dst->fieldCount == src->fieldCount)
    {
        uint32_t total = 0;
        uint32_t i;
        for (i = 0; i < dst->fieldCount; i++) {
            uint8_t elemCost = 0;
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (HOPTCConversionCost(c, dstElem, srcElem, &elemCost) != 0) {
                return -1;
            }
            total += elemCost;
        }
        *outCost = total > 255u ? 255u : (uint8_t)total;
        return 0;
    }

    if (dst->kind == HOPTCType_OPTIONAL && src->kind != HOPTCType_OPTIONAL) {
        *outCost = 4;
        return 0;
    }

    if (dst->kind == HOPTCType_OPTIONAL && src->kind == HOPTCType_OPTIONAL) {
        return HOPTCConversionCost(c, dst->baseType, src->baseType, outCost);
    }

    if (HOPTCIsRawptrType(c, dstType) && HOPTCIsRawptrType(c, srcType)) {
        *outCost = 0;
        return 0;
    }

    if (dst->kind == HOPTCType_REF && src->kind == HOPTCType_REF) {
        int      sameBase = dst->baseType == src->baseType;
        uint32_t upcastDistance = 0;
        int      upcastBase =
            HOPTCEmbedDistanceToType(c, src->baseType, dst->baseType, &upcastDistance) == 0;
        if (upcastBase && !sameBase) {
            *outCost = (uint8_t)(2u + (upcastDistance > 0 ? (upcastDistance - 1u) : 0u));
            return 0;
        }
        if (!HOPTCTypeIsMutable(dst) && HOPTCTypeIsMutable(src) && sameBase) {
            *outCost = 1;
            return 0;
        }
    }

    if (dst->kind == HOPTCType_PTR && src->kind == HOPTCType_PTR) {
        int      sameBase = dst->baseType == src->baseType;
        uint32_t upcastDistance = 0;
        int      upcastBase =
            HOPTCEmbedDistanceToType(c, src->baseType, dst->baseType, &upcastDistance) == 0;
        if (upcastBase && !sameBase) {
            *outCost = (uint8_t)(2u + (upcastDistance > 0 ? (upcastDistance - 1u) : 0u));
            return 0;
        }
    }

    if (dst->kind == HOPTCType_SLICE && src->kind == HOPTCType_SLICE
        && dst->baseType == src->baseType && !HOPTCTypeIsMutable(dst) && HOPTCTypeIsMutable(src))
    {
        *outCost = 1;
        return 0;
    }

    if (src->kind == HOPTCType_NAMED && HOPTCIsTypeDerivedFromEmbedded(c, srcType, dstType)) {
        uint32_t distance = 0;
        if (HOPTCEmbedDistanceToType(c, srcType, dstType, &distance) == 0) {
            *outCost = (uint8_t)(2u + (distance > 0 ? (distance - 1u) : 0u));
        } else {
            *outCost = 2;
        }
        return 0;
    }

    *outCost = 1;
    return 0;
}

int HOPTCCostVectorCompare(const uint8_t* a, const uint8_t* b, uint32_t len) {
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

int32_t HOPTCUnwrapCallArgExprNode(HOPTypeCheckCtx* c, int32_t argNode) {
    const HOPAstNode* n;
    if (argNode < 0 || (uint32_t)argNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[argNode];
    if (n->kind == HOPAst_CALL_ARG) {
        return HOPAstFirstChild(c->ast, argNode);
    }
    return argNode;
}

int HOPTCCollectCallArgInfo(
    HOPTypeCheckCtx*  c,
    int32_t           callNode,
    int32_t           calleeNode,
    int               includeReceiver,
    int32_t           receiverNode,
    HOPTCCallArgInfo* outArgs,
    int32_t* _Nullable outArgTypes,
    uint32_t* outArgCount) {
    uint32_t argCount = 0;
    int32_t  argNode = HOPAstNextSibling(c->ast, calleeNode);
    if (includeReceiver) {
        const HOPAstNode* recvNode;
        if (receiverNode < 0 || (uint32_t)receiverNode >= c->ast->len) {
            return HOPTCFailNode(c, callNode, HOPDiag_EXPECTED_EXPR);
        }
        if (argCount >= HOPTC_MAX_CALL_ARGS) {
            return HOPTCFailNode(c, callNode, HOPDiag_ARENA_OOM);
        }
        recvNode = &c->ast->nodes[receiverNode];
        outArgs[argCount] = (HOPTCCallArgInfo){
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
        if (outArgTypes != NULL && HOPTCTypeExpr(c, receiverNode, &outArgTypes[argCount]) != 0) {
            return -1;
        }
        argCount++;
    }
    while (argNode >= 0) {
        const HOPAstNode* arg = &c->ast->nodes[argNode];
        int32_t           exprNode = HOPTCUnwrapCallArgExprNode(c, argNode);
        if (argCount >= HOPTC_MAX_CALL_ARGS) {
            return HOPTCFailNode(c, callNode, HOPDiag_ARENA_OOM);
        }
        if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
            return HOPTCFailNode(c, argNode, HOPDiag_EXPECTED_EXPR);
        }
        outArgs[argCount] = (HOPTCCallArgInfo){
            .argNode = argNode,
            .exprNode = exprNode,
            .start = arg->start,
            .end = arg->end,
            .explicitNameStart = arg->dataStart,
            .explicitNameEnd = arg->dataEnd,
            .implicitNameStart = 0,
            .implicitNameEnd = 0,
            .spread = (uint8_t)(((arg->flags & HOPAstFlag_CALL_ARG_SPREAD) != 0) ? 1 : 0),
            ._reserved = { 0, 0, 0 },
        };
        if ((arg->kind == HOPAst_CALL_ARG || arg->kind == HOPAst_IDENT)
            && !(outArgs[argCount].explicitNameEnd > outArgs[argCount].explicitNameStart)
            && c->ast->nodes[exprNode].kind == HOPAst_IDENT)
        {
            uint32_t nameStart = c->ast->nodes[exprNode].dataStart;
            uint32_t nameEnd = c->ast->nodes[exprNode].dataEnd;
            if (HOPTCResolveTypeValueName(c, nameStart, nameEnd) < 0) {
                outArgs[argCount].implicitNameStart = nameStart;
                outArgs[argCount].implicitNameEnd = nameEnd;
            }
        }
        if (outArgTypes != NULL && HOPTCTypeExpr(c, exprNode, &outArgTypes[argCount]) != 0) {
            return -1;
        }
        argCount++;
        argNode = HOPAstNextSibling(c->ast, argNode);
    }
    *outArgCount = argCount;
    return 0;
}

int HOPTCIsMainFunction(HOPTypeCheckCtx* c, const HOPTCFunction* fn) {
    return HOPNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "main");
}

int32_t HOPTCResolveImplicitMainContextType(HOPTypeCheckCtx* c) {
    uint32_t i;
    int32_t  typeId = HOPTCFindNamedTypeByLiteral(c, "builtin__Context");
    if (typeId >= 0) {
        return typeId;
    }
    for (i = 0; i < c->typeLen; i++) {
        const HOPTCType* t = &c->types[i];
        if (t->kind != HOPTCType_NAMED) {
            continue;
        }
        if (HOPNameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            && HOPNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__Context"))
        {
            return (int32_t)i;
        }
    }
    typeId = HOPTCFindNamedTypeByLiteral(c, "Context");
    if (typeId >= 0) {
        return typeId;
    }
    return -1;
}

int HOPTCCurrentContextFieldType(
    HOPTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType) {
    if (c->currentContextType >= 0) {
        if (HOPTCFieldLookup(c, c->currentContextType, fieldStart, fieldEnd, outType, NULL) == 0) {
            return 0;
        }
        return -1;
    }
    if (c->hasImplicitMainRootContext) {
        if (c->implicitMainContextType >= 0) {
            if (HOPTCFieldLookup(c, c->implicitMainContextType, fieldStart, fieldEnd, outType, NULL)
                == 0)
            {
                return 0;
            }
            return -1;
        }
        if (HOPNameEqLiteral(c->src, fieldStart, fieldEnd, "allocator")) {
            int32_t t = HOPTCFindMemAllocatorType(c);
            if (t < 0) {
                return -1;
            }
            *outType = t;
            return 0;
        }
        if (HOPNameEqLiteral(c->src, fieldStart, fieldEnd, "logger")) {
            int32_t t = HOPTCFindNamedTypeByLiteral(c, "builtin__Logger");
            if (t < 0) {
                t = HOPTCFindNamedTypeByLiteral(c, "Logger");
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

int HOPTCCurrentContextFieldTypeByLiteral(
    HOPTypeCheckCtx* c, const char* fieldName, int32_t* outType) {
    if (c->currentContextType >= 0) {
        int32_t  typeId = c->currentContextType;
        uint32_t i;
        if (typeId >= 0 && (uint32_t)typeId < c->typeLen
            && c->types[typeId].kind == HOPTCType_ALIAS)
        {
            if (HOPTCResolveAliasTypeId(c, typeId) != 0) {
                return -1;
            }
            typeId = c->types[typeId].baseType;
        }
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen
            || (c->types[typeId].kind != HOPTCType_NAMED
                && c->types[typeId].kind != HOPTCType_ANON_STRUCT
                && c->types[typeId].kind != HOPTCType_ANON_UNION))
        {
            return -1;
        }
        if (c->types[typeId].kind == HOPTCType_NAMED
            && HOPTCEnsureNamedTypeFieldsResolved(c, typeId) != 0)
        {
            return -1;
        }
        for (i = 0; i < c->types[typeId].fieldCount; i++) {
            uint32_t idx = c->types[typeId].fieldStart + i;
            if (HOPNameEqLiteral(
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
                   && c->types[typeId].kind == HOPTCType_ALIAS)
            {
                if (HOPTCResolveAliasTypeId(c, typeId) != 0) {
                    return -1;
                }
                typeId = c->types[typeId].baseType;
            }
            if (typeId < 0 || (uint32_t)typeId >= c->typeLen
                || (c->types[typeId].kind != HOPTCType_NAMED
                    && c->types[typeId].kind != HOPTCType_ANON_STRUCT
                    && c->types[typeId].kind != HOPTCType_ANON_UNION))
            {
                return -1;
            }
            if (c->types[typeId].kind == HOPTCType_NAMED
                && HOPTCEnsureNamedTypeFieldsResolved(c, typeId) != 0)
            {
                return -1;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t idx = c->types[typeId].fieldStart + i;
                if (HOPNameEqLiteral(
                        c->src, c->fields[idx].nameStart, c->fields[idx].nameEnd, fieldName))
                {
                    *outType = c->fields[idx].typeId;
                    return 0;
                }
            }
            return -1;
        }
        if (HOPTCStrEqNullable(fieldName, "allocator")) {
            int32_t t = HOPTCFindMemAllocatorType(c);
            if (t < 0) {
                return -1;
            }
            *outType = t;
            return 0;
        }
        if (HOPTCStrEqNullable(fieldName, "logger")) {
            int32_t t = HOPTCFindNamedTypeByLiteral(c, "builtin__Logger");
            if (t < 0) {
                t = HOPTCFindNamedTypeByLiteral(c, "Logger");
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

int32_t HOPTCContextFindOverlayNode(HOPTypeCheckCtx* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast->len) {
        return -1;
    }
    {
        int32_t callNode = HOPAstFirstChild(c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? HOPAstNextSibling(c->ast, callNode) : -1;
        if (child >= 0 && c->ast->nodes[child].kind == HOPAst_CONTEXT_OVERLAY) {
            return child;
        }
    }
    return -1;
}

int32_t HOPTCContextFindDirectNode(HOPTypeCheckCtx* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast->len) {
        return -1;
    }
    {
        int32_t callNode = HOPAstFirstChild(c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? HOPAstNextSibling(c->ast, callNode) : -1;
        if (child >= 0 && c->ast->nodes[child].kind != HOPAst_CONTEXT_OVERLAY) {
            return child;
        }
    }
    return -1;
}

int32_t HOPTCContextFindOverlayBindMatch(
    HOPTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, const char* _Nullable fieldName) {
    int32_t overlayNode = HOPTCContextFindOverlayNode(c);
    int32_t child = overlayNode >= 0 ? HOPAstFirstChild(c->ast, overlayNode) : -1;
    while (child >= 0) {
        const HOPAstNode* bind = &c->ast->nodes[child];
        if (bind->kind == HOPAst_CONTEXT_BIND) {
            int matches =
                fieldName != NULL
                    ? HOPNameEqLiteral(c->src, bind->dataStart, bind->dataEnd, fieldName)
                    : HOPNameEqSlice(c->src, bind->dataStart, bind->dataEnd, fieldStart, fieldEnd);
            if (matches) {
                return child;
            }
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    return -1;
}

int32_t HOPTCContextFindOverlayBindByLiteral(HOPTypeCheckCtx* c, const char* fieldName) {
    return HOPTCContextFindOverlayBindMatch(c, 0, 0, fieldName);
}

int HOPTCGetEffectiveContextFieldType(
    HOPTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType) {
    int32_t bindNode;
    if (c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return HOPTCFailSpan(c, HOPDiag_CONTEXT_REQUIRED, fieldStart, fieldEnd);
    }
    bindNode = HOPTCContextFindOverlayBindMatch(c, fieldStart, fieldEnd, NULL);
    if (bindNode >= 0) {
        int32_t exprNode = HOPAstFirstChild(c->ast, bindNode);
        if (exprNode >= 0) {
            return HOPTCTypeExpr(c, exprNode, outType);
        }
    }
    if (HOPTCCurrentContextFieldType(c, fieldStart, fieldEnd, outType) != 0) {
        return HOPTCFailSpan(c, HOPDiag_CONTEXT_MISSING_FIELD, fieldStart, fieldEnd);
    }
    return 0;
}

int HOPTCGetEffectiveContextFieldTypeByLiteral(
    HOPTypeCheckCtx* c, const char* fieldName, int32_t* outType) {
    int32_t bindNode;
    if (c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return HOPTCFailSpan(c, HOPDiag_CONTEXT_REQUIRED, 0, 0);
    }
    bindNode = HOPTCContextFindOverlayBindByLiteral(c, fieldName);
    if (bindNode >= 0) {
        int32_t exprNode = HOPAstFirstChild(c->ast, bindNode);
        if (exprNode >= 0) {
            return HOPTCTypeExpr(c, exprNode, outType);
        }
    }
    if (HOPTCCurrentContextFieldTypeByLiteral(c, fieldName, outType) != 0) {
        return HOPTCFailSpan(c, HOPDiag_CONTEXT_MISSING_FIELD, 0, 0);
    }
    return 0;
}

int HOPTCValidateCurrentCallOverlay(HOPTypeCheckCtx* c) {
    int32_t overlayNode = HOPTCContextFindOverlayNode(c);
    int32_t directNode = HOPTCContextFindDirectNode(c);
    int32_t bind = overlayNode >= 0 ? HOPAstFirstChild(c->ast, overlayNode) : -1;
    if (directNode >= 0) {
        int32_t t;
        int32_t savedActive = c->activeCallWithNode;
        c->activeCallWithNode = -1;
        if (HOPTCTypeExpr(c, directNode, &t) != 0) {
            c->activeCallWithNode = savedActive;
            return -1;
        }
        c->activeCallWithNode = savedActive;
        return 0;
    }
    if (overlayNode >= 0 && c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return HOPTCFailSpan(
            c,
            HOPDiag_CONTEXT_REQUIRED,
            c->ast->nodes[overlayNode].start,
            c->ast->nodes[overlayNode].end);
    }
    while (bind >= 0) {
        const HOPAstNode* b = &c->ast->nodes[bind];
        int32_t           expectedType;
        int32_t           exprNode;
        int32_t           t;
        int32_t           scan;
        if (b->kind != HOPAst_CONTEXT_BIND) {
            return HOPTCFailNode(c, bind, HOPDiag_UNEXPECTED_TOKEN);
        }
        if (HOPTCCurrentContextFieldType(c, b->dataStart, b->dataEnd, &expectedType) != 0) {
            return HOPTCFailSpan(c, HOPDiag_CONTEXT_UNKNOWN_FIELD, b->dataStart, b->dataEnd);
        }
        scan = HOPAstFirstChild(c->ast, overlayNode);
        while (scan >= 0) {
            const HOPAstNode* bs = &c->ast->nodes[scan];
            if (scan != bind && bs->kind == HOPAst_CONTEXT_BIND
                && HOPNameEqSlice(c->src, bs->dataStart, bs->dataEnd, b->dataStart, b->dataEnd))
            {
                return HOPTCFailSpan(c, HOPDiag_CONTEXT_DUPLICATE_FIELD, b->dataStart, b->dataEnd);
            }
            scan = HOPAstNextSibling(c->ast, scan);
        }
        exprNode = HOPAstFirstChild(c->ast, bind);
        if (exprNode >= 0) {
            int32_t savedActive = c->activeCallWithNode;
            c->activeCallWithNode = -1;
            if (HOPTCTypeExpr(c, exprNode, &t) != 0) {
                c->activeCallWithNode = savedActive;
                return -1;
            }
            c->activeCallWithNode = savedActive;
            if (!HOPTCCanAssign(c, expectedType, t)) {
                return HOPTCFailNode(c, exprNode, HOPDiag_CONTEXT_TYPE_MISMATCH);
            }
        }
        bind = HOPAstNextSibling(c->ast, bind);
    }
    return 0;
}

int HOPTCValidateCallContextRequirements(HOPTypeCheckCtx* c, int32_t requiredContextType) {
    int32_t  typeId = requiredContextType;
    int32_t  directNode = HOPTCContextFindDirectNode(c);
    uint32_t i;
    if (requiredContextType < 0) {
        return 0;
    }
    if (directNode >= 0) {
        int32_t expectedContextRef = HOPTCInternRefType(
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
        if (HOPTCTypeExpr(c, directNode, &gotType) != 0) {
            c->activeCallWithNode = savedActive;
            return -1;
        }
        c->activeCallWithNode = savedActive;
        if (!HOPTCCanAssign(c, expectedContextRef, gotType)) {
            return HOPTCFailNode(c, directNode, HOPDiag_CONTEXT_CLAUSE_MISMATCH);
        }
        return 0;
    }
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == HOPTCType_ALIAS)
    {
        if (HOPTCResolveAliasTypeId(c, typeId) != 0) {
            return -1;
        }
        typeId = c->types[typeId].baseType;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen
        || (c->types[typeId].kind != HOPTCType_NAMED
            && c->types[typeId].kind != HOPTCType_ANON_STRUCT))
    {
        return HOPTCFailSpan(c, HOPDiag_TYPE_MISMATCH, 0, 0);
    }
    if (c->types[typeId].kind == HOPTCType_NAMED
        && HOPTCEnsureNamedTypeFieldsResolved(c, typeId) != 0)
    {
        return -1;
    }
    for (i = 0; i < c->types[typeId].fieldCount; i++) {
        uint32_t         fieldIdx = c->types[typeId].fieldStart + i;
        const HOPTCField field = c->fields[fieldIdx];
        int32_t          gotType = -1;
        if (field.nameEnd <= field.nameStart) {
            continue;
        }
        if (HOPTCGetEffectiveContextFieldType(c, field.nameStart, field.nameEnd, &gotType) != 0) {
            return -1;
        }
        if (!HOPTCCanAssign(c, field.typeId, gotType)) {
            return HOPTCFailSpan(c, HOPDiag_CONTEXT_TYPE_MISMATCH, field.nameStart, field.nameEnd);
        }
    }
    return 0;
}

int HOPTCGetFunctionTypeSignature(
    HOPTypeCheckCtx* c,
    int32_t          typeId,
    int32_t*         outReturnType,
    uint32_t*        outParamStart,
    uint32_t*        outParamCount,
    int* _Nullable outIsVariadic) {
    const HOPTCType* t;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return -1;
    }
    t = &c->types[typeId];
    if (t->kind != HOPTCType_FUNCTION) {
        return -1;
    }
    *outReturnType = t->baseType;
    *outParamStart = t->fieldStart;
    *outParamCount = t->fieldCount;
    if (outIsVariadic != NULL) {
        *outIsVariadic = (t->flags & HOPTCTypeFlag_FUNCTION_VARIADIC) != 0;
    }
    return 0;
}

void HOPTCCallMapErrorClear(HOPTCCallMapError* err) {
    err->code = 0;
    err->start = 0;
    err->end = 0;
    err->argStart = 0;
    err->argEnd = 0;
}

static int HOPTCParamNameStartsWithUnderscore(
    HOPTypeCheckCtx* c,
    const uint32_t*  paramNameStarts,
    const uint32_t*  paramNameEnds,
    uint32_t         paramIndex) {
    uint32_t start;
    uint32_t end;
    if (c == NULL || c->src.ptr == NULL || paramNameStarts == NULL || paramNameEnds == NULL) {
        return 0;
    }
    start = paramNameStarts[paramIndex];
    end = paramNameEnds[paramIndex];
    return end > start && c->src.ptr[start] == '_';
}

static uint32_t HOPTCPositionalCallPrefixEnd(
    HOPTypeCheckCtx* c,
    const uint32_t*  paramNameStarts,
    const uint32_t*  paramNameEnds,
    uint32_t         paramCount,
    uint32_t         firstPositionalArgIndex) {
    uint32_t prefixEnd;
    if (firstPositionalArgIndex >= paramCount) {
        return paramCount;
    }
    prefixEnd = firstPositionalArgIndex + 1u;
    while (prefixEnd < paramCount
           && HOPTCParamNameStartsWithUnderscore(c, paramNameStarts, paramNameEnds, prefixEnd))
    {
        prefixEnd++;
    }
    return prefixEnd;
}

int HOPTCMapCallArgsToParams(
    HOPTypeCheckCtx*        c,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    const uint32_t*         paramNameStarts,
    const uint32_t*         paramNameEnds,
    uint32_t                paramCount,
    uint32_t                firstPositionalArgIndex,
    int32_t*                outMappedArgExprNodes,
    HOPTCCallMapError* _Nullable outError) {
    uint8_t  paramAssigned[HOPTC_MAX_CALL_ARGS];
    uint32_t positionalPrefixEnd;
    uint32_t i;
    if (paramCount > HOPTC_MAX_CALL_ARGS || argCount > paramCount) {
        return -1;
    }
    memset(paramAssigned, 0, sizeof(paramAssigned));
    for (i = 0; i < paramCount; i++) {
        outMappedArgExprNodes[i] = -1;
    }
    positionalPrefixEnd = HOPTCPositionalCallPrefixEnd(
        c, paramNameStarts, paramNameEnds, paramCount, firstPositionalArgIndex);

    if (firstPositionalArgIndex < argCount) {
        const HOPTCCallArgInfo* a = &callArgs[firstPositionalArgIndex];
        outMappedArgExprNodes[firstPositionalArgIndex] = a->exprNode;
        paramAssigned[firstPositionalArgIndex] = 1;
        if (a->explicitNameEnd > a->explicitNameStart
            && !HOPNameEqSlice(
                c->src,
                a->explicitNameStart,
                a->explicitNameEnd,
                paramNameStarts[firstPositionalArgIndex],
                paramNameEnds[firstPositionalArgIndex]))
        {
            if (outError != NULL) {
                outError->code = HOPDiag_CALL_FIRST_ARG_NAME_MISMATCH;
                outError->start = a->start;
                outError->end = a->end;
                outError->argStart = paramNameStarts[firstPositionalArgIndex];
                outError->argEnd = paramNameEnds[firstPositionalArgIndex];
            }
            return 1;
        }
    }

    for (i = 0; i < argCount; i++) {
        const HOPTCCallArgInfo* a = &callArgs[i];
        uint32_t                nameStart = 0;
        uint32_t                nameEnd = 0;
        uint32_t                p;
        int                     foundName = 0;
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
                    outError->code = HOPDiag_CALL_ARG_NAME_REQUIRED;
                    outError->argStart = 0;
                    outError->argEnd = 0;
                } else if (positionalPrefixEnd < paramCount) {
                    outError->code = HOPDiag_CALL_ARG_NAME_REQUIRED_AFTER_PARAM;
                    outError->argStart = paramNameStarts[positionalPrefixEnd];
                    outError->argEnd = paramNameEnds[positionalPrefixEnd];
                } else {
                    outError->code = HOPDiag_CALL_ARG_NAME_REQUIRED_AFTER_PARAM;
                    outError->argStart = 0;
                    outError->argEnd = 0;
                }
            }
            return 1;
        }

        for (p = firstPositionalArgIndex + 1u; p < paramCount; p++) {
            if (HOPNameEqSlice(c->src, nameStart, nameEnd, paramNameStarts[p], paramNameEnds[p])) {
                if (paramAssigned[p]) {
                    if (outError != NULL) {
                        outError->code = HOPDiag_CALL_ARG_DUPLICATE;
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
                outError->code = HOPDiag_CALL_ARG_UNKNOWN_NAME;
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
                outError->code = HOPDiag_CALL_ARG_MISSING;
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

int HOPTCPrepareCallBinding(
    HOPTypeCheckCtx*        c,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    const uint32_t*         paramNameStarts,
    const uint32_t*         paramNameEnds,
    const int32_t*          paramTypes,
    uint32_t                paramCount,
    int                     isVariadic,
    int                     allowNamedMapping,
    uint32_t                firstPositionalArgIndex,
    HOPTCCallBinding*       outBinding,
    HOPTCCallMapError*      outError) {
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
    for (i = 0; i < HOPTC_MAX_CALL_ARGS; i++) {
        outBinding->fixedMappedArgExprNodes[i] = -1;
        outBinding->argParamIndices[i] = -1;
        outBinding->argExpectedTypes[i] = -1;
    }
    if (paramCount > HOPTC_MAX_CALL_ARGS || argCount > HOPTC_MAX_CALL_ARGS) {
        return 1;
    }

    for (i = 0; i < argCount; i++) {
        if (!callArgs[i].spread) {
            continue;
        }
        if (i + 1u < argCount) {
            if (outError != NULL) {
                outError->code = HOPDiag_VARIADIC_SPREAD_NOT_LAST;
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
                outError->code = HOPDiag_VARIADIC_CALL_SHAPE_MISMATCH;
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
        if (c->types[outBinding->variadicParamType].kind == HOPTCType_SLICE) {
            outBinding->variadicElemType = c->types[outBinding->variadicParamType].baseType;
        } else if (c->types[outBinding->variadicParamType].kind == HOPTCType_PACK) {
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
                    outError->code = HOPDiag_VARIADIC_CALL_SHAPE_MISMATCH;
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
                    outError->code = HOPDiag_VARIADIC_CALL_SHAPE_MISMATCH;
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
            HOPTCCallMapError mapError;
            HOPTCCallMapErrorClear(&mapError);
            if (HOPTCMapCallArgsToParams(
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
        const HOPTCType* variadicType = &c->types[outBinding->variadicParamType];
        if (callArgs[i].explicitNameEnd > callArgs[i].explicitNameStart) {
            if (outError != NULL) {
                outError->code = HOPDiag_VARIADIC_CALL_SHAPE_MISMATCH;
                outError->start = callArgs[i].start;
                outError->end = callArgs[i].end;
                outError->argStart = 0;
                outError->argEnd = 0;
            }
            return 2;
        }
        outBinding->argParamIndices[i] = (int32_t)fixedCount;
        if (variadicType->kind == HOPTCType_PACK) {
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

int HOPTCCheckConstParamArgs(
    HOPTypeCheckCtx*        c,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    const HOPTCCallBinding* binding,
    const uint32_t*         paramNameStarts,
    const uint32_t*         paramNameEnds,
    const uint8_t*          paramFlags,
    uint32_t                paramCount,
    HOPTCCallMapError*      outError) {
    uint32_t i;
    if (outError != NULL) {
        HOPTCCallMapErrorClear(outError);
    }
    if (binding == NULL || paramFlags == NULL) {
        return 0;
    }
    for (i = 0; i < argCount; i++) {
        int32_t           p = binding->argParamIndices[i];
        int               isConst = 0;
        int               evalIsConst = 0;
        HOPCTFEValue      ignoredValue = { 0 };
        HOPTCConstEvalCtx evalCtx;
        HOPDiagCode       code = HOPDiag_CONST_PARAM_ARG_NOT_CONST;
        HOPTCInitConstEvalCtxFromParent(c, c != NULL ? c->activeConstEvalCtx : NULL, &evalCtx);
        if (p < 0 || (uint32_t)p >= paramCount) {
            continue;
        }
        isConst = (paramFlags[p] & HOPTCFuncParamFlag_CONST) != 0;
        if (!isConst) {
            continue;
        }
        if (binding->isVariadic && i == binding->spreadArgIndex
            && (uint32_t)p == binding->fixedCount)
        {
            code = HOPDiag_CONST_PARAM_SPREAD_NOT_CONST;
        }
        if (HOPTCEvalConstExprNode(&evalCtx, callArgs[i].exprNode, &ignoredValue, &evalIsConst)
            != 0)
        {
            return -1;
        }
        if (evalIsConst) {
            continue;
        }
        if (outError != NULL) {
            outError->code = code;
            outError->start = callArgs[i].start;
            outError->end = callArgs[i].end;
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

int HOPTCCheckConstBlocksForCall(
    HOPTypeCheckCtx*        c,
    int32_t                 fnIndex,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    const HOPTCCallBinding* binding,
    HOPTCCallMapError*      outError) {
    const HOPTCFunction* fn;
    int32_t              fnNode;
    int32_t              bodyNode = -1;
    int32_t              child;
    uint32_t             paramIndex = 0;
    uint32_t             variadicPackParamNameStart = 0;
    uint32_t             variadicPackParamNameEnd = 0;
    int                  hasConstBlock = 0;
    uint32_t             savedLocalLen;
    uint32_t             savedLocalUseLen;
    uint32_t             savedVariantNarrowLen;
    HOPTCConstEvalCtx*   savedActiveConstEvalCtx;
    HOPCTFEExecBinding*  paramBindings = NULL;
    uint32_t             paramBindingLen = 0;
    HOPCTFEExecEnv       paramFrame;
    HOPCTFEExecCtx       execCtx;
    HOPTCConstEvalCtx    evalCtx;
    HOPCTFEValue         retValue;
    int                  didReturn = 0;
    int                  isConst = 0;
    int                  rc;

    if (outError != NULL) {
        HOPTCCallMapErrorClear(outError);
    }
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return 0;
    }
    fn = &c->funcs[fnIndex];
    fnNode = fn->defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != HOPAst_FN) {
        return 0;
    }

    child = HOPAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == HOPAst_BLOCK) {
            bodyNode = child;
            break;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    if (bodyNode < 0) {
        return 0;
    }

    child = HOPAstFirstChild(c->ast, bodyNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == HOPAst_CONST_BLOCK) {
            hasConstBlock = 1;
            break;
        }
        child = HOPAstNextSibling(c->ast, child);
    }
    if (!hasConstBlock) {
        return 0;
    }

    savedLocalLen = c->localLen;
    savedLocalUseLen = c->localUseLen;
    savedVariantNarrowLen = c->variantNarrowLen;
    savedActiveConstEvalCtx = c->activeConstEvalCtx;

    if (fn->paramCount > 0) {
        paramBindings = (HOPCTFEExecBinding*)HOPArenaAlloc(
            c->arena,
            sizeof(HOPCTFEExecBinding) * fn->paramCount,
            (uint32_t)_Alignof(HOPCTFEExecBinding));
        if (paramBindings == NULL) {
            c->localLen = savedLocalLen;
            c->localUseLen = savedLocalUseLen;
            c->variantNarrowLen = savedVariantNarrowLen;
            c->activeConstEvalCtx = savedActiveConstEvalCtx;
            return HOPTCFailNode(c, fnNode, HOPDiag_ARENA_OOM);
        }
    }

    child = HOPAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &c->ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
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
                (c->funcParamFlags[fn->paramTypeStart + paramIndex] & HOPTCFuncParamFlag_CONST)
                != 0;

            if (!HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")
                && HOPTCLocalAdd(c, n->dataStart, n->dataEnd, paramType, isConstParam, -1) != 0)
            {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return -1;
            }
            if (!HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")) {
                addedLocal = 1;
            }
            if (addedLocal && (n->flags & HOPAstFlag_PARAM_VARIADIC) != 0
                && (paramType == c->typeAnytype
                    || ((uint32_t)paramType < c->typeLen
                        && c->types[paramType].kind == HOPTCType_PACK)))
            {
                c->locals[c->localLen - 1u].flags |= HOPTCLocalFlag_ANYPACK;
                variadicPackParamNameStart = n->dataStart;
                variadicPackParamNameEnd = n->dataEnd;
            }

            if (isConstParam && binding != NULL && paramBindings != NULL
                && !HOPNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_"))
            {
                int32_t           argIndex = -1;
                HOPCTFEValue      value;
                int               evalIsConst = 0;
                HOPTCConstEvalCtx evalArgCtx;
                uint32_t          i;
                HOPTCInitConstEvalCtxFromParent(c, savedActiveConstEvalCtx, &evalArgCtx);
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
                    if (HOPTCEvalConstExprNode(
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
                        if (outError != NULL) {
                            outError->code = HOPDiag_CONST_BLOCK_EVAL_FAILED;
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
        child = HOPAstNextSibling(c->ast, child);
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
    execCtx.evalExpr = HOPTCEvalConstExecExprCb;
    execCtx.evalExprCtx = &evalCtx;
    execCtx.resolveType = HOPTCEvalConstExecResolveTypeCb;
    execCtx.resolveTypeCtx = &evalCtx;
    execCtx.inferValueType = HOPTCEvalConstExecInferValueTypeCb;
    execCtx.inferValueTypeCtx = &evalCtx;
    execCtx.forInIndex = HOPTCEvalConstForInIndexCb;
    execCtx.forInIndexCtx = &evalCtx;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = HOPTC_CONST_FOR_MAX_ITERS;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    evalCtx.execCtx = &execCtx;
    evalCtx.callArgs = callArgs;
    evalCtx.callArgCount = argCount;
    evalCtx.callBinding = binding;
    evalCtx.callPackParamNameStart = variadicPackParamNameStart;
    evalCtx.callPackParamNameEnd = variadicPackParamNameEnd;
    c->activeConstEvalCtx = &evalCtx;

    child = HOPAstFirstChild(c->ast, bodyNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == HOPAst_CONST_BLOCK) {
            int32_t blockNode = HOPAstFirstChild(c->ast, child);
            int     mirSupported = 0;
            if (blockNode < 0 || c->ast->nodes[blockNode].kind != HOPAst_BLOCK) {
                if (outError != NULL) {
                    outError->code = HOPDiag_CONST_BLOCK_EVAL_FAILED;
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
            rc = HOPTCTryMirConstBlock(
                &evalCtx, blockNode, &retValue, &didReturn, &isConst, &mirSupported);
            if (rc != 0) {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return -1;
            }
            if (mirSupported && isConst && !didReturn) {
                child = HOPAstNextSibling(c->ast, child);
                continue;
            }
            if (didReturn) {
                HOPTCConstSetReasonNode(&evalCtx, blockNode, "const block must not return a value");
            } else if (evalCtx.nonConstReason == NULL) {
                HOPTCConstSetReasonNode(&evalCtx, blockNode, "const block is not const-evaluable");
            }
            c->lastConstEvalReason = c->activeConstEvalCtx->nonConstReason;
            c->lastConstEvalReasonStart = c->activeConstEvalCtx->nonConstStart;
            c->lastConstEvalReasonEnd = c->activeConstEvalCtx->nonConstEnd;
            if (outError != NULL) {
                outError->code = HOPDiag_CONST_BLOCK_EVAL_FAILED;
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
        child = HOPAstNextSibling(c->ast, child);
    }

    c->localLen = savedLocalLen;
    c->localUseLen = savedLocalUseLen;
    c->variantNarrowLen = savedVariantNarrowLen;
    c->activeConstEvalCtx = savedActiveConstEvalCtx;
    return 0;
}

int HOPTCResolveComparisonHookArgCost(
    HOPTypeCheckCtx* c, int32_t paramType, int32_t argType, uint8_t* outCost) {
    int32_t resolvedParam = HOPTCResolveAliasBaseType(c, paramType);
    uint8_t baseCost = 0;
    if (HOPTCConversionCost(c, paramType, argType, outCost) == 0) {
        return 0;
    }
    if (resolvedParam < 0 || (uint32_t)resolvedParam >= c->typeLen) {
        return -1;
    }
    if (c->types[resolvedParam].kind == HOPTCType_REF
        && !HOPTCTypeIsMutable(&c->types[resolvedParam])
        && HOPTCConversionCost(c, c->types[resolvedParam].baseType, argType, &baseCost) == 0)
    {
        *outCost = (uint8_t)(baseCost < 254u ? baseCost + 1u : 255u);
        return 0;
    }
    return -1;
}

int HOPTCResolveComparisonHook(
    HOPTypeCheckCtx* c,
    const char*      hookName,
    int32_t          lhsType,
    int32_t          rhsType,
    int32_t*         outFuncIndex) {
    uint8_t  bestCosts[2];
    int      haveBest = 0;
    int      ambiguous = 0;
    int      nameFound = 0;
    uint32_t bestTotal = 0;
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        uint8_t              curCosts[2];
        uint32_t             curTotal = 0;
        int                  cmp;
        if (!HOPNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, hookName)) {
            continue;
        }
        nameFound = 1;
        if (fn->paramCount != 2) {
            continue;
        }
        if (HOPTCResolveComparisonHookArgCost(
                c, c->funcParamTypes[fn->paramTypeStart], lhsType, &curCosts[0])
                != 0
            || HOPTCResolveComparisonHookArgCost(
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
        cmp = HOPTCCostVectorCompare(curCosts, bestCosts, 2u);
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

void HOPTCGatherCallCandidates(
    HOPTypeCheckCtx* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    int32_t*         outCandidates,
    uint32_t*        outCandidateCount,
    int*             outNameFound) {
    uint32_t count = 0;
    uint32_t i;
    *outNameFound = 0;
    for (i = 0; i < c->funcLen && count < HOPTC_MAX_CALL_CANDIDATES; i++) {
        if (HOPTCFunctionNameEq(c, i, nameStart, nameEnd)) {
            outCandidates[count++] = (int32_t)i;
            *outNameFound = 1;
        }
    }
    for (i = 0; i < c->funcLen && count < HOPTC_MAX_CALL_CANDIDATES; i++) {
        const HOPTCFunction* fn = &c->funcs[i];
        uint32_t             nameLen;
        uint32_t             candLen;
        if (nameEnd <= nameStart || nameEnd > c->src.len || fn->nameEnd <= fn->nameStart) {
            continue;
        }
        nameLen = nameEnd - nameStart;
        candLen = fn->nameEnd - fn->nameStart;
        if (candLen != 9u + nameLen) {
            continue;
        }
        if (memcmp(c->src.ptr + fn->nameStart, "builtin__", 9u) != 0) {
            continue;
        }
        if (memcmp(c->src.ptr + fn->nameStart + 9u, c->src.ptr + nameStart, nameLen) == 0) {
            outCandidates[count++] = (int32_t)i;
            *outNameFound = 1;
        }
    }
    *outCandidateCount = count;
}

void HOPTCGatherCallCandidatesByPkgMethod(
    HOPTypeCheckCtx* c,
    uint32_t         pkgStart,
    uint32_t         pkgEnd,
    uint32_t         methodStart,
    uint32_t         methodEnd,
    int32_t*         outCandidates,
    uint32_t*        outCandidateCount,
    int*             outNameFound) {
    uint32_t count = 0;
    uint32_t i;
    *outNameFound = 0;
    for (i = 0; i < c->funcLen && count < HOPTC_MAX_CALL_CANDIDATES; i++) {
        if (HOPTCNameEqPkgPrefixedMethod(
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

int HOPTCFunctionHasAnytypeParam(HOPTypeCheckCtx* c, int32_t fnIndex) {
    const HOPTCFunction* fn;
    uint32_t             p;
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return 0;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & HOPTCFunctionFlag_TEMPLATE) != 0) {
        return 1;
    }
    for (p = 0; p < fn->paramCount; p++) {
        if (c->funcParamTypes[fn->paramTypeStart + p] == c->typeAnytype) {
            return 1;
        }
    }
    return 0;
}

static int HOPTCInferTemplateArgBind(
    HOPTypeCheckCtx* c, int32_t* boundType, int32_t argType, uint32_t errStart, uint32_t errEnd) {
    if (*boundType < 0) {
        *boundType = argType;
        return 0;
    }
    if (*boundType == argType) {
        return 0;
    }
    if (HOPTCIsUntyped(c, *boundType) && !HOPTCIsUntyped(c, argType)
        && HOPTCCanAssign(c, argType, *boundType))
    {
        *boundType = argType;
        return 0;
    }
    if (!HOPTCIsUntyped(c, *boundType) && HOPTCIsUntyped(c, argType)
        && HOPTCCanAssign(c, *boundType, argType))
    {
        return 0;
    }
    (void)errStart;
    (void)errEnd;
    return 1;
}

static int HOPTCInferTemplateArgsFromTypes(
    HOPTypeCheckCtx* c,
    int32_t          paramType,
    int32_t          argType,
    const int32_t*   templateParamTypes,
    int32_t*         templateArgTypes,
    uint16_t         templateArgCount,
    uint32_t         errStart,
    uint32_t         errEnd) {
    uint16_t i;
    int32_t  resolvedParam = HOPTCResolveAliasBaseType(c, paramType);
    int32_t  resolvedArg = HOPTCResolveAliasBaseType(c, argType);
    if (resolvedParam < 0) {
        resolvedParam = paramType;
    }
    if (resolvedArg < 0) {
        resolvedArg = argType;
    }
    for (i = 0; i < templateArgCount; i++) {
        if (templateParamTypes[i] == paramType || templateParamTypes[i] == resolvedParam) {
            return HOPTCInferTemplateArgBind(c, &templateArgTypes[i], argType, errStart, errEnd);
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
        case HOPTCType_PTR:
        case HOPTCType_REF:
        case HOPTCType_ARRAY:
        case HOPTCType_SLICE:
        case HOPTCType_OPTIONAL:
            return HOPTCInferTemplateArgsFromTypes(
                c,
                c->types[resolvedParam].baseType,
                c->types[resolvedArg].baseType,
                templateParamTypes,
                templateArgTypes,
                templateArgCount,
                errStart,
                errEnd);
        case HOPTCType_TUPLE:
        case HOPTCType_PACK:
        case HOPTCType_FUNCTION: {
            uint32_t j;
            if (c->types[resolvedParam].fieldCount != c->types[resolvedArg].fieldCount) {
                return 0;
            }
            if (c->types[resolvedParam].kind == HOPTCType_FUNCTION
                && HOPTCInferTemplateArgsFromTypes(
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
                if (HOPTCInferTemplateArgsFromTypes(
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
        case HOPTCType_NAMED:
        case HOPTCType_ALIAS: {
            const HOPTCNamedType* paramNt = NULL;
            const HOPTCNamedType* argNt = NULL;
            uint32_t              ni;
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
                    if (HOPTCInferTemplateArgsFromTypes(
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

int HOPTCInstantiateAnytypeFunctionForCall(
    HOPTypeCheckCtx*        c,
    int32_t                 fnIndex,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int32_t                 autoRefFirstArgType,
    int32_t*                outFuncIndex,
    HOPTCCallMapError*      outError) {
    const HOPTCFunction* fn;
    int32_t              resolvedParamTypes[HOPTC_MAX_CALL_ARGS];
    uint32_t             p;
    HOPTCCallBinding     binding;
    HOPTCCallMapError    mapError;
    uint8_t              hasAnytypeParam = 0;
    uint8_t              hasAnyPack = 0;
    int32_t              templateArgTypes[64];
    int32_t              expectedReturnType = c->activeExpectedCallType;
    int32_t              packElems[HOPTC_MAX_CALL_ARGS];
    uint32_t             packElemCount = 0;

    if (outError != NULL) {
        HOPTCCallMapErrorClear(outError);
    }
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen || outFuncIndex == NULL) {
        return -1;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & HOPTCFunctionFlag_TEMPLATE) == 0
        || (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) != 0)
    {
        *outFuncIndex = fnIndex;
        return 0;
    }
    if (fn->paramCount > HOPTC_MAX_CALL_ARGS) {
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
            if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0 && p + 1u == fn->paramCount) {
                hasAnyPack = 1;
            }
        }
    }
    if (!hasAnytypeParam && fn->templateArgCount == 0) {
        *outFuncIndex = fnIndex;
        return 0;
    }

    HOPTCCallMapErrorClear(&mapError);
    {
        int prepStatus = HOPTCPrepareCallBinding(
            c,
            callArgs,
            argCount,
            &c->funcParamNameStarts[fn->paramTypeStart],
            &c->funcParamNameEnds[fn->paramTypeStart],
            &c->funcParamTypes[fn->paramTypeStart],
            fn->paramCount,
            (fn->flags & HOPTCFunctionFlag_VARIADIC) != 0,
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
        } else if (!HOPTCExprNeedsExpectedType(c, argExprNode)) {
            if (HOPTCTypeExpr(c, argExprNode, &argType) != 0) {
                c->activeExpectedCallType = savedExpectedCallType;
                return -1;
            }
        } else {
            HOPDiag savedDiag = { 0 };
            if (c->diag != NULL) {
                savedDiag = *c->diag;
            }
            if (HOPTCTypeExprExpected(c, argExprNode, expectedType, &argType) != 0) {
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
            && HOPTCInferTemplateArgsFromTypes(
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
        if (HOPTCConcretizeInferredType(c, argType, &argType) != 0) {
            return -1;
        }

        if (expectedType == c->typeAnytype) {
            if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0
                && (uint32_t)paramIndex + 1u == fn->paramCount)
            {
                if (binding.spreadArgIndex == p) {
                    int32_t spreadType = HOPTCResolveAliasBaseType(c, argType);
                    if (spreadType < 0 || (uint32_t)spreadType >= c->typeLen
                        || c->types[spreadType].kind != HOPTCType_PACK)
                    {
                        if (outError != NULL) {
                            outError->code = HOPDiag_ANYTYPE_SPREAD_REQUIRES_PACK;
                            outError->start = callArgs[p].start;
                            outError->end = callArgs[p].end;
                            outError->argStart = 0;
                            outError->argEnd = 0;
                        }
                        return 2;
                    }
                    resolvedParamTypes[paramIndex] = spreadType;
                } else {
                    if (packElemCount >= HOPTC_MAX_CALL_ARGS) {
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
        if (HOPTCInferTemplateArgsFromTypes(
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
            int32_t packType = HOPTCInternPackType(
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
                || c->types[spreadType].kind != HOPTCType_PACK)
            {
                return 1;
            }
        }
    }

    for (p = 0; p < fn->templateArgCount; p++) {
        if (templateArgTypes[p] < 0) {
            return 1;
        }
        if (HOPTCConcretizeInferredType(c, templateArgTypes[p], &templateArgTypes[p]) != 0) {
            return -1;
        }
    }
    if (fn->templateArgCount > 0) {
        for (p = 0; p < fn->paramCount; p++) {
            resolvedParamTypes[p] = HOPTCSubstituteType(
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
        const HOPTCFunction* cur = &c->funcs[p];
        uint32_t             j;
        if (cur->declNode != fn->declNode || (cur->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0
            || cur->paramCount != fn->paramCount || cur->contextType != fn->contextType
            || ((cur->flags & HOPTCFunctionFlag_VARIADIC)
                != (fn->flags & HOPTCFunctionFlag_VARIADIC)))
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
                || (c->funcParamFlags[cur->paramTypeStart + j] & HOPTCFuncParamFlag_CONST)
                       != (c->funcParamFlags[fn->paramTypeStart + j] & HOPTCFuncParamFlag_CONST))
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
        return HOPTCFailNode(c, fn->declNode, HOPDiag_ARENA_OOM);
    }

    {
        uint32_t       idx = c->funcLen++;
        HOPTCFunction* f = &c->funcs[idx];
        int32_t        typeId;
        for (p = 0; p < fn->paramCount; p++) {
            c->funcParamTypes[c->funcParamLen + p] = resolvedParamTypes[p];
            c->funcParamNameStarts[c->funcParamLen + p] =
                c->funcParamNameStarts[fn->paramTypeStart + p];
            c->funcParamNameEnds[c->funcParamLen + p] =
                c->funcParamNameEnds[fn->paramTypeStart + p];
            c->funcParamFlags[c->funcParamLen + p] =
                c->funcParamFlags[fn->paramTypeStart + p] & HOPTCFuncParamFlag_CONST;
        }
        f->nameStart = fn->nameStart;
        f->nameEnd = fn->nameEnd;
        f->returnType =
            fn->templateArgCount > 0
                ? HOPTCSubstituteType(
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
                ? HOPTCSubstituteType(
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
        f->flags = (fn->flags & HOPTCFunctionFlag_VARIADIC) | HOPTCFunctionFlag_TEMPLATE
                 | HOPTCFunctionFlag_TEMPLATE_INSTANCE;
        for (p = 0; p < fn->templateArgCount; p++) {
            c->genericArgTypes[c->genericArgLen++] = templateArgTypes[p];
        }
        if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0 && fn->paramCount > 0) {
            int32_t variadicType = resolvedParamTypes[fn->paramCount - 1u];
            if (variadicType >= 0 && (uint32_t)variadicType < c->typeLen
                && c->types[variadicType].kind == HOPTCType_PACK)
            {
                f->flags |= HOPTCFunctionFlag_TEMPLATE_HAS_ANYPACK;
            }
        }
        c->funcParamLen += fn->paramCount;
        typeId = HOPTCInternFunctionType(
            c,
            f->returnType,
            &c->funcParamTypes[f->paramTypeStart],
            &c->funcParamFlags[f->paramTypeStart],
            f->paramCount,
            (f->flags & HOPTCFunctionFlag_VARIADIC) != 0,
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

int HOPTCResolveCallFromCandidates(
    HOPTypeCheckCtx*        c,
    const int32_t*          candidates,
    uint32_t                candidateCount,
    int                     nameFound,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int                     autoRefFirstArg,
    int32_t*                outFuncIndex,
    int32_t*                outMutRefTempArgNode) {
    uint8_t           bestCosts[HOPTC_MAX_CALL_ARGS];
    int               haveBest = 0;
    int               ambiguous = 0;
    int               hasExpectedDependentArg = 0;
    int32_t           mutRefTempArgNode = -1;
    int32_t           autoRefType = -1;
    int               hasAutoRefType = 0;
    HOPTCCallMapError firstMapError;
    int               hasMapError = 0;
    uint32_t          bestTotal = 0;
    uint32_t          i;
    uint32_t          p;
    HOPTCCallMapErrorClear(&firstMapError);
    if (!nameFound) {
        return 1;
    }

    if (argCount > HOPTC_MAX_CALL_ARGS) {
        return -1;
    }
    if (autoRefFirstArg && argCount > 0 && HOPTCExprIsAssignable(c, callArgs[0].exprNode)) {
        int32_t           argType;
        const HOPAstNode* argNode = &c->ast->nodes[callArgs[0].exprNode];
        if (HOPTCTypeExpr(c, callArgs[0].exprNode, &argType) != 0) {
            return -1;
        }
        autoRefType = HOPTCInternPtrType(c, argType, argNode->start, argNode->end);
        if (autoRefType < 0) {
            return -1;
        }
        hasAutoRefType = 1;
    }
    for (p = 0; p < argCount; p++) {
        hasExpectedDependentArg =
            hasExpectedDependentArg || HOPTCExprNeedsExpectedType(c, callArgs[p].exprNode) != 0;
    }

    for (i = 0; i < candidateCount; i++) {
        int32_t              fnIdx = candidates[i];
        int32_t              candidateFnIdx = fnIdx;
        const HOPTCFunction* fn = &c->funcs[candidateFnIdx];
        uint8_t              curCosts[HOPTC_MAX_CALL_ARGS];
        HOPTCCallBinding     binding;
        uint32_t             curTotal = 0;
        int                  viable = 1;
        int                  cmp;
        HOPTCCallMapError    mapError;
        HOPTCCallMapErrorClear(&mapError);
        if ((fn->flags & HOPTCFunctionFlag_TEMPLATE) != 0
            && (fn->flags & HOPTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
        {
            int instantiateStatus = HOPTCInstantiateAnytypeFunctionForCall(
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
                }
                continue;
            }
            fn = &c->funcs[candidateFnIdx];
        }
        {
            int prepStatus = HOPTCPrepareCallBinding(
                c,
                callArgs,
                argCount,
                &c->funcParamNameStarts[fn->paramTypeStart],
                &c->funcParamNameEnds[fn->paramTypeStart],
                &c->funcParamTypes[fn->paramTypeStart],
                fn->paramCount,
                (fn->flags & HOPTCFunctionFlag_VARIADIC) != 0,
                1,
                firstPositionalArgIndex,
                &binding,
                &mapError);
            if (prepStatus != 0) {
                if (prepStatus == 2 && !hasMapError && mapError.code != 0) {
                    hasMapError = 1;
                    firstMapError = mapError;
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
                if (HOPTCIsMutableRefType(c, paramType) && HOPTCExprIsCompoundTemporary(c, argNode))
                {
                    if (mutRefTempArgNode < 0) {
                        mutRefTempArgNode = argNode;
                    }
                    viable = 0;
                    break;
                }
                if (!HOPTCExprNeedsExpectedType(c, argNode)) {
                    if (HOPTCTypeExpr(c, argNode, &argType) != 0) {
                        return -1;
                    }
                } else {
                    HOPDiag savedDiag = { 0 };
                    if (c->diag != NULL) {
                        savedDiag = *c->diag;
                    }
                    if (HOPTCTypeExprExpected(c, argNode, paramType, &argType) != 0) {
                        if (c->diag != NULL) {
                            *c->diag = savedDiag;
                        }
                        if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0
                            && p >= binding.fixedInputCount)
                        {
                            mapError.code =
                                (binding.spreadArgIndex == p)
                                    ? HOPDiag_VARIADIC_SPREAD_NON_SLICE
                                    : HOPDiag_VARIADIC_ARG_TYPE_MISMATCH;
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
            if (HOPTCConversionCost(c, paramType, argType, &cost) != 0) {
                if ((fn->flags & HOPTCFunctionFlag_VARIADIC) != 0 && p >= binding.fixedInputCount) {
                    mapError.code =
                        (binding.spreadArgIndex == p)
                            ? HOPDiag_VARIADIC_SPREAD_NON_SLICE
                            : HOPDiag_VARIADIC_ARG_TYPE_MISMATCH;
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
            int constStatus = HOPTCCheckConstParamArgs(
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
            int constBlockStatus = HOPTCCheckConstBlocksForCall(
                c, candidateFnIdx, callArgs, argCount, &binding, &mapError);
            if (constBlockStatus < 0) {
                return -1;
            }
            if (constBlockStatus != 0) {
                viable = 0;
            }
        }
        if (!viable) {
            if (!hasMapError && mapError.code != 0) {
                hasMapError = 1;
                firstMapError = mapError;
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
        cmp = HOPTCCostVectorCompare(curCosts, bestCosts, argCount);
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
            HOPTCSetDiagWithArg(
                c->diag,
                firstMapError.code,
                firstMapError.start,
                firstMapError.end,
                firstMapError.argStart,
                firstMapError.argEnd);
            if (firstMapError.code == HOPDiag_CONST_BLOCK_EVAL_FAILED) {
                HOPTCAttachConstEvalReason(c);
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

int HOPTCResolveCallByName(
    HOPTypeCheckCtx*        c,
    uint32_t                nameStart,
    uint32_t                nameEnd,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int                     autoRefFirstArg,
    int32_t*                outFuncIndex,
    int32_t*                outMutRefTempArgNode) {
    int32_t  candidates[HOPTC_MAX_CALL_CANDIDATES];
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

    HOPTCGatherCallCandidates(c, nameStart, nameEnd, candidates, &candidateCount, &nameFound);
    return HOPTCResolveCallFromCandidates(
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

int HOPTCResolveCallByPkgMethod(
    HOPTypeCheckCtx*        c,
    uint32_t                pkgStart,
    uint32_t                pkgEnd,
    uint32_t                methodStart,
    uint32_t                methodEnd,
    const HOPTCCallArgInfo* callArgs,
    uint32_t                argCount,
    uint32_t                firstPositionalArgIndex,
    int                     autoRefFirstArg,
    int32_t*                outFuncIndex,
    int32_t*                outMutRefTempArgNode) {
    int32_t  candidates[HOPTC_MAX_CALL_CANDIDATES];
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

    HOPTCGatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, methodStart, methodEnd, candidates, &candidateCount, &nameFound);
    return HOPTCResolveCallFromCandidates(
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

int HOPTCResolveDependentPtrReturnForCall(
    HOPTypeCheckCtx* c, int32_t fnIndex, int32_t argNode, int32_t* outType) {
    const HOPTCFunction* fn;
    int32_t              returnType;
    int32_t              reflectedType = -1;
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
        || c->types[returnType].kind != HOPTCType_PTR
        || c->types[returnType].baseType != c->typeType)
    {
        return 0;
    }
    if (HOPTCResolveReflectedTypeValueExpr(c, argNode, &reflectedType) != 0) {
        return 0;
    }
    reflectedType = HOPTCInternPtrType(
        c, reflectedType, c->ast->nodes[argNode].start, c->ast->nodes[argNode].end);
    if (reflectedType < 0) {
        return -1;
    }
    *outType = reflectedType;
    return 1;
}

HOP_API_END
