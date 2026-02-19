#include "libsl-impl.h"

SL_API_BEGIN

typedef enum {
    SLTCType_INVALID = 0,
    SLTCType_BUILTIN,
    SLTCType_NAMED,
    SLTCType_PTR,
    SLTCType_REF,
    SLTCType_ARRAY,
    SLTCType_SLICE,
    SLTCType_UNTYPED_INT,
    SLTCType_UNTYPED_FLOAT,
    SLTCType_FUNCTION,
    SLTCType_OPTIONAL,
    SLTCType_NULL,
} SLTCTypeKind;

typedef enum {
    SLBuiltin_INVALID = 0,
    SLBuiltin_VOID,
    SLBuiltin_BOOL,
    SLBuiltin_STR,
    SLBuiltin_MEMALLOCATOR,
    SLBuiltin_U8,
    SLBuiltin_U16,
    SLBuiltin_U32,
    SLBuiltin_U64,
    SLBuiltin_I8,
    SLBuiltin_I16,
    SLBuiltin_I32,
    SLBuiltin_I64,
    SLBuiltin_USIZE,
    SLBuiltin_ISIZE,
    SLBuiltin_F32,
    SLBuiltin_F64,
} SLBuiltinKind;

typedef struct {
    SLTCTypeKind  kind;
    SLBuiltinKind builtin;
    int32_t       baseType;
    int32_t       declNode;
    int32_t       funcIndex;
    uint32_t      arrayLen;
    uint32_t      nameStart;
    uint32_t      nameEnd;
    uint32_t      fieldStart;
    uint16_t      fieldCount;
    uint16_t      flags;
} SLTCType;

enum {
    SLTCTypeFlag_VARSIZE = 1u << 0,
    SLTCTypeFlag_MUTABLE = 1u << 1,
    SLTCTypeFlag_VISITING = 1u << 14,
    SLTCTypeFlag_VISITED = 1u << 15,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    uint32_t lenNameStart;
    uint32_t lenNameEnd;
    uint16_t flags;
} SLTCField;

enum {
    SLTCFieldFlag_DEPENDENT = 1u << 0,
    SLTCFieldFlag_EMBEDDED = 1u << 1,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
    int32_t  declNode;
} SLTCNamedType;

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  returnType;
    uint32_t paramTypeStart;
    uint16_t paramCount;
    int32_t  declNode;
    int32_t  defNode;
    int32_t  funcTypeId;
} SLTCFunction;

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
} SLTCLocal;

typedef struct {
    SLArena*     arena;
    const SLAst* ast;
    SLStrView    src;
    SLDiag*      diag;

    SLTCType* types;
    uint32_t  typeLen;
    uint32_t  typeCap;

    SLTCField* fields;
    uint32_t   fieldLen;
    uint32_t   fieldCap;

    SLTCNamedType* namedTypes;
    uint32_t       namedTypeLen;
    uint32_t       namedTypeCap;

    SLTCFunction* funcs;
    uint32_t      funcLen;
    uint32_t      funcCap;

    int32_t* funcParamTypes;
    uint32_t funcParamLen;
    uint32_t funcParamCap;

    int32_t* scratchParamTypes;
    uint32_t scratchParamCap;

    SLTCLocal* locals;
    uint32_t   localLen;
    uint32_t   localCap;

    int32_t typeVoid;
    int32_t typeBool;
    int32_t typeStr;
    int32_t typeMemAllocator;
    int32_t typeUsize;
    int32_t typeUntypedInt;
    int32_t typeUntypedFloat;
    int32_t typeNull;
} SLTypeCheckCtx;

static void SLTCSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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

static int SLTCFailSpan(SLTypeCheckCtx* c, SLDiagCode code, uint32_t start, uint32_t end) {
    SLTCSetDiag(c->diag, code, start, end);
    return -1;
}

static int SLTCFailNode(SLTypeCheckCtx* c, int32_t nodeId, SLDiagCode code) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, code, 0, 0);
    }
    return SLTCFailSpan(c, code, c->ast->nodes[nodeId].start, c->ast->nodes[nodeId].end);
}

static int32_t SLAstFirstChild(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int32_t SLAstNextSibling(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

static int SLNameEqSlice(
    SLStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
    uint32_t i;
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != (bEnd - bStart)) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (src.ptr[aStart + i] != src.ptr[bStart + i]) {
            return 0;
        }
    }
    return 1;
}

static int SLNameEqLiteral(SLStrView src, uint32_t start, uint32_t end, const char* lit) {
    uint32_t i = 0;
    uint32_t len;
    if (end < start) {
        return 0;
    }
    len = end - start;
    while (i < len) {
        if (lit[i] == '\0' || src.ptr[start + i] != lit[i]) {
            return 0;
        }
        i++;
    }
    return lit[i] == '\0';
}

static int32_t SLTCAddType(
    SLTypeCheckCtx* c, const SLTCType* t, uint32_t errStart, uint32_t errEnd) {
    int32_t idx;
    if (c->typeLen >= c->typeCap) {
        SLTCSetDiag(c->diag, SLDiag_ARENA_OOM, errStart, errEnd);
        return -1;
    }
    idx = (int32_t)c->typeLen++;
    c->types[idx] = *t;
    return idx;
}

static int32_t SLTCAddBuiltinType(SLTypeCheckCtx* c, const char* name, SLBuiltinKind builtinKind) {
    SLTCType t;
    uint32_t i = 0;
    while (name[i] != '\0') {
        i++;
    }
    t.kind = SLTCType_BUILTIN;
    t.builtin = builtinKind;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = i;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, 0, 0);
}

static int SLTCEnsureInitialized(SLTypeCheckCtx* c) {
    SLTCType t;

    c->typeVoid = -1;
    c->typeBool = -1;
    c->typeStr = -1;
    c->typeMemAllocator = -1;
    c->typeUsize = -1;
    c->typeUntypedInt = -1;
    c->typeUntypedFloat = -1;
    c->typeNull = -1;

    c->typeVoid = SLTCAddBuiltinType(c, "void", SLBuiltin_VOID);
    c->typeBool = SLTCAddBuiltinType(c, "bool", SLBuiltin_BOOL);
    c->typeStr = SLTCAddBuiltinType(c, "str", SLBuiltin_STR);
    c->typeMemAllocator = SLTCAddBuiltinType(c, "MemAllocator", SLBuiltin_MEMALLOCATOR);
    if (c->typeVoid < 0 || c->typeBool < 0 || c->typeStr < 0 || c->typeMemAllocator < 0) {
        return -1;
    }

    if (SLTCAddBuiltinType(c, "u8", SLBuiltin_U8) < 0
        || SLTCAddBuiltinType(c, "u16", SLBuiltin_U16) < 0
        || SLTCAddBuiltinType(c, "u32", SLBuiltin_U32) < 0
        || SLTCAddBuiltinType(c, "u64", SLBuiltin_U64) < 0
        || SLTCAddBuiltinType(c, "i8", SLBuiltin_I8) < 0
        || SLTCAddBuiltinType(c, "i16", SLBuiltin_I16) < 0
        || SLTCAddBuiltinType(c, "i32", SLBuiltin_I32) < 0
        || SLTCAddBuiltinType(c, "i64", SLBuiltin_I64) < 0
        || SLTCAddBuiltinType(c, "uint", SLBuiltin_USIZE) < 0
        || SLTCAddBuiltinType(c, "int", SLBuiltin_ISIZE) < 0
        || SLTCAddBuiltinType(c, "f32", SLBuiltin_F32) < 0
        || SLTCAddBuiltinType(c, "f64", SLBuiltin_F64) < 0)
    {
        return -1;
    }

    t.kind = SLTCType_UNTYPED_INT;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    c->typeUntypedInt = SLTCAddType(c, &t, 0, 0);
    if (c->typeUntypedInt < 0) {
        return -1;
    }

    t.kind = SLTCType_UNTYPED_FLOAT;
    c->typeUntypedFloat = SLTCAddType(c, &t, 0, 0);
    if (c->typeUntypedFloat < 0) {
        return -1;
    }

    t.kind = SLTCType_NULL;
    c->typeNull = SLTCAddType(c, &t, 0, 0);
    return c->typeNull < 0 ? -1 : 0;
}

