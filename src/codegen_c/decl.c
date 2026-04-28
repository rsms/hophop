#include "internal.h"
#include "../typecheck/internal.h"

H2_API_BEGIN
typedef enum {
    H2CForeignDecl_NONE = 0,
    H2CForeignDecl_C_IMPORT,
    H2CForeignDecl_WASM_IMPORT,
    H2CForeignDecl_EXPORT,
} H2CForeignDeclKind;

typedef struct {
    H2CForeignDeclKind kind;
    uint32_t           arg0Start;
    uint32_t           arg0End;
    uint32_t           arg1Start;
    uint32_t           arg1End;
} H2CForeignDeclInfo;

static int CDirectiveNameEq(const H2CBackendC* c, int32_t nodeId, const char* name) {
    const H2AstNode* n = NodeAt(c, nodeId);
    size_t           len = StrLen(name);
    return n != NULL && n->kind == H2Ast_DIRECTIVE && n->dataEnd >= n->dataStart
        && (size_t)(n->dataEnd - n->dataStart) == len
        && memcmp(c->unit->source + n->dataStart, name, len) == 0;
}

static int32_t CDirectiveArgAt(const H2CBackendC* c, int32_t nodeId, uint32_t index) {
    uint32_t i = 0;
    int32_t  child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        if (i == index) {
            return child;
        }
        i++;
        child = AstNextSibling(&c->ast, child);
    }
    return -1;
}

