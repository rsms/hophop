#include "internal.h"
#include "../typecheck/internal.h"

H2_API_BEGIN
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

void SetDiag(H2Diag* _Nullable diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = H2DiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

int EnsureCapArena(
    H2Arena* arena, void** ptr, uint32_t* cap, uint32_t need, size_t elemSize, uint32_t align) {
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
    newPtr = H2ArenaAlloc(arena, allocSize, align);
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

int BufReserve(H2Buf* b, uint32_t extra) {
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

int BufAppend(H2Buf* b, const char* s, uint32_t len) {
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

int BufAppendCStr(H2Buf* b, const char* s) {
    return BufAppend(b, s, (uint32_t)StrLen(s));
}

int BufAppendChar(H2Buf* b, char c) {
    return BufAppend(b, &c, 1u);
}

int BufAppendU32(H2Buf* b, uint32_t value) {
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

int BufAppendSlice(H2Buf* b, const char* src, uint32_t start, uint32_t end) {
    if (end < start) {
        return -1;
    }
    return BufAppend(b, src + start, end - start);
}

char* _Nullable BufFinish(H2Buf* b) {
    char* out;
    if (b->v == NULL) {
        if (b->arena == NULL) {
            return NULL;
        }
        out = (char*)H2ArenaAlloc(b->arena, 1u, (uint32_t)_Alignof(char));
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

void EmitIndent(H2CBackendC* c, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        (void)BufAppendCStr(&c->out, "    ");
    }
}

int IsBuiltinType(const char* s) {
    return StrEq(s, "bool") || StrEq(s, "str") || StrEq(s, "u8") || StrEq(s, "u16")
        || StrEq(s, "u32") || StrEq(s, "u64") || StrEq(s, "i8") || StrEq(s, "i16")
        || StrEq(s, "i32") || StrEq(s, "i64") || StrEq(s, "uint") || StrEq(s, "int")
        || StrEq(s, "rawptr") || StrEq(s, "f32") || StrEq(s, "f64") || StrEq(s, "const_int")
        || StrEq(s, "const_float") || StrEq(s, "type") || StrEq(s, "anytype");
}

int IsIntegerCTypeName(const char* s) {
    return StrEq(s, "__hop_u8") || StrEq(s, "__hop_u16") || StrEq(s, "__hop_u32")
        || StrEq(s, "__hop_u64") || StrEq(s, "__hop_uint") || StrEq(s, "__hop_i8")
        || StrEq(s, "__hop_i16") || StrEq(s, "__hop_i32") || StrEq(s, "__hop_i64")
        || StrEq(s, "__hop_int");
}

int IsFloatCTypeName(const char* s) {
    return StrEq(s, "__hop_f32") || StrEq(s, "__hop_f64");
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

void TypeRefSetInvalid(H2TypeRef* t) {
    t->baseName = NULL;
    t->ptrDepth = 0;
    t->valid = 0;
    t->containerKind = H2TypeContainer_SCALAR;
    t->containerPtrDepth = 0;
    t->arrayLen = 0;
    t->hasArrayLen = 0;
    t->readOnly = 0;
    t->isOptional = 0;
}

void TypeRefSetScalar(H2TypeRef* t, const char* baseName) {
    t->baseName = baseName;
    t->ptrDepth = 0;
    t->valid = 1;
    t->containerKind = H2TypeContainer_SCALAR;
    t->containerPtrDepth = 0;
    t->arrayLen = 0;
    t->hasArrayLen = 0;
    t->readOnly = 0;
    t->isOptional = 0;
}

int EnsureAnonTypeByFields(
    H2CBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const H2TypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName);

static int TypeRefOptionalPayloadType(const H2TypeRef* optionalType, H2TypeRef* outPayload) {
    if (optionalType == NULL || outPayload == NULL || !optionalType->valid
        || !optionalType->isOptional)
    {
        return 0;
    }
    *outPayload = *optionalType;
    outPayload->isOptional = 0;
    return 1;
}

static int TypeRefOptionalPayloadUsesNullSentinel(const H2TypeRef* payload) {
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

int TypeRefIsPointerBackedOptional(const H2TypeRef* t) {
    H2TypeRef payload;
    return TypeRefOptionalPayloadType(t, &payload)
        && TypeRefOptionalPayloadUsesNullSentinel(&payload);
}

int TypeRefIsTaggedOptional(const H2TypeRef* t) {
    H2TypeRef payload;
    return TypeRefOptionalPayloadType(t, &payload)
        && !TypeRefOptionalPayloadUsesNullSentinel(&payload);
}

int TypeRefLowerForStorage(H2CBackendC* c, const H2TypeRef* type, H2TypeRef* outType) {
    static const char* fieldNames[2] = { "__hop_tag", "__hop_value" };
    H2TypeRef          fieldTypes[2];
    H2TypeRef          payload;
    const char*        anonName = NULL;
    if (c == NULL || type == NULL || outType == NULL) {
        return -1;
    }
    *outType = *type;
    if (!TypeRefIsTaggedOptional(type)) {
        return 0;
    }
    payload = *type;
    payload.isOptional = 0;
    TypeRefSetScalar(&fieldTypes[0], "__hop_u8");
    fieldTypes[1] = payload;
    if (EnsureAnonTypeByFields(c, 0, fieldNames, fieldTypes, 2, &anonName) != 0 || anonName == NULL)
    {
        return -1;
    }
    TypeRefSetScalar(outType, anonName);
    return 0;
}

const char* ResolveScalarAliasBaseName(const H2CBackendC* c, const char* typeName);

void CanonicalizeTypeRefBaseName(const H2CBackendC* c, H2TypeRef* t) {
    const char* canonical;
    if (t == NULL || !t->valid || t->baseName == NULL) {
        return;
    }
    canonical = ResolveScalarAliasBaseName(c, t->baseName);
    if (canonical != NULL) {
        t->baseName = canonical;
    }
}

int TypeRefEqual(const H2TypeRef* a, const H2TypeRef* b);
int AddFieldInfo(
    H2CBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int32_t   defaultExprNode,
    int       isDependent,
    int       isEmbedded,
    H2TypeRef type);
const H2AnonTypeInfo* _Nullable FindAnonTypeByCName(const H2CBackendC* c, const char* cName);
int EnsureAnonTypeByFields(
    H2CBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const H2TypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName);
int EnsureAnonTypeVisible(H2CBackendC* c, const H2TypeRef* type, uint32_t depth);
int EmitTypeRefWithName(H2CBackendC* c, const H2TypeRef* t, const char* name);
int EmitTypeNameWithDepth(H2CBackendC* c, const H2TypeRef* type);
int TypeRefIsPointerLike(const H2TypeRef* t);
int ResolveTopLevelConstTypeValueBySlice(
    H2CBackendC* c, uint32_t start, uint32_t end, H2TypeRef* outType);
int EmitDeclNode(
    H2CBackendC* c,
    int32_t      nodeId,
    uint32_t     depth,
    int          declarationOnly,
    int          isPrivate,
    int          emitBody);
int IsStrBaseName(const char* _Nullable s);
int ShouldEmitDeclNode(const H2CBackendC* c, int32_t nodeId);

int SliceStructPtrDepth(const H2TypeRef* t) {
    int stars = t->ptrDepth;
    if (t->containerPtrDepth > 0) {
        stars += t->containerPtrDepth - 1;
    }
    return stars;
}

static int TypeRefIsBorrowedStrValueC(const H2TypeRef* t) {
    return t != NULL && t->valid && t->containerKind == H2TypeContainer_SCALAR
        && t->containerPtrDepth == 0 && t->ptrDepth == 1 && t->readOnly != 0
        && IsStrBaseName(t->baseName);
}

static int TypeRefIsPointerBackedStrC(const H2TypeRef* t) {
    return t != NULL && t->valid && t->containerKind == H2TypeContainer_SCALAR
        && t->containerPtrDepth == 0 && IsStrBaseName(t->baseName)
        && !TypeRefIsBorrowedStrValueC(t);
}

const H2AstNode* _Nullable NodeAt(const H2CBackendC* c, int32_t nodeId);

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

void SetDiagNode(H2CBackendC* c, int32_t nodeId, H2DiagCode code) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n != NULL) {
        SetDiag(c->diag, code, n->start, n->end);
    } else {
        SetDiag(c->diag, code, 0, 0);
    }
}

int BufAppendI64(H2Buf* b, int64_t value) {
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

int EvalConstIntExpr(H2CBackendC* c, int32_t nodeId, int64_t* outValue, int* outIsConst) {
    if (outIsConst == NULL || outValue == NULL) {
        return -1;
    }
    *outIsConst = 0;
    if (c->constEval == NULL) {
        return 0;
    }
    return H2ConstEvalSessionEvalIntExpr(c->constEval, nodeId, outValue, outIsConst);
}

int EvalConstFloatExpr(H2CBackendC* c, int32_t nodeId, double* outValue, int* outIsConst) {
    H2CTFEValue value = { 0 };
    int         isConst = 0;
    if (outIsConst == NULL || outValue == NULL) {
        return -1;
    }
    *outIsConst = 0;
    if (c->constEval == NULL) {
        return 0;
    }
    if (H2ConstEvalSessionEvalExpr(c->constEval, nodeId, &value, &isConst) != 0 || !isConst) {
        return 0;
    }
    if (value.kind == H2CTFEValue_FLOAT) {
        *outValue = value.f64;
        *outIsConst = 1;
        return 0;
    }
    if (value.kind == H2CTFEValue_INT) {
        *outValue = (double)value.i64;
        *outIsConst = 1;
        return 0;
    }
    return 0;
}

int ConstIntFitsIntegerType(const char* typeName, int64_t value) {
    if (typeName == NULL) {
        return 0;
    }
    if (StrEq(typeName, "__hop_u8")) {
        return value >= 0 && value <= (int64_t)UINT8_MAX;
    }
    if (StrEq(typeName, "__hop_u16")) {
        return value >= 0 && value <= (int64_t)UINT16_MAX;
    }
    if (StrEq(typeName, "__hop_u32")) {
        return value >= 0 && value <= (int64_t)UINT32_MAX;
    }
    if (StrEq(typeName, "__hop_u64") || StrEq(typeName, "__hop_uint")) {
        return value >= 0;
    }
    if (StrEq(typeName, "__hop_i8")) {
        return value >= (int64_t)INT8_MIN && value <= (int64_t)INT8_MAX;
    }
    if (StrEq(typeName, "__hop_i16")) {
        return value >= (int64_t)INT16_MIN && value <= (int64_t)INT16_MAX;
    }
    if (StrEq(typeName, "__hop_i32")) {
        return value >= (int64_t)INT32_MIN && value <= (int64_t)INT32_MAX;
    }
    if (StrEq(typeName, "__hop_i64") || StrEq(typeName, "__hop_int")) {
        return 1;
    }
    return 0;
}

uint32_t ConstIntBitLen(uint64_t v) {
    uint32_t bits = 0;
    while (v != 0u) {
        bits++;
        v >>= 1u;
    }
    return bits;
}

int ConstIntFitsFloatType(const char* typeName, int64_t value) {
    uint32_t precisionBits;
    uint64_t magnitude;
    if (typeName == NULL) {
        return 0;
    }
    if (StrEq(typeName, "__hop_f32")) {
        precisionBits = 23u;
    } else if (StrEq(typeName, "__hop_f64")) {
        precisionBits = 53u;
    } else {
        return 0;
    }
    if (value < 0) {
        magnitude = (uint64_t)(-(value + 1)) + 1u;
    } else {
        magnitude = (uint64_t)value;
    }
    return ConstIntBitLen(magnitude) <= precisionBits;
}

int ConstFloatFitsFloatType(const char* typeName, double value) {
    if (typeName == NULL) {
        return 0;
    }
    if (StrEq(typeName, "__hop_f64")) {
        return 1;
    }
    if (!StrEq(typeName, "__hop_f32")) {
        return 0;
    }
    if (value != value) {
        return 1;
    }
    return (double)(float)value == value;
}

int EmitConstEvaluatedScalar(
    H2CBackendC* c, const H2TypeRef* dstType, const H2CTFEValue* value, int* outEmitted) {
    if (outEmitted == NULL || c == NULL || dstType == NULL || value == NULL) {
        return -1;
    }
    *outEmitted = 0;
    if (!dstType->valid || dstType->containerKind != H2TypeContainer_SCALAR
        || dstType->containerPtrDepth != 0 || dstType->isOptional)
    {
        return 0;
    }
    switch (value->kind) {
        case H2CTFEValue_INT:
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(") != 0 || BufAppendI64(&c->out, value->i64) != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            *outEmitted = 1;
            return 0;
        case H2CTFEValue_BOOL:
            if (BufAppendCStr(&c->out, "((") != 0 || EmitTypeNameWithDepth(c, dstType) != 0
                || BufAppendCStr(&c->out, ")(") != 0
                || BufAppendChar(&c->out, value->b ? '1' : '0') != 0
                || BufAppendCStr(&c->out, "))") != 0)
            {
                return -1;
            }
            *outEmitted = 1;
            return 0;
        case H2CTFEValue_NULL:
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
        case H2CTFEValue_TYPE:
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

char* _Nullable DupSlice(H2CBackendC* c, const char* src, uint32_t start, uint32_t end) {
    uint32_t len;
    char*    out;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    out = (char*)H2ArenaAlloc(&c->arena, len + 1u, (uint32_t)_Alignof(char));
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
    H2CBackendC* c, const H2AstNode* paramNode, uint32_t paramIndex) {
    if (paramNode == NULL) {
        return NULL;
    }
    if (SliceIsHoleName(c->unit->source, paramNode->dataStart, paramNode->dataEnd)) {
        H2Buf b = { 0 };
        b.arena = &c->arena;
        if (BufAppendCStr(&b, "__hop_v") != 0 || BufAppendU32(&b, paramIndex) != 0) {
            return NULL;
        }
        return BufFinish(&b);
    }
    return DupSlice(c, c->unit->source, paramNode->dataStart, paramNode->dataEnd);
}

char* _Nullable DupAndReplaceDots(H2CBackendC* c, const char* src, uint32_t start, uint32_t end) {
    char*    out;
    uint32_t i;
    uint32_t len;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    out = (char*)H2ArenaAlloc(&c->arena, len + 1u, (uint32_t)_Alignof(char));
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

static char* _Nullable DupAndReplaceDotsDouble(
    H2CBackendC* c, const char* src, uint32_t start, uint32_t end) {
    char*    out;
    uint32_t i;
    uint32_t len;
    uint32_t dotCount = 0;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    for (i = 0; i < len; i++) {
        if (src[start + i] == '.') {
            dotCount++;
        }
    }
    out = (char*)H2ArenaAlloc(&c->arena, len + dotCount + 1u, (uint32_t)_Alignof(char));
    if (out == NULL) {
        return NULL;
    }
    {
        uint32_t j = 0;
        for (i = 0; i < len; i++) {
            char ch = src[start + i];
            if (ch == '.') {
                out[j++] = '_';
                out[j++] = '_';
            } else {
                out[j++] = ch;
            }
        }
        out[j] = '\0';
    }
    return out;
}

char* _Nullable DupCStr(H2CBackendC* c, const char* s) {
    size_t n = StrLen(s);
    char*  out;
    if (n > UINT32_MAX - 1u) {
        return NULL;
    }
    out = (char*)H2ArenaAlloc(&c->arena, (uint32_t)n + 1u, (uint32_t)_Alignof(char));
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1u);
    return out;
}

int32_t AstFirstChild(const H2Ast* ast, int32_t nodeId);
int32_t AstNextSibling(const H2Ast* ast, int32_t nodeId);
const H2AstNode* _Nullable NodeAt(const H2CBackendC* c, int32_t nodeId);

int DecodeStringLiteralNode(
    H2CBackendC* c, const H2AstNode* n, uint8_t** outBytes, uint32_t* outLen) {
    H2StringLitErr litErr = { 0 };
    if (n == NULL || n->kind != H2Ast_STRING) {
        return -1;
    }
    if (H2DecodeStringLiteralArena(
            &c->arena, c->unit->source, n->dataStart, n->dataEnd, outBytes, outLen, &litErr)
        != 0)
    {
        SetDiag(c->diag, H2StringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
        return -1;
    }
    return 0;
}

int AppendDecodedStringExpr(
    H2CBackendC* c, int32_t nodeId, uint8_t** bytes, uint32_t* len, uint32_t* cap) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == H2Ast_STRING) {
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
                SetDiag(c->diag, H2Diag_ARENA_OOM, n->start, n->end);
                return -1;
            }
            memcpy(*bytes + *len, part, partLen);
            *len += partLen;
        }
        return 0;
    }
    if (n->kind == H2Ast_BINARY && (H2TokenKind)n->op == H2Tok_ADD
        && H2IsStringLiteralConcatChain(&c->ast, nodeId))
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
    H2CBackendC* c,
    int32_t      nodeId,
    uint8_t**    outBytes,
    uint32_t*    outLen,
    uint32_t*    outStart,
    uint32_t*    outEnd) {
    uint8_t*         bytes = NULL;
    uint32_t         len = 0;
    uint32_t         cap = 0;
    const H2AstNode* n = NodeAt(c, nodeId);
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

int GetOrAddStringLiteralExpr(H2CBackendC* c, int32_t nodeId, int32_t* outLiteralId) {
    uint8_t* decoded = NULL;
    uint32_t decodedLen = 0;
    uint32_t spanStart = 0;
    uint32_t spanEnd = 0;
    uint32_t i;

    if (DecodeStringExpr(c, nodeId, &decoded, &decodedLen, &spanStart, &spanEnd) != 0) {
        if (c->diag != NULL && c->diag->code == H2Diag_NONE) {
            SetDiag(c->diag, H2Diag_CODEGEN_INTERNAL, spanStart, spanEnd);
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
            sizeof(H2StringLiteral),
            (uint32_t)_Alignof(H2StringLiteral))
        != 0)
    {
        SetDiag(c->diag, H2Diag_ARENA_OOM, spanStart, spanEnd);
        return -1;
    }

    c->stringLits[c->stringLitLen].bytes = decoded;
    c->stringLits[c->stringLitLen].len = decodedLen;
    *outLiteralId = (int32_t)c->stringLitLen;
    c->stringLitLen++;
    return 0;
}

int GetOrAddStringLiteralBytes(
    H2CBackendC* c, const uint8_t* bytes, uint32_t len, int32_t* outLiteralId) {
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
        copied = (uint8_t*)H2ArenaAlloc(&c->arena, len, (uint32_t)_Alignof(uint8_t));
        if (copied == NULL) {
            SetDiag(c->diag, H2Diag_ARENA_OOM, 0, 0);
            return -1;
        }
        memcpy(copied, bytes, (size_t)len);
    }
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->stringLits,
            &c->stringLitCap,
            c->stringLitLen + 1u,
            sizeof(H2StringLiteral),
            (uint32_t)_Alignof(H2StringLiteral))
        != 0)
    {
        SetDiag(c->diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }
    c->stringLits[c->stringLitLen].bytes = copied;
    c->stringLits[c->stringLitLen].len = len;
    *outLiteralId = (int32_t)c->stringLitLen;
    c->stringLitLen++;
    return 0;
}

int CollectStringLiterals(H2CBackendC* c) {
    uint32_t nodeId;

    c->stringLitByNodeLen = c->ast.len;
    c->stringLitByNode = (int32_t*)H2ArenaAlloc(
        &c->arena, c->stringLitByNodeLen * (uint32_t)sizeof(int32_t), (uint32_t)_Alignof(int32_t));
    if (c->stringLitByNode == NULL) {
        return -1;
    }
    for (nodeId = 0; nodeId < c->stringLitByNodeLen; nodeId++) {
        c->stringLitByNode[nodeId] = -1;
    }

    for (nodeId = 0; nodeId < c->ast.len; nodeId++) {
        const H2AstNode* n = &c->ast.nodes[nodeId];
        int              shouldCollect = 0;
        if (n->kind == H2Ast_STRING) {
            shouldCollect = 1;
        } else if (
            n->kind == H2Ast_BINARY && (H2TokenKind)n->op == H2Tok_ADD
            && H2IsStringLiteralConcatChain(&c->ast, (int32_t)nodeId))
        {
            shouldCollect = 1;
        }
        if (shouldCollect) {
            uint32_t scanNodeId;
            int      skip = 0;
            for (scanNodeId = 0; scanNodeId < c->ast.len; scanNodeId++) {
                const H2AstNode* parent = &c->ast.nodes[scanNodeId];
                if (parent->kind == H2Ast_ASSERT) {
                    int32_t condNode = AstFirstChild(&c->ast, (int32_t)scanNodeId);
                    int32_t fmtNode = AstNextSibling(&c->ast, condNode);
                    if (n->kind == H2Ast_STRING && fmtNode == (int32_t)nodeId) {
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

int IsTypeDeclKind(H2AstKind kind) {
    return kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM
        || kind == H2Ast_TYPE_ALIAS;
}

int IsDeclKind(H2AstKind kind) {
    return kind == H2Ast_FN || kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM
        || kind == H2Ast_TYPE_ALIAS || kind == H2Ast_VAR || kind == H2Ast_CONST;
}

int IsPubDeclNode(const H2AstNode* n) {
    return (n->flags & H2AstFlag_PUB) != 0;
}

int32_t AstFirstChild(const H2Ast* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

int32_t AstNextSibling(const H2Ast* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

const H2AstNode* _Nullable NodeAt(const H2CBackendC* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast.len) {
        return NULL;
    }
    return &c->ast.nodes[nodeId];
}

int GetDeclNameSpan(const H2CBackendC* c, int32_t nodeId, uint32_t* outStart, uint32_t* outEnd) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL || !IsDeclKind(n->kind) || n->dataEnd <= n->dataStart) {
        return -1;
    }
    *outStart = n->dataStart;
    *outEnd = n->dataEnd;
    return 0;
}

static int AddNameLiteral(
    H2CBackendC* c, const char* name, H2AstKind kind, int isExported, int forcePkgPrefix) {
    uint32_t i;
    char*    nameDup;
    char*    cName;
    H2Buf    tmp = { 0 };

    tmp.arena = &c->arena;

    for (i = 0; i < c->nameLen; i++) {
        if (StrEq(c->names[i].name, name)) {
            if (isExported) {
                c->names[i].isExported = 1;
            }
            return 0;
        }
    }

    nameDup = DupCStr(c, name);
    if (nameDup == NULL) {
        return -1;
    }

    if (!forcePkgPrefix && HasDoubleUnderscore(nameDup)) {
        cName = DupCStr(c, nameDup);
    } else {
        if (BufAppendCStr(&tmp, c->unit->packageName) != 0 || BufAppendCStr(&tmp, "__") != 0
            || BufAppendCStr(&tmp, nameDup) != 0)
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
            sizeof(H2NameMap),
            (uint32_t)_Alignof(H2NameMap))
        != 0)
    {
        return -1;
    }
    c->names[c->nameLen].name = nameDup;
    c->names[c->nameLen].cName = cName;
    c->names[c->nameLen].kind = kind;
    c->names[c->nameLen].isExported = isExported;
    c->nameLen++;
    return 0;
}

int AddName(H2CBackendC* c, uint32_t nameStart, uint32_t nameEnd, H2AstKind kind, int isExported) {
    char* name = DupSlice(c, c->unit->source, nameStart, nameEnd);
    if (name == NULL) {
        return -1;
    }
    return AddNameLiteral(c, name, kind, isExported, 0);
}

const H2NameMap* _Nullable FindNameBySlice(const H2CBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->names[i].name)) {
            return &c->names[i];
        }
    }
    return NULL;
}

const H2NameMap* _Nullable FindNameByCString(const H2CBackendC* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (StrEq(c->names[i].name, name)) {
            return &c->names[i];
        }
    }
    return NULL;
}

static int32_t FindParentNodeId(const H2Ast* ast, int32_t childNodeId) {
    uint32_t i;
    if (ast == NULL || childNodeId < 0 || (uint32_t)childNodeId >= ast->len) {
        return -1;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t child = ast->nodes[i].firstChild;
        while (child >= 0) {
            if (child == childNodeId) {
                return (int32_t)i;
            }
            child = ast->nodes[child].nextSibling;
        }
    }
    return -1;
}

int BuildTypeDeclFlatName(H2CBackendC* c, int32_t nodeId, char** outName) {
    int32_t          chain[64];
    uint32_t         chainLen = 0;
    int32_t          cur = nodeId;
    H2Buf            b = { 0 };
    uint32_t         i;
    const H2AstNode* n = NodeAt(c, nodeId);
    if (outName == NULL || n == NULL || !IsTypeDeclKind(n->kind)) {
        return -1;
    }
    while (cur >= 0) {
        const H2AstNode* cn = NodeAt(c, cur);
        int32_t          parent;
        if (cn == NULL || !IsTypeDeclKind(cn->kind)) {
            break;
        }
        if (chainLen >= (uint32_t)(sizeof(chain) / sizeof(chain[0]))) {
            return -1;
        }
        chain[chainLen++] = cur;
        parent = FindParentNodeId(&c->ast, cur);
        while (parent >= 0) {
            const H2AstNode* pn = NodeAt(c, parent);
            if (pn != NULL && IsTypeDeclKind(pn->kind)) {
                break;
            }
            parent = FindParentNodeId(&c->ast, parent);
        }
        cur = parent;
    }
    b.arena = &c->arena;
    for (i = chainLen; i > 0; i--) {
        const H2AstNode* cn = NodeAt(c, chain[i - 1u]);
        if (cn == NULL) {
            return -1;
        }
        if (i != chainLen && BufAppendCStr(&b, "__") != 0) {
            return -1;
        }
        if (BufAppendSlice(&b, c->unit->source, cn->dataStart, cn->dataEnd) != 0) {
            return -1;
        }
    }
    *outName = BufFinish(&b);
    return *outName == NULL ? -1 : 0;
}

const H2NameMap* _Nullable FindTypeDeclMapByNode(H2CBackendC* c, int32_t nodeId) {
    char* flat = NULL;
    if (BuildTypeDeclFlatName(c, nodeId, &flat) != 0) {
        return NULL;
    }
    return FindNameByCString(c, flat);
}

static int SliceContainsDot(const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = start; i < end; i++) {
        if (src[i] == '.') {
            return 1;
        }
    }
    return 0;
}

static int32_t FindEnclosingTypeDeclNode(const H2CBackendC* c, int32_t nodeId) {
    int32_t parent = FindParentNodeId(&c->ast, nodeId);
    while (parent >= 0) {
        const H2AstNode* pn = NodeAt(c, parent);
        if (pn != NULL && IsTypeDeclKind(pn->kind)) {
            return parent;
        }
        parent = FindParentNodeId(&c->ast, parent);
    }
    return -1;
}

static const char* _Nullable ResolveTypeNameInScope(
    H2CBackendC* c, int32_t typeRefNodeId, uint32_t start, uint32_t end) {
    const char*      resolved = ResolveTypeName(c, start, end);
    const H2NameMap* map;
    int32_t          ownerNodeId;
    if (resolved == NULL) {
        return NULL;
    }
    if (SliceContainsDot(c->unit->source, start, end)) {
        return resolved;
    }
    map = FindNameByCName(c, resolved);
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        return resolved;
    }
    ownerNodeId = FindEnclosingTypeDeclNode(c, typeRefNodeId);
    while (ownerNodeId >= 0) {
        char* ownerFlat = NULL;
        H2Buf b = { 0 };
        char* candidate;
        if (BuildTypeDeclFlatName(c, ownerNodeId, &ownerFlat) != 0 || ownerFlat == NULL) {
            return NULL;
        }
        b.arena = &c->arena;
        if (BufAppendCStr(&b, ownerFlat) != 0 || BufAppendCStr(&b, "__") != 0
            || BufAppendSlice(&b, c->unit->source, start, end) != 0)
        {
            return NULL;
        }
        candidate = BufFinish(&b);
        if (candidate == NULL) {
            return NULL;
        }
        map = FindNameByCString(c, candidate);
        if (map != NULL && IsTypeDeclKind(map->kind)) {
            return map->cName;
        }
        ownerNodeId = FindEnclosingTypeDeclNode(c, ownerNodeId);
    }
    {
        H2Buf b = { 0 };
        char* candidate;
        b.arena = &c->arena;
        if (BufAppendCStr(&b, "builtin__") != 0
            || BufAppendSlice(&b, c->unit->source, start, end) != 0)
        {
            return NULL;
        }
        candidate = BufFinish(&b);
        if (candidate == NULL) {
            return NULL;
        }
        map = FindNameByCString(c, candidate);
        if (map != NULL && IsTypeDeclKind(map->kind)) {
            return map->cName;
        }
    }
    return resolved;
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

int ResolveMainSemanticContextType(H2CBackendC* c, H2TypeRef* outType) {
    const H2NameMap* map = FindNameByCString(c, "builtin__Context");
    uint32_t         i;
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        TypeRefSetScalar(outType, map->cName);
        return 0;
    }
    for (i = 0; i < c->nameLen; i++) {
        if (IsTypeDeclKind(c->names[i].kind)
            && NameHasPrefixSuffix(c->names[i].name, "builtin", "__Context"))
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
    TypeRefSetScalar(outType, "__hop_Context");
    return 0;
}

const char* ResolveRuneTypeBaseName(H2CBackendC* c) {
    const H2NameMap* map = FindNameByCString(c, "builtin__rune");
    uint32_t         i;
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        return map->cName;
    }
    for (i = 0; i < c->nameLen; i++) {
        if (IsTypeDeclKind(c->names[i].kind)
            && NameHasPrefixSuffix(c->names[i].name, "builtin", "__rune"))
        {
            return c->names[i].cName;
        }
    }
    map = FindNameByCString(c, "rune");
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        return map->cName;
    }
    return "__hop_u32";
}

const H2NameMap* _Nullable FindNameByCName(const H2CBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->nameLen; i++) {
        if (StrEq(c->names[i].cName, cName)) {
            return &c->names[i];
        }
    }
    return NULL;
}

static int32_t CodegenCFindNamedTypeIndexByTypeId(const H2TypeCheckCtx* tc, int32_t typeId) {
    uint32_t i;
    if (tc == NULL || typeId < 0) {
        return -1;
    }
    for (i = 0; i < tc->namedTypeLen; i++) {
        if (tc->namedTypes[i].typeId == typeId) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int CodegenCTypeIdContainsTypeParam(const H2TypeCheckCtx* tc, int32_t typeId) {
    int32_t  namedIndex;
    uint16_t i;
    if (tc == NULL || typeId < 0 || (uint32_t)typeId >= tc->typeLen) {
        return 0;
    }
    if (tc->types[typeId].kind == H2TCType_TYPE_PARAM) {
        return 1;
    }
    namedIndex = CodegenCFindNamedTypeIndexByTypeId(tc, typeId);
    if (namedIndex >= 0) {
        const H2TCNamedType* nt = &tc->namedTypes[(uint32_t)namedIndex];
        for (i = 0; i < nt->templateArgCount; i++) {
            if (CodegenCTypeIdContainsTypeParam(tc, tc->genericArgTypes[nt->templateArgStart + i]))
            {
                return 1;
            }
        }
    }
    if (tc->types[typeId].baseType >= 0) {
        return CodegenCTypeIdContainsTypeParam(tc, tc->types[typeId].baseType);
    }
    return 0;
}

int CodegenCNodeHasTypeParams(const H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    int32_t          child;
    if (n == NULL || !IsDeclKind(n->kind)) {
        return 0;
    }
    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        const H2AstNode* ch = NodeAt(c, child);
        if (ch != NULL && ch->kind == H2Ast_TYPE_PARAM) {
            return 1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int CodegenCPushActiveFunctionTypeContext(H2CBackendC* c, uint32_t tcFuncIndex) {
    H2TypeCheckCtx*     tc;
    const H2TCFunction* fn;
    if (c == NULL || c->constEval == NULL || tcFuncIndex == UINT32_MAX) {
        return 0;
    }
    tc = &c->constEval->tc;
    if (tcFuncIndex >= tc->funcLen) {
        return -1;
    }
    fn = &tc->funcs[tcFuncIndex];
    c->activeTcFuncIndex = tcFuncIndex;
    c->activeTcNamedTypeIndex = -1;
    tc->activeGenericArgStart = fn->templateArgStart;
    tc->activeGenericArgCount = fn->templateArgCount;
    tc->activeGenericDeclNode = fn->templateArgCount > 0 ? fn->declNode : -1;
    return 0;
}

int CodegenCPushActiveNamedTypeContext(H2CBackendC* c, uint32_t tcNamedIndex) {
    H2TypeCheckCtx*      tc;
    const H2TCNamedType* nt;
    if (c == NULL || c->constEval == NULL) {
        return 0;
    }
    tc = &c->constEval->tc;
    if (tcNamedIndex >= tc->namedTypeLen) {
        return -1;
    }
    nt = &tc->namedTypes[tcNamedIndex];
    c->activeTcFuncIndex = UINT32_MAX;
    c->activeTcNamedTypeIndex = (int32_t)tcNamedIndex;
    tc->activeGenericArgStart = nt->templateArgStart;
    tc->activeGenericArgCount = nt->templateArgCount;
    tc->activeGenericDeclNode = nt->templateArgCount > 0 ? nt->declNode : -1;
    return 0;
}

void CodegenCPopActiveTypeContext(
    H2CBackendC* c,
    uint32_t     savedFuncIndex,
    int32_t      savedNamedTypeIndex,
    uint32_t     savedArgStart,
    uint16_t     savedArgCount,
    int32_t      savedDeclNode) {
    if (c == NULL) {
        return;
    }
    c->activeTcFuncIndex = savedFuncIndex;
    c->activeTcNamedTypeIndex = savedNamedTypeIndex;
    if (c->constEval != NULL) {
        H2TypeCheckCtx* tc = &c->constEval->tc;
        tc->activeGenericArgStart = savedArgStart;
        tc->activeGenericArgCount = savedArgCount;
        tc->activeGenericDeclNode = savedDeclNode;
    }
}

int ResolveTypeValueNameExprTypeRef(
    H2CBackendC* c, uint32_t start, uint32_t end, H2TypeRef* outTypeRef) {
    const H2NameMap* map;
    const char*      resolvedTypeName;
    if (c != NULL && c->constEval != NULL
        && (c->activeTcFuncIndex != UINT32_MAX || c->activeTcNamedTypeIndex >= 0))
    {
        H2TypeCheckCtx* tc = &c->constEval->tc;
        uint32_t        paramArgStart = tc->activeGenericArgStart;
        uint32_t        concreteArgStart = tc->activeGenericArgStart;
        uint16_t        i;
        uint16_t        argCount = tc->activeGenericArgCount;
        if (c->activeTcFuncIndex != UINT32_MAX) {
            const H2TCFunction* fn = &tc->funcs[c->activeTcFuncIndex];
            concreteArgStart = fn->templateArgStart;
            argCount = fn->templateArgCount;
            if (fn->templateRootFuncIndex >= 0) {
                paramArgStart = tc->funcs[(uint32_t)fn->templateRootFuncIndex].templateArgStart;
            }
        } else if (c->activeTcNamedTypeIndex >= 0) {
            const H2TCNamedType* nt = &tc->namedTypes[(uint32_t)c->activeTcNamedTypeIndex];
            concreteArgStart = nt->templateArgStart;
            argCount = nt->templateArgCount;
            if (nt->templateRootNamedIndex >= 0) {
                paramArgStart =
                    tc->namedTypes[(uint32_t)nt->templateRootNamedIndex].templateArgStart;
            }
        }
        for (i = 0; i < argCount; i++) {
            int32_t paramTypeId = tc->genericArgTypes[paramArgStart + i];
            if (paramTypeId >= 0 && (uint32_t)paramTypeId < tc->typeLen
                && tc->types[paramTypeId].kind == H2TCType_TYPE_PARAM && end >= start
                && tc->types[paramTypeId].nameEnd >= tc->types[paramTypeId].nameStart
                && end - start == tc->types[paramTypeId].nameEnd - tc->types[paramTypeId].nameStart
                && memcmp(
                       c->unit->source + start,
                       c->unit->source + tc->types[paramTypeId].nameStart,
                       end - start)
                       == 0)
            {
                int32_t concreteTypeId = tc->genericArgTypes[concreteArgStart + i];
                if (ParseTypeRefFromConstEvalTypeId(c, concreteTypeId, outTypeRef) == 0
                    && outTypeRef->valid)
                {
                    return 1;
                }
            }
        }
    }
    if (SliceEq(c->unit->source, start, end, "bool")) {
        TypeRefSetScalar(outTypeRef, "__hop_bool");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "str")) {
        TypeRefSetScalar(outTypeRef, "__hop_str");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u8")) {
        TypeRefSetScalar(outTypeRef, "__hop_u8");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u16")) {
        TypeRefSetScalar(outTypeRef, "__hop_u16");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u32")) {
        TypeRefSetScalar(outTypeRef, "__hop_u32");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "u64")) {
        TypeRefSetScalar(outTypeRef, "__hop_u64");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i8")) {
        TypeRefSetScalar(outTypeRef, "__hop_i8");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i16")) {
        TypeRefSetScalar(outTypeRef, "__hop_i16");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i32")) {
        TypeRefSetScalar(outTypeRef, "__hop_i32");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "i64")) {
        TypeRefSetScalar(outTypeRef, "__hop_i64");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "uint")) {
        TypeRefSetScalar(outTypeRef, "__hop_uint");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "int")) {
        TypeRefSetScalar(outTypeRef, "__hop_int");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "rawptr")) {
        TypeRefSetScalar(outTypeRef, "void");
        outTypeRef->ptrDepth = 1;
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "const_int")) {
        TypeRefSetScalar(outTypeRef, "__hop_int");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "f32")) {
        TypeRefSetScalar(outTypeRef, "__hop_f32");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "f64")) {
        TypeRefSetScalar(outTypeRef, "__hop_f64");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "const_float")) {
        TypeRefSetScalar(outTypeRef, "__hop_f64");
        return 1;
    }
    if (SliceEq(c->unit->source, start, end, "type")) {
        TypeRefSetScalar(outTypeRef, "__hop_type");
        return 1;
    }
    map = FindNameBySlice(c, start, end);
    if (map != NULL && IsTypeDeclKind(map->kind)) {
        TypeRefSetScalar(outTypeRef, map->cName);
        return 1;
    }
    resolvedTypeName = ResolveTypeName(c, start, end);
    if (resolvedTypeName != NULL) {
        map = FindNameByCName(c, resolvedTypeName);
        if (map != NULL && IsTypeDeclKind(map->kind)) {
            TypeRefSetScalar(outTypeRef, map->cName);
            return 1;
        }
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
    H2TypeTagKind_INVALID = 0,
    H2TypeTagKind_PRIMITIVE = 1,
    H2TypeTagKind_ALIAS = 2,
    H2TypeTagKind_STRUCT = 3,
    H2TypeTagKind_UNION = 4,
    H2TypeTagKind_ENUM = 5,
    H2TypeTagKind_POINTER = 6,
    H2TypeTagKind_REFERENCE = 7,
    H2TypeTagKind_SLICE = 8,
    H2TypeTagKind_ARRAY = 9,
    H2TypeTagKind_OPTIONAL = 10,
    H2TypeTagKind_FUNCTION = 11,
    H2TypeTagKind_TUPLE = 12,
};

