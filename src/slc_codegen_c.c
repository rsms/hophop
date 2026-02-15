#include "libsl-impl.h"
#include "slc_codegen.h"

#if SL_LIBC
    #include <stdlib.h>
#else
void* _Nullable malloc(size_t size);
void* _Nullable realloc(void* _Nullable ptr, size_t size);
void free(void* _Nullable ptr);
#endif

SL_API_BEGIN

typedef struct {
    char*    v;
    uint32_t len;
    uint32_t cap;
} SLBuf;

typedef struct {
    const char* baseName;
    int         ptrDepth;
    int         valid;
} SLTypeRef;

typedef struct {
    char*     name;
    char*     cName;
    SLASTKind kind;
    int       isExported;
} SLNameMap;

typedef struct {
    int32_t nodeId;
} SLNodeRef;

typedef struct {
    char*     name;
    SLTypeRef returnType;
} SLFnSig;

typedef struct {
    char*     ownerType;
    char*     fieldName;
    SLTypeRef type;
} SLFieldInfo;

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

    void*   arenaMem;
    SLArena arena;
    SLAST   ast;

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

    SLLocal* locals;
    uint32_t localLen;
    uint32_t localCap;

    uint32_t* localScopeMarks;
    uint32_t  localScopeLen;
    uint32_t  localScopeCap;

    SLStringLiteral* stringLits;
    uint32_t         stringLitLen;
    uint32_t         stringLitCap;

    int32_t* stringLitByNode;
    uint32_t stringLitByNodeLen;
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
    diag->start = start;
    diag->end = end;
}