static int CFindDirectiveRun(
    const H2CBackendC* c,
    int32_t            declNodeId,
    int32_t*           outFirstDirective,
    int32_t*           outLastDirective) {
    int32_t child = AstFirstChild(&c->ast, c->ast.root);
    int32_t first = -1;
    int32_t last = -1;
    while (child >= 0) {
        const H2AstNode* n = NodeAt(c, child);
        if (n != NULL && n->kind == H2Ast_DIRECTIVE) {
            if (first < 0) {
                first = child;
            }
            last = child;
        } else {
            if (child == declNodeId) {
                *outFirstDirective = first;
                *outLastDirective = last;
                return 0;
            }
            first = -1;
            last = -1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    *outFirstDirective = -1;
    *outLastDirective = -1;
    return -1;
}

static int GetForeignDeclInfo(const H2CBackendC* c, int32_t nodeId, H2CForeignDeclInfo* out) {
    int32_t firstDirective = -1;
    int32_t lastDirective = -1;
    int32_t child;
    *out = (H2CForeignDeclInfo){ 0 };
    if (CFindDirectiveRun(c, nodeId, &firstDirective, &lastDirective) != 0 || firstDirective < 0) {
        return 0;
    }
    child = firstDirective;
    while (child >= 0) {
        if (CDirectiveNameEq(c, child, "c_import")) {
            int32_t arg0 = CDirectiveArgAt(c, child, 0u);
            if (arg0 >= 0) {
                out->kind = H2CForeignDecl_C_IMPORT;
                out->arg0Start = c->ast.nodes[arg0].start;
                out->arg0End = c->ast.nodes[arg0].end;
            }
        } else if (CDirectiveNameEq(c, child, "wasm_import")) {
            int32_t arg0 = CDirectiveArgAt(c, child, 0u);
            int32_t arg1 = CDirectiveArgAt(c, child, 1u);
            if (arg0 >= 0 && arg1 >= 0) {
                out->kind = H2CForeignDecl_WASM_IMPORT;
                out->arg0Start = c->ast.nodes[arg0].start;
                out->arg0End = c->ast.nodes[arg0].end;
                out->arg1Start = c->ast.nodes[arg1].start;
                out->arg1End = c->ast.nodes[arg1].end;
            }
        } else if (CDirectiveNameEq(c, child, "export")) {
            int32_t arg0 = CDirectiveArgAt(c, child, 0u);
            if (arg0 >= 0) {
                out->kind = H2CForeignDecl_EXPORT;
                out->arg0Start = c->ast.nodes[arg0].start;
                out->arg0End = c->ast.nodes[arg0].end;
            }
        }
        if (child == lastDirective) {
            break;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return out->kind != H2CForeignDecl_NONE;
}

static const char* _Nullable ForeignStubName(H2CBackendC* c, int32_t nodeId, const char* suffix) {
    H2Buf b = { .arena = &c->arena };
    char* name;
    if (BufAppendCStr(&b, "__hop_foreign_") != 0 || BufAppendCStr(&b, suffix) != 0
        || BufAppendChar(&b, '_') != 0 || BufAppendU32(&b, (uint32_t)nodeId) != 0)
    {
        return NULL;
    }
    name = BufFinish(&b);
    return name;
}

static void BuildForeignArgName(char* dst, uint32_t dstCap, uint32_t index) {
    uint32_t n = index;
    uint32_t len = 0;
    char     digits[16];
    uint32_t i;
    if (dst == NULL || dstCap < 5u) {
        return;
    }
    dst[0] = 'a';
    dst[1] = 'r';
    dst[2] = 'g';
    if (n == 0u) {
        digits[len++] = '0';
    } else {
        while (n > 0u && len < (uint32_t)sizeof(digits)) {
            digits[len++] = (char)('0' + (n % 10u));
            n /= 10u;
        }
    }
    if (3u + len + 1u > dstCap) {
        dst[3] = '0';
        dst[4] = '\0';
        return;
    }
    for (i = 0; i < len; i++) {
        dst[3u + i] = digits[len - 1u - i];
    }
    dst[3u + len] = '\0';
}

static int EmitAsmLabelAttr(
    H2CBackendC* c, uint32_t labelStart, uint32_t labelEnd, int leadingSpace) {
    if (leadingSpace && BufAppendChar(&c->out, ' ') != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "__asm__(") != 0
        || BufAppendSlice(&c->out, c->unit->source, labelStart, labelEnd) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitWasmImportAttrs(
    H2CBackendC* c,
    uint32_t     moduleStart,
    uint32_t     moduleEnd,
    uint32_t     nameStart,
    uint32_t     nameEnd) {
    if (BufAppendCStr(&c->out, " __attribute__((import_module(") != 0
        || BufAppendSlice(&c->out, c->unit->source, moduleStart, moduleEnd) != 0
        || BufAppendCStr(&c->out, "), import_name(") != 0
        || BufAppendSlice(&c->out, c->unit->source, nameStart, nameEnd) != 0
        || BufAppendCStr(&c->out, ")))") != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitForeignFnParamList(
    H2CBackendC* c, const H2FnSig* fnSig, int includeNames, int* _Nullable outNeedsNames) {
    uint32_t paramIndex;
    int      firstParam = 1;
    if (fnSig->isVariadic) {
        SetDiagNode(c, fnSig->nodeId, H2Diag_CODEGEN_INTERNAL);
        if (c->diag != NULL) {
            c->diag->detail = "foreign variadic functions are not supported";
        }
        return -1;
    }
    if (outNeedsNames != NULL) {
        *outNeedsNames = 0;
    }
    for (paramIndex = 0; paramIndex < fnSig->paramLen; paramIndex++) {
        const char* paramName = fnSig->paramNames != NULL ? fnSig->paramNames[paramIndex] : NULL;
        char        fallback[24];
        const char* emitName = NULL;
        if (!firstParam && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (includeNames) {
            if (paramName == NULL || paramName[0] == '\0') {
                BuildForeignArgName(fallback, (uint32_t)sizeof(fallback), paramIndex);
                emitName = fallback;
            } else {
                emitName = paramName;
            }
        }
        if (EmitTypeRefWithName(c, &fnSig->paramTypes[paramIndex], emitName) != 0) {
            return -1;
        }
        if (outNeedsNames != NULL && includeNames) {
            *outNeedsNames = 1;
        }
        firstParam = 0;
    }
    if (firstParam && BufAppendCStr(&c->out, "void") != 0) {
        return -1;
    }
    return 0;
}

static int EmitForeignImportFn(
    H2CBackendC* c, int32_t nodeId, const H2FnSig* fnSig, int emitBody, int isPrivate) {
    const char*        localName = fnSig->cName;
    const char*        stubName = ForeignStubName(c, nodeId, "fn");
    H2CForeignDeclInfo foreign = { 0 };
    if (stubName == NULL || !GetForeignDeclInfo(c, nodeId, &foreign)
        || (foreign.kind != H2CForeignDecl_C_IMPORT && foreign.kind != H2CForeignDecl_WASM_IMPORT))
    {
        return -1;
    }

    EmitIndent(c, 0);
    if (BufAppendCStr(&c->out, "extern ") != 0
        || EmitTypeRefWithName(c, &fnSig->returnType, stubName) != 0
        || BufAppendChar(&c->out, '(') != 0 || EmitForeignFnParamList(c, fnSig, 1, NULL) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    if (foreign.kind == H2CForeignDecl_C_IMPORT) {
        if (EmitAsmLabelAttr(c, foreign.arg0Start, foreign.arg0End, 1) != 0) {
            return -1;
        }
    } else if (
        EmitWasmImportAttrs(
            c, foreign.arg0Start, foreign.arg0End, foreign.arg1Start, foreign.arg1End)
        != 0)
    {
        return -1;
    }
    if (BufAppendChar(&c->out, ';') != 0 || BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }

    EmitIndent(c, 0);
    if (isPrivate && BufAppendCStr(&c->out, "static ") != 0) {
        return -1;
    }
    if (EmitTypeRefWithName(c, &fnSig->returnType, localName) != 0
        || BufAppendChar(&c->out, '(') != 0 || EmitForeignFnParamList(c, fnSig, 1, NULL) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    if (!emitBody) {
        return BufAppendCStr(&c->out, ";\n");
    }
    if (BufAppendCStr(&c->out, " {\n") != 0) {
        return -1;
    }
    EmitIndent(c, 1u);
    if (fnSig->returnType.baseName != NULL && StrEq(fnSig->returnType.baseName, "void")) {
        if (BufAppendCStr(&c->out, stubName) != 0 || BufAppendChar(&c->out, '(') != 0) {
            return -1;
        }
    } else if (
        BufAppendCStr(&c->out, "return ") != 0 || BufAppendCStr(&c->out, stubName) != 0
        || BufAppendChar(&c->out, '(') != 0)
    {
        return -1;
    }
    {
        uint32_t paramIndex;
        for (paramIndex = 0; paramIndex < fnSig->paramLen; paramIndex++) {
            const char* paramName =
                fnSig->paramNames != NULL ? fnSig->paramNames[paramIndex] : NULL;
            char fallback[24];
            if (paramIndex != 0u && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (paramName == NULL || paramName[0] == '\0') {
                BuildForeignArgName(fallback, (uint32_t)sizeof(fallback), paramIndex);
                paramName = fallback;
            }
            if (BufAppendCStr(&c->out, paramName) != 0) {
                return -1;
            }
        }
    }
    if (BufAppendCStr(&c->out, ");\n}\n") != 0) {
        return -1;
    }
    return 0;
}

static int EmitExportWrapper(H2CBackendC* c, int32_t nodeId, const H2FnSig* fnSig) {
    const char*        exportName = ForeignStubName(c, nodeId, "export");
    H2CForeignDeclInfo foreign = { 0 };
    if (exportName == NULL || !GetForeignDeclInfo(c, nodeId, &foreign)
        || foreign.kind != H2CForeignDecl_EXPORT)
    {
        return -1;
    }
    EmitIndent(c, 0);
    if (EmitTypeRefWithName(c, &fnSig->returnType, exportName) != 0
        || BufAppendChar(&c->out, '(') != 0 || EmitForeignFnParamList(c, fnSig, 1, NULL) != 0
        || BufAppendChar(&c->out, ')') != 0
        || EmitAsmLabelAttr(c, foreign.arg0Start, foreign.arg0End, 1) != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, " {\n") != 0) {
        return -1;
    }
    EmitIndent(c, 1u);
    if (fnSig->returnType.baseName != NULL && StrEq(fnSig->returnType.baseName, "void")) {
        if (BufAppendCStr(&c->out, fnSig->cName) != 0 || BufAppendChar(&c->out, '(') != 0) {
            return -1;
        }
    } else if (
        BufAppendCStr(&c->out, "return ") != 0 || BufAppendCStr(&c->out, fnSig->cName) != 0
        || BufAppendChar(&c->out, '(') != 0)
    {
        return -1;
    }
    {
        uint32_t paramIndex;
        for (paramIndex = 0; paramIndex < fnSig->paramLen; paramIndex++) {
            const char* paramName =
                fnSig->paramNames != NULL ? fnSig->paramNames[paramIndex] : NULL;
            char fallback[24];
            if (paramIndex != 0u && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (paramName == NULL || paramName[0] == '\0') {
                BuildForeignArgName(fallback, (uint32_t)sizeof(fallback), paramIndex);
                paramName = fallback;
            }
            if (BufAppendCStr(&c->out, paramName) != 0) {
                return -1;
            }
        }
    }
    if (BufAppendCStr(&c->out, ");\n}\n") != 0) {
        return -1;
    }
    return 0;
}

static int EmitForeignImportVarLike(
    H2CBackendC*     c,
    int32_t          nodeId,
    const char*      localName,
    const H2TypeRef* type,
    int32_t          typeNode,
    int              isConst) {
    const char*        stubName = ForeignStubName(c, nodeId, isConst ? "const" : "var");
    H2CForeignDeclInfo foreign = { 0 };
    if (stubName == NULL || !GetForeignDeclInfo(c, nodeId, &foreign)
        || (foreign.kind != H2CForeignDecl_C_IMPORT && foreign.kind != H2CForeignDecl_WASM_IMPORT))
    {
        return -1;
    }
    EmitIndent(c, 0);
    if (BufAppendCStr(&c->out, "extern ") != 0) {
        return -1;
    }
    if (isConst && BufAppendCStr(&c->out, "const ") != 0) {
        return -1;
    }
    if ((typeNode >= 0 && EmitTypeWithName(c, typeNode, stubName) != 0)
        || (typeNode < 0 && EmitTypeRefWithName(c, type, stubName) != 0))
    {
        return -1;
    }
    if (foreign.kind == H2CForeignDecl_C_IMPORT) {
        if (EmitAsmLabelAttr(c, foreign.arg0Start, foreign.arg0End, 1) != 0) {
            return -1;
        }
    } else if (
        EmitWasmImportAttrs(
            c, foreign.arg0Start, foreign.arg0End, foreign.arg1Start, foreign.arg1End)
        != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, ";\n#define ") != 0 || BufAppendCStr(&c->out, localName) != 0
        || BufAppendChar(&c->out, ' ') != 0 || BufAppendCStr(&c->out, stubName) != 0
        || BufAppendChar(&c->out, '\n') != 0)
    {
        return -1;
    }
    return 0;
}

static const H2NameMap* _Nullable FindDeclMap(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return NULL;
    }
    if (IsTypeDeclKind(n->kind)) {
        return FindTypeDeclMapByNode(c, nodeId);
    }
    return FindNameBySlice(c, n->dataStart, n->dataEnd);
}

int IsMainFunctionNode(const H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    return n != NULL && n->kind == H2Ast_FN
        && SliceEq(c->unit->source, n->dataStart, n->dataEnd, "main");
}

int IsExplicitlyExportedNode(const H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    const H2NameMap* map;
    if (n == NULL) {
        return 0;
    }
    map = FindDeclMap((H2CBackendC*)c, nodeId);
    return map != NULL && map->kind == n->kind && map->isExported;
}

int IsExportedNode(const H2CBackendC* c, int32_t nodeId) {
    if (IsMainFunctionNode(c, nodeId)) {
        return 1;
    }
    return IsExplicitlyExportedNode(c, nodeId);
}

int IsExportedTypeNode(const H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    const H2NameMap* map;
    if (n == NULL || !IsTypeDeclKind(n->kind)) {
        return 0;
    }
    map = FindDeclMap((H2CBackendC*)c, nodeId);
    return map != NULL && IsTypeDeclKind(map->kind) && map->isExported;
}

static int SourceSliceHasPrefix(const char* src, uint32_t start, uint32_t end, const char* prefix) {
    uint32_t i = 0;
    if (end < start) {
        return 0;
    }
    while (prefix[i] != '\0') {
        if (start + i >= end || src[start + i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int IsBuiltinFormatterSourceFnName(const H2CBackendC* c, const H2AstNode* n) {
    return SliceEq(c->unit->source, n->dataStart, n->dataEnd, "format")
        || SliceEq(c->unit->source, n->dataStart, n->dataEnd, "format_str")
        || SourceSliceHasPrefix(c->unit->source, n->dataStart, n->dataEnd, "fmt_");
}

static int HasBuiltinFormatterTemplateInstance(const H2CBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->fnSigLen; i++) {
        const H2FnSig* sig = &c->fnSigs[i];
        if ((sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0
            && (StrHasPrefix(sig->cName, "builtin__format__ti")
                || StrHasPrefix(sig->cName, "builtin__format_str__ti")
                || StrHasPrefix(sig->cName, "builtin__fmt_")))
        {
            return 1;
        }
    }
    return 0;
}

int EmitEnumDecl(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    const H2NameMap* map = FindDeclMap(c, nodeId);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int              hasPayload = EnumDeclHasPayload(c, nodeId);
    int              first = 1;

    if (child >= 0) {
        const H2AstNode* firstChild = NodeAt(c, child);
        if (firstChild != NULL
            && (firstChild->kind == H2Ast_TYPE_NAME || firstChild->kind == H2Ast_TYPE_PTR
                || firstChild->kind == H2Ast_TYPE_REF || firstChild->kind == H2Ast_TYPE_MUTREF
                || firstChild->kind == H2Ast_TYPE_ARRAY || firstChild->kind == H2Ast_TYPE_VARRAY
                || firstChild->kind == H2Ast_TYPE_SLICE || firstChild->kind == H2Ast_TYPE_MUTSLICE
                || firstChild->kind == H2Ast_TYPE_OPTIONAL || firstChild->kind == H2Ast_TYPE_FN
                || firstChild->kind == H2Ast_TYPE_ANON_STRUCT
                || firstChild->kind == H2Ast_TYPE_ANON_UNION
                || firstChild->kind == H2Ast_TYPE_TUPLE))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }

    if (hasPayload) {
        int32_t scan = child;
        while (scan >= 0) {
            int32_t payloadTypeNode = EnumVariantPayloadTypeNode(c, scan);
            if (payloadTypeNode >= 0) {
                H2TypeRef payloadType;
                if (ParseTypeRef(c, payloadTypeNode, &payloadType) != 0 || !payloadType.valid) {
                    return -1;
                }
                CanonicalizeTypeRefBaseName(c, &payloadType);
                if (EnsureAnonTypeVisible(c, &payloadType, depth) != 0) {
                    return -1;
                }
            }
            scan = AstNextSibling(&c->ast, scan);
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
            const H2AstNode* item = NodeAt(c, child);
            if (item != NULL && item->kind == H2Ast_FIELD) {
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
        const H2AstNode* item = NodeAt(c, child);
        if (item != NULL && item->kind == H2Ast_FIELD) {
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
        const H2AstNode* firstChild = NodeAt(c, child);
        if (firstChild != NULL
            && (firstChild->kind == H2Ast_TYPE_NAME || firstChild->kind == H2Ast_TYPE_PTR
                || firstChild->kind == H2Ast_TYPE_REF || firstChild->kind == H2Ast_TYPE_MUTREF
                || firstChild->kind == H2Ast_TYPE_ARRAY || firstChild->kind == H2Ast_TYPE_VARRAY
                || firstChild->kind == H2Ast_TYPE_SLICE || firstChild->kind == H2Ast_TYPE_MUTSLICE
                || firstChild->kind == H2Ast_TYPE_OPTIONAL || firstChild->kind == H2Ast_TYPE_FN
                || firstChild->kind == H2Ast_TYPE_ANON_STRUCT
                || firstChild->kind == H2Ast_TYPE_ANON_UNION
                || firstChild->kind == H2Ast_TYPE_TUPLE))
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
        const H2AstNode* item = NodeAt(c, child);
        int32_t          payload = EnumVariantPayloadTypeNode(c, child);
        if (item != NULL && item->kind == H2Ast_FIELD && payload >= 0) {
            char* fieldName = DupSlice(c, c->unit->source, item->dataStart, item->dataEnd);
            if (fieldName == NULL) {
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (EmitTypeWithName(c, payload, fieldName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
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

int NodeHasDirectDependentFields(H2CBackendC* c, int32_t nodeId) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const H2AstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == H2Ast_FIELD) {
            int32_t          typeNode = AstFirstChild(&c->ast, child);
            const H2AstNode* tn = NodeAt(c, typeNode);
            if (tn != NULL && tn->kind == H2Ast_TYPE_VARRAY) {
                return 1;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int EmitVarSizeStructDecl(H2CBackendC* c, int32_t nodeId, uint32_t depth) {
    const H2NameMap* map = FindDeclMap(c, nodeId);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int              emittedHelper = 0;

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef struct ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, "__hdr {\n") != 0)
    {
        return -1;
    }

    while (child >= 0) {
        const H2AstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == H2Ast_FIELD) {
            int32_t          typeNode = AstFirstChild(&c->ast, child);
            const H2AstNode* tn = NodeAt(c, typeNode);
            char*            name = DupSlice(c, c->unit->source, field->dataStart, field->dataEnd);
            H2TypeRef        fieldType;
            const char*      varSizeBaseName = NULL;
            if (name == NULL) {
                return -1;
            }
            if (typeNode >= 0 && ParseTypeRef(c, typeNode, &fieldType) == 0) {
                varSizeBaseName = ResolveVarSizeValueBaseName(c, &fieldType);
            }
            if (tn != NULL && tn->kind == H2Ast_TYPE_VARRAY) {
            } else {
                EmitIndent(c, depth + 1u);
                if ((varSizeBaseName != NULL && IsStrBaseName(varSizeBaseName))
                        ? (BufAppendCStr(&c->out, varSizeBaseName) != 0
                           || BufAppendChar(&c->out, ' ') != 0 || BufAppendCStr(&c->out, name) != 0)
                        : (EmitTypeWithName(c, typeNode, name) != 0)
                              || BufAppendCStr(&c->out, ";\n") != 0)
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
        const H2AstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == H2Ast_FIELD) {
            int32_t          typeNode = AstFirstChild(&c->ast, child);
            const H2AstNode* tn = NodeAt(c, typeNode);
            if (tn != NULL && tn->kind == H2Ast_TYPE_VARRAY) {
                H2TypeRef depType;
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
                if (BufAppendCStr(&c->out, "__hop_int off = (__hop_int)sizeof(") != 0
                    || BufAppendCStr(&c->out, map->cName) != 0
                    || BufAppendCStr(&c->out, "__hdr);\n") != 0)
                {
                    return -1;
                }

                walk = AstFirstChild(&c->ast, nodeId);
                while (walk >= 0) {
                    const H2AstNode* wf = NodeAt(c, walk);
                    if (wf != NULL && wf->kind == H2Ast_FIELD) {
                        int32_t          wt = AstFirstChild(&c->ast, walk);
                        const H2AstNode* wtn = NodeAt(c, wt);
                        if (wtn != NULL && wtn->kind == H2Ast_TYPE_VARRAY) {
                            int32_t welem = AstFirstChild(&c->ast, wt);
                            EmitIndent(c, depth + 1u);
                            if (BufAppendCStr(&c->out, "off = __hop_align_up(off, _Alignof(") != 0
                                || EmitTypeForCast(c, welem) != 0
                                || BufAppendCStr(&c->out, "));\n") != 0)
                            {
                                return -1;
                            }
                            if (walk == child) {
                                EmitIndent(c, depth + 1u);
                                if (BufAppendCStr(&c->out, "return (") != 0
                                    || EmitTypeNameWithDepth(c, &depType) != 0
                                    || BufAppendCStr(&c->out, ")((__hop_u8*)p + off);\n") != 0)
                                {
                                    return -1;
                                }
                                break;
                            }
                            EmitIndent(c, depth + 1u);
                            if (BufAppendCStr(&c->out, "off += p->") != 0
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
                                EmitIndent(c, depth + 1u);
                                if (BufAppendCStr(&c->out, "off += ") != 0) {
                                    return -1;
                                }
                                if (IsStrBaseName(wVarSizeBaseName)) {
                                    if (BufAppendCStr(
                                            &c->out, "__hop_packed_str_size((__hop_str*)&p->")
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
        if (BufAppendCStr(&c->out, "static inline __hop_int ") != 0
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__sizeof(") != 0
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "* p) {\n") != 0)
        {
            return -1;
        }
        EmitIndent(c, depth + 1u);
        if (BufAppendCStr(&c->out, "__hop_int off = (__hop_int)sizeof(") != 0
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "__hdr);\n") != 0)
        {
            return -1;
        }
        while (walk >= 0) {
            const H2AstNode* wf = NodeAt(c, walk);
            if (wf != NULL && wf->kind == H2Ast_FIELD) {
                int32_t          wt = AstFirstChild(&c->ast, walk);
                const H2AstNode* wtn = NodeAt(c, wt);
                if (wtn != NULL && wtn->kind == H2Ast_TYPE_VARRAY) {
                    int32_t welem = AstFirstChild(&c->ast, wt);
                    EmitIndent(c, depth + 1u);
                    if (BufAppendCStr(&c->out, "off = __hop_align_up(off, _Alignof(") != 0
                        || EmitTypeForCast(c, welem) != 0 || BufAppendCStr(&c->out, "));\n") != 0)
                    {
                        return -1;
                    }
                    EmitIndent(c, depth + 1u);
                    if (BufAppendCStr(&c->out, "off += p->") != 0
                        || BufAppendSlice(&c->out, c->unit->source, wtn->dataStart, wtn->dataEnd)
                               != 0
                        || BufAppendCStr(&c->out, " * sizeof(") != 0
                        || EmitTypeForCast(c, welem) != 0 || BufAppendCStr(&c->out, ");\n") != 0)
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
                        EmitIndent(c, depth + 1u);
                        if (BufAppendCStr(&c->out, "off += ") != 0) {
                            return -1;
                        }
                        if (IsStrBaseName(wVarSizeBaseName)) {
                            if (BufAppendCStr(&c->out, "__hop_packed_str_size((__hop_str*)&p->")
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
        if (BufAppendCStr(&c->out, "off = __hop_align_up(off, _Alignof(") != 0
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

int EmitStructOrUnionDecl(H2CBackendC* c, int32_t nodeId, uint32_t depth, int isUnion) {
    const H2NameMap* map = FindDeclMap(c, nodeId);
    int32_t          child = AstFirstChild(&c->ast, nodeId);

    if (map != NULL && StrEq(map->cName, "builtin__SourceLocation")) {
        return 0;
    }

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
        const H2AstNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == H2Ast_FIELD) {
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

static int EmitTemplateInstanceStructOrUnionDecl(
    H2CBackendC* c, uint32_t tcNamedIndex, uint32_t depth, int forwardOnly) {
    H2TypeCheckCtx*      tc;
    const H2TCNamedType* nt;
    const H2AstNode*     decl;
    const H2NameMap*     rootMap;
    char*                cName;
    uint32_t             savedArgStart;
    uint16_t             savedArgCount;
    int32_t              savedDeclNode;
    uint32_t             savedFuncIndex;
    int32_t              savedNamedTypeIndex;
    int32_t              fieldNode;
    if (c == NULL || c->constEval == NULL) {
        return 0;
    }
    tc = &c->constEval->tc;
    if (tcNamedIndex >= tc->namedTypeLen) {
        return -1;
    }
    nt = &tc->namedTypes[tcNamedIndex];
    if (nt->templateRootNamedIndex < 0 || nt->templateArgCount == 0 || nt->typeId < 0
        || (uint32_t)nt->typeId >= tc->typeLen)
    {
        return 0;
    }
    {
        uint16_t argIndex;
        for (argIndex = 0; argIndex < nt->templateArgCount; argIndex++) {
            int32_t argType = tc->genericArgTypes[nt->templateArgStart + argIndex];
            if (argType >= 0 && (uint32_t)argType < tc->typeLen
                && tc->types[argType].kind == H2TCType_TYPE_PARAM)
            {
                return 0;
            }
        }
    }
    decl = NodeAt(c, nt->declNode);
    if (decl == NULL || (decl->kind != H2Ast_STRUCT && decl->kind != H2Ast_UNION)) {
        return 0;
    }
    rootMap = FindTypeDeclMapByNode(c, nt->declNode);
    if (rootMap == NULL) {
        return -1;
    }
    cName = BuildTemplateNamedTypeCName(c, rootMap->cName, tcNamedIndex);
    if (cName == NULL) {
        return -1;
    }
    EmitIndent(c, depth);
    if (forwardOnly) {
        if (BufAppendCStr(&c->out, "typedef ") != 0
            || BufAppendCStr(&c->out, decl->kind == H2Ast_UNION ? "union " : "struct ") != 0
            || BufAppendCStr(&c->out, cName) != 0 || BufAppendChar(&c->out, ' ') != 0
            || BufAppendCStr(&c->out, cName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "typedef ") != 0
        || BufAppendCStr(&c->out, decl->kind == H2Ast_UNION ? "union " : "struct ") != 0
        || BufAppendCStr(&c->out, cName) != 0 || BufAppendCStr(&c->out, " {\n") != 0)
    {
        return -1;
    }
    savedArgStart = tc->activeGenericArgStart;
    savedArgCount = tc->activeGenericArgCount;
    savedDeclNode = tc->activeGenericDeclNode;
    savedFuncIndex = c->activeTcFuncIndex;
    savedNamedTypeIndex = c->activeTcNamedTypeIndex;
    if (CodegenCPushActiveNamedTypeContext(c, tcNamedIndex) != 0) {
        return -1;
    }
    fieldNode = AstFirstChild(&c->ast, nt->declNode);
    while (fieldNode >= 0) {
        const H2AstNode* field = NodeAt(c, fieldNode);
        int32_t          typeNode;
        H2TypeRef        fieldType;
        char*            fieldName;
        if (field == NULL || field->kind != H2Ast_FIELD) {
            fieldNode = AstNextSibling(&c->ast, fieldNode);
            continue;
        }
        typeNode = AstFirstChild(&c->ast, fieldNode);
        if (typeNode < 0 || ParseTypeRef(c, typeNode, &fieldType) != 0) {
            CodegenCPopActiveTypeContext(
                c,
                savedFuncIndex,
                savedNamedTypeIndex,
                savedArgStart,
                savedArgCount,
                savedDeclNode);
            return -1;
        }
        fieldName = DupSlice(c, c->unit->source, field->dataStart, field->dataEnd);
        if (fieldName == NULL) {
            CodegenCPopActiveTypeContext(
                c,
                savedFuncIndex,
                savedNamedTypeIndex,
                savedArgStart,
                savedArgCount,
                savedDeclNode);
            return -1;
        }
        EmitIndent(c, depth + 1u);
        if (EmitTypeRefWithName(c, &fieldType, fieldName) != 0
            || BufAppendCStr(&c->out, ";\n") != 0)
        {
            CodegenCPopActiveTypeContext(
                c,
                savedFuncIndex,
                savedNamedTypeIndex,
                savedArgStart,
                savedArgCount,
                savedDeclNode);
            return -1;
        }
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }
    CodegenCPopActiveTypeContext(
        c, savedFuncIndex, savedNamedTypeIndex, savedArgStart, savedArgCount, savedDeclNode);
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, cName) != 0
        || BufAppendCStr(&c->out, ";\n") != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitTemplateInstanceTypeDecls(H2CBackendC* c, uint32_t depth, int forwardOnly) {
    H2TypeCheckCtx* tc;
    uint32_t        i;
    if (c == NULL || c->constEval == NULL) {
        return 0;
    }
    tc = &c->constEval->tc;
    for (i = 0; i < tc->namedTypeLen; i++) {
        if (EmitTemplateInstanceStructOrUnionDecl(c, i, depth, forwardOnly) != 0) {
            return -1;
        }
    }
    return 0;
}

int EmitForwardTypeDecls(H2CBackendC* c) {
    uint32_t i;
    int      emittedAny = 0;
    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
        const H2NameMap* map;
        if (n == NULL
            || (n->kind != H2Ast_STRUCT && n->kind != H2Ast_UNION && n->kind != H2Ast_ENUM))
        {
            continue;
        }
        if (CodegenCNodeHasTypeParams(c, nodeId)) {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        map = FindDeclMap(c, nodeId);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (StrEq(map->cName, "builtin__SourceLocation")) {
            if (BufAppendCStr(&c->out, "typedef __hop_SourceLocation builtin__SourceLocation;\n")
                != 0)
            {
                return -1;
            }
            emittedAny = 1;
            continue;
        }
        if (n->kind == H2Ast_ENUM) {
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
        } else if (n->kind == H2Ast_STRUCT && NodeHasDirectDependentFields(c, nodeId)) {
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
                || BufAppendCStr(&c->out, n->kind == H2Ast_UNION ? "union " : "struct ") != 0
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
        const H2AstNode* n = NodeAt(c, nodeId);
        const H2NameMap* map;
        if (n == NULL
            || (n->kind != H2Ast_STRUCT && n->kind != H2Ast_UNION && n->kind != H2Ast_ENUM))
        {
            continue;
        }
        if (CodegenCNodeHasTypeParams(c, nodeId)) {
            continue;
        }
        if (!ShouldEmitDeclNode(c, nodeId)) {
            continue;
        }
        map = FindDeclMap(c, nodeId);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (StrEq(map->cName, "builtin__SourceLocation")) {
            if (BufAppendCStr(&c->out, "typedef __hop_SourceLocation builtin__SourceLocation;\n")
                != 0)
            {
                return -1;
            }
            emittedAny = 1;
            continue;
        }
        if (n->kind == H2Ast_ENUM) {
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
        } else if (n->kind == H2Ast_STRUCT && NodeHasDirectDependentFields(c, nodeId)) {
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
                || BufAppendCStr(&c->out, n->kind == H2Ast_UNION ? "union " : "struct ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
        emittedAny = 1;
    }
    if (EmitTemplateInstanceTypeDecls(c, 0, 1) != 0) {
        return -1;
    }
    if (emittedAny && BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }
    return 0;
}

int EmitForwardAnonTypeDecls(H2CBackendC* c) {
    uint32_t i;
    if (c->anonTypeLen == 0) {
        return 0;
    }
    for (i = 0; i < c->anonTypeLen; i++) {
        const H2AnonTypeInfo* t = &c->anonTypes[i];
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

int EmitAnonTypeDecls(H2CBackendC* c) {
    uint32_t i;
    if (c->anonTypeLen == 0) {
        return 0;
    }
    for (i = 0; i < c->anonTypeLen; i++) {
        H2AnonTypeInfo* t = &c->anonTypes[i];
        uint32_t        j;
        for (j = 0; j < t->fieldCount; j++) {
            const H2FieldInfo* f = &c->fieldInfos[t->fieldStart + j];
            if (EnsureAnonTypeVisible(c, &f->type, 0) != 0) {
                return -1;
            }
        }
        if ((t->flags & H2AnonTypeFlag_EMITTED_GLOBAL) != 0) {
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
            const H2FieldInfo* f = &c->fieldInfos[t->fieldStart + j];
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
        t->flags |= H2AnonTypeFlag_EMITTED_GLOBAL;
    }
    return 0;
}

int EmitHeaderTypeAliasDecls(H2CBackendC* c) {
    uint32_t i;
    int      emittedAny = 0;
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != H2Ast_TYPE_ALIAS) {
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

int EmitFnTypeAliasDecls(H2CBackendC* c) {
    uint32_t i;
    if (c->fnTypeAliasLen == 0) {
        return 0;
    }
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        const H2FnTypeAlias* alias = &c->fnTypeAliases[i];
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
                H2Buf paramNameBuf = { 0 };
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

int FnNodeHasBody(const H2CBackendC* c, int32_t nodeId) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const H2AstNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == H2Ast_BLOCK) {
            return 1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int HasFunctionBodyForName(const H2CBackendC* c, int32_t nodeId) {
    const char* fnCName = FindFnCNameByNodeId(c, nodeId);
    uint32_t    i;
    if (fnCName == NULL) {
        return 0;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          otherId = c->topDecls[i].nodeId;
        const H2AstNode* other = NodeAt(c, otherId);
        const char*      otherCName;
        if (other == NULL || other->kind != H2Ast_FN || otherId == nodeId
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
    H2CBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          emitBody,
    int          isPrivate,
    const H2FnSig* _Nullable forcedSig) {
    const H2AstNode* n = NodeAt(c, nodeId);
    const char*      fnCName;
    const H2FnSig*   fnSig;
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          bodyNode = -1;
    int              firstParam = 1;
    int              isMainFn;
    int              hasFnContext;
    uint32_t         savedLocalLen;
    H2TypeRef        savedReturnType = c->currentReturnType;
    int              savedHasReturnType = c->hasCurrentReturnType;
    H2TypeRef        savedContextType = c->currentContextType;
    int              savedHasContext = c->hasCurrentContext;
    int              savedCurrentFunctionIsMain = c->currentFunctionIsMain;
    const char*      savedActivePackParamName = c->activePackParamName;
    char**           savedActivePackElemNames = c->activePackElemNames;
    H2TypeRef*       savedActivePackElemTypes = c->activePackElemTypes;
    uint32_t         savedActivePackElemCount = c->activePackElemCount;
    uint32_t         savedTcFuncIndex = c->activeTcFuncIndex;
    int32_t          savedTcNamedTypeIndex = c->activeTcNamedTypeIndex;
    uint32_t  savedTcArgStart = c->constEval != NULL ? c->constEval->tc.activeGenericArgStart : 0;
    uint16_t  savedTcArgCount = c->constEval != NULL ? c->constEval->tc.activeGenericArgCount : 0;
    int32_t   savedTcDeclNode = c->constEval != NULL ? c->constEval->tc.activeGenericDeclNode : -1;
    H2TypeRef fnReturnType;
    H2TypeRef fnContextType;
    H2TypeRef fnSemanticContextType;
    H2TypeRef fnContextParamType;
    H2TypeRef fnContextLocalType;
    int       forceStatic = 0;

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
    if (!emitBody && (fnSig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0
        && c->emitPrivateFnDeclStatic == 0)
    {
        return 0;
    }
    {
        H2CForeignDeclInfo foreign = { 0 };
        if (GetForeignDeclInfo(c, nodeId, &foreign)
            && (foreign.kind == H2CForeignDecl_C_IMPORT
                || foreign.kind == H2CForeignDecl_WASM_IMPORT))
        {
            return EmitForeignImportFn(c, nodeId, fnSig, emitBody, isPrivate);
        }
    }
    fnCName = fnSig->cName;
    isMainFn = IsMainFunctionNode(c, nodeId);
    hasFnContext = fnSig->hasContext || isMainFn;
    if (hasFnContext) {
        if (isMainFn) {
            TypeRefSetScalar(&fnContextType, "__hop_Context");
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
    forceStatic = ((fnSig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0)
               && (emitBody || c->emitPrivateFnDeclStatic);
    if ((fnSig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0 && fnSig->tcFuncIndex == UINT32_MAX) {
        forceStatic = 0;
    }
    if (forceStatic) {
        if (BufAppendCStr(&c->out, "static ") != 0) {
            return -1;
        }
    }

    while (child >= 0) {
        const H2AstNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == H2Ast_BLOCK) {
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
            if (BufAppendCStr(&c->out, "__hop_Context *context __attribute__((unused))") != 0) {
                return -1;
            }
        } else {
            if (EmitTypeRefWithName(c, &fnContextParamType, "context") != 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, " __attribute__((unused))") != 0) {
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
                    : "__hop_v";
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
                    : "__hop_v";
            if (AddLocal(c, paramName, fnSig->paramTypes[paramIndex]) != 0) {
                return -1;
            }
        }
    }
    c->activePackParamName = NULL;
    c->activePackElemNames = NULL;
    c->activePackElemTypes = NULL;
    c->activePackElemCount = 0;
    if ((fnSig->flags & H2FnSigFlag_EXPANDED_ANYPACK) != 0 && fnSig->packParamName != NULL
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
    if ((fnSig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0
        && CodegenCPushActiveFunctionTypeContext(c, fnSig->tcFuncIndex) != 0)
    {
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
    if (BufAppendChar(&c->out, ' ') != 0 || EmitBlockInline(c, bodyNode, depth) != 0) {
        CodegenCPopActiveTypeContext(
            c,
            savedTcFuncIndex,
            savedTcNamedTypeIndex,
            savedTcArgStart,
            savedTcArgCount,
            savedTcDeclNode);
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
    CodegenCPopActiveTypeContext(
        c,
        savedTcFuncIndex,
        savedTcNamedTypeIndex,
        savedTcArgStart,
        savedTcArgCount,
        savedTcDeclNode);
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
    if (emitBody) {
        H2CForeignDeclInfo foreign = { 0 };
        if (GetForeignDeclInfo(c, nodeId, &foreign) && foreign.kind == H2CForeignDecl_EXPORT) {
            if (EmitExportWrapper(c, nodeId, fnSig) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

int EmitConstDecl(
    H2CBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const H2AstNode*  n = NodeAt(c, nodeId);
    H2CCGVarLikeParts parts;
    uint32_t          i;
    H2TypeRef         sharedType;
    if (n == NULL) {
        return -1;
    }
    if (ResolveVarLikeParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (!parts.grouped) {
        const H2NameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        int32_t          typeNode = parts.typeNode;
        int32_t          initNode = parts.initNode;
        H2TypeRef        type;
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
        {
            H2CForeignDeclInfo foreign = { 0 };
            if (GetForeignDeclInfo(c, nodeId, &foreign)
                && (foreign.kind == H2CForeignDecl_C_IMPORT
                    || foreign.kind == H2CForeignDecl_WASM_IMPORT))
            {
                return EmitForeignImportVarLike(c, nodeId, map->cName, &type, typeNode, 1);
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
                    H2CTFEValue constValue;
                    int         isConst = 0;
                    if (H2ConstEvalSessionEvalTopLevelConst(
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
        const H2AstNode* nameAst = NodeAt(c, nameNode);
        const H2NameMap* map;
        int32_t          initNode = -1;
        H2TypeRef        type;
        if (nameAst == NULL) {
            return -1;
        }
        map = FindNameBySlice(c, nameAst->dataStart, nameAst->dataEnd);
        if (map == NULL) {
            return -1;
        }
        if (parts.initNode >= 0) {
            if (NodeAt(c, parts.initNode) == NULL
                || NodeAt(c, parts.initNode)->kind != H2Ast_EXPR_LIST)
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
    H2CBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const H2AstNode*  n = NodeAt(c, nodeId);
    H2CCGVarLikeParts parts;
    uint32_t          i;
    H2TypeRef         sharedType;
    if (n == NULL) {
        return -1;
    }
    if (ResolveVarLikeParts(c, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (!parts.grouped) {
        const H2NameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        int32_t          typeNode = parts.typeNode;
        int32_t          initNode = parts.initNode;
        H2TypeRef        type;
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
        {
            H2CForeignDeclInfo foreign = { 0 };
            if (GetForeignDeclInfo(c, nodeId, &foreign)
                && (foreign.kind == H2CForeignDecl_C_IMPORT
                    || foreign.kind == H2CForeignDecl_WASM_IMPORT))
            {
                return EmitForeignImportVarLike(c, nodeId, map->cName, &type, typeNode, 0);
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
        const H2AstNode* nameAst = NodeAt(c, nameNode);
        const H2NameMap* map;
        int32_t          initNode = -1;
        H2TypeRef        type;
        if (nameAst == NULL) {
            return -1;
        }
        map = FindNameBySlice(c, nameAst->dataStart, nameAst->dataEnd);
        if (map == NULL) {
            return -1;
        }
        if (parts.initNode >= 0) {
            if (NodeAt(c, parts.initNode) == NULL
                || NodeAt(c, parts.initNode)->kind != H2Ast_EXPR_LIST)
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
    H2CBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const H2NameMap* map = FindDeclMap(c, nodeId);
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
    H2CBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          declarationOnly,
    int          isPrivate,
    int          emitBody) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case H2Ast_STRUCT:
            return CodegenCNodeHasTypeParams(c, nodeId)
                     ? 0
                     : EmitStructOrUnionDecl(c, nodeId, depth, 0);
        case H2Ast_UNION:
            return CodegenCNodeHasTypeParams(c, nodeId)
                     ? 0
                     : EmitStructOrUnionDecl(c, nodeId, depth, 1);
        case H2Ast_ENUM: return EmitEnumDecl(c, nodeId, depth);
        case H2Ast_TYPE_ALIAS:
            return EmitTypeAliasDecl(c, nodeId, depth, declarationOnly, isPrivate);
        case H2Ast_FN: {
            const H2FnSig* sigs[H2CCG_MAX_CALL_CANDIDATES];
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
                if (CodegenCNodeHasTypeParams(c, nodeId)) {
                    return 0;
                }
                if (importedBeforeOwnOffset) {
                    return 0;
                }
                if (EmitFnDeclOrDef(c, nodeId, depth, emitBody, isPrivate, NULL) != 0) {
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, nodeId, H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
                return 0;
            }
            if (nSigs > (uint32_t)(sizeof(sigs) / sizeof(sigs[0]))) {
                nSigs = (uint32_t)(sizeof(sigs) / sizeof(sigs[0]));
            }
            for (i = 0; i < nSigs; i++) {
                if ((sigs[i]->flags & (H2FnSigFlag_TEMPLATE_BASE | H2FnSigFlag_TEMPLATE_INSTANCE))
                    != 0)
                {
                    continue;
                }
                if (importedBeforeOwnOffset) {
                    continue;
                }
                if (EmitFnDeclOrDef(c, nodeId, depth, emitBody, isPrivate, sigs[i]) != 0) {
                    if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                        SetDiagNode(c, nodeId, H2Diag_CODEGEN_INTERNAL);
                    }
                    return -1;
                }
                emitted = 1;
            }
            (void)emitted;
            return 0;
        }
        case H2Ast_VAR:   return EmitVarDecl(c, nodeId, depth, declarationOnly, isPrivate);
        case H2Ast_CONST: return EmitConstDecl(c, nodeId, depth, declarationOnly, isPrivate);
        default:          return 0;
    }
}

int EmitPrelude(H2CBackendC* c) {
    return BufAppendCStr(&c->out, "#include <builtin/builtin.h>\n");
}

char* _Nullable BuildDefaultMacro(H2CBackendC* c, const char* pkgName, const char* suffix) {
    H2Buf  b = { 0 };
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

int EmitHeader(H2CBackendC* c) {
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
    if (EmitTemplateInstanceTypeDecls(c, 0, 0) != 0) {
        return -1;
    }
    if (EmitFnTypeAliasDecls(c) != 0) {
        return -1;
    }

    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
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
        const H2AstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != H2Ast_FN || !FnNodeHasBody(c, nodeId)
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
        const H2AstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != H2Ast_TYPE_ALIAS || IsExportedTypeNode(c, nodeId)) {
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
        const H2AstNode* n = NodeAt(c, nodeId);
        int              exported;
        if (n == NULL || n->kind != H2Ast_FN || !FnNodeHasBody(c, nodeId)) {
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
        const H2FnSig*   sig = &c->fnSigs[i];
        const H2AstNode* fnNode;
        if ((sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0) {
            continue;
        }
        fnNode = NodeAt(c, sig->nodeId);
        if (fnNode == NULL || fnNode->kind != H2Ast_FN || !FnNodeHasBody(c, sig->nodeId)) {
            continue;
        }
        if (EmitFnDeclOrDef(c, sig->nodeId, 0, 0, 1, sig) != 0 || BufAppendChar(&c->out, '\n') != 0)
        {
            if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                SetDiagNode(c, sig->nodeId, H2Diag_CODEGEN_INTERNAL);
            }
            c->emitPrivateFnDeclStatic = 0;
            return -1;
        }
    }
    c->emitPrivateFnDeclStatic = 0;

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
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

        if (n->kind == H2Ast_FN) {
            if (FnNodeHasBody(c, nodeId)) {
                if (EmitDeclNode(c, nodeId, 0, 0, !exported, 1) != 0
                    || BufAppendChar(&c->out, '\n') != 0)
                {
                    return -1;
                }
            } else {
                H2CForeignDeclInfo foreign = { 0 };
                if (GetForeignDeclInfo(c, nodeId, &foreign)
                    && (foreign.kind == H2CForeignDecl_C_IMPORT
                        || foreign.kind == H2CForeignDecl_WASM_IMPORT))
                {
                    if (EmitDeclNode(c, nodeId, 0, 0, !exported, 1) != 0
                        || BufAppendChar(&c->out, '\n') != 0)
                    {
                        return -1;
                    }
                } else if (!exported && !HasFunctionBodyForName(c, nodeId)) {
                    if (EmitDeclNode(c, nodeId, 0, 1, 1, 0) != 0
                        || BufAppendChar(&c->out, '\n') != 0)
                    {
                        return -1;
                    }
                }
            }
            continue;
        }

        if (n->kind == H2Ast_CONST) {
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
        const H2FnSig*   sig = &c->fnSigs[i];
        const H2AstNode* fnNode;
        if ((sig->flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0) {
            continue;
        }
        fnNode = NodeAt(c, sig->nodeId);
        if (fnNode == NULL || fnNode->kind != H2Ast_FN || !FnNodeHasBody(c, sig->nodeId)) {
            continue;
        }
        if (EmitFnDeclOrDef(c, sig->nodeId, 0, 1, 1, sig) != 0 || BufAppendChar(&c->out, '\n') != 0)
        {
            if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
                SetDiagNode(c, sig->nodeId, H2Diag_CODEGEN_INTERNAL);
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

int ShouldEmitDeclNode(const H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n;
    if (c == NULL) {
        return 0;
    }
    n = NodeAt(c, nodeId);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_FN) {
        const H2NameMap* map = FindDeclMap((H2CBackendC*)c, nodeId);
        if (c->unit->packageName != NULL && StrEq(c->unit->packageName, "builtin")
            && IsBuiltinFormatterSourceFnName(c, n))
        {
            return 1;
        }
        if (map != NULL && map->cName != NULL
            && (StrEq(map->cName, "builtin__format") || StrEq(map->cName, "builtin__format_str")
                || StrHasPrefix(map->cName, "builtin__fmt_")))
        {
            return HasBuiltinFormatterTemplateInstance(c);
        }
    }
    if (c->options != NULL && c->options->emitNodeStartOffsetEnabled != 0
        && n->start < c->options->emitNodeStartOffset)
    {
        if (n->kind == H2Ast_FN) {
            const H2FnSig* sigs[H2CCG_MAX_CALL_CANDIDATES];
            uint32_t       nSigs = FindFnSigCandidatesByNodeId(
                c, nodeId, sigs, (uint32_t)(sizeof(sigs) / sizeof(sigs[0])));
            uint32_t i;
            if (nSigs > (uint32_t)(sizeof(sigs) / sizeof(sigs[0]))) {
                nSigs = (uint32_t)(sizeof(sigs) / sizeof(sigs[0]));
            }
            for (i = 0; i < nSigs; i++) {
                if ((sigs[i]->flags & H2FnSigFlag_TEMPLATE_INSTANCE) != 0) {
                    return 1;
                }
            }
        }
        return 0;
    }
    return 1;
}

int InitAst(H2CBackendC* c) {
    H2Diag diag = { 0 };
    void* _Nullable allocatorCtx = NULL;
    H2ArenaGrowFn _Nullable growFn = NULL;
    H2ArenaFreeFn _Nullable freeFn = NULL;

    c->ast.nodes = NULL;
    c->ast.len = 0;
    c->ast.root = -1;
    if (c->options != NULL) {
        allocatorCtx = c->options->allocatorCtx;
        growFn = c->options->arenaGrow;
        freeFn = c->options->arenaFree;
    }
    H2ArenaInitEx(
        &c->arena,
        c->arenaInlineStorage,
        (uint32_t)sizeof(c->arenaInlineStorage),
        allocatorCtx,
        growFn,
        freeFn);
    c->out.arena = &c->arena;
    if (H2Parse(
            &c->arena,
            (H2StrView){ c->unit->source, c->unit->sourceLen },
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

char* _Nullable AllocOutputCopy(H2CBackendC* c) {
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

void FreeContext(H2CBackendC* c) {
    H2ArenaDispose(&c->arena);
}

int EmitCBackend(
    const H2CodegenBackend* backend,
    const H2CodegenUnit*    unit,
    const H2CodegenOptions* _Nullable options,
    H2CodegenArtifact* _Nonnull outArtifact,
    H2Diag* _Nullable diag) {
    H2CBackendC c;
    (void)backend;

    memset(&c, 0, sizeof(c));
    c.unit = unit;
    c.options = options;
    c.diag = diag;
    c.activeCallWithNode = -1;
    c.activeTcFuncIndex = UINT32_MAX;
    c.activeTcNamedTypeIndex = -1;
    TypeRefSetInvalid(&c.currentContextType);

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    *outArtifact = (H2CodegenArtifact){ 0 };

    if (InitAst(&c) != 0) {
        FreeContext(&c);
        return -1;
    }
    if (H2ConstEvalSessionInit(
            &c.arena, &c.ast, (H2StrView){ c.unit->source, c.unit->sourceLen }, &c.constEval, diag)
        != 0)
    {
        FreeContext(&c);
        return -1;
    }
    if (CollectDeclSets(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            SetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectFnAndFieldInfo(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            SetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectTypeAliasInfo(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            SetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectFnTypeAliases(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            SetDiag(diag, H2Diag_CODEGEN_INTERNAL, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectVarSizeTypesFromDeclSets(&c) != 0 || PropagateVarSizeTypes(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            SetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (CollectStringLiterals(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            SetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }
    if (EmitHeader(&c) != 0) {
        if (diag != NULL && diag->code == H2Diag_NONE) {
            SetDiag(diag, H2Diag_CODEGEN_INTERNAL, 0, 0);
        }
        FreeContext(&c);
        return -1;
    }

    outArtifact->data = (uint8_t*)AllocOutputCopy(&c);
    if (outArtifact->data == NULL) {
        SetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        FreeContext(&c);
        return -1;
    }
    outArtifact->len = (uint32_t)StrLen((const char*)outArtifact->data);
    outArtifact->isBinary = 0;

    FreeContext(&c);
    return 0;
}

const H2CodegenBackend gHOPCodegenBackendC = {
    .name = "c",
    .emit = EmitCBackend,
};

H2_API_END