static int32_t SLTCFindBuiltinType(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        const SLTCType* t = &c->types[i];
        if (t->kind != SLTCType_BUILTIN) {
            continue;
        }
        if (SLNameEqLiteral(c->src, start, end, "void") && t->builtin == SLBuiltin_VOID) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "bool") && t->builtin == SLBuiltin_BOOL) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "str") && t->builtin == SLBuiltin_STR) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "MemAllocator")
            && t->builtin == SLBuiltin_MEMALLOCATOR)
        {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u8") && t->builtin == SLBuiltin_U8) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u16") && t->builtin == SLBuiltin_U16) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u32") && t->builtin == SLBuiltin_U32) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "u64") && t->builtin == SLBuiltin_U64) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i8") && t->builtin == SLBuiltin_I8) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i16") && t->builtin == SLBuiltin_I16) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i32") && t->builtin == SLBuiltin_I32) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "i64") && t->builtin == SLBuiltin_I64) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "uint") && t->builtin == SLBuiltin_USIZE) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "int") && t->builtin == SLBuiltin_ISIZE) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "f32") && t->builtin == SLBuiltin_F32) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "f64") && t->builtin == SLBuiltin_F64) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLTCFindBuiltinByKind(SLTypeCheckCtx* c, SLBuiltinKind builtinKind) {
    uint32_t i;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_BUILTIN && c->types[i].builtin == builtinKind) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLTCFindNamedTypeIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (SLNameEqSlice(c->src, c->namedTypes[i].nameStart, c->namedTypes[i].nameEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLTCFindFunctionIndex(SLTypeCheckCtx* c, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        if (SLNameEqSlice(c->src, c->funcs[i].nameStart, c->funcs[i].nameEnd, start, end)) {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLTCInternPtrType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_PTR && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_PTR;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

static int SLTCTypeIsMutable(const SLTCType* t) {
    return (t->flags & SLTCTypeFlag_MUTABLE) != 0;
}

static int32_t SLTCInternRefType(
    SLTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_REF && c->types[i].baseType == baseType
            && SLTCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_REF;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? SLTCTypeFlag_MUTABLE : 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

static int32_t SLTCInternArrayType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t arrayLen, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_ARRAY && c->types[i].baseType == baseType
            && c->types[i].arrayLen == arrayLen)
        {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_ARRAY;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = arrayLen;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

static int32_t SLTCInternSliceType(
    SLTypeCheckCtx* c, int32_t baseType, int isMutable, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_SLICE && c->types[i].baseType == baseType
            && SLTCTypeIsMutable(&c->types[i]) == isMutable)
        {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_SLICE;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = isMutable ? SLTCTypeFlag_MUTABLE : 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

static int32_t SLTCInternOptionalType(
    SLTypeCheckCtx* c, int32_t baseType, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_OPTIONAL && c->types[i].baseType == baseType) {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_OPTIONAL;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = baseType;
    t.declNode = -1;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

static int32_t SLTCInternFunctionType(
    SLTypeCheckCtx* c, int32_t funcIndex, uint32_t errStart, uint32_t errEnd) {
    uint32_t i;
    SLTCType t;
    for (i = 0; i < c->typeLen; i++) {
        if (c->types[i].kind == SLTCType_FUNCTION && c->types[i].funcIndex == funcIndex) {
            return (int32_t)i;
        }
    }
    t.kind = SLTCType_FUNCTION;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = -1;
    t.funcIndex = funcIndex;
    t.arrayLen = 0;
    t.nameStart = 0;
    t.nameEnd = 0;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    return SLTCAddType(c, &t, errStart, errEnd);
}

static int SLTCParseArrayLen(SLTypeCheckCtx* c, const SLAstNode* node, uint32_t* outLen) {
    uint32_t i;
    uint32_t v = 0;
    if (node->dataEnd <= node->dataStart) {
        return -1;
    }
    for (i = node->dataStart; i < node->dataEnd; i++) {
        unsigned char ch = (unsigned char)c->src.ptr[i];
        if (ch < (unsigned char)'0' || ch > (unsigned char)'9') {
            return -1;
        }
        v = v * 10u + (uint32_t)(ch - (unsigned char)'0');
    }
    *outLen = v;
    return 0;
}

typedef struct {
    int32_t  elemType;
    int      indexable;
    int      sliceable;
    int      sliceMutable;
    int      hasKnownLen;
    uint32_t knownLen;
} SLTCIndexBaseInfo;

static int SLTCResolveIndexBaseInfo(SLTypeCheckCtx* c, int32_t baseType, SLTCIndexBaseInfo* out) {
    const SLTCType* t;
    out->elemType = -1;
    out->indexable = 0;
    out->sliceable = 0;
    out->sliceMutable = 0;
    out->hasKnownLen = 0;
    out->knownLen = 0;

    if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
        return -1;
    }
    t = &c->types[baseType];
    switch (t->kind) {
        case SLTCType_ARRAY:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = 1;
            out->hasKnownLen = 1;
            out->knownLen = t->arrayLen;
            return 0;
        case SLTCType_SLICE:
            out->elemType = t->baseType;
            out->indexable = 1;
            out->sliceable = 1;
            out->sliceMutable = SLTCTypeIsMutable(t);
            return 0;
        case SLTCType_PTR: {
            int32_t pointee = t->baseType;
            if (pointee < 0 || (uint32_t)pointee >= c->typeLen) {
                return -1;
            }
            if (c->types[pointee].kind == SLTCType_ARRAY) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = 1;
                out->hasKnownLen = 1;
                out->knownLen = c->types[pointee].arrayLen;
                return 0;
            }
            if (c->types[pointee].kind == SLTCType_SLICE) {
                out->elemType = c->types[pointee].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = SLTCTypeIsMutable(&c->types[pointee]);
                return 0;
            }
            out->elemType = pointee;
            out->indexable = 1;
            out->sliceMutable = 1;
            return 0;
        }
        case SLTCType_REF: {
            int32_t refBase = t->baseType;
            if (refBase < 0 || (uint32_t)refBase >= c->typeLen) {
                return -1;
            }
            if (c->types[refBase].kind == SLTCType_ARRAY) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = SLTCTypeIsMutable(t);
                out->hasKnownLen = 1;
                out->knownLen = c->types[refBase].arrayLen;
                return 0;
            }
            if (c->types[refBase].kind == SLTCType_SLICE) {
                out->elemType = c->types[refBase].baseType;
                out->indexable = 1;
                out->sliceable = 1;
                out->sliceMutable = SLTCTypeIsMutable(t) && SLTCTypeIsMutable(&c->types[refBase]);
                return 0;
            }
            out->elemType = refBase;
            out->indexable = 1;
            out->sliceMutable = SLTCTypeIsMutable(t);
            return 0;
        }
        default: return 0;
    }
}

static int SLTCParseIntLiteralToI64(SLTypeCheckCtx* c, uint32_t start, uint32_t end, int64_t* out) {
    uint64_t v = 0;
    uint32_t i;
    if (end <= start) {
        return -1;
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)c->src.ptr[i];
        if (ch < (unsigned char)'0' || ch > (unsigned char)'9') {
            return -1;
        }
        if (v > (uint64_t)INT64_MAX / 10u
            || (v == (uint64_t)INT64_MAX / 10u
                && (uint64_t)(ch - (unsigned char)'0') > (uint64_t)INT64_MAX % 10u))
        {
            return -1;
        }
        v = v * 10u + (uint64_t)(ch - (unsigned char)'0');
    }
    *out = (int64_t)v;
    return 0;
}

static int SLTCConstIntExpr(SLTypeCheckCtx* c, int32_t nodeId, int64_t* out, int* isConst) {
    const SLAstNode* n;
    *isConst = 0;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return -1;
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == SLAst_INT) {
        if (SLTCParseIntLiteralToI64(c, n->dataStart, n->dataEnd, out) != 0) {
            return -1;
        }
        *isConst = 1;
        return 0;
    }
    if (n->kind == SLAst_UNARY) {
        int32_t           child = SLAstFirstChild(c->ast, nodeId);
        int               childConst = 0;
        int64_t           childValue = 0;
        const SLTokenKind op = (SLTokenKind)n->op;
        if ((op == SLTok_ADD || op == SLTok_SUB)
            && SLTCConstIntExpr(c, child, &childValue, &childConst) == 0 && childConst)
        {
            if (op == SLTok_SUB) {
                childValue = -childValue;
            }
            *out = childValue;
            *isConst = 1;
            return 0;
        }
    }
    return 0;
}

static void SLTCMarkRuntimeBoundsCheck(SLTypeCheckCtx* c, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return;
    }
    ((SLAstNode*)&c->ast->nodes[nodeId])->flags |= SLAstFlag_INDEX_RUNTIME_BOUNDS;
}

static int SLTCResolveTypeNode(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_TYPE, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case SLAst_TYPE_NAME: {
            int32_t typeId = SLTCFindBuiltinType(c, n->dataStart, n->dataEnd);
            if (typeId >= 0) {
                *outType = typeId;
                return 0;
            }
            {
                int32_t namedIndex = SLTCFindNamedTypeIndex(c, n->dataStart, n->dataEnd);
                if (namedIndex < 0) {
                    return SLTCFailSpan(c, SLDiag_UNKNOWN_TYPE, n->dataStart, n->dataEnd);
                }
                *outType = c->namedTypes[namedIndex].typeId;
                return 0;
            }
        }
        case SLAst_TYPE_PTR: {
            int32_t child = SLAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t ptrType;
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            ptrType = SLTCInternPtrType(c, baseType, n->start, n->end);
            if (ptrType < 0) {
                return -1;
            }
            *outType = ptrType;
            return 0;
        }
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF: {
            int32_t child = SLAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t refType;
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            refType = SLTCInternRefType(
                c, baseType, n->kind == SLAst_TYPE_MUTREF, n->start, n->end);
            if (refType < 0) {
                return -1;
            }
            *outType = refType;
            return 0;
        }
        case SLAst_TYPE_ARRAY: {
            int32_t  child = SLAstFirstChild(c->ast, nodeId);
            int32_t  baseType;
            int32_t  arrayType;
            uint32_t arrayLen;
            if (SLTCParseArrayLen(c, n, &arrayLen) != 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
            }
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            arrayType = SLTCInternArrayType(c, baseType, arrayLen, n->start, n->end);
            if (arrayType < 0) {
                return -1;
            }
            *outType = arrayType;
            return 0;
        }
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE: {
            int32_t child = SLAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t sliceType;
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            sliceType = SLTCInternSliceType(
                c, baseType, n->kind == SLAst_TYPE_MUTSLICE, n->start, n->end);
            if (sliceType < 0) {
                return -1;
            }
            *outType = sliceType;
            return 0;
        }
        case SLAst_TYPE_OPTIONAL: {
            int32_t child = SLAstFirstChild(c->ast, nodeId);
            int32_t baseType;
            int32_t optType;
            if (SLTCResolveTypeNode(c, child, &baseType) != 0) {
                return -1;
            }
            /* Restrict ?T to pointer/reference/slice types only (plain value types deferred). */
            if (c->types[baseType].kind != SLTCType_PTR && c->types[baseType].kind != SLTCType_REF
                && c->types[baseType].kind != SLTCType_SLICE)
            {
                return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
            }
            optType = SLTCInternOptionalType(c, baseType, n->start, n->end);
            if (optType < 0) {
                return -1;
            }
            *outType = optType;
            return 0;
        }
        case SLAst_TYPE_VARRAY: return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
        default:                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
}

static int SLTCAddNamedType(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLAstNode* node = &c->ast->nodes[nodeId];
    SLTCType         t;
    int32_t          typeId;
    uint32_t         idx;

    if (node->dataEnd <= node->dataStart) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }

    if (SLTCFindNamedTypeIndex(c, node->dataStart, node->dataEnd) >= 0) {
        return SLTCFailSpan(c, SLDiag_DUPLICATE_SYMBOL, node->dataStart, node->dataEnd);
    }

    t.kind = SLTCType_NAMED;
    t.builtin = SLBuiltin_INVALID;
    t.baseType = -1;
    t.declNode = nodeId;
    t.funcIndex = -1;
    t.arrayLen = 0;
    t.nameStart = node->dataStart;
    t.nameEnd = node->dataEnd;
    t.fieldStart = 0;
    t.fieldCount = 0;
    t.flags = 0;
    typeId = SLTCAddType(c, &t, node->start, node->end);
    if (typeId < 0) {
        return -1;
    }

    if (c->namedTypeLen >= c->namedTypeCap) {
        return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
    }

    idx = c->namedTypeLen++;
    c->namedTypes[idx].nameStart = node->dataStart;
    c->namedTypes[idx].nameEnd = node->dataEnd;
    c->namedTypes[idx].typeId = typeId;
    c->namedTypes[idx].declNode = nodeId;
    return 0;
}

