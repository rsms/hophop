#include "libsl-impl.h"

SL_API_BEGIN

static void SLPSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->start = start;
    diag->end = end;
}

typedef struct {
    SLStrView      src;
    const SLToken* tok;
    uint32_t       tokLen;
    uint32_t       pos;
    SLASTNode*     nodes;
    uint32_t       nodeLen;
    uint32_t       nodeCap;
    SLDiag*        diag;
} SLParser;

static const SLToken* SLPPeek(SLParser* p) {
    if (p->pos >= p->tokLen) {
        return &p->tok[p->tokLen - 1];
    }
    return &p->tok[p->pos];
}

static const SLToken* SLPPrev(SLParser* p) {
    if (p->pos == 0) {
        return &p->tok[0];
    }
    return &p->tok[p->pos - 1];
}

static int SLPAt(SLParser* p, SLTokenKind kind) {
    return SLPPeek(p)->kind == kind;
}

static int SLPMatch(SLParser* p, SLTokenKind kind) {
    if (!SLPAt(p, kind)) {
        return 0;
    }
    p->pos++;
    return 1;
}

static int SLPFail(SLParser* p, SLDiagCode code) {
    const SLToken* t = SLPPeek(p);
    SLPSetDiag(p->diag, code, t->start, t->end);
    return -1;
}

static int SLPExpect(SLParser* p, SLTokenKind kind, SLDiagCode code, const SLToken** out) {
    if (!SLPAt(p, kind)) {
        return SLPFail(p, code);
    }
    *out = SLPPeek(p);
    p->pos++;
    return 0;
}