const H2TypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const H2CBackendC* c, const char* aliasName);

uint8_t TypeTagKindFromTypeRef(const H2CBackendC* c, const H2TypeRef* t) {
    const H2NameMap* map;
    if (c == NULL || t == NULL || !t->valid) {
        return H2TypeTagKind_INVALID;
    }
    if (t->isOptional) {
        return H2TypeTagKind_OPTIONAL;
    }
    if (t->containerKind == H2TypeContainer_ARRAY) {
        return H2TypeTagKind_ARRAY;
    }
    if (t->containerKind == H2TypeContainer_SLICE_RO
        || t->containerKind == H2TypeContainer_SLICE_MUT)
    {
        return H2TypeTagKind_SLICE;
    }
    if (t->containerPtrDepth > 0 || t->ptrDepth > 0) {
        return t->readOnly ? H2TypeTagKind_REFERENCE : H2TypeTagKind_POINTER;
    }
    if (t->baseName == NULL) {
        return H2TypeTagKind_INVALID;
    }
    if (FindTypeAliasInfoByAliasName(c, t->baseName) != NULL) {
        return H2TypeTagKind_ALIAS;
    }
    map = FindNameByCName(c, t->baseName);
    if (map != NULL) {
        switch (map->kind) {
            case H2Ast_STRUCT:     return H2TypeTagKind_STRUCT;
            case H2Ast_UNION:      return H2TypeTagKind_UNION;
            case H2Ast_ENUM:       return H2TypeTagKind_ENUM;
            case H2Ast_TYPE_ALIAS: return H2TypeTagKind_ALIAS;
            default:               break;
        }
    }
    if (StrHasPrefix(t->baseName, "__hop_fn_t_")) {
        return H2TypeTagKind_FUNCTION;
    }
    if (StrHasPrefix(t->baseName, "__hop_tuple_")) {
        return H2TypeTagKind_TUPLE;
    }
    return H2TypeTagKind_PRIMITIVE;
}

