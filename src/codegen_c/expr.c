#include "internal.h"
#include "../fmt_parse.h"

H2_API_BEGIN

static int TypeRefIsStringByteSequence(const H2TypeRef* t);
static int TypeRefIsBorrowedStrValue(const H2TypeRef* t);
static int EmitStrValueExpr(H2CBackendC* c, int32_t exprNode, const H2TypeRef* type);
static int EmitStrValueName(H2CBackendC* c, const char* name, const H2TypeRef* type);
static int EmitStrAddressExpr(H2CBackendC* c, int32_t exprNode, const H2TypeRef* type);

int IsTypeNodeKind(H2AstKind kind) {
    return kind == H2Ast_TYPE_NAME || kind == H2Ast_TYPE_PTR || kind == H2Ast_TYPE_REF
        || kind == H2Ast_TYPE_MUTREF || kind == H2Ast_TYPE_ARRAY || kind == H2Ast_TYPE_VARRAY
        || kind == H2Ast_TYPE_SLICE || kind == H2Ast_TYPE_MUTSLICE || kind == H2Ast_TYPE_OPTIONAL
        || kind == H2Ast_TYPE_FN || kind == H2Ast_TYPE_ANON_STRUCT || kind == H2Ast_TYPE_ANON_UNION
        || kind == H2Ast_TYPE_TUPLE;
}

int IsActivePackIdent(const H2CBackendC* c, uint32_t start, uint32_t end) {
    return c != NULL && c->activePackParamName != NULL
        && SliceEqName(c->unit->source, start, end, c->activePackParamName);
}

static int CallArgIsActivePackIdent(const H2CBackendC* c, const H2CCallArgInfo* arg) {
    const H2AstNode* n;
    if (c == NULL || arg == NULL || arg->exprNode < 0) {
        return 0;
    }
    n = NodeAt(c, arg->exprNode);
    return n != NULL && n->kind == H2Ast_IDENT && IsActivePackIdent(c, n->dataStart, n->dataEnd);
}

static int EmitActivePackElemNameCoerced(
    H2CBackendC* c, uint32_t packIndex, const H2TypeRef* dstType) {
    const char*      name;
    const H2TypeRef* elemType;
    uint8_t          cost = 0;
    if (c == NULL || dstType == NULL || !dstType->valid || c->activePackElemNames == NULL
        || c->activePackElemTypes == NULL || packIndex >= c->activePackElemCount
        || c->activePackElemNames[packIndex] == NULL)
    {
        return -1;
    }
    name = c->activePackElemNames[packIndex];
    elemType = &c->activePackElemTypes[packIndex];
    if (TypeRefAssignableCost(c, dstType, elemType, &cost) != 0) {
        return -1;
    }
    if (TypeRefIsBorrowedStrValue(dstType) && TypeRefIsStr(elemType)) {
        return EmitStrValueName(c, name, elemType);
    }
    if (TypeRefEqual(dstType, elemType)) {
        return BufAppendCStr(&c->out, name);
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
        || BufAppendCStr(&c->out, ")(") != 0 || BufAppendCStr(&c->out, name) != 0
        || BufAppendCStr(&c->out, "))") != 0)
    {
        return -1;
    }
    return 0;
}

int ResolveActivePackConstIndex(
    H2CBackendC* c, int32_t idxNode, uint32_t* outIndex, const H2TypeRef** outElemType) {
    int64_t value = 0;
    int     isConst = 0;
    if (c == NULL || outIndex == NULL || c->activePackElemCount == 0 || idxNode < 0) {
        return -1;
    }
    if (EvalConstIntExpr(c, idxNode, &value, &isConst) != 0 || !isConst || value < 0
        || (uint64_t)value >= (uint64_t)c->activePackElemCount)
    {
        return -1;
    }
    *outIndex = (uint32_t)value;
    if (outElemType != NULL && c->activePackElemTypes != NULL) {
        *outElemType = &c->activePackElemTypes[*outIndex];
    }
    return 0;
}

static int EmitBuiltinPanicCall(H2CBackendC* c, int32_t msgNode) {
    H2TypeRef msgType;
    if (c == NULL || msgNode < 0) {
        return -1;
    }
    if (InferExprType(c, msgNode, &msgType) != 0 || !msgType.valid || !TypeRefIsStr(&msgType)) {
        return -1;
    }
    if (c->unit != NULL && c->unit->usesPlatform) {
        if (BufAppendCStr(&c->out, "platform__panic(") != 0
            || EmitStrValueExpr(c, msgNode, &msgType) != 0
            || BufAppendCStr(&c->out, ", (__hop_i32)0)") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "__hop_panic(") != 0 || EmitStrAddressExpr(c, msgNode, &msgType) != 0
        || BufAppendCStr(&c->out, ", __FILE__, __LINE__)") != 0)
    {
        return -1;
    }
    return 0;
}

/* Returns 0 on success, 1 when exprNode is not an active-pack index expression, -1 on error. */
int ResolveActivePackIndexExpr(
    H2CBackendC* c,
    int32_t      exprNode,
    int32_t*     outIdxNode,
    int*         outIsConst,
    uint32_t*    outConstIndex) {
    const H2AstNode* n;
    int32_t          baseNode;
    int32_t          idxNode;
    int32_t          extraNode;
    const H2AstNode* baseAst;
    int64_t          idxValue = 0;
    int              idxIsConst = 0;

    if (c == NULL || outIdxNode == NULL || outIsConst == NULL || outConstIndex == NULL) {
        return -1;
    }
    *outIdxNode = -1;
    *outIsConst = 0;
    *outConstIndex = 0;

    n = NodeAt(c, exprNode);
    if (n != NULL && n->kind == H2Ast_CALL_ARG) {
        exprNode = AstFirstChild(&c->ast, exprNode);
        n = NodeAt(c, exprNode);
    }
    if (n == NULL || n->kind != H2Ast_INDEX || (n->flags & H2AstFlag_INDEX_SLICE) != 0) {
        return 1;
    }
    baseNode = AstFirstChild(&c->ast, exprNode);
    idxNode = AstNextSibling(&c->ast, baseNode);
    extraNode = idxNode >= 0 ? AstNextSibling(&c->ast, idxNode) : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
        return 1;
    }
    baseAst = NodeAt(c, baseNode);
    if (baseAst == NULL || baseAst->kind != H2Ast_IDENT
        || !IsActivePackIdent(c, baseAst->dataStart, baseAst->dataEnd))
    {
        return 1;
    }
    if (EvalConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
        return -1;
    }
    if (idxIsConst) {
        if (idxValue < 0 || (uint64_t)idxValue >= (uint64_t)c->activePackElemCount) {
            return -1;
        }
        *outIdxNode = idxNode;
        *outIsConst = 1;
        *outConstIndex = (uint32_t)idxValue;
        return 0;
    }
    *outIdxNode = idxNode;
    *outIsConst = 0;
    return 0;
}

int EmitDynamicActivePackIndexCoerced(
    H2CBackendC* c, int32_t idxNode, const H2TypeRef* _Nullable dstType) {
    uint32_t tempId;
    H2Buf    idxNameBuf = { 0 };
    H2Buf    valueNameBuf = { 0 };
    char*    idxName;
    char*    valueName;
    uint32_t i;
    if (c == NULL || idxNode < 0 || dstType == NULL || !dstType->valid) {
        return -1;
    }
    tempId = FmtNextTempId(c);
    idxNameBuf.arena = &c->arena;
    valueNameBuf.arena = &c->arena;
    if (BufAppendCStr(&idxNameBuf, "__hop_pack_i") != 0 || BufAppendU32(&idxNameBuf, tempId) != 0
        || BufAppendCStr(&valueNameBuf, "__hop_pack_v") != 0
        || BufAppendU32(&valueNameBuf, tempId) != 0)
    {
        return -1;
    }
    idxName = BufFinish(&idxNameBuf);
    valueName = BufFinish(&valueNameBuf);
    if (idxName == NULL || valueName == NULL) {
        return -1;
    }

    if (BufAppendCStr(&c->out, "(__extension__({ __hop_uint ") != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendCStr(&c->out, " = (__hop_uint)(") != 0
        || EmitExpr(c, idxNode) != 0 || BufAppendCStr(&c->out, "); ") != 0)
    {
        return -1;
    }
    if (EmitTypeNameWithDepth(c, dstType) != 0 || BufAppendChar(&c->out, ' ') != 0
        || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; switch (") != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendCStr(&c->out, ") { ") != 0)
    {
        return -1;
    }

    for (i = 0; i < c->activePackElemCount; i++) {
        uint8_t cost = 0;
        if (c->activePackElemNames == NULL || c->activePackElemNames[i] == NULL) {
            return -1;
        }
        if (TypeRefAssignableCost(c, dstType, &c->activePackElemTypes[i], &cost) == 0) {
            if (BufAppendCStr(&c->out, "case ") != 0 || BufAppendU32(&c->out, i) != 0
                || BufAppendCStr(&c->out, "u: ") != 0 || BufAppendCStr(&c->out, valueName) != 0
                || BufAppendCStr(&c->out, " = ") != 0)
            {
                return -1;
            }
            if (TypeRefIsBorrowedStrValue(dstType) && TypeRefIsStr(&c->activePackElemTypes[i])) {
                if (EmitStrValueName(c, c->activePackElemNames[i], &c->activePackElemTypes[i]) != 0
                    || BufAppendCStr(&c->out, "; break; ") != 0)
                {
                    return -1;
                }
            } else if (
                BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(") != 0
                || BufAppendCStr(&c->out, c->activePackElemNames[i]) != 0
                || BufAppendCStr(&c->out, ")); break; ") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "case ") != 0 || BufAppendU32(&c->out, i) != 0
                || BufAppendCStr(
                       &c->out,
                       "u: __hop_panic(__hop_strlit(\"anytype pack element type mismatch\"), "
                       "__FILE__, __LINE__); __builtin_unreachable(); ")
                       != 0)
            {
                return -1;
            }
        }
    }
    if (BufAppendCStr(
            &c->out,
            "default: __hop_panic(__hop_strlit(\"anytype pack index out of bounds\"), __FILE__, "
            "__LINE__); __builtin_unreachable(); } ")
            != 0
        || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; }))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitDynamicActivePackTypeTag(H2CBackendC* c, int32_t idxNode) {
    uint32_t tempId;
    H2Buf    idxNameBuf = { 0 };
    H2Buf    valueNameBuf = { 0 };
    char*    idxName;
    char*    valueName;
    uint32_t i;
    if (c == NULL || idxNode < 0) {
        return -1;
    }
    tempId = FmtNextTempId(c);
    idxNameBuf.arena = &c->arena;
    valueNameBuf.arena = &c->arena;
    if (BufAppendCStr(&idxNameBuf, "__hop_pack_ti") != 0 || BufAppendU32(&idxNameBuf, tempId) != 0
        || BufAppendCStr(&valueNameBuf, "__hop_pack_tv") != 0
        || BufAppendU32(&valueNameBuf, tempId) != 0)
    {
        return -1;
    }
    idxName = BufFinish(&idxNameBuf);
    valueName = BufFinish(&valueNameBuf);
    if (idxName == NULL || valueName == NULL) {
        return -1;
    }

    if (BufAppendCStr(&c->out, "(__extension__({ __hop_uint ") != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendCStr(&c->out, " = (__hop_uint)(") != 0
        || EmitExpr(c, idxNode) != 0 || BufAppendCStr(&c->out, "); __hop_type ") != 0
        || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; switch (") != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendCStr(&c->out, ") { ") != 0)
    {
        return -1;
    }
    for (i = 0; i < c->activePackElemCount; i++) {
        if (c->activePackElemTypes == NULL) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "case ") != 0 || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(&c->out, "u: ") != 0 || BufAppendCStr(&c->out, valueName) != 0
            || BufAppendCStr(&c->out, " = ") != 0
            || EmitTypeTagLiteralFromTypeRef(c, &c->activePackElemTypes[i]) != 0
            || BufAppendCStr(&c->out, "; break; ") != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(
            &c->out,
            "default: __hop_panic(__hop_strlit(\"anytype pack index out of bounds\"), __FILE__, "
            "__LINE__); __builtin_unreachable(); } ")
            != 0
        || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; }))") != 0)
    {
        return -1;
    }
    return 0;
}

int DecodeNewExprNodes(
    H2CBackendC* c,
    int32_t      nodeId,
    int32_t*     outTypeNode,
    int32_t*     outCountNode,
    int32_t*     outInitNode,
    int32_t*     outAllocNode) {
    const H2AstNode* n = NodeAt(c, nodeId);
    int32_t          typeNode;
    int32_t          nextNode;
    int              hasCount;
    int              hasInit;
    int              hasAlloc;
    if (n == NULL || n->kind != H2Ast_NEW) {
        return -1;
    }
    hasCount = (n->flags & H2AstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & H2AstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & H2AstFlag_NEW_HAS_ALLOC) != 0;

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

uint32_t ListCount(const H2Ast* ast, int32_t listNode) {
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

int32_t ListItemAt(const H2Ast* ast, int32_t listNode, uint32_t index) {
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

int ResolveVarLikeParts(H2CBackendC* c, int32_t nodeId, H2CCGVarLikeParts* out) {
    int32_t          firstChild = AstFirstChild(&c->ast, nodeId);
    const H2AstNode* firstNode;
    out->nameListNode = -1;
    out->typeNode = -1;
    out->initNode = -1;
    out->nameCount = 0;
    out->grouped = 0;
    if (firstChild < 0) {
        return 0;
    }
    firstNode = NodeAt(c, firstChild);
    if (firstNode != NULL && firstNode->kind == H2Ast_NAME_LIST) {
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

int InferVarLikeDeclType(H2CBackendC* c, int32_t initNode, H2TypeRef* outType) {
    if (initNode < 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (InferExprType(c, initNode, outType) != 0 || !outType->valid) {
        return -1;
    }
    if (outType->containerKind == H2TypeContainer_SCALAR && outType->containerPtrDepth == 0
        && outType->ptrDepth == 0 && !outType->isOptional && outType->baseName != NULL
        && StrEq(outType->baseName, "__hop_i32"))
    {
        outType->baseName = "__hop_int";
    }
    return 0;
}

int FindTopLevelVarLikeNodeBySliceEx(
    const H2CBackendC* c,
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
        const H2AstNode* n = NodeAt(c, nodeId);
        if (n != NULL && (n->kind == H2Ast_VAR || n->kind == H2Ast_CONST)) {
            if (SliceSpanEq(c->unit->source, n->dataStart, n->dataEnd, start, end)) {
                *outNodeId = nodeId;
                if (outNameIndex != NULL) {
                    *outNameIndex = 0;
                }
                return 0;
            }
            {
                int32_t          firstChild = AstFirstChild(&c->ast, nodeId);
                const H2AstNode* firstNode = NodeAt(c, firstChild);
                if (firstNode != NULL && firstNode->kind == H2Ast_NAME_LIST) {
                    uint32_t j;
                    uint32_t nameCount = ListCount(&c->ast, firstChild);
                    for (j = 0; j < nameCount; j++) {
                        int32_t          nameNode = ListItemAt(&c->ast, firstChild, j);
                        const H2AstNode* name = NodeAt(c, nameNode);
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
    const H2CBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId) {
    return FindTopLevelVarLikeNodeBySliceEx(c, start, end, outNodeId, NULL);
}

int InferTopLevelVarLikeType(
    H2CBackendC* c, int32_t nodeId, uint32_t nameStart, uint32_t nameEnd, H2TypeRef* outType) {
    H2CCGVarLikeParts parts;
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
        && NodeAt(c, parts.initNode)->kind == H2Ast_EXPR_LIST)
    {
        uint32_t i;
        uint32_t initCount = ListCount(&c->ast, parts.initNode);
        for (i = 0; i < parts.nameCount && i < initCount; i++) {
            int32_t          nameNode = ListItemAt(&c->ast, parts.nameListNode, i);
            const H2AstNode* name = NodeAt(c, nameNode);
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
    H2CBackendC* c, uint32_t start, uint32_t end, H2TypeRef* outType) {
    int32_t           nodeId = -1;
    int32_t           nameIndex = -1;
    const H2AstNode*  n;
    H2CCGVarLikeParts parts;
    int32_t           initNode = -1;
    H2CTFEValue       value;
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
    if (n == NULL || n->kind != H2Ast_CONST) {
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
        && NodeAt(c, parts.initNode)->kind == H2Ast_EXPR_LIST
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
    if (H2ConstEvalSessionEvalExpr(c->constEval, initNode, &value, &isConst) != 0) {
        return 0;
    }
    if (!isConst || value.kind != H2CTFEValue_TYPE) {
        return 0;
    }
    if (ParseTypeRefFromConstEvalTypeTag(c, value.typeTag, outType) != 0 || !outType->valid) {
        TypeRefSetInvalid(outType);
        return 0;
    }
    return 1;
}

int TypeRefEqual(const H2TypeRef* a, const H2TypeRef* b) {
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

int ExpandAliasSourceType(const H2CBackendC* c, const H2TypeRef* src, H2TypeRef* outExpanded) {
    const H2TypeAliasInfo* alias;
    if (!src->valid || src->baseName == NULL) {
        return 0;
    }
    alias = FindTypeAliasInfoByAliasName(c, src->baseName);
    if (alias == NULL) {
        return 0;
    }

    /* Wrapped alias: preserve wrappers only when alias target is scalar. */
    if ((src->ptrDepth > 0 || src->containerPtrDepth > 0 || src->isOptional)
        && alias->targetType.containerKind == H2TypeContainer_SCALAR
        && alias->targetType.ptrDepth == 0 && alias->targetType.containerPtrDepth == 0
        && alias->targetType.baseName != NULL)
    {
        *outExpanded = *src;
        outExpanded->baseName = alias->targetType.baseName;
        return 1;
    }

    /* Unwrapped alias can expand to any target type form. */
    if (src->containerKind == H2TypeContainer_SCALAR && src->ptrDepth == 0
        && src->containerPtrDepth == 0 && !src->isOptional)
    {
        *outExpanded = alias->targetType;
        return 1;
    }
    return 0;
}

static int TypeRefIsFunctionAlias(const H2CBackendC* c, const H2TypeRef* type) {
    return type != NULL && type->valid && type->containerKind == H2TypeContainer_SCALAR
        && type->ptrDepth == 0 && type->containerPtrDepth == 0 && type->baseName != NULL
        && FindFnTypeAliasByName(c, type->baseName) != NULL;
}

int TypeRefAssignableCost(
    H2CBackendC* c, const H2TypeRef* dst, const H2TypeRef* src, uint8_t* outCost) {
    const H2FieldInfo* path[64];
    uint32_t           pathLen = 0;
    H2TypeRef          expandedSrc;
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
        H2TypeRef inner = *dst;
        inner.isOptional = 0;
        if (TypeRefAssignableCost(c, &inner, src, outCost) == 0) {
            *outCost = 4;
            return 0;
        }
        return -1;
    }
    if (dst->isOptional && src->isOptional) {
        H2TypeRef d = *dst;
        H2TypeRef s = *src;
        d.isOptional = 0;
        s.isOptional = 0;
        return TypeRefAssignableCost(c, &d, &s, outCost);
    }
    if (!dst->isOptional && src->isOptional) {
        return -1;
    }

    if (dst->containerKind == H2TypeContainer_ARRAY && src->containerKind == H2TypeContainer_ARRAY
        && dst->containerPtrDepth == src->containerPtrDepth && dst->ptrDepth == src->ptrDepth
        && dst->baseName != NULL && src->baseName != NULL && StrEq(dst->baseName, src->baseName)
        && dst->hasArrayLen && src->hasArrayLen && src->arrayLen <= dst->arrayLen)
    {
        *outCost = dst->arrayLen == src->arrayLen ? 0 : 1;
        return 0;
    }

    if (dst->containerKind == H2TypeContainer_SLICE_RO
        || dst->containerKind == H2TypeContainer_SLICE_MUT)
    {
        const char* srcBase = ResolveScalarAliasBaseName(c, src->baseName);
        if (srcBase == NULL) {
            srcBase = src->baseName;
        }
        if (src->containerKind == H2TypeContainer_SCALAR && src->containerPtrDepth == 0
            && src->ptrDepth > 0 && IsStrBaseName(srcBase) && dst->baseName != NULL
            && StrEq(dst->baseName, "__hop_u8"))
        {
            if (dst->containerKind == H2TypeContainer_SLICE_MUT && src->readOnly) {
                return -1;
            }
            *outCost = 1;
            return 0;
        }
        if ((src->containerKind == H2TypeContainer_SLICE_RO
             || src->containerKind == H2TypeContainer_SLICE_MUT)
            && dst->containerPtrDepth == src->containerPtrDepth && dst->ptrDepth == src->ptrDepth
            && dst->baseName != NULL && src->baseName != NULL
            && StrEq(dst->baseName, src->baseName))
        {
            if (dst->containerKind == H2TypeContainer_SLICE_RO
                && src->containerKind == H2TypeContainer_SLICE_MUT)
            {
                *outCost = 1;
            } else if (dst->containerKind == src->containerKind) {
                *outCost = 0;
            } else {
                return -1;
            }
            return 0;
        }
        if (src->containerKind == H2TypeContainer_ARRAY && dst->ptrDepth == src->ptrDepth
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

int ExprNeedsExpectedType(const H2CBackendC* c, int32_t exprNode) {
    const H2AstNode* n = NodeAt(c, exprNode);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = AstFirstChild(&c->ast, exprNode);
        return ExprNeedsExpectedType(c, inner);
    }
    if (n->kind == H2Ast_COMPOUND_LIT) {
        return 1;
    }
    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, exprNode);
        const H2AstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == H2Ast_COMPOUND_LIT) {
            int32_t          rhsChild = AstFirstChild(&c->ast, rhsNode);
            const H2AstNode* rhsTypeNode = NodeAt(c, rhsChild);
            return !(rhsTypeNode != NULL && IsTypeNodeKind(rhsTypeNode->kind));
        }
    }
    return 0;
}

int ExprCanRetryWithExpectedType(const H2CBackendC* c, int32_t exprNode) {
    const H2AstNode* n = NodeAt(c, exprNode);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = AstFirstChild(&c->ast, exprNode);
        return ExprCanRetryWithExpectedType(c, inner);
    }
    if (n->kind == H2Ast_INT || n->kind == H2Ast_RUNE || n->kind == H2Ast_FLOAT) {
        return 1;
    }
    if (n->kind == H2Ast_UNARY
        && ((H2TokenKind)n->op == H2Tok_ADD || (H2TokenKind)n->op == H2Tok_SUB))
    {
        int32_t          inner = AstFirstChild(&c->ast, exprNode);
        const H2AstNode* innerNode = NodeAt(c, inner);
        return innerNode != NULL
            && (innerNode->kind == H2Ast_INT || innerNode->kind == H2Ast_RUNE
                || innerNode->kind == H2Ast_FLOAT);
    }
    return 0;
}

int32_t UnwrapCallArgExprNode(const H2CBackendC* c, int32_t argNode) {
    const H2AstNode* n = NodeAt(c, argNode);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == H2Ast_CALL_ARG) {
        return AstFirstChild(&c->ast, argNode);
    }
    return argNode;
}

int CollectCallArgInfo(
    H2CBackendC*    c,
    int32_t         callNode,
    int32_t         calleeNode,
    int             includeReceiver,
    int32_t         receiverNode,
    H2CCallArgInfo* outArgs,
    H2TypeRef*      outArgTypes,
    uint32_t*       outArgCount) {
    int32_t  argNode = AstNextSibling(&c->ast, calleeNode);
    uint32_t argCount = 0;
    (void)callNode;
    if (includeReceiver) {
        if (argCount >= H2CCG_MAX_CALL_ARGS) {
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
        const H2AstNode* arg = NodeAt(c, argNode);
        int32_t          exprNode = UnwrapCallArgExprNode(c, argNode);
        if (argCount >= H2CCG_MAX_CALL_ARGS) {
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
            (uint8_t)(((arg->flags & H2AstFlag_CALL_ARG_SPREAD) != 0) ? 1 : 0);
        outArgs[argCount]._reserved[0] = 0;
        outArgs[argCount]._reserved[1] = 0;
        outArgs[argCount]._reserved[2] = 0;
        if (arg->dataEnd > arg->dataStart) {
            outArgs[argCount].explicitNameStart = arg->dataStart;
            outArgs[argCount].explicitNameEnd = arg->dataEnd;
        } else {
            const H2AstNode* expr = NodeAt(c, exprNode);
            if (expr != NULL && expr->kind == H2Ast_IDENT) {
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
    const H2CBackendC* c,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2FnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound) {
    const H2FnSig*   candidates[H2CCG_MAX_CALL_CANDIDATES];
    const H2FnSig*   byName[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t         candidateLen = 0;
    int              nameFound = 0;
    int              hasTemplateInstance = 0;
    uint32_t         i, j;
    const H2NameMap* mappedName = NULL;

    i = FindFnSigCandidatesBySlice(
        c, nameStart, nameEnd, byName, (uint32_t)(sizeof(byName) / sizeof(byName[0])));
    if (i > 0) {
        nameFound = 1;
        if (i > (uint32_t)(sizeof(byName) / sizeof(byName[0]))) {
            i = (uint32_t)(sizeof(byName) / sizeof(byName[0]));
        }
        for (j = 0; j < i && candidateLen < H2CCG_MAX_CALL_CANDIDATES; j++) {
            if ((byName[j]->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0) {
                hasTemplateInstance = 1;
            }
        }
        for (j = 0; j < i && candidateLen < H2CCG_MAX_CALL_CANDIDATES; j++) {
            if (hasTemplateInstance && (byName[j]->flags & H2FnSigFlag_TEMPLATE_BASE) != 0) {
                continue;
            }
            candidates[candidateLen++] = byName[j];
        }
    }
    mappedName = FindNameBySlice(c, nameStart, nameEnd);
    if (mappedName != NULL && mappedName->cName != NULL) {
        for (i = 0; i < c->fnSigLen && candidateLen < H2CCG_MAX_CALL_CANDIDATES; i++) {
            const H2FnSig* sig = &c->fnSigs[i];
            int            dup = 0;
            uint32_t       mappedLen = (uint32_t)StrLen(mappedName->cName);
            int            cNameMatches =
                StrEq(sig->cName, mappedName->cName)
                || (StrHasPrefix(sig->cName, mappedName->cName)
                    && StrHasPrefix(sig->cName + mappedLen, "__ti"));
            if (!StrEq(sig->hopName, mappedName->cName) && !cNameMatches) {
                continue;
            }
            for (j = 0; j < candidateLen; j++) {
                if (candidates[j] == sig) {
                    dup = 1;
                    break;
                }
            }
            if (dup) {
                continue;
            }
            if ((sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0) {
                hasTemplateInstance = 1;
            }
            candidates[candidateLen++] = sig;
            nameFound = 1;
        }
    }
    for (i = 0; i < c->fnSigLen && candidateLen < H2CCG_MAX_CALL_CANDIDATES; i++) {
        const H2FnSig* sig = &c->fnSigs[i];
        uint32_t       nameLen;
        uint32_t       candLen;
        if (nameEnd <= nameStart || sig->hopName == NULL) {
            continue;
        }
        nameLen = nameEnd - nameStart;
        candLen = (uint32_t)StrLen(sig->hopName);
        if (candLen != 9u + nameLen) {
            continue;
        }
        if (memcmp(sig->hopName, "builtin__", 9u) != 0) {
            continue;
        }
        if (memcmp(sig->hopName + 9u, c->unit->source + nameStart, nameLen) == 0) {
            if ((sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0) {
                hasTemplateInstance = 1;
            }
            candidates[candidateLen++] = sig;
            nameFound = 1;
        }
    }
    if (hasTemplateInstance) {
        uint32_t out = 0;
        for (i = 0; i < candidateLen; i++) {
            if ((candidates[i]->flags & H2FnSigFlag_TEMPLATE_BASE) != 0) {
                continue;
            }
            candidates[out++] = candidates[i];
        }
        candidateLen = out;
    }
    for (i = 0; i < candidateLen; i++) {
        outCandidates[i] = candidates[i];
    }
    *outCandidateLen = candidateLen;
    *outNameFound = nameFound;
}

void GatherCallCandidatesByPkgMethod(
    const H2CBackendC* c,
    uint32_t           pkgStart,
    uint32_t           pkgEnd,
    uint32_t           methodStart,
    uint32_t           methodEnd,
    const H2FnSig**    outCandidates,
    uint32_t*          outCandidateLen,
    int*               outNameFound) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    int            nameFound = 0;
    int            hasTemplateInstance = 0;
    uint32_t       i;
    for (i = 0; i < c->fnSigLen && candidateLen < H2CCG_MAX_CALL_CANDIDATES; i++) {
        if (NameEqPkgPrefixedMethod(
                c->fnSigs[i].hopName, c->unit->source, pkgStart, pkgEnd, methodStart, methodEnd))
        {
            if ((c->fnSigs[i].flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0) {
                hasTemplateInstance = 1;
            }
            candidates[candidateLen++] = &c->fnSigs[i];
            nameFound = 1;
        }
    }
    if (hasTemplateInstance) {
        uint32_t out = 0;
        for (i = 0; i < candidateLen; i++) {
            if ((candidates[i]->flags & H2FnSigFlag_TEMPLATE_BASE) != 0) {
                continue;
            }
            candidates[out++] = candidates[i];
        }
        candidateLen = out;
    }
    for (i = 0; i < candidateLen; i++) {
        outCandidates[i] = candidates[i];
    }
    *outCandidateLen = candidateLen;
    *outNameFound = nameFound;
}

static const H2FnSig* _Nullable FindSingleTemplateInstanceCandidate(
    const H2FnSig* const* candidates, uint32_t candidateLen, uint32_t argCount) {
    const H2FnSig* single = NULL;
    uint32_t       i;
    if (candidates == NULL) {
        return NULL;
    }
    for (i = 0; i < candidateLen; i++) {
        const H2FnSig* sig = candidates[i];
        if (sig == NULL || (sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0 || sig->isVariadic != 0
            || sig->paramLen != argCount)
        {
            continue;
        }
        if (single != NULL) {
            return NULL;
        }
        single = sig;
    }
    return single;
}

static const H2FnSig* _Nullable FindSingleVariadicFnSigBySlice(
    const H2CBackendC* c, uint32_t nameStart, uint32_t nameEnd) {
    const H2NameMap* mappedName = NULL;
    const H2FnSig*   single = NULL;
    uint32_t         i;
    if (c == NULL) {
        return NULL;
    }
    mappedName = FindNameBySlice(c, nameStart, nameEnd);
    for (i = 0; i < c->fnSigLen; i++) {
        const H2FnSig* sig = &c->fnSigs[i];
        int            matches;
        if (sig->isVariadic == 0) {
            continue;
        }
        matches = SliceEqName(c->unit->source, nameStart, nameEnd, sig->hopName);
        if (!matches && mappedName != NULL && mappedName->cName != NULL) {
            matches = StrEq(sig->hopName, mappedName->cName)
                   || StrEq(sig->cName, mappedName->cName);
        }
        if (!matches) {
            continue;
        }
        if (single != NULL) {
            return NULL;
        }
        single = sig;
    }
    return single;
}

static void BindPositionalTemplateInstanceFallback(
    const H2FnSig* sig, const int32_t* argNodes, uint32_t argCount, H2CCallBinding* outBinding) {
    uint32_t i;
    if (outBinding == NULL) {
        return;
    }
    memset(outBinding, 0, sizeof(*outBinding));
    outBinding->isVariadic = 0;
    outBinding->fixedCount = argCount;
    outBinding->fixedInputCount = argCount;
    outBinding->spreadArgIndex = UINT32_MAX;
    for (i = 0; i < argCount; i++) {
        outBinding->fixedMappedArgNodes[i] = argNodes[i];
        outBinding->argParamIndices[i] = (int32_t)i;
        outBinding->argExpectedTypes[i] = sig->paramTypes[i];
    }
}

static int CGParamNameStartsWithUnderscore(const H2FnSig* sig, uint32_t paramIndex) {
    const char* pn;
    if (sig == NULL || sig->paramNames == NULL || paramIndex >= sig->paramLen) {
        return 0;
    }
    pn = sig->paramNames[paramIndex];
    return pn != NULL && pn[0] == '_';
}

static uint32_t CGPositionalCallPrefixEnd(
    const H2FnSig* sig, uint32_t paramCount, uint32_t firstPositionalArgIndex) {
    uint32_t prefixEnd;
    if (sig == NULL || firstPositionalArgIndex >= paramCount) {
        return paramCount;
    }
    prefixEnd = firstPositionalArgIndex + 1u;
    while (prefixEnd < paramCount && CGParamNameStartsWithUnderscore(sig, prefixEnd)) {
        prefixEnd++;
    }
    return prefixEnd;
}

int MapCallArgsToParams(
    const H2CBackendC*    c,
    const H2FnSig*        sig,
    const H2CCallArgInfo* callArgs,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int32_t*              outMappedArgNodes,
    H2TypeRef*            outMappedArgTypes,
    const H2TypeRef*      argTypes) {
    uint8_t  assigned[H2CCG_MAX_CALL_ARGS];
    uint32_t positionalPrefixEnd;
    uint32_t i;
    if (argCount > sig->paramLen || argCount > H2CCG_MAX_CALL_ARGS) {
        return -1;
    }
    memset(assigned, 0, sizeof(assigned));
    for (i = 0; i < argCount; i++) {
        outMappedArgNodes[i] = -1;
        TypeRefSetInvalid(&outMappedArgTypes[i]);
    }
    positionalPrefixEnd = CGPositionalCallPrefixEnd(sig, argCount, firstPositionalArgIndex);
    if (firstPositionalArgIndex < argCount) {
        outMappedArgNodes[firstPositionalArgIndex] = callArgs[firstPositionalArgIndex].exprNode;
        outMappedArgTypes[firstPositionalArgIndex] = argTypes[firstPositionalArgIndex];
        assigned[firstPositionalArgIndex] = 1;
    }
    for (i = 0; i < argCount; i++) {
        const H2CCallArgInfo* a = &callArgs[i];
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
        } else if (i < positionalPrefixEnd) {
            outMappedArgNodes[i] = a->exprNode;
            outMappedArgTypes[i] = argTypes[i];
            assigned[i] = 1;
            continue;
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
    H2CBackendC*          c,
    int32_t               callNode,
    const char*           calleeName,
    const H2FnSig*        sig,
    const H2CCallBinding* binding,
    int                   autoRefFirstArg);

int PrepareCallBinding(
    const H2CBackendC*    c,
    const H2FnSig*        sig,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   allowNamedMapping,
    H2CCallBinding*       out) {
    uint32_t i;
    uint32_t spreadArgIndex = UINT32_MAX;
    uint32_t fixedCount;
    uint32_t fixedInputCount;
    if (sig == NULL || out == NULL) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    out->isVariadic = sig->isVariadic != 0;
    out->activePackSpread = 0;
    out->spreadArgIndex = UINT32_MAX;
    out->activePackSpreadArgIndex = UINT32_MAX;
    out->activePackSpreadParamStart = UINT32_MAX;
    for (i = 0; i < H2CCG_MAX_CALL_ARGS; i++) {
        out->fixedMappedArgNodes[i] = -1;
        out->explicitTailNodes[i] = -1;
        out->argParamIndices[i] = -1;
        TypeRefSetInvalid(&out->argExpectedTypes[i]);
    }
    if (argCount > H2CCG_MAX_CALL_ARGS || sig->paramLen > H2CCG_MAX_CALL_ARGS) {
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
        if (spreadArgIndex != UINT32_MAX) {
            uint32_t expandedParamLen;
            if (spreadArgIndex + 1u != argCount || c == NULL || c->activePackParamName == NULL
                || !CallArgIsActivePackIdent(c, &callArgs[spreadArgIndex])
                || callArgs[spreadArgIndex].explicitNameEnd
                       > callArgs[spreadArgIndex].explicitNameStart)
            {
                return -1;
            }
            expandedParamLen = argCount - 1u + c->activePackElemCount;
            if (expandedParamLen != sig->paramLen || spreadArgIndex > sig->paramLen) {
                return -1;
            }
            out->activePackSpread = 1;
            out->activePackSpreadArgIndex = spreadArgIndex;
            out->activePackSpreadParamStart = spreadArgIndex;
            out->fixedCount = sig->paramLen;
            out->fixedInputCount = argCount - 1u;
            out->spreadArgIndex = spreadArgIndex;
            for (i = 0; i < spreadArgIndex; i++) {
                out->fixedMappedArgNodes[i] = argNodes[i];
                out->argParamIndices[i] = (int32_t)i;
                out->argExpectedTypes[i] = sig->paramTypes[i];
            }
            out->argParamIndices[spreadArgIndex] = (int32_t)spreadArgIndex;
            out->argExpectedTypes[spreadArgIndex] = sig->paramTypes[spreadArgIndex];
            return 0;
        }
        if (argCount != sig->paramLen) {
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
            H2FnSig fixedSig = *sig;
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
        H2TypeRef elemType = sig->paramTypes[fixedCount];
        elemType.containerKind = H2TypeContainer_SCALAR;
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
    H2CBackendC*          c,
    const H2FnSig*        sig,
    const int32_t*        argNodes,
    uint32_t              argCount,
    const H2CCallBinding* binding) {
    uint32_t i;
    if (c == NULL || sig == NULL || argNodes == NULL || binding == NULL || c->constEval == NULL
        || sig->paramFlags == NULL)
    {
        return 1;
    }
    for (i = 0; i < argCount; i++) {
        int32_t     p = binding->argParamIndices[i];
        int         isConst = 0;
        H2CTFEValue ignoredValue = { 0 };
        if (p < 0 || (uint32_t)p >= sig->paramLen) {
            continue;
        }
        if ((sig->paramFlags[p] & H2CCGParamFlag_CONST) == 0) {
            continue;
        }
        if (H2ConstEvalSessionEvalExpr(c->constEval, argNodes[i], &ignoredValue, &isConst) != 0) {
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
    H2CBackendC*          c,
    const H2FnSig**       candidates,
    uint32_t              candidateLen,
    int                   nameFound,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding* _Nullable outBinding,
    const H2FnSig** outSig,
    const char**    outCalleeName) {
    const H2FnSig* bestSig = NULL;
    const char*    bestName = NULL;
    uint8_t        bestCosts[H2CCG_MAX_CALL_ARGS];
    H2CCallBinding bestBinding;
    uint32_t       bestTotal = 0;
    int            ambiguous = 0;
    H2TypeRef      autoRefType;
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
            if (autoRefType.containerKind == H2TypeContainer_SCALAR) {
                autoRefType.ptrDepth++;
            } else {
                autoRefType.containerPtrDepth++;
            }
            hasAutoRefType = 1;
        }
    }

    for (i = 0; i < candidateLen; i++) {
        const H2FnSig* sig = candidates[i];
        uint8_t        costs[H2CCG_MAX_CALL_ARGS];
        H2CCallBinding binding;
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
            H2TypeRef argType;
            uint8_t   cost = 0;
            H2TypeRef paramType = binding.argExpectedTypes[p];
            if (binding.activePackSpread && p == binding.activePackSpreadArgIndex) {
                uint32_t k;
                if ((c->activePackElemCount > 0 && c->activePackElemTypes == NULL)
                    || binding.activePackSpreadParamStart + c->activePackElemCount > sig->paramLen)
                {
                    viable = 0;
                    break;
                }
                costs[p] = 0;
                for (k = 0; k < c->activePackElemCount; k++) {
                    uint8_t elemCost = 0;
                    paramType = sig->paramTypes[binding.activePackSpreadParamStart + k];
                    if (!paramType.valid
                        || TypeRefAssignableCost(
                               c, &paramType, &c->activePackElemTypes[k], &elemCost)
                               != 0)
                    {
                        viable = 0;
                        break;
                    }
                    if (elemCost > costs[p]) {
                        costs[p] = elemCost;
                    }
                    total += elemCost;
                }
                if (!viable) {
                    break;
                }
                continue;
            }
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
                if (ExprCanRetryWithExpectedType(c, argNodes[p])
                    && InferExprTypeExpected(c, argNodes[p], &paramType, &argType) == 0
                    && argType.valid && TypeRefAssignableCost(c, &paramType, &argType, &cost) == 0)
                {
                    if (cost < 255u) {
                        cost++;
                    }
                } else {
                    viable = 0;
                    break;
                }
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

/* Returns 0 on success (exactly one dynamic active-pack index arg found), 1 when none found,
 * 2 when multiple dynamic active-pack index args are present, -1 on error. */
int FindSingleDynamicActivePackCallArg(
    H2CBackendC*   c,
    const int32_t* argNodes,
    uint32_t       argCount,
    uint32_t*      outArgIndex,
    int32_t*       outIdxNode) {
    uint32_t i;
    int      found = 0;
    if (c == NULL || argNodes == NULL || outArgIndex == NULL || outIdxNode == NULL) {
        return -1;
    }
    *outArgIndex = 0;
    *outIdxNode = -1;
    for (i = 0; i < argCount; i++) {
        int32_t  idxNode = -1;
        int      isConstIndex = 0;
        uint32_t constIndex = 0;
        int rc = ResolveActivePackIndexExpr(c, argNodes[i], &idxNode, &isConstIndex, &constIndex);
        if (rc < 0) {
            return -1;
        }
        if (rc == 0 && !isConstIndex) {
            if (found) {
                return 2;
            }
            found = 1;
            *outArgIndex = i;
            *outIdxNode = idxNode;
        }
    }
    return found ? 0 : 1;
}

int MaterializeTemplateInstanceForDispatchCase(
    H2CBackendC*          c,
    const H2FnSig*        baseSig,
    const H2CCallBinding* binding,
    const H2TypeRef*      caseArgTypes,
    uint32_t              argCount) {
    H2TypeRef* paramTypes;
    uint8_t*   paramFlags = NULL;
    uint32_t   p;
    uint32_t   tempId;
    H2Buf      cNameBuf = { 0 };
    char*      cName;
    int        replaced = 0;
    uint16_t   sigFlags;
    if (c == NULL || baseSig == NULL || binding == NULL || caseArgTypes == NULL
        || (baseSig->flags & H2FnSigFlag_TEMPLATE_BASE) == 0 || baseSig->paramLen == 0)
    {
        return 1;
    }
    paramTypes = (H2TypeRef*)H2ArenaAlloc(
        &c->arena, baseSig->paramLen * sizeof(H2TypeRef), (uint32_t)_Alignof(H2TypeRef));
    if (paramTypes == NULL) {
        return -1;
    }
    for (p = 0; p < baseSig->paramLen; p++) {
        paramTypes[p] = baseSig->paramTypes[p];
    }
    if (baseSig->paramFlags != NULL) {
        paramFlags = (uint8_t*)H2ArenaAlloc(
            &c->arena, baseSig->paramLen * sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
        if (paramFlags == NULL) {
            return -1;
        }
        for (p = 0; p < baseSig->paramLen; p++) {
            paramFlags[p] = baseSig->paramFlags[p] & H2CCGParamFlag_CONST;
        }
    }
    for (p = 0; p < argCount; p++) {
        int32_t paramIndex = binding->argParamIndices[p];
        if (paramIndex < 0 || (uint32_t)paramIndex >= baseSig->paramLen) {
            continue;
        }
        if (baseSig->paramFlags != NULL
            && (baseSig->paramFlags[paramIndex] & H2CCGParamFlag_ANYTYPE) != 0)
        {
            if (!caseArgTypes[p].valid) {
                return 1;
            }
            paramTypes[paramIndex] = caseArgTypes[p];
            replaced = 1;
        }
    }
    if (!replaced) {
        return 1;
    }

    tempId = FmtNextTempId(c);
    cNameBuf.arena = &c->arena;
    if (BufAppendCStr(&cNameBuf, baseSig->cName) != 0 || BufAppendCStr(&cNameBuf, "__ti_rt") != 0
        || BufAppendU32(&cNameBuf, tempId) != 0)
    {
        return -1;
    }
    cName = BufFinish(&cNameBuf);
    if (cName == NULL) {
        return -1;
    }

    sigFlags =
        (uint16_t)(H2FnSigFlag_TEMPLATE_INSTANCE | (baseSig->flags & H2FnSigFlag_EXPANDED_ANYPACK));
    if (AddFnSig(
            c,
            baseSig->hopName,
            cName,
            baseSig->nodeId,
            baseSig->returnType,
            paramTypes,
            baseSig->paramNames,
            paramFlags,
            baseSig->paramLen,
            baseSig->isVariadic,
            baseSig->hasContext,
            baseSig->contextType,
            sigFlags,
            UINT32_MAX,
            baseSig->packArgStart,
            baseSig->packArgCount,
            baseSig->packParamName)
        != 0)
    {
        return -1;
    }
    return 0;
}

int FindTemplateInstanceSigForDispatchCase(
    const H2CBackendC* c,
    const H2FnSig*     baseSig,
    const H2TypeRef*   paramTypes,
    const uint8_t*     paramFlags,
    const H2FnSig**    outSig) {
    uint32_t i;
    if (c == NULL || baseSig == NULL || paramTypes == NULL || outSig == NULL) {
        return -1;
    }
    for (i = 0; i < c->fnSigLen; i++) {
        const H2FnSig* sig = &c->fnSigs[i];
        uint32_t       p;
        if (sig->nodeId != baseSig->nodeId || (sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0
            || sig->paramLen != baseSig->paramLen || sig->isVariadic != baseSig->isVariadic
            || sig->hasContext != baseSig->hasContext)
        {
            continue;
        }
        for (p = 0; p < sig->paramLen; p++) {
            uint8_t sigPflags = sig->paramFlags != NULL ? sig->paramFlags[p] : 0u;
            uint8_t wantPflags = paramFlags != NULL ? paramFlags[p] : 0u;
            if (!TypeRefEqual(&sig->paramTypes[p], &paramTypes[p])
                || (sigPflags & H2CCGParamFlag_CONST) != (wantPflags & H2CCGParamFlag_CONST))
            {
                break;
            }
        }
        if (p == sig->paramLen) {
            *outSig = sig;
            return 0;
        }
    }
    return 1;
}

int EmitInlineStaticFnPrototypeForSig(H2CBackendC* c, const H2FnSig* sig) {
    uint32_t p;
    int      first = 1;
    if (c == NULL || sig == NULL) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "extern ") != 0
        || EmitTypeRefWithName(c, &sig->returnType, sig->cName) != 0
        || BufAppendChar(&c->out, '(') != 0)
    {
        return -1;
    }
    if (sig->hasContext) {
        H2TypeRef contextParamType = sig->contextType;
        contextParamType.ptrDepth++;
        if (EmitTypeRefWithName(c, &contextParamType, "__hop_ctx") != 0) {
            return -1;
        }
        first = 0;
    }
    for (p = 0; p < sig->paramLen; p++) {
        H2Buf nameBuf = { 0 };
        char* name;
        if (!first && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        nameBuf.arena = &c->arena;
        if (BufAppendCStr(&nameBuf, "__hop_p") != 0 || BufAppendU32(&nameBuf, p) != 0) {
            return -1;
        }
        name = BufFinish(&nameBuf);
        if (name == NULL || EmitTypeRefWithName(c, &sig->paramTypes[p], name) != 0) {
            return -1;
        }
        first = 0;
    }
    if (first && BufAppendCStr(&c->out, "void") != 0) {
        return -1;
    }
    return BufAppendCStr(&c->out, "); ");
}

static int TypeRefIsVoidReturn(const H2TypeRef* t) {
    return t != NULL && t->valid && t->containerKind == H2TypeContainer_SCALAR && t->ptrDepth == 0
        && t->containerPtrDepth == 0 && !t->isOptional && t->baseName != NULL
        && StrEq(t->baseName, "void");
}

int EmitEmptyRuntimeAnytypeDispatchPanic(
    H2CBackendC* c, int32_t idxNode, const H2TypeRef* returnType) {
    uint32_t tempId;
    H2Buf    idxNameBuf = { 0 };
    H2Buf    valueNameBuf = { 0 };
    char*    idxName = NULL;
    char*    valueName = NULL;
    int      returnsVoid;
    if (c == NULL || idxNode < 0 || returnType == NULL || !returnType->valid) {
        return -1;
    }
    returnsVoid = TypeRefIsVoidReturn(returnType);
    tempId = FmtNextTempId(c);
    idxNameBuf.arena = &c->arena;
    valueNameBuf.arena = &c->arena;
    if (BufAppendCStr(&idxNameBuf, "__hop_pack_ci") != 0 || BufAppendU32(&idxNameBuf, tempId) != 0
        || BufAppendCStr(&valueNameBuf, "__hop_pack_cr") != 0
        || BufAppendU32(&valueNameBuf, tempId) != 0)
    {
        return -1;
    }
    idxName = BufFinish(&idxNameBuf);
    valueName = BufFinish(&valueNameBuf);
    if (idxName == NULL || valueName == NULL) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(__extension__({ __hop_uint ") != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendCStr(&c->out, " = (__hop_uint)(") != 0
        || EmitExpr(c, idxNode) != 0 || BufAppendCStr(&c->out, "); ") != 0)
    {
        return -1;
    }
    if (!returnsVoid) {
        if (EmitTypeNameWithDepth(c, returnType) != 0 || BufAppendChar(&c->out, ' ') != 0
            || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, " = {0}; ") != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, "switch (") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(
               &c->out,
               ") { default: __hop_panic(__hop_strlit(\"anytype pack index out of bounds\"), "
               "__FILE__, __LINE__); __builtin_unreachable(); } ")
               != 0)
    {
        return -1;
    }
    if (!returnsVoid) {
        if (BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; }))") != 0) {
            return -1;
        }
    } else if (BufAppendCStr(&c->out, "}))") != 0) {
        return -1;
    }
    return 0;
}

int EmitRuntimeAnytypeDispatchFromTemplateBase(
    H2CBackendC*          c,
    int32_t               callNode,
    const H2FnSig*        baseSig,
    const H2CCallBinding* baseBinding,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount) {
    uint32_t       dynamicArgIndex = 0;
    int32_t        idxNode = -1;
    H2CCallBinding caseBindings[H2CCG_MAX_CALL_ARGS] = { 0 };
    const H2FnSig* caseSigs[H2CCG_MAX_CALL_ARGS] = { 0 };
    const char*    caseNames[H2CCG_MAX_CALL_ARGS] = { 0 };
    H2TypeRef      returnType;
    int            returnsVoid = 0;
    uint32_t       i;
    uint32_t       tempId;
    H2Buf          idxNameBuf = { 0 };
    H2Buf          valueNameBuf = { 0 };
    char*          idxName = NULL;
    char*          valueName = NULL;
    if (c == NULL || baseSig == NULL || baseBinding == NULL || argNodes == NULL || argTypes == NULL
        || c->activePackElemCount == 0 || c->activePackElemCount > H2CCG_MAX_CALL_ARGS)
    {
        return 1;
    }
    if ((baseSig->flags & H2FnSigFlag_TEMPLATE_BASE) == 0) {
        return 1;
    }
    if (FindSingleDynamicActivePackCallArg(c, argNodes, argCount, &dynamicArgIndex, &idxNode) != 0
        || idxNode < 0 || dynamicArgIndex >= argCount)
    {
        return 1;
    }

    for (i = 0; i < c->activePackElemCount; i++) {
        H2TypeRef      caseArgTypes[H2CCG_MAX_CALL_ARGS];
        H2TypeRef*     caseParamTypes;
        uint8_t*       caseParamFlags = NULL;
        const H2FnSig* caseSig = NULL;
        uint32_t       p;
        int            replaced = 0;
        for (p = 0; p < argCount; p++) {
            caseArgTypes[p] = argTypes[p];
        }
        caseArgTypes[dynamicArgIndex] = c->activePackElemTypes[i];
        caseBindings[i] = *baseBinding;

        caseParamTypes = (H2TypeRef*)H2ArenaAlloc(
            &c->arena, baseSig->paramLen * sizeof(H2TypeRef), (uint32_t)_Alignof(H2TypeRef));
        if (caseParamTypes == NULL) {
            return -1;
        }
        for (p = 0; p < baseSig->paramLen; p++) {
            caseParamTypes[p] = baseSig->paramTypes[p];
        }
        if (baseSig->paramFlags != NULL) {
            caseParamFlags = (uint8_t*)H2ArenaAlloc(
                &c->arena, baseSig->paramLen * sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
            if (caseParamFlags == NULL) {
                return -1;
            }
            for (p = 0; p < baseSig->paramLen; p++) {
                caseParamFlags[p] = baseSig->paramFlags[p] & H2CCGParamFlag_CONST;
            }
        }
        for (p = 0; p < argCount; p++) {
            int32_t paramIndex = caseBindings[i].argParamIndices[p];
            if (paramIndex < 0 || (uint32_t)paramIndex >= baseSig->paramLen) {
                continue;
            }
            if (baseSig->paramFlags != NULL
                && (baseSig->paramFlags[paramIndex] & H2CCGParamFlag_ANYTYPE) != 0)
            {
                caseParamTypes[paramIndex] = caseArgTypes[p];
                caseBindings[i].argExpectedTypes[p] = caseArgTypes[p];
                replaced = 1;
            }
        }
        if (!replaced) {
            return 1;
        }
        if (MaterializeTemplateInstanceForDispatchCase(
                c, baseSig, &caseBindings[i], caseArgTypes, argCount)
            != 0)
        {
            return -1;
        }
        if (FindTemplateInstanceSigForDispatchCase(
                c, baseSig, caseParamTypes, caseParamFlags, &caseSig)
            != 0)
        {
            return -1;
        }
        if (caseSig == NULL) {
            return 1;
        }
        caseSigs[i] = caseSig;
        caseNames[i] = caseSig->cName;
    }

    if (caseSigs[0] == NULL) {
        return 1;
    }
    returnType = caseSigs[0]->returnType;
    for (i = 1; i < c->activePackElemCount; i++) {
        if (!TypeRefEqual(&returnType, &caseSigs[i]->returnType)) {
            return -1;
        }
    }
    returnsVoid =
        returnType.valid && returnType.containerKind == H2TypeContainer_SCALAR
        && returnType.ptrDepth == 0 && returnType.containerPtrDepth == 0 && !returnType.isOptional
        && returnType.baseName != NULL && StrEq(returnType.baseName, "void");

    tempId = FmtNextTempId(c);
    idxNameBuf.arena = &c->arena;
    valueNameBuf.arena = &c->arena;
    if (BufAppendCStr(&idxNameBuf, "__hop_pack_ci") != 0 || BufAppendU32(&idxNameBuf, tempId) != 0
        || BufAppendCStr(&valueNameBuf, "__hop_pack_cr") != 0
        || BufAppendU32(&valueNameBuf, tempId) != 0)
    {
        return -1;
    }
    idxName = BufFinish(&idxNameBuf);
    valueName = BufFinish(&valueNameBuf);
    if (idxName == NULL || valueName == NULL) {
        return -1;
    }

    if (BufAppendCStr(&c->out, "(__extension__({ __hop_uint ") != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendCStr(&c->out, " = (__hop_uint)(") != 0
        || EmitExpr(c, idxNode) != 0 || BufAppendCStr(&c->out, "); ") != 0)
    {
        return -1;
    }
    if (!returnsVoid) {
        if (EmitTypeNameWithDepth(c, &returnType) != 0 || BufAppendChar(&c->out, ' ') != 0
            || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; ") != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, "switch (") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, ") { ") != 0)
    {
        return -1;
    }
    for (i = 0; i < c->activePackElemCount; i++) {
        if (caseSigs[i] == NULL) {
            return 1;
        }
        if (BufAppendCStr(&c->out, "case ") != 0 || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(&c->out, "u: ") != 0
            || EmitInlineStaticFnPrototypeForSig(c, caseSigs[i]) != 0)
        {
            return -1;
        }
        if (!returnsVoid) {
            if (BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, " = ") != 0) {
                return -1;
            }
        }
        if (EmitResolvedCall(c, callNode, caseNames[i], caseSigs[i], &caseBindings[i], 0) != 0
            || BufAppendCStr(&c->out, "; break; ") != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(
            &c->out,
            "default: __hop_panic(__hop_strlit(\"anytype pack index out of bounds\"), __FILE__, "
            "__LINE__); __builtin_unreachable(); } ")
        != 0)
    {
        return -1;
    }
    if (!returnsVoid) {
        if (BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; }))") != 0) {
            return -1;
        }
    } else {
        if (BufAppendCStr(&c->out, "}))") != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitRuntimeAnytypeDispatchCallBySlice(
    H2CBackendC*          c,
    int32_t               callNode,
    uint32_t              calleeStart,
    uint32_t              calleeEnd,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex) {
    uint32_t       dynamicArgIndex = 0;
    int32_t        idxNode = -1;
    H2CCallBinding caseBindings[H2CCG_MAX_CALL_ARGS] = { 0 };
    const H2FnSig* caseSigs[H2CCG_MAX_CALL_ARGS] = { 0 };
    const char*    caseNames[H2CCG_MAX_CALL_ARGS] = { 0 };
    uint8_t        caseAutoRef[H2CCG_MAX_CALL_ARGS] = { 0 };
    H2TypeRef      returnType;
    int            returnsVoid = 0;
    uint32_t       i;
    uint32_t       tempId;
    H2Buf          idxNameBuf = { 0 };
    H2Buf          valueNameBuf = { 0 };
    char*          idxName = NULL;
    char*          valueName = NULL;

    if (c == NULL || callArgs == NULL || argNodes == NULL || argTypes == NULL) {
        return -1;
    }
    if (c->activePackElemCount == 0 || c->activePackElemCount > H2CCG_MAX_CALL_ARGS) {
        return 1;
    }
    if (FindSingleDynamicActivePackCallArg(c, argNodes, argCount, &dynamicArgIndex, &idxNode) != 0
        || idxNode < 0)
    {
        return 1;
    }
    if (dynamicArgIndex >= argCount) {
        return -1;
    }

    for (i = 0; i < c->activePackElemCount; i++) {
        H2TypeRef      caseArgTypes[H2CCG_MAX_CALL_ARGS];
        const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
        uint32_t       candidateLen = 0;
        int            nameFound = 0;
        const H2FnSig* resolvedSig = NULL;
        const char*    resolvedName = NULL;
        int            status;
        uint32_t       j;
        for (j = 0; j < argCount; j++) {
            caseArgTypes[j] = argTypes[j];
        }
        caseArgTypes[dynamicArgIndex] = c->activePackElemTypes[i];

        GatherCallCandidatesBySlice(
            c, calleeStart, calleeEnd, candidates, &candidateLen, &nameFound);
        status = ResolveCallTargetFromCandidates(
            c,
            candidates,
            candidateLen,
            nameFound,
            callArgs,
            argNodes,
            caseArgTypes,
            argCount,
            firstPositionalArgIndex,
            0,
            &caseBindings[i],
            &resolvedSig,
            &resolvedName);
        if (status == 2) {
            status = ResolveCallTargetFromCandidates(
                c,
                candidates,
                candidateLen,
                nameFound,
                callArgs,
                argNodes,
                caseArgTypes,
                argCount,
                firstPositionalArgIndex,
                1,
                &caseBindings[i],
                &resolvedSig,
                &resolvedName);
            caseAutoRef[i] = (status == 0) ? 1 : 0;
        } else {
            caseAutoRef[i] = 0;
        }

        if (status == 0 && resolvedSig != NULL) {
            int32_t paramIndex = caseBindings[i].argParamIndices[dynamicArgIndex];
            int     unresolvedAnytype = (resolvedSig->flags & H2FnSigFlag_TEMPLATE_BASE) != 0;
            if (!unresolvedAnytype && paramIndex >= 0
                && (uint32_t)paramIndex < resolvedSig->paramLen && resolvedSig->paramFlags != NULL
                && (resolvedSig->paramFlags[paramIndex] & H2CCGParamFlag_ANYTYPE) != 0)
            {
                unresolvedAnytype = 1;
            }
            if (unresolvedAnytype) {
                int matRc = MaterializeTemplateInstanceForDispatchCase(
                    c, resolvedSig, &caseBindings[i], caseArgTypes, argCount);
                if (matRc < 0) {
                    return -1;
                }
                if (matRc == 0) {
                    GatherCallCandidatesBySlice(
                        c, calleeStart, calleeEnd, candidates, &candidateLen, &nameFound);
                    status = ResolveCallTargetFromCandidates(
                        c,
                        candidates,
                        candidateLen,
                        nameFound,
                        callArgs,
                        argNodes,
                        caseArgTypes,
                        argCount,
                        firstPositionalArgIndex,
                        0,
                        &caseBindings[i],
                        &resolvedSig,
                        &resolvedName);
                    if (status == 2) {
                        status = ResolveCallTargetFromCandidates(
                            c,
                            candidates,
                            candidateLen,
                            nameFound,
                            callArgs,
                            argNodes,
                            caseArgTypes,
                            argCount,
                            firstPositionalArgIndex,
                            1,
                            &caseBindings[i],
                            &resolvedSig,
                            &resolvedName);
                        caseAutoRef[i] = (status == 0) ? 1 : 0;
                    } else {
                        caseAutoRef[i] = 0;
                    }
                }
            }
        }

        if (status != 0 || resolvedSig == NULL
            || (resolvedSig->flags & H2FnSigFlag_TEMPLATE_BASE) != 0)
        {
            const H2FnSig* narrowed[H2CCG_MAX_CALL_CANDIDATES];
            uint32_t       narrowedLen = 0;
            uint32_t       k;
            for (k = 0; k < candidateLen && narrowedLen < H2CCG_MAX_CALL_CANDIDATES; k++) {
                H2CCallBinding candBinding;
                int32_t        dynParamIndex;
                if (PrepareCallBinding(
                        c,
                        candidates[k],
                        callArgs,
                        argNodes,
                        caseArgTypes,
                        argCount,
                        firstPositionalArgIndex,
                        1,
                        &candBinding)
                    != 0)
                {
                    continue;
                }
                dynParamIndex = candBinding.argParamIndices[dynamicArgIndex];
                if (dynParamIndex < 0 || (uint32_t)dynParamIndex >= candidates[k]->paramLen
                    || !TypeRefEqual(
                        &candBinding.argExpectedTypes[dynamicArgIndex],
                        &caseArgTypes[dynamicArgIndex]))
                {
                    continue;
                }
                narrowed[narrowedLen++] = candidates[k];
            }
            if (narrowedLen > 0) {
                status = ResolveCallTargetFromCandidates(
                    c,
                    narrowed,
                    narrowedLen,
                    1,
                    callArgs,
                    argNodes,
                    caseArgTypes,
                    argCount,
                    firstPositionalArgIndex,
                    0,
                    &caseBindings[i],
                    &resolvedSig,
                    &resolvedName);
                if (status == 2) {
                    status = ResolveCallTargetFromCandidates(
                        c,
                        narrowed,
                        narrowedLen,
                        1,
                        callArgs,
                        argNodes,
                        caseArgTypes,
                        argCount,
                        firstPositionalArgIndex,
                        1,
                        &caseBindings[i],
                        &resolvedSig,
                        &resolvedName);
                    caseAutoRef[i] = (status == 0) ? 1 : 0;
                } else {
                    caseAutoRef[i] = 0;
                }
            }
        }

        if (status != 0 || resolvedSig == NULL || resolvedName == NULL
            || (resolvedSig->flags & H2FnSigFlag_TEMPLATE_BASE) != 0)
        {
            return 1;
        }
        caseSigs[i] = resolvedSig;
        caseNames[i] = resolvedName;
    }

    if (caseSigs[0] == NULL) {
        return 1;
    }
    returnType = caseSigs[0]->returnType;
    for (i = 1; i < c->activePackElemCount; i++) {
        if (!TypeRefEqual(&returnType, &caseSigs[i]->returnType)) {
            return -1;
        }
    }
    returnsVoid =
        returnType.valid && returnType.containerKind == H2TypeContainer_SCALAR
        && returnType.ptrDepth == 0 && returnType.containerPtrDepth == 0 && !returnType.isOptional
        && returnType.baseName != NULL && StrEq(returnType.baseName, "void");

    tempId = FmtNextTempId(c);
    idxNameBuf.arena = &c->arena;
    valueNameBuf.arena = &c->arena;
    if (BufAppendCStr(&idxNameBuf, "__hop_pack_ci") != 0 || BufAppendU32(&idxNameBuf, tempId) != 0
        || BufAppendCStr(&valueNameBuf, "__hop_pack_cr") != 0
        || BufAppendU32(&valueNameBuf, tempId) != 0)
    {
        return -1;
    }
    idxName = BufFinish(&idxNameBuf);
    valueName = BufFinish(&valueNameBuf);
    if (idxName == NULL || valueName == NULL) {
        return -1;
    }

    if (BufAppendCStr(&c->out, "(__extension__({ __hop_uint ") != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendCStr(&c->out, " = (__hop_uint)(") != 0
        || EmitExpr(c, idxNode) != 0 || BufAppendCStr(&c->out, "); ") != 0)
    {
        return -1;
    }
    if (!returnsVoid) {
        if (EmitTypeNameWithDepth(c, &returnType) != 0 || BufAppendChar(&c->out, ' ') != 0
            || BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; ") != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, "switch (") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, ") { ") != 0)
    {
        return -1;
    }
    for (i = 0; i < c->activePackElemCount; i++) {
        if (caseSigs[i] == NULL || caseNames[i] == NULL) {
            return 1;
        }
        if (BufAppendCStr(&c->out, "case ") != 0 || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(&c->out, "u: ") != 0)
        {
            return -1;
        }
        if (!returnsVoid) {
            if (BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, " = ") != 0) {
                return -1;
            }
        }
        if (EmitResolvedCall(
                c, callNode, caseNames[i], caseSigs[i], &caseBindings[i], caseAutoRef[i])
                != 0
            || BufAppendCStr(&c->out, "; break; ") != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(
            &c->out,
            "default: __hop_panic(__hop_strlit(\"anytype pack index out of bounds\"), __FILE__, "
            "__LINE__); __builtin_unreachable(); } ")
        != 0)
    {
        return -1;
    }
    if (!returnsVoid) {
        if (BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, "; }))") != 0) {
            return -1;
        }
    } else {
        if (BufAppendCStr(&c->out, "}))") != 0) {
            return -1;
        }
    }
    return 0;
}

int TryEmitRuntimeAnytypeDispatchCallBySlice(
    H2CBackendC*          c,
    int32_t               callNode,
    uint32_t              calleeStart,
    uint32_t              calleeEnd,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   resolvedStatus,
    const H2FnSig* _Nullable resolvedSig,
    const H2CCallBinding* _Nullable resolvedBinding) {
    uint32_t dynamicArgIndex = 0;
    int32_t  idxNode = -1;
    int      shouldDispatch = 0;
    int      dispatchRc;

    if (c == NULL || callArgs == NULL || argNodes == NULL || argTypes == NULL) {
        return -1;
    }
    if (c->activePackElemCount > H2CCG_MAX_CALL_ARGS) {
        return 1;
    }
    if (FindSingleDynamicActivePackCallArg(c, argNodes, argCount, &dynamicArgIndex, &idxNode) != 0
        || idxNode < 0)
    {
        return 1;
    }
    if (resolvedStatus == 3) {
        shouldDispatch = 1;
    } else if (resolvedStatus == 0 && resolvedSig != NULL) {
        int32_t paramIndex = -1;
        if (resolvedBinding != NULL && dynamicArgIndex < argCount) {
            paramIndex = resolvedBinding->argParamIndices[dynamicArgIndex];
        }
        shouldDispatch = (resolvedSig->flags & H2FnSigFlag_TEMPLATE_BASE) != 0;
        if (!shouldDispatch && paramIndex >= 0 && (uint32_t)paramIndex < resolvedSig->paramLen
            && resolvedSig->paramFlags != NULL
            && (resolvedSig->paramFlags[paramIndex] & H2CCGParamFlag_ANYTYPE) != 0)
        {
            shouldDispatch = 1;
        }
    }
    if (!shouldDispatch) {
        return 1;
    }
    if (c->activePackElemCount == 0) {
        if (resolvedStatus == 0 && resolvedSig != NULL) {
            return EmitEmptyRuntimeAnytypeDispatchPanic(c, idxNode, &resolvedSig->returnType);
        }
        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
            SetDiagNode(c, callNode, H2Diag_CODEGEN_INTERNAL);
        }
        return -1;
    }

    if (resolvedStatus == 0 && resolvedSig != NULL && resolvedBinding != NULL
        && (resolvedSig->flags & H2FnSigFlag_TEMPLATE_BASE) != 0)
    {
        dispatchRc = EmitRuntimeAnytypeDispatchFromTemplateBase(
            c, callNode, resolvedSig, resolvedBinding, argNodes, argTypes, argCount);
        if (dispatchRc == 0) {
            return 0;
        }
        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
            SetDiagNode(c, callNode, H2Diag_CODEGEN_INTERNAL);
        }
        return -1;
    }

    dispatchRc = EmitRuntimeAnytypeDispatchCallBySlice(
        c,
        callNode,
        calleeStart,
        calleeEnd,
        callArgs,
        argNodes,
        argTypes,
        argCount,
        firstPositionalArgIndex);
    if (dispatchRc == 0) {
        return 0;
    }
    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
        SetDiagNode(c, callNode, H2Diag_CODEGEN_INTERNAL);
    }
    return -1;
}

static int ResolveCallTargetByMappedCName(
    H2CBackendC*          c,
    const char*           mappedCName,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding*       outBinding,
    const H2FnSig**       outSig,
    const char**          outCalleeName);

static int TryEmitFormatTemplateCall(
    H2CBackendC* c,
    int32_t      nodeId,
    int32_t      child,
    int          isSelector,
    int32_t      recvNode,
    const char*  mappedCName) {
    H2CCallArgInfo callArgs[H2CCG_MAX_CALL_ARGS];
    int32_t        argNodes[H2CCG_MAX_CALL_ARGS];
    H2TypeRef      argTypes[H2CCG_MAX_CALL_ARGS];
    H2CCallBinding binding;
    const H2FnSig* resolvedSig = NULL;
    const char*    resolvedName = NULL;
    uint32_t       argCount = 0;
    uint32_t       i;
    size_t         mappedLen;
    int            status;
    if (mappedCName == NULL) {
        return 1;
    }
    mappedLen = StrLen(mappedCName);
    if (CollectCallArgInfo(
            c,
            nodeId,
            child,
            isSelector ? 1 : 0,
            isSelector ? recvNode : -1,
            callArgs,
            argTypes,
            &argCount)
        != 0)
    {
        return 1;
    }
    for (i = 0; i < argCount; i++) {
        argNodes[i] = callArgs[i].exprNode;
    }
    status = ResolveCallTargetByMappedCName(
        c,
        mappedCName,
        callArgs,
        argNodes,
        argTypes,
        argCount,
        isSelector ? 1u : 0u,
        0,
        &binding,
        &resolvedSig,
        &resolvedName);
    if (status == 0 && resolvedName != NULL) {
        return EmitResolvedCall(c, nodeId, resolvedName, resolvedSig, &binding, 0);
    }
    {
        const H2FnSig* bestSig = NULL;
        uint32_t       bestScore = UINT32_MAX;
        int            ambiguous = 0;
        for (i = 0; i < c->fnSigLen; i++) {
            const H2FnSig* sig = &c->fnSigs[i];
            uint32_t       score = 0;
            uint32_t       p;
            if ((sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0
                || !StrHasPrefix(sig->cName, mappedCName) || sig->paramLen != argCount
                || sig->cName[mappedLen] != '_' || sig->cName[mappedLen + 1u] != '_'
                || sig->cName[mappedLen + 2u] != 't' || sig->cName[mappedLen + 3u] != 'i')
            {
                continue;
            }
            for (p = 0; p < argCount; p++) {
                const H2AstNode* expr = NodeAt(c, argNodes[p]);
                const char*      baseName = sig->paramTypes[p].baseName;
                if (baseName != NULL) {
                    const char* resolved = ResolveScalarAliasBaseName(c, baseName);
                    if (resolved != NULL) {
                        baseName = resolved;
                    }
                }
                if (expr != NULL && expr->kind == H2Ast_CALL_ARG) {
                    expr = NodeAt(c, UnwrapCallArgExprNode(c, argNodes[p]));
                }
                if (expr != NULL && expr->kind == H2Ast_STRING) {
                    if (!TypeRefIsStr(&sig->paramTypes[p])) {
                        score += 1000u;
                    }
                    continue;
                }
                if (expr != NULL && (expr->kind == H2Ast_INT || expr->kind == H2Ast_RUNE)) {
                    if (baseName == NULL || !IsIntegerCTypeName(baseName)) {
                        score += 1000u;
                    }
                    continue;
                }
                if (expr != NULL && expr->kind == H2Ast_BOOL) {
                    if (baseName == NULL || !StrEq(baseName, "__hop_bool")) {
                        score += 1000u;
                    }
                    continue;
                }
                if (argTypes[p].valid) {
                    uint8_t cost = 0;
                    if (TypeRefAssignableCost(c, &sig->paramTypes[p], &argTypes[p], &cost) != 0) {
                        score += 1000u;
                    } else {
                        score += cost;
                    }
                }
            }
            if (score < bestScore) {
                bestSig = sig;
                bestScore = score;
                ambiguous = 0;
            } else if (score == bestScore) {
                ambiguous = 1;
            }
        }
        if (bestSig != NULL && !ambiguous && bestScore < 1000u) {
            BindPositionalTemplateInstanceFallback(bestSig, argNodes, argCount, &binding);
            return EmitResolvedCall(c, nodeId, bestSig->cName, bestSig, &binding, 0);
        }
    }
    return 1;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
int ResolveCallTarget(
    H2CBackendC*          c,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding* _Nullable outBinding,
    const H2FnSig** outSig,
    const char**    outCalleeName) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
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
    H2CBackendC*          c,
    uint32_t              pkgStart,
    uint32_t              pkgEnd,
    uint32_t              methodStart,
    uint32_t              methodEnd,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding* _Nullable outBinding,
    const H2FnSig** outSig,
    const char**    outCalleeName) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
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

static int ResolveCallTargetByMappedCName(
    H2CBackendC*          c,
    const char*           mappedCName,
    const H2CCallArgInfo* callArgs,
    const int32_t*        argNodes,
    const H2TypeRef*      argTypes,
    uint32_t              argCount,
    uint32_t              firstPositionalArgIndex,
    int                   autoRefFirstArg,
    H2CCallBinding*       outBinding,
    const H2FnSig**       outSig,
    const char**          outCalleeName) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    uint32_t       mappedLen;
    uint32_t       i;
    int            hasTemplateInstance = 0;
    int            rc;
    if (c == NULL || mappedCName == NULL) {
        return 1;
    }
    mappedLen = (uint32_t)StrLen(mappedCName);
    for (i = 0; i < c->fnSigLen && candidateLen < H2CCG_MAX_CALL_CANDIDATES; i++) {
        const H2FnSig* sig = &c->fnSigs[i];
        if (StrEq(sig->cName, mappedCName)
            || (StrHasPrefix(sig->cName, mappedCName)
                && StrHasPrefix(sig->cName + mappedLen, "__ti"))
            || StrEq(sig->hopName, mappedCName))
        {
            if ((sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0) {
                hasTemplateInstance = 1;
            }
            candidates[candidateLen++] = sig;
        }
    }
    if (hasTemplateInstance) {
        uint32_t out = 0;
        for (i = 0; i < candidateLen; i++) {
            if ((candidates[i]->flags & H2FnSigFlag_TEMPLATE_BASE) != 0) {
                continue;
            }
            candidates[out++] = candidates[i];
        }
        candidateLen = out;
    }
    rc = ResolveCallTargetFromCandidates(
        c,
        candidates,
        candidateLen,
        candidateLen > 0,
        callArgs,
        argNodes,
        argTypes,
        argCount,
        firstPositionalArgIndex,
        autoRefFirstArg,
        outBinding,
        outSig,
        outCalleeName);
    if (rc != 2) {
        return rc;
    }
    if (hasTemplateInstance) {
        const H2FnSig* bestSig = NULL;
        H2CCallBinding bestBinding;
        uint32_t       bestTotal = 0;
        int            ambiguous = 0;
        memset(&bestBinding, 0, sizeof(bestBinding));
        for (i = 0; i < candidateLen; i++) {
            const H2FnSig* sig = candidates[i];
            H2CCallBinding binding;
            uint32_t       p;
            uint32_t       total = 0;
            int            viable = 1;
            if (sig == NULL || (sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0) {
                continue;
            }
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
                int32_t   paramIndex = binding.argParamIndices[p];
                H2TypeRef paramType;
                H2TypeRef argType;
                uint8_t   cost = 0;
                if (binding.activePackSpread && p == binding.activePackSpreadArgIndex) {
                    uint32_t k;
                    for (k = 0; k < c->activePackElemCount; k++) {
                        if (binding.activePackSpreadParamStart + k >= sig->paramLen
                            || TypeRefAssignableCost(
                                   c,
                                   &sig->paramTypes[binding.activePackSpreadParamStart + k],
                                   &c->activePackElemTypes[k],
                                   &cost)
                                   != 0)
                        {
                            viable = 0;
                            break;
                        }
                        total += cost;
                    }
                    if (!viable) {
                        break;
                    }
                    continue;
                }
                if (paramIndex < 0 || (uint32_t)paramIndex >= sig->paramLen) {
                    viable = 0;
                    break;
                }
                paramType = sig->paramTypes[paramIndex];
                if (argTypes[p].valid) {
                    argType = argTypes[p];
                } else if (
                    InferExprTypeExpected(c, argNodes[p], &paramType, &argType) != 0
                    || !argType.valid)
                {
                    viable = 0;
                    break;
                }
                if (TypeRefAssignableCost(c, &paramType, &argType, &cost) != 0) {
                    viable = 0;
                    break;
                }
                total += cost;
            }
            if (!viable) {
                continue;
            }
            if (bestSig == NULL || total < bestTotal) {
                bestSig = sig;
                bestBinding = binding;
                bestTotal = total;
                ambiguous = 0;
            } else if (total == bestTotal) {
                ambiguous = 1;
            }
        }
        if (bestSig != NULL && !ambiguous) {
            *outBinding = bestBinding;
            *outSig = bestSig;
            *outCalleeName = bestSig->cName;
            return 0;
        }
        if (ambiguous) {
            return 3;
        }
    }
    return rc;
}

int InferExprType_IDENT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType);

static uint32_t ArrayLitElementCount(H2CBackendC* c, int32_t nodeId) {
    uint32_t count = 0;
    int32_t  child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        count++;
        child = AstNextSibling(&c->ast, child);
    }
    return count;
}

static int ArrayLitElementTypeFromExpected(
    const H2TypeRef* expectedType, H2TypeRef* outElem, uint32_t* outLen, int* outHasLen) {
    H2TypeRef t;
    if (outElem != NULL) {
        TypeRefSetInvalid(outElem);
    }
    if (outLen != NULL) {
        *outLen = 0;
    }
    if (outHasLen != NULL) {
        *outHasLen = 0;
    }
    if (expectedType == NULL || !expectedType->valid || outElem == NULL) {
        return 0;
    }
    t = *expectedType;
    if (t.containerKind != H2TypeContainer_SCALAR && t.containerPtrDepth > 0) {
        t.containerPtrDepth--;
    }
    if (t.containerKind == H2TypeContainer_ARRAY) {
        *outElem = t;
        outElem->containerKind = H2TypeContainer_SCALAR;
        outElem->containerPtrDepth = 0;
        outElem->arrayLen = 0;
        outElem->hasArrayLen = 0;
        if (outLen != NULL) {
            *outLen = t.arrayLen;
        }
        if (outHasLen != NULL) {
            *outHasLen = t.hasArrayLen;
        }
        return 1;
    }
    if (t.containerKind == H2TypeContainer_SLICE_RO || t.containerKind == H2TypeContainer_SLICE_MUT)
    {
        *outElem = t;
        outElem->containerKind = H2TypeContainer_SCALAR;
        outElem->containerPtrDepth = 0;
        outElem->arrayLen = 0;
        outElem->hasArrayLen = 0;
        return 1;
    }
    return 0;
}

int InferArrayLiteralType(
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType, H2TypeRef* outType) {
    const H2AstNode* litNode = NodeAt(c, nodeId);
    H2TypeRef        elemType;
    uint32_t         elemCount;
    uint32_t         expectedLen = 0;
    int              hasExpectedLen = 0;
    int32_t          child;
    if (litNode == NULL || litNode->kind != H2Ast_ARRAY_LIT) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    elemCount = ArrayLitElementCount(c, nodeId);
    if (ArrayLitElementTypeFromExpected(expectedType, &elemType, &expectedLen, &hasExpectedLen)) {
        if (hasExpectedLen && elemCount > expectedLen) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        child = AstFirstChild(&c->ast, nodeId);
        while (child >= 0) {
            H2TypeRef childType;
            uint8_t   cost = 0;
            if (InferExprTypeExpected(c, child, &elemType, &childType) != 0 || !childType.valid
                || TypeRefAssignableCost(c, &elemType, &childType, &cost) != 0)
            {
                TypeRefSetInvalid(outType);
                return -1;
            }
            child = AstNextSibling(&c->ast, child);
        }
        *outType = elemType;
        outType->containerKind = H2TypeContainer_ARRAY;
        outType->arrayLen = hasExpectedLen ? expectedLen : elemCount;
        outType->hasArrayLen = 1;
        outType->readOnly = 0;
        return 0;
    }
    if (elemCount == 0u) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    TypeRefSetInvalid(&elemType);
    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        H2TypeRef childType;
        if (InferExprType(c, child, &childType) != 0 || !childType.valid) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        if (!elemType.valid) {
            elemType = childType;
        } else {
            uint8_t cost = 0;
            if (TypeRefAssignableCost(c, &elemType, &childType, &cost) != 0) {
                if (TypeRefAssignableCost(c, &childType, &elemType, &cost) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                elemType = childType;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    CanonicalizeTypeRefBaseName(c, &elemType);
    *outType = elemType;
    outType->containerKind = H2TypeContainer_ARRAY;
    outType->containerPtrDepth = 0;
    outType->arrayLen = elemCount;
    outType->hasArrayLen = 1;
    outType->readOnly = 0;
    return 0;
}

int InferCompoundLiteralType(
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType, H2TypeRef* outType) {
    const H2AstNode*      litNode = NodeAt(c, nodeId);
    int32_t               child;
    int32_t               firstField;
    int                   hasExplicitType;
    H2TypeRef             explicitType;
    H2TypeRef             targetValueType;
    H2TypeRef             resultType;
    const char*           ownerType = NULL;
    const H2NameMap*      ownerMap;
    const H2AnonTypeInfo* anonOwner = NULL;
    int                   isUnion = 0;
    int                   isEnumVariantLiteral = 0;
    uint32_t              enumVariantStart = 0;
    uint32_t              enumVariantEnd = 0;
    uint32_t              explicitFieldCount = 0;
    uint8_t               cost = 0;

    if (litNode == NULL || litNode->kind != H2Ast_COMPOUND_LIT) {
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
            H2TypeRef   fieldTypes[256];
            uint32_t    fieldCount = 0;
            int32_t     scan = firstField;
            const char* anonName;
            while (scan >= 0) {
                const H2AstNode* fieldNode = NodeAt(c, scan);
                H2TypeRef        exprType;
                int32_t          exprNode;
                uint32_t         i;
                if (fieldNode == NULL || fieldNode->kind != H2Ast_COMPOUND_FIELD) {
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
                    if ((fieldNode->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    {
                        H2AstNode identNode = { 0 };
                        identNode.kind = H2Ast_IDENT;
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
            if (targetValueType.containerKind != H2TypeContainer_SCALAR) {
                if (targetValueType.containerPtrDepth > 0) {
                    targetValueType.containerPtrDepth--;
                }
            } else if (targetValueType.ptrDepth > 0) {
                targetValueType.ptrDepth--;
            }
            resultType = *expectedType;
        }
    }

    if (!targetValueType.valid || targetValueType.containerKind != H2TypeContainer_SCALAR
        || targetValueType.containerPtrDepth != 0 || targetValueType.ptrDepth != 0
        || targetValueType.baseName == NULL || targetValueType.isOptional)
    {
        TypeRefSetInvalid(outType);
        return -1;
    }

    ownerType = CanonicalFieldOwnerType(c, targetValueType.baseName);
    if (ownerType == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    ownerMap = FindNameByCName(c, ownerType);
    anonOwner = FindAnonTypeByCName(c, ownerType);
    if ((ownerMap == NULL
         || (ownerMap->kind != H2Ast_STRUCT && ownerMap->kind != H2Ast_UNION
             && ownerMap->kind != H2Ast_ENUM))
        && anonOwner == NULL)
    {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (ownerMap != NULL && ownerMap->kind == H2Ast_ENUM) {
        int32_t     enumNodeId = -1;
        H2TypeRef   payloadType;
        const char* payloadOwner;
        if (!isEnumVariantLiteral || FindEnumDeclNodeByCName(c, ownerType, &enumNodeId) != 0
            || !EnumDeclHasPayload(c, enumNodeId))
        {
            TypeRefSetInvalid(outType);
            return -1;
        }
        if (ResolveEnumVariantPayloadType(
                c, ownerType, enumVariantStart, enumVariantEnd, &payloadType)
                != 0
            || !payloadType.valid)
        {
            TypeRefSetInvalid(outType);
            return -1;
        }
        payloadOwner = CanonicalFieldOwnerType(c, payloadType.baseName);
        if (payloadOwner == NULL) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        isUnion = 0;
    } else if (ownerMap != NULL) {
        isUnion = ownerMap->kind == H2Ast_UNION;
    } else {
        isUnion = anonOwner->isUnion;
    }

    while (firstField >= 0) {
        const H2AstNode* fieldNode = NodeAt(c, firstField);
        int32_t          exprNode;
        int32_t          scan;
        H2TypeRef        exprType;
        H2TypeRef        fieldType;

        if (fieldNode == NULL || fieldNode->kind != H2Ast_COMPOUND_FIELD) {
            TypeRefSetInvalid(outType);
            return -1;
        }

        scan = hasExplicitType ? AstNextSibling(&c->ast, child) : child;
        while (scan >= 0 && scan != firstField) {
            const H2AstNode* prevField = NodeAt(c, scan);
            if (prevField != NULL && prevField->kind == H2Ast_COMPOUND_FIELD
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
        if (exprNode < 0 && (fieldNode->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
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
            const H2FieldInfo* fieldPath[64];
            const H2FieldInfo* field = NULL;
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
            H2AstNode identNode = { 0 };
            identNode.kind = H2Ast_IDENT;
            identNode.start = fieldNode->start;
            identNode.end = fieldNode->end;
            identNode.dataStart = fieldNode->dataStart;
            identNode.dataEnd = fieldNode->dataEnd;
            if (InferExprType_IDENT(c, firstField, &identNode, &exprType) != 0 || !exprType.valid) {
                TypeRefSetInvalid(outType);
                return -1;
            }
        } else if (
            TypeRefIsFunctionAlias(c, &fieldType) && NodeAt(c, exprNode) != NULL
            && NodeAt(c, exprNode)->kind == H2Ast_IDENT)
        {
            exprType = fieldType;
        } else if (
            InferExprTypeExpected(c, exprNode, &fieldType, &exprType) != 0 || !exprType.valid)
        {
            TypeRefSetInvalid(outType);
            return -1;
        }
        if (TypeRefAssignableCost(c, &fieldType, &exprType, &cost) != 0) {
            const H2AstNode* expr = NodeAt(c, exprNode);
            const char*      dstBase =
                fieldType.baseName != NULL
                    ? ResolveScalarAliasBaseName(c, fieldType.baseName)
                    : NULL;
            if (dstBase == NULL) {
                dstBase = fieldType.baseName;
            }
            if (!(expr != NULL && expr->kind == H2Ast_INT && dstBase != NULL
                  && (IsIntegerCTypeName(dstBase) || IsFloatCTypeName(dstBase)))
                && !(
                    expr != NULL && expr->kind == H2Ast_FLOAT && dstBase != NULL
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
        H2TypeRef expectedValueType = *expectedType;
        if (expectedValueType.containerKind != H2TypeContainer_SCALAR) {
            if (expectedValueType.containerPtrDepth > 0) {
                expectedValueType.containerPtrDepth--;
            }
        } else if (expectedValueType.ptrDepth > 0) {
            expectedValueType.ptrDepth--;
        }
        if (expectedValueType.valid && expectedValueType.containerKind == H2TypeContainer_SCALAR
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
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType, H2TypeRef* outType) {
    const H2AstNode* n = NodeAt(c, nodeId);
    int32_t          idxNode = -1;
    int              isConstIndex = 0;
    uint32_t         constIndex = 0;
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (expectedType != NULL && expectedType->valid
        && ResolveActivePackIndexExpr(c, nodeId, &idxNode, &isConstIndex, &constIndex) == 0
        && !isConstIndex)
    {
        *outType = *expectedType;
        return 0;
    }

    if (n->kind == H2Ast_COMPOUND_LIT) {
        return InferCompoundLiteralType(c, nodeId, expectedType, outType);
    }
    if (n->kind == H2Ast_ARRAY_LIT) {
        return InferArrayLiteralType(c, nodeId, expectedType, outType);
    }

    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, nodeId);
        const H2AstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == H2Ast_COMPOUND_LIT) {
            H2TypeRef rhsExpected;
            H2TypeRef rhsType;
            int       haveExpected = 0;
            if (expectedType != NULL && expectedType->valid) {
                rhsExpected = *expectedType;
                if (rhsExpected.containerKind != H2TypeContainer_SCALAR) {
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
            if (rhsType.containerKind == H2TypeContainer_SCALAR) {
                rhsType.ptrDepth++;
            } else {
                rhsType.containerPtrDepth++;
            }
            *outType = rhsType;
            return 0;
        }
    }

    if (expectedType != NULL && expectedType->valid
        && expectedType->containerKind == H2TypeContainer_SCALAR && expectedType->ptrDepth == 0
        && expectedType->containerPtrDepth == 0)
    {
        H2TypeRef   srcType;
        const char* dstBase = ResolveScalarAliasBaseName(c, expectedType->baseName);
        if (dstBase == NULL) {
            dstBase = expectedType->baseName;
        }
        if (dstBase != NULL && InferExprType(c, nodeId, &srcType) == 0 && srcType.valid
            && srcType.containerKind == H2TypeContainer_SCALAR && srcType.ptrDepth == 0
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
                if (srcBase != NULL && StrEq(srcBase, "__hop_int")) {
                    if (EvalConstIntExpr(c, nodeId, &intValue, &isConst) != 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    if (!isConst) {
                        *outType = srcType;
                        return 0;
                    }
                    if (!ConstIntFitsIntegerType(dstBase, intValue)) {
                        SetDiagNode(c, nodeId, H2Diag_TYPE_MISMATCH);
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
                        SetDiagNode(c, nodeId, H2Diag_TYPE_MISMATCH);
                        return -1;
                    }
                    *outType = *expectedType;
                    return 0;
                }
            }
            if (IsFloatCTypeName(dstBase)) {
                if (srcBase != NULL && StrEq(srcBase, "__hop_int")) {
                    if (EvalConstIntExpr(c, nodeId, &intValue, &isConst) != 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    if (!isConst) {
                        *outType = srcType;
                        return 0;
                    }
                    if (!ConstIntFitsFloatType(dstBase, intValue)) {
                        SetDiagNode(c, nodeId, H2Diag_TYPE_MISMATCH);
                        return -1;
                    }
                    *outType = *expectedType;
                    return 0;
                }
                if (srcBase != NULL && StrEq(srcBase, "__hop_f64")) {
                    if (EvalConstFloatExpr(c, nodeId, &floatValue, &isConst) != 0) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    if (!isConst) {
                        *outType = srcType;
                        return 0;
                    }
                    if (!ConstFloatFitsFloatType(dstBase, floatValue)) {
                        SetDiagNode(c, nodeId, H2Diag_TYPE_MISMATCH);
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

int InferExprType_IDENT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    const H2Local* local = FindLocalBySlice(c, n->dataStart, n->dataEnd);
    (void)nodeId;
    if (IsActivePackIdent(c, n->dataStart, n->dataEnd)) {
        TypeRefSetInvalid(outType);
        return 0;
    }
    if (local != NULL) {
        *outType = local->type;
        return 0;
    }
    {
        const H2FnSig* sig = FindFnSigBySlice(c, n->dataStart, n->dataEnd);
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
        H2TypeRef typeValue;
        if (ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, &typeValue)) {
            TypeRefSetScalar(outType, "__hop_type");
            return 0;
        }
    }
    TypeRefSetInvalid(outType);
    return 0;
}

int InferExprType_COMPOUND_LIT(
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)n;
    return InferCompoundLiteralType(c, nodeId, NULL, outType);
}

int InferExprType_CALL_WITH_CONTEXT(
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
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

int InferExprType_CALL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t          callee = AstFirstChild(&c->ast, nodeId);
    const H2AstNode* cn = NodeAt(c, callee);
    (void)n;
    if (cn != NULL && cn->kind == H2Ast_FIELD_EXPR) {
        const H2NameMap* enumMap = NULL;
        uint32_t         variantStart = 0;
        uint32_t         variantEnd = 0;
        H2TypeRef        payloadType;
        int              variantRc = ResolveEnumSelectorByFieldExpr(
            c, callee, &enumMap, NULL, NULL, &variantStart, &variantEnd);
        if (variantRc < 0) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        if (variantRc == 1 && enumMap != NULL
            && ResolveEnumVariantPayloadType(
                   c, enumMap->cName, variantStart, variantEnd, &payloadType)
                   == 0)
        {
            TypeRefSetScalar(outType, enumMap->cName);
            return 0;
        }
    }
    if (cn != NULL && cn->kind == H2Ast_IDENT) {
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "len")) {
            int32_t          argNode = AstNextSibling(&c->ast, callee);
            int32_t          argExprNode = UnwrapCallArgExprNode(c, argNode);
            int32_t          extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            const H2AstNode* arg = NodeAt(c, argExprNode);
            H2TypeRef        argType;
            if (argNode < 0 || extraNode >= 0 || argExprNode < 0) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (argExprNode >= 0 && extraNode < 0 && arg != NULL && arg->kind == H2Ast_IDENT
                && IsActivePackIdent(c, arg->dataStart, arg->dataEnd))
            {
                TypeRefSetScalar(outType, "__hop_int");
                return 0;
            }
            if (InferExprType(c, argExprNode, &argType) != 0 || !argType.valid) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            TypeRefSetScalar(outType, "__hop_int");
            return 0;
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "kind")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                const char* kindTypeName;
                H2TypeRef   argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    kindTypeName = FindReflectKindTypeName(c);
                    TypeRefSetScalar(outType, kindTypeName != NULL ? kindTypeName : "__hop_u8");
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "base")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                H2TypeRef argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    TypeRefSetScalar(outType, "__hop_type");
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "is_alias")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                H2TypeRef argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    TypeRefSetScalar(outType, "__hop_bool");
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "type_name")) {
            int32_t argNode = AstNextSibling(&c->ast, callee);
            int32_t extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            if (argNode >= 0 && extraNode < 0) {
                H2TypeRef argType;
                if (InferExprType(c, argNode, &argType) != 0) {
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                if (argType.valid && TypeRefIsTypeValue(&argType)) {
                    TypeRefSetScalar(outType, "__hop_str");
                    outType->ptrDepth = 1;
                    outType->readOnly = 1;
                    return 0;
                }
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "typeof")) {
            int32_t   argNode = AstNextSibling(&c->ast, callee);
            int32_t   extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            int32_t   idxNode = -1;
            int       isConstIndex = 0;
            uint32_t  constIndex = 0;
            H2TypeRef argType;
            if (argNode < 0 || extraNode >= 0) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (ResolveActivePackIndexExpr(c, argNode, &idxNode, &isConstIndex, &constIndex) == 0
                && !isConstIndex)
            {
                TypeRefSetScalar(outType, "__hop_type");
                return 0;
            }
            if (InferExprType(c, argNode, &argType) != 0 || !argType.valid) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            TypeRefSetScalar(outType, "__hop_type");
            return 0;
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "ptr")
            || SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "slice"))
        {
            int32_t   argNode = AstNextSibling(&c->ast, callee);
            int32_t   extraNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
            H2TypeRef argType;
            if (argNode < 0 || extraNode >= 0) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (InferExprType(c, argNode, &argType) != 0 || !argType.valid) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (TypeRefIsTypeValue(&argType)) {
                TypeRefSetScalar(outType, "__hop_type");
                return 0;
            }
        }
        if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "array")) {
            int32_t     typeArgNode = AstNextSibling(&c->ast, callee);
            int32_t     lenArgNode = typeArgNode >= 0 ? AstNextSibling(&c->ast, typeArgNode) : -1;
            int32_t     extraNode = lenArgNode >= 0 ? AstNextSibling(&c->ast, lenArgNode) : -1;
            H2TypeRef   typeArgType;
            H2TypeRef   lenArgType;
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
                && lenArgType.containerKind == H2TypeContainer_SCALAR && lenArgType.ptrDepth == 0
                && lenArgType.containerPtrDepth == 0 && !lenArgType.isOptional
                && lenBaseName != NULL && IsIntegerCTypeName(lenBaseName))
            {
                TypeRefSetScalar(outType, "__hop_type");
                return 0;
            }
        }
        H2CCallArgInfo callArgs[H2CCG_MAX_CALL_ARGS];
        int32_t        argNodes[H2CCG_MAX_CALL_ARGS];
        H2TypeRef      argTypes[H2CCG_MAX_CALL_ARGS];
        uint32_t       argCount = 0;
        uint32_t       i;
        const H2FnSig* resolved = NULL;
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
            {
                const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
                uint32_t       candidateLen = 0;
                const H2FnSig* single = NULL;
                GatherCallCandidatesBySlice(
                    c, cn->dataStart, cn->dataEnd, candidates, &candidateLen, &status);
                if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
                    candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
                }
                for (i = 0; i < candidateLen; i++) {
                    const H2FnSig* sig = candidates[i];
                    if (sig == NULL || (sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0
                        || sig->isVariadic != 0 || sig->paramLen != argCount)
                    {
                        continue;
                    }
                    if (single != NULL) {
                        single = NULL;
                        break;
                    }
                    single = sig;
                }
                if (single != NULL) {
                    *outType = single->returnType;
                    return 0;
                }
            }
        }
    } else if (cn != NULL && cn->kind == H2Ast_FIELD_EXPR) {
        int32_t            recvNode = AstFirstChild(&c->ast, callee);
        H2TypeRef          recvType;
        const H2FieldInfo* fieldPath[64];
        uint32_t           fieldPathLen = 0;
        const H2FieldInfo* field = NULL;
        if (recvNode >= 0 && InferExprType(c, recvNode, &recvType) == 0 && recvType.valid) {
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "kind")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    const char* kindTypeName = FindReflectKindTypeName(c);
                    TypeRefSetScalar(outType, kindTypeName != NULL ? kindTypeName : "__hop_u8");
                    return 0;
                }
            }
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "base")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    TypeRefSetScalar(outType, "__hop_type");
                    return 0;
                }
            }
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "is_alias")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    TypeRefSetScalar(outType, "__hop_bool");
                    return 0;
                }
            }
            if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "type_name")) {
                int32_t nextArgNode = AstNextSibling(&c->ast, callee);
                if (nextArgNode < 0 && TypeRefIsTypeValue(&recvType)) {
                    TypeRefSetScalar(outType, "__hop_str");
                    outType->ptrDepth = 1;
                    outType->readOnly = 1;
                    return 0;
                }
            }
            if (recvType.containerKind != H2TypeContainer_SCALAR && recvType.containerPtrDepth > 0)
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
            if (recvType.valid && recvType.containerKind == H2TypeContainer_SCALAR
                && recvType.ptrDepth == 0 && recvType.containerPtrDepth == 0
                && recvType.baseName != NULL
                && SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "impl")
                && StrEq(recvType.baseName, "__hop_MemAllocator"))
            {
                TypeRefSetScalar(outType, "__hop_uint");
                return 0;
            }
            if (recvType.valid && recvType.containerKind == H2TypeContainer_SCALAR
                && recvType.ptrDepth == 0 && recvType.containerPtrDepth == 0
                && recvType.baseName != NULL
                && SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "handler")
                && StrEq(recvType.baseName, "__hop_Logger"))
            {
                TypeRefSetScalar(outType, "void");
                return 0;
            }
            H2CCallArgInfo callArgs[H2CCG_MAX_CALL_ARGS];
            int32_t        argNodes[H2CCG_MAX_CALL_ARGS];
            H2TypeRef      argTypes[H2CCG_MAX_CALL_ARGS];
            uint32_t       argCount = 0;
            uint32_t       i;
            const H2FnSig* resolved = NULL;
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
                {
                    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
                    uint32_t       candidateLen = 0;
                    const H2FnSig* single = NULL;
                    int            nameFound = 0;
                    GatherCallCandidatesBySlice(
                        c, cn->dataStart, cn->dataEnd, candidates, &candidateLen, &nameFound);
                    if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
                        candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
                    }
                    single = FindSingleTemplateInstanceCandidate(
                        candidates, candidateLen, argCount);
                    if (single != NULL) {
                        *outType = single->returnType;
                        return 0;
                    }
                }
            }
        } else if (
            field != NULL && field->type.valid
            && field->type.containerKind == H2TypeContainer_SCALAR && field->type.ptrDepth == 0
            && field->type.containerPtrDepth == 0 && field->type.baseName != NULL)
        {
            const H2FnTypeAlias* alias = FindFnTypeAliasByName(c, field->type.baseName);
            if (alias != NULL) {
                *outType = alias->returnType;
                return 0;
            }
        }
    }
    {
        H2TypeRef            calleeType;
        const H2FnTypeAlias* alias = NULL;
        if (InferExprType(c, callee, &calleeType) == 0 && calleeType.valid
            && calleeType.containerKind == H2TypeContainer_SCALAR && calleeType.ptrDepth == 0
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

int InferExprType_NEW(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)n;
    return InferNewExprType(c, nodeId, outType);
}

int InferExprType_UNARY(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    if (InferExprType(c, child, outType) != 0) {
        return -1;
    }
    if ((H2TokenKind)n->op == H2Tok_AND) {
        if (!outType->valid) {
            TypeRefSetScalar(outType, "void");
            outType->ptrDepth = 1;
            return 0;
        }
        if (outType->containerKind == H2TypeContainer_SCALAR) {
            outType->ptrDepth++;
        } else {
            outType->containerPtrDepth++;
        }
    } else if ((H2TokenKind)n->op == H2Tok_MUL) {
        if (outType->valid && outType->containerKind != H2TypeContainer_SCALAR
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
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    const H2NameMap*       enumMap = NULL;
    int32_t                recv = AstFirstChild(&c->ast, nodeId);
    const H2AstNode*       recvNode = NodeAt(c, recv);
    const H2VariantNarrow* narrow = NULL;
    int32_t                recvLocalIdx = -1;
    H2TypeRef              recvType;
    const H2FieldInfo*     fieldPath[64];
    uint32_t               fieldPathLen = 0;
    const H2FieldInfo*     field = NULL;
    H2TypeRef              narrowFieldType;
    if (ResolveEnumSelectorByFieldExpr(c, nodeId, &enumMap, NULL, NULL, NULL, NULL) != 0
        && enumMap != NULL)
    {
        TypeRefSetScalar(outType, enumMap->cName);
        return 0;
    }
    if (recvNode != NULL && recvNode->kind == H2Ast_IDENT) {
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
    if (recvType.containerKind != H2TypeContainer_SCALAR && recvType.containerPtrDepth > 0) {
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

int InferExprType_INDEX(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t          base = AstFirstChild(&c->ast, nodeId);
    int32_t          idx = AstNextSibling(&c->ast, base);
    const H2AstNode* baseNode = NodeAt(c, base);
    if ((n->flags & H2AstFlag_INDEX_SLICE) == 0 && baseNode != NULL && baseNode->kind == H2Ast_IDENT
        && IsActivePackIdent(c, baseNode->dataStart, baseNode->dataEnd))
    {
        uint32_t         packIndex = 0;
        const H2TypeRef* elemType = NULL;
        if (ResolveActivePackConstIndex(c, idx, &packIndex, &elemType) == 0 && elemType != NULL) {
            *outType = *elemType;
            return 0;
        }
        TypeRefSetInvalid(outType);
        return 0;
    }
    if (InferExprType(c, base, outType) != 0) {
        return -1;
    }
    if ((n->flags & H2AstFlag_INDEX_SLICE) != 0) {
        if (!outType->valid) {
            TypeRefSetInvalid(outType);
            return 0;
        }
        if (TypeRefIsStringByteSequence(outType)) {
            outType->hasArrayLen = 0;
            outType->arrayLen = 0;
            return 0;
        }
        if (outType->containerKind == H2TypeContainer_ARRAY) {
            outType->containerKind =
                outType->readOnly ? H2TypeContainer_SLICE_RO : H2TypeContainer_SLICE_MUT;
            outType->containerPtrDepth = 1;
            outType->readOnly = outType->containerKind == H2TypeContainer_SLICE_RO;
        } else if (
            outType->containerKind == H2TypeContainer_SLICE_RO
            || outType->containerKind == H2TypeContainer_SLICE_MUT)
        {
            outType->containerPtrDepth = 1;
            outType->readOnly = outType->containerKind == H2TypeContainer_SLICE_RO;
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
    if (TypeRefIsStringByteSequence(outType)) {
        TypeRefSetScalar(outType, "__hop_u8");
        return 0;
    }
    if (outType->containerKind == H2TypeContainer_ARRAY
        || outType->containerKind == H2TypeContainer_SLICE_RO
        || outType->containerKind == H2TypeContainer_SLICE_MUT)
    {
        outType->containerKind = H2TypeContainer_SCALAR;
        outType->containerPtrDepth = 0;
        outType->hasArrayLen = 0;
        outType->arrayLen = 0;
        outType->readOnly = 0;
    } else if (outType->ptrDepth > 0) {
        outType->ptrDepth--;
    }
    return 0;
}

int InferExprType_CAST(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t expr = AstFirstChild(&c->ast, nodeId);
    int32_t typeNode = AstNextSibling(&c->ast, expr);
    (void)n;
    return ParseTypeRef(c, typeNode, outType);
}

int InferExprType_SIZEOF(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__hop_int");
    return 0;
}

int InferExprType_STRING(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__hop_str");
    outType->ptrDepth = 1;
    outType->readOnly = 1;
    return 0;
}

int InferExprType_BOOL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__hop_bool");
    return 0;
}

int InferExprType_INT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__hop_int");
    return 0;
}

int InferExprType_RUNE(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__hop_int");
    return 0;
}

int InferExprType_FLOAT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetScalar(outType, "__hop_f64");
    return 0;
}

int InferExprType_NULL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    (void)c;
    (void)nodeId;
    (void)n;
    TypeRefSetInvalid(outType);
    return 0;
}

int InferExprType_UNWRAP(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t inner = AstFirstChild(&c->ast, nodeId);
    (void)n;
    if (InferExprType(c, inner, outType) != 0) {
        return -1;
    }
    outType->isOptional = 0;
    return 0;
}

int InferExprType_TUPLE_EXPR(
    H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t     child = AstFirstChild(&c->ast, nodeId);
    const char* fieldNames[256];
    H2TypeRef   fieldTypes[256];
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

int InferExprType_CALL_ARG(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t inner = AstFirstChild(&c->ast, nodeId);
    (void)n;
    if (inner < 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    return InferExprType(c, inner, outType);
}

static int ExprIsHoleIdent(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    return n != NULL && n->kind == H2Ast_IDENT
        && SliceIsHoleName(c->unit->source, n->dataStart, n->dataEnd);
}

int InferExprType_BINARY(H2CBackendC* c, int32_t nodeId, const H2AstNode* n, H2TypeRef* outType) {
    int32_t     lhsNode = AstFirstChild(&c->ast, nodeId);
    int32_t     rhsNode = lhsNode >= 0 ? AstNextSibling(&c->ast, lhsNode) : -1;
    H2TypeRef   lhsType;
    H2TypeRef   rhsType;
    const char* lhsBase;
    const char* rhsBase;
    H2TokenKind op = (H2TokenKind)n->op;
    TypeRefSetInvalid(outType);
    if (lhsNode < 0 || rhsNode < 0) {
        return 0;
    }
    if (op == H2Tok_ASSIGN && ExprIsHoleIdent(c, lhsNode)) {
        return InferExprType(c, rhsNode, outType);
    }
    if (InferExprType(c, lhsNode, &lhsType) != 0 || InferExprType(c, rhsNode, &rhsType) != 0) {
        return -1;
    }
    if (op == H2Tok_ASSIGN || op == H2Tok_ADD_ASSIGN || op == H2Tok_SUB_ASSIGN
        || op == H2Tok_MUL_ASSIGN || op == H2Tok_DIV_ASSIGN || op == H2Tok_MOD_ASSIGN
        || op == H2Tok_AND_ASSIGN || op == H2Tok_OR_ASSIGN || op == H2Tok_XOR_ASSIGN
        || op == H2Tok_LSHIFT_ASSIGN || op == H2Tok_RSHIFT_ASSIGN)
    {
        *outType = lhsType;
        return 0;
    }
    if (op == H2Tok_EQ || op == H2Tok_NEQ || op == H2Tok_LT || op == H2Tok_GT || op == H2Tok_LTE
        || op == H2Tok_GTE || op == H2Tok_LOGICAL_AND || op == H2Tok_LOGICAL_OR)
    {
        TypeRefSetScalar(outType, "__hop_bool");
        return 0;
    }
    if (!lhsType.valid) {
        *outType = rhsType;
        return 0;
    }
    if (!rhsType.valid) {
        *outType = lhsType;
        return 0;
    }
    lhsBase = ResolveScalarAliasBaseName(c, lhsType.baseName);
    rhsBase = ResolveScalarAliasBaseName(c, rhsType.baseName);
    if (lhsBase == NULL) {
        lhsBase = lhsType.baseName;
    }
    if (rhsBase == NULL) {
        rhsBase = rhsType.baseName;
    }
    if (lhsType.containerKind == H2TypeContainer_SCALAR
        && rhsType.containerKind == H2TypeContainer_SCALAR && lhsType.ptrDepth == 0
        && lhsType.containerPtrDepth == 0 && !lhsType.isOptional && rhsType.ptrDepth == 0
        && rhsType.containerPtrDepth == 0 && !rhsType.isOptional && lhsBase != NULL
        && rhsBase != NULL)
    {
        if (StrEq(lhsBase, "__hop_f64") || StrEq(rhsBase, "__hop_f64")) {
            TypeRefSetScalar(outType, "__hop_f64");
            return 0;
        }
        if (StrEq(lhsBase, "__hop_f32") || StrEq(rhsBase, "__hop_f32")) {
            TypeRefSetScalar(outType, "__hop_f32");
            return 0;
        }
        if (StrEq(lhsBase, "__hop_int") && !StrEq(rhsBase, "__hop_int")) {
            *outType = rhsType;
            return 0;
        }
        if (StrEq(lhsBase, "__hop_uint") && !StrEq(rhsBase, "__hop_uint")) {
            *outType = rhsType;
            return 0;
        }
        if (StrEq(rhsBase, "__hop_int") && !StrEq(lhsBase, "__hop_int")) {
            *outType = lhsType;
            return 0;
        }
        if (StrEq(rhsBase, "__hop_uint") && !StrEq(lhsBase, "__hop_uint")) {
            *outType = lhsType;
            return 0;
        }
    }
    *outType = lhsType;
    return 0;
}

int InferExprType(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }

    switch (n->kind) {
        case H2Ast_IDENT:             return InferExprType_IDENT(c, nodeId, n, outType);
        case H2Ast_COMPOUND_LIT:      return InferExprType_COMPOUND_LIT(c, nodeId, n, outType);
        case H2Ast_ARRAY_LIT:         return InferArrayLiteralType(c, nodeId, NULL, outType);
        case H2Ast_CALL_WITH_CONTEXT: return InferExprType_CALL_WITH_CONTEXT(c, nodeId, n, outType);
        case H2Ast_CALL:              return InferExprType_CALL(c, nodeId, n, outType);
        case H2Ast_NEW:               return InferExprType_NEW(c, nodeId, n, outType);
        case H2Ast_UNARY:             return InferExprType_UNARY(c, nodeId, n, outType);
        case H2Ast_FIELD_EXPR:        return InferExprType_FIELD_EXPR(c, nodeId, n, outType);
        case H2Ast_INDEX:             return InferExprType_INDEX(c, nodeId, n, outType);
        case H2Ast_CAST:              return InferExprType_CAST(c, nodeId, n, outType);
        case H2Ast_SIZEOF:            return InferExprType_SIZEOF(c, nodeId, n, outType);
        case H2Ast_STRING:            return InferExprType_STRING(c, nodeId, n, outType);
        case H2Ast_BOOL:              return InferExprType_BOOL(c, nodeId, n, outType);
        case H2Ast_INT:               return InferExprType_INT(c, nodeId, n, outType);
        case H2Ast_RUNE:              return InferExprType_RUNE(c, nodeId, n, outType);
        case H2Ast_FLOAT:             return InferExprType_FLOAT(c, nodeId, n, outType);
        case H2Ast_NULL:              return InferExprType_NULL(c, nodeId, n, outType);
        case H2Ast_UNWRAP:            return InferExprType_UNWRAP(c, nodeId, n, outType);
        case H2Ast_BINARY:            return InferExprType_BINARY(c, nodeId, n, outType);
        case H2Ast_TUPLE_EXPR:        return InferExprType_TUPLE_EXPR(c, nodeId, n, outType);
        case H2Ast_CALL_ARG:          return InferExprType_CALL_ARG(c, nodeId, n, outType);
        case H2Ast_TYPE_VALUE:        TypeRefSetScalar(outType, "__hop_type"); return 0;
        default:                      TypeRefSetInvalid(outType); return 0;
    }
}

int InferNewExprType(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType) {
    int32_t  typeNode = -1;
    int32_t  countNode = -1;
    int32_t  initNode = -1;
    int32_t  allocNode = -1;
    int64_t  countValue = 0;
    int      countIsConst = 0;
    uint32_t arrayLen;

    TypeRefSetInvalid(outType);
    if (NodeAt(c, nodeId) != NULL && (NodeAt(c, nodeId)->flags & H2AstFlag_NEW_HAS_ARRAY_LIT) != 0)
    {
        int32_t litNode = AstFirstChild(&c->ast, nodeId);
        if (InferArrayLiteralType(c, litNode, NULL, outType) != 0 || !outType->valid) {
            TypeRefSetInvalid(outType);
            return 0;
        }
        outType->containerPtrDepth++;
        return 0;
    }
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
            outType->containerKind = H2TypeContainer_ARRAY;
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

const char* UnaryOpString(H2TokenKind op) {
    switch (op) {
        case H2Tok_ADD: return "+";
        case H2Tok_SUB: return "-";
        case H2Tok_NOT: return "!";
        case H2Tok_MUL: return "*";
        case H2Tok_AND: return "&";
        default:        return "";
    }
}

const char* BinaryOpString(H2TokenKind op) {
    switch (op) {
        case H2Tok_ASSIGN:        return "=";
        case H2Tok_ADD:           return "+";
        case H2Tok_SUB:           return "-";
        case H2Tok_MUL:           return "*";
        case H2Tok_DIV:           return "/";
        case H2Tok_MOD:           return "%";
        case H2Tok_AND:           return "&";
        case H2Tok_OR:            return "|";
        case H2Tok_XOR:           return "^";
        case H2Tok_LSHIFT:        return "<<";
        case H2Tok_RSHIFT:        return ">>";
        case H2Tok_EQ:            return "==";
        case H2Tok_NEQ:           return "!=";
        case H2Tok_LT:            return "<";
        case H2Tok_GT:            return ">";
        case H2Tok_LTE:           return "<=";
        case H2Tok_GTE:           return ">=";
        case H2Tok_LOGICAL_AND:   return "&&";
        case H2Tok_LOGICAL_OR:    return "||";
        case H2Tok_ADD_ASSIGN:    return "+=";
        case H2Tok_SUB_ASSIGN:    return "-=";
        case H2Tok_MUL_ASSIGN:    return "*=";
        case H2Tok_DIV_ASSIGN:    return "/=";
        case H2Tok_MOD_ASSIGN:    return "%=";
        case H2Tok_AND_ASSIGN:    return "&=";
        case H2Tok_OR_ASSIGN:     return "|=";
        case H2Tok_XOR_ASSIGN:    return "^=";
        case H2Tok_LSHIFT_ASSIGN: return "<<=";
        case H2Tok_RSHIFT_ASSIGN: return ">>=";
        default:                  return "";
    }
}

int EmitHexByte(H2Buf* b, uint8_t value) {
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

int BufAppendHexU64Literal(H2Buf* b, uint64_t value) {
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

int EmitStringLiteralPool(H2CBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->stringLitLen; i++) {
        uint32_t               j;
        const H2StringLiteral* lit = &c->stringLits[i];
        if (BufAppendCStr(&c->out, "static const __hop_u8 hop_lit_ro_bytes_") != 0
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
            || BufAppendCStr(&c->out, "static const __hop_str hop_lit_ro_") != 0
            || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(
                   &c->out,
                   " = { (__hop_u8*)(uintptr_t)"
                   "(const void*)hop_lit_ro_bytes_")
                   != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, ", ") != 0
            || BufAppendU32(&c->out, lit->len) != 0 || BufAppendCStr(&c->out, "u };\n") != 0)
        {
            return -1;
        }

        if (BufAppendCStr(&c->out, "static __hop_u8 hop_lit_rw_bytes_") != 0
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
            || BufAppendCStr(&c->out, "static __hop_str hop_lit_rw_") != 0
            || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(&c->out, " = { hop_lit_rw_bytes_") != 0
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

int EmitExpr(H2CBackendC* c, int32_t nodeId);
int EmitExprCoerced(H2CBackendC* c, int32_t exprNode, const H2TypeRef* _Nullable dstType);
int EmitArrayLiteral(H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType);
int EmitAssertFormatArg(H2CBackendC* c, int32_t nodeId);

int IsStrBaseName(const char* _Nullable s) {
    return s != NULL && (StrEq(s, "__hop_str") || StrEq(s, "builtin__str"));
}

int TypeRefIsStr(const H2TypeRef* t) {
    return t->valid && t->containerKind == H2TypeContainer_SCALAR && IsStrBaseName(t->baseName);
}

static int TypeRefIsBorrowedStrValue(const H2TypeRef* t) {
    return TypeRefIsStr(t) && t->containerPtrDepth == 0 && t->ptrDepth == 1 && t->readOnly != 0;
}

static int TypeRefIsPointerBackedStr(const H2TypeRef* t) {
    return TypeRefIsStr(t) && t->containerPtrDepth == 0 && !TypeRefIsBorrowedStrValue(t);
}

static int TypeRefIsMutableStrPointer(const H2TypeRef* t) {
    return TypeRefIsPointerBackedStr(t) && t->readOnly == 0;
}

static int TypeRefIsStrValueLike(const H2TypeRef* t) {
    return TypeRefIsBorrowedStrValue(t);
}

static int TypeRefIsStringByteSequence(const H2TypeRef* t) {
    return TypeRefIsStr(t) && t->containerPtrDepth == 0 && t->ptrDepth <= 1;
}

int EmitStringLiteralValue(H2CBackendC* c, int32_t literalId, int writable) {
    if (BufAppendCStr(&c->out, "hop_lit_") != 0
        || BufAppendCStr(&c->out, writable ? "rw_" : "ro_") != 0
        || BufAppendU32(&c->out, (uint32_t)literalId) != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitStringLiteralPointer(H2CBackendC* c, int32_t literalId, int writable) {
    if (BufAppendCStr(&c->out, "((__hop_str*)(void*)&hop_lit_") != 0
        || BufAppendCStr(&c->out, writable ? "rw_" : "ro_") != 0
        || BufAppendU32(&c->out, (uint32_t)literalId) != 0 || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitStrValueExpr(H2CBackendC* c, int32_t exprNode, const H2TypeRef* type) {
    if (c == NULL || type == NULL || !TypeRefIsStr(type)) {
        return -1;
    }
    if (TypeRefIsPointerBackedStr(type)) {
        if (BufAppendCStr(&c->out, "(*(") != 0 || EmitExpr(c, exprNode) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }
    return EmitExpr(c, exprNode);
}

static int EmitStrValueName(H2CBackendC* c, const char* name, const H2TypeRef* type) {
    if (c == NULL || name == NULL || type == NULL || !TypeRefIsStr(type)) {
        return -1;
    }
    if (TypeRefIsPointerBackedStr(type)) {
        if (BufAppendCStr(&c->out, "(*(") != 0 || BufAppendCStr(&c->out, name) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }
    return BufAppendCStr(&c->out, name);
}

static int EmitStrAddressExpr(H2CBackendC* c, int32_t exprNode, const H2TypeRef* type) {
    uint32_t tempId;
    if (c == NULL || type == NULL || !TypeRefIsStr(type)) {
        return -1;
    }
    if (TypeRefIsPointerBackedStr(type)) {
        if (BufAppendCStr(&c->out, "((const __hop_str*)(") != 0 || EmitExpr(c, exprNode) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }
    tempId = FmtNextTempId(c);
    if (BufAppendCStr(&c->out, "(__extension__({ __hop_str __hop_panic_msg_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = ") != 0
        || EmitStrValueExpr(c, exprNode, type) != 0
        || BufAppendCStr(&c->out, "; &__hop_panic_msg_") != 0 || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "; }))") != 0)
    {
        return -1;
    }
    return 0;
}

int TypeRefContainerWritable(const H2TypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if (t->containerKind == H2TypeContainer_ARRAY || t->containerKind == H2TypeContainer_SLICE_MUT)
    {
        return t->readOnly == 0;
    }
    /* For slice-pointer forms, readOnly distinguishes &[...] from *[...]. */
    if (t->containerKind == H2TypeContainer_SLICE_RO && t->containerPtrDepth > 0) {
        return t->readOnly == 0;
    }
    return 0;
}

int EmitElementTypeName(H2CBackendC* c, const H2TypeRef* t, int asConst) {
    int         i;
    const char* baseName;
    int         ptrDepth;
    if (!t->valid || t->baseName == NULL) {
        return -1;
    }
    if (asConst && BufAppendCStr(&c->out, "const ") != 0) {
        return -1;
    }
    if (TypeRefIsBorrowedStrValue(t)
        || (t->containerKind == H2TypeContainer_ARRAY && t->containerPtrDepth == 0
            && t->ptrDepth == 1 && IsStrBaseName(t->baseName)))
    {
        baseName = "__hop_str";
        ptrDepth = 0;
    } else {
        baseName = t->baseName;
        ptrDepth = t->ptrDepth;
    }
    if (BufAppendCStr(&c->out, baseName) != 0) {
        return -1;
    }
    for (i = 0; i < ptrDepth; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitLenExprFromType(H2CBackendC* c, int32_t exprNode, const H2TypeRef* t) {
    if (TypeRefIsStr(t)) {
        if (BufAppendCStr(&c->out, "__hop_len(") != 0 || EmitStrValueExpr(c, exprNode, t) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (t->containerKind == H2TypeContainer_ARRAY && t->hasArrayLen) {
        if (t->containerPtrDepth > 0) {
            if (BufAppendCStr(&c->out, "((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ") == 0 ? 0 : ") != 0
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
    if (t->containerKind == H2TypeContainer_SLICE_RO
        || t->containerKind == H2TypeContainer_SLICE_MUT)
    {
        if (t->containerPtrDepth > 0) {
            int stars = SliceStructPtrDepth(t);
            if (stars > 0) {
                if (BufAppendCStr(&c->out, "((") != 0 || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, ") == 0 ? 0 : (__hop_int)((") != 0
                    || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, ")->len))") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "(__hop_int)((") != 0 || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, ").len)") != 0)
                {
                    return -1;
                }
            }
        } else {
            if (BufAppendCStr(&c->out, "(__hop_int)((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").len)") != 0)
            {
                return -1;
            }
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "__hop_len(") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitElemPtrExpr(
    H2CBackendC* c, int32_t baseNode, const H2TypeRef* baseType, int wantWritableElem) {
    int elemConst = !wantWritableElem;
    if (TypeRefIsStr(baseType)) {
        const char* ptrType = elemConst ? "const __hop_u8*" : "__hop_u8*";
        if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, ptrType) != 0
            || BufAppendCStr(&c->out, ")(") != 0)
        {
            return -1;
        }
        if (TypeRefIsMutableStrPointer(baseType)) {
            if (BufAppendCStr(&c->out, "((") != 0 || EmitExpr(c, baseNode) != 0
                || BufAppendCStr(&c->out, ") == 0 ? (const void*)0 : (const void*)__hop_cstr(*(")
                       != 0
                || EmitExpr(c, baseNode) != 0 || BufAppendCStr(&c->out, ")))") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "(const void*)__hop_cstr(") != 0
                || EmitStrValueExpr(c, baseNode, baseType) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
        }
        return BufAppendCStr(&c->out, "))");
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitElementTypeName(c, baseType, elemConst) != 0
        || BufAppendCStr(&c->out, "*)(") != 0)
    {
        return -1;
    }
    if (baseType->containerKind == H2TypeContainer_ARRAY) {
        if (EmitExpr(c, baseNode) != 0) {
            return -1;
        }
    } else if (
        baseType->containerKind == H2TypeContainer_SLICE_RO
        || baseType->containerKind == H2TypeContainer_SLICE_MUT)
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

static int EmitLenExprFromNameType(H2CBackendC* c, const char* name, const H2TypeRef* t) {
    if (TypeRefIsStr(t)) {
        if (BufAppendCStr(&c->out, "__hop_len(") != 0 || EmitStrValueName(c, name, t) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (t->containerKind == H2TypeContainer_ARRAY && t->hasArrayLen) {
        if (t->containerPtrDepth > 0) {
            if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, ") == 0 ? 0 : ") != 0
                || BufAppendU32(&c->out, t->arrayLen) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        return BufAppendU32(&c->out, t->arrayLen);
    }
    if (t->containerKind == H2TypeContainer_SLICE_RO
        || t->containerKind == H2TypeContainer_SLICE_MUT)
    {
        if (t->containerPtrDepth > 0) {
            int stars = SliceStructPtrDepth(t);
            if (stars > 0) {
                if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, name) != 0
                    || BufAppendCStr(&c->out, ") == 0 ? 0 : (__hop_int)((") != 0
                    || BufAppendCStr(&c->out, name) != 0 || BufAppendCStr(&c->out, ")->len))") != 0)
                {
                    return -1;
                }
                return 0;
            }
        }
        if (BufAppendCStr(&c->out, "(__hop_int)((") != 0 || BufAppendCStr(&c->out, name) != 0
            || BufAppendCStr(&c->out, ").len)") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "__hop_len(") != 0 || BufAppendCStr(&c->out, name) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitForInElemExprFromNameType(
    H2CBackendC* c, const char* name, const char* idxName, const H2TypeRef* baseType) {
    if (TypeRefIsStr(baseType)) {
        if (BufAppendCStr(&c->out, "((const __hop_u8*)(") != 0) {
            return -1;
        }
        if (TypeRefIsMutableStrPointer(baseType)) {
            if (BufAppendCStr(&c->out, "__hop_cstr(*(") != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "__hop_cstr(") != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, "))") != 0 || BufAppendChar(&c->out, '[') != 0
            || BufAppendCStr(&c->out, idxName) != 0 || BufAppendChar(&c->out, ']') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (baseType->containerKind == H2TypeContainer_ARRAY
        || baseType->containerKind == H2TypeContainer_SLICE_RO
        || baseType->containerKind == H2TypeContainer_SLICE_MUT)
    {
        int elemConst = !TypeRefContainerWritable(baseType);
        if (BufAppendChar(&c->out, '(') != 0 || BufAppendChar(&c->out, '(') != 0
            || EmitElementTypeName(c, baseType, elemConst) != 0
            || BufAppendCStr(&c->out, "*)(") != 0)
        {
            return -1;
        }
        if (baseType->containerKind == H2TypeContainer_ARRAY) {
            if (BufAppendCStr(&c->out, name) != 0) {
                return -1;
            }
        } else if (baseType->containerPtrDepth > 0 && SliceStructPtrDepth(baseType) > 0) {
            if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, ")->ptr") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, ").ptr") != 0)
            {
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, "))") != 0 || BufAppendChar(&c->out, '[') != 0
            || BufAppendCStr(&c->out, idxName) != 0 || BufAppendChar(&c->out, ']') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, name) != 0 || BufAppendChar(&c->out, '[') != 0
        || BufAppendCStr(&c->out, idxName) != 0 || BufAppendChar(&c->out, ']') != 0)
    {
        return -1;
    }
    return 0;
}

static int ResolveForInElemType(
    const H2TypeRef* sourceType, H2TypeRef* outElemType, int* outMutable) {
    H2TypeRef t = *sourceType;
    if (!t.valid) {
        return -1;
    }
    if (TypeRefIsStr(&t)) {
        TypeRefSetScalar(outElemType, "__hop_u8");
        *outMutable = 0;
        return 0;
    }
    if (t.containerKind == H2TypeContainer_ARRAY || t.containerKind == H2TypeContainer_SLICE_RO
        || t.containerKind == H2TypeContainer_SLICE_MUT)
    {
        *outMutable = TypeRefContainerWritable(&t);
        t.containerKind = H2TypeContainer_SCALAR;
        t.containerPtrDepth = 0;
        t.hasArrayLen = 0;
        t.arrayLen = 0;
        t.readOnly = (t.ptrDepth == 1 && IsStrBaseName(t.baseName)) ? 1 : 0;
        *outElemType = t;
        return 0;
    }
    return -1;
}

typedef enum {
    H2CCGForInValueMode_VALUE = 0,
    H2CCGForInValueMode_REF,
    H2CCGForInValueMode_ANY,
} H2CCGForInValueMode;

static int ForInTypeRefIsRef(const H2TypeRef* t) {
    return t->valid && t->containerKind == H2TypeContainer_SCALAR && t->ptrDepth > 0
        && t->readOnly != 0;
}

static int ForInTypeRefIsPtr(const H2TypeRef* t) {
    return t->valid && t->containerKind == H2TypeContainer_SCALAR && t->ptrDepth > 0
        && t->readOnly == 0;
}

static int ForInTypeRefDerefOne(const H2TypeRef* t, H2TypeRef* out) {
    H2TypeRef v = *t;
    if (!v.valid) {
        return -1;
    }
    if (v.containerKind == H2TypeContainer_SCALAR) {
        if (v.ptrDepth <= 0) {
            return -1;
        }
        v.ptrDepth--;
        if (v.ptrDepth == 0) {
            v.readOnly = 0;
        }
    } else {
        if (v.containerPtrDepth <= 0) {
            return -1;
        }
        v.containerPtrDepth--;
    }
    *out = v;
    return 0;
}

static int ForInPayloadTypeFromOptional(const H2TypeRef* returnType, H2TypeRef* outPayloadType) {
    H2TypeRef payload = *returnType;
    if (!payload.valid || !payload.isOptional) {
        return 0;
    }
    payload.isOptional = 0;
    *outPayloadType = payload;
    return 1;
}

static int ForInValueLocalTypeFromPayload(
    const H2TypeRef* payloadType, H2CCGForInValueMode valueMode, H2TypeRef* outLocalType) {
    if (valueMode == H2CCGForInValueMode_REF) {
        if (!ForInTypeRefIsRef(payloadType) && !ForInTypeRefIsPtr(payloadType)) {
            return -1;
        }
        *outLocalType = *payloadType;
        return 0;
    }
    if (valueMode == H2CCGForInValueMode_VALUE) {
        if (ForInTypeRefIsRef(payloadType) || ForInTypeRefIsPtr(payloadType)) {
            return ForInTypeRefDerefOne(payloadType, outLocalType);
        }
        *outLocalType = *payloadType;
        return 0;
    }
    if (valueMode == H2CCGForInValueMode_ANY) {
        *outLocalType = *payloadType;
        return 0;
    }
    return -1;
}

static int ForInValueLocalTypeFromDirect(
    const H2TypeRef* valueType, H2CCGForInValueMode valueMode, H2TypeRef* outLocalType) {
    if (!valueType->valid) {
        return -1;
    }
    if (valueMode == H2CCGForInValueMode_ANY) {
        *outLocalType = *valueType;
        return 0;
    }
    if (valueMode == H2CCGForInValueMode_REF) {
        if (!ForInTypeRefIsRef(valueType) && !ForInTypeRefIsPtr(valueType)) {
            return -1;
        }
        *outLocalType = *valueType;
        return 0;
    }
    if (valueMode == H2CCGForInValueMode_VALUE) {
        if (ForInTypeRefIsRef(valueType) || ForInTypeRefIsPtr(valueType)) {
            return ForInTypeRefDerefOne(valueType, outLocalType);
        }
        *outLocalType = *valueType;
        return 0;
    }
    return -1;
}

static int ForInTuple2FieldTypesFromPayload(
    const H2CBackendC* c,
    const H2TypeRef*   payloadType,
    H2TypeRef*         outKeyType,
    H2TypeRef*         outValueType) {
    H2TypeRef             pairType = *payloadType;
    H2TypeRef             expandedPairType;
    const H2AnonTypeInfo* tupleInfo = NULL;
    if (ForInTypeRefIsRef(payloadType) || ForInTypeRefIsPtr(payloadType)) {
        if (ForInTypeRefDerefOne(payloadType, &pairType) != 0) {
            return -1;
        }
    }
    if (ExpandAliasSourceType(c, &pairType, &expandedPairType)) {
        pairType = expandedPairType;
    }
    if (!TypeRefTupleInfo(c, &pairType, &tupleInfo) || tupleInfo == NULL
        || tupleInfo->fieldCount != 2u)
    {
        return -1;
    }
    *outKeyType = c->fieldInfos[tupleInfo->fieldStart].type;
    *outValueType = c->fieldInfos[tupleInfo->fieldStart + 1u].type;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
static int ResolveForInIteratorSigFromType(
    H2CBackendC*     c,
    const H2TypeRef* sourceType,
    const H2FnSig**  outSig,
    const char**     outCalleeName,
    H2TypeRef*       outIterType) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    const H2FnSig* bestSig = NULL;
    const char*    bestName = NULL;
    uint8_t        bestCost = 0;
    int            ambiguous = 0;
    uint32_t       i;
    candidateLen = FindFnSigCandidatesByName(
        c, "__iterator", candidates, (uint32_t)(sizeof(candidates) / sizeof(candidates[0])));
    if (candidateLen == 0) {
        return 1;
    }
    if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
        candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
    }
    for (i = 0; i < candidateLen; i++) {
        const H2FnSig* sig = candidates[i];
        uint8_t        cost = 0;
        if (sig->isVariadic || sig->paramLen != 1
            || TypeRefAssignableCost(c, &sig->paramTypes[0], sourceType, &cost) != 0)
        {
            continue;
        }
        if (bestSig == NULL) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            ambiguous = 0;
        } else if (cost == bestCost && sig != bestSig) {
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
    *outCalleeName = bestName;
    *outIterType = bestSig->returnType;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
static int ResolveForInIteratorSig(
    H2CBackendC*     c,
    const H2TypeRef* sourceType,
    const H2FnSig**  outSig,
    const char**     outCalleeName,
    H2TypeRef*       outIterType,
    int*             outAutoRefSource) {
    int       rc;
    H2TypeRef autoRefType;
    *outAutoRefSource = 0;
    rc = ResolveForInIteratorSigFromType(c, sourceType, outSig, outCalleeName, outIterType);
    if (rc != 2) {
        return rc;
    }
    autoRefType = *sourceType;
    if (autoRefType.containerKind == H2TypeContainer_SCALAR) {
        autoRefType.ptrDepth++;
    } else {
        autoRefType.containerPtrDepth++;
    }
    rc = ResolveForInIteratorSigFromType(c, &autoRefType, outSig, outCalleeName, outIterType);
    if (rc == 0) {
        *outAutoRefSource = 1;
    }
    return rc;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous, 4 bad return type */
static int ResolveForInNextValueSig(
    H2CBackendC*        c,
    const H2TypeRef*    iterPtrType,
    H2CCGForInValueMode valueMode,
    const H2FnSig**     outSig,
    const char**        outCalleeName,
    H2TypeRef*          outValueLocalType) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    const H2FnSig* bestSig = NULL;
    const char*    bestName = NULL;
    uint8_t        bestCost = 0;
    H2TypeRef      bestValueLocalType;
    int            haveBest = 0;
    int            ambiguous = 0;
    int            badReturnType = 0;
    uint32_t       i;
    TypeRefSetInvalid(&bestValueLocalType);

    candidateLen = FindFnSigCandidatesByName(
        c, "next_value", candidates, (uint32_t)(sizeof(candidates) / sizeof(candidates[0])));
    if (candidateLen == 0) {
        return 1;
    }
    if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
        candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
    }
    for (i = 0; i < candidateLen; i++) {
        const H2FnSig* sig = candidates[i];
        uint8_t        cost = 0;
        H2TypeRef      payloadType;
        H2TypeRef      valueLocalType;
        TypeRefSetInvalid(&payloadType);
        TypeRefSetInvalid(&valueLocalType);
        if (sig->isVariadic || sig->paramLen != 1
            || TypeRefAssignableCost(c, &sig->paramTypes[0], iterPtrType, &cost) != 0)
        {
            continue;
        }
        if (!ForInPayloadTypeFromOptional(&sig->returnType, &payloadType)
            || (valueMode != H2CCGForInValueMode_ANY
                && ForInValueLocalTypeFromPayload(&payloadType, valueMode, &valueLocalType) != 0))
        {
            badReturnType = 1;
            continue;
        }
        if (!haveBest) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            bestValueLocalType = valueLocalType;
            haveBest = 1;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            bestValueLocalType = valueLocalType;
            ambiguous = 0;
        } else if (cost == bestCost && sig != bestSig) {
            ambiguous = 1;
        }
    }
    if (!haveBest) {
        return badReturnType ? 4 : 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outSig = bestSig;
    *outCalleeName = bestName;
    *outValueLocalType = bestValueLocalType;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous, 4 bad return type */
static int ResolveForInNextKeySig(
    H2CBackendC*     c,
    const H2TypeRef* iterPtrType,
    const H2FnSig**  outSig,
    const char**     outCalleeName,
    H2TypeRef*       outKeyLocalType,
    H2TypeRef*       outKeyOptionalType) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    const H2FnSig* bestSig = NULL;
    const char*    bestName = NULL;
    uint8_t        bestCost = 0;
    H2TypeRef      bestKeyLocalType;
    H2TypeRef      bestKeyOptionalType;
    int            haveBest = 0;
    int            ambiguous = 0;
    int            badReturnType = 0;
    uint32_t       i;
    TypeRefSetInvalid(&bestKeyLocalType);
    TypeRefSetInvalid(&bestKeyOptionalType);

    candidateLen = FindFnSigCandidatesByName(
        c, "next_key", candidates, (uint32_t)(sizeof(candidates) / sizeof(candidates[0])));
    if (candidateLen == 0) {
        return 1;
    }
    if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
        candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
    }
    for (i = 0; i < candidateLen; i++) {
        const H2FnSig* sig = candidates[i];
        uint8_t        cost = 0;
        H2TypeRef      payloadType;
        if (sig->isVariadic || sig->paramLen != 1
            || TypeRefAssignableCost(c, &sig->paramTypes[0], iterPtrType, &cost) != 0)
        {
            continue;
        }
        if (!ForInPayloadTypeFromOptional(&sig->returnType, &payloadType)) {
            badReturnType = 1;
            continue;
        }
        if (!haveBest) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            bestKeyLocalType = payloadType;
            bestKeyOptionalType = sig->returnType;
            haveBest = 1;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            bestKeyLocalType = payloadType;
            bestKeyOptionalType = sig->returnType;
            ambiguous = 0;
        } else if (cost == bestCost && sig != bestSig) {
            ambiguous = 1;
        }
    }
    if (!haveBest) {
        return badReturnType ? 4 : 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outSig = bestSig;
    *outCalleeName = bestName;
    *outKeyLocalType = bestKeyLocalType;
    *outKeyOptionalType = bestKeyOptionalType;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous, 4 bad return type */
static int ResolveForInNextKeyAndValueSig(
    H2CBackendC*        c,
    const H2TypeRef*    iterPtrType,
    H2CCGForInValueMode valueMode,
    const H2FnSig**     outSig,
    const char**        outCalleeName,
    H2TypeRef*          outKeyLocalType,
    H2TypeRef*          outValueLocalType,
    H2TypeRef*          outPairOptionalType) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    const H2FnSig* bestSig = NULL;
    const char*    bestName = NULL;
    uint8_t        bestCost = 0;
    H2TypeRef      bestKeyLocalType;
    H2TypeRef      bestValueLocalType;
    H2TypeRef      bestPairOptionalType;
    int            haveBest = 0;
    int            ambiguous = 0;
    int            badReturnType = 0;
    uint32_t       i;
    TypeRefSetInvalid(&bestKeyLocalType);
    TypeRefSetInvalid(&bestValueLocalType);
    TypeRefSetInvalid(&bestPairOptionalType);

    candidateLen = FindFnSigCandidatesByName(
        c,
        "next_key_and_value",
        candidates,
        (uint32_t)(sizeof(candidates) / sizeof(candidates[0])));
    if (candidateLen == 0) {
        return 1;
    }
    if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
        candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
    }
    for (i = 0; i < candidateLen; i++) {
        const H2FnSig* sig = candidates[i];
        uint8_t        cost = 0;
        H2TypeRef      payloadType;
        H2TypeRef      keyFieldType;
        H2TypeRef      valueFieldType;
        H2TypeRef      valueLocalType;
        TypeRefSetInvalid(&payloadType);
        TypeRefSetInvalid(&keyFieldType);
        TypeRefSetInvalid(&valueFieldType);
        TypeRefSetInvalid(&valueLocalType);
        if (sig->isVariadic || sig->paramLen != 1
            || TypeRefAssignableCost(c, &sig->paramTypes[0], iterPtrType, &cost) != 0)
        {
            continue;
        }
        if (!ForInPayloadTypeFromOptional(&sig->returnType, &payloadType)
            || ForInTuple2FieldTypesFromPayload(c, &payloadType, &keyFieldType, &valueFieldType)
                   != 0
            || (valueMode != H2CCGForInValueMode_ANY
                && ForInValueLocalTypeFromDirect(&valueFieldType, valueMode, &valueLocalType) != 0))
        {
            badReturnType = 1;
            continue;
        }
        if (!haveBest) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            bestKeyLocalType = keyFieldType;
            bestValueLocalType = valueLocalType;
            bestPairOptionalType = sig->returnType;
            haveBest = 1;
            ambiguous = 0;
        } else if (cost < bestCost) {
            bestSig = sig;
            bestName = sig->cName;
            bestCost = cost;
            bestKeyLocalType = keyFieldType;
            bestValueLocalType = valueLocalType;
            bestPairOptionalType = sig->returnType;
            ambiguous = 0;
        } else if (cost == bestCost && sig != bestSig) {
            ambiguous = 1;
        }
    }
    if (!haveBest) {
        return badReturnType ? 4 : 2;
    }
    if (ambiguous) {
        return 3;
    }
    *outSig = bestSig;
    *outCalleeName = bestName;
    *outKeyLocalType = bestKeyLocalType;
    *outValueLocalType = bestValueLocalType;
    *outPairOptionalType = bestPairOptionalType;
    return 0;
}

int EmitSliceExpr(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    int32_t          baseNode = AstFirstChild(&c->ast, nodeId);
    int32_t          child = AstNextSibling(&c->ast, baseNode);
    int              hasStart = (n->flags & H2AstFlag_INDEX_HAS_START) != 0;
    int              hasEnd = (n->flags & H2AstFlag_INDEX_HAS_END) != 0;
    int32_t          startNode = -1;
    int32_t          endNode = -1;
    H2TypeRef        baseType;
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
    if (TypeRefIsStringByteSequence(&baseType)) {
        outMut = baseType.ptrDepth > 0 && !baseType.readOnly;
        if (baseType.ptrDepth > 0) {
            if (BufAppendCStr(&c->out, "(&(__hop_str){ .ptr = ") != 0) {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "((__hop_str){ .ptr = ") != 0) {
                return -1;
            }
        }
        if (EmitElemPtrExpr(c, baseNode, &baseType, outMut) != 0
            || BufAppendCStr(&c->out, " + (__hop_int)(") != 0)
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
        if (BufAppendCStr(&c->out, "), .len = (__hop_int)(") != 0) {
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
        return BufAppendCStr(&c->out, ") })");
    }
    if (BufAppendCStr(&c->out, "((") != 0
        || BufAppendCStr(&c->out, outMut ? "__hop_slice_mut" : "__hop_slice_ro") != 0
        || BufAppendCStr(&c->out, "){ ") != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, outMut ? "(void*)(" : "(const void*)(") != 0
        || EmitElemPtrExpr(c, baseNode, &baseType, outMut) != 0
        || BufAppendCStr(&c->out, " + (__hop_int)(") != 0)
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
    if (BufAppendCStr(&c->out, ")), (__hop_int)((") != 0) {
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

int TypeRefIsPointerLike(const H2TypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if (t->containerKind == H2TypeContainer_SCALAR) {
        return t->ptrDepth > 0;
    }
    if (t->containerKind == H2TypeContainer_ARRAY) {
        return t->ptrDepth > 0 || t->containerPtrDepth > 0;
    }
    if (t->containerKind == H2TypeContainer_SLICE_RO
        || t->containerKind == H2TypeContainer_SLICE_MUT)
    {
        return t->ptrDepth > 0 || t->containerPtrDepth > 0;
    }
    return 0;
}

static int TypeRefOptionalPayloadTypeExpr(const H2TypeRef* optionalType, H2TypeRef* outPayload) {
    if (optionalType == NULL || outPayload == NULL || !optionalType->valid
        || !optionalType->isOptional)
    {
        return 0;
    }
    *outPayload = *optionalType;
    outPayload->isOptional = 0;
    return 1;
}

static int TypeRefOptionalPayloadUsesNullSentinelExpr(const H2TypeRef* payload) {
    if (payload == NULL || !payload->valid) {
        return 0;
    }
    if (TypeRefIsPointerLike(payload)) {
        return 1;
    }
    return (payload->containerKind == H2TypeContainer_SLICE_RO
            || payload->containerKind == H2TypeContainer_SLICE_MUT)
        && payload->ptrDepth == 0 && payload->containerPtrDepth == 0;
}

static int EmitOptionalIsSomeFromNameType(
    H2CBackendC* c, const char* name, const H2TypeRef* optionalType) {
    H2TypeRef payloadType;
    if (c == NULL || name == NULL || optionalType == NULL
        || !TypeRefOptionalPayloadTypeExpr(optionalType, &payloadType))
    {
        return -1;
    }
    if (TypeRefOptionalPayloadUsesNullSentinelExpr(&payloadType)) {
        if ((payloadType.containerKind == H2TypeContainer_SLICE_RO
             || payloadType.containerKind == H2TypeContainer_SLICE_MUT)
            && SliceStructPtrDepth(&payloadType) == 0)
        {
            if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, ".ptr != NULL)") != 0)
            {
                return -1;
            }
        } else if (TypeRefIsBorrowedStrValue(&payloadType)) {
            if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, ".ptr != NULL)") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, " != NULL)") != 0)
            {
                return -1;
            }
        }
        return 0;
    }
    if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
        || BufAppendCStr(&c->out, ".__hop_tag != 0u)") != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitOptionalIsSomeExpr(
    H2CBackendC* c, int32_t exprNode, const H2TypeRef* optionalType, int useCoercedExpr) {
    H2TypeRef payloadType;
    if (c == NULL || optionalType == NULL
        || !TypeRefOptionalPayloadTypeExpr(optionalType, &payloadType))
    {
        return -1;
    }
    if (TypeRefOptionalPayloadUsesNullSentinelExpr(&payloadType)) {
        if ((payloadType.containerKind == H2TypeContainer_SLICE_RO
             || payloadType.containerKind == H2TypeContainer_SLICE_MUT)
            && SliceStructPtrDepth(&payloadType) == 0)
        {
            if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_opt = ") != 0) {
                return -1;
            }
            if (useCoercedExpr) {
                if (EmitExprCoerced(c, exprNode, optionalType) != 0) {
                    return -1;
                }
            } else if (EmitExpr(c, exprNode) != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, "; __hop_opt.ptr != NULL; }))") != 0) {
                return -1;
            }
        } else if (TypeRefIsBorrowedStrValue(&payloadType)) {
            if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_opt = ") != 0) {
                return -1;
            }
            if (useCoercedExpr) {
                if (EmitExprCoerced(c, exprNode, optionalType) != 0) {
                    return -1;
                }
            } else if (EmitExpr(c, exprNode) != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, "; __hop_opt.ptr != NULL; }))") != 0) {
                return -1;
            }
        } else {
            if (BufAppendChar(&c->out, '(') != 0) {
                return -1;
            }
            if (useCoercedExpr) {
                if (EmitExprCoerced(c, exprNode, optionalType) != 0) {
                    return -1;
                }
            } else if (EmitExpr(c, exprNode) != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, " != NULL)") != 0) {
                return -1;
            }
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_opt = ") != 0) {
        return -1;
    }
    if (useCoercedExpr) {
        if (EmitExprCoerced(c, exprNode, optionalType) != 0) {
            return -1;
        }
    } else if (EmitExpr(c, exprNode) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "; __hop_opt.__hop_tag != 0u; }))") != 0) {
        return -1;
    }
    return 0;
}

static int EmitTaggedOptionalNoneLiteral(H2CBackendC* c, const H2TypeRef* optionalType) {
    H2TypeRef storageType;
    if (c == NULL || optionalType == NULL || !TypeRefIsTaggedOptional(optionalType)) {
        return -1;
    }
    if (TypeRefLowerForStorage(c, optionalType, &storageType) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, &storageType) != 0
        || BufAppendCStr(&c->out, "){ .__hop_tag = 0u })") != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitTaggedOptionalSomeLiteral(
    H2CBackendC* c, int32_t exprNode, const H2TypeRef* optionalType) {
    H2TypeRef payloadType;
    H2TypeRef storageType;
    if (c == NULL || optionalType == NULL
        || !TypeRefOptionalPayloadTypeExpr(optionalType, &payloadType)
        || !TypeRefIsTaggedOptional(optionalType))
    {
        return -1;
    }
    if (TypeRefLowerForStorage(c, optionalType, &storageType) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, &storageType) != 0
        || BufAppendCStr(&c->out, "){ .__hop_tag = 1u, .__hop_value = ") != 0)
    {
        return -1;
    }
    if (EmitExprCoerced(c, exprNode, &payloadType) != 0 || BufAppendCStr(&c->out, " })") != 0) {
        return -1;
    }
    return 0;
}

static int EmitTaggedOptionalConvertFromOptional(
    H2CBackendC* c, int32_t exprNode, const H2TypeRef* dstType, const H2TypeRef* srcType) {
    H2TypeRef dstPayload;
    H2TypeRef srcPayload;
    H2TypeRef dstStorage;
    if (c == NULL || dstType == NULL || srcType == NULL || !TypeRefIsTaggedOptional(dstType)
        || !TypeRefIsTaggedOptional(srcType)
        || !TypeRefOptionalPayloadTypeExpr(dstType, &dstPayload)
        || !TypeRefOptionalPayloadTypeExpr(srcType, &srcPayload))
    {
        return -1;
    }
    if (TypeRefLowerForStorage(c, dstType, &dstStorage) != 0) {
        return -1;
    }
    if (TypeRefEqual(&dstPayload, &srcPayload)) {
        return EmitExpr(c, exprNode);
    }
    if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_opt = ") != 0
        || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, "; __auto_type __hop_out = ((") != 0
        || EmitTypeNameWithDepth(c, &dstStorage) != 0
        || BufAppendCStr(&c->out, "){ .__hop_tag = 0u }); if (__hop_opt.__hop_tag != 0u) { ") != 0
        || BufAppendCStr(&c->out, "__hop_out.__hop_tag = 1u; __hop_out.__hop_value = ((") != 0
        || EmitTypeNameWithDepth(c, &dstPayload) != 0
        || BufAppendCStr(&c->out, ")(__hop_opt.__hop_value)); } __hop_out; }))") != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitTaggedOptionalCompareWithValue(
    H2CBackendC*     c,
    int32_t          optionalExprNode,
    const H2TypeRef* optionalType,
    int32_t          valueExprNode,
    int              wantEq) {
    H2TypeRef payloadType;
    if (c == NULL || optionalType == NULL || !TypeRefIsTaggedOptional(optionalType)
        || !TypeRefOptionalPayloadTypeExpr(optionalType, &payloadType))
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_opt = ") != 0
        || EmitExprCoerced(c, optionalExprNode, optionalType) != 0
        || BufAppendCStr(&c->out, "; __hop_opt.__hop_tag ") != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, wantEq ? "!= 0u && " : "== 0u || ") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(__hop_opt.__hop_value ") != 0
        || BufAppendCStr(&c->out, wantEq ? "== " : "!= ") != 0 || BufAppendChar(&c->out, '(') != 0
        || EmitExprCoerced(c, valueExprNode, &payloadType) != 0
        || BufAppendCStr(&c->out, ")); }))") != 0)
    {
        return -1;
    }
    return 0;
}

int TypeRefIsOwnedRuntimeArrayStruct(const H2TypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if ((t->containerKind != H2TypeContainer_SLICE_RO
         && t->containerKind != H2TypeContainer_SLICE_MUT)
        || t->containerPtrDepth == 0)
    {
        return 0;
    }
    return SliceStructPtrDepth(t) == 0;
}

int TypeRefIsNamedDeclKind(const H2CBackendC* c, const H2TypeRef* t, H2AstKind wantKind) {
    const char*           baseName;
    const H2NameMap*      map;
    const H2AnonTypeInfo* anon;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR
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
        if (wantKind == H2Ast_STRUCT) {
            return anon->isUnion == 0;
        }
        if (wantKind == H2Ast_UNION) {
            return anon->isUnion != 0;
        }
    }
    return 0;
}

int TypeRefDerefReadonlyRefLike(const H2TypeRef* in, H2TypeRef* outBase) {
    if (in == NULL || outBase == NULL || !in->valid || !in->readOnly) {
        return -1;
    }
    *outBase = *in;
    if (outBase->containerKind == H2TypeContainer_SCALAR) {
        if (outBase->ptrDepth == 0) {
            return -1;
        }
        outBase->ptrDepth--;
        outBase->readOnly = 0;
        return 0;
    }
    if (outBase->containerKind == H2TypeContainer_ARRAY
        || outBase->containerKind == H2TypeContainer_SLICE_RO
        || outBase->containerKind == H2TypeContainer_SLICE_MUT)
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
    H2CBackendC*     c,
    const H2TypeRef* paramType,
    const H2TypeRef* argType,
    uint8_t*         outCost,
    int*             outAutoRef) {
    uint8_t   baseCost = 0;
    H2TypeRef byValueType;
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
    H2CBackendC*     c,
    const char*      hookName,
    const H2TypeRef* lhsType,
    const H2TypeRef* rhsType,
    const H2FnSig**  outSig,
    const char**     outCalleeName,
    int              outAutoRef[2]) {
    const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
    uint32_t       candidateLen = 0;
    const H2FnSig* bestSig = NULL;
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
        const H2FnSig* sig = candidates[i];
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

int EmitExprAutoRefCoerced(H2CBackendC* c, int32_t argNode, const H2TypeRef* paramType) {
    H2TypeRef byValueType;
    if (TypeRefDerefReadonlyRefLike(paramType, &byValueType) != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(__extension__({ ") != 0
        || EmitTypeNameWithDepth(c, &byValueType) != 0
        || BufAppendCStr(&c->out, " __hop_cmp_arg = ") != 0
        || EmitExprCoerced(c, argNode, &byValueType) != 0 || BufAppendCStr(&c->out, "; ((") != 0
        || EmitTypeNameWithDepth(c, paramType) != 0
        || BufAppendCStr(&c->out, ")(&__hop_cmp_arg)); }))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExprAsSliceRO(H2CBackendC* c, int32_t exprNode, const H2TypeRef* exprType) {
    if (BufAppendCStr(&c->out, "((__hop_slice_ro){ (const void*)(") != 0
        || EmitElemPtrExpr(c, exprNode, exprType, 0) != 0
        || BufAppendCStr(&c->out, "), (__hop_int)(") != 0
        || EmitLenExprFromType(c, exprNode, exprType) != 0 || BufAppendCStr(&c->out, ") })") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitComparisonHookCall(
    H2CBackendC*   c,
    const H2FnSig* sig,
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

int EmitPointerIdentityExpr(H2CBackendC* c, int32_t exprNode, const H2TypeRef* exprType) {
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

int EmitNewAllocArgExpr(H2CBackendC* c, int32_t allocArg) {
    if (allocArg >= 0) {
        H2TypeRef allocType;
        if (InferExprType(c, allocArg, &allocType) == 0 && allocType.valid
            && allocType.ptrDepth == 0 && allocType.containerPtrDepth == 0)
        {
            if (BufAppendCStr(&c->out, "(&(") != 0 || EmitExpr(c, allocArg) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
        return EmitExpr(c, allocArg);
    }
    return BufAppendCStr(&c->out, "(&(context->allocator))");
}

const char* _Nullable ResolveVarSizeValueBaseName(H2CBackendC* c, const H2TypeRef* valueType) {
    const char* baseName;
    if (valueType == NULL || !valueType->valid || valueType->containerKind != H2TypeContainer_SCALAR
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

static int EmitElemPtrExprFromNameType(
    H2CBackendC* c, const char* name, const H2TypeRef* baseType, int wantWritableElem) {
    int elemConst = !wantWritableElem;
    if (baseType == NULL || !baseType->valid || name == NULL) {
        return -1;
    }
    if (TypeRefIsStr(baseType)) {
        const char* ptrType = elemConst ? "const __hop_u8*" : "__hop_u8*";
        if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, ptrType) != 0
            || BufAppendCStr(&c->out, ")(") != 0)
        {
            return -1;
        }
        if (TypeRefIsMutableStrPointer(baseType)) {
            if (BufAppendCStr(&c->out, "((") != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, name) != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, ") == 0 ? (const void*)0 : (const void*)__hop_cstr(*(") != 0)
            {
                return -1;
            }
            if (BufAppendCStr(&c->out, name) != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, ")))") != 0) {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "(const void*)__hop_cstr(") != 0
                || BufAppendCStr(&c->out, name) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
        }
        return BufAppendCStr(&c->out, "))");
    }
    if (baseType->containerKind != H2TypeContainer_ARRAY
        && baseType->containerKind != H2TypeContainer_SLICE_RO
        && baseType->containerKind != H2TypeContainer_SLICE_MUT)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitElementTypeName(c, baseType, elemConst) != 0
        || BufAppendCStr(&c->out, "*)(") != 0)
    {
        return -1;
    }
    if (baseType->containerKind == H2TypeContainer_ARRAY) {
        if (BufAppendCStr(&c->out, name) != 0) {
            return -1;
        }
    } else if (baseType->containerPtrDepth > 0) {
        int stars = SliceStructPtrDepth(baseType);
        if (stars > 0) {
            if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, ") == 0 ? (const void*)0 : (const void*)((") != 0
                || BufAppendCStr(&c->out, name) != 0 || BufAppendCStr(&c->out, ")->ptr))") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
                || BufAppendCStr(&c->out, ").ptr") != 0)
            {
                return -1;
            }
        }
    } else {
        if (BufAppendChar(&c->out, '(') != 0 || BufAppendCStr(&c->out, name) != 0
            || BufAppendCStr(&c->out, ").ptr") != 0)
        {
            return -1;
        }
    }
    return BufAppendCStr(&c->out, "))");
}

int EmitCopyCallExpr(H2CBackendC* c, int32_t calleeNode) {
    int32_t   dstNode = AstNextSibling(&c->ast, calleeNode);
    int32_t   srcNode = dstNode >= 0 ? AstNextSibling(&c->ast, dstNode) : -1;
    int32_t   extra = srcNode >= 0 ? AstNextSibling(&c->ast, srcNode) : -1;
    int32_t   dstExpr = UnwrapCallArgExprNode(c, dstNode);
    int32_t   srcExpr = UnwrapCallArgExprNode(c, srcNode);
    H2TypeRef dstType;
    H2TypeRef srcType;
    uint32_t  tempId;
    H2Buf     dstNameBuf = { 0 };
    H2Buf     srcNameBuf = { 0 };
    char*     dstName;
    char*     srcName;

    if (dstNode < 0 || srcNode < 0 || extra >= 0 || dstExpr < 0 || srcExpr < 0) {
        return -1;
    }
    if (InferExprType(c, dstExpr, &dstType) != 0 || !dstType.valid
        || InferExprType(c, srcExpr, &srcType) != 0 || !srcType.valid)
    {
        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
            SetDiagNode(c, dstExpr >= 0 ? dstExpr : calleeNode, H2Diag_CODEGEN_INTERNAL);
        }
        return -1;
    }
    tempId = FmtNextTempId(c);
    dstNameBuf.arena = &c->arena;
    srcNameBuf.arena = &c->arena;
    if (BufAppendCStr(&dstNameBuf, "__hop_copy_dst") != 0 || BufAppendU32(&dstNameBuf, tempId) != 0
        || BufAppendCStr(&srcNameBuf, "__hop_copy_src") != 0
        || BufAppendU32(&srcNameBuf, tempId) != 0)
    {
        return -1;
    }
    dstName = BufFinish(&dstNameBuf);
    srcName = BufFinish(&srcNameBuf);
    if (dstName == NULL || srcName == NULL) {
        return -1;
    }

    if (BufAppendCStr(&c->out, "(__extension__({ __auto_type ") != 0
        || BufAppendCStr(&c->out, dstName) != 0 || BufAppendCStr(&c->out, " = ") != 0)
    {
        return -1;
    }
    if (EmitExpr(c, dstNode) != 0) {
        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
            SetDiagNode(c, dstExpr, H2Diag_CODEGEN_INTERNAL);
        }
        return -1;
    }
    if (BufAppendCStr(&c->out, "; __auto_type ") != 0 || BufAppendCStr(&c->out, srcName) != 0
        || BufAppendCStr(&c->out, " = ") != 0)
    {
        return -1;
    }
    if (EmitExpr(c, srcNode) != 0) {
        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
            SetDiagNode(c, srcExpr, H2Diag_CODEGEN_INTERNAL);
        }
        return -1;
    }
    if (BufAppendCStr(&c->out, "; __hop_copy((void*)(") != 0
        || EmitElemPtrExprFromNameType(c, dstName, &dstType, 1) != 0
        || BufAppendCStr(&c->out, "), (") != 0 || EmitLenExprFromNameType(c, dstName, &dstType) != 0
        || BufAppendCStr(&c->out, "), (const void*)(") != 0
        || EmitElemPtrExprFromNameType(c, srcName, &srcType, 0) != 0
        || BufAppendCStr(&c->out, "), (") != 0 || EmitLenExprFromNameType(c, srcName, &srcType) != 0
        || BufAppendCStr(&c->out, "), ") != 0)
    {
        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
            SetDiagNode(c, dstExpr, H2Diag_CODEGEN_INTERNAL);
        }
        return -1;
    }
    if (TypeRefIsStr(&dstType)) {
        if (BufAppendCStr(&c->out, "(__hop_int)sizeof(__hop_u8)") != 0) {
            return -1;
        }
    } else {
        if (BufAppendCStr(&c->out, "(__hop_int)sizeof(") != 0
            || EmitElementTypeName(c, &dstType, 0) != 0 || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, "); }))") != 0) {
        return -1;
    }
    return 0;
}

int EmitConcatCallExpr(H2CBackendC* c, int32_t calleeNode) {
    int32_t   aNode = AstNextSibling(&c->ast, calleeNode);
    int32_t   bNode = aNode >= 0 ? AstNextSibling(&c->ast, aNode) : -1;
    int32_t   extra = bNode >= 0 ? AstNextSibling(&c->ast, bNode) : -1;
    uint32_t  tempId;
    H2TypeRef aType;
    H2TypeRef bType;
    if (aNode < 0 || bNode < 0 || extra >= 0) {
        return -1;
    }
    if (InferExprType(c, aNode, &aType) != 0 || !aType.valid || InferExprType(c, bNode, &bType) != 0
        || !bType.valid)
    {
        return -1;
    }
    c->fmtTempCounter++;
    if (c->fmtTempCounter == 0) {
        c->fmtTempCounter = 1;
    }
    tempId = c->fmtTempCounter;
    if (BufAppendCStr(&c->out, "({ __hop_str* hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "; __hop_str hop_concat_a_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = ") != 0
        || EmitStrValueExpr(c, aNode, &aType) != 0
        || BufAppendCStr(&c->out, "; __hop_str hop_concat_b_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = ") != 0
        || EmitStrValueExpr(c, bNode, &bType) != 0
        || BufAppendCStr(&c->out, "; __hop_int hop_concat_lenA_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = __hop_len(") != 0
        || BufAppendCStr(&c->out, "hop_concat_a_") != 0 || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "); __hop_int hop_concat_lenB_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = __hop_len(") != 0
        || BufAppendCStr(&c->out, "hop_concat_b_") != 0 || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "); __hop_int hop_concat_outLen_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = hop_concat_lenA_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " + hop_concat_lenB_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, "; hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, " = (__hop_str*)__hop_new((__hop_MemAllocator*)(") != 0
        || EmitNewAllocArgExpr(c, -1) != 0
        || BufAppendCStr(&c->out, "), (__hop_int)sizeof(__hop_str) + hop_concat_outLen_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, " + 1, _Alignof(__hop_str)); if (hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, " != NULL) { hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "->ptr = (__hop_u8*)(void*)(hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, " + 1); hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "->len = hop_concat_outLen_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "; if (hop_concat_lenA_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, " > 0) { __hop_memcpy(") != 0
        || BufAppendCStr(&c->out, "hop_concat_out_") != 0 || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "->ptr, __hop_cstr(hop_concat_a_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "), (__hop_uint)hop_concat_lenA_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "); } if (hop_concat_lenB_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, " > 0) { __hop_memcpy(") != 0
        || BufAppendCStr(&c->out, "hop_concat_out_") != 0 || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "->ptr + hop_concat_lenA_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, ", __hop_cstr(hop_concat_b_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "), (__hop_uint)hop_concat_lenB_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, "); } hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "->ptr[hop_concat_outLen_") != 0
        || BufAppendU32(&c->out, tempId) != 0
        || BufAppendCStr(&c->out, "] = 0; } hop_concat_out_") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, "; })") != 0)
    {
        return -1;
    }
    return 0;
}

uint32_t FmtNextTempId(H2CBackendC* c) {
    c->fmtTempCounter++;
    if (c->fmtTempCounter == 0) {
        c->fmtTempCounter = 1;
    }
    return c->fmtTempCounter;
}

int TypeRefIsSignedIntegerLike(const H2CBackendC* c, const H2TypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    return StrEq(base, "__hop_i8") || StrEq(base, "__hop_i16") || StrEq(base, "__hop_i32")
        || StrEq(base, "__hop_i64") || StrEq(base, "__hop_int");
}

int TypeRefIsIntegerLike(const H2CBackendC* c, const H2TypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR || t->ptrDepth != 0
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

int TypeRefIsFloatLike(const H2CBackendC* c, const H2TypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR || t->ptrDepth != 0
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

int TypeRefIsBoolLike(const H2CBackendC* c, const H2TypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    return StrEq(base, "__hop_bool");
}

int TypeRefIsNamedEnumLike(const H2CBackendC* c, const H2TypeRef* t) {
    return TypeRefIsNamedDeclKind(c, t, H2Ast_ENUM);
}

int TypeRefIsFmtStringLike(const H2CBackendC* c, const H2TypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR || t->baseName == NULL
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

int TypeRefIsFmtValueType(const H2CBackendC* c, const H2TypeRef* t) {
    const char* base;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->baseName == NULL || t->isOptional)
    {
        return 0;
    }
    base = ResolveScalarAliasBaseName(c, t->baseName);
    if (base == NULL) {
        base = t->baseName;
    }
    return StrHasSuffix(base, "__FmtValue");
}

int EmitFmtAppendLiteralBytes(
    H2CBackendC* c, const char* builderName, const uint8_t* bytes, uint32_t len) {
    uint32_t i;
    if (bytes == NULL || len == 0) {
        return 0;
    }
    if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_bytes(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ") != 0
        || BufAppendCStr(&c->out, "(const __hop_u8*)\"") != 0)
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

int EvalConstStringExpr(
    H2CBackendC*      c,
    int32_t           nodeId,
    const uint8_t**   outBytes,
    uint32_t*         outLen,
    const H2AstNode** outNode) {
    const H2AstNode* n;
    H2CTFEValue      value = { 0 };
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
    while (n->kind == H2Ast_CALL_ARG) {
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
    if (H2ConstEvalSessionEvalExpr(c->constEval, nodeId, &value, &isConst) != 0) {
        return -1;
    }
    if (!isConst || value.kind != H2CTFEValue_STRING) {
        return -1;
    }
    *outBytes = value.s.bytes;
    *outLen = value.s.len;
    *outNode = n;
    return 0;
}

int EmitFmtAppendLiteralText(
    H2CBackendC* c, const char* builderName, const char* text, uint32_t len) {
    return EmitFmtAppendLiteralBytes(c, builderName, (const uint8_t*)text, len);
}

int EmitFmtBuilderInitStmt(H2CBackendC* c, const char* builderName, int32_t allocArgNode) {
    if (BufAppendCStr(&c->out, "    __hop_fmt_builder ") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ";\n    __hop_fmt_builder_init(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", (__hop_MemAllocator*)(") != 0
        || EmitNewAllocArgExpr(c, allocArgNode) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
    {
        return -1;
    }
    return 0;
}

char* _Nullable FmtMakeExprField(H2CBackendC* c, const char* baseExpr, const char* fieldName) {
    H2Buf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, "(") != 0 || BufAppendCStr(&b, baseExpr) != 0
        || BufAppendCStr(&b, ").") != 0 || BufAppendCStr(&b, fieldName) != 0)
    {
        return NULL;
    }
    return BufFinish(&b);
}

char* _Nullable FmtMakeExprIndex(H2CBackendC* c, const char* baseExpr, const char* indexExpr) {
    H2Buf b = { 0 };
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
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth);

int EmitFmtAppendReflectArray(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth) {
    uint32_t i;
    if (type == NULL || !type->valid || type->containerKind != H2TypeContainer_ARRAY
        || type->containerPtrDepth != 0 || !type->hasArrayLen)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", '[');\n") != 0)
    {
        return -1;
    }
    for (i = 0; i < type->arrayLen; i++) {
        H2TypeRef elemType = *type;
        char      idxBuf[24];
        char*     idxExpr;
        if (i > 0 && EmitFmtAppendLiteralText(c, builderName, ", ", 2u) != 0) {
            return -1;
        }
        idxBuf[0] = '\0';
        {
            H2Buf b = { 0 };
            b.arena = &c->arena;
            if (BufAppendU32(&b, i) != 0) {
                return -1;
            }
            idxExpr = BufFinish(&b);
        }
        if (idxExpr == NULL) {
            return -1;
        }
        elemType.containerKind = H2TypeContainer_SCALAR;
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
    if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ']');\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFmtAppendReflectSlice(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth) {
    H2TypeRef elemType;
    uint32_t  loopId;
    H2Buf     idxNameBuf = { 0 };
    H2Buf     elemExprBuf = { 0 };
    char*     idxName;
    char*     elemExpr;
    if (type == NULL || !type->valid
        || (type->containerKind != H2TypeContainer_SLICE_RO
            && type->containerKind != H2TypeContainer_SLICE_MUT)
        || type->containerPtrDepth != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", '[');\n") != 0)
    {
        return -1;
    }
    loopId = FmtNextTempId(c);
    idxNameBuf.arena = &c->arena;
    elemExprBuf.arena = &c->arena;
    if (BufAppendCStr(&idxNameBuf, "__hop_fmt_i") != 0 || BufAppendU32(&idxNameBuf, loopId) != 0) {
        return -1;
    }
    idxName = BufFinish(&idxNameBuf);
    if (idxName == NULL) {
        return -1;
    }
    elemType = *type;
    elemType.containerKind = H2TypeContainer_SCALAR;
    elemType.containerPtrDepth = 0;
    elemType.hasArrayLen = 0;
    elemType.arrayLen = 0;
    if (BufAppendCStr(&c->out, "    for (__hop_int ") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, " = 0; ") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, " < (__hop_int)((") != 0 || BufAppendCStr(&c->out, expr) != 0
        || BufAppendCStr(&c->out, ").len); ") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, "++) {\n") != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "        if (") != 0 || BufAppendCStr(&c->out, idxName) != 0
        || BufAppendCStr(&c->out, " > 0) { __hop_fmt_builder_append_str(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", __hop_strlit(\", \")); }\n") != 0)
    {
        return -1;
    }
    {
        H2Buf typeNameBuf = { 0 };
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
        || BufAppendCStr(&c->out, "    __hop_fmt_builder_append_char(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ']');\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFmtAppendReflectStruct(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth) {
    const char* baseName;
    uint32_t    i;
    uint32_t    fieldCount = 0;
    if (type == NULL || !type->valid || type->containerKind != H2TypeContainer_SCALAR
        || type->containerPtrDepth != 0 || type->ptrDepth != 0 || type->baseName == NULL)
    {
        return -1;
    }
    baseName = ResolveScalarAliasBaseName(c, type->baseName);
    if (baseName == NULL) {
        baseName = type->baseName;
    }
    if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_str(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", __hop_strlit(\"{ \"));\n") != 0)
    {
        return -1;
    }
    for (i = 0; i < c->fieldInfoLen; i++) {
        const H2FieldInfo* f = &c->fieldInfos[i];
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
    if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_str(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0
        || BufAppendCStr(&c->out, ", __hop_strlit(\" }\"));\n") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFmtAppendReflectExpr(
    H2CBackendC*     c,
    const char*      builderName,
    const char*      expr,
    const H2TypeRef* type,
    uint32_t         depth) {
    if (depth > 8u) {
        return EmitFmtAppendLiteralText(c, builderName, "...", 3u);
    }
    if (type == NULL || !type->valid) {
        return EmitFmtAppendLiteralText(c, builderName, "<invalid>", 9u);
    }
    if (TypeRefIsFmtStringLike(c, type)) {
        if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_escaped_str(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ") != 0)
        {
            return -1;
        }
        if (TypeRefIsMutableStrPointer(type)) {
            if (BufAppendCStr(&c->out, "*(") != 0 || BufAppendCStr(&c->out, expr) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
        } else if (BufAppendCStr(&c->out, expr) != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, ");\n") != 0) {
            return -1;
        }
        return 0;
    }
    if (TypeRefIsBoolLike(c, type)) {
        if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_str(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", (") != 0
            || BufAppendCStr(&c->out, expr) != 0
            || BufAppendCStr(&c->out, ") ? __hop_strlit(\"true\") : __hop_strlit(\"false\"));\n")
                   != 0)
        {
            return -1;
        }
        return 0;
    }
    if (TypeRefIsIntegerLike(c, type) || TypeRefIsNamedEnumLike(c, type)) {
        if (TypeRefIsSignedIntegerLike(c, type)) {
            if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_i64(&") != 0
                || BufAppendCStr(&c->out, builderName) != 0
                || BufAppendCStr(&c->out, ", (__hop_i64)(") != 0
                || BufAppendCStr(&c->out, expr) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_u64(&") != 0
                || BufAppendCStr(&c->out, builderName) != 0
                || BufAppendCStr(&c->out, ", (__hop_u64)(") != 0
                || BufAppendCStr(&c->out, expr) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
            {
                return -1;
            }
        }
        return 0;
    }
    if (TypeRefIsFloatLike(c, type)) {
        if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_f64g(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0
            || BufAppendCStr(&c->out, ", (__hop_f64)(") != 0 || BufAppendCStr(&c->out, expr) != 0
            || BufAppendCStr(&c->out, "));\n") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (type->containerKind == H2TypeContainer_ARRAY && type->containerPtrDepth == 0) {
        return EmitFmtAppendReflectArray(c, builderName, expr, type, depth);
    }
    if ((type->containerKind == H2TypeContainer_SLICE_RO
         || type->containerKind == H2TypeContainer_SLICE_MUT)
        && type->containerPtrDepth == 0)
    {
        return EmitFmtAppendReflectSlice(c, builderName, expr, type, depth);
    }
    if (TypeRefIsNamedDeclKind(c, type, H2Ast_STRUCT) || TypeRefTupleInfo(c, type, NULL)) {
        return EmitFmtAppendReflectStruct(c, builderName, expr, type, depth);
    }
    if (type->isOptional) {
        H2TypeRef payloadType;
        if (!TypeRefOptionalPayloadTypeExpr(type, &payloadType)) {
            return -1;
        }
        if (!TypeRefOptionalPayloadUsesNullSentinelExpr(&payloadType)) {
            if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_str(&") != 0
                || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ((") != 0
                || BufAppendCStr(&c->out, expr) != 0
                || BufAppendCStr(
                       &c->out,
                       ").__hop_tag == 0u) ? __hop_strlit(\"null\") : "
                       "__hop_strlit(\"<some>\"));\n")
                       != 0)
            {
                return -1;
            }
            return 0;
        }
        if ((payloadType.containerKind == H2TypeContainer_SLICE_RO
             || payloadType.containerKind == H2TypeContainer_SLICE_MUT)
            && SliceStructPtrDepth(&payloadType) == 0)
        {
            if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_str(&") != 0
                || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ((") != 0
                || BufAppendCStr(&c->out, expr) != 0
                || BufAppendCStr(
                       &c->out,
                       ").ptr == NULL) ? __hop_strlit(\"null\") : __hop_strlit(\"<ptr>\"));\n")
                       != 0)
            {
                return -1;
            }
            return 0;
        }
        if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_str(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ((") != 0
            || BufAppendCStr(&c->out, expr) != 0
            || BufAppendCStr(
                   &c->out, ") == 0) ? __hop_strlit(\"null\") : __hop_strlit(\"<ptr>\"));\n")
                   != 0)
        {
            return -1;
        }
        return 0;
    }
    if (TypeRefIsPointerLike(type)) {
        if (BufAppendCStr(&c->out, "    __hop_fmt_builder_append_str(&") != 0
            || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ", ((") != 0
            || BufAppendCStr(&c->out, expr) != 0
            || BufAppendCStr(
                   &c->out, ") == 0) ? __hop_strlit(\"null\") : __hop_strlit(\"<ptr>\"));\n")
                   != 0)
        {
            return -1;
        }
        return 0;
    }
    return EmitFmtAppendLiteralText(c, builderName, "<unsupported>", 13u);
}

int EmitExprCoerceFmtValue(
    H2CBackendC* c, int32_t exprNode, const H2TypeRef* srcType, const H2TypeRef* dstType) {
    uint32_t    tempId;
    H2Buf       valueNameBuf = { 0 };
    H2Buf       builderNameBuf = { 0 };
    H2Buf       reprNameBuf = { 0 };
    char*       valueName;
    char*       builderName;
    char*       reprName;
    const char* kindExpr = "((__hop_u8)3u)";
    if (c == NULL || srcType == NULL || dstType == NULL) {
        return -1;
    }
    tempId = FmtNextTempId(c);
    valueNameBuf.arena = &c->arena;
    builderNameBuf.arena = &c->arena;
    reprNameBuf.arena = &c->arena;
    if (BufAppendCStr(&valueNameBuf, "__hop_refv_v") != 0
        || BufAppendU32(&valueNameBuf, tempId) != 0
        || BufAppendCStr(&builderNameBuf, "__hop_refv_b") != 0
        || BufAppendU32(&builderNameBuf, tempId) != 0
        || BufAppendCStr(&reprNameBuf, "__hop_refv_r") != 0
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
            kindExpr = "((__hop_u8)1u)";
        } else {
            kindExpr = "((__hop_u8)2u)";
        }
    } else if (TypeRefIsFloatLike(c, srcType)) {
        kindExpr = "((__hop_u8)4u)";
    } else if (TypeRefIsFmtStringLike(c, srcType)) {
        kindExpr = "((__hop_u8)5u)";
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
    if (BufAppendCStr(&c->out, "    __hop_str* ") != 0 || BufAppendCStr(&c->out, reprName) != 0
        || BufAppendCStr(&c->out, " = __hop_fmt_builder_finish(&") != 0
        || BufAppendCStr(&c->out, builderName) != 0 || BufAppendCStr(&c->out, ");\n    ((") != 0
        || EmitTypeNameWithDepth(c, dstType) != 0 || BufAppendCStr(&c->out, "){ .kind = ") != 0
        || BufAppendCStr(&c->out, kindExpr) != 0 || BufAppendCStr(&c->out, ", .repr = ") != 0
        || BufAppendCStr(&c->out, reprName) != 0 || BufAppendCStr(&c->out, " });\n}))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitFreeCallExpr(H2CBackendC* c, int32_t allocArgNode, int32_t valueNode) {
    H2TypeRef valueType;
    if (InferExprType(c, valueNode, &valueType) != 0 || !valueType.valid) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "__hop_free((__hop_MemAllocator*)(") != 0
        || EmitNewAllocArgExpr(c, allocArgNode) != 0 || BufAppendCStr(&c->out, "), ") != 0)
    {
        return -1;
    }

    if (valueType.containerKind == H2TypeContainer_SCALAR && valueType.containerPtrDepth == 0
        && valueType.ptrDepth > 0)
    {
        H2TypeRef pointeeType = valueType;
        pointeeType.ptrDepth--;
        if (BufAppendCStr(&c->out, "(void*)(") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, "), ") != 0)
        {
            return -1;
        }
        if (pointeeType.ptrDepth == 0 && pointeeType.containerKind == H2TypeContainer_SCALAR
            && IsStrBaseName(pointeeType.baseName))
        {
            if (BufAppendCStr(&c->out, "__hop_packed_str_size((") != 0
                || EmitExpr(c, valueNode) != 0
                || BufAppendCStr(&c->out, ")), _Alignof(__hop_str))") != 0)
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

    if ((valueType.containerKind == H2TypeContainer_SLICE_RO
         || valueType.containerKind == H2TypeContainer_SLICE_MUT)
        && valueType.containerPtrDepth > 0 && SliceStructPtrDepth(&valueType) == 0)
    {
        if (BufAppendCStr(&c->out, "(void*)((") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, ").ptr), ((__hop_int)((") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, ").len) * (__hop_int)sizeof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0
            || BufAppendCStr(&c->out, ")), _Alignof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }

    if (valueType.containerKind == H2TypeContainer_ARRAY && valueType.containerPtrDepth > 0
        && valueType.hasArrayLen)
    {
        if (BufAppendCStr(&c->out, "(void*)(") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, "), ((__hop_int)") != 0
            || BufAppendU32(&c->out, valueType.arrayLen) != 0
            || BufAppendCStr(&c->out, " * (__hop_int)sizeof(") != 0
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
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable dstType, int requireNonNull) {
    int32_t          typeNode = -1;
    int32_t          countArg = -1;
    int32_t          initArg = -1;
    int32_t          allocArg = -1;
    H2TypeRef        elemType;
    const char*      varSizeBaseName = NULL;
    const char*      ownerTypeName = NULL;
    const H2NameMap* ownerMap = NULL;
    int              needsImplicitInit = 0;
    int              dstIsRuntimeArray = 0;
    int              dstIsRuntimeArrayMut = 0;
    int              isVarSizeStr = 0;

    if (NodeAt(c, nodeId) != NULL && (NodeAt(c, nodeId)->flags & H2AstFlag_NEW_HAS_ARRAY_LIT) != 0)
    {
        int32_t          litNode = AstFirstChild(&c->ast, nodeId);
        int32_t          allocNode = litNode >= 0 ? AstNextSibling(&c->ast, litNode) : -1;
        const H2AstNode* lit = NodeAt(c, litNode);
        H2TypeRef        expectedValue;
        H2TypeRef        arrayType;
        H2TypeRef        elemType;
        uint32_t         elemCount;
        int              useRuntimeSlice = 0;
        uint32_t         i = 0;
        int32_t          child;
        TypeRefSetInvalid(&expectedValue);
        if (lit == NULL || lit->kind != H2Ast_ARRAY_LIT) {
            return -1;
        }
        if (dstType != NULL && dstType->valid) {
            expectedValue = *dstType;
            if (expectedValue.containerKind != H2TypeContainer_SCALAR) {
                if (expectedValue.containerPtrDepth > 0) {
                    expectedValue.containerPtrDepth--;
                }
            } else if (expectedValue.ptrDepth > 0) {
                expectedValue.ptrDepth--;
            }
        }
        if (InferArrayLiteralType(
                c, litNode, expectedValue.valid ? &expectedValue : NULL, &arrayType)
                != 0
            || !arrayType.valid || arrayType.containerKind != H2TypeContainer_ARRAY)
        {
            return -1;
        }
        elemType = arrayType;
        elemType.containerKind = H2TypeContainer_SCALAR;
        elemType.containerPtrDepth = 0;
        elemType.arrayLen = 0;
        elemType.hasArrayLen = 0;
        elemCount = ArrayLitElementCount(c, litNode);
        if (dstType != NULL && dstType->valid
            && (dstType->containerKind == H2TypeContainer_SLICE_RO
                || dstType->containerKind == H2TypeContainer_SLICE_MUT)
            && dstType->containerPtrDepth > 0)
        {
            useRuntimeSlice = 1;
        }
        if (BufAppendCStr(&c->out, "(__extension__({\n    ") != 0
            || EmitElementTypeName(c, &elemType, 0) != 0
            || BufAppendCStr(&c->out, "* __hop_p = (") != 0
            || EmitElementTypeName(c, &elemType, 0) != 0
            || BufAppendCStr(&c->out, "*)__hop_new_array((__hop_MemAllocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocNode) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitElementTypeName(c, &elemType, 0) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitElementTypeName(c, &elemType, 0) != 0
            || BufAppendCStr(&c->out, "), (__hop_int)") != 0
            || BufAppendU32(&c->out, useRuntimeSlice ? elemCount : arrayType.arrayLen) != 0
            || BufAppendCStr(&c->out, ");\n    if (__hop_p != NULL) {\n") != 0)
        {
            return -1;
        }
        child = AstFirstChild(&c->ast, litNode);
        while (child >= 0) {
            if (BufAppendCStr(&c->out, "        __hop_p[") != 0 || BufAppendU32(&c->out, i) != 0
                || BufAppendCStr(&c->out, "] = ") != 0 || EmitExprCoerced(c, child, &elemType) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
            i++;
            child = AstNextSibling(&c->ast, child);
        }
        if (useRuntimeSlice) {
            if (BufAppendCStr(&c->out, "    }\n    ((") != 0
                || EmitTypeNameWithDepth(c, &expectedValue) != 0
                || BufAppendCStr(&c->out, "){ ") != 0
                || BufAppendCStr(
                       &c->out,
                       dstType->containerKind == H2TypeContainer_SLICE_MUT
                           ? "(void*)__hop_p"
                           : "(const void*)__hop_p")
                       != 0
                || BufAppendCStr(&c->out, ", (__hop_int)") != 0
                || BufAppendU32(&c->out, elemCount) != 0
                || BufAppendCStr(&c->out, " });\n}))") != 0)
            {
                return -1;
            }
            (void)requireNonNull;
            return 0;
        }
        if (BufAppendCStr(&c->out, "    }\n    (") != 0 || EmitElementTypeName(c, &elemType, 0) != 0
            || BufAppendCStr(&c->out, " (*)[") != 0
            || BufAppendU32(&c->out, arrayType.arrayLen) != 0 || BufAppendCStr(&c->out, "])") != 0)
        {
            return -1;
        }
        if (requireNonNull) {
            if (BufAppendCStr(&c->out, "__hop_unwrap((const void*)__hop_p)") != 0) {
                return -1;
            }
        } else if (BufAppendCStr(&c->out, "__hop_p") != 0) {
            return -1;
        }
        return BufAppendCStr(&c->out, ";\n}))");
    }

    if (DecodeNewExprNodes(c, nodeId, &typeNode, &countArg, &initArg, &allocArg) != 0) {
        return -1;
    }
    if (ParseTypeRef(c, typeNode, &elemType) != 0 || !elemType.valid) {
        return -1;
    }
    if (dstType != NULL
        && (dstType->containerKind == H2TypeContainer_SLICE_RO
            || dstType->containerKind == H2TypeContainer_SLICE_MUT)
        && dstType->containerPtrDepth > 0)
    {
        dstIsRuntimeArray = 1;
        dstIsRuntimeArrayMut = dstType->containerKind == H2TypeContainer_SLICE_MUT;
    }
    if (countArg < 0) {
        varSizeBaseName = ResolveVarSizeValueBaseName(c, &elemType);
        isVarSizeStr = varSizeBaseName != NULL && IsStrBaseName(varSizeBaseName);
    }
    if (countArg < 0 && initArg < 0 && varSizeBaseName == NULL
        && elemType.containerKind == H2TypeContainer_SCALAR && elemType.containerPtrDepth == 0
        && elemType.ptrDepth == 0 && elemType.baseName != NULL && !elemType.isOptional)
    {
        ownerTypeName = ResolveScalarAliasBaseName(c, elemType.baseName);
        if (ownerTypeName != NULL) {
            ownerMap = FindNameByCName(c, ownerTypeName);
            if (ownerMap != NULL && ownerMap->kind == H2Ast_STRUCT
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
                        ? "((__hop_slice_mut){ (void*)__hop_unwrap((const void*)("
                        : "((__hop_slice_ro){ (const void*)__hop_unwrap((const "
                          "void*)(")
                != 0)
            {
                return -1;
            }
            if (BufAppendCStr(&c->out, "__hop_new_array((__hop_MemAllocator*)(") != 0
                || EmitNewAllocArgExpr(c, allocArg) != 0
                || BufAppendCStr(&c->out, "), sizeof(") != 0
                || EmitTypeNameWithDepth(c, &elemType) != 0
                || BufAppendCStr(&c->out, "), _Alignof(") != 0
                || EmitTypeNameWithDepth(c, &elemType) != 0
                || BufAppendCStr(&c->out, "), (__hop_int)(") != 0 || EmitExpr(c, countArg) != 0
                || BufAppendCStr(&c->out, ")))), (__hop_int)(") != 0 || EmitExpr(c, countArg) != 0
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
                dstIsRuntimeArrayMut ? "__hop_new_array_slice_mut((__hop_MemAllocator*)("
                                     : "__hop_new_array_slice_ro((__hop_MemAllocator*)(")
                != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), (__hop_int)(") != 0 || EmitExpr(c, countArg) != 0
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
        if (requireNonNull && BufAppendCStr(&c->out, "__hop_unwrap((const void*)(") != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "__hop_new_array((__hop_MemAllocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), (__hop_int)(") != 0 || EmitExpr(c, countArg) != 0
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
            if (BufAppendCStr(&c->out, "__hop_unwrap((const void*)(") != 0) {
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, "__hop_new((__hop_MemAllocator*)(") != 0
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

    if (BufAppendCStr(&c->out, "((") != 0
        || (isVarSizeStr ? BufAppendCStr(&c->out, varSizeBaseName)
                         : EmitTypeNameWithDepth(c, &elemType))
        || BufAppendCStr(&c->out, "*)") != 0)
    {
        return -1;
    }
    if (requireNonNull && BufAppendCStr(&c->out, "__hop_unwrap((const void*)(") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(__extension__({\n    ") != 0
        || (isVarSizeStr ? BufAppendCStr(&c->out, varSizeBaseName)
                         : EmitTypeNameWithDepth(c, &elemType))
        || BufAppendCStr(&c->out, "* __hop_p;\n") != 0)
    {
        return -1;
    }

    if (varSizeBaseName != NULL) {
        const H2AstNode* initNode;
        const char*      initOwnerType;
        int32_t          initOwnerNodeId = -1;
        int32_t          fieldNode;
        if (initArg < 0) {
            return -1;
        }
        initNode = NodeAt(c, initArg);
        initOwnerType = ResolveScalarAliasBaseName(c, elemType.baseName);
        if (initOwnerType == NULL || initNode == NULL || initNode->kind != H2Ast_COMPOUND_LIT) {
            return -1;
        }
        {
            uint32_t i;
            for (i = 0; i < c->topDeclLen; i++) {
                int32_t          topNodeId = c->topDecls[i].nodeId;
                const H2AstNode* topNode = NodeAt(c, topNodeId);
                const H2NameMap* topMap;
                if (topNode == NULL || topNode->kind != H2Ast_STRUCT) {
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
        if (BufAppendCStr(&c->out, "    ") != 0
            || (isVarSizeStr ? BufAppendCStr(&c->out, varSizeBaseName)
                             : EmitTypeNameWithDepth(c, &elemType))
            || BufAppendCStr(&c->out, " __hop_init = {0};\n") != 0)
        {
            return -1;
        }
        fieldNode = AstFirstChild(&c->ast, initArg);
        if (fieldNode >= 0 && IsTypeNodeKind(NodeAt(c, fieldNode)->kind)) {
            fieldNode = AstNextSibling(&c->ast, fieldNode);
        }
        while (fieldNode >= 0) {
            const H2AstNode*   field = NodeAt(c, fieldNode);
            const H2FieldInfo* fieldPath[64];
            const H2FieldInfo* resolvedField = NULL;
            uint32_t           fieldPathLen = 0;
            int32_t            exprNode;
            uint32_t           i;
            if (field == NULL || field->kind != H2Ast_COMPOUND_FIELD) {
                return -1;
            }
            exprNode = AstFirstChild(&c->ast, fieldNode);
            if (exprNode < 0 && (field->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
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
            if (BufAppendCStr(&c->out, "    __hop_init") != 0) {
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
        if (BufAppendCStr(&c->out, "    __hop_int __hop_size = ") != 0) {
            return -1;
        }
        if (IsStrBaseName(varSizeBaseName)) {
            if (BufAppendCStr(&c->out, "__hop_packed_str_size((__hop_str*)&__hop_init)") != 0) {
                return -1;
            }
        } else if (
            BufAppendCStr(&c->out, varSizeBaseName) != 0
            || BufAppendCStr(&c->out, "__sizeof(&__hop_init)") != 0)
        {
            return -1;
        }
        if (BufAppendCStr(&c->out, ";\n") != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "    __hop_p = (") != 0
            || (isVarSizeStr ? BufAppendCStr(&c->out, varSizeBaseName)
                             : EmitTypeNameWithDepth(c, &elemType))
            || BufAppendCStr(&c->out, "*)__hop_new((__hop_MemAllocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0
            || BufAppendCStr(&c->out, "), __hop_size, _Alignof(") != 0
            || (isVarSizeStr ? BufAppendCStr(&c->out, varSizeBaseName)
                             : EmitTypeNameWithDepth(c, &elemType))
            || BufAppendCStr(&c->out, "));\n") != 0)
        {
            return -1;
        }
        if (BufAppendCStr(&c->out, "    if (__hop_p != NULL) {\n        *__hop_p = __hop_init;\n")
            != 0)
        {
            return -1;
        }
        if (IsStrBaseName(varSizeBaseName)) {
            if (BufAppendCStr(
                    &c->out,
                    "        if (__hop_p->ptr == (__hop_u8*)0 && __hop_p->len > 0) {\n"
                    "            __hop_p->ptr = (__hop_u8*)(void*)(__hop_p + 1);\n"
                    "            __hop_p->ptr[__hop_p->len] = 0;\n"
                    "        }\n")
                != 0)
            {
                return -1;
            }
        } else if (initOwnerNodeId >= 0) {
            int32_t declField = AstFirstChild(&c->ast, initOwnerNodeId);
            if (BufAppendCStr(&c->out, "        __hop_int __hop_off = (__hop_int)sizeof(") != 0
                || BufAppendCStr(&c->out, varSizeBaseName) != 0
                || BufAppendCStr(&c->out, "__hdr);\n") != 0)
            {
                return -1;
            }
            while (declField >= 0) {
                const H2AstNode* df = NodeAt(c, declField);
                if (df != NULL && df->kind == H2Ast_FIELD) {
                    int32_t          wt = AstFirstChild(&c->ast, declField);
                    const H2AstNode* wtn = NodeAt(c, wt);
                    if (wtn != NULL && wtn->kind == H2Ast_TYPE_VARRAY) {
                        int32_t welem = AstFirstChild(&c->ast, wt);
                        if (BufAppendCStr(
                                &c->out, "        __hop_off = __hop_align_up(__hop_off, _Alignof(")
                                != 0
                            || EmitTypeForCast(c, welem) != 0
                            || BufAppendCStr(&c->out, "));\n        __hop_off += __hop_p->") != 0
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
                        H2TypeRef   wFieldType;
                        const char* wVarSizeBaseName = NULL;
                        if (ParseTypeRef(c, wt, &wFieldType) != 0) {
                            return -1;
                        }
                        wVarSizeBaseName = ResolveVarSizeValueBaseName(c, &wFieldType);
                        if (wVarSizeBaseName != NULL) {
                            if (IsStrBaseName(wVarSizeBaseName)) {
                                if (BufAppendCStr(&c->out, "        if (__hop_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".ptr == (__hop_u8*)0 && __hop_p->")
                                           != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".len > 0) {\n        __hop_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(
                                           &c->out,
                                           ".ptr = (__hop_u8*)((__hop_u8*)__hop_p + __hop_off);\n")
                                           != 0
                                    || BufAppendCStr(&c->out, "        __hop_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".ptr[__hop_p->") != 0
                                    || BufAppendSlice(
                                           &c->out, c->unit->source, df->dataStart, df->dataEnd)
                                           != 0
                                    || BufAppendCStr(&c->out, ".len] = 0;\n        }\n") != 0)
                                {
                                    return -1;
                                }
                            }
                            if (BufAppendCStr(&c->out, "        __hop_off += ") != 0) {
                                return -1;
                            }
                            if (IsStrBaseName(wVarSizeBaseName)) {
                                if (BufAppendCStr(
                                        &c->out, "__hop_packed_str_size((__hop_str*)&__hop_p->")
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
                                || BufAppendCStr(&c->out, "__sizeof(&__hop_p->") != 0
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
        if (BufAppendCStr(&c->out, "    __hop_p = (") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "*)__hop_new((__hop_MemAllocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0
            || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || EmitTypeNameWithDepth(c, &elemType) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
        {
            return -1;
        }
        if (initArg >= 0 || needsImplicitInit) {
            if (BufAppendCStr(&c->out, "    if (__hop_p != NULL) {\n        *__hop_p = ") != 0) {
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

    if (BufAppendCStr(&c->out, "    __hop_p;\n}))") != 0) {
        return -1;
    }
    if (requireNonNull && BufAppendCStr(&c->out, "))") != 0) {
        return -1;
    }
    return BufAppendChar(&c->out, ')');
}

int EmitExprCoerced(H2CBackendC* c, int32_t exprNode, const H2TypeRef* _Nullable dstType) {
    const H2AstNode*   expr = NodeAt(c, exprNode);
    H2TypeRef          srcType;
    const H2FieldInfo* embedPath[64];
    uint32_t           embedPathLen = 0;
    int32_t            idxNode = -1;
    int                isConstIndex = 0;
    uint32_t           constIndex = 0;
    if (dstType == NULL || !dstType->valid) {
        return EmitExpr(c, exprNode);
    }
    if (ResolveActivePackIndexExpr(c, exprNode, &idxNode, &isConstIndex, &constIndex) == 0
        && !isConstIndex)
    {
        return EmitDynamicActivePackIndexCoerced(c, idxNode, dstType);
    }
    if (expr != NULL && expr->kind == H2Ast_NEW) {
        int requireNonNull = TypeRefIsPointerLike(dstType) && !dstType->isOptional;
        return EmitNewExpr(c, exprNode, dstType, requireNonNull);
    }
    if (expr != NULL
        && (expr->kind == H2Ast_STRING
            || (expr->kind == H2Ast_BINARY && (H2TokenKind)expr->op == H2Tok_ADD))
        && dstType->containerKind == H2TypeContainer_SCALAR && dstType->containerPtrDepth == 0
        && dstType->ptrDepth > 0 && IsStrBaseName(dstType->baseName))
    {
        int32_t literalId = -1;
        if ((uint32_t)exprNode < c->stringLitByNodeLen) {
            literalId = c->stringLitByNode[exprNode];
        }
        if (literalId < 0) {
            return -1;
        }
        return dstType->readOnly
                 ? EmitStringLiteralValue(c, literalId, 0)
                 : EmitStringLiteralPointer(c, literalId, 1);
    }
    if (TypeRefIsFmtValueType(c, dstType)) {
        if (InferExprType(c, exprNode, &srcType) == 0 && srcType.valid
            && !TypeRefIsFmtValueType(c, &srcType))
        {
            return EmitExprCoerceFmtValue(c, exprNode, &srcType, dstType);
        }
    }
    if (expr != NULL && expr->kind == H2Ast_COMPOUND_LIT) {
        if (TypeRefIsPointerLike(dstType)) {
            H2TypeRef        targetType = *dstType;
            const H2TypeRef* literalExpected = NULL;
            if (targetType.containerKind != H2TypeContainer_SCALAR) {
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
    if (expr != NULL && expr->kind == H2Ast_ARRAY_LIT) {
        return EmitArrayLiteral(c, exprNode, dstType);
    }
    if (expr != NULL && expr->kind == H2Ast_UNARY && (H2TokenKind)expr->op == H2Tok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, exprNode);
        const H2AstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == H2Ast_COMPOUND_LIT) {
            H2TypeRef        targetType = *dstType;
            const H2TypeRef* literalExpected = NULL;
            if (targetType.containerKind != H2TypeContainer_SCALAR) {
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
    if (dstType->isOptional && expr != NULL && expr->kind == H2Ast_NULL
        && TypeRefIsTaggedOptional(dstType))
    {
        return EmitTaggedOptionalNoneLiteral(c, dstType);
    }
    if (TypeRefIsBorrowedStrValue(dstType) && expr != NULL && expr->kind == H2Ast_NULL) {
        return BufAppendCStr(&c->out, "((__hop_str){ (__hop_u8*)(uintptr_t)0, (__hop_int)0 })");
    }
    if (TypeRefIsBorrowedStrValue(dstType) && expr != NULL
        && (expr->kind == H2Ast_CALL || expr->kind == H2Ast_CALL_WITH_CONTEXT))
    {
        int32_t          callNode = exprNode;
        int32_t          calleeNode;
        const H2AstNode* callee;
        if (expr->kind == H2Ast_CALL_WITH_CONTEXT) {
            callNode = AstFirstChild(&c->ast, exprNode);
        }
        calleeNode = AstFirstChild(&c->ast, callNode);
        callee = NodeAt(c, calleeNode);
        if (callee != NULL && callee->kind == H2Ast_IDENT
            && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "concat"))
        {
            if (BufAppendCStr(&c->out, "(*(") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (expr != NULL && expr->kind == H2Ast_IDENT && TypeRefIsFunctionAlias(c, dstType)) {
        return EmitExpr(c, exprNode);
    }
    if (InferExprType(c, exprNode, &srcType) != 0 || !srcType.valid) {
        if (TypeRefIsBorrowedStrValue(dstType) && expr != NULL && expr->kind == H2Ast_BINARY
            && (H2TokenKind)expr->op == H2Tok_ADD)
        {
            if (BufAppendCStr(&c->out, "(*(") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
        if (TypeRefIsBorrowedStrValue(dstType) && expr != NULL
            && (expr->kind == H2Ast_CALL || expr->kind == H2Ast_CALL_WITH_CONTEXT))
        {
            if (BufAppendCStr(&c->out, "(*(") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
        if (dstType->isOptional && TypeRefIsTaggedOptional(dstType)) {
            return EmitTaggedOptionalSomeLiteral(c, exprNode, dstType);
        }
        return EmitExpr(c, exprNode);
    }
    if (TypeRefIsStrValueLike(dstType) && TypeRefIsStr(&srcType)) {
        return EmitStrValueExpr(c, exprNode, &srcType);
    }
    if (TypeRefIsFmtValueType(c, dstType) && !TypeRefIsFmtValueType(c, &srcType)) {
        return EmitExprCoerceFmtValue(c, exprNode, &srcType, dstType);
    }
    if (dstType->isOptional && TypeRefIsTaggedOptional(dstType)) {
        if (srcType.isOptional) {
            if (TypeRefIsTaggedOptional(&srcType)) {
                return EmitTaggedOptionalConvertFromOptional(c, exprNode, dstType, &srcType);
            }
            return EmitExpr(c, exprNode);
        }
        return EmitTaggedOptionalSomeLiteral(c, exprNode, dstType);
    }
    if (srcType.containerKind == H2TypeContainer_SCALAR
        && dstType->containerKind == H2TypeContainer_SCALAR && srcType.baseName != NULL
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
    if ((dstType->containerKind == H2TypeContainer_SLICE_RO
         || dstType->containerKind == H2TypeContainer_SLICE_MUT)
        && dstType->containerPtrDepth > 0 && SliceStructPtrDepth(dstType) == 0)
    {
        const char* srcBase = ResolveScalarAliasBaseName(c, srcType.baseName);
        if (srcBase == NULL) {
            srcBase = srcType.baseName;
        }
        if (srcType.containerKind == H2TypeContainer_SCALAR && srcType.containerPtrDepth == 0
            && srcType.ptrDepth > 0 && IsStrBaseName(srcBase) && dstType->baseName != NULL
            && StrEq(dstType->baseName, "__hop_u8"))
        {
            if (dstType->containerKind == H2TypeContainer_SLICE_MUT && srcType.readOnly) {
                return EmitExpr(c, exprNode);
            }
            if (TypeRefIsMutableStrPointer(&srcType)) {
                if (dstType->containerKind == H2TypeContainer_SLICE_MUT) {
                    if (BufAppendCStr(&c->out, "(((") != 0 || EmitExpr(c, exprNode) != 0
                        || BufAppendCStr(
                               &c->out,
                               ") == 0) ? ((__hop_slice_mut){ (void*)0, (__hop_int)0 }) : "
                               "((__hop_slice_mut){ (void*)__hop_cstr(*(")
                               != 0
                        || EmitExpr(c, exprNode) != 0
                        || BufAppendCStr(&c->out, ")), (__hop_int)__hop_len(*(") != 0
                        || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, ")) }))") != 0)
                    {
                        return -1;
                    }
                } else {
                    if (BufAppendCStr(&c->out, "(((") != 0 || EmitExpr(c, exprNode) != 0
                        || BufAppendCStr(
                               &c->out,
                               ") == 0) ? ((__hop_slice_ro){ (const void*)0, (__hop_int)0 }) : "
                               "((__hop_slice_ro){ (const void*)__hop_cstr(*(")
                               != 0
                        || EmitExpr(c, exprNode) != 0
                        || BufAppendCStr(&c->out, ")), (__hop_int)__hop_len(*(") != 0
                        || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, ")) }))") != 0)
                    {
                        return -1;
                    }
                }
            } else if (dstType->containerKind == H2TypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__hop_slice_mut){ (void*)__hop_cstr(") != 0
                    || EmitStrValueExpr(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, "), (__hop_int)__hop_len(") != 0
                    || EmitStrValueExpr(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__hop_slice_ro){ (const void*)__hop_cstr(") != 0
                    || EmitStrValueExpr(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, "), (__hop_int)__hop_len(") != 0
                    || EmitStrValueExpr(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
        if (dstType->containerKind == H2TypeContainer_SLICE_RO
            && srcType.containerKind == H2TypeContainer_SLICE_MUT && srcType.containerPtrDepth > 0
            && SliceStructPtrDepth(&srcType) == 0 && srcType.ptrDepth == dstType->ptrDepth
            && srcType.containerPtrDepth == dstType->containerPtrDepth && srcType.baseName != NULL
            && dstType->baseName != NULL && StrEq(srcType.baseName, dstType->baseName))
        {
            if (BufAppendCStr(&c->out, "((__hop_slice_ro){ (const void*)((") != 0
                || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").ptr), (__hop_int)((") != 0
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
        if (srcType.containerKind == H2TypeContainer_ARRAY && srcType.ptrDepth == dstType->ptrDepth
            && (srcType.containerPtrDepth == dstType->containerPtrDepth
                || srcType.containerPtrDepth + 1 == dstType->containerPtrDepth)
            && srcType.baseName != NULL && dstType->baseName != NULL
            && StrEq(srcType.baseName, dstType->baseName))
        {
            if (dstType->containerKind == H2TypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__hop_slice_mut){ (void*)(") != 0
                    || EmitElemPtrExpr(c, exprNode, &srcType, 1) != 0
                    || BufAppendCStr(&c->out, "), (__hop_int)(") != 0
                    || EmitLenExprFromType(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__hop_slice_ro){ (const void*)(") != 0
                    || EmitElemPtrExpr(c, exprNode, &srcType, 0) != 0
                    || BufAppendCStr(&c->out, "), (__hop_int)(") != 0
                    || EmitLenExprFromType(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
    }
    if (dstType->containerKind == H2TypeContainer_SLICE_RO && dstType->containerPtrDepth == 0) {
        if (srcType.containerKind == H2TypeContainer_SLICE_RO && srcType.containerPtrDepth == 0) {
            return EmitExpr(c, exprNode);
        }
        if ((srcType.containerKind == H2TypeContainer_SLICE_MUT && srcType.containerPtrDepth == 0)
            || srcType.containerKind == H2TypeContainer_ARRAY)
        {
            if (BufAppendCStr(&c->out, "((__hop_slice_ro){ (const void*)(") != 0
                || EmitElemPtrExpr(c, exprNode, &srcType, 0) != 0
                || BufAppendCStr(&c->out, "), (__hop_int)(") != 0
                || EmitLenExprFromType(c, exprNode, &srcType) != 0
                || BufAppendCStr(&c->out, ") })") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (dstType->containerKind == H2TypeContainer_SLICE_MUT && dstType->containerPtrDepth == 0) {
        if (srcType.containerKind == H2TypeContainer_SLICE_MUT && srcType.containerPtrDepth == 0) {
            return EmitExpr(c, exprNode);
        }
        if (srcType.containerKind == H2TypeContainer_ARRAY) {
            if (BufAppendCStr(&c->out, "((__hop_slice_mut){ (void*)(") != 0
                || EmitElemPtrExpr(c, exprNode, &srcType, 1) != 0
                || BufAppendCStr(&c->out, "), (__hop_int)(") != 0
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

int32_t ActiveCallOverlayNode(const H2CBackendC* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast.len) {
        return -1;
    }
    {
        int32_t callNode = AstFirstChild(&c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? AstNextSibling(&c->ast, callNode) : -1;
        if (child >= 0) {
            const H2AstNode* n = NodeAt(c, child);
            if (n != NULL && n->kind == H2Ast_CONTEXT_OVERLAY) {
                return child;
            }
        }
    }
    return -1;
}

int32_t ActiveCallDirectContextNode(const H2CBackendC* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast.len) {
        return -1;
    }
    {
        int32_t callNode = AstFirstChild(&c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? AstNextSibling(&c->ast, callNode) : -1;
        if (child >= 0) {
            const H2AstNode* n = NodeAt(c, child);
            if (n != NULL && n->kind != H2Ast_CONTEXT_OVERLAY) {
                return child;
            }
        }
    }
    return -1;
}

int32_t FindActiveOverlayBindByName(const H2CBackendC* c, const char* fieldName) {
    int32_t overlayNode = ActiveCallOverlayNode(c);
    int32_t child = overlayNode >= 0 ? AstFirstChild(&c->ast, overlayNode) : -1;
    while (child >= 0) {
        const H2AstNode* b = NodeAt(c, child);
        if (b != NULL && b->kind == H2Ast_CONTEXT_BIND
            && SliceEqName(c->unit->source, b->dataStart, b->dataEnd, fieldName))
        {
            return child;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return -1;
}

int EmitCurrentContextFieldRaw(H2CBackendC* c, const char* fieldName) {
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
    H2CBackendC* c, const char* fieldName, const H2TypeRef* requiredType) {
    if (c->hasCurrentContext) {
        const H2FieldInfo* srcField = NULL;
        if (c->currentContextType.valid && c->currentContextType.baseName != NULL) {
            srcField = FindFieldInfoByName(c, c->currentContextType.baseName, fieldName);
        }
        if (requiredType != NULL && requiredType->valid && srcField != NULL && srcField->type.valid
            && !TypeRefEqual(&srcField->type, requiredType))
        {
            const H2FieldInfo* embedPath[64];
            uint32_t           embedPathLen = 0;
            uint8_t            cost = 0;
            if (srcField->type.containerKind == H2TypeContainer_SCALAR
                && requiredType->containerKind == H2TypeContainer_SCALAR
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

            if (StrEq(fieldName, "logger") && srcField->type.containerKind == H2TypeContainer_SCALAR
                && requiredType->containerKind == H2TypeContainer_SCALAR
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
    H2CBackendC* c, const char* fieldName, const H2TypeRef* requiredType) {
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

int EmitContextArgForSig(H2CBackendC* c, const H2FnSig* sig) {
    uint32_t i;
    uint32_t fieldCount = 0;
    int32_t  directContextNode = ActiveCallDirectContextNode(c);
    if (sig == NULL || !sig->hasContext) {
        return 0;
    }
    if (!sig->contextType.valid || sig->contextType.baseName == NULL) {
        return -1;
    }
    if (directContextNode >= 0) {
        H2TypeRef contextParamType = sig->contextType;
        contextParamType.ptrDepth++;
        contextParamType.readOnly = 0;
        return EmitExprCoerced(c, directContextNode, &contextParamType);
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
            const H2FieldInfo* f = &c->fieldInfos[i];
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
    H2CBackendC*          c,
    int32_t               callNode,
    const char*           calleeName,
    const H2FnSig*        sig,
    const H2CCallBinding* binding,
    int                   autoRefFirstArg) {
    uint32_t i;
    (void)callNode;
    if (sig == NULL || binding == NULL) {
        return -1;
    }
    if (StrEq(calleeName, "print")) {
        calleeName = "builtin__print";
    } else if (StrEq(calleeName, "source_location_of")) {
        calleeName = "builtin__source_location_of";
    }
    if (BufAppendCStr(&c->out, calleeName) != 0 || BufAppendChar(&c->out, '(') != 0) {
        return -1;
    }
    if (sig != NULL && (sig->hasContext || StrEq(calleeName, "builtin__print"))) {
        if (sig->hasContext) {
            if (EmitContextArgForSig(c, sig) != 0) {
                return -1;
            }
        } else if (BufAppendCStr(&c->out, "context") != 0) {
            return -1;
        }
        if (sig->paramLen > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
    }
    for (i = 0; i < sig->paramLen; i++) {
        int32_t   argNode = -1;
        H2TypeRef argType;
        if (i != 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (binding->activePackSpread && i >= binding->activePackSpreadParamStart
            && i < binding->activePackSpreadParamStart + c->activePackElemCount)
        {
            uint32_t packIndex = i - binding->activePackSpreadParamStart;
            if (EmitActivePackElemNameCoerced(c, packIndex, &sig->paramTypes[i]) != 0) {
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, callNode, H2Diag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            continue;
        }
        if (!binding->isVariadic || i < binding->fixedCount) {
            argNode = binding->fixedMappedArgNodes[i];
            {
                int32_t          argExprNode = argNode;
                const H2AstNode* argExpr = NodeAt(c, argExprNode);
                int32_t          argCallNode = argExprNode;
                const H2AstNode* argCallee = NULL;
                if (argExpr != NULL && argExpr->kind == H2Ast_CALL_ARG) {
                    argExprNode = AstFirstChild(&c->ast, argExprNode);
                    argExpr = NodeAt(c, argExprNode);
                    argCallNode = argExprNode;
                }
                if (argExpr != NULL && argExpr->kind == H2Ast_CALL_WITH_CONTEXT) {
                    argCallNode = AstFirstChild(&c->ast, argExprNode);
                    argExpr = NodeAt(c, argCallNode);
                }
                if (argExpr != NULL && argExpr->kind == H2Ast_CALL) {
                    argCallee = NodeAt(c, AstFirstChild(&c->ast, argCallNode));
                }
                if (StrEq(calleeName, "builtin__print") && argExpr != NULL
                    && (argExpr->kind == H2Ast_CALL || argExpr->kind == H2Ast_CALL_WITH_CONTEXT))
                {
                    if (BufAppendCStr(&c->out, "(*(") != 0 || EmitExpr(c, argExprNode) != 0
                        || BufAppendCStr(&c->out, "))") != 0)
                    {
                        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                            SetDiagNode(c, argNode, H2Diag_CODEGEN_INTERNAL);
                        }
                        return -1;
                    }
                    continue;
                }
                if ((TypeRefIsBorrowedStrValue(&sig->paramTypes[i])
                     || StrEq(calleeName, "builtin__print"))
                    && argCallee != NULL && argCallee->kind == H2Ast_IDENT
                    && SliceEq(c->unit->source, argCallee->dataStart, argCallee->dataEnd, "concat"))
                {
                    if (BufAppendCStr(&c->out, "(*(") != 0 || EmitExpr(c, argExprNode) != 0
                        || BufAppendCStr(&c->out, "))") != 0)
                    {
                        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                            SetDiagNode(c, argNode, H2Diag_CODEGEN_INTERNAL);
                        }
                        return -1;
                    }
                    continue;
                }
            }
            if (argNode < 0) {
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, callNode, H2Diag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            if (autoRefFirstArg && i == 0) {
                if (BufAppendCStr(&c->out, "((") != 0
                    || EmitTypeNameWithDepth(c, &sig->paramTypes[i]) != 0
                    || BufAppendCStr(&c->out, ")(&(") != 0 || EmitExpr(c, argNode) != 0
                    || BufAppendCStr(&c->out, ")))") != 0)
                {
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, argNode, H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
            } else if (
                InferExprType(c, argNode, &argType) == 0 && argType.valid
                && TypeRefIsBorrowedStrValue(&sig->paramTypes[i]) && TypeRefIsStr(&argType))
            {
                if (EmitStrValueExpr(c, argNode, &argType) != 0) {
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, argNode, H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
            } else if (EmitExprCoerced(c, argNode, &sig->paramTypes[i]) != 0) {
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, argNode, H2Diag_CODEGEN_INTERNAL);
                }
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
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, argNode >= 0 ? argNode : callNode, H2Diag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            continue;
        }

        if (binding->explicitTailCount == 0) {
            if (sig->paramTypes[i].containerKind == H2TypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__hop_slice_mut){ (void*)NULL, (__hop_int)0 })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__hop_slice_ro){ (const void*)NULL, (__hop_int)0 })")
                    != 0)
                {
                    return -1;
                }
            }
            continue;
        }

        {
            H2TypeRef elemType = sig->paramTypes[i];
            uint32_t  j;
            elemType.containerKind = H2TypeContainer_SCALAR;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            if (BufAppendCStr(&c->out, "(__extension__({ ") != 0
                || EmitTypeNameWithDepth(c, &elemType) != 0
                || BufAppendCStr(&c->out, " __hop_va[") != 0
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
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, binding->explicitTailNodes[j], H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
            }
            if (sig->paramTypes[i].containerKind == H2TypeContainer_SLICE_MUT) {
                if (BufAppendCStr(
                        &c->out, " }; ((__hop_slice_mut){ (void*)(__hop_va), (__hop_int)(")
                        != 0
                    || BufAppendU32(&c->out, binding->explicitTailCount) != 0
                    || BufAppendCStr(&c->out, "u) }); }))") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(
                        &c->out, " }; ((__hop_slice_ro){ (const void*)(__hop_va), (__hop_int)(")
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
    H2CBackendC* c, const char* base, const H2FieldInfo* const* path, uint32_t pathLen) {
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
    H2CBackendC* c, const H2AstNode* field, int32_t exprNode, const H2TypeRef* _Nullable dstType) {
    if (exprNode >= 0) {
        return EmitExprCoerced(c, exprNode, dstType);
    }
    if (field == NULL || (field->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
        return -1;
    }
    return BufAppendSlice(&c->out, c->unit->source, field->dataStart, field->dataEnd);
}

static int LocalNameExists(const H2CBackendC* c, const char* name) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (StrEq(c->locals[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

static int DirectFieldNameExists(const H2CBackendC* c, const char* ownerType, const char* name) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        const H2FieldInfo* f = &c->fieldInfos[i];
        if (StrEq(f->ownerType, ownerType) && !f->isEmbedded && StrEq(f->fieldName, name)) {
            return 1;
        }
    }
    return 0;
}

static const char* _Nullable EmbeddedFieldOwnerType(
    const H2CBackendC* c, const H2FieldInfo* field) {
    if (field == NULL || !field->type.valid || field->type.containerKind != H2TypeContainer_SCALAR
        || field->type.ptrDepth != 0 || field->type.containerPtrDepth != 0
        || field->type.baseName == NULL)
    {
        return NULL;
    }
    return CanonicalFieldOwnerType(c, field->type.baseName);
}

static int EmitFieldPathExpr(
    H2CBackendC* c, const char* base, const H2FieldInfo* const* path, uint32_t pathLen) {
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

static int EmitPromotedFieldLocalBindings(
    H2CBackendC*              c,
    const char*               outerOwnerType,
    const char*               base,
    const H2FieldInfo* const* path,
    uint32_t                  pathLen,
    const char*               ownerType) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        const H2FieldInfo* f = &c->fieldInfos[i];
        if (!StrEq(f->ownerType, ownerType)) {
            continue;
        }
        if (f->isEmbedded) {
            const char* nestedOwner = EmbeddedFieldOwnerType(c, f);
            if (nestedOwner != NULL && pathLen < 64u) {
                const H2FieldInfo* nestedPath[64];
                uint32_t           j;
                for (j = 0; j < pathLen; j++) {
                    nestedPath[j] = path[j];
                }
                nestedPath[pathLen] = f;
                if (EmitPromotedFieldLocalBindings(
                        c, outerOwnerType, base, nestedPath, pathLen + 1u, nestedOwner)
                    != 0)
                {
                    return -1;
                }
            }
            continue;
        }
        if (DirectFieldNameExists(c, outerOwnerType, f->fieldName)
            || LocalNameExists(c, f->fieldName) || !f->type.valid || f->type.baseName == NULL
            || TypeRefIsFunctionAlias(c, &f->type))
        {
            continue;
        }
        if (BufAppendCStr(&c->out, "    ") != 0 || EmitTypeNameWithDepth(c, &f->type) != 0
            || BufAppendChar(&c->out, ' ') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
            || BufAppendCStr(&c->out, " = ") != 0 || EmitFieldPathExpr(c, base, path, pathLen) != 0
            || BufAppendChar(&c->out, '.') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
            || BufAppendCStr(&c->out, ";\n") != 0 || AddLocal(c, f->fieldName, f->type) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int EmitReplayPromotedExplicitFieldsForTop(
    H2CBackendC* c, int32_t firstField, const char* ownerType, const H2FieldInfo* topField) {
    int32_t  fieldNode = firstField;
    uint32_t tempIndex = 0;
    while (fieldNode >= 0) {
        const H2AstNode*   field = NodeAt(c, fieldNode);
        const H2FieldInfo* fieldPath[64];
        const H2FieldInfo* resolvedField = NULL;
        uint32_t           fieldPathLen = 0;
        if (field == NULL || field->kind != H2Ast_COMPOUND_FIELD) {
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
        if (fieldPathLen > 1u && fieldPath[0] == topField) {
            if (BufAppendCStr(&c->out, "    ") != 0
                || EmitFieldPathLValue(c, "__hop_tmp", fieldPath, fieldPathLen) != 0
                || BufAppendCStr(&c->out, " = __hop_exp_") != 0
                || BufAppendU32(&c->out, tempIndex) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
        tempIndex++;
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }
    return 0;
}

int EmitEnumVariantCompoundLiteral(
    H2CBackendC*     c,
    int32_t          nodeId,
    int32_t          firstField,
    const char*      enumTypeName,
    uint32_t         variantStart,
    uint32_t         variantEnd,
    const H2TypeRef* valueType) {
    H2TypeRef        payloadType;
    const char*      payloadOwner;
    const H2NameMap* payloadMap;
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, valueType) != 0
        || BufAppendCStr(&c->out, "){ .tag = ") != 0 || BufAppendCStr(&c->out, enumTypeName) != 0
        || BufAppendCStr(&c->out, "__") != 0
        || BufAppendSlice(&c->out, c->unit->source, variantStart, variantEnd) != 0)
    {
        return -1;
    }
    if (ResolveEnumVariantPayloadType(c, enumTypeName, variantStart, variantEnd, &payloadType) != 0
        || !payloadType.valid)
    {
        return -1;
    }
    payloadOwner = CanonicalFieldOwnerType(c, payloadType.baseName);
    if (payloadOwner == NULL) {
        return -1;
    }
    payloadMap = FindNameByCName(c, payloadOwner);
    if (BufAppendCStr(&c->out, ", .payload.") != 0
        || BufAppendSlice(&c->out, c->unit->source, variantStart, variantEnd) != 0
        || BufAppendCStr(&c->out, " = ") != 0)
    {
        return -1;
    }
    if (payloadMap != NULL && payloadMap->kind == H2Ast_STRUCT
        && StructHasFieldDefaults(c, payloadOwner))
    {
        if (EmitCompoundLiteralOrderedStruct(c, firstField, payloadOwner, &payloadType) != 0) {
            return -1;
        }
    } else if (EmitCompoundLiteralDesignated(c, firstField, payloadOwner, &payloadType) != 0) {
        return -1;
    }
    (void)nodeId;
    return BufAppendCStr(&c->out, "})");
}

static int EmitEnumVariantCallLiteral(
    H2CBackendC*     c,
    int32_t          nodeId,
    const H2NameMap* enumMap,
    uint32_t         variantStart,
    uint32_t         variantEnd,
    const H2TypeRef* valueType) {
    H2TypeRef             payloadType;
    const H2AnonTypeInfo* tupleInfo = NULL;
    int32_t               callee = AstFirstChild(&c->ast, nodeId);
    int32_t               arg = AstNextSibling(&c->ast, callee);
    uint32_t              i;
    if (enumMap == NULL || enumMap->cName == NULL) {
        return -1;
    }
    if (ResolveEnumVariantPayloadType(c, enumMap->cName, variantStart, variantEnd, &payloadType)
            != 0
        || !payloadType.valid)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, valueType) != 0
        || BufAppendCStr(&c->out, "){ .tag = ") != 0 || BufAppendCStr(&c->out, enumMap->cName) != 0
        || BufAppendCStr(&c->out, "__") != 0
        || BufAppendSlice(&c->out, c->unit->source, variantStart, variantEnd) != 0
        || BufAppendCStr(&c->out, ", .payload.") != 0
        || BufAppendSlice(&c->out, c->unit->source, variantStart, variantEnd) != 0
        || BufAppendCStr(&c->out, " = ") != 0)
    {
        return -1;
    }
    if (TypeRefTupleInfo(c, &payloadType, &tupleInfo) && tupleInfo != NULL) {
        if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, &payloadType) != 0
            || BufAppendCStr(&c->out, "){") != 0)
        {
            return -1;
        }
        for (i = 0; i < tupleInfo->fieldCount; i++) {
            const H2FieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
            int32_t            argExpr;
            if (arg < 0) {
                return -1;
            }
            argExpr = UnwrapCallArgExprNode(c, arg);
            if (argExpr < 0) {
                return -1;
            }
            if (i > 0 && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (BufAppendChar(&c->out, '.') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
                || BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, argExpr, &f->type) != 0)
            {
                return -1;
            }
            arg = AstNextSibling(&c->ast, arg);
        }
        if (arg >= 0) {
            return -1;
        }
        if (BufAppendChar(&c->out, '}') != 0 || BufAppendChar(&c->out, ')') != 0) {
            return -1;
        }
    } else {
        int32_t argExpr;
        if (arg < 0 || AstNextSibling(&c->ast, arg) >= 0) {
            return -1;
        }
        argExpr = UnwrapCallArgExprNode(c, arg);
        if (argExpr < 0 || EmitExprCoerced(c, argExpr, &payloadType) != 0) {
            return -1;
        }
    }
    return BufAppendCStr(&c->out, "})");
}

int EmitCompoundLiteralDesignated(
    H2CBackendC* c, int32_t firstField, const char* ownerType, const H2TypeRef* valueType) {
    int32_t fieldNode = firstField;
    int     first = 1;

    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, valueType) != 0
        || BufAppendCStr(&c->out, "){") != 0)
    {
        return -1;
    }

    while (fieldNode >= 0) {
        const H2AstNode*   field = NodeAt(c, fieldNode);
        const H2FieldInfo* fieldPath[64];
        const H2FieldInfo* resolvedField = NULL;
        uint32_t           fieldPathLen = 0;
        int32_t            exprNode;
        if (field == NULL || field->kind != H2Ast_COMPOUND_FIELD) {
            return -1;
        }
        exprNode = AstFirstChild(&c->ast, fieldNode);
        if (exprNode < 0 && (field->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
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
    H2CBackendC* c, int32_t firstField, const char* ownerType, const H2TypeRef* valueType) {
    const H2FieldInfo* directFields[256];
    uint8_t            directExplicit[256];
    uint32_t           directCount = 0;
    uint32_t           i;
    uint32_t           tempIndex = 0;
    int32_t            fieldNode = firstField;

    for (i = 0; i < c->fieldInfoLen; i++) {
        const H2FieldInfo* f = &c->fieldInfos[i];
        if (!StrEq(f->ownerType, ownerType)) {
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
        || BufAppendCStr(&c->out, " __hop_tmp = {0};\n") != 0)
    {
        PopScope(c);
        return -1;
    }

    while (fieldNode >= 0) {
        const H2AstNode*   field = NodeAt(c, fieldNode);
        const H2FieldInfo* fieldPath[64];
        const H2FieldInfo* resolvedField = NULL;
        uint32_t           fieldPathLen = 0;
        int32_t            exprNode;
        const H2AstNode*   expr;
        int                directFunctionFieldInit = 0;

        if (field == NULL || field->kind != H2Ast_COMPOUND_FIELD) {
            PopScope(c);
            return -1;
        }
        exprNode = AstFirstChild(&c->ast, fieldNode);
        if (exprNode < 0 && (field->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) == 0) {
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
        expr = NodeAt(c, exprNode);
        directFunctionFieldInit =
            expr != NULL && expr->kind == H2Ast_IDENT
            && TypeRefIsFunctionAlias(c, &resolvedField->type);

        if (directFunctionFieldInit) {
            if (BufAppendCStr(&c->out, "    ") != 0
                || EmitFieldPathLValue(c, "__hop_tmp", fieldPath, fieldPathLen) != 0
                || BufAppendCStr(&c->out, " = ") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                PopScope(c);
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "    ") != 0
                || EmitTypeNameWithDepth(c, &resolvedField->type) != 0
                || BufAppendCStr(&c->out, " __hop_exp_") != 0
                || BufAppendU32(&c->out, tempIndex) != 0 || BufAppendCStr(&c->out, " = ") != 0
                || EmitCompoundFieldValueCoerced(c, field, exprNode, &resolvedField->type) != 0
                || BufAppendCStr(&c->out, ";\n    ") != 0
                || EmitFieldPathLValue(c, "__hop_tmp", fieldPath, fieldPathLen) != 0
                || BufAppendCStr(&c->out, " = __hop_exp_") != 0
                || BufAppendU32(&c->out, tempIndex) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                PopScope(c);
                return -1;
            }
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
        const H2FieldInfo* f = directFields[i];
        if (!directExplicit[i] && f->defaultExprNode >= 0) {
            if (BufAppendCStr(&c->out, "    __hop_tmp.") != 0
                || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, " = ") != 0
                || EmitExprCoerced(c, f->defaultExprNode, &f->type) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                PopScope(c);
                return -1;
            }
        }
        if (f->isEmbedded
            && EmitReplayPromotedExplicitFieldsForTop(c, firstField, ownerType, f) != 0)
        {
            PopScope(c);
            return -1;
        }
        if (BufAppendCStr(&c->out, "    ") != 0 || EmitTypeNameWithDepth(c, &f->type) != 0
            || BufAppendChar(&c->out, ' ') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
            || BufAppendCStr(&c->out, " = __hop_tmp.") != 0
            || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            PopScope(c);
            return -1;
        }
        if (AddLocal(c, f->fieldName, f->type) != 0) {
            PopScope(c);
            return -1;
        }
        if (f->isEmbedded) {
            const char* embeddedOwner = EmbeddedFieldOwnerType(c, f);
            if (embeddedOwner != NULL) {
                const H2FieldInfo* path[1];
                path[0] = f;
                if (EmitPromotedFieldLocalBindings(
                        c, ownerType, "__hop_tmp", path, 1u, embeddedOwner)
                    != 0)
                {
                    PopScope(c);
                    return -1;
                }
            }
        }
    }

    if (BufAppendCStr(&c->out, "    __hop_tmp;\n}))") != 0) {
        PopScope(c);
        return -1;
    }

    PopScope(c);
    return 0;
}

int StructHasFieldDefaults(const H2CBackendC* c, const char* ownerType) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        const H2FieldInfo* f = &c->fieldInfos[i];
        if (StrEq(f->ownerType, ownerType) && f->defaultExprNode >= 0) {
            return 1;
        }
    }
    return 0;
}

int EmitCompoundLiteral(H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType) {
    const H2AstNode* litNode = NodeAt(c, nodeId);
    H2TypeRef        litType;
    H2TypeRef        valueType;
    const char*      ownerType;
    const H2NameMap* ownerMap;
    int32_t          fieldNode;
    int32_t          typeNode = -1;

    if (litNode == NULL || litNode->kind != H2Ast_COMPOUND_LIT) {
        return -1;
    }
    if (InferCompoundLiteralType(c, nodeId, expectedType, &litType) != 0 || !litType.valid) {
        return -1;
    }

    valueType = litType;
    if (valueType.containerKind != H2TypeContainer_SCALAR) {
        if (valueType.containerPtrDepth <= 0) {
            return -1;
        }
        valueType.containerPtrDepth--;
    } else if (valueType.ptrDepth > 0) {
        valueType.ptrDepth--;
    }
    if (!valueType.valid || valueType.containerKind != H2TypeContainer_SCALAR
        || valueType.containerPtrDepth != 0 || valueType.ptrDepth != 0
        || valueType.baseName == NULL)
    {
        return -1;
    }
    {
        const char* storageType = ResolveScalarAliasBaseName(c, valueType.baseName);
        ownerType = CanonicalFieldOwnerType(c, valueType.baseName);
        if (storageType != NULL) {
            valueType.baseName = storageType;
        }
    }
    if (ownerType == NULL) {
        return -1;
    }
    ownerMap = FindNameByCName(c, ownerType);

    fieldNode = AstFirstChild(&c->ast, nodeId);
    if (fieldNode >= 0 && NodeAt(c, fieldNode) != NULL
        && IsTypeNodeKind(NodeAt(c, fieldNode)->kind))
    {
        typeNode = fieldNode;
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }

    if (ownerMap != NULL && ownerMap->kind == H2Ast_ENUM) {
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

    if (ownerMap != NULL && ownerMap->kind == H2Ast_STRUCT && StructHasFieldDefaults(c, ownerType))
    {
        return EmitCompoundLiteralOrderedStruct(c, fieldNode, ownerType, &valueType);
    }
    return EmitCompoundLiteralDesignated(c, fieldNode, ownerType, &valueType);
}

static int EmitReadonlyArrayLiteralStatic(
    H2CBackendC* c,
    int32_t      nodeId,
    const H2TypeRef* _Nonnull expectedType,
    const H2TypeRef* _Nonnull arrayType,
    const H2TypeRef* _Nonnull elemType,
    uint32_t targetLen) {
    uint32_t tempId = FmtNextTempId(c);
    int32_t  child;
    uint32_t i = 0;
    if (BufAppendCStr(&c->out, "(__extension__({\n    static const ") != 0
        || EmitElementTypeName(c, elemType, 0) != 0
        || BufAppendCStr(&c->out, " __hop_array_lit_") != 0 || BufAppendU32(&c->out, tempId) != 0
        || BufAppendChar(&c->out, '[') != 0 || BufAppendU32(&c->out, targetLen) != 0
        || BufAppendCStr(&c->out, "] = {") != 0)
    {
        return -1;
    }
    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        if (i > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (EmitExprCoerced(c, child, elemType) != 0) {
            return -1;
        }
        i++;
        child = AstNextSibling(&c->ast, child);
    }
    if (BufAppendCStr(&c->out, "};\n    ") != 0) {
        return -1;
    }
    if (expectedType->containerKind == H2TypeContainer_SLICE_RO) {
        if (BufAppendCStr(&c->out, "((__hop_slice_ro){ (const void*)__hop_array_lit_") != 0
            || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, ", (__hop_int)") != 0
            || BufAppendU32(&c->out, arrayType->arrayLen) != 0
            || BufAppendCStr(&c->out, " })") != 0)
        {
            return -1;
        }
    } else if (
        expectedType->containerKind == H2TypeContainer_ARRAY && expectedType->containerPtrDepth > 0)
    {
        if (BufAppendChar(&c->out, '(') != 0 || EmitElementTypeName(c, elemType, 0) != 0
            || BufAppendCStr(&c->out, "*)(uintptr_t)(const void*)__hop_array_lit_") != 0
            || BufAppendU32(&c->out, tempId) != 0)
        {
            return -1;
        }
    } else {
        return -1;
    }
    return BufAppendCStr(&c->out, ";\n}))");
}

int EmitArrayLiteral(H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType) {
    H2TypeRef arrayType;
    H2TypeRef elemType;
    uint32_t  targetLen = 0;
    int       hasTargetLen = 0;
    int32_t   child;
    uint32_t  i = 0;
    int       needsCompoundCast;
    if (InferArrayLiteralType(c, nodeId, expectedType, &arrayType) != 0 || !arrayType.valid
        || arrayType.containerKind != H2TypeContainer_ARRAY)
    {
        return -1;
    }
    elemType = arrayType;
    elemType.containerKind = H2TypeContainer_SCALAR;
    elemType.containerPtrDepth = 0;
    elemType.arrayLen = 0;
    elemType.hasArrayLen = 0;
    if (expectedType != NULL && expectedType->valid) {
        H2TypeRef expectedElem;
        if (ArrayLitElementTypeFromExpected(expectedType, &expectedElem, &targetLen, &hasTargetLen))
        {
            elemType = expectedElem;
        }
        if ((expectedType->readOnly || expectedType->containerKind == H2TypeContainer_SLICE_RO)
            && (expectedType->containerKind == H2TypeContainer_SLICE_RO
                || (expectedType->containerKind == H2TypeContainer_ARRAY
                    && expectedType->containerPtrDepth > 0)))
        {
            return EmitReadonlyArrayLiteralStatic(
                c,
                nodeId,
                expectedType,
                &arrayType,
                &elemType,
                hasTargetLen ? targetLen : arrayType.arrayLen);
        }
    }
    needsCompoundCast =
        expectedType == NULL || !expectedType->valid
        || expectedType->containerKind != H2TypeContainer_ARRAY
        || expectedType->containerPtrDepth != 0;
    if (needsCompoundCast) {
        if (BufAppendCStr(&c->out, "((") != 0 || EmitElementTypeName(c, &elemType, 0) != 0
            || BufAppendChar(&c->out, '[') != 0
            || BufAppendU32(&c->out, hasTargetLen ? targetLen : arrayType.arrayLen) != 0
            || BufAppendCStr(&c->out, "])") != 0)
        {
            return -1;
        }
    }
    if (BufAppendChar(&c->out, '{') != 0) {
        return -1;
    }
    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        if (i > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (EmitExprCoerced(c, child, &elemType) != 0) {
            return -1;
        }
        i++;
        child = AstNextSibling(&c->ast, child);
    }
    if (BufAppendChar(&c->out, '}') != 0) {
        return -1;
    }
    return needsCompoundCast ? BufAppendChar(&c->out, ')') : 0;
}

static int EmitFixedStrArrayLiteralInitializer(
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* elemType) {
    int32_t  child = AstFirstChild(&c->ast, nodeId);
    uint32_t i = 0;
    if (BufAppendChar(&c->out, '{') != 0) {
        return -1;
    }
    while (child >= 0) {
        const H2AstNode* childNode = NodeAt(c, child);
        if (i > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (childNode != NULL
            && (childNode->kind == H2Ast_STRING
                || (childNode->kind == H2Ast_BINARY && (H2TokenKind)childNode->op == H2Tok_ADD)))
        {
            int32_t literalId = -1;
            if ((uint32_t)child < c->stringLitByNodeLen) {
                literalId = c->stringLitByNode[child];
            }
            if (literalId < 0 || EmitStringLiteralValue(c, literalId, 0) != 0) {
                return -1;
            }
        } else if (EmitExprCoerced(c, child, elemType) != 0) {
            return -1;
        }
        i++;
        child = AstNextSibling(&c->ast, child);
    }
    return BufAppendChar(&c->out, '}');
}

int EmitExpr_IDENT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    const H2Local* local = NULL;
    int32_t        topVarLikeNode = -1;
    (void)nodeId;
    local = FindLocalBySlice(c, n->dataStart, n->dataEnd);
    if (local != NULL) {
        int32_t localIdx = FindLocalIndexBySlice(c, n->dataStart, n->dataEnd);
        if (c->hasActiveOptionalNarrow && localIdx >= 0
            && localIdx == c->activeOptionalNarrowLocalIdx
            && TypeRefIsTaggedOptional(&c->activeOptionalNarrowStorageType))
        {
            if (BufAppendChar(&c->out, '(') != 0
                || AppendMappedIdentifier(c, n->dataStart, n->dataEnd) != 0
                || BufAppendCStr(&c->out, ".__hop_value)") != 0)
            {
                return -1;
            }
            return 0;
        }
        return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
    }
    if (FindFnSigBySlice(c, n->dataStart, n->dataEnd) != NULL
        || FindTopLevelVarLikeNodeBySlice(c, n->dataStart, n->dataEnd, &topVarLikeNode) == 0)
    {
        return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
    }
    {
        H2TypeRef typeValue;
        if (ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, &typeValue)) {
            return EmitTypeTagLiteralFromTypeRef(c, &typeValue);
        }
    }
    return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
}

int EmitExpr_INT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    (void)nodeId;
    return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
}

int EmitExpr_RUNE(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    uint32_t     rune = 0;
    H2RuneLitErr runeErr = { 0 };
    (void)nodeId;
    if (H2DecodeRuneLiteralValidate(c->unit->source, n->dataStart, n->dataEnd, &rune, &runeErr)
        != 0)
    {
        SetDiag(c->diag, H2RuneLitErrDiagCode(runeErr.kind), runeErr.start, runeErr.end);
        return -1;
    }
    return BufAppendU32(&c->out, rune);
}

int EmitExpr_FLOAT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    (void)nodeId;
    return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
}

int EmitExpr_BOOL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    (void)nodeId;
    if (n->dataEnd > n->dataStart && c->unit->source[n->dataStart] == 't') {
        return BufAppendCStr(&c->out, "1");
    }
    return BufAppendCStr(&c->out, "0");
}

int EmitExpr_COMPOUND_LIT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    (void)n;
    return EmitCompoundLiteral(c, nodeId, NULL);
}

int EmitExpr_ARRAY_LIT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    (void)n;
    return EmitArrayLiteral(c, nodeId, NULL);
}

int EmitExpr_STRING(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t literalId = -1;
    (void)n;
    if (nodeId >= 0 && (uint32_t)nodeId < c->stringLitByNodeLen) {
        literalId = c->stringLitByNode[nodeId];
    }
    if (literalId < 0) {
        return -1;
    }
    return EmitStringLiteralValue(c, literalId, 0);
}

int EmitExpr_UNARY(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    if ((H2TokenKind)n->op == H2Tok_AND && child >= 0) {
        const H2AstNode* cn = NodeAt(c, child);
        if (cn != NULL && cn->kind == H2Ast_FIELD_EXPR) {
            int32_t            recv = AstFirstChild(&c->ast, child);
            H2TypeRef          recvType;
            H2TypeRef          ownerType;
            const H2FieldInfo* fieldPath[64];
            uint32_t           fieldPathLen = 0;
            const H2FieldInfo* field = NULL;
            H2TypeRef          childType;
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
            H2TypeRef childType;
            if (InferExprType(c, child, &childType) == 0 && childType.valid
                && childType.containerKind == H2TypeContainer_ARRAY
                && childType.containerPtrDepth == 0)
            {
                return EmitElemPtrExpr(c, child, &childType, TypeRefContainerWritable(&childType));
            }
        }
    }
    if ((H2TokenKind)n->op == H2Tok_MUL && child >= 0) {
        H2TypeRef childType;
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
        || BufAppendCStr(&c->out, UnaryOpString((H2TokenKind)n->op)) != 0 || EmitExpr(c, child) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_BINARY(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t     lhs = AstFirstChild(&c->ast, nodeId);
    int32_t     rhs = AstNextSibling(&c->ast, lhs);
    H2TokenKind op = (H2TokenKind)n->op;
    if (op == H2Tok_ADD) {
        int32_t literalId = -1;
        if (nodeId >= 0 && (uint32_t)nodeId < c->stringLitByNodeLen) {
            literalId = c->stringLitByNode[nodeId];
        }
        if (literalId >= 0) {
            return EmitStringLiteralValue(c, literalId, 0);
        }
    }
    if (op == H2Tok_ASSIGN) {
        H2TypeRef lhsType;
        if (lhs < 0 || rhs < 0) {
            return -1;
        }
        if (ExprIsHoleIdent(c, lhs)) {
            return EmitExpr(c, rhs);
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
    if (op == H2Tok_EQ || op == H2Tok_NEQ || op == H2Tok_LT || op == H2Tok_LTE || op == H2Tok_GT
        || op == H2Tok_GTE)
    {
        H2TypeRef        lhsType;
        H2TypeRef        rhsType;
        const H2FnSig*   hookSig = NULL;
        const char*      hookCalleeName = NULL;
        int              hookAutoRef[2] = { 0, 0 };
        int              hookStatus;
        int              lhsNull;
        int              rhsNull;
        int              isEqOp = (op == H2Tok_EQ || op == H2Tok_NEQ);
        const H2TypeRef* seqType = NULL;
        lhsNull = lhs >= 0 && NodeAt(c, lhs) != NULL && NodeAt(c, lhs)->kind == H2Ast_NULL;
        rhsNull = rhs >= 0 && NodeAt(c, rhs) != NULL && NodeAt(c, rhs)->kind == H2Ast_NULL;
        if (lhs < 0 || rhs < 0 || InferExprType(c, lhs, &lhsType) != 0
            || InferExprType(c, rhs, &rhsType) != 0)
        {
            goto emit_raw_binary;
        }
        if ((!lhsType.valid && !lhsNull) || (!rhsType.valid && !rhsNull)) {
            goto emit_raw_binary;
        }
        if (isEqOp && lhsType.valid && lhsType.isOptional && rhsNull) {
            if (op == H2Tok_EQ && BufAppendChar(&c->out, '!') != 0) {
                return -1;
            }
            return EmitOptionalIsSomeExpr(c, lhs, &lhsType, 0);
        }
        if (isEqOp && rhsType.valid && rhsType.isOptional && lhsNull) {
            if (op == H2Tok_EQ && BufAppendChar(&c->out, '!') != 0) {
                return -1;
            }
            return EmitOptionalIsSomeExpr(c, rhs, &rhsType, 0);
        }
        if (isEqOp && lhsType.valid && lhsType.isOptional && rhsType.valid && !rhsType.isOptional
            && !rhsNull && TypeRefIsTaggedOptional(&lhsType))
        {
            return EmitTaggedOptionalCompareWithValue(c, lhs, &lhsType, rhs, op == H2Tok_EQ);
        }
        if (isEqOp && rhsType.valid && rhsType.isOptional && lhsType.valid && !lhsType.isOptional
            && !lhsNull && TypeRefIsTaggedOptional(&rhsType))
        {
            return EmitTaggedOptionalCompareWithValue(c, rhs, &rhsType, lhs, op == H2Tok_EQ);
        }

        {
            const char* lhsPayloadEnum = NULL;
            const char* rhsPayloadEnum = NULL;
            if (ResolvePayloadEnumType(c, &lhsType, &lhsPayloadEnum)
                && ResolvePayloadEnumType(c, &rhsType, &rhsPayloadEnum) && lhsPayloadEnum != NULL
                && rhsPayloadEnum != NULL && StrEq(lhsPayloadEnum, rhsPayloadEnum))
            {
                if (isEqOp) {
                    if (op == H2Tok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                        return -1;
                    }
                    if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_cmp_a = ") != 0
                        || EmitExprCoerced(c, lhs, &lhsType) != 0
                        || BufAppendCStr(&c->out, "; __auto_type __hop_cmp_b = ") != 0
                        || EmitExprCoerced(c, rhs, &lhsType) != 0
                        || BufAppendCStr(
                               &c->out,
                               "; __hop_mem_equal((const void*)&__hop_cmp_a, (const "
                               "void*)&__hop_cmp_b, (__hop_uint)sizeof(__hop_cmp_a)); }))")
                               != 0)
                    {
                        return -1;
                    }
                    if (op == H2Tok_NEQ && BufAppendChar(&c->out, ')') != 0) {
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
                if (op == H2Tok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                    return -1;
                }
                if (BufAppendChar(&c->out, '(') != 0
                    || EmitComparisonHookCall(c, hookSig, hookCalleeName, lhs, rhs, hookAutoRef)
                           != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                if (op == H2Tok_NEQ && BufAppendChar(&c->out, ')') != 0) {
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
                case H2Tok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GTE:
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
                if (op == H2Tok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__hop_str_equal(") != 0
                    || EmitExprCoerced(c, lhs, &lhsType) != 0 || BufAppendCStr(&c->out, ", ") != 0
                    || EmitExprCoerced(c, rhs, &lhsType) != 0 || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                if (op == H2Tok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                    return -1;
                }
                return 0;
            }
            if (BufAppendCStr(&c->out, "(__hop_str_order(") != 0
                || EmitExprCoerced(c, lhs, &lhsType) != 0 || BufAppendCStr(&c->out, ", ") != 0
                || EmitExprCoerced(c, rhs, &lhsType) != 0 || BufAppendCStr(&c->out, ") ") != 0)
            {
                return -1;
            }
            switch (op) {
                case H2Tok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GTE:
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
            if (BufAppendCStr(&c->out, "(__hop_ptr_order(") != 0) {
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
                case H2Tok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GTE:
                    if (BufAppendCStr(&c->out, ">= 0)") != 0) {
                        return -1;
                    }
                    break;
                default: return -1;
            }
            return 0;
        }

        if ((lhsType.containerKind == H2TypeContainer_ARRAY
             || lhsType.containerKind == H2TypeContainer_SLICE_RO
             || lhsType.containerKind == H2TypeContainer_SLICE_MUT)
            && !TypeRefIsPointerLike(&lhsType))
        {
            seqType = &lhsType;
            if (isEqOp) {
                if (op == H2Tok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__hop_slice_equal_ro(") != 0
                    || EmitExprAsSliceRO(c, lhs, &lhsType) != 0 || BufAppendCStr(&c->out, ", ") != 0
                    || EmitExprAsSliceRO(c, rhs, &rhsType) != 0
                    || BufAppendCStr(&c->out, ", (__hop_int)sizeof(") != 0
                    || EmitElementTypeName(c, seqType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
                {
                    return -1;
                }
                if (op == H2Tok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                    return -1;
                }
                return 0;
            }
            if (BufAppendCStr(&c->out, "(__hop_slice_order_ro(") != 0
                || EmitExprAsSliceRO(c, lhs, &lhsType) != 0 || BufAppendCStr(&c->out, ", ") != 0
                || EmitExprAsSliceRO(c, rhs, &rhsType) != 0
                || BufAppendCStr(&c->out, ", (__hop_int)sizeof(") != 0
                || EmitElementTypeName(c, seqType, 0) != 0 || BufAppendCStr(&c->out, ")) ") != 0)
            {
                return -1;
            }
            switch (op) {
                case H2Tok_LT:
                    if (BufAppendCStr(&c->out, "< 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_LTE:
                    if (BufAppendCStr(&c->out, "<= 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GT:
                    if (BufAppendCStr(&c->out, "> 0)") != 0) {
                        return -1;
                    }
                    break;
                case H2Tok_GTE:
                    if (BufAppendCStr(&c->out, ">= 0)") != 0) {
                        return -1;
                    }
                    break;
                default: return -1;
            }
            return 0;
        }

        if (isEqOp
            && (TypeRefIsNamedDeclKind(c, &lhsType, H2Ast_STRUCT)
                || TypeRefIsNamedDeclKind(c, &lhsType, H2Ast_UNION)))
        {
            if (op == H2Tok_NEQ && BufAppendCStr(&c->out, "(!") != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_cmp_a = ") != 0
                || EmitExprCoerced(c, lhs, &lhsType) != 0
                || BufAppendCStr(&c->out, "; __auto_type __hop_cmp_b = ") != 0
                || EmitExprCoerced(c, rhs, &lhsType) != 0
                || BufAppendCStr(
                       &c->out,
                       "; __hop_mem_equal((const void*)&__hop_cmp_a, (const "
                       "void*)&__hop_cmp_b, (__hop_uint)sizeof(__hop_cmp_a)); }))")
                       != 0)
            {
                return -1;
            }
            if (op == H2Tok_NEQ && BufAppendChar(&c->out, ')') != 0) {
                return -1;
            }
            return 0;
        }
    }
emit_raw_binary:
    if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, lhs) != 0
        || BufAppendChar(&c->out, ' ') != 0
        || BufAppendCStr(&c->out, BinaryOpString((H2TokenKind)n->op)) != 0
        || BufAppendChar(&c->out, ' ') != 0 || EmitExpr(c, rhs) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_CALL_WITH_CONTEXT(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t          savedActive = c->activeCallWithNode;
    int32_t          callNode = AstFirstChild(&c->ast, nodeId);
    int32_t          calleeNode = -1;
    const H2AstNode* callee = NULL;
    int              rc;
    (void)n;
    if (callNode < 0 || NodeAt(c, callNode) == NULL || NodeAt(c, callNode)->kind != H2Ast_CALL) {
        return -1;
    }
    calleeNode = AstFirstChild(&c->ast, callNode);
    callee = NodeAt(c, calleeNode);
    c->activeCallWithNode = nodeId;
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "concat"))
    {
        if (BufAppendCStr(&c->out, "(*(") != 0 || EmitExpr(c, callNode) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            c->activeCallWithNode = savedActive;
            return -1;
        }
        c->activeCallWithNode = savedActive;
        return 0;
    }
    rc = EmitExpr(c, callNode);
    c->activeCallWithNode = savedActive;
    return rc;
}

int EmitExpr_CALL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    const H2AstNode* callee = NodeAt(c, child);
    (void)n;
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "kind"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        H2TypeRef argType;
        H2TypeRef reflectedType;
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
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "base"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        H2TypeRef argType;
        H2TypeRef reflectedType;
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
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "is_alias"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        H2TypeRef argType;
        H2TypeRef reflectedType;
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
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "type_name"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        H2TypeRef argType;
        H2TypeRef reflectedType;
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
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "typeof"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        int32_t   idxNode = -1;
        int       isConstIndex = 0;
        uint32_t  constIndex = 0;
        H2TypeRef argType;
        if (arg < 0 || extra >= 0) {
            return -1;
        }
        if (ResolveActivePackIndexExpr(c, arg, &idxNode, &isConstIndex, &constIndex) == 0
            && !isConstIndex)
        {
            return EmitDynamicActivePackTypeTag(c, idxNode);
        }
        if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
            return -1;
        }
        return EmitTypeTagLiteralFromTypeRef(c, &argType);
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "ptr")
            || SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "slice")
            || SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "array")))
    {
        int32_t   arg0 = AstNextSibling(&c->ast, child);
        int32_t   arg1 = arg0 >= 0 ? AstNextSibling(&c->ast, arg0) : -1;
        int32_t   arg2 = arg1 >= 0 ? AstNextSibling(&c->ast, arg1) : -1;
        H2TypeRef arg0Type;
        H2TypeRef arg1Type;
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
                        arg1Type.containerKind == H2TypeContainer_SCALAR && arg1Type.ptrDepth == 0
                        && arg1Type.containerPtrDepth == 0 && !arg1Type.isOptional
                        && lenBase != NULL && IsIntegerCTypeName(lenBase);
                }
            } else {
                builtinShape = arg1 < 0;
            }
        }
        if (builtinShape) {
            H2TypeRef reflectedType;
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
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "len"))
    {
        int32_t          arg = AstNextSibling(&c->ast, child);
        int32_t          argExpr = UnwrapCallArgExprNode(c, arg);
        int32_t          extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        H2TypeRef        argType;
        const H2AstNode* argNode = NodeAt(c, argExpr);
        if (arg < 0 || extra >= 0) {
            return -1;
        }
        if (argNode != NULL && argNode->kind == H2Ast_IDENT
            && IsActivePackIdent(c, argNode->dataStart, argNode->dataEnd))
        {
            return BufAppendU32(&c->out, c->activePackElemCount);
        }
        if (argExpr < 0 || InferExprType(c, argExpr, &argType) != 0 || !argType.valid) {
            return -1;
        }
        return EmitLenExprFromType(c, argExpr, &argType);
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "cstr"))
    {
        int32_t   arg = AstNextSibling(&c->ast, child);
        int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        H2TypeRef argType;
        if (arg < 0 || extra >= 0) {
            return -1;
        }
        if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "__hop_cstr(") != 0
            || (TypeRefIsStr(&argType) ? EmitStrValueExpr(c, arg, &argType) : EmitExpr(c, arg)) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "copy"))
    {
        return EmitCopyCallExpr(c, child);
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "concat"))
    {
        return EmitConcatCallExpr(c, child);
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
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
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "panic"))
    {
        int32_t msgArg = AstNextSibling(&c->ast, child);
        if (msgArg < 0) {
            return -1;
        }
        return EmitBuiltinPanicCall(c, msgArg);
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "format"))
    {
        int rc = TryEmitFormatTemplateCall(c, nodeId, child, 0, -1, "builtin__format");
        if (rc <= 0) {
            return rc;
        }
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "format_str"))
    {
        int rc = TryEmitFormatTemplateCall(c, nodeId, child, 0, -1, "builtin__format_str");
        if (rc <= 0) {
            return rc;
        }
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "platform__exit"))
    {
        int32_t statusArg = AstNextSibling(&c->ast, child);
        int32_t extra = statusArg >= 0 ? AstNextSibling(&c->ast, statusArg) : -1;
        if (statusArg < 0 || extra >= 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, "platform__exit(context, (__hop_i32)(") != 0
            || EmitExpr(c, statusArg) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (callee != NULL && callee->kind == H2Ast_FIELD_EXPR) {
        const H2NameMap* enumMap = NULL;
        uint32_t         variantStart = 0;
        uint32_t         variantEnd = 0;
        int              variantRc = ResolveEnumSelectorByFieldExpr(
            c, child, &enumMap, NULL, NULL, &variantStart, &variantEnd);
        int32_t            recvNode = AstFirstChild(&c->ast, child);
        H2TypeRef          recvType;
        H2TypeRef          ownerType;
        const H2FieldInfo* fieldPath[64];
        uint32_t           fieldPathLen = 0;
        const H2FieldInfo* field = NULL;
        int                hasField = 0;
        if (variantRc < 0) {
            return -1;
        }
        if (variantRc == 1 && enumMap != NULL) {
            H2TypeRef valueType;
            TypeRefSetScalar(&valueType, enumMap->cName);
            return EmitEnumVariantCallLiteral(
                c, nodeId, enumMap, variantStart, variantEnd, &valueType);
        }
        if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "format")) {
            int rc = TryEmitFormatTemplateCall(c, nodeId, child, 1, recvNode, "builtin__format");
            if (rc <= 0) {
                return rc;
            }
        }
        if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "format_str")) {
            int rc = TryEmitFormatTemplateCall(
                c, nodeId, child, 1, recvNode, "builtin__format_str");
            if (rc <= 0) {
                return rc;
            }
        }
        if (recvNode >= 0 && InferExprType(c, recvNode, &recvType) == 0 && recvType.valid) {
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "kind")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                H2TypeRef reflectedType;
                if (extra < 0 && TypeRefIsTypeValue(&recvType)) {
                    if (ResolveReflectedTypeValueExprTypeRef(c, recvNode, &reflectedType)) {
                        return EmitTypeTagKindLiteralFromTypeRef(c, &reflectedType);
                    }
                    return EmitRuntimeTypeTagKindFromExpr(c, recvNode);
                }
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "base")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                H2TypeRef reflectedType;
                if (extra < 0 && TypeRefIsTypeValue(&recvType)) {
                    if (!ResolveReflectedTypeValueExprTypeRef(c, recvNode, &reflectedType)) {
                        return -1;
                    }
                    return EmitTypeTagBaseLiteralFromTypeRef(c, &reflectedType);
                }
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "is_alias")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                H2TypeRef reflectedType;
                if (extra < 0 && TypeRefIsTypeValue(&recvType)) {
                    if (ResolveReflectedTypeValueExprTypeRef(c, recvNode, &reflectedType)) {
                        return EmitTypeTagIsAliasLiteralFromTypeRef(c, &reflectedType);
                    }
                    return EmitRuntimeTypeTagIsAliasFromExpr(c, recvNode);
                }
            }
            if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "type_name")) {
                int32_t   extra = AstNextSibling(&c->ast, child);
                H2TypeRef reflectedType;
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
            if (ownerType.containerKind != H2TypeContainer_SCALAR
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
                H2TypeRef recvExprType;
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
                if (BufAppendCStr(&c->out, "__hop_cstr(") != 0
                    || (TypeRefIsStr(&recvType)
                            ? EmitStrValueExpr(c, recvNode, &recvType)
                            : EmitExpr(c, recvNode))
                           != 0
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
                return EmitBuiltinPanicCall(c, recvNode);
            }
            if (recvNode >= 0) {
                H2CCallArgInfo callArgs[H2CCG_MAX_CALL_ARGS];
                int32_t        argNodes[H2CCG_MAX_CALL_ARGS];
                H2TypeRef      argTypes[H2CCG_MAX_CALL_ARGS];
                H2CCallBinding binding;
                uint32_t       argCount = 0;
                uint32_t       i;
                const H2FnSig* resolvedSig = NULL;
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
                    if (status != 0) {
                        const H2NameMap* mappedName = FindNameBySlice(
                            c, callee->dataStart, callee->dataEnd);
                        if (mappedName != NULL && mappedName->cName != NULL) {
                            int mappedStatus = ResolveCallTargetByMappedCName(
                                c,
                                mappedName->cName,
                                callArgs,
                                argNodes,
                                argTypes,
                                argCount,
                                1,
                                0,
                                &binding,
                                &resolvedSig,
                                &resolvedName);
                            if (mappedStatus == 0 && resolvedName != NULL) {
                                return EmitResolvedCall(
                                    c, nodeId, resolvedName, resolvedSig, &binding, 0);
                            }
                            if (mappedStatus == 2) {
                                mappedStatus = ResolveCallTargetByMappedCName(
                                    c,
                                    mappedName->cName,
                                    callArgs,
                                    argNodes,
                                    argTypes,
                                    argCount,
                                    1,
                                    1,
                                    &binding,
                                    &resolvedSig,
                                    &resolvedName);
                                if (mappedStatus == 0 && resolvedName != NULL) {
                                    return EmitResolvedCall(
                                        c, nodeId, resolvedName, resolvedSig, &binding, 1);
                                }
                            }
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
                    {
                        const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
                        uint32_t       candidateLen = 0;
                        const H2FnSig* single = NULL;
                        int            nameFound = 0;
                        GatherCallCandidatesBySlice(
                            c,
                            callee->dataStart,
                            callee->dataEnd,
                            candidates,
                            &candidateLen,
                            &nameFound);
                        if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
                            candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
                        }
                        single = FindSingleTemplateInstanceCandidate(
                            candidates, candidateLen, argCount);
                        if (single != NULL) {
                            BindPositionalTemplateInstanceFallback(
                                single, argNodes, argCount, &binding);
                            return EmitResolvedCall(c, nodeId, single->cName, single, &binding, 0);
                        }
                    }
                }
            }
        }
    }
    if (callee != NULL && callee->kind == H2Ast_IDENT) {
        H2CCallArgInfo callArgs[H2CCG_MAX_CALL_ARGS];
        int32_t        argNodes[H2CCG_MAX_CALL_ARGS];
        H2TypeRef      argTypes[H2CCG_MAX_CALL_ARGS];
        H2CCallBinding binding;
        uint32_t       argCount = 0;
        uint32_t       i;
        const H2FnSig* resolvedSig = NULL;
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
            {
                int dispatchRc = TryEmitRuntimeAnytypeDispatchCallBySlice(
                    c,
                    nodeId,
                    callee->dataStart,
                    callee->dataEnd,
                    callArgs,
                    argNodes,
                    argTypes,
                    argCount,
                    0,
                    status,
                    resolvedSig,
                    &binding);
                if (dispatchRc == 0) {
                    return 0;
                }
                if (dispatchRc < 0) {
                    return -1;
                }
            }
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
                {
                    int dispatchRc = TryEmitRuntimeAnytypeDispatchCallBySlice(
                        c,
                        nodeId,
                        callee->dataStart,
                        callee->dataEnd,
                        callArgs,
                        argNodes,
                        argTypes,
                        argCount,
                        0,
                        status,
                        resolvedSig,
                        &binding);
                    if (dispatchRc == 0) {
                        return 0;
                    }
                    if (dispatchRc < 0) {
                        return -1;
                    }
                }
                if (status == 0 && resolvedName != NULL) {
                    return EmitResolvedCall(c, nodeId, resolvedName, resolvedSig, &binding, 1);
                }
            }
            if (status != 0) {
                const H2NameMap* mappedName = FindNameBySlice(
                    c, callee->dataStart, callee->dataEnd);
                if (mappedName != NULL && mappedName->cName != NULL) {
                    int mappedStatus = ResolveCallTargetByMappedCName(
                        c,
                        mappedName->cName,
                        callArgs,
                        argNodes,
                        argTypes,
                        argCount,
                        0,
                        0,
                        &binding,
                        &resolvedSig,
                        &resolvedName);
                    if (mappedStatus == 0 && resolvedName != NULL) {
                        return EmitResolvedCall(c, nodeId, resolvedName, resolvedSig, &binding, 0);
                    }
                    if (mappedStatus == 2) {
                        mappedStatus = ResolveCallTargetByMappedCName(
                            c,
                            mappedName->cName,
                            callArgs,
                            argNodes,
                            argTypes,
                            argCount,
                            0,
                            1,
                            &binding,
                            &resolvedSig,
                            &resolvedName);
                        if (mappedStatus == 0 && resolvedName != NULL) {
                            return EmitResolvedCall(
                                c, nodeId, resolvedName, resolvedSig, &binding, 1);
                        }
                    }
                }
            }
            {
                const H2FnSig* candidates[H2CCG_MAX_CALL_CANDIDATES];
                uint32_t       candidateLen = 0;
                const H2FnSig* single = NULL;
                GatherCallCandidatesBySlice(
                    c, callee->dataStart, callee->dataEnd, candidates, &candidateLen, &status);
                if (candidateLen > (uint32_t)(sizeof(candidates) / sizeof(candidates[0]))) {
                    candidateLen = (uint32_t)(sizeof(candidates) / sizeof(candidates[0]));
                }
                for (i = 0; i < candidateLen; i++) {
                    const H2FnSig* sig = candidates[i];
                    if (sig == NULL || (sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0
                        || sig->isVariadic != 0 || sig->paramLen != argCount)
                    {
                        continue;
                    }
                    if (single != NULL) {
                        single = NULL;
                        break;
                    }
                    single = sig;
                }
                if (single != NULL) {
                    memset(&binding, 0, sizeof(binding));
                    binding.isVariadic = 0;
                    binding.fixedCount = argCount;
                    binding.fixedInputCount = argCount;
                    binding.spreadArgIndex = UINT32_MAX;
                    for (i = 0; i < argCount; i++) {
                        binding.fixedMappedArgNodes[i] = argNodes[i];
                        binding.argParamIndices[i] = (int32_t)i;
                        binding.argExpectedTypes[i] = single->paramTypes[i];
                    }
                    return EmitResolvedCall(c, nodeId, single->cName, single, &binding, 0);
                }
            }
        }
    }
    {
        const H2FnSig*       sig = NULL;
        const H2FnTypeAlias* typeAlias = NULL;
        uint32_t             argIndex = 0;
        int                  first = 1;
        int                  emittedBuiltinPrintFallback = 0;
        if (callee != NULL && callee->kind == H2Ast_IDENT) {
            sig = FindFnSigBySlice(c, callee->dataStart, callee->dataEnd);
            if (sig == NULL) {
                sig = FindSingleVariadicFnSigBySlice(c, callee->dataStart, callee->dataEnd);
            }
        }
        if (sig == NULL && callee != NULL) {
            H2TypeRef calleeType;
            if (InferExprType(c, AstFirstChild(&c->ast, nodeId), &calleeType) == 0
                && calleeType.valid && calleeType.containerKind == H2TypeContainer_SCALAR
                && calleeType.ptrDepth == 0 && calleeType.containerPtrDepth == 0
                && calleeType.baseName != NULL && !calleeType.isOptional)
            {
                typeAlias = FindFnTypeAliasByName(c, calleeType.baseName);
            }
        }
        if (sig != NULL && sig->isVariadic) {
            H2CCallArgInfo callArgs[H2CCG_MAX_CALL_ARGS];
            int32_t        argNodes[H2CCG_MAX_CALL_ARGS];
            H2TypeRef      argTypes[H2CCG_MAX_CALL_ARGS];
            H2CCallBinding binding;
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
        if ((callee != NULL && callee->kind == H2Ast_IDENT
             && (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "print")
                 || (FindNameBySlice(c, callee->dataStart, callee->dataEnd) != NULL
                     && StrEq(
                         FindNameBySlice(c, callee->dataStart, callee->dataEnd)->cName,
                         "builtin__print"))))
            || (sig != NULL && StrEq(sig->cName, "builtin__print")))
        {
            if (BufAppendCStr(&c->out, "builtin__print(context") != 0) {
                return -1;
            }
            first = 0;
            emittedBuiltinPrintFallback = 1;
        } else if (
            callee != NULL && callee->kind == H2Ast_IDENT
            && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "source_location_of"))
        {
            if (BufAppendCStr(&c->out, "builtin__source_location_of(") != 0) {
                return -1;
            }
        } else if (EmitExpr(c, child) != 0 || BufAppendChar(&c->out, '(') != 0) {
            return -1;
        }
        if (!emittedBuiltinPrintFallback && sig != NULL
            && (sig->hasContext || StrEq(sig->cName, "builtin__print")))
        {
            if (sig->hasContext) {
                if (EmitContextArgForSig(c, sig) != 0) {
                    return -1;
                }
            } else if (BufAppendCStr(&c->out, "context") != 0) {
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

int EmitExpr_NEW(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    (void)n;
    return EmitNewExpr(c, nodeId, NULL, 0);
}

int EmitExpr_INDEX(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t          base = AstFirstChild(&c->ast, nodeId);
    int32_t          idx = AstNextSibling(&c->ast, base);
    const H2AstNode* baseNode = NodeAt(c, base);
    if ((n->flags & H2AstFlag_INDEX_SLICE) != 0) {
        return EmitSliceExpr(c, nodeId);
    }
    if (base < 0 || idx < 0) {
        return -1;
    }
    if (baseNode != NULL && baseNode->kind == H2Ast_IDENT
        && IsActivePackIdent(c, baseNode->dataStart, baseNode->dataEnd))
    {
        uint32_t packIndex = 0;
        if (ResolveActivePackConstIndex(c, idx, &packIndex, NULL) != 0
            || c->activePackElemNames == NULL || packIndex >= c->activePackElemCount
            || c->activePackElemNames[packIndex] == NULL)
        {
            if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                SetDiagNode(c, idx >= 0 ? idx : nodeId, H2Diag_CODEGEN_INTERNAL);
            }
            return -1;
        }
        return BufAppendCStr(&c->out, c->activePackElemNames[packIndex]);
    }
    {
        H2TypeRef baseType;
        if (InferExprType(c, base, &baseType) != 0 || !baseType.valid) {
            return -1;
        }
        if (TypeRefIsStringByteSequence(&baseType)) {
            int writable = TypeRefIsMutableStrPointer(&baseType);
            if (BufAppendCStr(&c->out, "((") != 0
                || BufAppendCStr(&c->out, writable ? "__hop_u8*" : "const __hop_u8*") != 0
                || BufAppendCStr(&c->out, ")(") != 0)
            {
                return -1;
            }
            if (TypeRefIsMutableStrPointer(&baseType)) {
                if (BufAppendCStr(&c->out, "__hop_cstr(*(") != 0 || EmitExpr(c, base) != 0
                    || BufAppendCStr(&c->out, "))") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "__hop_cstr(") != 0
                    || EmitStrValueExpr(c, base, &baseType) != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
            }
            if (BufAppendCStr(&c->out, "))") != 0 || BufAppendChar(&c->out, '[') != 0
                || EmitExpr(c, idx) != 0 || BufAppendChar(&c->out, ']') != 0)
            {
                return -1;
            }
            return 0;
        }
        if (baseType.containerKind == H2TypeContainer_ARRAY
            || baseType.containerKind == H2TypeContainer_SLICE_RO
            || baseType.containerKind == H2TypeContainer_SLICE_MUT)
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

int EmitExpr_FIELD_EXPR(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    const H2NameMap*       enumMap = NULL;
    int32_t                enumDeclNode = -1;
    int                    enumHasPayload = 0;
    uint32_t               enumVariantStart = 0;
    uint32_t               enumVariantEnd = 0;
    int32_t                recv = AstFirstChild(&c->ast, nodeId);
    const H2AstNode*       recvNode = NodeAt(c, recv);
    int32_t                recvLocalIdx = -1;
    const H2VariantNarrow* narrow = NULL;
    H2TypeRef              recvType;
    H2TypeRef              ownerType;
    H2TypeRef              narrowFieldType;
    const H2FieldInfo*     fieldPath[64];
    uint32_t               fieldPathLen = 0;
    const H2FieldInfo*     field = NULL;
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

    if (recvNode != NULL && recvNode->kind == H2Ast_IDENT) {
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

int EmitExpr_CAST(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t   expr = AstFirstChild(&c->ast, nodeId);
    int32_t   typeNode = AstNextSibling(&c->ast, expr);
    int32_t   idxNode = -1;
    int       isConstIndex = 0;
    uint32_t  constIndex = 0;
    H2TypeRef dstType;
    H2TypeRef srcType;
    (void)n;
    if (ParseTypeRef(c, typeNode, &dstType) != 0 || !dstType.valid) {
        return -1;
    }
    if (ResolveActivePackIndexExpr(c, expr, &idxNode, &isConstIndex, &constIndex) == 0
        && !isConstIndex)
    {
        return EmitDynamicActivePackIndexCoerced(c, idxNode, &dstType);
    }
    if (TypeRefIsBorrowedStrValue(&dstType)) {
        if (InferExprType(c, expr, &srcType) == 0 && srcType.valid && TypeRefIsStr(&srcType)) {
            return EmitStrValueExpr(c, expr, &srcType);
        }
        if (BufAppendCStr(&c->out, "(*((__hop_str*)(uintptr_t)(") != 0 || EmitExpr(c, expr) != 0
            || BufAppendCStr(&c->out, ")))") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (InferExprType(c, expr, &srcType) == 0 && srcType.valid && TypeRefIsStr(&srcType)
        && dstType.containerKind == H2TypeContainer_SCALAR && dstType.containerPtrDepth == 0
        && !TypeRefIsStr(&dstType))
    {
        if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeForCast(c, typeNode) != 0
            || BufAppendCStr(&c->out, ")((uintptr_t)__hop_cstr(") != 0
            || EmitStrValueExpr(c, expr, &srcType) != 0 || BufAppendCStr(&c->out, ")))") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeForCast(c, typeNode) != 0
        || BufAppendCStr(&c->out, ")(") != 0 || EmitExpr(c, expr) != 0
        || BufAppendCStr(&c->out, "))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_SIZEOF(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t   inner = AstFirstChild(&c->ast, nodeId);
    H2TypeRef innerType;
    if (inner < 0) {
        return -1;
    }
    if (n->flags == 1) {
        if (BufAppendCStr(&c->out, "(__hop_int)sizeof(") != 0 || EmitTypeForCast(c, inner) != 0
            || BufAppendChar(&c->out, ')') != 0)
        {
            return -1;
        }
        return 0;
    }
    if (InferExprType(c, inner, &innerType) == 0 && innerType.valid) {
        if (innerType.containerKind == H2TypeContainer_SCALAR && innerType.ptrDepth == 1
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
        if (innerType.containerKind == H2TypeContainer_SCALAR && innerType.ptrDepth > 0) {
            H2TypeRef pointeeType = innerType;
            pointeeType.ptrDepth--;
            if (BufAppendCStr(&c->out, "(__hop_int)sizeof(") != 0
                || EmitTypeNameWithDepth(c, &pointeeType) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        if (innerType.containerKind == H2TypeContainer_ARRAY && innerType.hasArrayLen
            && innerType.containerPtrDepth == 1 && SliceStructPtrDepth(&innerType) == 0)
        {
            if (BufAppendCStr(&c->out, "((__hop_int)(") != 0
                || BufAppendU32(&c->out, innerType.arrayLen) != 0
                || BufAppendCStr(&c->out, ") * (__hop_int)sizeof(") != 0
                || EmitElementTypeName(c, &innerType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
        if ((innerType.containerKind == H2TypeContainer_SLICE_RO
             || innerType.containerKind == H2TypeContainer_SLICE_MUT)
            && innerType.containerPtrDepth == 1 && SliceStructPtrDepth(&innerType) == 0)
        {
            if (BufAppendCStr(&c->out, "((__hop_int)(") != 0
                || EmitLenExprFromType(c, inner, &innerType) != 0
                || BufAppendCStr(&c->out, ") * (__hop_int)sizeof(") != 0
                || EmitElementTypeName(c, &innerType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (BufAppendCStr(&c->out, "(__hop_int)sizeof(") != 0 || EmitExpr(c, inner) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_NULL(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    (void)nodeId;
    (void)n;
    return BufAppendCStr(&c->out, "NULL");
}

int EmitExpr_UNWRAP(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t   inner = AstFirstChild(&c->ast, nodeId);
    H2TypeRef innerType;
    (void)n;
    if (inner < 0) {
        return -1;
    }
    if (InferExprType(c, inner, &innerType) != 0 || !innerType.valid || !innerType.isOptional) {
        return -1;
    }
    if (TypeRefIsTaggedOptional(&innerType)) {
        if (BufAppendCStr(&c->out, "(__extension__({ __auto_type __hop_opt = ") != 0
            || EmitExpr(c, inner) != 0
            || BufAppendCStr(
                   &c->out,
                   "; if (__hop_opt.__hop_tag == 0u) { "
                   "__hop_panic(__hop_strlit(\"unwrap: null value\"), __FILE__, __LINE__); } "
                   "__hop_opt.__hop_value; }))")
                   != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "__hop_unwrap(") != 0 || EmitExpr(c, inner) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EmitExpr_CALL_ARG(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    int32_t inner = AstFirstChild(&c->ast, nodeId);
    (void)n;
    if (inner < 0) {
        return -1;
    }
    return EmitExpr(c, inner);
}

int EmitExpr_TUPLE_EXPR(H2CBackendC* c, int32_t nodeId, const H2AstNode* n) {
    H2TypeRef             tupleType;
    const H2AnonTypeInfo* tupleInfo = NULL;
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
        const H2FieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
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

int EmitExpr(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    int              rc;
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case H2Ast_IDENT:             rc = EmitExpr_IDENT(c, nodeId, n); break;
        case H2Ast_INT:               rc = EmitExpr_INT(c, nodeId, n); break;
        case H2Ast_RUNE:              rc = EmitExpr_RUNE(c, nodeId, n); break;
        case H2Ast_FLOAT:             rc = EmitExpr_FLOAT(c, nodeId, n); break;
        case H2Ast_BOOL:              rc = EmitExpr_BOOL(c, nodeId, n); break;
        case H2Ast_COMPOUND_LIT:      rc = EmitExpr_COMPOUND_LIT(c, nodeId, n); break;
        case H2Ast_ARRAY_LIT:         rc = EmitExpr_ARRAY_LIT(c, nodeId, n); break;
        case H2Ast_STRING:            rc = EmitExpr_STRING(c, nodeId, n); break;
        case H2Ast_UNARY:             rc = EmitExpr_UNARY(c, nodeId, n); break;
        case H2Ast_BINARY:            rc = EmitExpr_BINARY(c, nodeId, n); break;
        case H2Ast_CALL_WITH_CONTEXT: rc = EmitExpr_CALL_WITH_CONTEXT(c, nodeId, n); break;
        case H2Ast_CALL:              rc = EmitExpr_CALL(c, nodeId, n); break;
        case H2Ast_NEW:               rc = EmitExpr_NEW(c, nodeId, n); break;
        case H2Ast_INDEX:             rc = EmitExpr_INDEX(c, nodeId, n); break;
        case H2Ast_FIELD_EXPR:        rc = EmitExpr_FIELD_EXPR(c, nodeId, n); break;
        case H2Ast_CAST:              rc = EmitExpr_CAST(c, nodeId, n); break;
        case H2Ast_SIZEOF:            rc = EmitExpr_SIZEOF(c, nodeId, n); break;
        case H2Ast_NULL:              rc = EmitExpr_NULL(c, nodeId, n); break;
        case H2Ast_UNWRAP:            rc = EmitExpr_UNWRAP(c, nodeId, n); break;
        case H2Ast_TUPLE_EXPR:        rc = EmitExpr_TUPLE_EXPR(c, nodeId, n); break;
        case H2Ast_CALL_ARG:          rc = EmitExpr_CALL_ARG(c, nodeId, n); break;
        case H2Ast_TYPE_VALUE:        {
            H2TypeRef reflectedType;
            if (!ResolveReflectedTypeValueExprTypeRef(c, nodeId, &reflectedType)) {
                rc = -1;
                break;
            }
            rc = EmitTypeTagLiteralFromTypeRef(c, &reflectedType);
            break;
        }
        default:
            /* Unsupported AST kind in expression context. */
            rc = -1;
            break;
    }
    if (rc != 0 && c->diag != NULL && c->diag->code == H2Diag_NONE) {
        SetDiagNode(c, nodeId, H2Diag_CODEGEN_INTERNAL);
    }
    return rc;
}

int EmitStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth);

int EmitDeferredRange(H2CBackendC* c, uint32_t start, uint32_t depth) {
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

static int ExprBaseIsContext(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_IDENT) {
        return SliceEq(c->unit->source, n->dataStart, n->dataEnd, "context");
    }
    if (n->kind == H2Ast_FIELD_EXPR || n->kind == H2Ast_INDEX || n->kind == H2Ast_CAST) {
        return ExprBaseIsContext(c, AstFirstChild(&c->ast, nodeId));
    }
    if (n->kind == H2Ast_UNARY && n->op == H2Tok_MUL) {
        return ExprBaseIsContext(c, AstFirstChild(&c->ast, nodeId));
    }
    return 0;
}

static int ExprStmtAssignsContext(H2CBackendC* c, int32_t exprNode) {
    const H2AstNode* n = NodeAt(c, exprNode);
    int32_t          lhs;
    H2TokenKind      op;
    if (n == NULL || n->kind != H2Ast_BINARY) {
        return 0;
    }
    op = (H2TokenKind)n->op;
    if (op != H2Tok_ASSIGN && op != H2Tok_ADD_ASSIGN && op != H2Tok_SUB_ASSIGN
        && op != H2Tok_MUL_ASSIGN && op != H2Tok_DIV_ASSIGN && op != H2Tok_MOD_ASSIGN
        && op != H2Tok_AND_ASSIGN && op != H2Tok_OR_ASSIGN && op != H2Tok_XOR_ASSIGN
        && op != H2Tok_LSHIFT_ASSIGN && op != H2Tok_RSHIFT_ASSIGN)
    {
        return 0;
    }
    lhs = AstFirstChild(&c->ast, exprNode);
    return ExprBaseIsContext(c, lhs);
}

static int EnsureContextCow(H2CBackendC* c, uint32_t depth) {
    uint32_t scopeIdx;
    uint32_t tempId;
    if (!c->hasCurrentContext || c->localScopeLen == 0) {
        return 0;
    }
    scopeIdx = c->localScopeLen - 1u;
    if (c->contextCowScopeActive != NULL && c->contextCowScopeActive[scopeIdx]) {
        return 0;
    }
    tempId = ++c->fmtTempCounter;
    c->contextCowScopeActive[scopeIdx] = 1;
    c->contextCowScopeTempIds[scopeIdx] = tempId;
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "__typeof__(context) __hop_context_parent") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = context;\n") != 0)
    {
        return -1;
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "__typeof__(*context) __hop_context_copy") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = *context;\n") != 0)
    {
        return -1;
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "context = &__hop_context_copy") != 0
        || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
    {
        return -1;
    }
    return 0;
}

static int RestoreContextCow(H2CBackendC* c, uint32_t depth) {
    uint32_t scopeIdx;
    uint32_t tempId;
    if (c->localScopeLen == 0 || c->contextCowScopeActive == NULL) {
        return 0;
    }
    scopeIdx = c->localScopeLen - 1u;
    if (!c->contextCowScopeActive[scopeIdx]) {
        return 0;
    }
    tempId = c->contextCowScopeTempIds[scopeIdx];
    EmitIndent(c, depth);
    return BufAppendCStr(&c->out, "context = __hop_context_parent") != 0
                || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, ";\n") != 0
             ? -1
             : 0;
}

int EmitBlockImpl(H2CBackendC* c, int32_t nodeId, uint32_t depth, int inlineOpen) {
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
            if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                SetDiagNode(c, child, H2Diag_CODEGEN_INTERNAL);
            }
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
    if (RestoreContextCow(c, depth + 1u) != 0) {
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

int EmitBlock(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    return EmitBlockImpl(c, nodeId, depth, 0);
}

int EmitBlockInline(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    return EmitBlockImpl(c, nodeId, depth, 1);
}

int EmitVarLikeStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth, int isConst) {
    const H2AstNode*  n = NodeAt(c, nodeId);
    H2CCGVarLikeParts parts;
    H2TypeRef         sharedType;
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
        H2TypeRef type;
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
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, initNode, H2Diag_CODEGEN_INTERNAL);
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
        H2TypeRef             tupleInitType;
        const H2AnonTypeInfo* tupleInfo = NULL;
        if (parts.initNode >= 0) {
            if (NodeAt(c, parts.initNode) == NULL
                || NodeAt(c, parts.initNode)->kind != H2Ast_EXPR_LIST)
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
            if (BufAppendCStr(&c->out, "__auto_type __hop_tmp_tuple = ") != 0
                || EmitExprCoerced(c, tupleInitNode, &tupleInitType) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
            for (i = 0; i < parts.nameCount; i++) {
                int32_t            nameNode = ListItemAt(&c->ast, parts.nameListNode, i);
                const H2AstNode*   nameAst = NodeAt(c, nameNode);
                const H2FieldInfo* tf = &c->fieldInfos[tupleInfo->fieldStart + i];
                char*              name;
                int                isHole;
                H2TypeRef          type;
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
                    if (BufAppendCStr(&c->out, "(void)(__hop_tmp_tuple.") != 0
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
                if (BufAppendCStr(&c->out, " = __hop_tmp_tuple.") != 0
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
            const H2AstNode* nameAst = NodeAt(c, nameNode);
            int32_t          initNode = -1;
            char*            name;
            int              isHole;
            H2TypeRef        type;
            if (nameAst == NULL) {
                return -1;
            }
            name = DupSlice(c, c->unit->source, nameAst->dataStart, nameAst->dataEnd);
            if (name == NULL) {
                return -1;
            }
            isHole = StrEq(name, "_");
            if (parts.initNode >= 0 && NodeAt(c, parts.initNode) != NULL
                && NodeAt(c, parts.initNode)->kind == H2Ast_EXPR_LIST)
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
                            if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                                SetDiagNode(c, initNode, H2Diag_CODEGEN_INTERNAL);
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
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, initNode, H2Diag_CODEGEN_INTERNAL);
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

int EmitMultiAssignStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    int32_t  lhsList = AstFirstChild(&c->ast, nodeId);
    int32_t  rhsList = lhsList >= 0 ? AstNextSibling(&c->ast, lhsList) : -1;
    uint32_t lhsCount;
    uint32_t rhsCount;
    uint32_t i;
    if (lhsList < 0 || rhsList < 0 || NodeAt(c, lhsList) == NULL || NodeAt(c, rhsList) == NULL
        || NodeAt(c, lhsList)->kind != H2Ast_EXPR_LIST
        || NodeAt(c, rhsList)->kind != H2Ast_EXPR_LIST)
    {
        return -1;
    }
    lhsCount = ListCount(&c->ast, lhsList);
    rhsCount = ListCount(&c->ast, rhsList);
    for (i = 0; i < lhsCount; i++) {
        if (ExprBaseIsContext(c, ListItemAt(&c->ast, lhsList, i))
            && EnsureContextCow(c, depth) != 0)
        {
            return -1;
        }
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "{\n") != 0) {
        return -1;
    }
    if (rhsCount == lhsCount) {
        for (i = 0; i < lhsCount; i++) {
            int32_t   rhsNode = ListItemAt(&c->ast, rhsList, i);
            H2TypeRef rhsType;
            if (rhsNode < 0 || InferExprType(c, rhsNode, &rhsType) != 0 || !rhsType.valid) {
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "__auto_type __hop_tmp_") != 0
                || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, " = ") != 0
                || EmitExprCoerced(c, rhsNode, &rhsType) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
    } else if (rhsCount == 1u) {
        int32_t               rhsNode = ListItemAt(&c->ast, rhsList, 0);
        H2TypeRef             rhsType;
        const H2AnonTypeInfo* tupleInfo = NULL;
        if (rhsNode < 0 || InferExprType(c, rhsNode, &rhsType) != 0 || !rhsType.valid
            || !TypeRefTupleInfo(c, &rhsType, &tupleInfo) || tupleInfo->fieldCount != lhsCount)
        {
            return -1;
        }
        EmitIndent(c, depth + 1u);
        if (BufAppendCStr(&c->out, "__auto_type __hop_tmp_tuple = ") != 0
            || EmitExprCoerced(c, rhsNode, &rhsType) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
        for (i = 0; i < lhsCount; i++) {
            const H2FieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "__auto_type __hop_tmp_") != 0
                || BufAppendU32(&c->out, i) != 0
                || BufAppendCStr(&c->out, " = __hop_tmp_tuple.") != 0
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
        const H2AstNode* lhs = NodeAt(c, lhsNode);
        if (lhs == NULL) {
            return -1;
        }
        if (lhs->kind == H2Ast_IDENT
            && SliceEqName(c->unit->source, lhs->dataStart, lhs->dataEnd, "_"))
        {
            continue;
        }
        EmitIndent(c, depth + 1u);
        if (EmitExpr(c, lhsNode) != 0 || BufAppendCStr(&c->out, " = __hop_tmp_") != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
    }
    EmitIndent(c, depth);
    return BufAppendCStr(&c->out, "}\n");
}

int EmitShortAssignStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    int32_t   nameList = AstFirstChild(&c->ast, nodeId);
    int32_t   rhsList = nameList >= 0 ? AstNextSibling(&c->ast, nameList) : -1;
    uint32_t  nameCount;
    uint32_t  rhsCount;
    uint32_t  i;
    uint32_t  tempId;
    int32_t   existingLocal[256];
    int32_t   nameNodes[256];
    uint8_t   isBlank[256];
    H2TypeRef rhsTypes[256];
    if (nameList < 0 || rhsList < 0 || NodeAt(c, nameList) == NULL || NodeAt(c, rhsList) == NULL
        || NodeAt(c, nameList)->kind != H2Ast_NAME_LIST
        || NodeAt(c, rhsList)->kind != H2Ast_EXPR_LIST)
    {
        return -1;
    }
    nameCount = ListCount(&c->ast, nameList);
    rhsCount = ListCount(&c->ast, rhsList);
    if (nameCount == 0u || nameCount > 256u || (rhsCount != nameCount && rhsCount != 1u)) {
        return -1;
    }
    tempId = ++c->fmtTempCounter;
    if (tempId == 0u) {
        tempId = ++c->fmtTempCounter;
    }
    for (i = 0; i < nameCount; i++) {
        const H2AstNode* name;
        nameNodes[i] = ListItemAt(&c->ast, nameList, i);
        name = NodeAt(c, nameNodes[i]);
        if (name == NULL || name->kind != H2Ast_IDENT) {
            return -1;
        }
        isBlank[i] = (uint8_t)SliceEqName(c->unit->source, name->dataStart, name->dataEnd, "_");
        existingLocal[i] =
            isBlank[i] ? -1 : FindLocalIndexBySlice(c, name->dataStart, name->dataEnd);
    }

    if (nameCount == 1u && rhsCount == 1u && existingLocal[0] < 0 && !isBlank[0]) {
        int32_t          rhsNode = ListItemAt(&c->ast, rhsList, 0);
        const H2AstNode* rhs = NodeAt(c, rhsNode);
        H2TypeRef        directType;
        H2TypeRef        directElemType;
        if (rhs != NULL && rhs->kind == H2Ast_ARRAY_LIT
            && InferVarLikeDeclType(c, rhsNode, &directType) == 0 && directType.valid
            && directType.containerKind == H2TypeContainer_ARRAY)
        {
            const H2AstNode* name = NodeAt(c, nameNodes[0]);
            char* localName = DupSlice(c, c->unit->source, name->dataStart, name->dataEnd);
            directElemType = directType;
            directElemType.containerKind = H2TypeContainer_SCALAR;
            directElemType.containerPtrDepth = 0;
            directElemType.hasArrayLen = 0;
            directElemType.arrayLen = 0;
            if (!TypeRefIsStr(&directElemType)) {
                goto skip_direct_array_decl;
            }
            directElemType.baseName = "__hop_str";
            if (localName == NULL || EnsureAnonTypeVisible(c, &directType, depth) != 0) {
                return -1;
            }
            EmitIndent(c, depth);
            if (EmitTypeRefWithName(c, &directType, localName) != 0
                || BufAppendCStr(&c->out, " = ") != 0
                || EmitFixedStrArrayLiteralInitializer(c, rhsNode, &directElemType) != 0
                || BufAppendCStr(&c->out, ";\n") != 0 || AddLocal(c, localName, directType) != 0)
            {
                return -1;
            }
            return 0;
        }
    }
skip_direct_array_decl:

    if (rhsCount == nameCount) {
        for (i = 0; i < nameCount; i++) {
            int32_t rhsNode = ListItemAt(&c->ast, rhsList, i);
            if (rhsNode < 0 || InferExprType(c, rhsNode, &rhsTypes[i]) != 0 || !rhsTypes[i].valid) {
                return -1;
            }
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "__auto_type __hop_short_tmp") != 0
                || BufAppendU32(&c->out, tempId) != 0 || BufAppendChar(&c->out, '_') != 0
                || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, " = ") != 0
                || EmitExprCoerced(c, rhsNode, &rhsTypes[i]) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
            if (existingLocal[i] < 0 && !isBlank[i]
                && InferVarLikeDeclType(c, rhsNode, &rhsTypes[i]) != 0)
            {
                return -1;
            }
        }
    } else {
        int32_t               rhsNode = ListItemAt(&c->ast, rhsList, 0);
        H2TypeRef             tupleType;
        const H2AnonTypeInfo* tupleInfo = NULL;
        if (rhsNode < 0 || InferExprType(c, rhsNode, &tupleType) != 0 || !tupleType.valid
            || !TypeRefTupleInfo(c, &tupleType, &tupleInfo) || tupleInfo->fieldCount != nameCount)
        {
            return -1;
        }
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "__auto_type __hop_short_tuple") != 0
            || BufAppendU32(&c->out, tempId) != 0 || BufAppendCStr(&c->out, " = ") != 0
            || EmitExprCoerced(c, rhsNode, &tupleType) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
        for (i = 0; i < nameCount; i++) {
            const H2FieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
            rhsTypes[i] = f->type;
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "__auto_type __hop_short_tmp") != 0
                || BufAppendU32(&c->out, tempId) != 0 || BufAppendChar(&c->out, '_') != 0
                || BufAppendU32(&c->out, i) != 0
                || BufAppendCStr(&c->out, " = __hop_short_tuple") != 0
                || BufAppendU32(&c->out, tempId) != 0 || BufAppendChar(&c->out, '.') != 0
                || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
    }

    for (i = 0; i < nameCount; i++) {
        const H2AstNode* name = NodeAt(c, nameNodes[i]);
        char*            localName;
        if (name == NULL) {
            return -1;
        }
        if (isBlank[i]) {
            continue;
        }
        if (existingLocal[i] >= 0) {
            EmitIndent(c, depth);
            if (AppendMappedIdentifier(c, name->dataStart, name->dataEnd) != 0
                || BufAppendCStr(&c->out, " = __hop_short_tmp") != 0
                || BufAppendU32(&c->out, tempId) != 0 || BufAppendChar(&c->out, '_') != 0
                || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
            continue;
        }
        localName = DupSlice(c, c->unit->source, name->dataStart, name->dataEnd);
        if (localName == NULL || EnsureAnonTypeVisible(c, &rhsTypes[i], depth) != 0) {
            return -1;
        }
        EmitIndent(c, depth);
        if (EmitTypeRefWithName(c, &rhsTypes[i], localName) != 0
            || BufAppendCStr(&c->out, " = __hop_short_tmp") != 0
            || BufAppendU32(&c->out, tempId) != 0 || BufAppendChar(&c->out, '_') != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, ";\n") != 0
            || AddLocal(c, localName, rhsTypes[i]) != 0)
        {
            return -1;
        }
    }
    return 0;
}

int EmitForStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    const H2AstNode* forNode = NodeAt(c, nodeId);
    int32_t          nodes[4];
    int              count = 0;
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          body;
    const H2AstNode* bodyNode;
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

    if (forNode != NULL && (forNode->flags & H2AstFlag_FOR_IN) != 0) {
        int            hasKey = (forNode->flags & H2AstFlag_FOR_IN_HAS_KEY) != 0;
        int            keyRef = (forNode->flags & H2AstFlag_FOR_IN_KEY_REF) != 0;
        int            valueRef = (forNode->flags & H2AstFlag_FOR_IN_VALUE_REF) != 0;
        int            valueDiscard = (forNode->flags & H2AstFlag_FOR_IN_VALUE_DISCARD) != 0;
        int32_t        keyNode = -1;
        int32_t        valueNode = -1;
        int32_t        sourceNode = -1;
        int32_t        loopBodyNode = -1;
        H2TypeRef      sourceType;
        int            useSequencePath = 0;
        H2TypeRef      elemType;
        H2TypeRef      valueLocalType;
        H2TypeRef      keyType;
        H2TypeRef      iterType;
        H2TypeRef      iterPtrType;
        H2TypeRef      nextValueLocalType;
        H2TypeRef      nextValueOptionalType;
        H2TypeRef      nextKeyLocalType;
        H2TypeRef      nextKeyOptionalType;
        H2TypeRef      nextPairKeyLocalType;
        H2TypeRef      nextPairValueLocalType;
        H2TypeRef      nextPairOptionalType;
        const H2FnSig* iteratorSig = NULL;
        const H2FnSig* nextValueSig = NULL;
        const H2FnSig* nextKeySig = NULL;
        const H2FnSig* nextPairSig = NULL;
        const char*    iteratorCallee = NULL;
        int            iteratorAutoRefSource = 0;
        const char*    nextValueCallee = NULL;
        const char*    nextKeyCallee = NULL;
        const char*    nextPairCallee = NULL;
        int            useNextPair = 0;
        int            useNextKey = 0;
        int            pairValueNeedsDeref = 0;
        int            nextValuePayloadIsIndirect = 0;
        int            nextValueOptionalTagged = 0;
        int            pairPayloadIsIndirect = 0;
        int            pairOptionalTagged = 0;
        const char*    pairKeyFieldName = NULL;
        const char*    pairValueFieldName = NULL;
        int            elemMutable = 0;
        const char*    seqTmpName = "__hop_forin_seq";
        const char*    idxTmpName = "__hop_forin_idx";
        const char*    sourceTmpName = "__hop_forin_src";
        const char*    iterTmpName = "__hop_forin_it";
        const char*    nextTmpName = "__hop_forin_next";
        char*          valueName = NULL;
        char*          keyName = NULL;
        int            useArraySeqPtr = 0;
        int            rc;

        TypeRefSetInvalid(&elemType);
        TypeRefSetInvalid(&valueLocalType);
        TypeRefSetInvalid(&keyType);
        TypeRefSetInvalid(&iterType);
        TypeRefSetInvalid(&iterPtrType);
        TypeRefSetInvalid(&nextValueLocalType);
        TypeRefSetInvalid(&nextValueOptionalType);
        TypeRefSetInvalid(&nextKeyLocalType);
        TypeRefSetInvalid(&nextKeyOptionalType);
        TypeRefSetInvalid(&nextPairKeyLocalType);
        TypeRefSetInvalid(&nextPairValueLocalType);
        TypeRefSetInvalid(&nextPairOptionalType);
        if ((!hasKey && count != 3) || (hasKey && count != 4)) {
            return -1;
        }
        if (hasKey) {
            keyNode = nodes[0];
            valueNode = nodes[1];
            sourceNode = nodes[2];
            loopBodyNode = nodes[3];
        } else {
            valueNode = nodes[0];
            sourceNode = nodes[1];
            loopBodyNode = nodes[2];
        }
        if (loopBodyNode < 0 || NodeAt(c, loopBodyNode) == NULL
            || NodeAt(c, loopBodyNode)->kind != H2Ast_BLOCK)
        {
            return -1;
        }
        if (InferExprType(c, sourceNode, &sourceType) != 0 || !sourceType.valid) {
            SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
            return -1;
        }
        if (ResolveForInElemType(&sourceType, &elemType, &elemMutable) == 0) {
            useSequencePath = 1;
            valueLocalType = elemType;
            if (valueRef) {
                if (valueLocalType.containerKind == H2TypeContainer_SCALAR) {
                    valueLocalType.ptrDepth++;
                } else {
                    valueLocalType.containerPtrDepth++;
                }
                valueLocalType.readOnly = elemMutable ? 0 : 1;
            }
            TypeRefSetScalar(&keyType, "__hop_int");
        } else {
            H2CCGForInValueMode   valueMode = H2CCGForInValueMode_VALUE;
            H2TypeRef             payloadType;
            H2TypeRef             pairPayloadType;
            H2TypeRef             pairType;
            H2TypeRef             expandedPairType;
            const H2AnonTypeInfo* pairTupleInfo = NULL;
            rc = ResolveForInIteratorSig(
                c, &sourceType, &iteratorSig, &iteratorCallee, &iterType, &iteratorAutoRefSource);
            if (rc == 1 || rc == 2) {
                SetDiagNode(c, sourceNode, H2Diag_FOR_IN_INVALID_SOURCE);
                return -1;
            }
            if (rc == 3) {
                SetDiagNode(c, sourceNode, H2Diag_FOR_IN_ITERATOR_AMBIGUOUS);
                return -1;
            }
            if (rc != 0 || iteratorSig == NULL || iteratorCallee == NULL || !iterType.valid) {
                SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                return -1;
            }
            iterPtrType = iterType;
            if (iterPtrType.containerKind == H2TypeContainer_SCALAR) {
                iterPtrType.ptrDepth++;
            } else {
                iterPtrType.containerPtrDepth++;
            }
            if (keyRef) {
                SetDiagNode(c, keyNode, H2Diag_FOR_IN_KEY_REF_INVALID);
                return -1;
            }
            if (valueRef) {
                valueMode = H2CCGForInValueMode_REF;
            }
            if (hasKey && valueDiscard) {
                rc = ResolveForInNextKeySig(
                    c,
                    &iterPtrType,
                    &nextKeySig,
                    &nextKeyCallee,
                    &nextKeyLocalType,
                    &nextKeyOptionalType);
                if (rc == 1 || rc == 2) {
                    rc = ResolveForInNextKeyAndValueSig(
                        c,
                        &iterPtrType,
                        H2CCGForInValueMode_ANY,
                        &nextPairSig,
                        &nextPairCallee,
                        &nextPairKeyLocalType,
                        &nextPairValueLocalType,
                        &nextPairOptionalType);
                    if (rc == 0) {
                        useNextPair = 1;
                    }
                }
                if (rc == 4) {
                    SetDiagNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NON_BOOL);
                    return -1;
                }
                if (rc == 1 || rc == 2 || rc == 3) {
                    SetDiagNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                    return -1;
                }
                if (!useNextPair) {
                    if (nextKeySig == NULL || nextKeyCallee == NULL || !nextKeyLocalType.valid
                        || !nextKeyOptionalType.valid)
                    {
                        SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                        return -1;
                    }
                    useNextKey = 1;
                    keyType = nextKeyLocalType;
                    nextValueOptionalType = nextKeyOptionalType;
                    nextValueSig = nextKeySig;
                    nextValueCallee = nextKeyCallee;
                }
            } else if (hasKey) {
                rc = ResolveForInNextKeyAndValueSig(
                    c,
                    &iterPtrType,
                    valueDiscard ? H2CCGForInValueMode_ANY : valueMode,
                    &nextPairSig,
                    &nextPairCallee,
                    &nextPairKeyLocalType,
                    &nextPairValueLocalType,
                    &nextPairOptionalType);
                if (rc == 4) {
                    SetDiagNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NON_BOOL);
                    return -1;
                }
                if (rc == 1 || rc == 2 || rc == 3) {
                    SetDiagNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                    return -1;
                }
                if (rc != 0 || nextPairSig == NULL || nextPairCallee == NULL
                    || !nextPairOptionalType.valid)
                {
                    SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                useNextPair = 1;
            } else {
                rc = ResolveForInNextValueSig(
                    c,
                    &iterPtrType,
                    valueDiscard ? H2CCGForInValueMode_ANY : valueMode,
                    &nextValueSig,
                    &nextValueCallee,
                    &nextValueLocalType);
                if (rc == 1 || rc == 2) {
                    rc = ResolveForInNextKeyAndValueSig(
                        c,
                        &iterPtrType,
                        valueDiscard ? H2CCGForInValueMode_ANY : valueMode,
                        &nextPairSig,
                        &nextPairCallee,
                        &nextPairKeyLocalType,
                        &nextPairValueLocalType,
                        &nextPairOptionalType);
                    if (rc == 0) {
                        useNextPair = 1;
                    }
                }
                if (rc == 4) {
                    SetDiagNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NON_BOOL);
                    return -1;
                }
                if (rc == 1 || rc == 2 || rc == 3) {
                    SetDiagNode(c, sourceNode, H2Diag_FOR_IN_ADVANCE_NO_MATCHING_OVERLOAD);
                    return -1;
                }
                if (!useNextPair) {
                    if (nextValueSig == NULL || nextValueCallee == NULL
                        || !nextValueSig->returnType.valid)
                    {
                        SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                        return -1;
                    }
                    nextValueOptionalType = nextValueSig->returnType;
                    if (!valueDiscard) {
                        valueLocalType = nextValueLocalType;
                    }
                }
            }

            if (useNextPair) {
                if (nextPairSig == NULL || nextPairCallee == NULL || !nextPairOptionalType.valid) {
                    SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                nextValueOptionalType = nextPairOptionalType;
                nextValueSig = nextPairSig;
                nextValueCallee = nextPairCallee;
                if (!ForInPayloadTypeFromOptional(&nextPairSig->returnType, &pairPayloadType)) {
                    SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                pairPayloadIsIndirect =
                    ForInTypeRefIsRef(&pairPayloadType) || ForInTypeRefIsPtr(&pairPayloadType);
                pairOptionalTagged = TypeRefIsTaggedOptional(&nextValueOptionalType);
                pairType = pairPayloadType;
                if ((ForInTypeRefIsRef(&pairType) || ForInTypeRefIsPtr(&pairType))
                    && ForInTypeRefDerefOne(&pairType, &pairType) != 0)
                {
                    SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                if (ExpandAliasSourceType(c, &pairType, &expandedPairType)) {
                    pairType = expandedPairType;
                }
                if (!TypeRefTupleInfo(c, &pairType, &pairTupleInfo) || pairTupleInfo == NULL
                    || pairTupleInfo->fieldCount != 2u)
                {
                    SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                pairKeyFieldName = c->fieldInfos[pairTupleInfo->fieldStart].fieldName;
                pairValueFieldName = c->fieldInfos[pairTupleInfo->fieldStart + 1u].fieldName;
                if (pairKeyFieldName == NULL || pairValueFieldName == NULL) {
                    SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                keyType = nextPairKeyLocalType;
                if (!valueDiscard) {
                    valueLocalType = nextPairValueLocalType;
                    {
                        H2TypeRef pairValueFieldType =
                            c->fieldInfos[pairTupleInfo->fieldStart + 1u].type;
                        pairValueNeedsDeref =
                            (!valueRef
                             && (ForInTypeRefIsRef(&pairValueFieldType)
                                 || ForInTypeRefIsPtr(&pairValueFieldType)));
                    }
                }
            } else if (!useNextKey) {
                if (!ForInPayloadTypeFromOptional(&nextValueSig->returnType, &payloadType)) {
                    SetDiagNode(c, sourceNode, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                nextValueOptionalType = nextValueSig->returnType;
                nextValuePayloadIsIndirect =
                    ForInTypeRefIsRef(&payloadType) || ForInTypeRefIsPtr(&payloadType);
                nextValueOptionalTagged = TypeRefIsTaggedOptional(&nextValueOptionalType);
                if (!valueDiscard) {
                    valueLocalType = nextValueLocalType;
                }
            }
        }

        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "{\n") != 0) {
            return -1;
        }
        if (PushScope(c) != 0) {
            return -1;
        }

        useArraySeqPtr = useSequencePath && sourceType.containerKind == H2TypeContainer_ARRAY
                      && sourceType.containerPtrDepth == 0;
        if (EnsureAnonTypeVisible(c, &sourceType, depth + 1u) != 0) {
            PopScope(c);
            return -1;
        }
        {
            H2TypeRef sourceTmpType = sourceType;
            if (useArraySeqPtr) {
                sourceTmpType.containerPtrDepth = 1;
            }
            EmitIndent(c, depth + 1u);
            if (EmitTypeRefWithName(c, &sourceTmpType, useSequencePath ? seqTmpName : sourceTmpName)
                    != 0
                || BufAppendCStr(&c->out, " = ") != 0 || EmitExpr(c, sourceNode) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                PopScope(c);
                return -1;
            }
        }

        if (useSequencePath) {
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "for (__hop_int ") != 0
                || BufAppendCStr(&c->out, idxTmpName) != 0 || BufAppendCStr(&c->out, " = 0; ") != 0
                || BufAppendCStr(&c->out, idxTmpName) != 0 || BufAppendCStr(&c->out, " < ") != 0
                || EmitLenExprFromNameType(c, seqTmpName, &sourceType) != 0
                || BufAppendCStr(&c->out, "; ") != 0 || BufAppendCStr(&c->out, idxTmpName) != 0
                || BufAppendCStr(&c->out, " += 1) {\n") != 0)
            {
                PopScope(c);
                return -1;
            }

            if (PushScope(c) != 0) {
                PopScope(c);
                return -1;
            }

            if (hasKey) {
                const H2AstNode* keyNameNode = NodeAt(c, keyNode);
                keyName = DupSlice(
                    c, c->unit->source, keyNameNode->dataStart, keyNameNode->dataEnd);
                if (keyName == NULL) {
                    PopScope(c);
                    PopScope(c);
                    return -1;
                }
                EmitIndent(c, depth + 2u);
                if (EmitTypeRefWithName(c, &keyType, keyName) != 0
                    || BufAppendCStr(&c->out, " = ") != 0 || BufAppendCStr(&c->out, idxTmpName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0 || AddLocal(c, keyName, keyType) != 0)
                {
                    PopScope(c);
                    PopScope(c);
                    return -1;
                }
            }

            if (!valueDiscard) {
                const H2AstNode* valueNameNode = NodeAt(c, valueNode);
                valueName = DupSlice(
                    c, c->unit->source, valueNameNode->dataStart, valueNameNode->dataEnd);
                if (valueName == NULL) {
                    PopScope(c);
                    PopScope(c);
                    return -1;
                }
                if (EnsureAnonTypeVisible(c, &valueLocalType, depth + 2u) != 0) {
                    PopScope(c);
                    PopScope(c);
                    return -1;
                }
                EmitIndent(c, depth + 2u);
                if (EmitTypeRefWithName(c, &valueLocalType, valueName) != 0
                    || BufAppendCStr(&c->out, " = ") != 0)
                {
                    PopScope(c);
                    PopScope(c);
                    return -1;
                }
                if (valueRef) {
                    if (BufAppendChar(&c->out, '&') != 0) {
                        PopScope(c);
                        PopScope(c);
                        return -1;
                    }
                }
                if (EmitForInElemExprFromNameType(c, seqTmpName, idxTmpName, &sourceType) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0
                    || AddLocal(c, valueName, valueLocalType) != 0)
                {
                    PopScope(c);
                    PopScope(c);
                    return -1;
                }
            }

            if (EmitStmt(c, loopBodyNode, depth + 2u) != 0) {
                PopScope(c);
                PopScope(c);
                return -1;
            }

            PopScope(c);
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "}\n") != 0) {
                PopScope(c);
                return -1;
            }
            PopScope(c);
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "}\n");
        } else {
            if (EnsureAnonTypeVisible(c, &iterType, depth + 1u) != 0) {
                PopScope(c);
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (EmitTypeRefWithName(c, &iterType, iterTmpName) != 0
                || BufAppendCStr(&c->out, " = ") != 0 || BufAppendCStr(&c->out, iteratorCallee) != 0
                || BufAppendChar(&c->out, '(') != 0)
            {
                PopScope(c);
                return -1;
            }
            if (iteratorSig->hasContext) {
                if (EmitContextArgForSig(c, iteratorSig) != 0 || BufAppendCStr(&c->out, ", ") != 0)
                {
                    PopScope(c);
                    return -1;
                }
            }
            if (!iteratorAutoRefSource) {
                if (BufAppendCStr(&c->out, sourceTmpName) != 0
                    || BufAppendCStr(&c->out, ");\n") != 0)
                {
                    PopScope(c);
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((") != 0
                    || EmitTypeNameWithDepth(c, &iteratorSig->paramTypes[0]) != 0
                    || BufAppendCStr(&c->out, ")(&") != 0
                    || BufAppendCStr(&c->out, sourceTmpName) != 0
                    || BufAppendCStr(&c->out, ")));\n") != 0)
                {
                    PopScope(c);
                    return -1;
                }
            }

            if (hasKey) {
                const H2AstNode* keyNameNode = NodeAt(c, keyNode);
                keyName = DupSlice(
                    c, c->unit->source, keyNameNode->dataStart, keyNameNode->dataEnd);
                if (keyName == NULL || EnsureAnonTypeVisible(c, &keyType, depth + 1u) != 0) {
                    PopScope(c);
                    return -1;
                }
                EmitIndent(c, depth + 1u);
                if (EmitTypeRefWithName(c, &keyType, keyName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0 || AddLocal(c, keyName, keyType) != 0)
                {
                    PopScope(c);
                    return -1;
                }
            }

            if (!valueDiscard) {
                const H2AstNode* valueNameNode = NodeAt(c, valueNode);
                valueName = DupSlice(
                    c, c->unit->source, valueNameNode->dataStart, valueNameNode->dataEnd);
                if (valueName == NULL || EnsureAnonTypeVisible(c, &valueLocalType, depth + 1u) != 0)
                {
                    PopScope(c);
                    return -1;
                }
                EmitIndent(c, depth + 1u);
                if (EmitTypeRefWithName(c, &valueLocalType, valueName) != 0
                    || BufAppendCStr(&c->out, ";\n") != 0
                    || AddLocal(c, valueName, valueLocalType) != 0)
                {
                    PopScope(c);
                    return -1;
                }
            }

            if (EnsureAnonTypeVisible(c, &nextValueOptionalType, depth + 1u) != 0) {
                PopScope(c);
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (EmitTypeRefWithName(c, &nextValueOptionalType, nextTmpName) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                PopScope(c);
                return -1;
            }

            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "for (; ((") != 0 || BufAppendCStr(&c->out, nextTmpName) != 0
                || BufAppendCStr(&c->out, " = ") != 0
                || BufAppendCStr(&c->out, nextValueCallee) != 0 || BufAppendChar(&c->out, '(') != 0)
            {
                PopScope(c);
                return -1;
            }
            if (nextValueSig->hasContext) {
                if (EmitContextArgForSig(c, nextValueSig) != 0 || BufAppendCStr(&c->out, ", ") != 0)
                {
                    PopScope(c);
                    return -1;
                }
            }
            if (BufAppendChar(&c->out, '&') != 0 || BufAppendCStr(&c->out, iterTmpName) != 0) {
                PopScope(c);
                return -1;
            }
            if (BufAppendCStr(&c->out, ")), ") != 0
                || EmitOptionalIsSomeFromNameType(c, nextTmpName, &nextValueOptionalType) != 0
                || BufAppendCStr(&c->out, "); ) {\n") != 0)
            {
                PopScope(c);
                return -1;
            }

            if (hasKey) {
                EmitIndent(c, depth + 2u);
                if (BufAppendCStr(&c->out, keyName) != 0 || BufAppendCStr(&c->out, " = ") != 0) {
                    PopScope(c);
                    return -1;
                }
                if (useNextPair) {
                    if (pairOptionalTagged) {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, ".__hop_value.") != 0
                            || BufAppendCStr(&c->out, pairKeyFieldName) != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    } else if (pairPayloadIsIndirect) {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, "->") != 0
                            || BufAppendCStr(&c->out, pairKeyFieldName) != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    } else {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, ".") != 0
                            || BufAppendCStr(&c->out, pairKeyFieldName) != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    }
                } else {
                    if (BufAppendCStr(&c->out, nextTmpName) != 0) {
                        PopScope(c);
                        return -1;
                    }
                }
                if (BufAppendCStr(&c->out, ";\n") != 0) {
                    PopScope(c);
                    return -1;
                }
            }

            if (!valueDiscard) {
                EmitIndent(c, depth + 2u);
                if (BufAppendCStr(&c->out, valueName) != 0 || BufAppendCStr(&c->out, " = ") != 0) {
                    PopScope(c);
                    return -1;
                }
                if (useNextPair) {
                    if (pairValueNeedsDeref && BufAppendChar(&c->out, '*') != 0) {
                        PopScope(c);
                        return -1;
                    }
                    if (pairOptionalTagged) {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, ".__hop_value.") != 0
                            || BufAppendCStr(&c->out, pairValueFieldName) != 0
                            || BufAppendCStr(&c->out, ";\n") != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    } else if (pairPayloadIsIndirect) {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, "->") != 0
                            || BufAppendCStr(&c->out, pairValueFieldName) != 0
                            || BufAppendCStr(&c->out, ";\n") != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    } else {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, ".") != 0
                            || BufAppendCStr(&c->out, pairValueFieldName) != 0
                            || BufAppendCStr(&c->out, ";\n") != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    }
                } else {
                    if (!valueRef && nextValuePayloadIsIndirect && BufAppendChar(&c->out, '*') != 0)
                    {
                        PopScope(c);
                        return -1;
                    }
                    if (nextValueOptionalTagged && !nextValuePayloadIsIndirect) {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, ".__hop_value;\n") != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    } else {
                        if (BufAppendCStr(&c->out, nextTmpName) != 0
                            || BufAppendCStr(&c->out, ";\n") != 0)
                        {
                            PopScope(c);
                            return -1;
                        }
                    }
                }
            }

            if (EmitStmt(c, loopBodyNode, depth + 2u) != 0) {
                PopScope(c);
                return -1;
            }

            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "}\n") != 0) {
                PopScope(c);
                return -1;
            }
            PopScope(c);
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "}\n");
        }
    }

    body = nodes[count - 1];
    bodyNode = NodeAt(c, body);
    if (count == 1) {
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "for (;;)") != 0) {
            return -1;
        }
        if (bodyNode != NULL && bodyNode->kind == H2Ast_BLOCK) {
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

    if (count == 2 && NodeAt(c, nodes[0])->kind != H2Ast_VAR
        && NodeAt(c, nodes[0])->kind != H2Ast_CONST)
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
        if (NodeAt(c, init)->kind == H2Ast_VAR) {
            if (EmitVarLikeStmt(c, init, depth + 1u, 0) != 0) {
                return -1;
            }
        } else if (NodeAt(c, init)->kind == H2Ast_CONST) {
            if (EmitVarLikeStmt(c, init, depth + 1u, 1) != 0) {
                return -1;
            }
        } else if (NodeAt(c, init)->kind == H2Ast_SHORT_ASSIGN) {
            if (EmitShortAssignStmt(c, init, depth + 1u) != 0) {
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
    if (bodyNode != NULL && bodyNode->kind == H2Ast_BLOCK) {
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

int EmitSwitchStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    const H2AstNode* sw = NodeAt(c, nodeId);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          subjectNode = -1;
    int32_t          subjectLocalIdx = -1;
    H2TypeRef        subjectType;
    int              haveSubjectType = 0;
    int              subjectEnumHasPayload = 0;
    int              firstClause = 1;
    const char*      switchSubjectTemp = "__hop_sw_subject";

    TypeRefSetInvalid(&subjectType);
    if (sw == NULL) {
        return -1;
    }

    if (sw->flags == 1) {
        const H2AstNode* subjectAst;
        subjectNode = child;
        child = AstNextSibling(&c->ast, child);
        subjectAst = NodeAt(c, subjectNode);
        if (subjectAst != NULL && subjectAst->kind == H2Ast_IDENT) {
            subjectLocalIdx = FindLocalIndexBySlice(c, subjectAst->dataStart, subjectAst->dataEnd);
        }
        if (InferExprType(c, subjectNode, &subjectType) == 0 && subjectType.valid) {
            haveSubjectType = 1;
            if (subjectType.containerKind == H2TypeContainer_SCALAR
                && subjectType.containerPtrDepth == 0 && subjectType.ptrDepth == 0
                && subjectType.baseName != NULL)
            {
                const char*      baseName = ResolveScalarAliasBaseName(c, subjectType.baseName);
                const H2NameMap* map;
                int32_t          enumNodeId;
                if (baseName == NULL) {
                    baseName = subjectType.baseName;
                }
                map = FindNameByCName(c, baseName);
                if (map != NULL && map->kind == H2Ast_ENUM
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
        const H2AstNode* clause = NodeAt(c, child);
        if (clause != NULL && clause->kind == H2Ast_CASE) {
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
            const H2AstNode* bodyStmt;

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
                    const H2NameMap* labelEnumMap = NULL;
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
                            if (BufAppendCStr(
                                    &c->out, "(__extension__({ __auto_type __hop_cmp_b = ")
                                    != 0
                                || EmitExpr(c, labelExprNode) != 0
                                || BufAppendCStr(&c->out, "; __hop_mem_equal((const void*)&") != 0
                                || BufAppendCStr(&c->out, switchSubjectTemp) != 0
                                || BufAppendCStr(
                                       &c->out, ", (const void*)&__hop_cmp_b, (__hop_uint)sizeof(")
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
                    char*     aliasName;
                    H2TypeRef aliasType;
                    if (!haveSubjectType) {
                        PopScope(c);
                        return -1;
                    }
                    if (ResolveEnumVariantPayloadType(
                            c,
                            aliasEnumTypeNames[a],
                            aliasVariantStarts[a],
                            aliasVariantEnds[a],
                            &aliasType)
                        != 0)
                    {
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
                        || BufAppendCStr(&c->out, ".payload.") != 0
                        || BufAppendSlice(
                               &c->out, c->unit->source, aliasVariantStarts[a], aliasVariantEnds[a])
                               != 0
                        || BufAppendCStr(&c->out, ";\n") != 0)
                    {
                        PopScope(c);
                        return -1;
                    }
                    if (AddLocal(c, aliasName, aliasType) != 0) {
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
            if (bodyStmt != NULL && bodyStmt->kind == H2Ast_BLOCK) {
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
        } else if (clause != NULL && clause->kind == H2Ast_DEFAULT) {
            int32_t          bodyNode = AstFirstChild(&c->ast, child);
            const H2AstNode* bodyStmt = NodeAt(c, bodyNode);
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, firstClause ? "if (1) {\n" : "else {\n") != 0) {
                return -1;
            }
            firstClause = 0;
            if (PushScope(c) != 0) {
                return -1;
            }
            if (bodyStmt != NULL && bodyStmt->kind == H2Ast_BLOCK) {
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

int EmitAssertFormatArg(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    H2TypeRef        t;
    if (n == NULL) {
        return -1;
    }
    if (n->kind == H2Ast_STRING) {
        return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
    }
    if (InferExprType(c, nodeId, &t) != 0 || !t.valid) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "(const char*)(const void*)__hop_cstr(") != 0
        || (TypeRefIsStr(&t) ? EmitStrValueExpr(c, nodeId, &t) : EmitExpr(c, nodeId)) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

int EvalConstIntForIf(H2CBackendC* c, int32_t nodeId, int64_t* outValue, int* outKnown) {
    const H2AstNode* n = NodeAt(c, nodeId);
    int64_t          v = 0;
    int              isConst = 0;
    if (outValue == NULL || outKnown == NULL) {
        return -1;
    }
    *outKnown = 0;
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_CALL) {
        int32_t          callee = AstFirstChild(&c->ast, nodeId);
        int32_t          arg = AstNextSibling(&c->ast, callee);
        int32_t          argExpr = UnwrapCallArgExprNode(c, arg);
        int32_t          extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
        const H2AstNode* calleeNode = NodeAt(c, callee);
        const H2AstNode* argNode = NodeAt(c, argExpr);
        if (calleeNode != NULL && calleeNode->kind == H2Ast_IDENT && argNode != NULL
            && argNode->kind == H2Ast_IDENT
            && SliceEq(c->unit->source, calleeNode->dataStart, calleeNode->dataEnd, "len")
            && IsActivePackIdent(c, argNode->dataStart, argNode->dataEnd) && extra < 0)
        {
            *outValue = (int64_t)c->activePackElemCount;
            *outKnown = 1;
            return 0;
        }
    }
    if (EvalConstIntExpr(c, nodeId, &v, &isConst) == 0 && isConst) {
        *outValue = v;
        *outKnown = 1;
    }
    return 0;
}

int EvalConstBoolForIf(H2CBackendC* c, int32_t nodeId, int* outKnown, int* outValue) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (outKnown == NULL || outValue == NULL) {
        return -1;
    }
    *outKnown = 0;
    *outValue = 0;
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_BOOL) {
        *outKnown = 1;
        *outValue = SliceEq(c->unit->source, n->dataStart, n->dataEnd, "true") ? 1 : 0;
        return 0;
    }
    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_NOT) {
        int32_t inner = AstFirstChild(&c->ast, nodeId);
        int     known = 0;
        int     value = 0;
        if (EvalConstBoolForIf(c, inner, &known, &value) != 0) {
            return -1;
        }
        if (known) {
            *outKnown = 1;
            *outValue = value ? 0 : 1;
        }
        return 0;
    }
    if (n->kind == H2Ast_BINARY) {
        int32_t lhs = AstFirstChild(&c->ast, nodeId);
        int32_t rhs = AstNextSibling(&c->ast, lhs);
        if ((H2TokenKind)n->op == H2Tok_LOGICAL_AND || (H2TokenKind)n->op == H2Tok_LOGICAL_OR) {
            int lhsKnown = 0;
            int lhsValue = 0;
            int rhsKnown = 0;
            int rhsValue = 0;
            if (EvalConstBoolForIf(c, lhs, &lhsKnown, &lhsValue) != 0) {
                return -1;
            }
            if ((H2TokenKind)n->op == H2Tok_LOGICAL_AND && lhsKnown && !lhsValue) {
                *outKnown = 1;
                *outValue = 0;
                return 0;
            }
            if ((H2TokenKind)n->op == H2Tok_LOGICAL_OR && lhsKnown && lhsValue) {
                *outKnown = 1;
                *outValue = 1;
                return 0;
            }
            if (EvalConstBoolForIf(c, rhs, &rhsKnown, &rhsValue) != 0) {
                return -1;
            }
            if (lhsKnown && rhsKnown) {
                *outKnown = 1;
                *outValue = (H2TokenKind)n->op == H2Tok_LOGICAL_AND
                              ? (lhsValue && rhsValue)
                              : (lhsValue || rhsValue);
            }
            return 0;
        }
        if ((H2TokenKind)n->op == H2Tok_EQ || (H2TokenKind)n->op == H2Tok_NEQ
            || (H2TokenKind)n->op == H2Tok_LT || (H2TokenKind)n->op == H2Tok_LTE
            || (H2TokenKind)n->op == H2Tok_GT || (H2TokenKind)n->op == H2Tok_GTE)
        {
            if ((H2TokenKind)n->op == H2Tok_EQ || (H2TokenKind)n->op == H2Tok_NEQ) {
                H2TypeRef lhsType;
                H2TypeRef rhsType;
                int       lhsTypeKnown = ResolveReflectedTypeValueExprTypeRef(c, lhs, &lhsType);
                int       rhsTypeKnown = ResolveReflectedTypeValueExprTypeRef(c, rhs, &rhsType);
                if (lhsTypeKnown && rhsTypeKnown && lhsType.valid && rhsType.valid) {
                    *outKnown = 1;
                    *outValue = ((H2TokenKind)n->op == H2Tok_EQ)
                                  ? (TypeRefEqual(&lhsType, &rhsType) ? 1 : 0)
                                  : (TypeRefEqual(&lhsType, &rhsType) ? 0 : 1);
                    return 0;
                }
            }
            int64_t lv = 0;
            int64_t rv = 0;
            int     lk = 0;
            int     rk = 0;
            if (EvalConstIntForIf(c, lhs, &lv, &lk) != 0
                || EvalConstIntForIf(c, rhs, &rv, &rk) != 0)
            {
                return -1;
            }
            if (lk && rk) {
                int v = 0;
                switch ((H2TokenKind)n->op) {
                    case H2Tok_EQ:  v = (lv == rv); break;
                    case H2Tok_NEQ: v = (lv != rv); break;
                    case H2Tok_LT:  v = (lv < rv); break;
                    case H2Tok_LTE: v = (lv <= rv); break;
                    case H2Tok_GT:  v = (lv > rv); break;
                    case H2Tok_GTE: v = (lv >= rv); break;
                    default:        v = 0; break;
                }
                *outKnown = 1;
                *outValue = v;
            }
            return 0;
        }
    }
    return 0;
}

static int ResolveOptionalCondNarrow(
    H2CBackendC* c,
    int32_t      condNode,
    int32_t*     outLocalIdx,
    H2TypeRef*   outInnerType,
    int*         outThenIsSome) {
    const H2AstNode* n;
    if (c == NULL || outLocalIdx == NULL || outInnerType == NULL || outThenIsSome == NULL) {
        return 0;
    }
    if (condNode < 0 || (uint32_t)condNode >= c->ast.len) {
        return 0;
    }
    n = NodeAt(c, condNode);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_NOT) {
        int32_t innerNode = AstFirstChild(&c->ast, condNode);
        if (!ResolveOptionalCondNarrow(c, innerNode, outLocalIdx, outInnerType, outThenIsSome)) {
            return 0;
        }
        *outThenIsSome = !*outThenIsSome;
        return 1;
    }
    if (n->kind == H2Ast_IDENT) {
        int32_t        localIdx = FindLocalIndexBySlice(c, n->dataStart, n->dataEnd);
        const H2Local* local;
        if (localIdx < 0 || (uint32_t)localIdx >= c->localLen) {
            return 0;
        }
        local = &c->locals[localIdx];
        if (!local->type.valid || !local->type.isOptional) {
            return 0;
        }
        *outLocalIdx = localIdx;
        *outInnerType = local->type;
        outInnerType->isOptional = 0;
        *outThenIsSome = 1;
        return 1;
    }
    if (n->kind == H2Ast_BINARY
        && ((H2TokenKind)n->op == H2Tok_EQ || (H2TokenKind)n->op == H2Tok_NEQ))
    {
        int32_t lhsNode = AstFirstChild(&c->ast, condNode);
        int32_t rhsNode = AstNextSibling(&c->ast, lhsNode);
        int32_t identNode = -1;
        if (lhsNode < 0 || rhsNode < 0) {
            return 0;
        }
        if (NodeAt(c, lhsNode) != NULL && NodeAt(c, lhsNode)->kind == H2Ast_IDENT
            && NodeAt(c, rhsNode) != NULL && NodeAt(c, rhsNode)->kind == H2Ast_NULL)
        {
            identNode = lhsNode;
        } else if (
            NodeAt(c, rhsNode) != NULL && NodeAt(c, rhsNode)->kind == H2Ast_IDENT
            && NodeAt(c, lhsNode) != NULL && NodeAt(c, lhsNode)->kind == H2Ast_NULL)
        {
            identNode = rhsNode;
        } else {
            return 0;
        }
        {
            const H2AstNode* id = NodeAt(c, identNode);
            int32_t          localIdx =
                id != NULL ? FindLocalIndexBySlice(c, id->dataStart, id->dataEnd) : -1;
            const H2Local* local;
            if (localIdx < 0 || (uint32_t)localIdx >= c->localLen) {
                return 0;
            }
            local = &c->locals[localIdx];
            if (!local->type.valid || !local->type.isOptional) {
                return 0;
            }
            *outLocalIdx = localIdx;
            *outInnerType = local->type;
            outInnerType->isOptional = 0;
            *outThenIsSome = ((H2TokenKind)n->op == H2Tok_NEQ) ? 1 : 0;
            return 1;
        }
    }
    return 0;
}

int EmitStmt(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case H2Ast_BLOCK:        return EmitBlock(c, nodeId, depth);
        case H2Ast_VAR:          return EmitVarLikeStmt(c, nodeId, depth, 0);
        case H2Ast_CONST:        return EmitVarLikeStmt(c, nodeId, depth, 1);
        case H2Ast_CONST_BLOCK:  return 0;
        case H2Ast_MULTI_ASSIGN: return EmitMultiAssignStmt(c, nodeId, depth);
        case H2Ast_SHORT_ASSIGN: return EmitShortAssignStmt(c, nodeId, depth);
        case H2Ast_EXPR_STMT:    {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            if (ExprStmtAssignsContext(c, expr) && EnsureContextCow(c, depth) != 0) {
                return -1;
            }
            EmitIndent(c, depth);
            if (EmitExpr(c, expr) != 0 || BufAppendCStr(&c->out, ";\n") != 0) {
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, expr >= 0 ? expr : nodeId, H2Diag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            return 0;
        }
        case H2Ast_RETURN: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            if (EmitDeferredRange(c, 0, depth) != 0) {
                return -1;
            }
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "return") != 0) {
                return -1;
            }
            if (expr >= 0) {
                if (NodeAt(c, expr) != NULL && NodeAt(c, expr)->kind == H2Ast_EXPR_LIST) {
                    const H2AnonTypeInfo* tupleInfo = NULL;
                    H2TypeRef             tupleType;
                    H2TypeRef             payloadType;
                    H2TypeRef             optionalStorageType;
                    int                   wrapOptionalTuple = 0;
                    uint32_t              i;
                    int32_t               itemNode;
                    TypeRefSetInvalid(&tupleType);
                    TypeRefSetInvalid(&payloadType);
                    TypeRefSetInvalid(&optionalStorageType);
                    if (!c->hasCurrentReturnType) {
                        return -1;
                    }
                    tupleType = c->currentReturnType;
                    if (!TypeRefTupleInfo(c, &tupleType, &tupleInfo)) {
                        if (!TypeRefOptionalPayloadTypeExpr(&c->currentReturnType, &payloadType)
                            || !TypeRefTupleInfo(c, &payloadType, &tupleInfo))
                        {
                            return -1;
                        }
                        tupleType = payloadType;
                        wrapOptionalTuple = 1;
                    }
                    if (ListCount(&c->ast, expr) != tupleInfo->fieldCount) {
                        return -1;
                    }
                    if (wrapOptionalTuple) {
                        if (!TypeRefIsTaggedOptional(&c->currentReturnType)
                            || TypeRefLowerForStorage(
                                   c, &c->currentReturnType, &optionalStorageType)
                                   != 0)
                        {
                            return -1;
                        }
                        if (BufAppendCStr(&c->out, " ((") != 0
                            || EmitTypeNameWithDepth(c, &optionalStorageType) != 0
                            || BufAppendCStr(&c->out, "){ .__hop_tag = 1u, .__hop_value = ((") != 0
                            || EmitTypeNameWithDepth(c, &tupleType) != 0
                            || BufAppendCStr(&c->out, "){") != 0)
                        {
                            return -1;
                        }
                    } else {
                        if (BufAppendCStr(&c->out, " ((") != 0
                            || EmitTypeNameWithDepth(c, &tupleType) != 0
                            || BufAppendCStr(&c->out, "){") != 0)
                        {
                            return -1;
                        }
                    }
                    itemNode = AstFirstChild(&c->ast, expr);
                    for (i = 0; i < tupleInfo->fieldCount; i++) {
                        const H2FieldInfo* f = &c->fieldInfos[tupleInfo->fieldStart + i];
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
                    if (itemNode >= 0) {
                        return -1;
                    }
                    if (wrapOptionalTuple) {
                        if (BufAppendCStr(&c->out, "}) })") != 0) {
                            return -1;
                        }
                    } else {
                        if (BufAppendCStr(&c->out, "})") != 0) {
                            return -1;
                        }
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
        case H2Ast_ASSERT: {
            int32_t cond = AstFirstChild(&c->ast, nodeId);
            int32_t fmtNode;
            if (cond < 0) {
                return -1;
            }
            EmitIndent(c, depth);
            fmtNode = AstNextSibling(&c->ast, cond);
            if (fmtNode < 0) {
                if (BufAppendCStr(&c->out, "__hop_assert(") != 0 || EmitExpr(c, cond) != 0
                    || BufAppendCStr(&c->out, ");\n") != 0)
                {
                    return -1;
                }
            } else {
                int32_t argNode;
                if (BufAppendCStr(&c->out, "__hop_assertf(") != 0 || EmitExpr(c, cond) != 0
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
        case H2Ast_DEL: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            int32_t allocArg = -1;
            if ((n->flags & H2AstFlag_DEL_HAS_ALLOC) != 0) {
                int32_t scan = expr;
                while (scan >= 0) {
                    int32_t next = AstNextSibling(&c->ast, scan);
                    if (next < 0) {
                        allocArg = scan;
                        break;
                    }
                    scan = next;
                }
            }
            while (expr >= 0 && expr != allocArg) {
                EmitIndent(c, depth);
                if (EmitFreeCallExpr(c, allocArg, expr) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
                {
                    return -1;
                }
                expr = AstNextSibling(&c->ast, expr);
            }
            return 0;
        }
        case H2Ast_IF: {
            int32_t          cond = AstFirstChild(&c->ast, nodeId);
            int32_t          thenNode = AstNextSibling(&c->ast, cond);
            int32_t          elseNode = AstNextSibling(&c->ast, thenNode);
            const H2AstNode* thenStmt = NodeAt(c, thenNode);
            const H2AstNode* elseStmt = NodeAt(c, elseNode);
            H2TypeRef        condType;
            H2TypeRef        narrowInnerType;
            H2TypeRef        savedLocalType;
            int              haveCondType = 0;
            int              condKnown = 0;
            int              condValue = 0;
            int              hasOptionalNarrow = 0;
            int              narrowThenIsSome = 0;
            int32_t          narrowLocalIdx = -1;
            int32_t          savedActiveOptionalNarrowLocalIdx = -1;
            uint8_t          savedHasActiveOptionalNarrow = 0;
            H2TypeRef        savedActiveOptionalNarrowStorageType;
            if (EvalConstBoolForIf(c, cond, &condKnown, &condValue) != 0) {
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, cond >= 0 ? cond : nodeId, H2Diag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            hasOptionalNarrow = ResolveOptionalCondNarrow(
                c, cond, &narrowLocalIdx, &narrowInnerType, &narrowThenIsSome);
            if (condKnown) {
                if (condValue) {
                    if (hasOptionalNarrow && narrowThenIsSome && narrowLocalIdx >= 0
                        && (uint32_t)narrowLocalIdx < c->localLen)
                    {
                        savedLocalType = c->locals[narrowLocalIdx].type;
                        c->locals[narrowLocalIdx].type = narrowInnerType;
                        savedHasActiveOptionalNarrow = c->hasActiveOptionalNarrow;
                        savedActiveOptionalNarrowLocalIdx = c->activeOptionalNarrowLocalIdx;
                        savedActiveOptionalNarrowStorageType = c->activeOptionalNarrowStorageType;
                        c->hasActiveOptionalNarrow = 1;
                        c->activeOptionalNarrowLocalIdx = narrowLocalIdx;
                        c->activeOptionalNarrowStorageType = savedLocalType;
                    }
                    if (EmitStmt(c, thenNode, depth) != 0) {
                        if (hasOptionalNarrow && narrowThenIsSome && narrowLocalIdx >= 0
                            && (uint32_t)narrowLocalIdx < c->localLen)
                        {
                            c->locals[narrowLocalIdx].type = savedLocalType;
                            c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                            c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                            c->activeOptionalNarrowStorageType =
                                savedActiveOptionalNarrowStorageType;
                        }
                        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                            SetDiagNode(
                                c, thenNode >= 0 ? thenNode : nodeId, H2Diag_CODEGEN_INTERNAL);
                        }
                        return -1;
                    }
                    if (hasOptionalNarrow && narrowThenIsSome && narrowLocalIdx >= 0
                        && (uint32_t)narrowLocalIdx < c->localLen)
                    {
                        c->locals[narrowLocalIdx].type = savedLocalType;
                        c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                        c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                        c->activeOptionalNarrowStorageType = savedActiveOptionalNarrowStorageType;
                    }
                    return 0;
                }
                if (elseNode >= 0) {
                    if (hasOptionalNarrow && !narrowThenIsSome && narrowLocalIdx >= 0
                        && (uint32_t)narrowLocalIdx < c->localLen)
                    {
                        savedLocalType = c->locals[narrowLocalIdx].type;
                        c->locals[narrowLocalIdx].type = narrowInnerType;
                        savedHasActiveOptionalNarrow = c->hasActiveOptionalNarrow;
                        savedActiveOptionalNarrowLocalIdx = c->activeOptionalNarrowLocalIdx;
                        savedActiveOptionalNarrowStorageType = c->activeOptionalNarrowStorageType;
                        c->hasActiveOptionalNarrow = 1;
                        c->activeOptionalNarrowLocalIdx = narrowLocalIdx;
                        c->activeOptionalNarrowStorageType = savedLocalType;
                    }
                    if (EmitStmt(c, elseNode, depth) != 0) {
                        if (hasOptionalNarrow && !narrowThenIsSome && narrowLocalIdx >= 0
                            && (uint32_t)narrowLocalIdx < c->localLen)
                        {
                            c->locals[narrowLocalIdx].type = savedLocalType;
                            c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                            c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                            c->activeOptionalNarrowStorageType =
                                savedActiveOptionalNarrowStorageType;
                        }
                        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                            SetDiagNode(c, elseNode, H2Diag_CODEGEN_INTERNAL);
                        }
                        return -1;
                    }
                    if (hasOptionalNarrow && !narrowThenIsSome && narrowLocalIdx >= 0
                        && (uint32_t)narrowLocalIdx < c->localLen)
                    {
                        c->locals[narrowLocalIdx].type = savedLocalType;
                        c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                        c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                        c->activeOptionalNarrowStorageType = savedActiveOptionalNarrowStorageType;
                    }
                    return 0;
                }
                return 0;
            }
            haveCondType = (InferExprType(c, cond, &condType) == 0 && condType.valid);
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "if (") != 0) {
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, cond >= 0 ? cond : nodeId, H2Diag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            if (haveCondType && condType.isOptional) {
                if (EmitOptionalIsSomeExpr(c, cond, &condType, 1) != 0) {
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, cond >= 0 ? cond : nodeId, H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
            } else if (EmitExpr(c, cond) != 0) {
                if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                    SetDiagNode(c, cond >= 0 ? cond : nodeId, H2Diag_CODEGEN_INTERNAL);
                }
                return -1;
            }
            if (hasOptionalNarrow && narrowThenIsSome && narrowLocalIdx >= 0
                && (uint32_t)narrowLocalIdx < c->localLen)
            {
                savedLocalType = c->locals[narrowLocalIdx].type;
                c->locals[narrowLocalIdx].type = narrowInnerType;
                savedHasActiveOptionalNarrow = c->hasActiveOptionalNarrow;
                savedActiveOptionalNarrowLocalIdx = c->activeOptionalNarrowLocalIdx;
                savedActiveOptionalNarrowStorageType = c->activeOptionalNarrowStorageType;
                c->hasActiveOptionalNarrow = 1;
                c->activeOptionalNarrowLocalIdx = narrowLocalIdx;
                c->activeOptionalNarrowStorageType = savedLocalType;
            }
            if (thenStmt != NULL && thenStmt->kind == H2Ast_BLOCK) {
                if (BufAppendCStr(&c->out, ") ") != 0 || EmitBlockInline(c, thenNode, depth) != 0) {
                    if (hasOptionalNarrow && narrowThenIsSome && narrowLocalIdx >= 0
                        && (uint32_t)narrowLocalIdx < c->localLen)
                    {
                        c->locals[narrowLocalIdx].type = savedLocalType;
                        c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                        c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                        c->activeOptionalNarrowStorageType = savedActiveOptionalNarrowStorageType;
                    }
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, thenNode >= 0 ? thenNode : nodeId, H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, ")\n") != 0 || EmitStmt(c, thenNode, depth) != 0) {
                    if (hasOptionalNarrow && narrowThenIsSome && narrowLocalIdx >= 0
                        && (uint32_t)narrowLocalIdx < c->localLen)
                    {
                        c->locals[narrowLocalIdx].type = savedLocalType;
                        c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                        c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                        c->activeOptionalNarrowStorageType = savedActiveOptionalNarrowStorageType;
                    }
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, thenNode >= 0 ? thenNode : nodeId, H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
            }
            if (hasOptionalNarrow && narrowThenIsSome && narrowLocalIdx >= 0
                && (uint32_t)narrowLocalIdx < c->localLen)
            {
                c->locals[narrowLocalIdx].type = savedLocalType;
                c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                c->activeOptionalNarrowStorageType = savedActiveOptionalNarrowStorageType;
            }
            if (elseNode >= 0) {
                if (hasOptionalNarrow && !narrowThenIsSome && narrowLocalIdx >= 0
                    && (uint32_t)narrowLocalIdx < c->localLen)
                {
                    savedLocalType = c->locals[narrowLocalIdx].type;
                    c->locals[narrowLocalIdx].type = narrowInnerType;
                    savedHasActiveOptionalNarrow = c->hasActiveOptionalNarrow;
                    savedActiveOptionalNarrowLocalIdx = c->activeOptionalNarrowLocalIdx;
                    savedActiveOptionalNarrowStorageType = c->activeOptionalNarrowStorageType;
                    c->hasActiveOptionalNarrow = 1;
                    c->activeOptionalNarrowLocalIdx = narrowLocalIdx;
                    c->activeOptionalNarrowStorageType = savedLocalType;
                }
                EmitIndent(c, depth);
                if (elseStmt != NULL && elseStmt->kind == H2Ast_BLOCK) {
                    if (BufAppendCStr(&c->out, "else ") != 0
                        || EmitBlockInline(c, elseNode, depth) != 0)
                    {
                        if (hasOptionalNarrow && !narrowThenIsSome && narrowLocalIdx >= 0
                            && (uint32_t)narrowLocalIdx < c->localLen)
                        {
                            c->locals[narrowLocalIdx].type = savedLocalType;
                            c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                            c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                            c->activeOptionalNarrowStorageType =
                                savedActiveOptionalNarrowStorageType;
                        }
                        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                            SetDiagNode(c, elseNode, H2Diag_CODEGEN_INTERNAL);
                        }
                        return -1;
                    }
                } else {
                    if (BufAppendCStr(&c->out, "else\n") != 0 || EmitStmt(c, elseNode, depth) != 0)
                    {
                        if (hasOptionalNarrow && !narrowThenIsSome && narrowLocalIdx >= 0
                            && (uint32_t)narrowLocalIdx < c->localLen)
                        {
                            c->locals[narrowLocalIdx].type = savedLocalType;
                            c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                            c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                            c->activeOptionalNarrowStorageType =
                                savedActiveOptionalNarrowStorageType;
                        }
                        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                            SetDiagNode(c, elseNode, H2Diag_CODEGEN_INTERNAL);
                        }
                        return -1;
                    }
                }
                if (hasOptionalNarrow && !narrowThenIsSome && narrowLocalIdx >= 0
                    && (uint32_t)narrowLocalIdx < c->localLen)
                {
                    c->locals[narrowLocalIdx].type = savedLocalType;
                    c->hasActiveOptionalNarrow = savedHasActiveOptionalNarrow;
                    c->activeOptionalNarrowLocalIdx = savedActiveOptionalNarrowLocalIdx;
                    c->activeOptionalNarrowStorageType = savedActiveOptionalNarrowStorageType;
                }
            }
            return 0;
        }
        case H2Ast_FOR:    return EmitForStmt(c, nodeId, depth);
        case H2Ast_SWITCH: return EmitSwitchStmt(c, nodeId, depth);
        case H2Ast_BREAK:
            if (c->deferScopeLen > 0
                && EmitDeferredRange(c, c->deferScopeMarks[c->deferScopeLen - 1u], depth) != 0)
            {
                return -1;
            }
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "break;\n");
        case H2Ast_CONTINUE:
            if (c->deferScopeLen > 0
                && EmitDeferredRange(c, c->deferScopeMarks[c->deferScopeLen - 1u], depth) != 0)
            {
                return -1;
            }
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "continue;\n");
        case H2Ast_DEFER: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (child < 0) {
                return -1;
            }
            return AddDeferredStmt(c, child);
        }
        default: SetDiag(c->diag, H2Diag_CODEGEN_INTERNAL, n->start, n->end); return -1;
    }
}

H2_API_END