static int EnsureCap(void** ptr, uint32_t* cap, uint32_t need, size_t elemSize) {
    uint32_t newCap;
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
    newPtr = realloc(*ptr, (size_t)newCap * elemSize);
    if (newPtr == NULL) {
        return -1;
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
    return EnsureCap((void**)&b->v, &b->cap, need, sizeof(char));
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
        out = (char*)malloc(1u);
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
        || StrEq(s, "i16") || StrEq(s, "i32") || StrEq(s, "i64") || StrEq(s, "usize")
        || StrEq(s, "isize") || StrEq(s, "f32") || StrEq(s, "f64");
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

static char* _Nullable DupSlice(const char* src, uint32_t start, uint32_t end) {
    uint32_t len;
    char*    out;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    out = (char*)malloc((size_t)len + 1u);
    if (out == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, src + start, len);
    }
    out[len] = '\0';
    return out;
}

static char* _Nullable DupAndReplaceDots(const char* src, uint32_t start, uint32_t end) {
    char*    out;
    uint32_t i;
    uint32_t len;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    out = (char*)malloc((size_t)len + 1u);
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

static char* _Nullable DupCStr(const char* s) {
    size_t n = StrLen(s);
    char*  out = (char*)malloc(n + 1u);
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
    const char* src, uint32_t start, uint32_t end, uint8_t** outBytes, uint32_t* outLen) {
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
                free(bytes);
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
                        free(bytes);
                        return -1;
                    }
                    hi = HexDigitValue((unsigned char)src[i]);
                    lo = HexDigitValue((unsigned char)src[i + 1u]);
                    if (hi < 0 || lo < 0) {
                        free(bytes);
                        return -1;
                    }
                    ch = (unsigned char)((hi << 4) | lo);
                    i += 2u;
                    break;
                }
                default: ch = esc; break;
            }
        }
        if (EnsureCap((void**)&bytes, &cap, len + 1u, sizeof(uint8_t)) != 0) {
            free(bytes);
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

    if (DecodeStringLiteral(c->unit->source, start, end, &decoded, &decodedLen) != 0) {
        SetDiag(c->diag, SLDiag_UNEXPECTED_TOKEN, start, end);
        return -1;
    }

    for (i = 0; i < c->stringLitLen; i++) {
        if (c->stringLits[i].len == decodedLen
            && ((decodedLen == 0)
                || memcmp(c->stringLits[i].bytes, decoded, (size_t)decodedLen) == 0))
        {
            free(decoded);
            *outLiteralId = (int32_t)i;
            return 0;
        }
    }

    if (EnsureCap(
            (void**)&c->stringLits, &c->stringLitCap, c->stringLitLen + 1u, sizeof(SLStringLiteral))
        != 0)
    {
        free(decoded);
        SetDiag(c->diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }

    c->stringLits[c->stringLitLen].bytes = decoded;
    c->stringLits[c->stringLitLen].len = decodedLen;
    *outLiteralId = (int32_t)c->stringLitLen;
    c->stringLitLen++;
    return 0;
}

static int32_t AstFirstChild(const SLAST* ast, int32_t nodeId);
static int32_t AstNextSibling(const SLAST* ast, int32_t nodeId);

static int CollectStringLiterals(SLCBackendC* c) {
    uint32_t nodeId;

    c->stringLitByNodeLen = c->ast.len;
    c->stringLitByNode = (int32_t*)malloc((size_t)c->stringLitByNodeLen * sizeof(int32_t));
    if (c->stringLitByNode == NULL) {
        return -1;
    }
    for (nodeId = 0; nodeId < c->stringLitByNodeLen; nodeId++) {
        c->stringLitByNode[nodeId] = -1;
    }

    for (nodeId = 0; nodeId < c->ast.len; nodeId++) {
        const SLASTNode* n = &c->ast.nodes[nodeId];
        if (n->kind == SLAST_STRING) {
            uint32_t scanNodeId;
            int      skip = 0;
            for (scanNodeId = 0; scanNodeId < c->ast.len; scanNodeId++) {
                const SLASTNode* parent = &c->ast.nodes[scanNodeId];
                if (parent->kind == SLAST_ASSERT) {
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

static int IsTypeDeclKind(SLASTKind kind) {
    return kind == SLAST_STRUCT || kind == SLAST_UNION || kind == SLAST_ENUM;
}

static int IsDeclKind(SLASTKind kind) {
    return kind == SLAST_FN || kind == SLAST_STRUCT || kind == SLAST_UNION || kind == SLAST_ENUM
        || kind == SLAST_CONST;
}

static int32_t AstFirstChild(const SLAST* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int32_t AstNextSibling(const SLAST* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

static const SLASTNode* _Nullable NodeAt(const SLCBackendC* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast.len) {
        return NULL;
    }
    return &c->ast.nodes[nodeId];
}

static int GetDeclNameSpan(
    const SLCBackendC* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd) {
    const SLASTNode* n = NodeAt(c, nodeId);
    if (n == NULL || !IsDeclKind(n->kind) || n->dataEnd <= n->dataStart) {
        return -1;
    }
    *outStart = n->dataStart;
    *outEnd = n->dataEnd;
    return 0;
}

static int AddName(
    SLCBackendC* c, uint32_t nameStart, uint32_t nameEnd, SLASTKind kind, int isExported) {
    uint32_t i;
    char*    name;
    char*    cName;
    SLBuf    tmp = { 0 };

    for (i = 0; i < c->nameLen; i++) {
        if (SliceEqName(c->unit->source, nameStart, nameEnd, c->names[i].name)) {
            if (isExported) {
                c->names[i].isExported = 1;
            }
            return 0;
        }
    }

    name = DupSlice(c->unit->source, nameStart, nameEnd);
    if (name == NULL) {
        return -1;
    }

    if (HasDoubleUnderscore(name)) {
        cName = DupCStr(name);
    } else {
        if (BufAppendCStr(&tmp, c->unit->packageName) != 0 || BufAppendCStr(&tmp, "__") != 0
            || BufAppendCStr(&tmp, name) != 0)
        {
            free(name);
            free(tmp.v);
            return -1;
        }
        cName = BufFinish(&tmp);
    }
    if (cName == NULL) {
        free(name);
        return -1;
    }

    if (EnsureCap((void**)&c->names, &c->nameCap, c->nameLen + 1u, sizeof(SLNameMap)) != 0) {
        free(name);
        free(cName);
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
    static const char* const builtinNames[] = {
        "void", "bool", "str", "u8",    "u16",   "u32", "u64", "i8",
        "i16",  "i32",  "i64", "usize", "isize", "f32", "f64",
    };

    normalized = DupAndReplaceDots(c->unit->source, start, end);
    if (normalized == NULL) {
        return NULL;
    }

    if (IsBuiltinType(normalized)) {
        for (i = 0; i < (uint32_t)(sizeof(builtinNames) / sizeof(builtinNames[0])); i++) {
            if (StrEq(normalized, builtinNames[i])) {
                free(normalized);
                return builtinNames[i];
            }
        }
        free(normalized);
        return "void";
    }

    mapped = FindNameByCString(c, normalized);
    if (mapped != NULL && IsTypeDeclKind(mapped->kind)) {
        free(normalized);
        return mapped->cName;
    }

    return normalized;
}

static int AddNodeRef(SLNodeRef** arr, uint32_t* len, uint32_t* cap, int32_t nodeId) {
    if (EnsureCap((void**)arr, cap, *len + 1u, sizeof(SLNodeRef)) != 0) {
        return -1;
    }
    (*arr)[*len].nodeId = nodeId;
    (*len)++;
    return 0;
}

static int CollectDeclSets(SLCBackendC* c) {
    int32_t child = AstFirstChild(&c->ast, c->ast.root);
    while (child >= 0) {
        const SLASTNode* n = NodeAt(c, child);
        if (n == NULL) {
            return -1;
        }
        if (n->kind == SLAST_PUB) {
            int32_t pubChild = AstFirstChild(&c->ast, child);
            while (pubChild >= 0) {
                uint32_t         start;
                uint32_t         end;
                const SLASTNode* pn = NodeAt(c, pubChild);
                if (pn != NULL && IsDeclKind(pn->kind)) {
                    if (AddNodeRef(&c->pubDecls, &c->pubDeclLen, &c->pubDeclCap, pubChild) != 0) {
                        return -1;
                    }
                    if (GetDeclNameSpan(c, pubChild, &start, &end) == 0) {
                        if (AddName(c, start, end, pn->kind, 1) != 0) {
                            return -1;
                        }
                    }
                }
                pubChild = AstNextSibling(&c->ast, pubChild);
            }
        } else if (IsDeclKind(n->kind)) {
            uint32_t start;
            uint32_t end;
            if (AddNodeRef(&c->topDecls, &c->topDeclLen, &c->topDeclCap, child) != 0) {
                return -1;
            }
            if (GetDeclNameSpan(c, child, &start, &end) == 0) {
                if (AddName(c, start, end, n->kind, 0) != 0) {
                    return -1;
                }
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

static int ParseTypeRef(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType) {
    const SLASTNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        outType->valid = 0;
        outType->ptrDepth = 0;
        outType->baseName = NULL;
        return -1;
    }
    switch (n->kind) {
        case SLAST_TYPE_NAME: {
            const char* name = ResolveTypeName(c, n->dataStart, n->dataEnd);
            if (name == NULL) {
                return -1;
            }
            outType->valid = 1;
            outType->ptrDepth = 0;
            outType->baseName = name;
            return 0;
        }
        case SLAST_TYPE_PTR: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (ParseTypeRef(c, child, outType) != 0) {
                return -1;
            }
            outType->ptrDepth++;
            return 0;
        }
        case SLAST_TYPE_ARRAY: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            return ParseTypeRef(c, child, outType);
        }
        default:
            outType->valid = 0;
            outType->ptrDepth = 0;
            outType->baseName = NULL;
            return -1;
    }
}

static int AddFnSig(SLCBackendC* c, const char* name, SLTypeRef returnType) {
    uint32_t i;
    for (i = 0; i < c->fnSigLen; i++) {
        if (StrEq(c->fnSigs[i].name, name)) {
            c->fnSigs[i].returnType = returnType;
            return 0;
        }
    }
    if (EnsureCap((void**)&c->fnSigs, &c->fnSigCap, c->fnSigLen + 1u, sizeof(SLFnSig)) != 0) {
        return -1;
    }
    c->fnSigs[c->fnSigLen].name = (char*)name;
    c->fnSigs[c->fnSigLen].returnType = returnType;
    c->fnSigLen++;
    return 0;
}

static int AddFieldInfo(
    SLCBackendC* c, const char* ownerType, const char* fieldName, SLTypeRef type) {
    if (EnsureCap(
            (void**)&c->fieldInfos, &c->fieldInfoCap, c->fieldInfoLen + 1u, sizeof(SLFieldInfo))
        != 0)
    {
        return -1;
    }
    c->fieldInfos[c->fieldInfoLen].ownerType = (char*)ownerType;
    c->fieldInfos[c->fieldInfoLen].fieldName = (char*)fieldName;
    c->fieldInfos[c->fieldInfoLen].type = type;
    c->fieldInfoLen++;
    return 0;
}

static const SLFnSig* _Nullable FindFnSigBySlice(
    const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i;
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
    const SLASTNode* n = NodeAt(c, nodeId);
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

    if (n->kind == SLAST_FN) {
        int32_t   child = AstFirstChild(&c->ast, nodeId);
        SLTypeRef returnType;
        returnType.valid = 1;
        returnType.ptrDepth = 0;
        returnType.baseName = "void";
        while (child >= 0) {
            const SLASTNode* ch = NodeAt(c, child);
            if (ch != NULL
                && (ch->kind == SLAST_TYPE_NAME || ch->kind == SLAST_TYPE_PTR
                    || ch->kind == SLAST_TYPE_ARRAY)
                && ch->flags == 1)
            {
                if (ParseTypeRef(c, child, &returnType) != 0) {
                    return -1;
                }
                break;
            }
            child = AstNextSibling(&c->ast, child);
        }
        return AddFnSig(c, mapName->cName, returnType);
    }

    if (n->kind == SLAST_STRUCT || n->kind == SLAST_UNION) {
        int32_t child = AstFirstChild(&c->ast, nodeId);
        while (child >= 0) {
            const SLASTNode* field = NodeAt(c, child);
            if (field != NULL && field->kind == SLAST_FIELD) {
                int32_t   typeNode = AstFirstChild(&c->ast, child);
                SLTypeRef fieldType;
                char*     fieldName;
                if (ParseTypeRef(c, typeNode, &fieldType) != 0) {
                    return -1;
                }
                fieldName = DupSlice(c->unit->source, field->dataStart, field->dataEnd);
                if (fieldName == NULL) {
                    return -1;
                }
                if (AddFieldInfo(c, mapName->cName, fieldName, fieldType) != 0) {
                    free(fieldName);
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

static int PushScope(SLCBackendC* c) {
    if (EnsureCap(
            (void**)&c->localScopeMarks, &c->localScopeCap, c->localScopeLen + 1u, sizeof(uint32_t))
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

static int AddLocal(SLCBackendC* c, const char* name, SLTypeRef type) {
    if (EnsureCap((void**)&c->locals, &c->localCap, c->localLen + 1u, sizeof(SLLocal)) != 0) {
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
    int i;
    if (!type->valid || type->baseName == NULL) {
        return BufAppendCStr(&c->out, "void");
    }
    if (BufAppendCStr(&c->out, type->baseName) != 0) {
        return -1;
    }
    for (i = 0; i < type->ptrDepth; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    return 0;
}

typedef struct {
    int      kind; /* 1 ptr, 2 array */
    uint32_t dataStart;
    uint32_t dataEnd;
} SLTypeOp;

static int CollectTypeOps(
    const SLAST* ast,
    int32_t      nodeId,
    SLTypeOp*    ops,
    uint32_t*    opLen,
    uint32_t     opCap,
    int32_t*     outBaseNode) {
    const SLASTNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    n = &ast->nodes[nodeId];
    if (n->kind == SLAST_TYPE_PTR) {
        if (*opLen >= opCap) {
            return -1;
        }
        ops[*opLen].kind = 1;
        ops[*opLen].dataStart = 0;
        ops[*opLen].dataEnd = 0;
        (*opLen)++;
        return CollectTypeOps(ast, n->firstChild, ops, opLen, opCap, outBaseNode);
    }
    if (n->kind == SLAST_TYPE_ARRAY) {
        if (*opLen >= opCap) {
            return -1;
        }
        ops[*opLen].kind = 2;
        ops[*opLen].dataStart = n->dataStart;
        ops[*opLen].dataEnd = n->dataEnd;
        (*opLen)++;
        return CollectTypeOps(ast, n->firstChild, ops, opLen, opCap, outBaseNode);
    }
    if (n->kind == SLAST_TYPE_NAME) {
        *outBaseNode = nodeId;
        return 0;
    }
    return -1;
}

static int EmitTypeWithName(SLCBackendC* c, int32_t typeNode, const char* name) {
    SLTypeOp         ops[32];
    uint32_t         opLen = 0;
    int32_t          baseNode = -1;
    const SLASTNode* n;
    const char*      baseType;
    char*            decl;
    uint32_t         i;

    if (CollectTypeOps(&c->ast, typeNode, ops, &opLen, 32u, &baseNode) != 0) {
        return -1;
    }
    n = NodeAt(c, baseNode);
    if (n == NULL) {
        return -1;
    }

    baseType = ResolveTypeName(c, n->dataStart, n->dataEnd);
    if (baseType == NULL) {
        return -1;
    }

    decl = DupCStr(name);
    if (decl == NULL) {
        return -1;
    }

    for (i = 0; i < opLen; i++) {
        SLBuf tmp = { 0 };
        if (ops[i].kind == 1) {
            if (BufAppendChar(&tmp, '*') != 0 || BufAppendCStr(&tmp, decl) != 0) {
                free(decl);
                free(tmp.v);
                return -1;
            }
        } else {
            if (decl[0] == '*') {
                if (BufAppendChar(&tmp, '(') != 0 || BufAppendCStr(&tmp, decl) != 0
                    || BufAppendChar(&tmp, ')') != 0)
                {
                    free(decl);
                    free(tmp.v);
                    return -1;
                }
            } else if (BufAppendCStr(&tmp, decl) != 0) {
                free(decl);
                free(tmp.v);
                return -1;
            }
            if (BufAppendChar(&tmp, '[') != 0
                || BufAppendSlice(&tmp, c->unit->source, ops[i].dataStart, ops[i].dataEnd) != 0
                || BufAppendChar(&tmp, ']') != 0)
            {
                free(decl);
                free(tmp.v);
                return -1;
            }
        }
        free(decl);
        decl = BufFinish(&tmp);
        if (decl == NULL) {
            return -1;
        }
    }

    if (BufAppendCStr(&c->out, baseType) != 0 || BufAppendChar(&c->out, ' ') != 0
        || BufAppendCStr(&c->out, decl) != 0)
    {
        free(decl);
        return -1;
    }

    free(decl);
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
    const SLASTNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        outType->valid = 0;
        return -1;
    }

    switch (n->kind) {
        case SLAST_IDENT: {
            const SLLocal* local = FindLocalBySlice(c, n->dataStart, n->dataEnd);
            if (local != NULL) {
                *outType = local->type;
                return 0;
            }
            outType->valid = 0;
            return 0;
        }
        case SLAST_CALL: {
            int32_t          callee = AstFirstChild(&c->ast, nodeId);
            const SLASTNode* cn = NodeAt(c, callee);
            if (cn != NULL && cn->kind == SLAST_IDENT) {
                const SLFnSig* sig = FindFnSigBySlice(c, cn->dataStart, cn->dataEnd);
                if (sig != NULL) {
                    *outType = sig->returnType;
                    return 0;
                }
            }
            outType->valid = 0;
            return 0;
        }
        case SLAST_UNARY: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (InferExprType(c, child, outType) != 0) {
                return -1;
            }
            if ((SLTokenKind)n->op == SLTok_AND) {
                if (!outType->valid) {
                    outType->valid = 1;
                    outType->baseName = "void";
                    outType->ptrDepth = 1;
                    return 0;
                }
                outType->ptrDepth++;
            } else if ((SLTokenKind)n->op == SLTok_MUL) {
                if (outType->valid && outType->ptrDepth > 0) {
                    outType->ptrDepth--;
                } else {
                    outType->valid = 0;
                }
            }
            return 0;
        }
        case SLAST_FIELD_EXPR: {
            int32_t            recv = AstFirstChild(&c->ast, nodeId);
            SLTypeRef          recvType;
            const SLFieldInfo* field;
            if (InferExprType(c, recv, &recvType) != 0 || !recvType.valid) {
                outType->valid = 0;
                return 0;
            }
            if (recvType.ptrDepth > 0) {
                recvType.ptrDepth--;
            }
            field = FindFieldInfo(c, recvType.baseName, n->dataStart, n->dataEnd);
            if (field != NULL) {
                *outType = field->type;
                return 0;
            }
            outType->valid = 0;
            return 0;
        }
        case SLAST_INDEX: {
            int32_t base = AstFirstChild(&c->ast, nodeId);
            if (InferExprType(c, base, outType) != 0) {
                return -1;
            }
            if (outType->valid && outType->ptrDepth > 0) {
                outType->ptrDepth--;
            }
            return 0;
        }
        case SLAST_CAST: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            int32_t typeNode = AstNextSibling(&c->ast, expr);
            return ParseTypeRef(c, typeNode, outType);
        }
        case SLAST_STRING:
            outType->valid = 1;
            outType->baseName = "str";
            outType->ptrDepth = 0;
            return 0;
        case SLAST_BOOL:
            outType->valid = 1;
            outType->baseName = "bool";
            outType->ptrDepth = 0;
            return 0;
        case SLAST_INT:
            outType->valid = 1;
            outType->baseName = "i32";
            outType->ptrDepth = 0;
            return 0;
        case SLAST_FLOAT:
            outType->valid = 1;
            outType->baseName = "f64";
            outType->ptrDepth = 0;
            return 0;
        default: outType->valid = 0; return 0;
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
    if (BufAppendCStr(&c->out, "(str)(const u8*)(const void*)&sl_lit_") != 0
        || BufAppendU32(&c->out, (uint32_t)literalId) != 0)
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
        if (BufAppendCStr(&c->out, "static const struct { u32 len; u8 bytes[") != 0
            || BufAppendU32(&c->out, lit->len + 1u) != 0
            || BufAppendCStr(&c->out, "]; } sl_lit_") != 0 || BufAppendU32(&c->out, i) != 0
            || BufAppendCStr(&c->out, " = { ") != 0 || BufAppendU32(&c->out, lit->len) != 0
            || BufAppendCStr(&c->out, ", { ") != 0)
        {
            return -1;
        }

        for (j = 0; j < lit->len; j++) {
            if (j > 0 && BufAppendCStr(&c->out, ", ") != 0) {
                return -1;
            }
            if (EmitHexByte(&c->out, lit->bytes[j]) != 0) {
                return -1;
            }
        }
        if (lit->len > 0 && BufAppendCStr(&c->out, ", ") != 0) {
            return -1;
        }
        if (EmitHexByte(&c->out, 0u) != 0) {
            return -1;
        }
        if (BufAppendCStr(&c->out, " } };\n") != 0) {
            return -1;
        }
    }
    if (c->stringLitLen > 0 && BufAppendChar(&c->out, '\n') != 0) {
        return -1;
    }
    return 0;
}

static int EmitExpr(SLCBackendC* c, int32_t nodeId) {
    const SLASTNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAST_IDENT:  return AppendMappedIdentifier(c, n->dataStart, n->dataEnd);
        case SLAST_INT:
        case SLAST_FLOAT:
        case SLAST_BOOL:   return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
        case SLAST_STRING: {
            int32_t literalId = -1;
            if (nodeId >= 0 && (uint32_t)nodeId < c->stringLitByNodeLen) {
                literalId = c->stringLitByNode[nodeId];
            }
            if (literalId < 0) {
                return -1;
            }
            return EmitStringLiteralRef(c, literalId);
        }
        case SLAST_UNARY: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (BufAppendChar(&c->out, '(') != 0
                || BufAppendCStr(&c->out, UnaryOpString((SLTokenKind)n->op)) != 0
                || EmitExpr(c, child) != 0 || BufAppendChar(&c->out, ')') != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAST_BINARY: {
            int32_t lhs = AstFirstChild(&c->ast, nodeId);
            int32_t rhs = AstNextSibling(&c->ast, lhs);
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
        case SLAST_CALL: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            int     first = 1;
            if (EmitExpr(c, child) != 0 || BufAppendChar(&c->out, '(') != 0) {
                return -1;
            }
            child = AstNextSibling(&c->ast, child);
            while (child >= 0) {
                if (!first && BufAppendCStr(&c->out, ", ") != 0) {
                    return -1;
                }
                if (EmitExpr(c, child) != 0) {
                    return -1;
                }
                first = 0;
                child = AstNextSibling(&c->ast, child);
            }
            return BufAppendChar(&c->out, ')');
        }
        case SLAST_INDEX: {
            int32_t base = AstFirstChild(&c->ast, nodeId);
            int32_t idx = AstNextSibling(&c->ast, base);
            if (EmitExpr(c, base) != 0 || BufAppendChar(&c->out, '[') != 0 || EmitExpr(c, idx) != 0
                || BufAppendChar(&c->out, ']') != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAST_FIELD_EXPR: {
            int32_t   recv = AstFirstChild(&c->ast, nodeId);
            SLTypeRef recvType;
            int       useArrow = 0;
            if (InferExprType(c, recv, &recvType) == 0 && recvType.valid && recvType.ptrDepth > 0) {
                useArrow = 1;
            }
            if (EmitExpr(c, recv) != 0 || BufAppendCStr(&c->out, useArrow ? "->" : ".") != 0
                || BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd) != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAST_CAST: {
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
        default: return -1;
    }
}

static int EmitStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth);

static int EmitBlock(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    int32_t child = AstFirstChild(&c->ast, nodeId);
    if (PushScope(c) != 0) {
        return -1;
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "{\n") != 0) {
        PopScope(c);
        return -1;
    }
    while (child >= 0) {
        if (EmitStmt(c, child, depth + 1u) != 0) {
            PopScope(c);
            return -1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "}\n") != 0) {
        PopScope(c);
        return -1;
    }
    PopScope(c);
    return 0;
}

static int EmitVarLikeStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isConst) {
    const SLASTNode* n = NodeAt(c, nodeId);
    int32_t          typeNode = AstFirstChild(&c->ast, nodeId);
    int32_t          initNode = AstNextSibling(&c->ast, typeNode);
    char*            name;
    SLTypeRef        type;

    if (n == NULL) {
        return -1;
    }
    name = DupSlice(c->unit->source, n->dataStart, n->dataEnd);
    if (name == NULL) {
        return -1;
    }
    if (ParseTypeRef(c, typeNode, &type) != 0) {
        free(name);
        return -1;
    }

    EmitIndent(c, depth);
    if (isConst && BufAppendCStr(&c->out, "const ") != 0) {
        free(name);
        return -1;
    }
    if (EmitTypeWithName(c, typeNode, name) != 0) {
        free(name);
        return -1;
    }
    if (initNode >= 0) {
        if (BufAppendCStr(&c->out, " = ") != 0 || EmitExpr(c, initNode) != 0) {
            free(name);
            return -1;
        }
    }
    if (BufAppendCStr(&c->out, ";\n") != 0) {
        free(name);
        return -1;
    }

    if (AddLocal(c, name, type) != 0) {
        free(name);
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
        if (NodeAt(c, body)->kind == SLAST_BLOCK) {
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

    if (count == 2 && NodeAt(c, nodes[0])->kind != SLAST_VAR
        && NodeAt(c, nodes[0])->kind != SLAST_CONST)
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
        if (NodeAt(c, init)->kind == SLAST_VAR) {
            if (EmitVarLikeStmt(c, init, depth + 1u, 0) != 0) {
                return -1;
            }
        } else if (NodeAt(c, init)->kind == SLAST_CONST) {
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
    const SLASTNode* sw = NodeAt(c, nodeId);
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
        const SLASTNode* clause = NodeAt(c, child);
        if (clause != NULL && clause->kind == SLAST_CASE) {
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
        } else if (clause != NULL && clause->kind == SLAST_DEFAULT) {
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
    const SLASTNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == SLAST_STRING) {
        return BufAppendSlice(&c->out, c->unit->source, n->dataStart, n->dataEnd);
    }
    if (BufAppendCStr(&c->out, "(const char*)(const void*)cstr(") != 0 || EmitExpr(c, nodeId) != 0
        || BufAppendChar(&c->out, ')') != 0)
    {
        return -1;
    }
    return 0;
}

static int EmitStmt(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLASTNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAST_BLOCK:     return EmitBlock(c, nodeId, depth);
        case SLAST_VAR:       return EmitVarLikeStmt(c, nodeId, depth, 0);
        case SLAST_CONST:     return EmitVarLikeStmt(c, nodeId, depth, 1);
        case SLAST_EXPR_STMT: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            EmitIndent(c, depth);
            if (EmitExpr(c, expr) != 0 || BufAppendCStr(&c->out, ";\n") != 0) {
                return -1;
            }
            return 0;
        }
        case SLAST_RETURN: {
            int32_t expr = AstFirstChild(&c->ast, nodeId);
            EmitIndent(c, depth);
            if (BufAppendCStr(&c->out, "return") != 0) {
                return -1;
            }
            if (expr >= 0) {
                if (BufAppendChar(&c->out, ' ') != 0 || EmitExpr(c, expr) != 0) {
                    return -1;
                }
            }
            return BufAppendCStr(&c->out, ";\n");
        }
        case SLAST_ASSERT: {
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
                        &c->out, "SL_ASSERT_FAIL(__FILE__, __LINE__, \"assertion failed\");\n")
                    != 0)
                {
                    return -1;
                }
            } else {
                int32_t argNode;
                if (BufAppendCStr(&c->out, "SL_ASSERTF_FAIL(__FILE__, __LINE__, ") != 0
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
        case SLAST_IF: {
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
        case SLAST_FOR:      return EmitForStmt(c, nodeId, depth);
        case SLAST_SWITCH:   return EmitSwitchStmt(c, nodeId, depth);
        case SLAST_BREAK:    EmitIndent(c, depth); return BufAppendCStr(&c->out, "break;\n");
        case SLAST_CONTINUE: EmitIndent(c, depth); return BufAppendCStr(&c->out, "continue;\n");
        case SLAST_DEFER:    SetDiag(c->diag, SLDiag_UNEXPECTED_TOKEN, n->start, n->end); return -1;
        default:             SetDiag(c->diag, SLDiag_UNEXPECTED_TOKEN, n->start, n->end); return -1;
    }
}

static int IsMainFunctionNode(const SLCBackendC* c, int32_t nodeId) {
    const SLASTNode* n = NodeAt(c, nodeId);
    return n != NULL && n->kind == SLAST_FN
        && SliceEq(c->unit->source, n->dataStart, n->dataEnd, "main");
}

static int IsExplicitlyExportedNode(const SLCBackendC* c, int32_t nodeId) {
    const SLASTNode* n = NodeAt(c, nodeId);
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
    const SLASTNode* n = NodeAt(c, nodeId);
    const SLNameMap* map;
    if (n == NULL || !IsTypeDeclKind(n->kind)) {
        return 0;
    }
    map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    return map != NULL && IsTypeDeclKind(map->kind) && map->isExported;
}

static int EmitEnumDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth) {
    const SLASTNode* n = NodeAt(c, nodeId);
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
        const SLASTNode* firstChild = NodeAt(c, child);
        if (firstChild != NULL
            && (firstChild->kind == SLAST_TYPE_NAME || firstChild->kind == SLAST_TYPE_PTR
                || firstChild->kind == SLAST_TYPE_ARRAY))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }

    while (child >= 0) {
        const SLASTNode* item = NodeAt(c, child);
        if (item != NULL && item->kind == SLAST_FIELD) {
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

static int EmitStructOrUnionDecl(SLCBackendC* c, int32_t nodeId, uint32_t depth, int isUnion) {
    const SLASTNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          child = AstFirstChild(&c->ast, nodeId);

    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef ") != 0
        || BufAppendCStr(&c->out, isUnion ? "union " : "struct ") != 0
        || BufAppendCStr(&c->out, map->cName) != 0 || BufAppendCStr(&c->out, " {\n") != 0)
    {
        return -1;
    }

    while (child >= 0) {
        const SLASTNode* field = NodeAt(c, child);
        if (field != NULL && field->kind == SLAST_FIELD) {
            int32_t typeNode = AstFirstChild(&c->ast, child);
            char*   name = DupSlice(c->unit->source, field->dataStart, field->dataEnd);
            if (name == NULL) {
                return -1;
            }
            EmitIndent(c, depth + 1u);
            if (EmitTypeWithName(c, typeNode, name) != 0 || BufAppendCStr(&c->out, ";\n") != 0) {
                free(name);
                return -1;
            }
            free(name);
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

static int EmitFnDeclOrDef(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int emitBody, int isPrivate) {
    const SLASTNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          child = AstFirstChild(&c->ast, nodeId);
    int32_t          bodyNode = -1;
    int32_t          returnTypeNode = -1;
    int              firstParam = 1;
    uint32_t         savedLocalLen;

    EmitIndent(c, depth);
    if (isPrivate && emitBody && BufAppendCStr(&c->out, "static ") != 0) {
        return -1;
    }

    while (child >= 0) {
        const SLASTNode* ch = NodeAt(c, child);
        if (ch != NULL
            && (ch->kind == SLAST_TYPE_NAME || ch->kind == SLAST_TYPE_PTR
                || ch->kind == SLAST_TYPE_ARRAY)
            && ch->flags == 1)
        {
            returnTypeNode = child;
        } else if (ch != NULL && ch->kind == SLAST_BLOCK) {
            bodyNode = child;
        }
        child = AstNextSibling(&c->ast, child);
    }

    if (returnTypeNode >= 0) {
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
        const SLASTNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == SLAST_PARAM) {
            int32_t typeNode = AstFirstChild(&c->ast, child);
            char*   paramName = DupSlice(c->unit->source, ch->dataStart, ch->dataEnd);
            if (paramName == NULL) {
                return -1;
            }
            if (!firstParam && BufAppendCStr(&c->out, ", ") != 0) {
                free(paramName);
                return -1;
            }
            if (EmitTypeWithName(c, typeNode, paramName) != 0) {
                free(paramName);
                return -1;
            }
            firstParam = 0;
            free(paramName);
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
        const SLASTNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == SLAST_PARAM) {
            int32_t   typeNode = AstFirstChild(&c->ast, child);
            SLTypeRef t;
            char*     paramName = DupSlice(c->unit->source, ch->dataStart, ch->dataEnd);
            if (paramName == NULL) {
                return -1;
            }
            if (ParseTypeRef(c, typeNode, &t) != 0 || AddLocal(c, paramName, t) != 0) {
                free(paramName);
                return -1;
            }
        }
        child = AstNextSibling(&c->ast, child);
    }

    if (EmitBlock(c, bodyNode, depth) != 0) {
        return -1;
    }

    PopScope(c);
    c->localLen = savedLocalLen;
    return 0;
}

static int EmitConstDecl(
    SLCBackendC* c, int32_t nodeId, uint32_t depth, int declarationOnly, int isPrivate) {
    const SLASTNode* n = NodeAt(c, nodeId);
    const SLNameMap* map = FindNameBySlice(c, n->dataStart, n->dataEnd);
    int32_t          typeNode = AstFirstChild(&c->ast, nodeId);
    int32_t          initNode = AstNextSibling(&c->ast, typeNode);

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
            if (EmitExpr(c, initNode) != 0) {
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
    const SLASTNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }

    switch (n->kind) {
        case SLAST_STRUCT: return EmitStructOrUnionDecl(c, nodeId, depth, 0);
        case SLAST_UNION:  return EmitStructOrUnionDecl(c, nodeId, depth, 1);
        case SLAST_ENUM:   return EmitEnumDecl(c, nodeId, depth);
        case SLAST_FN:     return EmitFnDeclOrDef(c, nodeId, depth, emitBody, isPrivate);
        case SLAST_CONST:  return EmitConstDecl(c, nodeId, depth, declarationOnly, isPrivate);
        default:           return 0;
    }
}

static int EmitPrelude(SLCBackendC* c) {
    return BufAppendCStr(
        &c->out,
        "#include <stddef.h>\n"
        "#include <stdint.h>\n\n"
        "typedef uint8_t  u8;\n"
        "typedef uint16_t u16;\n"
        "typedef uint32_t u32;\n"
        "typedef uint64_t u64;\n"
        "typedef int8_t   i8;\n"
        "typedef int16_t  i16;\n"
        "typedef int32_t  i32;\n"
        "typedef int64_t  i64;\n"
        "typedef size_t   usize;\n"
        "typedef ptrdiff_t isize;\n"
        "typedef float    f32;\n"
        "typedef double   f64;\n"
        "typedef _Bool    bool;\n"
        "typedef const u8* str;\n"
        "typedef struct { u32 len; u8 bytes[1]; } sl_strhdr;\n"
        "static inline u32 len(str s) { return ((const sl_strhdr*)(const void*)s)->len; }\n"
        "static inline const u8* cstr(str s) {\n"
        "    return ((const sl_strhdr*)(const void*)s)->bytes;\n"
        "}\n"
        "#ifndef SL_TRAP\n"
        "  #if defined(__clang__) || defined(__GNUC__)\n"
        "    #define SL_TRAP() __builtin_trap()\n"
        "  #else\n"
        "    #define SL_TRAP() do { *(volatile int*)0 = 0; } while (0)\n"
        "  #endif\n"
        "#endif\n"
        "#ifndef SL_ASSERT_FAIL\n"
        "  #define SL_ASSERT_FAIL(file,line,msg) SL_TRAP()\n"
        "#endif\n"
        "#ifndef SL_ASSERTF_FAIL\n"
        "  #define SL_ASSERTF_FAIL(file,line,fmt,...) SL_ASSERT_FAIL(file,line,fmt)\n"
        "#endif\n\n");
}

static char* _Nullable BuildDefaultMacro(const char* pkgName, const char* suffix) {
    SLBuf  b = { 0 };
    size_t i;
    for (i = 0; pkgName[i] != '\0'; i++) {
        char ch = pkgName[i];
        if (IsAlnumChar(ch)) {
            ch = ToUpperChar(ch);
        } else {
            ch = '_';
        }
        if (BufAppendChar(&b, ch) != 0) {
            free(b.v);
            return NULL;
        }
    }
    if (BufAppendCStr(&b, suffix) != 0) {
        free(b.v);
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

    defaultGuard = BuildDefaultMacro(c->unit->packageName, "_H");
    defaultImpl = BuildDefaultMacro(c->unit->packageName, "_IMPL");
    if (defaultGuard == NULL || defaultImpl == NULL) {
        free(defaultGuard);
        free(defaultImpl);
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
        free(defaultGuard);
        free(defaultImpl);
        return -1;
    }

    if (EmitPrelude(c) != 0) {
        free(defaultGuard);
        free(defaultImpl);
        return -1;
    }

    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const SLASTNode* n = NodeAt(c, nodeId);
        if (n == NULL) {
            continue;
        }
        if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            free(defaultGuard);
            free(defaultImpl);
            return -1;
        }
    }

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t nodeId = c->topDecls[i].nodeId;
        if (IsMainFunctionNode(c, nodeId) && !IsExplicitlyExportedNode(c, nodeId)) {
            if (EmitDeclNode(c, nodeId, 0, 1, 0, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
                free(defaultGuard);
                free(defaultImpl);
                return -1;
            }
            break;
        }
    }

    if (BufAppendCStr(&c->out, "#ifdef ") != 0 || BufAppendCStr(&c->out, impl) != 0
        || BufAppendCStr(&c->out, "\n\n") != 0)
    {
        free(defaultGuard);
        free(defaultImpl);
        return -1;
    }

    if (EmitStringLiteralPool(c) != 0) {
        free(defaultGuard);
        free(defaultImpl);
        return -1;
    }

    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLASTNode* n = NodeAt(c, nodeId);
        int              exported;
        if (n == NULL) {
            continue;
        }
        exported = IsExportedNode(c, nodeId);

        if (IsTypeDeclKind(n->kind) && IsExportedTypeNode(c, nodeId)) {
            continue;
        }

        if (n->kind == SLAST_FN) {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            int     hasBody = 0;
            while (child >= 0) {
                const SLASTNode* ch = NodeAt(c, child);
                if (ch != NULL && ch->kind == SLAST_BLOCK) {
                    hasBody = 1;
                    break;
                }
                child = AstNextSibling(&c->ast, child);
            }
            if (hasBody) {
                if (EmitDeclNode(c, nodeId, 0, 0, !exported, 1) != 0
                    || BufAppendChar(&c->out, '\n') != 0)
                {
                    free(defaultGuard);
                    free(defaultImpl);
                    return -1;
                }
            } else if (!exported) {
                if (EmitDeclNode(c, nodeId, 0, 1, 1, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
                    free(defaultGuard);
                    free(defaultImpl);
                    return -1;
                }
            }
            continue;
        }

        if (n->kind == SLAST_CONST) {
            if (EmitDeclNode(c, nodeId, 0, 0, !exported, 0) != 0
                || BufAppendChar(&c->out, '\n') != 0)
            {
                free(defaultGuard);
                free(defaultImpl);
                return -1;
            }
            continue;
        }

        if (EmitDeclNode(c, nodeId, 0, 0, !exported, 0) != 0 || BufAppendChar(&c->out, '\n') != 0) {
            free(defaultGuard);
            free(defaultImpl);
            return -1;
        }
    }

    if (BufAppendCStr(&c->out, "#endif /* ") != 0 || BufAppendCStr(&c->out, impl) != 0
        || BufAppendCStr(&c->out, " */\n\n#endif /* ") != 0 || BufAppendCStr(&c->out, guard) != 0
        || BufAppendCStr(&c->out, " */\n") != 0)
    {
        free(defaultGuard);
        free(defaultImpl);
        return -1;
    }

    free(defaultGuard);
    free(defaultImpl);
    return 0;
}

static int InitAst(SLCBackendC* c) {
    uint64_t arenaCap64;
    size_t   arenaCap;
    SLDiag   diag;

    c->arenaMem = NULL;
    c->ast.nodes = NULL;
    c->ast.len = 0;
    c->ast.root = -1;

    arenaCap64 = (uint64_t)(c->unit->sourceLen + 128u) * (uint64_t)sizeof(SLASTNode) + 65536u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        return -1;
    }
    arenaCap = (size_t)arenaCap64;
    c->arenaMem = malloc(arenaCap);
    if (c->arenaMem == NULL) {
        return -1;
    }

    SLArenaInit(&c->arena, c->arenaMem, (uint32_t)arenaCap);
    if (SLParse(&c->arena, (SLStrView){ c->unit->source, c->unit->sourceLen }, &c->ast, &diag) != 0)
    {
        if (c->diag != NULL) {
            *c->diag = diag;
        }
        return -1;
    }
    return 0;
}

static void FreeContext(SLCBackendC* c) {
    uint32_t i;
    free(c->arenaMem);
    for (i = 0; i < c->nameLen; i++) {
        free(c->names[i].name);
        free(c->names[i].cName);
    }
    free(c->names);
    free(c->pubDecls);
    free(c->topDecls);
    free(c->fnSigs);
    for (i = 0; i < c->fieldInfoLen; i++) {
        free(c->fieldInfos[i].fieldName);
    }
    free(c->fieldInfos);
    for (i = 0; i < c->localLen; i++) {
        free(c->locals[i].name);
    }
    free(c->locals);
    free(c->localScopeMarks);
    for (i = 0; i < c->stringLitLen; i++) {
        free(c->stringLits[i].bytes);
    }
    free(c->stringLits);
    free(c->stringLitByNode);
    free(c->out.v);
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

    *outHeader = BufFinish(&c.out);
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
