#include "internal.h"

SL_API_BEGIN

int SLTCResolveNamedTypeFields(SLTypeCheckCtx* c, uint32_t namedIndex) {
    const SLTCNamedType* nt = &c->namedTypes[namedIndex];
    int32_t              declNode = nt->declNode;
    SLAstKind            kind = c->ast->nodes[declNode].kind;
    int32_t              child;
    SLTCField*           pendingFields = NULL;
    int32_t*             fieldNodes = NULL;
    int32_t*             fieldTypesForDefaults = NULL;
    uint32_t             pendingCap = 0;
    uint32_t             fieldCount = 0;
    uint32_t             fieldOrdinal = 0;
    int                  sawDependent = 0;
    int                  sawEmbedded = 0;

    if (kind == SLAst_TYPE_ALIAS) {
        return 0;
    }

    child = SLAstFirstChild(c->ast, declNode);
    if (kind == SLAst_ENUM && child >= 0
        && (c->ast->nodes[child].kind == SLAst_TYPE_NAME
            || c->ast->nodes[child].kind == SLAst_TYPE_PTR
            || c->ast->nodes[child].kind == SLAst_TYPE_ARRAY
            || c->ast->nodes[child].kind == SLAst_TYPE_REF
            || c->ast->nodes[child].kind == SLAst_TYPE_MUTREF
            || c->ast->nodes[child].kind == SLAst_TYPE_SLICE
            || c->ast->nodes[child].kind == SLAst_TYPE_MUTSLICE
            || c->ast->nodes[child].kind == SLAst_TYPE_VARRAY
            || c->ast->nodes[child].kind == SLAst_TYPE_FN
            || c->ast->nodes[child].kind == SLAst_TYPE_ANON_STRUCT
            || c->ast->nodes[child].kind == SLAst_TYPE_ANON_UNION
            || c->ast->nodes[child].kind == SLAst_TYPE_TUPLE))
    {
        child = SLAstNextSibling(c->ast, child);
    }

    {
        int32_t scan = child;
        while (scan >= 0) {
            if (c->ast->nodes[scan].kind == SLAst_FIELD
                && (kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM))
            {
                pendingCap++;
            }
            scan = SLAstNextSibling(c->ast, scan);
        }
        if (pendingCap > 0) {
            pendingFields = (SLTCField*)SLArenaAlloc(
                c->arena, pendingCap * sizeof(SLTCField), (uint32_t)_Alignof(SLTCField));
            fieldNodes = (int32_t*)SLArenaAlloc(
                c->arena, pendingCap * sizeof(int32_t), (uint32_t)_Alignof(int32_t));
            fieldTypesForDefaults = (int32_t*)SLArenaAlloc(
                c->arena, pendingCap * sizeof(int32_t), (uint32_t)_Alignof(int32_t));
            if (pendingFields == NULL || fieldNodes == NULL || fieldTypesForDefaults == NULL) {
                return SLTCFailNode(c, declNode, SLDiag_ARENA_OOM);
            }
            scan = child;
            pendingCap = 0;
            while (scan >= 0) {
                if (c->ast->nodes[scan].kind == SLAst_FIELD
                    && (kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM))
                {
                    fieldNodes[pendingCap] = scan;
                    fieldTypesForDefaults[pendingCap] = -1;
                    pendingCap++;
                }
                scan = SLAstNextSibling(c->ast, scan);
            }
        }
    }

    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_FIELD
            && (kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM))
        {
            uint32_t currentFieldOrdinal = fieldOrdinal++;
            int32_t  typeNode = SLAstFirstChild(c->ast, child);
            int32_t  typeId;
            uint16_t fieldFlags = 0;
            uint32_t lenNameStart = 0;
            uint32_t lenNameEnd = 0;
            uint32_t i;
            int      isEmbedded = (n->flags & SLAstFlag_FIELD_EMBEDDED) != 0;
            if (kind == SLAst_ENUM) {
                typeId = nt->typeId;
            } else {
                if (typeNode < 0) {
                    return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
                }

                if (isEmbedded) {
                    int32_t embeddedDeclNode;
                    int32_t embedType;
                    if (kind != SLAst_STRUCT) {
                        return SLTCFailSpan(
                            c, SLDiag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                    }
                    if (sawEmbedded) {
                        return SLTCFailSpan(
                            c, SLDiag_MULTIPLE_EMBEDDED_FIELDS, n->dataStart, n->dataEnd);
                    }
                    if (fieldCount != 0) {
                        return SLTCFailSpan(
                            c, SLDiag_EMBEDDED_FIELD_NOT_FIRST, n->dataStart, n->dataEnd);
                    }
                    if (sawDependent) {
                        return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
                    }
                    if (SLTCResolveTypeNode(c, typeNode, &typeId) != 0) {
                        return -1;
                    }
                    embedType = typeId;
                    if (embedType >= 0 && (uint32_t)embedType < c->typeLen
                        && c->types[embedType].kind == SLTCType_ALIAS)
                    {
                        embedType = SLTCResolveAliasBaseType(c, embedType);
                        if (embedType < 0) {
                            return -1;
                        }
                    }
                    if (embedType < 0 || (uint32_t)embedType >= c->typeLen) {
                        return SLTCFailSpan(
                            c, SLDiag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                    }
                    if (c->types[embedType].kind == SLTCType_NAMED) {
                        embeddedDeclNode = c->types[embedType].declNode;
                        if (embeddedDeclNode < 0 || (uint32_t)embeddedDeclNode >= c->ast->len
                            || c->ast->nodes[embeddedDeclNode].kind != SLAst_STRUCT)
                        {
                            return SLTCFailSpan(
                                c, SLDiag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                        }
                    } else if (c->types[embedType].kind != SLTCType_ANON_STRUCT) {
                        return SLTCFailSpan(
                            c, SLDiag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                    }
                    typeId = embedType;
                    fieldFlags = SLTCFieldFlag_EMBEDDED;
                    sawEmbedded = 1;
                } else if (c->ast->nodes[typeNode].kind == SLAst_TYPE_VARRAY) {
                    int32_t  elemTypeNode;
                    int32_t  elemType;
                    int32_t  ptrType;
                    int32_t  lenFieldType = -1;
                    uint32_t j;
                    if (kind != SLAst_STRUCT) {
                        return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
                    }
                    sawDependent = 1;
                    lenNameStart = c->ast->nodes[typeNode].dataStart;
                    lenNameEnd = c->ast->nodes[typeNode].dataEnd;

                    j = 0;
                    while (j < fieldCount) {
                        if ((pendingFields[j].flags & SLTCFieldFlag_DEPENDENT) == 0
                            && SLNameEqSlice(
                                c->src,
                                pendingFields[j].nameStart,
                                pendingFields[j].nameEnd,
                                lenNameStart,
                                lenNameEnd))
                        {
                            lenFieldType = pendingFields[j].typeId;
                            break;
                        }
                        j++;
                    }
                    if (lenFieldType < 0) {
                        return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, lenNameStart, lenNameEnd);
                    }
                    if (!SLTCIsIntegerType(c, lenFieldType)) {
                        return SLTCFailSpan(c, SLDiag_TYPE_MISMATCH, lenNameStart, lenNameEnd);
                    }

                    elemTypeNode = SLAstFirstChild(c->ast, typeNode);
                    if (elemTypeNode < 0) {
                        return SLTCFailNode(c, typeNode, SLDiag_EXPECTED_TYPE);
                    }
                    if (SLTCResolveTypeNode(c, elemTypeNode, &elemType) != 0) {
                        return -1;
                    }
                    ptrType = SLTCInternPtrType(c, elemType, n->start, n->end);
                    if (ptrType < 0) {
                        return -1;
                    }
                    typeId = ptrType;
                    fieldFlags = SLTCFieldFlag_DEPENDENT;
                    c->types[nt->typeId].flags |= SLTCTypeFlag_VARSIZE;
                } else {
                    if (sawDependent) {
                        return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
                    }
                    if (SLTCResolveTypeNode(c, typeNode, &typeId) != 0) {
                        return -1;
                    }
                }
            }

            if (fieldTypesForDefaults != NULL && currentFieldOrdinal < pendingCap) {
                fieldTypesForDefaults[currentFieldOrdinal] = typeId;
            }

            for (i = 0; i < fieldCount; i++) {
                if (SLNameEqSlice(
                        c->src,
                        pendingFields[i].nameStart,
                        pendingFields[i].nameEnd,
                        n->dataStart,
                        n->dataEnd))
                {
                    return SLTCFailSpan(c, SLDiag_DUPLICATE_SYMBOL, n->dataStart, n->dataEnd);
                }
            }

            if (kind == SLAst_STRUCT || kind == SLAst_UNION) {
                int32_t defaultNode = typeNode >= 0 ? SLAstNextSibling(c->ast, typeNode) : -1;
                if (defaultNode >= 0) {
                    int32_t        defaultType;
                    const int32_t* savedDefaultFieldNodes = c->defaultFieldNodes;
                    const int32_t* savedDefaultFieldTypes = c->defaultFieldTypes;
                    uint32_t       savedDefaultFieldCount = c->defaultFieldCount;
                    uint32_t       savedDefaultFieldCurrentIndex = c->defaultFieldCurrentIndex;

                    if (kind != SLAst_STRUCT) {
                        return SLTCFailNode(c, defaultNode, SLDiag_TYPE_MISMATCH);
                    }
                    if (isEmbedded) {
                        return SLTCFailSpan(
                            c, SLDiag_FIELD_DEFAULT_ON_EMBEDDED, n->dataStart, n->dataEnd);
                    }

                    c->defaultFieldNodes = fieldNodes;
                    c->defaultFieldTypes = fieldTypesForDefaults;
                    c->defaultFieldCount = pendingCap;
                    c->defaultFieldCurrentIndex = currentFieldOrdinal;
                    if (SLTCTypeExprExpected(c, defaultNode, typeId, &defaultType) != 0) {
                        c->defaultFieldNodes = savedDefaultFieldNodes;
                        c->defaultFieldTypes = savedDefaultFieldTypes;
                        c->defaultFieldCount = savedDefaultFieldCount;
                        c->defaultFieldCurrentIndex = savedDefaultFieldCurrentIndex;
                        return -1;
                    }
                    c->defaultFieldNodes = savedDefaultFieldNodes;
                    c->defaultFieldTypes = savedDefaultFieldTypes;
                    c->defaultFieldCount = savedDefaultFieldCount;
                    c->defaultFieldCurrentIndex = savedDefaultFieldCurrentIndex;

                    if (!SLTCCanAssign(c, typeId, defaultType)) {
                        SLTCSetDiagWithArg(
                            c->diag,
                            SLDiag_FIELD_DEFAULT_TYPE_MISMATCH,
                            c->ast->nodes[defaultNode].start,
                            c->ast->nodes[defaultNode].end,
                            n->dataStart,
                            n->dataEnd);
                        return -1;
                    }
                }
            }

            if (fieldCount >= pendingCap) {
                return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
            }
            pendingFields[fieldCount].nameStart = n->dataStart;
            pendingFields[fieldCount].nameEnd = n->dataEnd;
            pendingFields[fieldCount].typeId = typeId;
            pendingFields[fieldCount].lenNameStart = lenNameStart;
            pendingFields[fieldCount].lenNameEnd = lenNameEnd;
            pendingFields[fieldCount].flags = fieldFlags;
            fieldCount++;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    {
        uint32_t fieldStart = c->fieldLen;
        uint32_t i;
        if (c->fieldLen + fieldCount > c->fieldCap) {
            return SLTCFailNode(c, declNode, SLDiag_ARENA_OOM);
        }
        for (i = 0; i < fieldCount; i++) {
            c->fields[c->fieldLen++] = pendingFields[i];
        }
        c->types[nt->typeId].fieldStart = fieldStart;
    }
    c->types[nt->typeId].fieldCount = (uint16_t)fieldCount;
    return 0;
}

int SLTCResolveAllNamedTypeFields(SLTypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (SLTCResolveNamedTypeFields(c, i) != 0) {
            return -1;
        }
    }
    return 0;
}

int SLTCResolveAllTypeAliases(SLTypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        int32_t typeId = c->namedTypes[i].typeId;
        if (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == SLTCType_ALIAS
            && SLTCResolveAliasTypeId(c, typeId) != 0)
        {
            return -1;
        }
    }
    return 0;
}

int SLTCCheckEmbeddedCycleFrom(SLTypeCheckCtx* c, int32_t typeId) {
    SLTCType* type;
    int32_t   embedIdx;
    int32_t   baseType;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != SLTCType_NAMED) {
        return 0;
    }
    type = &c->types[typeId];
    if ((type->flags & SLTCTypeFlag_VISITING) != 0) {
        return 0;
    }
    if ((type->flags & SLTCTypeFlag_VISITED) != 0) {
        return 0;
    }
    type->flags |= SLTCTypeFlag_VISITING;

    embedIdx = SLTCFindEmbeddedFieldIndex(c, typeId);
    if (embedIdx >= 0) {
        baseType = c->fields[embedIdx].typeId;
        if (baseType >= 0 && (uint32_t)baseType < c->typeLen
            && c->types[baseType].kind == SLTCType_ALIAS)
        {
            baseType = SLTCResolveAliasBaseType(c, baseType);
            if (baseType < 0) {
                return -1;
            }
        }
        if (baseType >= 0 && (uint32_t)baseType < c->typeLen
            && c->types[baseType].kind == SLTCType_NAMED)
        {
            if ((c->types[baseType].flags & SLTCTypeFlag_VISITING) != 0) {
                return SLTCFailSpan(
                    c,
                    SLDiag_EMBEDDED_CYCLE,
                    c->fields[embedIdx].nameStart,
                    c->fields[embedIdx].nameEnd);
            }
            if (SLTCCheckEmbeddedCycleFrom(c, baseType) != 0) {
                return -1;
            }
        }
    }

    type->flags &= (uint16_t)~SLTCTypeFlag_VISITING;
    type->flags |= SLTCTypeFlag_VISITED;
    return 0;
}

