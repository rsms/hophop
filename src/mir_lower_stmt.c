#include "libhop-impl.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_lower_stmt.h"

HOP_API_BEGIN

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t slot;
    int32_t  typeNode;
    int32_t  initExprNode;
    uint8_t  mutable;
    uint8_t  _reserved[2];
} HOPMirLowerLocal;

typedef struct {
    uint32_t breakJumpStart;
    uint32_t continueJumpStart;
    uint32_t continueTargetPc;
    uint32_t blockDepth;
    uint8_t  hasContinue;
    uint8_t  _reserved[3];
} HOPMirLowerControl;

typedef struct {
    uint32_t deferStart;
} HOPMirLowerBlockScope;

typedef struct {
    uint32_t atIndex;
    uint32_t slot;
    uint32_t start;
    uint32_t end;
    uint32_t next;
} HOPMirChunkInsert;

typedef struct {
    HOPArena*             arena;
    const HOPAst*         ast;
    HOPStrView            src;
    HOPMirProgramBuilder  builder;
    uint32_t              sourceIndex;
    uint32_t              functionIndex;
    int32_t               functionReturnTypeNode;
    HOPMirLowerLocal*     locals;
    uint32_t              localLen;
    uint32_t              localCap;
    uint32_t              breakJumps[256];
    uint32_t              breakJumpLen;
    uint32_t              continueJumps[256];
    uint32_t              continueJumpLen;
    HOPMirLowerControl    controls[32];
    uint32_t              controlLen;
    int32_t               deferredStmtNodes[256];
    uint32_t              deferredStmtLen;
    HOPMirLowerBlockScope blockScopes[64];
    uint32_t              blockDepth;
    uint8_t               loweringDeferred;
    uint8_t               _reserved2[3];
    int                   supported;
    HOPDiag* _Nullable diag;
    HOPMirLowerConstExprFn _Nullable lowerConstExpr;
    void* _Nullable lowerConstExprCtx;
} HOPMirStmtLower;

static void HOPMirLowerStmtSetDiag(
    HOPDiag* _Nullable diag, HOPDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

static void HOPMirLowerStmtSetUnsupportedDetail(
    HOPMirStmtLower* c, uint32_t start, uint32_t end, const char* reason) {
    if (c == NULL) {
        return;
    }
    c->supported = 0;
    if (c->diag == NULL || reason == NULL || reason[0] == '\0' || c->diag->detail != NULL) {
        return;
    }
    c->diag->start = start;
    c->diag->end = end;
    c->diag->detail = reason;
}

static int HOPMirStmtLowerIsTypeNodeKind(HOPAstKind kind) {
    return kind == HOPAst_TYPE_NAME || kind == HOPAst_TYPE_PTR || kind == HOPAst_TYPE_REF
        || kind == HOPAst_TYPE_MUTREF || kind == HOPAst_TYPE_ARRAY || kind == HOPAst_TYPE_VARRAY
        || kind == HOPAst_TYPE_SLICE || kind == HOPAst_TYPE_MUTSLICE || kind == HOPAst_TYPE_OPTIONAL
        || kind == HOPAst_TYPE_FN || kind == HOPAst_TYPE_TUPLE || kind == HOPAst_TYPE_ANON_STRUCT
        || kind == HOPAst_TYPE_ANON_UNION;
}

static int32_t HOPMirStmtLowerFunctionReturnTypeNode(const HOPAst* ast, int32_t fnNode) {
    int32_t child;
    if (ast == NULL || fnNode < 0 || (uint32_t)fnNode >= ast->len) {
        return -1;
    }
    child = ast->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (HOPMirStmtLowerIsTypeNodeKind(ast->nodes[child].kind)) {
            return child;
        }
        if (ast->nodes[child].kind == HOPAst_BLOCK) {
            break;
        }
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static uint32_t HOPMirStmtLowerAstListCount(const HOPAst* ast, int32_t listNode) {
    uint32_t count = 0;
    int32_t  child;
    if (ast == NULL || listNode < 0 || (uint32_t)listNode >= ast->len) {
        return 0;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        count++;
        child = ast->nodes[child].nextSibling;
    }
    return count;
}

static int32_t HOPMirStmtLowerAstListItemAt(const HOPAst* ast, int32_t listNode, uint32_t index) {
    uint32_t i = 0;
    int32_t  child;
    if (ast == NULL || listNode < 0 || (uint32_t)listNode >= ast->len) {
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

static uint32_t HOPMirStmtLowerFnPc(const HOPMirStmtLower* c) {
    return c->builder.instLen - c->builder.funcs[c->functionIndex].instStart;
}

static int HOPMirStmtLowerEnsureLocalCap(HOPMirStmtLower* c, uint32_t needLen) {
    uint32_t          newCap;
    HOPMirLowerLocal* newLocals;
    if (needLen <= c->localCap) {
        return 0;
    }
    newCap = c->localCap == 0 ? 8u : c->localCap;
    while (newCap < needLen) {
        if (newCap > UINT32_MAX / 2u) {
            newCap = needLen;
            break;
        }
        newCap *= 2u;
    }
    newLocals = (HOPMirLowerLocal*)HOPArenaAlloc(
        c->arena, sizeof(HOPMirLowerLocal) * newCap, (uint32_t)_Alignof(HOPMirLowerLocal));
    if (newLocals == NULL) {
        HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    if (c->locals != NULL && c->localLen > 0) {
        memcpy(newLocals, c->locals, sizeof(HOPMirLowerLocal) * c->localLen);
    }
    c->locals = newLocals;
    c->localCap = newCap;
    return 0;
}

static int HOPMirStmtLowerPushLocal(
    HOPMirStmtLower* c,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    int              mutable,
    int              isParam,
    int              zeroInit,
    int32_t          typeNode,
    int32_t          initExprNode,
    uint32_t*        outSlot) {
    HOPMirLocal   local = { 0 };
    HOPMirTypeRef typeRef = { 0 };
    uint32_t      slot = 0;
    if (HOPMirStmtLowerEnsureLocalCap(c, c->localLen + 1u) != 0) {
        return -1;
    }
    local.typeRef = UINT32_MAX;
    local.flags = mutable ? HOPMirLocalFlag_MUTABLE : HOPMirLocalFlag_NONE;
    local.nameStart = nameStart;
    local.nameEnd = nameEnd;
    if (isParam) {
        local.flags |= HOPMirLocalFlag_PARAM;
    }
    if (zeroInit) {
        local.flags |= HOPMirLocalFlag_ZERO_INIT;
    }
    if (typeNode >= 0) {
        typeRef.astNode = (uint32_t)typeNode;
        typeRef.sourceRef = c->sourceIndex;
        typeRef.flags = 0;
        typeRef.aux = 0;
        if (HOPMirProgramBuilderAddType(&c->builder, &typeRef, &local.typeRef) != 0) {
            HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, nameStart, nameEnd);
            return -1;
        }
    }
    if (HOPMirProgramBuilderAddLocal(&c->builder, &local, &slot) != 0) {
        HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, nameStart, nameEnd);
        return -1;
    }
    c->locals[c->localLen].nameStart = nameStart;
    c->locals[c->localLen].nameEnd = nameEnd;
    c->locals[c->localLen].slot = slot;
    c->locals[c->localLen].typeNode = typeNode;
    c->locals[c->localLen].initExprNode = initExprNode;
    c->locals[c->localLen].mutable = mutable ? 1u : 0u;
    c->locals[c->localLen]._reserved[0] = 0;
    c->locals[c->localLen]._reserved[1] = 0;
    if (outSlot != NULL) {
        *outSlot = slot;
    }
    c->localLen++;
    return 0;
}

static int HOPMirStmtLowerFindLocal(
    const HOPMirStmtLower* c,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    uint32_t*              outSlot,
    int* _Nullable outMutable) {
    uint32_t nameLen;
    uint32_t i = c->localLen;
    if (c == NULL || nameEnd < nameStart || nameEnd > c->src.len) {
        return 0;
    }
    nameLen = nameEnd - nameStart;
    while (i > 0) {
        const HOPMirLowerLocal* local;
        i--;
        local = &c->locals[i];
        if (local->nameEnd >= local->nameStart && local->nameEnd - local->nameStart == nameLen
            && memcmp(c->src.ptr + local->nameStart, c->src.ptr + nameStart, nameLen) == 0)
        {
            if (outSlot != NULL) {
                *outSlot = local->slot;
            }
            if (outMutable != NULL) {
                *outMutable = local->mutable != 0;
            }
            return 1;
        }
    }
    return 0;
}

static int HOPMirStmtLowerNameEqLiteral(
    const HOPMirStmtLower* c, uint32_t start, uint32_t end, const char* lit);

static const HOPMirLowerLocal* _Nullable HOPMirStmtLowerFindLocalRef(
    const HOPMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t nameLen;
    uint32_t i;
    if (c == NULL || nameEnd < nameStart || nameEnd > c->src.len) {
        return NULL;
    }
    nameLen = nameEnd - nameStart;
    i = c->localLen;
    while (i > 0) {
        const HOPMirLowerLocal* local;
        i--;
        local = &c->locals[i];
        if (local->nameEnd >= local->nameStart && local->nameEnd - local->nameStart == nameLen
            && memcmp(c->src.ptr + local->nameStart, c->src.ptr + nameStart, nameLen) == 0)
        {
            return local;
        }
    }
    return NULL;
}

static int HOPMirStmtLowerBuiltinTypeSize(
    const HOPMirStmtLower* c, int32_t typeNode, int64_t* outSize) {
    const HOPAstNode* type;
    if (c == NULL || outSize == NULL || typeNode < 0 || (uint32_t)typeNode >= c->ast->len) {
        return 0;
    }
    type = &c->ast->nodes[typeNode];
    switch (type->kind) {
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF:
        case HOPAst_TYPE_FN:       *outSize = (int64_t)sizeof(void*); return 1;
        case HOPAst_TYPE_SLICE:
        case HOPAst_TYPE_MUTSLICE: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case HOPAst_TYPE_NAME:     {
            uint32_t len = type->dataEnd >= type->dataStart ? type->dataEnd - type->dataStart : 0u;
            const char* name = c->src.ptr + type->dataStart;
            if ((len == 2u && memcmp(name, "u8", 2) == 0)
                || (len == 2u && memcmp(name, "i8", 2) == 0)
                || (len == 4u && memcmp(name, "bool", 4) == 0))
            {
                *outSize = 1;
                return 1;
            }
            if ((len == 3u && memcmp(name, "u16", 3) == 0)
                || (len == 3u && memcmp(name, "i16", 3) == 0))
            {
                *outSize = 2;
                return 1;
            }
            if ((len == 3u && memcmp(name, "u32", 3) == 0)
                || (len == 3u && memcmp(name, "i32", 3) == 0)
                || (len == 3u && memcmp(name, "f32", 3) == 0)
                || (len == 4u && memcmp(name, "rune", 4) == 0))
            {
                *outSize = 4;
                return 1;
            }
            if ((len == 3u && memcmp(name, "u64", 3) == 0)
                || (len == 3u && memcmp(name, "i64", 3) == 0)
                || (len == 3u && memcmp(name, "f64", 3) == 0))
            {
                *outSize = 8;
                return 1;
            }
            if ((len == 3u && memcmp(name, "int", 3) == 0)
                || (len == 4u && memcmp(name, "uint", 4) == 0)
                || (len == 5u && memcmp(name, "usize", 5) == 0)
                || (len == 5u && memcmp(name, "isize", 5) == 0))
            {
                *outSize = (int64_t)sizeof(uintptr_t);
                return 1;
            }
            if (len == 3u && memcmp(name, "str", 3) == 0) {
                *outSize = (int64_t)(sizeof(void*) * 2u);
                return 1;
            }
            if (len == 4u && memcmp(name, "type", 4) == 0) {
                *outSize = (int64_t)sizeof(void*);
                return 1;
            }
            return 0;
        }
        default: return 0;
    }
}

static int HOPMirStmtLowerCastNeedsCoerce(const HOPMirStmtLower* c, int32_t typeNode) {
    const HOPAstNode* type;
    int32_t           childNode;
    if (c == NULL || typeNode < 0 || (uint32_t)typeNode >= c->ast->len) {
        return 0;
    }
    type = &c->ast->nodes[typeNode];
    if ((type->kind == HOPAst_TYPE_REF || type->kind == HOPAst_TYPE_PTR) && type->firstChild >= 0
        && (uint32_t)type->firstChild < c->ast->len)
    {
        childNode = type->firstChild;
        if (c->ast->nodes[childNode].kind == HOPAst_TYPE_NAME
            && HOPMirStmtLowerNameEqLiteral(
                c, c->ast->nodes[childNode].dataStart, c->ast->nodes[childNode].dataEnd, "str"))
        {
            return 0;
        }
    }
    if (type->kind != HOPAst_TYPE_NAME) {
        return 1;
    }
    if (HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "bool")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "f32")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "f64")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u8")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u16")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u32")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u64")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "uint")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i8")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i16")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i32")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i64")
        || HOPMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "int"))
    {
        return 0;
    }
    return 1;
}

static int HOPMirStmtLowerInferInitExprSize(
    const HOPMirStmtLower* c, int32_t exprNode, int64_t* outSize) {
    const HOPAstNode* expr;
    if (c == NULL || outSize == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    switch (expr->kind) {
        case HOPAst_INT:    *outSize = (int64_t)sizeof(uintptr_t); return 1;
        case HOPAst_FLOAT:  *outSize = 8; return 1;
        case HOPAst_BOOL:   *outSize = 1; return 1;
        case HOPAst_STRING: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case HOPAst_CAST:   {
            int32_t valueNode = expr->firstChild;
            int32_t typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode < 0 || typeNode < 0 || c->ast->nodes[typeNode].nextSibling >= 0) {
                return 0;
            }
            return HOPMirStmtLowerBuiltinTypeSize(c, typeNode, outSize);
        }
        default: return 0;
    }
}

static int HOPMirStmtLowerTryConstSizeofExpr(
    const HOPMirStmtLower* c, int32_t exprNode, int64_t* outSize) {
    const HOPAstNode* expr;
    int32_t           innerNode;
    if (c == NULL || outSize == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    if (expr->kind != HOPAst_SIZEOF) {
        return 0;
    }
    innerNode = expr->firstChild;
    if (innerNode < 0 || (uint32_t)innerNode >= c->ast->len) {
        return 0;
    }
    if (expr->flags == 1u) {
        if (HOPMirStmtLowerBuiltinTypeSize(c, innerNode, outSize)) {
            return 1;
        }
        if (c->ast->nodes[innerNode].kind == HOPAst_TYPE_NAME
            || c->ast->nodes[innerNode].kind == HOPAst_IDENT)
        {
            const HOPMirLowerLocal* local = HOPMirStmtLowerFindLocalRef(
                c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
            if (local != NULL) {
                if (local->typeNode >= 0
                    && HOPMirStmtLowerBuiltinTypeSize(c, local->typeNode, outSize))
                {
                    return 1;
                }
                if (local->initExprNode >= 0
                    && HOPMirStmtLowerInferInitExprSize(c, local->initExprNode, outSize))
                {
                    return 1;
                }
            }
        }
        return 0;
    }
    if (c->ast->nodes[innerNode].kind == HOPAst_IDENT) {
        const HOPMirLowerLocal* local = HOPMirStmtLowerFindLocalRef(
            c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
        if (local == NULL) {
            return 0;
        }
        if (local->typeNode >= 0 && HOPMirStmtLowerBuiltinTypeSize(c, local->typeNode, outSize)) {
            return 1;
        }
        if (local->initExprNode >= 0
            && HOPMirStmtLowerInferInitExprSize(c, local->initExprNode, outSize))
        {
            return 1;
        }
        return 0;
    }
    return HOPMirStmtLowerInferInitExprSize(c, innerNode, outSize);
}

static int HOPMirStmtLowerAppendInst(
    HOPMirStmtLower* c,
    HOPMirOp         op,
    uint16_t         tok,
    uint32_t         aux,
    uint32_t         start,
    uint32_t         end,
    uint32_t* _Nullable outInstIndex) {
    HOPMirInst inst;
    inst.op = op;
    inst.tok = tok;
    inst._reserved = 0;
    inst.aux = aux;
    inst.start = start;
    inst.end = end;
    if (HOPMirProgramBuilderAppendInst(&c->builder, &inst) != 0) {
        HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, start, end);
        return -1;
    }
    if (outInstIndex != NULL) {
        *outInstIndex = c->builder.instLen - 1u;
    }
    return 0;
}

static int HOPMirStmtLowerAddFieldRef(
    HOPMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd, uint32_t* outIndex) {
    HOPMirField field = { 0 };
    uint32_t    i;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || outIndex == NULL || nameEnd < nameStart || nameEnd > c->src.len) {
        return -1;
    }
    for (i = 0; i < c->builder.fieldLen; i++) {
        const HOPMirField* existing = &c->builder.fields[i];
        if (existing->nameStart == nameStart && existing->nameEnd == nameEnd
            && existing->sourceRef == c->sourceIndex && existing->ownerTypeRef == UINT32_MAX
            && existing->typeRef == UINT32_MAX)
        {
            *outIndex = i;
            return 0;
        }
    }
    field.nameStart = nameStart;
    field.nameEnd = nameEnd;
    field.sourceRef = c->sourceIndex;
    field.ownerTypeRef = UINT32_MAX;
    field.typeRef = UINT32_MAX;
    if (HOPMirProgramBuilderAddField(&c->builder, &field, outIndex) != 0) {
        HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, nameStart, nameEnd);
        return -1;
    }
    return 0;
}

