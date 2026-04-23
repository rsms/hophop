#include "internal.h"

H2_API_BEGIN

static int H2TCTypeContainsAnyParamRec(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    const int32_t*  paramTypes,
    uint16_t        paramCount,
    uint32_t        depth) {
    uint16_t i;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || depth > c->typeLen) {
        return 0;
    }
    for (i = 0; i < paramCount; i++) {
        if (paramTypes[i] == typeId) {
            return 1;
        }
    }
    switch (c->types[typeId].kind) {
        case H2TCType_PTR:
        case H2TCType_REF:
        case H2TCType_ARRAY:
        case H2TCType_SLICE:
        case H2TCType_OPTIONAL:
        case H2TCType_ALIAS:
            return H2TCTypeContainsAnyParamRec(
                c, c->types[typeId].baseType, paramTypes, paramCount, depth + 1u);
        case H2TCType_TUPLE:
        case H2TCType_PACK:
        case H2TCType_FUNCTION: {
            uint32_t j;
            if (c->types[typeId].kind == H2TCType_FUNCTION
                && H2TCTypeContainsAnyParamRec(
                    c, c->types[typeId].baseType, paramTypes, paramCount, depth + 1u))
            {
                return 1;
            }
            for (j = 0; j < c->types[typeId].fieldCount; j++) {
                if (H2TCTypeContainsAnyParamRec(
                        c,
                        c->funcParamTypes[c->types[typeId].fieldStart + j],
                        paramTypes,
                        paramCount,
                        depth + 1u))
                {
                    return 1;
                }
            }
            return 0;
        }
        case H2TCType_NAMED: {
            uint32_t ni;
            for (ni = 0; ni < c->namedTypeLen; ni++) {
                const H2TCNamedType* nt = &c->namedTypes[ni];
                uint16_t             j;
                if (nt->typeId != typeId) {
                    continue;
                }
                for (j = 0; j < nt->templateArgCount; j++) {
                    if (H2TCTypeContainsAnyParamRec(
                            c,
                            c->genericArgTypes[nt->templateArgStart + j],
                            paramTypes,
                            paramCount,
                            depth + 1u))
                    {
                        return 1;
                    }
                }
                break;
            }
            return 0;
        }
        default: return 0;
    }
}

