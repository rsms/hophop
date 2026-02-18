#include "libsl-impl.h"
#include "slc_codegen.h"

SL_API_BEGIN

typedef struct {
    const char* baseName;
    int         ptrDepth;
    int         valid;
    int         containerKind; /* 0 scalar, 1 array, 2 ro-slice, 3 mut-slice */
    int         containerPtrDepth;
    uint32_t    arrayLen;
    int         hasArrayLen;
    int         readOnly;
} SLTypeRef;

typedef struct {
    SLArena* _Nullable arena;
    char*    v;
    uint32_t len;
    uint32_t cap;
} SLBuf;

typedef struct {
    char*     name;
    char*     cName;
    SLAstKind kind;
    int       isExported;
} SLNameMap;

typedef struct {
    int32_t nodeId;
} SLNodeRef;

typedef struct {
    char*      name;
    SLTypeRef  returnType;
    SLTypeRef* paramTypes;
    uint32_t   paramLen;
} SLFnSig;

typedef struct {
    char*     ownerType;
    char*     fieldName;
    char*     lenFieldName;
    int       isDependent;
    SLTypeRef type;
} SLFieldInfo;

typedef struct {
    char* cName;
    int   isUnion;
    int   isVarSize;
} SLVarSizeType;

typedef struct {
    char*     name;
    SLTypeRef type;
} SLLocal;

typedef struct {
    uint8_t* _Nullable bytes;
    uint32_t len;
} SLStringLiteral;

typedef struct {
    const SLCodegenUnit*    unit;
    const SLCodegenOptions* options;
    SLDiag*                 diag;

    SLArena arena;
    uint8_t arenaInlineStorage[16384];
    SLAst   ast;

    SLBuf out;

    SLNameMap* names;
    uint32_t   nameLen;
    uint32_t   nameCap;

    SLNodeRef* pubDecls;
    uint32_t   pubDeclLen;
    uint32_t   pubDeclCap;

    SLNodeRef* topDecls;
    uint32_t   topDeclLen;
    uint32_t   topDeclCap;

    SLFnSig* fnSigs;
    uint32_t fnSigLen;
    uint32_t fnSigCap;

    SLFieldInfo* fieldInfos;
    uint32_t     fieldInfoLen;
    uint32_t     fieldInfoCap;

    SLVarSizeType* varSizeTypes;
    uint32_t       varSizeTypeLen;
    uint32_t       varSizeTypeCap;

    SLLocal* locals;
    uint32_t localLen;
    uint32_t localCap;

    uint32_t* localScopeMarks;
    uint32_t  localScopeLen;
    uint32_t  localScopeCap;

    int32_t* deferredStmtNodes;
    uint32_t deferredStmtLen;
    uint32_t deferredStmtCap;

    uint32_t* deferScopeMarks;
    uint32_t  deferScopeLen;
    uint32_t  deferScopeCap;

    SLStringLiteral* stringLits;
    uint32_t         stringLitLen;
    uint32_t         stringLitCap;

    int32_t* stringLitByNode;
    uint32_t stringLitByNodeLen;

    int       emitPrivateFnDeclStatic;
    SLTypeRef currentReturnType;
    int       hasCurrentReturnType;
} SLCBackendC;