int SLTCCheckEmbeddedCycles(SLTypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        int32_t typeId = c->namedTypes[i].typeId;
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
            continue;
        }
        if (SLTCCheckEmbeddedCycleFrom(c, typeId) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->typeLen; i++) {
        c->types[i].flags &= (uint16_t)~(SLTCTypeFlag_VISITING | SLTCTypeFlag_VISITED);
    }
    return 0;
}

int SLTCPropagateVarSizeNamedTypes(SLTypeCheckCtx* c) {
    int changed = 1;
    while (changed) {
        uint32_t i;
        changed = 0;
        for (i = 0; i < c->namedTypeLen; i++) {
            int32_t  typeId = c->namedTypes[i].typeId;
            uint32_t j;
            if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
                continue;
            }
            if ((c->types[typeId].flags & SLTCTypeFlag_VARSIZE) != 0) {
                continue;
            }
            for (j = 0; j < c->types[typeId].fieldCount; j++) {
                uint32_t        fieldIdx = c->types[typeId].fieldStart + j;
                const SLTCField field = c->fields[fieldIdx];
                if ((field.flags & SLTCFieldFlag_DEPENDENT) != 0
                    || SLTCTypeContainsVarSizeByValue(c, field.typeId))
                {
                    c->types[typeId].flags |= SLTCTypeFlag_VARSIZE;
                    changed = 1;
                    break;
                }
            }
        }
    }
    return 0;
}

