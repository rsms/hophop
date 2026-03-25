#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_lower_stmt.h"

SL_API_BEGIN

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t slot;
    int32_t  typeNode;
    int32_t  initExprNode;
    uint8_t  mutable;
    uint8_t  _reserved[2];
} SLMirLowerLocal;

typedef struct {
    uint32_t breakJumpStart;
    uint32_t continueJumpStart;
    uint32_t continueTargetPc;
    uint32_t blockDepth;
    uint8_t  hasContinue;
    uint8_t  _reserved[3];
} SLMirLowerControl;

typedef struct {
    uint32_t deferStart;
} SLMirLowerBlockScope;

typedef struct {
    uint32_t atIndex;
    uint32_t slot;
    uint32_t start;
    uint32_t end;
    uint32_t next;
} SLMirChunkInsert;

typedef struct {
    SLArena*              arena;
    const SLAst*          ast;
    SLStrView             src;
    SLMirProgramBuilder   builder;
    uint32_t              functionIndex;
    int32_t               functionReturnTypeNode;
    SLMirLowerLocal*      locals;
    uint32_t              localLen;
    uint32_t              localCap;
    uint32_t              breakJumps[256];
    uint32_t              breakJumpLen;
    uint32_t              continueJumps[256];
    uint32_t              continueJumpLen;
    SLMirLowerControl     controls[32];
    uint32_t              controlLen;
    int32_t               deferredStmtNodes[256];
    uint32_t              deferredStmtLen;
    SLMirLowerBlockScope  blockScopes[64];
    uint32_t              blockDepth;
    uint8_t               loweringDeferred;
    uint8_t               _reserved2[3];
    int                   supported;
    SLDiag*               diag;
    SLMirLowerConstExprFn lowerConstExpr;
    void*                 lowerConstExprCtx;
} SLMirStmtLower;

static void SLMirLowerStmtSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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

static void SLMirLowerStmtSetUnsupportedDetail(
    SLMirStmtLower* c, uint32_t start, uint32_t end, const char* reason) {
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

static int SLMirStmtLowerIsTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE || kind == SLAst_TYPE_ANON_STRUCT
        || kind == SLAst_TYPE_ANON_UNION;
}

static int32_t SLMirStmtLowerFunctionReturnTypeNode(const SLAst* ast, int32_t fnNode) {
    int32_t child;
    if (ast == NULL || fnNode < 0 || (uint32_t)fnNode >= ast->len) {
        return -1;
    }
    child = ast->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (SLMirStmtLowerIsTypeNodeKind(ast->nodes[child].kind)) {
            return child;
        }
        if (ast->nodes[child].kind == SLAst_BLOCK) {
            break;
        }
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static uint32_t SLMirStmtLowerAstListCount(const SLAst* ast, int32_t listNode) {
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

static int32_t SLMirStmtLowerAstListItemAt(const SLAst* ast, int32_t listNode, uint32_t index) {
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

static uint32_t SLMirStmtLowerFnPc(const SLMirStmtLower* c) {
    return c->builder.instLen - c->builder.funcs[c->functionIndex].instStart;
}

static int SLMirStmtLowerEnsureLocalCap(SLMirStmtLower* c, uint32_t needLen) {
    uint32_t         newCap;
    SLMirLowerLocal* newLocals;
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
    newLocals = (SLMirLowerLocal*)SLArenaAlloc(
        c->arena, sizeof(SLMirLowerLocal) * newCap, (uint32_t)_Alignof(SLMirLowerLocal));
    if (newLocals == NULL) {
        SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }
    if (c->locals != NULL && c->localLen > 0) {
        memcpy(newLocals, c->locals, sizeof(SLMirLowerLocal) * c->localLen);
    }
    c->locals = newLocals;
    c->localCap = newCap;
    return 0;
}

static int SLMirStmtLowerPushLocal(
    SLMirStmtLower* c,
    uint32_t        nameStart,
    uint32_t        nameEnd,
    int             mutable,
    int             isParam,
    int             zeroInit,
    int32_t         typeNode,
    int32_t         initExprNode,
    uint32_t*       outSlot) {
    SLMirLocal   local = { 0 };
    SLMirTypeRef typeRef = { 0 };
    uint32_t     slot = 0;
    if (SLMirStmtLowerEnsureLocalCap(c, c->localLen + 1u) != 0) {
        return -1;
    }
    local.typeRef = UINT32_MAX;
    local.flags = mutable ? SLMirLocalFlag_MUTABLE : SLMirLocalFlag_NONE;
    local.nameStart = nameStart;
    local.nameEnd = nameEnd;
    if (isParam) {
        local.flags |= SLMirLocalFlag_PARAM;
    }
    if (zeroInit) {
        local.flags |= SLMirLocalFlag_ZERO_INIT;
    }
    if (typeNode >= 0) {
        typeRef.astNode = (uint32_t)typeNode;
        typeRef.flags = 0;
        if (SLMirProgramBuilderAddType(&c->builder, &typeRef, &local.typeRef) != 0) {
            SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, nameStart, nameEnd);
            return -1;
        }
    }
    if (SLMirProgramBuilderAddLocal(&c->builder, &local, &slot) != 0) {
        SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, nameStart, nameEnd);
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

static int SLMirStmtLowerFindLocal(
    const SLMirStmtLower* c,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    uint32_t*             outSlot,
    int* _Nullable outMutable) {
    uint32_t nameLen;
    uint32_t i = c->localLen;
    if (c == NULL || nameEnd < nameStart || nameEnd > c->src.len) {
        return 0;
    }
    nameLen = nameEnd - nameStart;
    while (i > 0) {
        const SLMirLowerLocal* local;
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

static int SLMirStmtLowerNameEqLiteral(
    const SLMirStmtLower* c, uint32_t start, uint32_t end, const char* lit);

static const SLMirLowerLocal* _Nullable SLMirStmtLowerFindLocalRef(
    const SLMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t nameLen;
    uint32_t i;
    if (c == NULL || nameEnd < nameStart || nameEnd > c->src.len) {
        return NULL;
    }
    nameLen = nameEnd - nameStart;
    i = c->localLen;
    while (i > 0) {
        const SLMirLowerLocal* local;
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

static int SLMirStmtLowerBuiltinTypeSize(
    const SLMirStmtLower* c, int32_t typeNode, int64_t* outSize) {
    const SLAstNode* type;
    if (c == NULL || outSize == NULL || typeNode < 0 || (uint32_t)typeNode >= c->ast->len) {
        return 0;
    }
    type = &c->ast->nodes[typeNode];
    switch (type->kind) {
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
        case SLAst_TYPE_FN:       *outSize = (int64_t)sizeof(void*); return 1;
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case SLAst_TYPE_NAME:     {
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

static int SLMirStmtLowerCastNeedsCoerce(const SLMirStmtLower* c, int32_t typeNode) {
    const SLAstNode* type;
    int32_t          childNode;
    if (c == NULL || typeNode < 0 || (uint32_t)typeNode >= c->ast->len) {
        return 0;
    }
    type = &c->ast->nodes[typeNode];
    if ((type->kind == SLAst_TYPE_REF || type->kind == SLAst_TYPE_PTR) && type->firstChild >= 0
        && (uint32_t)type->firstChild < c->ast->len)
    {
        childNode = type->firstChild;
        if (c->ast->nodes[childNode].kind == SLAst_TYPE_NAME
            && SLMirStmtLowerNameEqLiteral(
                c, c->ast->nodes[childNode].dataStart, c->ast->nodes[childNode].dataEnd, "str"))
        {
            return 0;
        }
    }
    if (type->kind != SLAst_TYPE_NAME) {
        return 1;
    }
    if (SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "bool")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "f32")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "f64")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u8")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u16")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u32")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "u64")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "uint")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i8")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i16")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i32")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "i64")
        || SLMirStmtLowerNameEqLiteral(c, type->dataStart, type->dataEnd, "int"))
    {
        return 0;
    }
    return 1;
}

static int SLMirStmtLowerInferInitExprSize(
    const SLMirStmtLower* c, int32_t exprNode, int64_t* outSize) {
    const SLAstNode* expr;
    if (c == NULL || outSize == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    switch (expr->kind) {
        case SLAst_INT:    *outSize = (int64_t)sizeof(uintptr_t); return 1;
        case SLAst_FLOAT:  *outSize = 8; return 1;
        case SLAst_BOOL:   *outSize = 1; return 1;
        case SLAst_STRING: *outSize = (int64_t)(sizeof(void*) * 2u); return 1;
        case SLAst_CAST:   {
            int32_t valueNode = expr->firstChild;
            int32_t typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode < 0 || typeNode < 0 || c->ast->nodes[typeNode].nextSibling >= 0) {
                return 0;
            }
            return SLMirStmtLowerBuiltinTypeSize(c, typeNode, outSize);
        }
        default: return 0;
    }
}

static int SLMirStmtLowerTryConstSizeofExpr(
    const SLMirStmtLower* c, int32_t exprNode, int64_t* outSize) {
    const SLAstNode* expr;
    int32_t          innerNode;
    if (c == NULL || outSize == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    if (expr->kind != SLAst_SIZEOF) {
        return 0;
    }
    innerNode = expr->firstChild;
    if (innerNode < 0 || (uint32_t)innerNode >= c->ast->len) {
        return 0;
    }
    if (expr->flags == 1u) {
        if (SLMirStmtLowerBuiltinTypeSize(c, innerNode, outSize)) {
            return 1;
        }
        if (c->ast->nodes[innerNode].kind == SLAst_TYPE_NAME
            || c->ast->nodes[innerNode].kind == SLAst_IDENT)
        {
            const SLMirLowerLocal* local = SLMirStmtLowerFindLocalRef(
                c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
            if (local != NULL) {
                if (local->typeNode >= 0
                    && SLMirStmtLowerBuiltinTypeSize(c, local->typeNode, outSize))
                {
                    return 1;
                }
                if (local->initExprNode >= 0
                    && SLMirStmtLowerInferInitExprSize(c, local->initExprNode, outSize))
                {
                    return 1;
                }
            }
        }
        return 0;
    }
    if (c->ast->nodes[innerNode].kind == SLAst_IDENT) {
        const SLMirLowerLocal* local = SLMirStmtLowerFindLocalRef(
            c, c->ast->nodes[innerNode].dataStart, c->ast->nodes[innerNode].dataEnd);
        if (local == NULL) {
            return 0;
        }
        if (local->typeNode >= 0 && SLMirStmtLowerBuiltinTypeSize(c, local->typeNode, outSize)) {
            return 1;
        }
        if (local->initExprNode >= 0
            && SLMirStmtLowerInferInitExprSize(c, local->initExprNode, outSize))
        {
            return 1;
        }
        return 0;
    }
    return SLMirStmtLowerInferInitExprSize(c, innerNode, outSize);
}

static int SLMirStmtLowerAppendInst(
    SLMirStmtLower* c,
    SLMirOp         op,
    uint16_t        tok,
    uint32_t        aux,
    uint32_t        start,
    uint32_t        end,
    uint32_t* _Nullable outInstIndex) {
    SLMirInst inst;
    inst.op = op;
    inst.tok = tok;
    inst._reserved = 0;
    inst.aux = aux;
    inst.start = start;
    inst.end = end;
    if (SLMirProgramBuilderAppendInst(&c->builder, &inst) != 0) {
        SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }
    if (outInstIndex != NULL) {
        *outInstIndex = c->builder.instLen - 1u;
    }
    return 0;
}

static int SLMirStmtLowerAddFieldRef(
    SLMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd, uint32_t* outIndex) {
    SLMirField field = { 0 };
    uint32_t   i;
    if (outIndex != NULL) {
        *outIndex = UINT32_MAX;
    }
    if (c == NULL || outIndex == NULL || nameEnd < nameStart || nameEnd > c->src.len) {
        return -1;
    }
    for (i = 0; i < c->builder.fieldLen; i++) {
        const SLMirField* existing = &c->builder.fields[i];
        if (existing->nameStart == nameStart && existing->nameEnd == nameEnd
            && existing->ownerTypeRef == UINT32_MAX && existing->typeRef == UINT32_MAX)
        {
            *outIndex = i;
            return 0;
        }
    }
    field.nameStart = nameStart;
    field.nameEnd = nameEnd;
    field.ownerTypeRef = UINT32_MAX;
    field.typeRef = UINT32_MAX;
    if (SLMirProgramBuilderAddField(&c->builder, &field, outIndex) != 0) {
        SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, nameStart, nameEnd);
        return -1;
    }
    return 0;
}

static int SLMirStmtLowerAppendLoadValueBySlice(
    SLMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd, uint32_t start, uint32_t end) {
    uint32_t  slot = 0;
    SLMirInst inst = { 0 };
    if (c == NULL) {
        return -1;
    }
    if (SLMirStmtLowerFindLocal(c, nameStart, nameEnd, &slot, NULL)) {
        return SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_LOAD, 0, slot, start, end, NULL);
    }
    inst.op = SLMirOp_LOAD_IDENT;
    inst.tok = SLTok_IDENT;
    inst.aux = 0u;
    inst.start = nameStart;
    inst.end = nameEnd;
    return SLMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag);
}

static int SLMirStmtLowerAppendStoreValueBySlice(
    SLMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd) {
    SLMirInst inst = { 0 };
    if (c == NULL) {
        return -1;
    }
    inst.op = SLMirOp_STORE_IDENT;
    inst.tok = SLTok_IDENT;
    inst.aux = 0u;
    inst.start = nameStart;
    inst.end = nameEnd;
    return SLMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag);
}

static int SLMirStmtLowerExpr(SLMirStmtLower* c, int32_t exprNode);

static int SLMirStmtLowerNameEqLiteral(
    const SLMirStmtLower* c, uint32_t start, uint32_t end, const char* lit) {
    size_t litLen = 0;
    if (c == NULL || lit == NULL || end < start || end > c->src.len) {
        return 0;
    }
    while (lit[litLen] != '\0') {
        litLen++;
    }
    return (size_t)(end - start) == litLen && memcmp(c->src.ptr + start, lit, litLen) == 0;
}

static int SLMirStmtLowerNameEqLiteralOrPkgBuiltin(
    const SLMirStmtLower* c, uint32_t start, uint32_t end, const char* lit, const char* pkgPrefix) {
    size_t litLen = 0;
    size_t pkgLen = 0;
    size_t i;
    if (SLMirStmtLowerNameEqLiteral(c, start, end, lit)) {
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

static int SLMirStmtLowerNameIsCompilerDiagBuiltin(
    const SLMirStmtLower* c, uint32_t start, uint32_t end) {
    return SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "error", "compiler")
        || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "error_at", "compiler")
        || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "warn", "compiler")
        || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "warn_at", "compiler");
}