static size_t StrLen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int StrEq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int IsAlnumChar(char ch) {
    unsigned char c = (unsigned char)ch;
    return (c >= (unsigned char)'0' && c <= (unsigned char)'9')
        || (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

static char ToUpperChar(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c >= (unsigned char)'a' && c <= (unsigned char)'z') {
        c = (unsigned char)(c - (unsigned char)'a' + (unsigned char)'A');
    }
    return (char)c;
}

static void SetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

static int EnsureCapArena(
    SLArena* arena, void** ptr, uint32_t* cap, uint32_t need, size_t elemSize, uint32_t align) {
    uint32_t newCap;
    uint64_t allocSize64;
    uint32_t allocSize;
    void*    newPtr;

    if (need <= *cap) {
        return 0;
    }
    newCap = *cap == 0 ? 8u : *cap;
    while (newCap < need) {
        if (newCap > UINT32_MAX / 2u) {
            newCap = need;
            break;
        }
        newCap *= 2u;
    }
    allocSize64 = (uint64_t)newCap * (uint64_t)elemSize;
    if (allocSize64 > UINT32_MAX) {
        return -1;
    }
    allocSize = (uint32_t)allocSize64;
    newPtr = SLArenaAlloc(arena, allocSize, align);
    if (newPtr == NULL) {
        return -1;
    }
    if (*ptr != NULL && *cap > 0) {
        memcpy(newPtr, *ptr, (size_t)(*cap) * elemSize);
    }
    *ptr = newPtr;
    *cap = newCap;
    return 0;
}

static int BufReserve(SLBuf* b, uint32_t extra) {
    uint32_t need;
    if (UINT32_MAX - b->len < extra + 1u) {
        return -1;
    }
    need = b->len + extra + 1u;
    if (b->arena == NULL) {
        return -1;
    }
    return EnsureCapArena(
        b->arena, (void**)&b->v, &b->cap, need, sizeof(char), (uint32_t)_Alignof(char));
}

static int BufAppend(SLBuf* b, const char* s, uint32_t len) {
    if (len == 0) {
        return 0;
    }
    if (BufReserve(b, len) != 0) {
        return -1;
    }
    memcpy(b->v + b->len, s, len);
    b->len += len;
    b->v[b->len] = '\0';
    return 0;
}

static int BufAppendCStr(SLBuf* b, const char* s) {
    return BufAppend(b, s, (uint32_t)StrLen(s));
}

static int BufAppendChar(SLBuf* b, char c) {
    return BufAppend(b, &c, 1u);
}

static int BufAppendU32(SLBuf* b, uint32_t value) {
    char     tmp[16];
    uint32_t i = 0;
    uint32_t n = value;

    if (n == 0) {
        return BufAppendChar(b, '0');
    }

    while (n > 0 && i < (uint32_t)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (n % 10u));
        n /= 10u;
    }
    while (i > 0) {
        if (BufAppendChar(b, tmp[--i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int BufAppendSlice(SLBuf* b, const char* src, uint32_t start, uint32_t end) {
    if (end < start) {
        return -1;
    }
    return BufAppend(b, src + start, end - start);
}

static char* _Nullable BufFinish(SLBuf* b) {
    char* out;
    if (b->v == NULL) {
        if (b->arena == NULL) {
            return NULL;
        }
        out = (char*)SLArenaAlloc(b->arena, 1u, (uint32_t)_Alignof(char));
        if (out == NULL) {
            return NULL;
        }
        out[0] = '\0';
        return out;
    }
    out = b->v;
    b->v = NULL;
    b->len = 0;
    b->cap = 0;
    return out;
}

static void EmitIndent(SLCBackendC* c, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        (void)BufAppendCStr(&c->out, "    ");
    }
}

static int IsBuiltinType(const char* s) {
    return StrEq(s, "void") || StrEq(s, "bool") || StrEq(s, "str") || StrEq(s, "MemAllocator")
        || StrEq(s, "u8") || StrEq(s, "u16") || StrEq(s, "u32") || StrEq(s, "u64") || StrEq(s, "i8")
        || StrEq(s, "i16") || StrEq(s, "i32") || StrEq(s, "i64") || StrEq(s, "uint")
        || StrEq(s, "int") || StrEq(s, "f32") || StrEq(s, "f64");
}

static int SliceEq(const char* src, uint32_t start, uint32_t end, const char* s) {
    uint32_t i = 0;
    uint32_t len;
    if (end < start) {
        return 0;
    }
    len = end - start;
    while (i < len) {
        if (s[i] == '\0' || src[start + i] != s[i]) {
            return 0;
        }
        i++;
    }
    return s[i] == '\0';
}

static int SliceEqName(const char* src, uint32_t start, uint32_t end, const char* s) {
    return SliceEq(src, start, end, s);
}

static int SliceSpanEq(
    const char* src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != (bEnd - bStart)) {
        return 0;
    }
    return memcmp(src + aStart, src + bStart, (size_t)len) == 0;
}

enum {
    SLTypeContainer_SCALAR = 0,
    SLTypeContainer_ARRAY = 1,
    SLTypeContainer_SLICE_RO = 2,
    SLTypeContainer_SLICE_MUT = 3,
};

static void TypeRefSetInvalid(SLTypeRef* t) {
    t->baseName = NULL;
    t->ptrDepth = 0;
    t->valid = 0;
    t->containerKind = SLTypeContainer_SCALAR;
    t->containerPtrDepth = 0;
    t->arrayLen = 0;
    t->hasArrayLen = 0;
    t->readOnly = 0;
}

static void TypeRefSetScalar(SLTypeRef* t, const char* baseName) {
    t->baseName = baseName;
    t->ptrDepth = 0;
    t->valid = 1;
    t->containerKind = SLTypeContainer_SCALAR;
    t->containerPtrDepth = 0;
    t->arrayLen = 0;
    t->hasArrayLen = 0;
    t->readOnly = 0;
}

static int ParseArrayLenLiteral(const char* src, uint32_t start, uint32_t end, uint32_t* outLen) {
    uint64_t v = 0;
    uint32_t i;
    if (end <= start) {
        return -1;
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)src[i];
        if (ch < (unsigned char)'0' || ch > (unsigned char)'9') {
            return -1;
        }
        v = v * 10u + (uint64_t)(ch - (unsigned char)'0');
        if (v > (uint64_t)UINT32_MAX) {
            return -1;
        }
    }
    *outLen = (uint32_t)v;
    return 0;
}

static char* _Nullable DupSlice(SLCBackendC* c, const char* src, uint32_t start, uint32_t end) {
    uint32_t len;
    char*    out;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    out = (char*)SLArenaAlloc(&c->arena, len + 1u, (uint32_t)_Alignof(char));
    if (out == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
    return out;
}

static char* _Nullable DupAndReplaceDots(
    SLCBackendC* c, const char* src, uint32_t start, uint32_t end) {
    char*    out;
    uint32_t i;
    uint32_t len;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    out = (char*)SLArenaAlloc(&c->arena, len + 1u, (uint32_t)_Alignof(char));
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        char ch = src[start + i];
        out[i] = ch == '.' ? '_' : ch;
    }
    out[len] = '\0';
    return out;
}

static char* _Nullable DupCStr(SLCBackendC* c, const char* s) {
    size_t n = StrLen(s);
    char*  out;
    if (n > UINT32_MAX - 1u) {
        return NULL;
    }
    out = (char*)SLArenaAlloc(&c->arena, (uint32_t)n + 1u, (uint32_t)_Alignof(char));
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1u);
    return out;
}

static int HexDigitValue(unsigned char c) {
    if (c >= (unsigned char)'0' && c <= (unsigned char)'9') {
        return (int)(c - (unsigned char)'0');
    }
    if (c >= (unsigned char)'a' && c <= (unsigned char)'f') {
        return 10 + (int)(c - (unsigned char)'a');
    }
    if (c >= (unsigned char)'A' && c <= (unsigned char)'F') {
        return 10 + (int)(c - (unsigned char)'A');
    }
    return -1;
}

static int DecodeStringLiteral(
    SLCBackendC* c,
    const char*  src,
    uint32_t     start,
    uint32_t     end,
    uint8_t**    outBytes,
    uint32_t*    outLen) {
    uint8_t* bytes = NULL;
    uint32_t len = 0;
    uint32_t cap = 0;
    uint32_t i;

    if (end <= start + 1u || src[start] != '"' || src[end - 1u] != '"') {
        return -1;
    }

    i = start + 1u;
    while (i < end - 1u) {
        unsigned char ch = (unsigned char)src[i++];
        if (ch == (unsigned char)'\\') {
            unsigned char esc;
            if (i >= end - 1u) {
                return -1;
            }
            esc = (unsigned char)src[i++];
            switch (esc) {
                case (unsigned char)'\\': ch = (unsigned char)'\\'; break;
                case (unsigned char)'"':  ch = (unsigned char)'"'; break;
                case (unsigned char)'n':  ch = (unsigned char)'\n'; break;
                case (unsigned char)'t':  ch = (unsigned char)'\t'; break;
                case (unsigned char)'r':  ch = (unsigned char)'\r'; break;
                case (unsigned char)'0':  ch = (unsigned char)'\0'; break;
                case (unsigned char)'x':  {
                    int hi;
                    int lo;
                    if (i + 1u >= end - 1u) {
                        return -1;
                    }
                    hi = HexDigitValue((unsigned char)src[i]);
                    lo = HexDigitValue((unsigned char)src[i + 1u]);
                    if (hi < 0 || lo < 0) {
                        return -1;
                    }
                    ch = (unsigned char)((hi << 4) | lo);
                    i += 2u;
                    break;
                }
                default: ch = esc; break;
            }
        }
        if (EnsureCapArena(
                &c->arena,
                (void**)&bytes,
                &cap,
                len + 1u,
                sizeof(uint8_t),
                (uint32_t)_Alignof(uint8_t))
            != 0)
        {
            return -1;
        }
        bytes[len++] = (uint8_t)ch;
    }

    *outBytes = bytes;
    *outLen = len;
    return 0;
}

static int GetOrAddStringLiteral(
    SLCBackendC* c, uint32_t start, uint32_t end, int32_t* outLiteralId) {
    uint8_t* decoded = NULL;
    uint32_t decodedLen = 0;
    uint32_t i;

    if (DecodeStringLiteral(c, c->unit->source, start, end, &decoded, &decodedLen) != 0) {
        SetDiag(c->diag, SLDiag_UNEXPECTED_TOKEN, start, end);
        return -1;
    }

    for (i = 0; i < c->stringLitLen; i++) {
        if (c->stringLits[i].len == decodedLen
            && ((decodedLen == 0)
                || memcmp(c->stringLits[i].bytes, decoded, (size_t)decodedLen) == 0))
        {
            *outLiteralId = (int32_t)i;
            return 0;
        }
    }

    if (EnsureCapArena(
            &c->arena,
            (void**)&c->stringLits,
            &c->stringLitCap,
            c->stringLitLen + 1u,
            sizeof(SLStringLiteral),
            (uint32_t)_Alignof(SLStringLiteral))
        != 0)
    {
        SetDiag(c->diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }

    c->stringLits[c->stringLitLen].bytes = decoded;
    c->stringLits[c->stringLitLen].len = decodedLen;
    *outLiteralId = (int32_t)c->stringLitLen;
    c->stringLitLen++;
    return 0;
}

static int32_t AstFirstChild(const SLAst* ast, int32_t nodeId);
static int32_t AstNextSibling(const SLAst* ast, int32_t nodeId);

static int CollectStringLiterals(SLCBackendC* c) {
    uint32_t nodeId;

    c->stringLitByNodeLen = c->ast.len;
    c->stringLitByNode = (int32_t*)SLArenaAlloc(
        &c->arena, c->stringLitByNodeLen * (uint32_t)sizeof(int32_t), (uint32_t)_Alignof(int32_t));
    if (c->stringLitByNode == NULL) {
        return -1;
    }
    for (nodeId = 0; nodeId < c->stringLitByNodeLen; nodeId++) {
        c->stringLitByNode[nodeId] = -1;
    }

    for (nodeId = 0; nodeId < c->ast.len; nodeId++) {
        const SLAstNode* n = &c->ast.nodes[nodeId];
        if (n->kind == SLAst_STRING) {
            uint32_t scanNodeId;
            int      skip = 0;
            for (scanNodeId = 0; scanNodeId < c->ast.len; scanNodeId++) {
                const SLAstNode* parent = &c->ast.nodes[scanNodeId];
                if (parent->kind == SLAst_ASSERT) {
                    int32_t condNode = AstFirstChild(&c->ast, (int32_t)scanNodeId);
                    int32_t fmtNode = AstNextSibling(&c->ast, condNode);
                    if (fmtNode == (int32_t)nodeId) {
                        skip = 1;
                        break;
                    }
                }
            }
            if (skip) {
                continue;
            }
            int32_t literalId;
            if (GetOrAddStringLiteral(c, n->dataStart, n->dataEnd, &literalId) != 0) {
                return -1;
            }
            c->stringLitByNode[nodeId] = literalId;
        }
    }
    return 0;
}

static int HasDoubleUnderscore(const char* s) {
    const char* p = s;
    while (*p != '\0') {
        if (p[0] == '_' && p[1] == '_') {
            return 1;
        }
        p++;
    }
    return 0;
}

static int IsTypeDeclKind(SLAstKind kind) {
    return kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM;
}

static int IsDeclKind(SLAstKind kind) {
    return kind == SLAst_FN || kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_CONST;
}

static int IsPubDeclNode(const SLAstNode* n) {
    return (n->flags & SLAstFlag_PUB) != 0;
}

static int32_t AstFirstChild(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int32_t AstNextSibling(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

static const SLAstNode* _Nullable NodeAt(const SLCBackendC* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast.len) {
        return NULL;
    }
    return &c->ast.nodes[nodeId];
}

static int GetDeclNameSpan(
    const SLCBackendC* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL || !IsDeclKind(n->kind) || n->dataEnd <= n->dataStart) {
        return -1;
    }
    *outStart = n->dataStart;
    *outEnd = n->dataEnd;
    return 0;
}

static int AddName(
    SLCBackendC* c, uint32_t nameStart, uint32_t nameEnd, SLAstKind kind, int isExported) {
    uint32_t i;
    char*    name;
    char*    cName;
    SLBuf    tmp = { 0 };

    tmp.arena = &c->arena;

    for (i = 0; i < c->nameLen; i++) {
        if (SliceEqName(c->unit->source, nameStart, nameEnd, c->names[i].name)) {
            if (isExported) {
                c->names[i].isExported = 1;
            }
            return 0;
        }
    }

    name = DupSlice(c, c->unit->source, nameStart, nameEnd);
    if (name == NULL) {
        return -1;
    }

    if (HasDoubleUnderscore(name)) {
        cName = DupCStr(c, name);
    } else {
        if (BufAppendCStr(&tmp, c->unit->packageName) != 0 || BufAppendCStr(&tmp, "__") != 0
            || BufAppendCStr(&tmp, name) != 0)
        {
            return -1;
        }
        cName = BufFinish(&tmp);
    }
    if (cName == NULL) {
        return -1;
    }

    if (EnsureCapArena(
            &c->arena,
            (void**)&c->names,
            &c->nameCap,
            c->nameLen + 1u,
            sizeof(SLNameMap),
            (uint32_t)_Alignof(SLNameMap))
        != 0)
    {
        return -1;
    }
    c->names[c->nameLen].name = name;
    c->names[c->nameLen].cName = cName;
    c->names[c->nameLen].kind = kind;
    c->names[c->nameLen].isExported = isExported;
    c->nameLen++;
    return 0;
}

static const SLNameMap* _Nullable FindNameBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->names[i].name)) {
            return &c->names[i];
        }
    }
    return NULL;
}

static const SLNameMap* _Nullable FindNameByCString(const SLCBackendC* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (StrEq(c->names[i].name, name)) {
            return &c->names[i];
        }
    }
    return NULL;
}

static const char* _Nullable ResolveTypeName(SLCBackendC* c, uint32_t start, uint32_t end) {
    const SLNameMap*         mapped;
    char*                    normalized;
    uint32_t                 i;
    static const char* const builtinSlNames[] = {
        "void", "bool", "str", "MemAllocator", "u8",   "u16", "u32", "u64",
        "i8",   "i16",  "i32", "i64",          "uint", "int", "f32", "f64",
    };
    static const char* const builtinCNames[] = {
        "void",      "__sl_bool", "__sl_str", "__sl_MemAllocator", "__sl_u8",  "__sl_u16",
        "__sl_u32",  "__sl_u64",  "__sl_i8",  "__sl_i16",          "__sl_i32", "__sl_i64",
        "__sl_uint", "__sl_int",  "__sl_f32", "__sl_f64",
    };

    normalized = DupAndReplaceDots(c, c->unit->source, start, end);
    if (normalized == NULL) {
        return NULL;
    }

    if (IsBuiltinType(normalized)) {
        for (i = 0; i < (uint32_t)(sizeof(builtinSlNames) / sizeof(builtinSlNames[0])); i++) {
            if (StrEq(normalized, builtinSlNames[i])) {
                return builtinCNames[i];
            }
        }
        return "void";
    }

    mapped = FindNameByCString(c, normalized);
    if (mapped != NULL && IsTypeDeclKind(mapped->kind)) {
        return mapped->cName;
    }

    return normalized;
}

static const char* _Nullable ResolveTypeNameFromExprArg(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return NULL;
    }
    if (n->kind == SLAst_IDENT) {
        return ResolveTypeName(c, n->dataStart, n->dataEnd);
    }
    return NULL;
}

static int AddNodeRef(
    SLCBackendC* c, SLNodeRef** arr, uint32_t* len, uint32_t* cap, int32_t nodeId) {
    if (EnsureCapArena(
            &c->arena,
            (void**)arr,
            cap,
            *len + 1u,
            sizeof(SLNodeRef),
            (uint32_t)_Alignof(SLNodeRef))
        != 0)
    {
        return -1;
    }
    (*arr)[*len].nodeId = nodeId;
    (*len)++;
    return 0;
}