static int HOPMirStmtLowerAppendLoadValueBySlice(
    HOPMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd, uint32_t start, uint32_t end) {
    uint32_t   slot = 0;
    HOPMirInst inst = { 0 };
    if (c == NULL) {
        return -1;
    }
    if (HOPMirStmtLowerFindLocal(c, nameStart, nameEnd, &slot, NULL)) {
        return HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_LOAD, 0, slot, start, end, NULL);
    }
    inst.op = HOPMirOp_LOAD_IDENT;
    inst.tok = HOPTok_IDENT;
    inst.aux = 0u;
    inst.start = nameStart;
    inst.end = nameEnd;
    return HOPMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag);
}

static int HOPMirStmtLowerAppendStoreValueBySlice(
    HOPMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd) {
    HOPMirInst inst = { 0 };
    if (c == NULL) {
        return -1;
    }
    inst.op = HOPMirOp_STORE_IDENT;
    inst.tok = HOPTok_IDENT;
    inst.aux = 0u;
    inst.start = nameStart;
    inst.end = nameEnd;
    return HOPMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag);
}

static int HOPMirStmtLowerExpr(HOPMirStmtLower* c, int32_t exprNode);

static int HOPMirStmtLowerNameEqLiteral(
    const HOPMirStmtLower* c, uint32_t start, uint32_t end, const char* lit) {
    size_t litLen = 0;
    if (c == NULL || lit == NULL || end < start || end > c->src.len) {
        return 0;
    }
    while (lit[litLen] != '\0') {
        litLen++;
    }
    return (size_t)(end - start) == litLen && memcmp(c->src.ptr + start, lit, litLen) == 0;
}

static int HOPMirStmtLowerContextFieldFromSlice(
    const HOPMirStmtLower* c, uint32_t start, uint32_t end, uint32_t* outField) {
    if (outField != NULL) {
        *outField = HOPMirContextField_INVALID;
    }
    if (c == NULL || outField == NULL) {
        return 0;
    }
    if (HOPMirStmtLowerNameEqLiteral(c, start, end, "allocator")) {
        *outField = HOPMirContextField_ALLOCATOR;
        return 1;
    }
    if (HOPMirStmtLowerNameEqLiteral(c, start, end, "temp_allocator")) {
        *outField = HOPMirContextField_TEMP_ALLOCATOR;
        return 1;
    }
    if (HOPMirStmtLowerNameEqLiteral(c, start, end, "logger")) {
        *outField = HOPMirContextField_LOGGER;
        return 1;
    }
    return 0;
}

static int HOPMirStmtLowerIsContextFieldExpr(
    const HOPMirStmtLower* c, int32_t exprNode, uint32_t* outField) {
    const HOPAstNode* expr;
    int32_t           baseNode;
    if (outField != NULL) {
        *outField = HOPMirContextField_INVALID;
    }
    if (c == NULL || outField == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    if (expr->kind != HOPAst_FIELD_EXPR) {
        return 0;
    }
    baseNode = expr->firstChild;
    if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len
        || c->ast->nodes[baseNode].kind != HOPAst_IDENT
        || !HOPMirStmtLowerNameEqLiteral(
            c, c->ast->nodes[baseNode].dataStart, c->ast->nodes[baseNode].dataEnd, "context"))
    {
        return 0;
    }
    return HOPMirStmtLowerContextFieldFromSlice(c, expr->dataStart, expr->dataEnd, outField);
}

static int HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(
    const HOPMirStmtLower* c,
    uint32_t               start,
    uint32_t               end,
    const char*            lit,
    const char*            pkgPrefix) {
    size_t litLen = 0;
    size_t pkgLen = 0;
    size_t i;
    if (HOPMirStmtLowerNameEqLiteral(c, start, end, lit)) {
        return 1;
    }
    if (c == NULL || lit == NULL || pkgPrefix == NULL || end < start || end > c->src.len) {
        return 0;
    }
    while (lit[litLen] != '\0') {
        litLen++;
    }
    while (pkgPrefix[pkgLen] != '\0') {
        pkgLen++;
    }
    if ((size_t)(end - start) != pkgLen + 2u + litLen) {
        return 0;
    }
    for (i = 0; i < pkgLen; i++) {
        if (c->src.ptr[start + i] != pkgPrefix[i]) {
            return 0;
        }
    }
    if (c->src.ptr[start + pkgLen] != '_' || c->src.ptr[start + pkgLen + 1u] != '_') {
        return 0;
    }
    return memcmp(c->src.ptr + start + pkgLen + 2u, lit, litLen) == 0;
}

static int HOPMirStmtLowerNameIsCompilerDiagBuiltin(
    const HOPMirStmtLower* c, uint32_t start, uint32_t end) {
    return HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "error", "compiler")
        || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "error_at", "compiler")
        || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "warn", "compiler")
        || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "warn_at", "compiler");
}

static int HOPMirStmtLowerNameIsLazyTypeBuiltin(
    const HOPMirStmtLower* c, uint32_t start, uint32_t end) {
    return HOPMirStmtLowerNameEqLiteral(c, start, end, "typeof")
        || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "kind", "reflect")
        || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "base", "reflect")
        || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "is_alias", "reflect")
        || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "type_name", "reflect")
        || HOPMirStmtLowerNameEqLiteral(c, start, end, "ptr")
        || HOPMirStmtLowerNameEqLiteral(c, start, end, "slice")
        || HOPMirStmtLowerNameEqLiteral(c, start, end, "array");
}

static int HOPMirStmtLowerCallUsesLazyBuiltin(const HOPMirStmtLower* c, int32_t callNode) {
    const HOPAstNode* call;
    const HOPAstNode* callee;
    int32_t           calleeNode;
    int32_t           recvNode;
    if (c == NULL || c->ast == NULL || callNode < 0 || (uint32_t)callNode >= c->ast->len) {
        return 0;
    }
    call = &c->ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (call->kind != HOPAst_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == HOPAst_IDENT) {
        return HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(
                   c, callee->dataStart, callee->dataEnd, "source_location_of", "builtin")
            || HOPMirStmtLowerNameIsLazyTypeBuiltin(c, callee->dataStart, callee->dataEnd)
            || HOPMirStmtLowerNameIsCompilerDiagBuiltin(c, callee->dataStart, callee->dataEnd);
    }
    if (callee->kind != HOPAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len
        || c->ast->nodes[recvNode].kind != HOPAst_IDENT)
    {
        return 0;
    }
    if (HOPMirStmtLowerNameEqLiteral(
            c, c->ast->nodes[recvNode].dataStart, c->ast->nodes[recvNode].dataEnd, "builtin")
        && HOPMirStmtLowerNameEqLiteral(
            c, callee->dataStart, callee->dataEnd, "source_location_of"))
    {
        return 1;
    }
    if (HOPMirStmtLowerNameEqLiteral(
            c, c->ast->nodes[recvNode].dataStart, c->ast->nodes[recvNode].dataEnd, "reflect")
        && (HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "kind", "reflect")
            || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "base", "reflect")
            || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "is_alias", "reflect")
            || HOPMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "type_name", "reflect")))
    {
        return 1;
    }
    if (!HOPMirStmtLowerNameEqLiteral(
            c, c->ast->nodes[recvNode].dataStart, c->ast->nodes[recvNode].dataEnd, "compiler"))
    {
        return 0;
    }
    return HOPMirStmtLowerNameIsCompilerDiagBuiltin(c, callee->dataStart, callee->dataEnd);
}

static int HOPMirStmtLowerCallCanUseManualLowering(HOPMirStmtLower* c, int32_t callNode) {
    int32_t calleeNode;
    if (c == NULL || callNode < 0 || (uint32_t)callNode >= c->ast->len
        || c->ast->nodes[callNode].kind != HOPAst_CALL
        || HOPMirStmtLowerCallUsesLazyBuiltin(c, callNode))
    {
        return 0;
    }
    calleeNode = c->ast->nodes[callNode].firstChild;
    if (calleeNode < 0 || (uint32_t)calleeNode >= c->ast->len) {
        return 0;
    }
    return 1;
}

static int HOPMirStmtLowerCallExpr(HOPMirStmtLower* c, int32_t exprNode) {
    const HOPAstNode* call;
    int32_t           calleeNode;
    int32_t           argNode;
    uint32_t          argc = 0;
    uint32_t          callFlags = 0;
    uint16_t          callTokFlags = 0;
    uint32_t          callStart = 0;
    uint32_t          callEnd = 0;
    uint32_t          localSlot = 0;
    int               isBuiltinLen = 0;
    int               isBuiltinCStr = 0;
    int               isIndirectLocalCall = 0;
    HOPMirInst        inst = { 0 };
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        if (c != NULL) {
            c->supported = 0;
        }
        return 0;
    }
    call = &c->ast->nodes[exprNode];
    calleeNode = call->firstChild;
    if (call->kind != HOPAst_CALL || calleeNode < 0 || (uint32_t)calleeNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    if (c->ast->nodes[calleeNode].kind == HOPAst_IDENT) {
        if (HOPMirStmtLowerFindLocal(
                c,
                c->ast->nodes[calleeNode].dataStart,
                c->ast->nodes[calleeNode].dataEnd,
                &localSlot,
                NULL))
        {
            if (HOPMirStmtLowerAppendInst(
                    c,
                    HOPMirOp_LOCAL_LOAD,
                    0,
                    localSlot,
                    c->ast->nodes[calleeNode].dataStart,
                    c->ast->nodes[calleeNode].dataEnd,
                    NULL)
                != 0)
            {
                return -1;
            }
            isIndirectLocalCall = 1;
        }
        callStart = c->ast->nodes[calleeNode].dataStart;
        callEnd = c->ast->nodes[calleeNode].dataEnd;
        if (!isIndirectLocalCall) {
            isBuiltinLen = HOPMirStmtLowerNameEqLiteral(c, callStart, callEnd, "len");
            isBuiltinCStr = HOPMirStmtLowerNameEqLiteral(c, callStart, callEnd, "cstr");
        }
    } else if (c->ast->nodes[calleeNode].kind == HOPAst_FIELD_EXPR) {
        int32_t baseNode = c->ast->nodes[calleeNode].firstChild;
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        argc = 1u;
        callFlags = HOPMirSymbolFlag_CALL_RECEIVER_ARG0;
        callStart = c->ast->nodes[calleeNode].dataStart;
        callEnd = c->ast->nodes[calleeNode].dataEnd;
        isBuiltinCStr = HOPMirStmtLowerNameEqLiteral(c, callStart, callEnd, "cstr");
    } else {
        c->supported = 0;
        return 0;
    }
    argNode = c->ast->nodes[calleeNode].nextSibling;
    while (argNode >= 0) {
        int32_t valueNode = argNode;
        int     isSpread = 0;
        if (c->ast->nodes[argNode].kind == HOPAst_CALL_ARG) {
            valueNode = c->ast->nodes[argNode].firstChild;
            if (valueNode < 0) {
                c->supported = 0;
                return 0;
            }
            isSpread = (c->ast->nodes[argNode].flags & HOPAstFlag_CALL_ARG_SPREAD) != 0u;
        }
        if (isSpread) {
            if (c->ast->nodes[argNode].nextSibling >= 0
                || (callTokFlags & HOPMirCallArgFlag_SPREAD_LAST) != 0u)
            {
                c->supported = 0;
                return 0;
            }
            callTokFlags |= HOPMirCallArgFlag_SPREAD_LAST;
        }
        if (HOPMirStmtLowerExpr(c, valueNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (argc == UINT16_MAX) {
            c->supported = 0;
            return 0;
        }
        argc++;
        argNode = c->ast->nodes[argNode].nextSibling;
    }
    if (isBuiltinLen && callFlags == 0u && argc == 1u) {
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_SEQ_LEN, HOPTok_INVALID, 0, callStart, callEnd, NULL);
    }
    if (isBuiltinCStr && argc == 1u) {
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_STR_CSTR, HOPTok_INVALID, 0, callStart, callEnd, NULL);
    }
    inst.op = isIndirectLocalCall ? HOPMirOp_CALL_INDIRECT : HOPMirOp_CALL;
    inst.tok = (uint16_t)argc | callTokFlags;
    inst.aux = isIndirectLocalCall ? 0u : HOPMirRawCallAuxPack((uint32_t)exprNode, callFlags);
    inst.start = callStart;
    inst.end = callEnd;
    return HOPMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag);
}

static int HOPMirStmtLowerAppendIntConst(
    HOPMirStmtLower* c, int64_t value, uint32_t start, uint32_t end) {
    HOPMirConst valueConst = { 0 };
    uint32_t    constIndex = 0;
    if (c == NULL) {
        return -1;
    }
    valueConst.kind = HOPMirConst_INT;
    valueConst.bits = (uint64_t)value;
    if (HOPMirProgramBuilderAddConst(&c->builder, &valueConst, &constIndex) != 0) {
        HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, start, end);
        return -1;
    }
    return HOPMirStmtLowerAppendInst(c, HOPMirOp_PUSH_CONST, 0, constIndex, start, end, NULL);
}

static int HOPMirStmtLowerAppendConstValue(
    HOPMirStmtLower* c, const HOPMirConst* value, uint32_t start, uint32_t end) {
    uint32_t constIndex = 0;
    if (c == NULL || value == NULL) {
        return -1;
    }
    if (HOPMirProgramBuilderAddConst(&c->builder, value, &constIndex) != 0) {
        HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, start, end);
        return -1;
    }
    return HOPMirStmtLowerAppendInst(c, HOPMirOp_PUSH_CONST, 0, constIndex, start, end, NULL);
}

static int HOPMirStmtLowerAppendTupleMake(
    HOPMirStmtLower* c, uint32_t elemCount, int32_t typeNodeHint, uint32_t start, uint32_t end) {
    uint32_t aux = UINT32_MAX;
    if (c == NULL || elemCount > UINT16_MAX) {
        if (c != NULL) {
            c->supported = 0;
        }
        return 0;
    }
    if (typeNodeHint >= 0) {
        aux = (uint32_t)typeNodeHint;
    }
    return HOPMirStmtLowerAppendInst(
        c, HOPMirOp_TUPLE_MAKE, (uint16_t)elemCount, aux, start, end, NULL);
}

static int HOPMirStmtLowerRangeHasChar(HOPStrView src, uint32_t start, uint32_t end, char ch) {
    uint32_t i;
    if (end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (src.ptr[i] == ch) {
            return 1;
        }
    }
    return 0;
}

