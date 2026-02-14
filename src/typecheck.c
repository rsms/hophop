#include "libsl.h"

typedef enum {
    SLTCType_INVALID = 0,
    SLTCType_BUILTIN,
    SLTCType_NAMED,
    SLTCType_PTR,
    SLTCType_ARRAY,
    SLTCType_UNTYPED_INT,
    SLTCType_UNTYPED_FLOAT,
    SLTCType_FUNCTION,
} SLTCTypeKind;

typedef enum {
    SLBuiltin_INVALID = 0,
    SLBuiltin_VOID,
    SLBuiltin_BOOL,
    SLBuiltin_STR,
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
} SLTCType;

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeId;
} SLTCField;

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
    const SLAST* ast;
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
    int32_t typeUntypedInt;
    int32_t typeUntypedFloat;
} SLTypeCheckCtx;

static void SLTCSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->start = start;
    diag->end = end;
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

static int32_t SLASTFirstChild(const SLAST* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int32_t SLASTNextSibling(const SLAST* ast, int32_t nodeId) {
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
    return SLTCAddType(c, &t, 0, 0);
}

static int SLTCEnsureInitialized(SLTypeCheckCtx* c) {
    SLTCType t;

    c->typeVoid = -1;
    c->typeBool = -1;
    c->typeStr = -1;
    c->typeUntypedInt = -1;
    c->typeUntypedFloat = -1;

    c->typeVoid = SLTCAddBuiltinType(c, "void", SLBuiltin_VOID);
    c->typeBool = SLTCAddBuiltinType(c, "bool", SLBuiltin_BOOL);
    c->typeStr = SLTCAddBuiltinType(c, "str", SLBuiltin_STR);
    if (c->typeVoid < 0 || c->typeBool < 0 || c->typeStr < 0) {
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
        || SLTCAddBuiltinType(c, "usize", SLBuiltin_USIZE) < 0
        || SLTCAddBuiltinType(c, "isize", SLBuiltin_ISIZE) < 0
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
    c->typeUntypedInt = SLTCAddType(c, &t, 0, 0);
    if (c->typeUntypedInt < 0) {
        return -1;
    }

    t.kind = SLTCType_UNTYPED_FLOAT;
    c->typeUntypedFloat = SLTCAddType(c, &t, 0, 0);
    return c->typeUntypedFloat < 0 ? -1 : 0;
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
        if (SLNameEqLiteral(c->src, start, end, "usize") && t->builtin == SLBuiltin_USIZE) {
            return (int32_t)i;
        }
        if (SLNameEqLiteral(c->src, start, end, "isize") && t->builtin == SLBuiltin_ISIZE) {
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
    return SLTCAddType(c, &t, errStart, errEnd);
}

static int SLTCParseArrayLen(SLTypeCheckCtx* c, const SLASTNode* node, uint32_t* outLen) {
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

static int SLTCResolveTypeNode(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLASTNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_TYPE, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case SLAST_TYPE_NAME: {
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
        case SLAST_TYPE_PTR: {
            int32_t child = SLASTFirstChild(c->ast, nodeId);
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
        case SLAST_TYPE_ARRAY: {
            int32_t  child = SLASTFirstChild(c->ast, nodeId);
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
        default: return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
}

static int SLTCAddNamedType(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLASTNode* node = &c->ast->nodes[nodeId];
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
    SLASTKind kind = c->ast->nodes[nodeId].kind;
    if (kind == SLAST_PUB) {
        int32_t ch = SLASTFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (SLTCCollectTypeDeclsFromNode(c, ch) != 0) {
                return -1;
            }
            ch = SLASTNextSibling(c->ast, ch);
        }
        return 0;
    }
    if (kind == SLAST_STRUCT || kind == SLAST_UNION || kind == SLAST_ENUM) {
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

static int SLTCIsUntyped(SLTypeCheckCtx* c, int32_t typeId) {
    return typeId == c->typeUntypedInt || typeId == c->typeUntypedFloat;
}

static int SLTCCanAssign(SLTypeCheckCtx* c, int32_t dstType, int32_t srcType) {
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
    SLASTKind            kind = c->ast->nodes[declNode].kind;
    int32_t              child;
    uint32_t             fieldStart = c->fieldLen;
    uint32_t             fieldCount = 0;

    child = SLASTFirstChild(c->ast, declNode);
    if (kind == SLAST_ENUM && child >= 0
        && (c->ast->nodes[child].kind == SLAST_TYPE_NAME
            || c->ast->nodes[child].kind == SLAST_TYPE_PTR
            || c->ast->nodes[child].kind == SLAST_TYPE_ARRAY))
    {
        child = SLASTNextSibling(c->ast, child);
    }

    while (child >= 0) {
        const SLASTNode* n = &c->ast->nodes[child];
        if (n->kind == SLAST_FIELD && (kind == SLAST_STRUCT || kind == SLAST_UNION)) {
            int32_t  typeNode = SLASTFirstChild(c->ast, child);
            int32_t  typeId;
            uint32_t i;
            if (typeNode < 0) {
                return SLTCFailNode(c, child, SLDiag_EXPECTED_TYPE);
            }
            if (SLTCResolveTypeNode(c, typeNode, &typeId) != 0) {
                return -1;
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
            c->fieldLen++;
            fieldCount++;
        }
        child = SLASTNextSibling(c->ast, child);
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

static int SLTCReadFunctionSig(
    SLTypeCheckCtx* c,
    int32_t         funNode,
    int32_t*        outReturnType,
    uint32_t*       outParamCount,
    int*            outHasBody) {
    int32_t  child = SLASTFirstChild(c->ast, funNode);
    uint32_t paramCount = 0;
    int32_t  returnType = c->typeVoid;
    int      hasBody = 0;

    while (child >= 0) {
        const SLASTNode* n = &c->ast->nodes[child];
        if (n->kind == SLAST_PARAM) {
            int32_t typeNode = SLASTFirstChild(c->ast, child);
            int32_t typeId;
            if (paramCount >= c->scratchParamCap) {
                return SLTCFailNode(c, child, SLDiag_ARENA_OOM);
            }
            if (SLTCResolveTypeNode(c, typeNode, &typeId) != 0) {
                return -1;
            }
            c->scratchParamTypes[paramCount++] = typeId;
        } else if (
            (n->kind == SLAST_TYPE_NAME || n->kind == SLAST_TYPE_PTR || n->kind == SLAST_TYPE_ARRAY)
            && n->flags == 1)
        {
            if (SLTCResolveTypeNode(c, child, &returnType) != 0) {
                return -1;
            }
        } else if (n->kind == SLAST_BLOCK) {
            hasBody = 1;
        }
        child = SLASTNextSibling(c->ast, child);
    }

    *outReturnType = returnType;
    *outParamCount = paramCount;
    *outHasBody = hasBody;
    return 0;
}

static int SLTCCollectFunctionFromNode(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLASTNode* n = &c->ast->nodes[nodeId];
    int32_t          existing;
    int32_t          returnType;
    uint32_t         paramCount;
    int              hasBody;

    if (n->kind == SLAST_PUB) {
        int32_t ch = SLASTFirstChild(c->ast, nodeId);
        while (ch >= 0) {
            if (SLTCCollectFunctionFromNode(c, ch) != 0) {
                return -1;
            }
            ch = SLASTNextSibling(c->ast, ch);
        }
        return 0;
    }

    if (n->kind != SLAST_FN) {
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
    SLTypeCheckCtx* c, int32_t typeId, uint32_t fieldStart, uint32_t fieldEnd, int32_t* outType) {
    uint32_t i;
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen) {
        return -1;
    }
    if (c->types[typeId].kind == SLTCType_PTR) {
        typeId = c->types[typeId].baseType;
    }
    if (typeId < 0 || (uint32_t)typeId >= c->typeLen || c->types[typeId].kind != SLTCType_NAMED) {
        return -1;
    }
    for (i = 0; i < c->types[typeId].fieldCount; i++) {
        uint32_t idx = c->types[typeId].fieldStart + i;
        if (SLNameEqSlice(
                c->src, c->fields[idx].nameStart, c->fields[idx].nameEnd, fieldStart, fieldEnd))
        {
            *outType = c->fields[idx].typeId;
            return 0;
        }
    }
    return -1;
}

static int SLTCExprIsAssignable(const SLAST* ast, int32_t exprNode) {
    const SLASTNode* n;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return 0;
    }
    n = &ast->nodes[exprNode];
    if (n->kind == SLAST_IDENT || n->kind == SLAST_FIELD_EXPR || n->kind == SLAST_INDEX) {
        return 1;
    }
    if (n->kind == SLAST_UNARY && n->op == SLTok_MUL) {
        return 1;
    }
    return 0;
}

static int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType);
static int SLTCTypeStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth);

static int SLTCTypeExpr(SLTypeCheckCtx* c, int32_t nodeId, int32_t* outType) {
    const SLASTNode* n;
    if (nodeId < 0 || (uint32_t)nodeId >= c->ast->len) {
        return SLTCFailSpan(c, SLDiag_EXPECTED_EXPR, 0, 0);
    }
    n = &c->ast->nodes[nodeId];

    switch (n->kind) {
        case SLAST_IDENT: {
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
        case SLAST_INT:    *outType = c->typeUntypedInt; return 0;
        case SLAST_FLOAT:  *outType = c->typeUntypedFloat; return 0;
        case SLAST_STRING: *outType = c->typeStr; return 0;
        case SLAST_BOOL:   *outType = c->typeBool; return 0;
        case SLAST_CALL:   {
            int32_t  calleeNode = SLASTFirstChild(c->ast, nodeId);
            int32_t  calleeType;
            int32_t  funcIdx;
            int32_t  argNode;
            uint32_t argIndex = 0;
            if (calleeNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_NOT_CALLABLE);
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
            argNode = SLASTNextSibling(c->ast, calleeNode);
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
                argNode = SLASTNextSibling(c->ast, argNode);
            }
            if (argIndex != c->funcs[funcIdx].paramCount) {
                return SLTCFailNode(c, nodeId, SLDiag_ARITY_MISMATCH);
            }
            *outType = c->funcs[funcIdx].returnType;
            return 0;
        }
        case SLAST_CAST: {
            int32_t exprNode = SLASTFirstChild(c->ast, nodeId);
            int32_t typeNode;
            int32_t ignoredType;
            int32_t targetType;
            if (exprNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            typeNode = SLASTNextSibling(c->ast, exprNode);
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
        case SLAST_FIELD_EXPR: {
            int32_t recvNode = SLASTFirstChild(c->ast, nodeId);
            int32_t recvType;
            int32_t fieldType;
            if (recvNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNKNOWN_SYMBOL);
            }
            if (SLTCTypeExpr(c, recvNode, &recvType) != 0) {
                return -1;
            }
            if (SLTCFieldLookup(c, recvType, n->dataStart, n->dataEnd, &fieldType) != 0) {
                return SLTCFailSpan(c, SLDiag_UNKNOWN_SYMBOL, n->dataStart, n->dataEnd);
            }
            *outType = fieldType;
            return 0;
        }
        case SLAST_INDEX: {
            int32_t baseNode = SLASTFirstChild(c->ast, nodeId);
            int32_t idxNode;
            int32_t baseType;
            int32_t idxType;
            if (baseNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            idxNode = SLASTNextSibling(c->ast, baseNode);
            if (idxNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            if (SLTCTypeExpr(c, baseNode, &baseType) != 0
                || SLTCTypeExpr(c, idxNode, &idxType) != 0)
            {
                return -1;
            }
            if (!SLTCIsIntegerType(c, idxType)) {
                return SLTCFailNode(c, idxNode, SLDiag_TYPE_MISMATCH);
            }
            if (c->types[baseType].kind == SLTCType_ARRAY
                || c->types[baseType].kind == SLTCType_PTR)
            {
                *outType = c->types[baseType].baseType;
                return 0;
            }
            return SLTCFailNode(c, baseNode, SLDiag_TYPE_MISMATCH);
        }
        case SLAST_UNARY: {
            int32_t rhsNode = SLASTFirstChild(c->ast, nodeId);
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
                    if (c->types[rhsType].kind != SLTCType_PTR) {
                        return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
                    }
                    *outType = c->types[rhsType].baseType;
                    return 0;
                case SLTok_AND: {
                    int32_t ptrType = SLTCInternPtrType(c, rhsType, n->start, n->end);
                    if (ptrType < 0) {
                        return -1;
                    }
                    *outType = ptrType;
                    return 0;
                }
                default: return SLTCFailNode(c, nodeId, SLDiag_TYPE_MISMATCH);
            }
        }
        case SLAST_BINARY: {
            int32_t     lhsNode = SLASTFirstChild(c->ast, nodeId);
            int32_t     rhsNode;
            int32_t     lhsType;
            int32_t     rhsType;
            int32_t     commonType;
            SLTokenKind op;
            if (lhsNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            rhsNode = SLASTNextSibling(c->ast, lhsNode);
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
                if (!SLTCExprIsAssignable(c->ast, lhsNode)) {
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
        default: return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }
}

static int SLTCTypeVarLike(SLTypeCheckCtx* c, int32_t nodeId) {
    const SLASTNode* n = &c->ast->nodes[nodeId];
    int32_t          typeNode = SLASTFirstChild(c->ast, nodeId);
    int32_t          initNode;
    int32_t          declType;

    if (typeNode < 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_TYPE);
    }
    if (SLTCResolveTypeNode(c, typeNode, &declType) != 0) {
        return -1;
    }

    initNode = SLASTNextSibling(c->ast, typeNode);
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

static int SLTCTypeBlock(
    SLTypeCheckCtx* c, int32_t blockNode, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t savedLocalLen = c->localLen;
    int32_t  child = SLASTFirstChild(c->ast, blockNode);
    while (child >= 0) {
        if (SLTCTypeStmt(c, child, returnType, loopDepth, switchDepth) != 0) {
            return -1;
        }
        child = SLASTNextSibling(c->ast, child);
    }
    c->localLen = savedLocalLen;
    return 0;
}

static int SLTCTypeForStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    uint32_t savedLocalLen = c->localLen;
    int32_t  child = SLASTFirstChild(c->ast, nodeId);
    int32_t  nodes[4];
    int      count = 0;
    int      i;

    while (child >= 0 && count < 4) {
        nodes[count++] = child;
        child = SLASTNextSibling(c->ast, child);
    }

    if (count == 0) {
        return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
    }

    for (i = 0; i < count - 1; i++) {
        const SLASTNode* n = &c->ast->nodes[nodes[i]];
        if (n->kind == SLAST_VAR || n->kind == SLAST_CONST) {
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

    if (c->ast->nodes[nodes[count - 1]].kind != SLAST_BLOCK) {
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
    const SLASTNode* sw = &c->ast->nodes[nodeId];
    int32_t          child = SLASTFirstChild(c->ast, nodeId);
    int32_t          subjectType = -1;

    if (sw->flags == 1) {
        if (child < 0) {
            return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
        }
        if (SLTCTypeExpr(c, child, &subjectType) != 0) {
            return -1;
        }
        child = SLASTNextSibling(c->ast, child);
    }

    while (child >= 0) {
        const SLASTNode* clause = &c->ast->nodes[child];
        if (clause->kind == SLAST_CASE) {
            int32_t caseChild = SLASTFirstChild(c->ast, child);
            int32_t bodyNode = -1;
            while (caseChild >= 0) {
                int32_t next = SLASTNextSibling(c->ast, caseChild);
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
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAST_BLOCK) {
                return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                return -1;
            }
        } else if (clause->kind == SLAST_DEFAULT) {
            int32_t bodyNode = SLASTFirstChild(c->ast, child);
            if (bodyNode < 0 || c->ast->nodes[bodyNode].kind != SLAST_BLOCK) {
                return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeBlock(c, bodyNode, returnType, loopDepth, switchDepth + 1) != 0) {
                return -1;
            }
        } else {
            return SLTCFailNode(c, child, SLDiag_UNEXPECTED_TOKEN);
        }
        child = SLASTNextSibling(c->ast, child);
    }

    return 0;
}

static int SLTCTypeStmt(
    SLTypeCheckCtx* c, int32_t nodeId, int32_t returnType, int loopDepth, int switchDepth) {
    const SLASTNode* n = &c->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAST_BLOCK:     return SLTCTypeBlock(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAST_VAR:
        case SLAST_CONST:     return SLTCTypeVarLike(c, nodeId);
        case SLAST_EXPR_STMT: {
            int32_t expr = SLASTFirstChild(c->ast, nodeId);
            int32_t t;
            if (expr < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_EXPR);
            }
            return SLTCTypeExpr(c, expr, &t);
        }
        case SLAST_RETURN: {
            int32_t expr = SLASTFirstChild(c->ast, nodeId);
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
        case SLAST_IF: {
            int32_t cond = SLASTFirstChild(c->ast, nodeId);
            int32_t thenNode;
            int32_t elseNode;
            int32_t condType;
            if (cond < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_EXPECTED_BOOL);
            }
            thenNode = SLASTNextSibling(c->ast, cond);
            if (thenNode < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            if (SLTCTypeExpr(c, cond, &condType) != 0) {
                return -1;
            }
            if (!SLTCIsBoolType(c, condType)) {
                return SLTCFailNode(c, cond, SLDiag_EXPECTED_BOOL);
            }
            if (SLTCTypeStmt(c, thenNode, returnType, loopDepth, switchDepth) != 0) {
                return -1;
            }
            elseNode = SLASTNextSibling(c->ast, thenNode);
            if (elseNode >= 0 && SLTCTypeStmt(c, elseNode, returnType, loopDepth, switchDepth) != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAST_FOR:    return SLTCTypeForStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAST_SWITCH: return SLTCTypeSwitchStmt(c, nodeId, returnType, loopDepth, switchDepth);
        case SLAST_BREAK:
            if (loopDepth <= 0 && switchDepth <= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case SLAST_CONTINUE:
            if (loopDepth <= 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return 0;
        case SLAST_DEFER: {
            int32_t stmt = SLASTFirstChild(c->ast, nodeId);
            if (stmt < 0) {
                return SLTCFailNode(c, nodeId, SLDiag_UNEXPECTED_TOKEN);
            }
            return SLTCTypeStmt(c, stmt, returnType, loopDepth, switchDepth);
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

    child = SLASTFirstChild(c->ast, nodeId);
    while (child >= 0) {
        const SLASTNode* n = &c->ast->nodes[child];
        if (n->kind == SLAST_PARAM) {
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
        } else if (n->kind == SLAST_BLOCK) {
            bodyNode = child;
        }
        child = SLASTNextSibling(c->ast, child);
    }

    if (bodyNode < 0) {
        return 0;
    }

    return SLTCTypeBlock(c, bodyNode, fn->returnType, 0, 0);
}

static int SLTCCollectFunctionDecls(SLTypeCheckCtx* c) {
    int32_t child = SLASTFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (SLTCCollectFunctionFromNode(c, child) != 0) {
            return -1;
        }
        child = SLASTNextSibling(c->ast, child);
    }
    return 0;
}

static int SLTCCollectTypeDecls(SLTypeCheckCtx* c) {
    int32_t child = SLASTFirstChild(c->ast, c->ast->root);
    while (child >= 0) {
        if (SLTCCollectTypeDeclsFromNode(c, child) != 0) {
            return -1;
        }
        child = SLASTNextSibling(c->ast, child);
    }
    return 0;
}

int SLTypeCheck(SLArena* arena, const SLAST* ast, SLStrView src, SLDiag* diag) {
    SLTypeCheckCtx c;
    uint32_t       capBase;
    uint32_t       i;

    SLDiagClear(diag);

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

    if (SLTCCollectTypeDecls(&c) != 0) {
        return -1;
    }
    if (SLTCResolveAllNamedTypeFields(&c) != 0) {
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
