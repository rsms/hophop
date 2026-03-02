#include "internal.h"

SL_API_BEGIN
size_t StrLen(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

int StrEq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int StrHasPrefix(const char* s, const char* prefix) {
    while (*prefix != '\0') {
        if (*s != *prefix) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

int StrHasSuffix(const char* s, const char* suffix) {
    size_t sLen;
    size_t suffixLen;
    if (s == NULL || suffix == NULL) {
        return 0;
    }
    sLen = StrLen(s);
    suffixLen = StrLen(suffix);
    if (suffixLen > sLen) {
        return 0;
    }
    return memcmp(s + (sLen - suffixLen), suffix, suffixLen) == 0;
}

int IsAlnumChar(char ch) {
    unsigned char c = (unsigned char)ch;
    return (c >= (unsigned char)'0' && c <= (unsigned char)'9')
        || (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

int IsAsciiSpaceChar(unsigned char c) {
    return c == (unsigned char)' ' || c == (unsigned char)'\t' || c == (unsigned char)'\n'
        || c == (unsigned char)'\r' || c == (unsigned char)'\f' || c == (unsigned char)'\v';
}

int IsIdentStartChar(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z') || c == (unsigned char)'_';
}

int IsIdentContinueChar(unsigned char c) {
    return IsIdentStartChar(c) || (c >= (unsigned char)'0' && c <= (unsigned char)'9');
}

char ToUpperChar(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c >= (unsigned char)'a' && c <= (unsigned char)'z') {
        c = (unsigned char)(c - (unsigned char)'a' + (unsigned char)'A');
    }
    return (char)c;
}

void SetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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

int EnsureCapArena(
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

int BufReserve(SLBuf* b, uint32_t extra) {
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

int BufAppend(SLBuf* b, const char* s, uint32_t len) {
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

int BufAppendCStr(SLBuf* b, const char* s) {
    return BufAppend(b, s, (uint32_t)StrLen(s));
}

int BufAppendChar(SLBuf* b, char c) {
    return BufAppend(b, &c, 1u);
}

int BufAppendU32(SLBuf* b, uint32_t value) {
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

int BufAppendSlice(SLBuf* b, const char* src, uint32_t start, uint32_t end) {
    if (end < start) {
        return -1;
    }
    return BufAppend(b, src + start, end - start);
}

char* _Nullable BufFinish(SLBuf* b) {
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

void EmitIndent(SLCBackendC* c, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        (void)BufAppendCStr(&c->out, "    ");
    }
}

int IsBuiltinType(const char* s) {
    return StrEq(s, "bool") || StrEq(s, "str") || StrEq(s, "u8") || StrEq(s, "u16")
        || StrEq(s, "u32") || StrEq(s, "u64") || StrEq(s, "i8") || StrEq(s, "i16")
        || StrEq(s, "i32") || StrEq(s, "i64") || StrEq(s, "uint") || StrEq(s, "int")
        || StrEq(s, "f32") || StrEq(s, "f64") || StrEq(s, "type") || StrEq(s, "anytype");
}

int IsIntegerCTypeName(const char* s) {
    return StrEq(s, "__sl_u8") || StrEq(s, "__sl_u16") || StrEq(s, "__sl_u32")
        || StrEq(s, "__sl_u64") || StrEq(s, "__sl_uint") || StrEq(s, "__sl_i8")
        || StrEq(s, "__sl_i16") || StrEq(s, "__sl_i32") || StrEq(s, "__sl_i64")
        || StrEq(s, "__sl_int");
}

int IsFloatCTypeName(const char* s) {
    return StrEq(s, "__sl_f32") || StrEq(s, "__sl_f64");
}

int SliceEq(const char* src, uint32_t start, uint32_t end, const char* s) {
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

int SliceEqName(const char* src, uint32_t start, uint32_t end, const char* s) {
    return SliceEq(src, start, end, s);
}

int SliceSpanEq(const char* src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

int NameEqPkgPrefixedMethod(
    const char* name,
    const char* src,
    uint32_t    pkgStart,
    uint32_t    pkgEnd,
    uint32_t    methodStart,
    uint32_t    methodEnd) {
    uint32_t pkgLen;
    uint32_t methodLen;
    if (name == NULL || pkgEnd < pkgStart || methodEnd < methodStart) {
        return 0;
    }
    pkgLen = pkgEnd - pkgStart;
    methodLen = methodEnd - methodStart;
    if (StrLen(name) != (size_t)(pkgLen + 2u + methodLen)) {
        return 0;
    }
    if (memcmp(name, src + pkgStart, (size_t)pkgLen) != 0 || name[pkgLen] != '_'
        || name[pkgLen + 1u] != '_')
    {
        return 0;
    }
    return memcmp(name + pkgLen + 2u, src + methodStart, (size_t)methodLen) == 0;
}

int TypeNamePkgPrefixLen(const char* typeName, uint32_t* outPkgLen) {
    uint32_t i = 0;
    if (typeName == NULL || outPkgLen == NULL) {
        return 0;
    }
    while (typeName[i] != '\0') {
        if (typeName[i] == '_' && typeName[i + 1u] == '_' && i > 0) {
            *outPkgLen = i;
            return 1;
        }
        i++;
    }
    return 0;
}

void TypeRefSetInvalid(SLTypeRef* t) {
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

void TypeRefSetScalar(SLTypeRef* t, const char* baseName) {
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

const char* ResolveScalarAliasBaseName(const SLCBackendC* c, const char* typeName);

void CanonicalizeTypeRefBaseName(const SLCBackendC* c, SLTypeRef* t) {
    const char* canonical;
    if (t == NULL || !t->valid || t->baseName == NULL) {
        return;
    }
    canonical = ResolveScalarAliasBaseName(c, t->baseName);
    if (canonical != NULL) {
        t->baseName = canonical;
    }
}

int TypeRefEqual(const SLTypeRef* a, const SLTypeRef* b);
int AddFieldInfo(
    SLCBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int32_t   defaultExprNode,
    int       isDependent,
    int       isEmbedded,
    SLTypeRef type);
const SLAnonTypeInfo* _Nullable FindAnonTypeByCName(const SLCBackendC* c, const char* cName);
int EnsureAnonTypeByFields(
    SLCBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const SLTypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName);
int EnsureAnonTypeVisible(SLCBackendC* c, const SLTypeRef* type, uint32_t depth);
int EmitTypeRefWithName(SLCBackendC* c, const SLTypeRef* t, const char* name);
int EmitTypeNameWithDepth(SLCBackendC* c, const SLTypeRef* type);
int TypeRefIsPointerLike(const SLTypeRef* t);
int ResolveTopLevelConstTypeValueBySlice(
    SLCBackendC* c, uint32_t start, uint32_t end, SLTypeRef* outType);
int EmitDeclNode(
    SLCBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          declarationOnly,
    int          isPrivate,
    int          emitBody);
int IsStrBaseName(const char* _Nullable s);
int ShouldEmitDeclNode(const SLCBackendC* c, int32_t nodeId);

int SliceStructPtrDepth(const SLTypeRef* t) {
    int stars = t->ptrDepth;
    if (t->containerPtrDepth > 0) {
        stars += t->containerPtrDepth - 1;
    }
    return stars;
}

const SLAstNode* _Nullable NodeAt(const SLCBackendC* c, int32_t nodeId);

int ParseArrayLenLiteral(const char* src, uint32_t start, uint32_t end, uint32_t* outLen) {
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

void SetDiagNode(SLCBackendC* c, int32_t nodeId, SLDiagCode code) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n != NULL) {
        SetDiag(c->diag, code, n->start, n->end);
    } else {
        SetDiag(c->diag, code, 0, 0);
    }
}

int BufAppendI64(SLBuf* b, int64_t value) {
    char     tmp[32];
    uint32_t len = 0;
    uint64_t mag;
    if (value < 0) {
        if (BufAppendChar(b, '-') != 0) {
            return -1;
        }
        mag = (uint64_t)(-(value + 1)) + 1u;
    } else {
        mag = (uint64_t)value;
    }
    if (mag == 0) {
        return BufAppendChar(b, '0');
    }
    while (mag > 0) {
        tmp[len++] = (char)('0' + (mag % 10u));
        mag /= 10u;
    }
    while (len > 0) {
        if (BufAppendChar(b, tmp[--len]) != 0) {
            return -1;
        }
    }
    return 0;
}

int EvalConstIntExpr(SLCBackendC* c, int32_t nodeId, int64_t* outValue, int* outIsConst) {
    if (outIsConst == NULL || outValue == NULL) {
        return -1;
    }
    *outIsConst = 0;
    if (c->constEval == NULL) {
        return 0;
    }
    return SLConstEvalSessionEvalIntExpr(c->constEval, nodeId, outValue, outIsConst);
}

int ConstIntFitsIntegerType(const char* typeName, int64_t value) {
    if (typeName == NULL) {
        return 0;
    }
    if (StrEq(typeName, "__sl_u8")) {
        return value >= 0 && value <= (int64_t)UINT8_MAX;
    }
    if (StrEq(typeName, "__sl_u16")) {
        return value >= 0 && value <= (int64_t)UINT16_MAX;
    }
    if (StrEq(typeName, "__sl_u32")) {
        return value >= 0 && value <= (int64_t)UINT32_MAX;
    }
    if (StrEq(typeName, "__sl_u64") || StrEq(typeName, "__sl_uint")) {
        return value >= 0;
    }
    if (StrEq(typeName, "__sl_i8")) {
        return value >= (int64_t)INT8_MIN && value <= (int64_t)INT8_MAX;
    }
    if (StrEq(typeName, "__sl_i16")) {
        return value >= (int64_t)INT16_MIN && value <= (int64_t)INT16_MAX;
    }
    if (StrEq(typeName, "__sl_i32")) {
        return value >= (int64_t)INT32_MIN && value <= (int64_t)INT32_MAX;
    }
    if (StrEq(typeName, "__sl_i64") || StrEq(typeName, "__sl_int")) {
        return 1;
    }
    return 0;
}

int EmitConstEvaluatedScalar(
    SLCBackendC* c, const SLTypeRef* dstType, const SLCTFEValue* value, int* outEmitted) {
    if (outEmitted == NULL || c == NULL || dstType == NULL || value == NULL) {
        return -1;
    }
    *outEmitted = 0;
    if (!dstType->valid || dstType->containerKind != SLTypeContainer_SCALAR
        || dstType->containerPtrDepth != 0 || dstType->isOptional)
    {
        return 0;
    }
    switch (value->kind) {
        case SLCTFEValue_INT:
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(") != 0 || BufAppendI64(&c->out, value->i64) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            *outEmitted = 1;
            return 0;
        case SLCTFEValue_BOOL:
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(") != 0
                || BufAppendChar(&c->out, value->b ? '1' : '0') != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            *outEmitted = 1;
            return 0;
        case SLCTFEValue_NULL:
            if (!TypeRefIsPointerLike(dstType)) {
                return 0;
            }
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(NULL))") != 0)
            {
                return -1;
            }
            *outEmitted = 1;
            return 0;
        case SLCTFEValue_TYPE:
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(") != 0
                || BufAppendHexU64Literal(&c->out, value->typeTag) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            *outEmitted = 1;
            return 0;
        default: return 0;
    }
}

char* _Nullable DupSlice(SLCBackendC* c, const char* src, uint32_t start, uint32_t end) {
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

int SliceIsHoleName(const char* src, uint32_t start, uint32_t end) {
    return end == start + 1u && src[start] == '_';
}

char* _Nullable DupParamNameForEmit(
    SLCBackendC* c, const SLAstNode* paramNode, uint32_t paramIndex) {
    if (paramNode == NULL) {
        return NULL;
    }
    if (SliceIsHoleName(c->unit->source, paramNode->dataStart, paramNode->dataEnd)) {
        SLBuf b = { 0 };
        b.arena = &c->arena;
        if (BufAppendCStr(&b, "__sl_v") != 0 || BufAppendU32(&b, paramIndex) != 0) {
            return NULL;
        }
        return BufFinish(&b);
    }
    return DupSlice(c, c->unit->source, paramNode->dataStart, paramNode->dataEnd);
}

char* _Nullable DupAndReplaceDots(SLCBackendC* c, const char* src, uint32_t start, uint32_t end) {
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

char* _Nullable DupCStr(SLCBackendC* c, const char* s) {
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

int32_t AstFirstChild(const SLAst* ast, int32_t nodeId);
int32_t AstNextSibling(const SLAst* ast, int32_t nodeId);
const SLAstNode* _Nullable NodeAt(const SLCBackendC* c, int32_t nodeId);

int DecodeStringLiteralNode(
    SLCBackendC* c, const SLAstNode* n, uint8_t** outBytes, uint32_t* outLen) {
    SLStringLitErr litErr = { 0 };
    if (n == NULL || n->kind != SLAst_STRING) {
        return -1;
    }
    if (SLDecodeStringLiteralArena(
            &c->arena, c->unit->source, n->dataStart, n->dataEnd, outBytes, outLen, &litErr)
        != 0)
    {
        SetDiag(c->diag, SLStringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
        return -1;
    }
    return 0;
}

int AppendDecodedStringExpr(
    SLCBackendC* c, int32_t nodeId, uint8_t** bytes, uint32_t* len, uint32_t* cap) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == SLAst_STRING) {
        uint8_t* part = NULL;
        uint32_t partLen = 0;
        if (DecodeStringLiteralNode(c, n, &part, &partLen) != 0) {
            return -1;
        }
        if (partLen > 0) {
            if (EnsureCapArena(
                    &c->arena,
                    (void**)bytes,
                    cap,
                    *len + partLen,
                    sizeof(uint8_t),
                    (uint32_t)_Alignof(uint8_t))
                != 0)
            {
                SetDiag(c->diag, SLDiag_ARENA_OOM, n->start, n->end);
                return -1;
            }
            memcpy(*bytes + *len, part, partLen);
            *len += partLen;
        }
        return 0;
    }
    if (n->kind == SLAst_BINARY && (SLTokenKind)n->op == SLTok_ADD
        && SLIsStringLiteralConcatChain(&c->ast, nodeId))
    {
        int32_t lhs = AstFirstChild(&c->ast, nodeId);
        int32_t rhs = AstNextSibling(&c->ast, lhs);
        if (lhs < 0 || rhs < 0) {
            return -1;
        }
        if (AppendDecodedStringExpr(c, lhs, bytes, len, cap) != 0
            || AppendDecodedStringExpr(c, rhs, bytes, len, cap) != 0)
        {
            return -1;
        }
        return 0;
    }
    return -1;
}

int DecodeStringExpr(
    SLCBackendC* c,
    int32_t      nodeId,
    uint8_t**    outBytes,
    uint32_t*    outLen,
    uint32_t*    outStart,
    uint32_t*    outEnd) {
    uint8_t*         bytes = NULL;
    uint32_t         len = 0;
    uint32_t         cap = 0;
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }
    if (AppendDecodedStringExpr(c, nodeId, &bytes, &len, &cap) != 0) {
        return -1;
    }
    *outBytes = bytes;
    *outLen = len;
    *outStart = n->start;
    *outEnd = n->end;
    return 0;
}

int GetOrAddStringLiteralExpr(SLCBackendC* c, int32_t nodeId, int32_t* outLiteralId) {
    uint8_t* decoded = NULL;
    uint32_t decodedLen = 0;
    uint32_t spanStart = 0;
    uint32_t spanEnd = 0;
    uint32_t i;

    if (DecodeStringExpr(c, nodeId, &decoded, &decodedLen, &spanStart, &spanEnd) != 0) {
        if (c->diag != NULL && c->diag->code == SLDiag_NONE) {
            SetDiag(c->diag, SLDiag_CODEGEN_INTERNAL, spanStart, spanEnd);
        }
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
        SetDiag(c->diag, SLDiag_ARENA_OOM, spanStart, spanEnd);
        return -1;
    }

    c->stringLits[c->stringLitLen].bytes = decoded;
    c->stringLits[c->stringLitLen].len = decodedLen;
    *outLiteralId = (int32_t)c->stringLitLen;
    c->stringLitLen++;
    return 0;
}

int GetOrAddStringLiteralBytes(
    SLCBackendC* c, const uint8_t* bytes, uint32_t len, int32_t* outLiteralId) {
    uint32_t i;
    uint8_t* copied = NULL;
    if (c == NULL || outLiteralId == NULL) {
        return -1;
    }
    for (i = 0; i < c->stringLitLen; i++) {
        if (c->stringLits[i].len == len
            && ((len == 0u) || memcmp(c->stringLits[i].bytes, bytes, (size_t)len) == 0))
        {
            *outLiteralId = (int32_t)i;
            return 0;
        }
    }
    if (len > 0u) {
        copied = (uint8_t*)SLArenaAlloc(&c->arena, len, (uint32_t)_Alignof(uint8_t));
        if (copied == NULL) {
            SetDiag(c->diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
        memcpy(copied, bytes, (size_t)len);
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
        SetDiag(c->diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    c->stringLits[c->stringLitLen].bytes = copied;
    c->stringLits[c->stringLitLen].len = len;
    *outLiteralId = (int32_t)c->stringLitLen;
    c->stringLitLen++;
    return 0;
}

int CollectStringLiterals(SLCBackendC* c) {
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
        int              shouldCollect = 0;
        if (n->kind == SLAst_STRING) {
            shouldCollect = 1;
        } else if (
            n->kind == SLAst_BINARY && (SLTokenKind)n->op == SLTok_ADD
            && SLIsStringLiteralConcatChain(&c->ast, (int32_t)nodeId))
        {
            shouldCollect = 1;
        }
        if (shouldCollect) {
            uint32_t scanNodeId;
            int      skip = 0;
            for (scanNodeId = 0; scanNodeId < c->ast.len; scanNodeId++) {
                const SLAstNode* parent = &c->ast.nodes[scanNodeId];
                if (parent->kind == SLAst_ASSERT) {
                    int32_t condNode = AstFirstChild(&c->ast, (int32_t)scanNodeId);
                    int32_t fmtNode = AstNextSibling(&c->ast, condNode);
                    if (n->kind == SLAst_STRING && fmtNode == (int32_t)nodeId) {
                        skip = 1;
                        break;
                    }
                }
            }
            if (skip) {
                continue;
            }
            {
                int32_t literalId;
                if (GetOrAddStringLiteralExpr(c, (int32_t)nodeId, &literalId) != 0) {
                    return -1;
                }
                c->stringLitByNode[nodeId] = literalId;
            }
        }
    }
    return 0;
}

int HasDoubleUnderscore(const char* s) {
    const char* p = s;
    while (*p != '\0') {
        if (p[0] == '_' && p[1] == '_') {
            return 1;
        }
        p++;
    }
    return 0;
}

int IsTypeDeclKind(SLAstKind kind) {
    return kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS;
}

int IsDeclKind(SLAstKind kind) {
    return kind == SLAst_FN || kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS || kind == SLAst_VAR || kind == SLAst_CONST;
}

int IsPubDeclNode(const SLAstNode* n) {
    return (n->flags & SLAstFlag_PUB) != 0;
}

int32_t AstFirstChild(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

int32_t AstNextSibling(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

const SLAstNode* _Nullable NodeAt(const SLCBackendC* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast.len) {
        return NULL;
    }
    return &c->ast.nodes[nodeId];
}

int GetDeclNameSpan(const SLCBackendC* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL || !IsDeclKind(n->kind) || n->dataEnd <= n->dataStart) {
        return -1;
    }
    *outStart = n->dataStart;
    *outEnd = n->dataEnd;
    return 0;
}

int AddName(SLCBackendC* c, uint32_t nameStart, uint32_t nameEnd, SLAstKind kind, int isExported) {
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

const SLNameMap* _Nullable FindNameBySlice(const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->names[i].name)) {
            return &c->names[i];
        }
    }
    return NULL;
}

const SLNameMap* _Nullable FindNameByCString(const SLCBackendC* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (StrEq(c->names[i].name, name)) {
            return &c->names[i];
        }
    }
    return NULL;
}

int NameHasPrefixSuffix(const char* name, const char* prefix, const char* suffix) {
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

int ResolveMainSemanticContextType(SLCBackendC* c, SLTypeRef* outType) {
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
    TypeRefSetScalar(outType, "__sl_Context");
    return 0;
}

const char* ResolveRuneTypeBaseName(SLCBackendC* c) {
    const SLNameMap* map = FindNameByCString(c, "core__rune");
    uint32_t         i;
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        return map->cName;
    }
    for (i = 0; i < c->nameLen; i++) {
        if (IsTypeDeclKind(c->names[i].kind)
            && NameHasPrefixSuffix(c->names[i].name, "core", "__rune"))
        {
            return c->names[i].cName;
        }
    }
    map = FindNameByCString(c, "rune");
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        return map->cName;
    }
    return "__sl_u32";
}

const SLNameMap* _Nullable FindNameByCName(const SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (StrEq(c->names[i].cName, cName)) {
            return &c->names[i];
        }
    }
    return NULL;
}

int ResolveTypeValueNameExprTypeRef(
    SLCBackendC* c, uint32_t start, uint32_t end, SLTypeRef* outTypeRef) {
    const SLNameMap* map;
    if (SliceEq(c->unit->source, start, end, "bool")) {
        TypeRefSetScalar(outTypeRef, "__sl_bool");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "str")) {
        TypeRefSetScalar(outTypeRef, "__sl_str");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u8")) {
        TypeRefSetScalar(outTypeRef, "__sl_u8");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u16")) {
        TypeRefSetScalar(outTypeRef, "__sl_u16");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u32")) {
        TypeRefSetScalar(outTypeRef, "__sl_u32");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u64")) {
        TypeRefSetScalar(outTypeRef, "__sl_u64");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i8")) {
        TypeRefSetScalar(outTypeRef, "__sl_i8");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i16")) {
        TypeRefSetScalar(outTypeRef, "__sl_i16");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i32")) {
        TypeRefSetScalar(outTypeRef, "__sl_i32");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i64")) {
        TypeRefSetScalar(outTypeRef, "__sl_i64");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "uint")) {
        TypeRefSetScalar(outTypeRef, "__sl_uint");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "int")) {
        TypeRefSetScalar(outTypeRef, "__sl_int");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "f32")) {
        TypeRefSetScalar(outTypeRef, "__sl_f32");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "f64")) {
        TypeRefSetScalar(outTypeRef, "__sl_f64");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "type")) {
        TypeRefSetScalar(outTypeRef, "__sl_type");
        return 1;
    }
    map = FindNameBySlice(c, start, end);
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        TypeRefSetScalar(outTypeRef, map->cName);
        return 1;
    }
    TypeRefSetInvalid(outTypeRef);
    return 0;
}

uint64_t TypeTagHashAddByte(uint64_t h, uint8_t b) {
    __uint128_t wide;
    h ^= (uint64_t)b;
    wide = (__uint128_t)h * 1099511628211ULL;
    return (uint64_t)wide;
}

uint64_t TypeTagHashAddU32(uint64_t h, uint32_t v) {
    h = TypeTagHashAddByte(h, (uint8_t)(v & 0xffu));
    h = TypeTagHashAddByte(h, (uint8_t)((v >> 8u) & 0xffu));
    h = TypeTagHashAddByte(h, (uint8_t)((v >> 16u) & 0xffu));
    h = TypeTagHashAddByte(h, (uint8_t)((v >> 24u) & 0xffu));
    return h;
}

uint64_t TypeTagHashAddStr(uint64_t h, const char* s) {
    size_t i = 0;
    if (s == NULL) {
        return TypeTagHashAddByte(h, 0xffu);
    }
    while (s[i] != '\0') {
        h = TypeTagHashAddByte(h, (uint8_t)s[i]);
        i++;
    }
    return TypeTagHashAddByte(h, 0u);
}

enum {
    SLTypeTagKind_INVALID = 0,
    SLTypeTagKind_PRIMITIVE = 1,
    SLTypeTagKind_ALIAS = 2,
    SLTypeTagKind_STRUCT = 3,
    SLTypeTagKind_UNION = 4,
    SLTypeTagKind_ENUM = 5,
    SLTypeTagKind_POINTER = 6,
    SLTypeTagKind_REFERENCE = 7,
    SLTypeTagKind_SLICE = 8,
    SLTypeTagKind_ARRAY = 9,
    SLTypeTagKind_OPTIONAL = 10,
    SLTypeTagKind_FUNCTION = 11,
    SLTypeTagKind_TUPLE = 12,
};

const SLTypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const SLCBackendC* c, const char* aliasName);

uint8_t TypeTagKindFromTypeRef(const SLCBackendC* c, const SLTypeRef* t) {
    const SLNameMap* map;
    if (c == NULL || t == NULL || !t->valid) {
        return SLTypeTagKind_INVALID;
    }
    if (t->isOptional) {
        return SLTypeTagKind_OPTIONAL;
    }
    if (t->containerKind == SLTypeContainer_ARRAY) {
        return SLTypeTagKind_ARRAY;
    }
    if (t->containerKind == SLTypeContainer_SLICE_RO
        || t->containerKind == SLTypeContainer_SLICE_MUT)
    {
        return SLTypeTagKind_SLICE;
    }
    if (t->containerPtrDepth > 0 || t->ptrDepth > 0) {
        return t->readOnly ? SLTypeTagKind_REFERENCE : SLTypeTagKind_POINTER;
    }
    if (t->baseName == NULL) {
        return SLTypeTagKind_INVALID;
    }
    if (FindTypeAliasInfoByAliasName(c, t->baseName) != NULL) {
        return SLTypeTagKind_ALIAS;
    }
    map = FindNameByCName(c, t->baseName);
    if (map != NULL) {
        switch (map->kind) {
            case SLAst_STRUCT:     return SLTypeTagKind_STRUCT;
            case SLAst_UNION:      return SLTypeTagKind_UNION;
            case SLAst_ENUM:       return SLTypeTagKind_ENUM;
            case SLAst_TYPE_ALIAS: return SLTypeTagKind_ALIAS;
            default:               break;
        }
    }
    if (StrHasPrefix(t->baseName, "__sl_fn_t_")) {
        return SLTypeTagKind_FUNCTION;
    }
    if (StrHasPrefix(t->baseName, "__sl_tuple_")) {
        return SLTypeTagKind_TUPLE;
    }
    return SLTypeTagKind_PRIMITIVE;
}

uint64_t TypeTagFromTypeRef(const SLCBackendC* c, const SLTypeRef* t) {
    uint8_t  kind;
    uint64_t h = 1469598103934665603ULL;
    if (t == NULL || !t->valid) {
        return 0u;
    }
    kind = TypeTagKindFromTypeRef(c, t);
    if (kind == SLTypeTagKind_INVALID) {
        kind = SLTypeTagKind_PRIMITIVE;
    }
    h = TypeTagHashAddStr(h, t->baseName);
    h = TypeTagHashAddU32(h, (uint32_t)t->ptrDepth);
    h = TypeTagHashAddU32(h, (uint32_t)t->containerKind);
    h = TypeTagHashAddU32(h, (uint32_t)t->containerPtrDepth);
    h = TypeTagHashAddU32(h, t->arrayLen);
    h = TypeTagHashAddU32(h, (uint32_t)t->hasArrayLen);
    h = TypeTagHashAddU32(h, (uint32_t)t->readOnly);
    h = TypeTagHashAddU32(h, (uint32_t)t->isOptional);
    if (h == 0u) {
        h = 1u;
    }
    return ((uint64_t)kind << 56u) | (h & 0x00ffffffffffffffULL);
}

int EmitTypeTagLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t) {
    uint64_t tag = TypeTagFromTypeRef(c, t);
    return BufAppendHexU64Literal(&c->out, tag);
}

const SLTypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const SLCBackendC* c, const char* aliasName) {
    uint32_t i;
    for (i = 0; i < c->typeAliasLen; i++) {
        if (StrEq(c->typeAliases[i].aliasName, aliasName)) {
            return &c->typeAliases[i];
        }
    }
    return NULL;
}

int AddTypeAliasInfo(SLCBackendC* c, const char* aliasName, SLTypeRef targetType) {
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

const char* ResolveScalarAliasBaseName(const SLCBackendC* c, const char* typeName) {
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

int ResolveReflectedTypeValueExprTypeRef(SLCBackendC* c, int32_t exprNode, SLTypeRef* outTypeRef) {
    const SLAstNode* n = NodeAt(c, exprNode);
    if (outTypeRef == NULL) {
        return 0;
    }
    TypeRefSetInvalid(outTypeRef);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == SLAst_CALL_ARG) {
        int32_t inner = AstFirstChild(&c->ast, exprNode);
        if (inner < 0) {
            return 0;
        }
        return ResolveReflectedTypeValueExprTypeRef(c, inner, outTypeRef);
    }
    if (n->kind == SLAst_IDENT) {
        return ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, outTypeRef);
    }
    if (n->kind == SLAst_TYPE_NAME) {
        return ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, outTypeRef);
    }
    if (n->kind == SLAst_CALL) {
        int32_t          calleeNode = AstFirstChild(&c->ast, exprNode);
        const SLAstNode* callee = NodeAt(c, calleeNode);
        int32_t          argNode;
        int32_t          nextNode;
        if (callee == NULL || callee->kind != SLAst_IDENT) {
            return 0;
        }
        argNode = AstNextSibling(&c->ast, calleeNode);
        nextNode = argNode >= 0 ? AstNextSibling(&c->ast, argNode) : -1;
        if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "typeof")) {
            if (argNode < 0 || nextNode >= 0) {
                return 0;
            }
            if (InferExprType(c, argNode, outTypeRef) != 0 || !outTypeRef->valid) {
                TypeRefSetInvalid(outTypeRef);
                return 0;
            }
            return 1;
        }
        if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "ptr")) {
            SLTypeRef elemType;
            if (argNode < 0 || nextNode >= 0) {
                return 0;
            }
            if (!ResolveReflectedTypeValueExprTypeRef(c, argNode, &elemType)) {
                return 0;
            }
            if (elemType.containerKind == SLTypeContainer_ARRAY
                || elemType.containerKind == SLTypeContainer_SLICE_RO
                || elemType.containerKind == SLTypeContainer_SLICE_MUT)
            {
                elemType.containerPtrDepth++;
                if (elemType.containerKind == SLTypeContainer_SLICE_RO
                    || elemType.containerKind == SLTypeContainer_SLICE_MUT)
                {
                    elemType.containerKind = SLTypeContainer_SLICE_MUT;
                }
                elemType.readOnly = 0;
            } else {
                elemType.ptrDepth++;
                elemType.readOnly = 0;
            }
            *outTypeRef = elemType;
            return 1;
        }
        if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "slice")) {
            SLTypeRef elemType;
            if (argNode < 0 || nextNode >= 0) {
                return 0;
            }
            if (!ResolveReflectedTypeValueExprTypeRef(c, argNode, &elemType)) {
                return 0;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                return 0;
            }
            elemType.containerKind = SLTypeContainer_SLICE_RO;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            elemType.readOnly = 1;
            *outTypeRef = elemType;
            return 1;
        }
        if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "array")) {
            SLTypeRef elemType;
            int32_t   lenNode = nextNode;
            int32_t   extraNode = lenNode >= 0 ? AstNextSibling(&c->ast, lenNode) : -1;
            int32_t   lenExprNode = lenNode;
            int64_t   lenValue = 0;
            int       lenIsConst = 0;
            if (argNode < 0 || lenNode < 0 || extraNode >= 0) {
                return 0;
            }
            if (!ResolveReflectedTypeValueExprTypeRef(c, argNode, &elemType)) {
                return 0;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                return 0;
            }
            if (NodeAt(c, lenExprNode) != NULL && NodeAt(c, lenExprNode)->kind == SLAst_CALL_ARG) {
                lenExprNode = AstFirstChild(&c->ast, lenExprNode);
            }
            if (lenExprNode < 0 || EvalConstIntExpr(c, lenExprNode, &lenValue, &lenIsConst) != 0
                || !lenIsConst || lenValue < 0 || lenValue > (int64_t)UINT32_MAX)
            {
                return 0;
            }
            elemType.containerKind = SLTypeContainer_ARRAY;
            elemType.containerPtrDepth = 0;
            elemType.arrayLen = (uint32_t)lenValue;
            elemType.hasArrayLen = 1;
            elemType.readOnly = 0;
            *outTypeRef = elemType;
            return 1;
        }
    }
    return 0;
}

