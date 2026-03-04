#include "libsl-impl.h"
#include "mir.h"

SL_API_BEGIN

typedef struct {
    const SLAst* ast;
    SLMirInst*   v;
    uint32_t     len;
    uint32_t     cap;
    int          supported;
    SLDiag*      diag;
} SLMirBuilder;

static void SLMirSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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

static int SLMirIsAllowedUnaryToken(SLTokenKind tok) {
    return tok == SLTok_ADD || tok == SLTok_SUB || tok == SLTok_NOT;
}

static int SLMirIsAllowedBinaryToken(SLTokenKind tok) {
    switch (tok) {
        case SLTok_ADD:
        case SLTok_SUB:
        case SLTok_MUL:
        case SLTok_DIV:
        case SLTok_MOD:
        case SLTok_AND:
        case SLTok_OR:
        case SLTok_XOR:
        case SLTok_LSHIFT:
        case SLTok_RSHIFT:
        case SLTok_EQ:
        case SLTok_NEQ:
        case SLTok_LT:
        case SLTok_GT:
        case SLTok_LTE:
        case SLTok_GTE:
        case SLTok_LOGICAL_AND:
        case SLTok_LOGICAL_OR:  return 1;
        default:                return 0;
    }
}

static int SLMirEmitInst(
    SLMirBuilder* b, SLMirOp op, SLTokenKind tok, uint32_t start, uint32_t end) {
    if (!b->supported) {
        return 0;
    }
    if (b->len >= b->cap) {
        SLMirSetDiag(b->diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }
    b->v[b->len].op = op;
    b->v[b->len].tok = (uint16_t)tok;
    b->v[b->len]._reserved = 0;
    b->v[b->len].start = start;
    b->v[b->len].end = end;
    b->len++;
    return 0;
}

static int SLMirBuildExprNode(SLMirBuilder* b, int32_t nodeId) {
    const SLAstNode* n;
    if (!b->supported) {
        return 0;
    }
    if (nodeId < 0 || (uint32_t)nodeId >= b->ast->len) {
        SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, 0, 0);
        return -1;
    }
    n = &b->ast->nodes[nodeId];
    switch (n->kind) {
        case SLAst_INT:
            return SLMirEmitInst(b, SLMirOp_PUSH_INT, SLTok_INT, n->dataStart, n->dataEnd);
        case SLAst_RUNE:
            return SLMirEmitInst(b, SLMirOp_PUSH_INT, SLTok_RUNE, n->dataStart, n->dataEnd);
        case SLAst_FLOAT:
            return SLMirEmitInst(b, SLMirOp_PUSH_FLOAT, SLTok_FLOAT, n->dataStart, n->dataEnd);
        case SLAst_BOOL:
            return SLMirEmitInst(b, SLMirOp_PUSH_BOOL, SLTok_TRUE, n->dataStart, n->dataEnd);
        case SLAst_STRING:
            return SLMirEmitInst(b, SLMirOp_PUSH_STRING, SLTok_STRING, n->dataStart, n->dataEnd);
        case SLAst_NULL: return SLMirEmitInst(b, SLMirOp_PUSH_NULL, SLTok_NULL, n->start, n->end);
        case SLAst_IDENT:
            return SLMirEmitInst(b, SLMirOp_LOAD_IDENT, SLTok_IDENT, n->dataStart, n->dataEnd);
        case SLAst_CALL: {
            int32_t  callee = b->ast->nodes[nodeId].firstChild;
            int32_t  arg;
            uint32_t argc = 0;
            if (callee < 0 || b->ast->nodes[callee].kind != SLAst_IDENT) {
                b->supported = 0;
                return 0;
            }
            arg = b->ast->nodes[callee].nextSibling;
            while (arg >= 0) {
                int32_t exprNode = arg;
                if (b->ast->nodes[arg].kind == SLAst_CALL_ARG) {
                    exprNode = b->ast->nodes[arg].firstChild;
                    if (exprNode < 0) {
                        b->supported = 0;
                        return 0;
                    }
                }
                if (SLMirBuildExprNode(b, exprNode) != 0) {
                    return -1;
                }
                if (argc == UINT16_MAX) {
                    b->supported = 0;
                    return 0;
                }
                argc++;
                arg = b->ast->nodes[arg].nextSibling;
            }
            return SLMirEmitInst(
                b,
                SLMirOp_CALL,
                (SLTokenKind)argc,
                b->ast->nodes[callee].dataStart,
                b->ast->nodes[callee].dataEnd);
        }
        case SLAst_UNARY: {
            int32_t child = b->ast->nodes[nodeId].firstChild;
            if (!SLMirIsAllowedUnaryToken((SLTokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (child < 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (SLMirBuildExprNode(b, child) != 0) {
                return -1;
            }
            return SLMirEmitInst(b, SLMirOp_UNARY, (SLTokenKind)n->op, n->start, n->end);
        }
        case SLAst_BINARY: {
            int32_t lhs = b->ast->nodes[nodeId].firstChild;
            int32_t rhs = lhs >= 0 ? b->ast->nodes[lhs].nextSibling : -1;
            if (!SLMirIsAllowedBinaryToken((SLTokenKind)n->op)) {
                b->supported = 0;
                return 0;
            }
            if (lhs < 0 || rhs < 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (SLMirBuildExprNode(b, lhs) != 0 || SLMirBuildExprNode(b, rhs) != 0) {
                return -1;
            }
            return SLMirEmitInst(b, SLMirOp_BINARY, (SLTokenKind)n->op, n->start, n->end);
        }
        case SLAst_INDEX: {
            int32_t baseNode = b->ast->nodes[nodeId].firstChild;
            int32_t idxNode = baseNode >= 0 ? b->ast->nodes[baseNode].nextSibling : -1;
            int32_t extraNode = idxNode >= 0 ? b->ast->nodes[idxNode].nextSibling : -1;
            if ((n->flags & SLAstFlag_INDEX_SLICE) != 0) {
                b->supported = 0;
                return 0;
            }
            if (baseNode < 0 || idxNode < 0 || extraNode >= 0) {
                SLMirSetDiag(b->diag, SLDiag_EXPECTED_EXPR, n->start, n->end);
                return -1;
            }
            if (SLMirBuildExprNode(b, baseNode) != 0 || SLMirBuildExprNode(b, idxNode) != 0) {
                return -1;
            }
            return SLMirEmitInst(b, SLMirOp_INDEX, SLTok_INVALID, n->start, n->end);
        }
        default: b->supported = 0; return 0;
    }
}

int SLMirBuildExpr(
    SLArena*     arena,
    const SLAst* ast,
    int32_t      nodeId,
    SLMirChunk*  outChunk,
    int*         outSupported,
    SLDiag* _Nullable diag) {
    SLMirBuilder b;
    uint32_t     cap;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (outChunk == NULL || outSupported == NULL || arena == NULL || ast == NULL
        || ast->nodes == NULL)
    {
        SLMirSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }

    outChunk->v = NULL;
    outChunk->len = 0;
    *outSupported = 0;

    cap = ast->len * 4u + 8u;
    b.v = (SLMirInst*)SLArenaAlloc(
        arena, cap * (uint32_t)sizeof(SLMirInst), (uint32_t)_Alignof(SLMirInst));
    if (b.v == NULL) {
        SLMirSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    b.ast = ast;
    b.len = 0;
    b.cap = cap;
    b.supported = 1;
    b.diag = diag;

    if (SLMirBuildExprNode(&b, nodeId) != 0) {
        return -1;
    }

    if (b.supported) {
        uint32_t start = 0;
        uint32_t end = 0;
        if (nodeId >= 0 && (uint32_t)nodeId < ast->len) {
            start = ast->nodes[nodeId].start;
            end = ast->nodes[nodeId].end;
        }
        if (SLMirEmitInst(&b, SLMirOp_RETURN, SLTok_EOF, start, end) != 0) {
            return -1;
        }
        outChunk->v = b.v;
        outChunk->len = b.len;
        *outSupported = 1;
    }

    return 0;
}

SL_API_END