static int HOPMirStmtLowerFindCharForward(
    HOPStrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
    uint32_t i;
    if (outPos != NULL) {
        *outPos = 0;
    }
    if (outPos == NULL || end < start || end > src.len) {
        return 0;
    }
    for (i = start; i < end; i++) {
        if (src.ptr[i] == ch) {
            *outPos = i;
            return 1;
        }
    }
    return 0;
}

static HOPMirLowerControl* _Nullable HOPMirStmtLowerCurrentBreakable(HOPMirStmtLower* c) {
    if (c == NULL || c->controlLen == 0) {
        return NULL;
    }
    return &c->controls[c->controlLen - 1u];
}

static HOPMirLowerControl* _Nullable HOPMirStmtLowerCurrentContinuable(HOPMirStmtLower* c) {
    uint32_t i;
    if (c == NULL || c->controlLen == 0) {
        return NULL;
    }
    i = c->controlLen;
    while (i > 0) {
        HOPMirLowerControl* control;
        i--;
        control = &c->controls[i];
        if (control->hasContinue) {
            return control;
        }
    }
    return NULL;
}

static int HOPMirStmtLowerPushControl(
    HOPMirStmtLower* c, int hasContinue, uint32_t continueTargetPc) {
    if (c == NULL || c->controlLen >= (uint32_t)(sizeof(c->controls) / sizeof(c->controls[0]))) {
        if (c != NULL) {
            c->supported = 0;
        }
        return 0;
    }
    c->controls[c->controlLen++] = (HOPMirLowerControl){
        .breakJumpStart = c->breakJumpLen,
        .continueJumpStart = c->continueJumpLen,
        .continueTargetPc = continueTargetPc,
        .blockDepth = c->blockDepth,
        .hasContinue = hasContinue ? 1u : 0u,
    };
    return 1;
}

static int HOPMirStmtLowerRecordControlJump(
    HOPMirStmtLower* c, int isContinue, uint32_t instIndex) {
    if (c == NULL || c->controlLen == 0) {
        if (c != NULL) {
            c->supported = 0;
        }
        return 0;
    }
    if (isContinue) {
        if (c->continueJumpLen
            >= (uint32_t)(sizeof(c->continueJumps) / sizeof(c->continueJumps[0])))
        {
            c->supported = 0;
            return 0;
        }
        c->continueJumps[c->continueJumpLen++] = instIndex;
        return 1;
    }
    if (c->breakJumpLen >= (uint32_t)(sizeof(c->breakJumps) / sizeof(c->breakJumps[0]))) {
        c->supported = 0;
        return 0;
    }
    c->breakJumps[c->breakJumpLen++] = instIndex;
    return 1;
}

static void HOPMirStmtLowerPatchJumpRange(
    HOPMirStmtLower* c, const uint32_t* jumps, uint32_t start, uint32_t end, uint32_t targetPc) {
    uint32_t i;
    if (c == NULL || jumps == NULL) {
        return;
    }
    for (i = start; i < end; i++) {
        c->builder.insts[jumps[i]].aux = targetPc;
    }
}

static void HOPMirStmtLowerFinishControl(
    HOPMirStmtLower* c, uint32_t continueTargetPc, uint32_t breakTargetPc) {
    HOPMirLowerControl control;
    if (c == NULL || c->controlLen == 0) {
        return;
    }
    control = c->controls[c->controlLen - 1u];
    if (control.hasContinue) {
        HOPMirStmtLowerPatchJumpRange(
            c, c->continueJumps, control.continueJumpStart, c->continueJumpLen, continueTargetPc);
        c->continueJumpLen = control.continueJumpStart;
    }
    HOPMirStmtLowerPatchJumpRange(
        c, c->breakJumps, control.breakJumpStart, c->breakJumpLen, breakTargetPc);
    c->breakJumpLen = control.breakJumpStart;
    c->controlLen--;
}

static int HOPMirStmtLowerExprInstStackDelta(const HOPMirInst* inst, int32_t* outDelta) {
    uint32_t elemCount = 0;
    if (inst == NULL || outDelta == NULL) {
        return 0;
    }
    switch (inst->op) {
        case HOPMirOp_PUSH_CONST:
        case HOPMirOp_PUSH_INT:
        case HOPMirOp_PUSH_FLOAT:
        case HOPMirOp_PUSH_BOOL:
        case HOPMirOp_PUSH_STRING:
        case HOPMirOp_PUSH_NULL:
        case HOPMirOp_LOAD_IDENT:      *outDelta = 1; return 1;
        case HOPMirOp_UNARY:
        case HOPMirOp_CAST:
        case HOPMirOp_SEQ_LEN:
        case HOPMirOp_STR_CSTR:
        case HOPMirOp_OPTIONAL_WRAP:
        case HOPMirOp_OPTIONAL_UNWRAP: *outDelta = 0; return 1;
        case HOPMirOp_BINARY:
        case HOPMirOp_INDEX:           *outDelta = -1; return 1;
        case HOPMirOp_SLICE_MAKE:
            *outDelta = 0 - (((inst->tok & HOPAstFlag_INDEX_HAS_START) != 0u) ? 1 : 0)
                      - (((inst->tok & HOPAstFlag_INDEX_HAS_END) != 0u) ? 1 : 0);
            return 1;
        case HOPMirOp_CALL: *outDelta = 1 - (int32_t)HOPMirCallArgCountFromTok(inst->tok); return 1;
        case HOPMirOp_TUPLE_MAKE:
            elemCount = (uint32_t)inst->tok;
            *outDelta = 1 - (int32_t)elemCount;
            return 1;
        default: return 0;
    }
}

static int HOPMirStmtLowerFindCallArgStart(
    const HOPMirChunk* chunk, uint32_t callIndex, uint32_t argCount, uint32_t* outArgStart) {
    int32_t  need = 0;
    uint32_t i;
    if (outArgStart != NULL) {
        *outArgStart = UINT32_MAX;
    }
    if (chunk == NULL || outArgStart == NULL || callIndex > chunk->len || argCount == 0u) {
        return 0;
    }
    need = (int32_t)argCount;
    i = callIndex;
    while (i > 0u) {
        int32_t delta = 0;
        i--;
        if (!HOPMirStmtLowerExprInstStackDelta(&chunk->v[i], &delta)) {
            return 0;
        }
        need -= delta;
        if (need == 0) {
            *outArgStart = i;
            return 1;
        }
    }
    return 0;
}

static int HOPMirStmtLowerRewriteExprChunk(HOPMirStmtLower* c, const HOPMirChunk* chunk) {
    HOPMirChunkInsert* inserts = NULL;
    uint32_t*          insertHeads = NULL;
    uint32_t           insertLen = 0;
    uint32_t           i;
    uint32_t           chunkLen;
    uint32_t           emitIndex;
    uint32_t           insertIndex;
    uint8_t*           callIndirect = NULL;
    if (chunk == NULL) {
        return -1;
    }
    chunkLen = chunk->len;
    if (chunkLen != 0u) {
        inserts = (HOPMirChunkInsert*)HOPArenaAlloc(
            c->arena, sizeof(HOPMirChunkInsert) * chunkLen, (uint32_t)_Alignof(HOPMirChunkInsert));
        insertHeads = (uint32_t*)HOPArenaAlloc(
            c->arena, sizeof(uint32_t) * chunkLen, (uint32_t)_Alignof(uint32_t));
        callIndirect = (uint8_t*)HOPArenaAlloc(
            c->arena, chunkLen * sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
        if (inserts == NULL || insertHeads == NULL || callIndirect == NULL) {
            HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, 0, 0);
            return -1;
        }
        memset(callIndirect, 0, chunkLen * sizeof(uint8_t));
        for (i = 0; i < chunkLen; i++) {
            insertHeads[i] = UINT32_MAX;
        }
        for (i = chunkLen; i > 0u; i--) {
            HOPMirInst inst = chunk->v[i - 1u];
            uint32_t   slot = 0;
            uint32_t   argStart = UINT32_MAX;
            if (inst.op != HOPMirOp_CALL || HOPMirCallTokDropsReceiverArg0(inst.tok)
                || !HOPMirStmtLowerFindLocal(c, inst.start, inst.end, &slot, NULL))
            {
                continue;
            }
            if (HOPMirCallArgCountFromTok(inst.tok) == 0u) {
                argStart = i - 1u;
            } else if (!HOPMirStmtLowerFindCallArgStart(
                           chunk, i - 1u, HOPMirCallArgCountFromTok(inst.tok), &argStart))
            {
                c->supported = 0;
                return 0;
            }
            inserts[insertLen].atIndex = argStart;
            inserts[insertLen].slot = slot;
            inserts[insertLen].start = inst.start;
            inserts[insertLen].end = inst.end;
            inserts[insertLen].next = insertHeads[argStart];
            insertHeads[argStart] = insertLen++;
            callIndirect[i - 1u] = 1u;
        }
    }
    for (emitIndex = 0; emitIndex < chunkLen; emitIndex++) {
        insertIndex = insertHeads != NULL ? insertHeads[emitIndex] : UINT32_MAX;
        while (insertIndex != UINT32_MAX) {
            if (HOPMirStmtLowerAppendInst(
                    c,
                    HOPMirOp_LOCAL_LOAD,
                    0,
                    inserts[insertIndex].slot,
                    inserts[insertIndex].start,
                    inserts[insertIndex].end,
                    NULL)
                != 0)
            {
                return -1;
            }
            insertIndex = inserts[insertIndex].next;
        }
        HOPMirInst inst = chunk->v[emitIndex];
        if (inst.op == HOPMirOp_LOAD_IDENT) {
            uint32_t slot = 0;
            if (HOPMirStmtLowerFindLocal(c, inst.start, inst.end, &slot, NULL)) {
                inst.op = HOPMirOp_LOCAL_LOAD;
                inst.tok = 0;
                inst.aux = slot;
            }
        }
        if (inst.op == HOPMirOp_AGG_GET || inst.op == HOPMirOp_AGG_ADDR) {
            uint32_t fieldRef = UINT32_MAX;
            if (HOPMirStmtLowerAddFieldRef(c, inst.start, inst.end, &fieldRef) != 0) {
                return -1;
            }
            inst.aux = fieldRef;
        }
        if (callIndirect != NULL && callIndirect[emitIndex] != 0u) {
            inst.op = HOPMirOp_CALL_INDIRECT;
            inst.aux = 0u;
        }
        if (HOPMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag) != 0) {
            return -1;
        }
    }
    return 0;
}

static int HOPMirStmtLowerIsReplayableExpr(const HOPMirStmtLower* c, int32_t exprNode);