int SLTCReadFunctionSig(
    SLTypeCheckCtx* c,
    int32_t         funNode,
    int32_t*        outReturnType,
    uint32_t*       outParamCount,
    int*            outIsVariadic,
    int32_t*        outContextType,
    int*            outHasBody) {
    int32_t  child = SLAstFirstChild(c->ast, funNode);
    uint32_t paramCount = 0;
    int32_t  returnType = c->typeVoid;
    int32_t  contextType = -1;
    int      isVariadic = 0;
    int      hasBody = 0;

    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            int32_t typeNode = SLAstFirstChild(c->ast, child);
            int32_t typeId;
            if (paramCount >= c->scratchParamCap) {
                return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
            }
            if (SLTCResolveTypeNode(c, typeNode, &typeId) != 0) {
                return -1;
            }
            if (SLTCTypeContainsVarSizeByValue(c, typeId)) {
                return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
            }
            if ((n->flags & SLAstFlag_PARAM_VARIADIC) != 0) {
                int32_t sliceType = SLTCInternSliceType(c, typeId, 0, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                typeId = sliceType;
                isVariadic = 1;
            }
            c->scratchParamTypes[paramCount++] = typeId;
            c->scratchParamFlags[paramCount - 1u] =
                (n->flags & SLAstFlag_PARAM_CONST) != 0 ? SLTCFuncParamFlag_CONST : 0u;
        } else if (
            (n->kind == SLAst_TYPE_NAME || n->kind == SLAst_TYPE_PTR || n->kind == SLAst_TYPE_ARRAY
             || n->kind == SLAst_TYPE_REF || n->kind == SLAst_TYPE_MUTREF
             || n->kind == SLAst_TYPE_SLICE || n->kind == SLAst_TYPE_MUTSLICE
             || n->kind == SLAst_TYPE_VARRAY || n->kind == SLAst_TYPE_OPTIONAL
             || n->kind == SLAst_TYPE_FN || n->kind == SLAst_TYPE_ANON_STRUCT
             || n->kind == SLAst_TYPE_ANON_UNION || n->kind == SLAst_TYPE_TUPLE)
            && n->flags == 1)
        {
            if (SLTCResolveTypeNode(c, child, &returnType) != 0) {
                return -1;
            }
            if (SLTCTypeContainsVarSizeByValue(c, returnType)) {
                return SLTCFailNode(c, child, SLDiag_TYPE_MISMATCH);
            }
        } else if (n->kind == SLAst_CONTEXT_CLAUSE) {
            int32_t typeNode = SLAstFirstChild(c->ast, child);
            if (contextType >= 0) {
                return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
            }
            if (typeNode < 0) {
                return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
            }
            if (SLTCResolveTypeNode(c, typeNode, &contextType) != 0) {
                return -1;
            }
        } else if (n->kind == SLAst_BLOCK) {
            hasBody = 1;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    *outReturnType = returnType;
    *outParamCount = paramCount;
    if (outIsVariadic != NULL) {
        *outIsVariadic = isVariadic;
    }
    *outContextType = contextType;
    *outHasBody = hasBody;
    return 0;
}

int SLTCCollectFunctionFromNode(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    int32_t          returnType;
    int32_t          contextType;
    uint32_t         paramCount;
    int              isVariadic = 0;
    int              hasBody;
    int32_t          savedActiveTypeParamFnNode = c->activeTypeParamFnNode;

    if (n->kind == SLAst_PUB) {
        int32_t ch = SLAstFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (SLTCCollectFunctionFromNode(c, ch) != 0) {
                return -1;
            }
            ch = SLAstNextSibling(c->ast, ch);
        }
        return 0;
    }

    if (n->kind != SLAst_FN) {
        return 0;
    }

    c->activeTypeParamFnNode = nodeId;
    if (SLTCReadFunctionSig(
            c, nodeId, &returnType, &paramCount, &isVariadic, &contextType, &hasBody)
        != 0)
    {
        c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
        return -1;
    }
    c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
    if (contextType >= 0 && SLNameEqLiteral(c->src, n->dataStart, n->dataEnd, "main")) {
        return SLTCFailSpan(c, SLDiag_UNEXPECTED_TOKEN, n->start, n->end);
    }
    {
        int isEqualHook = 0;
        int intType = -1;
        if (SLTCIsComparisonHookName(c, n->dataStart, n->dataEnd, &isEqualHook)) {
            if (paramCount != 2 || contextType >= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_INVALID_COMPARISON_HOOK_SIGNATURE);
            }
            intType = SLTCFindBuiltinByKind(c, SLBuiltin_ISIZE);
            if ((isEqualHook && returnType != c->typeBool)
                || (!isEqualHook && returnType != intType))
            {
                return SLTCFailNode(c, nodeId, SLDiag_INVALID_COMPARISON_HOOK_SIGNATURE);
            }
            if (!SLTCCanAssign(c, c->scratchParamTypes[0], c->scratchParamTypes[1])) {
                return SLTCFailNode(c, nodeId, SLDiag_INVALID_COMPARISON_HOOK_SIGNATURE);
            }
        }
    }

    {
        uint32_t i;
        for (i = 0; i < c->funcLen; i++) {
            SLTCFunction* f = &c->funcs[i];
            uint32_t      p;
            if (!SLTCFunctionNameEq(c, i, n->dataStart, n->dataEnd)) {
                continue;
            }
            if (f->paramCount != paramCount || f->returnType != returnType
                || (((f->flags & SLTCFunctionFlag_VARIADIC) != 0) != (isVariadic != 0)))
            {
                continue;
            }
            if (f->contextType != contextType) {
                return SLTCFailSpan(c, SLDiag_CONTEXT_CLAUSE_MISMATCH, n->start, n->end);
            }
            for (p = 0; p < paramCount; p++) {
                if (c->funcParamTypes[f->paramTypeStart + p] != c->scratchParamTypes[p]) {
                    break;
                }
                if ((c->funcParamFlags[f->paramTypeStart + p] & SLTCFuncParamFlag_CONST)
                    != (c->scratchParamFlags[p] & SLTCFuncParamFlag_CONST))
                {
                    break;
                }
            }
            if (p != paramCount) {
                continue;
            }
            if (hasBody) {
                if (f->defNode >= 0) {
                    return SLTCFailSpan(c, SLDiag_DUPLICATE_SYMBOL, n->dataStart, n->dataEnd);
                }
                f->defNode = nodeId;
            }
            return 0;
        }
    }

    if (c->funcLen >= c->funcCap || c->funcParamLen + paramCount > c->funcParamCap) {
        return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
    }

    {
        uint32_t      idx = c->funcLen++;
        SLTCFunction* f = &c->funcs[idx];
        int32_t       child = SLAstFirstChild(c->ast, nodeId);
        uint32_t      paramIndex = 0;
        f->nameStart = n->dataStart;
        f->nameEnd = n->dataEnd;
        f->returnType = returnType;
        f->paramTypeStart = c->funcParamLen;
        f->paramCount = (uint16_t)paramCount;
        f->contextType = contextType;
        f->declNode = nodeId;
        f->defNode = hasBody ? nodeId : -1;
        f->funcTypeId = -1;
        f->flags = (uint16_t)(isVariadic ? SLTCFunctionFlag_VARIADIC : 0u);
        while (child >= 0) {
            const SLAstNode* ch = &c->ast->nodes[child];
            if (ch->kind == SLAst_PARAM) {
                if (paramIndex >= paramCount) {
                    return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
                }
                c->funcParamTypes[c->funcParamLen] = c->scratchParamTypes[paramIndex];
                c->funcParamNameStarts[c->funcParamLen] = ch->dataStart;
                c->funcParamNameEnds[c->funcParamLen] = ch->dataEnd;
                c->funcParamFlags[c->funcParamLen] =
                    c->scratchParamFlags[paramIndex] & SLTCFuncParamFlag_CONST;
                c->funcParamLen++;
                paramIndex++;
            }
            child = SLAstNextSibling(c->ast, child);
        }
        if (paramIndex != paramCount) {
            return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
        }
    }

    return 0;
}