int H2TCResolveNamedTypeFields(H2TypeCheckCtx* c, uint32_t namedIndex) {
    const H2TCNamedType* nt = &c->namedTypes[namedIndex];
    int32_t              declNode = nt->declNode;
    H2AstKind            kind = c->ast->nodes[declNode].kind;
    int32_t              child;
    H2TCField*           pendingFields = NULL;
    int32_t*             fieldNodes = NULL;
    int32_t*             fieldTypesForDefaults = NULL;
    uint32_t             pendingCap = 0;
    uint32_t             fieldCount = 0;
    uint32_t             fieldOrdinal = 0;
    int                  sawDependent = 0;
    int                  sawEmbedded = 0;
    uint32_t             savedActiveGenericArgStart = c->activeGenericArgStart;
    uint16_t             savedActiveGenericArgCount = c->activeGenericArgCount;
    int32_t              savedActiveGenericDeclNode = c->activeGenericDeclNode;
    int skipDefaultChecks = nt->templateArgCount > 0 && nt->templateRootNamedIndex < 0;

    c->activeGenericArgStart = nt->templateArgStart;
    c->activeGenericArgCount = nt->templateArgCount;
    c->activeGenericDeclNode = nt->templateArgCount > 0 ? declNode : -1;

    if (kind == H2Ast_TYPE_ALIAS) {
        c->activeGenericArgStart = savedActiveGenericArgStart;
        c->activeGenericArgCount = savedActiveGenericArgCount;
        c->activeGenericDeclNode = savedActiveGenericDeclNode;
        return 0;
    }

    child = H2AstFirstChild(c->ast, declNode);
    while (child >= 0 && c->ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
        child = H2AstNextSibling(c->ast, child);
    }
    if (kind == H2Ast_ENUM && child >= 0
        && (c->ast->nodes[child].kind == H2Ast_TYPE_NAME
            || c->ast->nodes[child].kind == H2Ast_TYPE_PTR
            || c->ast->nodes[child].kind == H2Ast_TYPE_ARRAY
            || c->ast->nodes[child].kind == H2Ast_TYPE_REF
            || c->ast->nodes[child].kind == H2Ast_TYPE_MUTREF
            || c->ast->nodes[child].kind == H2Ast_TYPE_SLICE
            || c->ast->nodes[child].kind == H2Ast_TYPE_MUTSLICE
            || c->ast->nodes[child].kind == H2Ast_TYPE_VARRAY
            || c->ast->nodes[child].kind == H2Ast_TYPE_FN
            || c->ast->nodes[child].kind == H2Ast_TYPE_ANON_STRUCT
            || c->ast->nodes[child].kind == H2Ast_TYPE_ANON_UNION
            || c->ast->nodes[child].kind == H2Ast_TYPE_TUPLE))
    {
        child = H2AstNextSibling(c->ast, child);
    }

    {
        int32_t scan = child;
        while (scan >= 0) {
            if (c->ast->nodes[scan].kind == H2Ast_FIELD
                && (kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM))
            {
                pendingCap++;
            }
            scan = H2AstNextSibling(c->ast, scan);
        }
        if (pendingCap > 0) {
            pendingFields = (H2TCField*)H2ArenaAlloc(
                c->arena, pendingCap * sizeof(H2TCField), (uint32_t)_Alignof(H2TCField));
            fieldNodes = (int32_t*)H2ArenaAlloc(
                c->arena, pendingCap * sizeof(int32_t), (uint32_t)_Alignof(int32_t));
            fieldTypesForDefaults = (int32_t*)H2ArenaAlloc(
                c->arena, pendingCap * sizeof(int32_t), (uint32_t)_Alignof(int32_t));
            if (pendingFields == NULL || fieldNodes == NULL || fieldTypesForDefaults == NULL) {
                return H2TCFailNode(c, declNode, H2Diag_ARENA_OOM);
            }
            scan = child;
            pendingCap = 0;
            while (scan >= 0) {
                if (c->ast->nodes[scan].kind == H2Ast_FIELD
                    && (kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM))
                {
                    fieldNodes[pendingCap] = scan;
                    fieldTypesForDefaults[pendingCap] = -1;
                    pendingCap++;
                }
                scan = H2AstNextSibling(c->ast, scan);
            }
        }
    }

    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_TYPE_PARAM) {
            child = H2AstNextSibling(c->ast, child);
            continue;
        }
        if (n->kind == H2Ast_FIELD
            && (kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM))
        {
            uint32_t currentFieldOrdinal = fieldOrdinal++;
            int32_t  typeNode = H2AstFirstChild(c->ast, child);
            int32_t  typeId;
            uint16_t fieldFlags = 0;
            uint32_t lenNameStart = 0;
            uint32_t lenNameEnd = 0;
            uint32_t i;
            int      isEmbedded = (n->flags & H2AstFlag_FIELD_EMBEDDED) != 0;
            if (kind == H2Ast_ENUM) {
                typeId = nt->typeId;
            } else {
                if (typeNode < 0) {
                    return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
                }

                if (isEmbedded) {
                    int32_t embeddedDeclNode;
                    int32_t embedType;
                    if (kind != H2Ast_STRUCT) {
                        return H2TCFailSpan(
                            c, H2Diag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                    }
                    if (sawEmbedded) {
                        return H2TCFailSpan(
                            c, H2Diag_MULTIPLE_EMBEDDED_FIELDS, n->dataStart, n->dataEnd);
                    }
                    if (fieldCount != 0) {
                        return H2TCFailSpan(
                            c, H2Diag_EMBEDDED_FIELD_NOT_FIRST, n->dataStart, n->dataEnd);
                    }
                    if (sawDependent) {
                        return H2TCFailNode(c, typeNode, H2Diag_TYPE_MISMATCH);
                    }
                    if (H2TCResolveTypeNode(c, typeNode, &typeId) != 0) {
                        return -1;
                    }
                    embedType = typeId;
                    if (embedType >= 0 && (uint32_t)embedType < c->typeLen
                        && c->types[embedType].kind == H2TCType_ALIAS)
                    {
                        embedType = H2TCResolveAliasBaseType(c, embedType);
                        if (embedType < 0) {
                            return -1;
                        }
                    }
                    if (embedType < 0 || (uint32_t)embedType >= c->typeLen) {
                        return H2TCFailSpan(
                            c, H2Diag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                    }
                    if (c->types[embedType].kind == H2TCType_NAMED) {
                        embeddedDeclNode = c->types[embedType].declNode;
                        if (embeddedDeclNode < 0 || (uint32_t)embeddedDeclNode >= c->ast->len
                            || c->ast->nodes[embeddedDeclNode].kind != H2Ast_STRUCT)
                        {
                            return H2TCFailSpan(
                                c, H2Diag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                        }
                    } else if (c->types[embedType].kind != H2TCType_ANON_STRUCT) {
                        return H2TCFailSpan(
                            c, H2Diag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                    }
                    typeId = embedType;
                    fieldFlags = H2TCFieldFlag_EMBEDDED;
                    sawEmbedded = 1;
                } else if (c->ast->nodes[typeNode].kind == H2Ast_TYPE_VARRAY) {
                    int32_t  elemTypeNode;
                    int32_t  elemType;
                    int32_t  ptrType;
                    int32_t  lenFieldType = -1;
                    uint32_t j;
                    if (kind != H2Ast_STRUCT) {
                        return H2TCFailNode(c, typeNode, H2Diag_TYPE_MISMATCH);
                    }
                    sawDependent = 1;
                    lenNameStart = c->ast->nodes[typeNode].dataStart;
                    lenNameEnd = c->ast->nodes[typeNode].dataEnd;

                    j = 0;
                    while (j < fieldCount) {
                        if ((pendingFields[j].flags & H2TCFieldFlag_DEPENDENT) == 0
                            && H2NameEqSlice(
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
                        return H2TCFailSpan(c, H2Diag_UNKNOWN_SYMBOL, lenNameStart, lenNameEnd);
                    }
                    if (!H2TCIsIntegerType(c, lenFieldType)) {
                        return H2TCFailSpan(c, H2Diag_TYPE_MISMATCH, lenNameStart, lenNameEnd);
                    }

                    elemTypeNode = H2AstFirstChild(c->ast, typeNode);
                    if (elemTypeNode < 0) {
                        return H2TCFailNode(c, typeNode, H2Diag_EXPECTED_TYPE);
                    }
                    if (H2TCResolveTypeNode(c, elemTypeNode, &elemType) != 0) {
                        return -1;
                    }
                    ptrType = H2TCInternPtrType(c, elemType, n->start, n->end);
                    if (ptrType < 0) {
                        return -1;
                    }
                    typeId = ptrType;
                    fieldFlags = H2TCFieldFlag_DEPENDENT;
                    c->types[nt->typeId].flags |= H2TCTypeFlag_VARSIZE;
                } else {
                    if (sawDependent) {
                        return H2TCFailNode(c, typeNode, H2Diag_TYPE_MISMATCH);
                    }
                    if (H2TCResolveTypeNode(c, typeNode, &typeId) != 0) {
                        return -1;
                    }
                }
            }

            if (fieldTypesForDefaults != NULL && currentFieldOrdinal < pendingCap) {
                fieldTypesForDefaults[currentFieldOrdinal] = typeId;
            }

            if (kind == H2Ast_STRUCT || kind == H2Ast_UNION) {
                int32_t dupTypeIndex = H2TCFindNamedTypeIndexOwned(
                    c, nt->typeId, n->dataStart, n->dataEnd);
                if (dupTypeIndex >= 0) {
                    const H2TCNamedType* dupType = &c->namedTypes[(uint32_t)dupTypeIndex];
                    if (dupType->nameStart > n->dataStart) {
                        return H2TCFailDuplicateDefinition(
                            c, dupType->nameStart, dupType->nameEnd, n->dataStart, n->dataEnd);
                    }
                    return H2TCFailDuplicateDefinition(
                        c, n->dataStart, n->dataEnd, dupType->nameStart, dupType->nameEnd);
                }
            }

            for (i = 0; i < fieldCount; i++) {
                if (H2NameEqSlice(
                        c->src,
                        pendingFields[i].nameStart,
                        pendingFields[i].nameEnd,
                        n->dataStart,
                        n->dataEnd))
                {
                    return H2TCFailDuplicateDefinition(
                        c,
                        n->dataStart,
                        n->dataEnd,
                        pendingFields[i].nameStart,
                        pendingFields[i].nameEnd);
                }
            }

            if (kind == H2Ast_STRUCT || kind == H2Ast_UNION) {
                int32_t defaultNode = typeNode >= 0 ? H2AstNextSibling(c->ast, typeNode) : -1;
                if (defaultNode >= 0 && !skipDefaultChecks) {
                    int32_t        defaultType;
                    const int32_t* savedDefaultFieldNodes = c->defaultFieldNodes;
                    const int32_t* savedDefaultFieldTypes = c->defaultFieldTypes;
                    uint32_t       savedDefaultFieldCount = c->defaultFieldCount;
                    uint32_t       savedDefaultFieldCurrentIndex = c->defaultFieldCurrentIndex;

                    if (kind != H2Ast_STRUCT) {
                        return H2TCFailNode(c, defaultNode, H2Diag_FIELD_DEFAULTS_STRUCT_ONLY);
                    }
                    c->defaultFieldNodes = fieldNodes;
                    c->defaultFieldTypes = fieldTypesForDefaults;
                    c->defaultFieldCount = pendingCap;
                    c->defaultFieldCurrentIndex = currentFieldOrdinal;
                    if (H2TCTypeExprExpected(c, defaultNode, typeId, &defaultType) != 0) {
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

                    if (!H2TCCanAssign(c, typeId, defaultType)) {
                        H2TCSetDiagWithArg(
                            c->diag,
                            H2Diag_FIELD_DEFAULT_TYPE_MISMATCH,
                            c->ast->nodes[defaultNode].start,
                            c->ast->nodes[defaultNode].end,
                            n->dataStart,
                            n->dataEnd);
                        return -1;
                    }
                }
            }

            if (fieldCount >= pendingCap) {
                return H2TCFailNode(c, child, H2Diag_ARENA_OOM);
            }
            pendingFields[fieldCount].nameStart = n->dataStart;
            pendingFields[fieldCount].nameEnd = n->dataEnd;
            pendingFields[fieldCount].typeId = typeId;
            pendingFields[fieldCount].lenNameStart = lenNameStart;
            pendingFields[fieldCount].lenNameEnd = lenNameEnd;
            pendingFields[fieldCount].flags = fieldFlags;
            fieldCount++;
        }
        child = H2AstNextSibling(c->ast, child);
    }

    {
        uint32_t fieldStart = c->fieldLen;
        uint32_t i;
        if (c->fieldLen + fieldCount > c->fieldCap) {
            return H2TCFailNode(c, declNode, H2Diag_ARENA_OOM);
        }
        for (i = 0; i < fieldCount; i++) {
            c->fields[c->fieldLen++] = pendingFields[i];
        }
        c->types[nt->typeId].fieldStart = fieldStart;
    }
    c->types[nt->typeId].fieldCount = (uint16_t)fieldCount;
    c->activeGenericArgStart = savedActiveGenericArgStart;
    c->activeGenericArgCount = savedActiveGenericArgCount;
    c->activeGenericDeclNode = savedActiveGenericDeclNode;
    return 0;
}

static int H2TCResolveNamedTypeInstanceFields(H2TypeCheckCtx* c, uint32_t namedIndex) {
    const H2TCNamedType* nt = &c->namedTypes[namedIndex];
    const H2TCNamedType* rootNt;
    const H2TCType*      rootType;
    uint16_t             i;
    uint32_t             fieldStart;
    if (nt->templateRootNamedIndex < 0) {
        return H2TCResolveNamedTypeFields(c, namedIndex);
    }
    rootNt = &c->namedTypes[(uint32_t)nt->templateRootNamedIndex];
    if (H2TCEnsureNamedTypeFieldsResolved(c, rootNt->typeId) != 0) {
        return -1;
    }
    rootType = &c->types[rootNt->typeId];
    if (c->fieldLen + rootType->fieldCount > c->fieldCap) {
        return H2TCFailNode(c, nt->declNode, H2Diag_ARENA_OOM);
    }
    fieldStart = c->fieldLen;
    for (i = 0; i < rootType->fieldCount; i++) {
        H2TCField field = c->fields[rootType->fieldStart + i];
        field.typeId = H2TCSubstituteType(
            c,
            field.typeId,
            &c->genericArgTypes[rootNt->templateArgStart],
            &c->genericArgTypes[nt->templateArgStart],
            nt->templateArgCount,
            field.nameStart,
            field.nameEnd);
        if (field.typeId < 0) {
            return -1;
        }
        c->fields[c->fieldLen++] = field;
    }
    c->types[nt->typeId].fieldStart = fieldStart;
    c->types[nt->typeId].fieldCount = rootType->fieldCount;
    if ((rootType->flags & H2TCTypeFlag_VARSIZE) != 0) {
        c->types[nt->typeId].flags |= H2TCTypeFlag_VARSIZE;
    }
    return 0;
}

int H2TCEnsureNamedTypeFieldsResolved(H2TypeCheckCtx* c, int32_t typeId) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        const H2TCNamedType* nt = &c->namedTypes[i];
        if (nt->typeId != typeId) {
            continue;
        }
        if (c->types[typeId].fieldCount == 0 && c->ast->nodes[nt->declNode].kind != H2Ast_TYPE_ALIAS
            && c->ast->nodes[nt->declNode].kind != H2Ast_ENUM)
        {
            return nt->templateRootNamedIndex >= 0
                     ? H2TCResolveNamedTypeInstanceFields(c, i)
                     : H2TCResolveNamedTypeFields(c, i);
        }
        return 0;
    }
    return 0;
}

int H2TCResolveAllNamedTypeFields(H2TypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        int32_t savedOwnerTypeId = c->currentTypeOwnerTypeId;
        c->currentTypeOwnerTypeId = c->namedTypes[i].typeId;
        if (c->namedTypes[i].templateRootNamedIndex >= 0) {
            c->currentTypeOwnerTypeId = savedOwnerTypeId;
            continue;
        }
        if (H2TCResolveNamedTypeFields(c, i) != 0) {
            c->currentTypeOwnerTypeId = savedOwnerTypeId;
            return -1;
        }
        c->currentTypeOwnerTypeId = savedOwnerTypeId;
    }
    for (i = 0; i < c->namedTypeLen; i++) {
        int32_t savedOwnerTypeId = c->currentTypeOwnerTypeId;
        c->currentTypeOwnerTypeId = c->namedTypes[i].typeId;
        if (c->namedTypes[i].templateRootNamedIndex < 0) {
            c->currentTypeOwnerTypeId = savedOwnerTypeId;
            continue;
        }
        if (H2TCResolveNamedTypeInstanceFields(c, i) != 0) {
            c->currentTypeOwnerTypeId = savedOwnerTypeId;
            return -1;
        }
        c->currentTypeOwnerTypeId = savedOwnerTypeId;
    }
    return 0;
}

int H2TCResolveAllTypeAliases(H2TypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        int32_t  typeId = c->namedTypes[i].typeId;
        int32_t  savedOwnerTypeId = c->currentTypeOwnerTypeId;
        uint32_t savedActiveGenericArgStart = c->activeGenericArgStart;
        uint16_t savedActiveGenericArgCount = c->activeGenericArgCount;
        int32_t  savedActiveGenericDeclNode = c->activeGenericDeclNode;
        c->currentTypeOwnerTypeId = c->namedTypes[i].ownerTypeId;
        c->activeGenericArgStart = c->namedTypes[i].templateArgStart;
        c->activeGenericArgCount = c->namedTypes[i].templateArgCount;
        c->activeGenericDeclNode =
            c->namedTypes[i].templateArgCount > 0 ? c->namedTypes[i].declNode : -1;
        if (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == H2TCType_ALIAS
            && H2TCResolveAliasTypeId(c, typeId) != 0)
        {
            c->currentTypeOwnerTypeId = savedOwnerTypeId;
            c->activeGenericArgStart = savedActiveGenericArgStart;
            c->activeGenericArgCount = savedActiveGenericArgCount;
            c->activeGenericDeclNode = savedActiveGenericDeclNode;
            return -1;
        }
        c->currentTypeOwnerTypeId = savedOwnerTypeId;
        c->activeGenericArgStart = savedActiveGenericArgStart;
        c->activeGenericArgCount = savedActiveGenericArgCount;
        c->activeGenericDeclNode = savedActiveGenericDeclNode;
    }
    return 0;
}

int H2TCCheckEmbeddedCycleFrom(H2TypeCheckCtx* c, int32_t typeId) {
    H2TCType* type;
    int32_t   embedIdx;
    int32_t   baseType;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != H2TCType_NAMED) {
        return 0;
    }
    type = &c->types[typeId];
    if ((type->flags & H2TCTypeFlag_VISITING) != 0) {
        return 0;
    }
    if ((type->flags & H2TCTypeFlag_VISITED) != 0) {
        return 0;
    }
    type->flags |= H2TCTypeFlag_VISITING;

    embedIdx = H2TCFindEmbeddedFieldIndex(c, typeId);
    if (embedIdx >= 0) {
        baseType = c->fields[embedIdx].typeId;
        if (baseType >= 0 && (uint32_t)baseType < c->typeLen
            && c->types[baseType].kind == H2TCType_ALIAS)
        {
            baseType = H2TCResolveAliasBaseType(c, baseType);
            if (baseType < 0) {
                return -1;
            }
        }
        if (baseType >= 0 && (uint32_t)baseType < c->typeLen
            && c->types[baseType].kind == H2TCType_NAMED)
        {
            if ((c->types[baseType].flags & H2TCTypeFlag_VISITING) != 0) {
                return H2TCFailSpan(
                    c,
                    H2Diag_EMBEDDED_CYCLE,
                    c->fields[embedIdx].nameStart,
                    c->fields[embedIdx].nameEnd);
            }
            if (H2TCCheckEmbeddedCycleFrom(c, baseType) != 0) {
                return -1;
            }
        }
    }

    type->flags &= (uint16_t)~H2TCTypeFlag_VISITING;
    type->flags |= H2TCTypeFlag_VISITED;
    return 0;
}

