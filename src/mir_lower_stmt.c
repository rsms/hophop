#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_lower_stmt.h"

SL_API_BEGIN

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t slot;
    uint8_t  mutable;
    uint8_t  _reserved[3];
} SLMirLowerLocal;

typedef struct {
    SLArena*            arena;
    const SLAst*        ast;
    SLStrView           src;
    SLMirProgramBuilder builder;
    uint32_t            functionIndex;
    uint32_t            localCount;
    SLMirLowerLocal*    locals;
    uint32_t            localLen;
    uint32_t            localCap;
    int                 supported;
    SLDiag*             diag;
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
    SLMirStmtLower* c, uint32_t nameStart, uint32_t nameEnd, int mutable, uint32_t* outSlot) {
    if (SLMirStmtLowerEnsureLocalCap(c, c->localLen + 1u) != 0) {
        return -1;
    }
    c->locals[c->localLen].nameStart = nameStart;
    c->locals[c->localLen].nameEnd = nameEnd;
    c->locals[c->localLen].slot = c->localCount;
    c->locals[c->localLen].mutable = mutable ? 1u : 0u;
    c->locals[c->localLen]._reserved[0] = 0;
    c->locals[c->localLen]._reserved[1] = 0;
    c->locals[c->localLen]._reserved[2] = 0;
    if (outSlot != NULL) {
        *outSlot = c->localCount;
    }
    c->localLen++;
    c->localCount++;
    return 0;
}