static int CollectDeclSets(SLCBackendC* c) {
    int32_t child = AstFirstChild(&c->ast, c->ast.root);
    while (child >= 0) {
        const SLAstNode* n = NodeAt(c, child);
        uint32_t         start;
        uint32_t         end;
        int              isExported;
        if (n == NULL) {
            return -1;
        }
        if (IsDeclKind(n->kind)) {
            isExported = IsPubDeclNode(n);
            if (AddNodeRef(c, &c->topDecls, &c->topDeclLen, &c->topDeclCap, child) != 0) {
                return -1;
            }
            if (isExported) {
                if (AddNodeRef(c, &c->pubDecls, &c->pubDeclLen, &c->pubDeclCap, child) != 0) {
                    return -1;
                }
            }
            if (GetDeclNameSpan(c, child, &start, &end) == 0) {
                if (AddName(c, start, end, n->kind, isExported) != 0) {
                    return -1;
                }
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

static int ParseTypeRef(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    switch (n->kind) {
        case SLAst_TYPE_NAME: {
            const char* name = ResolveTypeName(c, n->dataStart, n->dataEnd);
            if (name == NULL) {
                return -1;
            }
            TypeRefSetScalar(outType, name);
            return 0;
        }
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            SLTypeRef childType;
            int       isReadOnlyRef = (n->kind == SLAst_TYPE_REF);
            int       isRef = (n->kind == SLAst_TYPE_REF || n->kind == SLAst_TYPE_MUTREF);
            if (ParseTypeRef(c, child, &childType) != 0) {
                return -1;
            }
            if (childType.containerKind == SLTypeContainer_ARRAY
                || childType.containerKind == SLTypeContainer_SLICE_RO
                || childType.containerKind == SLTypeContainer_SLICE_MUT)
            {
                childType.containerPtrDepth++;
                if (isRef) {
                    childType.readOnly = isReadOnlyRef;
                } else {
                    childType.readOnly = 0;
                }
            } else {
                childType.ptrDepth++;
            }
            *outType = childType;
            return 0;
        }
        case SLAst_TYPE_ARRAY: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            SLTypeRef elemType;
            uint32_t  len = 0;
            if (ParseTypeRef(c, child, &elemType) != 0) {
                return -1;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (ParseArrayLenLiteral(c->unit->source, n->dataStart, n->dataEnd, &len) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.containerKind = SLTypeContainer_ARRAY;
            elemType.containerPtrDepth = 0;
            elemType.arrayLen = len;
            elemType.hasArrayLen = 1;
            elemType.readOnly = 0;
            *outType = elemType;
            return 0;
        }
        case SLAst_TYPE_VARRAY: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            SLTypeRef elemType;
            if (ParseTypeRef(c, child, &elemType) != 0) {
                return -1;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.ptrDepth++;
            *outType = elemType;
            return 0;
        }
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            SLTypeRef elemType;
            if (ParseTypeRef(c, child, &elemType) != 0) {
                return -1;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.containerKind =
                n->kind == SLAst_TYPE_MUTSLICE
                    ? SLTypeContainer_SLICE_MUT
                    : SLTypeContainer_SLICE_RO;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            elemType.readOnly = n->kind == SLAst_TYPE_MUTSLICE ? 0 : 1;
            *outType = elemType;
            return 0;
        }
        case SLAst_TYPE_OPTIONAL: {
            /* Emit the inner type; C has no optional, so we just pass through. */
            int32_t child = AstFirstChild(&c->ast, nodeId);
            return ParseTypeRef(c, child, outType);
        }
        default: TypeRefSetInvalid(outType); return -1;
    }
}

static int AddFnSig(
    SLCBackendC* c,
    const char*  name,
    SLTypeRef    returnType,
    SLTypeRef*   paramTypes,
    uint32_t     paramLen) {
    uint32_t i;
    for (i = 0; i < c->fnSigLen; i++) {
        if (StrEq(c->fnSigs[i].name, name)) {
            c->fnSigs[i].returnType = returnType;
            c->fnSigs[i].paramTypes = paramTypes;
            c->fnSigs[i].paramLen = paramLen;
            return 0;
        }
    }
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->fnSigs,
            &c->fnSigCap,
            c->fnSigLen + 1u,
            sizeof(SLFnSig),
            (uint32_t)_Alignof(SLFnSig))
        != 0)
    {
        return -1;
    }
    c->fnSigs[c->fnSigLen].name = (char*)name;
    c->fnSigs[c->fnSigLen].returnType = returnType;
    c->fnSigs[c->fnSigLen].paramTypes = paramTypes;
    c->fnSigs[c->fnSigLen].paramLen = paramLen;
    c->fnSigLen++;
    return 0;
}

static int AddFieldInfo(
    SLCBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int       isDependent,
    SLTypeRef type) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->fieldInfos,
            &c->fieldInfoCap,
            c->fieldInfoLen + 1u,
            sizeof(SLFieldInfo),
            (uint32_t)_Alignof(SLFieldInfo))
        != 0)
    {
        return -1;
    }
    c->fieldInfos[c->fieldInfoLen].ownerType = (char*)ownerType;
    c->fieldInfos[c->fieldInfoLen].fieldName = (char*)fieldName;
    c->fieldInfos[c->fieldInfoLen].lenFieldName = (char*)lenFieldName;
    c->fieldInfos[c->fieldInfoLen].isDependent = isDependent;
    c->fieldInfos[c->fieldInfoLen].type = type;
    c->fieldInfoLen++;
    return 0;
}

static const SLFnSig* _Nullable FindFnSigBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t         i;
    const SLNameMap* map = FindNameBySlice(c, start, end);
    if (map != NULL) {
        for (i = 0; i < c->fnSigLen; i++) {
            if (StrEq(c->fnSigs[i].name, map->cName)) {
                return &c->fnSigs[i];
            }
        }
    }
    for (i = 0; i < c->fnSigLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->fnSigs[i].name)) {
            return &c->fnSigs[i];
        }
    }
    return NULL;
}

static const SLFieldInfo* _Nullable FindFieldInfo(
    const SLCBackendC* c, const char* ownerType, uint32_t fieldStart, uint32_t fieldEnd) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        if (StrEq(c->fieldInfos[i].ownerType, ownerType)
            && SliceEqName(c->unit->source, fieldStart, fieldEnd, c->fieldInfos[i].fieldName))
        {
            return &c->fieldInfos[i];
        }
    }
    return NULL;
}

static int CollectFnAndFieldInfoFromNode(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    uint32_t         nameStart;
    uint32_t         nameEnd;
    const SLNameMap* mapName;

    if (n == NULL || !IsDeclKind(n->kind) || GetDeclNameSpan(c, nodeId, &nameStart, &nameEnd) != 0)
    {
        return 0;
    }
    mapName = FindNameBySlice(c, nameStart, nameEnd);
    if (mapName == NULL) {
        return 0;
    }

    if (n->kind == SLAst_FN) {
        int32_t    child = AstFirstChild(&c->ast, nodeId);
        SLTypeRef  returnType;
        SLTypeRef* paramTypes = NULL;
        uint32_t   paramLen = 0;
        uint32_t   paramCap = 0;
        TypeRefSetScalar(&returnType, "void");
        while (child >= 0) {
            const SLAstNode* ch = NodeAt(c, child);
            if (ch != NULL && ch->kind == SLAst_PARAM) {
                int32_t   typeNode = AstFirstChild(&c->ast, child);
                SLTypeRef paramType;
                if (ParseTypeRef(c, typeNode, &paramType) != 0) {
                    return -1;
                }
                if (paramLen >= paramCap) {
                    uint32_t need = paramLen + 1u;
                    if (EnsureCapArena(
                            &c->arena,
                            (void**)&paramTypes,
                            &paramCap,
                            need,
                            sizeof(SLTypeRef),
                            (uint32_t)_Alignof(SLTypeRef))
                        != 0)
                    {
                        return -1;
                    }
                }
                paramTypes[paramLen++] = paramType;
            } else if (
                ch != NULL
                && (ch->kind == SLAst_TYPE_NAME || ch->kind == SLAst_TYPE_PTR
                    || ch->kind == SLAst_TYPE_REF || ch->kind == SLAst_TYPE_MUTREF
                    || ch->kind == SLAst_TYPE_ARRAY || ch->kind == SLAst_TYPE_VARRAY
                    || ch->kind == SLAst_TYPE_SLICE || ch->kind == SLAst_TYPE_MUTSLICE
                    || ch->kind == SLAst_TYPE_OPTIONAL)
                && ch->flags == 1)
            {
                if (ParseTypeRef(c, child, &returnType) != 0) {
                    return -1;
                }
                break;
            }
            child = AstNextSibling(&c->ast, child);
        }
        return AddFnSig(c, mapName->cName, returnType, paramTypes, paramLen);
    }

    if (n->kind == SLAst_STRUCT || n->kind == SLAst_UNION) {
        int32_t child = AstFirstChild(&c->ast, nodeId);
        while (child >= 0) {
            const SLAstNode* field = NodeAt(c, child);
            if (field != NULL && field->kind == SLAst_FIELD) {
                int32_t     typeNode = AstFirstChild(&c->ast, child);
                const char* lenFieldName = NULL;
                int         isDependent = 0;
                SLTypeRef   fieldType;
                char*       fieldName;
                if (ParseTypeRef(c, typeNode, &fieldType) != 0) {
                    return -1;
                }
                if (typeNode >= 0 && NodeAt(c, typeNode) != NULL
                    && NodeAt(c, typeNode)->kind == SLAst_TYPE_VARRAY)
                {
                    lenFieldName = DupSlice(
                        c,
                        c->unit->source,
                        NodeAt(c, typeNode)->dataStart,
                        NodeAt(c, typeNode)->dataEnd);
                    if (lenFieldName == NULL) {
                        return -1;
                    }
                    isDependent = 1;
                }
                fieldName = DupSlice(c, c->unit->source, field->dataStart, field->dataEnd);
                if (fieldName == NULL) {
                    return -1;
                }
                if (AddFieldInfo(c, mapName->cName, fieldName, lenFieldName, isDependent, fieldType)
                    != 0)
                {
                    return -1;
                }
            }
            child = AstNextSibling(&c->ast, child);
        }
    }

    return 0;
}

static int CollectFnAndFieldInfo(SLCBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->pubDeclLen; i++) {
        if (CollectFnAndFieldInfoFromNode(c, c->pubDecls[i].nodeId) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->topDeclLen; i++) {
        if (CollectFnAndFieldInfoFromNode(c, c->topDecls[i].nodeId) != 0) {
            return -1;
        }
    }
    return 0;
}