int H2TCCheckEmbeddedCycles(H2TypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        int32_t typeId = c->namedTypes[i].typeId;
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
            continue;
        }
        if (H2TCCheckEmbeddedCycleFrom(c, typeId) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->typeLen; i++) {
        c->types[i].flags &= (uint16_t)~(H2TCTypeFlag_VISITING | H2TCTypeFlag_VISITED);
    }
    return 0;
}

int H2TCPropagateVarSizeNamedTypes(H2TypeCheckCtx* c) {
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
            if ((c->types[typeId].flags & H2TCTypeFlag_VARSIZE) != 0) {
                continue;
            }
            for (j = 0; j < c->types[typeId].fieldCount; j++) {
                uint32_t        fieldIdx = c->types[typeId].fieldStart + j;
                const H2TCField field = c->fields[fieldIdx];
                if ((field.flags & H2TCFieldFlag_DEPENDENT) != 0
                    || H2TCTypeContainsVarSizeByValue(c, field.typeId))
                {
                    c->types[typeId].flags |= H2TCTypeFlag_VARSIZE;
                    changed = 1;
                    break;
                }
            }
        }
    }
    return 0;
}

int H2TCReadFunctionSig(
    H2TypeCheckCtx* c,
    int32_t         funNode,
    int32_t*        outReturnType,
    uint32_t*       outParamCount,
    int*            outIsVariadic,
    int32_t*        outContextType,
    int*            outHasBody) {
    int32_t  child = H2AstFirstChild(c->ast, funNode);
    uint32_t paramCount = 0;
    int32_t  returnType = c->typeVoid;
    int32_t  contextType = -1;
    int      isVariadic = 0;
    int      hasBody = 0;
    int32_t  savedParamTypes[H2TC_MAX_CALL_ARGS];
    uint8_t  savedParamFlags[H2TC_MAX_CALL_ARGS];

    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_TYPE_PARAM) {
            child = H2AstNextSibling(c->ast, child);
            continue;
        }
        if (n->kind == H2Ast_PARAM) {
            int32_t typeNode = H2AstFirstChild(c->ast, child);
            int32_t typeId;
            if (paramCount >= c->scratchParamCap) {
                return H2TCFailNode(c, child, H2Diag_ARENA_OOM);
            }
            c->allowAnytypeParamType = 1;
            c->allowConstNumericTypeName = 1;
            if (H2TCResolveTypeNode(c, typeNode, &typeId) != 0) {
                c->allowAnytypeParamType = 0;
                c->allowConstNumericTypeName = 0;
                return -1;
            }
            c->allowAnytypeParamType = 0;
            c->allowConstNumericTypeName = 0;
            if ((typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat)
                && (n->flags & H2AstFlag_PARAM_CONST) == 0)
            {
                return H2TCFailSpan(
                    c, H2Diag_CONST_NUMERIC_PARAM_REQUIRES_CONST, n->dataStart, n->dataEnd);
            }
            if (H2TCTypeContainsVarSizeByValue(c, typeId)) {
                return H2TCFailVarSizeByValue(c, typeNode, typeId, "parameter position");
            }
            if ((n->flags & H2AstFlag_PARAM_VARIADIC) != 0) {
                int32_t sliceType;
                if (typeId != c->typeAnytype) {
                    sliceType = H2TCInternSliceType(c, typeId, 0, n->start, n->end);
                    if (sliceType < 0) {
                        return -1;
                    }
                    typeId = sliceType;
                }
                isVariadic = 1;
            }
            c->scratchParamTypes[paramCount++] = typeId;
            c->scratchParamFlags[paramCount - 1u] =
                (n->flags & H2AstFlag_PARAM_CONST) != 0 ? H2TCFuncParamFlag_CONST : 0u;
        } else if (
            (n->kind == H2Ast_TYPE_NAME || n->kind == H2Ast_TYPE_PTR || n->kind == H2Ast_TYPE_ARRAY
             || n->kind == H2Ast_TYPE_REF || n->kind == H2Ast_TYPE_MUTREF
             || n->kind == H2Ast_TYPE_SLICE || n->kind == H2Ast_TYPE_MUTSLICE
             || n->kind == H2Ast_TYPE_VARRAY || n->kind == H2Ast_TYPE_OPTIONAL
             || n->kind == H2Ast_TYPE_FN || n->kind == H2Ast_TYPE_ANON_STRUCT
             || n->kind == H2Ast_TYPE_ANON_UNION || n->kind == H2Ast_TYPE_TUPLE)
            && n->flags == 1)
        {
            uint32_t i;
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
                return H2TCFailVarSizeByValue(c, child, returnType, "return position");
            }
        } else if (n->kind == H2Ast_CONTEXT_CLAUSE) {
            int32_t typeNode = H2AstFirstChild(c->ast, child);
            if (contextType >= 0) {
                return H2TCFailNode(c, child, H2Diag_UNEXPECTED_TOKEN);
            }
            if (typeNode < 0) {
                return H2TCFailNode(c, child, H2Diag_EXPECTED_TYPE);
            }
            if (H2TCResolveTypeNode(c, typeNode, &contextType) != 0) {
                return -1;
            }
        } else if (n->kind == H2Ast_BLOCK) {
            hasBody = 1;
        }
        child = H2AstNextSibling(c->ast, child);
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

