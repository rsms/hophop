#include "internal.h"
#include "../mir_exec.h"
#include "../mir_lower_stmt.h"

SL_API_BEGIN

static int SLTCTryMirConstBlock(
    SLTCConstEvalCtx* evalCtx,
    int32_t           blockNode,
    SLCTFEValue*      outValue,
    int*              outDidReturn,
    int*              outIsConst,
    int*              outSupported) {
    SLTypeCheckCtx*      c;
    SLMirProgram         program = { 0 };
    SLMirExecEnv         env = { 0 };
    SLTCMirConstLowerCtx lowerCtx;
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
    if (SLTCMirConstInitLowerCtx(evalCtx, &lowerCtx) != 0) {
        return -1;
    }
    if (SLMirLowerAppendSimpleFunction(
            &lowerCtx.builder,
            c->arena,
            c->ast,
            c->src,
            -1,
            blockNode,
            &mirFnIndex,
            &supported,
            c->diag)
        != 0)
    {
        return -1;
    }
    if (!supported || mirFnIndex == UINT32_MAX) {
        return 0;
    }
    rewriteRc = SLTCMirConstRewriteDirectCalls(&lowerCtx, mirFnIndex);
    if (rewriteRc < 0) {
        return -1;
    }
    if (rewriteRc == 0) {
        return 0;
    }
    SLMirProgramBuilderFinish(&lowerCtx.builder, &program);
    env.src = c->src;
    env.resolveIdent = SLTCResolveConstIdent;
    env.resolveCall = SLTCResolveConstCall;
    env.resolveCtx = evalCtx;
    env.zeroInitLocal = SLTCMirConstZeroInitLocal;
    env.zeroInitCtx = evalCtx;
    env.coerceValueForType = SLTCMirConstCoerceValueForType;
    env.coerceValueCtx = evalCtx;
    env.indexValue = SLTCMirConstIndexValue;
    env.indexValueCtx = evalCtx;
    env.makeTuple = SLTCMirConstMakeTuple;
    env.makeTupleCtx = evalCtx;
    env.backwardJumpLimit = SLTC_CONST_FOR_MAX_ITERS;
    env.diag = c->diag;
    if (SLMirEvalFunction(c->arena, &program, mirFnIndex, NULL, 0, &env, outValue, &mirIsConst)
        != 0)
    {
        return -1;
    }
    *outSupported = 1;
    *outIsConst = mirIsConst;
    if (mirIsConst) {
        *outDidReturn = outValue->kind != SLCTFEValue_INVALID;
    }
    return 0;
}

static void SLTCInitConstEvalCtxFromParent(
    SLTypeCheckCtx* c, const SLTCConstEvalCtx* _Nullable parent, SLTCConstEvalCtx* outCtx) {
    if (outCtx == NULL) {
        return;
    }
    memset(outCtx, 0, sizeof(*outCtx));
    outCtx->tc = c;
    if (parent == NULL) {
        return;
    }
    outCtx->execCtx = parent->execCtx;
    outCtx->callArgs = parent->callArgs;
    outCtx->callArgCount = parent->callArgCount;
    outCtx->callBinding = parent->callBinding;
    outCtx->callPackParamNameStart = parent->callPackParamNameStart;
    outCtx->callPackParamNameEnd = parent->callPackParamNameEnd;
    outCtx->fnDepth = parent->fnDepth;
    memcpy(outCtx->fnStack, parent->fnStack, sizeof(outCtx->fnStack));
}

int SLTCResolveAnonAggregateTypeNode(
    SLTypeCheckCtx* c, int32_t nodeId, int isUnion, int32_t* outType) {
    int32_t          fieldNode = SLAstFirstChild(c->ast, nodeId);
    SLTCAnonFieldSig fieldSigs[SLTC_MAX_ANON_FIELDS];
    uint32_t         fieldCount = 0;

    while (fieldNode >= 0) {
        const SLAstNode* field = &c->ast->nodes[fieldNode];
        int32_t          typeNode;
        int32_t          typeId;
        uint32_t         i;
        if (field->kind != SLAst_FIELD) {
            return SLTCFailNode(c, fieldNode, SLDiag_EXPECTED_TYPE);
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
                return SLTCFailSpan(c, SLDiag_DUPLICATE_SYMBOL, field->dataStart, field->dataEnd);
            }
        }
        typeNode = SLAstFirstChild(c->ast, fieldNode);
        if (typeNode < 0) {
            return SLTCFailNode(c, fieldNode, SLDiag_EXPECTED_TYPE);
        }
        if (c->ast->nodes[typeNode].kind == SLAst_TYPE_VARRAY) {
            return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
        }
        if (SLTCResolveTypeNode(c, typeNode, &typeId) != 0) {
            return -1;
        }
        fieldSigs[fieldCount].nameStart = field->dataStart;
        fieldSigs[fieldCount].nameEnd = field->dataEnd;
        fieldSigs[fieldCount].typeId = typeId;
        fieldCount++;
        fieldNode = SLAstNextSibling(c->ast, fieldNode);
    }

    {
        int32_t typeId = SLTCInternAnonAggregateType(
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

int SLTCResolveAliasTypeId(SLTypeCheckCtx* c, int32_t typeId) {
    SLTCType*        t;
    int32_t          targetNode;
    int32_t          targetType = -1;
    const SLAstNode* decl;

    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return SLTCFailSpan(c, SLDiag_UNKNOWN_TYPE, 0, 0);
    }
    t = &c->types[typeId];
    if (t->kind != SLTCType_ALIAS) {
        return 0;
    }
    if ((t->flags & SLTCTypeFlag_ALIAS_RESOLVED) != 0) {
        return 0;
    }
    if ((t->flags & SLTCTypeFlag_ALIAS_RESOLVING) != 0) {
        return SLTCFailNode(c, t->declNode, SLDiag_TYPE_MISMATCH);
    }
    if (t->declNode < 0 || (uint32_t)t->declNode >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_UNKNOWN_TYPE, 0, 0);
    }

    decl = &c->ast->nodes[t->declNode];
    targetNode = SLAstFirstChild(c->ast, t->declNode);
    if (targetNode < 0) {
        return SLTCFailNode(c, t->declNode, SLDiag_EXPECTED_TYPE);
    }

    t->flags |= SLTCTypeFlag_ALIAS_RESOLVING;
    if (SLTCResolveTypeNode(c, targetNode, &targetType) != 0) {
        t->flags &= (uint16_t)~SLTCTypeFlag_ALIAS_RESOLVING;
        return -1;
    }
    if (targetType < 0 || (uint32_t)targetType >= c->typeLen || targetType == typeId) {
        t->flags &= (uint16_t)~SLTCTypeFlag_ALIAS_RESOLVING;
        return SLTCFailSpan(c, SLDiag_TYPE_MISMATCH, decl->start, decl->end);
    }
    if (c->types[targetType].kind == SLTCType_ALIAS && SLTCResolveAliasTypeId(c, targetType) != 0) {
        t->flags &= (uint16_t)~SLTCTypeFlag_ALIAS_RESOLVING;
        return -1;
    }

    t->baseType = targetType;
    t->flags &= (uint16_t)~SLTCTypeFlag_ALIAS_RESOLVING;
    t->flags |= SLTCTypeFlag_ALIAS_RESOLVED;
    return 0;
}

int32_t SLTCResolveAliasBaseType(SLTypeCheckCtx* c, int32_t typeId) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == SLTCType_ALIAS
           && depth++ <= c->typeLen)
    {
        if (SLTCResolveAliasTypeId(c, typeId) != 0) {
            return -1;
        }
        typeId = c->types[typeId].baseType;
    }
    return typeId;
}

