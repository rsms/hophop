#include "codegen.h"
#include "libsl-impl.h"

SL_API_BEGIN

typedef struct {
    const char* baseName;
    int         ptrDepth;
    int         valid;
    int         containerKind; /* 0 scalar, 1 array, 2 ro-slice, 3 rw-slice */
    int         containerPtrDepth;
    uint32_t    arrayLen;
    int         hasArrayLen;
    int         readOnly;
    int         isOptional;
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
    char*      slName;
    char*      cName;
    SLTypeRef  returnType;
    SLTypeRef* paramTypes;
    uint32_t   paramLen;
    SLTypeRef  contextType;
    int        hasContext;
} SLFnSig;

typedef struct {
    char*      aliasName;
    SLTypeRef  returnType;
    SLTypeRef* paramTypes;
    uint32_t   paramLen;
} SLFnTypeAlias;

typedef struct {
    char*     aliasName;
    SLTypeRef targetType;
} SLTypeAliasInfo;

typedef struct {
    char*    name;
    uint32_t memberStart;
    uint16_t memberCount;
} SLFnGroup;

typedef struct {
    char*     ownerType;
    char*     fieldName;
    char*     lenFieldName;
    int       isDependent;
    int       isEmbedded;
    int32_t   defaultExprNode;
    SLTypeRef type;
} SLFieldInfo;

typedef struct {
    char* cName;
    int   isUnion;
    int   isVarSize;
} SLVarSizeType;

typedef struct {
    char*    key;
    char*    cName;
    int      isUnion;
    uint32_t fieldStart;
    uint16_t fieldCount;
    uint16_t flags;
} SLAnonTypeInfo;

enum {
    SLAnonTypeFlag_EMITTED_GLOBAL = 1u << 0,
};

typedef struct {
    char*     name;
    SLTypeRef type;
} SLLocal;

typedef struct {
    int32_t nodeId;
    char*   cName;
} SLFnNodeName;

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

    SLFnTypeAlias* fnTypeAliases;
    uint32_t       fnTypeAliasLen;
    uint32_t       fnTypeAliasCap;

    SLTypeAliasInfo* typeAliases;
    uint32_t         typeAliasLen;
    uint32_t         typeAliasCap;

    SLFnGroup* fnGroups;
    uint32_t   fnGroupLen;
    uint32_t   fnGroupCap;

    char**   fnGroupMembers;
    uint32_t fnGroupMemberLen;
    uint32_t fnGroupMemberCap;

    SLFieldInfo* fieldInfos;
    uint32_t     fieldInfoLen;
    uint32_t     fieldInfoCap;

    SLVarSizeType* varSizeTypes;
    uint32_t       varSizeTypeLen;
    uint32_t       varSizeTypeCap;

    SLAnonTypeInfo* anonTypes;
    uint32_t        anonTypeLen;
    uint32_t        anonTypeCap;

    SLLocal* locals;
    uint32_t localLen;
    uint32_t localCap;

    SLFnNodeName* fnNodeNames;
    uint32_t      fnNodeNameLen;
    uint32_t      fnNodeNameCap;

    uint32_t* localScopeMarks;
    uint32_t  localScopeLen;
    uint32_t  localScopeCap;

    char**   localAnonTypedefs;
    uint32_t localAnonTypedefLen;
    uint32_t localAnonTypedefCap;

    uint32_t* localAnonTypedefScopeMarks;
    uint32_t  localAnonTypedefScopeLen;
    uint32_t  localAnonTypedefScopeCap;

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
    SLTypeRef currentContextType;
    int       hasCurrentContext;
    int       currentFunctionIsMain;
    int32_t   activeCallWithNode;
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

static int StrHasPrefix(const char* s, const char* prefix) {
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
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
    return StrEq(s, "void") || StrEq(s, "bool") || StrEq(s, "str") || StrEq(s, "u8")
        || StrEq(s, "u16") || StrEq(s, "u32") || StrEq(s, "u64") || StrEq(s, "i8")
        || StrEq(s, "i16") || StrEq(s, "i32") || StrEq(s, "i64") || StrEq(s, "uint")
        || StrEq(s, "int") || StrEq(s, "f32") || StrEq(s, "f64");
}

static int IsIntegerCTypeName(const char* s) {
    return StrEq(s, "__sl_u8") || StrEq(s, "__sl_u16") || StrEq(s, "__sl_u32")
        || StrEq(s, "__sl_u64") || StrEq(s, "__sl_uint") || StrEq(s, "__sl_i8")
        || StrEq(s, "__sl_i16") || StrEq(s, "__sl_i32") || StrEq(s, "__sl_i64")
        || StrEq(s, "__sl_int");
}

static int IsFloatCTypeName(const char* s) {
    return StrEq(s, "__sl_f32") || StrEq(s, "__sl_f64");
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
    t->isOptional = 0;
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
    t->isOptional = 0;
}

static const char* ResolveScalarAliasBaseName(const SLCBackendC* c, const char* typeName);

static void CanonicalizeTypeRefBaseName(const SLCBackendC* c, SLTypeRef* t) {
    const char* canonical;
    if (t == NULL || !t->valid || t->baseName == NULL) {
        return;
    }
    canonical = ResolveScalarAliasBaseName(c, t->baseName);
    if (canonical != NULL) {
        t->baseName = canonical;
    }
}

static int TypeRefEqual(const SLTypeRef* a, const SLTypeRef* b);
static int AddFieldInfo(
    SLCBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int32_t   defaultExprNode,
    int       isDependent,
    int       isEmbedded,
    SLTypeRef type);
static const SLAnonTypeInfo* _Nullable FindAnonTypeByCName(const SLCBackendC* c, const char* cName);
static int EnsureAnonTypeByFields(
    SLCBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const SLTypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName);
static int EnsureAnonTypeVisible(SLCBackendC* c, const SLTypeRef* type, uint32_t depth);
static int EmitTypeRefWithName(SLCBackendC* c, const SLTypeRef* t, const char* name);
static int EmitDeclNode(
    SLCBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          declarationOnly,
    int          isPrivate,
    int          emitBody);
static int IsStrBaseName(const char* _Nullable s);

static int SliceStructPtrDepth(const SLTypeRef* t) {
    int stars = t->ptrDepth;
    if (t->containerPtrDepth > 0) {
        stars += t->containerPtrDepth - 1;
    }
    return stars;
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
        SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, start, end);
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
    return kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS;
}

static int IsDeclKind(SLAstKind kind) {
    return kind == SLAst_FN || kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS || kind == SLAst_VAR || kind == SLAst_CONST
        || kind == SLAst_FN_GROUP;
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

static int NameHasPrefixSuffix(const char* name, const char* prefix, const char* suffix) {
    size_t nameLen;
    size_t prefixLen;
    size_t suffixLen;
    if (name == NULL || prefix == NULL || suffix == NULL) {
        return 0;
    }
    nameLen = StrLen(name);
    prefixLen = StrLen(prefix);
    suffixLen = StrLen(suffix);
    if (nameLen < prefixLen + suffixLen) {
        return 0;
    }
    return memcmp(name, prefix, prefixLen) == 0
        && memcmp(name + nameLen - suffixLen, suffix, suffixLen) == 0;
}

static int ResolveMainSemanticContextType(SLCBackendC* c, SLTypeRef* outType) {
    const SLNameMap* map = FindNameByCString(c, "core__Context");
    uint32_t         i;
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        TypeRefSetScalar(outType, map->cName);
        return 0;
    }
    for (i = 0; i < c->nameLen; i++) {
        if (IsTypeDeclKind(c->names[i].kind)
            && NameHasPrefixSuffix(c->names[i].name, "core", "__Context"))
        {
            TypeRefSetScalar(outType, c->names[i].cName);
            return 0;
        }
    }
    map = FindNameByCString(c, "Context");
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        TypeRefSetScalar(outType, map->cName);
        return 0;
    }
    TypeRefSetScalar(outType, "__sl_MainContext");
    return 0;
}

static const SLNameMap* _Nullable FindNameByCName(const SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (StrEq(c->names[i].cName, cName)) {
            return &c->names[i];
        }
    }
    return NULL;
}

static const SLTypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const SLCBackendC* c, const char* aliasName) {
    uint32_t i;
    for (i = 0; i < c->typeAliasLen; i++) {
        if (StrEq(c->typeAliases[i].aliasName, aliasName)) {
            return &c->typeAliases[i];
        }
    }
    return NULL;
}

static int AddTypeAliasInfo(SLCBackendC* c, const char* aliasName, SLTypeRef targetType) {
    if (FindTypeAliasInfoByAliasName(c, aliasName) != NULL) {
        return 0;
    }
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->typeAliases,
            &c->typeAliasCap,
            c->typeAliasLen + 1u,
            sizeof(SLTypeAliasInfo),
            (uint32_t)_Alignof(SLTypeAliasInfo))
        != 0)
    {
        return -1;
    }
    c->typeAliases[c->typeAliasLen].aliasName = (char*)aliasName;
    c->typeAliases[c->typeAliasLen].targetType = targetType;
    c->typeAliasLen++;
    return 0;
}

static const char* ResolveScalarAliasBaseName(const SLCBackendC* c, const char* typeName) {
    uint32_t guard = 0;
    while (typeName != NULL && guard++ <= c->typeAliasLen) {
        const SLTypeAliasInfo* alias = FindTypeAliasInfoByAliasName(c, typeName);
        if (alias == NULL || !alias->targetType.valid || alias->targetType.baseName == NULL
            || alias->targetType.containerKind != SLTypeContainer_SCALAR
            || alias->targetType.ptrDepth != 0 || alias->targetType.containerPtrDepth != 0
            || alias->targetType.isOptional)
        {
            break;
        }
        typeName = alias->targetType.baseName;
    }
    return typeName;
}

static const char* _Nullable ResolveTypeName(SLCBackendC* c, uint32_t start, uint32_t end) {
    const SLNameMap*         mapped;
    char*                    normalized;
    uint32_t                 i;
    static const char* const builtinSlNames[] = {
        "void", "bool", "str", "u8",   "u16", "u32", "u64", "i8",
        "i16",  "i32",  "i64", "uint", "int", "f32", "f64",
    };
    static const char* const builtinCNames[] = {
        "void",     "__sl_bool", "__sl_str", "__sl_u8",  "__sl_u16",
        "__sl_u32", "__sl_u64",  "__sl_i8",  "__sl_i16", "__sl_i32",
        "__sl_i64", "__sl_uint", "__sl_int", "__sl_f32", "__sl_f64",
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

static const SLFnTypeAlias* _Nullable FindFnTypeAliasByName(
    const SLCBackendC* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        if (StrEq(c->fnTypeAliases[i].aliasName, name)) {
            return &c->fnTypeAliases[i];
        }
    }
    return NULL;
}

static int EnsureFnTypeAlias(
    SLCBackendC* c,
    SLTypeRef    returnType,
    SLTypeRef*   paramTypes,
    uint32_t     paramLen,
    const char** outAliasName) {
    uint32_t i;
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        const SLFnTypeAlias* alias = &c->fnTypeAliases[i];
        uint32_t             p;
        int                  same = 1;
        if (!TypeRefEqual(&alias->returnType, &returnType) || alias->paramLen != paramLen) {
            continue;
        }
        for (p = 0; p < paramLen; p++) {
            if (!TypeRefEqual(&alias->paramTypes[p], &paramTypes[p])) {
                same = 0;
                break;
            }
        }
        if (same) {
            *outAliasName = alias->aliasName;
            return 0;
        }
    }

    {
        SLTypeRef* paramCopy = NULL;
        char*      aliasName;
        SLBuf      b = { 0 };
        if (paramLen > 0) {
            uint32_t p;
            paramCopy = (SLTypeRef*)SLArenaAlloc(
                &c->arena, (uint32_t)(sizeof(SLTypeRef) * paramLen), (uint32_t)_Alignof(SLTypeRef));
            if (paramCopy == NULL) {
                return -1;
            }
            for (p = 0; p < paramLen; p++) {
                paramCopy[p] = paramTypes[p];
            }
        }
        b.arena = &c->arena;
        if (BufAppendCStr(&b, "__sl_fn_t_") != 0 || BufAppendU32(&b, c->fnTypeAliasLen) != 0) {
            return -1;
        }
        aliasName = BufFinish(&b);
        if (aliasName == NULL) {
            return -1;
        }
        if (EnsureCapArena(
                &c->arena,
                (void**)&c->fnTypeAliases,
                &c->fnTypeAliasCap,
                c->fnTypeAliasLen + 1u,
                sizeof(SLFnTypeAlias),
                (uint32_t)_Alignof(SLFnTypeAlias))
            != 0)
        {
            return -1;
        }
        c->fnTypeAliases[c->fnTypeAliasLen].aliasName = aliasName;
        c->fnTypeAliases[c->fnTypeAliasLen].returnType = returnType;
        c->fnTypeAliases[c->fnTypeAliasLen].paramTypes = paramCopy;
        c->fnTypeAliases[c->fnTypeAliasLen].paramLen = paramLen;
        c->fnTypeAliasLen++;
        *outAliasName = aliasName;
        return 0;
    }
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
            if (outType->baseName != NULL && StrEq(outType->baseName, "core__str")) {
                outType->baseName = "__sl_str";
            } else if (outType->baseName != NULL && StrEq(outType->baseName, "core__Allocator")) {
                outType->baseName = "__sl_Allocator";
            }
            return 0;
        }
        case SLAst_TYPE_ANON_STRUCT:
        case SLAst_TYPE_ANON_UNION:  {
            int32_t     fieldNode = AstFirstChild(&c->ast, nodeId);
            const char* fieldNames[256];
            SLTypeRef   fieldTypes[256];
            uint32_t    fieldCount = 0;
            const char* anonName;
            while (fieldNode >= 0) {
                const SLAstNode* field = NodeAt(c, fieldNode);
                int32_t          typeNode;
                if (field == NULL || field->kind != SLAst_FIELD) {
                    return -1;
                }
                if (fieldCount >= (uint32_t)(sizeof(fieldNames) / sizeof(fieldNames[0]))) {
                    return -1;
                }
                typeNode = AstFirstChild(&c->ast, fieldNode);
                if (typeNode < 0 || ParseTypeRef(c, typeNode, &fieldTypes[fieldCount]) != 0) {
                    return -1;
                }
                CanonicalizeTypeRefBaseName(c, &fieldTypes[fieldCount]);
                fieldNames[fieldCount] = DupSlice(
                    c, c->unit->source, field->dataStart, field->dataEnd);
                if (fieldNames[fieldCount] == NULL) {
                    return -1;
                }
                fieldCount++;
                fieldNode = AstNextSibling(&c->ast, fieldNode);
            }
            if (EnsureAnonTypeByFields(
                    c,
                    n->kind == SLAst_TYPE_ANON_UNION,
                    fieldNames,
                    fieldTypes,
                    fieldCount,
                    &anonName)
                != 0)
            {
                return -1;
            }
            TypeRefSetScalar(outType, anonName);
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
                if (childType.containerKind == SLTypeContainer_SLICE_RO
                    || childType.containerKind == SLTypeContainer_SLICE_MUT)
                {
                    childType.containerKind =
                        isRef ? SLTypeContainer_SLICE_RO : SLTypeContainer_SLICE_MUT;
                }
                if (isRef) {
                    childType.readOnly = isReadOnlyRef;
                } else {
                    childType.readOnly = 0;
                }
            } else {
                childType.ptrDepth++;
                if (isRef) {
                    childType.readOnly = isReadOnlyRef;
                } else {
                    childType.readOnly = 0;
                }
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
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (ParseTypeRef(c, child, outType) != 0) {
                return -1;
            }
            outType->isOptional = 1;
            return 0;
        }
        case SLAst_TYPE_FN: {
            int32_t     child = AstFirstChild(&c->ast, nodeId);
            SLTypeRef   returnType;
            SLTypeRef*  paramTypes = NULL;
            uint32_t    paramLen = 0;
            uint32_t    paramCap = 0;
            const char* aliasName;
            TypeRefSetScalar(&returnType, "void");
            while (child >= 0) {
                const SLAstNode* ch = NodeAt(c, child);
                if (ch != NULL && ch->flags == 1) {
                    if (ParseTypeRef(c, child, &returnType) != 0) {
                        return -1;
                    }
                } else {
                    SLTypeRef paramType;
                    if (ParseTypeRef(c, child, &paramType) != 0) {
                        return -1;
                    }
                    if (paramLen >= paramCap) {
                        if (EnsureCapArena(
                                &c->arena,
                                (void**)&paramTypes,
                                &paramCap,
                                paramLen + 1u,
                                sizeof(SLTypeRef),
                                (uint32_t)_Alignof(SLTypeRef))
                            != 0)
                        {
                            return -1;
                        }
                    }
                    paramTypes[paramLen++] = paramType;
                }
                child = AstNextSibling(&c->ast, child);
            }
            if (EnsureFnTypeAlias(c, returnType, paramTypes, paramLen, &aliasName) != 0) {
                return -1;
            }
            TypeRefSetScalar(outType, aliasName);
            return 0;
        }
        default: TypeRefSetInvalid(outType); return -1;
    }
}