static int AddVarSizeType(SLCBackendC* c, const char* cName, int isUnion) {
    uint32_t i;
    char*    copy;
    for (i = 0; i < c->varSizeTypeLen; i++) {
        if (StrEq(c->varSizeTypes[i].cName, cName)) {
            return 0;
        }
    }
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->varSizeTypes,
            &c->varSizeTypeCap,
            c->varSizeTypeLen + 1u,
            sizeof(SLVarSizeType),
            (uint32_t)_Alignof(SLVarSizeType))
        != 0)
    {
        return -1;
    }
    copy = DupCStr(c, cName);
    if (copy == NULL) {
        return -1;
    }
    c->varSizeTypes[c->varSizeTypeLen].cName = copy;
    c->varSizeTypes[c->varSizeTypeLen].isUnion = isUnion;
    c->varSizeTypes[c->varSizeTypeLen].isVarSize = 0;
    c->varSizeTypeLen++;
    return 0;
}

static SLVarSizeType* _Nullable FindVarSizeType(SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->varSizeTypeLen; i++) {
        if (StrEq(c->varSizeTypes[i].cName, cName)) {
            return &c->varSizeTypes[i];
        }
    }
    return NULL;
}

static int CollectVarSizeTypesFromDeclSets(SLCBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        const SLNameMap* map;
        if (n == NULL || (n->kind != SLAst_STRUCT && n->kind != SLAst_UNION)) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        if (AddVarSizeType(c, map->cName, n->kind == SLAst_UNION) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        const SLNameMap* map;
        if (n == NULL || (n->kind != SLAst_STRUCT && n->kind != SLAst_UNION)) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        if (AddVarSizeType(c, map->cName, n->kind == SLAst_UNION) != 0) {
            return -1;
        }
    }
    return 0;
}

static int IsVarSizeTypeName(const SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->varSizeTypeLen; i++) {
        if (StrEq(c->varSizeTypes[i].cName, cName)) {
            return c->varSizeTypes[i].isVarSize;
        }
    }
    return 0;
}

static int PropagateVarSizeTypes(SLCBackendC* c) {
    int changed = 1;
    while (changed) {
        uint32_t i;
        changed = 0;
        for (i = 0; i < c->fieldInfoLen; i++) {
            SLFieldInfo*   field = &c->fieldInfos[i];
            SLVarSizeType* owner;
            if (field->ownerType == NULL) {
                continue;
            }
            owner = FindVarSizeType(c, field->ownerType);
            if (owner == NULL || owner->isVarSize) {
                continue;
            }
            if (field->isDependent) {
                owner->isVarSize = 1;
                changed = 1;
                continue;
            }
            if (field->type.valid && field->type.containerPtrDepth == 0 && field->type.ptrDepth == 0
                && field->type.baseName != NULL
                && (field->type.containerKind == SLTypeContainer_SCALAR
                    || field->type.containerKind == SLTypeContainer_ARRAY)
                && IsVarSizeTypeName(c, field->type.baseName))
            {
                owner->isVarSize = 1;
                changed = 1;
            }
        }
    }
    return 0;
}

static int PushScope(SLCBackendC* c) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->localScopeMarks,
            &c->localScopeCap,
            c->localScopeLen + 1u,
            sizeof(uint32_t),
            (uint32_t)_Alignof(uint32_t))
        != 0)
    {
        return -1;
    }
    c->localScopeMarks[c->localScopeLen++] = c->localLen;
    return 0;
}

static void PopScope(SLCBackendC* c) {
    if (c->localScopeLen == 0) {
        c->localLen = 0;
        return;
    }
    c->localScopeLen--;
    c->localLen = c->localScopeMarks[c->localScopeLen];
}

static int PushDeferScope(SLCBackendC* c) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->deferScopeMarks,
            &c->deferScopeCap,
            c->deferScopeLen + 1u,
            sizeof(uint32_t),
            (uint32_t)_Alignof(uint32_t))
        != 0)
    {
        return -1;
    }
    c->deferScopeMarks[c->deferScopeLen++] = c->deferredStmtLen;
    return 0;
}

static void PopDeferScope(SLCBackendC* c) {
    if (c->deferScopeLen == 0) {
        c->deferredStmtLen = 0;
        return;
    }
    c->deferScopeLen--;
    c->deferredStmtLen = c->deferScopeMarks[c->deferScopeLen];
}

static int AddDeferredStmt(SLCBackendC* c, int32_t stmtNodeId) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->deferredStmtNodes,
            &c->deferredStmtCap,
            c->deferredStmtLen + 1u,
            sizeof(int32_t),
            (uint32_t)_Alignof(int32_t))
        != 0)
    {
        return -1;
    }
    c->deferredStmtNodes[c->deferredStmtLen++] = stmtNodeId;
    return 0;
}

static int AddLocal(SLCBackendC* c, const char* name, SLTypeRef type) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->locals,
            &c->localCap,
            c->localLen + 1u,
            sizeof(SLLocal),
            (uint32_t)_Alignof(SLLocal))
        != 0)
    {
        return -1;
    }
    c->locals[c->localLen].name = (char*)name;
    c->locals[c->localLen].type = type;
    c->localLen++;
    return 0;
}

static const SLLocal* _Nullable FindLocalBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (SliceEqName(c->unit->source, start, end, c->locals[i].name)) {
            return &c->locals[i];
        }
    }
    return NULL;
}

static int AppendMappedIdentifier(SLCBackendC* c, uint32_t start, uint32_t end) {
    const SLLocal*   local = FindLocalBySlice(c, start, end);
    const SLNameMap* map;
    if (local != NULL) {
        return BufAppendCStr(&c->out, local->name);
    }
    map = FindNameBySlice(c, start, end);
    if (map != NULL) {
        return BufAppendCStr(&c->out, map->cName);
    }
    return BufAppendSlice(&c->out, c->unit->source, start, end);
}