static int SLTCCollectTypeDeclsFromNode(SLTypeCheckCtx* c, int32_t nodeId) {
    SLAstKind kind = c->ast->nodes[nodeId].kind;
    if (kind == SLAst_PUB) {
        int32_t ch = SLAstFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (SLTCCollectTypeDeclsFromNode(c, ch) != 0) {
                return -1;
            }
            ch = SLAstNextSibling(c->ast, ch);
        }
        return 0;
    }
    if (kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM) {
        return SLTCAddNamedType(c, nodeId);
    }
    return 0;
}

static int SLTCIsIntegerType(SLTypeCheckCtx* c, int32_t typeId) {
    SLBuiltinKind b;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedInt) {
        return 1;
    }
    if (c->types[typeId].kind != SLTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == SLBuiltin_U8 || b == SLBuiltin_U16 || b == SLBuiltin_U32 || b == SLBuiltin_U64
        || b == SLBuiltin_I8 || b == SLBuiltin_I16 || b == SLBuiltin_I32 || b == SLBuiltin_I64
        || b == SLBuiltin_USIZE || b == SLBuiltin_ISIZE;
}

static int SLTCIsFloatType(SLTypeCheckCtx* c, int32_t typeId) {
    SLBuiltinKind b;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeUntypedFloat) {
        return 1;
    }
    if (c->types[typeId].kind != SLTCType_BUILTIN) {
        return 0;
    }
    b = c->types[typeId].builtin;
    return b == SLBuiltin_F32 || b == SLBuiltin_F64;
}

static int SLTCIsNumericType(SLTypeCheckCtx* c, int32_t typeId) {
    return SLTCIsIntegerType(c, typeId) || SLTCIsFloatType(c, typeId);
}

static int SLTCIsBoolType(SLTypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeBool;
}

static int SLTCTypeSupportsLen(SLTypeCheckCtx* c, int32_t typeId) {
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (typeId == c->typeStr) {
        return 1;
    }
    if (c->types[typeId].kind == SLTCType_ARRAY || c->types[typeId].kind == SLTCType_SLICE) {
        return 1;
    }
    if (c->types[typeId].kind == SLTCType_PTR || c->types[typeId].kind == SLTCType_REF) {
        int32_t baseType = c->types[typeId].baseType;
        if (baseType < 0 || (uint32_t)baseType >= c->typeLen) {
            return 0;
        }
        return c->types[baseType].kind == SLTCType_ARRAY
            || c->types[baseType].kind == SLTCType_SLICE;
    }
    return 0;
}

static int SLTCIsUntyped(SLTypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

static int SLTCTypeIsVarSize(SLTypeCheckCtx* c, int32_t typeId) {
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    return (c->types[typeId].flags & SLTCTypeFlag_VARSIZE) != 0;
}

static int SLTCTypeContainsVarSizeByValue(SLTypeCheckCtx* c, int32_t typeId) {
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return 0;
    }
    if (c->types[typeId].kind == SLTCType_PTR || c->types[typeId].kind == SLTCType_REF
        || c->types[typeId].kind == SLTCType_SLICE || c->types[typeId].kind == SLTCType_OPTIONAL)
    {
        return 0;
    }
    if (c->types[typeId].kind == SLTCType_ARRAY) {
        return SLTCTypeContainsVarSizeByValue(c, c->types[typeId].baseType);
    }
    return SLTCTypeIsVarSize(c, typeId);
}

static int32_t SLTCFindEmbeddedFieldIndex(SLTypeCheckCtx* c, int32_t namedTypeId) {
    uint32_t i;
    if (namedTypeId < 0 || (uint32_t)namedTypeId >= c->typeLen
        || c->types[namedTypeId].kind != SLTCType_NAMED)
    {
        return -1;
    }
    for (i = 0; i < c->types[namedTypeId].fieldCount; i++) {
        uint32_t idx = c->types[namedTypeId].fieldStart + i;
        if ((c->fields[idx].flags & SLTCFieldFlag_EMBEDDED) != 0) {
            return (int32_t)idx;
        }
    }
    return -1;
}

static int SLTCIsNamedDerivedFrom(SLTypeCheckCtx* c, int32_t srcType, int32_t dstType) {
    uint32_t depth = 0;
    int32_t  cur = srcType;
    if (srcType == dstType) {
        return 1;
    }
    while (depth++ <= c->typeLen) {
        int32_t embedIdx;
        if (cur < 0 || (uint32_t)cur >= c->typeLen || c->types[cur].kind != SLTCType_NAMED) {
            return 0;
        }
        embedIdx = SLTCFindEmbeddedFieldIndex(c, cur);
        if (embedIdx < 0) {
            return 0;
        }
        cur = c->fields[embedIdx].typeId;
        if (cur == dstType) {
            return 1;
        }
    }
    return 0;
}

static int SLTCCanAssign(SLTypeCheckCtx* c, int32_t dstType, int32_t srcType) {
    const SLTCType* dst;
    const SLTCType* src;

    if (dstType == srcType) {
        return 1;
    }
    if (srcType == c->typeUntypedInt && SLTCIsIntegerType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedFloat && SLTCIsFloatType(c, dstType)) {
        return 1;
    }
    if (srcType == c->typeUntypedInt && SLTCIsFloatType(c, dstType)) {
        return 1;
    }

    if (dstType < 0 || (uint32_t)dstType >= c->typeLen || srcType < 0
        || (uint32_t)srcType >= c->typeLen)
    {
        return 0;
    }
    dst = &c->types[dstType];
    src = &c->types[srcType];

    if (dst->kind == SLTCType_NAMED && src->kind == SLTCType_NAMED
        && SLTCIsNamedDerivedFrom(c, srcType, dstType))
    {
        return 1;
    }

    if (dst->kind == SLTCType_REF) {
        if (src->kind == SLTCType_REF
            && (dst->baseType == src->baseType
                || SLTCIsNamedDerivedFrom(c, src->baseType, dst->baseType)))
        {
            return !SLTCTypeIsMutable(dst) || SLTCTypeIsMutable(src);
        }
        if (src->kind == SLTCType_PTR
            && (dst->baseType == src->baseType
                || SLTCIsNamedDerivedFrom(c, src->baseType, dst->baseType)))
        {
            return 1;
        }
        return 0;
    }

    if (dst->kind == SLTCType_PTR) {
        /* Owned pointers (*T) can only come from new(); references (&T) cannot be
         * implicitly promoted to owned pointers. */
        return 0;
    }

    if (dst->kind == SLTCType_SLICE) {
        if (src->kind == SLTCType_SLICE && dst->baseType == src->baseType) {
            return !SLTCTypeIsMutable(dst) || SLTCTypeIsMutable(src);
        }
        if (src->kind == SLTCType_ARRAY && dst->baseType == src->baseType) {
            return 1;
        }
        if (src->kind == SLTCType_PTR) {
            int32_t pointee = src->baseType;
            if (pointee >= 0 && (uint32_t)pointee < c->typeLen) {
                const SLTCType* p = &c->types[pointee];
                if (p->kind == SLTCType_ARRAY && p->baseType == dst->baseType) {
                    return 1;
                }
            }
        }
        return 0;
    }

    if (dst->kind == SLTCType_OPTIONAL) {
        /* null can be assigned to ?T */
        if (src->kind == SLTCType_NULL) {
            return 1;
        }
        /* T can be assigned to T? (implicit lift) */
        if (srcType == dst->baseType
            || (src->kind == SLTCType_NAMED && c->types[dst->baseType].kind == SLTCType_NAMED
                && SLTCIsNamedDerivedFrom(c, srcType, dst->baseType)))
        {
            return 1;
        }
        /* ?T can be assigned to ?T (also handles mutable sub-type coercions) */
        if (src->kind == SLTCType_OPTIONAL) {
            return SLTCCanAssign(c, dst->baseType, src->baseType);
        }
        return 0;
    }

    /* null can only be assigned to ?T, not to plain types */
    if (src->kind == SLTCType_NULL) {
        return 0;
    }

    return 0;
}

static int SLTCCoerceForBinary(
    SLTypeCheckCtx* c, int32_t leftType, int32_t rightType, int32_t* outType) {
    if (leftType == rightType) {
        *outType = leftType;
        return 0;
    }
    if (SLTCIsUntyped(c, leftType) && !SLTCIsUntyped(c, rightType)
        && SLTCCanAssign(c, rightType, leftType))
    {
        *outType = rightType;
        return 0;
    }
    if (SLTCIsUntyped(c, rightType) && !SLTCIsUntyped(c, leftType)
        && SLTCCanAssign(c, leftType, rightType))
    {
        *outType = leftType;
        return 0;
    }
    if (leftType == c->typeUntypedInt && rightType == c->typeUntypedFloat) {
        *outType = c->typeUntypedFloat;
        return 0;
    }
    if (leftType == c->typeUntypedFloat && rightType == c->typeUntypedInt) {
        *outType = c->typeUntypedFloat;
        return 0;
    }
    return -1;
}