static int H2TCFunctionIdentityMatchesScratch(
    H2TypeCheckCtx*     c,
    const H2TCFunction* f,
    uint32_t            paramCount,
    int32_t             returnType,
    int32_t             contextType,
    int                 isVariadic) {
    uint32_t p;
    if (f->paramCount != paramCount || f->returnType != returnType
        || (((f->flags & H2TCFunctionFlag_VARIADIC) != 0) != (isVariadic != 0))
        || f->contextType != contextType)
    {
        return 0;
    }
    for (p = 0; p < paramCount; p++) {
        if (c->funcParamTypes[f->paramTypeStart + p] != c->scratchParamTypes[p]) {
            return 0;
        }
        if ((c->funcParamFlags[f->paramTypeStart + p] & H2TCFuncParamFlag_CONST)
            != (c->scratchParamFlags[p] & H2TCFuncParamFlag_CONST))
        {
            return 0;
        }
    }
    return 1;
}

static int H2TCFunctionNameIsBuiltinQualifiedSlice(
    H2TypeCheckCtx* c, const H2TCFunction* f, uint32_t start, uint32_t end) {
    uint32_t nameLen;
    uint32_t candLen;
    if (end <= start || end > c->src.len || f->nameEnd <= f->nameStart) {
        return 0;
    }
    nameLen = end - start;
    candLen = f->nameEnd - f->nameStart;
    if (candLen != 9u + nameLen) {
        return 0;
    }
    if (memcmp(c->src.ptr + f->nameStart, "builtin__", 9u) != 0) {
        return 0;
    }
    return memcmp(c->src.ptr + f->nameStart + 9u, c->src.ptr + start, nameLen) == 0;
}