static int HOPMirStmtLowerExpr(HOPMirStmtLower* c, int32_t exprNode) {
    const HOPAstNode* expr;
    HOPMirChunk       chunk = { 0 };
    int               supported = 0;
    uint32_t          elemCount = 0;
    uint32_t          i;
    int32_t           lhsNode;
    int32_t           rhsNode;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    {
        int64_t sizeValue = 0;
        if (HOPMirStmtLowerTryConstSizeofExpr(c, exprNode, &sizeValue)) {
            return HOPMirStmtLowerAppendIntConst(c, sizeValue, expr->start, expr->end);
        }
        if (expr->kind == HOPAst_CAST) {
            int32_t valueNode = expr->firstChild;
            int32_t typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode >= 0 && typeNode >= 0 && c->ast->nodes[typeNode].nextSibling < 0
                && HOPMirStmtLowerTryConstSizeofExpr(c, valueNode, &sizeValue))
            {
                return HOPMirStmtLowerAppendIntConst(c, sizeValue, expr->start, expr->end);
            }
        }
    }
    if (c->lowerConstExpr != NULL && !HOPMirStmtLowerCallUsesLazyBuiltin(c, exprNode)) {
        HOPMirConst loweredConst = { 0 };
        int lowerRc = c->lowerConstExpr(c->lowerConstExprCtx, exprNode, &loweredConst, c->diag);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            return HOPMirStmtLowerAppendConstValue(c, &loweredConst, expr->start, expr->end);
        }
        if (expr->kind == HOPAst_CAST) {
            int32_t valueNode = expr->firstChild;
            int32_t typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode >= 0 && typeNode >= 0 && c->ast->nodes[typeNode].nextSibling < 0) {
                lowerRc = c->lowerConstExpr(
                    c->lowerConstExprCtx, valueNode, &loweredConst, c->diag);
                if (lowerRc < 0) {
                    return -1;
                }
                if (lowerRc > 0) {
                    return HOPMirStmtLowerAppendConstValue(
                        c, &loweredConst, expr->start, expr->end);
                }
            }
        }
    }
    if (expr->kind == HOPAst_CAST) {
        int32_t       valueNode = expr->firstChild;
        int32_t       typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
        int32_t       extraNode = typeNode >= 0 ? c->ast->nodes[typeNode].nextSibling : -1;
        HOPMirTypeRef typeRef = { 0 };
        uint32_t      typeRefIndex = UINT32_MAX;
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0
            && HOPMirStmtLowerCastNeedsCoerce(c, typeNode))
        {
            if (HOPMirStmtLowerExpr(c, valueNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            typeRef.astNode = (uint32_t)typeNode;
            typeRef.sourceRef = c->sourceIndex;
            typeRef.flags = 0u;
            typeRef.aux = 0u;
            if (HOPMirProgramBuilderAddType(&c->builder, &typeRef, &typeRefIndex) != 0) {
                HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, expr->start, expr->end);
                return -1;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_COERCE, 0, typeRefIndex, expr->start, expr->end, NULL);
        }
    }
    if (expr->kind == HOPAst_INDEX && (expr->flags & HOPAstFlag_INDEX_SLICE) != 0u) {
        int32_t  baseNode = expr->firstChild;
        int32_t  extraNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
        int32_t  startNode = -1;
        int32_t  endNode = -1;
        uint16_t sliceFlags =
            (uint16_t)(expr->flags & (HOPAstFlag_INDEX_HAS_START | HOPAstFlag_INDEX_HAS_END));
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if ((expr->flags & HOPAstFlag_INDEX_HAS_START) != 0u) {
            startNode = extraNode;
            extraNode = startNode >= 0 ? c->ast->nodes[startNode].nextSibling : -1;
            if (startNode < 0 || (uint32_t)startNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, startNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        }
        if ((expr->flags & HOPAstFlag_INDEX_HAS_END) != 0u) {
            endNode = extraNode;
            extraNode = endNode >= 0 ? c->ast->nodes[endNode].nextSibling : -1;
            if (endNode < 0 || (uint32_t)endNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, endNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        }
        if (extraNode >= 0) {
            c->supported = 0;
            return 0;
        }
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_SLICE_MAKE, sliceFlags, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == HOPAst_INDEX) {
        int32_t baseNode = expr->firstChild;
        int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
        if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        return HOPMirStmtLowerAppendInst(c, HOPMirOp_INDEX, 0, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == HOPAst_NEW) {
        return HOPMirStmtLowerAppendInst(
            c,
            HOPMirOp_ALLOC_NEW,
            (uint16_t)expr->flags,
            (uint32_t)exprNode,
            expr->start,
            expr->end,
            NULL);
    }
    if (expr->kind == HOPAst_CALL_WITH_CONTEXT) {
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_CTX_SET, 0, (uint32_t)exprNode, expr->start, expr->end, NULL);
    }
    if (expr->kind == HOPAst_TYPE_VALUE) {
        int32_t           typeNode = expr->firstChild;
        const HOPAstNode* type =
            typeNode >= 0 && (uint32_t)typeNode < c->ast->len ? &c->ast->nodes[typeNode] : NULL;
        if (type == NULL || type->kind != HOPAst_TYPE_NAME) {
            c->supported = 0;
            return 0;
        }
        return HOPMirStmtLowerAppendLoadValueBySlice(
            c, type->dataStart, type->dataEnd, expr->start, expr->end);
    }
    if (expr->kind == HOPAst_TUPLE_EXPR) {
        elemCount = HOPMirStmtLowerAstListCount(c->ast, exprNode);
        if (elemCount == 0u) {
            c->supported = 0;
            return 0;
        }
        for (i = 0; i < elemCount; i++) {
            int32_t itemNode = HOPMirStmtLowerAstListItemAt(c->ast, exprNode, i);
            if (itemNode < 0) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, itemNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        }
        return HOPMirStmtLowerAppendTupleMake(c, elemCount, exprNode, expr->start, expr->end);
    }
    if (expr->kind == HOPAst_UNWRAP) {
        int32_t childNode = expr->firstChild;
        if (childNode < 0 || (uint32_t)childNode >= c->ast->len
            || c->ast->nodes[childNode].nextSibling >= 0)
        {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerExpr(c, childNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_OPTIONAL_UNWRAP, 0, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == HOPAst_UNARY) {
        int32_t child = expr->firstChild;
        if (child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == HOPAst_IDENT)
        {
            uint32_t slot = 0;
            if (HOPMirStmtLowerFindLocal(
                    c, c->ast->nodes[child].dataStart, c->ast->nodes[child].dataEnd, &slot, NULL))
            {
                if ((HOPTokenKind)expr->op == HOPTok_AND) {
                    return HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_LOCAL_ADDR, 0, slot, expr->start, expr->end, NULL);
                }
            }
            if ((HOPTokenKind)expr->op == HOPTok_AND) {
                if (HOPMirStmtLowerAppendLoadValueBySlice(
                        c,
                        c->ast->nodes[child].dataStart,
                        c->ast->nodes[child].dataEnd,
                        c->ast->nodes[child].start,
                        c->ast->nodes[child].end)
                    != 0)
                {
                    return -1;
                }
                return HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_ADDR_OF, 0, 0, expr->start, expr->end, NULL);
            }
        }
        if ((HOPTokenKind)expr->op == HOPTok_AND && child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == HOPAst_FIELD_EXPR)
        {
            int32_t  baseNode = c->ast->nodes[child].firstChild;
            uint32_t fieldRef = UINT32_MAX;
            uint32_t contextField = HOPMirContextField_INVALID;
            if (HOPMirStmtLowerIsContextFieldExpr(c, child, &contextField)) {
                return HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_CTX_ADDR, 0, contextField, expr->start, expr->end, NULL);
            }
            if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (HOPMirStmtLowerAddFieldRef(
                    c, c->ast->nodes[child].dataStart, c->ast->nodes[child].dataEnd, &fieldRef)
                != 0)
            {
                return -1;
            }
            return HOPMirStmtLowerAppendInst(
                c,
                HOPMirOp_AGG_ADDR,
                0,
                fieldRef,
                c->ast->nodes[child].dataStart,
                c->ast->nodes[child].dataEnd,
                NULL);
        }
        if ((HOPTokenKind)expr->op == HOPTok_AND && child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == HOPAst_INDEX
            && (c->ast->nodes[child].flags & 0x7u) == 0u)
        {
            int32_t baseNode = c->ast->nodes[child].firstChild;
            int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
            if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (HOPMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_ARRAY_ADDR, 0, 0, expr->start, expr->end, NULL);
        }
        if ((HOPTokenKind)expr->op == HOPTok_AND && child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == HOPAst_COMPOUND_LIT)
        {
            if (HOPMirStmtLowerExpr(c, child) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_ADDR_OF, 0, 0, expr->start, expr->end, NULL);
        }
        if ((HOPTokenKind)expr->op == HOPTok_MUL && HOPMirStmtLowerIsReplayableExpr(c, child)) {
            if (HOPMirStmtLowerExpr(c, child) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_DEREF_LOAD, 0, 0, expr->start, expr->end, NULL);
        }
        if ((HOPTokenKind)expr->op != HOPTok_AND && (HOPTokenKind)expr->op != HOPTok_MUL) {
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, child) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_UNARY, (uint16_t)expr->op, 0, expr->start, expr->end, NULL);
        }
    }
    if (expr->kind == HOPAst_FIELD_EXPR) {
        int32_t  baseNode = expr->firstChild;
        uint32_t fieldRef = UINT32_MAX;
        uint32_t contextField = HOPMirContextField_INVALID;
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerIsContextFieldExpr(c, exprNode, &contextField)) {
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_CTX_GET, 0, contextField, expr->start, expr->end, NULL);
        }
        if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerAddFieldRef(c, expr->dataStart, expr->dataEnd, &fieldRef) != 0) {
            return -1;
        }
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_AGG_GET, 0, fieldRef, expr->dataStart, expr->dataEnd, NULL);
    }
    if (expr->kind == HOPAst_BINARY) {
        lhsNode = expr->firstChild;
        rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
        if (lhsNode < 0 || rhsNode < 0 || c->ast->nodes[rhsNode].nextSibling >= 0
            || (HOPTokenKind)expr->op == HOPTok_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_ADD_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_SUB_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_MUL_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_DIV_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_MOD_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_AND_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_OR_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_XOR_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_LSHIFT_ASSIGN
            || (HOPTokenKind)expr->op == HOPTok_RSHIFT_ASSIGN)
        {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerExpr(c, lhsNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_BINARY, (uint16_t)expr->op, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == HOPAst_COMPOUND_LIT) {
        int32_t       child = expr->firstChild;
        int32_t       typeNode = -1;
        HOPMirTypeRef typeRef = { 0 };
        uint32_t      typeRefIndex = UINT32_MAX;
        uint32_t      fieldCount = 0;
        if (child >= 0 && (uint32_t)child < c->ast->len
            && HOPMirStmtLowerIsTypeNodeKind(c->ast->nodes[child].kind))
        {
            typeNode = child;
            child = c->ast->nodes[child].nextSibling;
        }
        if (typeNode < 0) {
            int32_t scan = child;
            while (scan >= 0) {
                if (c->ast->nodes[scan].kind != HOPAst_COMPOUND_FIELD) {
                    c->supported = 0;
                    return 0;
                }
                fieldCount++;
                scan = c->ast->nodes[scan].nextSibling;
            }
            if (fieldCount > UINT16_MAX) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerAppendInst(
                    c,
                    HOPMirOp_AGG_MAKE,
                    (uint16_t)fieldCount,
                    (uint32_t)exprNode,
                    expr->start,
                    expr->end,
                    NULL)
                != 0)
            {
                return -1;
            }
        } else {
            typeRef.astNode = (uint32_t)typeNode;
            typeRef.sourceRef = c->sourceIndex;
            typeRef.flags = 0u;
            typeRef.aux = 0u;
            if (HOPMirProgramBuilderAddType(&c->builder, &typeRef, &typeRefIndex) != 0) {
                HOPMirLowerStmtSetDiag(c->diag, HOPDiag_ARENA_OOM, expr->start, expr->end);
                return -1;
            }
            if (HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_AGG_ZERO, 0, typeRefIndex, expr->start, expr->end, NULL)
                != 0)
            {
                return -1;
            }
        }
        while (child >= 0) {
            const HOPAstNode* field = &c->ast->nodes[child];
            int32_t           valueNode = c->ast->nodes[child].firstChild;
            uint32_t          fieldRef = UINT32_MAX;
            if (field->kind != HOPAst_COMPOUND_FIELD) {
                c->supported = 0;
                return 0;
            }
            if (valueNode >= 0) {
                if (HOPMirStmtLowerExpr(c, valueNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            } else if ((field->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
                if (HOPMirStmtLowerAppendLoadValueBySlice(
                        c, field->dataStart, field->dataEnd, field->start, field->end)
                    != 0)
                {
                    return -1;
                }
            } else {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerAddFieldRef(c, field->dataStart, field->dataEnd, &fieldRef) != 0) {
                return -1;
            }
            if (HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_AGG_SET, 0, fieldRef, field->dataStart, field->dataEnd, NULL)
                != 0)
            {
                return -1;
            }
            child = c->ast->nodes[child].nextSibling;
        }
        if (typeNode >= 0) {
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_COERCE, 0, typeRefIndex, expr->start, expr->end, NULL);
        }
        return 0;
    }
    if (expr->kind == HOPAst_CALL && HOPMirStmtLowerCallCanUseManualLowering(c, exprNode)) {
        return HOPMirStmtLowerCallExpr(c, exprNode);
    }
    if (HOPMirBuildExpr(c->arena, c->ast, c->src, exprNode, &chunk, &supported, c->diag) != 0) {
        return -1;
    }
    if (!supported || chunk.len == 0 || chunk.v[chunk.len - 1].op != HOPMirOp_RETURN) {
        c->supported = 0;
        return 0;
    }
    chunk.len--;
    return HOPMirStmtLowerRewriteExprChunk(c, &chunk);
}

static int HOPMirStmtLowerBinaryOpForAssign(HOPTokenKind tok, HOPTokenKind* outTok) {
    switch (tok) {
        case HOPTok_ADD_ASSIGN:    *outTok = HOPTok_ADD; return 1;
        case HOPTok_SUB_ASSIGN:    *outTok = HOPTok_SUB; return 1;
        case HOPTok_MUL_ASSIGN:    *outTok = HOPTok_MUL; return 1;
        case HOPTok_DIV_ASSIGN:    *outTok = HOPTok_DIV; return 1;
        case HOPTok_MOD_ASSIGN:    *outTok = HOPTok_MOD; return 1;
        case HOPTok_AND_ASSIGN:    *outTok = HOPTok_AND; return 1;
        case HOPTok_OR_ASSIGN:     *outTok = HOPTok_OR; return 1;
        case HOPTok_XOR_ASSIGN:    *outTok = HOPTok_XOR; return 1;
        case HOPTok_LSHIFT_ASSIGN: *outTok = HOPTok_LSHIFT; return 1;
        case HOPTok_RSHIFT_ASSIGN: *outTok = HOPTok_RSHIFT; return 1;
        default:                   *outTok = HOPTok_INVALID; return 0;
    }
}

static int HOPMirStmtLowerIsReplayableExpr(const HOPMirStmtLower* c, int32_t exprNode) {
    const HOPAstNode* expr;
    int32_t           lhsNode;
    int32_t           rhsNode;
    HOPTokenKind      binaryTok = HOPTok_INVALID;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    switch (expr->kind) {
        case HOPAst_IDENT:
        case HOPAst_INT:
        case HOPAst_FLOAT:
        case HOPAst_STRING:
        case HOPAst_BOOL:
        case HOPAst_NULL:   return 1;
        case HOPAst_UNARY:
            if ((HOPTokenKind)expr->op == HOPTok_AND) {
                return 0;
            }
            lhsNode = expr->firstChild;
            return lhsNode >= 0 && c->ast->nodes[lhsNode].nextSibling < 0
                && HOPMirStmtLowerIsReplayableExpr(c, lhsNode);
        case HOPAst_BINARY:
            lhsNode = expr->firstChild;
            rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
            if (lhsNode < 0 || rhsNode < 0 || c->ast->nodes[rhsNode].nextSibling >= 0
                || (HOPTokenKind)expr->op == HOPTok_ASSIGN)
            {
                return 0;
            }
            if (HOPMirStmtLowerBinaryOpForAssign((HOPTokenKind)expr->op, &binaryTok)) {
                return 0;
            }
            return HOPMirStmtLowerIsReplayableExpr(c, lhsNode)
                && HOPMirStmtLowerIsReplayableExpr(c, rhsNode);
        case HOPAst_INDEX:
            lhsNode = expr->firstChild;
            rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
            return (expr->flags & 0x7u) == 0u && lhsNode >= 0 && rhsNode >= 0
                && c->ast->nodes[rhsNode].nextSibling < 0
                && HOPMirStmtLowerIsReplayableExpr(c, lhsNode)
                && HOPMirStmtLowerIsReplayableExpr(c, rhsNode);
        case HOPAst_FIELD_EXPR:
            lhsNode = expr->firstChild;
            return lhsNode >= 0 && c->ast->nodes[lhsNode].nextSibling < 0
                && HOPMirStmtLowerIsReplayableExpr(c, lhsNode);
        default: return 0;
    }
}

static int32_t HOPMirStmtLowerVarInitExprNode(const HOPAst* ast, int32_t nodeId) {
    int32_t firstChild;
    int32_t nextNode;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0) {
        return -1;
    }
    if (ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
        nextNode = ast->nodes[firstChild].nextSibling;
        if (nextNode >= 0 && HOPMirStmtLowerIsTypeNodeKind(ast->nodes[nextNode].kind)) {
            nextNode = ast->nodes[nextNode].nextSibling;
        }
        return nextNode;
    }
    if (HOPMirStmtLowerIsTypeNodeKind(ast->nodes[firstChild].kind)) {
        return ast->nodes[firstChild].nextSibling;
    }
    return firstChild;
}

static int32_t HOPMirStmtLowerVarLikeDeclTypeNode(const HOPAst* ast, int32_t nodeId) {
    int32_t firstChild;
    int32_t afterNames;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0 || (uint32_t)firstChild >= ast->len) {
        return -1;
    }
    if (ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
        afterNames = ast->nodes[firstChild].nextSibling;
        if (afterNames >= 0 && HOPMirStmtLowerIsTypeNodeKind(ast->nodes[afterNames].kind)) {
            return afterNames;
        }
        return -1;
    }
    if (HOPMirStmtLowerIsTypeNodeKind(ast->nodes[firstChild].kind)) {
        return firstChild;
    }
    return -1;
}

static int32_t HOPMirStmtLowerVarLikeInitExprNodeAt(
    const HOPAst* ast, int32_t nodeId, int32_t nameIndex) {
    int32_t firstChild;
    int32_t initNode;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len || nameIndex < 0) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0 || (uint32_t)firstChild >= ast->len) {
        return -1;
    }
    initNode = HOPMirStmtLowerVarInitExprNode(ast, nodeId);
    if (initNode < 0 || (uint32_t)initNode >= ast->len) {
        return -1;
    }
    if (ast->nodes[firstChild].kind != HOPAst_NAME_LIST) {
        return nameIndex == 0 ? initNode : -1;
    }
    if (ast->nodes[initNode].kind != HOPAst_EXPR_LIST) {
        return -1;
    }
    {
        uint32_t nameCount = HOPMirStmtLowerAstListCount(ast, firstChild);
        uint32_t initCount = HOPMirStmtLowerAstListCount(ast, initNode);
        if ((uint32_t)nameIndex >= nameCount) {
            return -1;
        }
        if (initCount == nameCount) {
            return HOPMirStmtLowerAstListItemAt(ast, initNode, (uint32_t)nameIndex);
        }
        if (initCount != 1u) {
            return -1;
        }
        {
            int32_t onlyInit = HOPMirStmtLowerAstListItemAt(ast, initNode, 0u);
            if (onlyInit < 0 || (uint32_t)onlyInit >= ast->len
                || ast->nodes[onlyInit].kind != HOPAst_TUPLE_EXPR)
            {
                return -1;
            }
            return HOPMirStmtLowerAstListItemAt(ast, onlyInit, (uint32_t)nameIndex);
        }
    }
}

static int HOPMirStmtLowerStmt(HOPMirStmtLower* c, int32_t stmtNode);

static int HOPMirStmtLowerEmitDeferredRange(HOPMirStmtLower* c, uint32_t start) {
    uint32_t i;
    uint8_t  savedLoweringDeferred;
    if (c == NULL || start > c->deferredStmtLen) {
        return -1;
    }
    savedLoweringDeferred = c->loweringDeferred;
    c->loweringDeferred = 1u;
    for (i = c->deferredStmtLen; i > start; i--) {
        if (HOPMirStmtLowerStmt(c, c->deferredStmtNodes[i - 1u]) != 0 || !c->supported) {
            c->loweringDeferred = savedLoweringDeferred;
            return c->supported ? -1 : 0;
        }
    }
    c->loweringDeferred = savedLoweringDeferred;
    return 0;
}