static int EmitTypeNameWithDepth(SLCBackendC* c, const SLTypeRef* type) {
    int         i;
    const char* base = NULL;
    int         stars = 0;
    if (!type->valid) {
        return BufAppendCStr(&c->out, "void");
    }
    if (type->containerKind == SLTypeContainer_SLICE_RO
        || type->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (type->containerPtrDepth > 0) {
            /* *[T] / *mut[T] — element pointer */
            base = type->baseName;
            stars = type->ptrDepth + type->containerPtrDepth;
        } else {
            base = type->containerKind == SLTypeContainer_SLICE_MUT
                     ? "__sl_slice_mut"
                     : "__sl_slice_ro";
            stars = 0;
        }
    } else {
        if (type->baseName == NULL) {
            return BufAppendCStr(&c->out, "void");
        }
        base = type->baseName;
        stars = type->ptrDepth;
        if (type->containerKind == SLTypeContainer_ARRAY && type->containerPtrDepth > 0) {
            stars += type->containerPtrDepth;
        }
    }
    if (BufAppendCStr(&c->out, base) != 0) {
        return -1;
    }
    for (i = 0; i < stars; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    return 0;
}

static int EmitTypeWithName(SLCBackendC* c, int32_t typeNode, const char* name) {
    SLTypeRef t;
    int       i;
    int       stars;
    if (ParseTypeRef(c, typeNode, &t) != 0 || !t.valid) {
        return -1;
    }
    if (t.containerKind == SLTypeContainer_SLICE_RO || t.containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (t.containerPtrDepth > 0) {
            /* *[T] / *mut[T] — element pointer, emits as T* */
            if (t.baseName == NULL) {
                return -1;
            }
            if (BufAppendCStr(&c->out, t.baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
                return -1;
            }
            stars = t.ptrDepth + t.containerPtrDepth;
            for (i = 0; i < stars; i++) {
                if (BufAppendChar(&c->out, '*') != 0) {
                    return -1;
                }
            }
            return BufAppendCStr(&c->out, name);
        }
        if (BufAppendCStr(
                &c->out,
                t.containerKind == SLTypeContainer_SLICE_MUT ? "__sl_slice_mut" : "__sl_slice_ro")
                != 0
            || BufAppendChar(&c->out, ' ') != 0)
        {
            return -1;
        }
        return BufAppendCStr(&c->out, name);
    }
    if (t.baseName == NULL) {
        return -1;
    }
    if (BufAppendCStr(&c->out, t.baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
        return -1;
    }
    stars = t.ptrDepth;
    if (t.containerKind == SLTypeContainer_ARRAY && t.containerPtrDepth > 0) {
        stars += t.containerPtrDepth;
    }
    for (i = 0; i < stars; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    {
        const SLAstNode* tn = NodeAt(c, typeNode);
        if (tn != NULL && tn->kind == SLAst_TYPE_OPTIONAL) {
            if (BufAppendCStr(&c->out, "/* optional */ ") != 0) {
                return -1;
            }
        }
    }
    if (BufAppendCStr(&c->out, name) != 0) {
        return -1;
    }
    if (t.containerKind == SLTypeContainer_ARRAY && t.containerPtrDepth == 0 && t.hasArrayLen) {
        if (BufAppendChar(&c->out, '[') != 0 || BufAppendU32(&c->out, t.arrayLen) != 0
            || BufAppendChar(&c->out, ']') != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int EmitTypeForCast(SLCBackendC* c, int32_t typeNode) {
    SLTypeRef t;
    if (ParseTypeRef(c, typeNode, &t) != 0) {
        return -1;
    }
    return EmitTypeNameWithDepth(c, &t);
}

static int InferExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType);

static int InferExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }

    switch (n->kind) {
        case SLAst_IDENT: {
            const SLLocal* local = FindLocalBySlice(c, n->dataStart, n->dataEnd);
            if (local != NULL) {
                *outType = local->type;
                return 0;
            }
            TypeRefSetInvalid(outType);
            return 0;
        }
        case SLAst_CALL: {
            int32_t          callee = AstFirstChild(&c->ast, nodeId);
            const SLAstNode* cn = NodeAt(c, callee);
            if (cn != NULL && cn->kind == SLAst_IDENT) {
                const SLFnSig* sig = FindFnSigBySlice(c, cn->dataStart, cn->dataEnd);
                if (sig != NULL) {
                    *outType = sig->returnType;
                    return 0;
                }
            }
            TypeRefSetInvalid(outType);
            return 0;
        }
        case SLAst_UNARY: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (InferExprType(c, child, outType) != 0) {
                return -1;
            }
            if ((SLTokenKind)n->op == SLTok_AND) {
                if (!outType->valid) {
                    TypeRefSetScalar(outType, "void");
                    outType->ptrDepth = 1;
                    return 0;
                }
                if (outType->containerKind == SLTypeContainer_SCALAR) {
                    outType->ptrDepth++;
                } else {
                    outType->containerPtrDepth++;
                }
            } else if ((SLTokenKind)n->op == SLTok_MUL) {
                if (outType->valid && outType->containerKind != SLTypeContainer_SCALAR
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
        case SLAst_FIELD_EXPR: {
            int32_t            recv = AstFirstChild(&c->ast, nodeId);
            SLTypeRef          recvType;
            const SLFieldInfo* field;
            if (InferExprType(c, recv, &recvType) != 0 || !recvType.valid) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            if (recvType.containerKind != SLTypeContainer_SCALAR && recvType.containerPtrDepth > 0)
            {
                recvType.containerPtrDepth--;
            } else if (recvType.ptrDepth > 0) {
                recvType.ptrDepth--;
            }
            field = FindFieldInfo(c, recvType.baseName, n->dataStart, n->dataEnd);
            if (field != NULL) {
                *outType = field->type;
                return 0;
            }
            TypeRefSetInvalid(outType);
            return 0;
        }
        case SLAst_INDEX: {
            int32_t base = AstFirstChild(&c->ast, nodeId);
            if (InferExprType(c, base, outType) != 0) {
                return -1;
            }
            if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
                if (!outType->valid) {
                    TypeRefSetInvalid(outType);
                    return 0;
                }
                if (outType->containerKind == SLTypeContainer_ARRAY) {
                    outType->containerKind =
                        outType->readOnly ? SLTypeContainer_SLICE_RO : SLTypeContainer_SLICE_MUT;
                    outType->containerPtrDepth = 0;
                    outType->readOnly = outType->containerKind == SLTypeContainer_SLICE_RO;
                } else if (
                    outType->containerKind == SLTypeContainer_SLICE_RO
                    || outType->containerKind == SLTypeContainer_SLICE_MUT)
                {
                    outType->containerPtrDepth = 0;
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
            if (outType->containerKind == SLTypeContainer_ARRAY
                || outType->containerKind == SLTypeContainer_SLICE_RO
                || outType->containerKind == SLTypeContainer_SLICE_MUT)
            {
                outType->containerKind = SLTypeContainer_SCALAR;
                outType->containerPtrDepth = 0;
                outType->hasArrayLen = 0;
                outType->arrayLen = 0;
                outType->readOnly = 0;
            } else if (outType->ptrDepth > 0) {
                outType->ptrDepth--;
            }
            return 0;
        }
        case SLAst_CAST: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            int32_t typeNode = AstNextSibling(&c->ast, expr);
            return ParseTypeRef(c, typeNode, outType);
        }
        case SLAst_SIZEOF: TypeRefSetScalar(outType, "__sl_uint"); return 0;
        case SLAst_STRING: TypeRefSetScalar(outType, "__sl_str"); return 0;
        case SLAst_BOOL:   TypeRefSetScalar(outType, "__sl_bool"); return 0;
        case SLAst_INT:    TypeRefSetScalar(outType, "__sl_i32"); return 0;
        case SLAst_FLOAT:  TypeRefSetScalar(outType, "__sl_f64"); return 0;
        case SLAst_NULL:   TypeRefSetInvalid(outType); return 0;
        case SLAst_UNWRAP: {
            int32_t inner = AstFirstChild(&c->ast, nodeId);
            return InferExprType(c, inner, outType);
        }
        default: TypeRefSetInvalid(outType); return 0;
    }
}

static const char* UnaryOpString(SLTokenKind op) {
    switch (op) {
        case SLTok_ADD: return "+";
        case SLTok_SUB: return "-";
        case SLTok_NOT: return "!";
        case SLTok_MUL: return "*";
        case SLTok_AND: return "&";
        default:        return "";
    }
}

static const char* BinaryOpString(SLTokenKind op) {
    switch (op) {
        case SLTok_ASSIGN:        return "=";
        case SLTok_ADD:           return "+";
        case SLTok_SUB:           return "-";
        case SLTok_MUL:           return "*";
        case SLTok_DIV:           return "/";
        case SLTok_MOD:           return "%";
        case SLTok_AND:           return "&";
        case SLTok_OR:            return "|";
        case SLTok_XOR:           return "^";
        case SLTok_LSHIFT:        return "<<";
        case SLTok_RSHIFT:        return ">>";
        case SLTok_EQ:            return "==";
        case SLTok_NEQ:           return "!=";
        case SLTok_LT:            return "<";
        case SLTok_GT:            return ">";
        case SLTok_LTE:           return "<=";
        case SLTok_GTE:           return ">=";
        case SLTok_LOGICAL_AND:   return "&&";
        case SLTok_LOGICAL_OR:    return "||";
        case SLTok_ADD_ASSIGN:    return "+=";
        case SLTok_SUB_ASSIGN:    return "-=";
        case SLTok_MUL_ASSIGN:    return "*=";
        case SLTok_DIV_ASSIGN:    return "/=";
        case SLTok_MOD_ASSIGN:    return "%=";
        case SLTok_AND_ASSIGN:    return "&=";
        case SLTok_OR_ASSIGN:     return "|=";
        case SLTok_XOR_ASSIGN:    return "^=";
        case SLTok_LSHIFT_ASSIGN: return "<<=";
        case SLTok_RSHIFT_ASSIGN: return ">>=";
        default:                  return "";
    }
}

static int EmitHexByte(SLBuf* b, uint8_t value) {
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

static int EmitStringLiteralRef(SLCBackendC* c, int32_t literalId) {
    if (BufAppendCStr(&c->out, "((__sl_str){ sl_lit_") != 0
        || BufAppendU32(&c->out, (uint32_t)literalId) != 0 || BufAppendCStr(&c->out, ", ") != 0
        || BufAppendU32(&c->out, c->stringLits[literalId].len) != 0
        || BufAppendCStr(&c->out, "u })") != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitStringLiteralPool(SLCBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->stringLitLen; i++) {
        uint32_t               j;
        const SLStringLiteral* lit = &c->stringLits[i];
        if (BufAppendCStr(&c->out, "static const __sl_u8 sl_lit_") != 0
            || BufAppendU32(&c->out, i) != 0 || BufAppendCStr(&c->out, "[] = { ") != 0)
        {
            return -1;
        }
        for (j = 0; j < lit->len; j++) {
            if (EmitHexByte(&c->out, lit->bytes[j]) != 0 || BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
        }
        if (EmitHexByte(&c->out, 0u) != 0 || BufAppendCStr(&c->out, " };\n") != 0) {
            return -1;
        }
    }
    if (c->stringLitLen > 0 && BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int EmitExpr(SLCBackendC* c, int32_t nodeId);
static int EmitAssertFormatArg(SLCBackendC* c, int32_t nodeId);

static int TypeRefIsStr(const SLTypeRef* t) {
    return t->valid && t->containerKind == SLTypeContainer_SCALAR && t->ptrDepth == 0
        && t->baseName != NULL && StrEq(t->baseName, "__sl_str");
}

static int TypeRefContainerWritable(const SLTypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if (t->containerKind == SLTypeContainer_ARRAY || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        return t->readOnly == 0;
    }
    /* *[T]: element pointer (ptrDepth > 0) — the pointer itself is writable */
    if (t->containerKind == SLTypeContainer_SLICE_RO && t->containerPtrDepth > 0) {
        return 1;
    }
    return 0;
}

static int EmitElementTypeName(SLCBackendC* c, const SLTypeRef* t, int asConst) {
    int i;
    if (!t->valid || t->baseName == NULL) {
        return -1;
    }
    if (asConst && BufAppendCStr(&c->out, "const ") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, t->baseName) != 0) {
        return -1;
    }
    for (i = 0; i < t->ptrDepth; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    return 0;
}

static int EmitLenExprFromType(SLCBackendC* c, int32_t exprNode, const SLTypeRef* t) {
    if (TypeRefIsStr(t)) {
        if (BufAppendCStr(&c->out, "(__sl_u32)((") != 0 || EmitExpr(c, exprNode) != 0
            || BufAppendCStr(&c->out, ").len)") != 0)
        {
            return -1;
        }
        return 0;
    }
    if (t->containerKind == SLTypeContainer_ARRAY && t->hasArrayLen) {
        if (t->containerPtrDepth > 0) {
            if (BufAppendCStr(&c->out, "((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ") == 0 ? 0u : ") != 0
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
    if (t->containerKind == SLTypeContainer_SLICE_RO
        || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (t->containerPtrDepth > 0) {
            if (BufAppendCStr(&c->out, "((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ") == 0 ? 0u : (__sl_u32)((") != 0
                || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, ")->len))") != 0)
            {
                return -1;
            }
        } else {
            if (BufAppendCStr(&c->out, "(__sl_u32)((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").len)") != 0)
            {
                return -1;
            }
        }
        return 0;
    }
    if (BufAppendCStr(&c->out, "__sl_len(") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitElemPtrExpr(
    SLCBackendC* c, int32_t baseNode, const SLTypeRef* baseType, int wantWritableElem) {
    int elemConst = !wantWritableElem;
    if (BufAppendCStr(&c->out, "((") != 0 || EmitElementTypeName(c, baseType, elemConst) != 0
        || BufAppendCStr(&c->out, "*)(") != 0)
    {
        return -1;
    }
    if (baseType->containerKind == SLTypeContainer_ARRAY) {
        if (EmitExpr(c, baseNode) != 0) {
            return -1;
        }
    } else if (
        baseType->containerKind == SLTypeContainer_SLICE_RO
        || baseType->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (baseType->containerPtrDepth > 0) {
            /* *[T] is already an element pointer; just emit the expression */
            if (EmitExpr(c, baseNode) != 0) {
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
        return -1;
    }
    return BufAppendCStr(&c->out, "))");
}

static int EmitSliceExpr(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    int32_t          baseNode = AstFirstChild(&c->ast, nodeId);
    int32_t          child = AstNextSibling(&c->ast, baseNode);
    int              hasStart = (n->flags & SLAstFlag_INDEX_HAS_START) != 0;
    int              hasEnd = (n->flags & SLAstFlag_INDEX_HAS_END) != 0;
    int32_t          startNode = -1;
    int32_t          endNode = -1;
    SLTypeRef        baseType;
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
    if (BufAppendCStr(&c->out, "((") != 0
        || BufAppendCStr(&c->out, outMut ? "__sl_slice_mut" : "__sl_slice_ro") != 0
        || BufAppendCStr(&c->out, "){ ") != 0)
    {
        return -1;
    }
    if (BufAppendCStr(&c->out, outMut ? "(void*)(" : "(const void*)(") != 0
        || EmitElemPtrExpr(c, baseNode, &baseType, outMut) != 0
        || BufAppendCStr(&c->out, " + (__sl_uint)(") != 0)
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
    if (BufAppendCStr(&c->out, ")), (__sl_uint)((") != 0) {
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
    if (outMut) {
        if (BufAppendCStr(&c->out, "), (__sl_uint)((") != 0) {
            return -1;
        }
        if (baseType.containerKind == SLTypeContainer_SLICE_MUT) {
            if (baseType.containerPtrDepth > 0) {
                if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, baseNode) != 0
                    || BufAppendCStr(&c->out, ")->cap") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, baseNode) != 0
                    || BufAppendCStr(&c->out, ").cap") != 0)
                {
                    return -1;
                }
            }
        } else if (baseType.hasArrayLen) {
            if (BufAppendU32(&c->out, baseType.arrayLen) != 0) {
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
        if (BufAppendCStr(&c->out, "))") != 0) {
            return -1;
        }
    }
    return BufAppendCStr(&c->out, " })");
}

static int EmitExprCoerced(SLCBackendC* c, int32_t exprNode, const SLTypeRef* dstType) {
    SLTypeRef srcType;
    if (dstType == NULL || !dstType->valid) {
        return EmitExpr(c, exprNode);
    }
    if (InferExprType(c, exprNode, &srcType) != 0 || !srcType.valid) {
        return EmitExpr(c, exprNode);
    }
    if (dstType->containerKind == SLTypeContainer_SLICE_RO && dstType->containerPtrDepth == 0) {
        if (srcType.containerKind == SLTypeContainer_SLICE_RO && srcType.containerPtrDepth == 0) {
            return EmitExpr(c, exprNode);
        }
        if ((srcType.containerKind == SLTypeContainer_SLICE_MUT && srcType.containerPtrDepth == 0)
            || srcType.containerKind == SLTypeContainer_ARRAY)
        {
            if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)(") != 0
                || EmitElemPtrExpr(c, exprNode, &srcType, 0) != 0
                || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                || EmitLenExprFromType(c, exprNode, &srcType) != 0
                || BufAppendCStr(&c->out, ") })") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    if (dstType->containerKind == SLTypeContainer_SLICE_MUT && dstType->containerPtrDepth == 0) {
        if (srcType.containerKind == SLTypeContainer_SLICE_MUT && srcType.containerPtrDepth == 0) {
            return EmitExpr(c, exprNode);
        }
        if (srcType.containerKind == SLTypeContainer_ARRAY) {
            if (BufAppendCStr(&c->out, "((__sl_slice_mut){ (void*)(") != 0
                || EmitElemPtrExpr(c, exprNode, &srcType, 1) != 0
                || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                || EmitLenExprFromType(c, exprNode, &srcType) != 0
                || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
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

static int EmitExpr(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAst_IDENT:  return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
        case SLAst_INT:
        case SLAst_FLOAT:
        case SLAst_BOOL:   return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
        case SLAst_STRING: {
            int32_t literalId = -1;
            if (nodeId >= 0 && (uint32_t)nodeId < c->stringLitByNodeLen) {
                literalId = c->stringLitByNode[nodeId];
            }
            if (literalId < 0) {
                return -1;
            }
            return EmitStringLiteralRef(c, literalId);
        }
        case SLAst_UNARY: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if ((SLTokenKind)n->op == SLTok_AND && child >= 0) {
                const SLAstNode* cn = NodeAt(c, child);
                if (cn != NULL && cn->kind == SLAst_FIELD_EXPR) {
                    int32_t            recv = AstFirstChild(&c->ast, child);
                    SLTypeRef          recvType;
                    SLTypeRef          ownerType;
                    const SLFieldInfo* field = NULL;
                    SLTypeRef          childType;
                    if (InferExprType(c, recv, &recvType) == 0 && recvType.valid) {
                        ownerType = recvType;
                        if (ownerType.ptrDepth > 0) {
                            ownerType.ptrDepth--;
                        }
                        if (ownerType.baseName != NULL) {
                            field = FindFieldInfo(
                                c, ownerType.baseName, cn->dataStart, cn->dataEnd);
                        }
                    }
                    if (field != NULL && field->isDependent
                        && InferExprType(c, child, &childType) == 0 && childType.valid)
                    {
                        if (BufAppendCStr(&c->out, "(&(") != 0
                            || EmitTypeNameWithDepth(c, &childType) != 0
                            || BufAppendCStr(&c->out, "){") != 0 || EmitExpr(c, child) != 0
                            || BufAppendCStr(&c->out, "})") != 0)
                        {
                            return -1;
                        }
                        return 0;
                    }
                }
                {
                    SLTypeRef childType;
                    if (InferExprType(c, child, &childType) == 0 && childType.valid
                        && childType.containerKind == SLTypeContainer_ARRAY
                        && childType.containerPtrDepth == 0)
                    {
                        return EmitElemPtrExpr(
                            c, child, &childType, TypeRefContainerWritable(&childType));
                    }
                }
            }
            if (BufAppendChar(&c->out, '(') != 0
                || BufAppendCStr(&c->out, UnaryOpString((SLTokenKind)n->op)) != 0
                || EmitExpr(c, child) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_BINARY: {
            int32_t lhs = AstFirstChild(&c->ast, nodeId);
            int32_t rhs = AstNextSibling(&c->ast, lhs);
            if ((SLTokenKind)n->op == SLTok_ASSIGN) {
                SLTypeRef lhsType;
                if (lhs < 0 || rhs < 0) {
                    return -1;
                }
                if (InferExprType(c, lhs, &lhsType) != 0 || !lhsType.valid) {
                    return -1;
                }
                if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, lhs) != 0
                    || BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, rhs, &lhsType) != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, lhs) != 0
                || BufAppendChar(&c->out, ' ') != 0
                || BufAppendCStr(&c->out, BinaryOpString((SLTokenKind)n->op)) != 0
                || BufAppendChar(&c->out, ' ') != 0 || EmitExpr(c, rhs) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_CALL: {
            int32_t          child = AstFirstChild(&c->ast, nodeId);
            const SLAstNode* callee = NodeAt(c, child);
            int              first = 1;
            if (callee != NULL && callee->kind == SLAst_IDENT
                && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "len"))
            {
                int32_t   arg = AstNextSibling(&c->ast, child);
                int32_t   extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
                SLTypeRef argType;
                if (arg < 0 || extra >= 0) {
                    return -1;
                }
                if (InferExprType(c, arg, &argType) != 0 || !argType.valid) {
                    return -1;
                }
                return EmitLenExprFromType(c, arg, &argType);
            }
            if (callee != NULL && callee->kind == SLAst_IDENT
                && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new"))
            {
                int32_t     allocArg = AstNextSibling(&c->ast, child);
                int32_t     typeArg = allocArg >= 0 ? AstNextSibling(&c->ast, allocArg) : -1;
                int32_t     countArg = typeArg >= 0 ? AstNextSibling(&c->ast, typeArg) : -1;
                int32_t     extraArg = countArg >= 0 ? AstNextSibling(&c->ast, countArg) : -1;
                const char* typeName;
                if (allocArg < 0 || typeArg < 0 || extraArg >= 0) {
                    return -1;
                }
                typeName = ResolveTypeNameFromExprArg(c, typeArg);
                if (typeName == NULL) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, typeName) != 0
                    || BufAppendCStr(&c->out, "*)") != 0)
                {
                    return -1;
                }
                if (countArg >= 0) {
                    if (BufAppendCStr(&c->out, "__sl_new_array(") != 0 || EmitExpr(c, allocArg) != 0
                        || BufAppendCStr(&c->out, ", sizeof(") != 0
                        || BufAppendCStr(&c->out, typeName) != 0
                        || BufAppendCStr(&c->out, "), _Alignof(") != 0
                        || BufAppendCStr(&c->out, typeName) != 0
                        || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                        || EmitExpr(c, countArg) != 0 || BufAppendCStr(&c->out, ")))") != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
                if (BufAppendCStr(&c->out, "__sl_new(") != 0 || EmitExpr(c, allocArg) != 0
                    || BufAppendCStr(&c->out, ", sizeof(") != 0
                    || BufAppendCStr(&c->out, typeName) != 0
                    || BufAppendCStr(&c->out, "), _Alignof(") != 0
                    || BufAppendCStr(&c->out, typeName) != 0 || BufAppendCStr(&c->out, ")))") != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (callee != NULL && callee->kind == SLAst_IDENT
                && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "cstr"))
            {
                int32_t arg = AstNextSibling(&c->ast, child);
                int32_t extra = arg >= 0 ? AstNextSibling(&c->ast, arg) : -1;
                if (arg < 0 || extra >= 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_cstr(") != 0 || EmitExpr(c, arg) != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (callee != NULL && callee->kind == SLAst_IDENT
                && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "panic"))
            {
                int32_t msgArg = AstNextSibling(&c->ast, child);
                if (msgArg < 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_panic(__FILE__, __LINE__, ") != 0
                    || EmitExpr(c, msgArg) != 0 || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                return 0;
            }
            {
                const SLFnSig* sig = NULL;
                uint32_t       argIndex = 0;
                if (callee != NULL && callee->kind == SLAst_IDENT) {
                    sig = FindFnSigBySlice(c, callee->dataStart, callee->dataEnd);
                }
                if (EmitExpr(c, child) != 0 || BufAppendChar(&c->out, '(') != 0) {
                    return -1;
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
        case SLAst_INDEX: {
            int32_t base = AstFirstChild(&c->ast, nodeId);
            int32_t idx = AstNextSibling(&c->ast, base);
            if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
                return EmitSliceExpr(c, nodeId);
            }
            if (base < 0 || idx < 0) {
                return -1;
            }
            {
                SLTypeRef baseType;
                if (InferExprType(c, base, &baseType) != 0 || !baseType.valid) {
                    return -1;
                }
                if (baseType.containerKind == SLTypeContainer_ARRAY
                    || baseType.containerKind == SLTypeContainer_SLICE_RO
                    || baseType.containerKind == SLTypeContainer_SLICE_MUT)
                {
                    if (BufAppendChar(&c->out, '(') != 0
                        || EmitElemPtrExpr(c, base, &baseType, TypeRefContainerWritable(&baseType))
                               != 0
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
        case SLAst_FIELD_EXPR: {
            int32_t            recv = AstFirstChild(&c->ast, nodeId);
            SLTypeRef          recvType;
            SLTypeRef          ownerType;
            const SLFieldInfo* field = NULL;
            int                useArrow = 0;

            if (InferExprType(c, recv, &recvType) == 0 && recvType.valid) {
                ownerType = recvType;
                if (ownerType.ptrDepth > 0) {
                    ownerType.ptrDepth--;
                    useArrow = 1;
                }
                if (ownerType.baseName != NULL) {
                    field = FindFieldInfo(c, ownerType.baseName, n->dataStart, n->dataEnd);
                }
            }

            if (field != NULL && field->isDependent) {
                if (BufAppendCStr(&c->out, ownerType.baseName) != 0
                    || BufAppendCStr(&c->out, "__") != 0
                    || BufAppendCStr(&c->out, field->fieldName) != 0
                    || BufAppendChar(&c->out, '(') != 0)
                {
                    return -1;
                }
                if (useArrow) {
                    if (EmitExpr(c, recv) != 0) {
                        return -1;
                    }
                } else {
                    if (BufAppendCStr(&c->out, "&(") != 0 || EmitExpr(c, recv) != 0
                        || BufAppendChar(&c->out, ')') != 0)
                    {
                        return -1;
                    }
                }
                return BufAppendChar(&c->out, ')');
            }

            if (EmitExpr(c, recv) != 0 || BufAppendCStr(&c->out, useArrow ? "->" : ".") != 0
                || BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd) != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_CAST: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            int32_t typeNode = AstNextSibling(&c->ast, expr);
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeForCast(c, typeNode) != 0
                || BufAppendCStr(&c->out, ")(") != 0 || EmitExpr(c, expr) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_SIZEOF: {
            int32_t   inner = AstFirstChild(&c->ast, nodeId);
            SLTypeRef innerType;
            if (inner < 0) {
                return -1;
            }
            if (n->flags == 1) {
                if (BufAppendCStr(&c->out, "sizeof(") != 0 || EmitTypeForCast(c, inner) != 0
                    || BufAppendChar(&c->out, ')') != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (InferExprType(c, inner, &innerType) == 0 && innerType.valid
                && innerType.ptrDepth > 0)
            {
                SLTypeRef baseType = innerType;
                baseType.ptrDepth--;
                if (baseType.baseName != NULL && IsVarSizeTypeName(c, baseType.baseName)) {
                    if (BufAppendCStr(&c->out, baseType.baseName) != 0
                        || BufAppendCStr(&c->out, "__sizeof(") != 0 || EmitExpr(c, inner) != 0
                        || BufAppendChar(&c->out, ')') != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
            }
            if (BufAppendCStr(&c->out, "sizeof(") != 0 || EmitExpr(c, inner) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_NULL:   return BufAppendCStr(&c->out, "NULL");
        case SLAst_UNWRAP: {
            int32_t inner = AstFirstChild(&c->ast, nodeId);
            if (inner < 0) {
                return -1;
            }
            if (BufAppendCStr(&c->out, "__sl_unwrap(") != 0 || EmitExpr(c, inner) != 0
                || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        default: return -1;
    }
}

static int EmitStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth);

static int EmitDeferredRange(SLCBackendC* c, uint32_t start, uint32_t depth) {
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

static int EmitBlock(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
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
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "{\n") != 0) {
        PopDeferScope(c);
        PopScope(c);
        return -1;
    }
    while (child >= 0) {
        if (EmitStmt(c, child, depth + 1u) != 0) {
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

static int EmitVarLikeStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isConst) {
    const SLAstNode* n = NodeAt(c, nodeId);
    int32_t          typeNode = AstFirstChild(&c->ast, nodeId);
    int32_t          initNode = AstNextSibling(&c->ast, typeNode);
    char*            name;
    SLTypeRef        type;

    if (n == NULL) {
        return -1;
    }
    name = DupSlice(c, c->unit->source, n->dataStart, n->dataEnd);
    if (name == NULL) {
        return -1;
    }
    if (ParseTypeRef(c, typeNode, &type) != 0) {
        return -1;
    }

    EmitIndent(c, depth);
    if (isConst && BufAppendCStr(&c->out, "const ") != 0) {
        return -1;
    }
    if (EmitTypeWithName(c, typeNode, name) != 0) {
        return -1;
    }
    if (initNode >= 0) {
        if (BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, initNode, &type) != 0) {
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, ";\n") != 0) {
        return -1;
    }

    if (AddLocal(c, name, type) != 0) {
        return -1;
    }
    return 0;
}

static int EmitForStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    int32_t nodes[4];
    int     count = 0;
    int32_t child = AstFirstChild(&c->ast, nodeId);
    int32_t body;
    int32_t init = -1;
    int32_t cond = -1;
    int32_t post = -1;

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = AstNextSibling(&c->ast, child);
    }
    if (count <= 0) {
        return -1;
    }

    body = nodes[count - 1];
    if (count == 1) {
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "for (;;)") != 0) {
            return -1;
        }
        if (NodeAt(c, body)->kind == SLAst_BLOCK) {
            if (BufAppendChar(&c->out, '\n') != 0) {
                return -1;
            }
            return EmitBlock(c, body, depth);
        }
        if (BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
        return EmitStmt(c, body, depth);
    }

    if (count == 2 && NodeAt(c, nodes[0])->kind != SLAst_VAR
        && NodeAt(c, nodes[0])->kind != SLAst_CONST)
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
        if (NodeAt(c, init)->kind == SLAst_VAR) {
            if (EmitVarLikeStmt(c, init, depth + 1u, 0) != 0) {
                return -1;
            }
        } else if (NodeAt(c, init)->kind == SLAst_CONST) {
            if (EmitVarLikeStmt(c, init, depth + 1u, 1) != 0) {
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
    if (BufAppendCStr(&c->out, ")\n") != 0) {
        return -1;
    }
    if (EmitStmt(c, body, init >= 0 ? depth + 1u : depth) != 0) {
        return -1;
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

static int EmitSwitchStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLAstNode* sw = NodeAt(c, nodeId);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          subject = -1;
    int              firstClause = 1;

    if (sw == NULL) {
        return -1;
    }

    if (sw->flags == 1) {
        subject = child;
        child = AstNextSibling(&c->ast, child);
    }

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "do {\n") != 0) {
        return -1;
    }

    while (child >= 0) {
        const SLAstNode* clause = NodeAt(c, child);
        if (clause != NULL && clause->kind == SLAst_CASE) {
            int32_t caseChild = AstFirstChild(&c->ast, child);
            int32_t bodyNode = -1;
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, firstClause ? "if (" : "else if (") != 0) {
                return -1;
            }
            firstClause = 0;

            while (caseChild >= 0) {
                int32_t next = AstNextSibling(&c->ast, caseChild);
                if (next < 0) {
                    bodyNode = caseChild;
                    break;
                }
                if (sw->flags == 1) {
                    if (BufAppendChar(&c->out, '(') != 0 || EmitExpr(c, subject) != 0
                        || BufAppendCStr(&c->out, ") == (") != 0 || EmitExpr(c, caseChild) != 0
                        || BufAppendChar(&c->out, ')') != 0)
                    {
                        return -1;
                    }
                } else if (EmitExpr(c, caseChild) != 0) {
                    return -1;
                }
                if (AstNextSibling(&c->ast, next) >= 0) {
                    if (BufAppendCStr(&c->out, " || ") != 0) {
                        return -1;
                    }
                }
                caseChild = next;
            }

            if (BufAppendCStr(&c->out, ")\n") != 0) {
                return -1;
            }
            if (EmitStmt(c, bodyNode, depth + 1u) != 0) {
                return -1;
            }
        } else if (clause != NULL && clause->kind == SLAst_DEFAULT) {
            int32_t bodyNode = AstFirstChild(&c->ast, child);
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, firstClause ? "if (1)\n" : "else\n") != 0) {
                return -1;
            }
            firstClause = 0;
            if (EmitStmt(c, bodyNode, depth + 1u) != 0) {
                return -1;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }

    EmitIndent(c, depth);
    return BufAppendCStr(&c->out, "} while (0);\n");
}

static int EmitAssertFormatArg(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == SLAst_STRING) {
        return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
    }
    if (BufAppendCStr(&c->out, "(const char*)(const void*)__sl_cstr(") != 0
        || EmitExpr(c, nodeId) != 0 || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAst_BLOCK:     return EmitBlock(c, nodeId, depth);
        case SLAst_VAR:       return EmitVarLikeStmt(c, nodeId, depth, 0);
        case SLAst_CONST:     return EmitVarLikeStmt(c, nodeId, depth, 1);
        case SLAst_EXPR_STMT: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            EmitIndent(c, depth);
            if (EmitExpr(c, expr) != 0 || BufAppendCStr(&c->out, ";\n") != 0) {
                return -1;
            }
            return 0;
        }
        case SLAst_RETURN: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            if (EmitDeferredRange(c, 0, depth) != 0) {
                return -1;
            }
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "return") != 0) {
                return -1;
            }
            if (expr >= 0) {
                if (BufAppendChar(&c->out, ' ') != 0
                    || EmitExprCoerced(
                           c, expr, c->hasCurrentReturnType ? &c->currentReturnType : NULL)
                           != 0)
                {
                    return -1;
                }
            }
            return BufAppendCStr(&c->out, ";\n");
        }
        case SLAst_ASSERT: {
            int32_t cond = AstFirstChild(&c->ast, nodeId);
            int32_t fmtNode;
            if (cond < 0) {
                return -1;
            }
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "do {\n") != 0) {
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "if (!(") != 0 || EmitExpr(c, cond) != 0
                || BufAppendCStr(&c->out, ")) {\n") != 0)
            {
                return -1;
            }
            fmtNode = AstNextSibling(&c->ast, cond);
            EmitIndent(c, depth + 2u);
            if (fmtNode < 0) {
                if (BufAppendCStr(
                        &c->out, "__sl_assert_fail(__FILE__, __LINE__, \"assertion failed\");\n")
                    != 0)
                {
                    return -1;
                }
            } else {
                int32_t argNode;
                if (BufAppendCStr(&c->out, "__sl_assertf_fail(__FILE__, __LINE__, ") != 0
                    || EmitAssertFormatArg(c, fmtNode) != 0)
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
            EmitIndent(c, depth + 1u);
            if (BufAppendCStr(&c->out, "}\n") != 0) {
                return -1;
            }
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "} while (0);\n");
        }
        case SLAst_IF: {
            int32_t cond = AstFirstChild(&c->ast, nodeId);
            int32_t thenNode = AstNextSibling(&c->ast, cond);
            int32_t elseNode = AstNextSibling(&c->ast, thenNode);
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "if (") != 0 || EmitExpr(c, cond) != 0
                || BufAppendCStr(&c->out, ")\n") != 0)
            {
                return -1;
            }
            if (EmitStmt(c, thenNode, depth) != 0) {
                return -1;
            }
            if (elseNode >= 0) {
                EmitIndent(c, depth);
                if (BufAppendCStr(&c->out, "else\n") != 0) {
                    return -1;
                }
                if (EmitStmt(c, elseNode, depth) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_FOR:    return EmitForStmt(c, nodeId, depth);
        case SLAst_SWITCH: return EmitSwitchStmt(c, nodeId, depth);
        case SLAst_BREAK:
            if (c->deferScopeLen > 0
                && EmitDeferredRange(c, c->deferScopeMarks[c->deferScopeLen - 1u], depth) != 0)
            {
                return -1;
            }
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "break;\n");
        case SLAst_CONTINUE:
            if (c->deferScopeLen > 0
                && EmitDeferredRange(c, c->deferScopeMarks[c->deferScopeLen - 1u], depth) != 0)
            {
                return -1;
            }
            EmitIndent(c, depth);
            return BufAppendCStr(&c->out, "continue;\n");
        case SLAst_DEFER: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (child < 0) {
                return -1;
            }
            return AddDeferredStmt(c, child);
        }
        default: SetDiag(c->diag, SLDiag_UNEXPECTED_TOKEN, n->start, n->end); return -1;
    }
}

static int IsMainFunctionNode(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    return n != NULL && n->kind == SLAst_FN
        && SliceEq(c->unit->source, n->dataStart, n->dataEnd, "main");
}

static int IsExplicitlyExportedNode(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map;
    if (n == NULL) {
        return 0;
    }
    map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    return map != NULL && map->kind == n->kind && map->isExported;
}

static int IsExportedNode(const SLCBackendC* c, int32_t nodeId) {
    if (IsMainFunctionNode(c, nodeId)) {
        return 1;
    }
    return IsExplicitlyExportedNode(c, nodeId);
}

static int IsExportedTypeNode(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map;
    if (n == NULL || !IsTypeDeclKind(n->kind)) {
        return 0;
    }
    map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    return map != NULL && IsTypeDeclKind(map->kind) && map->isExported;
}

static int EmitEnumDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int              first = 1;

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef enum ") != 0 || BufAppendCStr(&c->out, map->cName) != 0
        || BufAppendCStr(&c->out, " {\n") != 0)
    {
        return -1;
    }

    if (child >= 0) {
        const SLAstNode* firstChild = NodeAt(c, child);
        if (firstChild != NULL
            && (firstChild->kind == SLAst_TYPE_NAME || firstChild->kind == SLAst_TYPE_PTR
                || firstChild->kind == SLAst_TYPE_REF || firstChild->kind == SLAst_TYPE_MUTREF
                || firstChild->kind == SLAst_TYPE_ARRAY || firstChild->kind == SLAst_TYPE_VARRAY
                || firstChild->kind == SLAst_TYPE_SLICE || firstChild->kind == SLAst_TYPE_MUTSLICE
                || firstChild->kind == SLAst_TYPE_OPTIONAL))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }

    while (child >= 0) {
        const SLAstNode* item = NodeAt(c, child);
        if (item != NULL && item->kind == SLAst_FIELD) {
            int32_t initExpr = AstFirstChild(&c->ast, child);
            EmitIndent(c, depth + 1u);
            if (!first && BufAppendCStr(&c->out, ",\n") != 0) {
                return -1;
            }
            if (first) {
                first = 0;
            }
            if (BufAppendSlice(&c->out, c->unit->source, item->dataStart, item->dataEnd) != 0) {
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

static int NodeHasDirectDependentFields(SLCBackendC* c, int32_t nodeId) {
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

static int EmitVarSizeStructDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
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
                    || BufAppendCStr(&c->out, "* p)\n") != 0)
                {
                    return -1;
                }
                EmitIndent(c, depth);
                if (BufAppendCStr(&c->out, "{\n") != 0) {
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
            || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, "* p)\n") != 0)
        {
            return -1;
        }
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "{\n") != 0) {
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

static int EmitStructOrUnionDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isUnion) {
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

static int EmitForwardTypeDecls(SLCBackendC* c) {
    uint32_t i;
    int      emittedAny = 0;
    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        const SLNameMap* map;
        if (n == NULL || (n->kind != SLAst_STRUCT && n->kind != SLAst_UNION)) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (n->kind == SLAst_STRUCT && NodeHasDirectDependentFields(c, nodeId)) {
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
        if (n == NULL || (n->kind != SLAst_STRUCT && n->kind != SLAst_UNION)) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (n->kind == SLAst_STRUCT && NodeHasDirectDependentFields(c, nodeId)) {
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

static int FnNodeHasBody(const SLCBackendC* c, int32_t nodeId) {
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

static int HasFunctionBodyForName(const SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    uint32_t         i;
    if (n == NULL || n->kind != SLAst_FN) {
        return 0;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          otherId = c->topDecls[i].nodeId;
        const SLAstNode* other = NodeAt(c, otherId);
        if (other == NULL || other->kind != SLAst_FN || otherId == nodeId
            || !FnNodeHasBody(c, otherId))
        {
            continue;
        }
        if (SliceSpanEq(
                c->unit->source, n->dataStart, n->dataEnd, other->dataStart, other->dataEnd))
        {
            return 1;
        }
    }
    return 0;
}

static int EmitFnDeclOrDef(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int emitBody, int isPrivate) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          bodyNode = -1;
    int32_t          returnTypeNode = -1;
    int              firstParam = 1;
    uint32_t         savedLocalLen;
    SLTypeRef        savedReturnType = c->currentReturnType;
    int              savedHasReturnType = c->hasCurrentReturnType;
    SLTypeRef        fnReturnType;

    TypeRefSetScalar(&fnReturnType, "void");

    EmitIndent(c, depth);
    if (isPrivate && (emitBody || c->emitPrivateFnDeclStatic)
        && BufAppendCStr(&c->out, "static ") != 0)
    {
        return -1;
    }

    while (child >= 0) {
        const SLAstNode* ch = NodeAt(c, child);
        if (ch != NULL
            && (ch->kind == SLAst_TYPE_NAME || ch->kind == SLAst_TYPE_PTR
                || ch->kind == SLAst_TYPE_REF || ch->kind == SLAst_TYPE_MUTREF
                || ch->kind == SLAst_TYPE_ARRAY || ch->kind == SLAst_TYPE_VARRAY
                || ch->kind == SLAst_TYPE_SLICE || ch->kind == SLAst_TYPE_MUTSLICE
                || ch->kind == SLAst_TYPE_OPTIONAL)
            && ch->flags == 1)
        {
            returnTypeNode = child;
        } else if (ch != NULL && ch->kind == SLAst_BLOCK) {
            bodyNode = child;
        }
        child = AstNextSibling(&c->ast, child);
    }

    if (returnTypeNode >= 0) {
        if (ParseTypeRef(c, returnTypeNode, &fnReturnType) != 0) {
            return -1;
        }
        if (EmitTypeWithName(c, returnTypeNode, map->cName) != 0) {
            return -1;
        }
    } else if (BufAppendCStr(&c->out, "void ") != 0 || BufAppendCStr(&c->out, map->cName) != 0) {
        return -1;
    }

    if (BufAppendChar(&c->out, '(') != 0) {
        return -1;
    }

    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == SLAst_PARAM) {
            int32_t typeNode = AstFirstChild(&c->ast, child);
            char*   paramName = DupSlice(c, c->unit->source, ch->dataStart, ch->dataEnd);
            if (paramName == NULL) {
                return -1;
            }
            if (!firstParam && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (EmitTypeWithName(c, typeNode, paramName) != 0) {
                return -1;
            }
            firstParam = 0;
        }
        child = AstNextSibling(&c->ast, child);
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

    if (BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }

    savedLocalLen = c->localLen;
    if (PushScope(c) != 0) {
        return -1;
    }
    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == SLAst_PARAM) {
            int32_t   typeNode = AstFirstChild(&c->ast, child);
            SLTypeRef t;
            char*     paramName = DupSlice(c, c->unit->source, ch->dataStart, ch->dataEnd);
            if (paramName == NULL) {
                return -1;
            }
            if (ParseTypeRef(c, typeNode, &t) != 0 || AddLocal(c, paramName, t) != 0) {
                return -1;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }

    c->currentReturnType = fnReturnType;
    c->hasCurrentReturnType = 1;
    if (EmitBlock(c, bodyNode, depth) != 0) {
        c->currentReturnType = savedReturnType;
        c->hasCurrentReturnType = savedHasReturnType;
        return -1;
    }
    c->currentReturnType = savedReturnType;
    c->hasCurrentReturnType = savedHasReturnType;

    PopScope(c);
    c->localLen = savedLocalLen;
    return 0;
}

static int EmitConstDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          typeNode = AstFirstChild(&c->ast, nodeId);
    int32_t          initNode = AstNextSibling(&c->ast, typeNode);
    SLTypeRef        type;

    if (ParseTypeRef(c, typeNode, &type) != 0) {
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

    if (EmitTypeWithName(c, typeNode, map->cName) != 0) {
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

    return BufAppendCStr(&c->out, ";\n");
}

static int EmitDeclNode(
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
        case SLAst_FN:     return EmitFnDeclOrDef(c, nodeId, depth, emitBody, isPrivate);
        case SLAst_CONST:  return EmitConstDecl(c, nodeId, depth, declarationOnly, isPrivate);
        default:           return 0;
    }
}

static int EmitPrelude(SLCBackendC* c) {
    return BufAppendCStr(&c->out, "#include <sl-prelude.h>\n");
}

static char* _Nullable BuildDefaultMacro(SLCBackendC* c, const char* pkgName, const char* suffix) {
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

static int EmitHeader(SLCBackendC* c) {
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

    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n == NULL) {
            continue;
        }
        if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
    }

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t nodeId = c->topDecls[i].nodeId;
        if (IsMainFunctionNode(c, nodeId) && !IsExplicitlyExportedNode(c, nodeId)) {
            if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
                return -1;
            }
            break;
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

    c->emitPrivateFnDeclStatic = 1;
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        int              exported;
        if (n == NULL || n->kind != SLAst_FN || !FnNodeHasBody(c, nodeId)) {
            continue;
        }
        exported = IsExportedNode(c, nodeId);
        if (EmitDeclNode(c, nodeId, 0, 1, !exported, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
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

    if (BufAppendCStr(&c->out, "#endif /* ") != 0 || BufAppendCStr(&c->out, impl) != 0
        || BufAppendCStr(&c->out, " */\n\n#endif /* ") != 0 || BufAppendCStr(&c->out, guard) != 0
        || BufAppendCStr(&c->out, " */\n") != 0)
    {
        return -1;
    }
    return 0;
}

static int InitAst(SLCBackendC* c) {
    SLDiag diag;
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
    if (SLParse(&c->arena, (SLStrView){ c->unit->source, c->unit->sourceLen }, &c->ast, &diag) != 0)
    {
        if (c->diag != NULL) {
            *c->diag = diag;
        }
        return -1;
    }
    return 0;
}

static char* _Nullable AllocOutputCopy(SLCBackendC* c) {
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

static void FreeContext(SLCBackendC* c) {
    SLArenaDispose(&c->arena);
}

static int EmitCBackend(
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

    if (diag != NULL) {
        SLDiagClear(diag);
    }
    *outHeader = NULL;

    if (InitAst(&c) != 0) {
        FreeContext(&c);
        return -1;
    }
    if (CollectDeclSets(&c) != 0) {
        SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        FreeContext(&c);
        return -1;
    }
    if (CollectFnAndFieldInfo(&c) != 0) {
        SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        FreeContext(&c);
        return -1;
    }
    if (CollectVarSizeTypesFromDeclSets(&c) != 0 || PropagateVarSizeTypes(&c) != 0) {
        SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
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
            SetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
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