static int SLTCResolveNamedTypeFields(SLTypeCheckCtx* c, uint32_t namedIndex) {
    const SLTCNamedType* nt = &c->namedTypes[namedIndex];
    int32_t              declNode = nt->declNode;
    SLAstKind            kind = c->ast->nodes[declNode].kind;
    int32_t              child;
    uint32_t             fieldStart = c->fieldLen;
    uint32_t             fieldCount = 0;
    int                  sawDependent = 0;
    int                  sawEmbedded = 0;

    child = SLAstFirstChild(c->ast, declNode);
    if (kind == SLAst_ENUM && child >= 0
        && (c->ast->nodes[child].kind == SLAst_TYPE_NAME
            || c->ast->nodes[child].kind == SLAst_TYPE_PTR
            || c->ast->nodes[child].kind == SLAst_TYPE_ARRAY
            || c->ast->nodes[child].kind == SLAst_TYPE_REF
            || c->ast->nodes[child].kind == SLAst_TYPE_MUTREF
            || c->ast->nodes[child].kind == SLAst_TYPE_SLICE
            || c->ast->nodes[child].kind == SLAst_TYPE_MUTSLICE
            || c->ast->nodes[child].kind == SLAst_TYPE_VARRAY))
    {
        child = SLAstNextSibling(c->ast, child);
    }

    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_FIELD && (kind == SLAst_STRUCT || kind == SLAst_UNION)) {
            int32_t  typeNode = SLAstFirstChild(c->ast, child);
            int32_t  typeId;
            uint16_t fieldFlags = 0;
            uint32_t lenNameStart = 0;
            uint32_t lenNameEnd = 0;
            uint32_t i;
            int      isEmbedded = (n->flags & SLAstFlag_FIELD_EMBEDDED) != 0;
            if (typeNode < 0) {
                return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
            }

            if (isEmbedded) {
                int32_t embeddedDeclNode;
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
                if (typeId < 0 || (uint32_t)typeId >= c->typeLen
                    || c->types[typeId].kind != SLTCType_NAMED)
                {
                    return SLTCFailSpan(
                        c, SLDiag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                }
                embeddedDeclNode = c->types[typeId].declNode;
                if (embeddedDeclNode < 0 || (uint32_t)embeddedDeclNode >= c->ast->len
                    || c->ast->nodes[embeddedDeclNode].kind != SLAst_STRUCT)
                {
                    return SLTCFailSpan(
                        c, SLDiag_EMBEDDED_TYPE_NOT_STRUCT, n->dataStart, n->dataEnd);
                }
                fieldFlags = SLTCFieldFlag_EMBEDDED;
                sawEmbedded = 1;
            } else if (c->ast->nodes[typeNode].kind == SLAst_TYPE_VARRAY) {
                int32_t  elemTypeNode;
                int32_t  elemType;
                int32_t  ptrType;
                int32_t  lenFieldType = -1;
                uint32_t lenFieldIdx;
                if (kind != SLAst_STRUCT) {
                    return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
                }
                sawDependent = 1;
                lenNameStart = c->ast->nodes[typeNode].dataStart;
                lenNameEnd = c->ast->nodes[typeNode].dataEnd;

                lenFieldIdx = fieldStart;
                while (lenFieldIdx < c->fieldLen) {
                    if ((c->fields[lenFieldIdx].flags & SLTCFieldFlag_DEPENDENT) == 0
                        && SLNameEqSlice(
                            c->src,
                            c->fields[lenFieldIdx].nameStart,
                            c->fields[lenFieldIdx].nameEnd,
                            lenNameStart,
                            lenNameEnd))
                    {
                        lenFieldType = c->fields[lenFieldIdx].typeId;
                        break;
                    }
                    lenFieldIdx++;
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

            for (i = fieldStart; i < c->fieldLen; i++) {
                if (SLNameEqSlice(
                        c->src,
                        c->fields[i].nameStart,
                        c->fields[i].nameEnd,
                        n->dataStart,
                        n->dataEnd))
                {
                    return SLTCFailSpan(c, SLDiag_DUPLICATE_SYMBOL, n->dataStart, n->dataEnd);
                }
            }
            if (c->fieldLen >= c->fieldCap) {
                return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
            }
            c->fields[c->fieldLen].nameStart = n->dataStart;
            c->fields[c->fieldLen].nameEnd = n->dataEnd;
            c->fields[c->fieldLen].typeId = typeId;
            c->fields[c->fieldLen].lenNameStart = lenNameStart;
            c->fields[c->fieldLen].lenNameEnd = lenNameEnd;
            c->fields[c->fieldLen].flags = fieldFlags;
            c->fieldLen++;
            fieldCount++;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    c->types[nt->typeId].fieldStart = fieldStart;
    c->types[nt->typeId].fieldCount = (uint16_t)fieldCount;
    return 0;
}

static int SLTCResolveAllNamedTypeFields(SLTypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->namedTypeLen; i++) {
        if (SLTCResolveNamedTypeFields(c, i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLTCCheckEmbeddedCycleFrom(SLTypeCheckCtx* c, int32_t typeId) {
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

static int SLTCCheckEmbeddedCycles(SLTypeCheckCtx* c) {
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

static int SLTCPropagateVarSizeNamedTypes(SLTypeCheckCtx* c) {
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

static int SLTCReadFunctionSig(
    SLTypeCheckCtx* c,
    int32_t         funNode,
    int32_t*        outReturnType,
    uint32_t*       outParamCount,
    int*            outHasBody) {
    int32_t  child = SLAstFirstChild(c->ast, funNode);
    uint32_t paramCount = 0;
    int32_t  returnType = c->typeVoid;
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
            c->scratchParamTypes[paramCount++] = typeId;
        } else if (
            (n->kind == SLAst_TYPE_NAME || n->kind == SLAst_TYPE_PTR || n->kind == SLAst_TYPE_ARRAY
             || n->kind == SLAst_TYPE_REF || n->kind == SLAst_TYPE_MUTREF
             || n->kind == SLAst_TYPE_SLICE || n->kind == SLAst_TYPE_MUTSLICE
             || n->kind == SLAst_TYPE_VARRAY || n->kind == SLAst_TYPE_OPTIONAL)
            && n->flags == 1)
        {
            if (SLTCResolveTypeNode(c, child, &returnType) != 0) {
                return -1;
            }
            if (returnType == c->typeVoid) {
                return SLTCFailNode(c, child, SLDiag_VOID_RETURN_TYPE);
            }
            if (SLTCTypeContainsVarSizeByValue(c, returnType)) {
                return SLTCFailNode(c, child, SLDiag_TYPE_MISMATCH);
            }
        } else if (n->kind == SLAst_BLOCK) {
            hasBody = 1;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    *outReturnType = returnType;
    *outParamCount = paramCount;
    *outHasBody = hasBody;
    return 0;
}

static int SLTCCollectFunctionFromNode(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    int32_t          existing;
    int32_t          returnType;
    uint32_t         paramCount;
    int              hasBody;

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

    if (SLTCReadFunctionSig(c, nodeId, &returnType, &paramCount, &hasBody) != 0) {
        return -1;
    }

    existing = SLTCFindFunctionIndex(c, n->dataStart, n->dataEnd);
    if (existing >= 0) {
        SLTCFunction* f = &c->funcs[existing];
        uint32_t      i;
        if (f->paramCount != paramCount || f->returnType != returnType) {
            return SLTCFailSpan(c, SLDiag_TYPE_MISMATCH, n->dataStart, n->dataEnd);
        }
        for (i = 0; i < paramCount; i++) {
            if (c->funcParamTypes[f->paramTypeStart + i] != c->scratchParamTypes[i]) {
                return SLTCFailSpan(c, SLDiag_TYPE_MISMATCH, n->dataStart, n->dataEnd);
            }
        }
        if (hasBody) {
            if (f->defNode >= 0) {
                return SLTCFailSpan(c, SLDiag_DUPLICATE_SYMBOL, n->dataStart, n->dataEnd);
            }
            f->defNode = nodeId;
        }
        return 0;
    }

    if (c->funcLen >= c->funcCap || c->funcParamLen + paramCount > c->funcParamCap) {
        return SLTCFailNode(c, nodeId, SLDiag_ARENA_OOM);
    }

    {
        uint32_t      i;
        uint32_t      idx = c->funcLen++;
        SLTCFunction* f = &c->funcs[idx];
        f->nameStart = n->dataStart;
        f->nameEnd = n->dataEnd;
        f->returnType = returnType;
        f->paramTypeStart = c->funcParamLen;
        f->paramCount = (uint16_t)paramCount;
        f->declNode = nodeId;
        f->defNode = hasBody ? nodeId : -1;
        f->funcTypeId = -1;
        for (i = 0; i < paramCount; i++) {
            c->funcParamTypes[c->funcParamLen++] = c->scratchParamTypes[i];
        }
    }

    return 0;
}

static int SLTCFinalizeFunctionTypes(SLTypeCheckCtx* c) {
    uint32_t i;
    for (i = 0; i < c->funcLen; i++) {
        int32_t typeId = SLTCInternFunctionType(
            c, (int32_t)i, c->funcs[i].nameStart, c->funcs[i].nameEnd);
        if (typeId < 0) {
            return -1;
        }
        c->funcs[i].funcTypeId = typeId;
    }
    return 0;
}

static int32_t SLTCLocalFind(SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd) {
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

static int SLTCLocalAdd(SLTypeCheckCtx* c, uint32_t nameStart, uint32_t nameEnd, int32_t typeId) {
    if (c->localLen >= c->localCap) {
        return SLTCFailSpan(c, SLDiag_ARENA_OOM, nameStart, nameEnd);
    }
    c->locals[c->localLen].nameStart = nameStart;
    c->locals[c->localLen].nameEnd = nameEnd;
    c->locals[c->localLen].typeId = typeId;
    c->localLen++;
    return 0;
}

static int SLTCFieldLookup(
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
        if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != SLTCType_NAMED)
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
        if (embedIdx < 0) {
            return -1;
        }
        typeId = c->fields[embedIdx].typeId;
    }
    return -1;
}

static int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);

static int SLTCResolveTypeArgExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_TYPE, 0, 0);
    }
    n = &c->ast->nodes[nodeId];
    if (n->kind == SLAst_IDENT) {
        int32_t typeId = SLTCFindBuiltinType(c, n->dataStart, n->dataEnd);
        if (typeId >= 0) {
            *outType = typeId;
            return 0;
        }
        {
            int32_t namedIndex = SLTCFindNamedTypeIndex(c, n->dataStart, n->dataEnd);
            if (namedIndex >= 0) {
                *outType = c->namedTypes[namedIndex].typeId;
                return 0;
            }
        }
        return SLTCFailSpan(c, SLDiag_UNKNOWN_TYPE, n->dataStart, n->dataEnd);
    }
    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
}

static int SLTCExprIsAssignable(SLTypeCheckCtx* c, int32_t exprNode) {
    const SLAstNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[exprNode];
    if (n->kind == SLAst_IDENT) {
        return 1;
    }
    if (n->kind == SLAst_INDEX) {
        SLTCIndexBaseInfo info;
        int32_t           baseNode = SLAstFirstChild(c->ast, exprNode);
        int32_t           baseType;
        if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
            return 0;
        }
        if (baseNode < 0 || SLTCTypeExpr(c, baseNode, &baseType) != 0 || baseType < 0
            || (uint32_t)baseType >= c->typeLen)
        {
            return 0;
        }
        if (SLTCResolveIndexBaseInfo(c, baseType, &info) != 0) {
            return 0;
        }
        if (!info.indexable) {
            return 0;
        }
        if (c->types[baseType].kind == SLTCType_ARRAY || c->types[baseType].kind == SLTCType_PTR) {
            return 1;
        }
        return info.sliceMutable;
    }
    if (n->kind == SLAst_FIELD_EXPR) {
        int32_t  recvNode = SLAstFirstChild(c->ast, exprNode);
        int32_t  recvType;
        int32_t  fieldType;
        uint32_t fieldIndex = 0;
        if (recvNode < 0) {
            return 0;
        }
        if (SLTCTypeExpr(c, recvNode, &recvType) != 0) {
            return 0;
        }
        if (SLTCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, &fieldIndex) != 0) {
            return 0;
        }
        if ((c->fields[fieldIndex].flags & SLTCFieldFlag_DEPENDENT) != 0) {
            return 0;
        }
        if (recvType >= 0 && (uint32_t)recvType < c->typeLen
            && c->types[recvType].kind == SLTCType_REF)
        {
            return SLTCTypeIsMutable(&c->types[recvType]);
        }
        return 1;
    }
    if (n->kind == SLAst_UNARY && n->op == SLTok_MUL) {
        int32_t rhsNode = SLAstFirstChild(c->ast, exprNode);
        int32_t rhsType;
        if (rhsNode < 0 || SLTCTypeExpr(c, rhsNode, &rhsType) != 0 || rhsType < 0
            || (uint32_t)rhsType >= c->typeLen)
        {
            return 0;
        }
        if (c->types[rhsType].kind == SLTCType_PTR) {
            return 1;
        }
        if (c->types[rhsType].kind == SLTCType_REF) {
            return SLTCTypeIsMutable(&c->types[rhsType]);
        }
        return 0;
    }
    return 0;
}

static int SLTCTypeStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);

static int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLAstNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case SLAst_IDENT: {
            int32_t localIdx = SLTCLocalFind(c, n->dataStart, n->dataEnd);
            if (localIdx >= 0) {
                *outType = c->locals[localIdx].typeId;
                return 0;
            }
            {
                int32_t fnIdx = SLTCFindFunctionIndex(c, n->dataStart, n->dataEnd);
                if (fnIdx >= 0) {
                    *outType = c->funcs[fnIdx].funcTypeId;
                    return 0;
                }
            }
            return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
        }
        case SLAst_INT:    *outType = c->typeUntypedInt; return 0;
        case SLAst_FLOAT:  *outType = c->typeUntypedFloat; return 0;
        case SLAst_STRING: *outType = c->typeStr; return 0;
        case SLAst_BOOL:   *outType = c->typeBool; return 0;
        case SLAst_CALL:   {
            int32_t  calleeNode = SLAstFirstChild(c->ast, nodeId);
            int32_t  calleeType;
            int32_t  funcIdx;
            int32_t  argNode;
            uint32_t argIndex = 0;
            if (calleeNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_NOT_CALLABLE);
            }
            if (c->ast->nodes[calleeNode].kind == SLAst_IDENT) {
                const SLAstNode* callee = &c->ast->nodes[calleeNode];
                if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "len")) {
                    int32_t strArgNode = SLAstNextSibling(c->ast, calleeNode);
                    int32_t strArgType;
                    int32_t nextArgNode;
                    int32_t u32Type;
                    if (strArgNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    if (SLTCTypeExpr(c, strArgNode, &strArgType) != 0) {
                        return -1;
                    }
                    if (!SLTCTypeSupportsLen(c, strArgType)) {
                        return SLTCFailNode(c, strArgNode, SLDiag_TYPE_MISMATCH);
                    }
                    nextArgNode = SLAstNextSibling(c->ast, strArgNode);
                    if (nextArgNode >= 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    u32Type = SLTCFindBuiltinByKind(c, SLBuiltin_U32);
                    if (u32Type < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                    }
                    *outType = u32Type;
                    return 0;
                }
                if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "cstr")) {
                    int32_t strArgNode = SLAstNextSibling(c->ast, calleeNode);
                    int32_t strArgType;
                    int32_t nextArgNode;
                    int32_t u8Type;
                    int32_t u8PtrType;
                    if (strArgNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    if (SLTCTypeExpr(c, strArgNode, &strArgType) != 0) {
                        return -1;
                    }
                    if (!SLTCCanAssign(c, c->typeStr, strArgType)) {
                        return SLTCFailNode(c, strArgNode, SLDiag_TYPE_MISMATCH);
                    }
                    nextArgNode = SLAstNextSibling(c->ast, strArgNode);
                    if (nextArgNode >= 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    u8Type = SLTCFindBuiltinByKind(c, SLBuiltin_U8);
                    if (u8Type < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_TYPE);
                    }
                    u8PtrType = SLTCInternPtrType(c, u8Type, callee->start, callee->end);
                    if (u8PtrType < 0) {
                        return -1;
                    }
                    *outType = u8PtrType;
                    return 0;
                }
                if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "new")) {
                    int32_t allocArgNode = SLAstNextSibling(c->ast, calleeNode);
                    int32_t typeArgNode;
                    int32_t countArgNode;
                    int32_t nextArgNode;
                    int32_t allocArgType;
                    int32_t allocParamType;
                    int32_t elemType;
                    int32_t resultType;
                    int32_t countType;
                    int64_t countValue = 0;
                    int     countIsConst = 0;
                    if (allocArgNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    typeArgNode = SLAstNextSibling(c->ast, allocArgNode);
                    if (typeArgNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    countArgNode = SLAstNextSibling(c->ast, typeArgNode);
                    nextArgNode = countArgNode >= 0 ? SLAstNextSibling(c->ast, countArgNode) : -1;
                    if (nextArgNode >= 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }

                    if (SLTCTypeExpr(c, allocArgNode, &allocArgType) != 0) {
                        return -1;
                    }
                    allocParamType = SLTCInternRefType(
                        c, c->typeMemAllocator, 1, callee->start, callee->end);
                    if (allocParamType < 0) {
                        return -1;
                    }
                    if (!SLTCCanAssign(c, allocParamType, allocArgType)) {
                        return SLTCFailNode(c, allocArgNode, SLDiag_TYPE_MISMATCH);
                    }

                    if (SLTCResolveTypeArgExpr(c, typeArgNode, &elemType) != 0) {
                        return -1;
                    }
                    if (SLTCTypeContainsVarSizeByValue(c, elemType)) {
                        return SLTCFailNode(c, typeArgNode, SLDiag_TYPE_MISMATCH);
                    }

                    if (countArgNode >= 0) {
                        if (SLTCTypeExpr(c, countArgNode, &countType) != 0) {
                            return -1;
                        }
                        if (!SLTCIsIntegerType(c, countType)) {
                            return SLTCFailNode(c, countArgNode, SLDiag_TYPE_MISMATCH);
                        }
                        if (SLTCConstIntExpr(c, countArgNode, &countValue, &countIsConst) != 0) {
                            return SLTCFailNode(c, countArgNode, SLDiag_TYPE_MISMATCH);
                        }
                        if (countIsConst && countValue < 0) {
                            return SLTCFailNode(c, countArgNode, SLDiag_TYPE_MISMATCH);
                        }
                    }

                    if (countArgNode >= 0) {
                        if (countIsConst && countValue > 0) {
                            /* Compile-time constant count: new returns *[T N] */
                            int32_t arrayType = SLTCInternArrayType(
                                c, elemType, (uint32_t)countValue, callee->start, callee->end);
                            if (arrayType < 0) {
                                return -1;
                            }
                            resultType = SLTCInternPtrType(
                                c, arrayType, callee->start, callee->end);
                        } else {
                            /* Runtime count: new returns *[T] (element pointer) */
                            int32_t sliceType = SLTCInternSliceType(
                                c, elemType, 0, callee->start, callee->end);
                            if (sliceType < 0) {
                                return -1;
                            }
                            resultType = SLTCInternPtrType(
                                c, sliceType, callee->start, callee->end);
                        }
                    } else {
                        resultType = SLTCInternPtrType(c, elemType, callee->start, callee->end);
                    }
                    if (resultType < 0) {
                        return -1;
                    }
                    *outType = resultType;
                    return 0;
                }
                if (SLNameEqLiteral(c->src, callee->dataStart, callee->dataEnd, "panic")) {
                    int32_t msgArgNode = SLAstNextSibling(c->ast, calleeNode);
                    int32_t msgArgType;
                    int32_t nextArgNode;
                    if (msgArgNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    if (SLTCTypeExpr(c, msgArgNode, &msgArgType) != 0) {
                        return -1;
                    }
                    if (!SLTCCanAssign(c, c->typeStr, msgArgType)) {
                        return SLTCFailNode(c, msgArgNode, SLDiag_TYPE_MISMATCH);
                    }
                    nextArgNode = SLAstNextSibling(c->ast, msgArgNode);
                    if (nextArgNode >= 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                    }
                    *outType = c->typeVoid;
                    return 0;
                }
            }
            if (SLTCTypeExpr(c, calleeNode, &calleeType) != 0) {
                return -1;
            }
            if (calleeType < 0 || (uint32_t)calleeType >= c->typeLen
                || c->types[calleeType].kind != SLTCType_FUNCTION)
            {
                return SLTCFailNode(c, calleeNode, SLDiag_NOT_CALLABLE);
            }
            funcIdx = c->types[calleeType].funcIndex;
            argNode = SLAstNextSibling(c->ast, calleeNode);
            while (argNode >= 0) {
                int32_t argType;
                if (argIndex >= c->funcs[funcIdx].paramCount) {
                    return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
                }
                if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                    return -1;
                }
                if (!SLTCCanAssign(
                        c, c->funcParamTypes[c->funcs[funcIdx].paramTypeStart + argIndex], argType))
                {
                    return SLTCFailNode(c, argNode, SLDiag_TYPE_MISMATCH);
                }
                argIndex++;
                argNode = SLAstNextSibling(c->ast, argNode);
            }
            if (argIndex != c->funcs[funcIdx].paramCount) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            *outType = c->funcs[funcIdx].returnType;
            return 0;
        }
        case SLAst_CAST: {
            int32_t exprNode = SLAstFirstChild(c->ast, nodeId);
            int32_t typeNode;
            int32_t ignoredType;
            int32_t targetType;
            if (exprNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            typeNode = SLAstNextSibling(c->ast, exprNode);
            if (typeNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
            }
            if (SLTCTypeExpr(c, exprNode, &ignoredType) != 0) {
                return -1;
            }
            if (SLTCResolveTypeNode(c, typeNode, &targetType) != 0) {
                return -1;
            }
            *outType = targetType;
            return 0;
        }
        case SLAst_SIZEOF: {
            int32_t innerNode = SLAstFirstChild(c->ast, nodeId);
            int32_t innerType;
            if (innerNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (n->flags == 1) {
                if (SLTCResolveTypeNode(c, innerNode, &innerType) == 0) {
                    if (SLTCTypeContainsVarSizeByValue(c, innerType)) {
                        return SLTCFailNode(c, innerNode, SLDiag_TYPE_MISMATCH);
                    }
                    *outType = c->typeUsize;
                    return 0;
                }
                if (c->ast->nodes[innerNode].kind == SLAst_TYPE_NAME) {
                    int32_t localIdx = SLTCLocalFind(
                        c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
                    if (localIdx >= 0) {
                        if (c->diag != NULL) {
                            *c->diag = (SLDiag){ 0 };
                        }
                        *outType = c->typeUsize;
                        return 0;
                    }
                    {
                        int32_t fnIdx = SLTCFindFunctionIndex(
                            c,
                            c->ast->nodes[innerNode].dataStart,
                            c->ast->nodes[innerNode].dataEnd);
                        if (fnIdx >= 0) {
                            if (c->diag != NULL) {
                                *c->diag = (SLDiag){ 0 };
                            }
                            *outType = c->typeUsize;
                            return 0;
                        }
                    }
                }
            } else {
                if (SLTCTypeExpr(c, innerNode, &innerType) != 0) {
                    return -1;
                }
            }
            *outType = c->typeUsize;
            return 0;
        }
        case SLAst_FIELD_EXPR: {
            int32_t recvNode = SLAstFirstChild(c->ast, nodeId);
            int32_t recvType;
            int32_t fieldType;
            if (recvNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_SYMBOL);
            }
            if (SLTCTypeExpr(c, recvNode, &recvType) != 0) {
                return -1;
            }
            if (SLTCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType, NULL) != 0) {
                return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
            }
            *outType = fieldType;
            return 0;
        }
        case SLAst_INDEX: {
            int32_t           baseNode = SLAstFirstChild(c->ast, nodeId);
            int32_t           baseType;
            SLTCIndexBaseInfo info;
            if (baseNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, baseNode, &baseType) != 0) {
                return -1;
            }
            if (SLTCResolveIndexBaseInfo(c, baseType, &info) != 0 || !info.indexable
                || info.elemType < 0)
            {
                return SLTCFailNode(c, baseNode, SLDiag_TYPE_MISMATCH);
            }

            if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
                int     hasStart = (n->flags & SLAstFlag_INDEX_HAS_START) != 0;
                int     hasEnd = (n->flags & SLAstFlag_INDEX_HAS_END) != 0;
                int32_t child = SLAstNextSibling(c->ast, baseNode);
                int32_t startNode = -1;
                int32_t endNode = -1;
                int32_t sliceType;
                int64_t startValue = 0;
                int64_t endValue = 0;
                int     startIsConst = 0;
                int     endIsConst = 0;

                if (!info.sliceable) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }

                if (hasStart) {
                    int32_t startType;
                    startNode = child;
                    if (startNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                    }
                    if (SLTCTypeExpr(c, startNode, &startType) != 0) {
                        return -1;
                    }
                    if (!SLTCIsIntegerType(c, startType)) {
                        return SLTCFailNode(c, startNode, SLDiag_TYPE_MISMATCH);
                    }
                    if (SLTCConstIntExpr(c, startNode, &startValue, &startIsConst) != 0) {
                        return SLTCFailNode(c, startNode, SLDiag_TYPE_MISMATCH);
                    }
                    child = SLAstNextSibling(c->ast, child);
                }
                if (hasEnd) {
                    int32_t endType;
                    endNode = child;
                    if (endNode < 0) {
                        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                    }
                    if (SLTCTypeExpr(c, endNode, &endType) != 0) {
                        return -1;
                    }
                    if (!SLTCIsIntegerType(c, endType)) {
                        return SLTCFailNode(c, endNode, SLDiag_TYPE_MISMATCH);
                    }
                    if (SLTCConstIntExpr(c, endNode, &endValue, &endIsConst) != 0) {
                        return SLTCFailNode(c, endNode, SLDiag_TYPE_MISMATCH);
                    }
                    child = SLAstNextSibling(c->ast, child);
                }
                if (child >= 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                }

                if ((startIsConst && startValue < 0) || (endIsConst && endValue < 0)) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }

                if (info.hasKnownLen) {
                    int     startKnown = !hasStart || startIsConst;
                    int     endKnown = !hasEnd || endIsConst;
                    int64_t startBound = hasStart ? startValue : 0;
                    int64_t endBound = hasEnd ? endValue : (int64_t)info.knownLen;
                    if (startKnown && endKnown) {
                        if (startBound > endBound || endBound > (int64_t)info.knownLen) {
                            return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                        }
                    } else {
                        SLTCMarkRuntimeBoundsCheck(c, nodeId);
                    }
                } else {
                    if (startIsConst && endIsConst && startValue > endValue) {
                        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                    }
                    SLTCMarkRuntimeBoundsCheck(c, nodeId);
                }

                sliceType = SLTCInternSliceType(
                    c, info.elemType, info.sliceMutable, n->start, n->end);
                if (sliceType < 0) {
                    return -1;
                }
                *outType = sliceType;
                return 0;
            }

            {
                int32_t idxNode = SLAstNextSibling(c->ast, baseNode);
                int32_t idxType;
                int64_t idxValue = 0;
                int     idxIsConst = 0;

                if (idxNode < 0 || SLAstNextSibling(c->ast, idxNode) >= 0) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
                }
                if (SLTCTypeExpr(c, idxNode, &idxType) != 0) {
                    return -1;
                }
                if (!SLTCIsIntegerType(c, idxType)) {
                    return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
                }
                if (SLTCConstIntExpr(c, idxNode, &idxValue, &idxIsConst) != 0) {
                    return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
                }
                if (idxIsConst && idxValue < 0) {
                    return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
                }

                if (info.hasKnownLen) {
                    if (idxIsConst) {
                        if (idxValue >= (int64_t)info.knownLen) {
                            return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
                        }
                    } else {
                        SLTCMarkRuntimeBoundsCheck(c, nodeId);
                    }
                } else if (info.sliceable) {
                    SLTCMarkRuntimeBoundsCheck(c, nodeId);
                }
            }

            *outType = info.elemType;
            return 0;
        }
        case SLAst_UNARY: {
            int32_t rhsNode = SLAstFirstChild(c->ast, nodeId);
            int32_t rhsType;
            if (rhsNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, rhsNode, &rhsType) != 0) {
                return -1;
            }
            switch ((SLTokenKind)n->op) {
                case SLTok_ADD:
                case SLTok_SUB:
                    if (!SLTCIsNumericType(c, rhsType)) {
                        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                    }
                    *outType = rhsType;
                    return 0;
                case SLTok_NOT:
                    if (!SLTCIsBoolType(c, rhsType)) {
                        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
                    }
                    *outType = c->typeBool;
                    return 0;
                case SLTok_MUL:
                    if (c->types[rhsType].kind != SLTCType_PTR
                        && c->types[rhsType].kind != SLTCType_REF)
                    {
                        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                    }
                    *outType = c->types[rhsType].baseType;
                    return 0;
                case SLTok_AND: {
                    int32_t refType = SLTCInternRefType(c, rhsType, 1, n->start, n->end);
                    if (refType < 0) {
                        return -1;
                    }
                    *outType = refType;
                    return 0;
                }
                default: return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
        }
        case SLAst_BINARY: {
            int32_t     lhsNode = SLAstFirstChild(c->ast, nodeId);
            int32_t     rhsNode;
            int32_t     lhsType;
            int32_t     rhsType;
            int32_t     commonType;
            SLTokenKind op;
            if (lhsNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            rhsNode = SLAstNextSibling(c->ast, lhsNode);
            if (rhsNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, lhsNode, &lhsType) != 0 || SLTCTypeExpr(c, rhsNode, &rhsType) != 0)
            {
                return -1;
            }
            op = (SLTokenKind)n->op;

            if (op == SLTok_ASSIGN || op == SLTok_ADD_ASSIGN || op == SLTok_SUB_ASSIGN
                || op == SLTok_MUL_ASSIGN || op == SLTok_DIV_ASSIGN || op == SLTok_MOD_ASSIGN
                || op == SLTok_AND_ASSIGN || op == SLTok_OR_ASSIGN || op == SLTok_XOR_ASSIGN
                || op == SLTok_LSHIFT_ASSIGN || op == SLTok_RSHIFT_ASSIGN)
            {
                if (!SLTCExprIsAssignable(c, lhsNode)) {
                    return SLTCFailNode(c, lhsNode, SLDiag_TYPE_MISMATCH);
                }
                if (!SLTCCanAssign(c, lhsType, rhsType)) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }
                if (op != SLTok_ASSIGN && !SLTCIsNumericType(c, lhsType)) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }
                *outType = lhsType;
                return 0;
            }

            if (op == SLTok_LOGICAL_AND || op == SLTok_LOGICAL_OR) {
                if (!SLTCIsBoolType(c, lhsType) || !SLTCIsBoolType(c, rhsType)) {
                    return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
                }
                *outType = c->typeBool;
                return 0;
            }

            /* Allow ?T == null, null == ?T, ?T != null, null != ?T */
            if (op == SLTok_EQ || op == SLTok_NEQ) {
                int lhsIsOpt = c->types[lhsType].kind == SLTCType_OPTIONAL;
                int rhsIsOpt = c->types[rhsType].kind == SLTCType_OPTIONAL;
                int lhsIsNull = c->types[lhsType].kind == SLTCType_NULL;
                int rhsIsNull = c->types[rhsType].kind == SLTCType_NULL;
                if ((lhsIsOpt && rhsIsNull) || (lhsIsNull && rhsIsOpt)) {
                    *outType = c->typeBool;
                    return 0;
                }
            }

            if (SLTCCoerceForBinary(c, lhsType, rhsType, &commonType) != 0) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }

            if (op == SLTok_EQ || op == SLTok_NEQ || op == SLTok_LT || op == SLTok_GT
                || op == SLTok_LTE || op == SLTok_GTE)
            {
                *outType = c->typeBool;
                return 0;
            }

            if (!SLTCIsNumericType(c, commonType)) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            *outType = commonType;
            return 0;
        }
        case SLAst_NULL:   *outType = c->typeNull; return 0;
        case SLAst_UNWRAP: {
            int32_t inner = SLAstFirstChild(c->ast, nodeId);
            int32_t innerType;
            if (inner < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, inner, &innerType) != 0) {
                return -1;
            }
            if (c->types[innerType].kind != SLTCType_OPTIONAL) {
                return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
            *outType = c->types[innerType].baseType;
            return 0;
        }
        default: return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
}