static int HOPMirStmtLowerEmitDeferredForControl(
    HOPMirStmtLower* c, const HOPMirLowerControl* control) {
    uint32_t targetDepth;
    uint32_t i;
    uint32_t originalDeferredLen;
    uint32_t deferLimit;
    if (c == NULL || control == NULL) {
        return -1;
    }
    targetDepth = control->blockDepth;
    originalDeferredLen = c->deferredStmtLen;
    deferLimit = c->deferredStmtLen;
    i = c->blockDepth;
    while (i > targetDepth) {
        const HOPMirLowerBlockScope* scope = &c->blockScopes[i - 1u];
        c->deferredStmtLen = deferLimit;
        if (HOPMirStmtLowerEmitDeferredRange(c, scope->deferStart) != 0 || !c->supported) {
            c->deferredStmtLen = originalDeferredLen;
            return c->supported ? -1 : 0;
        }
        deferLimit = scope->deferStart;
        i--;
    }
    c->deferredStmtLen = originalDeferredLen;
    return 0;
}

static int HOPMirStmtLowerBlock(HOPMirStmtLower* c, int32_t blockNode) {
    uint32_t scopeMark = c->localLen;
    uint32_t blockDepth = c->blockDepth;
    uint32_t deferStart;
    int32_t  child;
    if (blockNode < 0 || (uint32_t)blockNode >= c->ast->len
        || c->ast->nodes[blockNode].kind != HOPAst_BLOCK)
    {
        c->supported = 0;
        return 0;
    }
    if (c->blockDepth >= (uint32_t)(sizeof(c->blockScopes) / sizeof(c->blockScopes[0]))) {
        c->supported = 0;
        return 0;
    }
    deferStart = c->deferredStmtLen;
    c->blockScopes[c->blockDepth++].deferStart = deferStart;
    child = c->ast->nodes[blockNode].firstChild;
    while (child >= 0) {
        int32_t nextChild = c->ast->nodes[child].nextSibling;
        if (c->ast->nodes[child].kind == HOPAst_DEFER) {
            int32_t deferredStmtNode = c->ast->nodes[child].firstChild;
            if (c->loweringDeferred || deferredStmtNode < 0
                || c->ast->nodes[deferredStmtNode].nextSibling >= 0
                || c->deferredStmtLen >= (uint32_t)(sizeof(c->deferredStmtNodes)
                                                    / sizeof(c->deferredStmtNodes[0])))
            {
                c->supported = 0;
                c->deferredStmtLen = deferStart;
                c->blockDepth = blockDepth;
                c->localLen = scopeMark;
                return 0;
            }
            c->deferredStmtNodes[c->deferredStmtLen++] = deferredStmtNode;
            child = nextChild;
            continue;
        }
        if (HOPMirStmtLowerStmt(c, child) != 0 || !c->supported) {
            c->deferredStmtLen = deferStart;
            c->blockDepth = blockDepth;
            c->localLen = scopeMark;
            return c->supported ? -1 : 0;
        }
        child = nextChild;
    }
    if (HOPMirStmtLowerEmitDeferredRange(c, deferStart) != 0 || !c->supported) {
        c->deferredStmtLen = deferStart;
        c->blockDepth = blockDepth;
        c->localLen = scopeMark;
        return c->supported ? -1 : 0;
    }
    c->deferredStmtLen = deferStart;
    c->blockDepth = blockDepth;
    c->localLen = scopeMark;
    return 0;
}