static int AddFnSig(
    SLCBackendC* c,
    const char*  slName,
    const char*  baseCName,
    int32_t      nodeId,
    SLTypeRef    returnType,
    SLTypeRef*   paramTypes,
    uint32_t     paramLen,
    int          hasContext,
    SLTypeRef    contextType) {
    const char* cName = baseCName;
    uint32_t    i;

    for (i = 0; i < c->fnSigLen; i++) {
        uint32_t p;
        int      sameSig = 1;
        if (!StrEq(c->fnSigs[i].slName, slName) || c->fnSigs[i].paramLen != paramLen
            || !TypeRefEqual(&c->fnSigs[i].returnType, &returnType))
        {
            continue;
        }
        if (c->fnSigs[i].hasContext != hasContext) {
            continue;
        }
        if (hasContext && !TypeRefEqual(&c->fnSigs[i].contextType, &contextType)) {
            continue;
        }
        for (p = 0; p < paramLen; p++) {
            if (!TypeRefEqual(&c->fnSigs[i].paramTypes[p], &paramTypes[p])) {
                sameSig = 0;
                break;
            }
        }
        if (sameSig) {
            uint32_t idx = c->fnNodeNameLen;
            for (idx = 0; idx < c->fnNodeNameLen; idx++) {
                if (c->fnNodeNames[idx].nodeId == nodeId) {
                    c->fnNodeNames[idx].cName = c->fnSigs[i].cName;
                    return 0;
                }
            }
            if (EnsureCapArena(
                    &c->arena,
                    (void**)&c->fnNodeNames,
                    &c->fnNodeNameCap,
                    c->fnNodeNameLen + 1u,
                    sizeof(SLFnNodeName),
                    (uint32_t)_Alignof(SLFnNodeName))
                != 0)
            {
                return -1;
            }
            c->fnNodeNames[c->fnNodeNameLen].nodeId = nodeId;
            c->fnNodeNames[c->fnNodeNameLen].cName = c->fnSigs[i].cName;
            c->fnNodeNameLen++;
            return 0;
        }
    }

    for (i = 0; i < c->fnSigLen; i++) {
        if (StrEq(c->fnSigs[i].cName, cName)) {
            uint32_t suffix = 1;
            while (1) {
                SLBuf    b = { 0 };
                char*    candidate;
                uint32_t j;
                int      used = 0;
                b.arena = &c->arena;
                if (BufAppendCStr(&b, baseCName) != 0 || BufAppendCStr(&b, "__ov") != 0
                    || BufAppendU32(&b, suffix) != 0)
                {
                    return -1;
                }
                candidate = BufFinish(&b);
                if (candidate == NULL) {
                    return -1;
                }
                for (j = 0; j < c->fnSigLen; j++) {
                    if (StrEq(c->fnSigs[j].cName, candidate)) {
                        used = 1;
                        break;
                    }
                }
                if (!used) {
                    cName = candidate;
                    break;
                }
                suffix++;
            }
            break;
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
    c->fnSigs[c->fnSigLen].slName = (char*)slName;
    c->fnSigs[c->fnSigLen].cName = (char*)cName;
    c->fnSigs[c->fnSigLen].returnType = returnType;
    c->fnSigs[c->fnSigLen].paramTypes = paramTypes;
    c->fnSigs[c->fnSigLen].paramLen = paramLen;
    c->fnSigs[c->fnSigLen].hasContext = hasContext;
    c->fnSigs[c->fnSigLen].contextType = contextType;
    c->fnSigLen++;

    if (EnsureCapArena(
            &c->arena,
            (void**)&c->fnNodeNames,
            &c->fnNodeNameCap,
            c->fnNodeNameLen + 1u,
            sizeof(SLFnNodeName),
            (uint32_t)_Alignof(SLFnNodeName))
        != 0)
    {
        return -1;
    }
    c->fnNodeNames[c->fnNodeNameLen].nodeId = nodeId;
    c->fnNodeNames[c->fnNodeNameLen].cName = (char*)cName;
    c->fnNodeNameLen++;
    return 0;
}

static int AddFnGroup(SLCBackendC* c, const char* name, char** memberNames, uint16_t memberCount) {
    uint32_t i;
    for (i = 0; i < c->fnGroupLen; i++) {
        if (StrEq(c->fnGroups[i].name, name)) {
            c->fnGroups[i].memberStart = c->fnGroupMemberLen;
            c->fnGroups[i].memberCount = memberCount;
            break;
        }
    }
    if (i == c->fnGroupLen) {
        if (EnsureCapArena(
                &c->arena,
                (void**)&c->fnGroups,
                &c->fnGroupCap,
                c->fnGroupLen + 1u,
                sizeof(SLFnGroup),
                (uint32_t)_Alignof(SLFnGroup))
            != 0)
        {
            return -1;
        }
        c->fnGroups[c->fnGroupLen].name = (char*)name;
        c->fnGroups[c->fnGroupLen].memberStart = c->fnGroupMemberLen;
        c->fnGroups[c->fnGroupLen].memberCount = memberCount;
        c->fnGroupLen++;
    }
    for (i = 0; i < memberCount; i++) {
        if (EnsureCapArena(
                &c->arena,
                (void**)&c->fnGroupMembers,
                &c->fnGroupMemberCap,
                c->fnGroupMemberLen + 1u,
                sizeof(char*),
                (uint32_t)_Alignof(char*))
            != 0)
        {
            return -1;
        }
        c->fnGroupMembers[c->fnGroupMemberLen++] = memberNames[i];
    }
    return 0;
}

static int AddFieldInfo(
    SLCBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int32_t   defaultExprNode,
    int       isDependent,
    int       isEmbedded,
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
    c->fieldInfos[c->fieldInfoLen].defaultExprNode = defaultExprNode;
    c->fieldInfos[c->fieldInfoLen].isDependent = isDependent;
    c->fieldInfos[c->fieldInfoLen].isEmbedded = isEmbedded;
    c->fieldInfos[c->fieldInfoLen].type = type;
    c->fieldInfoLen++;
    return 0;
}

static int AppendTypeRefKey(SLBuf* b, const SLTypeRef* t) {
    if (t == NULL || !t->valid || t->baseName == NULL) {
        return BufAppendCStr(b, "!");
    }
    if (BufAppendChar(b, 'T') != 0 || BufAppendChar(b, '[') != 0) {
        return -1;
    }
    if (BufAppendCStr(b, t->baseName) != 0 || BufAppendChar(b, '|') != 0) {
        return -1;
    }
    if (BufAppendU32(b, (uint32_t)t->ptrDepth) != 0 || BufAppendChar(b, '|') != 0) {
        return -1;
    }
    if (BufAppendU32(b, (uint32_t)t->containerKind) != 0 || BufAppendChar(b, '|') != 0) {
        return -1;
    }
    if (BufAppendU32(b, (uint32_t)t->containerPtrDepth) != 0 || BufAppendChar(b, '|') != 0) {
        return -1;
    }
    if (BufAppendU32(b, (uint32_t)t->hasArrayLen) != 0 || BufAppendChar(b, '|') != 0) {
        return -1;
    }
    if (BufAppendU32(b, t->arrayLen) != 0 || BufAppendChar(b, '|') != 0) {
        return -1;
    }
    if (BufAppendU32(b, (uint32_t)t->readOnly) != 0 || BufAppendChar(b, '|') != 0) {
        return -1;
    }
    if (BufAppendU32(b, (uint32_t)t->isOptional) != 0 || BufAppendCStr(b, "]") != 0) {
        return -1;
    }
    return 0;
}

static const SLAnonTypeInfo* _Nullable FindAnonTypeByKey(const SLCBackendC* c, const char* key) {
    uint32_t i;
    for (i = 0; i < c->anonTypeLen; i++) {
        if (StrEq(c->anonTypes[i].key, key)) {
            return &c->anonTypes[i];
        }
    }
    return NULL;
}

static const SLAnonTypeInfo* _Nullable FindAnonTypeByCName(
    const SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->anonTypeLen; i++) {
        if (StrEq(c->anonTypes[i].cName, cName)) {
            return &c->anonTypes[i];
        }
    }
    return NULL;
}

static int IsLocalAnonTypedefVisible(const SLCBackendC* c, const char* cName) {
    uint32_t i = c->localAnonTypedefLen;
    while (i > 0) {
        i--;
        if (StrEq(c->localAnonTypedefs[i], cName)) {
            return 1;
        }
    }
    return 0;
}

static int MarkLocalAnonTypedefVisible(SLCBackendC* c, const char* cName) {
    if (IsLocalAnonTypedefVisible(c, cName)) {
        return 0;
    }
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->localAnonTypedefs,
            &c->localAnonTypedefCap,
            c->localAnonTypedefLen + 1u,
            sizeof(char*),
            (uint32_t)_Alignof(char*))
        != 0)
    {
        return -1;
    }
    c->localAnonTypedefs[c->localAnonTypedefLen++] = (char*)cName;
    return 0;
}

static int IsAnonTypeNameVisible(const SLCBackendC* c, const char* cName) {
    const SLAnonTypeInfo* info = FindAnonTypeByCName(c, cName);
    if (info != NULL && (info->flags & SLAnonTypeFlag_EMITTED_GLOBAL) != 0) {
        return 1;
    }
    return IsLocalAnonTypedefVisible(c, cName);
}

static int EmitAnonTypeDeclAtDepth(SLCBackendC* c, const SLAnonTypeInfo* t, uint32_t depth) {
    uint32_t j;
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef ") != 0
        || BufAppendCStr(&c->out, t->isUnion ? "union " : "struct ") != 0
        || BufAppendCStr(&c->out, t->cName) != 0 || BufAppendCStr(&c->out, " {\n") != 0)
    {
        return -1;
    }
    for (j = 0; j < t->fieldCount; j++) {
        const SLFieldInfo* f = &c->fieldInfos[t->fieldStart + j];
        EmitIndent(c, depth + 1u);
        if (EmitTypeRefWithName(c, &f->type, f->fieldName) != 0
            || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, t->cName) != 0
        || BufAppendCStr(&c->out, ";\n") != 0)
    {
        return -1;
    }
    return 0;
}

static int EnsureAnonTypeVisible(SLCBackendC* c, const SLTypeRef* type, uint32_t depth) {
    const SLAnonTypeInfo* info;
    uint32_t              i;
    if (type == NULL || !type->valid || type->containerKind != SLTypeContainer_SCALAR
        || type->containerPtrDepth != 0 || type->ptrDepth != 0 || type->isOptional
        || type->baseName == NULL)
    {
        return 0;
    }
    info = FindAnonTypeByCName(c, type->baseName);
    if (info == NULL) {
        return 0;
    }
    if ((info->flags & SLAnonTypeFlag_EMITTED_GLOBAL) != 0
        || IsLocalAnonTypedefVisible(c, info->cName))
    {
        return 0;
    }
    for (i = 0; i < info->fieldCount; i++) {
        const SLFieldInfo* f = &c->fieldInfos[info->fieldStart + i];
        if (EnsureAnonTypeVisible(c, &f->type, depth) != 0) {
            return -1;
        }
    }
    if (EmitAnonTypeDeclAtDepth(c, info, depth) != 0) {
        return -1;
    }
    if (depth == 0) {
        ((SLAnonTypeInfo*)info)->flags |= SLAnonTypeFlag_EMITTED_GLOBAL;
        if (BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
    } else if (MarkLocalAnonTypedefVisible(c, info->cName) != 0) {
        return -1;
    }
    return 0;
}

static int EnsureAnonTypeByFields(
    SLCBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const SLTypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName) {
    uint32_t i;
    SLBuf    keyBuf = { 0 };
    char*    key;

    keyBuf.arena = &c->arena;
    if (BufAppendCStr(&keyBuf, isUnion ? "U{" : "S{") != 0) {
        return -1;
    }
    for (i = 0; i < fieldCount; i++) {
        if (BufAppendCStr(&keyBuf, fieldNames[i]) != 0 || BufAppendChar(&keyBuf, ':') != 0
            || AppendTypeRefKey(&keyBuf, &fieldTypes[i]) != 0 || BufAppendChar(&keyBuf, ';') != 0)
        {
            return -1;
        }
    }
    if (BufAppendChar(&keyBuf, '}') != 0) {
        return -1;
    }
    key = BufFinish(&keyBuf);
    if (key == NULL) {
        return -1;
    }
    {
        const SLAnonTypeInfo* info = FindAnonTypeByKey(c, key);
        if (info != NULL) {
            *outCName = info->cName;
            return 0;
        }
    }

    if (EnsureCapArena(
            &c->arena,
            (void**)&c->anonTypes,
            &c->anonTypeCap,
            c->anonTypeLen + 1u,
            sizeof(SLAnonTypeInfo),
            (uint32_t)_Alignof(SLAnonTypeInfo))
        != 0)
    {
        return -1;
    }

    {
        SLBuf    cNameBuf = { 0 };
        char*    cName;
        uint32_t fieldStart = c->fieldInfoLen;
        cNameBuf.arena = &c->arena;
        if (BufAppendCStr(&cNameBuf, "__sl_anon_") != 0
            || BufAppendChar(&cNameBuf, isUnion ? 'u' : 's') != 0
            || BufAppendChar(&cNameBuf, '_') != 0 || BufAppendU32(&cNameBuf, c->anonTypeLen) != 0)
        {
            return -1;
        }
        cName = BufFinish(&cNameBuf);
        if (cName == NULL) {
            return -1;
        }

        for (i = 0; i < fieldCount; i++) {
            if (AddFieldInfo(c, cName, fieldNames[i], NULL, -1, 0, 0, fieldTypes[i]) != 0) {
                return -1;
            }
        }
        c->anonTypes[c->anonTypeLen].key = key;
        c->anonTypes[c->anonTypeLen].cName = cName;
        c->anonTypes[c->anonTypeLen].isUnion = isUnion;
        c->anonTypes[c->anonTypeLen].fieldStart = fieldStart;
        c->anonTypes[c->anonTypeLen].fieldCount = (uint16_t)fieldCount;
        c->anonTypes[c->anonTypeLen].flags = 0;
        c->anonTypeLen++;
        *outCName = cName;
        return 0;
    }
}

static const SLFnSig* _Nullable FindFnSigBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end) {
    const SLFnSig* found = NULL;
    uint32_t       i;
    for (i = 0; i < c->fnSigLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->fnSigs[i].slName)) {
            if (found != NULL) {
                return NULL;
            }
            found = &c->fnSigs[i];
        }
    }
    return found;
}

static uint32_t FindFnSigCandidatesBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end, const SLFnSig** out, uint32_t cap) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < c->fnSigLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->fnSigs[i].slName)) {
            if (n < cap) {
                out[n] = &c->fnSigs[i];
            }
            n++;
        }
    }
    return n;
}

static uint32_t FindFnSigCandidatesByName(
    const SLCBackendC* c, const char* slName, const SLFnSig** out, uint32_t cap) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < c->fnSigLen; i++) {
        if (StrEq(c->fnSigs[i].slName, slName)) {
            if (n < cap) {
                out[n] = &c->fnSigs[i];
            }
            n++;
        }
    }
    return n;
}

static const char* _Nullable FindFnCNameByNodeId(const SLCBackendC* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->fnNodeNameLen; i++) {
        if (c->fnNodeNames[i].nodeId == nodeId) {
            return c->fnNodeNames[i].cName;
        }
    }
    return NULL;
}

static const SLFnSig* _Nullable FindFnSigByNodeId(const SLCBackendC* c, int32_t nodeId) {
    const char* cName = FindFnCNameByNodeId(c, nodeId);
    uint32_t    i;
    if (cName == NULL) {
        return NULL;
    }
    for (i = 0; i < c->fnSigLen; i++) {
        if (StrEq(c->fnSigs[i].cName, cName)) {
            return &c->fnSigs[i];
        }
    }
    return NULL;
}

static const SLFnGroup* _Nullable FindFnGroupBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t         i;
    const SLNameMap* map = FindNameBySlice(c, start, end);
    if (map != NULL) {
        for (i = 0; i < c->fnGroupLen; i++) {
            if (StrEq(c->fnGroups[i].name, map->cName)) {
                return &c->fnGroups[i];
            }
        }
    }
    for (i = 0; i < c->fnGroupLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->fnGroups[i].name)) {
            return &c->fnGroups[i];
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

static const SLFieldInfo* _Nullable FindEmbeddedFieldInfo(
    const SLCBackendC* c, const char* ownerType) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        if (StrEq(c->fieldInfos[i].ownerType, ownerType) && c->fieldInfos[i].isEmbedded) {
            return &c->fieldInfos[i];
        }
    }
    return NULL;
}