static int SLTCTypeVarLike(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    int32_t          typeNode = SLAstFirstChild(c->ast, nodeId);
    int32_t          initNode;
    int32_t          declType;

    if (typeNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    if (SLTCResolveTypeNode(c, typeNode, &declType) != 0) {
        return -1;
    }
    if (SLTCTypeContainsVarSizeByValue(c, declType)) {
        return SLTCFailNode(c, typeNode, SLDiag_TYPE_MISMATCH);
    }

    initNode = SLAstNextSibling(c->ast, typeNode);
    if (initNode >= 0) {
        int32_t initType;
        if (SLTCTypeExpr(c, initNode, &initType) != 0) {
            return -1;
        }
        if (!SLTCCanAssign(c, declType, initType)) {
            return SLTCFailNode(c, initNode, SLDiag_TYPE_MISMATCH);
        }
    }

    return SLTCLocalAdd(c, n->dataStart, n->dataEnd, declType);
}

/* Describes a narrowable local found in a null-check condition. */
typedef struct {
    int32_t localIdx;  /* index in c->locals[] */
    int32_t innerType; /* T from ?T */
} SLTCNullNarrow;

/*
 * Detects if condNode is a direct null check on a local optional variable:
 *   ident == null   ->  *outIsEq = 1
 *   ident != null   ->  *outIsEq = 0
 *   null == ident   ->  *outIsEq = 1  (symmetric)
 *   null != ident   ->  *outIsEq = 0  (symmetric)
 * Returns 1 if the pattern matched, 0 otherwise.
 */
static int SLTCGetNullNarrow(
    SLTypeCheckCtx* c, int32_t condNode, int* outIsEq, SLTCNullNarrow* out) {
    const SLAstNode* n;
    int32_t          lhs, rhs, identNode;
    SLTokenKind      op;
    int32_t          localIdx;
    int32_t          typeId;

    if (condNode < 0 || (uint32_t)condNode >= c->ast->len) {
        return 0;
    }
    n = &c->ast->nodes[condNode];
    if (n->kind != SLAst_BINARY) {
        return 0;
    }
    op = (SLTokenKind)n->op;
    if (op != SLTok_EQ && op != SLTok_NEQ) {
        return 0;
    }
    lhs = SLAstFirstChild(c->ast, condNode);
    rhs = lhs >= 0 ? SLAstNextSibling(c->ast, lhs) : -1;
    if (lhs < 0 || rhs < 0) {
        return 0;
    }
    /* Identify which side is the ident and which is null. */
    if (c->ast->nodes[lhs].kind == SLAst_IDENT && c->ast->nodes[rhs].kind == SLAst_NULL) {
        identNode = lhs;
    } else if (c->ast->nodes[rhs].kind == SLAst_IDENT && c->ast->nodes[lhs].kind == SLAst_NULL) {
        identNode = rhs;
    } else {
        return 0;
    }
    {
        const SLAstNode* id = &c->ast->nodes[identNode];
        localIdx = SLTCLocalFind(c, id->dataStart, id->dataEnd);
    }
    if (localIdx < 0) {
        return 0;
    }
    typeId = c->locals[localIdx].typeId;
    if (c->types[typeId].kind != SLTCType_OPTIONAL) {
        return 0;
    }
    *outIsEq = (op == SLTok_EQ);
    out->localIdx = localIdx;
    out->innerType = c->types[typeId].baseType;
    return 1;
}

/* Returns 1 if the last statement of blockNode is an unconditional terminator. */
static int SLTCBlockTerminates(SLTypeCheckCtx* c, int32_t blockNode) {
    int32_t child = SLAstFirstChild(c->ast, blockNode);
    int32_t last = -1;
    while (child >= 0) {
        last = child;
        child = SLAstNextSibling(c->ast, child);
    }
    if (last < 0) {
        return 0;
    }
    switch (c->ast->nodes[last].kind) {
        case SLAst_RETURN:
        case SLAst_BREAK:
        case SLAst_CONTINUE: return 1;
        default:             return 0;
    }
}

/*
 * Saved narrowing: remembers the original type of a local so it can be restored
 * after the narrowed region ends.
 */
typedef struct {
    int32_t localIdx;
    int32_t savedType;
} SLTCNarrowSave;

static int SLTCTypeBlock(
    SLTypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t       savedLocalLen = c->localLen;
    int32_t        child = SLAstFirstChild(c->ast, blockNode);
    SLTCNarrowSave narrows[8]; /* saved narrowings applied during this block */
    int            narrowLen = 0;
    int            i;

    while (child >= 0) {
        int32_t next = SLAstNextSibling(c->ast, child);
        if (SLTCTypeStmt(c, child, returnType, loopDepth, switchDepth) != 0) {
            for (i = 0; i < narrowLen; i++) {
                c->locals[narrows[i].localIdx].typeId = narrows[i].savedType;
            }
            c->localLen = savedLocalLen;
            return -1;
        }
        /*
         * Guard-pattern continuation narrowing:
         *   if x == null { <terminates> }   ->  x narrows to T for the rest of the block
         *   if x != null { <terminates> }   ->  x narrows to null for the rest of the block
         * Only fires when there is more code after the if (next >= 0) and no else clause.
         */
        if (next >= 0 && c->ast->nodes[child].kind == SLAst_IF && narrowLen < 8) {
            int32_t        condNode = SLAstFirstChild(c->ast, child);
            int32_t        thenNode = condNode >= 0 ? SLAstNextSibling(c->ast, condNode) : -1;
            int32_t        elseNode = thenNode >= 0 ? SLAstNextSibling(c->ast, thenNode) : -1;
            SLTCNullNarrow narrow;
            int            isEq;
            if (elseNode < 0 && thenNode >= 0 && condNode >= 0 && SLTCBlockTerminates(c, thenNode)
                && SLTCGetNullNarrow(c, condNode, &isEq, &narrow))
            {
                int32_t contType = isEq ? narrow.innerType : c->typeNull;
                narrows[narrowLen].localIdx = narrow.localIdx;
                narrows[narrowLen].savedType = c->locals[narrow.localIdx].typeId;
                narrowLen++;
                c->locals[narrow.localIdx].typeId = contType;
            }
        }
        child = next;
    }
    for (i = 0; i < narrowLen; i++) {
        c->locals[narrows[i].localIdx].typeId = narrows[i].savedType;
    }
    c->localLen = savedLocalLen;
    return 0;
}

static int SLTCTypeForStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t savedLocalLen = c->localLen;
    int32_t  child = SLAstFirstChild(c->ast, nodeId);
    int32_t  nodes[4];
    int      count = 0;
    int      i;

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = SLAstNextSibling(c->ast, child);
    }

    if (count == 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }

    for (i = 0; i < count - 1; i++) {
        const SLAstNode* n = &c->ast->nodes[nodes[i]];
        if (n->kind == SLAst_VAR || n->kind == SLAst_CONST) {
            if (SLTCTypeVarLike(c, nodes[i]) != 0) {
                return -1;
            }
        } else {
            int32_t t;
            if (SLTCTypeExpr(c, nodes[i], &t) != 0) {
                return -1;
            }
            if (i == 1 && count == 4 && !SLTCIsBoolType(c, t)) {
                return SLTCFailNode(c, nodes[i], SLDiag_EXPECTED_BOOL);
            }
            if (i == 0 && count == 2 && !SLTCIsBoolType(c, t)) {
                return SLTCFailNode(c, nodes[i], SLDiag_EXPECTED_BOOL);
            }
        }
    }

    if (c->ast->nodes[nodes[count - 1]].kind != SLAst_BLOCK) {
        return SLTCFailNode(c, nodes[count - 1], SLDiag_UNEXPECTED_TOKEN);
    }

    if (SLTCTypeBlock(c, nodes[count - 1], returnType, loopDepth + 1, switchDepth) != 0) {
        return -1;
    }

    c->localLen = savedLocalLen;
    return 0;
}