static int HOPMirStmtLowerIf(HOPMirStmtLower* c, int32_t ifNode) {
    int32_t  condNode = c->ast->nodes[ifNode].firstChild;
    int32_t  thenNode = condNode >= 0 ? c->ast->nodes[condNode].nextSibling : -1;
    int32_t  elseNode = thenNode >= 0 ? c->ast->nodes[thenNode].nextSibling : -1;
    uint32_t falseJumpInst = UINT32_MAX;
    uint32_t endJumpInst = UINT32_MAX;
    if (condNode < 0 || thenNode < 0) {
        c->supported = 0;
        return 0;
    }
    if (HOPMirStmtLowerExpr(c, condNode) != 0 || !c->supported) {
        return c->supported ? -1 : 0;
    }
    if (HOPMirStmtLowerAppendInst(
            c,
            HOPMirOp_JUMP_IF_FALSE,
            0,
            UINT32_MAX,
            c->ast->nodes[ifNode].start,
            c->ast->nodes[ifNode].end,
            &falseJumpInst)
        != 0)
    {
        return -1;
    }
    if (c->ast->nodes[thenNode].kind == HOPAst_BLOCK) {
        if (HOPMirStmtLowerBlock(c, thenNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
    } else if (c->ast->nodes[thenNode].kind == HOPAst_IF) {
        if (HOPMirStmtLowerIf(c, thenNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
    } else {
        c->supported = 0;
        return 0;
    }
    if (elseNode >= 0) {
        if (HOPMirStmtLowerAppendInst(
                c,
                HOPMirOp_JUMP,
                0,
                UINT32_MAX,
                c->ast->nodes[ifNode].start,
                c->ast->nodes[ifNode].end,
                &endJumpInst)
            != 0)
        {
            return -1;
        }
    }
    c->builder.insts[falseJumpInst].aux = HOPMirStmtLowerFnPc(c);
    if (elseNode >= 0) {
        if (c->ast->nodes[elseNode].kind == HOPAst_BLOCK) {
            if (HOPMirStmtLowerBlock(c, elseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        } else if (c->ast->nodes[elseNode].kind == HOPAst_IF) {
            if (HOPMirStmtLowerIf(c, elseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        } else {
            c->supported = 0;
            return 0;
        }
        c->builder.insts[endJumpInst].aux = HOPMirStmtLowerFnPc(c);
    }
    return 0;
}

static int HOPMirStmtLowerStoreToLValueFromStack(
    HOPMirStmtLower* c, int32_t lhsNode, uint32_t start, uint32_t end) {
    uint32_t slot = 0;
    int      mutable = 0;
    if (lhsNode < 0 || (uint32_t)lhsNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    if (c->ast->nodes[lhsNode].kind == HOPAst_IDENT
        && HOPMirStmtLowerFindLocal(
            c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, &slot, &mutable))
    {
        if (!mutable) {
            c->supported = 0;
            return 0;
        }
        return HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_STORE, 0, slot, start, end, NULL);
    }
    if (c->ast->nodes[lhsNode].kind == HOPAst_UNARY
        && (HOPTokenKind)c->ast->nodes[lhsNode].op == HOPTok_MUL)
    {
        int32_t derefBase = c->ast->nodes[lhsNode].firstChild;
        if (derefBase >= 0 && (uint32_t)derefBase < c->ast->len
            && HOPMirStmtLowerIsReplayableExpr(c, derefBase))
        {
            if (HOPMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(c, HOPMirOp_DEREF_STORE, 0, 0, start, end, NULL);
        }
    }
    if (c->ast->nodes[lhsNode].kind == HOPAst_INDEX && (c->ast->nodes[lhsNode].flags & 0x7u) == 0u)
    {
        int32_t baseNode = c->ast->nodes[lhsNode].firstChild;
        int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
        if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerAppendInst(c, HOPMirOp_ARRAY_ADDR, 0, 0, start, end, NULL) != 0) {
            return -1;
        }
        return HOPMirStmtLowerAppendInst(c, HOPMirOp_DEREF_STORE, 0, 0, start, end, NULL);
    }
    if (c->ast->nodes[lhsNode].kind == HOPAst_FIELD_EXPR) {
        int32_t  baseNode = c->ast->nodes[lhsNode].firstChild;
        uint32_t fieldRef = UINT32_MAX;
        uint32_t contextField = HOPMirContextField_INVALID;
        if (HOPMirStmtLowerIsContextFieldExpr(c, lhsNode, &contextField)) {
            if (HOPMirStmtLowerAppendInst(
                    c,
                    HOPMirOp_CTX_ADDR,
                    0,
                    contextField,
                    c->ast->nodes[lhsNode].start,
                    c->ast->nodes[lhsNode].end,
                    NULL)
                != 0)
            {
                return -1;
            }
            return HOPMirStmtLowerAppendInst(c, HOPMirOp_DEREF_STORE, 0, 0, start, end, NULL);
        }
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerAddFieldRef(
                c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, &fieldRef)
            != 0)
        {
            return -1;
        }
        if (HOPMirStmtLowerAppendInst(
                c,
                HOPMirOp_AGG_ADDR,
                0,
                fieldRef,
                c->ast->nodes[lhsNode].dataStart,
                c->ast->nodes[lhsNode].dataEnd,
                NULL)
            != 0)
        {
            return -1;
        }
        return HOPMirStmtLowerAppendInst(c, HOPMirOp_DEREF_STORE, 0, 0, start, end, NULL);
    }
    c->supported = 0;
    return 0;
}

static int HOPMirStmtLowerExprNodeAsStmt(
    HOPMirStmtLower* c, int32_t exprNode, uint32_t start, uint32_t end) {
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    if (c->ast->nodes[exprNode].kind == HOPAst_BINARY) {
        const HOPAstNode* expr = &c->ast->nodes[exprNode];
        int32_t           lhsNode = expr->firstChild;
        int32_t           rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
        uint32_t          slot = 0;
        int               mutable = 0;
        HOPTokenKind      binaryTok = HOPTok_INVALID;
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == HOPAst_IDENT
            && c->ast->nodes[lhsNode].dataEnd == c->ast->nodes[lhsNode].dataStart + 1u
            && c->ast->nodes[lhsNode].dataEnd <= c->src.len
            && c->src.ptr[c->ast->nodes[lhsNode].dataStart] == '_')
        {
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(c, HOPMirOp_DROP, 0, 0, expr->start, expr->end, NULL);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == HOPAst_IDENT
            && HOPMirStmtLowerFindLocal(
                c,
                c->ast->nodes[lhsNode].dataStart,
                c->ast->nodes[lhsNode].dataEnd,
                &slot,
                &mutable))
        {
            if (!mutable) {
                c->supported = 0;
                return 0;
            }
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                if (!HOPMirStmtLowerBinaryOpForAssign((HOPTokenKind)expr->op, &binaryTok)) {
                    c->supported = 0;
                    return 0;
                }
                if (HOPMirStmtLowerAppendInst(
                        c,
                        HOPMirOp_LOCAL_LOAD,
                        0,
                        slot,
                        c->ast->nodes[lhsNode].start,
                        c->ast->nodes[lhsNode].end,
                        NULL)
                    != 0)
                {
                    return -1;
                }
            }
            if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_LOCAL_STORE, 0, slot, expr->start, expr->end, NULL);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == HOPAst_IDENT)
        {
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendStoreValueBySlice(
                c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == HOPAst_UNARY
            && (HOPTokenKind)c->ast->nodes[lhsNode].op == HOPTok_MUL)
        {
            int32_t derefBase = c->ast->nodes[lhsNode].firstChild;
            if (derefBase >= 0 && (uint32_t)derefBase < c->ast->len
                && HOPMirStmtLowerIsReplayableExpr(c, derefBase))
            {
                if ((HOPTokenKind)expr->op == HOPTok_ASSIGN) {
                    if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (HOPMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    return HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
                }
                if (!HOPMirStmtLowerBinaryOpForAssign((HOPTokenKind)expr->op, &binaryTok)) {
                    c->supported = 0;
                    return 0;
                }
                if (HOPMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_DEREF_LOAD, 0, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
                if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
                if (HOPMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                return HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
            }
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == HOPAst_INDEX
            && (c->ast->nodes[lhsNode].flags & 0x7u) == 0u)
        {
            int32_t baseNode = c->ast->nodes[lhsNode].firstChild;
            int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
            if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
                c->supported = 0;
                return 0;
            }
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                if (!HOPMirStmtLowerBinaryOpForAssign((HOPTokenKind)expr->op, &binaryTok)
                    || !HOPMirStmtLowerIsReplayableExpr(c, lhsNode))
                {
                    c->supported = 0;
                    return 0;
                }
                if (HOPMirStmtLowerExpr(c, lhsNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            }
            if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (HOPMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (HOPMirStmtLowerAppendInst(
                    c,
                    HOPMirOp_ARRAY_ADDR,
                    0,
                    0,
                    c->ast->nodes[lhsNode].start,
                    c->ast->nodes[lhsNode].end,
                    NULL)
                != 0)
            {
                return -1;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == HOPAst_FIELD_EXPR)
        {
            int32_t  baseNode = c->ast->nodes[lhsNode].firstChild;
            uint32_t fieldRef = UINT32_MAX;
            uint32_t contextField = HOPMirContextField_INVALID;
            if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerIsContextFieldExpr(c, lhsNode, &contextField)) {
                if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                    if (!HOPMirStmtLowerBinaryOpForAssign((HOPTokenKind)expr->op, &binaryTok)) {
                        c->supported = 0;
                        return 0;
                    }
                    if (HOPMirStmtLowerAppendInst(
                            c,
                            HOPMirOp_CTX_GET,
                            0,
                            contextField,
                            c->ast->nodes[lhsNode].start,
                            c->ast->nodes[lhsNode].end,
                            NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                    if (HOPMirStmtLowerAppendInst(
                            c,
                            HOPMirOp_BINARY,
                            (uint16_t)binaryTok,
                            0,
                            expr->start,
                            expr->end,
                            NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                if (HOPMirStmtLowerAppendInst(
                        c,
                        HOPMirOp_CTX_ADDR,
                        0,
                        contextField,
                        c->ast->nodes[lhsNode].start,
                        c->ast->nodes[lhsNode].end,
                        NULL)
                    != 0)
                {
                    return -1;
                }
                return HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
            }
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                if (!HOPMirStmtLowerBinaryOpForAssign((HOPTokenKind)expr->op, &binaryTok)
                    || !HOPMirStmtLowerIsReplayableExpr(c, lhsNode))
                {
                    c->supported = 0;
                    return 0;
                }
                if (HOPMirStmtLowerExpr(c, lhsNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            }
            if (HOPMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            if (HOPMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (HOPMirStmtLowerAddFieldRef(
                    c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, &fieldRef)
                != 0)
            {
                return -1;
            }
            if (HOPMirStmtLowerAppendInst(
                    c,
                    HOPMirOp_AGG_ADDR,
                    0,
                    fieldRef,
                    c->ast->nodes[lhsNode].dataStart,
                    c->ast->nodes[lhsNode].dataEnd,
                    NULL)
                != 0)
            {
                return -1;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
        }
    }
    if (HOPMirStmtLowerExpr(c, exprNode) != 0 || !c->supported) {
        return c->supported ? -1 : 0;
    }
    return HOPMirStmtLowerAppendInst(c, HOPMirOp_DROP, 0, 0, start, end, NULL);
}

static int HOPMirStmtLowerExprStmt(HOPMirStmtLower* c, int32_t stmtNode) {
    int32_t exprNode = c->ast->nodes[stmtNode].firstChild;
    if (exprNode < 0 || c->ast->nodes[exprNode].nextSibling >= 0) {
        c->supported = 0;
        return 0;
    }
    return HOPMirStmtLowerExprNodeAsStmt(
        c, exprNode, c->ast->nodes[stmtNode].start, c->ast->nodes[stmtNode].end);
}

static int HOPMirStmtLowerDel(HOPMirStmtLower* c, int32_t stmtNode) {
    const HOPAstNode* s;
    int32_t           exprNode;
    int32_t           allocNode = -1;
    if (c == NULL || stmtNode < 0 || (uint32_t)stmtNode >= c->ast->len) {
        return -1;
    }
    s = &c->ast->nodes[stmtNode];
    exprNode = s->firstChild;
    if ((s->flags & HOPAstFlag_DEL_HAS_ALLOC) != 0u) {
        int32_t scan = exprNode;
        while (scan >= 0) {
            int32_t next = c->ast->nodes[scan].nextSibling;
            if (next < 0) {
                allocNode = scan;
                break;
            }
            scan = next;
        }
    }
    while (exprNode >= 0 && exprNode != allocNode) {
        if (HOPMirStmtLowerExpr(c, exprNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerAppendInst(
                c,
                HOPMirOp_DROP,
                0,
                0,
                c->ast->nodes[exprNode].start,
                c->ast->nodes[exprNode].end,
                NULL)
            != 0)
        {
            return -1;
        }
        exprNode = c->ast->nodes[exprNode].nextSibling;
    }
    if (allocNode >= 0) {
        if (HOPMirStmtLowerExpr(c, allocNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerAppendInst(
                c,
                HOPMirOp_DROP,
                0,
                0,
                c->ast->nodes[allocNode].start,
                c->ast->nodes[allocNode].end,
                NULL)
            != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int HOPMirStmtLowerSwitchTest(
    HOPMirStmtLower* c,
    uint32_t         subjectSlot,
    int              hasSubject,
    int32_t          labelExprNode,
    uint32_t         start,
    uint32_t         end) {
    if (hasSubject) {
        if (HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_LOAD, 0, subjectSlot, start, end, NULL)
            != 0)
        {
            return -1;
        }
    }
    if (HOPMirStmtLowerExpr(c, labelExprNode) != 0 || !c->supported) {
        return c->supported ? -1 : 0;
    }
    if (hasSubject) {
        return HOPMirStmtLowerAppendInst(
            c, HOPMirOp_BINARY, (uint16_t)HOPTok_EQ, 0, start, end, NULL);
    }
    return 0;
}

static int HOPMirStmtLowerSwitch(HOPMirStmtLower* c, int32_t stmtNode) {
    const HOPAstNode* s = &c->ast->nodes[stmtNode];
    int32_t           clauseNode = s->firstChild;
    int32_t           defaultBodyNode = -1;
    uint32_t          scopeMark = c->localLen;
    uint32_t          subjectSlot = UINT32_MAX;
    uint32_t          pendingNextClauseJump = UINT32_MAX;
    int               hasSubject = s->flags == 1;
    if (hasSubject) {
        if (clauseNode < 0 || (uint32_t)clauseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &subjectSlot) != 0) {
            return -1;
        }
        if (HOPMirStmtLowerExpr(c, clauseNode) != 0 || !c->supported) {
            c->localLen = scopeMark;
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerAppendInst(
                c, HOPMirOp_LOCAL_STORE, 0, subjectSlot, s->start, s->end, NULL)
            != 0)
        {
            c->localLen = scopeMark;
            return -1;
        }
        clauseNode = c->ast->nodes[clauseNode].nextSibling;
    }
    if (!HOPMirStmtLowerPushControl(c, 0, 0)) {
        c->localLen = scopeMark;
        return 0;
    }
    while (clauseNode >= 0) {
        const HOPAstNode* clause = &c->ast->nodes[clauseNode];
        if (pendingNextClauseJump != UINT32_MAX) {
            c->builder.insts[pendingNextClauseJump].aux = HOPMirStmtLowerFnPc(c);
            pendingNextClauseJump = UINT32_MAX;
        }
        if (clause->kind == HOPAst_CASE) {
            int32_t  caseChild = clause->firstChild;
            int32_t  bodyNode = -1;
            int32_t  aliasNode = -1;
            uint32_t pendingFalseJump = UINT32_MAX;
            uint32_t bodyJumps[64];
            uint32_t bodyJumpLen = 0;
            while (caseChild >= 0) {
                int32_t  next = c->ast->nodes[caseChild].nextSibling;
                int32_t  labelExprNode = caseChild;
                uint32_t falseJump = UINT32_MAX;
                uint32_t bodyJump = UINT32_MAX;
                if (next < 0) {
                    bodyNode = caseChild;
                    break;
                }
                if (pendingFalseJump != UINT32_MAX) {
                    c->builder.insts[pendingFalseJump].aux = HOPMirStmtLowerFnPc(c);
                }
                if (c->ast->nodes[caseChild].kind == HOPAst_CASE_PATTERN) {
                    labelExprNode = c->ast->nodes[caseChild].firstChild;
                    aliasNode = labelExprNode >= 0 ? c->ast->nodes[labelExprNode].nextSibling : -1;
                    if (labelExprNode < 0
                        || (aliasNode >= 0 && c->ast->nodes[aliasNode].nextSibling >= 0)
                        || (!hasSubject && aliasNode >= 0)
                        || (aliasNode >= 0 && c->ast->nodes[aliasNode].kind != HOPAst_IDENT))
                    {
                        c->supported = 0;
                        c->localLen = scopeMark;
                        c->controlLen--;
                        return 0;
                    }
                }
                if (labelExprNode < 0 || (uint32_t)labelExprNode >= c->ast->len) {
                    c->supported = 0;
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return 0;
                }
                if (HOPMirStmtLowerSwitchTest(
                        c,
                        subjectSlot,
                        hasSubject,
                        labelExprNode,
                        c->ast->nodes[labelExprNode].start,
                        c->ast->nodes[labelExprNode].end)
                        != 0
                    || !c->supported)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return c->supported ? -1 : 0;
                }
                if (HOPMirStmtLowerAppendInst(
                        c,
                        HOPMirOp_JUMP_IF_FALSE,
                        0,
                        UINT32_MAX,
                        c->ast->nodes[labelExprNode].start,
                        c->ast->nodes[labelExprNode].end,
                        &falseJump)
                    != 0)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return -1;
                }
                if (HOPMirStmtLowerAppendInst(
                        c,
                        HOPMirOp_JUMP,
                        0,
                        UINT32_MAX,
                        c->ast->nodes[labelExprNode].start,
                        c->ast->nodes[labelExprNode].end,
                        &bodyJump)
                    != 0)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return -1;
                }
                if (bodyJumpLen >= (uint32_t)(sizeof(bodyJumps) / sizeof(bodyJumps[0]))) {
                    c->supported = 0;
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return 0;
                }
                bodyJumps[bodyJumpLen++] = bodyJump;
                pendingFalseJump = falseJump;
                caseChild = next;
            }
            if (bodyNode < 0 || (uint32_t)bodyNode >= c->ast->len
                || c->ast->nodes[bodyNode].kind != HOPAst_BLOCK)
            {
                c->supported = 0;
                c->localLen = scopeMark;
                c->controlLen--;
                return 0;
            }
            {
                uint32_t bodyPc = HOPMirStmtLowerFnPc(c);
                uint32_t i;
                for (i = 0; i < bodyJumpLen; i++) {
                    c->builder.insts[bodyJumps[i]].aux = bodyPc;
                }
            }
            if (aliasNode >= 0) {
                uint32_t aliasSlot = UINT32_MAX;
                if (HOPMirStmtLowerPushLocal(
                        c,
                        c->ast->nodes[aliasNode].dataStart,
                        c->ast->nodes[aliasNode].dataEnd,
                        1,
                        0,
                        0,
                        -1,
                        -1,
                        &aliasSlot)
                    != 0)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return -1;
                }
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_LOCAL_LOAD, 0, subjectSlot, clause->start, clause->end, NULL)
                        != 0
                    || HOPMirStmtLowerAppendInst(
                           c, HOPMirOp_LOCAL_STORE, 0, aliasSlot, clause->start, clause->end, NULL)
                           != 0)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return -1;
                }
            }
            if (HOPMirStmtLowerBlock(c, bodyNode) != 0 || !c->supported) {
                c->localLen = scopeMark;
                c->controlLen--;
                return c->supported ? -1 : 0;
            }
            if (aliasNode >= 0) {
                c->localLen--;
            }
            {
                uint32_t endJump = UINT32_MAX;
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_JUMP, 0, UINT32_MAX, clause->start, clause->end, &endJump)
                    != 0)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return -1;
                }
                if (!HOPMirStmtLowerRecordControlJump(c, 0, endJump)) {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return 0;
                }
            }
            pendingNextClauseJump = pendingFalseJump;
        } else if (clause->kind == HOPAst_DEFAULT) {
            int32_t nextClause = c->ast->nodes[clauseNode].nextSibling;
            defaultBodyNode = clause->firstChild;
            if (defaultBodyNode < 0 || (uint32_t)defaultBodyNode >= c->ast->len
                || c->ast->nodes[defaultBodyNode].kind != HOPAst_BLOCK || nextClause >= 0)
            {
                c->supported = 0;
                c->localLen = scopeMark;
                c->controlLen--;
                return 0;
            }
        } else {
            c->supported = 0;
            c->localLen = scopeMark;
            c->controlLen--;
            return 0;
        }
        clauseNode = c->ast->nodes[clauseNode].nextSibling;
    }
    if (defaultBodyNode >= 0) {
        if (pendingNextClauseJump != UINT32_MAX) {
            c->builder.insts[pendingNextClauseJump].aux = HOPMirStmtLowerFnPc(c);
            pendingNextClauseJump = UINT32_MAX;
        }
        if (HOPMirStmtLowerBlock(c, defaultBodyNode) != 0 || !c->supported) {
            c->localLen = scopeMark;
            c->controlLen--;
            return c->supported ? -1 : 0;
        }
    }
    {
        uint32_t endPc = HOPMirStmtLowerFnPc(c);
        if (pendingNextClauseJump != UINT32_MAX) {
            c->builder.insts[pendingNextClauseJump].aux = endPc;
        }
        HOPMirStmtLowerFinishControl(c, 0, endPc);
    }
    c->localLen = scopeMark;
    return 0;
}

static int HOPMirStmtLowerForIn(HOPMirStmtLower* c, int32_t stmtNode) {
    const HOPAstNode* s = &c->ast->nodes[stmtNode];
    int32_t           parts[4];
    uint32_t          partLen = 0;
    int32_t           cur = s->firstChild;
    int               hasKey = (s->flags & HOPAstFlag_FOR_IN_HAS_KEY) != 0;
    int               keyRef = (s->flags & HOPAstFlag_FOR_IN_KEY_REF) != 0;
    int               valueRef = (s->flags & HOPAstFlag_FOR_IN_VALUE_REF) != 0;
    int               valueDiscard = (s->flags & HOPAstFlag_FOR_IN_VALUE_DISCARD) != 0;
    int32_t           keyNode = -1;
    int32_t           valueNode = -1;
    int32_t           sourceNode = -1;
    int32_t           bodyNode = -1;
    uint32_t          scopeMark = c->localLen;
    uint32_t          sourceSlot = UINT32_MAX;
    uint32_t          iterSlot = UINT32_MAX;
    uint32_t          keySlot = UINT32_MAX;
    uint32_t          valueSlot = UINT32_MAX;
    uint32_t          loopStartPc;
    uint32_t          continueTargetPc;
    uint32_t          loopEndPc;
    uint32_t          condFalseJump = UINT32_MAX;
    uint16_t          iterFlags = 0;
    if (keyRef) {
        c->supported = 0;
        return 0;
    }
    while (cur >= 0 && partLen < 4u) {
        parts[partLen++] = cur;
        cur = c->ast->nodes[cur].nextSibling;
    }
    if ((!hasKey && partLen != 3u) || (hasKey && partLen != 4u) || cur >= 0) {
        c->supported = 0;
        return 0;
    }
    if (hasKey) {
        keyNode = parts[0];
        valueNode = parts[1];
        sourceNode = parts[2];
        bodyNode = parts[3];
    } else {
        valueNode = parts[0];
        sourceNode = parts[1];
        bodyNode = parts[2];
    }
    if (bodyNode < 0 || (uint32_t)bodyNode >= c->ast->len
        || c->ast->nodes[bodyNode].kind != HOPAst_BLOCK || sourceNode < 0
        || (uint32_t)sourceNode >= c->ast->len)
    {
        c->supported = 0;
        return 0;
    }
    if (hasKey
        && (keyNode < 0 || (uint32_t)keyNode >= c->ast->len
            || c->ast->nodes[keyNode].kind != HOPAst_IDENT))
    {
        c->supported = 0;
        return 0;
    }
    if (!valueDiscard
        && (valueNode < 0 || (uint32_t)valueNode >= c->ast->len
            || c->ast->nodes[valueNode].kind != HOPAst_IDENT))
    {
        c->supported = 0;
        return 0;
    }
    if ((hasKey ? HOPMirIterFlag_HAS_KEY : 0u) != 0u) {
        iterFlags |= HOPMirIterFlag_HAS_KEY;
    }
    if (keyRef) {
        iterFlags |= HOPMirIterFlag_KEY_REF;
    }
    if (valueRef) {
        iterFlags |= HOPMirIterFlag_VALUE_REF;
    }
    if (valueDiscard) {
        iterFlags |= HOPMirIterFlag_VALUE_DISCARD;
    }
    if (HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &sourceSlot) != 0
        || HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &iterSlot) != 0)
    {
        return -1;
    }
    if (HOPMirStmtLowerExpr(c, sourceNode) != 0 || !c->supported) {
        c->localLen = scopeMark;
        return c->supported ? -1 : 0;
    }
    if (HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_STORE, 0, sourceSlot, s->start, s->end, NULL)
            != 0
        || HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_LOAD, 0, sourceSlot, s->start, s->end, NULL)
               != 0
        || HOPMirStmtLowerAppendInst(
               c, HOPMirOp_ITER_INIT, iterFlags, (uint32_t)sourceNode, s->start, s->end, NULL)
               != 0
        || HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_STORE, 0, iterSlot, s->start, s->end, NULL)
               != 0)
    {
        c->localLen = scopeMark;
        return -1;
    }
    if (hasKey
        && !(
            c->ast->nodes[keyNode].dataEnd == c->ast->nodes[keyNode].dataStart + 1u
            && c->src.ptr[c->ast->nodes[keyNode].dataStart] == '_')
        && HOPMirStmtLowerPushLocal(
               c,
               c->ast->nodes[keyNode].dataStart,
               c->ast->nodes[keyNode].dataEnd,
               1,
               0,
               0,
               -1,
               -1,
               &keySlot)
               != 0)
    {
        c->localLen = scopeMark;
        return -1;
    }
    if (!valueDiscard
        && !(
            c->ast->nodes[valueNode].dataEnd == c->ast->nodes[valueNode].dataStart + 1u
            && c->src.ptr[c->ast->nodes[valueNode].dataStart] == '_')
        && HOPMirStmtLowerPushLocal(
               c,
               c->ast->nodes[valueNode].dataStart,
               c->ast->nodes[valueNode].dataEnd,
               1,
               0,
               0,
               -1,
               -1,
               &valueSlot)
               != 0)
    {
        c->localLen = scopeMark;
        return -1;
    }
    loopStartPc = HOPMirStmtLowerFnPc(c);
    if (!HOPMirStmtLowerPushControl(c, 1, loopStartPc)) {
        c->localLen = scopeMark;
        return 0;
    }
    if (HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_LOAD, 0, iterSlot, s->start, s->end, NULL) != 0
        || HOPMirStmtLowerAppendInst(c, HOPMirOp_ITER_NEXT, iterFlags, 0, s->start, s->end, NULL)
               != 0
        || HOPMirStmtLowerAppendInst(
               c, HOPMirOp_JUMP_IF_FALSE, 0, UINT32_MAX, s->start, s->end, &condFalseJump)
               != 0)
    {
        c->localLen = scopeMark;
        c->controlLen--;
        return -1;
    }
    if (!valueDiscard && valueSlot != UINT32_MAX) {
        if (HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_STORE, 0, valueSlot, s->start, s->end, NULL)
            != 0)
        {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    } else if (!valueDiscard) {
        if (HOPMirStmtLowerAppendInst(c, HOPMirOp_DROP, 0, 0, s->start, s->end, NULL) != 0) {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    }
    if (hasKey && keySlot != UINT32_MAX) {
        if (HOPMirStmtLowerAppendInst(c, HOPMirOp_LOCAL_STORE, 0, keySlot, s->start, s->end, NULL)
            != 0)
        {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    } else if (hasKey) {
        if (HOPMirStmtLowerAppendInst(c, HOPMirOp_DROP, 0, 0, s->start, s->end, NULL) != 0) {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    }
    if (HOPMirStmtLowerBlock(c, bodyNode) != 0 || !c->supported) {
        c->localLen = scopeMark;
        c->controlLen--;
        return c->supported ? -1 : 0;
    }
    continueTargetPc = HOPMirStmtLowerFnPc(c);
    if (HOPMirStmtLowerAppendInst(c, HOPMirOp_JUMP, 0, loopStartPc, s->start, s->end, NULL) != 0) {
        c->localLen = scopeMark;
        c->controlLen--;
        return -1;
    }
    loopEndPc = HOPMirStmtLowerFnPc(c);
    c->builder.insts[condFalseJump].aux = loopEndPc;
    HOPMirStmtLowerFinishControl(c, continueTargetPc, loopEndPc);
    c->localLen = scopeMark;
    return 0;
}

static int HOPMirStmtLowerFor(HOPMirStmtLower* c, int32_t stmtNode) {
    const HOPAstNode* s = &c->ast->nodes[stmtNode];
    int32_t           parts[4];
    uint32_t          partLen = 0;
    int32_t           cur = s->firstChild;
    int32_t           initNode = -1;
    int32_t           condNode = -1;
    int32_t           postNode = -1;
    int32_t           bodyNode = -1;
    uint32_t          bodyStart;
    uint32_t          scopeMark = c->localLen;
    uint32_t          loopStartPc;
    uint32_t          condFalseJump = UINT32_MAX;
    uint32_t          continueTargetPc;
    uint32_t          loopEndPc;
    uint32_t          semi1 = 0;
    uint32_t          semi2 = 0;
    int               hasSemicolons;
    if ((s->flags & HOPAstFlag_FOR_IN) != 0) {
        return HOPMirStmtLowerForIn(c, stmtNode);
    }
    while (cur >= 0 && partLen < 4u) {
        parts[partLen++] = cur;
        cur = c->ast->nodes[cur].nextSibling;
    }
    if (partLen == 0 || cur >= 0) {
        c->supported = 0;
        return 0;
    }
    bodyNode = parts[partLen - 1u];
    if ((uint32_t)bodyNode >= c->ast->len || c->ast->nodes[bodyNode].kind != HOPAst_BLOCK) {
        c->supported = 0;
        return 0;
    }
    bodyStart = c->ast->nodes[bodyNode].start;
    hasSemicolons = HOPMirStmtLowerRangeHasChar(c->src, s->start, bodyStart, ';');
    if (!hasSemicolons) {
        if (partLen == 2u) {
            condNode = parts[0];
        } else if (partLen != 1u) {
            c->supported = 0;
            return 0;
        }
    } else {
        uint32_t i;
        if (!HOPMirStmtLowerFindCharForward(c->src, s->start, bodyStart, ';', &semi1)
            || !HOPMirStmtLowerFindCharForward(c->src, semi1 + 1u, bodyStart, ';', &semi2))
        {
            c->supported = 0;
            return 0;
        }
        for (i = 0; i + 1u < partLen; i++) {
            const HOPAstNode* part = &c->ast->nodes[parts[i]];
            if (part->end <= semi1) {
                if (initNode >= 0) {
                    c->supported = 0;
                    return 0;
                }
                initNode = parts[i];
            } else if (part->start > semi2) {
                if (postNode >= 0) {
                    c->supported = 0;
                    return 0;
                }
                postNode = parts[i];
            } else if (part->start > semi1 && part->end <= semi2) {
                if (condNode >= 0) {
                    c->supported = 0;
                    return 0;
                }
                condNode = parts[i];
            } else {
                c->supported = 0;
                return 0;
            }
        }
    }
    if (initNode >= 0) {
        if (c->ast->nodes[initNode].kind == HOPAst_VAR
            || c->ast->nodes[initNode].kind == HOPAst_CONST
            || c->ast->nodes[initNode].kind == HOPAst_SHORT_ASSIGN)
        {
            if (HOPMirStmtLowerStmt(c, initNode) != 0 || !c->supported) {
                c->localLen = scopeMark;
                return c->supported ? -1 : 0;
            }
        } else if (
            HOPMirStmtLowerExprNodeAsStmt(
                c, initNode, c->ast->nodes[initNode].start, c->ast->nodes[initNode].end)
                != 0
            || !c->supported)
        {
            c->localLen = scopeMark;
            return c->supported ? -1 : 0;
        }
    }
    loopStartPc = HOPMirStmtLowerFnPc(c);
    if (!HOPMirStmtLowerPushControl(c, 1, loopStartPc)) {
        c->localLen = scopeMark;
        return 0;
    }
    if (condNode >= 0) {
        if (HOPMirStmtLowerExpr(c, condNode) != 0 || !c->supported) {
            c->localLen = scopeMark;
            c->controlLen--;
            return c->supported ? -1 : 0;
        }
        if (HOPMirStmtLowerAppendInst(
                c, HOPMirOp_JUMP_IF_FALSE, 0, UINT32_MAX, s->start, s->end, &condFalseJump)
            != 0)
        {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    }
    if (HOPMirStmtLowerBlock(c, bodyNode) != 0 || !c->supported) {
        c->localLen = scopeMark;
        c->controlLen--;
        return c->supported ? -1 : 0;
    }
    continueTargetPc = postNode >= 0 ? HOPMirStmtLowerFnPc(c) : loopStartPc;
    if (postNode >= 0
        && (HOPMirStmtLowerExprNodeAsStmt(
                c, postNode, c->ast->nodes[postNode].start, c->ast->nodes[postNode].end)
                != 0
            || !c->supported))
    {
        c->localLen = scopeMark;
        c->controlLen--;
        return c->supported ? -1 : 0;
    }
    if (HOPMirStmtLowerAppendInst(c, HOPMirOp_JUMP, 0, loopStartPc, s->start, s->end, NULL) != 0) {
        c->localLen = scopeMark;
        c->controlLen--;
        return -1;
    }
    loopEndPc = HOPMirStmtLowerFnPc(c);
    if (condFalseJump != UINT32_MAX) {
        c->builder.insts[condFalseJump].aux = loopEndPc;
    }
    HOPMirStmtLowerFinishControl(c, continueTargetPc, loopEndPc);
    c->localLen = scopeMark;
    return 0;
}

static int HOPMirStmtLowerStmt(HOPMirStmtLower* c, int32_t stmtNode) {
    const HOPAstNode* s;
    if (stmtNode < 0 || (uint32_t)stmtNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    s = &c->ast->nodes[stmtNode];
    switch (s->kind) {
        case HOPAst_BLOCK:  return HOPMirStmtLowerBlock(c, stmtNode);
        case HOPAst_IF:     return HOPMirStmtLowerIf(c, stmtNode);
        case HOPAst_SWITCH: return HOPMirStmtLowerSwitch(c, stmtNode);
        case HOPAst_FOR:    return HOPMirStmtLowerFor(c, stmtNode);
        case HOPAst_BREAK:  {
            uint32_t            jumpInst = UINT32_MAX;
            HOPMirLowerControl* control = HOPMirStmtLowerCurrentBreakable(c);
            if (c->loweringDeferred || control == NULL) {
                if (c->loweringDeferred) {
                    HOPMirLowerStmtSetUnsupportedDetail(
                        c,
                        s->start,
                        s->end,
                        "deferred statement cannot alter control flow in const evaluation");
                } else {
                    c->supported = 0;
                }
                return 0;
            }
            if (c->deferredStmtLen > 0
                && (HOPMirStmtLowerEmitDeferredForControl(c, control) != 0 || !c->supported))
            {
                return c->supported ? -1 : 0;
            }
            if (HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_JUMP, 0, UINT32_MAX, s->start, s->end, &jumpInst)
                != 0)
            {
                return -1;
            }
            if (!HOPMirStmtLowerRecordControlJump(c, 0, jumpInst)) {
                return 0;
            }
            return 0;
        }
        case HOPAst_CONTINUE: {
            uint32_t            jumpInst = UINT32_MAX;
            HOPMirLowerControl* control = HOPMirStmtLowerCurrentContinuable(c);
            if (c->loweringDeferred || control == NULL) {
                if (c->loweringDeferred) {
                    HOPMirLowerStmtSetUnsupportedDetail(
                        c,
                        s->start,
                        s->end,
                        "deferred statement cannot alter control flow in const evaluation");
                } else {
                    c->supported = 0;
                }
                return 0;
            }
            if (c->deferredStmtLen > 0
                && (HOPMirStmtLowerEmitDeferredForControl(c, control) != 0 || !c->supported))
            {
                return c->supported ? -1 : 0;
            }
            if (HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_JUMP, 0, control->continueTargetPc, s->start, s->end, &jumpInst)
                != 0)
            {
                return -1;
            }
            if (!HOPMirStmtLowerRecordControlJump(c, 1, jumpInst)) {
                return 0;
            }
            return 0;
        }
        case HOPAst_RETURN: {
            int32_t  exprNode = s->firstChild;
            uint32_t exprCount = 0;
            if (c->loweringDeferred) {
                HOPMirLowerStmtSetUnsupportedDetail(
                    c,
                    s->start,
                    s->end,
                    "deferred statement cannot alter control flow in const evaluation");
                return 0;
            }
            if (exprNode < 0) {
                if (c->deferredStmtLen > 0
                    && (HOPMirStmtLowerEmitDeferredRange(c, 0) != 0 || !c->supported))
                {
                    return c->supported ? -1 : 0;
                }
                return HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_RETURN_VOID, 0, 0, s->start, s->end, NULL);
            }
            if (c->deferredStmtLen > 0
                && (HOPMirStmtLowerEmitDeferredRange(c, 0) != 0 || !c->supported))
            {
                return c->supported ? -1 : 0;
            }
            if (c->ast->nodes[exprNode].kind == HOPAst_EXPR_LIST) {
                exprCount = HOPMirStmtLowerAstListCount(c->ast, exprNode);
            } else {
                while (exprNode >= 0) {
                    exprCount++;
                    exprNode = c->ast->nodes[exprNode].nextSibling;
                }
                exprNode = s->firstChild;
            }
            if (exprCount == 1u) {
                if (c->ast->nodes[exprNode].kind == HOPAst_EXPR_LIST) {
                    exprNode = HOPMirStmtLowerAstListItemAt(c->ast, exprNode, 0u);
                    if (exprNode < 0) {
                        c->supported = 0;
                        return 0;
                    }
                }
                if (HOPMirStmtLowerExpr(c, exprNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            } else {
                int32_t returnTypeNode = c->functionReturnTypeNode;
                int32_t tupleReturnTypeNode = returnTypeNode;
                if (tupleReturnTypeNode >= 0 && (uint32_t)tupleReturnTypeNode < c->ast->len
                    && c->ast->nodes[tupleReturnTypeNode].kind == HOPAst_TYPE_OPTIONAL)
                {
                    tupleReturnTypeNode = c->ast->nodes[tupleReturnTypeNode].firstChild;
                }
                if (exprCount > UINT16_MAX || returnTypeNode < 0 || tupleReturnTypeNode < 0
                    || (uint32_t)tupleReturnTypeNode >= c->ast->len
                    || c->ast->nodes[tupleReturnTypeNode].kind != HOPAst_TYPE_TUPLE
                    || HOPMirStmtLowerAstListCount(c->ast, tupleReturnTypeNode) != exprCount)
                {
                    c->supported = 0;
                    return 0;
                }
                if (c->ast->nodes[exprNode].kind == HOPAst_EXPR_LIST) {
                    uint32_t i;
                    for (i = 0; i < exprCount; i++) {
                        int32_t itemNode = HOPMirStmtLowerAstListItemAt(c->ast, exprNode, i);
                        if (itemNode < 0) {
                            c->supported = 0;
                            return 0;
                        }
                        if (HOPMirStmtLowerExpr(c, itemNode) != 0 || !c->supported) {
                            return c->supported ? -1 : 0;
                        }
                    }
                } else {
                    int32_t returnExprNode = s->firstChild;
                    while (returnExprNode >= 0) {
                        int32_t itemNode = returnExprNode;
                        returnExprNode = c->ast->nodes[returnExprNode].nextSibling;
                        if (HOPMirStmtLowerExpr(c, itemNode) != 0 || !c->supported) {
                            return c->supported ? -1 : 0;
                        }
                    }
                }
                if (HOPMirStmtLowerAppendTupleMake(
                        c, exprCount, tupleReturnTypeNode, s->start, s->end)
                    != 0)
                {
                    return -1;
                }
            }
            return HOPMirStmtLowerAppendInst(c, HOPMirOp_RETURN, 0, 0, s->start, s->end, NULL);
        }
        case HOPAst_ASSERT: {
            int32_t condNode = s->firstChild;
            if (condNode < 0) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerExpr(c, condNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(c, HOPMirOp_ASSERT, 0, 0, s->start, s->end, NULL);
        }
        case HOPAst_MULTI_ASSIGN: {
            int32_t  lhsList = s->firstChild;
            int32_t  rhsList = lhsList >= 0 ? c->ast->nodes[lhsList].nextSibling : -1;
            uint32_t lhsCount;
            uint32_t rhsCount;
            uint32_t i;
            uint32_t tempSlots[256];
            uint32_t tupleTempSlot = UINT32_MAX;
            if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != HOPAst_EXPR_LIST
                || c->ast->nodes[rhsList].kind != HOPAst_EXPR_LIST)
            {
                c->supported = 0;
                return 0;
            }
            lhsCount = HOPMirStmtLowerAstListCount(c->ast, lhsList);
            rhsCount = HOPMirStmtLowerAstListCount(c->ast, rhsList);
            if (lhsCount == 0u || lhsCount > 256u || (rhsCount != lhsCount && rhsCount != 1u)) {
                c->supported = 0;
                return 0;
            }
            if (rhsCount == lhsCount) {
                for (i = 0; i < rhsCount; i++) {
                    int32_t rhsExpr = HOPMirStmtLowerAstListItemAt(c->ast, rhsList, i);
                    if (rhsExpr < 0
                        || HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tempSlots[i]) != 0)
                    {
                        return rhsExpr < 0 ? 0 : -1;
                    }
                    if (HOPMirStmtLowerExpr(c, rhsExpr) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (HOPMirStmtLowerAppendInst(
                            c, HOPMirOp_LOCAL_STORE, 0, tempSlots[i], s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
            } else {
                int32_t rhsExpr = HOPMirStmtLowerAstListItemAt(c->ast, rhsList, 0u);
                if (rhsExpr < 0
                    || HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tupleTempSlot) != 0)
                {
                    return rhsExpr < 0 ? 0 : -1;
                }
                if (HOPMirStmtLowerExpr(c, rhsExpr) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_LOCAL_STORE, 0, tupleTempSlot, s->start, s->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            for (i = 0; i < lhsCount; i++) {
                int32_t lhsExpr = HOPMirStmtLowerAstListItemAt(c->ast, lhsList, i);
                if (lhsExpr < 0) {
                    c->supported = 0;
                    return 0;
                }
                if (rhsCount == lhsCount) {
                    if (HOPMirStmtLowerAppendInst(
                            c, HOPMirOp_LOCAL_LOAD, 0, tempSlots[i], s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                } else {
                    if (HOPMirStmtLowerAppendInst(
                            c, HOPMirOp_LOCAL_LOAD, 0, tupleTempSlot, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                    if (HOPMirStmtLowerAppendIntConst(c, (int64_t)i, s->start, s->end) != 0) {
                        return -1;
                    }
                    if (HOPMirStmtLowerAppendInst(c, HOPMirOp_INDEX, 0, 0, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                if (HOPMirStmtLowerStoreToLValueFromStack(c, lhsExpr, s->start, s->end) != 0
                    || !c->supported)
                {
                    return c->supported ? -1 : 0;
                }
            }
            return 0;
        }
        case HOPAst_SHORT_ASSIGN: {
            int32_t  nameList = s->firstChild;
            int32_t  rhsList = nameList >= 0 ? c->ast->nodes[nameList].nextSibling : -1;
            uint32_t lhsCount;
            uint32_t rhsCount;
            uint32_t i;
            uint32_t tempSlots[256];
            uint32_t existingSlots[256];
            uint8_t  existingMutable[256];
            uint8_t  isExisting[256];
            uint8_t  isBlank[256];
            uint32_t tupleTempSlot = UINT32_MAX;
            if (nameList < 0 || rhsList < 0 || c->ast->nodes[nameList].kind != HOPAst_NAME_LIST
                || c->ast->nodes[rhsList].kind != HOPAst_EXPR_LIST)
            {
                c->supported = 0;
                return 0;
            }
            lhsCount = HOPMirStmtLowerAstListCount(c->ast, nameList);
            rhsCount = HOPMirStmtLowerAstListCount(c->ast, rhsList);
            if (lhsCount == 0u || lhsCount > 256u || (rhsCount != lhsCount && rhsCount != 1u)) {
                c->supported = 0;
                return 0;
            }
            for (i = 0; i < lhsCount; i++) {
                int32_t           nameNode = HOPMirStmtLowerAstListItemAt(c->ast, nameList, i);
                const HOPAstNode* name = nameNode >= 0 ? &c->ast->nodes[nameNode] : NULL;
                int               mutable = 0;
                uint32_t          slot = 0;
                if (name == NULL || name->kind != HOPAst_IDENT) {
                    c->supported = 0;
                    return 0;
                }
                isBlank[i] = (uint8_t)HOPMirStmtLowerNameEqLiteral(
                    c, name->dataStart, name->dataEnd, "_");
                isExisting[i] = 0;
                existingSlots[i] = UINT32_MAX;
                existingMutable[i] = 0;
                if (!isBlank[i]
                    && HOPMirStmtLowerFindLocal(c, name->dataStart, name->dataEnd, &slot, &mutable))
                {
                    isExisting[i] = 1;
                    existingSlots[i] = slot;
                    existingMutable[i] = mutable ? 1u : 0u;
                }
            }
            if (rhsCount == lhsCount) {
                for (i = 0; i < rhsCount; i++) {
                    int32_t rhsExpr = HOPMirStmtLowerAstListItemAt(c->ast, rhsList, i);
                    if (rhsExpr < 0
                        || HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tempSlots[i]) != 0)
                    {
                        return rhsExpr < 0 ? 0 : -1;
                    }
                    if (HOPMirStmtLowerExpr(c, rhsExpr) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (HOPMirStmtLowerAppendInst(
                            c, HOPMirOp_LOCAL_STORE, 0, tempSlots[i], s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
            } else {
                int32_t rhsExpr = HOPMirStmtLowerAstListItemAt(c->ast, rhsList, 0u);
                if (rhsExpr < 0
                    || HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tupleTempSlot) != 0)
                {
                    return rhsExpr < 0 ? 0 : -1;
                }
                if (HOPMirStmtLowerExpr(c, rhsExpr) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_LOCAL_STORE, 0, tupleTempSlot, s->start, s->end, NULL)
                    != 0)
                {
                    return -1;
                }
                for (i = 0; i < lhsCount; i++) {
                    if (HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tempSlots[i]) != 0) {
                        return -1;
                    }
                    if (HOPMirStmtLowerAppendInst(
                            c, HOPMirOp_LOCAL_LOAD, 0, tupleTempSlot, s->start, s->end, NULL)
                            != 0
                        || HOPMirStmtLowerAppendIntConst(c, (int64_t)i, s->start, s->end) != 0
                        || HOPMirStmtLowerAppendInst(
                               c, HOPMirOp_INDEX, 0, 0, s->start, s->end, NULL)
                               != 0
                        || HOPMirStmtLowerAppendInst(
                               c, HOPMirOp_LOCAL_STORE, 0, tempSlots[i], s->start, s->end, NULL)
                               != 0)
                    {
                        return -1;
                    }
                }
            }
            for (i = 0; i < lhsCount; i++) {
                int32_t           nameNode = HOPMirStmtLowerAstListItemAt(c->ast, nameList, i);
                const HOPAstNode* name = nameNode >= 0 ? &c->ast->nodes[nameNode] : NULL;
                uint32_t          targetSlot = UINT32_MAX;
                if (name == NULL) {
                    c->supported = 0;
                    return 0;
                }
                if (isBlank[i]) {
                    continue;
                }
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_LOCAL_LOAD, 0, tempSlots[i], s->start, s->end, NULL)
                    != 0)
                {
                    return -1;
                }
                if (isExisting[i]) {
                    if (!existingMutable[i]) {
                        c->supported = 0;
                        return 0;
                    }
                    targetSlot = existingSlots[i];
                } else if (
                    HOPMirStmtLowerPushLocal(
                        c,
                        name->dataStart,
                        name->dataEnd,
                        1,
                        0,
                        0,
                        -1,
                        rhsCount == lhsCount ? HOPMirStmtLowerAstListItemAt(c->ast, rhsList, i)
                                             : HOPMirStmtLowerAstListItemAt(c->ast, rhsList, 0u),
                        &targetSlot)
                    != 0)
                {
                    return -1;
                }
                if (HOPMirStmtLowerAppendInst(
                        c, HOPMirOp_LOCAL_STORE, 0, targetSlot, s->start, s->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
        case HOPAst_EXPR_STMT: return HOPMirStmtLowerExprStmt(c, stmtNode);
        case HOPAst_DEL:       return HOPMirStmtLowerDel(c, stmtNode);
        case HOPAst_CONST_BLOCK:
            /* Consteval blocks are compile-time-only and do not execute at runtime. */
            return 0;
        case HOPAst_VAR:
        case HOPAst_CONST: {
            int32_t  firstChild = s->firstChild;
            int32_t  typeNode = -1;
            int32_t  initNode = HOPMirStmtLowerVarInitExprNode(c->ast, stmtNode);
            int32_t  tupleInitNode = -1;
            uint32_t slot = 0;
            if (firstChild < 0) {
                c->supported = 0;
                return 0;
            }
            if (c->ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
                uint32_t nameCount = HOPMirStmtLowerAstListCount(c->ast, firstChild);
                uint32_t initCount = 0;
                uint32_t tupleTempSlot = UINT32_MAX;
                uint32_t nameLocalStart = c->localLen;
                int      useTupleInitTemp = 0;
                uint32_t i;
                typeNode = HOPMirStmtLowerVarLikeDeclTypeNode(c->ast, stmtNode);
                if (nameCount == 0u || (initNode < 0 && typeNode < 0)) {
                    c->supported = 0;
                    return 0;
                }
                if (initNode >= 0) {
                    if (c->ast->nodes[initNode].kind != HOPAst_EXPR_LIST) {
                        c->supported = 0;
                        return 0;
                    }
                    initCount = HOPMirStmtLowerAstListCount(c->ast, initNode);
                    if (initCount == 1u && nameCount > 1u) {
                        tupleInitNode = HOPMirStmtLowerAstListItemAt(c->ast, initNode, 0u);
                        if (tupleInitNode < 0) {
                            c->supported = 0;
                            return 0;
                        }
                        useTupleInitTemp = c->ast->nodes[tupleInitNode].kind != HOPAst_TUPLE_EXPR;
                    }
                }
                for (i = 0; i < nameCount; i++) {
                    uint32_t localSlot = UINT32_MAX;
                    int32_t  nameNode = HOPMirStmtLowerAstListItemAt(c->ast, firstChild, i);
                    const HOPAstNode* name = nameNode >= 0 ? &c->ast->nodes[nameNode] : NULL;
                    if (name == NULL || name->kind != HOPAst_IDENT) {
                        c->supported = 0;
                        return 0;
                    }
                    if (HOPMirStmtLowerPushLocal(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            s->kind == HOPAst_VAR,
                            0,
                            initNode < 0,
                            typeNode,
                            HOPMirStmtLowerVarLikeInitExprNodeAt(c->ast, stmtNode, (int32_t)i),
                            &localSlot)
                        != 0)
                    {
                        return -1;
                    }
                }
                if (useTupleInitTemp) {
                    if (HOPMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tupleTempSlot) != 0) {
                        return -1;
                    }
                    if (HOPMirStmtLowerExpr(c, tupleInitNode) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (HOPMirStmtLowerAppendInst(
                            c, HOPMirOp_LOCAL_STORE, 0, tupleTempSlot, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                for (i = 0; i < nameCount; i++) {
                    int32_t itemInitNode = HOPMirStmtLowerVarLikeInitExprNodeAt(
                        c->ast, stmtNode, (int32_t)i);
                    slot = c->locals[nameLocalStart + i].slot;
                    if (itemInitNode < 0) {
                        if (useTupleInitTemp) {
                            if (HOPMirStmtLowerAppendInst(
                                    c,
                                    HOPMirOp_LOCAL_LOAD,
                                    0,
                                    tupleTempSlot,
                                    s->start,
                                    s->end,
                                    NULL)
                                != 0)
                            {
                                return -1;
                            }
                            if (HOPMirStmtLowerAppendIntConst(c, (int64_t)i, s->start, s->end) != 0)
                            {
                                return -1;
                            }
                            if (HOPMirStmtLowerAppendInst(
                                    c, HOPMirOp_INDEX, 0, 0, s->start, s->end, NULL)
                                != 0)
                            {
                                return -1;
                            }
                            if (HOPMirStmtLowerAppendInst(
                                    c, HOPMirOp_LOCAL_STORE, 0, slot, s->start, s->end, NULL)
                                != 0)
                            {
                                return -1;
                            }
                            continue;
                        }
                        if (HOPMirStmtLowerAppendInst(
                                c, HOPMirOp_LOCAL_ZERO, 0, slot, s->start, s->end, NULL)
                            != 0)
                        {
                            return -1;
                        }
                        continue;
                    }
                    if (HOPMirStmtLowerExpr(c, itemInitNode) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (HOPMirStmtLowerAppendInst(
                            c, HOPMirOp_LOCAL_STORE, 0, slot, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                return 0;
            }
            if (HOPMirStmtLowerIsTypeNodeKind(c->ast->nodes[firstChild].kind)) {
                typeNode = firstChild;
            }
            if (initNode < 0 && typeNode < 0) {
                c->supported = 0;
                return 0;
            }
            if (HOPMirStmtLowerPushLocal(
                    c,
                    s->dataStart,
                    s->dataEnd,
                    s->kind == HOPAst_VAR,
                    0,
                    initNode < 0,
                    typeNode,
                    initNode,
                    &slot)
                != 0)
            {
                return -1;
            }
            if (initNode < 0) {
                return HOPMirStmtLowerAppendInst(
                    c, HOPMirOp_LOCAL_ZERO, 0, slot, s->start, s->end, NULL);
            }
            if (HOPMirStmtLowerExpr(c, initNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return HOPMirStmtLowerAppendInst(
                c, HOPMirOp_LOCAL_STORE, 0, slot, s->start, s->end, NULL);
        }
        default: c->supported = 0; return 0;
    }
}

int HOPMirLowerAppendSimpleFunctionWithOptions(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    const HOPMirLowerOptions* _Nullable options,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    HOPMirStmtLower c;
    HOPMirFunction  fn = { 0 };
    HOPMirSourceRef sourceRef = { 0 };
    HOPMirTypeRef   typeRef = { 0 };
    uint32_t        sourceIndex = 0;
    int32_t         returnTypeNode = -1;
    int32_t         child;
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (builder == NULL || outFunctionIndex == NULL || outSupported == NULL || arena == NULL
        || ast == NULL)
    {
        HOPMirLowerStmtSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outFunctionIndex = UINT32_MAX;
    *outSupported = 0;
    if (bodyNode < 0 || (uint32_t)bodyNode >= ast->len || ast->nodes[bodyNode].kind != HOPAst_BLOCK)
    {
        return 0;
    }
    memset(&c, 0, sizeof(c));
    c.arena = arena;
    c.ast = ast;
    c.src = src;
    c.supported = 1;
    c.diag = diag;
    c.lowerConstExpr = options != NULL ? options->lowerConstExpr : NULL;
    c.lowerConstExprCtx = options != NULL ? options->lowerConstExprCtx : NULL;
    c.builder = *builder;
    c.functionReturnTypeNode = -1;
    sourceRef.src = src;
    if (HOPMirProgramBuilderAddSource(&c.builder, &sourceRef, &sourceIndex) != 0) {
        HOPMirLowerStmtSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    c.sourceIndex = sourceIndex;

    fn.nameStart = fnNode >= 0 && (uint32_t)fnNode < ast->len ? ast->nodes[fnNode].dataStart : 0;
    fn.nameEnd = fnNode >= 0 && (uint32_t)fnNode < ast->len ? ast->nodes[fnNode].dataEnd : 0;
    fn.sourceRef = sourceIndex;
    fn.typeRef = UINT32_MAX;
    returnTypeNode = HOPMirStmtLowerFunctionReturnTypeNode(ast, fnNode);
    c.functionReturnTypeNode = returnTypeNode;
    if (returnTypeNode >= 0) {
        typeRef.astNode = (uint32_t)returnTypeNode;
        typeRef.sourceRef = sourceIndex;
        typeRef.flags = 0;
        typeRef.aux = 0;
        if (HOPMirProgramBuilderAddType(&c.builder, &typeRef, &fn.typeRef) != 0) {
            HOPMirLowerStmtSetDiag(
                diag,
                HOPDiag_ARENA_OOM,
                ast->nodes[returnTypeNode].start,
                ast->nodes[returnTypeNode].end);
            return -1;
        }
    }
    if (HOPMirProgramBuilderBeginFunction(&c.builder, &fn, &c.functionIndex) != 0) {
        HOPMirLowerStmtSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    if (fnNode >= 0 && (uint32_t)fnNode < ast->len) {
        child = ast->nodes[fnNode].firstChild;
        while (child >= 0) {
            if (ast->nodes[child].kind == HOPAst_PARAM) {
                int32_t  paramTypeNode = ast->nodes[child].firstChild;
                uint32_t slot = 0;
                if (HOPMirStmtLowerPushLocal(
                        &c,
                        ast->nodes[child].dataStart,
                        ast->nodes[child].dataEnd,
                        1,
                        1,
                        0,
                        paramTypeNode,
                        -1,
                        &slot)
                    != 0)
                {
                    return -1;
                }
                c.builder.funcs[c.functionIndex].paramCount++;
                if ((ast->nodes[child].flags & HOPAstFlag_PARAM_VARIADIC) != 0u) {
                    c.builder.funcs[c.functionIndex].flags |= HOPMirFunctionFlag_VARIADIC;
                }
            }
            child = ast->nodes[child].nextSibling;
        }
    }

    if (HOPMirStmtLowerBlock(&c, bodyNode) != 0) {
        return -1;
    }
    if (!c.supported) {
        return 0;
    }
    if (HOPMirStmtLowerAppendInst(
            &c,
            HOPMirOp_RETURN_VOID,
            0,
            0,
            ast->nodes[bodyNode].start,
            ast->nodes[bodyNode].end,
            NULL)
        != 0)
    {
        return -1;
    }
    if (HOPMirProgramBuilderEndFunction(&c.builder) != 0) {
        HOPMirLowerStmtSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *builder = c.builder;
    *outFunctionIndex = c.functionIndex;
    *outSupported = 1;
    return 0;
}

int HOPMirLowerAppendSimpleFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    return HOPMirLowerAppendSimpleFunctionWithOptions(
        builder, arena, ast, src, fnNode, bodyNode, NULL, outFunctionIndex, outSupported, diag);
}

int HOPMirLowerSimpleFunctionWithOptions(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    const HOPMirLowerOptions* _Nullable options,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    HOPMirProgramBuilder builder;
    uint32_t             functionIndex = UINT32_MAX;
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (outProgram == NULL || outSupported == NULL || arena == NULL || ast == NULL) {
        HOPMirLowerStmtSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outProgram = (HOPMirProgram){ 0 };
    HOPMirProgramBuilderInit(&builder, arena);
    if (HOPMirLowerAppendSimpleFunctionWithOptions(
            &builder,
            arena,
            ast,
            src,
            fnNode,
            bodyNode,
            options,
            &functionIndex,
            outSupported,
            diag)
        != 0)
    {
        return -1;
    }
    if (!*outSupported) {
        return 0;
    }
    HOPMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

int HOPMirLowerSimpleFunction(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    fnNode,
    int32_t    bodyNode,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    return HOPMirLowerSimpleFunctionWithOptions(
        arena, ast, src, fnNode, bodyNode, NULL, outProgram, outSupported, diag);
}

HOP_API_END