int H2TCCollectFunctionFromNode(H2TypeCheckCtx* c, int32_t nodeId) {
    const H2AstNode* n = &c->ast->nodes[nodeId];
    int32_t          returnType = -1;
    int32_t          contextType = -1;
    uint32_t         paramCount = 0;
    uint32_t         genericArgStart = c->genericArgLen;
    uint16_t         genericArgCount = 0;
    int              isVariadic = 0;
    int              hasAnytype = 0;
    int              hasAnyPack = 0;
    int              hasBody = 0;
    int32_t          savedActiveTypeParamFnNode = c->activeTypeParamFnNode;
    uint32_t         savedActiveGenericArgStart = c->activeGenericArgStart;
    uint16_t         savedActiveGenericArgCount = c->activeGenericArgCount;
    int32_t          savedActiveGenericDeclNode = c->activeGenericDeclNode;

    if (n->kind == H2Ast_PUB) {
        int32_t ch = H2AstFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (H2TCCollectFunctionFromNode(c, ch) != 0) {
                return -1;
            }
            ch = H2AstNextSibling(c->ast, ch);
        }
        return 0;
    }

    if (n->kind != H2Ast_FN) {
        return 0;
    }

    if (H2TCAppendDeclTypeParamPlaceholders(c, nodeId, &genericArgStart, &genericArgCount) != 0) {
        return -1;
    }
    c->activeTypeParamFnNode = nodeId;
    c->activeGenericArgStart = genericArgStart;
    c->activeGenericArgCount = genericArgCount;
    c->activeGenericDeclNode = genericArgCount > 0 ? nodeId : -1;
    if (H2TCReadFunctionSig(
            c, nodeId, &returnType, &paramCount, &isVariadic, &contextType, &hasBody)
        != 0)
    {
        c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
        c->activeGenericArgStart = savedActiveGenericArgStart;
        c->activeGenericArgCount = savedActiveGenericArgCount;
        c->activeGenericDeclNode = savedActiveGenericDeclNode;
        return -1;
    }
    c->activeTypeParamFnNode = savedActiveTypeParamFnNode;
    c->activeGenericArgStart = savedActiveGenericArgStart;
    c->activeGenericArgCount = savedActiveGenericArgCount;
    c->activeGenericDeclNode = savedActiveGenericDeclNode;
    {
        uint32_t p;
        for (p = 0; p < paramCount; p++) {
            if (c->scratchParamTypes[p] == c->typeAnytype) {
                hasAnytype = 1;
                if (isVariadic && p + 1u == paramCount) {
                    hasAnyPack = 1;
                }
            }
        }
    }
    if (contextType >= 0 && H2NameEqLiteral(c->src, n->dataStart, n->dataEnd, "main")) {
        return H2TCFailSpan(c, H2Diag_UNEXPECTED_TOKEN, n->start, n->end);
    }
    {
        int isEqualHook = 0;
        int intType = -1;
        if (H2TCIsComparisonHookName(c, n->dataStart, n->dataEnd, &isEqualHook)) {
            if (paramCount != 2 || contextType >= 0) {
                return H2TCFailNode(c, nodeId, H2Diag_INVALID_COMPARISON_HOOK_SIGNATURE);
            }
            intType = H2TCFindBuiltinByKind(c, H2Builtin_ISIZE);
            if ((isEqualHook && returnType != c->typeBool)
                || (!isEqualHook && returnType != intType))
            {
                return H2TCFailNode(c, nodeId, H2Diag_INVALID_COMPARISON_HOOK_SIGNATURE);
            }
            if (!H2TCCanAssign(c, c->scratchParamTypes[0], c->scratchParamTypes[1])) {
                return H2TCFailNode(c, nodeId, H2Diag_INVALID_COMPARISON_HOOK_SIGNATURE);
            }
        }
    }
    if (genericArgCount > 0 && paramCount == 0
        && H2TCTypeContainsAnyParamRec(
            c, returnType, &c->genericArgTypes[genericArgStart], genericArgCount, 0u))
    {
        return H2TCFailNode(c, nodeId, H2Diag_GENERIC_FN_ZERO_ARG_RETURN_FORBIDDEN);
    }

    {
        uint32_t i;
        for (i = 0; i < c->funcLen; i++) {
            H2TCFunction* f = &c->funcs[i];
            if (!H2TCFunctionNameEq(c, i, n->dataStart, n->dataEnd)) {
                continue;
            }
            if (f->paramCount != paramCount || f->returnType != returnType
                || (((f->flags & H2TCFunctionFlag_VARIADIC) != 0) != (isVariadic != 0)))
            {
                continue;
            }
            if (f->contextType != contextType) {
                return H2TCFailSpan(c, H2Diag_CONTEXT_CLAUSE_MISMATCH, n->start, n->end);
            }
            if (!H2TCFunctionIdentityMatchesScratch(
                    c, f, paramCount, returnType, contextType, isVariadic))
            {
                continue;
            }
            if (hasBody) {
                if (f->defNode >= 0) {
                    const H2AstNode* defNode = &c->ast->nodes[f->defNode];
                    return H2TCFailDuplicateDefinition(
                        c, n->dataStart, n->dataEnd, defNode->dataStart, defNode->dataEnd);
                }
                f->defNode = nodeId;
            }
            return 0;
        }
    }

    {
        uint32_t i;
        for (i = 0; i < c->funcLen; i++) {
            H2TCFunction* f = &c->funcs[i];
            if (!H2TCFunctionNameIsBuiltinQualifiedSlice(c, f, n->dataStart, n->dataEnd)) {
                continue;
            }
            if (H2TCFunctionIdentityMatchesScratch(
                    c, f, paramCount, returnType, contextType, isVariadic))
            {
                return H2TCFailDuplicateDefinition(
                    c, n->dataStart, n->dataEnd, f->nameStart, f->nameEnd);
            }
        }
    }

    if (c->funcLen >= c->funcCap || c->funcParamLen + paramCount > c->funcParamCap) {
        return H2TCFailNode(c, nodeId, H2Diag_ARENA_OOM);
    }

    {
        uint32_t      idx = c->funcLen++;
        H2TCFunction* f = &c->funcs[idx];
        int32_t       child = H2AstFirstChild(c->ast, nodeId);
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
        f->templateArgStart = genericArgStart;
        f->templateArgCount = genericArgCount;
        f->templateRootFuncIndex = -1;
        f->flags = 0;
        if (isVariadic) {
            f->flags |= H2TCFunctionFlag_VARIADIC;
        }
        if (hasAnytype || genericArgCount > 0) {
            f->flags |= H2TCFunctionFlag_TEMPLATE;
        }
        if (hasAnyPack) {
            f->flags |= H2TCFunctionFlag_TEMPLATE_HAS_ANYPACK;
        }
        while (child >= 0) {
            const H2AstNode* ch = &c->ast->nodes[child];
            if (ch->kind == H2Ast_PARAM) {
                if (paramIndex >= paramCount) {
                    return H2TCFailNode(c, nodeId, H2Diag_ARENA_OOM);
                }
                c->funcParamTypes[c->funcParamLen] = c->scratchParamTypes[paramIndex];
                c->funcParamNameStarts[c->funcParamLen] = ch->dataStart;
                c->funcParamNameEnds[c->funcParamLen] = ch->dataEnd;
                c->funcParamFlags[c->funcParamLen] =
                    c->scratchParamFlags[paramIndex] & H2TCFuncParamFlag_CONST;
                c->funcParamLen++;
                paramIndex++;
            }
            child = H2AstNextSibling(c->ast, child);
        }
        if (paramIndex != paramCount) {
            return H2TCFailNode(c, nodeId, H2Diag_ARENA_OOM);
        }
    }

    return 0;
}