static int SLTCTypeSwitchStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const SLAstNode* sw = &c->ast->nodes[nodeId];
    int32_t          child = SLAstFirstChild(c->ast, nodeId);
    int32_t          subjectType = -1;

    if (sw->flags == 1) {
        if (child < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        if (SLTCTypeExpr(c, child, &subjectType) != 0) {
            return -1;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    while (child >= 0) {
        const SLAstNode* clause = &c->ast->nodes[child];
        if (clause->kind == SLAst_CASE) {
            int32_t caseChild = SLAstFirstChild(c->ast, child);
            int32_t bodyNode = -1;
            while (caseChild >= 0) {
                int32_t next = SLAstNextSibling(c->ast, caseChild);
                if (next < 0) {
                    bodyNode = caseChild;
                    break;
                }
                if (sw->flags == 1) {
                    int32_t labelType;
                    if (SLTCTypeExpr(c, caseChild, &labelType) != 0) {
                        return -1;
                    }
                    if (!SLTCCanAssign(c, subjectType, labelType)) {
                        return SLTCFailNode(c, caseChild, SLDiag_TYPE_MISMATCH);
                    }
                } else {
                    int32_t condType;
                    if (SLTCTypeExpr(c, caseChild, &condType) != 0) {
                        return -1;
                    }
                    if (!SLTCIsBoolType(c, condType)) {
                        return SLTCFailNode(c, caseChild, SLDiag_EXPECTED_BOOL);
                    }
                }
                caseChild = next;
            }
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
                return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                return -1;
            }
        } else if (clause->kind == SLAst_DEFAULT) {
            int32_t bodyNode = SLAstFirstChild(c->ast, child);
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
                return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                return -1;
            }
        } else {
            return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
        }
        child = SLAstNextSibling(c->ast, child);
    }

    return 0;
}

