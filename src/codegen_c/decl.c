#include "internal.h"

SL_API_BEGIN
int IsMainFunctionNode(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    return n != NULL && n->kind == SLAst_FN
        && SliceEq(c->unit->source, n->dataStart, n->dataEnd, "main");
}

int IsExplicitlyExportedNode(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map;
    if (n == NULL) {
        return 0;
    }
    map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    return map != NULL && map->kind == n->kind && map->isExported;
}

int IsExportedNode(const SLCBackendC* c, int32_t nodeId) {
    if (IsMainFunctionNode(c, nodeId)) {
        return 1;
    }
    return IsExplicitlyExportedNode(c, nodeId);
}

int IsExportedTypeNode(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map;
    if (n == NULL || !IsTypeDeclKind(n->kind)) {
        return 0;
    }
    map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    return map != NULL && IsTypeDeclKind(map->kind) && map->isExported;
}

int EmitEnumDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int              hasPayload = EnumDeclHasPayload(c, nodeId);
    int              first = 1;

    if (child >= 0) {
        const SLAstNode* firstChild = NodeAt(c, child);
        if (firstChild != NULL
            && (firstChild->kind == SLAst_TYPE_NAME || firstChild->kind == SLAst_TYPE_PTR
                || firstChild->kind == SLAst_TYPE_REF || firstChild->kind == SLAst_TYPE_MUTREF
                || firstChild->kind == SLAst_TYPE_ARRAY || firstChild->kind == SLAst_TYPE_VARRAY
                || firstChild->kind == SLAst_TYPE_SLICE || firstChild->kind == SLAst_TYPE_MUTSLICE
                || firstChild->kind == SLAst_TYPE_OPTIONAL || firstChild->kind == SLAst_TYPE_FN
                || firstChild->kind == SLAst_TYPE_ANON_STRUCT
                || firstChild->kind == SLAst_TYPE_ANON_UNION
                || firstChild->kind == SLAst_TYPE_TUPLE))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }

    if (!hasPayload) {
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "typedef enum ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
            || BufAppendCStr(&c->out, " {\n") != 0)
        {
            return -1;
        }
        while (child >= 0) {
            const SLAstNode* item = NodeAt(c, child);
            if (item != NULL && item->kind == SLAst_FIELD) {
                int32_t initExpr = EnumVariantTagExprNode(c, child);
                EmitIndent(c, depth + 1u);
                if (!first && BufAppendCStr(&c->out, ",\n") != 0) {
                    return -1;
                }
                if (first) {
                    first = 0;
                }
                if (BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__") != 0
                    || BufAppendSlice(&c->out, c->unit->source, item->dataStart, item->dataEnd)
                           != 0)
                {
                    return -1;
                }
                if (initExpr >= 0) {
                    if (BufAppendCStr(&c->out, " = ") != 0 || EmitExpr(c, initExpr) != 0) {
                        return -1;
                    }
                }
            }
            child = AstNextSibling(&c->ast, child);
        }
        if (!first && BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }

        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
            || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
        return 0;
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef enum ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__tag {\n") != 0)
    {
        return -1;
    }
    while (child >= 0) {
        const SLAstNode* item = NodeAt(c, child);
        if (item != NULL && item->kind == SLAst_FIELD) {
            int32_t initExpr = EnumVariantTagExprNode(c, child);
            EmitIndent(c, depth + 1u);
            if (!first && BufAppendCStr(&c->out, ",\n") != 0) {
                return -1;
            }
            if (first) {
                first = 0;
            }
            if (BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__") != 0
                || BufAppendSlice(&c->out, c->unit->source, item->dataStart, item->dataEnd) != 0)
            {
                return -1;
            }
            if (initExpr >= 0) {
                if (BufAppendCStr(&c->out, " = ") != 0 || EmitExpr(c, initExpr) != 0) {
                    return -1;
                }
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    if (!first && BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__tag;\n") != 0)
    {
        return -1;
    }

    child = AstFirstChild(&c->ast, nodeId);
    if (child >= 0) {
        const SLAstNode* firstChild = NodeAt(c, child);
        if (firstChild != NULL
            && (firstChild->kind == SLAst_TYPE_NAME || firstChild->kind == SLAst_TYPE_PTR
                || firstChild->kind == SLAst_TYPE_REF || firstChild->kind == SLAst_TYPE_MUTREF
                || firstChild->kind == SLAst_TYPE_ARRAY || firstChild->kind == SLAst_TYPE_VARRAY
                || firstChild->kind == SLAst_TYPE_SLICE || firstChild->kind == SLAst_TYPE_MUTSLICE
                || firstChild->kind == SLAst_TYPE_OPTIONAL || firstChild->kind == SLAst_TYPE_FN
                || firstChild->kind == SLAst_TYPE_ANON_STRUCT
                || firstChild->kind == SLAst_TYPE_ANON_UNION
                || firstChild->kind == SLAst_TYPE_TUPLE))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef union ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__payload {\n") != 0)
    {
        return -1;
    }
    while (child >= 0) {
        const SLAstNode* item = NodeAt(c, child);
        int32_t          payload = AstFirstChild(&c->ast, child);
        if (item != NULL && item->kind == SLAst_FIELD && payload >= 0 && NodeAt(c, payload) != NULL
            && NodeAt(c, payload)->kind == SLAst_FIELD)
        {
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "struct {\n") != 0) {
                return -1;
            }
            while (payload >= 0) {
                const SLAstNode* pf = NodeAt(c, payload);
                int32_t          typeNode;
                char*            fieldName;
                if (pf == NULL || pf->kind != SLAst_FIELD) {
                    break;
                }
                typeNode = AstFirstChild(&c->ast, payload);
                fieldName = DupSlice(c, c->unit->source, pf->dataStart, pf->dataEnd);
                if (fieldName == NULL) {
                    return -1;
                }
                EmitIndent(c, depth + 2u);
                if (EmitTypeWithName(c, typeNode, fieldName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
                payload = AstNextSibling(&c->ast, payload);
            }
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "} ") != 0
                || BufAppendSlice(&c->out, c->unit->source, item->dataStart, item->dataEnd) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__payload;\n") != 0)
    {
        return -1;
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef struct ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, " {\n") != 0)
    {
        return -1;
    }
    EmitIndent(c, depth + 1u);
    if (BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__tag tag;\n") != 0) {
        return -1;
    }
    EmitIndent(c, depth + 1u);
    if (BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__payload payload;\n") != 0)
    {
        return -1;
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, ";\n") != 0)
    {
        return -1;
    }
    return 0;
}

int NodeHasDirectDependentFields(SLCBackendC* c, int32_t nodeId) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == SLAst_FIELD) {
            int32_t          typeNode = AstFirstChild(&c->ast, child);
            const SLAstNode* tn = NodeAt(c, typeNode);
            if (tn != NULL && tn->kind == SLAst_TYPE_VARRAY) {
                return 1;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int EmitVarSizeStructDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int              emittedHelper = 0;

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef struct ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__hdr {\n") != 0)
    {
        return -1;
    }

    while (child >= 0) {
        const SLAstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == SLAst_FIELD) {
            int32_t          typeNode = AstFirstChild(&c->ast, child);
            const SLAstNode* tn = NodeAt(c, typeNode);
            char*            name = DupSlice(c, c->unit->source, field->dataStart, field->dataEnd);
            if (name == NULL) {
                return -1;
            }
            if (tn != NULL && tn->kind == SLAst_TYPE_VARRAY) {
            } else {
                EmitIndent(c, depth + 1u);
                if (EmitTypeWithName(c, typeNode, name) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
            }
        }
        child = AstNextSibling(&c->ast, child);
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__hdr;\n") != 0)
    {
        return -1;
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__hdr ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, ";\n") != 0)
    {
        return -1;
    }

    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == SLAst_FIELD) {
            int32_t          typeNode = AstFirstChild(&c->ast, child);
            const SLAstNode* tn = NodeAt(c, typeNode);
            if (tn != NULL && tn->kind == SLAst_TYPE_VARRAY) {
                SLTypeRef depType;
                int32_t   elemTypeNode = AstFirstChild(&c->ast, typeNode);
                int32_t   walk;
                if (ParseTypeRef(c, typeNode, &depType) != 0) {
                    return -1;
                }
                EmitIndent(c, depth);
                if (BufAppendCStr(&c->out, "static inline ") != 0
                    || EmitTypeNameWithDepth(c, &depType) != 0 || BufAppendChar(&c->out, ' ') != 0
                    || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__") != 0
                    || BufAppendSlice(&c->out, c->unit->source, field->dataStart, field->dataEnd)
                           != 0
                    || BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, map->cName) != 0
                    || BufAppendCStr(&c->out, "* p) {\n") != 0)
                {
                    return -1;
                }
                EmitIndent(c, depth + 1u);
                if (BufAppendCStr(&c->out, "__sl_uint off = sizeof(") != 0
                    || BufAppendCStr(&c->out, map->cName) != 0
                    || BufAppendCStr(&c->out, "__hdr);\n") != 0)
                {
                    return -1;
                }

                walk = AstFirstChild(&c->ast, nodeId);
                while (walk >= 0) {
                    const SLAstNode* wf = NodeAt(c, walk);
                    if (wf != NULL && wf->kind == SLAst_FIELD) {
                        int32_t          wt = AstFirstChild(&c->ast, walk);
                        const SLAstNode* wtn = NodeAt(c, wt);
                        if (wtn != NULL && wtn->kind == SLAst_TYPE_VARRAY) {
                            int32_t welem = AstFirstChild(&c->ast, wt);
                            EmitIndent(c, depth + 1u);
                            if (BufAppendCStr(&c->out, "off = __sl_align_up(off, _Alignof(") != 0
                                || EmitTypeForCast(c, welem) != 0
                                || BufAppendCStr(&c->out, "));\n") != 0)
                            {
                                return -1;
                            }
                            if (walk == child) {
                                EmitIndent(c, depth + 1u);
                                if (BufAppendCStr(&c->out, "return (") != 0
                                    || EmitTypeNameWithDepth(c, &depType) != 0
                                    || BufAppendCStr(&c->out, ")((__sl_u8*)p + off);\n") != 0)
                                {
                                    return -1;
                                }
                                break;
                            }
                            EmitIndent(c, depth + 1u);
                            if (BufAppendCStr(&c->out, "off += (__sl_uint)p->") != 0
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
                                EmitIndent(c, depth + 1u);
                                if (BufAppendCStr(&c->out, "off += ") != 0) {
                                    return -1;
                                }
                                if (IsStrBaseName(wVarSizeBaseName)) {
                                    if (BufAppendCStr(&c->out, "__sl_str_sizeof((__sl_str*)&p->")
                                            != 0
                                        || BufAppendSlice(
                                               &c->out, c->unit->source, wf->dataStart, wf->dataEnd)
                                               != 0
                                        || BufAppendCStr(&c->out, ")") != 0)
                                    {
                                        return -1;
                                    }
                                } else if (
                                    BufAppendCStr(&c->out, wVarSizeBaseName) != 0
                                    || BufAppendCStr(&c->out, "__sizeof(&p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, wf->dataStart, wf->dataEnd)
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
                    walk = AstNextSibling(&c->ast, walk);
                }
                EmitIndent(c, depth);
                if (BufAppendCStr(&c->out, "}\n") != 0) {
                    return -1;
                }
                emittedHelper = 1;
                (void)elemTypeNode;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }

    if (emittedHelper) {
        int32_t walk = AstFirstChild(&c->ast, nodeId);
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "static inline __sl_uint ") != 0
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__sizeof(") != 0
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "* p) {\n") != 0)
        {
            return -1;
        }
        EmitIndent(c, depth + 1u);
        if (BufAppendCStr(&c->out, "__sl_uint off = sizeof(") != 0
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__hdr);\n") != 0)
        {
            return -1;
        }
        while (walk >= 0) {
            const SLAstNode* wf = NodeAt(c, walk);
            if (wf != NULL && wf->kind == SLAst_FIELD) {
                int32_t          wt = AstFirstChild(&c->ast, walk);
                const SLAstNode* wtn = NodeAt(c, wt);
                if (wtn != NULL && wtn->kind == SLAst_TYPE_VARRAY) {
                    int32_t welem = AstFirstChild(&c->ast, wt);
                    EmitIndent(c, depth + 1u);
                    if (BufAppendCStr(&c->out, "off = __sl_align_up(off, _Alignof(") != 0
                        || EmitTypeForCast(c, welem) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
                    {
                        return -1;
                    }
                    EmitIndent(c, depth + 1u);
                    if (BufAppendCStr(&c->out, "off += (__sl_uint)p->") != 0
                        || BufAppendSlice(&c->out, c->unit->source, wtn->dataStart, wtn->dataEnd)
                               != 0
                        || BufAppendCStr(&c->out, " * sizeof(") != 0
                        || EmitTypeForCast(c, welem) != 0 || BufAppendCStr(&c->out, ");\n") != 0)
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
                        EmitIndent(c, depth + 1u);
                        if (BufAppendCStr(&c->out, "off += ") != 0) {
                            return -1;
                        }
                        if (IsStrBaseName(wVarSizeBaseName)) {
                            if (BufAppendCStr(&c->out, "__sl_str_sizeof((__sl_str*)&p->") != 0
                                || BufAppendSlice(
                                       &c->out, c->unit->source, wf->dataStart, wf->dataEnd)
                                       != 0
                                || BufAppendCStr(&c->out, ")") != 0)
                            {
                                return -1;
                            }
                        } else if (
                            BufAppendCStr(&c->out, wVarSizeBaseName) != 0
                            || BufAppendCStr(&c->out, "__sizeof(&p->") != 0
                            || BufAppendSlice(&c->out, c->unit->source, wf->dataStart, wf->dataEnd)
                                   != 0
                            || BufAppendChar(&c->out, ')') != 0)
                        {
                            return -1;
                        }
                        if (BufAppendCStr(&c->out, " - sizeof(") != 0 || EmitTypeForCast(c, wt) != 0
                            || BufAppendCStr(&c->out, ");\n") != 0)
                        {
                            return -1;
                        }
                    }
                }
            }
            walk = AstNextSibling(&c->ast, walk);
        }
        EmitIndent(c, depth + 1u);
        if (BufAppendCStr(&c->out, "off = __sl_align_up(off, _Alignof(") != 0
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__hdr));\n") != 0)
        {
            return -1;
        }
        EmitIndent(c, depth + 1u);
        if (BufAppendCStr(&c->out, "return off;\n") != 0) {
            return -1;
        }
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "}\n") != 0) {
            return -1;
        }
    }

    return 0;
}

int EmitStructOrUnionDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isUnion) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          child = AstFirstChild(&c->ast, nodeId);

    if (!isUnion && NodeHasDirectDependentFields(c, nodeId)) {
        return EmitVarSizeStructDecl(c, nodeId, depth);
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef ") != 0
        || BufAppendCStr(&c->out, isUnion ? "union " : "struct ") != 0
        || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, " {\n") != 0)
    {
        return -1;
    }

    while (child >= 0) {
        const SLAstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == SLAst_FIELD) {
            int32_t typeNode = AstFirstChild(&c->ast, child);
            char*   name = DupSlice(c, c->unit->source, field->dataStart, field->dataEnd);
            if (name == NULL) {
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (EmitTypeWithName(c, typeNode, name) != 0 || BufAppendCStr(&c->out, ";\n") != 0) {
                return -1;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, ";\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitForwardTypeDecls(SLCBackendC* c) {
    uint32_t i;
    int      emittedAny = 0;
    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        const SLNameMap* map;
        if (n == NULL
            || (n->kind != SLAst_STRUCT && n->kind != SLAst_UNION && n->kind != SLAst_ENUM))
        {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (n->kind == SLAst_ENUM) {
            if (EnumDeclHasPayload(c, nodeId)) {
                if (BufAppendCStr(&c->out, "typedef struct ") != 0
                    || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                    || BufAppendCStr(&c->out, map->cName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "typedef enum ") != 0
                    || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                    || BufAppendCStr(&c->out, map->cName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
            }
        } else if (n->kind == SLAst_STRUCT && NodeHasDirectDependentFields(c, nodeId)) {
            if (BufAppendCStr(&c->out, "typedef struct ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__hdr ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0
                || BufAppendCStr(&c->out, "__hdr;\n") != 0)
            {
                return -1;
            }
            EmitIndent(c, 0);
            if (BufAppendCStr(&c->out, "typedef ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
                || BufAppendCStr(&c->out, "__hdr ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "typedef ") != 0
                || BufAppendCStr(&c->out, n->kind == SLAst_UNION ? "union " : "struct ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
        emittedAny = 1;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        const SLNameMap* map;
        if (n == NULL
            || (n->kind != SLAst_STRUCT && n->kind != SLAst_UNION && n->kind != SLAst_ENUM))
        {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (n->kind == SLAst_ENUM) {
            if (EnumDeclHasPayload(c, nodeId)) {
                if (BufAppendCStr(&c->out, "typedef struct ") != 0
                    || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                    || BufAppendCStr(&c->out, map->cName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "typedef enum ") != 0
                    || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                    || BufAppendCStr(&c->out, map->cName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
            }
        } else if (n->kind == SLAst_STRUCT && NodeHasDirectDependentFields(c, nodeId)) {
            if (BufAppendCStr(&c->out, "typedef struct ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__hdr ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0
                || BufAppendCStr(&c->out, "__hdr;\n") != 0)
            {
                return -1;
            }
            EmitIndent(c, 0);
            if (BufAppendCStr(&c->out, "typedef ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
                || BufAppendCStr(&c->out, "__hdr ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "typedef ") != 0
                || BufAppendCStr(&c->out, n->kind == SLAst_UNION ? "union " : "struct ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
        emittedAny = 1;
    }
    if (emittedAny && BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }
    return 0;
}

int EmitForwardAnonTypeDecls(SLCBackendC* c) {
    uint32_t i;
    if (c->anonTypeLen == 0) {
        return 0;
    }
    for (i = 0; i < c->anonTypeLen; i++) {
        const SLAnonTypeInfo* t = &c->anonTypes[i];
        EmitIndent(c, 0);
        if (BufAppendCStr(&c->out, "typedef ") != 0
            || BufAppendCStr(&c->out, t->isUnion ? "union " : "struct ") != 0
            || BufAppendCStr(&c->out, t->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
            || BufAppendCStr(&c->out, t->cName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
    }
    return BufAppendChar(&c->out, '\n');
}

int EmitAnonTypeDecls(SLCBackendC* c) {
    uint32_t i;
    if (c->anonTypeLen == 0) {
        return 0;
    }
    for (i = 0; i < c->anonTypeLen; i++) {
        SLAnonTypeInfo* t = &c->anonTypes[i];
        uint32_t        j;
        for (j = 0; j < t->fieldCount; j++) {
            const SLFieldInfo* f = &c->fieldInfos[t->fieldStart + j];
            if (EnsureAnonTypeVisible(c, &f->type, 0) != 0) {
                return -1;
            }
        }
        if ((t->flags & SLAnonTypeFlag_EMITTED_GLOBAL) != 0) {
            continue;
        }
        EmitIndent(c, 0);
        if (BufAppendCStr(&c->out, "typedef ") != 0
            || BufAppendCStr(&c->out, t->isUnion ? "union " : "struct ") != 0
            || BufAppendCStr(&c->out, t->cName) != 0 || BufAppendCStr(&c->out, " {\n") != 0)
        {
            return -1;
        }
        for (j = 0; j < t->fieldCount; j++) {
            const SLFieldInfo* f = &c->fieldInfos[t->fieldStart + j];
            EmitIndent(c, 1);
            if (EmitTypeRefWithName(c, &f->type, f->fieldName) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
        EmitIndent(c, 0);
        if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, t->cName) != 0
            || BufAppendCStr(&c->out, ";\n\n") != 0)
        {
            return -1;
        }
        t->flags |= SLAnonTypeFlag_EMITTED_GLOBAL;
    }
    return 0;
}

int EmitHeaderTypeAliasDecls(SLCBackendC* c) {
    uint32_t i;
    int      emittedAny = 0;
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != SLAst_TYPE_ALIAS) {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
        emittedAny = 1;
    }
    if (emittedAny && BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }
    return 0;
}

int EmitFnTypeAliasDecls(SLCBackendC* c) {
    uint32_t i;
    if (c->fnTypeAliasLen == 0) {
        return 0;
    }
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        const SLFnTypeAlias* alias = &c->fnTypeAliases[i];
        uint32_t             p;
        EmitIndent(c, 0);
        if (BufAppendCStr(&c->out, "typedef ") != 0
            || EmitTypeNameWithDepth(c, &alias->returnType) != 0
            || BufAppendCStr(&c->out, " (*") != 0 || BufAppendCStr(&c->out, alias->aliasName) != 0
            || BufAppendCStr(&c->out, ")(") != 0)
        {
            return -1;
        }
        if (alias->paramLen == 0) {
            if (BufAppendCStr(&c->out, "void") != 0) {
                return -1;
            }
        } else {
            for (p = 0; p < alias->paramLen; p++) {
                SLBuf paramNameBuf = { 0 };
                char* paramName;
                if (p > 0 && BufAppendCStr(&c->out, ", ") != 0) {
                    return -1;
                }
                paramNameBuf.arena = &c->arena;
                if (BufAppendCStr(&paramNameBuf, "p") != 0 || BufAppendU32(&paramNameBuf, p) != 0) {
                    return -1;
                }
                paramName = BufFinish(&paramNameBuf);
                if (paramName == NULL) {
                    return -1;
                }
                if (EmitTypeRefWithName(c, &alias->paramTypes[p], paramName) != 0) {
                    return -1;
                }
            }
        }
        if (BufAppendCStr(&c->out, ");\n") != 0) {
            return -1;
        }
    }
    return BufAppendChar(&c->out, '\n');
}

int FnNodeHasBody(const SLCBackendC* c, int32_t nodeId) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == SLAst_BLOCK) {
            return 1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int HasFunctionBodyForName(const SLCBackendC* c, int32_t nodeId) {
    const char* fnCName = FindFnCNameByNodeId(c, nodeId);
    uint32_t    i;
    if (fnCName == NULL) {
        return 0;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          otherId = c->topDecls[i].nodeId;
        const SLAstNode* other = NodeAt(c, otherId);
        const char*      otherCName;
        if (other == NULL || other->kind != SLAst_FN || otherId == nodeId
            || !FnNodeHasBody(c, otherId))
        {
            continue;
        }
        otherCName = FindFnCNameByNodeId(c, otherId);
        if (otherCName != NULL && StrEq(fnCName, otherCName)) {
            return 1;
        }
    }
    return 0;
}

int EmitFnDeclOrDef(
    SLCBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          emitBody,
    int          isPrivate,
    const SLFnSig* _Nullable forcedSig) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const char*      fnCName;
    const SLFnSig*   fnSig;
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          bodyNode = -1;
    int              firstParam = 1;
    int              isMainFn;
    int              hasFnContext;
    uint32_t         savedLocalLen;
    SLTypeRef        savedReturnType = c->currentReturnType;
    int              savedHasReturnType = c->hasCurrentReturnType;
    SLTypeRef        savedContextType = c->currentContextType;
    int              savedHasContext = c->hasCurrentContext;
    int              savedCurrentFunctionIsMain = c->currentFunctionIsMain;
    const char*      savedActivePackParamName = c->activePackParamName;
    char**           savedActivePackElemNames = c->activePackElemNames;
    SLTypeRef*       savedActivePackElemTypes = c->activePackElemTypes;
    uint32_t         savedActivePackElemCount = c->activePackElemCount;
    SLTypeRef        fnReturnType;
    SLTypeRef        fnContextType;
    SLTypeRef        fnSemanticContextType;
    SLTypeRef        fnContextParamType;
    SLTypeRef        fnContextLocalType;
    int              forceStatic = 0;

    (void)isPrivate;

    TypeRefSetScalar(&fnReturnType, "void");
    TypeRefSetInvalid(&fnContextType);
    TypeRefSetInvalid(&fnSemanticContextType);
    TypeRefSetInvalid(&fnContextParamType);
    TypeRefSetInvalid(&fnContextLocalType);

    if (n == NULL) {
        return -1;
    }
    fnSig = forcedSig != NULL ? forcedSig : FindFnSigByNodeId(c, nodeId);
    if (fnSig == NULL) {
        return -1;
    }
    if (!emitBody && (fnSig->flags & SLFnSigFlag_TEMPLATE_INSTANCE) != 0
        && c->emitPrivateFnDeclStatic == 0)
    {
        return 0;
    }
    fnCName = fnSig->cName;
    isMainFn = IsMainFunctionNode(c, nodeId);
    hasFnContext = fnSig->hasContext || isMainFn;
    if (hasFnContext) {
        if (isMainFn) {
            TypeRefSetScalar(&fnContextType, "__sl_Context");
            if (ResolveMainSemanticContextType(c, &fnSemanticContextType) != 0) {
                return -1;
            }
        } else {
            fnContextType = fnSig->contextType;
            fnSemanticContextType = fnContextType;
        }
        fnContextParamType = fnContextType;
        fnContextParamType.ptrDepth++;
        fnContextLocalType = fnContextParamType;
        if (isMainFn) {
            fnContextLocalType = fnSemanticContextType;
            fnContextLocalType.ptrDepth++;
        }
    }

    EmitIndent(c, depth);
    forceStatic = ((fnSig->flags & SLFnSigFlag_TEMPLATE_INSTANCE) != 0)
               && (emitBody || c->emitPrivateFnDeclStatic);
    if ((fnSig->flags & SLFnSigFlag_TEMPLATE_INSTANCE) != 0 && fnSig->tcFuncIndex == UINT32_MAX) {
        forceStatic = 0;
    }
    if (forceStatic) {
        if (BufAppendCStr(&c->out, "static ") != 0) {
            return -1;
        }
    }

    while (child >= 0) {
        const SLAstNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == SLAst_BLOCK) {
            bodyNode = child;
        }
        child = AstNextSibling(&c->ast, child);
    }

    fnReturnType = fnSig->returnType;
    if (EmitTypeRefWithName(c, &fnReturnType, fnCName) != 0) {
        return -1;
    }

    if (BufAppendChar(&c->out, '(') != 0) {
        return -1;
    }

    if (hasFnContext) {
        if (isMainFn) {
            if (BufAppendCStr(&c->out, "__sl_Context *context __attribute__((unused))") != 0) {
                return -1;
            }
        } else {
            if (EmitTypeRefWithName(c, &fnContextParamType, "context") != 0) {
                return -1;
            }
        }
        firstParam = 0;
    }

    {
        uint32_t paramIndex = 0;
        for (paramIndex = 0; paramIndex < fnSig->paramLen; paramIndex++) {
            const char* paramName =
                (fnSig->paramNames != NULL && fnSig->paramNames[paramIndex] != NULL
                 && fnSig->paramNames[paramIndex][0] != '\0')
                    ? fnSig->paramNames[paramIndex]
                    : "__sl_v";
            if (!firstParam && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (EmitTypeRefWithName(c, &fnSig->paramTypes[paramIndex], paramName) != 0) {
                return -1;
            }
            firstParam = 0;
        }
    }

    if (firstParam && BufAppendCStr(&c->out, "void") != 0) {
        return -1;
    }
    if (BufAppendChar(&c->out, ')') != 0) {
        return -1;
    }

    if (!emitBody || bodyNode < 0) {
        return BufAppendCStr(&c->out, ";\n");
    }

    savedLocalLen = c->localLen;
    if (PushScope(c) != 0) {
        return -1;
    }
    if (hasFnContext) {
        if (AddLocal(c, "context", fnContextLocalType) != 0) {
            return -1;
        }
    }
    {
        uint32_t paramIndex = 0;
        for (paramIndex = 0; paramIndex < fnSig->paramLen; paramIndex++) {
            const char* paramName =
                (fnSig->paramNames != NULL && fnSig->paramNames[paramIndex] != NULL
                 && fnSig->paramNames[paramIndex][0] != '\0')
                    ? fnSig->paramNames[paramIndex]
                    : "__sl_v";
            if (AddLocal(c, paramName, fnSig->paramTypes[paramIndex]) != 0) {
                return -1;
            }
        }
    }
    c->activePackParamName = NULL;
    c->activePackElemNames = NULL;
    c->activePackElemTypes = NULL;
    c->activePackElemCount = 0;
    if ((fnSig->flags & SLFnSigFlag_EXPANDED_ANYPACK) != 0 && fnSig->packParamName != NULL
        && fnSig->packArgStart + fnSig->packArgCount <= fnSig->paramLen)
    {
        c->activePackParamName = fnSig->packParamName;
        c->activePackElemNames = &fnSig->paramNames[fnSig->packArgStart];
        c->activePackElemTypes = &fnSig->paramTypes[fnSig->packArgStart];
        c->activePackElemCount = fnSig->packArgCount;
    }

    c->currentReturnType = fnReturnType;
    c->hasCurrentReturnType = 1;
    c->currentContextType = fnSemanticContextType;
    c->hasCurrentContext = hasFnContext;
    c->currentFunctionIsMain = !hasFnContext && isMainFn;
    if (BufAppendChar(&c->out, ' ') != 0 || EmitBlockInline(c, bodyNode, depth) != 0) {
        c->currentReturnType = savedReturnType;
        c->hasCurrentReturnType = savedHasReturnType;
        c->currentContextType = savedContextType;
        c->hasCurrentContext = savedHasContext;
        c->currentFunctionIsMain = savedCurrentFunctionIsMain;
        c->activePackParamName = savedActivePackParamName;
        c->activePackElemNames = savedActivePackElemNames;
        c->activePackElemTypes = savedActivePackElemTypes;
        c->activePackElemCount = savedActivePackElemCount;
        return -1;
    }
    c->currentReturnType = savedReturnType;
    c->hasCurrentReturnType = savedHasReturnType;
    c->currentContextType = savedContextType;
    c->hasCurrentContext = savedHasContext;
    c->currentFunctionIsMain = savedCurrentFunctionIsMain;
    c->activePackParamName = savedActivePackParamName;
    c->activePackElemNames = savedActivePackElemNames;
    c->activePackElemTypes = savedActivePackElemTypes;
    c->activePackElemCount = savedActivePackElemCount;

    PopScope(c);
    c->localLen = savedLocalLen;
    TrimVariantNarrowsToLocalLen(c);
    return 0;
}

int EmitConstDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const SLAstNode*  n = NodeAt(c, nodeId);
    SLCCGVarLikeParts parts;
    uint32_t          i;
    SLTypeRef         sharedType;
    if (n == NULL) {
        return -1;
    }
    if (ResolveVarLikeParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (!parts.grouped) {
        const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        int32_t          typeNode = parts.typeNode;
        int32_t          initNode = parts.initNode;
        SLTypeRef        type;
        if (map == NULL) {
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
        if (declarationOnly) {
            if (BufAppendCStr(&c->out, "extern const ") != 0) {
                return -1;
            }
        } else {
            if (isPrivate && BufAppendCStr(&c->out, "static ") != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, "const ") != 0) {
                return -1;
            }
        }
        if ((typeNode >= 0 && EmitTypeWithName(c, typeNode, map->cName) != 0)
            || (typeNode < 0 && EmitTypeRefWithName(c, &type, map->cName) != 0))
        {
            return -1;
        }
        if (!declarationOnly) {
            if (BufAppendCStr(&c->out, " = ") != 0) {
                return -1;
            }
            if (initNode >= 0) {
                int emittedConstValue = 0;
                if (c->constEval != NULL) {
                    SLCTFEValue constValue;
                    int         isConst = 0;
                    if (SLConstEvalSessionEvalTopLevelConst(
                            c->constEval, nodeId, &constValue, &isConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (isConst
                        && EmitConstEvaluatedScalar(c, &type, &constValue, &emittedConstValue) != 0)
                    {
                        return -1;
                    }
                }
                if (!emittedConstValue && EmitExprCoerced(c, initNode, &type) != 0) {
                    return -1;
                }
            } else if (BufAppendChar(&c->out, '0') != 0) {
                return -1;
            }
        }
        return BufAppendCStr(&c->out, ";\n");
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
    for (i = 0; i < parts.nameCount; i++) {
        int32_t          nameNode = ListItemAt(&c->ast, parts.nameListNode, i);
        const SLAstNode* nameAst = NodeAt(c, nameNode);
        const SLNameMap* map;
        int32_t          initNode = -1;
        SLTypeRef        type;
        if (nameAst == NULL) {
            return -1;
        }
        map = FindNameBySlice(c, nameAst->dataStart, nameAst->dataEnd);
        if (map == NULL) {
            return -1;
        }
        if (parts.initNode >= 0) {
            if (NodeAt(c, parts.initNode) == NULL
                || NodeAt(c, parts.initNode)->kind != SLAst_EXPR_LIST)
            {
                return -1;
            }
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

        EmitIndent(c, depth);
        if (declarationOnly) {
            if (BufAppendCStr(&c->out, "extern const ") != 0) {
                return -1;
            }
        } else {
            if (isPrivate && BufAppendCStr(&c->out, "static ") != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, "const ") != 0) {
                return -1;
            }
        }
        if ((parts.typeNode >= 0 && EmitTypeWithName(c, parts.typeNode, map->cName) != 0)
            || (parts.typeNode < 0 && EmitTypeRefWithName(c, &type, map->cName) != 0))
        {
            return -1;
        }
        if (!declarationOnly) {
            if (BufAppendCStr(&c->out, " = ") != 0) {
                return -1;
            }
            if (initNode >= 0) {
                if (EmitExprCoerced(c, initNode, &type) != 0) {
                    return -1;
                }
            } else if (BufAppendChar(&c->out, '0') != 0) {
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, ";\n") != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitVarDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const SLAstNode*  n = NodeAt(c, nodeId);
    SLCCGVarLikeParts parts;
    uint32_t          i;
    SLTypeRef         sharedType;
    if (n == NULL) {
        return -1;
    }
    if (ResolveVarLikeParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (!parts.grouped) {
        const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        int32_t          typeNode = parts.typeNode;
        int32_t          initNode = parts.initNode;
        SLTypeRef        type;
        if (map == NULL) {
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
        if (declarationOnly) {
            if (BufAppendCStr(&c->out, "extern ") != 0) {
                return -1;
            }
        } else if (isPrivate && BufAppendCStr(&c->out, "static ") != 0) {
            return -1;
        }
        if ((typeNode >= 0 && EmitTypeWithName(c, typeNode, map->cName) != 0)
            || (typeNode < 0 && EmitTypeRefWithName(c, &type, map->cName) != 0))
        {
            return -1;
        }
        if (!declarationOnly) {
            if (initNode >= 0) {
                if (BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, initNode, &type) != 0)
                {
                    return -1;
                }
            } else if (BufAppendCStr(&c->out, " = {0}") != 0) {
                return -1;
            }
        }
        return BufAppendCStr(&c->out, ";\n");
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

    for (i = 0; i < parts.nameCount; i++) {
        int32_t          nameNode = ListItemAt(&c->ast, parts.nameListNode, i);
        const SLAstNode* nameAst = NodeAt(c, nameNode);
        const SLNameMap* map;
        int32_t          initNode = -1;
        SLTypeRef        type;
        if (nameAst == NULL) {
            return -1;
        }
        map = FindNameBySlice(c, nameAst->dataStart, nameAst->dataEnd);
        if (map == NULL) {
            return -1;
        }
        if (parts.initNode >= 0) {
            if (NodeAt(c, parts.initNode) == NULL
                || NodeAt(c, parts.initNode)->kind != SLAst_EXPR_LIST)
            {
                return -1;
            }
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

        EmitIndent(c, depth);
        if (declarationOnly) {
            if (BufAppendCStr(&c->out, "extern ") != 0) {
                return -1;
            }
        } else if (isPrivate && BufAppendCStr(&c->out, "static ") != 0) {
            return -1;
        }
        if ((parts.typeNode >= 0 && EmitTypeWithName(c, parts.typeNode, map->cName) != 0)
            || (parts.typeNode < 0 && EmitTypeRefWithName(c, &type, map->cName) != 0))
        {
            return -1;
        }
        if (!declarationOnly) {
            if (initNode >= 0) {
                if (BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, initNode, &type) != 0)
                {
                    return -1;
                }
            } else if (BufAppendCStr(&c->out, " = {0}") != 0) {
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, ";\n") != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitTypeAliasDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          targetNode = AstFirstChild(&c->ast, nodeId);
    (void)declarationOnly;
    (void)isPrivate;
    if (map == NULL || targetNode < 0) {
        return -1;
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef ") != 0 || EmitTypeWithName(c, targetNode, map->cName) != 0
        || BufAppendCStr(&c->out, ";\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitDeclNode(
    SLCBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          declarationOnly,
    int          isPrivate,
    int          emitBody) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAst_STRUCT: return EmitStructOrUnionDecl(c, nodeId, depth, 0);
        case SLAst_UNION:  return EmitStructOrUnionDecl(c, nodeId, depth, 1);
        case SLAst_ENUM:   return EmitEnumDecl(c, nodeId, depth);
        case SLAst_TYPE_ALIAS:
            return EmitTypeAliasDecl(c, nodeId, depth, declarationOnly, isPrivate);
        case SLAst_FN: {
            const SLFnSig* sigs[SLCCG_MAX_CALL_CANDIDATES];
            uint32_t       nSigs = FindFnSigCandidatesByNodeId(
                c, nodeId, sigs, (uint32_t)(sizeof(sigs) / sizeof(sigs[0])));
            int      importedBeforeOwnOffset = 0;
            uint32_t i;
            int      emitted = 0;
            if (c->options != NULL && c->options->emitNodeStartOffsetEnabled != 0
                && n->start < c->options->emitNodeStartOffset)
            {
                importedBeforeOwnOffset = 1;
            }
            if (nSigs == 0) {
                if (importedBeforeOwnOffset) {
                    return 0;
                }
                if (EmitFnDeclOrDef(c, nodeId, depth, emitBody, isPrivate, NULL) != 0) {
                    if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                        SetDiagNode(c, nodeId, SLDiag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
                return 0;
            }
            if (nSigs > (uint32_t)(sizeof(sigs) / sizeof(sigs[0]))) {
                nSigs = (uint32_t)(sizeof(sigs) / sizeof(sigs[0]));
            }
            for (i = 0; i < nSigs; i++) {
                if ((sigs[i]->flags & SLFnSigFlag_TEMPLATE_INSTANCE) != 0) {
                    continue;
                }
                if (importedBeforeOwnOffset) {
                    continue;
                }
                if (EmitFnDeclOrDef(c, nodeId, depth, emitBody, isPrivate, sigs[i]) != 0) {
                    if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                        SetDiagNode(c, nodeId, SLDiag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
                emitted = 1;
            }
            (void)emitted;
            return 0;
        }
        case SLAst_VAR:   return EmitVarDecl(c, nodeId, depth, declarationOnly, isPrivate);
        case SLAst_CONST: return EmitConstDecl(c, nodeId, depth, declarationOnly, isPrivate);
        default:          return 0;
    }
}

int EmitPrelude(SLCBackendC* c) {
    return BufAppendCStr(&c->out, "#include <core/core.h>\n");
}

char* _Nullable BuildDefaultMacro(SLCBackendC* c, const char* pkgName, const char* suffix) {
    SLBuf  b = { 0 };
    size_t i;
    b.arena = &c->arena;
    for (i = 0; pkgName[i] != '\0'; i++) {
        char ch = pkgName[i];
        if (IsAlnumChar(ch)) {
            ch = ToUpperChar(ch);
        } else {
            ch = '_';
        }
        if (BufAppendChar(&b, ch) != 0) {
            return NULL;
        }
    }
    if (BufAppendCStr(&b, suffix) != 0) {
        return NULL;
    }
    return BufFinish(&b);
}

int EmitHeader(SLCBackendC* c) {
    char*       defaultGuard = NULL;
    char*       defaultImpl = NULL;
    const char* guard;
    const char* impl;
    uint32_t    i;

    defaultGuard = BuildDefaultMacro(c, c->unit->packageName, "_H");
    defaultImpl = BuildDefaultMacro(c, c->unit->packageName, "_IMPL");
    if (defaultGuard == NULL || defaultImpl == NULL) {
        return -1;
    }

    guard = (c->options != NULL && c->options->headerGuard != NULL)
              ? c->options->headerGuard
              : defaultGuard;
    impl =
        (c->options != NULL && c->options->implMacro != NULL) ? c->options->implMacro : defaultImpl;

    if (BufAppendCStr(&c->out, "#ifndef ") != 0 || BufAppendCStr(&c->out, guard) != 0
        || BufAppendCStr(&c->out, "\n#define ") != 0 || BufAppendCStr(&c->out, guard) != 0
        || BufAppendCStr(&c->out, "\n\n") != 0)
    {
        return -1;
    }

    if (EmitPrelude(c) != 0) {
        return -1;
    }
    if (EmitForwardTypeDecls(c) != 0) {
        return -1;
    }
    if (EmitForwardAnonTypeDecls(c) != 0) {
        return -1;
    }
    if (EmitHeaderTypeAliasDecls(c) != 0) {
        return -1;
    }
    if (EmitAnonTypeDecls(c) != 0) {
        return -1;
    }
    if (EmitFnTypeAliasDecls(c) != 0) {
        return -1;
    }

    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n == NULL) {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
    }

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t nodeId = c->topDecls[i].nodeId;
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        if (IsMainFunctionNode(c, nodeId) && !IsExplicitlyExportedNode(c, nodeId)) {
            if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
                return -1;
            }
            break;
        }
    }

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != SLAst_FN || !FnNodeHasBody(c, nodeId)
            || IsExportedNode(c, nodeId))
        {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
    }

    if (BufAppendCStr(&c->out, "#ifdef ") != 0 || BufAppendCStr(&c->out, impl) != 0
        || BufAppendCStr(&c->out, "\n\n") != 0)
    {
        return -1;
    }

    if (EmitStringLiteralPool(c) != 0) {
        return -1;
    }

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != SLAst_TYPE_ALIAS || IsExportedTypeNode(c, nodeId)) {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        if (EmitDeclNode(c, nodeId, 0, 0, 1, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
    }

    c->emitPrivateFnDeclStatic = 1;
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        int              exported;
        if (n == NULL || n->kind != SLAst_FN || !FnNodeHasBody(c, nodeId)) {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        exported = IsExportedNode(c, nodeId);
        if (EmitDeclNode(c, nodeId, 0, 1, !exported, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            c->emitPrivateFnDeclStatic = 0;
            return -1;
        }
    }
    for (i = 0; i < c->fnSigLen; i++) {
        const SLFnSig*   sig = &c->fnSigs[i];
        const SLAstNode* fnNode;
        if ((sig->flags & SLFnSigFlag_TEMPLATE_INSTANCE) == 0) {
            continue;
        }
        fnNode = NodeAt(c, sig->nodeId);
        if (fnNode == NULL || fnNode->kind != SLAst_FN || !FnNodeHasBody(c, sig->nodeId)) {
            continue;
        }
        if (EmitFnDeclOrDef(c, sig->nodeId, 0, 0, 1, sig) != 0 || BufAppendChar(&c->out, '\n') != 0)
        {
            if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                SetDiagNode(c, sig->nodeId, SLDiag_CODEGEN_INTERNAL);
            }
            c->emitPrivateFnDeclStatic = 0;
            return -1;
        }
    }
    c->emitPrivateFnDeclStatic = 0;

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        int              exported;
        if (n == NULL) {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        exported = IsExportedNode(c, nodeId);

        if (IsTypeDeclKind(n->kind) && IsExportedTypeNode(c, nodeId)) {
            continue;
        }

        if (n->kind == SLAst_FN) {
            if (FnNodeHasBody(c, nodeId)) {
                if (EmitDeclNode(c, nodeId, 0, 0, !exported, 1) != 0
                    || BufAppendChar(&c->out, '\n') != 0)
                {
                    return -1;
                }
            } else if (!exported && !HasFunctionBodyForName(c, nodeId)) {
                if (EmitDeclNode(c, nodeId, 0, 1, 1, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
                    return -1;
                }
            }
            continue;
        }

        if (n->kind == SLAst_CONST) {
            if (EmitDeclNode(c, nodeId, 0, 0, !exported, 0) != 0
                || BufAppendChar(&c->out, '\n') != 0)
            {
                return -1;
            }
            continue;
        }

        if (EmitDeclNode(c, nodeId, 0, 0, !exported, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->fnSigLen; i++) {
        const SLFnSig*   sig = &c->fnSigs[i];
        const SLAstNode* fnNode;
        if ((sig->flags & SLFnSigFlag_TEMPLATE_INSTANCE) == 0) {
            continue;
        }
        fnNode = NodeAt(c, sig->nodeId);
        if (fnNode == NULL || fnNode->kind != SLAst_FN || !FnNodeHasBody(c, sig->nodeId)) {
            continue;
        }
        if (EmitFnDeclOrDef(c, sig->nodeId, 0, 1, 1, sig) != 0 || BufAppendChar(&c->out, '\n') != 0)
        {
            if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
                SetDiagNode(c, sig->nodeId, SLDiag_CODEGEN_INTERNAL);
            }
            return -1;
        }
    }

    if (BufAppendCStr(&c->out, "#endif /* ") != 0 || BufAppendCStr(&c->out, impl) != 0
        || BufAppendCStr(&c->out, " */\n\n#endif /* ") != 0 || BufAppendCStr(&c->out, guard) != 0
        || BufAppendCStr(&c->out, " */\n") != 0)
    {
        return -1;
    }
    return 0;
}

int ShouldEmitDeclNode(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n;
    if (c == NULL) {
        return 0;
    }
    n = NodeAt(c, nodeId);
    if (n == NULL) {
        return 0;
    }
    if (c->options != NULL && c->options->emitNodeStartOffsetEnabled != 0
        && n->start < c->options->emitNodeStartOffset)
    {
        if (n->kind == SLAst_FN) {
            const SLFnSig* sigs[SLCCG_MAX_CALL_CANDIDATES];
            uint32_t       nSigs = FindFnSigCandidatesByNodeId(
                c, nodeId, sigs, (uint32_t)(sizeof(sigs) / sizeof(sigs[0])));
            uint32_t i;
            if (nSigs > (uint32_t)(sizeof(sigs) / sizeof(sigs[0]))) {
                nSigs = (uint32_t)(sizeof(sigs) / sizeof(sigs[0]));
            }
            for (i = 0; i < nSigs; i++) {
                if ((sigs[i]->flags & SLFnSigFlag_TEMPLATE_INSTANCE) != 0) {
                    return 1;
                }
            }
        }
        return 0;
    }
    return 1;
}

int InitAst(SLCBackendC* c) {
    SLDiag diag = {};
    void* _Nullable allocatorCtx = NULL;
    SLArenaGrowFn _Nullable growFn = NULL;
    SLArenaFreeFn _Nullable freeFn = NULL;

    c->ast.nodes = NULL;
    c->ast.len = 0;
    c->ast.root = -1;
    if (c->options != NULL) {
        allocatorCtx = c->options->allocatorCtx;
        growFn = c->options->arenaGrow;
        freeFn = c->options->arenaFree;
    }
    SLArenaInitEx(
        &c->arena,
        c->arenaInlineStorage,
        (uint32_t)sizeof(c->arenaInlineStorage),
        allocatorCtx,
        growFn,
        freeFn);
    c->out.arena = &c->arena;
    if (SLParse(
            &c->arena,
            (SLStrView){ c->unit->source, c->unit->sourceLen },
            NULL,
            &c->ast,
            NULL,
            &diag)
        != 0)
    {
        if (c->diag != NULL) {
            *c->diag = diag;
        }
        return -1;
    }
    return 0;
}

char* _Nullable AllocOutputCopy(SLCBackendC* c) {
    uint32_t needSize;
    uint32_t allocSize = 0;
    char*    out;
    if (c->options == NULL || c->options->arenaGrow == NULL) {
        return NULL;
    }
    if (c->out.len > UINT32_MAX - 1u) {
        return NULL;
    }
    needSize = c->out.len + 1u;
    out = (char*)c->options->arenaGrow(c->options->allocatorCtx, needSize, &allocSize);
    if (out == NULL) {
        return NULL;
    }
    if (allocSize < needSize) {
        if (c->options->arenaFree != NULL) {
            c->options->arenaFree(c->options->allocatorCtx, out, allocSize);
        }
        return NULL;
    }
    if (c->out.v != NULL) {
        memcpy(out, c->out.v, needSize);
    } else {
        out[0] = '\0';
    }
    return out;
}

void FreeContext(SLCBackendC* c) {
    SLArenaDispose(&c->arena);
}

int EmitCBackend(
    const SLCodegenBackend* backend,
    const SLCodegenUnit*    unit,
    const SLCodegenOptions* _Nullable options,
    char** outHeader,
    SLDiag* _Nullable diag) {
    SLCBackendC c;
    (void)backend;

    memset(&c, 0, sizeof(c));
    c.unit = unit;
    c.options = options;
    c.diag = diag;
    c.activeCallWithNode = -1;
    TypeRefSetInvalid(&c.currentContextType);

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    *outHeader = NULL;

    if (InitAst(&c) != 0) {
        FreeContext(&c);
        return -1;
    }
    if (SLConstEvalSessionInit(
            &c.arena, &c.ast, (SLStrView){ c.unit->source, c.unit->sourceLen }, &c.constEval, diag)
        != 0)
    {
        FreeContext(&c);
        return -1;
    }
    if (CollectDeclSets(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectFnAndFieldInfo(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectTypeAliasInfo(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectFnTypeAliases(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            SetDiag(diag, SLDiag_CODEGEN_INTERNAL, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectVarSizeTypesFromDeclSets(&c) != 0 || PropagateVarSizeTypes(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectStringLiterals(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (EmitHeader(&c) != 0) {
        if (diag != NULL && diag->code == SLDiag_NONE) {
            SetDiag(diag, SLDiag_CODEGEN_INTERNAL, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }

    *outHeader = AllocOutputCopy(&c);
    if (*outHeader == NULL) {
        SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        FreeContext(&c);
        return -1;
    }

    FreeContext(&c);
    return 0;
}

const SLCodegenBackend gSLCodegenBackendC = {
    .name = "c",
    .emit = EmitCBackend,
};

SL_API_END