int SLTCFinalizeFunctionTypes(SLTypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        int32_t typeId = SLTCInternFunctionType(
            c,
            c->funcs[i].returnType,
            &c->funcParamTypes[c->funcs[i].paramTypeStart],
            &c->funcParamFlags[c->funcs[i].paramTypeStart],
            c->funcs[i].paramCount,
            (c->funcs[i].flags & SLTCFunctionFlag_VARIADIC) != 0,
            (int32_t)i,
            c->funcs[i].nameStart,
            c->funcs[i].nameEnd);
        if (typeId < 0) {
            return -1;
        }
        c->funcs[i].funcTypeId = typeId;
    }
    return 0;
}

int32_t SLTCFindTopLevelVarLikeNode(
    SLTypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex) {
    int32_t child = SLAstFirstChild(c->ast, c->ast->root);
    if (outNameIndex != NULL) {
        *outNameIndex = -1;
    }
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_VAR || n->kind == SLAst_CONST) {
            int32_t nameIndex = SLTCVarLikeNameIndexBySlice(c, child, start, end);
            if (nameIndex >= 0) {
                if (outNameIndex != NULL) {
                    *outNameIndex = nameIndex;
                }
                return child;
            }
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return -1;
}

int SLTCTypeTopLevelVarLikeNode(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType) {
    uint8_t          state;
    SLTCVarLikeParts parts;
    int32_t          resolvedType;
    int32_t          initNode;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    if (c->topVarLikeTypeState == NULL || c->topVarLikeTypes == NULL) {
        return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
    }
    if (SLTCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    if (nameIndex < 0 || (uint32_t)nameIndex >= parts.nameCount) {
        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_SYMBOL);
    }
    state = c->topVarLikeTypeState[nodeId];
    if (state == SLTCTopVarLikeType_READY) {
        *outType = c->topVarLikeTypes[nodeId];
        return 0;
    }
    if (state == SLTCTopVarLikeType_VISITING) {
        int rc;
        if (c->ast->nodes[nodeId].kind == SLAst_CONST) {
            rc = SLTCFailNode(c, nodeId, SLDiag_CONST_INIT_CONST_REQUIRED);
            c->diag->detail = SLTCAllocDiagText(c, "cyclic top-level initializer dependency");
            return rc;
        }
        rc = SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        c->diag->detail = SLTCAllocDiagText(c, "cyclic top-level initializer dependency");
        return rc;
    }

    c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_VISITING;
    if (parts.typeNode >= 0) {
        initNode = parts.initNode;
        if (c->ast->nodes[nodeId].kind == SLAst_CONST && initNode < 0) {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return SLTCFailNode(c, nodeId, SLDiag_CONST_MISSING_INITIALIZER);
        }
        if (SLTCResolveTypeNode(c, parts.typeNode, &resolvedType) != 0) {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return -1;
        }
        if (c->ast->nodes[nodeId].kind == SLAst_VAR && initNode < 0
            && !SLTCEnumTypeHasTagZero(c, resolvedType))
        {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
        }
        c->topVarLikeTypes[nodeId] = resolvedType;
        c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_READY;
        *outType = resolvedType;
        return 0;
    }

    initNode = SLTCVarLikeInitExprNodeAt(c, nodeId, nameIndex);
    if (initNode < 0) {
        c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
    {
        int32_t initType;
        if (SLTCTypeExpr(c, initNode, &initType) != 0) {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return -1;
        }
        if (initType == c->typeNull) {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return SLTCFailNode(c, initNode, SLDiag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (initType == c->typeVoid) {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return SLTCFailNode(c, initNode, SLDiag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (SLTCConcretizeInferredType(c, initType, &resolvedType) != 0) {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return -1;
        }
        if (SLTCTypeContainsVarSizeByValue(c, resolvedType)) {
            c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
            return SLTCFailNode(c, initNode, SLDiag_TYPE_MISMATCH);
        }
    }
    if (!parts.grouped) {
        c->topVarLikeTypes[nodeId] = resolvedType;
        c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_READY;
    } else {
        c->topVarLikeTypeState[nodeId] = SLTCTopVarLikeType_UNSEEN;
    }
    *outType = resolvedType;
    return 0;
}

int32_t SLTCLocalFind(SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (SLNameEqSlice(c->src, c->locals[i].nameStart, c->locals[i].nameEnd, nameStart, nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int SLTCLocalAdd(
    SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t typeId, int isConst) {
    if (c->localLen >= c->localCap) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, nameStart, nameEnd);
    }
    c->locals[c->localLen].nameStart = nameStart;
    c->locals[c->localLen].nameEnd = nameEnd;
    c->locals[c->localLen].typeId = typeId;
    c->locals[c->localLen].flags = isConst ? SLTCLocalFlag_CONST : 0;
    c->localLen++;
    return 0;
}

int SLTCVariantNarrowPush(
    SLTypeCheckCtx* c,
    int32_t         localIdx,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd) {
    if (c->variantNarrowLen >= c->variantNarrowCap) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, variantStart, variantEnd);
    }
    c->variantNarrows[c->variantNarrowLen].localIdx = localIdx;
    c->variantNarrows[c->variantNarrowLen].enumTypeId = enumTypeId;
    c->variantNarrows[c->variantNarrowLen].variantStart = variantStart;
    c->variantNarrows[c->variantNarrowLen].variantEnd = variantEnd;
    c->variantNarrowLen++;
    return 0;
}

int SLTCVariantNarrowFind(
    SLTypeCheckCtx* c, int32_t localIdx, const SLTCVariantNarrow** outNarrow) {
    uint32_t i = c->variantNarrowLen;
    while (i > 0) {
        i--;
        if (c->variantNarrows[i].localIdx == localIdx) {
            *outNarrow = &c->variantNarrows[i];
            return 1;
        }
    }
    return 0;
}

int32_t SLTCEnumDeclFirstVariantNode(SLTypeCheckCtx* c, int32_t enumDeclNode) {
    int32_t child = SLAstFirstChild(c->ast, enumDeclNode);
    if (child >= 0 && SLTCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        child = SLAstNextSibling(c->ast, child);
    }
    return child;
}

int32_t SLTCEnumVariantTagExprNode(SLTypeCheckCtx* c, int32_t variantNode) {
    int32_t child = SLAstFirstChild(c->ast, variantNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind != SLAst_FIELD) {
            return child;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return -1;
}

int32_t SLTCFindEnumVariantNodeByName(
    SLTypeCheckCtx* c, int32_t enumTypeId, uint32_t variantStart, uint32_t variantEnd) {
    int32_t declNode;
    int32_t variant;
    if (enumTypeId < 0 || (uint32_t)enumTypeId >= c->typeLen) {
        return -1;
    }
    declNode = c->types[enumTypeId].declNode;
    if (declNode < 0 || (uint32_t)declNode >= c->ast->len
        || c->ast->nodes[declNode].kind != SLAst_ENUM)
    {
        return -1;
    }
    variant = SLTCEnumDeclFirstVariantNode(c, declNode);
    while (variant >= 0) {
        const SLAstNode* vn = &c->ast->nodes[variant];
        if (vn->kind == SLAst_FIELD
            && SLNameEqSlice(c->src, vn->dataStart, vn->dataEnd, variantStart, variantEnd))
        {
            return variant;
        }
        variant = SLAstNextSibling(c->ast, variant);
    }
    return -1;
}

int SLTCEnumVariantPayloadFieldType(
    SLTypeCheckCtx* c,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType) {
    int32_t variantNode = SLTCFindEnumVariantNodeByName(c, enumTypeId, variantStart, variantEnd);
    int32_t ch;
    if (variantNode < 0) {
        return -1;
    }
    ch = SLAstFirstChild(c->ast, variantNode);
    while (ch >= 0) {
        const SLAstNode* fn = &c->ast->nodes[ch];
        int32_t          typeNode;
        int32_t          typeId;
        if (fn->kind != SLAst_FIELD) {
            break;
        }
        if (!SLNameEqSlice(c->src, fn->dataStart, fn->dataEnd, fieldStart, fieldEnd)) {
            ch = SLAstNextSibling(c->ast, ch);
            continue;
        }
        typeNode = SLAstFirstChild(c->ast, ch);
        if (typeNode < 0) {
            return -1;
        }
        if (SLTCResolveTypeNode(c, typeNode, &typeId) != 0) {
            return -1;
        }
        *outType = typeId;
        return 0;
    }
    return -1;
}

int SLTCEnumTypeHasTagZero(SLTypeCheckCtx* c, int32_t enumTypeId) {
    int32_t declNode;
    int32_t variant;
    int64_t nextValue = 0;
    int     haveKnownSequence = 1;
    if (!SLTCIsNamedDeclKind(c, enumTypeId, SLAst_ENUM)) {
        return 1;
    }
    declNode = c->types[SLTCResolveAliasBaseType(c, enumTypeId)].declNode;
    if (declNode < 0 || (uint32_t)declNode >= c->ast->len) {
        return 1;
    }
    variant = SLTCEnumDeclFirstVariantNode(c, declNode);
    while (variant >= 0) {
        int32_t initExpr = SLTCEnumVariantTagExprNode(c, variant);
        int64_t v = 0;
        int     isConst = 0;
        if (initExpr >= 0) {
            if (SLTCConstIntExpr(c, initExpr, &v, &isConst) != 0 || !isConst) {
                haveKnownSequence = 0;
            } else {
                nextValue = v;
                haveKnownSequence = 1;
            }
        }
        if (haveKnownSequence && nextValue == 0) {
            return 1;
        }
        if (haveKnownSequence) {
            nextValue++;
        }
        variant = SLAstNextSibling(c->ast, variant);
    }
    return 0;
}

int SLTCCasePatternParts(
    SLTypeCheckCtx* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode) {
    const SLAstNode* n = &c->ast->nodes[caseLabelNode];
    if (n->kind == SLAst_CASE_PATTERN) {
        int32_t expr = SLAstFirstChild(c->ast, caseLabelNode);
        int32_t alias = expr >= 0 ? SLAstNextSibling(c->ast, expr) : -1;
        if (expr < 0) {
            return SLTCFailNode(c, caseLabelNode, SLDiag_EXPECTED_EXPR);
        }
        *outExprNode = expr;
        *outAliasNode = alias;
        return 0;
    }
    *outExprNode = caseLabelNode;
    *outAliasNode = -1;
    return 0;
}

int SLTCDecodeVariantPatternExpr(
    SLTypeCheckCtx* c,
    int32_t         exprNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd) {
    const SLAstNode* n;
    int32_t          recvNode;
    const SLAstNode* recv;
    int32_t          namedIndex;
    int32_t          enumTypeId;
    int32_t          declNode;
    int32_t          ignoredType;
    n = &c->ast->nodes[exprNode];
    if (n->kind != SLAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = SLAstFirstChild(c->ast, exprNode);
    if (recvNode < 0) {
        return 0;
    }
    recv = &c->ast->nodes[recvNode];
    if (recv->kind != SLAst_IDENT) {
        return 0;
    }
    namedIndex = SLTCFindNamedTypeIndex(c, recv->dataStart, recv->dataEnd);
    if (namedIndex < 0) {
        return 0;
    }
    enumTypeId = c->namedTypes[(uint32_t)namedIndex].typeId;
    declNode = c->namedTypes[(uint32_t)namedIndex].declNode;
    if (declNode < 0 || (uint32_t)declNode >= c->ast->len
        || c->ast->nodes[declNode].kind != SLAst_ENUM)
    {
        return 0;
    }
    if (SLTCFieldLookup(c, enumTypeId, n->dataStart, n->dataEnd, &ignoredType, NULL) != 0) {
        return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
    }
    *outEnumType = enumTypeId;
    *outVariantStart = n->dataStart;
    *outVariantEnd = n->dataEnd;
    return 1;
}

int SLTCResolveEnumVariantTypeName(
    SLTypeCheckCtx* c,
    int32_t         typeNameNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd) {
    const SLAstNode* n = &c->ast->nodes[typeNameNode];
    uint32_t         dot = n->dataEnd;
    int32_t          namedIdx;
    int32_t          enumTypeId;
    int32_t          ignoredType;
    if (n->kind != SLAst_TYPE_NAME || n->dataEnd <= n->dataStart) {
        return 0;
    }
    while (dot > n->dataStart) {
        dot--;
        if (c->src.ptr[dot] == '.') {
            break;
        }
    }
    if (dot <= n->dataStart || c->src.ptr[dot] != '.' || dot + 1u >= n->dataEnd) {
        return 0;
    }
    namedIdx = SLTCFindNamedTypeIndex(c, n->dataStart, dot);
    if (namedIdx < 0) {
        return 0;
    }
    enumTypeId = c->namedTypes[(uint32_t)namedIdx].typeId;
    if (!SLTCIsNamedDeclKind(c, enumTypeId, SLAst_ENUM)) {
        return 0;
    }
    if (SLTCFieldLookup(c, enumTypeId, dot + 1u, n->dataEnd, &ignoredType, NULL) != 0) {
        return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, dot + 1u, n->dataEnd);
    }
    *outEnumType = enumTypeId;
    *outVariantStart = dot + 1u;
    *outVariantEnd = n->dataEnd;
    return 1;
}

int SLTCFieldLookup(
    SLTypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType,
    uint32_t* _Nullable outFieldIndex) {
    uint32_t depth = 0;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return -1;
    }
    if (c->types[typeId].kind == SLTCType_PTR || c->types[typeId].kind == SLTCType_REF) {
        typeId = c->types[typeId].baseType;
    }
    while (depth++ <= c->typeLen) {
        uint32_t i;
        int32_t  embedIdx = -1;
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen
            || (c->types[typeId].kind != SLTCType_NAMED
                && c->types[typeId].kind != SLTCType_ANON_STRUCT
                && c->types[typeId].kind != SLTCType_ANON_UNION))
        {
            return -1;
        }
        for (i = 0; i < c->types[typeId].fieldCount; i++) {
            uint32_t idx = c->types[typeId].fieldStart + i;
            if (SLNameEqSlice(
                    c->src, c->fields[idx].nameStart, c->fields[idx].nameEnd, fieldStart, fieldEnd))
            {
                *outType = c->fields[idx].typeId;
                if (outFieldIndex != NULL) {
                    *outFieldIndex = idx;
                }
                return 0;
            }
            if ((c->fields[idx].flags & SLTCFieldFlag_EMBEDDED) != 0) {
                embedIdx = (int32_t)idx;
            }
        }
        if (c->types[typeId].kind != SLTCType_NAMED) {
            return -1;
        }
        if (embedIdx < 0) {
            return -1;
        }
        typeId = c->fields[embedIdx].typeId;
        if (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == SLTCType_ALIAS)
        {
            typeId = SLTCResolveAliasBaseType(c, typeId);
            if (typeId < 0) {
                return -1;
            }
        }
    }
    return -1;
}

int SLTCIsAsciiSpace(unsigned char ch) {
    return ch == (unsigned char)' ' || ch == (unsigned char)'\t' || ch == (unsigned char)'\r'
        || ch == (unsigned char)'\n' || ch == (unsigned char)'\f' || ch == (unsigned char)'\v';
}

int SLTCIsIdentStartChar(unsigned char ch) {
    return (ch >= (unsigned char)'a' && ch <= (unsigned char)'z')
        || (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') || ch == (unsigned char)'_';
}

int SLTCIsIdentContinueChar(unsigned char ch) {
    return SLTCIsIdentStartChar(ch) || (ch >= (unsigned char)'0' && ch <= (unsigned char)'9');
}

int SLTCFieldPathNextSegment(
    SLTypeCheckCtx* c,
    uint32_t        pathStart,
    uint32_t        pathEnd,
    uint32_t*       ioPos,
    uint32_t*       outSegStart,
    uint32_t*       outSegEnd) {
    uint32_t pos = *ioPos;
    while (pos < pathEnd && SLTCIsAsciiSpace((unsigned char)c->src.ptr[pos])) {
        pos++;
    }
    if (pos >= pathEnd) {
        *ioPos = pos;
        return 1;
    }
    if (!SLTCIsIdentStartChar((unsigned char)c->src.ptr[pos])) {
        return -1;
    }
    *outSegStart = pos;
    pos++;
    while (pos < pathEnd && SLTCIsIdentContinueChar((unsigned char)c->src.ptr[pos])) {
        pos++;
    }
    *outSegEnd = pos;
    while (pos < pathEnd && SLTCIsAsciiSpace((unsigned char)c->src.ptr[pos])) {
        pos++;
    }
    if (pos < pathEnd) {
        if (c->src.ptr[pos] != '.') {
            return -1;
        }
        pos++;
    }
    *ioPos = pos;
    (void)pathStart;
    return 0;
}

int SLTCFieldLookupPath(
    SLTypeCheckCtx* c,
    int32_t         ownerTypeId,
    uint32_t        pathStart,
    uint32_t        pathEnd,
    int32_t*        outType) {
    uint32_t pos = pathStart;
    int32_t  curType = ownerTypeId;
    int32_t  fieldType = -1;
    int      hadSegment = 0;

    for (;;) {
        uint32_t segStart = 0;
        uint32_t segEnd = 0;
        int      rc = SLTCFieldPathNextSegment(c, pathStart, pathEnd, &pos, &segStart, &segEnd);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            return -1;
        }
        hadSegment = 1;
        if (SLTCFieldLookup(c, curType, segStart, segEnd, &fieldType, NULL) != 0) {
            return -1;
        }
        curType = fieldType;
    }

    if (!hadSegment) {
        return -1;
    }
    *outType = fieldType;
    return 0;
}

SL_API_END