static int32_t SLPNewNode(SLParser* p, SLASTKind kind, uint32_t start, uint32_t end) {
    int32_t idx;
    if (p->nodeLen >= p->nodeCap) {
        SLPSetDiag(p->diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }
    idx = (int32_t)p->nodeLen++;
    p->nodes[idx].kind = kind;
    p->nodes[idx].start = start;
    p->nodes[idx].end = end;
    p->nodes[idx].firstChild = -1;
    p->nodes[idx].nextSibling = -1;
    p->nodes[idx].dataStart = 0;
    p->nodes[idx].dataEnd = 0;
    p->nodes[idx].op = 0;
    p->nodes[idx].flags = 0;
    return idx;
}

static int SLPAddChild(SLParser* p, int32_t parent, int32_t child) {
    int32_t n;
    if (parent < 0 || child < 0) {
        return -1;
    }
    if (p->nodes[parent].firstChild < 0) {
        p->nodes[parent].firstChild = child;
        return 0;
    }
    n = p->nodes[parent].firstChild;
    while (p->nodes[n].nextSibling >= 0) {
        n = p->nodes[n].nextSibling;
    }
    p->nodes[n].nextSibling = child;
    return 0;
}

static int SLIsAssignmentOp(SLTokenKind kind) {
    switch (kind) {
        case SLTok_ASSIGN:
        case SLTok_ADD_ASSIGN:
        case SLTok_SUB_ASSIGN:
        case SLTok_MUL_ASSIGN:
        case SLTok_DIV_ASSIGN:
        case SLTok_MOD_ASSIGN:
        case SLTok_AND_ASSIGN:
        case SLTok_OR_ASSIGN:
        case SLTok_XOR_ASSIGN:
        case SLTok_LSHIFT_ASSIGN:
        case SLTok_RSHIFT_ASSIGN: return 1;
        default:                  return 0;
    }
}

static int SLBinPrec(SLTokenKind kind) {
    if (SLIsAssignmentOp(kind)) {
        return 1;
    }
    switch (kind) {
        case SLTok_LOGICAL_OR:  return 2;
        case SLTok_LOGICAL_AND: return 3;
        case SLTok_OR:          return 4;
        case SLTok_XOR:         return 5;
        case SLTok_AND:         return 6;
        case SLTok_EQ:
        case SLTok_NEQ:         return 7;
        case SLTok_LT:
        case SLTok_GT:
        case SLTok_LTE:
        case SLTok_GTE:         return 8;
        case SLTok_LSHIFT:
        case SLTok_RSHIFT:      return 9;
        case SLTok_ADD:
        case SLTok_SUB:         return 10;
        case SLTok_MUL:
        case SLTok_DIV:
        case SLTok_MOD:         return 11;
        default:                return 0;
    }
}

static int SLPParseType(SLParser* p, int32_t* out);
static int SLPParseExpr(SLParser* p, int minPrec, int32_t* out);
static int SLPParseStmt(SLParser* p, int32_t* out);
static int SLPParseDecl(SLParser* p, int allowBody, int32_t* out);
static int SLPParseDeclInner(SLParser* p, int allowBody, int32_t* out);
static int SLPParseSwitchStmt(SLParser* p, int32_t* out);

static int SLPParseTypeName(SLParser* p, int32_t* out) {
    const SLToken* first;
    const SLToken* last;
    int32_t        n;

    if (SLPExpect(p, SLTok_IDENT, SLDiag_EXPECTED_TYPE, &first) != 0) {
        return -1;
    }
    last = first;
    while ((p->pos + 1u) < p->tokLen && p->tok[p->pos].kind == SLTok_DOT
           && p->tok[p->pos + 1u].kind == SLTok_IDENT && p->tok[p->pos].start == last->end
           && p->tok[p->pos + 1u].start == p->tok[p->pos].end)
    {
        p->pos++;
        last = &p->tok[p->pos];
        p->pos++;
    }

    n = SLPNewNode(p, SLAST_TYPE_NAME, first->start, last->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = first->start;
    p->nodes[n].dataEnd = last->end;
    *out = n;
    return 0;
}

static int SLPParseType(SLParser* p, int32_t* out) {
    const SLToken* t;
    int32_t        typeNode;
    int32_t        child;

    if (SLPMatch(p, SLTok_MUL)) {
        t = SLPPrev(p);
        typeNode = SLPNewNode(p, SLAST_TYPE_PTR, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (SLPParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (SLPMatch(p, SLTok_AND)) {
        t = SLPPrev(p);
        typeNode = SLPNewNode(p, SLAST_TYPE_REF, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (SLPParseType(p, &child) != 0) {
            return -1;
        }
        if (p->nodes[child].kind == SLAST_TYPE_SLICE || p->nodes[child].kind == SLAST_TYPE_MUTSLICE)
        {
            return SLPFail(p, SLDiag_EXPECTED_TYPE);
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (SLPMatch(p, SLTok_MUT)) {
        const SLToken* mutTok = SLPPrev(p);
        if (SLPMatch(p, SLTok_AND)) {
            typeNode = SLPNewNode(p, SLAST_TYPE_MUTREF, mutTok->start, mutTok->end);
            if (typeNode < 0) {
                return -1;
            }
            if (SLPParseType(p, &child) != 0) {
                return -1;
            }
            if (p->nodes[child].kind == SLAST_TYPE_SLICE
                || p->nodes[child].kind == SLAST_TYPE_MUTSLICE)
            {
                return SLPFail(p, SLDiag_EXPECTED_TYPE);
            }
            p->nodes[typeNode].end = p->nodes[child].end;
            return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
        }
        if (SLPMatch(p, SLTok_LBRACK)) {
            const SLToken* rb;
            typeNode = SLPNewNode(p, SLAST_TYPE_MUTSLICE, mutTok->start, mutTok->end);
            if (typeNode < 0) {
                return -1;
            }
            if (SLPParseType(p, &child) != 0) {
                return -1;
            }
            if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_TYPE, &rb) != 0) {
                return -1;
            }
            p->nodes[typeNode].end = rb->end;
            return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
        }
        return SLPFail(p, SLDiag_EXPECTED_TYPE);
    }

    if (SLPMatch(p, SLTok_LBRACK)) {
        const SLToken* lb = SLPPrev(p);
        const SLToken* nTok = NULL;
        const SLToken* rb;
        SLASTKind      kind;

        if (SLPParseType(p, &child) != 0) {
            return -1;
        }

        if (SLPMatch(p, SLTok_RBRACK)) {
            rb = SLPPrev(p);
            typeNode = SLPNewNode(p, SLAST_TYPE_SLICE, lb->start, rb->end);
            if (typeNode < 0) {
                return -1;
            }
            return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
        }

        if (SLPMatch(p, SLTok_DOT)) {
            kind = SLAST_TYPE_VARRAY;
            if (SLPExpect(p, SLTok_IDENT, SLDiag_EXPECTED_TYPE, &nTok) != 0) {
                return -1;
            }
        } else {
            kind = SLAST_TYPE_ARRAY;
            if (SLPExpect(p, SLTok_INT, SLDiag_EXPECTED_TYPE, &nTok) != 0) {
                return -1;
            }
        }

        typeNode = SLPNewNode(p, kind, lb->start, lb->end);
        if (typeNode < 0) {
            return -1;
        }
        p->nodes[typeNode].dataStart = nTok->start;
        p->nodes[typeNode].dataEnd = nTok->end;
        if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = rb->end;
        return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    return SLPParseTypeName(p, out);
}

static int SLPParsePrimary(SLParser* p, int32_t* out) {
    const SLToken* t = SLPPeek(p);
    int32_t        n;

    if (SLPMatch(p, SLTok_IDENT)) {
        n = SLPNewNode(p, SLAST_IDENT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_INT)) {
        n = SLPNewNode(p, SLAST_INT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_FLOAT)) {
        n = SLPNewNode(p, SLAST_FLOAT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_STRING)) {
        n = SLPNewNode(p, SLAST_STRING, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_TRUE) || SLPMatch(p, SLTok_FALSE)) {
        t = SLPPrev(p);
        n = SLPNewNode(p, SLAST_BOOL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_LPAREN)) {
        if (SLPParseExpr(p, 1, out) != 0) {
            return -1;
        }
        if (SLPExpect(p, SLTok_RPAREN, SLDiag_EXPECTED_EXPR, &t) != 0) {
            return -1;
        }
        return 0;
    }

    if (SLPMatch(p, SLTok_SIZEOF)) {
        const SLToken* kw = SLPPrev(p);
        const SLToken* rp;
        int32_t        inner;
        uint32_t       savePos;
        uint32_t       saveNodeLen;
        if (SLPExpect(p, SLTok_LPAREN, SLDiag_EXPECTED_EXPR, &t) != 0) {
            return -1;
        }
        n = SLPNewNode(p, SLAST_SIZEOF, kw->start, t->end);
        if (n < 0) {
            return -1;
        }

        savePos = p->pos;
        saveNodeLen = p->nodeLen;
        if (SLPParseType(p, &inner) == 0 && SLPAt(p, SLTok_RPAREN)) {
            p->nodes[n].flags = 1;
            if (SLPAddChild(p, n, inner) != 0) {
                return -1;
            }
            if (SLPExpect(p, SLTok_RPAREN, SLDiag_EXPECTED_EXPR, &rp) != 0) {
                return -1;
            }
            p->nodes[n].end = rp->end;
            *out = n;
            return 0;
        }
        p->pos = savePos;
        p->nodeLen = saveNodeLen;
        SLDiagClear(p->diag);

        if (SLPParseExpr(p, 1, &inner) != 0) {
            return -1;
        }
        p->nodes[n].flags = 0;
        if (SLPAddChild(p, n, inner) != 0) {
            return -1;
        }
        if (SLPExpect(p, SLTok_RPAREN, SLDiag_EXPECTED_EXPR, &rp) != 0) {
            return -1;
        }
        p->nodes[n].end = rp->end;
        *out = n;
        return 0;
    }

    return SLPFail(p, SLDiag_EXPECTED_EXPR);
}

static int SLPParsePostfix(SLParser* p, int32_t* expr) {
    for (;;) {
        int32_t        n;
        const SLToken* t;

        if (SLPMatch(p, SLTok_LPAREN)) {
            n = SLPNewNode(p, SLAST_CALL, p->nodes[*expr].start, SLPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (SLPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (!SLPAt(p, SLTok_RPAREN)) {
                for (;;) {
                    int32_t arg;
                    if (SLPParseExpr(p, 1, &arg) != 0) {
                        return -1;
                    }
                    if (SLPAddChild(p, n, arg) != 0) {
                        return -1;
                    }
                    if (!SLPMatch(p, SLTok_COMMA)) {
                        break;
                    }
                }
            }
            if (SLPExpect(p, SLTok_RPAREN, SLDiag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (SLPMatch(p, SLTok_LBRACK)) {
            int32_t idxExpr;
            n = SLPNewNode(p, SLAST_INDEX, p->nodes[*expr].start, SLPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (SLPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (SLPParseExpr(p, 1, &idxExpr) != 0) {
                return -1;
            }
            if (SLPAddChild(p, n, idxExpr) != 0) {
                return -1;
            }
            if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (SLPMatch(p, SLTok_DOT)) {
            const SLToken* fieldTok;
            n = SLPNewNode(p, SLAST_FIELD_EXPR, p->nodes[*expr].start, SLPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (SLPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (SLPExpect(p, SLTok_IDENT, SLDiag_EXPECTED_EXPR, &fieldTok) != 0) {
                return -1;
            }
            p->nodes[n].dataStart = fieldTok->start;
            p->nodes[n].dataEnd = fieldTok->end;
            p->nodes[n].end = fieldTok->end;
            *expr = n;
            continue;
        }

        if (SLPMatch(p, SLTok_AS)) {
            int32_t typeNode;
            n = SLPNewNode(p, SLAST_CAST, p->nodes[*expr].start, SLPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)SLTok_AS;
            if (SLPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (SLPParseType(p, &typeNode) != 0) {
                return -1;
            }
            if (SLPAddChild(p, n, typeNode) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[typeNode].end;
            *expr = n;
            continue;
        }

        break;
    }
    return 0;
}

static int SLPParsePrefix(SLParser* p, int32_t* out) {
    SLTokenKind    op = SLPPeek(p)->kind;
    int32_t        rhs;
    int32_t        n;
    const SLToken* t = SLPPeek(p);

    switch (op) {
        case SLTok_ADD:
        case SLTok_SUB:
        case SLTok_NOT:
        case SLTok_MUL:
        case SLTok_AND:
            p->pos++;
            if (SLPParsePrefix(p, &rhs) != 0) {
                return -1;
            }
            n = SLPNewNode(p, SLAST_UNARY, t->start, p->nodes[rhs].end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)op;
            if (SLPAddChild(p, n, rhs) != 0) {
                return -1;
            }
            *out = n;
            return 0;
        default:
            if (SLPParsePrimary(p, out) != 0) {
                return -1;
            }
            return SLPParsePostfix(p, out);
    }
}

static int SLPParseExpr(SLParser* p, int minPrec, int32_t* out) {
    int32_t lhs;
    if (SLPParsePrefix(p, &lhs) != 0) {
        return -1;
    }

    for (;;) {
        SLTokenKind op = SLPPeek(p)->kind;
        int         prec = SLBinPrec(op);
        int         rightAssoc = SLIsAssignmentOp(op);
        int32_t     rhs;
        int32_t     n;

        if (prec < minPrec || prec == 0) {
            break;
        }
        p->pos++;
        if (SLPParseExpr(p, rightAssoc ? prec : prec + 1, &rhs) != 0) {
            return -1;
        }
        n = SLPNewNode(p, SLAST_BINARY, p->nodes[lhs].start, p->nodes[rhs].end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].op = (uint16_t)op;
        if (SLPAddChild(p, n, lhs) != 0 || SLPAddChild(p, n, rhs) != 0) {
            return -1;
        }
        lhs = n;
    }

    *out = lhs;
    return 0;
}

static int SLPParseParam(SLParser* p, int32_t* out) {
    const SLToken* name;
    int32_t        param;
    int32_t        type;
    if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    if (SLPParseType(p, &type) != 0) {
        return -1;
    }
    param = SLPNewNode(p, SLAST_PARAM, name->start, p->nodes[type].end);
    if (param < 0) {
        return -1;
    }
    p->nodes[param].dataStart = name->start;
    p->nodes[param].dataEnd = name->end;
    if (SLPAddChild(p, param, type) != 0) {
        return -1;
    }
    *out = param;
    return 0;
}

static int SLPParseBlock(SLParser* p, int32_t* out) {
    const SLToken* lb;
    const SLToken* rb;
    int32_t        block;

    if (SLPExpect(p, SLTok_LBRACE, SLDiag_UNEXPECTED_TOKEN, &lb) != 0) {
        return -1;
    }
    block = SLPNewNode(p, SLAST_BLOCK, lb->start, lb->end);
    if (block < 0) {
        return -1;
    }

    while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
        int32_t stmt = -1;
        if (SLPAt(p, SLTok_SEMICOLON)) {
            p->pos++;
            continue;
        }
        if (SLPParseStmt(p, &stmt) != 0) {
            return -1;
        }
        if (stmt >= 0 && SLPAddChild(p, block, stmt) != 0) {
            return -1;
        }
    }

    if (SLPExpect(p, SLTok_RBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[block].end = rb->end;
    *out = block;
    return 0;
}

static int SLPParseVarLikeStmt(SLParser* p, SLASTKind kind, int requireSemi, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* name;
    int32_t        n;
    int32_t        type;

    p->pos++;
    if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    if (SLPParseType(p, &type) != 0) {
        return -1;
    }

    n = SLPNewNode(p, kind, kw->start, p->nodes[type].end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;
    if (SLPAddChild(p, n, type) != 0) {
        return -1;
    }

    if (SLPMatch(p, SLTok_ASSIGN)) {
        int32_t init;
        if (SLPParseExpr(p, 1, &init) != 0) {
            return -1;
        }
        if (SLPAddChild(p, n, init) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[init].end;
    }

    if (requireSemi) {
        if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
            return -1;
        }
        p->nodes[n].end = kw->end;
    }

    *out = n;
    return 0;
}

static int SLPParseIfStmt(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    int32_t        n;
    int32_t        cond;
    int32_t        thenBlock;

    p->pos++;
    if (SLPParseExpr(p, 1, &cond) != 0) {
        return -1;
    }
    if (SLPParseBlock(p, &thenBlock) != 0) {
        return -1;
    }

    n = SLPNewNode(p, SLAST_IF, kw->start, p->nodes[thenBlock].end);
    if (n < 0) {
        return -1;
    }
    if (SLPAddChild(p, n, cond) != 0 || SLPAddChild(p, n, thenBlock) != 0) {
        return -1;
    }

    if (SLPMatch(p, SLTok_SEMICOLON) && SLPAt(p, SLTok_ELSE)) {
        /* Allow newline between `}` and `else`. */
    } else if (p->pos > 0 && SLPPrev(p)->kind == SLTok_SEMICOLON && !SLPAt(p, SLTok_ELSE)) {
        p->pos--;
    }

    if (SLPMatch(p, SLTok_ELSE)) {
        int32_t elseNode;
        if (SLPAt(p, SLTok_IF)) {
            if (SLPParseIfStmt(p, &elseNode) != 0) {
                return -1;
            }
        } else {
            if (SLPParseBlock(p, &elseNode) != 0) {
                return -1;
            }
        }
        if (SLPAddChild(p, n, elseNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[elseNode].end;
    }

    *out = n;
    return 0;
}

static int SLPParseForStmt(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    int32_t        n;
    int32_t        body;
    int32_t        init = -1;
    int32_t        cond = -1;
    int32_t        post = -1;

    p->pos++;
    n = SLPNewNode(p, SLAST_FOR, kw->start, kw->end);
    if (n < 0) {
        return -1;
    }

    if (SLPAt(p, SLTok_LBRACE)) {
        if (SLPParseBlock(p, &body) != 0) {
            return -1;
        }
    } else {
        if (SLPAt(p, SLTok_SEMICOLON)) {
            p->pos++;
        } else if (SLPAt(p, SLTok_VAR)) {
            if (SLPParseVarLikeStmt(p, SLAST_VAR, 0, &init) != 0) {
                return -1;
            }
        } else {
            if (SLPParseExpr(p, 1, &init) != 0) {
                return -1;
            }
        }

        if (SLPMatch(p, SLTok_SEMICOLON)) {
            if (!SLPAt(p, SLTok_SEMICOLON)) {
                if (SLPParseExpr(p, 1, &cond) != 0) {
                    return -1;
                }
            }
            if (!SLPMatch(p, SLTok_SEMICOLON)) {
                return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
            }
            if (!SLPAt(p, SLTok_LBRACE)) {
                if (SLPParseExpr(p, 1, &post) != 0) {
                    return -1;
                }
            }
        } else {
            cond = init;
            init = -1;
        }

        if (SLPParseBlock(p, &body) != 0) {
            return -1;
        }
    }

    if (init >= 0 && SLPAddChild(p, n, init) != 0) {
        return -1;
    }
    if (cond >= 0 && SLPAddChild(p, n, cond) != 0) {
        return -1;
    }
    if (post >= 0 && SLPAddChild(p, n, post) != 0) {
        return -1;
    }
    if (SLPAddChild(p, n, body) != 0) {
        return -1;
    }
    p->nodes[n].end = p->nodes[body].end;
    *out = n;
    return 0;
}

static int SLPParseSwitchStmt(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* rb;
    int32_t        sw;
    int            sawDefault = 0;

    p->pos++;
    sw = SLPNewNode(p, SLAST_SWITCH, kw->start, kw->end);
    if (sw < 0) {
        return -1;
    }

    // Expression switch: switch <expr> { ... }
    // Condition switch:  switch { ... }
    if (!SLPAt(p, SLTok_LBRACE)) {
        int32_t subject;
        if (SLPParseExpr(p, 1, &subject) != 0) {
            return -1;
        }
        if (SLPAddChild(p, sw, subject) != 0) {
            return -1;
        }
        p->nodes[sw].flags = 1;
    }

    if (SLPExpect(p, SLTok_LBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }

    while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
        if (SLPMatch(p, SLTok_SEMICOLON)) {
            continue;
        }

        if (SLPMatch(p, SLTok_CASE)) {
            int32_t caseNode = SLPNewNode(p, SLAST_CASE, SLPPrev(p)->start, SLPPrev(p)->end);
            int32_t body;
            if (caseNode < 0) {
                return -1;
            }

            for (;;) {
                int32_t labelExpr;
                if (SLPParseExpr(p, 1, &labelExpr) != 0) {
                    return -1;
                }
                if (SLPAddChild(p, caseNode, labelExpr) != 0) {
                    return -1;
                }
                if (!SLPMatch(p, SLTok_COMMA)) {
                    break;
                }
            }

            if (SLPParseBlock(p, &body) != 0) {
                return -1;
            }
            if (SLPAddChild(p, caseNode, body) != 0) {
                return -1;
            }
            p->nodes[caseNode].end = p->nodes[body].end;
            if (SLPAddChild(p, sw, caseNode) != 0) {
                return -1;
            }
            continue;
        }

        if (SLPMatch(p, SLTok_DEFAULT)) {
            int32_t defNode;
            int32_t body;
            if (sawDefault) {
                return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
            }
            sawDefault = 1;
            defNode = SLPNewNode(p, SLAST_DEFAULT, SLPPrev(p)->start, SLPPrev(p)->end);
            if (defNode < 0) {
                return -1;
            }
            if (SLPParseBlock(p, &body) != 0) {
                return -1;
            }
            if (SLPAddChild(p, defNode, body) != 0) {
                return -1;
            }
            p->nodes[defNode].end = p->nodes[body].end;
            if (SLPAddChild(p, sw, defNode) != 0) {
                return -1;
            }
            continue;
        }

        return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
    }

    if (SLPExpect(p, SLTok_RBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[sw].end = rb->end;
    *out = sw;
    return 0;
}

static int SLPParseStmt(SLParser* p, int32_t* out) {
    const SLToken* kw;
    int32_t        n;
    int32_t        expr;
    int32_t        block;

    switch (SLPPeek(p)->kind) {
        case SLTok_VAR:    return SLPParseVarLikeStmt(p, SLAST_VAR, 1, out);
        case SLTok_CONST:  return SLPParseVarLikeStmt(p, SLAST_CONST, 1, out);
        case SLTok_IF:     return SLPParseIfStmt(p, out);
        case SLTok_FOR:    return SLPParseForStmt(p, out);
        case SLTok_SWITCH: return SLPParseSwitchStmt(p, out);
        case SLTok_RETURN:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAST_RETURN, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (!SLPAt(p, SLTok_SEMICOLON)) {
                if (SLPParseExpr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (SLPAddChild(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[expr].end;
            }
            if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            p->nodes[n].end = kw->end;
            *out = n;
            return 0;
        case SLTok_BREAK:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAST_BREAK, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            p->nodes[n].end = kw->end;
            *out = n;
            return 0;
        case SLTok_CONTINUE:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAST_CONTINUE, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            p->nodes[n].end = kw->end;
            *out = n;
            return 0;
        case SLTok_DEFER:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAST_DEFER, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (SLPAt(p, SLTok_LBRACE)) {
                if (SLPParseBlock(p, &block) != 0) {
                    return -1;
                }
            } else {
                if (SLPParseStmt(p, &block) != 0) {
                    return -1;
                }
            }
            if (SLPAddChild(p, n, block) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[block].end;
            *out = n;
            return 0;
        case SLTok_ASSERT:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAST_ASSERT, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (SLPParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (SLPAddChild(p, n, expr) != 0) {
                return -1;
            }
            while (SLPMatch(p, SLTok_COMMA)) {
                int32_t arg;
                if (SLPParseExpr(p, 1, &arg) != 0) {
                    return -1;
                }
                if (SLPAddChild(p, n, arg) != 0) {
                    return -1;
                }
            }
            if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            p->nodes[n].end = kw->end;
            *out = n;
            return 0;
        case SLTok_LBRACE: return SLPParseBlock(p, out);
        default:
            if (SLPParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            n = SLPNewNode(p, SLAST_EXPR_STMT, p->nodes[expr].start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (SLPAddChild(p, n, expr) != 0) {
                return -1;
            }
            *out = n;
            return 0;
    }
}

static int SLPParseFieldList(SLParser* p, int32_t agg) {
    while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
        const SLToken* name;
        int32_t        field;
        int32_t        type;
        if (SLPAt(p, SLTok_SEMICOLON) || SLPAt(p, SLTok_COMMA)) {
            p->pos++;
            continue;
        }
        if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &name) != 0) {
            return -1;
        }
        if (SLPParseType(p, &type) != 0) {
            return -1;
        }
        field = SLPNewNode(p, SLAST_FIELD, name->start, p->nodes[type].end);
        if (field < 0) {
            return -1;
        }
        p->nodes[field].dataStart = name->start;
        p->nodes[field].dataEnd = name->end;
        if (SLPAddChild(p, field, type) != 0) {
            return -1;
        }
        if (SLPAddChild(p, agg, field) != 0) {
            return -1;
        }
        if (SLPMatch(p, SLTok_SEMICOLON) || SLPMatch(p, SLTok_COMMA)) {
            continue;
        }
    }
    return 0;
}

static int SLPParseAggregateDecl(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* name;
    const SLToken* rb;
    SLASTKind      kind = SLAST_STRUCT;
    int32_t        n;

    if (kw->kind == SLTok_UNION) {
        kind = SLAST_UNION;
    } else if (kw->kind == SLTok_ENUM) {
        kind = SLAST_ENUM;
    }

    p->pos++;
    if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    n = SLPNewNode(p, kind, kw->start, name->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;

    if (kw->kind == SLTok_ENUM) {
        int32_t underType;
        if (SLPParseType(p, &underType) != 0) {
            return -1;
        }
        if (SLPAddChild(p, n, underType) != 0) {
            return -1;
        }
    }

    if (SLPExpect(p, SLTok_LBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    if (kw->kind == SLTok_ENUM) {
        while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
            const SLToken* itemName;
            int32_t        item;
            if (SLPAt(p, SLTok_COMMA) || SLPAt(p, SLTok_SEMICOLON)) {
                p->pos++;
                continue;
            }
            if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &itemName) != 0) {
                return -1;
            }
            item = SLPNewNode(p, SLAST_FIELD, itemName->start, itemName->end);
            if (item < 0) {
                return -1;
            }
            p->nodes[item].dataStart = itemName->start;
            p->nodes[item].dataEnd = itemName->end;
            if (SLPMatch(p, SLTok_ASSIGN)) {
                int32_t vexpr;
                if (SLPParseExpr(p, 1, &vexpr) != 0) {
                    return -1;
                }
                if (SLPAddChild(p, item, vexpr) != 0) {
                    return -1;
                }
                p->nodes[item].end = p->nodes[vexpr].end;
            }
            if (SLPAddChild(p, n, item) != 0) {
                return -1;
            }
            if (SLPMatch(p, SLTok_COMMA) || SLPMatch(p, SLTok_SEMICOLON)) {
                continue;
            }
        }
    } else {
        if (SLPParseFieldList(p, n) != 0) {
            return -1;
        }
    }

    if (SLPExpect(p, SLTok_RBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[n].end = rb->end;
    *out = n;
    return 0;
}

static int SLPParseFunDecl(SLParser* p, int allowBody, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* name;
    const SLToken* t;
    int32_t        fn;

    p->pos++;
    if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    if (SLPExpect(p, SLTok_LPAREN, SLDiag_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }

    fn = SLPNewNode(p, SLAST_FN, kw->start, name->end);
    if (fn < 0) {
        return -1;
    }
    p->nodes[fn].dataStart = name->start;
    p->nodes[fn].dataEnd = name->end;

    if (!SLPAt(p, SLTok_RPAREN)) {
        for (;;) {
            int32_t param;
            if (SLPParseParam(p, &param) != 0) {
                return -1;
            }
            if (SLPAddChild(p, fn, param) != 0) {
                return -1;
            }
            if (!SLPMatch(p, SLTok_COMMA)) {
                break;
            }
        }
    }

    if (SLPExpect(p, SLTok_RPAREN, SLDiag_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }
    p->nodes[fn].end = t->end;

    if (!SLPAt(p, SLTok_LBRACE) && !SLPAt(p, SLTok_SEMICOLON)) {
        int32_t retType;
        if (SLPParseType(p, &retType) != 0) {
            return -1;
        }
        p->nodes[retType].flags = 1;
        if (SLPAddChild(p, fn, retType) != 0) {
            return -1;
        }
        p->nodes[fn].end = p->nodes[retType].end;
    }

    if (SLPAt(p, SLTok_LBRACE)) {
        int32_t body;
        if (!allowBody) {
            return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
        }
        if (SLPParseBlock(p, &body) != 0) {
            return -1;
        }
        if (SLPAddChild(p, fn, body) != 0) {
            return -1;
        }
        p->nodes[fn].end = p->nodes[body].end;
    } else {
        if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &t) != 0) {
            return -1;
        }
        p->nodes[fn].end = t->end;
    }

    *out = fn;
    return 0;
}

static int SLPParseImport(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* alias = NULL;
    const SLToken* path;
    int32_t        n;
    p->pos++;

    if (SLPAt(p, SLTok_IDENT)) {
        if ((p->pos + 1u) < p->tokLen && p->tok[p->pos + 1u].kind == SLTok_STRING) {
            alias = SLPPeek(p);
            p->pos++;
        }
    }

    if (SLPExpect(p, SLTok_STRING, SLDiag_UNEXPECTED_TOKEN, &path) != 0) {
        return -1;
    }

    n = SLPNewNode(p, SLAST_IMPORT, kw->start, path->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = path->start;
    p->nodes[n].dataEnd = path->end;

    if (alias != NULL) {
        int32_t aliasNode = SLPNewNode(p, SLAST_IDENT, alias->start, alias->end);
        if (aliasNode < 0) {
            return -1;
        }
        p->nodes[aliasNode].dataStart = alias->start;
        p->nodes[aliasNode].dataEnd = alias->end;
        if (SLPAddChild(p, n, aliasNode) != 0) {
            return -1;
        }
    }
    if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
        return -1;
    }
    p->nodes[n].end = kw->end;
    *out = n;
    return 0;
}

static int SLPParseDeclInner(SLParser* p, int allowBody, int32_t* out) {
    switch (SLPPeek(p)->kind) {
        case SLTok_FN: return SLPParseFunDecl(p, allowBody, out);
        case SLTok_STRUCT:
        case SLTok_UNION:
        case SLTok_ENUM:
            if (SLPParseAggregateDecl(p, out) != 0) {
                return -1;
            }
            if (SLPMatch(p, SLTok_SEMICOLON)) {
                p->nodes[*out].end = SLPPrev(p)->end;
            }
            return 0;
        case SLTok_CONST: return SLPParseVarLikeStmt(p, SLAST_CONST, 1, out);
        default:          return SLPFail(p, SLDiag_EXPECTED_DECL);
    }
}

static int SLPParseDecl(SLParser* p, int allowBody, int32_t* out) {
    int      isPub = 0;
    uint32_t pubStart = 0;
    if (SLPMatch(p, SLTok_PUB)) {
        isPub = 1;
        pubStart = SLPPrev(p)->start;
    }
    if (SLPParseDeclInner(p, allowBody, out) != 0) {
        return -1;
    }
    if (isPub) {
        p->nodes[*out].start = pubStart;
        p->nodes[*out].flags |= SLASTFlag_PUB;
    }
    return 0;
}

const char* SLASTKindName(SLASTKind kind) {
    switch (kind) {
        case SLAST_FILE:          return "FILE";
        case SLAST_IMPORT:        return "IMPORT";
        case SLAST_PUB:           return "PUB";
        case SLAST_FN:            return "FN";
        case SLAST_PARAM:         return "PARAM";
        case SLAST_TYPE_NAME:     return "TYPE_NAME";
        case SLAST_TYPE_PTR:      return "TYPE_PTR";
        case SLAST_TYPE_REF:      return "TYPE_REF";
        case SLAST_TYPE_MUTREF:   return "TYPE_MUTREF";
        case SLAST_TYPE_ARRAY:    return "TYPE_ARRAY";
        case SLAST_TYPE_VARRAY:   return "TYPE_VARRAY";
        case SLAST_TYPE_SLICE:    return "TYPE_SLICE";
        case SLAST_TYPE_MUTSLICE: return "TYPE_MUTSLICE";
        case SLAST_STRUCT:        return "STRUCT";
        case SLAST_UNION:         return "UNION";
        case SLAST_ENUM:          return "ENUM";
        case SLAST_FIELD:         return "FIELD";
        case SLAST_BLOCK:         return "BLOCK";
        case SLAST_VAR:           return "VAR";
        case SLAST_CONST:         return "CONST";
        case SLAST_IF:            return "IF";
        case SLAST_FOR:           return "FOR";
        case SLAST_SWITCH:        return "SWITCH";
        case SLAST_CASE:          return "CASE";
        case SLAST_DEFAULT:       return "DEFAULT";
        case SLAST_RETURN:        return "RETURN";
        case SLAST_BREAK:         return "BREAK";
        case SLAST_CONTINUE:      return "CONTINUE";
        case SLAST_DEFER:         return "DEFER";
        case SLAST_ASSERT:        return "ASSERT";
        case SLAST_EXPR_STMT:     return "EXPR_STMT";
        case SLAST_IDENT:         return "IDENT";
        case SLAST_INT:           return "INT";
        case SLAST_FLOAT:         return "FLOAT";
        case SLAST_STRING:        return "STRING";
        case SLAST_BOOL:          return "BOOL";
        case SLAST_UNARY:         return "UNARY";
        case SLAST_BINARY:        return "BINARY";
        case SLAST_CALL:          return "CALL";
        case SLAST_INDEX:         return "INDEX";
        case SLAST_FIELD_EXPR:    return "FIELD_EXPR";
        case SLAST_CAST:          return "CAST";
        case SLAST_SIZEOF:        return "SIZEOF";
    }
    return "UNKNOWN";
}

int SLParse(SLArena* arena, SLStrView src, SLAST* out, SLDiag* diag) {
    SLTokenStream ts;
    SLParser      p;
    int32_t       root;

    SLDiagClear(diag);
    out->nodes = NULL;
    out->len = 0;
    out->root = -1;

    if (SLLex(arena, src, &ts, diag) != 0) {
        return -1;
    }

    p.src = src;
    p.tok = ts.v;
    p.tokLen = ts.len;
    p.pos = 0;
    p.nodeLen = 0;
    p.nodeCap = ts.len * 4u + 16u;
    p.diag = diag;
    p.nodes = (SLASTNode*)SLArenaAlloc(
        arena, p.nodeCap * (uint32_t)sizeof(SLASTNode), (uint32_t)_Alignof(SLASTNode));
    if (p.nodes == NULL) {
        SLPSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    root = SLPNewNode(&p, SLAST_FILE, 0, src.len);
    if (root < 0) {
        return -1;
    }

    while (SLPAt(&p, SLTok_IMPORT)) {
        int32_t imp;
        if (SLPParseImport(&p, &imp) != 0) {
            return -1;
        }
        if (SLPAddChild(&p, root, imp) != 0) {
            return -1;
        }
    }

    while (!SLPAt(&p, SLTok_EOF)) {
        int32_t decl;
        if (SLPAt(&p, SLTok_SEMICOLON)) {
            p.pos++;
            continue;
        }
        if (SLPParseDecl(&p, 1, &decl) != 0) {
            return -1;
        }
        if (SLPAddChild(&p, root, decl) != 0) {
            return -1;
        }
    }

    out->nodes = p.nodes;
    out->len = p.nodeLen;
    out->root = root;
    return 0;
}

SL_API_END