static const SLFieldInfo* _Nullable FindFieldInfoByName(
    const SLCBackendC* c, const char* ownerType, const char* fieldName) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        if (StrEq(c->fieldInfos[i].ownerType, ownerType)
            && StrEq(c->fieldInfos[i].fieldName, fieldName))
        {
            return &c->fieldInfos[i];
        }
    }
    return NULL;
}

static int ResolveFieldPathBySlice(
    const SLCBackendC*  c,
    const char*         ownerType,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const SLFieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const SLFieldInfo** _Nullable outField) {
    const SLFieldInfo* direct;
    const SLFieldInfo* embedded;
    const char*        embeddedBaseName;
    uint32_t           nestedLen = 0;

    if (outLen == NULL || cap == 0u) {
        return -1;
    }

    direct = FindFieldInfo(c, ownerType, fieldStart, fieldEnd);
    if (direct != NULL) {
        outPath[0] = direct;
        *outLen = 1u;
        if (outField != NULL) {
            *outField = direct;
        }
        return 0;
    }

    embedded = FindEmbeddedFieldInfo(c, ownerType);
    if (embedded == NULL || !embedded->type.valid
        || embedded->type.containerKind != SLTypeContainer_SCALAR || embedded->type.ptrDepth != 0
        || embedded->type.containerPtrDepth != 0 || embedded->type.baseName == NULL)
    {
        return -1;
    }
    if (cap < 2u) {
        return -1;
    }

    embeddedBaseName = ResolveScalarAliasBaseName(c, embedded->type.baseName);
    if (embeddedBaseName == NULL) {
        return -1;
    }
    if (ResolveFieldPathBySlice(
            c, embeddedBaseName, fieldStart, fieldEnd, outPath + 1u, cap - 1u, &nestedLen, outField)
        != 0)
    {
        return -1;
    }
    outPath[0] = embedded;
    *outLen = nestedLen + 1u;
    return 0;
}