int TypeRefIsTypeValue(const SLTypeRef* t) {
    return t != NULL && t->valid && t->containerKind == SLTypeContainer_SCALAR && t->ptrDepth == 0
        && t->containerPtrDepth == 0 && !t->isOptional && t->baseName != NULL
        && StrEq(t->baseName, "__sl_type");
}

const char* _Nullable FindReflectKindTypeName(const SLCBackendC* c) {
    uint32_t         i;
    const SLNameMap* direct = FindNameByCString(c, "reflect__Kind");
    if (direct != NULL && direct->kind == SLAst_ENUM) {
        return direct->cName;
    }
    for (i = 0; i < c->nameLen; i++) {
        const SLNameMap* map = &c->names[i];
        if (map->kind != SLAst_ENUM || map->name == NULL) {
            continue;
        }
        if (StrHasPrefix(map->name, "reflect") && StrHasSuffix(map->name, "__Kind")) {
            return map->cName;
        }
    }
    return NULL;
}

const char* TypeRefDisplayBaseName(const SLCBackendC* c, const char* baseName) {
    const SLNameMap* map;
    if (baseName == NULL) {
        return "<type>";
    }
    if (StrEq(baseName, "__sl_bool")) {
        return "bool";
    }
    if (StrEq(baseName, "__sl_str") || StrEq(baseName, "core__str")) {
        return "str";
    }
    if (StrEq(baseName, "__sl_u8")) {
        return "u8";
    }
    if (StrEq(baseName, "__sl_u16")) {
        return "u16";
    }
    if (StrEq(baseName, "__sl_u32")) {
        return "u32";
    }
    if (StrEq(baseName, "__sl_u64")) {
        return "u64";
    }
    if (StrEq(baseName, "__sl_i8")) {
        return "i8";
    }
    if (StrEq(baseName, "__sl_i16")) {
        return "i16";
    }
    if (StrEq(baseName, "__sl_i32")) {
        return "i32";
    }
    if (StrEq(baseName, "__sl_i64")) {
        return "i64";
    }
    if (StrEq(baseName, "__sl_uint")) {
        return "uint";
    }
    if (StrEq(baseName, "__sl_int")) {
        return "int";
    }
    if (StrEq(baseName, "__sl_f32")) {
        return "f32";
    }
    if (StrEq(baseName, "__sl_f64")) {
        return "f64";
    }
    if (StrEq(baseName, "__sl_type")) {
        return "type";
    }
    map = FindNameByCName(c, baseName);
    if (map != NULL && map->name != NULL) {
        return map->name;
    }
    return baseName;
}

int EmitTypeNameStringLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* _Nullable t) {
    const char* name = "<type>";
    int32_t     literalId;
    if (t != NULL && t->valid && !t->isOptional && t->containerKind == SLTypeContainer_SCALAR
        && t->ptrDepth == 0 && t->containerPtrDepth == 0)
    {
        name = TypeRefDisplayBaseName(c, t->baseName);
    }
    if (GetOrAddStringLiteralBytes(c, (const uint8_t*)name, (uint32_t)StrLen(name), &literalId)
        != 0)
    {
        return -1;
    }
    return EmitStringLiteralRef(c, literalId, 0);
}

int EmitTypeTagKindLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t) {
    uint8_t kind = TypeTagKindFromTypeRef(c, t);
    if (kind == SLTypeTagKind_INVALID) {
        kind = SLTypeTagKind_PRIMITIVE;
    }
    return BufAppendU32(&c->out, (uint32_t)kind);
}

int EmitRuntimeTypeTagKindFromExpr(SLCBackendC* c, int32_t exprNode) {
    if (BufAppendCStr(&c->out, "((__sl_u8)(((") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendCStr(&c->out, ") >> 56u) & 0xffu))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitTypeTagIsAliasLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t) {
    return BufAppendCStr(
        &c->out,
        TypeTagKindFromTypeRef(c, t) == SLTypeTagKind_ALIAS ? "((__sl_bool)1)" : "((__sl_bool)0)");
}

int EmitRuntimeTypeTagIsAliasFromExpr(SLCBackendC* c, int32_t exprNode) {
    if (BufAppendCStr(&c->out, "((__sl_bool)((((__sl_u64)(") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendCStr(&c->out, ")) >> 56u) & 0xffu) == 2u))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitRuntimeTypeTagCtorUnary(SLCBackendC* c, uint32_t kindTag, uint64_t salt, int32_t argNode) {
    if (BufAppendCStr(&c->out, "((__sl_type)((((__sl_u64)") != 0
        || BufAppendU32(&c->out, kindTag) != 0
        || BufAppendCStr(&c->out, "u) << 56u) | ((((((__sl_u64)(") != 0 || EmitExpr(c, argNode) != 0
        || BufAppendCStr(&c->out, ")) ^ ") != 0 || BufAppendHexU64Literal(&c->out, salt) != 0
        || BufAppendCStr(&c->out, ") * 1099511628211ULL) & 0x00ffffffffffffffULL))))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitRuntimeTypeTagCtorArray(SLCBackendC* c, int32_t elemTagNode, int32_t lenNode) {
    if (BufAppendCStr(&c->out, "((__sl_type)((((__sl_u64)9u) << 56u) | ((((((__sl_u64)(") != 0
        || EmitExpr(c, elemTagNode) != 0 || BufAppendCStr(&c->out, ")) ^ (((__sl_u64)(") != 0
        || EmitExpr(c, lenNode) != 0
        || BufAppendCStr(
               &c->out,
               ")) << 1u) ^ 0x517cc1b727220a95ULL) * 1099511628211ULL) & "
               "0x00ffffffffffffffULL))))")
               != 0)
    {
        return -1;
    }
    return 0;
}

int EmitTypeTagBaseLiteralFromTypeRef(SLCBackendC* c, const SLTypeRef* t) {
    const SLTypeAliasInfo* alias;
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR || t->ptrDepth != 0
        || t->containerPtrDepth != 0 || t->isOptional || t->baseName == NULL)
    {
        return -1;
    }
    alias = FindTypeAliasInfoByAliasName(c, t->baseName);
    if (alias == NULL || !alias->targetType.valid) {
        return -1;
    }
    return EmitTypeTagLiteralFromTypeRef(c, &alias->targetType);
}

int TypeRefIsRuneLike(const SLCBackendC* c, const SLTypeRef* typeRef) {
    const char* runeType;
    const char* typeName;
    uint32_t    guard = 0;
    if (typeRef == NULL || !typeRef->valid || typeRef->containerKind != SLTypeContainer_SCALAR
        || typeRef->ptrDepth != 0 || typeRef->containerPtrDepth != 0 || typeRef->isOptional
        || typeRef->baseName == NULL)
    {
        return 0;
    }
    runeType = ResolveRuneTypeBaseName((SLCBackendC*)c);
    typeName = typeRef->baseName;
    while (typeName != NULL && guard++ <= c->typeAliasLen + 1u) {
        const SLTypeAliasInfo* alias;
        if (StrEq(typeName, runeType)) {
            return 1;
        }
        alias = FindTypeAliasInfoByAliasName(c, typeName);
        if (alias == NULL || !alias->targetType.valid || alias->targetType.baseName == NULL
            || alias->targetType.containerKind != SLTypeContainer_SCALAR
            || alias->targetType.ptrDepth != 0 || alias->targetType.containerPtrDepth != 0
            || alias->targetType.isOptional)
        {
            break;
        }
        typeName = alias->targetType.baseName;
    }
    return 0;
}

const char* _Nullable ResolveTypeName(SLCBackendC* c, uint32_t start, uint32_t end) {
    const SLNameMap*         mapped;
    char*                    normalized;
    uint32_t                 i;
    static const char* const builtinSlNames[] = {
        "bool", "str", "u8",   "u16", "u32", "u64", "i8",   "i16",
        "i32",  "i64", "uint", "int", "f32", "f64", "type", "anytype",
    };
    static const char* const builtinCNames[] = {
        "__sl_bool", "__sl_str", "__sl_u8",   "__sl_u16", "__sl_u32",  "__sl_u64",
        "__sl_i8",   "__sl_i16", "__sl_i32",  "__sl_i64", "__sl_uint", "__sl_int",
        "__sl_f32",  "__sl_f64", "__sl_type", "__sl_u8",
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

void NormalizeCoreRuntimeTypeName(SLTypeRef* outType) {
    if (outType->baseName != NULL && StrEq(outType->baseName, "core__str")) {
        outType->baseName = "__sl_str";
    } else if (outType->baseName != NULL && StrEq(outType->baseName, "core__Allocator")) {
        outType->baseName = "__sl_Allocator";
    } else if (outType->baseName != NULL && StrEq(outType->baseName, "core__Logger")) {
        outType->baseName = "__sl_Logger";
    }
}

const char* _Nullable ConstEvalBuiltinCName(SLConstEvalBuiltinKind builtin) {
    switch (builtin) {
        case SLConstEvalBuiltinKind_VOID:  return "void";
        case SLConstEvalBuiltinKind_BOOL:  return "__sl_bool";
        case SLConstEvalBuiltinKind_STR:   return "__sl_str";
        case SLConstEvalBuiltinKind_TYPE:  return "__sl_type";
        case SLConstEvalBuiltinKind_U8:    return "__sl_u8";
        case SLConstEvalBuiltinKind_U16:   return "__sl_u16";
        case SLConstEvalBuiltinKind_U32:   return "__sl_u32";
        case SLConstEvalBuiltinKind_U64:   return "__sl_u64";
        case SLConstEvalBuiltinKind_I8:    return "__sl_i8";
        case SLConstEvalBuiltinKind_I16:   return "__sl_i16";
        case SLConstEvalBuiltinKind_I32:   return "__sl_i32";
        case SLConstEvalBuiltinKind_I64:   return "__sl_i64";
        case SLConstEvalBuiltinKind_USIZE: return "__sl_uint";
        case SLConstEvalBuiltinKind_ISIZE: return "__sl_int";
        case SLConstEvalBuiltinKind_F32:   return "__sl_f32";
        case SLConstEvalBuiltinKind_F64:   return "__sl_f64";
        default:                           return NULL;
    }
}

int ParseTypeRefFromConstEvalTypeId(SLCBackendC* c, int32_t typeId, SLTypeRef* outType) {
    SLConstEvalTypeInfo info;
    if (c == NULL || outType == NULL || c->constEval == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (SLConstEvalSessionGetTypeInfo(c->constEval, typeId, &info) != 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    switch (info.kind) {
        case SLConstEvalTypeKind_BUILTIN: {
            const char* baseName = ConstEvalBuiltinCName(info.builtin);
            if (baseName == NULL) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            TypeRefSetScalar(outType, baseName);
            NormalizeCoreRuntimeTypeName(outType);
            return 0;
        }
        case SLConstEvalTypeKind_NAMED:
        case SLConstEvalTypeKind_ALIAS: {
            uint32_t         nameStart = info.nameStart;
            uint32_t         nameEnd = info.nameEnd;
            const SLAstNode* decl = NULL;
            const char*      name;
            if (nameEnd <= nameStart && info.declNode >= 0) {
                decl = NodeAt(c, info.declNode);
                if (decl != NULL && decl->dataEnd > decl->dataStart) {
                    nameStart = decl->dataStart;
                    nameEnd = decl->dataEnd;
                }
            }
            if (nameEnd <= nameStart) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            name = ResolveTypeName(c, nameStart, nameEnd);
            if (name == NULL) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            TypeRefSetScalar(outType, name);
            NormalizeCoreRuntimeTypeName(outType);
            return 0;
        }
        case SLConstEvalTypeKind_PTR:
        case SLConstEvalTypeKind_REF: {
            SLTypeRef childType;
            int       isRef = info.kind == SLConstEvalTypeKind_REF;
            int       isReadOnlyRef = isRef && !(info.flags & SLConstEvalTypeFlag_MUTABLE);
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, &childType) != 0) {
                TypeRefSetInvalid(outType);
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
                childType.readOnly = isRef ? isReadOnlyRef : 0;
            } else {
                childType.ptrDepth++;
                childType.readOnly = isRef ? isReadOnlyRef : 0;
            }
            *outType = childType;
            return 0;
        }
        case SLConstEvalTypeKind_ARRAY: {
            SLTypeRef elemType;
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, &elemType) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.containerKind = SLTypeContainer_ARRAY;
            elemType.containerPtrDepth = 0;
            elemType.arrayLen = info.arrayLen;
            elemType.hasArrayLen = 1;
            elemType.readOnly = 0;
            *outType = elemType;
            return 0;
        }
        case SLConstEvalTypeKind_SLICE: {
            SLTypeRef elemType;
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, &elemType) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.containerKind =
                (info.flags & SLConstEvalTypeFlag_MUTABLE)
                    ? SLTypeContainer_SLICE_MUT
                    : SLTypeContainer_SLICE_RO;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            elemType.readOnly = (info.flags & SLConstEvalTypeFlag_MUTABLE) ? 0 : 1;
            *outType = elemType;
            return 0;
        }
        case SLConstEvalTypeKind_OPTIONAL:
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, outType) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            outType->isOptional = 1;
            return 0;
        case SLConstEvalTypeKind_UNTYPED_INT:   TypeRefSetScalar(outType, "__sl_int"); return 0;
        case SLConstEvalTypeKind_UNTYPED_FLOAT: TypeRefSetScalar(outType, "__sl_f64"); return 0;
        default:                                TypeRefSetInvalid(outType); return -1;
    }
}

int ParseTypeRefFromConstEvalTypeTag(SLCBackendC* c, uint64_t typeTag, SLTypeRef* outType) {
    int32_t typeId = -1;
    if (c == NULL || outType == NULL || c->constEval == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (SLConstEvalSessionDecodeTypeTag(c->constEval, typeTag, &typeId) != 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    return ParseTypeRefFromConstEvalTypeId(c, typeId, outType);
}

int AddNodeRef(SLCBackendC* c, SLNodeRef** arr, uint32_t* len, uint32_t* cap, int32_t nodeId) {
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

int CollectDeclSets(SLCBackendC* c) {
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
            if ((n->kind == SLAst_VAR || n->kind == SLAst_CONST)
                && NodeAt(c, AstFirstChild(&c->ast, child)) != NULL
                && NodeAt(c, AstFirstChild(&c->ast, child))->kind == SLAst_NAME_LIST)
            {
                int32_t  nameList = AstFirstChild(&c->ast, child);
                uint32_t i;
                uint32_t nameCount = ListCount(&c->ast, nameList);
                for (i = 0; i < nameCount; i++) {
                    int32_t          nameNode = ListItemAt(&c->ast, nameList, i);
                    const SLAstNode* name = NodeAt(c, nameNode);
                    if (name == NULL) {
                        return -1;
                    }
                    if (AddName(c, name->dataStart, name->dataEnd, n->kind, isExported) != 0) {
                        return -1;
                    }
                }
            } else if (GetDeclNameSpan(c, child, &start, &end) == 0) {
                if (AddName(c, start, end, n->kind, isExported) != 0) {
                    return -1;
                }
            }
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

const SLFnTypeAlias* _Nullable FindFnTypeAliasByName(const SLCBackendC* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        if (StrEq(c->fnTypeAliases[i].aliasName, name)) {
            return &c->fnTypeAliases[i];
        }
    }
    return NULL;
}

int EnsureFnTypeAlias(
    SLCBackendC* c,
    SLTypeRef    returnType,
    SLTypeRef*   paramTypes,
    uint32_t     paramLen,
    int          isVariadic,
    const char** outAliasName) {
    uint32_t i;
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        const SLFnTypeAlias* alias = &c->fnTypeAliases[i];
        uint32_t             p;
        int                  same = 1;
        if (!TypeRefEqual(&alias->returnType, &returnType) || alias->paramLen != paramLen
            || alias->isVariadic != (uint8_t)(isVariadic ? 1 : 0))
        {
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
        c->fnTypeAliases[c->fnTypeAliasLen].isVariadic = (uint8_t)(isVariadic ? 1 : 0);
        c->fnTypeAliases[c->fnTypeAliasLen]._reserved[0] = 0;
        c->fnTypeAliases[c->fnTypeAliasLen]._reserved[1] = 0;
        c->fnTypeAliases[c->fnTypeAliasLen]._reserved[2] = 0;
        c->fnTypeAliasLen++;
        *outAliasName = aliasName;
        return 0;
    }
}

const char* _Nullable TupleFieldName(SLCBackendC* c, uint32_t index) {
    SLBuf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, "__sl_t") != 0 || BufAppendU32(&b, index) != 0) {
        return NULL;
    }
    return BufFinish(&b);
}

int ParseTypeRef(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType) {
    const SLAstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    switch (n->kind) {
        case SLAst_TYPE_NAME: {
            SLTypeRef reflectedType;
            if (ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, &reflectedType)) {
                if (!reflectedType.valid) {
                    return -1;
                }
                *outType = reflectedType;
                NormalizeCoreRuntimeTypeName(outType);
                return 0;
            }
            {
                int resolvedConstType = ResolveTopLevelConstTypeValueBySlice(
                    c, n->dataStart, n->dataEnd, &reflectedType);
                if (resolvedConstType < 0) {
                    SetDiagNode(c, nodeId, SLDiag_CODEGEN_INTERNAL);
                    return -1;
                }
                if (resolvedConstType > 0) {
                    if (!reflectedType.valid) {
                        return -1;
                    }
                    *outType = reflectedType;
                    NormalizeCoreRuntimeTypeName(outType);
                    return 0;
                }
            }
            {
                char* normalized = DupAndReplaceDots(c, c->unit->source, n->dataStart, n->dataEnd);
                const char* name = ResolveTypeName(c, n->dataStart, n->dataEnd);
                if (normalized == NULL) {
                    SetDiagNode(c, nodeId, SLDiag_ARENA_OOM);
                    return -1;
                }
                if (!IsBuiltinType(normalized)) {
                    const SLNameMap* map = FindNameByCString(c, normalized);
                    if (map == NULL || !IsTypeDeclKind(map->kind)) {
                        SetDiag(c->diag, SLDiag_UNKNOWN_TYPE, n->dataStart, n->dataEnd);
                        return -1;
                    }
                }
                if (name == NULL) {
                    SetDiagNode(c, nodeId, SLDiag_CODEGEN_INTERNAL);
                    return -1;
                }
                TypeRefSetScalar(outType, name);
                NormalizeCoreRuntimeTypeName(outType);
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
            int32_t   lenNode = AstNextSibling(&c->ast, child);
            SLTypeRef elemType;
            int64_t   lenValue = 0;
            int       lenIsConst = 0;
            uint32_t  len = 0;
            if (ParseTypeRef(c, child, &elemType) != 0) {
                return -1;
            }
            if (elemType.containerKind != SLTypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (lenNode < 0) {
                if (ParseArrayLenLiteral(c->unit->source, n->dataStart, n->dataEnd, &len) != 0) {
                    SetDiagNode(c, nodeId, SLDiag_CODEGEN_INTERNAL);
                    TypeRefSetInvalid(outType);
                    return -1;
                }
            } else {
                if (EvalConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0 || !lenIsConst
                    || lenValue < 0 || lenValue > (int64_t)UINT32_MAX)
                {
                    SetDiagNode(c, lenNode, SLDiag_CODEGEN_INTERNAL);
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                len = (uint32_t)lenValue;
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
            int         isVariadic = 0;
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
                    if (ch != NULL && (ch->flags & SLAstFlag_PARAM_VARIADIC) != 0) {
                        if (isVariadic) {
                            return -1;
                        }
                        if (paramType.containerKind != SLTypeContainer_SCALAR) {
                            return -1;
                        }
                        paramType.containerKind = SLTypeContainer_SLICE_RO;
                        paramType.containerPtrDepth = 0;
                        paramType.hasArrayLen = 0;
                        paramType.arrayLen = 0;
                        paramType.readOnly = 1;
                        isVariadic = 1;
                    } else if (isVariadic) {
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
            if (EnsureFnTypeAlias(c, returnType, paramTypes, paramLen, isVariadic, &aliasName) != 0)
            {
                return -1;
            }
            TypeRefSetScalar(outType, aliasName);
            return 0;
        }
        case SLAst_TYPE_TUPLE: {
            int32_t     child = AstFirstChild(&c->ast, nodeId);
            const char* fieldNames[256];
            SLTypeRef   fieldTypes[256];
            uint32_t    fieldCount = 0;
            const char* anonName = NULL;
            while (child >= 0) {
                if (fieldCount >= (uint32_t)(sizeof(fieldNames) / sizeof(fieldNames[0]))) {
                    return -1;
                }
                if (ParseTypeRef(c, child, &fieldTypes[fieldCount]) != 0) {
                    return -1;
                }
                CanonicalizeTypeRefBaseName(c, &fieldTypes[fieldCount]);
                fieldNames[fieldCount] = TupleFieldName(c, fieldCount);
                if (fieldNames[fieldCount] == NULL) {
                    return -1;
                }
                fieldCount++;
                child = AstNextSibling(&c->ast, child);
            }
            if (fieldCount < 2u) {
                return -1;
            }
            if (EnsureAnonTypeByFields(c, 0, fieldNames, fieldTypes, fieldCount, &anonName) != 0) {
                return -1;
            }
            TypeRefSetScalar(outType, anonName);
            return 0;
        }
        default: TypeRefSetInvalid(outType); return -1;
    }
}

int AddFnSig(
    SLCBackendC* c,
    const char*  slName,
    const char*  baseCName,
    int32_t      nodeId,
    SLTypeRef    returnType,
    SLTypeRef*   paramTypes,
    char** _Nullable paramNames,
    uint8_t* _Nullable paramFlags,
    uint32_t  paramLen,
    int       isVariadic,
    int       hasContext,
    SLTypeRef contextType) {
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
        if (c->fnSigs[i].isVariadic != (uint8_t)(isVariadic ? 1 : 0)) {
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
            if ((((c->fnSigs[i].paramFlags != NULL) ? c->fnSigs[i].paramFlags[p] : 0u)
                 & SLCCGParamFlag_CONST)
                != (((paramFlags != NULL) ? paramFlags[p] : 0u) & SLCCGParamFlag_CONST))
            {
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
    c->fnSigs[c->fnSigLen].paramNames = paramNames;
    c->fnSigs[c->fnSigLen].paramFlags = paramFlags;
    c->fnSigs[c->fnSigLen].paramLen = paramLen;
    c->fnSigs[c->fnSigLen].hasContext = hasContext;
    c->fnSigs[c->fnSigLen].contextType = contextType;
    c->fnSigs[c->fnSigLen].isVariadic = (uint8_t)(isVariadic ? 1 : 0);
    c->fnSigs[c->fnSigLen]._reserved[0] = 0;
    c->fnSigs[c->fnSigLen]._reserved[1] = 0;
    c->fnSigs[c->fnSigLen]._reserved[2] = 0;
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

int AddFieldInfo(
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

int AppendTypeRefKey(SLBuf* b, const SLTypeRef* t) {
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

const SLAnonTypeInfo* _Nullable FindAnonTypeByKey(const SLCBackendC* c, const char* key) {
    uint32_t i;
    for (i = 0; i < c->anonTypeLen; i++) {
        if (StrEq(c->anonTypes[i].key, key)) {
            return &c->anonTypes[i];
        }
    }
    return NULL;
}

const SLAnonTypeInfo* _Nullable FindAnonTypeByCName(const SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->anonTypeLen; i++) {
        if (StrEq(c->anonTypes[i].cName, cName)) {
            return &c->anonTypes[i];
        }
    }
    return NULL;
}

int IsTupleFieldName(const char* name, uint32_t index) {
    const char* prefix = "__sl_t";
    uint32_t    i = 0;
    uint32_t    n = index;
    char        digits[16];
    uint32_t    dlen = 0;
    if (name == NULL) {
        return 0;
    }
    while (prefix[i] != '\0') {
        if (name[i] != prefix[i]) {
            return 0;
        }
        i++;
    }
    if (n == 0) {
        digits[dlen++] = '0';
    } else {
        while (n > 0 && dlen < (uint32_t)sizeof(digits)) {
            digits[dlen++] = (char)('0' + (n % 10u));
            n /= 10u;
        }
    }
    if (name[i + dlen] != '\0') {
        return 0;
    }
    while (dlen > 0) {
        dlen--;
        if (name[i++] != digits[dlen]) {
            return 0;
        }
    }
    return 1;
}

int TypeRefTupleInfo(const SLCBackendC* c, const SLTypeRef* t, const SLAnonTypeInfo** outInfo) {
    const SLAnonTypeInfo* info;
    uint32_t              i;
    if (outInfo != NULL) {
        *outInfo = NULL;
    }
    if (t == NULL || !t->valid || t->baseName == NULL || t->ptrDepth != 0 || t->isOptional
        || t->containerKind != SLTypeContainer_SCALAR || t->containerPtrDepth != 0)
    {
        return 0;
    }
    info = FindAnonTypeByCName(c, t->baseName);
    if (info == NULL || info->isUnion || info->fieldCount < 2u) {
        return 0;
    }
    for (i = 0; i < info->fieldCount; i++) {
        const SLFieldInfo* f = &c->fieldInfos[info->fieldStart + i];
        if (!IsTupleFieldName(f->fieldName, i)) {
            return 0;
        }
    }
    if (outInfo != NULL) {
        *outInfo = info;
    }
    return 1;
}

int IsLocalAnonTypedefVisible(const SLCBackendC* c, const char* cName) {
    uint32_t i = c->localAnonTypedefLen;
    while (i > 0) {
        i--;
        if (StrEq(c->localAnonTypedefs[i], cName)) {
            return 1;
        }
    }
    return 0;
}

int MarkLocalAnonTypedefVisible(SLCBackendC* c, const char* cName) {
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

int IsAnonTypeNameVisible(const SLCBackendC* c, const char* cName) {
    const SLAnonTypeInfo* info = FindAnonTypeByCName(c, cName);
    if (info != NULL && (info->flags & SLAnonTypeFlag_EMITTED_GLOBAL) != 0) {
        return 1;
    }
    return IsLocalAnonTypedefVisible(c, cName);
}

int EmitAnonTypeDeclAtDepth(SLCBackendC* c, const SLAnonTypeInfo* t, uint32_t depth) {
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

int EnsureAnonTypeVisible(SLCBackendC* c, const SLTypeRef* type, uint32_t depth) {
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

int EnsureAnonTypeByFields(
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

const SLFnSig* _Nullable FindFnSigBySlice(const SLCBackendC* c, uint32_t start, uint32_t end) {
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

uint32_t FindFnSigCandidatesBySlice(
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

uint32_t FindFnSigCandidatesByName(
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

const char* _Nullable FindFnCNameByNodeId(const SLCBackendC* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->fnNodeNameLen; i++) {
        if (c->fnNodeNames[i].nodeId == nodeId) {
            return c->fnNodeNames[i].cName;
        }
    }
    return NULL;
}

const SLFnSig* _Nullable FindFnSigByNodeId(const SLCBackendC* c, int32_t nodeId) {
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

const SLFieldInfo* _Nullable FindFieldInfo(
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

const SLFieldInfo* _Nullable FindEmbeddedFieldInfo(const SLCBackendC* c, const char* ownerType) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        if (StrEq(c->fieldInfos[i].ownerType, ownerType) && c->fieldInfos[i].isEmbedded) {
            return &c->fieldInfos[i];
        }
    }
    return NULL;
}

const SLFieldInfo* _Nullable FindFieldInfoByName(
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

const char* _Nullable CanonicalFieldOwnerType(
    const SLCBackendC* c, const char* _Nullable ownerType) {
    const char* canonical = ResolveScalarAliasBaseName(c, ownerType);
    if (canonical == NULL) {
        canonical = ownerType;
    }
    if (canonical == NULL) {
        return NULL;
    }
    if (StrEq(canonical, "__sl_Allocator")) {
        return "core__Allocator";
    }
    if (StrEq(canonical, "__sl_Logger")) {
        return "core__Logger";
    }
    return canonical;
}

int ResolveCoreStrFieldBySlice(
    const SLCBackendC* c, uint32_t fieldStart, uint32_t fieldEnd, const SLFieldInfo** outField) {
    static int         inited = 0;
    static SLFieldInfo lenField;
    static SLFieldInfo ptrField;
    if (!inited) {
        memset(&lenField, 0, sizeof(lenField));
        memset(&ptrField, 0, sizeof(ptrField));
        lenField.ownerType = "core__str";
        lenField.fieldName = "len";
        TypeRefSetScalar(&lenField.type, "__sl_uint");

        ptrField.ownerType = "core__str";
        ptrField.fieldName = "ptr";
        TypeRefSetScalar(&ptrField.type, "__sl_u8");
        ptrField.type.ptrDepth = 1;
        inited = 1;
    }
    if (SliceEqName(c->unit->source, fieldStart, fieldEnd, "len")) {
        *outField = &lenField;
        return 0;
    }
    if (SliceEqName(c->unit->source, fieldStart, fieldEnd, "ptr")) {
        *outField = &ptrField;
        return 0;
    }
    return -1;
}

int ResolveFieldPathSingleSegment(
    const SLCBackendC*  c,
    const char*         ownerTypeIn,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const SLFieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const SLFieldInfo** _Nullable outField) {
    const SLFieldInfo* direct;
    const SLFieldInfo* embedded;
    const char*        embeddedBaseName;
    const char*        ownerType;
    uint32_t           nestedLen = 0;

    if (outLen == NULL || cap == 0u) {
        return -1;
    }

    ownerType = CanonicalFieldOwnerType(c, ownerTypeIn);
    if (ownerType == NULL) {
        return -1;
    }

    if (IsStrBaseName(ownerType)) {
        const SLFieldInfo* strField = NULL;
        if (ResolveCoreStrFieldBySlice(c, fieldStart, fieldEnd, &strField) == 0) {
            outPath[0] = strField;
            *outLen = 1u;
            if (outField != NULL) {
                *outField = strField;
            }
            return 0;
        }
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

    embeddedBaseName = CanonicalFieldOwnerType(c, embedded->type.baseName);
    if (embeddedBaseName == NULL) {
        return -1;
    }
    if (ResolveFieldPathSingleSegment(
            c, embeddedBaseName, fieldStart, fieldEnd, outPath + 1u, cap - 1u, &nestedLen, outField)
        != 0)
    {
        return -1;
    }
    outPath[0] = embedded;
    *outLen = nestedLen + 1u;
    return 0;
}

/* Returns 0 for a segment, 1 for end-of-path, -1 for malformed syntax. */
int FieldPathNextSegment(
    const char* src,
    uint32_t    pathEnd,
    uint32_t*   ioPos,
    uint32_t*   outSegStart,
    uint32_t*   outSegEnd) {
    uint32_t pos = *ioPos;
    while (pos < pathEnd && IsAsciiSpaceChar((unsigned char)src[pos])) {
        pos++;
    }
    if (pos >= pathEnd) {
        *ioPos = pos;
        return 1;
    }
    if (!IsIdentStartChar((unsigned char)src[pos])) {
        return -1;
    }
    *outSegStart = pos;
    pos++;
    while (pos < pathEnd && IsIdentContinueChar((unsigned char)src[pos])) {
        pos++;
    }
    *outSegEnd = pos;
    while (pos < pathEnd && IsAsciiSpaceChar((unsigned char)src[pos])) {
        pos++;
    }
    if (pos < pathEnd) {
        if (src[pos] != '.') {
            return -1;
        }
        pos++;
    }
    *ioPos = pos;
    return 0;
}

int ResolveFieldPathBySlice(
    const SLCBackendC*  c,
    const char*         ownerType,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const SLFieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const SLFieldInfo** _Nullable outField) {
    uint32_t           pos = fieldStart;
    uint32_t           totalLen = 0;
    const char*        curOwnerType = ownerType;
    const SLFieldInfo* lastField = NULL;

    while (pos <= fieldEnd) {
        const SLFieldInfo* segPath[64];
        const SLFieldInfo* segField = NULL;
        uint32_t           segLen = 0;
        uint32_t           segStart = 0;
        uint32_t           segEnd = 0;
        int      nextRc = FieldPathNextSegment(c->unit->source, fieldEnd, &pos, &segStart, &segEnd);
        uint32_t i;
        if (nextRc == 1) {
            break;
        }
        if (nextRc != 0 || curOwnerType == NULL) {
            return -1;
        }
        if (ResolveFieldPathSingleSegment(
                c,
                curOwnerType,
                segStart,
                segEnd,
                segPath,
                (uint32_t)(sizeof(segPath) / sizeof(segPath[0])),
                &segLen,
                &segField)
                != 0
            || segLen == 0)
        {
            return -1;
        }
        if (totalLen + segLen > cap) {
            return -1;
        }
        for (i = 0; i < segLen; i++) {
            outPath[totalLen + i] = segPath[i];
        }
        totalLen += segLen;
        lastField = segField;

        if (segField != NULL && segField->type.valid
            && segField->type.containerKind == SLTypeContainer_SCALAR
            && segField->type.ptrDepth == 0 && segField->type.containerPtrDepth == 0
            && segField->type.baseName != NULL && !segField->type.isOptional)
        {
            curOwnerType = ResolveScalarAliasBaseName(c, segField->type.baseName);
        } else {
            curOwnerType = NULL;
        }
    }

    if (totalLen == 0) {
        return -1;
    }
    *outLen = totalLen;
    if (outField != NULL) {
        *outField = lastField;
    }
    return 0;
}

int ResolveEmbeddedPathByNames(
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

int CollectFnAndFieldInfoFromNode(SLCBackendC* c, int32_t nodeId) {
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
        char**     paramNames = NULL;
        uint8_t*   paramFlags = NULL;
        uint32_t   paramLen = 0;
        uint32_t   paramTypeCap = 0;
        uint32_t   paramNameCap = 0;
        uint32_t   paramFlagCap = 0;
        SLTypeRef  contextType;
        int        isVariadic = 0;
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
                if ((ch->flags & SLAstFlag_PARAM_VARIADIC) != 0) {
                    if (isVariadic) {
                        return -1;
                    }
                    if (paramType.containerKind != SLTypeContainer_SCALAR) {
                        return -1;
                    }
                    paramType.containerKind = SLTypeContainer_SLICE_RO;
                    paramType.containerPtrDepth = 0;
                    paramType.hasArrayLen = 0;
                    paramType.arrayLen = 0;
                    paramType.readOnly = 1;
                    isVariadic = 1;
                } else if (isVariadic) {
                    /* Variadic parameter must be the final parameter. */
                    return -1;
                }
                if (paramLen >= paramTypeCap) {
                    uint32_t need = paramLen + 1u;
                    if (EnsureCapArena(
                            &c->arena,
                            (void**)&paramTypes,
                            &paramTypeCap,
                            need,
                            sizeof(SLTypeRef),
                            (uint32_t)_Alignof(SLTypeRef))
                        != 0)
                    {
                        return -1;
                    }
                    if (EnsureCapArena(
                            &c->arena,
                            (void**)&paramNames,
                            &paramNameCap,
                            need,
                            sizeof(char*),
                            (uint32_t)_Alignof(char*))
                        != 0)
                    {
                        return -1;
                    }
                    if (EnsureCapArena(
                            &c->arena,
                            (void**)&paramFlags,
                            &paramFlagCap,
                            need,
                            sizeof(uint8_t),
                            (uint32_t)_Alignof(uint8_t))
                        != 0)
                    {
                        return -1;
                    }
                }
                paramTypes[paramLen++] = paramType;
                paramNames[paramLen - 1u] = DupSlice(
                    c, c->unit->source, ch->dataStart, ch->dataEnd);
                if (paramNames[paramLen - 1u] == NULL) {
                    return -1;
                }
                paramFlags[paramLen - 1u] =
                    (uint8_t)(((ch->flags & SLAstFlag_PARAM_CONST) != 0)
                                  ? SLCCGParamFlag_CONST
                                  : 0u);
            } else if (
                ch != NULL
                && (ch->kind == SLAst_TYPE_NAME || ch->kind == SLAst_TYPE_PTR
                    || ch->kind == SLAst_TYPE_REF || ch->kind == SLAst_TYPE_MUTREF
                    || ch->kind == SLAst_TYPE_ARRAY || ch->kind == SLAst_TYPE_VARRAY
                    || ch->kind == SLAst_TYPE_SLICE || ch->kind == SLAst_TYPE_MUTSLICE
                    || ch->kind == SLAst_TYPE_OPTIONAL || ch->kind == SLAst_TYPE_FN
                    || ch->kind == SLAst_TYPE_ANON_STRUCT || ch->kind == SLAst_TYPE_ANON_UNION
                    || ch->kind == SLAst_TYPE_TUPLE)
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
            paramNames,
            paramFlags,
            paramLen,
            isVariadic,
            hasContext,
            contextType);
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

int CollectFnAndFieldInfo(SLCBackendC* c) {
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

int CollectTypeAliasInfo(SLCBackendC* c) {
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

int CollectFnTypeAliasesFromNode(SLCBackendC* c, int32_t nodeId) {
    const SLAstNode* n = NodeAt(c, nodeId);
    int32_t          child;
    if (n == NULL) {
        return 0;
    }
    if (n->kind == SLAst_TYPE_FN) {
        SLTypeRef ignoredType;
        if (ParseTypeRef(c, nodeId, &ignoredType) != 0) {
            SetDiagNode(c, nodeId, SLDiag_CODEGEN_INTERNAL);
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

int CollectFnTypeAliases(SLCBackendC* c) {
    return CollectFnTypeAliasesFromNode(c, c->ast.root);
}

int AddVarSizeType(SLCBackendC* c, const char* cName, int isUnion) {
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

SLVarSizeType* _Nullable FindVarSizeType(SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->varSizeTypeLen; i++) {
        if (StrEq(c->varSizeTypes[i].cName, cName)) {
            return &c->varSizeTypes[i];
        }
    }
    return NULL;
}

int CollectVarSizeTypesFromDeclSets(SLCBackendC* c) {
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

int IsVarSizeTypeName(const SLCBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->varSizeTypeLen; i++) {
        if (StrEq(c->varSizeTypes[i].cName, cName)) {
            return c->varSizeTypes[i].isVarSize;
        }
    }
    return 0;
}

int PropagateVarSizeTypes(SLCBackendC* c) {
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

int PushScope(SLCBackendC* c) {
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

void TrimVariantNarrowsToLocalLen(SLCBackendC* c);

void PopScope(SLCBackendC* c) {
    if (c->localScopeLen == 0) {
        c->localLen = 0;
        c->localAnonTypedefLen = 0;
        c->variantNarrowLen = 0;
        return;
    }
    c->localScopeLen--;
    c->localLen = c->localScopeMarks[c->localScopeLen];
    TrimVariantNarrowsToLocalLen(c);
    if (c->localAnonTypedefScopeLen > 0) {
        c->localAnonTypedefScopeLen--;
        c->localAnonTypedefLen = c->localAnonTypedefScopeMarks[c->localAnonTypedefScopeLen];
    } else {
        c->localAnonTypedefLen = 0;
    }
}

int PushDeferScope(SLCBackendC* c) {
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

void PopDeferScope(SLCBackendC* c) {
    if (c->deferScopeLen == 0) {
        c->deferredStmtLen = 0;
        return;
    }
    c->deferScopeLen--;
    c->deferredStmtLen = c->deferScopeMarks[c->deferScopeLen];
}

int AddDeferredStmt(SLCBackendC* c, int32_t stmtNodeId) {
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

int AddLocal(SLCBackendC* c, const char* name, SLTypeRef type) {
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

void TrimVariantNarrowsToLocalLen(SLCBackendC* c) {
    while (c->variantNarrowLen > 0) {
        if (c->variantNarrows[c->variantNarrowLen - 1u].localIdx < (int32_t)c->localLen) {
            break;
        }
        c->variantNarrowLen--;
    }
}

int AddVariantNarrow(
    SLCBackendC* c,
    int32_t      localIdx,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->variantNarrows,
            &c->variantNarrowCap,
            c->variantNarrowLen + 1u,
            sizeof(SLVariantNarrow),
            (uint32_t)_Alignof(SLVariantNarrow))
        != 0)
    {
        return -1;
    }
    c->variantNarrows[c->variantNarrowLen].localIdx = localIdx;
    c->variantNarrows[c->variantNarrowLen].enumTypeName = (char*)enumTypeName;
    c->variantNarrows[c->variantNarrowLen].variantStart = variantStart;
    c->variantNarrows[c->variantNarrowLen].variantEnd = variantEnd;
    c->variantNarrowLen++;
    return 0;
}

int32_t FindLocalIndexBySlice(const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (SliceEqName(c->unit->source, start, end, c->locals[i].name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

const SLVariantNarrow* _Nullable FindVariantNarrowByLocalIdx(
    const SLCBackendC* c, int32_t localIdx) {
    uint32_t i = c->variantNarrowLen;
    while (i > 0) {
        i--;
        if (c->variantNarrows[i].localIdx == localIdx) {
            return &c->variantNarrows[i];
        }
    }
    return NULL;
}

const SLLocal* _Nullable FindLocalBySlice(const SLCBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (SliceEqName(c->unit->source, start, end, c->locals[i].name)) {
            return &c->locals[i];
        }
    }
    return NULL;
}

int FindEnumDeclNodeBySlice(
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

int EnumDeclHasMemberBySlice(
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
                || firstChild->kind == SLAst_TYPE_ANON_UNION
                || firstChild->kind == SLAst_TYPE_TUPLE))
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

int EnumDeclHasPayload(const SLCBackendC* c, int32_t enumNodeId);

int ResolveEnumSelectorByFieldExpr(
    const SLCBackendC* c,
    int32_t            fieldExprNode,
    const SLNameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd) {
    const SLAstNode* n = NodeAt(c, fieldExprNode);
    int32_t          recvNode;
    const SLAstNode* recv;
    const SLLocal*   local;
    const SLNameMap* map;
    int32_t          enumDeclNode;

    if (outEnumMap != NULL) {
        *outEnumMap = NULL;
    }
    if (outEnumDeclNode != NULL) {
        *outEnumDeclNode = -1;
    }
    if (outEnumHasPayload != NULL) {
        *outEnumHasPayload = 0;
    }
    if (outVariantStart != NULL) {
        *outVariantStart = 0;
    }
    if (outVariantEnd != NULL) {
        *outVariantEnd = 0;
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
    if (outEnumDeclNode != NULL) {
        *outEnumDeclNode = enumDeclNode;
    }
    if (outEnumHasPayload != NULL) {
        *outEnumHasPayload = EnumDeclHasPayload(c, enumDeclNode);
    }
    if (outVariantStart != NULL) {
        *outVariantStart = n->dataStart;
    }
    if (outVariantEnd != NULL) {
        *outVariantEnd = n->dataEnd;
    }
    return 1;
}

int EnumDeclHasPayload(const SLCBackendC* c, int32_t enumNodeId) {
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
                || firstChild->kind == SLAst_TYPE_ANON_UNION
                || firstChild->kind == SLAst_TYPE_TUPLE))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }
    while (child >= 0) {
        int32_t payload = AstFirstChild(&c->ast, child);
        while (payload >= 0) {
            if (NodeAt(c, payload) != NULL && NodeAt(c, payload)->kind == SLAst_FIELD) {
                return 1;
            }
            payload = AstNextSibling(&c->ast, payload);
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int32_t EnumVariantTagExprNode(const SLCBackendC* c, int32_t variantNode) {
    int32_t child = AstFirstChild(&c->ast, variantNode);
    while (child >= 0) {
        const SLAstNode* n = NodeAt(c, child);
        if (n != NULL && n->kind != SLAst_FIELD) {
            return child;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return -1;
}

int FindEnumDeclNodeByCName(const SLCBackendC* c, const char* enumCName, int32_t* outNodeId) {
    uint32_t i;
    if (enumCName == NULL || outNodeId == NULL) {
        return -1;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const SLAstNode* n = NodeAt(c, nodeId);
        const SLNameMap* map;
        if (n == NULL || n->kind != SLAst_ENUM) {
            continue;
        }
        map = FindNameBySlice(c, n->dataStart, n->dataEnd);
        if (map != NULL && StrEq(map->cName, enumCName)) {
            *outNodeId = nodeId;
            return 0;
        }
    }
    return -1;
}

int FindEnumVariantNodeBySlice(
    const SLCBackendC* c,
    int32_t            enumNodeId,
    uint32_t           variantStart,
    uint32_t           variantEnd,
    int32_t*           outVariantNode) {
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
                || firstChild->kind == SLAst_TYPE_ANON_UNION
                || firstChild->kind == SLAst_TYPE_TUPLE))
        {
            child = AstNextSibling(&c->ast, child);
        }
    }
    while (child >= 0) {
        const SLAstNode* v = NodeAt(c, child);
        if (v != NULL && v->kind == SLAst_FIELD
            && SliceSpanEq(c->unit->source, v->dataStart, v->dataEnd, variantStart, variantEnd))
        {
            *outVariantNode = child;
            return 0;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return -1;
}

int ResolveEnumVariantPayloadFieldType(
    SLCBackendC* c,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd,
    uint32_t     fieldStart,
    uint32_t     fieldEnd,
    SLTypeRef*   outType) {
    int32_t enumNodeId;
    int32_t variantNode;
    int32_t payload = -1;
    if (FindEnumDeclNodeByCName(c, enumTypeName, &enumNodeId) != 0) {
        return -1;
    }
    if (FindEnumVariantNodeBySlice(c, enumNodeId, variantStart, variantEnd, &variantNode) != 0) {
        return -1;
    }
    payload = AstFirstChild(&c->ast, variantNode);
    while (payload >= 0) {
        const SLAstNode* f = NodeAt(c, payload);
        int32_t          typeNode;
        if (f == NULL || f->kind != SLAst_FIELD) {
            break;
        }
        if (!SliceSpanEq(c->unit->source, f->dataStart, f->dataEnd, fieldStart, fieldEnd)) {
            payload = AstNextSibling(&c->ast, payload);
            continue;
        }
        typeNode = AstFirstChild(&c->ast, payload);
        if (typeNode < 0 || ParseTypeRef(c, typeNode, outType) != 0) {
            return -1;
        }
        CanonicalizeTypeRefBaseName(c, outType);
        return 0;
    }
    return -1;
}

/* Returns 1 on success, 0 if node is not enum.variant syntax, -1 on error. */
int ResolveEnumVariantTypeNameNode(
    const SLCBackendC* c,
    int32_t            typeNode,
    const char**       outEnumCName,
    uint32_t*          outVariantStart,
    uint32_t*          outVariantEnd) {
    const SLAstNode* typeNameNode = NodeAt(c, typeNode);
    uint32_t         dotPos;
    const SLNameMap* enumMap;
    int32_t          enumDeclNode;
    if (typeNameNode == NULL || typeNameNode->kind != SLAst_TYPE_NAME) {
        return 0;
    }
    dotPos = typeNameNode->dataEnd;
    while (dotPos > typeNameNode->dataStart) {
        dotPos--;
        if (c->unit->source[dotPos] == '.') {
            break;
        }
    }
    if (dotPos <= typeNameNode->dataStart || c->unit->source[dotPos] != '.'
        || dotPos + 1u >= typeNameNode->dataEnd)
    {
        return 0;
    }
    enumMap = FindNameBySlice(c, typeNameNode->dataStart, dotPos);
    if (enumMap == NULL || enumMap->kind != SLAst_ENUM) {
        return 0;
    }
    if (FindEnumDeclNodeBySlice(c, typeNameNode->dataStart, dotPos, &enumDeclNode) != 0) {
        return -1;
    }
    if (!EnumDeclHasMemberBySlice(c, enumDeclNode, dotPos + 1u, typeNameNode->dataEnd)) {
        return -1;
    }
    *outEnumCName = enumMap->cName;
    *outVariantStart = dotPos + 1u;
    *outVariantEnd = typeNameNode->dataEnd;
    return 1;
}

int CasePatternParts(
    const SLCBackendC* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode) {
    const SLAstNode* n = NodeAt(c, caseLabelNode);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == SLAst_CASE_PATTERN) {
        int32_t expr = AstFirstChild(&c->ast, caseLabelNode);
        int32_t alias = expr >= 0 ? AstNextSibling(&c->ast, expr) : -1;
        if (expr < 0) {
            return -1;
        }
        *outExprNode = expr;
        *outAliasNode = alias;
        return 0;
    }
    *outExprNode = caseLabelNode;
    *outAliasNode = -1;
    return 0;
}

/* Returns 1 for enum variant selector syntax (Enum.Variant), 0 otherwise, -1 on error. */
int DecodeEnumVariantPatternExpr(
    const SLCBackendC* c,
    int32_t            exprNode,
    const SLNameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd) {
    const SLAstNode* n = NodeAt(c, exprNode);
    if (outEnumMap != NULL) {
        *outEnumMap = NULL;
    }
    if (outEnumDeclNode != NULL) {
        *outEnumDeclNode = -1;
    }
    if (outEnumHasPayload != NULL) {
        *outEnumHasPayload = 0;
    }
    if (outVariantStart != NULL) {
        *outVariantStart = 0;
    }
    if (outVariantEnd != NULL) {
        *outVariantEnd = 0;
    }
    if (n == NULL || n->kind != SLAst_FIELD_EXPR) {
        return 0;
    }
    return ResolveEnumSelectorByFieldExpr(
        c,
        exprNode,
        outEnumMap,
        outEnumDeclNode,
        outEnumHasPayload,
        outVariantStart,
        outVariantEnd);
}

int ResolvePayloadEnumType(const SLCBackendC* c, const SLTypeRef* t, const char** outEnumName) {
    const char*      baseName;
    const SLNameMap* map;
    int32_t          enumNodeId;
    if (outEnumName != NULL) {
        *outEnumName = NULL;
    }
    if (t == NULL || !t->valid || t->containerKind != SLTypeContainer_SCALAR
        || t->containerPtrDepth != 0 || t->ptrDepth != 0 || t->baseName == NULL)
    {
        return 0;
    }
    baseName = ResolveScalarAliasBaseName(c, t->baseName);
    if (baseName == NULL) {
        baseName = t->baseName;
    }
    map = FindNameByCName(c, baseName);
    if (map == NULL || map->kind != SLAst_ENUM) {
        return 0;
    }
    if (FindEnumDeclNodeByCName(c, baseName, &enumNodeId) != 0
        || !EnumDeclHasPayload(c, enumNodeId))
    {
        return 0;
    }
    if (outEnumName != NULL) {
        *outEnumName = baseName;
    }
    return 1;
}

int AppendMappedIdentifier(SLCBackendC* c, uint32_t start, uint32_t end) {
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

int EmitTypeNameWithDepth(SLCBackendC* c, const SLTypeRef* type) {
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

int EmitTypeWithName(SLCBackendC* c, int32_t typeNode, const char* name) {
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

int EmitTypeRefWithName(SLCBackendC* c, const SLTypeRef* t, const char* name);

int EmitAnonInlineTypeWithName(
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

int EmitTypeRefWithName(SLCBackendC* c, const SLTypeRef* t, const char* name) {
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

int EmitTypeForCast(SLCBackendC* c, int32_t typeNode) {
    SLTypeRef t;
    if (ParseTypeRef(c, typeNode, &t) != 0) {
        return -1;
    }
    return EmitTypeNameWithDepth(c, &t);
}

int InferExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType);
int InferExprTypeExpected(
    SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType, SLTypeRef* outType);
int EmitExpr(SLCBackendC* c, int32_t nodeId);
int EmitCompoundLiteral(SLCBackendC* c, int32_t nodeId, const SLTypeRef* _Nullable expectedType);
int EmitCompoundLiteralOrderedStruct(
    SLCBackendC* c, int32_t firstField, const char* ownerType, const SLTypeRef* valueType);
int EmitEffectiveContextFieldValue(
    SLCBackendC* c, const char* fieldName, const SLTypeRef* requiredType);
int InferNewExprType(SLCBackendC* c, int32_t nodeId, SLTypeRef* outType);
int EmitExprCoerced(SLCBackendC* c, int32_t exprNode, const SLTypeRef* dstType);
int EmitCompoundFieldValueCoerced(
    SLCBackendC* c, const SLAstNode* field, int32_t exprNode, const SLTypeRef* _Nullable dstType);
int EmitContextArgForSig(SLCBackendC* c, const SLFnSig* sig);
int StructHasFieldDefaults(const SLCBackendC* c, const char* ownerType);
int TypeRefAssignableCost(
    SLCBackendC* c, const SLTypeRef* dst, const SLTypeRef* src, uint8_t* outCost);

void SetPreferredAllocatorPtrType(SLTypeRef* outType) {
    TypeRefSetScalar(outType, "core__Allocator");
    outType->ptrDepth = 1;
}

SL_API_END