static int SLTCTypeStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const SLAstNode* n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAst_BLOCK:     return SLTCTypeBlock(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAst_VAR:
        case SLAst_CONST:     return SLTCTypeVarLike(c, nodeId);
        case SLAst_EXPR_STMT: {
            int32_t expr = SLAstFirstChild(c->ast, nodeId);
            int32_t t;
            if (expr < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            return SLTCTypeExpr(c, expr, &t);
        }
        case SLAst_RETURN: {
            int32_t expr = SLAstFirstChild(c->ast, nodeId);
            if (expr < 0) {
                if (returnType != c->typeVoid) {
                    return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                }
                return 0;
            }
            {
                int32_t t;
                if (SLTCTypeExpr(c, expr, &t) != 0) {
                    return -1;
                }
                if (!SLTCCanAssign(c, returnType, t)) {
                    return SLTCFailNode(c, expr, SLDiag_TYPE_MISMATCH);
                }
                return 0;
            }
        }
        case SLAst_IF: {
            int32_t        cond = SLAstFirstChild(c->ast, nodeId);
            int32_t        thenNode;
            int32_t        elseNode;
            int32_t        condType;
            SLTCNullNarrow narrow;
            int            isEq;
            int            hasNarrow;
            if (cond < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
            }
            thenNode = SLAstNextSibling(c->ast, cond);
            if (thenNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            if (!SLTCIsBoolType(c, condType)) {
                return SLTCFailNode(c, cond, SLDiag_EXPECTED_BOOL);
            }
            elseNode = SLAstNextSibling(c->ast, thenNode);
            hasNarrow = SLTCGetNullNarrow(c, cond, &isEq, &narrow);
            if (hasNarrow) {
                /*
                 * Apply branch narrowing:
                 *   x == null  -> then: x is null;  else: x is T
                 *   x != null  -> then: x is T;     else: x is null
                 */
                int32_t origType = c->locals[narrow.localIdx].typeId;
                int32_t trueType = isEq ? c->typeNull : narrow.innerType;
                int32_t falseType = isEq ? narrow.innerType : c->typeNull;
                c->locals[narrow.localIdx].typeId = trueType;
                if (SLTCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                    c->locals[narrow.localIdx].typeId = origType;
                    return -1;
                }
                c->locals[narrow.localIdx].typeId = falseType;
                if (elseNode >= 0
                    && SLTCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                {
                    c->locals[narrow.localIdx].typeId = origType;
                    return -1;
                }
                c->locals[narrow.localIdx].typeId = origType;
            } else {
                if (SLTCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                    return -1;
                }
                if (elseNode >= 0
                    && SLTCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_FOR:    return SLTCTypeForStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAst_SWITCH: return SLTCTypeSwitchStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAst_BREAK:
            if (loopDepth <= 0 && switchDepth <= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case SLAst_CONTINUE:
            if (loopDepth <= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case SLAst_DEFER: {
            int32_t stmt = SLAstFirstChild(c->ast, nodeId);
            if (stmt < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return SLTCTypeStmt(c, stmt, returnType, loopDepth, switchDepth);
        }
        case SLAst_ASSERT: {
            int32_t cond = SLAstFirstChild(c->ast, nodeId);
            int32_t condType;
            int32_t fmtNode;
            if (cond < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
            }
            if (SLTCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            if (!SLTCIsBoolType(c, condType)) {
                return SLTCFailNode(c, cond, SLDiag_EXPECTED_BOOL);
            }
            fmtNode = SLAstNextSibling(c->ast, cond);
            if (fmtNode >= 0) {
                int32_t fmtType;
                int32_t argNode;
                if (SLTCTypeExpr(c, fmtNode, &fmtType) != 0) {
                    return -1;
                }
                if (!SLTCCanAssign(c, c->typeStr, fmtType)) {
                    return SLTCFailNode(c, fmtNode, SLDiag_TYPE_MISMATCH);
                }
                argNode = SLAstNextSibling(c->ast, fmtNode);
                while (argNode >= 0) {
                    int32_t argType;
                    if (SLTCTypeExpr(c, argNode, &argType) != 0) {
                        return -1;
                    }
                    argNode = SLAstNextSibling(c->ast, argNode);
                }
            }
            return 0;
        }
        default: return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
    }
}

static int SLTCTypeFunctionBody(SLTypeCheckCtx* c, int32_t funcIndex) {
    const SLTCFunction* fn = &c->funcs[funcIndex];
    int32_t             nodeId = fn->defNode;
    int32_t             child;
    uint32_t            paramIndex = 0;
    int32_t             bodyNode = -1;

    if (nodeId < 0) {
        return 0;
    }

    c->localLen = 0;

    child = SLAstFirstChild(c->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* n = &c->ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            if (paramIndex >= fn->paramCount) {
                return SLTCFailNode(c, child, SLDiag_ARITY_MISMATCH);
            }
            if (SLTCLocalAdd(
                    c, n->dataStart, n->dataEnd, c->funcParamTypes[fn->paramTypeStart + paramIndex])
                != 0)
            {
                return -1;
            }
            paramIndex++;
        } else if (n->kind == SLAst_BLOCK) {
            bodyNode = child;
        }
        child = SLAstNextSibling(c->ast, child);
    }

    if (bodyNode < 0) {
        return 0;
    }

    return SLTCTypeBlock(c, bodyNode, fn->returnType, 0, 0);
}

static int SLTCCollectFunctionDecls(SLTypeCheckCtx* c) {
    int32_t child = SLAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (SLTCCollectFunctionFromNode(c, child) != 0) {
            return -1;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

static int SLTCCollectTypeDecls(SLTypeCheckCtx* c) {
    int32_t child = SLAstFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (SLTCCollectTypeDeclsFromNode(c, child) != 0) {
            return -1;
        }
        child = SLAstNextSibling(c->ast, child);
    }
    return 0;
}

int SLTypeCheck(SLArena* arena, const SLAst* ast, SLStrView src, SLDiag* diag) {
    SLTypeCheckCtx c;
    uint32_t       capBase;
    uint32_t       i;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }

    if (arena == NULL || ast == NULL || ast->nodes == NULL || ast->root < 0) {
        SLTCSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    capBase = ast->len < 32 ? 32u : ast->len;

    c.arena = arena;
    c.ast = ast;
    c.src = src;
    c.diag = diag;

    c.types = (SLTCType*)SLArenaAlloc(
        arena, sizeof(SLTCType) * capBase * 4u, (uint32_t)_Alignof(SLTCType));
    c.fields = (SLTCField*)SLArenaAlloc(
        arena, sizeof(SLTCField) * capBase * 4u, (uint32_t)_Alignof(SLTCField));
    c.namedTypes = (SLTCNamedType*)SLArenaAlloc(
        arena, sizeof(SLTCNamedType) * capBase, (uint32_t)_Alignof(SLTCNamedType));
    c.funcs = (SLTCFunction*)SLArenaAlloc(
        arena, sizeof(SLTCFunction) * capBase, (uint32_t)_Alignof(SLTCFunction));
    c.funcParamTypes = (int32_t*)SLArenaAlloc(
        arena, sizeof(int32_t) * capBase * 8u, (uint32_t)_Alignof(int32_t));
    c.scratchParamTypes = (int32_t*)SLArenaAlloc(
        arena, sizeof(int32_t) * capBase, (uint32_t)_Alignof(int32_t));
    c.locals = (SLTCLocal*)SLArenaAlloc(
        arena, sizeof(SLTCLocal) * capBase * 4u, (uint32_t)_Alignof(SLTCLocal));

    if (c.types == NULL || c.fields == NULL || c.namedTypes == NULL || c.funcs == NULL
        || c.funcParamTypes == NULL || c.scratchParamTypes == NULL || c.locals == NULL)
    {
        SLTCSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    c.typeLen = 0;
    c.typeCap = capBase * 4u;
    c.fieldLen = 0;
    c.fieldCap = capBase * 4u;
    c.namedTypeLen = 0;
    c.namedTypeCap = capBase;
    c.funcLen = 0;
    c.funcCap = capBase;
    c.funcParamLen = 0;
    c.funcParamCap = capBase * 8u;
    c.scratchParamCap = capBase;
    c.localLen = 0;
    c.localCap = capBase * 4u;

    if (SLTCEnsureInitialized(&c) != 0) {
        return -1;
    }
    c.typeUsize = SLTCFindBuiltinByKind(&c, SLBuiltin_USIZE);
    if (c.typeUsize < 0) {
        return SLTCFailSpan(&c, SLDiag_UNKNOWN_TYPE, 0, 0);
    }

    if (SLTCCollectTypeDecls(&c) != 0) {
        return -1;
    }
    if (SLTCResolveAllNamedTypeFields(&c) != 0) {
        return -1;
    }
    if (SLTCCheckEmbeddedCycles(&c) != 0) {
        return -1;
    }
    if (SLTCPropagateVarSizeNamedTypes(&c) != 0) {
        return -1;
    }
    if (SLTCCollectFunctionDecls(&c) != 0) {
        return -1;
    }
    if (SLTCFinalizeFunctionTypes(&c) != 0) {
        return -1;
    }

    for (i = 0; i < c.funcLen; i++) {
        if (SLTCTypeFunctionBody(&c, (int32_t)i) != 0) {
            return -1;
        }
    }

    return 0;
}

SL_API_END