static int SLMirStmtLowerFindLocal(
    const SLMirStmtLower* c,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    uint32_t*             outSlot,
    int* _Nullable outMutable) {
    uint32_t i = c->localLen;
    while (i > 0) {
        const SLMirLowerLocal* local;
        i--;
        local = &c->locals[i];
        if (local->nameStart == nameStart && local->nameEnd == nameEnd) {
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

static int SLMirStmtLowerRewriteExprChunk(SLMirStmtLower* c, const SLMirChunk* chunk) {
    uint32_t i;
    for (i = 0; i < chunk->len; i++) {
        SLMirInst inst = chunk->v[i];
        if (inst.op == SLMirOp_CALL && inst.aux != 0) {
            c->supported = 0;
            return 0;
        }
        if (inst.op == SLMirOp_LOAD_IDENT) {
            uint32_t slot = 0;
            if (SLMirStmtLowerFindLocal(c, inst.start, inst.end, &slot, NULL)) {
                inst.op = SLMirOp_LOCAL_LOAD;
                inst.tok = 0;
                inst.aux = slot;
            }
        }
        if (SLMirLowerAppendInst(&c->builder, c->arena, c->src, &inst, c->diag) != 0) {
            return -1;
        }
    }
    return 0;
}

static int SLMirStmtLowerIsSafeIfCondExpr(const SLAst* ast, int32_t exprNode) {
    if (ast == NULL || exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return 0;
    }
    switch (ast->nodes[exprNode].kind) {
        case SLAst_BOOL:
        case SLAst_UNARY:
        case SLAst_BINARY: return 1;
        default:           return 0;
    }
}

static int SLMirStmtLowerExpr(SLMirStmtLower* c, int32_t exprNode) {
    SLMirChunk chunk = { 0 };
    int        supported = 0;
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

static int32_t SLMirStmtLowerVarInitExprNode(const SLAst* ast, int32_t nodeId) {
    int32_t firstChild;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0) {
        return -1;
    }
    if (ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
        return -1;
    }
    if (ast->nodes[firstChild].kind == SLAst_TYPE_NAME
        || ast->nodes[firstChild].kind == SLAst_TYPE_PTR
        || ast->nodes[firstChild].kind == SLAst_TYPE_REF
        || ast->nodes[firstChild].kind == SLAst_TYPE_MUTREF
        || ast->nodes[firstChild].kind == SLAst_TYPE_ARRAY
        || ast->nodes[firstChild].kind == SLAst_TYPE_VARRAY
        || ast->nodes[firstChild].kind == SLAst_TYPE_SLICE
        || ast->nodes[firstChild].kind == SLAst_TYPE_MUTSLICE
        || ast->nodes[firstChild].kind == SLAst_TYPE_OPTIONAL
        || ast->nodes[firstChild].kind == SLAst_TYPE_FN
        || ast->nodes[firstChild].kind == SLAst_TYPE_TUPLE
        || ast->nodes[firstChild].kind == SLAst_TYPE_ANON_STRUCT
        || ast->nodes[firstChild].kind == SLAst_TYPE_ANON_UNION)
    {
        return ast->nodes[firstChild].nextSibling;
    }
    return firstChild;
}

static int SLMirStmtLowerStmt(SLMirStmtLower* c, int32_t stmtNode);

static int SLMirStmtLowerBlock(SLMirStmtLower* c, int32_t blockNode) {
    uint32_t scopeMark = c->localLen;
    int32_t  child;
    if (blockNode < 0 || (uint32_t)blockNode >= c->ast->len
        || c->ast->nodes[blockNode].kind != SLAst_BLOCK)
    {
        c->supported = 0;
        return 0;
    }
    child = c->ast->nodes[blockNode].firstChild;
    while (child >= 0) {
        if (SLMirStmtLowerStmt(c, child) != 0 || !c->supported) {
            c->localLen = scopeMark;
            return c->supported ? -1 : 0;
        }
        child = c->ast->nodes[child].nextSibling;
    }
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
    if (!SLMirStmtLowerIsSafeIfCondExpr(c->ast, condNode)) {
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

static int SLMirStmtLowerExprStmt(SLMirStmtLower* c, int32_t stmtNode) {
    int32_t exprNode = c->ast->nodes[stmtNode].firstChild;
    if (exprNode < 0 || c->ast->nodes[exprNode].nextSibling >= 0) {
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
    }
    if (SLMirStmtLowerExpr(c, exprNode) != 0 || !c->supported) {
        return c->supported ? -1 : 0;
    }
    return SLMirStmtLowerAppendInst(
        c, SLMirOp_DROP, 0, 0, c->ast->nodes[stmtNode].start, c->ast->nodes[stmtNode].end, NULL);
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
        case SLAst_RETURN: {
            int32_t exprNode = s->firstChild;
            if (exprNode < 0) {
                return SLMirStmtLowerAppendInst(
                    c, SLMirOp_RETURN_VOID, 0, 0, s->start, s->end, NULL);
            }
            if (c->ast->nodes[exprNode].nextSibling >= 0) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, exprNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(c, SLMirOp_RETURN, 0, 0, s->start, s->end, NULL);
        }
        case SLAst_EXPR_STMT: return SLMirStmtLowerExprStmt(c, stmtNode);
        case SLAst_VAR:
        case SLAst_CONST:     {
            int32_t  firstChild = s->firstChild;
            int32_t  initNode = SLMirStmtLowerVarInitExprNode(c->ast, stmtNode);
            uint32_t slot = 0;
            if (firstChild < 0 || (c->ast->nodes[firstChild].kind == SLAst_NAME_LIST)
                || initNode < 0)
            {
                c->supported = 0;
                return 0;
            }
            if (c->ast->nodes[firstChild].kind == SLAst_TYPE_NAME
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_PTR
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_REF
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_MUTREF
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_ARRAY
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_VARRAY
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_SLICE
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_MUTSLICE
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_OPTIONAL
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_FN
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_TUPLE
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_ANON_STRUCT
                || c->ast->nodes[firstChild].kind == SLAst_TYPE_ANON_UNION)
            {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerPushLocal(c, s->dataStart, s->dataEnd, s->kind == SLAst_VAR, &slot)
                != 0)
            {
                return -1;
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
    SLMirStmtLower c;
    SLMirFunction  fn = { 0 };
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
    c.builder = *builder;

    fn.nameStart = fnNode >= 0 && (uint32_t)fnNode < ast->len ? ast->nodes[fnNode].dataStart : 0;
    fn.nameEnd = fnNode >= 0 && (uint32_t)fnNode < ast->len ? ast->nodes[fnNode].dataEnd : 0;
    if (SLMirProgramBuilderBeginFunction(&c.builder, &fn, &c.functionIndex) != 0) {
        SLMirLowerStmtSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    if (fnNode >= 0 && (uint32_t)fnNode < ast->len) {
        child = ast->nodes[fnNode].firstChild;
        while (child >= 0) {
            if (ast->nodes[child].kind == SLAst_PARAM) {
                uint32_t slot = 0;
                if (SLMirStmtLowerPushLocal(
                        &c, ast->nodes[child].dataStart, ast->nodes[child].dataEnd, 1, &slot)
                    != 0)
                {
                    return -1;
                }
                c.builder.funcs[c.functionIndex].paramCount++;
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
    c.builder.funcs[c.functionIndex].localCount = c.localCount;
    if (SLMirProgramBuilderEndFunction(&c.builder) != 0) {
        SLMirLowerStmtSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *builder = c.builder;
    *outFunctionIndex = c.functionIndex;
    *outSupported = 1;
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
    if (SLMirLowerAppendSimpleFunction(
            &builder, arena, ast, src, fnNode, bodyNode, &functionIndex, outSupported, diag)
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

SL_API_END