int H2TCFinalizeFunctionTypes(H2TypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        int32_t typeId = H2TCInternFunctionType(
            c,
            c->funcs[i].returnType,
            &c->funcParamTypes[c->funcs[i].paramTypeStart],
            &c->funcParamFlags[c->funcs[i].paramTypeStart],
            c->funcs[i].paramCount,
            (c->funcs[i].flags & H2TCFunctionFlag_VARIADIC) != 0,
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

int32_t H2TCFindTopLevelVarLikeNode(
    H2TypeCheckCtx* c, uint32_t start, uint32_t end, int32_t* outNameIndex) {
    int32_t child = H2AstFirstChild(c->ast, c->ast->root);
    if (outNameIndex != NULL) {
        *outNameIndex = -1;
    }
    while (child >= 0) {
        const H2AstNode* n = &c->ast->nodes[child];
        if (n->kind == H2Ast_VAR || n->kind == H2Ast_CONST) {
            int32_t nameIndex = H2TCVarLikeNameIndexBySlice(c, child, start, end);
            if (nameIndex >= 0) {
                if (outNameIndex != NULL) {
                    *outNameIndex = nameIndex;
                }
                return child;
            }
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return -1;
}

int H2TCTypeTopLevelVarLikeNode(
    H2TypeCheckCtx* c, int32_t nodeId, int32_t nameIndex, int32_t* outType) {
    uint8_t          state;
    H2TCVarLikeParts parts;
    int32_t          resolvedType;
    int32_t          initNode;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }
    if (c->topVarLikeTypeState == NULL || c->topVarLikeTypes == NULL) {
        return H2TCFailNode(c, nodeId, H2Diag_ARENA_OOM);
    }
    if (H2TCVarLikeGetParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_TYPE);
    }
    if (nameIndex < 0 || (uint32_t)nameIndex >= parts.nameCount) {
        return H2TCFailNode(c, nodeId, H2Diag_UNKNOWN_SYMBOL);
    }
    state = c->topVarLikeTypeState[nodeId];
    if (state == H2TCTopVarLikeType_READY) {
        *outType = c->topVarLikeTypes[nodeId];
        return 0;
    }
    if (state == H2TCTopVarLikeType_VISITING) {
        int rc;
        if (c->ast->nodes[nodeId].kind == H2Ast_CONST) {
            rc = H2TCFailNode(c, nodeId, H2Diag_CONST_INIT_CONST_REQUIRED);
            c->diag->detail = H2TCAllocDiagText(c, "cyclic top-level initializer dependency");
            return rc;
        }
        rc = H2TCFailNode(c, nodeId, H2Diag_TYPE_MISMATCH);
        c->diag->detail = H2TCAllocDiagText(c, "cyclic top-level initializer dependency");
        return rc;
    }

    c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_VISITING;
    if (parts.typeNode >= 0) {
        initNode = parts.initNode;
        if (c->ast->nodes[nodeId].kind == H2Ast_CONST && initNode < 0
            && !H2TCHasForeignImportDirective(c->ast, c->src, nodeId))
        {
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            return H2TCFailNode(c, nodeId, H2Diag_CONST_MISSING_INITIALIZER);
        }
        c->allowConstNumericTypeName = c->ast->nodes[nodeId].kind == H2Ast_CONST ? 1u : 0u;
        if (H2TCResolveTypeNode(c, parts.typeNode, &resolvedType) != 0) {
            c->allowConstNumericTypeName = 0;
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            return -1;
        }
        c->allowConstNumericTypeName = 0;
        if (c->ast->nodes[nodeId].kind == H2Ast_VAR && initNode < 0
            && !H2TCEnumTypeHasTagZero(c, resolvedType))
        {
            char        typeBuf[H2TC_DIAG_TEXT_CAP];
            char        detailBuf[256];
            H2TCTextBuf typeText;
            H2TCTextBuf detailText;
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            H2TCTextBufInit(&typeText, typeBuf, (uint32_t)sizeof(typeBuf));
            H2TCFormatTypeRec(c, resolvedType, &typeText, 0);
            H2TCTextBufInit(&detailText, detailBuf, (uint32_t)sizeof(detailBuf));
            H2TCTextBufAppendCStr(&detailText, "enum type ");
            H2TCTextBufAppendCStr(&detailText, typeBuf);
            H2TCTextBufAppendCStr(&detailText, " has no zero-valued tag; initializer required");
            return H2TCFailDiagText(c, nodeId, H2Diag_ENUM_ZERO_INIT_REQUIRES_ZERO_TAG, detailBuf);
        }
        if (c->ast->nodes[nodeId].kind == H2Ast_VAR && initNode < 0
            && H2TCTypeIsTrackedPtrRef(c, resolvedType))
        {
            uint32_t nameStart = c->ast->nodes[nodeId].dataStart;
            uint32_t nameEnd = c->ast->nodes[nodeId].dataEnd;
            if (parts.grouped && parts.nameListNode >= 0) {
                int32_t nameNode = H2TCListItemAt(c->ast, parts.nameListNode, (uint32_t)nameIndex);
                if (nameNode >= 0) {
                    nameStart = c->ast->nodes[nameNode].dataStart;
                    nameEnd = c->ast->nodes[nameNode].dataEnd;
                }
            }
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            return H2TCFailTopLevelPtrRefMissingInitializer(
                c, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end, nameStart, nameEnd);
        }
        c->topVarLikeTypes[nodeId] = resolvedType;
        c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_READY;
        *outType = resolvedType;
        return 0;
    }

    initNode = H2TCVarLikeInitExprNodeAt(c, nodeId, nameIndex);
    if (initNode < 0) {
        c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
        return H2TCFailNode(c, nodeId, H2Diag_EXPECTED_EXPR);
    }
    {
        int32_t initType;
        if (H2TCTypeExpr(c, initNode, &initType) != 0) {
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            return -1;
        }
        if (initType == c->typeNull) {
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            return H2TCFailNode(c, initNode, H2Diag_INFER_NULL_TYPE_UNKNOWN);
        }
        if (initType == c->typeVoid) {
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            return H2TCFailNode(c, initNode, H2Diag_INFER_VOID_TYPE_UNKNOWN);
        }
        if (c->ast->nodes[nodeId].kind == H2Ast_CONST) {
            resolvedType = initType;
        } else {
            if (H2TCConcretizeInferredType(c, initType, &resolvedType) != 0) {
                c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
                return -1;
            }
        }
        if (H2TCTypeContainsVarSizeByValue(c, resolvedType)) {
            c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
            return H2TCFailVarSizeByValue(c, initNode, resolvedType, "variable position");
        }
    }
    if (!parts.grouped) {
        c->topVarLikeTypes[nodeId] = resolvedType;
        c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_READY;
    } else {
        c->topVarLikeTypeState[nodeId] = H2TCTopVarLikeType_UNSEEN;
    }
    *outType = resolvedType;
    return 0;
}

int32_t H2TCLocalFind(H2TypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (H2NameEqSlice(c->src, c->locals[i].nameStart, c->locals[i].nameEnd, nameStart, nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

int H2TCLocalAdd(
    H2TypeCheckCtx* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    int32_t         typeId,
    int             isConst,
    int32_t         initExprNode) {
    uint32_t localIdx;
    uint32_t useIdx;
    if (c->currentFunctionIndex >= 0 && H2NameEqLiteral(c->src, nameStart, nameEnd, "context")) {
        return H2TCFailSpan(c, H2Diag_RESERVED_NAME, nameStart, nameEnd);
    }
    if (c->localLen >= c->localCap || c->localUseLen >= c->localUseCap || c->localUses == NULL) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, nameStart, nameEnd);
    }
    localIdx = c->localLen;
    useIdx = c->localUseLen;
    c->locals[localIdx].nameStart = nameStart;
    c->locals[localIdx].nameEnd = nameEnd;
    c->locals[localIdx].typeId = typeId;
    c->locals[localIdx].initExprNode = initExprNode;
    c->locals[localIdx].flags = isConst ? H2TCLocalFlag_CONST : 0;
    if (H2TCTypeIsTrackedPtrRef(c, typeId)) {
        c->locals[localIdx].initState =
            initExprNode >= 0 ? H2TCLocalInit_INIT : H2TCLocalInit_UNINIT;
    } else {
        c->locals[localIdx].initState = H2TCLocalInit_UNTRACKED;
    }
    c->locals[localIdx]._reserved = 0;
    c->locals[localIdx].useIndex = useIdx;
    c->localUses[useIdx].nameStart = nameStart;
    c->localUses[useIdx].nameEnd = nameEnd;
    c->localUses[useIdx].ownerFnIndex = c->currentFunctionIndex;
    c->localUses[useIdx].readCount = 0;
    c->localUses[useIdx].writeCount = 0;
    c->localUses[useIdx].kind = H2TCLocalUseKind_LOCAL;
    c->localUses[useIdx].suppressWarning = 0;
    c->localUses[useIdx]._reserved = 0;
    c->localLen++;
    c->localUseLen++;
    return 0;
}

int H2TCVariantNarrowPush(
    H2TypeCheckCtx* c,
    int32_t         localIdx,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd) {
    if (c->variantNarrowLen >= c->variantNarrowCap) {
        return H2TCFailSpan(c, H2Diag_ARENA_OOM, variantStart, variantEnd);
    }
    c->variantNarrows[c->variantNarrowLen].localIdx = localIdx;
    c->variantNarrows[c->variantNarrowLen].enumTypeId = enumTypeId;
    c->variantNarrows[c->variantNarrowLen].variantStart = variantStart;
    c->variantNarrows[c->variantNarrowLen].variantEnd = variantEnd;
    c->variantNarrowLen++;
    return 0;
}

int H2TCVariantNarrowFind(
    H2TypeCheckCtx* c, int32_t localIdx, const H2TCVariantNarrow** outNarrow) {
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

int32_t H2TCEnumDeclFirstVariantNode(H2TypeCheckCtx* c, int32_t enumDeclNode) {
    int32_t child = H2AstFirstChild(c->ast, enumDeclNode);
    while (child >= 0 && c->ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
        child = H2AstNextSibling(c->ast, child);
    }
    if (child >= 0 && H2TCIsTypeNodeKind(c->ast->nodes[child].kind)) {
        child = H2AstNextSibling(c->ast, child);
    }
    return child;
}

int32_t H2TCEnumVariantTagExprNode(H2TypeCheckCtx* c, int32_t variantNode) {
    int32_t child = H2AstFirstChild(c->ast, variantNode);
    while (child >= 0) {
        if (c->ast->nodes[child].kind != H2Ast_FIELD) {
            return child;
        }
        child = H2AstNextSibling(c->ast, child);
    }
    return -1;
}

int32_t H2TCFindEnumVariantNodeByName(
    H2TypeCheckCtx* c, int32_t enumTypeId, uint32_t variantStart, uint32_t variantEnd) {
    int32_t declNode;
    int32_t variant;
    if (enumTypeId < 0 || (uint32_t)enumTypeId >= c->typeLen) {
        return -1;
    }
    declNode = c->types[enumTypeId].declNode;
    if (declNode < 0 || (uint32_t)declNode >= c->ast->len
        || c->ast->nodes[declNode].kind != H2Ast_ENUM)
    {
        return -1;
    }
    variant = H2TCEnumDeclFirstVariantNode(c, declNode);
    while (variant >= 0) {
        const H2AstNode* vn = &c->ast->nodes[variant];
        if (vn->kind == H2Ast_FIELD
            && H2NameEqSlice(c->src, vn->dataStart, vn->dataEnd, variantStart, variantEnd))
        {
            return variant;
        }
        variant = H2AstNextSibling(c->ast, variant);
    }
    return -1;
}

int H2TCEnumVariantPayloadFieldType(
    H2TypeCheckCtx* c,
    int32_t         enumTypeId,
    uint32_t        variantStart,
    uint32_t        variantEnd,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType) {
    int32_t variantNode = H2TCFindEnumVariantNodeByName(c, enumTypeId, variantStart, variantEnd);
    int32_t ch;
    if (variantNode < 0) {
        return -1;
    }
    ch = H2AstFirstChild(c->ast, variantNode);
    while (ch >= 0) {
        const H2AstNode* fn = &c->ast->nodes[ch];
        int32_t          typeNode;
        int32_t          typeId;
        if (fn->kind != H2Ast_FIELD) {
            break;
        }
        if (!H2NameEqSlice(c->src, fn->dataStart, fn->dataEnd, fieldStart, fieldEnd)) {
            ch = H2AstNextSibling(c->ast, ch);
            continue;
        }
        typeNode = H2AstFirstChild(c->ast, ch);
        if (typeNode < 0) {
            return -1;
        }
        if (H2TCResolveTypeNode(c, typeNode, &typeId) != 0) {
            return -1;
        }
        *outType = typeId;
        return 0;
    }
    return -1;
}

int H2TCEnumTypeHasTagZero(H2TypeCheckCtx* c, int32_t enumTypeId) {
    int32_t declNode;
    int32_t variant;
    int64_t nextValue = 0;
    int     haveKnownSequence = 1;
    if (!H2TCIsNamedDeclKind(c, enumTypeId, H2Ast_ENUM)) {
        return 1;
    }
    declNode = c->types[H2TCResolveAliasBaseType(c, enumTypeId)].declNode;
    if (declNode < 0 || (uint32_t)declNode >= c->ast->len) {
        return 1;
    }
    variant = H2TCEnumDeclFirstVariantNode(c, declNode);
    while (variant >= 0) {
        int32_t initExpr = H2TCEnumVariantTagExprNode(c, variant);
        int64_t v = 0;
        int     isConst = 0;
        if (initExpr >= 0) {
            if (H2TCConstIntExpr(c, initExpr, &v, &isConst) != 0 || !isConst) {
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
        variant = H2AstNextSibling(c->ast, variant);
    }
    return 0;
}

int H2TCCasePatternParts(
    H2TypeCheckCtx* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode) {
    const H2AstNode* n = &c->ast->nodes[caseLabelNode];
    if (n->kind == H2Ast_CASE_PATTERN) {
        int32_t expr = H2AstFirstChild(c->ast, caseLabelNode);
        int32_t alias = expr >= 0 ? H2AstNextSibling(c->ast, expr) : -1;
        if (expr < 0) {
            return H2TCFailNode(c, caseLabelNode, H2Diag_EXPECTED_EXPR);
        }
        *outExprNode = expr;
        *outAliasNode = alias;
        return 0;
    }
    *outExprNode = caseLabelNode;
    *outAliasNode = -1;
    return 0;
}

static int H2TCResolveTypePathExprType(
    H2TypeCheckCtx* c, int32_t exprNode, int32_t ownerTypeId, int32_t* outTypeId) {
    const H2AstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == H2Ast_IDENT) {
        int32_t typeId = H2TCResolveTypeNamePath(c, n->dataStart, n->dataEnd, ownerTypeId);
        if (typeId < 0) {
            return 0;
        }
        *outTypeId = typeId;
        return 1;
    }
    if (n->kind == H2Ast_FIELD_EXPR) {
        int32_t recvNode = H2AstFirstChild(c->ast, exprNode);
        int32_t recvTypeId;
        int32_t childIdx;
        if (recvNode < 0 || !H2TCResolveTypePathExprType(c, recvNode, ownerTypeId, &recvTypeId)) {
            return 0;
        }
        childIdx = H2TCFindNamedTypeIndexOwned(c, recvTypeId, n->dataStart, n->dataEnd);
        if (childIdx < 0) {
            return 0;
        }
        *outTypeId = c->namedTypes[(uint32_t)childIdx].typeId;
        return 1;
    }
    return 0;
}

int H2TCDecodeVariantPatternExpr(
    H2TypeCheckCtx* c,
    int32_t         exprNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd) {
    const H2AstNode* n;
    int32_t          recvNode;
    int32_t          enumTypeId;
    int32_t          recvTypeId;
    int32_t          ignoredType;
    n = &c->ast->nodes[exprNode];
    if (n->kind != H2Ast_FIELD_EXPR) {
        return 0;
    }
    recvNode = H2AstFirstChild(c->ast, exprNode);
    if (recvNode < 0) {
        return 0;
    }
    if (!H2TCResolveTypePathExprType(c, recvNode, c->currentTypeOwnerTypeId, &recvTypeId)) {
        return 0;
    }
    enumTypeId = H2TCResolveAliasBaseType(c, recvTypeId);
    if (!H2TCIsNamedDeclKind(c, enumTypeId, H2Ast_ENUM)) {
        return 0;
    }
    if (H2TCFieldLookup(c, enumTypeId, n->dataStart, n->dataEnd, &ignoredType, NULL) != 0) {
        return H2TCFailSpan(c, H2Diag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
    }
    *outEnumType = enumTypeId;
    *outVariantStart = n->dataStart;
    *outVariantEnd = n->dataEnd;
    return 1;
}

int H2TCResolveEnumVariantTypeName(
    H2TypeCheckCtx* c,
    int32_t         typeNameNode,
    int32_t*        outEnumType,
    uint32_t*       outVariantStart,
    uint32_t*       outVariantEnd) {
    const H2AstNode* n = &c->ast->nodes[typeNameNode];
    uint32_t         dot = n->dataEnd;
    int32_t          namedIdx;
    int32_t          enumTypeId;
    int32_t          ignoredType;
    if (n->kind != H2Ast_TYPE_NAME || n->dataEnd <= n->dataStart) {
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
    (void)namedIdx;
    enumTypeId = H2TCResolveTypeNamePath(c, n->dataStart, dot, c->currentTypeOwnerTypeId);
    if (enumTypeId < 0) {
        return 0;
    }
    enumTypeId = H2TCResolveAliasBaseType(c, enumTypeId);
    if (!H2TCIsNamedDeclKind(c, enumTypeId, H2Ast_ENUM)) {
        return 0;
    }
    if (H2TCFieldLookup(c, enumTypeId, dot + 1u, n->dataEnd, &ignoredType, NULL) != 0) {
        return H2TCFailSpan(c, H2Diag_UNKNOWN_SYMBOL, dot + 1u, n->dataEnd);
    }
    *outEnumType = enumTypeId;
    *outVariantStart = dot + 1u;
    *outVariantEnd = n->dataEnd;
    return 1;
}

int H2TCFieldLookup(
    H2TypeCheckCtx* c,
    int32_t         typeId,
    uint32_t        fieldStart,
    uint32_t        fieldEnd,
    int32_t*        outType,
    uint32_t* _Nullable outFieldIndex) {
    uint32_t depth = 0;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return -1;
    }
    if (c->types[typeId].kind == H2TCType_PTR || c->types[typeId].kind == H2TCType_REF) {
        typeId = c->types[typeId].baseType;
    }
    while (depth++ <= c->typeLen) {
        uint32_t i;
        int32_t  embedIdx = -1;
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
            if (H2NameEqSlice(
                    c->src, c->fields[idx].nameStart, c->fields[idx].nameEnd, fieldStart, fieldEnd))
            {
                *outType = c->fields[idx].typeId;
                if (outFieldIndex != NULL) {
                    *outFieldIndex = idx;
                }
                return 0;
            }
            if ((c->fields[idx].flags & H2TCFieldFlag_EMBEDDED) != 0) {
                embedIdx = (int32_t)idx;
            }
        }
        if (c->types[typeId].kind != H2TCType_NAMED) {
            return -1;
        }
        if (embedIdx < 0) {
            return -1;
        }
        typeId = c->fields[embedIdx].typeId;
        if (typeId >= 0 && (uint32_t)typeId < c->typeLen && c->types[typeId].kind == H2TCType_ALIAS)
        {
            typeId = H2TCResolveAliasBaseType(c, typeId);
            if (typeId < 0) {
                return -1;
            }
        }
    }
    return -1;
}

int H2TCIsAsciiSpace(unsigned char ch) {
    return ch == (unsigned char)' ' || ch == (unsigned char)'\t' || ch == (unsigned char)'\r'
        || ch == (unsigned char)'\n' || ch == (unsigned char)'\f' || ch == (unsigned char)'\v';
}

int H2TCIsIdentStartChar(unsigned char ch) {
    return (ch >= (unsigned char)'a' && ch <= (unsigned char)'z')
        || (ch >= (unsigned char)'A' && ch <= (unsigned char)'Z') || ch == (unsigned char)'_';
}

int H2TCIsIdentContinueChar(unsigned char ch) {
    return H2TCIsIdentStartChar(ch) || (ch >= (unsigned char)'0' && ch <= (unsigned char)'9');
}

int H2TCFieldPathNextSegment(
    H2TypeCheckCtx* c,
    uint32_t        pathStart,
    uint32_t        pathEnd,
    uint32_t*       ioPos,
    uint32_t*       outSegStart,
    uint32_t*       outSegEnd) {
    uint32_t pos = *ioPos;
    while (pos < pathEnd && H2TCIsAsciiSpace((unsigned char)c->src.ptr[pos])) {
        pos++;
    }
    if (pos >= pathEnd) {
        *ioPos = pos;
        return 1;
    }
    if (!H2TCIsIdentStartChar((unsigned char)c->src.ptr[pos])) {
        return -1;
    }
    *outSegStart = pos;
    pos++;
    while (pos < pathEnd && H2TCIsIdentContinueChar((unsigned char)c->src.ptr[pos])) {
        pos++;
    }
    *outSegEnd = pos;
    while (pos < pathEnd && H2TCIsAsciiSpace((unsigned char)c->src.ptr[pos])) {
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

int H2TCFieldLookupPath(
    H2TypeCheckCtx* c,
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
        int      rc = H2TCFieldPathNextSegment(c, pathStart, pathEnd, &pos, &segStart, &segEnd);
        if (rc == 1) {
            break;
        }
        if (rc != 0) {
            return -1;
        }
        hadSegment = 1;
        if (H2TCFieldLookup(c, curType, segStart, segEnd, &fieldType, NULL) != 0) {
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

H2_API_END
