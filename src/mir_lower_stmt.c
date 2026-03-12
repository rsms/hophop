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
    uint32_t breakJumpStart;
    uint32_t continueJumpStart;
    uint32_t loopStartPc;
} SLMirLowerLoop;

typedef struct {
    SLArena*            arena;
    const SLAst*        ast;
    SLStrView           src;
    SLMirProgramBuilder builder;
    uint32_t            functionIndex;
    SLMirLowerLocal*    locals;
    uint32_t            localLen;
    uint32_t            localCap;
    uint32_t            breakJumps[256];
    uint32_t            breakJumpLen;
    uint32_t            continueJumps[256];
    uint32_t            continueJumpLen;
    SLMirLowerLoop      loops[32];
    uint32_t            loopLen;
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
    uint32_t*       outSlot) {
    SLMirLocal   local = { 0 };
    SLMirTypeRef typeRef = { 0 };
    uint32_t     slot = 0;
    if (SLMirStmtLowerEnsureLocalCap(c, c->localLen + 1u) != 0) {
        return -1;
    }
    local.typeRef = UINT32_MAX;
    local.flags = mutable ? SLMirLocalFlag_MUTABLE : SLMirLocalFlag_NONE;
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
    c->locals[c->localLen].mutable = mutable ? 1u : 0u;
    c->locals[c->localLen]._reserved[0] = 0;
    c->locals[c->localLen]._reserved[1] = 0;
    c->locals[c->localLen]._reserved[2] = 0;
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

static SLMirLowerLoop* _Nullable SLMirStmtLowerCurrentLoop(SLMirStmtLower* c) {
    if (c == NULL || c->loopLen == 0) {
        return NULL;
    }
    return &c->loops[c->loopLen - 1u];
}

static int SLMirStmtLowerPushLoop(SLMirStmtLower* c, uint32_t loopStartPc) {
    if (c == NULL || c->loopLen >= (uint32_t)(sizeof(c->loops) / sizeof(c->loops[0]))) {
        if (c != NULL) {
            c->supported = 0;
        }
        return 0;
    }
    c->loops[c->loopLen++] = (SLMirLowerLoop){
        .breakJumpStart = c->breakJumpLen,
        .continueJumpStart = c->continueJumpLen,
        .loopStartPc = loopStartPc,
    };
    return 1;
}

static int SLMirStmtLowerRecordLoopJump(SLMirStmtLower* c, int isContinue, uint32_t instIndex) {
    if (c == NULL || c->loopLen == 0) {
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

static void SLMirStmtLowerFinishLoop(
    SLMirStmtLower* c, uint32_t continueTargetPc, uint32_t breakTargetPc) {
    SLMirLowerLoop loop;
    if (c == NULL || c->loopLen == 0) {
        return;
    }
    loop = c->loops[c->loopLen - 1u];
    SLMirStmtLowerPatchJumpRange(
        c, c->continueJumps, loop.continueJumpStart, c->continueJumpLen, continueTargetPc);
    SLMirStmtLowerPatchJumpRange(
        c, c->breakJumps, loop.breakJumpStart, c->breakJumpLen, breakTargetPc);
    c->continueJumpLen = loop.continueJumpStart;
    c->breakJumpLen = loop.breakJumpStart;
    c->loopLen--;
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

static int SLMirStmtLowerExpr(SLMirStmtLower* c, int32_t exprNode) {
    const SLAstNode* expr;
    SLMirChunk       chunk = { 0 };
    int              supported = 0;
    if (exprNode < 0 || (uint32_t)exprNode >= c->ast->len) {
        c->supported = 0;
        return 0;
    }
    expr = &c->ast->nodes[exprNode];
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
                if ((SLTokenKind)expr->op == SLTok_MUL) {
                    if (SLMirStmtLowerAppendInst(
                            c,
                            SLMirOp_LOCAL_LOAD,
                            0,
                            slot,
                            c->ast->nodes[child].start,
                            c->ast->nodes[child].end,
                            NULL)
                        != 0)
                    {
                        return -1;
                    }
                    return SLMirStmtLowerAppendInst(
                        c, SLMirOp_DEREF_LOAD, 0, 0, expr->start, expr->end, NULL);
                }
            }
        }
        if ((SLTokenKind)expr->op == SLTok_AND && child >= 0 && (uint32_t)child < c->ast->len
            && c->ast->nodes[child].kind == SLAst_FIELD_EXPR)
        {
            int32_t baseNode = c->ast->nodes[child].firstChild;
            if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
                return c->supported ? -1 : 0;
            }
            return SLMirStmtLowerAppendInst(
                c,
                SLMirOp_AGG_ADDR,
                0,
                0,
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
    }
    if (expr->kind == SLAst_FIELD_EXPR) {
        int32_t baseNode = expr->firstChild;
        if (baseNode < 0 || (uint32_t)baseNode >= c->ast->len) {
            c->supported = 0;
            return 0;
        }
        if (SLMirStmtLowerExpr(c, baseNode) != 0 || !c->supported) {
            return c->supported ? -1 : 0;
        }
        return SLMirStmtLowerAppendInst(
            c, SLMirOp_AGG_GET, 0, 0, expr->dataStart, expr->dataEnd, NULL);
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
    if (SLMirStmtLowerIsTypeNodeKind(ast->nodes[firstChild].kind)) {
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
            && c->ast->nodes[lhsNode].kind == SLAst_UNARY
            && (SLTokenKind)c->ast->nodes[lhsNode].op == SLTok_MUL)
        {
            int32_t derefBase = c->ast->nodes[lhsNode].firstChild;
            if (derefBase >= 0 && (uint32_t)derefBase < c->ast->len
                && c->ast->nodes[derefBase].kind == SLAst_IDENT
                && SLMirStmtLowerFindLocal(
                    c,
                    c->ast->nodes[derefBase].dataStart,
                    c->ast->nodes[derefBase].dataEnd,
                    &slot,
                    NULL))
            {
                if ((SLTokenKind)expr->op == SLTok_ASSIGN) {
                    if (SLMirStmtLowerExpr(c, rhsNode) != 0 || !c->supported) {
                        return c->supported ? -1 : 0;
                    }
                    if (SLMirStmtLowerAppendInst(
                            c,
                            SLMirOp_LOCAL_LOAD,
                            0,
                            slot,
                            c->ast->nodes[derefBase].start,
                            c->ast->nodes[derefBase].end,
                            NULL)
                        != 0)
                    {
                        return -1;
                    }
                    return SLMirStmtLowerAppendInst(
                        c, SLMirOp_DEREF_STORE, 0, 0, expr->start, expr->end, NULL);
                }
                if (!SLMirStmtLowerBinaryOpForAssign((SLTokenKind)expr->op, &binaryTok)) {
                    c->supported = 0;
                    return 0;
                }
                if (SLMirStmtLowerAppendInst(
                        c,
                        SLMirOp_LOCAL_LOAD,
                        0,
                        slot,
                        c->ast->nodes[derefBase].start,
                        c->ast->nodes[derefBase].end,
                        NULL)
                        != 0
                    || SLMirStmtLowerAppendInst(
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
                        != 0
                    || SLMirStmtLowerAppendInst(
                           c,
                           SLMirOp_LOCAL_LOAD,
                           0,
                           slot,
                           c->ast->nodes[derefBase].start,
                           c->ast->nodes[derefBase].end,
                           NULL)
                           != 0)
                {
                    return -1;
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
            int32_t baseNode = c->ast->nodes[lhsNode].firstChild;
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
            if (SLMirStmtLowerAppendInst(
                    c,
                    SLMirOp_AGG_ADDR,
                    0,
                    0,
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
        c->supported = 0;
        return 0;
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
    if (!SLMirStmtLowerPushLoop(c, loopStartPc)) {
        c->localLen = scopeMark;
        return 0;
    }
    if (condNode >= 0) {
        if (SLMirStmtLowerExpr(c, condNode) != 0 || !c->supported) {
            c->localLen = scopeMark;
            c->loopLen--;
            return c->supported ? -1 : 0;
        }
        if (SLMirStmtLowerAppendInst(
                c, SLMirOp_JUMP_IF_FALSE, 0, UINT32_MAX, s->start, s->end, &condFalseJump)
            != 0)
        {
            c->localLen = scopeMark;
            c->loopLen--;
            return -1;
        }
    }
    if (SLMirStmtLowerBlock(c, bodyNode) != 0 || !c->supported) {
        c->localLen = scopeMark;
        c->loopLen--;
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
        c->loopLen--;
        return c->supported ? -1 : 0;
    }
    if (SLMirStmtLowerAppendInst(c, SLMirOp_JUMP, 0, loopStartPc, s->start, s->end, NULL) != 0) {
        c->localLen = scopeMark;
        c->loopLen--;
        return -1;
    }
    loopEndPc = SLMirStmtLowerFnPc(c);
    if (condFalseJump != UINT32_MAX) {
        c->builder.insts[condFalseJump].aux = loopEndPc;
    }
    SLMirStmtLowerFinishLoop(c, continueTargetPc, loopEndPc);
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
        case SLAst_BLOCK: return SLMirStmtLowerBlock(c, stmtNode);
        case SLAst_IF:    return SLMirStmtLowerIf(c, stmtNode);
        case SLAst_FOR:   return SLMirStmtLowerFor(c, stmtNode);
        case SLAst_BREAK: {
            uint32_t        jumpInst = UINT32_MAX;
            SLMirLowerLoop* loop = SLMirStmtLowerCurrentLoop(c);
            if (loop == NULL) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerAppendInst(
                    c, SLMirOp_JUMP, 0, UINT32_MAX, s->start, s->end, &jumpInst)
                != 0)
            {
                return -1;
            }
            if (!SLMirStmtLowerRecordLoopJump(c, 0, jumpInst)) {
                return 0;
            }
            return 0;
        }
        case SLAst_CONTINUE: {
            uint32_t        jumpInst = UINT32_MAX;
            SLMirLowerLoop* loop = SLMirStmtLowerCurrentLoop(c);
            if (loop == NULL) {
                c->supported = 0;
                return 0;
            }
            if (SLMirStmtLowerAppendInst(
                    c, SLMirOp_JUMP, 0, loop->loopStartPc, s->start, s->end, &jumpInst)
                != 0)
            {
                return -1;
            }
            if (!SLMirStmtLowerRecordLoopJump(c, 1, jumpInst)) {
                return 0;
            }
            return 0;
        }
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
            int32_t  typeNode = -1;
            int32_t  initNode = SLMirStmtLowerVarInitExprNode(c->ast, stmtNode);
            uint32_t slot = 0;
            if (firstChild < 0 || c->ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
                c->supported = 0;
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
    c.builder = *builder;
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
                        &slot)
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