static int SLMirStmtLowerNameIsLazyTypeBuiltin(
    const SLMirStmtLower* c, uint32_t start, uint32_t end) {
    return SLMirStmtLowerNameEqLiteral(c, start, end, "typeof")
        || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "kind", "reflect")
        || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "base", "reflect")
        || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "is_alias", "reflect")
        || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(c, start, end, "type_name", "reflect")
        || SLMirStmtLowerNameEqLiteral(c, start, end, "ptr")
        || SLMirStmtLowerNameEqLiteral(c, start, end, "slice")
        || SLMirStmtLowerNameEqLiteral(c, start, end, "array");
}

static int SLMirStmtLowerCallUsesLazyBuiltin(const SLMirStmtLower* c, int32_t callNode) {
    const SLAstNode* call;
    const SLAstNode* callee;
    int32_t          calleeNode;
    int32_t          recvNode;
    if (c == NULL || c->ast == NULL || callNode < 0 || (uint32_t)callNode >= c->ast->len) {
        return 0;
    }
    call = &c->ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &c->ast->nodes[calleeNode] : NULL;
    if (call->kind != SLAst_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == SLAst_IDENT) {
        return SLMirStmtLowerNameEqLiteralOrPkgBuiltin(
                   c, callee->dataStart, callee->dataEnd, "span_of", "reflect")
            || SLMirStmtLowerNameIsLazyTypeBuiltin(c, callee->dataStart, callee->dataEnd)
            || SLMirStmtLowerNameIsCompilerDiagBuiltin(c, callee->dataStart, callee->dataEnd);
    }
    if (callee->kind != SLAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= c->ast->len
        || c->ast->nodes[recvNode].kind != SLAst_IDENT)
    {
        return 0;
    }
    if (SLMirStmtLowerNameEqLiteral(
            c, c->ast->nodes[recvNode].dataStart, c->ast->nodes[recvNode].dataEnd, "reflect")
        && (SLMirStmtLowerNameEqLiteral(c, callee->dataStart, callee->dataEnd, "span_of")
            || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "kind", "reflect")
            || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "base", "reflect")
            || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "is_alias", "reflect")
            || SLMirStmtLowerNameEqLiteralOrPkgBuiltin(
                c, callee->dataStart, callee->dataEnd, "type_name", "reflect")))
    {
        return 1;
    }
    if (!SLMirStmtLowerNameEqLiteral(
            c, c->ast->nodes[recvNode].dataStart, c->ast->nodes[recvNode].dataEnd, "compiler"))
    {
        return 0;
    }
    return SLMirStmtLowerNameIsCompilerDiagBuiltin(c, callee->dataStart, callee->dataEnd);
}

static int SLMirStmtLowerCallCanUseManualLowering(SLMirStmtLower* c, int32_t callNode) {
    int32_t calleeNode;
    if (c == NULL || callNode < 0 || (uint32_t)callNode >= c->ast->len
        || c->ast->nodes[callNode].kind != SLAst_CALL
        || SLMirStmtLowerCallUsesLazyBuiltin(c, callNode))
    {
        return 0;
    }
    calleeNode = c->ast->nodes[callNode].firstChild;
    if (calleeNode < 0 || (uint32_t)calleeNode >= c->ast->len) {
        return 0;
    }
    return 1;
}

static int SLMirStmtLowerCallExpr(SLMirStmtLower* c, int32_t exprNode) {
    const SLAstNode* call;
    int32_t          calleeNode;
    int32_t          argNode;
    uint32_t         argc = 0;
    uint32_t         callFlags = 0;
    uint16_t         callTokFlags = 0;
    uint32_t         callStart = 0;
    uint32_t         callEnd = 0;
    uint32_t         localSlot = 0;
    int              isBuiltinLen = 0;
    int              isBuiltinCStr = 0;
    int              isIndirectLocalCall = 0;
    SLMirInst        inst = { 0 };
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        if (c != NULL) {
            c->supported = 0;
        }
        return 0;
    }
    call = &c->ast->nodes[exprNode];
    calleeNode = call->firstChild;
    if (call->kind != SLAst_CALL || calleeNode < 0 || (uint32_t)calleeNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    if (c->ast->nodes[calleeNode].kind == SLAst_IDENT) {
        if (SLMirStmtLowerFindLocal(
                c,
                c->ast->nodes[calleeNode].dataStart,
                c->ast->nodes[calleeNode].dataEnd,
                &localSlot,
                NULL))
        {
            if (SLMirStmtLowerAppendInst(
                    c,
                    SLMirOp_LOCAL_LOAD,
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
            isBuiltinLen = SLMirStmtLowerNameEqLiteral(c, callStart, callEnd, "len");
            isBuiltinCStr = SLMirStmtLowerNameEqLiteral(c, callStart, callEnd, "cstr");
        }
    } else if (c->ast->nodes[calleeNode].kind == SLAst_FIELD_EXPR) {
        int32_t baseNode = c->ast->nodes[calleeNode].firstChild;
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        argc = 1u;
        callFlags = SLMirSymbolFlag_CALL_RECEIVER_ARG0;
        callStart = c->ast->nodes[calleeNode].dataStart;
        callEnd = c->ast->nodes[calleeNode].dataEnd;
        isBuiltinCStr = SLMirStmtLowerNameEqLiteral(c, callStart, callEnd, "cstr");
    } else {
        c->supported = 0;
        return 0;
    }
    argNode = c->ast->nodes[calleeNode].nextSibling;
    while (argNode >= 0) {
        int32_t valueNode = argNode;
        int     isSpread = 0;
        if (c->ast->nodes[argNode].kind == SLAst_CALL_ARG) {
            valueNode = c->ast->nodes[argNode].firstChild;
            if (valueNode < 0) {
                c->supported = 0;
                return 0;
            }
            isSpread = (c->ast->nodes[argNode].flags & SLAstFlag_CALL_ARG_SPREAD) != 0u;
        }
        if (isSpread) {
            if (c->ast->nodes[argNode].nextSibling >= 0
                || (callTokFlags & SLMirCallArgFlag_SPREAD_LAST) != 0u)
            {
                c->supported = 0;
                return 0;
            }
            callTokFlags |= SLMirCallArgFlag_SPREAD_LAST;
        }
        if (SLMirStmtLowerExpr(c, valueNode) != 0 || !c->supported) {
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
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_SEQ_LEN, SLTok_INVALID, 0, callStart, callEnd, NULL);
    }
    if (isBuiltinCStr && argc == 1u) {
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_STR_CSTR, SLTok_INVALID, 0, callStart, callEnd, NULL);
    }
    inst.op = isIndirectLocalCall ? SLMirOp_CALL_INDIRECT : SLMirOp_CALL;
    inst.tok = (uint16_t)argc | callTokFlags;
    inst.aux = isIndirectLocalCall ? 0u : SLMirRawCallAuxPack((uint32_t)exprNode, callFlags);
    inst.start = callStart;
    inst.end = callEnd;
    return SLMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag);
}

static int SLMirStmtLowerAppendIntConst(
    SLMirStmtLower* c, int64_t value, uint32_t start, uint32_t end) {
    SLMirConst valueConst = { 0 };
    uint32_t   constIndex = 0;
    if (c == NULL) {
        return -1;
    }
    valueConst.kind = SLMirConst_INT;
    valueConst.bits = (uint64_t)value;
    if (SLMirProgramBuilderAddConst(&c->builder, &valueConst, &constIndex) != 0) {
        SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }
    return SLMirStmtLowerAppendInst(c, SLMirOp_PUSH_CONST, 0, constIndex, start, end, NULL);
}

static int SLMirStmtLowerAppendConstValue(
    SLMirStmtLower* c, const SLMirConst* value, uint32_t start, uint32_t end) {
    uint32_t constIndex = 0;
    if (c == NULL || value == NULL) {
        return -1;
    }
    if (SLMirProgramBuilderAddConst(&c->builder, value, &constIndex) != 0) {
        SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }
    return SLMirStmtLowerAppendInst(c, SLMirOp_PUSH_CONST, 0, constIndex, start, end, NULL);
}

static int SLMirStmtLowerAppendTupleMake(
    SLMirStmtLower* c, uint32_t elemCount, int32_t typeNodeHint, uint32_t start, uint32_t end) {
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
    return SLMirStmtLowerAppendInst(
        c, SLMirOp_TUPLE_MAKE, (uint16_t)elemCount, aux, start, end, NULL);
}