uint64_t TypeTagFromTypeRef(const H2CBackendC* c, const H2TypeRef* t) {
    uint8_t  kind;
    uint64_t h = 1469598103934665603ULL;
    if (t == NULL || !t->valid) {
        return 0u;
    }
    kind = TypeTagKindFromTypeRef(c, t);
    if (kind == H2TypeTagKind_INVALID) {
        kind = H2TypeTagKind_PRIMITIVE;
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

int EmitTypeTagLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t) {
    uint64_t tag = TypeTagFromTypeRef(c, t);
    return BufAppendHexU64Literal(&c->out, tag);
}

const H2TypeAliasInfo* _Nullable FindTypeAliasInfoByAliasName(
    const H2CBackendC* c, const char* aliasName) {
    uint32_t i;
    for (i = 0; i < c->typeAliasLen; i++) {
        if (StrEq(c->typeAliases[i].aliasName, aliasName)) {
            return &c->typeAliases[i];
        }
    }
    return NULL;
}

int AddTypeAliasInfo(H2CBackendC* c, const char* aliasName, H2TypeRef targetType) {
    if (FindTypeAliasInfoByAliasName(c, aliasName) != NULL) {
        return 0;
    }
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->typeAliases,
            &c->typeAliasCap,
            c->typeAliasLen + 1u,
            sizeof(H2TypeAliasInfo),
            (uint32_t)_Alignof(H2TypeAliasInfo))
        != 0)
    {
        return -1;
    }
    c->typeAliases[c->typeAliasLen].aliasName = (char*)aliasName;
    c->typeAliases[c->typeAliasLen].targetType = targetType;
    c->typeAliasLen++;
    return 0;
}

const char* ResolveScalarAliasBaseName(const H2CBackendC* c, const char* typeName) {
    uint32_t guard = 0;
    while (typeName != NULL && guard++ <= c->typeAliasLen) {
        const H2TypeAliasInfo* alias = FindTypeAliasInfoByAliasName(c, typeName);
        if (alias == NULL || !alias->targetType.valid || alias->targetType.baseName == NULL
            || alias->targetType.containerKind != H2TypeContainer_SCALAR
            || alias->targetType.ptrDepth != 0 || alias->targetType.containerPtrDepth != 0
            || alias->targetType.isOptional)
        {
            break;
        }
        typeName = alias->targetType.baseName;
    }
    return typeName;
}