static int ResolveEmbeddedPathByNames(
    const SLCBackendC*  c,
    const char*         srcTypeName,
    const char*         dstTypeName,
    const SLFieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen) {
    const SLFieldInfo* embedded;
    const char*        curType;
    const char*        dstCanonical;
    uint32_t           n = 0;
    uint32_t           guard = 0;

    if (outLen == NULL || srcTypeName == NULL || dstTypeName == NULL) {
        return -1;
    }
    curType = ResolveScalarAliasBaseName(c, srcTypeName);
    dstCanonical = ResolveScalarAliasBaseName(c, dstTypeName);
    if (curType == NULL || dstCanonical == NULL) {
        return -1;
    }
    if (StrEq(curType, dstCanonical)) {
        *outLen = 0;
        return 0;
    }

    while (guard++ <= c->fieldInfoLen) {
        if (n >= cap) {
            return -1;
        }
        embedded = FindEmbeddedFieldInfo(c, curType);
        if (embedded == NULL || !embedded->type.valid
            || embedded->type.containerKind != SLTypeContainer_SCALAR
            || embedded->type.ptrDepth != 0 || embedded->type.containerPtrDepth != 0
            || embedded->type.baseName == NULL)
        {
            return -1;
        }
        outPath[n++] = embedded;
        curType = ResolveScalarAliasBaseName(c, embedded->type.baseName);
        if (curType == NULL) {
            return -1;
        }
        if (StrEq(curType, dstCanonical)) {
            *outLen = n;
            return 0;
        }
    }

    return -1;
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
        SLTypeRef  contextType;
        int        hasContext = 0;
        TypeRefSetScalar(&returnType, "void");
        TypeRefSetInvalid(&contextType);
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
                    || ch->kind == SLAst_TYPE_OPTIONAL || ch->kind == SLAst_TYPE_FN
                    || ch->kind == SLAst_TYPE_ANON_STRUCT || ch->kind == SLAst_TYPE_ANON_UNION)
                && ch->flags == 1)
            {
                if (ParseTypeRef(c, child, &returnType) != 0) {
                    return -1;
                }
            } else if (ch != NULL && ch->kind == SLAst_CONTEXT_CLAUSE) {
                int32_t typeNode = AstFirstChild(&c->ast, child);
                if (typeNode < 0 || ParseTypeRef(c, typeNode, &contextType) != 0) {
                    return -1;
                }
                hasContext = 1;
            }
            child = AstNextSibling(&c->ast, child);
        }
        return AddFnSig(
            c,
            mapName->name,
            mapName->cName,
            nodeId,
            returnType,
            paramTypes,
            paramLen,
            hasContext,
            contextType);
    }

    if (n->kind == SLAst_FN_GROUP) {
        int32_t  child = AstFirstChild(&c->ast, nodeId);
        char*    members[256];
        uint16_t memberCount = 0;
        while (child >= 0) {
            const SLAstNode* member = NodeAt(c, child);
            const SLNameMap* memberMap;
            if (member == NULL || member->kind != SLAst_IDENT) {
                return -1;
            }
            memberMap = FindNameBySlice(c, member->dataStart, member->dataEnd);
            if (memberMap == NULL) {
                return -1;
            }
            if (memberCount >= (uint16_t)(sizeof(members) / sizeof(members[0]))) {
                return -1;
            }
            members[memberCount++] = memberMap->name;
            child = AstNextSibling(&c->ast, child);
        }
        return AddFnGroup(c, mapName->cName, members, memberCount);
    }

    if (n->kind == SLAst_STRUCT || n->kind == SLAst_UNION) {
        int32_t child = AstFirstChild(&c->ast, nodeId);
        while (child >= 0) {
            const SLAstNode* field = NodeAt(c, child);
            if (field != NULL && field->kind == SLAst_FIELD) {
                int32_t     typeNode = AstFirstChild(&c->ast, child);
                int32_t     defaultExprNode = -1;
                const char* lenFieldName = NULL;
                int         isDependent = 0;
                int         isEmbedded = (field->flags & SLAstFlag_FIELD_EMBEDDED) != 0;
                SLTypeRef   fieldType;
                char*       fieldName;
                if (ParseTypeRef(c, typeNode, &fieldType) != 0) {
                    return -1;
                }
                if (typeNode >= 0) {
                    defaultExprNode = AstNextSibling(&c->ast, typeNode);
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
                if (AddFieldInfo(
                        c,
                        mapName->cName,
                        fieldName,
                        lenFieldName,
                        defaultExprNode,
                        isDependent,
                        isEmbedded,
                        fieldType)
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

static int CollectTypeAliasInfo(SLCBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        const SLNameMap* map;
        int32_t          targetNode;
        SLTypeRef        targetType;
        if (n == NULL || n->kind != SLAst_TYPE_ALIAS) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            return -1;
        }
        targetNode = AstFirstChild(&c->ast, nodeId);
        if (targetNode < 0) {
            return -1;
        }
        if (ParseTypeRef(c, targetNode, &targetType) != 0) {
            return -1;
        }
        if (AddTypeAliasInfo(c, map->cName, targetType) != 0) {
            return -1;
        }
    }
    return 0;
}

static int IsParseTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_ANON_STRUCT || kind == SLAst_TYPE_ANON_UNION;
}

static int CollectFnTypeAliasesFromNode(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    int32_t          child;
    if (n == NULL) {
        return 0;
    }
    if (IsParseTypeNodeKind(n->kind)) {
        SLTypeRef ignoredType;
        if (ParseTypeRef(c, nodeId, &ignoredType) != 0) {
            return -1;
        }
    }
    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        if (CollectFnTypeAliasesFromNode(c, child) != 0) {
            return -1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

static int CollectFnTypeAliases(SLCBackendC* c) {
    return CollectFnTypeAliasesFromNode(c, c->ast.root);
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
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->localAnonTypedefScopeMarks,
            &c->localAnonTypedefScopeCap,
            c->localAnonTypedefScopeLen + 1u,
            sizeof(uint32_t),
            (uint32_t)_Alignof(uint32_t))
        != 0)
    {
        return -1;
    }
    c->localScopeMarks[c->localScopeLen++] = c->localLen;
    c->localAnonTypedefScopeMarks[c->localAnonTypedefScopeLen++] = c->localAnonTypedefLen;
    return 0;
}

static void PopScope(SLCBackendC* c) {
    if (c->localScopeLen == 0) {
        c->localLen = 0;
        c->localAnonTypedefLen = 0;
        return;
    }
    c->localScopeLen--;
    c->localLen = c->localScopeMarks[c->localScopeLen];
    if (c->localAnonTypedefScopeLen > 0) {
        c->localAnonTypedefScopeLen--;
        c->localAnonTypedefLen = c->localAnonTypedefScopeMarks[c->localAnonTypedefScopeLen];
    } else {
        c->localAnonTypedefLen = 0;
    }
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

static int FindEnumDeclNodeBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId) {
    uint32_t i;
    if (outNodeId == NULL) {
        return -1;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n != NULL && n->kind == SLAst_ENUM
            && SliceSpanEq(c->unit->source, n->dataStart, n->dataEnd, start, end))
        {
            *outNodeId = nodeId;
            return 0;
        }
    }
    return -1;
}

static int EnumDeclHasMemberBySlice(
    const SLCBackendC* c, int32_t enumNodeId, uint32_t memberStart, uint32_t memberEnd) {
    int32_t child = AstFirstChild(&c->ast, enumNodeId);

    if (child >= 0) {
        const SLAstNode* firstChild = NodeAt(c, child);
        if (firstChild != NULL
            && (firstChild->kind == SLAst_TYPE_NAME || firstChild->kind == SLAst_TYPE_PTR
                || firstChild->kind == SLAst_TYPE_REF || firstChild->kind == SLAst_TYPE_MUTREF
                || firstChild->kind == SLAst_TYPE_ARRAY || firstChild->kind == SLAst_TYPE_VARRAY
                || firstChild->kind == SLAst_TYPE_SLICE || firstChild->kind == SLAst_TYPE_MUTSLICE
                || firstChild->kind == SLAst_TYPE_OPTIONAL || firstChild->kind == SLAst_TYPE_FN
                || firstChild->kind == SLAst_TYPE_ANON_STRUCT
                || firstChild->kind == SLAst_TYPE_ANON_UNION))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }

    while (child >= 0) {
        const SLAstNode* item = NodeAt(c, child);
        if (item != NULL && item->kind == SLAst_FIELD
            && SliceSpanEq(c->unit->source, item->dataStart, item->dataEnd, memberStart, memberEnd))
        {
            return 1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

static int ResolveEnumSelectorByFieldExpr(
    const SLCBackendC* c, int32_t fieldExprNode, const SLNameMap** outEnumMap) {
    const SLAstNode* n = NodeAt(c, fieldExprNode);
    int32_t          recvNode;
    const SLAstNode* recv;
    const SLLocal*   local;
    const SLNameMap* map;
    int32_t          enumDeclNode;

    if (outEnumMap != NULL) {
        *outEnumMap = NULL;
    }
    if (n == NULL || n->kind != SLAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = AstFirstChild(&c->ast, fieldExprNode);
    recv = NodeAt(c, recvNode);
    if (recv == NULL || recv->kind != SLAst_IDENT) {
        return 0;
    }
    local = FindLocalBySlice(c, recv->dataStart, recv->dataEnd);
    if (local != NULL) {
        return 0;
    }
    map = FindNameBySlice(c, recv->dataStart, recv->dataEnd);
    if (map == NULL || map->kind != SLAst_ENUM) {
        return 0;
    }
    if (FindEnumDeclNodeBySlice(c, recv->dataStart, recv->dataEnd, &enumDeclNode) != 0) {
        return 0;
    }
    if (!EnumDeclHasMemberBySlice(c, enumDeclNode, n->dataStart, n->dataEnd)) {
        return 0;
    }
    if (outEnumMap != NULL) {
        *outEnumMap = map;
    }
    return 1;
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
    int         inlineAnon = 0;
    int         inlineAnonIsUnion = 0;
    if (!type->valid) {
        return BufAppendCStr(&c->out, "void");
    }
    if (type->containerKind == SLTypeContainer_SLICE_RO
        || type->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (type->containerPtrDepth > 0) {
            /* *[T] / &[T] lower to slice structs (ptr+len) */
            base = type->containerKind == SLTypeContainer_SLICE_MUT
                     ? "__sl_slice_mut"
                     : "__sl_slice_ro";
            stars = SliceStructPtrDepth(type);
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
        if (StrHasPrefix(base, "__sl_anon_") && !IsAnonTypeNameVisible(c, base)) {
            const SLAnonTypeInfo* info = FindAnonTypeByCName(c, base);
            inlineAnon = 1;
            inlineAnonIsUnion = info != NULL ? info->isUnion : (base[10] == 'u');
        }
        stars = type->ptrDepth;
        if (type->containerKind == SLTypeContainer_ARRAY && type->containerPtrDepth > 0) {
            stars += type->containerPtrDepth;
        }
    }
    if (inlineAnon) {
        uint32_t i;
        if (BufAppendCStr(&c->out, inlineAnonIsUnion ? "union {\n" : "struct {\n") != 0) {
            return -1;
        }
        for (i = 0; i < c->fieldInfoLen; i++) {
            const SLFieldInfo* f = &c->fieldInfos[i];
            if (!StrEq(f->ownerType, base)) {
                continue;
            }
            EmitIndent(c, 1);
            if (EmitTypeRefWithName(c, &f->type, f->fieldName) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
            }
        }
        if (BufAppendChar(&c->out, '}') != 0) {
            return -1;
        }
    } else if (BufAppendCStr(&c->out, base) != 0) {
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
            int stars = SliceStructPtrDepth(&t);
            if (BufAppendCStr(
                    &c->out,
                    t.containerKind == SLTypeContainer_SLICE_MUT
                        ? "__sl_slice_mut"
                        : "__sl_slice_ro")
                    != 0
                || BufAppendChar(&c->out, ' ') != 0)
            {
                return -1;
            }
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

static int EmitTypeRefWithName(SLCBackendC* c, const SLTypeRef* t, const char* name);

static int EmitAnonInlineTypeWithName(
    SLCBackendC* c, const char* ownerType, int isUnion, const char* name) {
    uint32_t i;
    int      sawField = 0;
    if (BufAppendCStr(&c->out, isUnion ? "union {\n" : "struct {\n") != 0) {
        return -1;
    }
    for (i = 0; i < c->fieldInfoLen; i++) {
        const SLFieldInfo* f = &c->fieldInfos[i];
        if (!StrEq(f->ownerType, ownerType)) {
            continue;
        }
        sawField = 1;
        EmitIndent(c, 1);
        if (EmitTypeRefWithName(c, &f->type, f->fieldName) != 0
            || BufAppendCStr(&c->out, ";\n") != 0)
        {
            return -1;
        }
    }
    if (!sawField) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "} ") != 0 || BufAppendCStr(&c->out, name) != 0) {
        return -1;
    }
    return 0;
}

static int EmitTypeRefWithName(SLCBackendC* c, const SLTypeRef* t, const char* name) {
    int stars;
    int i;
    if (!t->valid) {
        return -1;
    }
    if (t->containerKind == SLTypeContainer_SLICE_RO
        || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (t->containerPtrDepth > 0) {
            int stars = SliceStructPtrDepth(t);
            if (BufAppendCStr(
                    &c->out,
                    t->containerKind == SLTypeContainer_SLICE_MUT
                        ? "__sl_slice_mut"
                        : "__sl_slice_ro")
                    != 0
                || BufAppendChar(&c->out, ' ') != 0)
            {
                return -1;
            }
            for (i = 0; i < stars; i++) {
                if (BufAppendChar(&c->out, '*') != 0) {
                    return -1;
                }
            }
            return BufAppendCStr(&c->out, name);
        }
        if (BufAppendCStr(
                &c->out,
                t->containerKind == SLTypeContainer_SLICE_MUT ? "__sl_slice_mut" : "__sl_slice_ro")
                != 0
            || BufAppendChar(&c->out, ' ') != 0)
        {
            return -1;
        }
        return BufAppendCStr(&c->out, name);
    }
    if (t->baseName == NULL) {
        return -1;
    }
    if (t->containerKind == SLTypeContainer_SCALAR && t->ptrDepth == 0 && t->containerPtrDepth == 0
        && !t->isOptional)
    {
        const SLAnonTypeInfo* info = FindAnonTypeByCName(c, t->baseName);
        if (info != NULL && (info->flags & SLAnonTypeFlag_EMITTED_GLOBAL) == 0
            && !IsLocalAnonTypedefVisible(c, info->cName))
        {
            return EmitAnonInlineTypeWithName(c, info->cName, info->isUnion, name);
        }
        if (info == NULL && StrHasPrefix(t->baseName, "__sl_anon_")
            && !IsAnonTypeNameVisible(c, t->baseName))
        {
            int isUnion = t->baseName[10] == 'u';
            return EmitAnonInlineTypeWithName(c, t->baseName, isUnion, name);
        }
    }
    if (BufAppendCStr(&c->out, t->baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
        return -1;
    }
    stars = t->ptrDepth;
    if (t->containerKind == SLTypeContainer_ARRAY && t->containerPtrDepth > 0) {
        stars += t->containerPtrDepth;
    }
    for (i = 0; i < stars; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    if (t->isOptional && BufAppendCStr(&c->out, "/* optional */ ") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, name) != 0) {
        return -1;
    }
    if (t->containerKind == SLTypeContainer_ARRAY && t->containerPtrDepth == 0 && t->hasArrayLen) {
        if (BufAppendChar(&c->out, '[') != 0 || BufAppendU32(&c->out, t->arrayLen) != 0
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
static int InferExprTypeExpected(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType);
static int EmitExpr(SLCBackendC* c, int32_t nodeId);
static int EmitCompoundLiteral(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType);
static int EmitEffectiveContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType);
static int InferBuiltinNewCallType(SLCBackendC* c, int32_t callNode, SLTypeRef* outType);
static int TypeRefAssignableCost(
    SLCBackendC* c, const SLTypeRef* dst, const SLTypeRef* src, uint8_t* outCost);

static void SetPreferredAllocatorPtrType(SLTypeRef* outType) {
    TypeRefSetScalar(outType, "core__Allocator");
    outType->ptrDepth = 1;
}

static int IsAllocatorPtrType(SLCBackendC* c, const SLTypeRef* gotType) {
    SLTypeRef want;
    uint8_t   cost = 0;
    SetPreferredAllocatorPtrType(&want);
    if (TypeRefAssignableCost(c, &want, gotType, &cost) == 0) {
        return 1;
    }
    TypeRefSetScalar(&want, "Allocator");
    want.ptrDepth = 1;
    if (TypeRefAssignableCost(c, &want, gotType, &cost) == 0) {
        return 1;
    }
    TypeRefSetScalar(&want, "__sl_Allocator");
    want.ptrDepth = 1;
    if (TypeRefAssignableCost(c, &want, gotType, &cost) == 0) {
        return 1;
    }
    TypeRefSetScalar(&want, "__sl_mem_Allocator");
    want.ptrDepth = 1;
    return TypeRefAssignableCost(c, &want, gotType, &cost) == 0;
}

static int IsTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_ANON_STRUCT || kind == SLAst_TYPE_ANON_UNION;
}

static void ResolveVarLikeTypeAndInitNode(
    SLCBackendC* c, int32_t nodeId, int32_t* outTypeNode, int32_t* outInitNode) {
    int32_t          firstChild = AstFirstChild(&c->ast, nodeId);
    const SLAstNode* firstNode;
    *outTypeNode = -1;
    *outInitNode = -1;
    if (firstChild < 0) {
        return;
    }
    firstNode = NodeAt(c, firstChild);
    if (firstNode != NULL && IsTypeNodeKind(firstNode->kind)) {
        *outTypeNode = firstChild;
        *outInitNode = AstNextSibling(&c->ast, firstChild);
    } else {
        *outInitNode = firstChild;
    }
}

static int InferVarLikeDeclType(SLCBackendC* c, int32_t initNode, SLTypeRef* outType) {
    if (initNode < 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (InferExprType(c, initNode, outType) != 0 || !outType->valid) {
        return -1;
    }
    if (outType->containerKind == SLTypeContainer_SCALAR && outType->containerPtrDepth == 0
        && outType->ptrDepth == 0 && outType->baseName != NULL
        && StrEq(outType->baseName, "__sl_i32"))
    {
        outType->baseName = "__sl_int";
    }
    return 0;
}

static int FindTopLevelVarLikeNodeBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId) {
    uint32_t i;
    if (outNodeId == NULL) {
        return -1;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n != NULL && (n->kind == SLAst_VAR || n->kind == SLAst_CONST)
            && SliceSpanEq(c->unit->source, n->dataStart, n->dataEnd, start, end))
        {
            *outNodeId = nodeId;
            return 0;
        }
    }
    return -1;
}

static int InferTopLevelVarLikeType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType) {
    int32_t typeNode;
    int32_t initNode;
    ResolveVarLikeTypeAndInitNode(c, nodeId, &typeNode, &initNode);
    if (typeNode >= 0) {
        return ParseTypeRef(c, typeNode, outType);
    }
    return InferVarLikeDeclType(c, initNode, outType);
}

#define SLCCG_MAX_CALL_ARGS       128u
#define SLCCG_MAX_CALL_CANDIDATES 256u

static int TypeRefEqual(const SLTypeRef* a, const SLTypeRef* b) {
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

static int ExpandAliasSourceType(
    const SLCBackendC* c, const SLTypeRef* src, SLTypeRef* outExpanded) {
    const SLTypeAliasInfo* alias;
    if (!src->valid || src->baseName == NULL) {
        return 0;
    }
    alias = FindTypeAliasInfoByAliasName(c, src->baseName);
    if (alias == NULL) {
        return 0;
    }

    /* Wrapped alias: preserve wrappers only when alias target is scalar. */
    if ((src->ptrDepth > 0 || src->containerPtrDepth > 0 || src->isOptional)
        && alias->targetType.containerKind == SLTypeContainer_SCALAR
        && alias->targetType.ptrDepth == 0 && alias->targetType.containerPtrDepth == 0
        && alias->targetType.baseName != NULL)
    {
        *outExpanded = *src;
        outExpanded->baseName = alias->targetType.baseName;
        return 1;
    }

    /* Unwrapped alias can expand to any target type form. */
    if (src->containerKind == SLTypeContainer_SCALAR && src->ptrDepth == 0
        && src->containerPtrDepth == 0 && !src->isOptional)
    {
        *outExpanded = alias->targetType;
        return 1;
    }
    return 0;
}

static int TypeRefAssignableCost(
    SLCBackendC* c, const SLTypeRef* dst, const SLTypeRef* src, uint8_t* outCost) {
    const SLFieldInfo* path[64];
    uint32_t           pathLen = 0;
    SLTypeRef          expandedSrc;
    uint8_t            expandedCost = 0;
    if (!dst->valid || !src->valid) {
        return -1;
    }
    if (TypeRefEqual(dst, src)) {
        *outCost = 0;
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
        SLTypeRef inner = *dst;
        inner.isOptional = 0;
        if (TypeRefAssignableCost(c, &inner, src, outCost) == 0) {
            *outCost = 4;
            return 0;
        }
        return -1;
    }
    if (dst->isOptional && src->isOptional) {
        SLTypeRef d = *dst;
        SLTypeRef s = *src;
        d.isOptional = 0;
        s.isOptional = 0;
        return TypeRefAssignableCost(c, &d, &s, outCost);
    }
    if (!dst->isOptional && src->isOptional) {
        return -1;
    }

    if (dst->containerKind == SLTypeContainer_SLICE_RO
        || dst->containerKind == SLTypeContainer_SLICE_MUT)
    {
        if (src->containerKind == SLTypeContainer_SCALAR && src->containerPtrDepth == 0
            && src->ptrDepth > 0 && IsStrBaseName(src->baseName) && dst->baseName != NULL
            && StrEq(dst->baseName, "__sl_u8"))
        {
            if (dst->containerKind == SLTypeContainer_SLICE_MUT && src->readOnly) {
                return -1;
            }
            *outCost = 1;
            return 0;
        }
        if ((src->containerKind == SLTypeContainer_SLICE_RO
             || src->containerKind == SLTypeContainer_SLICE_MUT)
            && dst->containerPtrDepth == src->containerPtrDepth && dst->ptrDepth == src->ptrDepth
            && dst->baseName != NULL && src->baseName != NULL
            && StrEq(dst->baseName, src->baseName))
        {
            if (dst->containerKind == SLTypeContainer_SLICE_RO
                && src->containerKind == SLTypeContainer_SLICE_MUT)
            {
                *outCost = 1;
            } else if (dst->containerKind == src->containerKind) {
                *outCost = 0;
            } else {
                return -1;
            }
            return 0;
        }
        if (src->containerKind == SLTypeContainer_ARRAY && dst->ptrDepth == src->ptrDepth
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

static int CostVecCmp(const uint8_t* a, const uint8_t* b, uint32_t len) {
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

static int ExprNeedsExpectedType(const SLCBackendC* c, int32_t exprNode) {
    const SLAstNode* n = NodeAt(c, exprNode);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == SLAst_COMPOUND_LIT) {
        return 1;
    }
    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, exprNode);
        const SLAstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == SLAst_COMPOUND_LIT) {
            int32_t          rhsChild = AstFirstChild(&c->ast, rhsNode);
            const SLAstNode* rhsTypeNode = NodeAt(c, rhsChild);
            return !(rhsTypeNode != NULL && IsTypeNodeKind(rhsTypeNode->kind));
        }
    }
    return 0;
}

static int CollectCallArgInfo(
    SLCBackendC* c,
    int32_t      callNode,
    int32_t      calleeNode,
    int          includeReceiver,
    int32_t      receiverNode,
    int32_t*     outArgNodes,
    SLTypeRef*   outArgTypes,
    uint32_t*    outArgCount) {
    int32_t  argNode = AstNextSibling(&c->ast, calleeNode);
    uint32_t argCount = 0;
    (void)callNode;
    if (includeReceiver) {
        if (argCount >= SLCCG_MAX_CALL_ARGS) {
            return -1;
        }
        outArgNodes[argCount] = receiverNode;
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
        if (argCount >= SLCCG_MAX_CALL_ARGS) {
            return -1;
        }
        outArgNodes[argCount] = argNode;
        if (!ExprNeedsExpectedType(c, argNode)
            && InferExprType(c, argNode, &outArgTypes[argCount]) != 0)
        {
            return -1;
        } else if (ExprNeedsExpectedType(c, argNode)) {
            TypeRefSetInvalid(&outArgTypes[argCount]);
        }
        argCount++;
        argNode = AstNextSibling(&c->ast, argNode);
    }
    *outArgCount = argCount;
    return 0;
}

/* Returns 0 success, 1 no name, 2 no match, 3 ambiguous */
static int ResolveCallTarget(
    SLCBackendC*     c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    const int32_t*   argNodes,
    const SLTypeRef* argTypes,
    uint32_t         argCount,
    const SLFnSig**  outSig,
    const char**     outCalleeName) {
    const SLFnSig*   candidates[SLCCG_MAX_CALL_CANDIDATES];
    const SLFnSig*   byName[SLCCG_MAX_CALL_CANDIDATES];
    uint32_t         candidateLen = 0;
    const SLFnSig*   bestSig = NULL;
    const char*      bestName = NULL;
    uint8_t          bestCosts[SLCCG_MAX_CALL_ARGS];
    uint32_t         bestTotal = 0;
    int              ambiguous = 0;
    int              nameFound = 0;
    uint32_t         i, j;
    const SLFnGroup* group = FindFnGroupBySlice(c, nameStart, nameEnd);

    i = FindFnSigCandidatesBySlice(
        c, nameStart, nameEnd, byName, (uint32_t)(sizeof(byName) / sizeof(byName[0])));
    if (i > 0) {
        nameFound = 1;
        if (i > (uint32_t)(sizeof(byName) / sizeof(byName[0]))) {
            i = (uint32_t)(sizeof(byName) / sizeof(byName[0]));
        }
        for (j = 0; j < i && candidateLen < SLCCG_MAX_CALL_CANDIDATES; j++) {
            candidates[candidateLen++] = byName[j];
        }
    }
    if (group != NULL) {
        nameFound = 1;
        for (i = 0; i < group->memberCount && candidateLen < SLCCG_MAX_CALL_CANDIDATES; i++) {
            uint32_t n = FindFnSigCandidatesByName(
                c,
                c->fnGroupMembers[group->memberStart + i],
                byName,
                (uint32_t)(sizeof(byName) / sizeof(byName[0])));
            if (n > 0) {
                uint32_t k;
                if (n > (uint32_t)(sizeof(byName) / sizeof(byName[0]))) {
                    n = (uint32_t)(sizeof(byName) / sizeof(byName[0]));
                }
                for (j = 0; j < n && candidateLen < SLCCG_MAX_CALL_CANDIDATES; j++) {
                    int dup = 0;
                    for (k = 0; k < candidateLen; k++) {
                        if (candidates[k] == byName[j]) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup) {
                        candidates[candidateLen++] = byName[j];
                    }
                }
            }
        }
    }

    if (!nameFound) {
        return 1;
    }

    for (i = 0; i < candidateLen; i++) {
        const SLFnSig* sig = candidates[i];
        uint8_t        costs[SLCCG_MAX_CALL_ARGS];
        uint32_t       total = 0;
        uint32_t       p;
        int            viable = 1;
        int            cmp;
        if (sig->paramLen != argCount) {
            continue;
        }
        for (p = 0; p < argCount; p++) {
            SLTypeRef argType;
            uint8_t   cost = 0;
            if (argTypes[p].valid) {
                argType = argTypes[p];
            } else {
                if (InferExprTypeExpected(c, argNodes[p], &sig->paramTypes[p], &argType) != 0
                    || !argType.valid)
                {
                    viable = 0;
                    break;
                }
            }
            if (TypeRefAssignableCost(c, &sig->paramTypes[p], &argType, &cost) != 0) {
                viable = 0;
                break;
            }
            costs[p] = cost;
            total += cost;
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
    *outCalleeName = bestName;
    return 0;
}

static int InferCompoundLiteralType(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType) {
    const SLAstNode*      litNode = NodeAt(c, nodeId);
    int32_t               child;
    int32_t               firstField;
    int                   hasExplicitType;
    SLTypeRef             explicitType;
    SLTypeRef             targetValueType;
    SLTypeRef             resultType;
    const char*           ownerType = NULL;
    const SLNameMap*      ownerMap;
    const SLAnonTypeInfo* anonOwner = NULL;
    int                   isUnion = 0;
    uint32_t              explicitFieldCount = 0;
    uint8_t               cost = 0;

    if (litNode == NULL || litNode->kind != SLAst_COMPOUND_LIT) {
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
        if (ParseTypeRef(c, child, &explicitType) != 0 || !explicitType.valid) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        targetValueType = explicitType;
        resultType = explicitType;
    } else {
        if (expectedType == NULL || !expectedType->valid) {
            const char* fieldNames[256];
            SLTypeRef   fieldTypes[256];
            uint32_t    fieldCount = 0;
            int32_t     scan = firstField;
            const char* anonName;
            while (scan >= 0) {
                const SLAstNode* fieldNode = NodeAt(c, scan);
                SLTypeRef        exprType;
                int32_t          exprNode;
                uint32_t         i;
                if (fieldNode == NULL || fieldNode->kind != SLAst_COMPOUND_FIELD) {
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
                if (exprNode < 0 || InferExprType(c, exprNode, &exprType) != 0 || !exprType.valid) {
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
            if (targetValueType.containerKind != SLTypeContainer_SCALAR) {
                if (targetValueType.containerPtrDepth > 0) {
                    targetValueType.containerPtrDepth--;
                }
            } else if (targetValueType.ptrDepth > 0) {
                targetValueType.ptrDepth--;
            }
            resultType = *expectedType;
        }
    }

    if (!targetValueType.valid || targetValueType.containerKind != SLTypeContainer_SCALAR
        || targetValueType.containerPtrDepth != 0 || targetValueType.ptrDepth != 0
        || targetValueType.baseName == NULL || targetValueType.isOptional)
    {
        TypeRefSetInvalid(outType);
        return -1;
    }

    ownerType = ResolveScalarAliasBaseName(c, targetValueType.baseName);
    if (ownerType == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    ownerMap = FindNameByCName(c, ownerType);
    anonOwner = FindAnonTypeByCName(c, ownerType);
    if ((ownerMap == NULL || (ownerMap->kind != SLAst_STRUCT && ownerMap->kind != SLAst_UNION))
        && anonOwner == NULL)
    {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (ownerMap != NULL) {
        isUnion = ownerMap->kind == SLAst_UNION;
    } else {
        isUnion = anonOwner->isUnion;
    }

    while (firstField >= 0) {
        const SLAstNode*   fieldNode = NodeAt(c, firstField);
        const SLFieldInfo* fieldPath[64];
        const SLFieldInfo* field = NULL;
        uint32_t           fieldPathLen = 0;
        int32_t            exprNode;
        int32_t            scan;
        SLTypeRef          exprType;

        if (fieldNode == NULL || fieldNode->kind != SLAst_COMPOUND_FIELD) {
            TypeRefSetInvalid(outType);
            return -1;
        }

        scan = hasExplicitType ? AstNextSibling(&c->ast, child) : child;
        while (scan >= 0 && scan != firstField) {
            const SLAstNode* prevField = NodeAt(c, scan);
            if (prevField != NULL && prevField->kind == SLAst_COMPOUND_FIELD
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
        if (exprNode < 0) {
            TypeRefSetInvalid(outType);
            return -1;
        }

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
        if (InferExprTypeExpected(c, exprNode, &field->type, &exprType) != 0 || !exprType.valid) {
            TypeRefSetInvalid(outType);
            return -1;
        }
        if (TypeRefAssignableCost(c, &field->type, &exprType, &cost) != 0) {
            const SLAstNode* expr = NodeAt(c, exprNode);
            const char*      dstBase =
                field->type.baseName != NULL
                         ? ResolveScalarAliasBaseName(c, field->type.baseName)
                         : NULL;
            if (dstBase == NULL) {
                dstBase = field->type.baseName;
            }
            if (!(expr != NULL && expr->kind == SLAst_INT && dstBase != NULL
                  && (IsIntegerCTypeName(dstBase) || IsFloatCTypeName(dstBase)))
                && !(
                    expr != NULL && expr->kind == SLAst_FLOAT && dstBase != NULL
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
        SLTypeRef expectedValueType = *expectedType;
        if (expectedValueType.containerKind != SLTypeContainer_SCALAR) {
            if (expectedValueType.containerPtrDepth > 0) {
                expectedValueType.containerPtrDepth--;
            }
        } else if (expectedValueType.ptrDepth > 0) {
            expectedValueType.ptrDepth--;
        }
        if (expectedValueType.valid && expectedValueType.containerKind == SLTypeContainer_SCALAR
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

static int InferExprTypeExpected(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }

    if (n->kind == SLAst_COMPOUND_LIT) {
        return InferCompoundLiteralType(c, nodeId, expectedType, outType);
    }

    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, nodeId);
        const SLAstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == SLAst_COMPOUND_LIT) {
            SLTypeRef rhsExpected;
            SLTypeRef rhsType;
            int       haveExpected = 0;
            if (expectedType != NULL && expectedType->valid) {
                rhsExpected = *expectedType;
                if (rhsExpected.containerKind != SLTypeContainer_SCALAR) {
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
            if (rhsType.containerKind == SLTypeContainer_SCALAR) {
                rhsType.ptrDepth++;
            } else {
                rhsType.containerPtrDepth++;
            }
            *outType = rhsType;
            return 0;
        }
    }

    return InferExprType(c, nodeId, outType);
}

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
            {
                const SLFnSig* sig = FindFnSigBySlice(c, n->dataStart, n->dataEnd);
                if (sig != NULL) {
                    const char* aliasName;
                    if (EnsureFnTypeAlias(
                            c, sig->returnType, sig->paramTypes, sig->paramLen, &aliasName)
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
                if (FindTopLevelVarLikeNodeBySlice(c, n->dataStart, n->dataEnd, &topVarLikeNode)
                    == 0)
                {
                    return InferTopLevelVarLikeType(c, topVarLikeNode, outType);
                }
            }
            TypeRefSetInvalid(outType);
            return 0;
        }
        case SLAst_COMPOUND_LIT:      return InferCompoundLiteralType(c, nodeId, NULL, outType);
        case SLAst_CALL_WITH_CONTEXT: {
            int32_t savedActive = c->activeCallWithNode;
            int32_t callNode = AstFirstChild(&c->ast, nodeId);
            int     rc;
            if (callNode < 0) {
                TypeRefSetInvalid(outType);
                return 0;
            }
            c->activeCallWithNode = nodeId;
            rc = InferExprType(c, callNode, outType);
            c->activeCallWithNode = savedActive;
            return rc;
        }
        case SLAst_CALL: {
            int32_t          callee = AstFirstChild(&c->ast, nodeId);
            const SLAstNode* cn = NodeAt(c, callee);
            if (cn != NULL && cn->kind == SLAst_IDENT) {
                if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "new")) {
                    return InferBuiltinNewCallType(c, nodeId, outType);
                }
                int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
                SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
                uint32_t       argCount = 0;
                const SLFnSig* resolved = NULL;
                const char*    resolvedName = NULL;
                if (CollectCallArgInfo(c, nodeId, callee, 0, -1, argNodes, argTypes, &argCount) == 0
                    && ResolveCallTarget(
                           c,
                           cn->dataStart,
                           cn->dataEnd,
                           argNodes,
                           argTypes,
                           argCount,
                           &resolved,
                           &resolvedName)
                           == 0
                    && resolved != NULL)
                {
                    (void)resolvedName;
                    *outType = resolved->returnType;
                    return 0;
                }
            } else if (cn != NULL && cn->kind == SLAst_FIELD_EXPR) {
                int32_t            recvNode = AstFirstChild(&c->ast, callee);
                SLTypeRef          recvType;
                const SLFieldInfo* fieldPath[64];
                uint32_t           fieldPathLen = 0;
                const SLFieldInfo* field = NULL;
                if (recvNode >= 0 && InferExprType(c, recvNode, &recvType) == 0 && recvType.valid) {
                    if (recvType.containerKind != SLTypeContainer_SCALAR
                        && recvType.containerPtrDepth > 0)
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
                    if (SliceEq(c->unit->source, cn->dataStart, cn->dataEnd, "new")) {
                        return InferBuiltinNewCallType(c, nodeId, outType);
                    }
                    int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
                    SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
                    uint32_t       argCount = 0;
                    const SLFnSig* resolved = NULL;
                    const char*    resolvedName = NULL;
                    if (CollectCallArgInfo(
                            c, nodeId, callee, 1, recvNode, argNodes, argTypes, &argCount)
                            == 0
                        && ResolveCallTarget(
                               c,
                               cn->dataStart,
                               cn->dataEnd,
                               argNodes,
                               argTypes,
                               argCount,
                               &resolved,
                               &resolvedName)
                               == 0
                        && resolved != NULL)
                    {
                        (void)resolvedName;
                        *outType = resolved->returnType;
                        return 0;
                    }
                }
            }
            {
                SLTypeRef            calleeType;
                const SLFnTypeAlias* alias = NULL;
                if (InferExprType(c, callee, &calleeType) == 0 && calleeType.valid
                    && calleeType.containerKind == SLTypeContainer_SCALAR
                    && calleeType.ptrDepth == 0 && calleeType.containerPtrDepth == 0
                    && calleeType.baseName != NULL && !calleeType.isOptional)
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
            const SLNameMap*   enumMap = NULL;
            int32_t            recv = AstFirstChild(&c->ast, nodeId);
            SLTypeRef          recvType;
            const SLFieldInfo* fieldPath[64];
            uint32_t           fieldPathLen = 0;
            const SLFieldInfo* field = NULL;
            if (ResolveEnumSelectorByFieldExpr(c, nodeId, &enumMap) != 0 && enumMap != NULL) {
                TypeRefSetScalar(outType, enumMap->cName);
                return 0;
            }
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
                    outType->containerPtrDepth = 1;
                    outType->readOnly = outType->containerKind == SLTypeContainer_SLICE_RO;
                } else if (
                    outType->containerKind == SLTypeContainer_SLICE_RO
                    || outType->containerKind == SLTypeContainer_SLICE_MUT)
                {
                    outType->containerPtrDepth = 1;
                    outType->readOnly = outType->containerKind == SLTypeContainer_SLICE_RO;
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
        case SLAst_STRING:
            TypeRefSetScalar(outType, "__sl_str");
            outType->ptrDepth = 1;
            outType->readOnly = 1;
            return 0;
        case SLAst_BOOL:   TypeRefSetScalar(outType, "__sl_bool"); return 0;
        case SLAst_INT:    TypeRefSetScalar(outType, "__sl_int"); return 0;
        case SLAst_FLOAT:  TypeRefSetScalar(outType, "__sl_f64"); return 0;
        case SLAst_NULL:   TypeRefSetInvalid(outType); return 0;
        case SLAst_UNWRAP: {
            int32_t inner = AstFirstChild(&c->ast, nodeId);
            if (InferExprType(c, inner, outType) != 0) {
                return -1;
            }
            outType->isOptional = 0;
            return 0;
        }
        default: TypeRefSetInvalid(outType); return 0;
    }
}

static int InferBuiltinNewCallType(SLCBackendC* c, int32_t callNode, SLTypeRef* outType) {
    int32_t          calleeNode = AstFirstChild(&c->ast, callNode);
    const SLAstNode* callee = NodeAt(c, calleeNode);
    int32_t          typeArg = -1;
    int32_t          countArg = -1;
    int32_t          extraArg = -1;
    const char*      elemTypeName = NULL;

    TypeRefSetInvalid(outType);
    if (callee == NULL) {
        return 0;
    }

    if (callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new"))
    {
        int32_t arg1 = AstNextSibling(&c->ast, calleeNode);
        int32_t arg2 = arg1 >= 0 ? AstNextSibling(&c->ast, arg1) : -1;
        int32_t arg3 = arg2 >= 0 ? AstNextSibling(&c->ast, arg2) : -1;
        int32_t arg4 = arg3 >= 0 ? AstNextSibling(&c->ast, arg3) : -1;

        if (arg1 < 0 || arg4 >= 0) {
            return 0;
        }
        if (arg3 >= 0) {
            typeArg = arg2;
            countArg = arg3;
        } else if (arg2 >= 0) {
            SLTypeRef got;
            int       isAlloc = 0;

            if (InferExprType(c, arg1, &got) == 0 && got.valid && IsAllocatorPtrType(c, &got)) {
                isAlloc = 1;
            }
            if (isAlloc) {
                typeArg = arg2;
            } else {
                typeArg = arg1;
                countArg = arg2;
            }
        } else {
            typeArg = arg1;
        }
    } else if (
        callee->kind == SLAst_FIELD_EXPR
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new"))
    {
        typeArg = AstNextSibling(&c->ast, calleeNode);
        countArg = typeArg >= 0 ? AstNextSibling(&c->ast, typeArg) : -1;
        extraArg = countArg >= 0 ? AstNextSibling(&c->ast, countArg) : -1;
    } else {
        return 0;
    }

    if (typeArg < 0 || extraArg >= 0) {
        return 0;
    }

    elemTypeName = ResolveTypeNameFromExprArg(c, typeArg);
    if (elemTypeName == NULL) {
        return 0;
    }

    TypeRefSetScalar(outType, elemTypeName);
    if (countArg >= 0) {
        const SLAstNode* count = NodeAt(c, countArg);
        uint32_t         arrayLen = 0;
        if (count != NULL && count->kind == SLAst_INT
            && ParseArrayLenLiteral(c->unit->source, count->dataStart, count->dataEnd, &arrayLen)
                   == 0
            && arrayLen > 0)
        {
            outType->containerKind = SLTypeContainer_ARRAY;
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

static int EmitStringLiteralRef(SLCBackendC* c, int32_t literalId, int writable) {
    if (BufAppendCStr(&c->out, "((__sl_str*)(void*)&sl_lit_") != 0
        || BufAppendCStr(&c->out, writable ? "rw_" : "ro_") != 0
        || BufAppendU32(&c->out, (uint32_t)literalId) != 0 || BufAppendChar(&c->out, ')') != 0)
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
        if (BufAppendCStr(&c->out, "static const struct { __sl_u32 len; __sl_u8 bytes[") != 0
            || BufAppendU32(&c->out, lit->len + 1u) != 0
            || BufAppendCStr(&c->out, "]; } sl_lit_ro_") != 0 || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(&c->out, " = { ") != 0 || BufAppendU32(&c->out, lit->len) != 0
            || BufAppendCStr(&c->out, "u, { ") != 0)
        {
            return -1;
        }
        for (j = 0; j < lit->len; j++) {
            if (EmitHexByte(&c->out, lit->bytes[j]) != 0 || BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
        }
        if (EmitHexByte(&c->out, 0u) != 0 || BufAppendCStr(&c->out, " } };\n") != 0) {
            return -1;
        }

        if (BufAppendCStr(&c->out, "static struct { __sl_u32 len; __sl_u8 bytes[") != 0
            || BufAppendU32(&c->out, lit->len + 1u) != 0
            || BufAppendCStr(&c->out, "]; } sl_lit_rw_") != 0 || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(&c->out, " = { ") != 0 || BufAppendU32(&c->out, lit->len) != 0
            || BufAppendCStr(&c->out, "u, { ") != 0)
        {
            return -1;
        }
        for (j = 0; j < lit->len; j++) {
            if (EmitHexByte(&c->out, lit->bytes[j]) != 0 || BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
        }
        if (EmitHexByte(&c->out, 0u) != 0 || BufAppendCStr(&c->out, " } };\n") != 0) {
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

static int IsStrBaseName(const char* _Nullable s) {
    return s != NULL && (StrEq(s, "__sl_str") || StrEq(s, "core__str"));
}

static int TypeRefIsStr(const SLTypeRef* t) {
    return t->valid && t->containerKind == SLTypeContainer_SCALAR && IsStrBaseName(t->baseName);
}

static int TypeRefContainerWritable(const SLTypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if (t->containerKind == SLTypeContainer_ARRAY || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        return t->readOnly == 0;
    }
    /* For slice-pointer forms, readOnly distinguishes &[...] from *[...]. */
    if (t->containerKind == SLTypeContainer_SLICE_RO && t->containerPtrDepth > 0) {
        return t->readOnly == 0;
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
        if (t->ptrDepth > 0) {
            if (BufAppendCStr(&c->out, "(__sl_u32)(((__sl_str*)(") != 0
                || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, "))->len)") != 0)
            {
                return -1;
            }
            return 0;
        } else {
            if (BufAppendCStr(&c->out, "(__sl_u32)((") != 0 || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").len)") != 0)
            {
                return -1;
            }
            return 0;
        }
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
            int stars = SliceStructPtrDepth(t);
            if (stars > 0) {
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
    if (BufAppendChar(&c->out, ')') != 0) {
        return -1;
    }
    return BufAppendCStr(&c->out, " })");
}

static int TypeRefIsPointerLike(const SLTypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if (t->containerKind == SLTypeContainer_SCALAR) {
        return t->ptrDepth > 0;
    }
    if (t->containerKind == SLTypeContainer_ARRAY) {
        return t->ptrDepth > 0 || t->containerPtrDepth > 0;
    }
    if (t->containerKind == SLTypeContainer_SLICE_RO
        || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        return t->ptrDepth > 0 || t->containerPtrDepth > 0;
    }
    return 0;
}

static int TypeRefIsOwnedRuntimeArrayStruct(const SLTypeRef* t) {
    if (!t->valid) {
        return 0;
    }
    if ((t->containerKind != SLTypeContainer_SLICE_RO
         && t->containerKind != SLTypeContainer_SLICE_MUT)
        || t->containerPtrDepth == 0)
    {
        return 0;
    }
    return SliceStructPtrDepth(t) == 0;
}

static int IsBuiltinNewCallExpr(SLCBackendC* c, int32_t exprNode) {
    const SLAstNode* n = NodeAt(c, exprNode);
    int32_t          calleeNode;
    const SLAstNode* callee;
    if (n != NULL && n->kind == SLAst_CALL_WITH_CONTEXT) {
        exprNode = AstFirstChild(&c->ast, exprNode);
        n = NodeAt(c, exprNode);
    }
    if (n == NULL || n->kind != SLAst_CALL) {
        return 0;
    }
    calleeNode = AstFirstChild(&c->ast, exprNode);
    callee = NodeAt(c, calleeNode);
    if (callee != NULL && callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new"))
    {
        return 1;
    }
    if (callee != NULL && callee->kind == SLAst_FIELD_EXPR
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new"))
    {
        int32_t            recvNode = AstFirstChild(&c->ast, calleeNode);
        SLTypeRef          recvType;
        const SLFieldInfo* fieldPath[64];
        uint32_t           fieldPathLen = 0;
        const SLFieldInfo* field = NULL;
        if (recvNode < 0 || InferExprType(c, recvNode, &recvType) != 0 || !recvType.valid) {
            return 0;
        }
        if (recvType.containerKind != SLTypeContainer_SCALAR && recvType.containerPtrDepth > 0) {
            recvType.containerPtrDepth--;
        } else if (recvType.ptrDepth > 0) {
            recvType.ptrDepth--;
        }
        if (recvType.baseName != NULL
            && ResolveFieldPathBySlice(
                   c,
                   recvType.baseName,
                   callee->dataStart,
                   callee->dataEnd,
                   fieldPath,
                   (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                   &fieldPathLen,
                   &field)
                   == 0
            && fieldPathLen > 0)
        {
            return 0;
        }
        return 1;
    }
    return 0;
}

static int EmitNewAllocArgExpr(SLCBackendC* c, int32_t allocArg) {
    if (allocArg >= 0) {
        return EmitExpr(c, allocArg);
    }
    {
        SLTypeRef want;
        SetPreferredAllocatorPtrType(&want);
        return EmitEffectiveContextFieldValue(c, "mem", &want);
    }
}

static int EmitConcatCallExpr(SLCBackendC* c, int32_t calleeNode) {
    int32_t aNode = AstNextSibling(&c->ast, calleeNode);
    int32_t bNode = aNode >= 0 ? AstNextSibling(&c->ast, aNode) : -1;
    int32_t extra = bNode >= 0 ? AstNextSibling(&c->ast, bNode) : -1;
    if (aNode < 0 || bNode < 0 || extra >= 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "__sl_concat((__sl_Allocator*)(") != 0
        || EmitNewAllocArgExpr(c, -1) != 0 || BufAppendCStr(&c->out, "), ") != 0
        || EmitExpr(c, aNode) != 0 || BufAppendCStr(&c->out, ", ") != 0 || EmitExpr(c, bNode) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitFreeCallExpr(SLCBackendC* c, int32_t allocArgNode, int32_t valueNode) {
    SLTypeRef valueType;
    if (InferExprType(c, valueNode, &valueType) != 0 || !valueType.valid) {
        return -1;
    }
    if (BufAppendCStr(&c->out, "__sl_free((__sl_Allocator*)(") != 0
        || EmitNewAllocArgExpr(c, allocArgNode) != 0 || BufAppendCStr(&c->out, "), ") != 0)
    {
        return -1;
    }

    if (valueType.containerKind == SLTypeContainer_SCALAR && valueType.containerPtrDepth == 0
        && valueType.ptrDepth > 0)
    {
        SLTypeRef pointeeType = valueType;
        pointeeType.ptrDepth--;
        if (BufAppendCStr(&c->out, "(void*)(") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, "), ") != 0)
        {
            return -1;
        }
        if (pointeeType.ptrDepth == 0 && pointeeType.containerKind == SLTypeContainer_SCALAR
            && IsStrBaseName(pointeeType.baseName))
        {
            if (BufAppendCStr(&c->out, "__sl_str_sizeof((__sl_str*)(") != 0
                || EmitExpr(c, valueNode) != 0
                || BufAppendCStr(&c->out, ")), _Alignof(__sl_u32))") != 0)
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

    if ((valueType.containerKind == SLTypeContainer_SLICE_RO
         || valueType.containerKind == SLTypeContainer_SLICE_MUT)
        && valueType.containerPtrDepth > 0 && SliceStructPtrDepth(&valueType) == 0)
    {
        if (BufAppendCStr(&c->out, "(void*)((") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, ").ptr), ((__sl_uint)((") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, ").len) * sizeof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0
            || BufAppendCStr(&c->out, ")), _Alignof(") != 0
            || EmitElementTypeName(c, &valueType, 0) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }

    if (valueType.containerKind == SLTypeContainer_ARRAY && valueType.containerPtrDepth > 0
        && valueType.hasArrayLen)
    {
        if (BufAppendCStr(&c->out, "(void*)(") != 0 || EmitExpr(c, valueNode) != 0
            || BufAppendCStr(&c->out, "), ((__sl_uint)") != 0
            || BufAppendU32(&c->out, valueType.arrayLen) != 0
            || BufAppendCStr(&c->out, " * sizeof(") != 0
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

static int EmitNewCallExpr(
    SLCBackendC* c, int32_t callNode, const SLTypeRef* _Nullable dstType, int requireNonNull) {
    int32_t          calleeNode = AstFirstChild(&c->ast, callNode);
    const SLAstNode* callee = NodeAt(c, calleeNode);
    int32_t          allocArg = -1;
    int32_t          typeArg = -1;
    int32_t          countArg = -1;
    int32_t          extraArg = -1;
    const char*      typeName;
    int              dstIsRuntimeArray = 0;
    int              dstIsRuntimeArrayMut = 0;
    if (callee == NULL) {
        return -1;
    }
    if (dstType != NULL
        && (dstType->containerKind == SLTypeContainer_SLICE_RO
            || dstType->containerKind == SLTypeContainer_SLICE_MUT)
        && dstType->containerPtrDepth > 0)
    {
        dstIsRuntimeArray = 1;
        dstIsRuntimeArrayMut = dstType->containerKind == SLTypeContainer_SLICE_MUT;
    }
    if (callee->kind == SLAst_IDENT
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new"))
    {
        int32_t arg1 = AstNextSibling(&c->ast, calleeNode);
        int32_t arg2 = arg1 >= 0 ? AstNextSibling(&c->ast, arg1) : -1;
        int32_t arg3 = arg2 >= 0 ? AstNextSibling(&c->ast, arg2) : -1;
        int32_t arg4 = arg3 >= 0 ? AstNextSibling(&c->ast, arg3) : -1;
        if (arg1 < 0 || arg4 >= 0) {
            return -1;
        }
        if (arg3 >= 0) {
            allocArg = arg1;
            typeArg = arg2;
            countArg = arg3;
        } else if (arg2 >= 0) {
            SLTypeRef got;
            int       isAlloc = 0;
            if (InferExprType(c, arg1, &got) == 0 && IsAllocatorPtrType(c, &got)) {
                isAlloc = 1;
            }
            if (isAlloc) {
                allocArg = arg1;
                typeArg = arg2;
            } else {
                typeArg = arg1;
                countArg = arg2;
            }
        } else {
            typeArg = arg1;
        }
    } else if (
        callee->kind == SLAst_FIELD_EXPR
        && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new"))
    {
        int32_t            recvNode = AstFirstChild(&c->ast, calleeNode);
        SLTypeRef          recvType;
        const SLFieldInfo* fieldPath[64];
        uint32_t           fieldPathLen = 0;
        const SLFieldInfo* field = NULL;
        if (recvNode < 0 || InferExprType(c, recvNode, &recvType) != 0 || !recvType.valid) {
            return -1;
        }
        if (recvType.containerKind != SLTypeContainer_SCALAR && recvType.containerPtrDepth > 0) {
            recvType.containerPtrDepth--;
        } else if (recvType.ptrDepth > 0) {
            recvType.ptrDepth--;
        }
        if (recvType.baseName != NULL
            && ResolveFieldPathBySlice(
                   c,
                   recvType.baseName,
                   callee->dataStart,
                   callee->dataEnd,
                   fieldPath,
                   (uint32_t)(sizeof(fieldPath) / sizeof(fieldPath[0])),
                   &fieldPathLen,
                   &field)
                   == 0
            && fieldPathLen > 0)
        {
            return -1;
        }
        allocArg = recvNode;
        typeArg = AstNextSibling(&c->ast, calleeNode);
        countArg = typeArg >= 0 ? AstNextSibling(&c->ast, typeArg) : -1;
        extraArg = countArg >= 0 ? AstNextSibling(&c->ast, countArg) : -1;
    } else {
        return -1;
    }
    if (typeArg < 0 || extraArg >= 0) {
        return -1;
    }
    typeName = ResolveTypeNameFromExprArg(c, typeArg);
    if (typeName == NULL) {
        return -1;
    }

    if (countArg >= 0 && dstIsRuntimeArray) {
        if (requireNonNull) {
            if (BufAppendCStr(
                    &c->out,
                    dstIsRuntimeArrayMut
                        ? "((__sl_slice_mut){ (void*)__sl_unwrap((const void*)("
                        : "((__sl_slice_ro){ (const void*)__sl_unwrap((const "
                          "void*)(")
                != 0)
            {
                return -1;
            }
            if (BufAppendCStr(&c->out, "__sl_new_array((__sl_Allocator*)(") != 0
                || EmitNewAllocArgExpr(c, allocArg) != 0
                || BufAppendCStr(&c->out, "), sizeof(") != 0
                || BufAppendCStr(&c->out, typeName) != 0
                || BufAppendCStr(&c->out, "), _Alignof(") != 0
                || BufAppendCStr(&c->out, typeName) != 0
                || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
                || BufAppendCStr(&c->out, ")))), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
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
                dstIsRuntimeArrayMut ? "__sl_new_array_slice_mut((__sl_Allocator*)("
                                     : "__sl_new_array_slice_ro((__sl_Allocator*)(")
                != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || BufAppendCStr(&c->out, typeName) != 0 || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || BufAppendCStr(&c->out, typeName) != 0
            || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
        return 0;
    }

    if (BufAppendCStr(&c->out, "((") != 0 || BufAppendCStr(&c->out, typeName) != 0
        || BufAppendCStr(&c->out, "*)") != 0)
    {
        return -1;
    }
    if (requireNonNull) {
        if (BufAppendCStr(&c->out, "__sl_unwrap((const void*)(") != 0) {
            return -1;
        }
    }
    if (countArg >= 0) {
        if (BufAppendCStr(&c->out, "__sl_new_array((__sl_Allocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || BufAppendCStr(&c->out, typeName) != 0 || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || BufAppendCStr(&c->out, typeName) != 0
            || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0 || EmitExpr(c, countArg) != 0
            || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
    } else {
        if (BufAppendCStr(&c->out, "__sl_new((__sl_Allocator*)(") != 0
            || EmitNewAllocArgExpr(c, allocArg) != 0 || BufAppendCStr(&c->out, "), sizeof(") != 0
            || BufAppendCStr(&c->out, typeName) != 0 || BufAppendCStr(&c->out, "), _Alignof(") != 0
            || BufAppendCStr(&c->out, typeName) != 0 || BufAppendCStr(&c->out, "))") != 0)
        {
            return -1;
        }
    }
    if (requireNonNull && BufAppendCStr(&c->out, "))") != 0) {
        return -1;
    }
    return BufAppendChar(&c->out, ')');
}

static int EmitExprCoerced(SLCBackendC* c, int32_t exprNode, const SLTypeRef* dstType) {
    const SLAstNode*   expr = NodeAt(c, exprNode);
    SLTypeRef          srcType;
    const SLFieldInfo* embedPath[64];
    uint32_t           embedPathLen = 0;
    if (dstType == NULL || !dstType->valid) {
        return EmitExpr(c, exprNode);
    }
    if (IsBuiltinNewCallExpr(c, exprNode)) {
        const SLAstNode* n = NodeAt(c, exprNode);
        int32_t          callNode = exprNode;
        int32_t          savedActive = c->activeCallWithNode;
        int              requireNonNull = TypeRefIsPointerLike(dstType) && !dstType->isOptional;
        int              rc;
        if (n != NULL && n->kind == SLAst_CALL_WITH_CONTEXT) {
            callNode = AstFirstChild(&c->ast, exprNode);
            c->activeCallWithNode = exprNode;
        }
        rc = EmitNewCallExpr(c, callNode, dstType, requireNonNull);
        c->activeCallWithNode = savedActive;
        return rc;
    }
    if (expr != NULL && expr->kind == SLAst_STRING
        && dstType->containerKind == SLTypeContainer_SCALAR && dstType->containerPtrDepth == 0
        && dstType->ptrDepth > 0 && IsStrBaseName(dstType->baseName))
    {
        int32_t literalId = -1;
        if ((uint32_t)exprNode < c->stringLitByNodeLen) {
            literalId = c->stringLitByNode[exprNode];
        }
        if (literalId < 0) {
            return -1;
        }
        return EmitStringLiteralRef(c, literalId, dstType->readOnly ? 0 : 1);
    }
    if (expr != NULL && expr->kind == SLAst_COMPOUND_LIT) {
        if (TypeRefIsPointerLike(dstType)) {
            SLTypeRef        targetType = *dstType;
            const SLTypeRef* literalExpected = NULL;
            if (targetType.containerKind != SLTypeContainer_SCALAR) {
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
    if (expr != NULL && expr->kind == SLAst_UNARY && (SLTokenKind)expr->op == SLTok_AND) {
        int32_t          rhsNode = AstFirstChild(&c->ast, exprNode);
        const SLAstNode* rhs = NodeAt(c, rhsNode);
        if (rhs != NULL && rhs->kind == SLAst_COMPOUND_LIT) {
            SLTypeRef        targetType = *dstType;
            const SLTypeRef* literalExpected = NULL;
            if (targetType.containerKind != SLTypeContainer_SCALAR) {
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
    if (InferExprType(c, exprNode, &srcType) != 0 || !srcType.valid) {
        return EmitExpr(c, exprNode);
    }
    if (srcType.containerKind == SLTypeContainer_SCALAR
        && dstType->containerKind == SLTypeContainer_SCALAR && srcType.baseName != NULL
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
    if ((dstType->containerKind == SLTypeContainer_SLICE_RO
         || dstType->containerKind == SLTypeContainer_SLICE_MUT)
        && dstType->containerPtrDepth > 0 && SliceStructPtrDepth(dstType) == 0)
    {
        if (srcType.containerKind == SLTypeContainer_SCALAR && srcType.containerPtrDepth == 0
            && srcType.ptrDepth > 0 && IsStrBaseName(srcType.baseName) && dstType->baseName != NULL
            && StrEq(dstType->baseName, "__sl_u8"))
        {
            if (dstType->containerKind == SLTypeContainer_SLICE_MUT && srcType.readOnly) {
                return EmitExpr(c, exprNode);
            }
            if (dstType->containerKind == SLTypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__sl_slice_mut){ (void*)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, "))->bytes), (__sl_uint)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, "))->len) })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0
                    || BufAppendCStr(&c->out, "))->bytes), (__sl_uint)(((__sl_str*)(") != 0
                    || EmitExpr(c, exprNode) != 0 || BufAppendCStr(&c->out, "))->len) })") != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
        if (dstType->containerKind == SLTypeContainer_SLICE_RO
            && srcType.containerKind == SLTypeContainer_SLICE_MUT && srcType.containerPtrDepth > 0
            && SliceStructPtrDepth(&srcType) == 0 && srcType.ptrDepth == dstType->ptrDepth
            && srcType.containerPtrDepth == dstType->containerPtrDepth && srcType.baseName != NULL
            && dstType->baseName != NULL && StrEq(srcType.baseName, dstType->baseName))
        {
            if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)((") != 0
                || EmitExpr(c, exprNode) != 0
                || BufAppendCStr(&c->out, ").ptr), (__sl_uint)((") != 0
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
        if (srcType.containerKind == SLTypeContainer_ARRAY && srcType.ptrDepth == dstType->ptrDepth
            && (srcType.containerPtrDepth == dstType->containerPtrDepth
                || srcType.containerPtrDepth + 1 == dstType->containerPtrDepth)
            && srcType.baseName != NULL && dstType->baseName != NULL
            && StrEq(srcType.baseName, dstType->baseName))
        {
            if (dstType->containerKind == SLTypeContainer_SLICE_MUT) {
                if (BufAppendCStr(&c->out, "((__sl_slice_mut){ (void*)(") != 0
                    || EmitElemPtrExpr(c, exprNode, &srcType, 1) != 0
                    || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                    || EmitLenExprFromType(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, "((__sl_slice_ro){ (const void*)(") != 0
                    || EmitElemPtrExpr(c, exprNode, &srcType, 0) != 0
                    || BufAppendCStr(&c->out, "), (__sl_uint)(") != 0
                    || EmitLenExprFromType(c, exprNode, &srcType) != 0
                    || BufAppendCStr(&c->out, ") })") != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
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
                || BufAppendCStr(&c->out, ") })") != 0)
            {
                return -1;
            }
            return 0;
        }
    }
    return EmitExpr(c, exprNode);
}

static int32_t ActiveCallOverlayNode(const SLCBackendC* c) {
    if (c->activeCallWithNode < 0 || (uint32_t)c->activeCallWithNode >= c->ast.len) {
        return -1;
    }
    {
        int32_t callNode = AstFirstChild(&c->ast, c->activeCallWithNode);
        int32_t child = callNode >= 0 ? AstNextSibling(&c->ast, callNode) : -1;
        if (child >= 0) {
            const SLAstNode* n = NodeAt(c, child);
            if (n != NULL && n->kind == SLAst_CONTEXT_OVERLAY) {
                return child;
            }
        }
    }
    return -1;
}

static int32_t FindActiveOverlayBindByName(const SLCBackendC* c, const char* fieldName) {
    int32_t overlayNode = ActiveCallOverlayNode(c);
    int32_t child = overlayNode >= 0 ? AstFirstChild(&c->ast, overlayNode) : -1;
    while (child >= 0) {
        const SLAstNode* b = NodeAt(c, child);
        if (b != NULL && b->kind == SLAst_CONTEXT_BIND
            && SliceEqName(c->unit->source, b->dataStart, b->dataEnd, fieldName))
        {
            return child;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return -1;
}

static int EmitCurrentContextFieldRaw(SLCBackendC* c, const char* fieldName) {
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

static int EmitCurrentContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType) {
    if (c->hasCurrentContext) {
        const SLFieldInfo* srcField = NULL;
        if (c->currentContextType.valid && c->currentContextType.baseName != NULL) {
            srcField = FindFieldInfoByName(c, c->currentContextType.baseName, fieldName);
        }
        if (requiredType != NULL && requiredType->valid && srcField != NULL && srcField->type.valid
            && !TypeRefEqual(&srcField->type, requiredType))
        {
            const SLFieldInfo* embedPath[64];
            uint32_t           embedPathLen = 0;
            uint8_t            cost = 0;
            if (srcField->type.containerKind == SLTypeContainer_SCALAR
                && requiredType->containerKind == SLTypeContainer_SCALAR
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
        }
    }
    return EmitCurrentContextFieldRaw(c, fieldName);
}

static int EmitEffectiveContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType) {
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

static int EmitContextArgForSig(SLCBackendC* c, const SLFnSig* sig) {
    uint32_t i;
    uint32_t fieldCount = 0;
    if (sig == NULL || !sig->hasContext) {
        return 0;
    }
    if (!sig->contextType.valid || sig->contextType.baseName == NULL) {
        return -1;
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
            const SLFieldInfo* f = &c->fieldInfos[i];
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

static int EmitResolvedCall(
    SLCBackendC*   c,
    int32_t        callNode,
    const char*    calleeName,
    const SLFnSig* sig,
    const int32_t* argNodes,
    uint32_t       argCount) {
    uint32_t i;
    (void)callNode;
    if (BufAppendCStr(&c->out, calleeName) != 0 || BufAppendChar(&c->out, '(') != 0) {
        return -1;
    }
    if (sig != NULL && sig->hasContext) {
        if (EmitContextArgForSig(c, sig) != 0) {
            return -1;
        }
        if (argCount > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
    }
    for (i = 0; i < argCount; i++) {
        if (i != 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (sig != NULL && i < sig->paramLen) {
            if (EmitExprCoerced(c, argNodes[i], &sig->paramTypes[i]) != 0) {
                return -1;
            }
        } else if (EmitExpr(c, argNodes[i]) != 0) {
            return -1;
        }
    }
    return BufAppendChar(&c->out, ')');
}

static int EmitFieldPathLValue(
    SLCBackendC* c, const char* base, const SLFieldInfo* const* path, uint32_t pathLen) {
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

static int EmitCompoundLiteralDesignated(
    SLCBackendC* c, int32_t firstField, const char* ownerType, const SLTypeRef* valueType) {
    int32_t fieldNode = firstField;
    int     first = 1;

    if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, valueType) != 0
        || BufAppendCStr(&c->out, "){") != 0)
    {
        return -1;
    }

    while (fieldNode >= 0) {
        const SLAstNode*   field = NodeAt(c, fieldNode);
        const SLFieldInfo* fieldPath[64];
        const SLFieldInfo* resolvedField = NULL;
        uint32_t           fieldPathLen = 0;
        int32_t            exprNode;
        if (field == NULL || field->kind != SLAst_COMPOUND_FIELD) {
            return -1;
        }
        exprNode = AstFirstChild(&c->ast, fieldNode);
        if (exprNode < 0) {
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
            || EmitExprCoerced(c, exprNode, &resolvedField->type) != 0)
        {
            return -1;
        }
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }

    return BufAppendCStr(&c->out, "})");
}

static int EmitCompoundLiteralOrderedStruct(
    SLCBackendC* c, int32_t firstField, const char* ownerType, const SLTypeRef* valueType) {
    const SLFieldInfo* directFields[256];
    uint8_t            directExplicit[256];
    uint32_t           directCount = 0;
    uint32_t           i;
    uint32_t           tempIndex = 0;
    int32_t            fieldNode = firstField;

    for (i = 0; i < c->fieldInfoLen; i++) {
        const SLFieldInfo* f = &c->fieldInfos[i];
        if (!StrEq(f->ownerType, ownerType) || f->isEmbedded) {
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
        || BufAppendCStr(&c->out, " __sl_tmp = {0};\n") != 0)
    {
        PopScope(c);
        return -1;
    }

    while (fieldNode >= 0) {
        const SLAstNode*   field = NodeAt(c, fieldNode);
        const SLFieldInfo* fieldPath[64];
        const SLFieldInfo* resolvedField = NULL;
        uint32_t           fieldPathLen = 0;
        int32_t            exprNode;

        if (field == NULL || field->kind != SLAst_COMPOUND_FIELD) {
            PopScope(c);
            return -1;
        }
        exprNode = AstFirstChild(&c->ast, fieldNode);
        if (exprNode < 0) {
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

        if (BufAppendCStr(&c->out, "    ") != 0
            || EmitTypeNameWithDepth(c, &resolvedField->type) != 0
            || BufAppendCStr(&c->out, " __sl_exp_") != 0 || BufAppendU32(&c->out, tempIndex) != 0
            || BufAppendCStr(&c->out, " = ") != 0
            || EmitExprCoerced(c, exprNode, &resolvedField->type) != 0
            || BufAppendCStr(&c->out, ";\n    ") != 0
            || EmitFieldPathLValue(c, "__sl_tmp", fieldPath, fieldPathLen) != 0
            || BufAppendCStr(&c->out, " = __sl_exp_") != 0 || BufAppendU32(&c->out, tempIndex) != 0
            || BufAppendCStr(&c->out, ";\n") != 0)
        {
            PopScope(c);
            return -1;
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
        const SLFieldInfo* f = directFields[i];
        if (!directExplicit[i] && f->defaultExprNode >= 0) {
            if (BufAppendCStr(&c->out, "    __sl_tmp.") != 0
                || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, " = ") != 0
                || EmitExprCoerced(c, f->defaultExprNode, &f->type) != 0
                || BufAppendCStr(&c->out, ";\n") != 0)
            {
                PopScope(c);
                return -1;
            }
        }
        if (BufAppendCStr(&c->out, "    ") != 0 || EmitTypeNameWithDepth(c, &f->type) != 0
            || BufAppendChar(&c->out, ' ') != 0 || BufAppendCStr(&c->out, f->fieldName) != 0
            || BufAppendCStr(&c->out, " = __sl_tmp.") != 0
            || BufAppendCStr(&c->out, f->fieldName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
        {
            PopScope(c);
            return -1;
        }
        if (AddLocal(c, f->fieldName, f->type) != 0) {
            PopScope(c);
            return -1;
        }
    }

    if (BufAppendCStr(&c->out, "    __sl_tmp;\n}))") != 0) {
        PopScope(c);
        return -1;
    }

    PopScope(c);
    return 0;
}

static int StructHasFieldDefaults(const SLCBackendC* c, const char* ownerType) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        const SLFieldInfo* f = &c->fieldInfos[i];
        if (StrEq(f->ownerType, ownerType) && !f->isEmbedded && f->defaultExprNode >= 0) {
            return 1;
        }
    }
    return 0;
}

static int EmitCompoundLiteral(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType) {
    const SLAstNode* litNode = NodeAt(c, nodeId);
    SLTypeRef        litType;
    SLTypeRef        valueType;
    const char*      ownerType;
    const SLNameMap* ownerMap;
    int32_t          fieldNode;

    if (litNode == NULL || litNode->kind != SLAst_COMPOUND_LIT) {
        return -1;
    }
    if (InferCompoundLiteralType(c, nodeId, expectedType, &litType) != 0 || !litType.valid) {
        return -1;
    }

    valueType = litType;
    if (valueType.containerKind != SLTypeContainer_SCALAR) {
        if (valueType.containerPtrDepth <= 0) {
            return -1;
        }
        valueType.containerPtrDepth--;
    } else if (valueType.ptrDepth > 0) {
        valueType.ptrDepth--;
    }
    if (!valueType.valid || valueType.containerKind != SLTypeContainer_SCALAR
        || valueType.containerPtrDepth != 0 || valueType.ptrDepth != 0
        || valueType.baseName == NULL)
    {
        return -1;
    }
    ownerType = ResolveScalarAliasBaseName(c, valueType.baseName);
    if (ownerType == NULL) {
        return -1;
    }
    valueType.baseName = ownerType;
    ownerMap = FindNameByCName(c, ownerType);

    fieldNode = AstFirstChild(&c->ast, nodeId);
    if (fieldNode >= 0 && NodeAt(c, fieldNode) != NULL
        && IsTypeNodeKind(NodeAt(c, fieldNode)->kind))
    {
        fieldNode = AstNextSibling(&c->ast, fieldNode);
    }

    if (ownerMap != NULL && ownerMap->kind == SLAst_STRUCT && StructHasFieldDefaults(c, ownerType))
    {
        return EmitCompoundLiteralOrderedStruct(c, fieldNode, ownerType, &valueType);
    }
    return EmitCompoundLiteralDesignated(c, fieldNode, ownerType, &valueType);
}

static int EmitExpr(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAst_IDENT:        return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
        case SLAst_INT:
        case SLAst_FLOAT:
        case SLAst_BOOL:         return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
        case SLAst_COMPOUND_LIT: return EmitCompoundLiteral(c, nodeId, NULL);
        case SLAst_STRING:       {
            int32_t literalId = -1;
            if (nodeId >= 0 && (uint32_t)nodeId < c->stringLitByNodeLen) {
                literalId = c->stringLitByNode[nodeId];
            }
            if (literalId < 0) {
                return -1;
            }
            return EmitStringLiteralRef(c, literalId, 0);
        }
        case SLAst_UNARY: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if ((SLTokenKind)n->op == SLTok_AND && child >= 0) {
                const SLAstNode* cn = NodeAt(c, child);
                if (cn != NULL && cn->kind == SLAst_FIELD_EXPR) {
                    int32_t            recv = AstFirstChild(&c->ast, child);
                    SLTypeRef          recvType;
                    SLTypeRef          ownerType;
                    const SLFieldInfo* fieldPath[64];
                    uint32_t           fieldPathLen = 0;
                    const SLFieldInfo* field = NULL;
                    SLTypeRef          childType;
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
            if ((SLTokenKind)n->op == SLTok_EQ || (SLTokenKind)n->op == SLTok_NEQ) {
                SLTypeRef lhsType;
                SLTypeRef rhsType;
                int       lhsNull =
                    lhs >= 0 && NodeAt(c, lhs) != NULL && NodeAt(c, lhs)->kind == SLAst_NULL;
                int rhsNull =
                    rhs >= 0 && NodeAt(c, rhs) != NULL && NodeAt(c, rhs)->kind == SLAst_NULL;
                if (lhs >= 0 && rhs >= 0 && InferExprType(c, lhs, &lhsType) == 0
                    && InferExprType(c, rhs, &rhsType) == 0)
                {
                    if (TypeRefIsOwnedRuntimeArrayStruct(&lhsType) && rhsNull) {
                        if (BufAppendCStr(&c->out, "(((") != 0 || EmitExpr(c, lhs) != 0
                            || BufAppendCStr(&c->out, ").ptr) ") != 0
                            || BufAppendCStr(&c->out, BinaryOpString((SLTokenKind)n->op)) != 0
                            || BufAppendCStr(&c->out, " NULL)") != 0)
                        {
                            return -1;
                        }
                        return 0;
                    }
                    if (TypeRefIsOwnedRuntimeArrayStruct(&rhsType) && lhsNull) {
                        if (BufAppendCStr(&c->out, "(NULL ") != 0
                            || BufAppendCStr(&c->out, BinaryOpString((SLTokenKind)n->op)) != 0
                            || BufAppendCStr(&c->out, " ((") != 0 || EmitExpr(c, rhs) != 0
                            || BufAppendCStr(&c->out, ").ptr))") != 0)
                        {
                            return -1;
                        }
                        return 0;
                    }
                }
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
        case SLAst_CALL_WITH_CONTEXT: {
            int32_t savedActive = c->activeCallWithNode;
            int32_t callNode = AstFirstChild(&c->ast, nodeId);
            int     rc;
            if (callNode < 0 || NodeAt(c, callNode) == NULL
                || NodeAt(c, callNode)->kind != SLAst_CALL)
            {
                return -1;
            }
            c->activeCallWithNode = nodeId;
            rc = EmitExpr(c, callNode);
            c->activeCallWithNode = savedActive;
            return rc;
        }
        case SLAst_CALL: {
            int32_t          child = AstFirstChild(&c->ast, nodeId);
            const SLAstNode* callee = NodeAt(c, child);
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
                return EmitNewCallExpr(c, nodeId, NULL, 0);
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
                && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "concat"))
            {
                return EmitConcatCallExpr(c, child);
            }
            if (callee != NULL && callee->kind == SLAst_IDENT
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
            if (callee != NULL && callee->kind == SLAst_IDENT
                && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "print"))
            {
                int32_t msgArg = AstNextSibling(&c->ast, child);
                int32_t extra = msgArg >= 0 ? AstNextSibling(&c->ast, msgArg) : -1;
                if (msgArg < 0 || extra >= 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_console_log(") != 0 || EmitExpr(c, msgArg) != 0
                    || BufAppendCStr(&c->out, ", (__sl_u64)(__sl_i64)(") != 0
                    || EmitEffectiveContextFieldValue(
                           c,
                           "console",
                           &(SLTypeRef){
                               .baseName = "__sl_i32",
                               .ptrDepth = 0,
                               .valid = 1,
                               .containerKind = SLTypeContainer_SCALAR,
                               .containerPtrDepth = 0,
                               .arrayLen = 0,
                               .hasArrayLen = 0,
                               .readOnly = 0,
                               .isOptional = 0,
                           })
                           != 0
                    || BufAppendCStr(&c->out, "))") != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (callee != NULL && callee->kind == SLAst_IDENT
                && SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "platform__exit"))
            {
                int32_t statusArg = AstNextSibling(&c->ast, child);
                int32_t extra = statusArg >= 0 ? AstNextSibling(&c->ast, statusArg) : -1;
                if (statusArg < 0 || extra >= 0) {
                    return -1;
                }
                if (BufAppendCStr(
                        &c->out,
                        "((void)__sl_platform_call(__sl_PlatformOp_"
                        "EXIT, (__sl_u64)(__sl_i64)(")
                        != 0
                    || EmitExpr(c, statusArg) != 0
                    || BufAppendCStr(&c->out, "), 0, 0, 0, 0, 0, 0))") != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (callee != NULL && callee->kind == SLAst_IDENT
                && SliceEq(
                    c->unit->source, callee->dataStart, callee->dataEnd, "platform__console_log"))
            {
                int32_t msgArg = AstNextSibling(&c->ast, child);
                int32_t flagsArg = msgArg >= 0 ? AstNextSibling(&c->ast, msgArg) : -1;
                int32_t extra = flagsArg >= 0 ? AstNextSibling(&c->ast, flagsArg) : -1;
                if (msgArg < 0 || flagsArg < 0 || extra >= 0) {
                    return -1;
                }
                if (BufAppendCStr(&c->out, "__sl_console_log(") != 0 || EmitExpr(c, msgArg) != 0
                    || BufAppendCStr(&c->out, ", (__sl_u64)(__sl_i64)(") != 0
                    || EmitExpr(c, flagsArg) != 0 || BufAppendCStr(&c->out, "))") != 0)
                {
                    return -1;
                }
                return 0;
            }
            if (callee != NULL && callee->kind == SLAst_FIELD_EXPR) {
                int32_t            recvNode = AstFirstChild(&c->ast, child);
                SLTypeRef          recvType;
                SLTypeRef          ownerType;
                const SLFieldInfo* fieldPath[64];
                uint32_t           fieldPathLen = 0;
                const SLFieldInfo* field = NULL;
                int                hasField = 0;
                if (recvNode >= 0 && InferExprType(c, recvNode, &recvType) == 0 && recvType.valid) {
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
                    if (ownerType.containerKind != SLTypeContainer_SCALAR
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
                        SLTypeRef recvExprType;
                        if (recvNode < 0 || extra >= 0) {
                            return -1;
                        }
                        if (InferExprType(c, recvNode, &recvExprType) != 0 || !recvExprType.valid) {
                            return -1;
                        }
                        return EmitLenExprFromType(c, recvNode, &recvExprType);
                    }
                    if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "new")) {
                        return EmitNewCallExpr(c, nodeId, NULL, 0);
                    }
                    if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "cstr")) {
                        int32_t extra = AstNextSibling(&c->ast, child);
                        if (recvNode < 0 || extra >= 0) {
                            return -1;
                        }
                        if (BufAppendCStr(&c->out, "__sl_cstr(") != 0 || EmitExpr(c, recvNode) != 0
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
                        if (BufAppendCStr(&c->out, "__sl_panic(__FILE__, __LINE__, ") != 0
                            || EmitExpr(c, recvNode) != 0 || BufAppendChar(&c->out, ')') != 0)
                        {
                            return -1;
                        }
                        return 0;
                    }
                    if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "print")) {
                        int32_t extra = AstNextSibling(&c->ast, child);
                        if (recvNode < 0 || extra >= 0) {
                            return -1;
                        }
                        if (BufAppendCStr(&c->out, "__sl_console_log(") != 0
                            || EmitExpr(c, recvNode) != 0
                            || BufAppendCStr(&c->out, ", (__sl_u64)(__sl_i64)(") != 0
                            || EmitEffectiveContextFieldValue(
                                   c,
                                   "console",
                                   &(SLTypeRef){
                                       .baseName = "__sl_i32",
                                       .ptrDepth = 0,
                                       .valid = 1,
                                       .containerKind = SLTypeContainer_SCALAR,
                                       .containerPtrDepth = 0,
                                       .arrayLen = 0,
                                       .hasArrayLen = 0,
                                       .readOnly = 0,
                                       .isOptional = 0,
                                   })
                                   != 0
                            || BufAppendCStr(&c->out, "))") != 0)
                        {
                            return -1;
                        }
                        return 0;
                    }
                    if (recvNode >= 0) {
                        int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
                        SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
                        uint32_t       argCount = 0;
                        const SLFnSig* resolvedSig = NULL;
                        const char*    resolvedName = NULL;
                        if (CollectCallArgInfo(
                                c, nodeId, child, 1, recvNode, argNodes, argTypes, &argCount)
                                == 0
                            && ResolveCallTarget(
                                   c,
                                   callee->dataStart,
                                   callee->dataEnd,
                                   argNodes,
                                   argTypes,
                                   argCount,
                                   &resolvedSig,
                                   &resolvedName)
                                   == 0
                            && resolvedName != NULL)
                        {
                            return EmitResolvedCall(
                                c, nodeId, resolvedName, resolvedSig, argNodes, argCount);
                        }
                    }
                }
            }
            if (callee != NULL && callee->kind == SLAst_IDENT) {
                int32_t        argNodes[SLCCG_MAX_CALL_ARGS];
                SLTypeRef      argTypes[SLCCG_MAX_CALL_ARGS];
                uint32_t       argCount = 0;
                const SLFnSig* resolvedSig = NULL;
                const char*    resolvedName = NULL;
                if (CollectCallArgInfo(c, nodeId, child, 0, -1, argNodes, argTypes, &argCount) == 0
                    && ResolveCallTarget(
                           c,
                           callee->dataStart,
                           callee->dataEnd,
                           argNodes,
                           argTypes,
                           argCount,
                           &resolvedSig,
                           &resolvedName)
                           == 0
                    && resolvedName != NULL)
                {
                    return EmitResolvedCall(
                        c, nodeId, resolvedName, resolvedSig, argNodes, argCount);
                }
            }
            {
                const SLFnSig*       sig = NULL;
                const SLFnTypeAlias* typeAlias = NULL;
                uint32_t             argIndex = 0;
                int                  first = 1;
                if (callee != NULL && callee->kind == SLAst_IDENT) {
                    sig = FindFnSigBySlice(c, callee->dataStart, callee->dataEnd);
                }
                if (sig == NULL && callee != NULL) {
                    SLTypeRef calleeType;
                    if (InferExprType(c, AstFirstChild(&c->ast, nodeId), &calleeType) == 0
                        && calleeType.valid && calleeType.containerKind == SLTypeContainer_SCALAR
                        && calleeType.ptrDepth == 0 && calleeType.containerPtrDepth == 0
                        && calleeType.baseName != NULL && !calleeType.isOptional)
                    {
                        typeAlias = FindFnTypeAliasByName(c, calleeType.baseName);
                    }
                }
                if (EmitExpr(c, child) != 0 || BufAppendChar(&c->out, '(') != 0) {
                    return -1;
                }
                if (sig != NULL && sig->hasContext) {
                    if (EmitContextArgForSig(c, sig) != 0) {
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
            const SLNameMap*   enumMap = NULL;
            int32_t            recv = AstFirstChild(&c->ast, nodeId);
            SLTypeRef          recvType;
            SLTypeRef          ownerType;
            const SLFieldInfo* fieldPath[64];
            uint32_t           fieldPathLen = 0;
            const SLFieldInfo* field = NULL;
            int                useArrow = 0;
            uint32_t           i;

            if (ResolveEnumSelectorByFieldExpr(c, nodeId, &enumMap) != 0 && enumMap != NULL) {
                if (BufAppendCStr(&c->out, enumMap->cName) != 0 || BufAppendCStr(&c->out, "__") != 0
                    || BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd) != 0)
                {
                    return -1;
                }
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

            if (field != NULL && field->isDependent && fieldPathLen > 0) {
                if (BufAppendCStr(&c->out, field->ownerType) != 0
                    || BufAppendCStr(&c->out, "__") != 0
                    || BufAppendCStr(&c->out, field->fieldName) != 0
                    || BufAppendChar(&c->out, '(') != 0)
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
            if (InferExprType(c, inner, &innerType) == 0 && innerType.valid) {
                if (innerType.containerKind == SLTypeContainer_SCALAR && innerType.ptrDepth == 1
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
                if (innerType.containerKind == SLTypeContainer_SCALAR && innerType.ptrDepth > 0) {
                    SLTypeRef pointeeType = innerType;
                    pointeeType.ptrDepth--;
                    if (BufAppendCStr(&c->out, "sizeof(") != 0
                        || EmitTypeNameWithDepth(c, &pointeeType) != 0
                        || BufAppendChar(&c->out, ')') != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
                if (innerType.containerKind == SLTypeContainer_ARRAY && innerType.hasArrayLen
                    && innerType.containerPtrDepth == 1 && SliceStructPtrDepth(&innerType) == 0)
                {
                    if (BufAppendCStr(&c->out, "((__sl_uint)(") != 0
                        || BufAppendU32(&c->out, innerType.arrayLen) != 0
                        || BufAppendCStr(&c->out, "u) * sizeof(") != 0
                        || EmitElementTypeName(c, &innerType, 0) != 0
                        || BufAppendCStr(&c->out, "))") != 0)
                    {
                        return -1;
                    }
                    return 0;
                }
                if ((innerType.containerKind == SLTypeContainer_SLICE_RO
                     || innerType.containerKind == SLTypeContainer_SLICE_MUT)
                    && innerType.containerPtrDepth == 1 && SliceStructPtrDepth(&innerType) == 0)
                {
                    if (BufAppendCStr(&c->out, "((__sl_uint)(") != 0
                        || EmitLenExprFromType(c, inner, &innerType) != 0
                        || BufAppendCStr(&c->out, ") * sizeof(") != 0
                        || EmitElementTypeName(c, &innerType, 0) != 0
                        || BufAppendCStr(&c->out, "))") != 0)
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

static int EmitBlockImpl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int inlineOpen) {
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

static int EmitBlock(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    return EmitBlockImpl(c, nodeId, depth, 0);
}

static int EmitBlockInline(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    return EmitBlockImpl(c, nodeId, depth, 1);
}

static int EmitVarLikeStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isConst) {
    const SLAstNode* n = NodeAt(c, nodeId);
    int32_t          typeNode;
    int32_t          initNode;
    char*            name;
    SLTypeRef        type;

    if (n == NULL) {
        return -1;
    }
    ResolveVarLikeTypeAndInitNode(c, nodeId, &typeNode, &initNode);
    name = DupSlice(c, c->unit->source, n->dataStart, n->dataEnd);
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

static int EmitForStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    int32_t          nodes[4];
    int              count = 0;
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          body;
    const SLAstNode* bodyNode;
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

    body = nodes[count - 1];
    bodyNode = NodeAt(c, body);
    if (count == 1) {
        EmitIndent(c, depth);
        if (BufAppendCStr(&c->out, "for (;;)") != 0) {
            return -1;
        }
        if (bodyNode != NULL && bodyNode->kind == SLAst_BLOCK) {
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
    if (bodyNode != NULL && bodyNode->kind == SLAst_BLOCK) {
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
            int32_t          caseChild = AstFirstChild(&c->ast, child);
            int32_t          bodyNode = -1;
            const SLAstNode* bodyStmt;
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

            bodyStmt = NodeAt(c, bodyNode);
            if (bodyStmt != NULL && bodyStmt->kind == SLAst_BLOCK) {
                if (BufAppendChar(&c->out, ')') != 0 || BufAppendChar(&c->out, ' ') != 0) {
                    return -1;
                }
                if (EmitBlockInline(c, bodyNode, depth + 1u) != 0) {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, ")\n") != 0) {
                    return -1;
                }
                if (EmitStmt(c, bodyNode, depth + 1u) != 0) {
                    return -1;
                }
            }
        } else if (clause != NULL && clause->kind == SLAst_DEFAULT) {
            int32_t          bodyNode = AstFirstChild(&c->ast, child);
            const SLAstNode* bodyStmt = NodeAt(c, bodyNode);
            int              isFirstClause = firstClause;
            EmitIndent(c, depth + 1u);
            if (bodyStmt != NULL && bodyStmt->kind == SLAst_BLOCK) {
                if (BufAppendCStr(&c->out, isFirstClause ? "if (1) " : "else ") != 0) {
                    return -1;
                }
                if (EmitBlockInline(c, bodyNode, depth + 1u) != 0) {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, isFirstClause ? "if (1)\n" : "else\n") != 0) {
                    return -1;
                }
                if (EmitStmt(c, bodyNode, depth + 1u) != 0) {
                    return -1;
                }
            }
            firstClause = 0;
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
            fmtNode = AstNextSibling(&c->ast, cond);
            if (fmtNode < 0) {
                if (BufAppendCStr(&c->out, "__sl_assert(") != 0 || EmitExpr(c, cond) != 0
                    || BufAppendCStr(&c->out, ");\n") != 0)
                {
                    return -1;
                }
            } else {
                int32_t argNode;
                if (BufAppendCStr(&c->out, "__sl_assertf(") != 0 || EmitExpr(c, cond) != 0
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
        case SLAst_IF: {
            int32_t          cond = AstFirstChild(&c->ast, nodeId);
            int32_t          thenNode = AstNextSibling(&c->ast, cond);
            int32_t          elseNode = AstNextSibling(&c->ast, thenNode);
            const SLAstNode* thenStmt = NodeAt(c, thenNode);
            const SLAstNode* elseStmt = NodeAt(c, elseNode);
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "if (") != 0 || EmitExpr(c, cond) != 0) {
                return -1;
            }
            if (thenStmt != NULL && thenStmt->kind == SLAst_BLOCK) {
                if (BufAppendCStr(&c->out, ") ") != 0 || EmitBlockInline(c, thenNode, depth) != 0) {
                    return -1;
                }
            } else {
                if (BufAppendCStr(&c->out, ")\n") != 0 || EmitStmt(c, thenNode, depth) != 0) {
                    return -1;
                }
            }
            if (elseNode >= 0) {
                EmitIndent(c, depth);
                if (elseStmt != NULL && elseStmt->kind == SLAst_BLOCK) {
                    if (BufAppendCStr(&c->out, "else ") != 0
                        || EmitBlockInline(c, elseNode, depth) != 0)
                    {
                        return -1;
                    }
                } else {
                    if (BufAppendCStr(&c->out, "else\n") != 0 || EmitStmt(c, elseNode, depth) != 0)
                    {
                        return -1;
                    }
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
        default: SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, n->start, n->end); return -1;
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
                || firstChild->kind == SLAst_TYPE_OPTIONAL || firstChild->kind == SLAst_TYPE_FN
                || firstChild->kind == SLAst_TYPE_ANON_STRUCT
                || firstChild->kind == SLAst_TYPE_ANON_UNION))
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
        if (n == NULL
            || (n->kind != SLAst_STRUCT && n->kind != SLAst_UNION && n->kind != SLAst_ENUM))
        {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (n->kind == SLAst_ENUM) {
            if (BufAppendCStr(&c->out, "typedef enum ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
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
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map == NULL) {
            continue;
        }
        EmitIndent(c, 0);
        if (n->kind == SLAst_ENUM) {
            if (BufAppendCStr(&c->out, "typedef enum ") != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendChar(&c->out, ' ') != 0
                || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, ";\n") != 0)
            {
                return -1;
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

static int EmitForwardAnonTypeDecls(SLCBackendC* c) {
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

static int EmitAnonTypeDecls(SLCBackendC* c) {
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

static int EmitHeaderTypeAliasDecls(SLCBackendC* c) {
    uint32_t i;
    int      emittedAny = 0;
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != SLAst_TYPE_ALIAS) {
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

static int EmitFnTypeAliasDecls(SLCBackendC* c) {
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

static int EmitFnDeclOrDef(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int emitBody, int isPrivate) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map;
    const char*      fnCName;
    const SLFnSig*   fnSig;
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          bodyNode = -1;
    int32_t          returnTypeNode = -1;
    int              firstParam = 1;
    int              isMainFn;
    int              hasFnContext;
    uint32_t         savedLocalLen;
    SLTypeRef        savedReturnType = c->currentReturnType;
    int              savedHasReturnType = c->hasCurrentReturnType;
    SLTypeRef        savedContextType = c->currentContextType;
    int              savedHasContext = c->hasCurrentContext;
    int              savedCurrentFunctionIsMain = c->currentFunctionIsMain;
    SLTypeRef        fnReturnType;
    SLTypeRef        fnContextType;
    SLTypeRef        fnSemanticContextType;
    SLTypeRef        fnContextParamType;
    SLTypeRef        fnContextLocalType;

    TypeRefSetScalar(&fnReturnType, "void");
    TypeRefSetInvalid(&fnContextType);
    TypeRefSetInvalid(&fnSemanticContextType);
    TypeRefSetInvalid(&fnContextParamType);
    TypeRefSetInvalid(&fnContextLocalType);

    if (n == NULL) {
        return -1;
    }
    map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    fnCName = FindFnCNameByNodeId(c, nodeId);
    if (fnCName == NULL && map != NULL) {
        fnCName = map->cName;
    }
    if (fnCName == NULL) {
        return -1;
    }
    fnSig = FindFnSigByNodeId(c, nodeId);
    isMainFn = IsMainFunctionNode(c, nodeId);
    hasFnContext = fnSig != NULL && (fnSig->hasContext || isMainFn);
    if (hasFnContext) {
        if (isMainFn) {
            TypeRefSetScalar(&fnContextType, "__sl_MainContext");
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
                || ch->kind == SLAst_TYPE_OPTIONAL || ch->kind == SLAst_TYPE_FN
                || ch->kind == SLAst_TYPE_ANON_STRUCT || ch->kind == SLAst_TYPE_ANON_UNION)
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
        if (EmitTypeWithName(c, returnTypeNode, fnCName) != 0) {
            return -1;
        }
    } else if (BufAppendCStr(&c->out, "void ") != 0 || BufAppendCStr(&c->out, fnCName) != 0) {
        return -1;
    }

    if (BufAppendChar(&c->out, '(') != 0) {
        return -1;
    }

    if (hasFnContext) {
        if (isMainFn) {
            if (BufAppendCStr(&c->out, "__sl_MainContext *context __attribute__((unused))") != 0) {
                return -1;
            }
        } else {
            if (EmitTypeRefWithName(c, &fnContextParamType, "context") != 0) {
                return -1;
            }
        }
        firstParam = 0;
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

    savedLocalLen = c->localLen;
    if (PushScope(c) != 0) {
        return -1;
    }
    if (hasFnContext) {
        if (AddLocal(c, "context", fnContextLocalType) != 0) {
            return -1;
        }
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
    c->currentContextType = fnSemanticContextType;
    c->hasCurrentContext = hasFnContext;
    c->currentFunctionIsMain = !hasFnContext && isMainFn;
    if (BufAppendChar(&c->out, ' ') != 0 || EmitBlockInline(c, bodyNode, depth) != 0) {
        c->currentReturnType = savedReturnType;
        c->hasCurrentReturnType = savedHasReturnType;
        c->currentContextType = savedContextType;
        c->hasCurrentContext = savedHasContext;
        c->currentFunctionIsMain = savedCurrentFunctionIsMain;
        return -1;
    }
    c->currentReturnType = savedReturnType;
    c->hasCurrentReturnType = savedHasReturnType;
    c->currentContextType = savedContextType;
    c->hasCurrentContext = savedHasContext;
    c->currentFunctionIsMain = savedCurrentFunctionIsMain;

    PopScope(c);
    c->localLen = savedLocalLen;
    return 0;
}

static int EmitConstDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          typeNode;
    int32_t          initNode;
    SLTypeRef        type;

    ResolveVarLikeTypeAndInitNode(c, nodeId, &typeNode, &initNode);
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
            if (EmitExprCoerced(c, initNode, &type) != 0) {
                return -1;
            }
        } else if (BufAppendChar(&c->out, '0') != 0) {
            return -1;
        }
    }

    return BufAppendCStr(&c->out, ";\n");
}

static int EmitVarDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const SLAstNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          typeNode;
    int32_t          initNode;
    SLTypeRef        type;

    ResolveVarLikeTypeAndInitNode(c, nodeId, &typeNode, &initNode);
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
            if (BufAppendCStr(&c->out, " = ") != 0 || EmitExprCoerced(c, initNode, &type) != 0) {
                return -1;
            }
        } else if (BufAppendCStr(&c->out, " = {0}") != 0) {
            return -1;
        }
    }
    return BufAppendCStr(&c->out, ";\n");
}

static int EmitTypeAliasDecl(
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
        case SLAst_TYPE_ALIAS:
            return EmitTypeAliasDecl(c, nodeId, depth, declarationOnly, isPrivate);
        case SLAst_FN:    return EmitFnDeclOrDef(c, nodeId, depth, emitBody, isPrivate);
        case SLAst_VAR:   return EmitVarDecl(c, nodeId, depth, declarationOnly, isPrivate);
        case SLAst_CONST: return EmitConstDecl(c, nodeId, depth, declarationOnly, isPrivate);
        default:          return 0;
    }
}

static int EmitPrelude(SLCBackendC* c) {
    return BufAppendCStr(&c->out, "#include <core/core.h>\n");
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

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        if (n == NULL || n->kind != SLAst_TYPE_ALIAS || IsExportedTypeNode(c, nodeId)) {
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
    if (CollectTypeAliasInfo(&c) != 0) {
        SetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
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