static int SLMirStmtLowerRangeHasChar(SLStrView src, uint32_t start, uint32_t end, char ch) {
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

static int SLMirStmtLowerFindCharForward(
    SLStrView src, uint32_t start, uint32_t end, char ch, uint32_t* outPos) {
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

static SLMirLowerControl* _Nullable SLMirStmtLowerCurrentBreakable(SLMirStmtLower* c) {
    if (c == NULL || c->controlLen == 0) {
        return NULL;
    }
    return &c->controls[c->controlLen - 1u];
}

static SLMirLowerControl* _Nullable SLMirStmtLowerCurrentContinuable(SLMirStmtLower* c) {
    uint32_t i;
    if (c == NULL || c->controlLen == 0) {
        return NULL;
    }
    i = c->controlLen;
    while (i > 0) {
        SLMirLowerControl* control;
        i--;
        control = &c->controls[i];
        if (control->hasContinue) {
            return control;
        }
    }
    return NULL;
}

static int SLMirStmtLowerPushControl(
    SLMirStmtLower* c, int hasContinue, uint32_t continueTargetPc) {
    if (c == NULL || c->controlLen >= (uint32_t)(sizeof(c->controls) / sizeof(c->controls[0]))) {
        if (c != NULL) {
            c->supported = 0;
        }
        return 0;
    }
    c->controls[c->controlLen++] = (SLMirLowerControl){
        .breakJumpStart = c->breakJumpLen,
        .continueJumpStart = c->continueJumpLen,
        .continueTargetPc = continueTargetPc,
        .blockDepth = c->blockDepth,
        .hasContinue = hasContinue ? 1u : 0u,
    };
    return 1;
}

static int SLMirStmtLowerRecordControlJump(SLMirStmtLower* c, int isContinue, uint32_t instIndex) {
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

static void SLMirStmtLowerPatchJumpRange(
    SLMirStmtLower* c, const uint32_t* jumps, uint32_t start, uint32_t end, uint32_t targetPc) {
    uint32_t i;
    if (c == NULL || jumps == NULL) {
        return;
    }
    for (i = start; i < end; i++) {
        c->builder.insts[jumps[i]].aux = targetPc;
    }
}

static void SLMirStmtLowerFinishControl(
    SLMirStmtLower* c, uint32_t continueTargetPc, uint32_t breakTargetPc) {
    SLMirLowerControl control;
    if (c == NULL || c->controlLen == 0) {
        return;
    }
    control = c->controls[c->controlLen - 1u];
    if (control.hasContinue) {
        SLMirStmtLowerPatchJumpRange(
            c, c->continueJumps, control.continueJumpStart, c->continueJumpLen, continueTargetPc);
        c->continueJumpLen = control.continueJumpStart;
    }
    SLMirStmtLowerPatchJumpRange(
        c, c->breakJumps, control.breakJumpStart, c->breakJumpLen, breakTargetPc);
    c->breakJumpLen = control.breakJumpStart;
    c->controlLen--;
}

static int SLMirStmtLowerExprInstStackDelta(const SLMirInst* inst, int32_t* outDelta) {
    uint32_t elemCount = 0;
    if (inst == NULL || outDelta == NULL) {
        return 0;
    }
    switch (inst->op) {
        case SLMirOp_PUSH_CONST:
        case SLMirOp_PUSH_INT:
        case SLMirOp_PUSH_FLOAT:
        case SLMirOp_PUSH_BOOL:
        case SLMirOp_PUSH_STRING:
        case SLMirOp_PUSH_NULL:
        case SLMirOp_LOAD_IDENT:      *outDelta = 1; return 1;
        case SLMirOp_UNARY:
        case SLMirOp_CAST:
        case SLMirOp_SEQ_LEN:
        case SLMirOp_STR_CSTR:
        case SLMirOp_OPTIONAL_WRAP:
        case SLMirOp_OPTIONAL_UNWRAP: *outDelta = 0; return 1;
        case SLMirOp_BINARY:
        case SLMirOp_INDEX:           *outDelta = -1; return 1;
        case SLMirOp_SLICE_MAKE:
            *outDelta = 0 - (((inst->tok & SLAstFlag_INDEX_HAS_START) != 0u) ? 1 : 0)
                      - (((inst->tok & SLAstFlag_INDEX_HAS_END) != 0u) ? 1 : 0);
            return 1;
        case SLMirOp_CALL: *outDelta = 1 - (int32_t)SLMirCallArgCountFromTok(inst->tok); return 1;
        case SLMirOp_TUPLE_MAKE:
            elemCount = (uint32_t)inst->tok;
            *outDelta = 1 - (int32_t)elemCount;
            return 1;
        default: return 0;
    }
}

static int SLMirStmtLowerFindCallArgStart(
    const SLMirChunk* chunk, uint32_t callIndex, uint32_t argCount, uint32_t* outArgStart) {
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
        if (!SLMirStmtLowerExprInstStackDelta(&chunk->v[i], &delta)) {
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

static int SLMirStmtLowerRewriteExprChunk(SLMirStmtLower* c, const SLMirChunk* chunk) {
    SLMirChunkInsert* inserts = NULL;
    uint32_t*         insertHeads = NULL;
    uint32_t          insertLen = 0;
    uint32_t          i;
    uint32_t          chunkLen;
    uint32_t          emitIndex;
    uint32_t          insertIndex;
    uint8_t*          callIndirect = NULL;
    if (chunk == NULL) {
        return -1;
    }
    chunkLen = chunk->len;
    if (chunkLen != 0u) {
        inserts = (SLMirChunkInsert*)SLArenaAlloc(
            c->arena, sizeof(SLMirChunkInsert) * chunkLen, (uint32_t)_Alignof(SLMirChunkInsert));
        insertHeads = (uint32_t*)SLArenaAlloc(
            c->arena, sizeof(uint32_t) * chunkLen, (uint32_t)_Alignof(uint32_t));
        callIndirect = (uint8_t*)SLArenaAlloc(
            c->arena, chunkLen * sizeof(uint8_t), (uint32_t)_Alignof(uint8_t));
        if (inserts == NULL || insertHeads == NULL || callIndirect == NULL) {
            SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, 0, 0);
            return -1;
        }
        memset(callIndirect, 0, chunkLen * sizeof(uint8_t));
        for (i = 0; i < chunkLen; i++) {
            insertHeads[i] = UINT32_MAX;
        }
        for (i = chunkLen; i > 0u; i--) {
            SLMirInst inst = chunk->v[i - 1u];
            uint32_t  slot = 0;
            uint32_t  argStart = UINT32_MAX;
            if (inst.op != SLMirOp_CALL || SLMirCallTokDropsReceiverArg0(inst.tok)
                || !SLMirStmtLowerFindLocal(c, inst.start, inst.end, &slot, NULL))
            {
                continue;
            }
            if (SLMirCallArgCountFromTok(inst.tok) == 0u) {
                argStart = i - 1u;
            } else if (!SLMirStmtLowerFindCallArgStart(
                           chunk, i - 1u, SLMirCallArgCountFromTok(inst.tok), &argStart))
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
            if (SLMirStmtLowerAppendInst(
                    c,
                    SLMirOp_LOCAL_LOAD,
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
        SLMirInst inst = chunk->v[emitIndex];
        if (inst.op == SLMirOp_LOAD_IDENT) {
            uint32_t slot = 0;
            if (SLMirStmtLowerFindLocal(c, inst.start, inst.end, &slot, NULL)) {
                inst.op = SLMirOp_LOCAL_LOAD;
                inst.tok = 0;
                inst.aux = slot;
            }
        }
        if (inst.op == SLMirOp_AGG_GET || inst.op == SLMirOp_AGG_ADDR) {
            uint32_t fieldRef = UINT32_MAX;
            if (SLMirStmtLowerAddFieldRef(c, inst.start, inst.end, &fieldRef) != 0) {
                return -1;
            }
            inst.aux = fieldRef;
        }
        if (callIndirect != NULL && callIndirect[emitIndex] != 0u) {
            inst.op = SLMirOp_CALL_INDIRECT;
            inst.aux = 0u;
        }
        if (SLMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLMirStmtLowerIsReplayableExpr(const SLMirStmtLower* c, int32_t exprNode);

static int SLMirStmtLowerExpr(SLMirStmtLower* c, int32_t exprNode) {
    const SLAstNode* expr;
    SLMirChunk       chunk = { 0 };
    int              supported = 0;
    uint32_t         elemCount = 0;
    uint32_t         i;
    int32_t          lhsNode;
    int32_t          rhsNode;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    {
        int64_t sizeValue = 0;
        if (SLMirStmtLowerTryConstSizeofExpr(c, exprNode, &sizeValue)) {
            return SLMirStmtLowerAppendIntConst(c, sizeValue, expr->start, expr->end);
        }
        if (expr->kind == SLAst_CAST) {
            int32_t valueNode = expr->firstChild;
            int32_t typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode >= 0 && typeNode >= 0 && c->ast->nodes[typeNode].nextSibling < 0
                && SLMirStmtLowerTryConstSizeofExpr(c, valueNode, &sizeValue))
            {
                return SLMirStmtLowerAppendIntConst(c, sizeValue, expr->start, expr->end);
            }
        }
    }
    if (c->lowerConstExpr != NULL && !SLMirStmtLowerCallUsesLazyBuiltin(c, exprNode)) {
        SLMirConst loweredConst = { 0 };
        int lowerRc = c->lowerConstExpr(c->lowerConstExprCtx, exprNode, &loweredConst, c->diag);
        if (lowerRc < 0) {
            return -1;
        }
        if (lowerRc > 0) {
            return SLMirStmtLowerAppendConstValue(c, &loweredConst, expr->start, expr->end);
        }
        if (expr->kind == SLAst_CAST) {
            int32_t valueNode = expr->firstChild;
            int32_t typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
            if (valueNode >= 0 && typeNode >= 0 && c->ast->nodes[typeNode].nextSibling < 0) {
                lowerRc = c->lowerConstExpr(
                    c->lowerConstExprCtx, valueNode, &loweredConst, c->diag);
                if (lowerRc < 0) {
                    return -1;
                }
                if (lowerRc > 0) {
                    return SLMirStmtLowerAppendConstValue(c, &loweredConst, expr->start, expr->end);
                }
            }
        }
    }
    if (expr->kind == SLAst_CAST) {
        int32_t      valueNode = expr->firstChild;
        int32_t      typeNode = valueNode >= 0 ? c->ast->nodes[valueNode].nextSibling : -1;
        int32_t      extraNode = typeNode >= 0 ? c->ast->nodes[typeNode].nextSibling : -1;
        SLMirTypeRef typeRef = { 0 };
        uint32_t     typeRefIndex = UINT32_MAX;
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0
            && SLMirStmtLowerCastNeedsCoerce(c, typeNode))
        {
            if (SLMirStmtLowerExpr(c, valueNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            typeRef.astNode = (uint32_t)typeNode;
            typeRef.flags = 0u;
            if (SLMirProgramBuilderAddType(&c->builder, &typeRef, &typeRefIndex) != 0) {
                SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, expr->start, expr->end);
                return -1;
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_COERCE, 0, typeRefIndex, expr->start, expr->end, NULL);
        }
    }
    if (expr->kind == SLAst_INDEX && (expr->flags & SLAstFlag_INDEX_SLICE) != 0u) {
        int32_t  baseNode = expr->firstChild;
        int32_t  extraNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
        int32_t  startNode = -1;
        int32_t  endNode = -1;
        uint16_t sliceFlags =
            (uint16_t)(expr->flags & (SLAstFlag_INDEX_HAS_START | SLAstFlag_INDEX_HAS_END));
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if ((expr->flags & SLAstFlag_INDEX_HAS_START) != 0u) {
            startNode = extraNode;
            extraNode = startNode >= 0 ? c->ast->nodes[startNode].nextSibling : -1;
            if (startNode < 0 || (uint32_t)startNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, startNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        }
        if ((expr->flags & SLAstFlag_INDEX_HAS_END) != 0u) {
            endNode = extraNode;
            extraNode = endNode >= 0 ? c->ast->nodes[endNode].nextSibling : -1;
            if (endNode < 0 || (uint32_t)endNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, endNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        }
        if (extraNode >= 0) {
            c->supported = 0;
            return 0;
        }
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_SLICE_MAKE, sliceFlags, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == SLAst_INDEX) {
        int32_t baseNode = expr->firstChild;
        int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
        if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        return SLMirStmtLowerAppendInst(c, SLMirOp_INDEX, 0, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == SLAst_NEW) {
        return SLMirStmtLowerAppendInst(
            c,
            SLMirOp_ALLOC_NEW,
            (uint16_t)expr->flags,
            (uint32_t)exprNode,
            expr->start,
            expr->end,
            NULL);
    }
    if (expr->kind == SLAst_CALL_WITH_CONTEXT) {
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_CTX_SET, 0, (uint32_t)exprNode, expr->start, expr->end, NULL);
    }
    if (expr->kind == SLAst_TUPLE_EXPR) {
        elemCount = SLMirStmtLowerAstListCount(c->ast, exprNode);
        if (elemCount == 0u) {
            c->supported = 0;
            return 0;
        }
        for (i = 0; i < elemCount; i++) {
            int32_t itemNode = SLMirStmtLowerAstListItemAt(c->ast, exprNode, i);
            if (itemNode < 0) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, itemNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        }
        return SLMirStmtLowerAppendTupleMake(c, elemCount, exprNode, expr->start, expr->end);
    }
    if (expr->kind == SLAst_UNWRAP) {
        int32_t childNode = expr->firstChild;
        if (childNode < 0 || (uint32_t)childNode >= c->ast->len
            || c->ast->nodes[childNode].nextSibling >= 0)
        {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, childNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_OPTIONAL_UNWRAP, 0, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == SLAst_UNARY) {
        int32_t child = expr->firstChild;
        if (child >= 0 && (uint32_t)child < c->ast->len && c->ast->nodes[child].kind == SLAst_IDENT)
        {
            uint32_t slot = 0;
            if (SLMirStmtLowerFindLocal(
                    c, c->ast->nodes[child].dataStart, c->ast->nodes[child].dataEnd, &slot, NULL))
            {
                if ((SLTokenKind)expr->op == SLTok_AND) {
                    return SLMirStmtLowerAppendInst(
                        c, SLMirOp_LOCAL_ADDR, 0, slot, expr->start, expr->end, NULL);
                }
            }
            if ((SLTokenKind)expr->op == SLTok_AND) {
                if (SLMirStmtLowerAppendLoadValueBySlice(
                        c,
                        c->ast->nodes[child].dataStart,
                        c->ast->nodes[child].dataEnd,
                        c->ast->nodes[child].start,
                        c->ast->nodes[child].end)
                    != 0)
                {
                    return -1;
                }
                return SLMirStmtLowerAppendInst(
                    c, SLMirOp_ADDR_OF, 0, 0, expr->start, expr->end, NULL);
            }
        }
        if ((SLTokenKind)expr->op == SLTok_AND && child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == SLAst_FIELD_EXPR)
        {
            int32_t  baseNode = c->ast->nodes[child].firstChild;
            uint32_t fieldRef = UINT32_MAX;
            if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (SLMirStmtLowerAddFieldRef(
                    c, c->ast->nodes[child].dataStart, c->ast->nodes[child].dataEnd, &fieldRef)
                != 0)
            {
                return -1;
            }
            return SLMirStmtLowerAppendInst(
                c,
                SLMirOp_AGG_ADDR,
                0,
                fieldRef,
                c->ast->nodes[child].dataStart,
                c->ast->nodes[child].dataEnd,
                NULL);
        }
        if ((SLTokenKind)expr->op == SLTok_AND && child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == SLAst_INDEX
            && (c->ast->nodes[child].flags & 0x7u) == 0u)
        {
            int32_t baseNode = c->ast->nodes[child].firstChild;
            int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
            if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (SLMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_ARRAY_ADDR, 0, 0, expr->start, expr->end, NULL);
        }
        if ((SLTokenKind)expr->op == SLTok_AND && child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == SLAst_COMPOUND_LIT)
        {
            if (SLMirStmtLowerExpr(c, child) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(c, SLMirOp_ADDR_OF, 0, 0, expr->start, expr->end, NULL);
        }
        if ((SLTokenKind)expr->op == SLTok_MUL && SLMirStmtLowerIsReplayableExpr(c, child)) {
            if (SLMirStmtLowerExpr(c, child) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_DEREF_LOAD, 0, 0, expr->start, expr->end, NULL);
        }
        if ((SLTokenKind)expr->op != SLTok_AND && (SLTokenKind)expr->op != SLTok_MUL) {
            if (child < 0 || (uint32_t)child >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, child) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_UNARY, (uint16_t)expr->op, 0, expr->start, expr->end, NULL);
        }
    }
    if (expr->kind == SLAst_FIELD_EXPR) {
        int32_t  baseNode = expr->firstChild;
        uint32_t fieldRef = UINT32_MAX;
        uint32_t contextField = SLMirContextField_INVALID;
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (c->ast->nodes[baseNode].kind == SLAst_IDENT
            && c->ast->nodes[baseNode].dataEnd - c->ast->nodes[baseNode].dataStart == 7u
            && memcmp(c->src.ptr + c->ast->nodes[baseNode].dataStart, "context", 7u) == 0)
        {
            uint32_t fieldLen = expr->dataEnd - expr->dataStart;
            if (fieldLen == 3u && memcmp(c->src.ptr + expr->dataStart, "mem", 3u) == 0) {
                contextField = SLMirContextField_MEM;
            } else if (fieldLen == 8u && memcmp(c->src.ptr + expr->dataStart, "temp_mem", 8u) == 0)
            {
                contextField = SLMirContextField_TEMP_MEM;
            } else if (fieldLen == 3u && memcmp(c->src.ptr + expr->dataStart, "log", 3u) == 0) {
                contextField = SLMirContextField_LOG;
            }
            if (contextField != SLMirContextField_INVALID) {
                return SLMirStmtLowerAppendInst(
                    c, SLMirOp_CTX_GET, 0, contextField, expr->start, expr->end, NULL);
            }
        }
        if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerAddFieldRef(c, expr->dataStart, expr->dataEnd, &fieldRef) != 0) {
            return -1;
        }
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_AGG_GET, 0, fieldRef, expr->dataStart, expr->dataEnd, NULL);
    }
    if (expr->kind == SLAst_BINARY) {
        lhsNode = expr->firstChild;
        rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
        if (lhsNode < 0 || rhsNode < 0 || c->ast->nodes[rhsNode].nextSibling >= 0
            || (SLTokenKind)expr->op == SLTok_ASSIGN || (SLTokenKind)expr->op == SLTok_ADD_ASSIGN
            || (SLTokenKind)expr->op == SLTok_SUB_ASSIGN
            || (SLTokenKind)expr->op == SLTok_MUL_ASSIGN
            || (SLTokenKind)expr->op == SLTok_DIV_ASSIGN
            || (SLTokenKind)expr->op == SLTok_MOD_ASSIGN
            || (SLTokenKind)expr->op == SLTok_AND_ASSIGN || (SLTokenKind)expr->op == SLTok_OR_ASSIGN
            || (SLTokenKind)expr->op == SLTok_XOR_ASSIGN
            || (SLTokenKind)expr->op == SLTok_LSHIFT_ASSIGN
            || (SLTokenKind)expr->op == SLTok_RSHIFT_ASSIGN)
        {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, lhsNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_BINARY, (uint16_t)expr->op, 0, expr->start, expr->end, NULL);
    }
    if (expr->kind == SLAst_COMPOUND_LIT) {
        int32_t      child = expr->firstChild;
        int32_t      typeNode = -1;
        SLMirTypeRef typeRef = { 0 };
        uint32_t     typeRefIndex = UINT32_MAX;
        uint32_t     fieldCount = 0;
        if (child >= 0 && (uint32_t)child < c->ast->len
            && SLMirStmtLowerIsTypeNodeKind(c->ast->nodes[child].kind))
        {
            typeNode = child;
            child = c->ast->nodes[child].nextSibling;
        }
        if (typeNode < 0) {
            int32_t scan = child;
            while (scan >= 0) {
                if (c->ast->nodes[scan].kind != SLAst_COMPOUND_FIELD) {
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
            if (SLMirStmtLowerAppendInst(
                    c,
                    SLMirOp_AGG_MAKE,
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
            typeRef.flags = 0u;
            if (SLMirProgramBuilderAddType(&c->builder, &typeRef, &typeRefIndex) != 0) {
                SLMirLowerStmtSetDiag(c->diag, SLDiag_ARENA_OOM, expr->start, expr->end);
                return -1;
            }
            if (SLMirStmtLowerAppendInst(
                    c, SLMirOp_AGG_ZERO, 0, typeRefIndex, expr->start, expr->end, NULL)
                != 0)
            {
                return -1;
            }
        }
        while (child >= 0) {
            const SLAstNode* field = &c->ast->nodes[child];
            int32_t          valueNode = c->ast->nodes[child].firstChild;
            uint32_t         fieldRef = UINT32_MAX;
            if (field->kind != SLAst_COMPOUND_FIELD) {
                c->supported = 0;
                return 0;
            }
            if (valueNode >= 0) {
                if (SLMirStmtLowerExpr(c, valueNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            } else if ((field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
                if (SLMirStmtLowerAppendLoadValueBySlice(
                        c, field->dataStart, field->dataEnd, field->start, field->end)
                    != 0)
                {
                    return -1;
                }
            } else {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerAddFieldRef(c, field->dataStart, field->dataEnd, &fieldRef) != 0) {
                return -1;
            }
            if (SLMirStmtLowerAppendInst(
                    c, SLMirOp_AGG_SET, 0, fieldRef, field->dataStart, field->dataEnd, NULL)
                != 0)
            {
                return -1;
            }
            child = c->ast->nodes[child].nextSibling;
        }
        if (typeNode >= 0) {
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_COERCE, 0, typeRefIndex, expr->start, expr->end, NULL);
        }
        return 0;
    }
    if (expr->kind == SLAst_CALL && SLMirStmtLowerCallCanUseManualLowering(c, exprNode)) {
        return SLMirStmtLowerCallExpr(c, exprNode);
    }
    if (SLMirBuildExpr(c->arena, c->ast, c->src, exprNode, &chunk, &supported, c->diag) != 0) {
        return -1;
    }
    if (!supported || chunk.len == 0 || chunk.v[chunk.len - 1].op != SLMirOp_RETURN) {
        c->supported = 0;
        return 0;
    }
    chunk.len--;
    return SLMirStmtLowerRewriteExprChunk(c, &chunk);
}

static int SLMirStmtLowerBinaryOpForAssign(SLTokenKind tok, SLTokenKind* outTok) {
    switch (tok) {
        case SLTok_ADD_ASSIGN:    *outTok = SLTok_ADD; return 1;
        case SLTok_SUB_ASSIGN:    *outTok = SLTok_SUB; return 1;
        case SLTok_MUL_ASSIGN:    *outTok = SLTok_MUL; return 1;
        case SLTok_DIV_ASSIGN:    *outTok = SLTok_DIV; return 1;
        case SLTok_MOD_ASSIGN:    *outTok = SLTok_MOD; return 1;
        case SLTok_AND_ASSIGN:    *outTok = SLTok_AND; return 1;
        case SLTok_OR_ASSIGN:     *outTok = SLTok_OR; return 1;
        case SLTok_XOR_ASSIGN:    *outTok = SLTok_XOR; return 1;
        case SLTok_LSHIFT_ASSIGN: *outTok = SLTok_LSHIFT; return 1;
        case SLTok_RSHIFT_ASSIGN: *outTok = SLTok_RSHIFT; return 1;
        default:                  *outTok = SLTok_INVALID; return 0;
    }
}

static int SLMirStmtLowerIsReplayableExpr(const SLMirStmtLower* c, int32_t exprNode) {
    const SLAstNode* expr;
    int32_t          lhsNode;
    int32_t          rhsNode;
    SLTokenKind      binaryTok = SLTok_INVALID;
    if (c == NULL || exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
    switch (expr->kind) {
        case SLAst_IDENT:
        case SLAst_INT:
        case SLAst_FLOAT:
        case SLAst_STRING:
        case SLAst_BOOL:
        case SLAst_NULL:   return 1;
        case SLAst_UNARY:
            if ((SLTokenKind)expr->op == SLTok_AND) {
                return 0;
            }
            lhsNode = expr->firstChild;
            return lhsNode >= 0 && c->ast->nodes[lhsNode].nextSibling < 0
                && SLMirStmtLowerIsReplayableExpr(c, lhsNode);
        case SLAst_BINARY:
            lhsNode = expr->firstChild;
            rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
            if (lhsNode < 0 || rhsNode < 0 || c->ast->nodes[rhsNode].nextSibling >= 0
                || (SLTokenKind)expr->op == SLTok_ASSIGN)
            {
                return 0;
            }
            if (SLMirStmtLowerBinaryOpForAssign((SLTokenKind)expr->op, &binaryTok)) {
                return 0;
            }
            return SLMirStmtLowerIsReplayableExpr(c, lhsNode)
                && SLMirStmtLowerIsReplayableExpr(c, rhsNode);
        case SLAst_INDEX:
            lhsNode = expr->firstChild;
            rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
            return (expr->flags & 0x7u) == 0u && lhsNode >= 0 && rhsNode >= 0
                && c->ast->nodes[rhsNode].nextSibling < 0
                && SLMirStmtLowerIsReplayableExpr(c, lhsNode)
                && SLMirStmtLowerIsReplayableExpr(c, rhsNode);
        case SLAst_FIELD_EXPR:
            lhsNode = expr->firstChild;
            return lhsNode >= 0 && c->ast->nodes[lhsNode].nextSibling < 0
                && SLMirStmtLowerIsReplayableExpr(c, lhsNode);
        default: return 0;
    }
}

static int32_t SLMirStmtLowerVarInitExprNode(const SLAst* ast, int32_t nodeId) {
    int32_t firstChild;
    int32_t nextNode;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0) {
        return -1;
    }
    if (ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
        nextNode = ast->nodes[firstChild].nextSibling;
        if (nextNode >= 0 && SLMirStmtLowerIsTypeNodeKind(ast->nodes[nextNode].kind)) {
            nextNode = ast->nodes[nextNode].nextSibling;
        }
        return nextNode;
    }
    if (SLMirStmtLowerIsTypeNodeKind(ast->nodes[firstChild].kind)) {
        return ast->nodes[firstChild].nextSibling;
    }
    return firstChild;
}

static int32_t SLMirStmtLowerVarLikeDeclTypeNode(const SLAst* ast, int32_t nodeId) {
    int32_t firstChild;
    int32_t afterNames;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0 || (uint32_t)firstChild >= ast->len) {
        return -1;
    }
    if (ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
        afterNames = ast->nodes[firstChild].nextSibling;
        if (afterNames >= 0 && SLMirStmtLowerIsTypeNodeKind(ast->nodes[afterNames].kind)) {
            return afterNames;
        }
        return -1;
    }
    if (SLMirStmtLowerIsTypeNodeKind(ast->nodes[firstChild].kind)) {
        return firstChild;
    }
    return -1;
}

static int32_t SLMirStmtLowerVarLikeInitExprNodeAt(
    const SLAst* ast, int32_t nodeId, int32_t nameIndex) {
    int32_t firstChild;
    int32_t initNode;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len || nameIndex < 0) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0 || (uint32_t)firstChild >= ast->len) {
        return -1;
    }
    initNode = SLMirStmtLowerVarInitExprNode(ast, nodeId);
    if (initNode < 0 || (uint32_t)initNode >= ast->len) {
        return -1;
    }
    if (ast->nodes[firstChild].kind != SLAst_NAME_LIST) {
        return nameIndex == 0 ? initNode : -1;
    }
    if (ast->nodes[initNode].kind != SLAst_EXPR_LIST) {
        return -1;
    }
    {
        uint32_t nameCount = SLMirStmtLowerAstListCount(ast, firstChild);
        uint32_t initCount = SLMirStmtLowerAstListCount(ast, initNode);
        if ((uint32_t)nameIndex >= nameCount) {
            return -1;
        }
        if (initCount == nameCount) {
            return SLMirStmtLowerAstListItemAt(ast, initNode, (uint32_t)nameIndex);
        }
        if (initCount != 1u) {
            return -1;
        }
        {
            int32_t onlyInit = SLMirStmtLowerAstListItemAt(ast, initNode, 0u);
            if (onlyInit < 0 || (uint32_t)onlyInit >= ast->len
                || ast->nodes[onlyInit].kind != SLAst_TUPLE_EXPR)
            {
                return -1;
            }
            return SLMirStmtLowerAstListItemAt(ast, onlyInit, (uint32_t)nameIndex);
        }
    }
}

static int SLMirStmtLowerStmt(SLMirStmtLower* c, int32_t stmtNode);

static int SLMirStmtLowerEmitDeferredRange(SLMirStmtLower* c, uint32_t start) {
    uint32_t i;
    uint8_t  savedLoweringDeferred;
    if (c == NULL || start > c->deferredStmtLen) {
        return -1;
    }
    savedLoweringDeferred = c->loweringDeferred;
    c->loweringDeferred = 1u;
    for (i = c->deferredStmtLen; i > start; i--) {
        if (SLMirStmtLowerStmt(c, c->deferredStmtNodes[i - 1u]) != 0 || !c->supported) {
            c->loweringDeferred = savedLoweringDeferred;
            return c->supported ? -1 : 0;
        }
    }
    c->loweringDeferred = savedLoweringDeferred;
    return 0;
}

static int SLMirStmtLowerEmitDeferredForControl(
    SLMirStmtLower* c, const SLMirLowerControl* control) {
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
        const SLMirLowerBlockScope* scope = &c->blockScopes[i - 1u];
        c->deferredStmtLen = deferLimit;
        if (SLMirStmtLowerEmitDeferredRange(c, scope->deferStart) != 0 || !c->supported) {
            c->deferredStmtLen = originalDeferredLen;
            return c->supported ? -1 : 0;
        }
        deferLimit = scope->deferStart;
        i--;
    }
    c->deferredStmtLen = originalDeferredLen;
    return 0;
}

static int SLMirStmtLowerBlock(SLMirStmtLower* c, int32_t blockNode) {
    uint32_t scopeMark = c->localLen;
    uint32_t blockDepth = c->blockDepth;
    uint32_t deferStart;
    int32_t  child;
    if (blockNode < 0 || (uint32_t)blockNode >= c->ast->len
        || c->ast->nodes[blockNode].kind != SLAst_BLOCK)
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
        if (c->ast->nodes[child].kind == SLAst_DEFER) {
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
        if (SLMirStmtLowerStmt(c, child) != 0 || !c->supported) {
            c->deferredStmtLen = deferStart;
            c->blockDepth = blockDepth;
            c->localLen = scopeMark;
            return c->supported ? -1 : 0;
        }
        child = nextChild;
    }
    if (SLMirStmtLowerEmitDeferredRange(c, deferStart) != 0 || !c->supported) {
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

static int SLMirStmtLowerIf(SLMirStmtLower* c, int32_t ifNode) {
    int32_t  condNode = c->ast->nodes[ifNode].firstChild;
    int32_t  thenNode = condNode >= 0 ? c->ast->nodes[condNode].nextSibling : -1;
    int32_t  elseNode = thenNode >= 0 ? c->ast->nodes[thenNode].nextSibling : -1;
    uint32_t falseJumpInst = UINT32_MAX;
    uint32_t endJumpInst = UINT32_MAX;
    if (condNode < 0 || thenNode < 0) {
        c->supported = 0;
        return 0;
    }
    if (SLMirStmtLowerExpr(c, condNode) != 0 || !c->supported) {
        return c->supported ? -1 : 0;
    }
    if (SLMirStmtLowerAppendInst(
            c,
            SLMirOp_JUMP_IF_FALSE,
            0,
            UINT32_MAX,
            c->ast->nodes[ifNode].start,
            c->ast->nodes[ifNode].end,
            &falseJumpInst)
        != 0)
    {
        return -1;
    }
    if (c->ast->nodes[thenNode].kind == SLAst_BLOCK) {
        if (SLMirStmtLowerBlock(c, thenNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
    } else if (c->ast->nodes[thenNode].kind == SLAst_IF) {
        if (SLMirStmtLowerIf(c, thenNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
    } else {
        c->supported = 0;
        return 0;
    }
    if (elseNode >= 0) {
        if (SLMirStmtLowerAppendInst(
                c,
                SLMirOp_JUMP,
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
    c->builder.insts[falseJumpInst].aux = SLMirStmtLowerFnPc(c);
    if (elseNode >= 0) {
        if (c->ast->nodes[elseNode].kind == SLAst_BLOCK) {
            if (SLMirStmtLowerBlock(c, elseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        } else if (c->ast->nodes[elseNode].kind == SLAst_IF) {
            if (SLMirStmtLowerIf(c, elseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
        } else {
            c->supported = 0;
            return 0;
        }
        c->builder.insts[endJumpInst].aux = SLMirStmtLowerFnPc(c);
    }
    return 0;
}

static int SLMirStmtLowerStoreToLValueFromStack(
    SLMirStmtLower* c, int32_t lhsNode, uint32_t start, uint32_t end) {
    uint32_t slot = 0;
    int      mutable = 0;
    if (lhsNode < 0 || (uint32_t)lhsNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    if (c->ast->nodes[lhsNode].kind == SLAst_IDENT
        && SLMirStmtLowerFindLocal(
            c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, &slot, &mutable))
    {
        if (!mutable) {
            c->supported = 0;
            return 0;
        }
        return SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_STORE, 0, slot, start, end, NULL);
    }
    if (c->ast->nodes[lhsNode].kind == SLAst_UNARY
        && (SLTokenKind)c->ast->nodes[lhsNode].op == SLTok_MUL)
    {
        int32_t derefBase = c->ast->nodes[lhsNode].firstChild;
        if (derefBase >= 0 && (uint32_t)derefBase < c->ast->len
            && SLMirStmtLowerIsReplayableExpr(c, derefBase))
        {
            if (SLMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(c, SLMirOp_DEREF_STORE, 0, 0, start, end, NULL);
        }
    }
    if (c->ast->nodes[lhsNode].kind == SLAst_INDEX && (c->ast->nodes[lhsNode].flags & 0x7u) == 0u) {
        int32_t baseNode = c->ast->nodes[lhsNode].firstChild;
        int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
        if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerAppendInst(c, SLMirOp_ARRAY_ADDR, 0, 0, start, end, NULL) != 0) {
            return -1;
        }
        return SLMirStmtLowerAppendInst(c, SLMirOp_DEREF_STORE, 0, 0, start, end, NULL);
    }
    if (c->ast->nodes[lhsNode].kind == SLAst_FIELD_EXPR) {
        int32_t  baseNode = c->ast->nodes[lhsNode].firstChild;
        uint32_t fieldRef = UINT32_MAX;
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerAddFieldRef(
                c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, &fieldRef)
            != 0)
        {
            return -1;
        }
        if (SLMirStmtLowerAppendInst(
                c,
                SLMirOp_AGG_ADDR,
                0,
                fieldRef,
                c->ast->nodes[lhsNode].dataStart,
                c->ast->nodes[lhsNode].dataEnd,
                NULL)
            != 0)
        {
            return -1;
        }
        return SLMirStmtLowerAppendInst(c, SLMirOp_DEREF_STORE, 0, 0, start, end, NULL);
    }
    c->supported = 0;
    return 0;
}

static int SLMirStmtLowerExprNodeAsStmt(
    SLMirStmtLower* c, int32_t exprNode, uint32_t start, uint32_t end) {
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    if (c->ast->nodes[exprNode].kind == SLAst_BINARY) {
        const SLAstNode* expr = &c->ast->nodes[exprNode];
        int32_t          lhsNode = expr->firstChild;
        int32_t          rhsNode = lhsNode >= 0 ? c->ast->nodes[lhsNode].nextSibling : -1;
        uint32_t         slot = 0;
        int              mutable = 0;
        SLTokenKind      binaryTok = SLTok_INVALID;
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == SLAst_IDENT
            && c->ast->nodes[lhsNode].dataEnd == c->ast->nodes[lhsNode].dataStart + 1u
            && c->ast->nodes[lhsNode].dataEnd <= c->src.len
            && c->src.ptr[c->ast->nodes[lhsNode].dataStart] == '_')
        {
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(c, SLMirOp_DROP, 0, 0, expr->start, expr->end, NULL);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == SLAst_IDENT
            && SLMirStmtLowerFindLocal(
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
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                if (!SLMirStmtLowerBinaryOpForAssign((SLTokenKind)expr->op, &binaryTok)) {
                    c->supported = 0;
                    return 0;
                }
                if (SLMirStmtLowerAppendInst(
                        c,
                        SLMirOp_LOCAL_LOAD,
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
            if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_LOCAL_STORE, 0, slot, expr->start, expr->end, NULL);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == SLAst_IDENT)
        {
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendStoreValueBySlice(
                c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == SLAst_UNARY
            && (SLTokenKind)c->ast->nodes[lhsNode].op == SLTok_MUL)
        {
            int32_t derefBase = c->ast->nodes[lhsNode].firstChild;
            if (derefBase >= 0 && (uint32_t)derefBase < c->ast->len
                && SLMirStmtLowerIsReplayableExpr(c, derefBase))
            {
                if ((SLTokenKind)expr->op == SLTok_ASSIGN) {
                    if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (SLMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    return SLMirStmtLowerAppendInst(
                        c, SLMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
                }
                if (!SLMirStmtLowerBinaryOpForAssign((SLTokenKind)expr->op, &binaryTok)) {
                    c->supported = 0;
                    return 0;
                }
                if (SLMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_DEREF_LOAD, 0, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
                if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
                if (SLMirStmtLowerExpr(c, derefBase) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                return SLMirStmtLowerAppendInst(
                    c, SLMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
            }
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == SLAst_INDEX
            && (c->ast->nodes[lhsNode].flags & 0x7u) == 0u)
        {
            int32_t baseNode = c->ast->nodes[lhsNode].firstChild;
            int32_t indexNode = baseNode >= 0 ? c->ast->nodes[baseNode].nextSibling : -1;
            if (baseNode < 0 || indexNode < 0 || c->ast->nodes[indexNode].nextSibling >= 0) {
                c->supported = 0;
                return 0;
            }
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                if (!SLMirStmtLowerBinaryOpForAssign((SLTokenKind)expr->op, &binaryTok)
                    || !SLMirStmtLowerIsReplayableExpr(c, lhsNode))
                {
                    c->supported = 0;
                    return 0;
                }
                if (SLMirStmtLowerExpr(c, lhsNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            }
            if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (SLMirStmtLowerExpr(c, indexNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (SLMirStmtLowerAppendInst(
                    c,
                    SLMirOp_ARRAY_ADDR,
                    0,
                    0,
                    c->ast->nodes[lhsNode].start,
                    c->ast->nodes[lhsNode].end,
                    NULL)
                != 0)
            {
                return -1;
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
        }
        if (lhsNode >= 0 && rhsNode >= 0 && c->ast->nodes[rhsNode].nextSibling < 0
            && c->ast->nodes[lhsNode].kind == SLAst_FIELD_EXPR)
        {
            int32_t  baseNode = c->ast->nodes[lhsNode].firstChild;
            uint32_t fieldRef = UINT32_MAX;
            if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                if (!SLMirStmtLowerBinaryOpForAssign((SLTokenKind)expr->op, &binaryTok)
                    || !SLMirStmtLowerIsReplayableExpr(c, lhsNode))
                {
                    c->supported = 0;
                    return 0;
                }
                if (SLMirStmtLowerExpr(c, lhsNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            }
            if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_BINARY, (uint16_t)binaryTok, 0, expr->start, expr->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            if (SLMirStmtLowerAddFieldRef(
                    c, c->ast->nodes[lhsNode].dataStart, c->ast->nodes[lhsNode].dataEnd, &fieldRef)
                != 0)
            {
                return -1;
            }
            if (SLMirStmtLowerAppendInst(
                    c,
                    SLMirOp_AGG_ADDR,
                    0,
                    fieldRef,
                    c->ast->nodes[lhsNode].dataStart,
                    c->ast->nodes[lhsNode].dataEnd,
                    NULL)
                != 0)
            {
                return -1;
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
        }
    }
    if (SLMirStmtLowerExpr(c, exprNode) != 0 || !c->supported) {
        return c->supported ? -1 : 0;
    }
    return SLMirStmtLowerAppendInst(c, SLMirOp_DROP, 0, 0, start, end, NULL);
}

static int SLMirStmtLowerExprStmt(SLMirStmtLower* c, int32_t stmtNode) {
    int32_t exprNode = c->ast->nodes[stmtNode].firstChild;
    if (exprNode < 0 || c->ast->nodes[exprNode].nextSibling >= 0) {
        c->supported = 0;
        return 0;
    }
    return SLMirStmtLowerExprNodeAsStmt(
        c, exprNode, c->ast->nodes[stmtNode].start, c->ast->nodes[stmtNode].end);
}

static int SLMirStmtLowerSwitchTest(
    SLMirStmtLower* c,
    uint32_t        subjectSlot,
    int             hasSubject,
    int32_t         labelExprNode,
    uint32_t        start,
    uint32_t        end) {
    if (hasSubject) {
        if (SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_LOAD, 0, subjectSlot, start, end, NULL) != 0)
        {
            return -1;
        }
    }
    if (SLMirStmtLowerExpr(c, labelExprNode) != 0 || !c->supported) {
        return c->supported ? -1 : 0;
    }
    if (hasSubject) {
        return SLMirStmtLowerAppendInst(c, SLMirOp_BINARY, (uint16_t)SLTok_EQ, 0, start, end, NULL);
    }
    return 0;
}

static int SLMirStmtLowerSwitch(SLMirStmtLower* c, int32_t stmtNode) {
    const SLAstNode* s = &c->ast->nodes[stmtNode];
    int32_t          clauseNode = s->firstChild;
    int32_t          defaultBodyNode = -1;
    uint32_t         scopeMark = c->localLen;
    uint32_t         subjectSlot = UINT32_MAX;
    uint32_t         pendingNextClauseJump = UINT32_MAX;
    int              hasSubject = s->flags == 1;
    if (hasSubject) {
        if (clauseNode < 0 || (uint32_t)clauseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &subjectSlot) != 0) {
            return -1;
        }
        if (SLMirStmtLowerExpr(c, clauseNode) != 0 || !c->supported) {
            c->localLen = scopeMark;
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_STORE, 0, subjectSlot, s->start, s->end, NULL)
            != 0)
        {
            c->localLen = scopeMark;
            return -1;
        }
        clauseNode = c->ast->nodes[clauseNode].nextSibling;
    }
    if (!SLMirStmtLowerPushControl(c, 0, 0)) {
        c->localLen = scopeMark;
        return 0;
    }
    while (clauseNode >= 0) {
        const SLAstNode* clause = &c->ast->nodes[clauseNode];
        if (pendingNextClauseJump != UINT32_MAX) {
            c->builder.insts[pendingNextClauseJump].aux = SLMirStmtLowerFnPc(c);
            pendingNextClauseJump = UINT32_MAX;
        }
        if (clause->kind == SLAst_CASE) {
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
                    c->builder.insts[pendingFalseJump].aux = SLMirStmtLowerFnPc(c);
                    pendingFalseJump = UINT32_MAX;
                }
                if (c->ast->nodes[caseChild].kind == SLAst_CASE_PATTERN) {
                    labelExprNode = c->ast->nodes[caseChild].firstChild;
                    aliasNode = labelExprNode >= 0 ? c->ast->nodes[labelExprNode].nextSibling : -1;
                    if (labelExprNode < 0
                        || (aliasNode >= 0 && c->ast->nodes[aliasNode].nextSibling >= 0)
                        || (!hasSubject && aliasNode >= 0)
                        || (aliasNode >= 0 && c->ast->nodes[aliasNode].kind != SLAst_IDENT))
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
                if (SLMirStmtLowerSwitchTest(
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
                if (SLMirStmtLowerAppendInst(
                        c,
                        SLMirOp_JUMP_IF_FALSE,
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
                if (SLMirStmtLowerAppendInst(
                        c,
                        SLMirOp_JUMP,
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
                || c->ast->nodes[bodyNode].kind != SLAst_BLOCK)
            {
                c->supported = 0;
                c->localLen = scopeMark;
                c->controlLen--;
                return 0;
            }
            {
                uint32_t bodyPc = SLMirStmtLowerFnPc(c);
                uint32_t i;
                for (i = 0; i < bodyJumpLen; i++) {
                    c->builder.insts[bodyJumps[i]].aux = bodyPc;
                }
            }
            if (aliasNode >= 0) {
                uint32_t aliasSlot = UINT32_MAX;
                if (SLMirStmtLowerPushLocal(
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
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_LOCAL_LOAD, 0, subjectSlot, clause->start, clause->end, NULL)
                        != 0
                    || SLMirStmtLowerAppendInst(
                           c, SLMirOp_LOCAL_STORE, 0, aliasSlot, clause->start, clause->end, NULL)
                           != 0)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return -1;
                }
            }
            if (SLMirStmtLowerBlock(c, bodyNode) != 0 || !c->supported) {
                c->localLen = scopeMark;
                c->controlLen--;
                return c->supported ? -1 : 0;
            }
            if (aliasNode >= 0) {
                c->localLen--;
            }
            {
                uint32_t endJump = UINT32_MAX;
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_JUMP, 0, UINT32_MAX, clause->start, clause->end, &endJump)
                    != 0)
                {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return -1;
                }
                if (!SLMirStmtLowerRecordControlJump(c, 0, endJump)) {
                    c->localLen = scopeMark;
                    c->controlLen--;
                    return 0;
                }
            }
            pendingNextClauseJump = pendingFalseJump;
        } else if (clause->kind == SLAst_DEFAULT) {
            int32_t nextClause = c->ast->nodes[clauseNode].nextSibling;
            defaultBodyNode = clause->firstChild;
            if (defaultBodyNode < 0 || (uint32_t)defaultBodyNode >= c->ast->len
                || c->ast->nodes[defaultBodyNode].kind != SLAst_BLOCK || nextClause >= 0)
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
            c->builder.insts[pendingNextClauseJump].aux = SLMirStmtLowerFnPc(c);
            pendingNextClauseJump = UINT32_MAX;
        }
        if (SLMirStmtLowerBlock(c, defaultBodyNode) != 0 || !c->supported) {
            c->localLen = scopeMark;
            c->controlLen--;
            return c->supported ? -1 : 0;
        }
    }
    {
        uint32_t endPc = SLMirStmtLowerFnPc(c);
        if (pendingNextClauseJump != UINT32_MAX) {
            c->builder.insts[pendingNextClauseJump].aux = endPc;
        }
        SLMirStmtLowerFinishControl(c, 0, endPc);
    }
    c->localLen = scopeMark;
    return 0;
}

static int SLMirStmtLowerForIn(SLMirStmtLower* c, int32_t stmtNode) {
    const SLAstNode* s = &c->ast->nodes[stmtNode];
    int32_t          parts[4];
    uint32_t         partLen = 0;
    int32_t          cur = s->firstChild;
    int              hasKey = (s->flags & SLAstFlag_FOR_IN_HAS_KEY) != 0;
    int              keyRef = (s->flags & SLAstFlag_FOR_IN_KEY_REF) != 0;
    int              valueRef = (s->flags & SLAstFlag_FOR_IN_VALUE_REF) != 0;
    int              valueDiscard = (s->flags & SLAstFlag_FOR_IN_VALUE_DISCARD) != 0;
    int32_t          keyNode = -1;
    int32_t          valueNode = -1;
    int32_t          sourceNode = -1;
    int32_t          bodyNode = -1;
    uint32_t         scopeMark = c->localLen;
    uint32_t         sourceSlot = UINT32_MAX;
    uint32_t         iterSlot = UINT32_MAX;
    uint32_t         keySlot = UINT32_MAX;
    uint32_t         valueSlot = UINT32_MAX;
    uint32_t         loopStartPc;
    uint32_t         continueTargetPc;
    uint32_t         loopEndPc;
    uint32_t         condFalseJump = UINT32_MAX;
    uint16_t         iterFlags = 0;
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
        || c->ast->nodes[bodyNode].kind != SLAst_BLOCK || sourceNode < 0
        || (uint32_t)sourceNode >= c->ast->len)
    {
        c->supported = 0;
        return 0;
    }
    if (hasKey
        && (keyNode < 0 || (uint32_t)keyNode >= c->ast->len
            || c->ast->nodes[keyNode].kind != SLAst_IDENT))
    {
        c->supported = 0;
        return 0;
    }
    if (!valueDiscard
        && (valueNode < 0 || (uint32_t)valueNode >= c->ast->len
            || c->ast->nodes[valueNode].kind != SLAst_IDENT))
    {
        c->supported = 0;
        return 0;
    }
    if ((hasKey ? SLMirIterFlag_HAS_KEY : 0u) != 0u) {
        iterFlags |= SLMirIterFlag_HAS_KEY;
    }
    if (keyRef) {
        iterFlags |= SLMirIterFlag_KEY_REF;
    }
    if (valueRef) {
        iterFlags |= SLMirIterFlag_VALUE_REF;
    }
    if (valueDiscard) {
        iterFlags |= SLMirIterFlag_VALUE_DISCARD;
    }
    if (SLMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &sourceSlot) != 0
        || SLMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &iterSlot) != 0)
    {
        return -1;
    }
    if (SLMirStmtLowerExpr(c, sourceNode) != 0 || !c->supported) {
        c->localLen = scopeMark;
        return c->supported ? -1 : 0;
    }
    if (SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_STORE, 0, sourceSlot, s->start, s->end, NULL) != 0
        || SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_LOAD, 0, sourceSlot, s->start, s->end, NULL)
               != 0
        || SLMirStmtLowerAppendInst(
               c, SLMirOp_ITER_INIT, iterFlags, (uint32_t)sourceNode, s->start, s->end, NULL)
               != 0
        || SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_STORE, 0, iterSlot, s->start, s->end, NULL)
               != 0)
    {
        c->localLen = scopeMark;
        return -1;
    }
    if (hasKey
        && !(
            c->ast->nodes[keyNode].dataEnd == c->ast->nodes[keyNode].dataStart + 1u
            && c->src.ptr[c->ast->nodes[keyNode].dataStart] == '_')
        && SLMirStmtLowerPushLocal(
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
        && SLMirStmtLowerPushLocal(
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
    loopStartPc = SLMirStmtLowerFnPc(c);
    if (!SLMirStmtLowerPushControl(c, 1, loopStartPc)) {
        c->localLen = scopeMark;
        return 0;
    }
    if (SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_LOAD, 0, iterSlot, s->start, s->end, NULL) != 0
        || SLMirStmtLowerAppendInst(c, SLMirOp_ITER_NEXT, iterFlags, 0, s->start, s->end, NULL) != 0
        || SLMirStmtLowerAppendInst(
               c, SLMirOp_JUMP_IF_FALSE, 0, UINT32_MAX, s->start, s->end, &condFalseJump)
               != 0)
    {
        c->localLen = scopeMark;
        c->controlLen--;
        return -1;
    }
    if (!valueDiscard && valueSlot != UINT32_MAX) {
        if (SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_STORE, 0, valueSlot, s->start, s->end, NULL)
            != 0)
        {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    } else if (!valueDiscard) {
        if (SLMirStmtLowerAppendInst(c, SLMirOp_DROP, 0, 0, s->start, s->end, NULL) != 0) {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    }
    if (hasKey && keySlot != UINT32_MAX) {
        if (SLMirStmtLowerAppendInst(c, SLMirOp_LOCAL_STORE, 0, keySlot, s->start, s->end, NULL)
            != 0)
        {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    } else if (hasKey) {
        if (SLMirStmtLowerAppendInst(c, SLMirOp_DROP, 0, 0, s->start, s->end, NULL) != 0) {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    }
    if (SLMirStmtLowerBlock(c, bodyNode) != 0 || !c->supported) {
        c->localLen = scopeMark;
        c->controlLen--;
        return c->supported ? -1 : 0;
    }
    continueTargetPc = SLMirStmtLowerFnPc(c);
    if (SLMirStmtLowerAppendInst(c, SLMirOp_JUMP, 0, loopStartPc, s->start, s->end, NULL) != 0) {
        c->localLen = scopeMark;
        c->controlLen--;
        return -1;
    }
    loopEndPc = SLMirStmtLowerFnPc(c);
    c->builder.insts[condFalseJump].aux = loopEndPc;
    SLMirStmtLowerFinishControl(c, continueTargetPc, loopEndPc);
    c->localLen = scopeMark;
    return 0;
}

static int SLMirStmtLowerFor(SLMirStmtLower* c, int32_t stmtNode) {
    const SLAstNode* s = &c->ast->nodes[stmtNode];
    int32_t          parts[4];
    uint32_t         partLen = 0;
    int32_t          cur = s->firstChild;
    int32_t          initNode = -1;
    int32_t          condNode = -1;
    int32_t          postNode = -1;
    int32_t          bodyNode = -1;
    uint32_t         bodyStart;
    uint32_t         scopeMark = c->localLen;
    uint32_t         loopStartPc;
    uint32_t         condFalseJump = UINT32_MAX;
    uint32_t         continueTargetPc;
    uint32_t         loopEndPc;
    uint32_t         semi1 = 0;
    uint32_t         semi2 = 0;
    int              hasSemicolons;
    if ((s->flags & SLAstFlag_FOR_IN) != 0) {
        return SLMirStmtLowerForIn(c, stmtNode);
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
    if ((uint32_t)bodyNode >= c->ast->len || c->ast->nodes[bodyNode].kind != SLAst_BLOCK) {
        c->supported = 0;
        return 0;
    }
    bodyStart = c->ast->nodes[bodyNode].start;
    hasSemicolons = SLMirStmtLowerRangeHasChar(c->src, s->start, bodyStart, ';');
    if (!hasSemicolons) {
        if (partLen == 2u) {
            condNode = parts[0];
        } else if (partLen != 1u) {
            c->supported = 0;
            return 0;
        }
    } else {
        uint32_t i;
        if (!SLMirStmtLowerFindCharForward(c->src, s->start, bodyStart, ';', &semi1)
            || !SLMirStmtLowerFindCharForward(c->src, semi1 + 1u, bodyStart, ';', &semi2))
        {
            c->supported = 0;
            return 0;
        }
        for (i = 0; i + 1u < partLen; i++) {
            const SLAstNode* part = &c->ast->nodes[parts[i]];
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
        if (c->ast->nodes[initNode].kind == SLAst_VAR
            || c->ast->nodes[initNode].kind == SLAst_CONST)
        {
            if (SLMirStmtLowerStmt(c, initNode) != 0 || !c->supported) {
                c->localLen = scopeMark;
                return c->supported ? -1 : 0;
            }
        } else if (
            SLMirStmtLowerExprNodeAsStmt(
                c, initNode, c->ast->nodes[initNode].start, c->ast->nodes[initNode].end)
                != 0
            || !c->supported)
        {
            c->localLen = scopeMark;
            return c->supported ? -1 : 0;
        }
    }
    loopStartPc = SLMirStmtLowerFnPc(c);
    if (!SLMirStmtLowerPushControl(c, 1, loopStartPc)) {
        c->localLen = scopeMark;
        return 0;
    }
    if (condNode >= 0) {
        if (SLMirStmtLowerExpr(c, condNode) != 0 || !c->supported) {
            c->localLen = scopeMark;
            c->controlLen--;
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerAppendInst(
                c, SLMirOp_JUMP_IF_FALSE, 0, UINT32_MAX, s->start, s->end, &condFalseJump)
            != 0)
        {
            c->localLen = scopeMark;
            c->controlLen--;
            return -1;
        }
    }
    if (SLMirStmtLowerBlock(c, bodyNode) != 0 || !c->supported) {
        c->localLen = scopeMark;
        c->controlLen--;
        return c->supported ? -1 : 0;
    }
    continueTargetPc = postNode >= 0 ? SLMirStmtLowerFnPc(c) : loopStartPc;
    if (postNode >= 0
        && (SLMirStmtLowerExprNodeAsStmt(
                c, postNode, c->ast->nodes[postNode].start, c->ast->nodes[postNode].end)
                != 0
            || !c->supported))
    {
        c->localLen = scopeMark;
        c->controlLen--;
        return c->supported ? -1 : 0;
    }
    if (SLMirStmtLowerAppendInst(c, SLMirOp_JUMP, 0, loopStartPc, s->start, s->end, NULL) != 0) {
        c->localLen = scopeMark;
        c->controlLen--;
        return -1;
    }
    loopEndPc = SLMirStmtLowerFnPc(c);
    if (condFalseJump != UINT32_MAX) {
        c->builder.insts[condFalseJump].aux = loopEndPc;
    }
    SLMirStmtLowerFinishControl(c, continueTargetPc, loopEndPc);
    c->localLen = scopeMark;
    return 0;
}

static int SLMirStmtLowerStmt(SLMirStmtLower* c, int32_t stmtNode) {
    const SLAstNode* s;
    if (stmtNode < 0 || (uint32_t)stmtNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    s = &c->ast->nodes[stmtNode];
    switch (s->kind) {
        case SLAst_BLOCK:  return SLMirStmtLowerBlock(c, stmtNode);
        case SLAst_IF:     return SLMirStmtLowerIf(c, stmtNode);
        case SLAst_SWITCH: return SLMirStmtLowerSwitch(c, stmtNode);
        case SLAst_FOR:    return SLMirStmtLowerFor(c, stmtNode);
        case SLAst_BREAK:  {
            uint32_t           jumpInst = UINT32_MAX;
            SLMirLowerControl* control = SLMirStmtLowerCurrentBreakable(c);
            if (c->loweringDeferred || control == NULL) {
                if (c->loweringDeferred) {
                    SLMirLowerStmtSetUnsupportedDetail(
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
                && (SLMirStmtLowerEmitDeferredForControl(c, control) != 0 || !c->supported))
            {
                return c->supported ? -1 : 0;
            }
            if (SLMirStmtLowerAppendInst(
                    c, SLMirOp_JUMP, 0, UINT32_MAX, s->start, s->end, &jumpInst)
                != 0)
            {
                return -1;
            }
            if (!SLMirStmtLowerRecordControlJump(c, 0, jumpInst)) {
                return 0;
            }
            return 0;
        }
        case SLAst_CONTINUE: {
            uint32_t           jumpInst = UINT32_MAX;
            SLMirLowerControl* control = SLMirStmtLowerCurrentContinuable(c);
            if (c->loweringDeferred || control == NULL) {
                if (c->loweringDeferred) {
                    SLMirLowerStmtSetUnsupportedDetail(
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
                && (SLMirStmtLowerEmitDeferredForControl(c, control) != 0 || !c->supported))
            {
                return c->supported ? -1 : 0;
            }
            if (SLMirStmtLowerAppendInst(
                    c, SLMirOp_JUMP, 0, control->continueTargetPc, s->start, s->end, &jumpInst)
                != 0)
            {
                return -1;
            }
            if (!SLMirStmtLowerRecordControlJump(c, 1, jumpInst)) {
                return 0;
            }
            return 0;
        }
        case SLAst_RETURN: {
            int32_t  exprNode = s->firstChild;
            uint32_t exprCount = 0;
            if (c->loweringDeferred) {
                SLMirLowerStmtSetUnsupportedDetail(
                    c,
                    s->start,
                    s->end,
                    "deferred statement cannot alter control flow in const evaluation");
                return 0;
            }
            if (exprNode < 0) {
                if (c->deferredStmtLen > 0
                    && (SLMirStmtLowerEmitDeferredRange(c, 0) != 0 || !c->supported))
                {
                    return c->supported ? -1 : 0;
                }
                return SLMirStmtLowerAppendInst(
                    c, SLMirOp_RETURN_VOID, 0, 0, s->start, s->end, NULL);
            }
            if (c->deferredStmtLen > 0
                && (SLMirStmtLowerEmitDeferredRange(c, 0) != 0 || !c->supported))
            {
                return c->supported ? -1 : 0;
            }
            if (c->ast->nodes[exprNode].kind == SLAst_EXPR_LIST) {
                exprCount = SLMirStmtLowerAstListCount(c->ast, exprNode);
            } else {
                while (exprNode >= 0) {
                    exprCount++;
                    exprNode = c->ast->nodes[exprNode].nextSibling;
                }
                exprNode = s->firstChild;
            }
            if (exprCount == 1u) {
                if (c->ast->nodes[exprNode].kind == SLAst_EXPR_LIST) {
                    exprNode = SLMirStmtLowerAstListItemAt(c->ast, exprNode, 0u);
                    if (exprNode < 0) {
                        c->supported = 0;
                        return 0;
                    }
                }
                if (SLMirStmtLowerExpr(c, exprNode) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
            } else {
                int32_t returnTypeNode = c->functionReturnTypeNode;
                int32_t tupleReturnTypeNode = returnTypeNode;
                if (tupleReturnTypeNode >= 0 && (uint32_t)tupleReturnTypeNode < c->ast->len
                    && c->ast->nodes[tupleReturnTypeNode].kind == SLAst_TYPE_OPTIONAL)
                {
                    tupleReturnTypeNode = c->ast->nodes[tupleReturnTypeNode].firstChild;
                }
                if (exprCount > UINT16_MAX || returnTypeNode < 0 || tupleReturnTypeNode < 0
                    || (uint32_t)tupleReturnTypeNode >= c->ast->len
                    || c->ast->nodes[tupleReturnTypeNode].kind != SLAst_TYPE_TUPLE
                    || SLMirStmtLowerAstListCount(c->ast, tupleReturnTypeNode) != exprCount)
                {
                    c->supported = 0;
                    return 0;
                }
                if (c->ast->nodes[exprNode].kind == SLAst_EXPR_LIST) {
                    uint32_t i;
                    for (i = 0; i < exprCount; i++) {
                        int32_t itemNode = SLMirStmtLowerAstListItemAt(c->ast, exprNode, i);
                        if (itemNode < 0) {
                            c->supported = 0;
                            return 0;
                        }
                        if (SLMirStmtLowerExpr(c, itemNode) != 0 || !c->supported) {
                            return c->supported ? -1 : 0;
                        }
                    }
                } else {
                    int32_t returnExprNode = s->firstChild;
                    while (returnExprNode >= 0) {
                        int32_t itemNode = returnExprNode;
                        returnExprNode = c->ast->nodes[returnExprNode].nextSibling;
                        if (SLMirStmtLowerExpr(c, itemNode) != 0 || !c->supported) {
                            return c->supported ? -1 : 0;
                        }
                    }
                }
                if (SLMirStmtLowerAppendTupleMake(
                        c, exprCount, tupleReturnTypeNode, s->start, s->end)
                    != 0)
                {
                    return -1;
                }
            }
            return SLMirStmtLowerAppendInst(c, SLMirOp_RETURN, 0, 0, s->start, s->end, NULL);
        }
        case SLAst_ASSERT: {
            int32_t condNode = s->firstChild;
            if (condNode < 0) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, condNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(c, SLMirOp_ASSERT, 0, 0, s->start, s->end, NULL);
        }
        case SLAst_MULTI_ASSIGN: {
            int32_t  lhsList = s->firstChild;
            int32_t  rhsList = lhsList >= 0 ? c->ast->nodes[lhsList].nextSibling : -1;
            uint32_t lhsCount;
            uint32_t rhsCount;
            uint32_t i;
            uint32_t tempSlots[256];
            uint32_t tupleTempSlot = UINT32_MAX;
            if (lhsList < 0 || rhsList < 0 || c->ast->nodes[lhsList].kind != SLAst_EXPR_LIST
                || c->ast->nodes[rhsList].kind != SLAst_EXPR_LIST)
            {
                c->supported = 0;
                return 0;
            }
            lhsCount = SLMirStmtLowerAstListCount(c->ast, lhsList);
            rhsCount = SLMirStmtLowerAstListCount(c->ast, rhsList);
            if (lhsCount == 0u || lhsCount > 256u || (rhsCount != lhsCount && rhsCount != 1u)) {
                c->supported = 0;
                return 0;
            }
            if (rhsCount == lhsCount) {
                for (i = 0; i < rhsCount; i++) {
                    int32_t rhsExpr = SLMirStmtLowerAstListItemAt(c->ast, rhsList, i);
                    if (rhsExpr < 0
                        || SLMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tempSlots[i]) != 0)
                    {
                        return rhsExpr < 0 ? 0 : -1;
                    }
                    if (SLMirStmtLowerExpr(c, rhsExpr) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (SLMirStmtLowerAppendInst(
                            c, SLMirOp_LOCAL_STORE, 0, tempSlots[i], s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
            } else {
                int32_t rhsExpr = SLMirStmtLowerAstListItemAt(c->ast, rhsList, 0u);
                if (rhsExpr < 0
                    || SLMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tupleTempSlot) != 0)
                {
                    return rhsExpr < 0 ? 0 : -1;
                }
                if (SLMirStmtLowerExpr(c, rhsExpr) != 0 || !c->supported) {
                    return c->supported ? -1 : 0;
                }
                if (SLMirStmtLowerAppendInst(
                        c, SLMirOp_LOCAL_STORE, 0, tupleTempSlot, s->start, s->end, NULL)
                    != 0)
                {
                    return -1;
                }
            }
            for (i = 0; i < lhsCount; i++) {
                int32_t lhsExpr = SLMirStmtLowerAstListItemAt(c->ast, lhsList, i);
                if (lhsExpr < 0) {
                    c->supported = 0;
                    return 0;
                }
                if (rhsCount == lhsCount) {
                    if (SLMirStmtLowerAppendInst(
                            c, SLMirOp_LOCAL_LOAD, 0, tempSlots[i], s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                } else {
                    if (SLMirStmtLowerAppendInst(
                            c, SLMirOp_LOCAL_LOAD, 0, tupleTempSlot, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                    if (SLMirStmtLowerAppendIntConst(c, (int64_t)i, s->start, s->end) != 0) {
                        return -1;
                    }
                    if (SLMirStmtLowerAppendInst(c, SLMirOp_INDEX, 0, 0, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                if (SLMirStmtLowerStoreToLValueFromStack(c, lhsExpr, s->start, s->end) != 0
                    || !c->supported)
                {
                    return c->supported ? -1 : 0;
                }
            }
            return 0;
        }
        case SLAst_EXPR_STMT: return SLMirStmtLowerExprStmt(c, stmtNode);
        case SLAst_CONST_BLOCK:
            /* Consteval blocks are compile-time-only and do not execute at runtime. */
            return 0;
        case SLAst_VAR:
        case SLAst_CONST: {
            int32_t  firstChild = s->firstChild;
            int32_t  typeNode = -1;
            int32_t  initNode = SLMirStmtLowerVarInitExprNode(c->ast, stmtNode);
            int32_t  tupleInitNode = -1;
            uint32_t slot = 0;
            if (firstChild < 0) {
                c->supported = 0;
                return 0;
            }
            if (c->ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
                uint32_t nameCount = SLMirStmtLowerAstListCount(c->ast, firstChild);
                uint32_t initCount = 0;
                uint32_t tupleTempSlot = UINT32_MAX;
                uint32_t nameLocalStart = c->localLen;
                int      useTupleInitTemp = 0;
                uint32_t i;
                typeNode = SLMirStmtLowerVarLikeDeclTypeNode(c->ast, stmtNode);
                if (nameCount == 0u || (initNode < 0 && typeNode < 0)) {
                    c->supported = 0;
                    return 0;
                }
                if (initNode >= 0) {
                    if (c->ast->nodes[initNode].kind != SLAst_EXPR_LIST) {
                        c->supported = 0;
                        return 0;
                    }
                    initCount = SLMirStmtLowerAstListCount(c->ast, initNode);
                    if (initCount == 1u && nameCount > 1u) {
                        tupleInitNode = SLMirStmtLowerAstListItemAt(c->ast, initNode, 0u);
                        if (tupleInitNode < 0) {
                            c->supported = 0;
                            return 0;
                        }
                        useTupleInitTemp = c->ast->nodes[tupleInitNode].kind != SLAst_TUPLE_EXPR;
                    }
                }
                for (i = 0; i < nameCount; i++) {
                    uint32_t         localSlot = UINT32_MAX;
                    int32_t          nameNode = SLMirStmtLowerAstListItemAt(c->ast, firstChild, i);
                    const SLAstNode* name = nameNode >= 0 ? &c->ast->nodes[nameNode] : NULL;
                    if (name == NULL || name->kind != SLAst_IDENT) {
                        c->supported = 0;
                        return 0;
                    }
                    if (SLMirStmtLowerPushLocal(
                            c,
                            name->dataStart,
                            name->dataEnd,
                            s->kind == SLAst_VAR,
                            0,
                            initNode < 0,
                            typeNode,
                            SLMirStmtLowerVarLikeInitExprNodeAt(c->ast, stmtNode, (int32_t)i),
                            &localSlot)
                        != 0)
                    {
                        return -1;
                    }
                }
                if (useTupleInitTemp) {
                    if (SLMirStmtLowerPushLocal(c, 0, 0, 0, 0, 0, -1, -1, &tupleTempSlot) != 0) {
                        return -1;
                    }
                    if (SLMirStmtLowerExpr(c, tupleInitNode) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (SLMirStmtLowerAppendInst(
                            c, SLMirOp_LOCAL_STORE, 0, tupleTempSlot, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                for (i = 0; i < nameCount; i++) {
                    int32_t itemInitNode = SLMirStmtLowerVarLikeInitExprNodeAt(
                        c->ast, stmtNode, (int32_t)i);
                    slot = c->locals[nameLocalStart + i].slot;
                    if (itemInitNode < 0) {
                        if (useTupleInitTemp) {
                            if (SLMirStmtLowerAppendInst(
                                    c, SLMirOp_LOCAL_LOAD, 0, tupleTempSlot, s->start, s->end, NULL)
                                != 0)
                            {
                                return -1;
                            }
                            if (SLMirStmtLowerAppendIntConst(c, (int64_t)i, s->start, s->end) != 0)
                            {
                                return -1;
                            }
                            if (SLMirStmtLowerAppendInst(
                                    c, SLMirOp_INDEX, 0, 0, s->start, s->end, NULL)
                                != 0)
                            {
                                return -1;
                            }
                            if (SLMirStmtLowerAppendInst(
                                    c, SLMirOp_LOCAL_STORE, 0, slot, s->start, s->end, NULL)
                                != 0)
                            {
                                return -1;
                            }
                            continue;
                        }
                        if (SLMirStmtLowerAppendInst(
                                c, SLMirOp_LOCAL_ZERO, 0, slot, s->start, s->end, NULL)
                            != 0)
                        {
                            return -1;
                        }
                        continue;
                    }
                    if (SLMirStmtLowerExpr(c, itemInitNode) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (SLMirStmtLowerAppendInst(
                            c, SLMirOp_LOCAL_STORE, 0, slot, s->start, s->end, NULL)
                        != 0)
                    {
                        return -1;
                    }
                }
                return 0;
            }
            if (SLMirStmtLowerIsTypeNodeKind(c->ast->nodes[firstChild].kind)) {
                typeNode = firstChild;
            }
            if (initNode < 0 && typeNode < 0) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerPushLocal(
                    c,
                    s->dataStart,
                    s->dataEnd,
                    s->kind == SLAst_VAR,
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
                return SLMirStmtLowerAppendInst(
                    c, SLMirOp_LOCAL_ZERO, 0, slot, s->start, s->end, NULL);
            }
            if (SLMirStmtLowerExpr(c, initNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(
                c, SLMirOp_LOCAL_STORE, 0, slot, s->start, s->end, NULL);
        }
        default: c->supported = 0; return 0;
    }
}

int SLMirLowerAppendSimpleFunctionWithOptions(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    const SLMirLowerOptions* _Nullable options,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirStmtLower c;
    SLMirFunction  fn = { 0 };
    SLMirSourceRef sourceRef = { 0 };
    SLMirTypeRef   typeRef = { 0 };
    uint32_t       sourceIndex = 0;
    int32_t        returnTypeNode = -1;
    int32_t        child;
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (builder == NULL || outFunctionIndex == NULL || outSupported == NULL || arena == NULL
        || ast == NULL)
    {
        SLMirLowerStmtSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outFunctionIndex = UINT32_MAX;
    *outSupported = 0;
    if (bodyNode < 0 || (uint32_t)bodyNode >= ast->len || ast->nodes[bodyNode].kind != SLAst_BLOCK)
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
    if (SLMirProgramBuilderAddSource(&c.builder, &sourceRef, &sourceIndex) != 0) {
        SLMirLowerStmtSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    fn.nameStart = fnNode >= 0 && (uint32_t)fnNode < ast->len ? ast->nodes[fnNode].dataStart : 0;
    fn.nameEnd = fnNode >= 0 && (uint32_t)fnNode < ast->len ? ast->nodes[fnNode].dataEnd : 0;
    fn.sourceRef = sourceIndex;
    fn.typeRef = UINT32_MAX;
    returnTypeNode = SLMirStmtLowerFunctionReturnTypeNode(ast, fnNode);
    c.functionReturnTypeNode = returnTypeNode;
    if (returnTypeNode >= 0) {
        typeRef.astNode = (uint32_t)returnTypeNode;
        typeRef.flags = 0;
        if (SLMirProgramBuilderAddType(&c.builder, &typeRef, &fn.typeRef) != 0) {
            SLMirLowerStmtSetDiag(
                diag,
                SLDiag_ARENA_OOM,
                ast->nodes[returnTypeNode].start,
                ast->nodes[returnTypeNode].end);
            return -1;
        }
    }
    if (SLMirProgramBuilderBeginFunction(&c.builder, &fn, &c.functionIndex) != 0) {
        SLMirLowerStmtSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    if (fnNode >= 0 && (uint32_t)fnNode < ast->len) {
        child = ast->nodes[fnNode].firstChild;
        while (child >= 0) {
            if (ast->nodes[child].kind == SLAst_PARAM) {
                int32_t  paramTypeNode = ast->nodes[child].firstChild;
                uint32_t slot = 0;
                if (SLMirStmtLowerPushLocal(
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
                if ((ast->nodes[child].flags & SLAstFlag_PARAM_VARIADIC) != 0u) {
                    c.builder.funcs[c.functionIndex].flags |= SLMirFunctionFlag_VARIADIC;
                }
            }
            child = ast->nodes[child].nextSibling;
        }
    }

    if (SLMirStmtLowerBlock(&c, bodyNode) != 0) {
        return -1;
    }
    if (!c.supported) {
        return 0;
    }
    if (SLMirStmtLowerAppendInst(
            &c,
            SLMirOp_RETURN_VOID,
            0,
            0,
            ast->nodes[bodyNode].start,
            ast->nodes[bodyNode].end,
            NULL)
        != 0)
    {
        return -1;
    }
    if (SLMirProgramBuilderEndFunction(&c.builder) != 0) {
        SLMirLowerStmtSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *builder = c.builder;
    *outFunctionIndex = c.functionIndex;
    *outSupported = 1;
    return 0;
}

int SLMirLowerAppendSimpleFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    return SLMirLowerAppendSimpleFunctionWithOptions(
        builder, arena, ast, src, fnNode, bodyNode, NULL, outFunctionIndex, outSupported, diag);
}

int SLMirLowerSimpleFunctionWithOptions(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    const SLMirLowerOptions* _Nullable options,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirProgramBuilder builder;
    uint32_t            functionIndex = UINT32_MAX;
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (outProgram == NULL || outSupported == NULL || arena == NULL || ast == NULL) {
        SLMirLowerStmtSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outProgram = (SLMirProgram){ 0 };
    SLMirProgramBuilderInit(&builder, arena);
    if (SLMirLowerAppendSimpleFunctionWithOptions(
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
    SLMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

int SLMirLowerSimpleFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   fnNode,
    int32_t   bodyNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    return SLMirLowerSimpleFunctionWithOptions(
        arena, ast, src, fnNode, bodyNode, NULL, outProgram, outSupported, diag);
}

SL_API_END