int SLTCFnNodeHasTypeParamName(
    SLTypeCheckCtx* c, int32_t fnNode, uint32_t nameStart, uint32_t nameEnd) {
    int32_t child;
    if (c == NULL || fnNode < 0 || (uint32_t)fnNode >= c->ast->len) {
        return 0;
    }
    if (c->ast->nodes[fnNode].kind != SLAst_FN) {
        return 0;
    }
    child = SLAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            int32_t          typeNode = SLAstFirstChild(c->ast, child);
            const SLAstNode* t = typeNode >= 0 ? &c->ast->nodes[typeNode] : NULL;
            if (t != NULL && t->kind == SLAst_TYPE_NAME
                && SLNameEqLiteral(c->src, t->dataStart, t->dataEnd, "type")
                && SLNameEqSlice(c->src, n->dataStart, n->dataEnd, nameStart, nameEnd))
            {
                return 1;
            }
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

int SLTCResolveActiveTypeParamType(
    SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t* outType) {
    if (c == NULL || outType == NULL) {
        return 0;
    }
    if (c->currentFunctionIndex >= 0 && (uint32_t)c->currentFunctionIndex < c->funcLen) {
        const SLTCFunction* fn = &c->funcs[c->currentFunctionIndex];
        uint32_t            p;
        for (p = 0; p < fn->paramCount; p++) {
            uint32_t paramIndex = fn->paramTypeStart + p;
            if (c->funcParamTypes[paramIndex] == c->typeType
                && SLNameEqSlice(
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
        && SLTCFnNodeHasTypeParamName(c, c->activeTypeParamFnNode, nameStart, nameEnd))
    {
        *outType = c->typeType;
        return 1;
    }
    return 0;
}

int SLTCResolveTypeNode(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_TYPE, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case SLAst_TYPE_NAME: {
            if (SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "anytype")) {
                if (!c->allowAnytypeParamType) {
                    return SLTCFailSpan(
                        c, SLDiag_ANYTYPE_INVALID_POSITION, n->dataStart, n->dataEnd);
                }
                *outType = c->typeAnytype;
                return 0;
            }
            int32_t typeId = SLTCFindBuiltinType(c, n->dataStart, n->dataEnd);
            if (typeId >= 0) {
                if ((typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat)
                    && !c->allowConstNumericTypeName)
                {
                    return SLTCFailSpan(
                        c, SLDiag_CONST_NUMERIC_INVALID_POSITION, n->dataStart, n->dataEnd);
                }
                *outType = typeId;
                return 0;
            }
            {
                int32_t resolvedType = SLTCResolveTypeNamePath(
                    c, n->dataStart, n->dataEnd, c->currentTypeOwnerTypeId);
                if (resolvedType >= 0) {
                    *outType = resolvedType;
                    if (*outType >= 0 && (uint32_t)*outType < c->typeLen
                        && c->types[*outType].kind == SLTCType_ALIAS
                        && SLTCResolveAliasTypeId(c, *outType) != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
            }
            {
                int32_t activeTypeParamType = -1;
                if (SLTCResolveActiveTypeParamType(
                        c, n->dataStart, n->dataEnd, &activeTypeParamType))
                {
                    *outType = activeTypeParamType;
                    return 0;
                }
            }
            {
                int32_t varLikeNameIndex = -1;
                int32_t varLikeNode = SLTCFindTopLevelVarLikeNode(
                    c, n->dataStart, n->dataEnd, &varLikeNameIndex);
                if (varLikeNode >= 0 && c->ast->nodes[varLikeNode].kind == SLAst_CONST) {
                    int32_t          resolvedConstType = -1;
                    SLTCConstEvalCtx evalCtx;
                    SLCTFEValue      value;
                    int              isConst = 0;
                    int32_t          reflectedType = -1;
                    if (SLTCTypeTopLevelVarLikeNode(
                            c, varLikeNode, varLikeNameIndex, &resolvedConstType)
                        != 0)
                    {
                        return -1;
                    }
                    if (resolvedConstType == c->typeType) {
                        memset(&evalCtx, 0, sizeof(evalCtx));
                        evalCtx.tc = c;
                        if (SLTCEvalTopLevelConstNodeAt(
                                c, &evalCtx, varLikeNode, varLikeNameIndex, &value, &isConst)
                            != 0)
                        {
                            return -1;
                        }
                        if (isConst && value.kind == SLCTFEValue_TYPE
                            && SLTCDecodeTypeTag(c, value.typeTag, &reflectedType) == 0)
                        {
                            *outType = reflectedType;
                            return 0;
                        }
                    }
                }
            }
            return SLTCFailSpan(c, SLDiag_UNKNOWN_TYPE, n->dataStart, n->dataEnd);
        }
        case SLAst_TYPE_PTR: {
            int32_t          child = SLAstFirstChild(c->ast, nodeId);
            int32_t          baseType;
            int32_t          ptrType;
            const SLAstNode* childNode;
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
            }
            childNode = &c->ast->nodes[child];
            if (childNode->kind == SLAst_TYPE_SLICE || childNode->kind == SLAst_TYPE_MUTSLICE) {
                int32_t elemNode = SLAstFirstChild(c->ast, child);
                int32_t elemType;
                int32_t sliceType;
                if (elemNode < 0) {
                    return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
                }
                if (SLTCResolveTypeNode(c, elemNode, &elemType) != 0) {
                    return -1;
                }
                sliceType = SLTCInternSliceType(c, elemType, 1, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                ptrType = SLTCInternPtrType(c, sliceType, n->start, n->end);
                if (ptrType < 0) {
                    return -1;
                }
                *outType = ptrType;
                return 0;
            }
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            if (baseType >= 0 && (uint32_t)baseType < c->typeLen
                && c->types[baseType].kind == SLTCType_SLICE
                && !SLTCTypeIsMutable(&c->types[baseType]))
            {
                int32_t mutableSliceType = SLTCInternSliceType(
                    c, c->types[baseType].baseType, 1, n->start, n->end);
                if (mutableSliceType < 0) {
                    return -1;
                }
                baseType = mutableSliceType;
            }
            ptrType = SLTCInternPtrType(c, baseType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF: {
            int32_t          child = SLAstFirstChild(c->ast, nodeId);
            int32_t          baseType;
            int32_t          refType;
            const SLAstNode* childNode;
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
            }
            childNode = &c->ast->nodes[child];
            if (childNode->kind == SLAst_TYPE_SLICE || childNode->kind == SLAst_TYPE_MUTSLICE) {
                int32_t elemNode = SLAstFirstChild(c->ast, child);
                int32_t elemType;
                int32_t sliceType;
                if (elemNode < 0) {
                    return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
                }
                if (SLTCResolveTypeNode(c, elemNode, &elemType) != 0) {
                    return -1;
                }
                sliceType = SLTCInternSliceType(c, elemType, 0, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                refType = SLTCInternRefType(
                    c, sliceType, n->kind == SLAst_TYPE_MUTREF, n->start, n->end);
                if (refType < 0) {
                    return -1;
                }
                *outType = refType;
                return 0;
            }
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            if (baseType >= 0 && (uint32_t)baseType < c->typeLen
                && c->types[baseType].kind == SLTCType_SLICE
                && SLTCTypeIsMutable(&c->types[baseType]))
            {
                int32_t readOnlySliceType = SLTCInternSliceType(
                    c, c->types[baseType].baseType, 0, n->start, n->end);
                if (readOnlySliceType < 0) {
                    return -1;
                }
                baseType = readOnlySliceType;
            }
            refType = SLTCInternRefType(
                c, baseType, n->kind == SLAst_TYPE_MUTREF, n->start, n->end);
            if (refType < 0) {
                return -1;
            }
            *outType = refType;
            return 0;
        }
        case SLAst_TYPE_ARRAY: {
            int32_t  child = SLAstFirstChild(c->ast, nodeId);
            int32_t  lenNode = child >= 0 ? SLAstNextSibling(c->ast, child) : -1;
            int32_t  baseType;
            int32_t  arrayType;
            int64_t  lenValue = 0;
            int      lenIsConst = 0;
            uint32_t arrayLen;
            if (child < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
            }
            if (lenNode >= 0) {
                int32_t lenType;
                if (SLAstNextSibling(c->ast, lenNode) >= 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
                }
                if (SLTCTypeExpr(c, lenNode, &lenType) != 0) {
                    return -1;
                }
                if (!SLTCIsIntegerType(c, lenType)) {
                    return SLTCFailNode(c, lenNode, SLDiag_TYPE_MISMATCH);
                }
                if (SLTCConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0) {
                    return -1;
                }
                if (!lenIsConst) {
                    int rc = SLTCFailNode(c, lenNode, SLDiag_ARRAY_LEN_CONST_REQUIRED);
                    SLTCAttachConstEvalReason(c);
                    return rc;
                }
                if (lenValue < 0 || lenValue > (int64_t)UINT32_MAX) {
                    return SLTCFailNode(c, lenNode, SLDiag_ARRAY_LEN_CONST_REQUIRED);
                }
                arrayLen = (uint32_t)lenValue;
            } else if (SLTCParseArrayLen(c, n, &arrayLen) != 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
            }
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            arrayType = SLTCInternArrayType(c, baseType, arrayLen, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            *outType = arrayType;
            return 0;
        }
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE: {
            int32_t child = SLAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t sliceType;
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            sliceType = SLTCInternSliceType(
                c, baseType, n->kind == SLAst_TYPE_MUTSLICE, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            *outType = sliceType;
            return 0;
        }
        case SLAst_TYPE_OPTIONAL: {
            int32_t child = SLAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t optType;
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            optType = SLTCInternOptionalType(c, baseType, n->start, n->end);
            if (optType < 0) {
                return -1;
            }
            *outType = optType;
            return 0;
        }
        case SLAst_TYPE_FN: {
            int32_t  child = SLAstFirstChild(c->ast, nodeId);
            int32_t  returnType = c->typeVoid;
            uint32_t paramCount = 0;
            int      isVariadic = 0;
            int      sawReturnType = 0;
            int32_t  savedParamTypes[SLTC_MAX_CALL_ARGS];
            uint8_t  savedParamFlags[SLTC_MAX_CALL_ARGS];
            while (child >= 0) {
                const SLAstNode* ch = &c->ast->nodes[child];
                if (ch->flags == 1) {
                    uint32_t i;
                    if (sawReturnType) {
                        return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
                    }
                    if (paramCount > SLTC_MAX_CALL_ARGS) {
                        return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
                    }
                    for (i = 0; i < paramCount; i++) {
                        savedParamTypes[i] = c->scratchParamTypes[i];
                        savedParamFlags[i] = c->scratchParamFlags[i];
                    }
                    c->allowConstNumericTypeName = 1;
                    if (SLTCResolveTypeNode(c, child, &returnType) != 0) {
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowConstNumericTypeName = 0;
                    for (i = 0; i < paramCount; i++) {
                        c->scratchParamTypes[i] = savedParamTypes[i];
                        c->scratchParamFlags[i] = savedParamFlags[i];
                    }
                    if (SLTCTypeContainsVarSizeByValue(c, returnType)) {
                        return SLTCFailNode(c, child, SLDiag_TYPE_MISMATCH);
                    }
                    sawReturnType = 1;
                } else {
                    int32_t paramType;
                    if (paramCount >= c->scratchParamCap) {
                        return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
                    }
                    c->allowAnytypeParamType = 1;
                    c->allowConstNumericTypeName = 1;
                    if (SLTCResolveTypeNode(c, child, &paramType) != 0) {
                        c->allowAnytypeParamType = 0;
                        c->allowConstNumericTypeName = 0;
                        return -1;
                    }
                    c->allowAnytypeParamType = 0;
                    c->allowConstNumericTypeName = 0;
                    if ((paramType == c->typeUntypedInt || paramType == c->typeUntypedFloat)
                        && (ch->flags & SLAstFlag_PARAM_CONST) == 0)
                    {
                        return SLTCFailSpan(
                            c,
                            SLDiag_CONST_NUMERIC_PARAM_REQUIRES_CONST,
                            ch->dataStart,
                            ch->dataEnd);
                    }
                    if (SLTCTypeContainsVarSizeByValue(c, paramType)) {
                        return SLTCFailNode(c, child, SLDiag_TYPE_MISMATCH);
                    }
                    if ((ch->flags & SLAstFlag_PARAM_VARIADIC) != 0) {
                        int32_t sliceType;
                        if (isVariadic) {
                            return SLTCFailNode(c, child, SLDiag_VARIADIC_PARAM_NOT_LAST);
                        }
                        if (paramType != c->typeAnytype) {
                            sliceType = SLTCInternSliceType(c, paramType, 0, ch->start, ch->end);
                            if (sliceType < 0) {
                                return -1;
                            }
                            paramType = sliceType;
                        }
                        isVariadic = 1;
                    } else if (isVariadic) {
                        return SLTCFailNode(c, child, SLDiag_VARIADIC_PARAM_NOT_LAST);
                    }
                    c->scratchParamTypes[paramCount++] = paramType;
                    c->scratchParamFlags[paramCount - 1u] =
                        (ch->flags & SLAstFlag_PARAM_CONST) != 0 ? SLTCFuncParamFlag_CONST : 0u;
                }
                child = SLAstNextSibling(c->ast, child);
            }
            {
                int32_t fnType = SLTCInternFunctionType(
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
        case SLAst_TYPE_TUPLE: {
            int32_t  child = SLAstFirstChild(c->ast, nodeId);
            uint32_t elemCount = 0;
            while (child >= 0) {
                int32_t elemType;
                if (elemCount >= c->scratchParamCap) {
                    return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
                }
                if (SLTCResolveTypeNode(c, child, &elemType) != 0) {
                    return -1;
                }
                if (SLTCTypeContainsVarSizeByValue(c, elemType)) {
                    return SLTCFailNode(c, child, SLDiag_TYPE_MISMATCH);
                }
                c->scratchParamTypes[elemCount++] = elemType;
                child = SLAstNextSibling(c->ast, child);
            }
            if (elemCount < 2u) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
            }
            {
                int32_t tupleType = SLTCInternTupleType(
                    c, c->scratchParamTypes, elemCount, n->start, n->end);
                if (tupleType < 0) {
                    return -1;
                }
                *outType = tupleType;
                return 0;
            }
        }
        case SLAst_TYPE_ANON_STRUCT: return SLTCResolveAnonAggregateTypeNode(c, nodeId, 0, outType);
        case SLAst_TYPE_ANON_UNION:  return SLTCResolveAnonAggregateTypeNode(c, nodeId, 1, outType);
        case SLAst_TYPE_VARRAY:      return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
        default:                     return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
}

int SLTCAddNamedType(SLTypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId, int32_t* outTypeId) {
    const SLAstNode* node = &c->ast->nodes[nodeId];
    SLTCType         t;
    int32_t          typeId;
    uint32_t         idx;

    if (node->dataEnd <= node->dataStart) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }

    if (SLTCFindNamedTypeIndexOwned(c, ownerTypeId, node->dataStart, node->dataEnd) >= 0) {
        return SLTCFailSpan(c, SLDiag_DUPLICATE_SYMBOL, node->dataStart, node->dataEnd);
    }

    t.kind = node->kind == SLAst_TYPE_ALIAS ? SLTCType_ALIAS : SLTCType_NAMED;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = nodeId;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = node->dataStart;
    t.nameEnd = node->dataEnd;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    typeId = SLTCAddType(c, &t, node->start, node->end);
    if (typeId < 0) {
        return -1;
    }

    if (c->namedTypeLen >= c->namedTypeCap) {
        return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
    }

    idx = c->namedTypeLen++;
    c->namedTypes[idx].nameStart = node->dataStart;
    c->namedTypes[idx].nameEnd = node->dataEnd;
    c->namedTypes[idx].typeId = typeId;
    c->namedTypes[idx].declNode = nodeId;
    c->namedTypes[idx].ownerTypeId = ownerTypeId;
    if (outTypeId != NULL) {
        *outTypeId = typeId;
    }
    return 0;
}

static int SLTCIsNamedTypeDeclKind(SLAstKind kind) {
    return kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS;
}

static int SLTCCollectTypeDeclsFromNodeWithOwner(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t ownerTypeId) {
    SLAstKind kind = c->ast->nodes[nodeId].kind;
    if (kind == SLAst_PUB) {
        int32_t ch = SLAstFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (SLTCCollectTypeDeclsFromNodeWithOwner(c, ch, ownerTypeId) != 0) {
                return -1;
            }
            ch = SLAstNextSibling(c->ast, ch);
        }
        return 0;
    }
    if (SLTCIsNamedTypeDeclKind(kind)) {
        int32_t declaredTypeId = -1;
        if (SLTCAddNamedType(c, nodeId, ownerTypeId, &declaredTypeId) != 0) {
            return -1;
        }
        if (kind == SLAst_STRUCT || kind == SLAst_UNION) {
            int32_t child = SLAstFirstChild(c->ast, nodeId);
            while (child >= 0) {
                SLAstKind childKind = c->ast->nodes[child].kind;
                if (SLTCIsNamedTypeDeclKind(childKind)
                    && SLTCCollectTypeDeclsFromNodeWithOwner(c, child, declaredTypeId) != 0)
                {
                    return -1;
                }
                child = SLAstNextSibling(c->ast, child);
            }
        }
        return 0;
    }
    return 0;
}

int SLTCCollectTypeDeclsFromNode(SLTypeCheckCtx* c, int32_t nodeId) {
    return SLTCCollectTypeDeclsFromNodeWithOwner(c, nodeId, -1);
}

int SLTCIsIntegerType(SLTypeCheckCtx* c, int32_t typeId) {
    SLBuiltinKind b;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedInt) {
        return 1;
    }
    if (c->types[typeId].kind != SLTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == SLBuiltin_U8 || b == SLBuiltin_U16 || b == SLBuiltin_U32 || b == SLBuiltin_U64
        || b == SLBuiltin_I8 || b == SLBuiltin_I16 || b == SLBuiltin_I32 || b == SLBuiltin_I64
        || b == SLBuiltin_USIZE || b == SLBuiltin_ISIZE;
}

int SLTCIsConstNumericType(SLTypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

int SLTCTypeIsRuneLike(SLTypeCheckCtx* c, int32_t typeId) {
    uint32_t depth = 0;
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && depth++ <= c->typeLen) {
        if (typeId == c->typeRune) {
            return 1;
        }
        if (c->types[typeId].kind != SLTCType_ALIAS) {
            break;
        }
        if (SLTCResolveAliasTypeId(c, typeId) != 0) {
            return 0;
        }
        typeId = c->types[typeId].baseType;
    }
    return 0;
}

uint32_t SLTCU64BitLen(uint64_t v) {
    uint32_t bits = 0;
    while (v != 0u) {
        v >>= 1u;
        bits++;
    }
    return bits;
}

int SLTCConstIntFitsType(SLTypeCheckCtx* c, int64_t value, int32_t typeId) {
    SLBuiltinKind b;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedInt) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != SLTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    switch (b) {
        case SLBuiltin_U8:    return value >= 0 && value <= (int64_t)UINT8_MAX;
        case SLBuiltin_U16:   return value >= 0 && value <= (int64_t)UINT16_MAX;
        case SLBuiltin_U32:   return value >= 0 && value <= (int64_t)UINT32_MAX;
        case SLBuiltin_U64:
        case SLBuiltin_USIZE: return value >= 0;
        case SLBuiltin_I8:    return value >= (int64_t)INT8_MIN && value <= (int64_t)INT8_MAX;
        case SLBuiltin_I16:   return value >= (int64_t)INT16_MIN && value <= (int64_t)INT16_MAX;
        case SLBuiltin_I32:   return value >= (int64_t)INT32_MIN && value <= (int64_t)INT32_MAX;
        case SLBuiltin_I64:
        case SLBuiltin_ISIZE: return 1;
        default:              return 0;
    }
}

int SLTCConstIntFitsFloatType(SLTypeCheckCtx* c, int64_t value, int32_t typeId) {
    SLBuiltinKind b;
    uint32_t      precisionBits;
    uint64_t      magnitude;
    uint32_t      bits;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != SLTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    if (b == SLBuiltin_F32) {
        precisionBits = 23u;
    } else if (b == SLBuiltin_F64) {
        precisionBits = 53u;
    } else {
        return 0;
    }
    if (value < 0) {
        magnitude = (uint64_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint64_t)value;
    }
    bits = SLTCU64BitLen(magnitude);
    return bits <= precisionBits;
}

int SLTCConstFloatFitsType(SLTypeCheckCtx* c, double value, int32_t typeId) {
    SLBuiltinKind b;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != SLTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    if (b == SLBuiltin_F64) {
        return 1;
    }
    if (b != SLBuiltin_F32) {
        return 0;
    }
    if (value != value) {
        return 1;
    }
    return (double)(float)value == value;
}

int SLTCFailConstIntRange(SLTypeCheckCtx* c, int32_t nodeId, int64_t value, int32_t expectedType) {
    char        dstTypeBuf[SLTC_DIAG_TEXT_CAP];
    char        detailBuf[256];
    SLTCTextBuf dstTypeText;
    SLTCTextBuf detailText;
    SLTCSetDiag(
        c->diag, SLDiag_TYPE_MISMATCH, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
    if (c->diag == NULL) {
        return -1;
    }
    SLTCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    SLTCFormatTypeRec(c, expectedType, &dstTypeText, 0);
    SLTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    SLTCTextBufAppendCStr(&detailText, "constant value 0x");
    SLTCTextBufAppendHexU64(&detailText, (uint64_t)value);
    SLTCTextBufAppendCStr(&detailText, " is out of range for ");
    SLTCTextBufAppendCStr(&detailText, dstTypeBuf);
    c->diag->detail = SLTCAllocDiagText(c, detailBuf);
    return -1;
}

int SLTCFailConstFloatRange(SLTypeCheckCtx* c, int32_t nodeId, int32_t expectedType) {
    char        dstTypeBuf[SLTC_DIAG_TEXT_CAP];
    char        detailBuf[256];
    SLTCTextBuf dstTypeText;
    SLTCTextBuf detailText;
    SLTCSetDiag(
        c->diag, SLDiag_TYPE_MISMATCH, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
    if (c->diag == NULL) {
        return -1;
    }
    SLTCTextBufInit(&dstTypeText, dstTypeBuf, (uint32_t)sizeof(dstTypeBuf));
    SLTCFormatTypeRec(c, expectedType, &dstTypeText, 0);
    SLTCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
    SLTCTextBufAppendCStr(&detailText, "constant value is not representable for ");
    SLTCTextBufAppendCStr(&detailText, dstTypeBuf);
    c->diag->detail = SLTCAllocDiagText(c, detailBuf);
    return -1;
}

int SLTCIsFloatType(SLTypeCheckCtx* c, int32_t typeId) {
    SLBuiltinKind b;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (c->types[typeId].kind != SLTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == SLBuiltin_F32 || b == SLBuiltin_F64;
}

int SLTCIsNumericType(SLTypeCheckCtx* c, int32_t typeId) {
    return SLTCIsIntegerType(c, typeId) || SLTCIsFloatType(c, typeId);
}

int SLTCIsBoolType(SLTypeCheckCtx* c, int32_t typeId) {
    typeId = SLTCResolveAliasBaseType(c, typeId);
    return typeId == c->typeBool;
}

int SLTCIsNamedDeclKind(SLTypeCheckCtx* c, int32_t typeId, SLAstKind kind) {
    int32_t declNode;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != SLTCType_NAMED) {
        return 0;
    }
    declNode = c->types[typeId].declNode;
    return declNode >= 0 && (uint32_t)declNode < c->ast->len
        && c->ast->nodes[declNode].kind == kind;
}

int SLTCIsStringLikeType(SLTypeCheckCtx* c, int32_t typeId) {
    int32_t baseType;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind != SLTCType_PTR && c->types[typeId].kind != SLTCType_REF) {
        return 0;
    }
    baseType = SLTCResolveAliasBaseType(c, c->types[typeId].baseType);
    return baseType == c->typeStr;
}

int SLTCTypeSupportsFmtReflectRec(SLTypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    uint32_t i;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (SLTCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case SLTCType_BUILTIN:
        case SLTCType_UNTYPED_INT:
        case SLTCType_UNTYPED_FLOAT:
            return SLTCIsBoolType(c, typeId) || SLTCIsNumericType(c, typeId)
                || typeId == c->typeType;
        case SLTCType_ARRAY:
        case SLTCType_SLICE:
            return SLTCTypeSupportsFmtReflectRec(c, c->types[typeId].baseType, depth + 1u);
        case SLTCType_PTR:
        case SLTCType_REF: return 1;
        case SLTCType_OPTIONAL:
            return SLTCTypeSupportsFmtReflectRec(c, c->types[typeId].baseType, depth + 1u);
        case SLTCType_NULL: return 1;
        case SLTCType_NAMED:
            if (SLTCIsNamedDeclKind(c, typeId, SLAst_ENUM)) {
                return 1;
            }
            if (SLTCIsNamedDeclKind(c, typeId, SLAst_UNION)) {
                return 0;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!SLTCTypeSupportsFmtReflectRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case SLTCType_ANON_STRUCT:
        case SLTCType_TUPLE:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!SLTCTypeSupportsFmtReflectRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case SLTCType_ANON_UNION: return 0;
        default:                  return 0;
    }
}

int SLTCIsComparableTypeRec(SLTypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    uint32_t i;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (SLTCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case SLTCType_BUILTIN:
        case SLTCType_UNTYPED_INT:
        case SLTCType_UNTYPED_FLOAT:
            return SLTCIsBoolType(c, typeId) || SLTCIsNumericType(c, typeId)
                || typeId == c->typeType;
        case SLTCType_ARRAY:
        case SLTCType_SLICE:
            return SLTCIsComparableTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case SLTCType_PTR:
        case SLTCType_REF: return 1;
        case SLTCType_NAMED:
            if (SLTCIsNamedDeclKind(c, typeId, SLAst_ENUM)) {
                return 1;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!SLTCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case SLTCType_ANON_STRUCT:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!SLTCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case SLTCType_ANON_UNION:
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t fieldIdx = c->types[typeId].fieldStart + i;
                if (!SLTCIsComparableTypeRec(c, c->fields[fieldIdx].typeId, depth + 1u)) {
                    return 0;
                }
            }
            return 1;
        case SLTCType_OPTIONAL:
            return SLTCIsComparableTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case SLTCType_NULL: return 1;
        default:            return 0;
    }
}

int SLTCIsOrderedTypeRec(SLTypeCheckCtx* c, int32_t typeId, uint32_t depth) {
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    if (SLTCIsStringLikeType(c, typeId)) {
        return 1;
    }
    switch (c->types[typeId].kind) {
        case SLTCType_BUILTIN:
        case SLTCType_UNTYPED_INT:
        case SLTCType_UNTYPED_FLOAT: return SLTCIsNumericType(c, typeId);
        case SLTCType_ARRAY:
        case SLTCType_SLICE:         return SLTCIsOrderedTypeRec(c, c->types[typeId].baseType, depth + 1u);
        case SLTCType_PTR:
        case SLTCType_REF:           return 1;
        case SLTCType_NAMED:         return SLTCIsNamedDeclKind(c, typeId, SLAst_ENUM);
        case SLTCType_OPTIONAL:
            return SLTCIsOrderedTypeRec(c, c->types[typeId].baseType, depth + 1u);
        default: return 0;
    }
}

int SLTCIsComparableType(SLTypeCheckCtx* c, int32_t typeId) {
    return SLTCIsComparableTypeRec(c, typeId, 0u);
}

int SLTCIsOrderedType(SLTypeCheckCtx* c, int32_t typeId) {
    return SLTCIsOrderedTypeRec(c, typeId, 0u);
}

int SLTCTypeSupportsLen(SLTypeCheckCtx* c, int32_t typeId) {
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind == SLTCType_PACK) {
        return 1;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind == SLTCType_ARRAY) {
        return 1;
    }
    if (c->types[typeId].kind == SLTCType_SLICE) {
        return 1;
    }
    if (c->types[typeId].kind == SLTCType_PTR || c->types[typeId].kind == SLTCType_REF) {
        int32_t baseType = SLTCResolveAliasBaseType(c, c->types[typeId].baseType);
        if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
            return 0;
        }
        if (baseType == c->typeStr) {
            return 1;
        }
        return c->types[baseType].kind == SLTCType_ARRAY
            || c->types[baseType].kind == SLTCType_SLICE;
    }
    return 0;
}

int SLTCIsUntyped(SLTypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

int SLTCIsTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_ANON_STRUCT || kind == SLAst_TYPE_ANON_UNION
        || kind == SLAst_TYPE_TUPLE;
}

int SLTCConcretizeInferredType(SLTypeCheckCtx* c, int32_t typeId, int32_t* outType) {
    const SLTCType* t;
    uint32_t        i;
    if (typeId == c->typeUntypedInt) {
        int32_t t = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
        if (t < 0) {
            return SLTCFailSpan(c, SLDiag_UNKNOWN_TYPE, 0, 0);
        }
        *outType = t;
        return 0;
    }
    if (typeId == c->typeUntypedFloat) {
        int32_t t = SLTCFindBuiltinByKind(c, SLBuiltin_F64);
        if (t < 0) {
            return SLTCFailSpan(c, SLDiag_UNKNOWN_TYPE, 0, 0);
        }
        *outType = t;
        return 0;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        *outType = typeId;
        return 0;
    }
    t = &c->types[typeId];
    if (t->kind == SLTCType_TUPLE) {
        int32_t elems[256];
        int     changed = 0;
        if (t->fieldCount > (uint16_t)(sizeof(elems) / sizeof(elems[0]))) {
            return SLTCFailSpan(c, SLDiag_ARENA_OOM, 0, 0);
        }
        for (i = 0; i < t->fieldCount; i++) {
            int32_t elem = c->funcParamTypes[t->fieldStart + i];
            int32_t concreteElem = elem;
            if (SLTCConcretizeInferredType(c, elem, &concreteElem) != 0) {
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
        *outType = SLTCInternTupleType(c, elems, t->fieldCount, 0, 0);
        return *outType < 0 ? -1 : 0;
    }
    *outType = typeId;
    return 0;
}

int SLTCTypeIsVarSize(SLTypeCheckCtx* c, int32_t typeId) {
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    return (c->types[typeId].flags & SLTCTypeFlag_VARSIZE) != 0;
}

int SLTCTypeContainsVarSizeByValue(SLTypeCheckCtx* c, int32_t typeId) {
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind == SLTCType_PTR || c->types[typeId].kind == SLTCType_REF) {
        return 0;
    }
    if (c->types[typeId].kind == SLTCType_SLICE) {
        return 1;
    }
    if (c->types[typeId].kind == SLTCType_OPTIONAL) {
        return SLTCTypeContainsVarSizeByValue(c, c->types[typeId].baseType);
    }
    if (c->types[typeId].kind == SLTCType_ARRAY) {
        return SLTCTypeContainsVarSizeByValue(c, c->types[typeId].baseType);
    }
    if (c->types[typeId].kind == SLTCType_TUPLE) {
        uint32_t i;
        for (i = 0; i < c->types[typeId].fieldCount; i++) {
            if (SLTCTypeContainsVarSizeByValue(
                    c, c->funcParamTypes[c->types[typeId].fieldStart + i]))
            {
                return 1;
            }
        }
        return 0;
    }
    return SLTCTypeIsVarSize(c, typeId);
}

int SLTCIsComparisonHookName(
    SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int* outIsEqualHook) {
    if (SLNameEqLiteral(c->src, nameStart, nameEnd, "__equal")) {
        *outIsEqualHook = 1;
        return 1;
    }
    if (SLNameEqLiteral(c->src, nameStart, nameEnd, "__order")) {
        *outIsEqualHook = 0;
        return 1;
    }
    return 0;
}

int SLTCTypeIsU8Slice(SLTypeCheckCtx* c, int32_t typeId, int requireMutable) {
    int32_t u8Type;
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind != SLTCType_SLICE) {
        return 0;
    }
    u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
    if (u8Type < 0 || c->types[typeId].baseType != u8Type) {
        return 0;
    }
    if (requireMutable && !SLTCTypeIsMutable(&c->types[typeId])) {
        return 0;
    }
    return 1;
}

int SLTCTypeIsFreeablePointer(SLTypeCheckCtx* c, int32_t typeId) {
    typeId = SLTCResolveAliasBaseType(c, typeId);
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind == SLTCType_OPTIONAL) {
        return SLTCTypeIsFreeablePointer(c, c->types[typeId].baseType);
    }
    return c->types[typeId].kind == SLTCType_PTR;
}

int32_t SLTCFindEmbeddedFieldIndex(SLTypeCheckCtx* c, int32_t namedTypeId) {
    uint32_t i;
    if (namedTypeId < 0 || (uint32_t)namedTypeId >= c->typeLen
        || c->types[namedTypeId].kind != SLTCType_NAMED)
    {
        return -1;
    }
    for (i = 0; i < c->types[namedTypeId].fieldCount; i++) {
        uint32_t idx = c->types[namedTypeId].fieldStart + i;
        if ((c->fields[idx].flags & SLTCFieldFlag_EMBEDDED) != 0) {
            return (int32_t)idx;
        }
    }
    return -1;
}

int SLTCEmbedDistanceToType(
    SLTypeCheckCtx* c, int32_t srcType, int32_t dstType, uint32_t* outDistance) {
    uint32_t depth = 0;
    int32_t  cur = srcType;
    if (srcType == dstType) {
        *outDistance = 0;
        return 0;
    }
    while (depth++ <= c->typeLen) {
        int32_t embedIdx;
        if (cur < 0 || (uint32_t)cur >= c->typeLen || c->types[cur].kind != SLTCType_NAMED) {
            return -1;
        }
        embedIdx = SLTCFindEmbeddedFieldIndex(c, cur);
        if (embedIdx < 0) {
            return -1;
        }
        cur = c->fields[embedIdx].typeId;
        if (cur >= 0 && (uint32_t)cur < c->typeLen && c->types[cur].kind == SLTCType_ALIAS) {
            cur = SLTCResolveAliasBaseType(c, cur);
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

int SLTCIsTypeDerivedFromEmbedded(SLTypeCheckCtx* c, int32_t srcType, int32_t dstType) {
    uint32_t distance = 0;
    return SLTCEmbedDistanceToType(c, srcType, dstType, &distance) == 0;
}

int SLTCCanAssign(SLTypeCheckCtx* c, int32_t dstType, int32_t srcType) {
    const SLTCType* dst;
    const SLTCType* src;

    if (dstType == srcType) {
        return 1;
    }
    if (SLTCTypeIsFmtValue(c, dstType)) {
        return SLTCTypeSupportsFmtReflectRec(c, srcType, 0u);
    }
    if (dstType == c->typeAnytype) {
        return srcType >= 0 && (uint32_t)srcType < c->typeLen;
    }
    if (dstType >= 0 && (uint32_t)dstType < c->typeLen && c->types[dstType].kind == SLTCType_ALIAS)
    {
        /* Alias types are nominal: only exact alias matches assign implicitly.
         * Widening is one-way (Alias -> Target), handled via srcType alias path. */
        return 0;
    }
    if (srcType == c->typeUntypedInt && SLTCIsIntegerType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedFloat && SLTCIsFloatType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedInt && SLTCIsFloatType(c, dstType)) {
        return 1;
    }

    if (dstType < 0 || (uint32_t)dstType >= c->typeLen || srcType < 0
        || (uint32_t)srcType >= c->typeLen)
    {
        return 0;
    }

    if (c->types[srcType].kind == SLTCType_ALIAS) {
        int32_t srcBaseType = SLTCResolveAliasBaseType(c, srcType);
        if (srcBaseType < 0 || srcBaseType == srcType) {
            return 0;
        }
        return SLTCCanAssign(c, dstType, srcBaseType);
    }

    dst = &c->types[dstType];
    src = &c->types[srcType];

    if (dst->kind == SLTCType_PACK) {
        uint32_t i;
        if (src->kind != SLTCType_PACK || dst->fieldCount != src->fieldCount) {
            return 0;
        }
        for (i = 0; i < dst->fieldCount; i++) {
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (!SLTCCanAssign(c, dstElem, srcElem)) {
                return 0;
            }
        }
        return 1;
    }

    if (dst->kind == SLTCType_TUPLE) {
        uint32_t i;
        if (src->kind != SLTCType_TUPLE || dst->fieldCount != src->fieldCount) {
            return 0;
        }
        for (i = 0; i < dst->fieldCount; i++) {
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (!SLTCCanAssign(c, dstElem, srcElem)) {
                return 0;
            }
        }
        return 1;
    }

    if (src->kind == SLTCType_NAMED && SLTCIsTypeDerivedFromEmbedded(c, srcType, dstType)) {
        return 1;
    }

    if (dst->kind == SLTCType_REF) {
        if (src->kind == SLTCType_REF && src->baseType == c->typeStr
            && SLTCTypeIsU8Slice(c, dst->baseType, 0))
        {
            return 1;
        }
        if (src->kind == SLTCType_PTR && src->baseType == c->typeStr
            && SLTCTypeIsU8Slice(c, dst->baseType, 0))
        {
            return 1;
        }
        if (src->kind == SLTCType_REF && SLTCCanAssign(c, dst->baseType, src->baseType)) {
            return !SLTCTypeIsMutable(dst) || SLTCTypeIsMutable(src);
        }
        if (src->kind == SLTCType_PTR && SLTCCanAssign(c, dst->baseType, src->baseType)) {
            return 1;
        }
        if (src->kind == SLTCType_ARRAY && SLTCCanAssign(c, dst->baseType, srcType)) {
            return 1;
        }
        return 0;
    }

    if (dst->kind == SLTCType_PTR) {
        /* Owned pointers (*T) can only come from new; references (&T) cannot be
         * implicitly promoted to owned pointers. */
        if (src->kind == SLTCType_ARRAY && dst->baseType >= 0
            && (uint32_t)dst->baseType < c->typeLen)
        {
            const SLTCType* dstBase = &c->types[dst->baseType];
            if (dstBase->kind == SLTCType_SLICE && dstBase->baseType == src->baseType) {
                return 1;
            }
        }
        if (src->kind == SLTCType_PTR && dst->baseType >= 0 && src->baseType >= 0
            && (uint32_t)dst->baseType < c->typeLen && (uint32_t)src->baseType < c->typeLen)
        {
            const SLTCType* dstBase = &c->types[dst->baseType];
            const SLTCType* srcBase = &c->types[src->baseType];
            int32_t         dstBaseResolved = SLTCResolveAliasBaseType(c, dst->baseType);
            int32_t         srcBaseResolved = SLTCResolveAliasBaseType(c, src->baseType);
            if (dstBaseResolved >= 0 && srcBaseResolved >= 0 && dstBaseResolved == srcBaseResolved)
            {
                return 1;
            }
            if (dstBaseResolved >= 0 && srcBaseResolved >= 0
                && SLTCIsTypeDerivedFromEmbedded(c, srcBaseResolved, dstBaseResolved))
            {
                return 1;
            }
            if (src->baseType == c->typeStr && SLTCTypeIsU8Slice(c, dst->baseType, 1)) {
                return 1;
            }
            if (dstBase->kind == SLTCType_SLICE) {
                if (srcBase->kind == SLTCType_SLICE && dstBase->baseType == srcBase->baseType) {
                    return !SLTCTypeIsMutable(dstBase) || SLTCTypeIsMutable(srcBase);
                }
                if (srcBase->kind == SLTCType_ARRAY && dstBase->baseType == srcBase->baseType) {
                    return 1;
                }
            }
        }
        return 0;
    }

    if (dst->kind == SLTCType_SLICE) {
        if (srcType == c->typeStr && SLTCTypeIsU8Slice(c, dstType, SLTCTypeIsMutable(dst))) {
            return 1;
        }
        if (src->kind == SLTCType_SLICE && dst->baseType == src->baseType) {
            return !SLTCTypeIsMutable(dst) || SLTCTypeIsMutable(src);
        }
        if (src->kind == SLTCType_ARRAY && dst->baseType == src->baseType) {
            return 1;
        }
        if (src->kind == SLTCType_PTR) {
            int32_t pointee = src->baseType;
            if (pointee >= 0 && (uint32_t)pointee < c->typeLen) {
                const SLTCType* p = &c->types[pointee];
                if (p->kind == SLTCType_ARRAY && p->baseType == dst->baseType) {
                    return 1;
                }
            }
        }
        return 0;
    }

    if (dst->kind == SLTCType_OPTIONAL) {
        /* null can be assigned to ?T */
        if (src->kind == SLTCType_NULL) {
            return 1;
        }
        /* T can be assigned to ?T (implicit lift through base assignability) */
        if (src->kind != SLTCType_OPTIONAL && SLTCCanAssign(c, dst->baseType, srcType)) {
            return 1;
        }
        /* ?T can be assigned to ?T (also handles mutable sub-type coercions) */
        if (src->kind == SLTCType_OPTIONAL) {
            return SLTCCanAssign(c, dst->baseType, src->baseType);
        }
        return 0;
    }

    /* null can only be assigned to ?T, not to plain types */
    if (src->kind == SLTCType_NULL) {
        return 0;
    }

    return 0;
}

int SLTCCoerceForBinary(SLTypeCheckCtx* c, int32_t leftType, int32_t rightType, int32_t* outType) {
    if (leftType == rightType) {
        *outType = leftType;
        return 0;
    }
    if (SLTCIsUntyped(c, leftType) && !SLTCIsUntyped(c, rightType)
        && SLTCCanAssign(c, rightType, leftType))
    {
        *outType = rightType;
        return 0;
    }
    if (SLTCIsUntyped(c, rightType) && !SLTCIsUntyped(c, leftType)
        && SLTCCanAssign(c, leftType, rightType))
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

int SLTCConversionCost(SLTypeCheckCtx* c, int32_t dstType, int32_t srcType, uint8_t* outCost) {
    const SLTCType* dst;
    const SLTCType* src;

    if (!SLTCCanAssign(c, dstType, srcType)) {
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
    if (SLTCTypeIsFmtValue(c, dstType)) {
        *outCost = 5;
        return 0;
    }
    if (srcType == c->typeUntypedInt || srcType == c->typeUntypedFloat) {
        *outCost = 3;
        return 0;
    }
    if (srcType >= 0 && (uint32_t)srcType < c->typeLen && c->types[srcType].kind == SLTCType_ALIAS)
    {
        int32_t srcBaseType = SLTCResolveAliasBaseType(c, srcType);
        if (srcBaseType < 0 || srcBaseType == srcType) {
            return -1;
        }
        if (dstType == srcBaseType) {
            *outCost = 1;
            return 0;
        }
        if (SLTCConversionCost(c, dstType, srcBaseType, outCost) == 0) {
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

    if (dst->kind == SLTCType_TUPLE && src->kind == SLTCType_TUPLE
        && dst->fieldCount == src->fieldCount)
    {
        uint32_t total = 0;
        uint32_t i;
        for (i = 0; i < dst->fieldCount; i++) {
            uint8_t elemCost = 0;
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (SLTCConversionCost(c, dstElem, srcElem, &elemCost) != 0) {
                return -1;
            }
            total += elemCost;
        }
        *outCost = total > 255u ? 255u : (uint8_t)total;
        return 0;
    }

    if (dst->kind == SLTCType_PACK && src->kind == SLTCType_PACK
        && dst->fieldCount == src->fieldCount)
    {
        uint32_t total = 0;
        uint32_t i;
        for (i = 0; i < dst->fieldCount; i++) {
            uint8_t elemCost = 0;
            int32_t dstElem = c->funcParamTypes[dst->fieldStart + i];
            int32_t srcElem = c->funcParamTypes[src->fieldStart + i];
            if (SLTCConversionCost(c, dstElem, srcElem, &elemCost) != 0) {
                return -1;
            }
            total += elemCost;
        }
        *outCost = total > 255u ? 255u : (uint8_t)total;
        return 0;
    }

    if (dst->kind == SLTCType_OPTIONAL && src->kind != SLTCType_OPTIONAL) {
        *outCost = 4;
        return 0;
    }

    if (dst->kind == SLTCType_OPTIONAL && src->kind == SLTCType_OPTIONAL) {
        return SLTCConversionCost(c, dst->baseType, src->baseType, outCost);
    }

    if (dst->kind == SLTCType_REF && src->kind == SLTCType_REF) {
        int      sameBase = dst->baseType == src->baseType;
        uint32_t upcastDistance = 0;
        int      upcastBase =
            SLTCEmbedDistanceToType(c, src->baseType, dst->baseType, &upcastDistance) == 0;
        if (upcastBase && !sameBase) {
            *outCost = (uint8_t)(2u + (upcastDistance > 0 ? (upcastDistance - 1u) : 0u));
            return 0;
        }
        if (!SLTCTypeIsMutable(dst) && SLTCTypeIsMutable(src) && sameBase) {
            *outCost = 1;
            return 0;
        }
    }

    if (dst->kind == SLTCType_PTR && src->kind == SLTCType_PTR) {
        int      sameBase = dst->baseType == src->baseType;
        uint32_t upcastDistance = 0;
        int      upcastBase =
            SLTCEmbedDistanceToType(c, src->baseType, dst->baseType, &upcastDistance) == 0;
        if (upcastBase && !sameBase) {
            *outCost = (uint8_t)(2u + (upcastDistance > 0 ? (upcastDistance - 1u) : 0u));
            return 0;
        }
    }

    if (dst->kind == SLTCType_SLICE && src->kind == SLTCType_SLICE && dst->baseType == src->baseType
        && !SLTCTypeIsMutable(dst) && SLTCTypeIsMutable(src))
    {
        *outCost = 1;
        return 0;
    }

    if (src->kind == SLTCType_NAMED && SLTCIsTypeDerivedFromEmbedded(c, srcType, dstType)) {
        uint32_t distance = 0;
        if (SLTCEmbedDistanceToType(c, srcType, dstType, &distance) == 0) {
            *outCost = (uint8_t)(2u + (distance > 0 ? (distance - 1u) : 0u));
        } else {
            *outCost = 2;
        }
        return 0;
    }

    *outCost = 1;
    return 0;
}

int SLTCCostVectorCompare(const uint8_t* a, const uint8_t* b, uint32_t len) {
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

int32_t SLTCUnwrapCallArgExprNode(SLTypeCheckCtx* c, int32_t argNode) {
    const SLAstNode* n;
    if (argNode < 0 || (uint32_t)argNode >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[argNode];
    if (n->kind == SLAst_CALL_ARG) {
        return SLAstFirstChild(c->ast, argNode);
    }
    return argNode;
}

int SLTCCollectCallArgInfo(
    SLTypeCheckCtx*  c,
    int32_t          callNode,
    int32_t          calleeNode,
    int              includeReceiver,
    int32_t          receiverNode,
    SLTCCallArgInfo* outArgs,
    int32_t* _Nullable outArgTypes,
    uint32_t* outArgCount) {
    uint32_t argCount = 0;
    int32_t  argNode = SLAstNextSibling(c->ast, calleeNode);
    if (includeReceiver) {
        const SLAstNode* recvNode;
        if (receiverNode < 0 || (uint32_t)receiverNode >= c->ast->len) {
            return SLTCFailNode(c, callNode, SLDiag_EXPECTED_EXPR);
        }
        if (argCount >= SLTC_MAX_CALL_ARGS) {
            return SLTCFailNode(c, callNode, SLDiag_ARENA_OOM);
        }
        recvNode = &c->ast->nodes[receiverNode];
        outArgs[argCount] = (SLTCCallArgInfo){
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
        if (outArgTypes != NULL && SLTCTypeExpr(c, receiverNode, &outArgTypes[argCount]) != 0) {
            return -1;
        }
        argCount++;
    }
    while (argNode >= 0) {
        const SLAstNode* arg = &c->ast->nodes[argNode];
        int32_t          exprNode = SLTCUnwrapCallArgExprNode(c, argNode);
        if (argCount >= SLTC_MAX_CALL_ARGS) {
            return SLTCFailNode(c, callNode, SLDiag_ARENA_OOM);
        }
        if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
            return SLTCFailNode(c, argNode, SLDiag_EXPECTED_EXPR);
        }
        outArgs[argCount] = (SLTCCallArgInfo){
            .argNode = argNode,
            .exprNode = exprNode,
            .start = arg->start,
            .end = arg->end,
            .explicitNameStart = arg->dataStart,
            .explicitNameEnd = arg->dataEnd,
            .implicitNameStart = 0,
            .implicitNameEnd = 0,
            .spread = (uint8_t)(((arg->flags & SLAstFlag_CALL_ARG_SPREAD) != 0) ? 1 : 0),
            ._reserved = { 0, 0, 0 },
        };
        if ((arg->kind == SLAst_CALL_ARG || arg->kind == SLAst_IDENT)
            && !(outArgs[argCount].explicitNameEnd > outArgs[argCount].explicitNameStart)
            && c->ast->nodes[exprNode].kind == SLAst_IDENT)
        {
            uint32_t nameStart = c->ast->nodes[exprNode].dataStart;
            uint32_t nameEnd = c->ast->nodes[exprNode].dataEnd;
            if (SLTCResolveTypeValueName(c, nameStart, nameEnd) < 0) {
                outArgs[argCount].implicitNameStart = nameStart;
                outArgs[argCount].implicitNameEnd = nameEnd;
            }
        }
        if (outArgTypes != NULL && SLTCTypeExpr(c, exprNode, &outArgTypes[argCount]) != 0) {
            return -1;
        }
        argCount++;
        argNode = SLAstNextSibling(c->ast, argNode);
    }
    *outArgCount = argCount;
    return 0;
}

int SLTCIsMainFunction(SLTypeCheckCtx* c, const SLTCFunction* fn) {
    return SLNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, "main");
}

int32_t SLTCResolveImplicitMainContextType(SLTypeCheckCtx* c) {
    uint32_t i;
    int32_t  typeId = SLTCFindNamedTypeByLiteral(c, "builtin__Context");
    if (typeId >= 0) {
        return typeId;
    }
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_NAMED) {
            continue;
        }
        if (SLNameHasPrefix(c->src, t->nameStart, t->nameEnd, "builtin")
            && SLNameHasSuffix(c->src, t->nameStart, t->nameEnd, "__Context"))
        {
            return (int32_t)i;
        }
    }
    typeId = SLTCFindNamedTypeByLiteral(c, "Context");
    if (typeId >= 0) {
        return typeId;
    }
    return -1;
}

int SLTCCurrentContextFieldType(
    SLTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType) {
    if (c->currentContextType >= 0) {
        if (SLTCFieldLookup(c, c->currentContextType, fieldStart, fieldEnd, outType, NULL) == 0) {
            return 0;
        }
        return -1;
    }
    if (c->hasImplicitMainRootContext) {
        if (c->implicitMainContextType >= 0) {
            if (SLTCFieldLookup(c, c->implicitMainContextType, fieldStart, fieldEnd, outType, NULL)
                == 0)
            {
                return 0;
            }
            return -1;
        }
        if (SLNameEqLiteral(c->src, fieldStart, fieldEnd, "mem")) {
            int32_t t = SLTCFindMemAllocatorType(c);
            int32_t ptrType;
            if (t < 0) {
                return -1;
            }
            ptrType = SLTCInternPtrType(c, t, fieldStart, fieldEnd);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        if (SLNameEqLiteral(c->src, fieldStart, fieldEnd, "log")) {
            int32_t t = SLTCFindNamedTypeByLiteral(c, "builtin__Logger");
            if (t < 0) {
                t = SLTCFindNamedTypeByLiteral(c, "Logger");
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

int SLTCCurrentContextFieldTypeByLiteral(
    SLTypeCheckCtx* c, const char* fieldName, int32_t* outType) {
    if (c->currentContextType >= 0) {
        int32_t  typeId = c->currentContextType;
        uint32_t i;
        if (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == SLTCType_ALIAS)
        {
            if (SLTCResolveAliasTypeId(c, typeId) != 0) {
                return -1;
            }
            typeId = c->types[typeId].baseType;
        }
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen
            || (c->types[typeId].kind != SLTCType_NAMED
                && c->types[typeId].kind != SLTCType_ANON_STRUCT
                && c->types[typeId].kind != SLTCType_ANON_UNION))
        {
            return -1;
        }
        for (i = 0; i < c->types[typeId].fieldCount; i++) {
            uint32_t idx = c->types[typeId].fieldStart + i;
            if (SLNameEqLiteral(
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
                   && c->types[typeId].kind == SLTCType_ALIAS)
            {
                if (SLTCResolveAliasTypeId(c, typeId) != 0) {
                    return -1;
                }
                typeId = c->types[typeId].baseType;
            }
            if (typeId < 0 || (uint32_t)typeId >= c->typeLen
                || (c->types[typeId].kind != SLTCType_NAMED
                    && c->types[typeId].kind != SLTCType_ANON_STRUCT
                    && c->types[typeId].kind != SLTCType_ANON_UNION))
            {
                return -1;
            }
            for (i = 0; i < c->types[typeId].fieldCount; i++) {
                uint32_t idx = c->types[typeId].fieldStart + i;
                if (SLNameEqLiteral(
                        c->src, c->fields[idx].nameStart, c->fields[idx].nameEnd, fieldName))
                {
                    *outType = c->fields[idx].typeId;
                    return 0;
                }
            }
            return -1;
        }
        if (fieldName[0] == 'm' && fieldName[1] == 'e' && fieldName[2] == 'm'
            && fieldName[3] == '\0')
        {
            int32_t t = SLTCFindMemAllocatorType(c);
            int32_t ptrType;
            if (t < 0) {
                return -1;
            }
            ptrType = SLTCInternPtrType(c, t, 0, 0);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        if (fieldName[0] == 'l' && fieldName[1] == 'o' && fieldName[2] == 'g'
            && fieldName[3] == '\0')
        {
            int32_t t = SLTCFindNamedTypeByLiteral(c, "builtin__Logger");
            if (t < 0) {
                t = SLTCFindNamedTypeByLiteral(c, "Logger");
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

int32_t SLTCContextFindOverlayNode(SLTypeCheckCtx* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast->len) {
        return -1;
    }
    {
        int32_t callNode = SLAstFirstChild(c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? SLAstNextSibling(c->ast, callNode) : -1;
        if (child >= 0 && c->ast->nodes[child].kind == SLAst_CONTEXT_OVERLAY) {
            return child;
        }
    }
    return -1;
}

int32_t SLTCContextFindOverlayBindMatch(
    SLTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, const char* _Nullable fieldName) {
    int32_t overlayNode = SLTCContextFindOverlayNode(c);
    int32_t child = overlayNode >= 0 ? SLAstFirstChild(c->ast, overlayNode) : -1;
    while (child >= 0) {
        const SLAstNode* bind = &c->ast->nodes[child];
        if (bind->kind == SLAst_CONTEXT_BIND) {
            int matches =
                fieldName != NULL
                    ? SLNameEqLiteral(c->src, bind->dataStart, bind->dataEnd, fieldName)
                    : SLNameEqSlice(c->src, bind->dataStart, bind->dataEnd, fieldStart, fieldEnd);
            if (matches) {
                return child;
            }
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return -1;
}

int32_t SLTCContextFindOverlayBindByLiteral(SLTypeCheckCtx* c, const char* fieldName) {
    return SLTCContextFindOverlayBindMatch(c, 0, 0, fieldName);
}

int SLTCGetEffectiveContextFieldType(
    SLTypeCheckCtx* c, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType) {
    int32_t bindNode;
    if (c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return SLTCFailSpan(c, SLDiag_CONTEXT_REQUIRED, fieldStart, fieldEnd);
    }
    bindNode = SLTCContextFindOverlayBindMatch(c, fieldStart, fieldEnd, NULL);
    if (bindNode >= 0) {
        int32_t exprNode = SLAstFirstChild(c->ast, bindNode);
        if (exprNode >= 0) {
            return SLTCTypeExpr(c, exprNode, outType);
        }
    }
    if (SLTCCurrentContextFieldType(c, fieldStart, fieldEnd, outType) != 0) {
        return SLTCFailSpan(c, SLDiag_CONTEXT_MISSING_FIELD, fieldStart, fieldEnd);
    }
    return 0;
}

int SLTCGetEffectiveContextFieldTypeByLiteral(
    SLTypeCheckCtx* c, const char* fieldName, int32_t* outType) {
    int32_t bindNode;
    if (c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return SLTCFailSpan(c, SLDiag_CONTEXT_REQUIRED, 0, 0);
    }
    bindNode = SLTCContextFindOverlayBindByLiteral(c, fieldName);
    if (bindNode >= 0) {
        int32_t exprNode = SLAstFirstChild(c->ast, bindNode);
        if (exprNode >= 0) {
            return SLTCTypeExpr(c, exprNode, outType);
        }
    }
    if (SLTCCurrentContextFieldTypeByLiteral(c, fieldName, outType) != 0) {
        return SLTCFailSpan(c, SLDiag_CONTEXT_MISSING_FIELD, 0, 0);
    }
    return 0;
}

int SLTCValidateCurrentCallOverlay(SLTypeCheckCtx* c) {
    int32_t overlayNode = SLTCContextFindOverlayNode(c);
    int32_t bind = overlayNode >= 0 ? SLAstFirstChild(c->ast, overlayNode) : -1;
    if (overlayNode >= 0 && c->currentContextType < 0 && !c->hasImplicitMainRootContext) {
        return SLTCFailSpan(
            c,
            SLDiag_CONTEXT_REQUIRED,
            c->ast->nodes[overlayNode].start,
            c->ast->nodes[overlayNode].end);
    }
    while (bind >= 0) {
        const SLAstNode* b = &c->ast->nodes[bind];
        int32_t          expectedType;
        int32_t          exprNode;
        int32_t          t;
        int32_t          scan;
        if (b->kind != SLAst_CONTEXT_BIND) {
            return SLTCFailNode(c, bind, SLDiag_UNEXPECTED_TOKEN);
        }
        if (SLTCCurrentContextFieldType(c, b->dataStart, b->dataEnd, &expectedType) != 0) {
            return SLTCFailSpan(c, SLDiag_CONTEXT_UNKNOWN_FIELD, b->dataStart, b->dataEnd);
        }
        scan = SLAstFirstChild(c->ast, overlayNode);
        while (scan >= 0) {
            const SLAstNode* bs = &c->ast->nodes[scan];
            if (scan != bind && bs->kind == SLAst_CONTEXT_BIND
                && SLNameEqSlice(c->src, bs->dataStart, bs->dataEnd, b->dataStart, b->dataEnd))
            {
                return SLTCFailSpan(c, SLDiag_CONTEXT_DUPLICATE_FIELD, b->dataStart, b->dataEnd);
            }
            scan = SLAstNextSibling(c->ast, scan);
        }
        exprNode = SLAstFirstChild(c->ast, bind);
        if (exprNode >= 0) {
            int32_t savedActive = c->activeCallWithNode;
            c->activeCallWithNode = -1;
            if (SLTCTypeExpr(c, exprNode, &t) != 0) {
                c->activeCallWithNode = savedActive;
                return -1;
            }
            c->activeCallWithNode = savedActive;
            if (!SLTCCanAssign(c, expectedType, t)) {
                return SLTCFailNode(c, exprNode, SLDiag_CONTEXT_TYPE_MISMATCH);
            }
        }
        bind = SLAstNextSibling(c->ast, bind);
    }
    return 0;
}

int SLTCValidateCallContextRequirements(SLTypeCheckCtx* c, int32_t requiredContextType) {
    int32_t  typeId = requiredContextType;
    uint32_t i;
    if (requiredContextType < 0) {
        return 0;
    }
    while (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == SLTCType_ALIAS)
    {
        if (SLTCResolveAliasTypeId(c, typeId) != 0) {
            return -1;
        }
        typeId = c->types[typeId].baseType;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen
        || (c->types[typeId].kind != SLTCType_NAMED
            && c->types[typeId].kind != SLTCType_ANON_STRUCT))
    {
        return SLTCFailSpan(c, SLDiag_TYPE_MISMATCH, 0, 0);
    }
    for (i = 0; i < c->types[typeId].fieldCount; i++) {
        uint32_t        fieldIdx = c->types[typeId].fieldStart + i;
        const SLTCField field = c->fields[fieldIdx];
        int32_t         gotType;
        if (field.nameEnd <= field.nameStart) {
            continue;
        }
        if (SLTCGetEffectiveContextFieldType(c, field.nameStart, field.nameEnd, &gotType) != 0) {
            return -1;
        }
        if (!SLTCCanAssign(c, field.typeId, gotType)) {
            return SLTCFailSpan(c, SLDiag_CONTEXT_TYPE_MISMATCH, field.nameStart, field.nameEnd);
        }
    }
    return 0;
}

int SLTCGetFunctionTypeSignature(
    SLTypeCheckCtx* c,
    int32_t         typeId,
    int32_t*        outReturnType,
    uint32_t*       outParamStart,
    uint32_t*       outParamCount,
    int* _Nullable outIsVariadic) {
    const SLTCType* t;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return -1;
    }
    t = &c->types[typeId];
    if (t->kind != SLTCType_FUNCTION) {
        return -1;
    }
    *outReturnType = t->baseType;
    *outParamStart = t->fieldStart;
    *outParamCount = t->fieldCount;
    if (outIsVariadic != NULL) {
        *outIsVariadic = (t->flags & SLTCTypeFlag_FUNCTION_VARIADIC) != 0;
    }
    return 0;
}

void SLTCCallMapErrorClear(SLTCCallMapError* err) {
    err->code = 0;
    err->start = 0;
    err->end = 0;
    err->argStart = 0;
    err->argEnd = 0;
}

int SLTCMapCallArgsToParams(
    SLTypeCheckCtx*        c,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    uint32_t               paramCount,
    uint32_t               firstPositionalArgIndex,
    int32_t*               outMappedArgExprNodes,
    SLTCCallMapError* _Nullable outError) {
    uint8_t  paramAssigned[SLTC_MAX_CALL_ARGS];
    uint32_t i;
    if (paramCount > SLTC_MAX_CALL_ARGS || argCount > paramCount) {
        return -1;
    }
    memset(paramAssigned, 0, sizeof(paramAssigned));
    for (i = 0; i < paramCount; i++) {
        outMappedArgExprNodes[i] = -1;
    }

    if (firstPositionalArgIndex < argCount) {
        const SLTCCallArgInfo* a = &callArgs[firstPositionalArgIndex];
        outMappedArgExprNodes[firstPositionalArgIndex] = a->exprNode;
        paramAssigned[firstPositionalArgIndex] = 1;
        if (a->explicitNameEnd > a->explicitNameStart
            && !SLNameEqSlice(
                c->src,
                a->explicitNameStart,
                a->explicitNameEnd,
                paramNameStarts[firstPositionalArgIndex],
                paramNameEnds[firstPositionalArgIndex]))
        {
            if (outError != NULL) {
                outError->code = SLDiag_CALL_FIRST_ARG_NAME_MISMATCH;
                outError->start = a->start;
                outError->end = a->end;
                outError->argStart = paramNameStarts[firstPositionalArgIndex];
                outError->argEnd = paramNameEnds[firstPositionalArgIndex];
            }
            return 1;
        }
    }

    for (i = 0; i < argCount; i++) {
        const SLTCCallArgInfo* a = &callArgs[i];
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
        } else if (a->implicitNameEnd > a->implicitNameStart) {
            nameStart = a->implicitNameStart;
            nameEnd = a->implicitNameEnd;
            foundName = 1;
        }
        if (!foundName) {
            if (outError != NULL) {
                outError->code = SLDiag_CALL_ARG_NAME_REQUIRED;
                outError->start = a->start;
                outError->end = a->end;
                outError->argStart = 0;
                outError->argEnd = 0;
            }
            return 1;
        }

        for (p = firstPositionalArgIndex + 1u; p < paramCount; p++) {
            if (SLNameEqSlice(c->src, nameStart, nameEnd, paramNameStarts[p], paramNameEnds[p])) {
                if (paramAssigned[p]) {
                    if (outError != NULL) {
                        outError->code = SLDiag_CALL_ARG_DUPLICATE;
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
                outError->code = SLDiag_CALL_ARG_UNKNOWN_NAME;
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
                outError->code = SLDiag_CALL_ARG_MISSING;
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

int SLTCPrepareCallBinding(
    SLTypeCheckCtx*        c,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const int32_t*         paramTypes,
    uint32_t               paramCount,
    int                    isVariadic,
    int                    allowNamedMapping,
    uint32_t               firstPositionalArgIndex,
    SLTCCallBinding*       outBinding,
    SLTCCallMapError*      outError) {
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
    for (i = 0; i < SLTC_MAX_CALL_ARGS; i++) {
        outBinding->fixedMappedArgExprNodes[i] = -1;
        outBinding->argParamIndices[i] = -1;
        outBinding->argExpectedTypes[i] = -1;
    }
    if (paramCount > SLTC_MAX_CALL_ARGS || argCount > SLTC_MAX_CALL_ARGS) {
        return 1;
    }

    for (i = 0; i < argCount; i++) {
        if (!callArgs[i].spread) {
            continue;
        }
        if (i + 1u < argCount) {
            if (outError != NULL) {
                outError->code = SLDiag_VARIADIC_SPREAD_NOT_LAST;
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
                outError->code = SLDiag_VARIADIC_CALL_SHAPE_MISMATCH;
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
        if (c->types[outBinding->variadicParamType].kind == SLTCType_SLICE) {
            outBinding->variadicElemType = c->types[outBinding->variadicParamType].baseType;
        } else if (c->types[outBinding->variadicParamType].kind == SLTCType_PACK) {
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
                    outError->code = SLDiag_VARIADIC_CALL_SHAPE_MISMATCH;
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
                    outError->code = SLDiag_VARIADIC_CALL_SHAPE_MISMATCH;
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
            SLTCCallMapError mapError;
            SLTCCallMapErrorClear(&mapError);
            if (SLTCMapCallArgsToParams(
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
        const SLTCType* variadicType = &c->types[outBinding->variadicParamType];
        if (callArgs[i].explicitNameEnd > callArgs[i].explicitNameStart) {
            if (outError != NULL) {
                outError->code = SLDiag_VARIADIC_CALL_SHAPE_MISMATCH;
                outError->start = callArgs[i].start;
                outError->end = callArgs[i].end;
                outError->argStart = 0;
                outError->argEnd = 0;
            }
            return 2;
        }
        outBinding->argParamIndices[i] = (int32_t)fixedCount;
        if (variadicType->kind == SLTCType_PACK) {
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

int SLTCCheckConstParamArgs(
    SLTypeCheckCtx*        c,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    const SLTCCallBinding* binding,
    const uint32_t*        paramNameStarts,
    const uint32_t*        paramNameEnds,
    const uint8_t*         paramFlags,
    uint32_t               paramCount,
    SLTCCallMapError*      outError) {
    uint32_t i;
    if (outError != NULL) {
        SLTCCallMapErrorClear(outError);
    }
    if (binding == NULL || paramFlags == NULL) {
        return 0;
    }
    for (i = 0; i < argCount; i++) {
        int32_t          p = binding->argParamIndices[i];
        int              isConst = 0;
        int              evalIsConst = 0;
        SLCTFEValue      ignoredValue = { 0 };
        SLTCConstEvalCtx evalCtx;
        SLDiagCode       code = SLDiag_CONST_PARAM_ARG_NOT_CONST;
        SLTCInitConstEvalCtxFromParent(c, c != NULL ? c->activeConstEvalCtx : NULL, &evalCtx);
        if (p < 0 || (uint32_t)p >= paramCount) {
            continue;
        }
        isConst = (paramFlags[p] & SLTCFuncParamFlag_CONST) != 0;
        if (!isConst) {
            continue;
        }
        if (binding->isVariadic && i == binding->spreadArgIndex
            && (uint32_t)p == binding->fixedCount)
        {
            code = SLDiag_CONST_PARAM_SPREAD_NOT_CONST;
        }
        if (SLTCEvalConstExprNode(&evalCtx, callArgs[i].exprNode, &ignoredValue, &evalIsConst) != 0)
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

int SLTCCheckConstBlocksForCall(
    SLTypeCheckCtx*        c,
    int32_t                fnIndex,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    const SLTCCallBinding* binding,
    SLTCCallMapError*      outError) {
    const SLTCFunction* fn;
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
    SLTCConstEvalCtx*   savedActiveConstEvalCtx;
    SLCTFEExecBinding*  paramBindings = NULL;
    uint32_t            paramBindingLen = 0;
    SLCTFEExecEnv       paramFrame;
    SLCTFEExecCtx       execCtx;
    SLTCConstEvalCtx    evalCtx;
    SLCTFEValue         retValue;
    int                 didReturn = 0;
    int                 isConst = 0;
    int                 rc;

    if (outError != NULL) {
        SLTCCallMapErrorClear(outError);
    }
    if (c == NULL || fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return 0;
    }
    fn = &c->funcs[fnIndex];
    fnNode = fn->defNode;
    if (fnNode < 0 || (uint32_t)fnNode >= c->ast->len || c->ast->nodes[fnNode].kind != SLAst_FN) {
        return 0;
    }

    child = SLAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == SLAst_BLOCK) {
            bodyNode = child;
            break;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    if (bodyNode < 0) {
        return 0;
    }

    child = SLAstFirstChild(c->ast, bodyNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == SLAst_CONST_BLOCK) {
            hasConstBlock = 1;
            break;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    if (!hasConstBlock) {
        return 0;
    }

    savedLocalLen = c->localLen;
    savedLocalUseLen = c->localUseLen;
    savedVariantNarrowLen = c->variantNarrowLen;
    savedActiveConstEvalCtx = c->activeConstEvalCtx;

    if (fn->paramCount > 0) {
        paramBindings = (SLCTFEExecBinding*)SLArenaAlloc(
            c->arena,
            sizeof(SLCTFEExecBinding) * fn->paramCount,
            (uint32_t)_Alignof(SLCTFEExecBinding));
        if (paramBindings == NULL) {
            c->localLen = savedLocalLen;
            c->localUseLen = savedLocalUseLen;
            c->variantNarrowLen = savedVariantNarrowLen;
            c->activeConstEvalCtx = savedActiveConstEvalCtx;
            return SLTCFailNode(c, fnNode, SLDiag_ARENA_OOM);
        }
    }

    child = SLAstFirstChild(c->ast, fnNode);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
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
                (c->funcParamFlags[fn->paramTypeStart + paramIndex] & SLTCFuncParamFlag_CONST) != 0;

            if (!SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_")
                && SLTCLocalAdd(c, n->dataStart, n->dataEnd, paramType, isConstParam, -1) != 0)
            {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
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
                variadicPackParamNameStart = n->dataStart;
                variadicPackParamNameEnd = n->dataEnd;
            }

            if (isConstParam && binding != NULL && paramBindings != NULL
                && !SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "_"))
            {
                int32_t          argIndex = -1;
                SLCTFEValue      value;
                int              evalIsConst = 0;
                SLTCConstEvalCtx evalArgCtx;
                uint32_t         i;
                SLTCInitConstEvalCtxFromParent(c, savedActiveConstEvalCtx, &evalArgCtx);
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
                    if (SLTCEvalConstExprNode(
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
                            outError->code = SLDiag_CONST_BLOCK_EVAL_FAILED;
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
        child = SLAstNextSibling(c->ast, child);
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
    execCtx.evalExpr = SLTCEvalConstExecExprCb;
    execCtx.evalExprCtx = &evalCtx;
    execCtx.resolveType = SLTCEvalConstExecResolveTypeCb;
    execCtx.resolveTypeCtx = &evalCtx;
    execCtx.inferValueType = SLTCEvalConstExecInferValueTypeCb;
    execCtx.inferValueTypeCtx = &evalCtx;
    execCtx.forInIndex = SLTCEvalConstForInIndexCb;
    execCtx.forInIndexCtx = &evalCtx;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = SLTC_CONST_FOR_MAX_ITERS;
    memset(&evalCtx, 0, sizeof(evalCtx));
    evalCtx.tc = c;
    evalCtx.execCtx = &execCtx;
    evalCtx.callArgs = callArgs;
    evalCtx.callArgCount = argCount;
    evalCtx.callBinding = binding;
    evalCtx.callPackParamNameStart = variadicPackParamNameStart;
    evalCtx.callPackParamNameEnd = variadicPackParamNameEnd;
    c->activeConstEvalCtx = &evalCtx;

    child = SLAstFirstChild(c->ast, bodyNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind == SLAst_CONST_BLOCK) {
            int32_t blockNode = SLAstFirstChild(c->ast, child);
            int     mirSupported = 0;
            if (blockNode < 0 || c->ast->nodes[blockNode].kind != SLAst_BLOCK) {
                if (outError != NULL) {
                    outError->code = SLDiag_CONST_BLOCK_EVAL_FAILED;
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
            rc = SLTCTryMirConstBlock(
                &evalCtx, blockNode, &retValue, &didReturn, &isConst, &mirSupported);
            if (rc != 0) {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return -1;
            }
            if (mirSupported && isConst) {
                if (didReturn) {
                    SLTCConstSetReasonNode(
                        &evalCtx, blockNode, "const block must not return a value");
                    c->lastConstEvalReason = c->activeConstEvalCtx->nonConstReason;
                    c->lastConstEvalReasonStart = c->activeConstEvalCtx->nonConstStart;
                    c->lastConstEvalReasonEnd = c->activeConstEvalCtx->nonConstEnd;
                    if (outError != NULL) {
                        outError->code = SLDiag_CONST_BLOCK_EVAL_FAILED;
                        outError->start =
                            argCount > 0 ? callArgs[0].start : c->ast->nodes[child].start;
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
                child = SLAstNextSibling(c->ast, child);
                continue;
            }
            evalCtx.nonConstReason = NULL;
            evalCtx.nonConstStart = 0;
            evalCtx.nonConstEnd = 0;
            SLCTFEExecResetReason(&execCtx);
            execCtx.pendingReturnExprNode = -1;
            rc = SLCTFEExecEvalBlock(&execCtx, blockNode, &retValue, &didReturn, &isConst);
            if (rc != 0) {
                c->localLen = savedLocalLen;
                c->localUseLen = savedLocalUseLen;
                c->variantNarrowLen = savedVariantNarrowLen;
                c->activeConstEvalCtx = savedActiveConstEvalCtx;
                return -1;
            }
            if (!isConst || didReturn) {
                c->lastConstEvalReason = execCtx.nonConstReason;
                c->lastConstEvalReasonStart = execCtx.nonConstStart;
                c->lastConstEvalReasonEnd = execCtx.nonConstEnd;
                if (outError != NULL) {
                    outError->code = SLDiag_CONST_BLOCK_EVAL_FAILED;
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
        }
        child = SLAstNextSibling(c->ast, child);
    }

    c->localLen = savedLocalLen;
    c->localUseLen = savedLocalUseLen;
    c->variantNarrowLen = savedVariantNarrowLen;
    c->activeConstEvalCtx = savedActiveConstEvalCtx;
    return 0;
}

int SLTCResolveComparisonHookArgCost(
    SLTypeCheckCtx* c, int32_t paramType, int32_t argType, uint8_t* outCost) {
    int32_t resolvedParam = SLTCResolveAliasBaseType(c, paramType);
    uint8_t baseCost = 0;
    if (SLTCConversionCost(c, paramType, argType, outCost) == 0) {
        return 0;
    }
    if (resolvedParam < 0 || (uint32_t)resolvedParam >= c->typeLen) {
        return -1;
    }
    if (c->types[resolvedParam].kind == SLTCType_REF && !SLTCTypeIsMutable(&c->types[resolvedParam])
        && SLTCConversionCost(c, c->types[resolvedParam].baseType, argType, &baseCost) == 0)
    {
        *outCost = (uint8_t)(baseCost < 254u ? baseCost + 1u : 255u);
        return 0;
    }
    return -1;
}

int SLTCResolveComparisonHook(
    SLTypeCheckCtx* c,
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
        const SLTCFunction* fn = &c->funcs[i];
        uint8_t             curCosts[2];
        uint32_t            curTotal = 0;
        int                 cmp;
        if (!SLNameEqLiteral(c->src, fn->nameStart, fn->nameEnd, hookName)) {
            continue;
        }
        nameFound = 1;
        if (fn->paramCount != 2) {
            continue;
        }
        if (SLTCResolveComparisonHookArgCost(
                c, c->funcParamTypes[fn->paramTypeStart], lhsType, &curCosts[0])
                != 0
            || SLTCResolveComparisonHookArgCost(
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
        cmp = SLTCCostVectorCompare(curCosts, bestCosts, 2u);
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

void SLTCGatherCallCandidates(
    SLTypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    int32_t*        outCandidates,
    uint32_t*       outCandidateCount,
    int*            outNameFound) {
    uint32_t count = 0;
    uint32_t i;
    *outNameFound = 0;
    for (i = 0; i < c->funcLen && count < SLTC_MAX_CALL_CANDIDATES; i++) {
        if (SLTCFunctionNameEq(c, i, nameStart, nameEnd)) {
            outCandidates[count++] = (int32_t)i;
            *outNameFound = 1;
        }
    }
    *outCandidateCount = count;
}

void SLTCGatherCallCandidatesByPkgMethod(
    SLTypeCheckCtx* c,
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
    for (i = 0; i < c->funcLen && count < SLTC_MAX_CALL_CANDIDATES; i++) {
        if (SLTCNameEqPkgPrefixedMethod(
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

int SLTCFunctionHasAnytypeParam(SLTypeCheckCtx* c, int32_t fnIndex) {
    const SLTCFunction* fn;
    uint32_t            p;
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen) {
        return 0;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & SLTCFunctionFlag_TEMPLATE) != 0) {
        return 1;
    }
    for (p = 0; p < fn->paramCount; p++) {
        if (c->funcParamTypes[fn->paramTypeStart + p] == c->typeAnytype) {
            return 1;
        }
    }
    return 0;
}

int SLTCInstantiateAnytypeFunctionForCall(
    SLTypeCheckCtx*        c,
    int32_t                fnIndex,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int32_t                autoRefFirstArgType,
    int32_t*               outFuncIndex,
    SLTCCallMapError*      outError) {
    const SLTCFunction* fn;
    int32_t             resolvedParamTypes[SLTC_MAX_CALL_ARGS];
    uint32_t            p;
    SLTCCallBinding     binding;
    SLTCCallMapError    mapError;
    uint8_t             hasAnytypeParam = 0;
    uint8_t             hasAnyPack = 0;
    int32_t             packElems[SLTC_MAX_CALL_ARGS];
    uint32_t            packElemCount = 0;

    if (outError != NULL) {
        SLTCCallMapErrorClear(outError);
    }
    if (fnIndex < 0 || (uint32_t)fnIndex >= c->funcLen || outFuncIndex == NULL) {
        return -1;
    }
    fn = &c->funcs[fnIndex];
    if ((fn->flags & SLTCFunctionFlag_TEMPLATE) == 0
        || (fn->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) != 0)
    {
        *outFuncIndex = fnIndex;
        return 0;
    }
    if (fn->paramCount > SLTC_MAX_CALL_ARGS) {
        return -1;
    }

    for (p = 0; p < fn->paramCount; p++) {
        resolvedParamTypes[p] = c->funcParamTypes[fn->paramTypeStart + p];
        if (resolvedParamTypes[p] == c->typeAnytype) {
            hasAnytypeParam = 1;
            if ((fn->flags & SLTCFunctionFlag_VARIADIC) != 0 && p + 1u == fn->paramCount) {
                hasAnyPack = 1;
            }
        }
    }
    if (!hasAnytypeParam) {
        *outFuncIndex = fnIndex;
        return 0;
    }

    SLTCCallMapErrorClear(&mapError);
    {
        int prepStatus = SLTCPrepareCallBinding(
            c,
            callArgs,
            argCount,
            &c->funcParamNameStarts[fn->paramTypeStart],
            &c->funcParamNameEnds[fn->paramTypeStart],
            &c->funcParamTypes[fn->paramTypeStart],
            fn->paramCount,
            (fn->flags & SLTCFunctionFlag_VARIADIC) != 0,
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
        int32_t argExprNode = callArgs[p].exprNode;
        int32_t paramIndex = binding.argParamIndices[p];
        int32_t expectedType = binding.argExpectedTypes[p];
        if (paramIndex < 0 || (uint32_t)paramIndex >= fn->paramCount) {
            return 1;
        }
        if (p == 0 && autoRefFirstArgType >= 0) {
            argType = autoRefFirstArgType;
        } else if (!SLTCExprNeedsExpectedType(c, argExprNode)) {
            if (SLTCTypeExpr(c, argExprNode, &argType) != 0) {
                return -1;
            }
        } else {
            SLDiag savedDiag = { 0 };
            if (c->diag != NULL) {
                savedDiag = *c->diag;
            }
            if (SLTCTypeExprExpected(c, argExprNode, expectedType, &argType) != 0) {
                if (c->diag != NULL) {
                    *c->diag = savedDiag;
                }
                return 1;
            }
        }
        if (SLTCConcretizeInferredType(c, argType, &argType) != 0) {
            return -1;
        }

        if (expectedType == c->typeAnytype) {
            if ((fn->flags & SLTCFunctionFlag_VARIADIC) != 0
                && (uint32_t)paramIndex + 1u == fn->paramCount)
            {
                if (binding.spreadArgIndex == p) {
                    int32_t spreadType = SLTCResolveAliasBaseType(c, argType);
                    if (spreadType < 0 || (uint32_t)spreadType >= c->typeLen
                        || c->types[spreadType].kind != SLTCType_PACK)
                    {
                        if (outError != NULL) {
                            outError->code = SLDiag_ANYTYPE_SPREAD_REQUIRES_PACK;
                            outError->start = callArgs[p].start;
                            outError->end = callArgs[p].end;
                            outError->argStart = 0;
                            outError->argEnd = 0;
                        }
                        return 2;
                    }
                    resolvedParamTypes[paramIndex] = spreadType;
                } else {
                    if (packElemCount >= SLTC_MAX_CALL_ARGS) {
                        return -1;
                    }
                    packElems[packElemCount++] = argType;
                }
            } else {
                resolvedParamTypes[paramIndex] = argType;
            }
        }
    }

    if (hasAnyPack) {
        int32_t lastParamIndex = (int32_t)fn->paramCount - 1;
        if (lastParamIndex < 0) {
            return 1;
        }
        if (binding.spreadArgIndex == UINT32_MAX) {
            int32_t packType = SLTCInternPackType(
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
                || c->types[spreadType].kind != SLTCType_PACK)
            {
                return 1;
            }
        }
    }

    for (p = 0; p < c->funcLen; p++) {
        const SLTCFunction* cur = &c->funcs[p];
        uint32_t            j;
        if (cur->declNode != fn->declNode || (cur->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) == 0
            || cur->paramCount != fn->paramCount || cur->returnType != fn->returnType
            || cur->contextType != fn->contextType
            || ((cur->flags & SLTCFunctionFlag_VARIADIC)
                != (fn->flags & SLTCFunctionFlag_VARIADIC)))
        {
            continue;
        }
        for (j = 0; j < fn->paramCount; j++) {
            if (c->funcParamTypes[cur->paramTypeStart + j] != resolvedParamTypes[j]
                || (c->funcParamFlags[cur->paramTypeStart + j] & SLTCFuncParamFlag_CONST)
                       != (c->funcParamFlags[fn->paramTypeStart + j] & SLTCFuncParamFlag_CONST))
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
        return SLTCFailNode(c, fn->declNode, SLDiag_ARENA_OOM);
    }

    {
        uint32_t      idx = c->funcLen++;
        SLTCFunction* f = &c->funcs[idx];
        int32_t       typeId;
        for (p = 0; p < fn->paramCount; p++) {
            c->funcParamTypes[c->funcParamLen + p] = resolvedParamTypes[p];
            c->funcParamNameStarts[c->funcParamLen + p] =
                c->funcParamNameStarts[fn->paramTypeStart + p];
            c->funcParamNameEnds[c->funcParamLen + p] =
                c->funcParamNameEnds[fn->paramTypeStart + p];
            c->funcParamFlags[c->funcParamLen + p] =
                c->funcParamFlags[fn->paramTypeStart + p] & SLTCFuncParamFlag_CONST;
        }
        f->nameStart = fn->nameStart;
        f->nameEnd = fn->nameEnd;
        f->returnType = fn->returnType;
        f->paramTypeStart = c->funcParamLen;
        f->paramCount = fn->paramCount;
        f->contextType = fn->contextType;
        f->declNode = fn->declNode;
        f->defNode = fn->defNode;
        f->funcTypeId = -1;
        f->flags = (fn->flags & SLTCFunctionFlag_VARIADIC) | SLTCFunctionFlag_TEMPLATE
                 | SLTCFunctionFlag_TEMPLATE_INSTANCE;
        if ((fn->flags & SLTCFunctionFlag_VARIADIC) != 0 && fn->paramCount > 0) {
            int32_t variadicType = resolvedParamTypes[fn->paramCount - 1u];
            if (variadicType >= 0 && (uint32_t)variadicType < c->typeLen
                && c->types[variadicType].kind == SLTCType_PACK)
            {
                f->flags |= SLTCFunctionFlag_TEMPLATE_HAS_ANYPACK;
            }
        }
        c->funcParamLen += fn->paramCount;
        typeId = SLTCInternFunctionType(
            c,
            f->returnType,
            &c->funcParamTypes[f->paramTypeStart],
            &c->funcParamFlags[f->paramTypeStart],
            f->paramCount,
            (f->flags & SLTCFunctionFlag_VARIADIC) != 0,
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

int SLTCResolveCallFromCandidates(
    SLTypeCheckCtx*        c,
    const int32_t*         candidates,
    uint32_t               candidateCount,
    int                    nameFound,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode) {
    uint8_t          bestCosts[SLTC_MAX_CALL_ARGS];
    int              haveBest = 0;
    int              ambiguous = 0;
    int              hasExpectedDependentArg = 0;
    int32_t          mutRefTempArgNode = -1;
    int32_t          autoRefType = -1;
    int              hasAutoRefType = 0;
    SLTCCallMapError firstMapError;
    int              hasMapError = 0;
    uint32_t         bestTotal = 0;
    uint32_t         i;
    uint32_t         p;
    SLTCCallMapErrorClear(&firstMapError);
    if (!nameFound) {
        return 1;
    }

    if (argCount > SLTC_MAX_CALL_ARGS) {
        return -1;
    }
    if (autoRefFirstArg && argCount > 0 && SLTCExprIsAssignable(c, callArgs[0].exprNode)) {
        int32_t          argType;
        const SLAstNode* argNode = &c->ast->nodes[callArgs[0].exprNode];
        if (SLTCTypeExpr(c, callArgs[0].exprNode, &argType) != 0) {
            return -1;
        }
        autoRefType = SLTCInternPtrType(c, argType, argNode->start, argNode->end);
        if (autoRefType < 0) {
            return -1;
        }
        hasAutoRefType = 1;
    }
    for (p = 0; p < argCount; p++) {
        hasExpectedDependentArg =
            hasExpectedDependentArg || SLTCExprNeedsExpectedType(c, callArgs[p].exprNode) != 0;
    }

    for (i = 0; i < candidateCount; i++) {
        int32_t             fnIdx = candidates[i];
        int32_t             candidateFnIdx = fnIdx;
        const SLTCFunction* fn = &c->funcs[candidateFnIdx];
        uint8_t             curCosts[SLTC_MAX_CALL_ARGS];
        SLTCCallBinding     binding;
        uint32_t            curTotal = 0;
        int                 viable = 1;
        int                 cmp;
        SLTCCallMapError    mapError;
        SLTCCallMapErrorClear(&mapError);
        if ((fn->flags & SLTCFunctionFlag_TEMPLATE) != 0
            && (fn->flags & SLTCFunctionFlag_TEMPLATE_INSTANCE) == 0)
        {
            int instantiateStatus = SLTCInstantiateAnytypeFunctionForCall(
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
            int prepStatus = SLTCPrepareCallBinding(
                c,
                callArgs,
                argCount,
                &c->funcParamNameStarts[fn->paramTypeStart],
                &c->funcParamNameEnds[fn->paramTypeStart],
                &c->funcParamTypes[fn->paramTypeStart],
                fn->paramCount,
                (fn->flags & SLTCFunctionFlag_VARIADIC) != 0,
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
                if (SLTCIsMutableRefType(c, paramType) && SLTCExprIsCompoundTemporary(c, argNode)) {
                    if (mutRefTempArgNode < 0) {
                        mutRefTempArgNode = argNode;
                    }
                    viable = 0;
                    break;
                }
                if (!SLTCExprNeedsExpectedType(c, argNode)) {
                    if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                        return -1;
                    }
                } else {
                    SLDiag savedDiag = { 0 };
                    if (c->diag != NULL) {
                        savedDiag = *c->diag;
                    }
                    if (SLTCTypeExprExpected(c, argNode, paramType, &argType) != 0) {
                        if (c->diag != NULL) {
                            *c->diag = savedDiag;
                        }
                        if ((fn->flags & SLTCFunctionFlag_VARIADIC) != 0
                            && p >= binding.fixedInputCount)
                        {
                            mapError.code =
                                (binding.spreadArgIndex == p)
                                    ? SLDiag_VARIADIC_SPREAD_NON_SLICE
                                    : SLDiag_VARIADIC_ARG_TYPE_MISMATCH;
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
            if (SLTCConversionCost(c, paramType, argType, &cost) != 0) {
                if ((fn->flags & SLTCFunctionFlag_VARIADIC) != 0 && p >= binding.fixedInputCount) {
                    mapError.code =
                        (binding.spreadArgIndex == p)
                            ? SLDiag_VARIADIC_SPREAD_NON_SLICE
                            : SLDiag_VARIADIC_ARG_TYPE_MISMATCH;
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
            int constStatus = SLTCCheckConstParamArgs(
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
            int constBlockStatus = SLTCCheckConstBlocksForCall(
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
        cmp = SLTCCostVectorCompare(curCosts, bestCosts, argCount);
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
            SLTCSetDiagWithArg(
                c->diag,
                firstMapError.code,
                firstMapError.start,
                firstMapError.end,
                firstMapError.argStart,
                firstMapError.argEnd);
            if (firstMapError.code == SLDiag_CONST_BLOCK_EVAL_FAILED) {
                SLTCAttachConstEvalReason(c);
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

int SLTCResolveCallByName(
    SLTypeCheckCtx*        c,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode) {
    int32_t  candidates[SLTC_MAX_CALL_CANDIDATES];
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

    SLTCGatherCallCandidates(c, nameStart, nameEnd, candidates, &candidateCount, &nameFound);
    return SLTCResolveCallFromCandidates(
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

int SLTCResolveCallByPkgMethod(
    SLTypeCheckCtx*        c,
    uint32_t               pkgStart,
    uint32_t               pkgEnd,
    uint32_t               methodStart,
    uint32_t               methodEnd,
    const SLTCCallArgInfo* callArgs,
    uint32_t               argCount,
    uint32_t               firstPositionalArgIndex,
    int                    autoRefFirstArg,
    int32_t*               outFuncIndex,
    int32_t*               outMutRefTempArgNode) {
    int32_t  candidates[SLTC_MAX_CALL_CANDIDATES];
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

    SLTCGatherCallCandidatesByPkgMethod(
        c, pkgStart, pkgEnd, methodStart, methodEnd, candidates, &candidateCount, &nameFound);
    return SLTCResolveCallFromCandidates(
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

int SLTCResolveDependentPtrReturnForCall(
    SLTypeCheckCtx* c, int32_t fnIndex, int32_t argNode, int32_t* outType) {
    const SLTCFunction* fn;
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
        || c->types[returnType].kind != SLTCType_PTR
        || c->types[returnType].baseType != c->typeType)
    {
        return 0;
    }
    if (SLTCResolveReflectedTypeValueExpr(c, argNode, &reflectedType) != 0) {
        return 0;
    }
    reflectedType = SLTCInternPtrType(
        c, reflectedType, c->ast->nodes[argNode].start, c->ast->nodes[argNode].end);
    if (reflectedType < 0) {
        return -1;
    }
    *outType = reflectedType;
    return 1;
}

SL_API_END