int ResolveReflectedTypeValueExprTypeRef(H2CBackendC* c, int32_t exprNode, H2TypeRef* outTypeRef) {
    const H2AstNode* n = NodeAt(c, exprNode);
    if (outTypeRef == NULL) {
        return 0;
    }
    TypeRefSetInvalid(outTypeRef);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_CALL_ARG) {
        int32_t inner = AstFirstChild(&c->ast, exprNode);
        if (inner < 0) {
            return 0;
        }
        return ResolveReflectedTypeValueExprTypeRef(c, inner, outTypeRef);
    }
    if (n->kind == H2Ast_IDENT) {
        return ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, outTypeRef);
    }
    if (n->kind == H2Ast_TYPE_NAME) {
        if (AstFirstChild(&c->ast, exprNode) >= 0) {
            return ParseTypeRef(c, exprNode, outTypeRef) == 0 && outTypeRef->valid;
        }
        return ResolveTypeValueNameExprTypeRef(c, n->dataStart, n->dataEnd, outTypeRef);
    }
    if (n->kind == H2Ast_TYPE_VALUE) {
        int32_t typeNode = AstFirstChild(&c->ast, exprNode);
        if (typeNode < 0) {
            return 0;
        }
        return ParseTypeRef(c, typeNode, outTypeRef) == 0 && outTypeRef->valid;
    }
    if (n->kind == H2Ast_CALL) {
        int32_t          calleeNode = AstFirstChild(&c->ast, exprNode);
        const H2AstNode* callee = NodeAt(c, calleeNode);
        int32_t          argNode;
        int32_t          nextNode;
        if (callee == NULL || callee->kind != H2Ast_IDENT) {
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
            H2TypeRef elemType;
            if (argNode < 0 || nextNode >= 0) {
                return 0;
            }
            if (!ResolveReflectedTypeValueExprTypeRef(c, argNode, &elemType)) {
                return 0;
            }
            if (elemType.containerKind == H2TypeContainer_ARRAY
                || elemType.containerKind == H2TypeContainer_SLICE_RO
                || elemType.containerKind == H2TypeContainer_SLICE_MUT)
            {
                elemType.containerPtrDepth++;
                if (elemType.containerKind == H2TypeContainer_SLICE_RO
                    || elemType.containerKind == H2TypeContainer_SLICE_MUT)
                {
                    elemType.containerKind = H2TypeContainer_SLICE_MUT;
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
            H2TypeRef elemType;
            if (argNode < 0 || nextNode >= 0) {
                return 0;
            }
            if (!ResolveReflectedTypeValueExprTypeRef(c, argNode, &elemType)) {
                return 0;
            }
            if (elemType.containerKind != H2TypeContainer_SCALAR) {
                return 0;
            }
            elemType.containerKind = H2TypeContainer_SLICE_RO;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            elemType.readOnly = 1;
            *outTypeRef = elemType;
            return 1;
        }
        if (SliceEq(c->unit->source, callee->dataStart, callee->dataEnd, "array")) {
            H2TypeRef elemType;
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
            if (elemType.containerKind != H2TypeContainer_SCALAR) {
                return 0;
            }
            if (NodeAt(c, lenExprNode) != NULL && NodeAt(c, lenExprNode)->kind == H2Ast_CALL_ARG) {
                lenExprNode = AstFirstChild(&c->ast, lenExprNode);
            }
            if (lenExprNode < 0 || EvalConstIntExpr(c, lenExprNode, &lenValue, &lenIsConst) != 0
                || !lenIsConst || lenValue < 0 || lenValue > (int64_t)UINT32_MAX)
            {
                return 0;
            }
            elemType.containerKind = H2TypeContainer_ARRAY;
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

int TypeRefIsTypeValue(const H2TypeRef* t) {
    return t != NULL && t->valid && t->containerKind == H2TypeContainer_SCALAR && t->ptrDepth == 0
        && t->containerPtrDepth == 0 && !t->isOptional && t->baseName != NULL
        && StrEq(t->baseName, "__hop_type");
}

const char* _Nullable FindReflectKindTypeName(const H2CBackendC* c) {
    uint32_t         i;
    const H2NameMap* direct = FindNameByCString(c, "reflect__Kind");
    if (direct != NULL && direct->kind == H2Ast_ENUM) {
        return direct->cName;
    }
    for (i = 0; i < c->nameLen; i++) {
        const H2NameMap* map = &c->names[i];
        if (map->kind != H2Ast_ENUM || map->name == NULL) {
            continue;
        }
        if (StrHasPrefix(map->name, "reflect") && StrHasSuffix(map->name, "__Kind")) {
            return map->cName;
        }
    }
    return NULL;
}

const char* TypeRefDisplayBaseName(const H2CBackendC* c, const char* baseName) {
    const H2NameMap* map;
    if (baseName == NULL) {
        return "<type>";
    }
    if (StrEq(baseName, "__hop_bool")) {
        return "bool";
    }
    if (StrEq(baseName, "__hop_str") || StrEq(baseName, "builtin__str")) {
        return "str";
    }
    if (StrEq(baseName, "__hop_u8")) {
        return "u8";
    }
    if (StrEq(baseName, "__hop_u16")) {
        return "u16";
    }
    if (StrEq(baseName, "__hop_u32")) {
        return "u32";
    }
    if (StrEq(baseName, "__hop_u64")) {
        return "u64";
    }
    if (StrEq(baseName, "__hop_i8")) {
        return "i8";
    }
    if (StrEq(baseName, "__hop_i16")) {
        return "i16";
    }
    if (StrEq(baseName, "__hop_i32")) {
        return "i32";
    }
    if (StrEq(baseName, "__hop_i64")) {
        return "i64";
    }
    if (StrEq(baseName, "__hop_uint")) {
        return "uint";
    }
    if (StrEq(baseName, "__hop_int")) {
        return "int";
    }
    if (StrEq(baseName, "__hop_f32")) {
        return "f32";
    }
    if (StrEq(baseName, "__hop_f64")) {
        return "f64";
    }
    if (StrEq(baseName, "__hop_type")) {
        return "type";
    }
    map = FindNameByCName(c, baseName);
    if (map != NULL && map->name != NULL) {
        return map->name;
    }
    return baseName;
}

int EmitTypeNameStringLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* _Nullable t) {
    const char* name = "<type>";
    int32_t     literalId;
    if (t != NULL && t->valid && !t->isOptional && t->containerKind == H2TypeContainer_SCALAR
        && t->ptrDepth == 0 && t->containerPtrDepth == 0)
    {
        name = TypeRefDisplayBaseName(c, t->baseName);
    }
    if (GetOrAddStringLiteralBytes(c, (const uint8_t*)name, (uint32_t)StrLen(name), &literalId)
        != 0)
    {
        return -1;
    }
    return EmitStringLiteralValue(c, literalId, 0);
}

int EmitTypeTagKindLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t) {
    uint8_t kind = TypeTagKindFromTypeRef(c, t);
    if (kind == H2TypeTagKind_INVALID) {
        kind = H2TypeTagKind_PRIMITIVE;
    }
    return BufAppendU32(&c->out, (uint32_t)kind);
}

int EmitRuntimeTypeTagKindFromExpr(H2CBackendC* c, int32_t exprNode) {
    if (BufAppendCStr(&c->out, "((__hop_u8)(((") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendCStr(&c->out, ") >> 56u) & 0xffu))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitTypeTagIsAliasLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t) {
    return BufAppendCStr(
        &c->out,
        TypeTagKindFromTypeRef(c, t) == H2TypeTagKind_ALIAS
            ? "((__hop_bool)1)"
            : "((__hop_bool)0)");
}

int EmitRuntimeTypeTagIsAliasFromExpr(H2CBackendC* c, int32_t exprNode) {
    if (BufAppendCStr(&c->out, "((__hop_bool)((((__hop_u64)(") != 0 || EmitExpr(c, exprNode) != 0
        || BufAppendCStr(&c->out, ")) >> 56u) & 0xffu) == 2u))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitRuntimeTypeTagCtorUnary(H2CBackendC* c, uint32_t kindTag, uint64_t salt, int32_t argNode) {
    if (BufAppendCStr(&c->out, "((__hop_type)((((__hop_u64)") != 0
        || BufAppendU32(&c->out, kindTag) != 0
        || BufAppendCStr(&c->out, "u) << 56u) | ((((((__hop_u64)(") != 0
        || EmitExpr(c, argNode) != 0 || BufAppendCStr(&c->out, ")) ^ ") != 0
        || BufAppendHexU64Literal(&c->out, salt) != 0
        || BufAppendCStr(&c->out, ") * 1099511628211ULL) & 0x00ffffffffffffffULL))))") != 0)
    {
        return -1;
    }
    return 0;
}

int EmitRuntimeTypeTagCtorArray(H2CBackendC* c, int32_t elemTagNode, int32_t lenNode) {
    if (BufAppendCStr(&c->out, "((__hop_type)((((__hop_u64)9u) << 56u) | ((((((__hop_u64)(") != 0
        || EmitExpr(c, elemTagNode) != 0 || BufAppendCStr(&c->out, ")) ^ (((__hop_u64)(") != 0
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

int EmitTypeTagBaseLiteralFromTypeRef(H2CBackendC* c, const H2TypeRef* t) {
    const H2TypeAliasInfo* alias;
    if (t == NULL || !t->valid || t->containerKind != H2TypeContainer_SCALAR || t->ptrDepth != 0
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

int TypeRefIsRuneLike(const H2CBackendC* c, const H2TypeRef* typeRef) {
    const char* runeType;
    const char* typeName;
    uint32_t    guard = 0;
    if (typeRef == NULL || !typeRef->valid || typeRef->containerKind != H2TypeContainer_SCALAR
        || typeRef->ptrDepth != 0 || typeRef->containerPtrDepth != 0 || typeRef->isOptional
        || typeRef->baseName == NULL)
    {
        return 0;
    }
    runeType = ResolveRuneTypeBaseName((H2CBackendC*)c);
    typeName = typeRef->baseName;
    while (typeName != NULL && guard++ <= c->typeAliasLen + 1u) {
        const H2TypeAliasInfo* alias;
        if (StrEq(typeName, runeType)) {
            return 1;
        }
        alias = FindTypeAliasInfoByAliasName(c, typeName);
        if (alias == NULL || !alias->targetType.valid || alias->targetType.baseName == NULL
            || alias->targetType.containerKind != H2TypeContainer_SCALAR
            || alias->targetType.ptrDepth != 0 || alias->targetType.containerPtrDepth != 0
            || alias->targetType.isOptional)
        {
            break;
        }
        typeName = alias->targetType.baseName;
    }
    return 0;
}

const char* _Nullable ResolveTypeName(H2CBackendC* c, uint32_t start, uint32_t end) {
    const H2NameMap*         mapped;
    char*                    normalized;
    char*                    normalizedDouble = NULL;
    uint32_t                 pos;
    int                      hasDot = 0;
    uint32_t                 i;
    static const char* const builtinSlNames[] = {
        "bool", "str", "u8",     "u16",       "u32", "u64", "i8",          "i16",  "i32",     "i64",
        "uint", "int", "rawptr", "const_int", "f32", "f64", "const_float", "type", "anytype",
    };
    static const char* const builtinCNames[] = {
        "__hop_bool", "__hop_str", "__hop_u8",  "__hop_u16",  "__hop_u32", "__hop_u64", "__hop_i8",
        "__hop_i16",  "__hop_i32", "__hop_i64", "__hop_uint", "__hop_int", "void",      "__hop_int",
        "__hop_f32",  "__hop_f64", "__hop_f64", "__hop_type", "__hop_u8",
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

    for (pos = start; pos < end; pos++) {
        if (c->unit->source[pos] == '.') {
            hasDot = 1;
            break;
        }
    }
    if (hasDot) {
        normalizedDouble = DupAndReplaceDotsDouble(c, c->unit->source, start, end);
        if (normalizedDouble == NULL) {
            return NULL;
        }
        mapped = FindNameByCString(c, normalizedDouble);
        if (mapped != NULL && IsTypeDeclKind(mapped->kind)) {
            return mapped->cName;
        }
    }

    return normalized;
}

void NormalizeCoreRuntimeTypeName(H2TypeRef* outType) {
    if (outType->baseName != NULL && StrEq(outType->baseName, "builtin__str")) {
        outType->baseName = "__hop_str";
    } else if (outType->baseName != NULL && StrEq(outType->baseName, "builtin__MemAllocator")) {
        outType->baseName = "__hop_MemAllocator";
    } else if (outType->baseName != NULL && StrEq(outType->baseName, "builtin__Logger")) {
        outType->baseName = "__hop_Logger";
    } else if (outType->baseName != NULL && StrEq(outType->baseName, "builtin__Context")) {
        outType->baseName = "__hop_Context";
    } else if (outType->baseName != NULL && StrEq(outType->baseName, "builtin__PrintContext")) {
        outType->baseName = "__hop_PrintContext";
    }
}

const char* _Nullable ConstEvalBuiltinCName(H2ConstEvalBuiltinKind builtin) {
    switch (builtin) {
        case H2ConstEvalBuiltinKind_VOID:   return "void";
        case H2ConstEvalBuiltinKind_BOOL:   return "__hop_bool";
        case H2ConstEvalBuiltinKind_STR:    return "__hop_str";
        case H2ConstEvalBuiltinKind_TYPE:   return "__hop_type";
        case H2ConstEvalBuiltinKind_U8:     return "__hop_u8";
        case H2ConstEvalBuiltinKind_U16:    return "__hop_u16";
        case H2ConstEvalBuiltinKind_U32:    return "__hop_u32";
        case H2ConstEvalBuiltinKind_U64:    return "__hop_u64";
        case H2ConstEvalBuiltinKind_I8:     return "__hop_i8";
        case H2ConstEvalBuiltinKind_I16:    return "__hop_i16";
        case H2ConstEvalBuiltinKind_I32:    return "__hop_i32";
        case H2ConstEvalBuiltinKind_I64:    return "__hop_i64";
        case H2ConstEvalBuiltinKind_USIZE:  return "__hop_uint";
        case H2ConstEvalBuiltinKind_ISIZE:  return "__hop_int";
        case H2ConstEvalBuiltinKind_RAWPTR: return "void";
        case H2ConstEvalBuiltinKind_F32:    return "__hop_f32";
        case H2ConstEvalBuiltinKind_F64:    return "__hop_f64";
        default:                            return NULL;
    }
}

int ParseTypeRefFromConstEvalTypeId(H2CBackendC* c, int32_t typeId, H2TypeRef* outType) {
    H2ConstEvalTypeInfo info;
    if (c == NULL || outType == NULL || c->constEval == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (H2ConstEvalSessionGetTypeInfo(c->constEval, typeId, &info) != 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    switch (info.kind) {
        case H2ConstEvalTypeKind_BUILTIN: {
            const char* baseName = ConstEvalBuiltinCName(info.builtin);
            if (baseName == NULL) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            TypeRefSetScalar(outType, baseName);
            if (info.builtin == H2ConstEvalBuiltinKind_RAWPTR) {
                outType->ptrDepth = 1;
            }
            NormalizeCoreRuntimeTypeName(outType);
            return 0;
        }
        case H2ConstEvalTypeKind_NAMED:
        case H2ConstEvalTypeKind_ALIAS: {
            uint32_t         nameStart = info.nameStart;
            uint32_t         nameEnd = info.nameEnd;
            const H2AstNode* decl = NULL;
            const char*      name;
            int32_t namedIndex = CodegenCFindNamedTypeIndexByTypeId(&c->constEval->tc, typeId);
            if (namedIndex >= 0) {
                const H2TCNamedType* nt = &c->constEval->tc.namedTypes[(uint32_t)namedIndex];
                if (nt->templateRootNamedIndex >= 0 && nt->templateArgCount > 0) {
                    const H2NameMap* map;
                    char*            cName;
                    map = FindTypeDeclMapByNode(c, nt->declNode);
                    if (map == NULL) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    cName = BuildTemplateNamedTypeCName(c, map->cName, (uint32_t)namedIndex);
                    if (cName == NULL) {
                        TypeRefSetInvalid(outType);
                        return -1;
                    }
                    TypeRefSetScalar(outType, cName);
                    return 0;
                }
            }
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
        case H2ConstEvalTypeKind_PTR:
        case H2ConstEvalTypeKind_REF: {
            H2TypeRef childType;
            int       isRef = info.kind == H2ConstEvalTypeKind_REF;
            int       isReadOnlyRef = isRef && !(info.flags & H2ConstEvalTypeFlag_MUTABLE);
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, &childType) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (childType.containerKind == H2TypeContainer_ARRAY
                || childType.containerKind == H2TypeContainer_SLICE_RO
                || childType.containerKind == H2TypeContainer_SLICE_MUT)
            {
                childType.containerPtrDepth++;
                if (childType.containerKind == H2TypeContainer_SLICE_RO
                    || childType.containerKind == H2TypeContainer_SLICE_MUT)
                {
                    childType.containerKind =
                        isRef ? H2TypeContainer_SLICE_RO : H2TypeContainer_SLICE_MUT;
                }
                childType.readOnly = isRef ? isReadOnlyRef : 0;
            } else {
                childType.ptrDepth++;
                childType.readOnly = isRef ? isReadOnlyRef : 0;
            }
            *outType = childType;
            return 0;
        }
        case H2ConstEvalTypeKind_ARRAY: {
            H2TypeRef elemType;
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, &elemType) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (elemType.containerKind != H2TypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.containerKind = H2TypeContainer_ARRAY;
            elemType.containerPtrDepth = 0;
            elemType.arrayLen = info.arrayLen;
            elemType.hasArrayLen = 1;
            elemType.readOnly = 0;
            *outType = elemType;
            return 0;
        }
        case H2ConstEvalTypeKind_SLICE: {
            H2TypeRef elemType;
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, &elemType) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (elemType.containerKind != H2TypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.containerKind =
                (info.flags & H2ConstEvalTypeFlag_MUTABLE)
                    ? H2TypeContainer_SLICE_MUT
                    : H2TypeContainer_SLICE_RO;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            elemType.readOnly = (info.flags & H2ConstEvalTypeFlag_MUTABLE) ? 0 : 1;
            *outType = elemType;
            return 0;
        }
        case H2ConstEvalTypeKind_OPTIONAL:
            if (ParseTypeRefFromConstEvalTypeId(c, info.baseTypeId, outType) != 0) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            outType->isOptional = 1;
            return 0;
        case H2ConstEvalTypeKind_UNTYPED_INT:   TypeRefSetScalar(outType, "__hop_int"); return 0;
        case H2ConstEvalTypeKind_UNTYPED_FLOAT: TypeRefSetScalar(outType, "__hop_f64"); return 0;
        default:                                TypeRefSetInvalid(outType); return -1;
    }
}

int ParseTypeRefFromConstEvalTypeTag(H2CBackendC* c, uint64_t typeTag, H2TypeRef* outType) {
    int32_t typeId = -1;
    if (c == NULL || outType == NULL || c->constEval == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    if (H2ConstEvalSessionDecodeTypeTag(c->constEval, typeTag, &typeId) != 0) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    return ParseTypeRefFromConstEvalTypeId(c, typeId, outType);
}

int AddNodeRef(H2CBackendC* c, H2NodeRef** arr, uint32_t* len, uint32_t* cap, int32_t nodeId) {
    if (EnsureCapArena(
            &c->arena,
            (void**)arr,
            cap,
            *len + 1u,
            sizeof(H2NodeRef),
            (uint32_t)_Alignof(H2NodeRef))
        != 0)
    {
        return -1;
    }
    (*arr)[*len].nodeId = nodeId;
    (*len)++;
    return 0;
}

static int IsNodeImportedSurface(const H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL || c->options == NULL || c->options->emitNodeStartOffsetEnabled == 0) {
        return 0;
    }
    return n->start < c->options->emitNodeStartOffset;
}

static int CollectDeclSetsFromNode(
    H2CBackendC* c, int32_t nodeId, int isNested, int inheritedExported) {
    const H2AstNode* n = NodeAt(c, nodeId);
    uint32_t         start;
    uint32_t         end;
    int              isExported;
    if (n == NULL || !IsDeclKind(n->kind)) {
        return 0;
    }
    isExported = isNested ? inheritedExported : IsPubDeclNode(n);
    if (AddNodeRef(c, &c->topDecls, &c->topDeclLen, &c->topDeclCap, nodeId) != 0) {
        return -1;
    }
    if (isExported) {
        if (AddNodeRef(c, &c->pubDecls, &c->pubDeclLen, &c->pubDeclCap, nodeId) != 0) {
            return -1;
        }
    }
    if ((n->kind == H2Ast_VAR || n->kind == H2Ast_CONST)
        && NodeAt(c, AstFirstChild(&c->ast, nodeId)) != NULL
        && NodeAt(c, AstFirstChild(&c->ast, nodeId))->kind == H2Ast_NAME_LIST)
    {
        int32_t  nameList = AstFirstChild(&c->ast, nodeId);
        uint32_t i;
        uint32_t nameCount = ListCount(&c->ast, nameList);
        for (i = 0; i < nameCount; i++) {
            int32_t          nameNode = ListItemAt(&c->ast, nameList, i);
            const H2AstNode* name = NodeAt(c, nameNode);
            if (name == NULL) {
                return -1;
            }
            if (AddName(c, name->dataStart, name->dataEnd, n->kind, isExported) != 0) {
                return -1;
            }
        }
    } else if (IsTypeDeclKind(n->kind)) {
        if (isNested) {
            char* flatName = NULL;
            int   forcePkgPrefix = IsNodeImportedSurface(c, nodeId) ? 0 : 1;
            if (BuildTypeDeclFlatName(c, nodeId, &flatName) != 0
                || AddNameLiteral(c, flatName, n->kind, isExported, forcePkgPrefix) != 0)
            {
                return -1;
            }
        } else if (GetDeclNameSpan(c, nodeId, &start, &end) == 0) {
            if (AddName(c, start, end, n->kind, isExported) != 0) {
                return -1;
            }
        }
    } else if (GetDeclNameSpan(c, nodeId, &start, &end) == 0) {
        if (AddName(c, start, end, n->kind, isExported) != 0) {
            return -1;
        }
    }

    if (n->kind == H2Ast_STRUCT || n->kind == H2Ast_UNION) {
        int32_t child = AstFirstChild(&c->ast, nodeId);
        while (child >= 0) {
            const H2AstNode* ch = NodeAt(c, child);
            if (ch != NULL && IsTypeDeclKind(ch->kind)
                && CollectDeclSetsFromNode(c, child, 1, isExported) != 0)
            {
                return -1;
            }
            child = AstNextSibling(&c->ast, child);
        }
    }
    return 0;
}

int CollectDeclSets(H2CBackendC* c) {
    int32_t child = AstFirstChild(&c->ast, c->ast.root);
    while (child >= 0) {
        if (CollectDeclSetsFromNode(c, child, 0, 0) != 0) {
            return -1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

const H2FnTypeAlias* _Nullable FindFnTypeAliasByName(const H2CBackendC* c, const char* name) {
    uint32_t i;
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        if (StrEq(c->fnTypeAliases[i].aliasName, name)) {
            return &c->fnTypeAliases[i];
        }
    }
    return NULL;
}

int EnsureFnTypeAlias(
    H2CBackendC* c,
    H2TypeRef    returnType,
    H2TypeRef* _Nullable paramTypes,
    uint32_t     paramLen,
    int          isVariadic,
    const char** outAliasName) {
    uint32_t i;
    for (i = 0; i < c->fnTypeAliasLen; i++) {
        const H2FnTypeAlias* alias = &c->fnTypeAliases[i];
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
        H2TypeRef* paramCopy = NULL;
        char*      aliasName;
        H2Buf      b = { 0 };
        if (paramLen > 0) {
            uint32_t p;
            paramCopy = (H2TypeRef*)H2ArenaAlloc(
                &c->arena, (uint32_t)(sizeof(H2TypeRef) * paramLen), (uint32_t)_Alignof(H2TypeRef));
            if (paramCopy == NULL) {
                return -1;
            }
            for (p = 0; p < paramLen; p++) {
                paramCopy[p] = paramTypes[p];
            }
        }
        b.arena = &c->arena;
        if (BufAppendCStr(&b, "__hop_fn_t_") != 0 || BufAppendU32(&b, c->fnTypeAliasLen) != 0) {
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
                sizeof(H2FnTypeAlias),
                (uint32_t)_Alignof(H2FnTypeAlias))
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

const char* _Nullable TupleFieldName(H2CBackendC* c, uint32_t index) {
    H2Buf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, "__hop_t") != 0 || BufAppendU32(&b, index) != 0) {
        return NULL;
    }
    return BufFinish(&b);
}

static int ParseTypeRefFromActiveTypecheck(
    H2CBackendC* c, int32_t nodeId, H2TypeRef* outType, int* outHandled) {
    int32_t          typeId = -1;
    const H2AstNode* n;
    if (outHandled != NULL) {
        *outHandled = 0;
    }
    if (c == NULL || outType == NULL || outHandled == NULL || c->constEval == NULL || nodeId < 0
        || (uint32_t)nodeId >= c->ast.len)
    {
        return 0;
    }
    if (c->activeTcFuncIndex == UINT32_MAX && c->activeTcNamedTypeIndex < 0
        && AstFirstChild(&c->ast, nodeId) < 0)
    {
        return 0;
    }
    n = NodeAt(c, nodeId);
    if (n != NULL && n->kind == H2Ast_TYPE_NAME && AstFirstChild(&c->ast, nodeId) < 0
        && (c->activeTcFuncIndex != UINT32_MAX || c->activeTcNamedTypeIndex >= 0))
    {
        H2TypeCheckCtx* tc = &c->constEval->tc;
        uint32_t        paramArgStart = tc->activeGenericArgStart;
        uint32_t        concreteArgStart = tc->activeGenericArgStart;
        uint16_t        argCount = tc->activeGenericArgCount;
        uint16_t        i;
        if (c->activeTcFuncIndex != UINT32_MAX) {
            const H2TCFunction* fn = &tc->funcs[c->activeTcFuncIndex];
            concreteArgStart = fn->templateArgStart;
            argCount = fn->templateArgCount;
            if (fn->templateRootFuncIndex >= 0) {
                paramArgStart = tc->funcs[(uint32_t)fn->templateRootFuncIndex].templateArgStart;
            }
        } else {
            const H2TCNamedType* nt = &tc->namedTypes[(uint32_t)c->activeTcNamedTypeIndex];
            concreteArgStart = nt->templateArgStart;
            argCount = nt->templateArgCount;
            if (nt->templateRootNamedIndex >= 0) {
                paramArgStart =
                    tc->namedTypes[(uint32_t)nt->templateRootNamedIndex].templateArgStart;
            }
        }
        for (i = 0; i < argCount; i++) {
            int32_t paramTypeId = tc->genericArgTypes[paramArgStart + i];
            if (paramTypeId >= 0 && (uint32_t)paramTypeId < tc->typeLen
                && tc->types[paramTypeId].kind == H2TCType_TYPE_PARAM && n->dataEnd >= n->dataStart
                && tc->types[paramTypeId].nameEnd >= tc->types[paramTypeId].nameStart
                && n->dataEnd - n->dataStart
                       == tc->types[paramTypeId].nameEnd - tc->types[paramTypeId].nameStart
                && memcmp(
                       c->unit->source + n->dataStart,
                       c->unit->source + tc->types[paramTypeId].nameStart,
                       n->dataEnd - n->dataStart)
                       == 0)
            {
                int32_t concreteTypeId = tc->genericArgTypes[concreteArgStart + i];
                if (ParseTypeRefFromConstEvalTypeId(c, concreteTypeId, outType) != 0
                    || !outType->valid)
                {
                    return -1;
                }
                *outHandled = 1;
                return 0;
            }
        }
    }
    if (H2TCResolveTypeNode(&c->constEval->tc, nodeId, &typeId) != 0) {
        return 0;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->constEval->tc.typeLen
        || c->constEval->tc.types[typeId].kind == H2TCType_TYPE_PARAM)
    {
        return 0;
    }
    if (ParseTypeRefFromConstEvalTypeId(c, typeId, outType) != 0 || !outType->valid) {
        return -1;
    }
    *outHandled = 1;
    return 0;
}

int ParseTypeRef(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType) {
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        TypeRefSetInvalid(outType);
        return -1;
    }
    switch (n->kind) {
        case H2Ast_TYPE_NAME: {
            H2TypeRef reflectedType;
            int       handled = 0;
            if (ParseTypeRefFromActiveTypecheck(c, nodeId, outType, &handled) != 0) {
                return -1;
            }
            if (handled) {
                NormalizeCoreRuntimeTypeName(outType);
                return 0;
            }
            if (SliceEq(c->unit->source, n->dataStart, n->dataEnd, "rawptr")) {
                TypeRefSetScalar(outType, "void");
                outType->ptrDepth = 1;
                return 0;
            }
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
                    SetDiagNode(c, nodeId, H2Diag_CODEGEN_INTERNAL);
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
                char* normalizedDouble = NULL;
                const char* name = ResolveTypeNameInScope(c, nodeId, n->dataStart, n->dataEnd);
                if (normalized == NULL) {
                    SetDiagNode(c, nodeId, H2Diag_ARENA_OOM);
                    return -1;
                }
                if (!IsBuiltinType(normalized)) {
                    const H2NameMap* map = FindNameByCName(c, name);
                    if (map == NULL || !IsTypeDeclKind(map->kind)) {
                        map = FindNameByCString(c, normalized);
                    }
                    int      hasDot = 0;
                    uint32_t p;
                    for (p = n->dataStart; p < n->dataEnd; p++) {
                        if (c->unit->source[p] == '.') {
                            hasDot = 1;
                            break;
                        }
                    }
                    if ((map == NULL || !IsTypeDeclKind(map->kind)) && hasDot) {
                        normalizedDouble = DupAndReplaceDotsDouble(
                            c, c->unit->source, n->dataStart, n->dataEnd);
                        if (normalizedDouble == NULL) {
                            SetDiagNode(c, nodeId, H2Diag_ARENA_OOM);
                            return -1;
                        }
                        map = FindNameByCString(c, normalizedDouble);
                    }
                    if (map == NULL || !IsTypeDeclKind(map->kind)) {
                        SetDiag(c->diag, H2Diag_UNKNOWN_TYPE, n->dataStart, n->dataEnd);
                        return -1;
                    }
                }
                if (name == NULL) {
                    SetDiagNode(c, nodeId, H2Diag_CODEGEN_INTERNAL);
                    return -1;
                }
                TypeRefSetScalar(outType, name);
                NormalizeCoreRuntimeTypeName(outType);
            }
            return 0;
        }
        case H2Ast_TYPE_ANON_STRUCT:
        case H2Ast_TYPE_ANON_UNION:  {
            int32_t     fieldNode = AstFirstChild(&c->ast, nodeId);
            const char* fieldNames[256];
            H2TypeRef   fieldTypes[256];
            uint32_t    fieldCount = 0;
            const char* anonName;
            while (fieldNode >= 0) {
                const H2AstNode* field = NodeAt(c, fieldNode);
                int32_t          typeNode;
                if (field == NULL || field->kind != H2Ast_FIELD) {
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
                    n->kind == H2Ast_TYPE_ANON_UNION,
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
        case H2Ast_TYPE_PTR:
        case H2Ast_TYPE_REF:
        case H2Ast_TYPE_MUTREF: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            H2TypeRef childType;
            int       isReadOnlyRef = (n->kind == H2Ast_TYPE_REF);
            int       isRef = (n->kind == H2Ast_TYPE_REF || n->kind == H2Ast_TYPE_MUTREF);
            if (ParseTypeRef(c, child, &childType) != 0) {
                return -1;
            }
            if (childType.containerKind == H2TypeContainer_ARRAY
                || childType.containerKind == H2TypeContainer_SLICE_RO
                || childType.containerKind == H2TypeContainer_SLICE_MUT)
            {
                childType.containerPtrDepth++;
                if (childType.containerKind == H2TypeContainer_SLICE_RO
                    || childType.containerKind == H2TypeContainer_SLICE_MUT)
                {
                    childType.containerKind =
                        isRef ? H2TypeContainer_SLICE_RO : H2TypeContainer_SLICE_MUT;
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
        case H2Ast_TYPE_ARRAY: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            int32_t   lenNode = AstNextSibling(&c->ast, child);
            H2TypeRef elemType;
            int64_t   lenValue = 0;
            int       lenIsConst = 0;
            uint32_t  len = 0;
            if (ParseTypeRef(c, child, &elemType) != 0) {
                return -1;
            }
            if (elemType.containerKind != H2TypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            if (lenNode < 0) {
                if (ParseArrayLenLiteral(c->unit->source, n->dataStart, n->dataEnd, &len) != 0) {
                    SetDiagNode(c, nodeId, H2Diag_CODEGEN_INTERNAL);
                    TypeRefSetInvalid(outType);
                    return -1;
                }
            } else {
                if (EvalConstIntExpr(c, lenNode, &lenValue, &lenIsConst) != 0 || !lenIsConst
                    || lenValue < 0 || lenValue > (int64_t)UINT32_MAX)
                {
                    SetDiagNode(c, lenNode, H2Diag_CODEGEN_INTERNAL);
                    TypeRefSetInvalid(outType);
                    return -1;
                }
                len = (uint32_t)lenValue;
            }
            elemType.containerKind = H2TypeContainer_ARRAY;
            elemType.containerPtrDepth = 0;
            elemType.arrayLen = len;
            elemType.hasArrayLen = 1;
            elemType.readOnly = 0;
            *outType = elemType;
            return 0;
        }
        case H2Ast_TYPE_VARRAY: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            H2TypeRef elemType;
            if (ParseTypeRef(c, child, &elemType) != 0) {
                return -1;
            }
            if (elemType.containerKind != H2TypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.ptrDepth++;
            *outType = elemType;
            return 0;
        }
        case H2Ast_TYPE_SLICE:
        case H2Ast_TYPE_MUTSLICE: {
            int32_t   child = AstFirstChild(&c->ast, nodeId);
            H2TypeRef elemType;
            if (ParseTypeRef(c, child, &elemType) != 0) {
                return -1;
            }
            if (elemType.containerKind != H2TypeContainer_SCALAR) {
                TypeRefSetInvalid(outType);
                return -1;
            }
            elemType.containerKind =
                n->kind == H2Ast_TYPE_MUTSLICE
                    ? H2TypeContainer_SLICE_MUT
                    : H2TypeContainer_SLICE_RO;
            elemType.containerPtrDepth = 0;
            elemType.hasArrayLen = 0;
            elemType.arrayLen = 0;
            elemType.readOnly = n->kind == H2Ast_TYPE_MUTSLICE ? 0 : 1;
            *outType = elemType;
            return 0;
        }
        case H2Ast_TYPE_OPTIONAL: {
            int32_t child = AstFirstChild(&c->ast, nodeId);
            if (ParseTypeRef(c, child, outType) != 0) {
                return -1;
            }
            outType->isOptional = 1;
            {
                H2TypeRef lowered;
                if (TypeRefLowerForStorage(c, outType, &lowered) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        case H2Ast_TYPE_FN: {
            int32_t     child = AstFirstChild(&c->ast, nodeId);
            H2TypeRef   returnType;
            H2TypeRef*  paramTypes = NULL;
            uint32_t    paramLen = 0;
            uint32_t    paramCap = 0;
            int         isVariadic = 0;
            const char* aliasName;
            TypeRefSetScalar(&returnType, "void");
            while (child >= 0) {
                const H2AstNode* ch = NodeAt(c, child);
                if (ch != NULL && ch->flags == 1) {
                    if (ParseTypeRef(c, child, &returnType) != 0) {
                        return -1;
                    }
                } else {
                    H2TypeRef paramType;
                    if (ParseTypeRef(c, child, &paramType) != 0) {
                        return -1;
                    }
                    if (ch != NULL && (ch->flags & H2AstFlag_PARAM_VARIADIC) != 0) {
                        if (isVariadic) {
                            return -1;
                        }
                        if (paramType.containerKind != H2TypeContainer_SCALAR) {
                            return -1;
                        }
                        paramType.containerKind = H2TypeContainer_SLICE_RO;
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
                                sizeof(H2TypeRef),
                                (uint32_t)_Alignof(H2TypeRef))
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
        case H2Ast_TYPE_TUPLE: {
            int32_t     child = AstFirstChild(&c->ast, nodeId);
            const char* fieldNames[256];
            H2TypeRef   fieldTypes[256];
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
    H2CBackendC* c,
    const char*  hopName,
    const char*  baseCName,
    int32_t      nodeId,
    H2TypeRef    returnType,
    H2TypeRef* _Nullable paramTypes,
    char** _Nullable paramNames,
    uint8_t* _Nullable paramFlags,
    uint32_t  paramLen,
    int       isVariadic,
    int       hasContext,
    H2TypeRef contextType,
    uint16_t  sigFlags,
    uint32_t  tcFuncIndex,
    uint32_t  packArgStart,
    uint32_t  packArgCount,
    char* _Nullable packParamName) {
    const char* cName = baseCName;
    uint32_t    i;

    for (i = 0; i < c->fnSigLen; i++) {
        uint32_t p;
        int      sameSig = 1;
        if (!StrEq(c->fnSigs[i].hopName, hopName) || c->fnSigs[i].paramLen != paramLen
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
            if (((c->fnSigs[i].paramFlags != NULL) ? c->fnSigs[i].paramFlags[p] : 0u)
                != ((paramFlags != NULL) ? paramFlags[p] : 0u))
            {
                sameSig = 0;
                break;
            }
        }
        if (sameSig) {
            uint32_t idx;
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
                    sizeof(H2FnNodeName),
                    (uint32_t)_Alignof(H2FnNodeName))
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
                H2Buf    b = { 0 };
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
            sizeof(H2FnSig),
            (uint32_t)_Alignof(H2FnSig))
        != 0)
    {
        return -1;
    }
    c->fnSigs[c->fnSigLen].hopName = (char*)hopName;
    c->fnSigs[c->fnSigLen].cName = (char*)cName;
    c->fnSigs[c->fnSigLen].nodeId = nodeId;
    c->fnSigs[c->fnSigLen].tcFuncIndex = tcFuncIndex;
    c->fnSigs[c->fnSigLen].returnType = returnType;
    c->fnSigs[c->fnSigLen].paramTypes = paramTypes;
    c->fnSigs[c->fnSigLen].paramNames = paramNames;
    c->fnSigs[c->fnSigLen].paramFlags = paramFlags;
    c->fnSigs[c->fnSigLen].paramLen = paramLen;
    c->fnSigs[c->fnSigLen].packArgStart = packArgStart;
    c->fnSigs[c->fnSigLen].packArgCount = packArgCount;
    c->fnSigs[c->fnSigLen].packParamName = packParamName;
    c->fnSigs[c->fnSigLen].flags = sigFlags;
    c->fnSigs[c->fnSigLen].hasContext = hasContext;
    c->fnSigs[c->fnSigLen].contextType = contextType;
    c->fnSigs[c->fnSigLen].isVariadic = (uint8_t)(isVariadic ? 1 : 0);
    c->fnSigs[c->fnSigLen]._reserved[0] = 0;
    c->fnSigLen++;

    if (EnsureCapArena(
            &c->arena,
            (void**)&c->fnNodeNames,
            &c->fnNodeNameCap,
            c->fnNodeNameLen + 1u,
            sizeof(H2FnNodeName),
            (uint32_t)_Alignof(H2FnNodeName))
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
    H2CBackendC* c,
    const char*  ownerType,
    const char*  fieldName,
    const char* _Nullable lenFieldName,
    int32_t   defaultExprNode,
    int       isDependent,
    int       isEmbedded,
    H2TypeRef type) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->fieldInfos,
            &c->fieldInfoCap,
            c->fieldInfoLen + 1u,
            sizeof(H2FieldInfo),
            (uint32_t)_Alignof(H2FieldInfo))
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

int AppendTypeRefKey(H2Buf* b, const H2TypeRef* t) {
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

const H2AnonTypeInfo* _Nullable FindAnonTypeByKey(const H2CBackendC* c, const char* key) {
    uint32_t i;
    for (i = 0; i < c->anonTypeLen; i++) {
        if (StrEq(c->anonTypes[i].key, key)) {
            return &c->anonTypes[i];
        }
    }
    return NULL;
}

const H2AnonTypeInfo* _Nullable FindAnonTypeByCName(const H2CBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->anonTypeLen; i++) {
        if (StrEq(c->anonTypes[i].cName, cName)) {
            return &c->anonTypes[i];
        }
    }
    return NULL;
}

int IsTupleFieldName(const char* name, uint32_t index) {
    const char* prefix = "__hop_t";
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

int TypeRefTupleInfo(const H2CBackendC* c, const H2TypeRef* t, const H2AnonTypeInfo** outInfo) {
    const H2AnonTypeInfo* info;
    uint32_t              i;
    if (outInfo != NULL) {
        *outInfo = NULL;
    }
    if (t == NULL || !t->valid || t->baseName == NULL || t->ptrDepth != 0 || t->isOptional
        || t->containerKind != H2TypeContainer_SCALAR || t->containerPtrDepth != 0)
    {
        return 0;
    }
    info = FindAnonTypeByCName(c, t->baseName);
    if (info == NULL || info->isUnion || info->fieldCount < 2u) {
        return 0;
    }
    for (i = 0; i < info->fieldCount; i++) {
        const H2FieldInfo* f = &c->fieldInfos[info->fieldStart + i];
        if (!IsTupleFieldName(f->fieldName, i)) {
            return 0;
        }
    }
    if (outInfo != NULL) {
        *outInfo = info;
    }
    return 1;
}

int IsLocalAnonTypedefVisible(const H2CBackendC* c, const char* cName) {
    uint32_t i = c->localAnonTypedefLen;
    while (i > 0) {
        i--;
        if (StrEq(c->localAnonTypedefs[i], cName)) {
            return 1;
        }
    }
    return 0;
}

int MarkLocalAnonTypedefVisible(H2CBackendC* c, const char* cName) {
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

int IsAnonTypeNameVisible(const H2CBackendC* c, const char* cName) {
    const H2AnonTypeInfo* info = FindAnonTypeByCName(c, cName);
    if (info != NULL && (info->flags & H2AnonTypeFlag_EMITTED_GLOBAL) != 0) {
        return 1;
    }
    return IsLocalAnonTypedefVisible(c, cName);
}

int EmitAnonTypeDeclAtDepth(H2CBackendC* c, const H2AnonTypeInfo* t, uint32_t depth) {
    uint32_t j;
    EmitIndent(c, depth);
    if (BufAppendCStr(&c->out, "typedef ") != 0
        || BufAppendCStr(&c->out, t->isUnion ? "union " : "struct ") != 0
        || BufAppendCStr(&c->out, t->cName) != 0 || BufAppendCStr(&c->out, " {\n") != 0)
    {
        return -1;
    }
    for (j = 0; j < t->fieldCount; j++) {
        const H2FieldInfo* f = &c->fieldInfos[t->fieldStart + j];
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

int EnsureAnonTypeVisible(H2CBackendC* c, const H2TypeRef* type, uint32_t depth) {
    const H2AnonTypeInfo* info;
    uint32_t              i;
    if (type == NULL || !type->valid || type->containerKind != H2TypeContainer_SCALAR
        || type->containerPtrDepth != 0 || type->ptrDepth != 0 || type->isOptional
        || type->baseName == NULL)
    {
        return 0;
    }
    info = FindAnonTypeByCName(c, type->baseName);
    if (info == NULL) {
        return 0;
    }
    if ((info->flags & H2AnonTypeFlag_EMITTED_GLOBAL) != 0
        || IsLocalAnonTypedefVisible(c, info->cName))
    {
        return 0;
    }
    for (i = 0; i < info->fieldCount; i++) {
        const H2FieldInfo* f = &c->fieldInfos[info->fieldStart + i];
        if (EnsureAnonTypeVisible(c, &f->type, depth) != 0) {
            return -1;
        }
    }
    if (EmitAnonTypeDeclAtDepth(c, info, depth) != 0) {
        return -1;
    }
    if (depth == 0) {
        ((H2AnonTypeInfo*)info)->flags |= H2AnonTypeFlag_EMITTED_GLOBAL;
        if (BufAppendChar(&c->out, '\n') != 0) {
            return -1;
        }
    } else if (MarkLocalAnonTypedefVisible(c, info->cName) != 0) {
        return -1;
    }
    return 0;
}

int EnsureAnonTypeByFields(
    H2CBackendC*     c,
    int              isUnion,
    const char**     fieldNames,
    const H2TypeRef* fieldTypes,
    uint32_t         fieldCount,
    const char**     outCName) {
    uint32_t i;
    H2Buf    keyBuf = { 0 };
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
        const H2AnonTypeInfo* info = FindAnonTypeByKey(c, key);
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
            sizeof(H2AnonTypeInfo),
            (uint32_t)_Alignof(H2AnonTypeInfo))
        != 0)
    {
        return -1;
    }

    {
        H2Buf    cNameBuf = { 0 };
        char*    cName;
        uint32_t fieldStart = c->fieldInfoLen;
        cNameBuf.arena = &c->arena;
        if (BufAppendCStr(&cNameBuf, "__hop_anon_") != 0
            || (c->unit != NULL && c->unit->packageName != NULL
                && (BufAppendCStr(&cNameBuf, c->unit->packageName) != 0
                    || BufAppendChar(&cNameBuf, '_') != 0))
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

const H2FnSig* _Nullable FindFnSigBySlice(const H2CBackendC* c, uint32_t start, uint32_t end) {
    const H2FnSig* found = NULL;
    uint32_t       i;
    for (i = 0; i < c->fnSigLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->fnSigs[i].hopName)) {
            if (found != NULL) {
                return NULL;
            }
            found = &c->fnSigs[i];
        }
    }
    return found;
}

uint32_t FindFnSigCandidatesBySlice(
    const H2CBackendC* c, uint32_t start, uint32_t end, const H2FnSig** out, uint32_t cap) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < c->fnSigLen; i++) {
        if (SliceEqName(c->unit->source, start, end, c->fnSigs[i].hopName)) {
            if (n < cap) {
                out[n] = &c->fnSigs[i];
            }
            n++;
        }
    }
    return n;
}

uint32_t FindFnSigCandidatesByName(
    const H2CBackendC* c, const char* hopName, const H2FnSig** out, uint32_t cap) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < c->fnSigLen; i++) {
        if (StrEq(c->fnSigs[i].hopName, hopName)) {
            if (n < cap) {
                out[n] = &c->fnSigs[i];
            }
            n++;
        }
    }
    return n;
}

const char* _Nullable FindFnCNameByNodeId(const H2CBackendC* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->fnNodeNameLen; i++) {
        if (c->fnNodeNames[i].nodeId == nodeId) {
            return c->fnNodeNames[i].cName;
        }
    }
    return NULL;
}

const H2FnSig* _Nullable FindFnSigByNodeId(const H2CBackendC* c, int32_t nodeId) {
    uint32_t i;
    for (i = 0; i < c->fnSigLen; i++) {
        if (c->fnSigs[i].nodeId == nodeId
            && (c->fnSigs[i].flags & H2FnSigFlag_TEMPLATE_INSTANCE) == 0)
        {
            return &c->fnSigs[i];
        }
    }
    for (i = 0; i < c->fnSigLen; i++) {
        if (c->fnSigs[i].nodeId == nodeId) {
            return &c->fnSigs[i];
        }
    }
    {
        const char* cName = FindFnCNameByNodeId(c, nodeId);
        if (cName != NULL) {
            for (i = 0; i < c->fnSigLen; i++) {
                if (StrEq(c->fnSigs[i].cName, cName)) {
                    return &c->fnSigs[i];
                }
            }
        }
    }
    return NULL;
}

uint32_t FindFnSigCandidatesByNodeId(
    const H2CBackendC* c, int32_t nodeId, const H2FnSig** out, uint32_t cap) {
    uint32_t i;
    uint32_t n = 0;
    for (i = 0; i < c->fnSigLen; i++) {
        if (c->fnSigs[i].nodeId == nodeId) {
            if (n < cap) {
                out[n] = &c->fnSigs[i];
            }
            n++;
        }
    }
    return n;
}

const H2FieldInfo* _Nullable FindFieldInfo(
    const H2CBackendC* c, const char* ownerType, uint32_t fieldStart, uint32_t fieldEnd) {
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

const H2FieldInfo* _Nullable FindEmbeddedFieldInfo(const H2CBackendC* c, const char* ownerType) {
    uint32_t i;
    for (i = 0; i < c->fieldInfoLen; i++) {
        if (StrEq(c->fieldInfos[i].ownerType, ownerType) && c->fieldInfos[i].isEmbedded) {
            return &c->fieldInfos[i];
        }
    }
    return NULL;
}

const H2FieldInfo* _Nullable FindFieldInfoByName(
    const H2CBackendC* c, const char* ownerType, const char* fieldName) {
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
    const H2CBackendC* c, const char* _Nullable ownerType) {
    const char* canonical = ResolveScalarAliasBaseName(c, ownerType);
    if (canonical == NULL) {
        canonical = ownerType;
    }
    if (canonical == NULL) {
        return NULL;
    }
    if (StrEq(canonical, "__hop_MemAllocator")) {
        return "builtin__MemAllocator";
    }
    if (StrEq(canonical, "__hop_Logger")) {
        return "builtin__Logger";
    }
    if (StrEq(canonical, "__hop_Context")) {
        return "builtin__Context";
    }
    if (StrEq(canonical, "__hop_PrintContext")) {
        return "builtin__PrintContext";
    }
    return canonical;
}

int ResolveCoreStrFieldBySlice(
    const H2CBackendC* c, uint32_t fieldStart, uint32_t fieldEnd, const H2FieldInfo** outField) {
    static int         inited = 0;
    static H2FieldInfo lenField;
    static H2FieldInfo ptrField;
    if (!inited) {
        memset(&lenField, 0, sizeof(lenField));
        memset(&ptrField, 0, sizeof(ptrField));
        lenField.ownerType = "builtin__str";
        lenField.fieldName = "len";
        TypeRefSetScalar(&lenField.type, "__hop_int");

        ptrField.ownerType = "builtin__str";
        ptrField.fieldName = "ptr";
        TypeRefSetScalar(&ptrField.type, "__hop_u8");
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
    const H2CBackendC*  c,
    const char*         ownerTypeIn,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const H2FieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const H2FieldInfo** _Nullable outField) {
    const H2FieldInfo* direct;
    const H2FieldInfo* embedded;
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
        const H2FieldInfo* strField = NULL;
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
        || embedded->type.containerKind != H2TypeContainer_SCALAR || embedded->type.ptrDepth != 0
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
    const H2CBackendC*  c,
    const char*         ownerType,
    uint32_t            fieldStart,
    uint32_t            fieldEnd,
    const H2FieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen,
    const H2FieldInfo** _Nullable outField) {
    uint32_t           pos = fieldStart;
    uint32_t           totalLen = 0;
    const char*        curOwnerType = ownerType;
    const H2FieldInfo* lastField = NULL;

    while (pos <= fieldEnd) {
        const H2FieldInfo* segPath[64];
        const H2FieldInfo* segField = NULL;
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
            && segField->type.containerKind == H2TypeContainer_SCALAR
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
    const H2CBackendC*  c,
    const char*         srcTypeName,
    const char*         dstTypeName,
    const H2FieldInfo** outPath,
    uint32_t            cap,
    uint32_t*           outLen) {
    const H2FieldInfo* embedded;
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
            || embedded->type.containerKind != H2TypeContainer_SCALAR
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

static int TypeRefIsNamedPtr(const H2TypeRef* t, const char* name) {
    return t != NULL && t->valid && t->containerKind == H2TypeContainer_SCALAR && t->ptrDepth == 1
        && t->baseName != NULL && StrEq(t->baseName, name);
}

static int TypeRefIsNamedValue(const H2TypeRef* t, const char* name) {
    return t != NULL && t->valid && t->containerKind == H2TypeContainer_SCALAR && t->ptrDepth == 0
        && t->baseName != NULL && StrEq(t->baseName, name);
}

static int FnSigIsNoContextAbiCallback(
    const H2TypeRef* returnType, const H2TypeRef* paramTypes, uint32_t paramLen) {
    if (returnType == NULL || paramTypes == NULL) {
        return 0;
    }
    if (paramLen == 7
        && (TypeRefIsNamedPtr(&paramTypes[0], "builtin__MemAllocator")
            || TypeRefIsNamedPtr(&paramTypes[0], "__hop_MemAllocator")
            || TypeRefIsNamedPtr(&paramTypes[0], "MemAllocator")))
    {
        return 1;
    }
    if (paramLen == 4 && TypeRefIsNamedValue(returnType, "void")
        && (TypeRefIsNamedPtr(&paramTypes[0], "builtin__Logger")
            || TypeRefIsNamedPtr(&paramTypes[0], "__hop_Logger")
            || TypeRefIsNamedPtr(&paramTypes[0], "Logger")))
    {
        return 1;
    }
    return 0;
}

static int NodeSubtreeNeedsAmbientContext(H2CBackendC* c, int32_t nodeId) {
    int32_t          child;
    const H2AstNode* n = NodeAt(c, nodeId);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_NEW || n->kind == H2Ast_DEL || n->kind == H2Ast_CALL
        || n->kind == H2Ast_CALL_WITH_CONTEXT)
    {
        return 1;
    }
    if (n->kind == H2Ast_FOR && (n->flags & H2AstFlag_FOR_IN) != 0) {
        /* For-in lowering may synthesize iterator hook calls that need ambient context. */
        return 1;
    }
    if (n->kind == H2Ast_IDENT && SliceEq(c->unit->source, n->dataStart, n->dataEnd, "context")) {
        return 1;
    }
    child = AstFirstChild(&c->ast, nodeId);
    while (child >= 0) {
        if (NodeSubtreeNeedsAmbientContext(c, child)) {
            return 1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int CollectFnAndFieldInfoFromNode(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    uint32_t         nameStart;
    uint32_t         nameEnd;
    const H2NameMap* mapName;

    if (n == NULL || !IsDeclKind(n->kind) || GetDeclNameSpan(c, nodeId, &nameStart, &nameEnd) != 0)
    {
        return 0;
    }
    if (IsTypeDeclKind(n->kind)) {
        mapName = FindTypeDeclMapByNode(c, nodeId);
    } else {
        mapName = FindNameBySlice(c, nameStart, nameEnd);
    }
    if (mapName == NULL) {
        return 0;
    }

    if (n->kind == H2Ast_FN) {
        if (CodegenCNodeHasTypeParams(c, nodeId)) {
            return 0;
        }
        int32_t    child = AstFirstChild(&c->ast, nodeId);
        H2TypeRef  returnType;
        H2TypeRef* paramTypes = NULL;
        char**     paramNames = NULL;
        uint8_t*   paramFlags = NULL;
        uint32_t   paramLen = 0;
        uint32_t   paramTypeCap = 0;
        uint32_t   paramNameCap = 0;
        uint32_t   paramFlagCap = 0;
        H2TypeRef  contextType;
        int        isVariadic = 0;
        int        hasAnytypeParam = 0;
        int32_t    bodyNode = -1;
        int        hasContext = 0;
        TypeRefSetScalar(&returnType, "void");
        TypeRefSetInvalid(&contextType);
        if (ResolveMainSemanticContextType(c, &contextType) != 0) {
            return -1;
        }
        while (child >= 0) {
            const H2AstNode* ch = NodeAt(c, child);
            if (ch != NULL && ch->kind == H2Ast_PARAM) {
                int32_t          typeNode = AstFirstChild(&c->ast, child);
                H2TypeRef        paramType;
                int              isAnytypeParam = 0;
                uint8_t          pflags = 0;
                const H2AstNode* typeAst = NodeAt(c, typeNode);
                if (ParseTypeRef(c, typeNode, &paramType) != 0) {
                    return -1;
                }
                if (typeAst != NULL && typeAst->kind == H2Ast_TYPE_NAME
                    && SliceEq(c->unit->source, typeAst->dataStart, typeAst->dataEnd, "anytype"))
                {
                    isAnytypeParam = 1;
                    hasAnytypeParam = 1;
                }
                if ((ch->flags & H2AstFlag_PARAM_VARIADIC) != 0) {
                    if (isVariadic) {
                        return -1;
                    }
                    if (paramType.containerKind != H2TypeContainer_SCALAR) {
                        return -1;
                    }
                    paramType.containerKind = H2TypeContainer_SLICE_RO;
                    paramType.containerPtrDepth = 0;
                    paramType.hasArrayLen = 0;
                    paramType.arrayLen = 0;
                    paramType.readOnly = 1;
                    isVariadic = 1;
                    if (isAnytypeParam) {
                        pflags |= H2CCGParamFlag_ANYPACK;
                    }
                } else if (isVariadic) {
                    /* Variadic parameter must be the final parameter. */
                    return -1;
                }
                if (isAnytypeParam) {
                    pflags |= H2CCGParamFlag_ANYTYPE;
                }
                if (paramLen >= paramTypeCap) {
                    uint32_t need = paramLen + 1u;
                    if (EnsureCapArena(
                            &c->arena,
                            (void**)&paramTypes,
                            &paramTypeCap,
                            need,
                            sizeof(H2TypeRef),
                            (uint32_t)_Alignof(H2TypeRef))
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
                if ((ch->flags & H2AstFlag_PARAM_CONST) != 0) {
                    pflags |= H2CCGParamFlag_CONST;
                }
                paramFlags[paramLen - 1u] = pflags;
            } else if (
                ch != NULL
                && (ch->kind == H2Ast_TYPE_NAME || ch->kind == H2Ast_TYPE_PTR
                    || ch->kind == H2Ast_TYPE_REF || ch->kind == H2Ast_TYPE_MUTREF
                    || ch->kind == H2Ast_TYPE_ARRAY || ch->kind == H2Ast_TYPE_VARRAY
                    || ch->kind == H2Ast_TYPE_SLICE || ch->kind == H2Ast_TYPE_MUTSLICE
                    || ch->kind == H2Ast_TYPE_OPTIONAL || ch->kind == H2Ast_TYPE_FN
                    || ch->kind == H2Ast_TYPE_ANON_STRUCT || ch->kind == H2Ast_TYPE_ANON_UNION
                    || ch->kind == H2Ast_TYPE_TUPLE)
                && ch->flags == 1)
            {
                if (ParseTypeRef(c, child, &returnType) != 0) {
                    return -1;
                }
            } else if (ch != NULL && ch->kind == H2Ast_BLOCK) {
                bodyNode = child;
            } else if (ch != NULL && ch->kind == H2Ast_CONTEXT_CLAUSE) {
                int32_t typeNode = AstFirstChild(&c->ast, child);
                if (typeNode < 0 || ParseTypeRef(c, typeNode, &contextType) != 0) {
                    return -1;
                }
            }
            child = AstNextSibling(&c->ast, child);
        }
        hasContext = (bodyNode >= 0 && NodeSubtreeNeedsAmbientContext(c, bodyNode))
                  || StrHasPrefix(mapName->cName, "platform__");
        if (FnSigIsNoContextAbiCallback(&returnType, paramTypes, paramLen)) {
            hasContext = 0;
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
            contextType,
            hasAnytypeParam ? H2FnSigFlag_TEMPLATE_BASE : 0u,
            UINT32_MAX,
            0u,
            0u,
            NULL);
    }

    if (n->kind == H2Ast_STRUCT || n->kind == H2Ast_UNION) {
        if (CodegenCNodeHasTypeParams(c, nodeId)) {
            return 0;
        }
        int32_t child = AstFirstChild(&c->ast, nodeId);
        while (child >= 0) {
            const H2AstNode* field = NodeAt(c, child);
            if (field != NULL && field->kind == H2Ast_FIELD) {
                int32_t     typeNode = AstFirstChild(&c->ast, child);
                int32_t     defaultExprNode = -1;
                const char* lenFieldName = NULL;
                int         isDependent = 0;
                int         isEmbedded = (field->flags & H2AstFlag_FIELD_EMBEDDED) != 0;
                H2TypeRef   fieldType;
                char*       fieldName;
                if (ParseTypeRef(c, typeNode, &fieldType) != 0) {
                    return -1;
                }
                if (typeNode >= 0) {
                    defaultExprNode = AstNextSibling(&c->ast, typeNode);
                }
                if (typeNode >= 0 && NodeAt(c, typeNode) != NULL
                    && NodeAt(c, typeNode)->kind == H2Ast_TYPE_VARRAY)
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

char* _Nullable BuildTemplateInstanceCName(
    H2CBackendC* c, const char* baseCName, uint32_t tcFuncIndex) {
    H2Buf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, baseCName) != 0 || BufAppendCStr(&b, "__ti") != 0
        || BufAppendU32(&b, tcFuncIndex) != 0)
    {
        return NULL;
    }
    return BufFinish(&b);
}

char* _Nullable BuildTemplateNamedTypeCName(
    H2CBackendC* c, const char* baseCName, uint32_t tcNamedIndex) {
    H2Buf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, baseCName) != 0 || BufAppendCStr(&b, "__tn") != 0
        || BufAppendU32(&b, tcNamedIndex) != 0)
    {
        return NULL;
    }
    return BufFinish(&b);
}

char* _Nullable DupParamNameFromSpanOrDefault(
    H2CBackendC* c, uint32_t start, uint32_t end, uint32_t index) {
    if (end > start) {
        char* n = DupSlice(c, c->unit->source, start, end);
        if (n != NULL && n[0] != '\0') {
            return n;
        }
    }
    {
        H2Buf b = { 0 };
        b.arena = &c->arena;
        if (BufAppendCStr(&b, "__hop_p") != 0 || BufAppendU32(&b, index) != 0) {
            return NULL;
        }
        return BufFinish(&b);
    }
}

char* _Nullable BuildExpandedPackElemName(H2CBackendC* c, const char* packName, uint32_t index) {
    H2Buf b = { 0 };
    b.arena = &c->arena;
    if (BufAppendCStr(&b, packName) != 0 || BufAppendCStr(&b, "__") != 0
        || BufAppendU32(&b, index) != 0)
    {
        return NULL;
    }
    return BufFinish(&b);
}

int CollectTemplateInstanceFnSigs(H2CBackendC* c) {
    const H2TypeCheckCtx* tc;
    uint32_t              i;
    if (c == NULL || c->constEval == NULL) {
        return 0;
    }
    tc = &c->constEval->tc;
    for (i = 0; i < tc->funcLen; i++) {
        const H2TCFunction* fn = &tc->funcs[i];
        int32_t             nodeId;
        const H2AstNode*    fnNode;
        const H2NameMap*    map;
        char*               cName;
        H2TypeRef           returnType;
        H2TypeRef           contextType;
        H2TypeRef*          paramTypes = NULL;
        char**              paramNames = NULL;
        uint8_t*            paramFlags = NULL;
        uint32_t            paramLen = 0;
        uint32_t            paramTypeCap = 0;
        uint32_t            paramNameCap = 0;
        uint32_t            paramFlagCap = 0;
        uint32_t            packArgStart = 0;
        uint32_t            packArgCount = 0;
        char*               packParamName = NULL;
        int                 isVariadic = (fn->flags & H2TCFunctionFlag_VARIADIC) != 0;
        int                 hasContext = 1;
        uint32_t            p;

        if ((fn->flags & H2TCFunctionFlag_TEMPLATE_INSTANCE) == 0) {
            continue;
        }
        if (CodegenCTypeIdContainsTypeParam(tc, fn->returnType)
            || (fn->contextType >= 0 && CodegenCTypeIdContainsTypeParam(tc, fn->contextType)))
        {
            continue;
        }
        nodeId = fn->defNode >= 0 ? fn->defNode : fn->declNode;
        fnNode = NodeAt(c, nodeId);
        if (fnNode == NULL || fnNode->kind != H2Ast_FN) {
            return -1;
        }
        map = FindNameBySlice(c, fnNode->dataStart, fnNode->dataEnd);
        if (map == NULL) {
            return -1;
        }
        if (ParseTypeRefFromConstEvalTypeId(c, fn->returnType, &returnType) != 0) {
            continue;
        }
        if (ResolveMainSemanticContextType(c, &contextType) != 0) {
            continue;
        }
        cName = BuildTemplateInstanceCName(c, map->cName, i);
        if (cName == NULL) {
            return -1;
        }

        for (p = 0; p < fn->paramCount; p++) {
            int32_t  typeId = tc->funcParamTypes[fn->paramTypeStart + p];
            uint8_t  pflags = 0;
            uint32_t nameStart = tc->funcParamNameStarts[fn->paramTypeStart + p];
            uint32_t nameEnd = tc->funcParamNameEnds[fn->paramTypeStart + p];
            if ((tc->funcParamFlags[fn->paramTypeStart + p] & H2TCFuncParamFlag_CONST) != 0) {
                pflags |= H2CCGParamFlag_CONST;
            }
            if (CodegenCTypeIdContainsTypeParam(tc, typeId)) {
                paramLen = 0;
                break;
            }
            if (isVariadic && p + 1u == fn->paramCount && typeId >= 0
                && (uint32_t)typeId < tc->typeLen && tc->types[typeId].kind == H2TCType_PACK)
            {
                const H2TCType* packType = &tc->types[typeId];
                uint32_t        k;
                isVariadic = 0;
                packArgStart = paramLen;
                packArgCount = packType->fieldCount;
                packParamName = DupParamNameFromSpanOrDefault(c, nameStart, nameEnd, p);
                if (packParamName == NULL) {
                    return -1;
                }
                for (k = 0; k < packType->fieldCount; k++) {
                    int32_t   elemTypeId = tc->funcParamTypes[packType->fieldStart + k];
                    H2TypeRef paramType;
                    char*     elemName;
                    if (ParseTypeRefFromConstEvalTypeId(c, elemTypeId, &paramType) != 0) {
                        break;
                    }
                    elemName = BuildExpandedPackElemName(c, packParamName, k);
                    if (elemName == NULL) {
                        return -1;
                    }
                    if (paramLen >= paramTypeCap || paramLen >= paramNameCap
                        || paramLen >= paramFlagCap)
                    {
                        uint32_t need = paramLen + 1u;
                        if (EnsureCapArena(
                                &c->arena,
                                (void**)&paramTypes,
                                &paramTypeCap,
                                need,
                                sizeof(H2TypeRef),
                                (uint32_t)_Alignof(H2TypeRef))
                                != 0
                            || EnsureCapArena(
                                   &c->arena,
                                   (void**)&paramNames,
                                   &paramNameCap,
                                   need,
                                   sizeof(char*),
                                   (uint32_t)_Alignof(char*))
                                   != 0
                            || EnsureCapArena(
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
                    paramTypes[paramLen] = paramType;
                    paramNames[paramLen] = elemName;
                    paramFlags[paramLen] = pflags;
                    paramLen++;
                }
                if (k != packType->fieldCount) {
                    continue;
                }
                continue;
            }

            {
                H2TypeRef paramType;
                char*     paramName;
                if (ParseTypeRefFromConstEvalTypeId(c, typeId, &paramType) != 0) {
                    paramLen = 0;
                    break;
                }
                paramName = DupParamNameFromSpanOrDefault(c, nameStart, nameEnd, p);
                if (paramName == NULL) {
                    return -1;
                }
                if (paramLen >= paramTypeCap || paramLen >= paramNameCap
                    || paramLen >= paramFlagCap)
                {
                    uint32_t need = paramLen + 1u;
                    if (EnsureCapArena(
                            &c->arena,
                            (void**)&paramTypes,
                            &paramTypeCap,
                            need,
                            sizeof(H2TypeRef),
                            (uint32_t)_Alignof(H2TypeRef))
                            != 0
                        || EnsureCapArena(
                               &c->arena,
                               (void**)&paramNames,
                               &paramNameCap,
                               need,
                               sizeof(char*),
                               (uint32_t)_Alignof(char*))
                               != 0
                        || EnsureCapArena(
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
                paramTypes[paramLen] = paramType;
                paramNames[paramLen] = paramName;
                paramFlags[paramLen] = pflags;
                paramLen++;
            }
        }
        if (paramLen == 0 && fn->paramCount > 0) {
            continue;
        }

        if (AddFnSig(
                c,
                map->name,
                cName,
                nodeId,
                returnType,
                paramTypes,
                paramNames,
                paramFlags,
                paramLen,
                isVariadic,
                hasContext,
                contextType,
                (uint16_t)(H2FnSigFlag_TEMPLATE_INSTANCE
                           | (packParamName != NULL ? H2FnSigFlag_EXPANDED_ANYPACK : 0u)),
                i,
                packArgStart,
                packArgCount,
                packParamName)
            != 0)
        {
            return -1;
        }
    }
    return 0;
}

int CollectTemplateInstanceNamedTypes(H2CBackendC* c) {
    H2TypeCheckCtx* tc;
    uint32_t        i;
    if (c == NULL || c->constEval == NULL) {
        return 0;
    }
    tc = &c->constEval->tc;
    for (i = 0; i < tc->namedTypeLen; i++) {
        const H2TCNamedType* nt = &tc->namedTypes[i];
        const H2AstNode*     decl;
        const H2NameMap*     rootMap;
        char*                cName;
        int32_t              fieldNode;
        uint32_t             savedArgStart = tc->activeGenericArgStart;
        uint16_t             savedArgCount = tc->activeGenericArgCount;
        int32_t              savedDeclNode = tc->activeGenericDeclNode;
        uint32_t             savedFuncIndex = c->activeTcFuncIndex;
        int32_t              savedNamedTypeIndex = c->activeTcNamedTypeIndex;
        if (nt->templateRootNamedIndex < 0 || nt->templateArgCount == 0) {
            continue;
        }
        if (nt->typeId < 0 || (uint32_t)nt->typeId >= tc->typeLen) {
            continue;
        }
        if (CodegenCTypeIdContainsTypeParam(tc, nt->typeId)) {
            continue;
        }
        decl = NodeAt(c, nt->declNode);
        if (decl == NULL || (decl->kind != H2Ast_STRUCT && decl->kind != H2Ast_UNION)) {
            continue;
        }
        rootMap = FindTypeDeclMapByNode(c, nt->declNode);
        if (rootMap == NULL) {
            return -1;
        }
        cName = BuildTemplateNamedTypeCName(c, rootMap->cName, i);
        if (cName == NULL) {
            return -1;
        }
        if (AddNameLiteral(c, cName, decl->kind, 0, 0) != 0) {
            return -1;
        }
        if (CodegenCPushActiveNamedTypeContext(c, i) != 0) {
            return -1;
        }
        fieldNode = AstFirstChild(&c->ast, nt->declNode);
        while (fieldNode >= 0) {
            const H2AstNode* field = NodeAt(c, fieldNode);
            int32_t          typeNode;
            int32_t          defaultExprNode;
            H2TypeRef        fieldType;
            char*            fieldName;
            char*            lenFieldName = NULL;
            int              isDependent = 0;
            int              isEmbedded = 0;
            if (field == NULL || field->kind != H2Ast_FIELD) {
                fieldNode = AstNextSibling(&c->ast, fieldNode);
                continue;
            }
            typeNode = AstFirstChild(&c->ast, fieldNode);
            defaultExprNode = typeNode >= 0 ? AstNextSibling(&c->ast, typeNode) : -1;
            isEmbedded = (field->flags & H2AstFlag_FIELD_EMBEDDED) != 0;
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
            if (typeNode >= 0 && NodeAt(c, typeNode) != NULL
                && NodeAt(c, typeNode)->kind == H2Ast_TYPE_VARRAY)
            {
                lenFieldName = DupSlice(
                    c,
                    c->unit->source,
                    NodeAt(c, typeNode)->dataStart,
                    NodeAt(c, typeNode)->dataEnd);
                isDependent = 1;
                if (lenFieldName == NULL) {
                    CodegenCPopActiveTypeContext(
                        c,
                        savedFuncIndex,
                        savedNamedTypeIndex,
                        savedArgStart,
                        savedArgCount,
                        savedDeclNode);
                    return -1;
                }
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
            if (AddFieldInfo(
                    c,
                    cName,
                    fieldName,
                    lenFieldName,
                    defaultExprNode,
                    isDependent,
                    isEmbedded,
                    fieldType)
                != 0)
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
    }
    return 0;
}

int CollectFnAndFieldInfo(H2CBackendC* c) {
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
    if (CollectTemplateInstanceFnSigs(c) != 0) {
        return -1;
    }
    if (CollectTemplateInstanceNamedTypes(c) != 0) {
        return -1;
    }
    return 0;
}

int CollectTypeAliasInfo(H2CBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
        const H2NameMap* map;
        int32_t          targetNode;
        H2TypeRef        targetType;
        if (n == NULL || n->kind != H2Ast_TYPE_ALIAS) {
            continue;
        }
        map = FindTypeDeclMapByNode(c, nodeId);
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

int CollectFnTypeAliasesFromNode(H2CBackendC* c, int32_t nodeId) {
    const H2AstNode* n = NodeAt(c, nodeId);
    int32_t          child;
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_TYPE_FN) {
        H2TypeRef ignoredType;
        if (ParseTypeRef(c, nodeId, &ignoredType) != 0) {
            SetDiagNode(c, nodeId, H2Diag_CODEGEN_INTERNAL);
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

int CollectFnTypeAliases(H2CBackendC* c) {
    return CollectFnTypeAliasesFromNode(c, c->ast.root);
}

int AddVarSizeType(H2CBackendC* c, const char* cName, int isUnion) {
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
            sizeof(H2VarSizeType),
            (uint32_t)_Alignof(H2VarSizeType))
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

H2VarSizeType* _Nullable FindVarSizeType(H2CBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->varSizeTypeLen; i++) {
        if (StrEq(c->varSizeTypes[i].cName, cName)) {
            return &c->varSizeTypes[i];
        }
    }
    return NULL;
}

int CollectVarSizeTypesFromDeclSets(H2CBackendC* c) {
    uint32_t i;
    for (i = 0; i < c->pubDeclLen; i++) {
        int32_t          nodeId = c->pubDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
        const H2NameMap* map;
        if (n == NULL || (n->kind != H2Ast_STRUCT && n->kind != H2Ast_UNION)) {
            continue;
        }
        map = FindTypeDeclMapByNode(c, nodeId);
        if (map == NULL) {
            continue;
        }
        if (AddVarSizeType(c, map->cName, n->kind == H2Ast_UNION) != 0) {
            return -1;
        }
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
        const H2NameMap* map;
        if (n == NULL || (n->kind != H2Ast_STRUCT && n->kind != H2Ast_UNION)) {
            continue;
        }
        map = FindTypeDeclMapByNode(c, nodeId);
        if (map == NULL) {
            continue;
        }
        if (AddVarSizeType(c, map->cName, n->kind == H2Ast_UNION) != 0) {
            return -1;
        }
    }
    return 0;
}

int IsVarSizeTypeName(const H2CBackendC* c, const char* cName) {
    uint32_t i;
    for (i = 0; i < c->varSizeTypeLen; i++) {
        if (StrEq(c->varSizeTypes[i].cName, cName)) {
            return c->varSizeTypes[i].isVarSize;
        }
    }
    return 0;
}

int PropagateVarSizeTypes(H2CBackendC* c) {
    int changed = 1;
    while (changed) {
        uint32_t i;
        changed = 0;
        for (i = 0; i < c->fieldInfoLen; i++) {
            H2FieldInfo*   field = &c->fieldInfos[i];
            H2VarSizeType* owner;
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
                && (field->type.containerKind == H2TypeContainer_SCALAR
                    || field->type.containerKind == H2TypeContainer_ARRAY)
                && IsVarSizeTypeName(c, field->type.baseName))
            {
                owner->isVarSize = 1;
                changed = 1;
            }
        }
    }
    return 0;
}

int PushScope(H2CBackendC* c) {
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
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->contextCowScopeActive,
            &c->contextCowScopeActiveCap,
            c->localScopeLen + 1u,
            sizeof(uint8_t),
            (uint32_t)_Alignof(uint8_t))
            != 0
        || EnsureCapArena(
               &c->arena,
               (void**)&c->contextCowScopeTempIds,
               &c->contextCowScopeTempCap,
               c->localScopeLen + 1u,
               sizeof(uint32_t),
               (uint32_t)_Alignof(uint32_t))
               != 0)
    {
        return -1;
    }
    c->localScopeMarks[c->localScopeLen++] = c->localLen;
    c->contextCowScopeActive[c->localScopeLen - 1u] = 0;
    c->contextCowScopeTempIds[c->localScopeLen - 1u] = 0;
    c->localAnonTypedefScopeMarks[c->localAnonTypedefScopeLen++] = c->localAnonTypedefLen;
    return 0;
}

void TrimVariantNarrowsToLocalLen(H2CBackendC* c);

void PopScope(H2CBackendC* c) {
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

int PushDeferScope(H2CBackendC* c) {
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

void PopDeferScope(H2CBackendC* c) {
    if (c->deferScopeLen == 0) {
        c->deferredStmtLen = 0;
        return;
    }
    c->deferScopeLen--;
    c->deferredStmtLen = c->deferScopeMarks[c->deferScopeLen];
}

int AddDeferredStmt(H2CBackendC* c, int32_t stmtNodeId) {
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

int AddLocal(H2CBackendC* c, const char* name, H2TypeRef type) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->locals,
            &c->localCap,
            c->localLen + 1u,
            sizeof(H2Local),
            (uint32_t)_Alignof(H2Local))
        != 0)
    {
        return -1;
    }
    c->locals[c->localLen].name = (char*)name;
    c->locals[c->localLen].type = type;
    c->localLen++;
    return 0;
}

void TrimVariantNarrowsToLocalLen(H2CBackendC* c) {
    while (c->variantNarrowLen > 0) {
        if (c->variantNarrows[c->variantNarrowLen - 1u].localIdx < (int32_t)c->localLen) {
            break;
        }
        c->variantNarrowLen--;
    }
}

int AddVariantNarrow(
    H2CBackendC* c,
    int32_t      localIdx,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd) {
    if (EnsureCapArena(
            &c->arena,
            (void**)&c->variantNarrows,
            &c->variantNarrowCap,
            c->variantNarrowLen + 1u,
            sizeof(H2VariantNarrow),
            (uint32_t)_Alignof(H2VariantNarrow))
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

int32_t FindLocalIndexBySlice(const H2CBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (SliceEqName(c->unit->source, start, end, c->locals[i].name)) {
            return (int32_t)i;
        }
    }
    return -1;
}

const H2VariantNarrow* _Nullable FindVariantNarrowByLocalIdx(
    const H2CBackendC* c, int32_t localIdx) {
    uint32_t i = c->variantNarrowLen;
    while (i > 0) {
        i--;
        if (c->variantNarrows[i].localIdx == localIdx) {
            return &c->variantNarrows[i];
        }
    }
    return NULL;
}

const H2Local* _Nullable FindLocalBySlice(const H2CBackendC* c, uint32_t start, uint32_t end) {
    uint32_t i = c->localLen;
    while (i > 0) {
        i--;
        if (SliceEqName(c->unit->source, start, end, c->locals[i].name)) {
            return &c->locals[i];
        }
    }
    return NULL;
}

int FindEnumDeclNodeByCName(const H2CBackendC* c, const char* enumCName, int32_t* outNodeId);

int FindEnumDeclNodeBySlice(
    const H2CBackendC* c, uint32_t start, uint32_t end, int32_t* outNodeId) {
    const char*      enumCName;
    const H2NameMap* map;
    if (outNodeId == NULL) {
        return -1;
    }
    enumCName = ResolveTypeName((H2CBackendC*)c, start, end);
    if (enumCName == NULL) {
        return -1;
    }
    map = FindNameByCName(c, enumCName);
    if (map == NULL || map->kind != H2Ast_ENUM) {
        return -1;
    }
    return FindEnumDeclNodeByCName(c, enumCName, outNodeId);
}

int EnumDeclHasMemberBySlice(
    const H2CBackendC* c, int32_t enumNodeId, uint32_t memberStart, uint32_t memberEnd) {
    int32_t child = AstFirstChild(&c->ast, enumNodeId);

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

    while (child >= 0) {
        const H2AstNode* item = NodeAt(c, child);
        if (item != NULL && item->kind == H2Ast_FIELD
            && SliceSpanEq(c->unit->source, item->dataStart, item->dataEnd, memberStart, memberEnd))
        {
            return 1;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int EnumDeclHasPayload(const H2CBackendC* c, int32_t enumNodeId);

static int ResolveTypePathSpanByExpr(
    const H2CBackendC* c, int32_t exprNode, uint32_t* outStart, uint32_t* outEnd) {
    const H2AstNode* n = NodeAt(c, exprNode);
    if (n == NULL) {
        return 0;
    }
    if (n->kind == H2Ast_IDENT) {
        *outStart = n->dataStart;
        *outEnd = n->dataEnd;
        return 1;
    }
    if (n->kind == H2Ast_FIELD_EXPR) {
        int32_t recvNode = AstFirstChild(&c->ast, exprNode);
        if (ResolveTypePathSpanByExpr(c, recvNode, outStart, outEnd)) {
            *outEnd = n->dataEnd;
            return 1;
        }
    }
    return 0;
}

static int ResolveEnumTypeByPathExpr(
    const H2CBackendC* c,
    int32_t            exprNode,
    const H2NameMap**  outEnumMap,
    int32_t*           outEnumDeclNode) {
    int32_t          rootNodeId = exprNode;
    const H2AstNode* root = NodeAt(c, rootNodeId);
    uint32_t         pathStart = 0;
    uint32_t         pathEnd = 0;
    const char*      enumCName;
    const H2NameMap* map;
    if (outEnumMap == NULL || outEnumDeclNode == NULL) {
        return -1;
    }
    while (root != NULL && root->kind == H2Ast_FIELD_EXPR) {
        rootNodeId = AstFirstChild(&c->ast, rootNodeId);
        root = NodeAt(c, rootNodeId);
    }
    if (root == NULL || root->kind != H2Ast_IDENT) {
        return 0;
    }
    if (FindLocalBySlice(c, root->dataStart, root->dataEnd) != NULL) {
        return 0;
    }
    if (!ResolveTypePathSpanByExpr(c, exprNode, &pathStart, &pathEnd)) {
        return 0;
    }
    enumCName = ResolveTypeName((H2CBackendC*)c, pathStart, pathEnd);
    if (enumCName == NULL) {
        return -1;
    }
    map = FindNameByCName(c, enumCName);
    if (map == NULL || map->kind != H2Ast_ENUM) {
        return 0;
    }
    if (FindEnumDeclNodeByCName(c, enumCName, outEnumDeclNode) != 0) {
        return -1;
    }
    *outEnumMap = map;
    return 1;
}

int ResolveEnumSelectorByFieldExpr(
    const H2CBackendC* c,
    int32_t            fieldExprNode,
    const H2NameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd) {
    const H2AstNode* n = NodeAt(c, fieldExprNode);
    int32_t          recvNode;
    const H2NameMap* map;
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
    if (n == NULL || n->kind != H2Ast_FIELD_EXPR) {
        return 0;
    }
    recvNode = AstFirstChild(&c->ast, fieldExprNode);
    {
        int rc = ResolveEnumTypeByPathExpr(c, recvNode, &map, &enumDeclNode);
        if (rc <= 0) {
            return rc;
        }
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

int EnumDeclHasPayload(const H2CBackendC* c, int32_t enumNodeId) {
    int32_t child = AstFirstChild(&c->ast, enumNodeId);
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
    while (child >= 0) {
        int32_t payload = AstFirstChild(&c->ast, child);
        while (payload >= 0) {
            if (NodeAt(c, payload) != NULL && NodeAt(c, payload)->kind == H2Ast_FIELD) {
                return 1;
            }
            payload = AstNextSibling(&c->ast, payload);
        }
        child = AstNextSibling(&c->ast, child);
    }
    return 0;
}

int32_t EnumVariantTagExprNode(const H2CBackendC* c, int32_t variantNode) {
    int32_t child = AstFirstChild(&c->ast, variantNode);
    while (child >= 0) {
        const H2AstNode* n = NodeAt(c, child);
        if (n != NULL && n->kind != H2Ast_FIELD) {
            return child;
        }
        child = AstNextSibling(&c->ast, child);
    }
    return -1;
}

int FindEnumDeclNodeByCName(const H2CBackendC* c, const char* enumCName, int32_t* outNodeId) {
    uint32_t i;
    if (enumCName == NULL || outNodeId == NULL) {
        return -1;
    }
    for (i = 0; i < c->topDeclLen; i++) {
        int32_t          nodeId = c->topDecls[i].nodeId;
        const H2AstNode* n = NodeAt(c, nodeId);
        const H2NameMap* map;
        if (n == NULL || n->kind != H2Ast_ENUM) {
            continue;
        }
        map = FindTypeDeclMapByNode((H2CBackendC*)c, nodeId);
        if (map != NULL && StrEq(map->cName, enumCName)) {
            *outNodeId = nodeId;
            return 0;
        }
    }
    return -1;
}

int FindEnumVariantNodeBySlice(
    const H2CBackendC* c,
    int32_t            enumNodeId,
    uint32_t           variantStart,
    uint32_t           variantEnd,
    int32_t*           outVariantNode) {
    int32_t child = AstFirstChild(&c->ast, enumNodeId);
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
    while (child >= 0) {
        const H2AstNode* v = NodeAt(c, child);
        if (v != NULL && v->kind == H2Ast_FIELD
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
    H2CBackendC* c,
    const char*  enumTypeName,
    uint32_t     variantStart,
    uint32_t     variantEnd,
    uint32_t     fieldStart,
    uint32_t     fieldEnd,
    H2TypeRef*   outType) {
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
        const H2AstNode* f = NodeAt(c, payload);
        int32_t          typeNode;
        if (f == NULL || f->kind != H2Ast_FIELD) {
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
    const H2CBackendC* c,
    int32_t            typeNode,
    const char**       outEnumCName,
    uint32_t*          outVariantStart,
    uint32_t*          outVariantEnd) {
    const H2AstNode* typeNameNode = NodeAt(c, typeNode);
    uint32_t         dotPos;
    const char*      enumCName;
    const H2NameMap* enumMap;
    int32_t          enumDeclNode;
    if (typeNameNode == NULL || typeNameNode->kind != H2Ast_TYPE_NAME) {
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
    enumCName = ResolveTypeName((H2CBackendC*)c, typeNameNode->dataStart, dotPos);
    if (enumCName == NULL) {
        return -1;
    }
    enumMap = FindNameByCName(c, enumCName);
    if (enumMap == NULL || enumMap->kind != H2Ast_ENUM) {
        return 0;
    }
    if (FindEnumDeclNodeByCName(c, enumCName, &enumDeclNode) != 0) {
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
    const H2CBackendC* c, int32_t caseLabelNode, int32_t* outExprNode, int32_t* outAliasNode) {
    const H2AstNode* n = NodeAt(c, caseLabelNode);
    if (n == NULL) {
        return -1;
    }
    if (n->kind == H2Ast_CASE_PATTERN) {
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
    const H2CBackendC* c,
    int32_t            exprNode,
    const H2NameMap** _Nullable outEnumMap,
    int32_t* _Nullable outEnumDeclNode,
    int* _Nullable outEnumHasPayload,
    uint32_t* _Nullable outVariantStart,
    uint32_t* _Nullable outVariantEnd) {
    const H2AstNode* n = NodeAt(c, exprNode);
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
    if (n == NULL || n->kind != H2Ast_FIELD_EXPR) {
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

int ResolvePayloadEnumType(const H2CBackendC* c, const H2TypeRef* t, const char** outEnumName) {
    const char*      baseName;
    const H2NameMap* map;
    int32_t          enumNodeId;
    if (outEnumName != NULL) {
        *outEnumName = NULL;
    }
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
    if (map == NULL || map->kind != H2Ast_ENUM) {
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

int AppendMappedIdentifier(H2CBackendC* c, uint32_t start, uint32_t end) {
    const H2Local*   local = FindLocalBySlice(c, start, end);
    const H2NameMap* map;
    if (local != NULL) {
        return BufAppendCStr(&c->out, local->name);
    }
    map = FindNameBySlice(c, start, end);
    if (map != NULL) {
        return BufAppendCStr(&c->out, map->cName);
    }
    return BufAppendSlice(&c->out, c->unit->source, start, end);
}

int EmitTypeNameWithDepth(H2CBackendC* c, const H2TypeRef* type) {
    int              i;
    const char*      base = NULL;
    int              stars = 0;
    int              inlineAnon = 0;
    int              inlineAnonIsUnion = 0;
    H2TypeRef        lowered;
    const H2TypeRef* t = type;
    if (type == NULL) {
        return -1;
    }
    if (TypeRefLowerForStorage(c, type, &lowered) != 0) {
        return -1;
    }
    t = &lowered;
    if (!t->valid) {
        return BufAppendCStr(&c->out, "void");
    }
    if (t->containerKind == H2TypeContainer_SLICE_RO
        || t->containerKind == H2TypeContainer_SLICE_MUT)
    {
        if (t->containerPtrDepth > 0) {
            /* *[T] / &[T] lower to slice structs (ptr+len) */
            base = t->containerKind == H2TypeContainer_SLICE_MUT
                     ? "__hop_slice_mut"
                     : "__hop_slice_ro";
            stars = SliceStructPtrDepth(t);
        } else {
            base = t->containerKind == H2TypeContainer_SLICE_MUT
                     ? "__hop_slice_mut"
                     : "__hop_slice_ro";
            stars = 0;
        }
    } else if (TypeRefIsBorrowedStrValueC(t)) {
        base = t->baseName;
        stars = 0;
    } else if (TypeRefIsPointerBackedStrC(t)) {
        base = t->baseName;
        stars = t->ptrDepth > 0 ? t->ptrDepth : 1;
    } else {
        if (t->baseName == NULL) {
            return BufAppendCStr(&c->out, "void");
        }
        base = t->baseName;
        if (StrHasPrefix(base, "__hop_anon_") && !IsAnonTypeNameVisible(c, base)) {
            const H2AnonTypeInfo* info = FindAnonTypeByCName(c, base);
            inlineAnon = 1;
            inlineAnonIsUnion = info != NULL ? info->isUnion : (base[10] == 'u');
        }
        stars = t->ptrDepth;
        if (t->containerKind == H2TypeContainer_ARRAY && t->containerPtrDepth > 0) {
            stars += t->containerPtrDepth;
        }
    }
    if (inlineAnon) {
        uint32_t i;
        if (BufAppendCStr(&c->out, inlineAnonIsUnion ? "union {\n" : "struct {\n") != 0) {
            return -1;
        }
        for (i = 0; i < c->fieldInfoLen; i++) {
            const H2FieldInfo* f = &c->fieldInfos[i];
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

int EmitTypeWithName(H2CBackendC* c, int32_t typeNode, const char* name) {
    H2TypeRef        t;
    H2TypeRef        lowered;
    const H2TypeRef* src;
    int              i;
    int              stars;
    int              pointerBackedOptional = 0;
    if (ParseTypeRef(c, typeNode, &t) != 0 || !t.valid) {
        return -1;
    }
    if (TypeRefLowerForStorage(c, &t, &lowered) != 0) {
        return -1;
    }
    pointerBackedOptional = TypeRefIsPointerBackedOptional(&t);
    src = &lowered;
    if (src->containerKind == H2TypeContainer_SLICE_RO
        || src->containerKind == H2TypeContainer_SLICE_MUT)
    {
        if (src->containerPtrDepth > 0) {
            int stars = SliceStructPtrDepth(src);
            if (BufAppendCStr(
                    &c->out,
                    src->containerKind == H2TypeContainer_SLICE_MUT
                        ? "__hop_slice_mut"
                        : "__hop_slice_ro")
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
                src->containerKind == H2TypeContainer_SLICE_MUT
                    ? "__hop_slice_mut"
                    : "__hop_slice_ro")
                != 0
            || BufAppendChar(&c->out, ' ') != 0)
        {
            return -1;
        }
        return BufAppendCStr(&c->out, name);
    }
    if (TypeRefIsBorrowedStrValueC(src)) {
        if (BufAppendCStr(&c->out, src->baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
            return -1;
        }
        return BufAppendCStr(&c->out, name);
    }
    if (TypeRefIsPointerBackedStrC(src)) {
        int stars = src->ptrDepth > 0 ? src->ptrDepth : 1;
        if (BufAppendCStr(&c->out, src->baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
            return -1;
        }
        for (i = 0; i < stars; i++) {
            if (BufAppendChar(&c->out, '*') != 0) {
                return -1;
            }
        }
        return BufAppendCStr(&c->out, name);
    }
    if (src->baseName == NULL) {
        return -1;
    }
    if (BufAppendCStr(&c->out, src->baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
        return -1;
    }
    stars = src->ptrDepth;
    if (src->containerKind == H2TypeContainer_ARRAY && src->containerPtrDepth > 0) {
        stars += src->containerPtrDepth;
    }
    for (i = 0; i < stars; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    if (pointerBackedOptional && BufAppendCStr(&c->out, "/* optional */ ") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, name) != 0) {
        return -1;
    }
    if (src->containerKind == H2TypeContainer_ARRAY && src->containerPtrDepth == 0
        && src->hasArrayLen)
    {
        if (BufAppendChar(&c->out, '[') != 0 || BufAppendU32(&c->out, src->arrayLen) != 0
            || BufAppendChar(&c->out, ']') != 0)
        {
            return -1;
        }
    }
    return 0;
}

int EmitTypeRefWithName(H2CBackendC* c, const H2TypeRef* t, const char* name);

int EmitAnonInlineTypeWithName(
    H2CBackendC* c, const char* ownerType, int isUnion, const char* name) {
    uint32_t i;
    int      sawField = 0;
    if (BufAppendCStr(&c->out, isUnion ? "union {\n" : "struct {\n") != 0) {
        return -1;
    }
    for (i = 0; i < c->fieldInfoLen; i++) {
        const H2FieldInfo* f = &c->fieldInfos[i];
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

int EmitTypeRefWithName(H2CBackendC* c, const H2TypeRef* t, const char* name) {
    int              stars;
    int              i;
    H2TypeRef        lowered;
    const H2TypeRef* src = t;
    int              pointerBackedOptional = 0;
    if (t == NULL) {
        return -1;
    }
    pointerBackedOptional = TypeRefIsPointerBackedOptional(t);
    if (TypeRefLowerForStorage(c, t, &lowered) != 0) {
        return -1;
    }
    src = &lowered;
    if (!src->valid) {
        return -1;
    }
    if (src->containerKind == H2TypeContainer_SLICE_RO
        || src->containerKind == H2TypeContainer_SLICE_MUT)
    {
        if (src->containerPtrDepth > 0) {
            int stars = SliceStructPtrDepth(src);
            if (BufAppendCStr(
                    &c->out,
                    src->containerKind == H2TypeContainer_SLICE_MUT
                        ? "__hop_slice_mut"
                        : "__hop_slice_ro")
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
                src->containerKind == H2TypeContainer_SLICE_MUT
                    ? "__hop_slice_mut"
                    : "__hop_slice_ro")
                != 0
            || BufAppendChar(&c->out, ' ') != 0)
        {
            return -1;
        }
        return BufAppendCStr(&c->out, name);
    }
    if (TypeRefIsBorrowedStrValueC(src)) {
        if (BufAppendCStr(&c->out, src->baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
            return -1;
        }
        return BufAppendCStr(&c->out, name);
    }
    if (TypeRefIsPointerBackedStrC(src)) {
        int stars = src->ptrDepth > 0 ? src->ptrDepth : 1;
        if (BufAppendCStr(&c->out, src->baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
            return -1;
        }
        for (i = 0; i < stars; i++) {
            if (BufAppendChar(&c->out, '*') != 0) {
                return -1;
            }
        }
        return BufAppendCStr(&c->out, name);
    }
    if (src->baseName == NULL) {
        return -1;
    }
    if (src->containerKind == H2TypeContainer_SCALAR && src->ptrDepth == 0
        && src->containerPtrDepth == 0 && !src->isOptional)
    {
        const H2AnonTypeInfo* info = FindAnonTypeByCName(c, src->baseName);
        if (info != NULL && (info->flags & H2AnonTypeFlag_EMITTED_GLOBAL) == 0
            && !IsLocalAnonTypedefVisible(c, info->cName))
        {
            return EmitAnonInlineTypeWithName(c, info->cName, info->isUnion, name);
        }
        if (info == NULL && StrHasPrefix(src->baseName, "__hop_anon_")
            && !IsAnonTypeNameVisible(c, src->baseName))
        {
            int isUnion = src->baseName[10] == 'u';
            return EmitAnonInlineTypeWithName(c, src->baseName, isUnion, name);
        }
    }
    if (BufAppendCStr(&c->out, src->baseName) != 0 || BufAppendChar(&c->out, ' ') != 0) {
        return -1;
    }
    stars = src->ptrDepth;
    if (src->containerKind == H2TypeContainer_ARRAY && src->containerPtrDepth > 0) {
        stars += src->containerPtrDepth;
    }
    for (i = 0; i < stars; i++) {
        if (BufAppendChar(&c->out, '*') != 0) {
            return -1;
        }
    }
    if (pointerBackedOptional && BufAppendCStr(&c->out, "/* optional */ ") != 0) {
        return -1;
    }
    if (BufAppendCStr(&c->out, name) != 0) {
        return -1;
    }
    if (src->containerKind == H2TypeContainer_ARRAY && src->containerPtrDepth == 0
        && src->hasArrayLen)
    {
        if (BufAppendChar(&c->out, '[') != 0 || BufAppendU32(&c->out, src->arrayLen) != 0
            || BufAppendChar(&c->out, ']') != 0)
        {
            return -1;
        }
    }
    return 0;
}

int EmitTypeForCast(H2CBackendC* c, int32_t typeNode) {
    H2TypeRef t;
    if (ParseTypeRef(c, typeNode, &t) != 0) {
        return -1;
    }
    return EmitTypeNameWithDepth(c, &t);
}

int InferExprType(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType);
int InferExprTypeExpected(
    H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType, H2TypeRef* outType);
int EmitExpr(H2CBackendC* c, int32_t nodeId);
int EmitCompoundLiteral(H2CBackendC* c, int32_t nodeId, const H2TypeRef* _Nullable expectedType);
int EmitCompoundLiteralOrderedStruct(
    H2CBackendC* c, int32_t firstField, const char* ownerType, const H2TypeRef* valueType);
int EmitEffectiveContextFieldValue(
    H2CBackendC* c, const char* fieldName, const H2TypeRef* requiredType);
int InferNewExprType(H2CBackendC* c, int32_t nodeId, H2TypeRef* outType);
int EmitExprCoerced(H2CBackendC* c, int32_t exprNode, const H2TypeRef* _Nullable dstType);
int EmitCompoundFieldValueCoerced(
    H2CBackendC* c, const H2AstNode* field, int32_t exprNode, const H2TypeRef* _Nullable dstType);
int EmitContextArgForSig(H2CBackendC* c, const H2FnSig* sig);
int StructHasFieldDefaults(const H2CBackendC* c, const char* ownerType);
int TypeRefAssignableCost(
    H2CBackendC* c, const H2TypeRef* dst, const H2TypeRef* src, uint8_t* outCost);

void SetPreferredAllocatorPtrType(H2TypeRef* outType) {
    TypeRefSetScalar(outType, "builtin__MemAllocator");
    outType->ptrDepth = 1;
}

H2_API_END
