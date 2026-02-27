#include "libsl-impl.h"

SL_API_BEGIN

static void SLPSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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

static void SLPSetDiagWithArg(
    SLDiag*    diag,
    SLDiagCode code,
    uint32_t   start,
    uint32_t   end,
    uint32_t   argStart,
    uint32_t   argEnd) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = argStart;
    diag->argEnd = argEnd;
}

typedef struct {
    SLStrView      src;
    SLArena*       arena;
    const SLToken* tok;
    uint32_t       tokLen;
    uint32_t       pos;
    SLAstNode*     nodes;
    uint32_t       nodeLen;
    uint32_t       nodeCap;
    SLDiag*        diag;
    SLFeatures     features;
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

static int SLPReservedName(const SLParser* p, const SLToken* tok) {
    static const char reservedPrefix[5] = { '_', '_', 's', 'l', '_' };
    uint32_t          n = tok->end - tok->start;
    return n >= 5u && memcmp(p->src.ptr + tok->start, reservedPrefix, 5u) == 0;
}

static int SLPIsHoleName(const SLParser* p, const SLToken* tok) {
    return tok->kind == SLTok_IDENT && tok->end == tok->start + 1u && p->src.ptr[tok->start] == '_';
}

static int SLPFailReservedName(SLParser* p, const SLToken* tok) {
    SLPSetDiagWithArg(p->diag, SLDiag_RESERVED_NAME, tok->start, tok->end, tok->start, tok->end);
    return -1;
}

static int SLPExpectDeclName(SLParser* p, const SLToken** out, int allowHole) {
    const SLToken* tok;
    if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &tok) != 0) {
        return -1;
    }
    if (!allowHole && SLPIsHoleName(p, tok)) {
        return SLPFailReservedName(p, tok);
    }
    if (SLPReservedName(p, tok)) {
        SLPSetDiag(p->diag, SLDiag_RESERVED_SL_PREFIX, tok->start, tok->end);
        return -1;
    }
    *out = tok;
    return 0;
}

static int SLPExpectFnName(SLParser* p, const SLToken** out) {
    const SLToken* tok = SLPPeek(p);
    if (tok->kind == SLTok_SIZEOF) {
        *out = tok;
        p->pos++;
        return 0;
    }
    return SLPExpectDeclName(p, out, 0);
}

static int SLPIsFieldSeparator(SLTokenKind kind) {
    return kind == SLTok_SEMICOLON || kind == SLTok_COMMA || kind == SLTok_RBRACE
        || kind == SLTok_ASSIGN || kind == SLTok_EOF;
}

static int SLPAnonymousFieldLookahead(SLParser* p, const SLToken** outLastIdent) {
    uint32_t       i = p->pos;
    const SLToken* last;

    if (i >= p->tokLen || p->tok[i].kind != SLTok_IDENT) {
        return 0;
    }
    last = &p->tok[i];
    i++;
    while ((i + 1u) < p->tokLen && p->tok[i].kind == SLTok_DOT && p->tok[i + 1u].kind == SLTok_IDENT
           && p->tok[i].start == last->end && p->tok[i + 1u].start == p->tok[i].end)
    {
        last = &p->tok[i + 1u];
        i += 2u;
    }
    if (i >= p->tokLen || !SLPIsFieldSeparator(p->tok[i].kind)) {
        return 0;
    }
    if (outLastIdent != NULL) {
        *outLastIdent = last;
    }
    return 1;
}

static int32_t SLPNewNode(SLParser* p, SLAstKind kind, uint32_t start, uint32_t end) {
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
static int SLPParseFnType(SLParser* p, int32_t* out);
static int SLPParseExpr(SLParser* p, int minPrec, int32_t* out);
static int SLPParseStmt(SLParser* p, int32_t* out);
static int SLPParseDecl(SLParser* p, int allowBody, int32_t* out);
static int SLPParseDeclInner(SLParser* p, int allowBody, int32_t* out);
static int SLPParseSwitchStmt(SLParser* p, int32_t* out);

static int SLPIsTypeStart(SLTokenKind kind) {
    switch (kind) {
        case SLTok_IDENT:
        case SLTok_STRUCT:
        case SLTok_UNION:
        case SLTok_MUL:
        case SLTok_AND:
        case SLTok_MUT:
        case SLTok_LBRACE:
        case SLTok_LBRACK:
        case SLTok_QUESTION: return 1;
        case SLTok_FN:       return 1;
        default:             return 0;
    }
}

static int SLPCloneSubtree(SLParser* p, int32_t nodeId, int32_t* out) {
    const SLAstNode* src;
    int32_t          clone;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= p->nodeLen) {
        return -1;
    }
    src = &p->nodes[nodeId];
    clone = SLPNewNode(p, src->kind, src->start, src->end);
    if (clone < 0) {
        return -1;
    }
    p->nodes[clone].dataStart = src->dataStart;
    p->nodes[clone].dataEnd = src->dataEnd;
    p->nodes[clone].op = src->op;
    p->nodes[clone].flags = src->flags;
    child = src->firstChild;
    while (child >= 0) {
        int32_t childClone;
        if (SLPCloneSubtree(p, child, &childClone) != 0) {
            return -1;
        }
        if (SLPAddChild(p, clone, childClone) != 0) {
            return -1;
        }
        child = p->nodes[child].nextSibling;
    }
    *out = clone;
    return 0;
}

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

    n = SLPNewNode(p, SLAst_TYPE_NAME, first->start, last->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = first->start;
    p->nodes[n].dataEnd = last->end;
    *out = n;
    return 0;
}

static int SLPTryParseFnTypeNamedParamGroup(
    SLParser* p, int32_t fnTypeNode, int* outConsumedGroup) {
    uint32_t savedPos = p->pos;
    uint32_t savedNodeLen = p->nodeLen;
    SLDiag   savedDiag = { 0 };
    uint32_t nameCount = 0;
    int32_t  typeNode = -1;
    uint32_t i;

    if (p->diag != NULL) {
        savedDiag = *p->diag;
    }
    if (!SLPAt(p, SLTok_IDENT)) {
        *outConsumedGroup = 0;
        return 0;
    }

    for (;;) {
        const SLToken* name;
        if (SLPExpectDeclName(p, &name, 0) != 0) {
            goto not_group;
        }
        nameCount++;
        if (!SLPMatch(p, SLTok_COMMA)) {
            break;
        }
        if (!SLPAt(p, SLTok_IDENT)) {
            goto not_group;
        }
    }

    if (nameCount == 0 || !SLPIsTypeStart(SLPPeek(p)->kind)) {
        goto not_group;
    }
    if (SLPParseType(p, &typeNode) != 0) {
        goto not_group;
    }

    for (i = 0; i < nameCount; i++) {
        int32_t paramType = -1;
        if (i == 0) {
            paramType = typeNode;
        } else if (SLPCloneSubtree(p, typeNode, &paramType) != 0) {
            return -1;
        }
        if (SLPAddChild(p, fnTypeNode, paramType) != 0) {
            return -1;
        }
    }
    *outConsumedGroup = 1;
    return 0;

not_group:
    p->pos = savedPos;
    p->nodeLen = savedNodeLen;
    if (p->diag != NULL) {
        *p->diag = savedDiag;
    }
    *outConsumedGroup = 0;
    return 0;
}

static int SLPParseFnType(SLParser* p, int32_t* out) {
    const SLToken* fnTok;
    const SLToken* rp;
    int32_t        fnTypeNode;

    if (SLPExpect(p, SLTok_FN, SLDiag_EXPECTED_TYPE, &fnTok) != 0) {
        return -1;
    }
    if (SLPExpect(p, SLTok_LPAREN, SLDiag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }

    fnTypeNode = SLPNewNode(p, SLAst_TYPE_FN, fnTok->start, rp->end);
    if (fnTypeNode < 0) {
        return -1;
    }

    if (!SLPAt(p, SLTok_RPAREN)) {
        for (;;) {
            int consumedGroup = 0;
            if (SLPAt(p, SLTok_IDENT)
                && SLPTryParseFnTypeNamedParamGroup(p, fnTypeNode, &consumedGroup) != 0)
            {
                return -1;
            }
            if (!consumedGroup) {
                int32_t paramType = -1;
                if (SLPParseType(p, &paramType) != 0) {
                    return -1;
                }
                if (SLPAddChild(p, fnTypeNode, paramType) != 0) {
                    return -1;
                }
            }
            if (!SLPMatch(p, SLTok_COMMA)) {
                break;
            }
        }
    }

    if (SLPExpect(p, SLTok_RPAREN, SLDiag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }
    p->nodes[fnTypeNode].end = rp->end;

    if (SLPIsTypeStart(SLPPeek(p)->kind)) {
        int32_t resultType = -1;
        if (SLPParseType(p, &resultType) != 0) {
            return -1;
        }
        p->nodes[resultType].flags = 1;
        if (SLPAddChild(p, fnTypeNode, resultType) != 0) {
            return -1;
        }
        p->nodes[fnTypeNode].end = p->nodes[resultType].end;
    }

    *out = fnTypeNode;
    return 0;
}

static int SLPParseAnonymousAggregateFieldDeclList(SLParser* p, int32_t aggTypeNode) {
    while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
        const SLToken* names[256];
        uint32_t       nameCount = 0;
        int32_t        typeNode = -1;
        int32_t        defaultExpr = -1;
        uint32_t       i;

        if (SLPMatch(p, SLTok_SEMICOLON) || SLPMatch(p, SLTok_COMMA)) {
            continue;
        }

        if (SLPExpectDeclName(p, &names[nameCount], 0) != 0) {
            return -1;
        }
        nameCount++;
        while (SLPMatch(p, SLTok_COMMA)) {
            if (!SLPAt(p, SLTok_IDENT)) {
                return SLPFail(p, SLDiag_EXPECTED_TYPE);
            }
            if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                return SLPFail(p, SLDiag_ARENA_OOM);
            }
            if (SLPExpectDeclName(p, &names[nameCount], 0) != 0) {
                return -1;
            }
            nameCount++;
        }

        if (SLPParseType(p, &typeNode) != 0) {
            return -1;
        }
        if (SLPMatch(p, SLTok_ASSIGN)) {
            if (nameCount > 1) {
                const SLToken* eq = SLPPrev(p);
                SLPSetDiag(p->diag, SLDiag_UNEXPECTED_TOKEN, eq->start, eq->end);
                return -1;
            }
            if (SLPParseExpr(p, 1, &defaultExpr) != 0) {
                return -1;
            }
        }
        for (i = 0; i < nameCount; i++) {
            int32_t fieldNode;
            int32_t fieldTypeNode;
            if (i == 0) {
                fieldTypeNode = typeNode;
            } else {
                if (SLPCloneSubtree(p, typeNode, &fieldTypeNode) != 0) {
                    return -1;
                }
            }
            fieldNode = SLPNewNode(p, SLAst_FIELD, names[i]->start, p->nodes[fieldTypeNode].end);
            if (fieldNode < 0) {
                return -1;
            }
            p->nodes[fieldNode].dataStart = names[i]->start;
            p->nodes[fieldNode].dataEnd = names[i]->end;
            if (SLPAddChild(p, fieldNode, fieldTypeNode) != 0) {
                return -1;
            }
            if (i == 0 && defaultExpr >= 0) {
                p->nodes[fieldNode].end = p->nodes[defaultExpr].end;
                if (SLPAddChild(p, fieldNode, defaultExpr) != 0) {
                    return -1;
                }
            }
            if (SLPAddChild(p, aggTypeNode, fieldNode) != 0) {
                return -1;
            }
        }

        if (SLPMatch(p, SLTok_SEMICOLON) || SLPMatch(p, SLTok_COMMA)) {
            continue;
        }
    }
    return 0;
}

static int SLPParseAnonymousAggregateType(SLParser* p, int32_t* out) {
    const SLToken* kw = NULL;
    const SLToken* lb;
    const SLToken* rb;
    SLAstKind      kind = SLAst_TYPE_ANON_STRUCT;
    int32_t        typeNode;

    if (SLPAt(p, SLTok_STRUCT) || SLPAt(p, SLTok_UNION)) {
        p->pos++;
        kw = SLPPrev(p);
        if (kw->kind == SLTok_UNION) {
            kind = SLAst_TYPE_ANON_UNION;
        }
    }

    if (SLPExpect(p, SLTok_LBRACE, SLDiag_EXPECTED_TYPE, &lb) != 0) {
        return -1;
    }
    typeNode = SLPNewNode(p, kind, kw != NULL ? kw->start : lb->start, lb->end);
    if (typeNode < 0) {
        return -1;
    }
    if (SLPParseAnonymousAggregateFieldDeclList(p, typeNode) != 0) {
        return -1;
    }
    if (SLPExpect(p, SLTok_RBRACE, SLDiag_EXPECTED_TYPE, &rb) != 0) {
        return -1;
    }
    p->nodes[typeNode].end = rb->end;
    *out = typeNode;
    return 0;
}

static int SLPParseType(SLParser* p, int32_t* out) {
    const SLToken* t;
    int32_t        typeNode;
    int32_t        child;

    /* Prefix '?' optional type. */
    if (SLPMatch(p, SLTok_QUESTION)) {
        t = SLPPrev(p);
        typeNode = SLPNewNode(p, SLAst_TYPE_OPTIONAL, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (SLPParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (SLPMatch(p, SLTok_MUL)) {
        t = SLPPrev(p);
        typeNode = SLPNewNode(p, SLAst_TYPE_PTR, t->start, t->end);
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
        typeNode = SLPNewNode(p, SLAst_TYPE_REF, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (SLPParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (SLPMatch(p, SLTok_MUT)) {
        (void)SLPPrev(p);
        return SLPFail(p, SLDiag_EXPECTED_TYPE);
    }

    if (SLPMatch(p, SLTok_LBRACK)) {
        const SLToken* lb = SLPPrev(p);
        const SLToken* nTok = NULL;
        int32_t        lenExpr = -1;
        const SLToken* rb;
        SLAstKind      kind;

        if (SLPParseType(p, &child) != 0) {
            return -1;
        }

        if (SLPMatch(p, SLTok_RBRACK)) {
            rb = SLPPrev(p);
            typeNode = SLPNewNode(p, SLAst_TYPE_SLICE, lb->start, rb->end);
            if (typeNode < 0) {
                return -1;
            }
            return SLPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
        }

        if (SLPMatch(p, SLTok_DOT)) {
            kind = SLAst_TYPE_VARRAY;
            if (SLPExpect(p, SLTok_IDENT, SLDiag_EXPECTED_TYPE, &nTok) != 0) {
                return -1;
            }
        } else {
            kind = SLAst_TYPE_ARRAY;
            if (SLPParseExpr(p, 1, &lenExpr) != 0) {
                return -1;
            }
        }

        typeNode = SLPNewNode(p, kind, lb->start, lb->end);
        if (typeNode < 0) {
            return -1;
        }
        if (nTok != NULL) {
            p->nodes[typeNode].dataStart = nTok->start;
            p->nodes[typeNode].dataEnd = nTok->end;
        }
        if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = rb->end;
        if (SLPAddChild(p, typeNode, child) != 0) {
            return -1;
        }
        if (kind == SLAst_TYPE_ARRAY) {
            const SLAstNode* lenNode = &p->nodes[lenExpr];
            if ((lenNode->kind == SLAst_INT && lenNode->dataEnd > lenNode->dataStart)
                && (lenNode->flags & SLAstFlag_PAREN) == 0)
            {
                p->nodes[typeNode].dataStart = lenNode->dataStart;
                p->nodes[typeNode].dataEnd = lenNode->dataEnd;
            } else {
                p->nodes[typeNode].dataStart = lenNode->start;
                p->nodes[typeNode].dataEnd = lenNode->end;
                if (SLPAddChild(p, typeNode, lenExpr) != 0) {
                    return -1;
                }
            }
        }
        *out = typeNode;
        return 0;
    }

    if (SLPAt(p, SLTok_FN)) {
        return SLPParseFnType(p, out);
    }

    if (SLPAt(p, SLTok_LBRACE) || SLPAt(p, SLTok_STRUCT) || SLPAt(p, SLTok_UNION)) {
        return SLPParseAnonymousAggregateType(p, out);
    }

    if (SLPParseTypeName(p, out) != 0) {
        return -1;
    }
    return 0;
}

static int SLPParseCompoundLiteralTail(SLParser* p, int32_t typeNode, int32_t* out) {
    const SLToken* lb;
    const SLToken* rb;
    int32_t        lit;

    if (SLPExpect(p, SLTok_LBRACE, SLDiag_EXPECTED_EXPR, &lb) != 0) {
        return -1;
    }

    lit = SLPNewNode(
        p, SLAst_COMPOUND_LIT, typeNode >= 0 ? p->nodes[typeNode].start : lb->start, lb->end);
    if (lit < 0) {
        return -1;
    }
    if (typeNode >= 0 && SLPAddChild(p, lit, typeNode) != 0) {
        return -1;
    }

    while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
        const SLToken* fieldName;
        const SLToken* eqTok;
        int32_t        field;
        int32_t        expr;

        if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &fieldName) != 0) {
            return -1;
        }
        field = SLPNewNode(p, SLAst_COMPOUND_FIELD, fieldName->start, fieldName->end);
        if (field < 0) {
            return -1;
        }
        p->nodes[field].dataStart = fieldName->start;
        p->nodes[field].dataEnd = fieldName->end;
        while (SLPMatch(p, SLTok_DOT)) {
            const SLToken* seg;
            if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &seg) != 0) {
                return -1;
            }
            p->nodes[field].dataEnd = seg->end;
        }
        if (SLPExpect(p, SLTok_ASSIGN, SLDiag_UNEXPECTED_TOKEN, &eqTok) != 0) {
            return -1;
        }
        if (SLPParseExpr(p, 1, &expr) != 0) {
            return -1;
        }
        if (SLPAddChild(p, field, expr) != 0 || SLPAddChild(p, lit, field) != 0) {
            return -1;
        }
        p->nodes[field].end = p->nodes[expr].end;

        if (SLPMatch(p, SLTok_COMMA) || SLPMatch(p, SLTok_SEMICOLON)) {
            if (SLPAt(p, SLTok_RBRACE)) {
                break;
            }
            continue;
        }
        break;
    }

    if (SLPExpect(p, SLTok_RBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[lit].end = rb->end;
    *out = lit;
    return 0;
}

static int SLPParseNewExpr(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPrev(p);
    const SLToken* rb;
    int32_t        n;
    int32_t        typeNode;
    int32_t        countNode = -1;
    int32_t        initNode = -1;
    int32_t        allocNode = -1;

    n = SLPNewNode(p, SLAst_NEW, kw->start, kw->end);
    if (n < 0) {
        return -1;
    }

    if (SLPMatch(p, SLTok_LBRACK)) {
        if (SLPParseType(p, &typeNode) != 0) {
            return -1;
        }
        if (SLPParseExpr(p, 1, &countNode) != 0) {
            return -1;
        }
        if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_EXPR, &rb) != 0) {
            return -1;
        }
        p->nodes[n].flags |= SLAstFlag_NEW_HAS_COUNT;
        p->nodes[n].end = rb->end;
    } else {
        if (SLPParseType(p, &typeNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[typeNode].end;
        if (SLPAt(p, SLTok_LBRACE)) {
            if (SLPParseCompoundLiteralTail(p, -1, &initNode) != 0) {
                return -1;
            }
            p->nodes[n].flags |= SLAstFlag_NEW_HAS_INIT;
            p->nodes[n].end = p->nodes[initNode].end;
        }
    }

    if (SLPAddChild(p, n, typeNode) != 0) {
        return -1;
    }
    if (countNode >= 0) {
        if (SLPAddChild(p, n, countNode) != 0) {
            return -1;
        }
    }
    if (initNode >= 0) {
        if (SLPAddChild(p, n, initNode) != 0) {
            return -1;
        }
    }

    if (SLPMatch(p, SLTok_WITH)) {
        if (SLPParseExpr(p, 1, &allocNode) != 0) {
            return -1;
        }
        if (SLPAddChild(p, n, allocNode) != 0) {
            return -1;
        }
        p->nodes[n].flags |= SLAstFlag_NEW_HAS_ALLOC;
        p->nodes[n].end = p->nodes[allocNode].end;
    }

    *out = n;
    return 0;
}

static int SLPParsePrimary(SLParser* p, int32_t* out) {
    const SLToken* t = SLPPeek(p);
    int32_t        n;

    if (SLPAt(p, SLTok_IDENT)) {
        uint32_t savedPos = p->pos;
        uint32_t savedNodeLen = p->nodeLen;
        SLDiag   savedDiag = { 0 };
        int32_t  typeNode;
        if (p->diag != NULL) {
            savedDiag = *p->diag;
        }
        if (SLPParseTypeName(p, &typeNode) == 0 && SLPAt(p, SLTok_LBRACE)
            && SLPPeek(p)->start == p->nodes[typeNode].end)
        {
            return SLPParseCompoundLiteralTail(p, typeNode, out);
        }
        p->pos = savedPos;
        p->nodeLen = savedNodeLen;
        if (p->diag != NULL) {
            *p->diag = savedDiag;
        }
    }

    if (SLPAt(p, SLTok_LBRACE)) {
        return SLPParseCompoundLiteralTail(p, -1, out);
    }

    if (SLPMatch(p, SLTok_IDENT) || SLPMatch(p, SLTok_CONTEXT)) {
        t = SLPPrev(p);
        n = SLPNewNode(p, SLAst_IDENT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_INT)) {
        n = SLPNewNode(p, SLAst_INT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_FLOAT)) {
        n = SLPNewNode(p, SLAst_FLOAT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_STRING)) {
        n = SLPNewNode(p, SLAst_STRING, t->start, t->end);
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
        n = SLPNewNode(p, SLAst_BOOL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_NULL)) {
        t = SLPPrev(p);
        n = SLPNewNode(p, SLAst_NULL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
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
        if (*out >= 0) {
            p->nodes[*out].flags |= SLAstFlag_PAREN;
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
        n = SLPNewNode(p, SLAst_SIZEOF, kw->start, t->end);
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
        if (p->diag != NULL) {
            *p->diag = (SLDiag){ 0 };
        }

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

    if (SLPMatch(p, SLTok_NEW)) {
        return SLPParseNewExpr(p, out);
    }

    return SLPFail(p, SLDiag_EXPECTED_EXPR);
}

static int SLPParsePostfix(SLParser* p, int32_t* expr) {
    for (;;) {
        int32_t        n;
        const SLToken* t;

        if (SLPMatch(p, SLTok_LPAREN)) {
            n = SLPNewNode(p, SLAst_CALL, p->nodes[*expr].start, SLPPrev(p)->end);
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

        if (SLPMatch(p, SLTok_WITH)) {
            int32_t        withNode;
            const SLToken* withTok = SLPPrev(p);
            if (p->nodes[*expr].kind != SLAst_CALL) {
                return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
            }
            withNode = SLPNewNode(p, SLAst_CALL_WITH_CONTEXT, p->nodes[*expr].start, withTok->end);
            if (withNode < 0) {
                return -1;
            }
            if (SLPAddChild(p, withNode, *expr) != 0) {
                return -1;
            }
            if (SLPMatch(p, SLTok_CONTEXT)) {
                const SLToken* kw = SLPPrev(p);
                p->nodes[withNode].flags |= SLAstFlag_CALL_WITH_CONTEXT_PASSTHROUGH;
                p->nodes[withNode].end = kw->end;
                *expr = withNode;
                continue;
            } else {
                const SLToken* lb;
                const SLToken* rb;
                int32_t        overlayNode;
                if (SLPExpect(p, SLTok_LBRACE, SLDiag_UNEXPECTED_TOKEN, &lb) != 0) {
                    return -1;
                }
                overlayNode = SLPNewNode(p, SLAst_CONTEXT_OVERLAY, lb->start, lb->end);
                if (overlayNode < 0) {
                    return -1;
                }
                if (!SLPAt(p, SLTok_RBRACE)) {
                    for (;;) {
                        const SLToken* bindTok;
                        int32_t        bindNode;
                        int32_t        bindExpr = -1;
                        if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &bindTok) != 0) {
                            return -1;
                        }
                        bindNode = SLPNewNode(p, SLAst_CONTEXT_BIND, bindTok->start, bindTok->end);
                        if (bindNode < 0) {
                            return -1;
                        }
                        p->nodes[bindNode].dataStart = bindTok->start;
                        p->nodes[bindNode].dataEnd = bindTok->end;
                        if (SLPMatch(p, SLTok_ASSIGN)) {
                            if (SLPParseExpr(p, 1, &bindExpr) != 0) {
                                return -1;
                            }
                            if (SLPAddChild(p, bindNode, bindExpr) != 0) {
                                return -1;
                            }
                            p->nodes[bindNode].end = p->nodes[bindExpr].end;
                        }
                        if (SLPAddChild(p, overlayNode, bindNode) != 0) {
                            return -1;
                        }
                        if (SLPMatch(p, SLTok_COMMA)) {
                            if (SLPAt(p, SLTok_RBRACE)) {
                                break;
                            }
                            continue;
                        }
                        break;
                    }
                }
                if (SLPExpect(p, SLTok_RBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
                    return -1;
                }
                p->nodes[overlayNode].end = rb->end;
                if (SLPAddChild(p, withNode, overlayNode) != 0) {
                    return -1;
                }
                p->nodes[withNode].end = rb->end;
                *expr = withNode;
                continue;
            }
        }

        if (SLPMatch(p, SLTok_LBRACK)) {
            int32_t firstExpr = -1;
            n = SLPNewNode(p, SLAst_INDEX, p->nodes[*expr].start, SLPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (SLPAddChild(p, n, *expr) != 0) {
                return -1;
            }

            if (SLPMatch(p, SLTok_COLON)) {
                p->nodes[n].flags |= SLAstFlag_INDEX_SLICE;
                if (!SLPAt(p, SLTok_RBRACK)) {
                    if (SLPParseExpr(p, 1, &firstExpr) != 0) {
                        return -1;
                    }
                    p->nodes[n].flags |= SLAstFlag_INDEX_HAS_END;
                    if (SLPAddChild(p, n, firstExpr) != 0) {
                        return -1;
                    }
                }
                if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_EXPR, &t) != 0) {
                    return -1;
                }
                p->nodes[n].end = t->end;
                *expr = n;
                continue;
            }

            if (SLPParseExpr(p, 1, &firstExpr) != 0) {
                return -1;
            }
            if (SLPMatch(p, SLTok_COLON)) {
                int32_t endExpr = -1;
                p->nodes[n].flags |= SLAstFlag_INDEX_SLICE;
                p->nodes[n].flags |= SLAstFlag_INDEX_HAS_START;
                if (SLPAddChild(p, n, firstExpr) != 0) {
                    return -1;
                }
                if (!SLPAt(p, SLTok_RBRACK)) {
                    if (SLPParseExpr(p, 1, &endExpr) != 0) {
                        return -1;
                    }
                    p->nodes[n].flags |= SLAstFlag_INDEX_HAS_END;
                    if (SLPAddChild(p, n, endExpr) != 0) {
                        return -1;
                    }
                }
                if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_EXPR, &t) != 0) {
                    return -1;
                }
                p->nodes[n].end = t->end;
                *expr = n;
                continue;
            }

            if (SLPAddChild(p, n, firstExpr) != 0) {
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
            n = SLPNewNode(p, SLAst_FIELD_EXPR, p->nodes[*expr].start, SLPPrev(p)->end);
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
            n = SLPNewNode(p, SLAst_CAST, p->nodes[*expr].start, SLPPrev(p)->end);
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

        if (SLPMatch(p, SLTok_NOT)) {
            t = SLPPrev(p);
            n = SLPNewNode(p, SLAst_UNWRAP, p->nodes[*expr].start, t->end);
            if (n < 0) {
                return -1;
            }
            if (SLPAddChild(p, n, *expr) != 0) {
                return -1;
            }
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
            n = SLPNewNode(p, SLAst_UNARY, t->start, p->nodes[rhs].end);
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
        n = SLPNewNode(p, SLAst_BINARY, p->nodes[lhs].start, p->nodes[rhs].end);
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

static int SLPParseParamGroup(SLParser* p, int32_t fnNode) {
    const SLToken* lastName = NULL;
    int32_t        firstParam = -1;
    int32_t        lastParam = -1;
    int32_t        type = -1;

    for (;;) {
        const SLToken* name;
        int32_t        param;
        if (SLPExpectDeclName(p, &name, 1) != 0) {
            return -1;
        }
        lastName = name;
        param = SLPNewNode(p, SLAst_PARAM, name->start, name->end);
        if (param < 0) {
            return -1;
        }
        p->nodes[param].dataStart = name->start;
        p->nodes[param].dataEnd = name->end;
        if (firstParam < 0) {
            firstParam = param;
        } else {
            p->nodes[lastParam].nextSibling = param;
        }
        lastParam = param;

        if (!SLPMatch(p, SLTok_COMMA)) {
            break;
        }
        if (!SLPAt(p, SLTok_IDENT)) {
            return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
        }
    }

    if (!SLPIsTypeStart(SLPPeek(p)->kind)) {
        if (lastName != NULL) {
            SLPSetDiagWithArg(
                p->diag,
                SLDiag_PARAM_MISSING_TYPE,
                lastName->start,
                lastName->end,
                lastName->start,
                lastName->end);
        } else {
            SLPSetDiag(p->diag, SLDiag_PARAM_MISSING_TYPE, SLPPeek(p)->start, SLPPeek(p)->end);
        }
        return -1;
    }

    if (SLPParseType(p, &type) != 0) {
        return -1;
    }

    {
        int32_t param = firstParam;
        while (param >= 0) {
            int32_t nextParam = p->nodes[param].nextSibling;
            int32_t typeNode = -1;
            if (param == firstParam) {
                typeNode = type;
            } else if (SLPCloneSubtree(p, type, &typeNode) != 0) {
                return -1;
            }
            if (SLPAddChild(p, param, typeNode) != 0) {
                return -1;
            }
            p->nodes[param].end = p->nodes[typeNode].end;
            param = nextParam;
        }
    }

    if (SLPAddChild(p, fnNode, firstParam) != 0) {
        return -1;
    }
    return 0;
}

static int SLPParseBlock(SLParser* p, int32_t* out) {
    const SLToken* lb;
    const SLToken* rb;
    int32_t        block;

    if (SLPExpect(p, SLTok_LBRACE, SLDiag_UNEXPECTED_TOKEN, &lb) != 0) {
        return -1;
    }
    block = SLPNewNode(p, SLAst_BLOCK, lb->start, lb->end);
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

/* Statement separators may omit trailing semicolon before closing `}`. */
static int SLPConsumeStmtTerminator(SLParser* p, const SLToken** semiTok) {
    if (SLPAt(p, SLTok_SEMICOLON)) {
        if (semiTok != NULL) {
            *semiTok = SLPPeek(p);
        }
        p->pos++;
        return 1;
    }
    if (SLPAt(p, SLTok_RBRACE)) {
        if (semiTok != NULL) {
            *semiTok = NULL;
        }
        return 0;
    }
    return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
}

static int SLPParseVarLikeStmt(
    SLParser* p, SLAstKind kind, int requireSemi, int allowHole, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* name;
    uint32_t       stmtStart = kw->start;
    int32_t        n;
    int32_t        type = -1;
    int32_t        init = -1;

    p->pos++;
    if (SLPExpectDeclName(p, &name, allowHole) != 0) {
        return -1;
    }

    if (SLPIsHoleName(p, name)) {
        int hasSemi;
        if (!SLPMatch(p, SLTok_ASSIGN)) {
            return SLPFailReservedName(p, name);
        }
        if (SLPParseExpr(p, 1, &init) != 0) {
            return -1;
        }
        if (!requireSemi) {
            *out = init;
            return 0;
        }
        hasSemi = SLPConsumeStmtTerminator(p, &kw);
        if (hasSemi < 0) {
            return -1;
        }
        n = SLPNewNode(p, SLAst_EXPR_STMT, stmtStart, hasSemi ? kw->end : p->nodes[init].end);
        if (n < 0) {
            return -1;
        }
        if (SLPAddChild(p, n, init) != 0) {
            return -1;
        }
        *out = n;
        return 0;
    }

    if (SLPMatch(p, SLTok_ASSIGN)) {
        if (SLPParseExpr(p, 1, &init) != 0) {
            return -1;
        }
    } else {
        if (SLPParseType(p, &type) != 0) {
            return -1;
        }
        if (SLPMatch(p, SLTok_ASSIGN)) {
            if (SLPParseExpr(p, 1, &init) != 0) {
                return -1;
            }
        }
    }

    n = SLPNewNode(p, kind, kw->start, init >= 0 ? p->nodes[init].end : p->nodes[type].end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;
    if (type >= 0) {
        if (SLPAddChild(p, n, type) != 0) {
            return -1;
        }
    }
    if (init >= 0) {
        if (SLPAddChild(p, n, init) != 0) {
            return -1;
        }
    }

    if (requireSemi) {
        int hasSemi = SLPConsumeStmtTerminator(p, &kw);
        if (hasSemi < 0) {
            return -1;
        }
        if (hasSemi) {
            p->nodes[n].end = kw->end;
        }
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

    n = SLPNewNode(p, SLAst_IF, kw->start, p->nodes[thenBlock].end);
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
    n = SLPNewNode(p, SLAst_FOR, kw->start, kw->end);
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
            if (SLPParseVarLikeStmt(p, SLAst_VAR, 0, 1, &init) != 0) {
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
    sw = SLPNewNode(p, SLAst_SWITCH, kw->start, kw->end);
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
            int32_t caseNode = SLPNewNode(p, SLAst_CASE, SLPPrev(p)->start, SLPPrev(p)->end);
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
            defNode = SLPNewNode(p, SLAst_DEFAULT, SLPPrev(p)->start, SLPPrev(p)->end);
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
        case SLTok_VAR:    return SLPParseVarLikeStmt(p, SLAst_VAR, 1, 1, out);
        case SLTok_CONST:  return SLPParseVarLikeStmt(p, SLAst_CONST, 1, 1, out);
        case SLTok_IF:     return SLPParseIfStmt(p, out);
        case SLTok_FOR:    return SLPParseForStmt(p, out);
        case SLTok_SWITCH: return SLPParseSwitchStmt(p, out);
        case SLTok_RETURN:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAst_RETURN, kw->start, kw->end);
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
            {
                int hasSemi = SLPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case SLTok_BREAK:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAst_BREAK, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            {
                int hasSemi = SLPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case SLTok_CONTINUE:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAst_CONTINUE, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            {
                int hasSemi = SLPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case SLTok_DEFER:
            kw = SLPPeek(p);
            p->pos++;
            n = SLPNewNode(p, SLAst_DEFER, kw->start, kw->end);
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
            n = SLPNewNode(p, SLAst_ASSERT, kw->start, kw->end);
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
            {
                int hasSemi = SLPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case SLTok_LBRACE: return SLPParseBlock(p, out);
        default:           {
            int hasSemi;
            if (SLPParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            hasSemi = SLPConsumeStmtTerminator(p, &kw);
            if (hasSemi < 0) {
                return -1;
            }
            n = SLPNewNode(
                p, SLAst_EXPR_STMT, p->nodes[expr].start, hasSemi ? kw->end : p->nodes[expr].end);
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
}

static int SLPParseFieldList(SLParser* p, int32_t agg) {
    while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
        const SLToken* names[256];
        uint32_t       nameCount = 0;
        const SLToken* embeddedTypeName = NULL;
        int32_t        type = -1;
        int32_t        defaultExpr = -1;
        uint32_t       i;
        int            isEmbedded = 0;
        if (SLPAt(p, SLTok_SEMICOLON) || SLPAt(p, SLTok_COMMA)) {
            p->pos++;
            continue;
        }
        if (SLPAnonymousFieldLookahead(p, &embeddedTypeName)
            && !(
                p->pos + 2u < p->tokLen && p->tok[p->pos].kind == SLTok_IDENT
                && p->tok[p->pos + 1u].kind == SLTok_COMMA
                && p->tok[p->pos + 2u].kind == SLTok_IDENT))
        {
            if (SLPParseTypeName(p, &type) != 0) {
                return -1;
            }
            isEmbedded = 1;
            nameCount = 1;
        } else {
            if (SLPExpectDeclName(p, &names[nameCount], 0) != 0) {
                return -1;
            }
            nameCount++;
            while (SLPMatch(p, SLTok_COMMA)) {
                if (!SLPAt(p, SLTok_IDENT)) {
                    return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
                }
                if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                    return SLPFail(p, SLDiag_ARENA_OOM);
                }
                if (SLPExpectDeclName(p, &names[nameCount], 0) != 0) {
                    return -1;
                }
                nameCount++;
            }
            if (SLPParseType(p, &type) != 0) {
                return -1;
            }
        }
        if (SLPMatch(p, SLTok_ASSIGN)) {
            if (!isEmbedded && nameCount > 1) {
                const SLToken* eq = SLPPrev(p);
                SLPSetDiag(p->diag, SLDiag_UNEXPECTED_TOKEN, eq->start, eq->end);
                return -1;
            }
            if (SLPParseExpr(p, 1, &defaultExpr) != 0) {
                return -1;
            }
        }
        for (i = 0; i < nameCount; i++) {
            int32_t field;
            int32_t fieldType;
            if (isEmbedded) {
                fieldType = type;
                if (i != 0) {
                    return SLPFail(p, SLDiag_UNEXPECTED_TOKEN);
                }
            } else if (i == 0) {
                fieldType = type;
            } else if (SLPCloneSubtree(p, type, &fieldType) != 0) {
                return -1;
            }
            field = SLPNewNode(
                p,
                SLAst_FIELD,
                isEmbedded ? p->nodes[fieldType].start : names[i]->start,
                p->nodes[fieldType].end);
            if (field < 0) {
                return -1;
            }
            if (isEmbedded) {
                p->nodes[field].dataStart = embeddedTypeName->start;
                p->nodes[field].dataEnd = embeddedTypeName->end;
                p->nodes[field].flags |= SLAstFlag_FIELD_EMBEDDED;
            } else {
                p->nodes[field].dataStart = names[i]->start;
                p->nodes[field].dataEnd = names[i]->end;
            }
            if (SLPAddChild(p, field, fieldType) != 0) {
                return -1;
            }
            if (i == 0 && defaultExpr >= 0) {
                p->nodes[field].end = p->nodes[defaultExpr].end;
                if (SLPAddChild(p, field, defaultExpr) != 0) {
                    return -1;
                }
            }
            if (SLPAddChild(p, agg, field) != 0) {
                return -1;
            }
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
    SLAstKind      kind = SLAst_STRUCT;
    int32_t        n;

    if (kw->kind == SLTok_UNION) {
        kind = SLAst_UNION;
    } else if (kw->kind == SLTok_ENUM) {
        kind = SLAst_ENUM;
    }

    p->pos++;
    if (SLPExpectDeclName(p, &name, 0) != 0) {
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
            if (SLPExpectDeclName(p, &itemName, 0) != 0) {
                return -1;
            }
            item = SLPNewNode(p, SLAst_FIELD, itemName->start, itemName->end);
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
    if (SLPExpectFnName(p, &name) != 0) {
        return -1;
    }

    if (SLPExpect(p, SLTok_LPAREN, SLDiag_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }

    fn = SLPNewNode(p, SLAst_FN, kw->start, name->end);
    if (fn < 0) {
        return -1;
    }
    p->nodes[fn].dataStart = name->start;
    p->nodes[fn].dataEnd = name->end;

    if (!SLPAt(p, SLTok_RPAREN)) {
        for (;;) {
            if (SLPParseParamGroup(p, fn) != 0) {
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

    if (!SLPAt(p, SLTok_LBRACE) && !SLPAt(p, SLTok_SEMICOLON) && !SLPAt(p, SLTok_CONTEXT)) {
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

    if (SLPMatch(p, SLTok_CONTEXT)) {
        const SLToken* contextTok = SLPPrev(p);
        int32_t        contextClause;
        int32_t        contextType;
        contextClause = SLPNewNode(p, SLAst_CONTEXT_CLAUSE, contextTok->start, contextTok->end);
        if (contextClause < 0) {
            return -1;
        }
        if (SLPParseType(p, &contextType) != 0) {
            return -1;
        }
        if (SLPAddChild(p, contextClause, contextType) != 0) {
            return -1;
        }
        p->nodes[contextClause].end = p->nodes[contextType].end;
        if (SLPAddChild(p, fn, contextClause) != 0) {
            return -1;
        }
        p->nodes[fn].end = p->nodes[contextClause].end;
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

static int SLPParseTypeAliasDecl(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* name;
    const SLToken* semi;
    int32_t        n;
    int32_t        targetType;

    p->pos++;
    if (SLPExpectDeclName(p, &name, 0) != 0) {
        return -1;
    }
    if (SLPParseType(p, &targetType) != 0) {
        return -1;
    }

    n = SLPNewNode(p, SLAst_TYPE_ALIAS, kw->start, p->nodes[targetType].end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;
    if (SLPAddChild(p, n, targetType) != 0) {
        return -1;
    }

    if (SLPExpect(p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &semi) != 0) {
        return -1;
    }
    p->nodes[n].end = semi->end;
    *out = n;
    return 0;
}

static int SLPParseImport(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* path;
    const SLToken* alias = NULL;
    int32_t        n;
    p->pos++;

    if (SLPExpect(p, SLTok_STRING, SLDiag_UNEXPECTED_TOKEN, &path) != 0) {
        return -1;
    }

    n = SLPNewNode(p, SLAst_IMPORT, kw->start, path->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = path->start;
    p->nodes[n].dataEnd = path->end;

    if (SLPMatch(p, SLTok_AS)) {
        int32_t aliasNode;
        if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &alias) != 0) {
            return -1;
        }
        if (SLPReservedName(p, alias)) {
            SLPSetDiag(p->diag, SLDiag_RESERVED_SL_PREFIX, alias->start, alias->end);
            return -1;
        }
        aliasNode = SLPNewNode(p, SLAst_IDENT, alias->start, alias->end);
        if (aliasNode < 0) {
            return -1;
        }
        p->nodes[aliasNode].dataStart = alias->start;
        p->nodes[aliasNode].dataEnd = alias->end;
        if (SLPAddChild(p, n, aliasNode) != 0) {
            return -1;
        }
    }

    if (SLPMatch(p, SLTok_LBRACE)) {
        if (!SLPAt(p, SLTok_RBRACE)) {
            for (;;) {
                const SLToken* symName = NULL;
                const SLToken* symAlias = NULL;
                int32_t        symNode;

                if (SLPAt(p, SLTok_MUL)) {
                    const SLToken* starTok = SLPPeek(p);
                    SLPSetDiag(
                        p->diag,
                        SLDiag_IMPORT_WILDCARD_NOT_SUPPORTED,
                        starTok->start,
                        starTok->end);
                    return -1;
                }
                if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &symName) != 0) {
                    return -1;
                }
                symNode = SLPNewNode(p, SLAst_IMPORT_SYMBOL, symName->start, symName->end);
                if (symNode < 0) {
                    return -1;
                }
                p->nodes[symNode].dataStart = symName->start;
                p->nodes[symNode].dataEnd = symName->end;

                if (SLPMatch(p, SLTok_AS)) {
                    int32_t symAliasNode;
                    if (SLPExpect(p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &symAlias) != 0) {
                        return -1;
                    }
                    if (SLPReservedName(p, symAlias)) {
                        SLPSetDiag(
                            p->diag, SLDiag_RESERVED_SL_PREFIX, symAlias->start, symAlias->end);
                        return -1;
                    }
                    symAliasNode = SLPNewNode(p, SLAst_IDENT, symAlias->start, symAlias->end);
                    if (symAliasNode < 0) {
                        return -1;
                    }
                    p->nodes[symAliasNode].dataStart = symAlias->start;
                    p->nodes[symAliasNode].dataEnd = symAlias->end;
                    if (SLPAddChild(p, symNode, symAliasNode) != 0) {
                        return -1;
                    }
                    p->nodes[symNode].end = symAlias->end;
                }

                if (SLPAddChild(p, n, symNode) != 0) {
                    return -1;
                }

                if (!SLPMatch(p, SLTok_COMMA) && !SLPMatch(p, SLTok_SEMICOLON)) {
                    break;
                }
                while (SLPMatch(p, SLTok_COMMA) || SLPMatch(p, SLTok_SEMICOLON)) {}
                if (SLPAt(p, SLTok_RBRACE)) {
                    break;
                }
            }
        }
        if (SLPExpect(p, SLTok_RBRACE, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
            return -1;
        }
        p->nodes[n].end = kw->end;
    }

    /* Detect feature imports and set feature flags using decoded import path bytes. */
    {
        SLStringLitErr litErr = { 0 };
        uint8_t*       decoded = NULL;
        uint32_t       decodedLen = 0;
        const uint8_t* name = NULL;
        uint32_t       nameLen = 0;
        if (SLDecodeStringLiteralArena(
                p->arena, p->src.ptr, path->start, path->end, &decoded, &decodedLen, &litErr)
            != 0)
        {
            SLPSetDiag(p->diag, SLStringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
            return -1;
        }
        if (decodedLen > 14u && memcmp(decoded, "slang/feature/", 14u) == 0) {
            name = decoded + 14u;
            nameLen = decodedLen - 14u;
        } else if (decodedLen > 8u && memcmp(decoded, "feature/", 8u) == 0) {
            name = decoded + 8u;
            nameLen = decodedLen - 8u;
        }
        if (name != NULL) {
            /* "optional" = 8 chars */
            if (nameLen == 8u && name[0] == (uint8_t)'o' && name[1] == (uint8_t)'p'
                && name[2] == (uint8_t)'t' && name[3] == (uint8_t)'i' && name[4] == (uint8_t)'o'
                && name[5] == (uint8_t)'n' && name[6] == (uint8_t)'a' && name[7] == (uint8_t)'l')
            {
                p->features |= SLFeature_OPTIONAL;
            }
            /* Unknown feature names: silently ignored here; CLI layer warns later. */
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
        case SLTok_FN:   return SLPParseFunDecl(p, allowBody, out);
        case SLTok_TYPE: return SLPParseTypeAliasDecl(p, out);
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
        case SLTok_VAR:   return SLPParseVarLikeStmt(p, SLAst_VAR, 1, 0, out);
        case SLTok_CONST: return SLPParseVarLikeStmt(p, SLAst_CONST, 1, 0, out);
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
        p->nodes[*out].flags |= SLAstFlag_PUB;
    }
    return 0;
}

const char* SLAstKindName(SLAstKind kind) {
    switch (kind) {
        case SLAst_FILE:              return "FILE";
        case SLAst_IMPORT:            return "IMPORT";
        case SLAst_IMPORT_SYMBOL:     return "IMPORT_SYMBOL";
        case SLAst_PUB:               return "PUB";
        case SLAst_FN:                return "FN";
        case SLAst_PARAM:             return "PARAM";
        case SLAst_CONTEXT_CLAUSE:    return "CONTEXT_CLAUSE";
        case SLAst_TYPE_NAME:         return "TYPE_NAME";
        case SLAst_TYPE_PTR:          return "TYPE_PTR";
        case SLAst_TYPE_REF:          return "TYPE_REF";
        case SLAst_TYPE_MUTREF:       return "TYPE_MUTREF";
        case SLAst_TYPE_ARRAY:        return "TYPE_ARRAY";
        case SLAst_TYPE_VARRAY:       return "TYPE_VARRAY";
        case SLAst_TYPE_SLICE:        return "TYPE_SLICE";
        case SLAst_TYPE_MUTSLICE:     return "TYPE_MUTSLICE";
        case SLAst_TYPE_OPTIONAL:     return "TYPE_OPTIONAL";
        case SLAst_TYPE_FN:           return "TYPE_FN";
        case SLAst_TYPE_ALIAS:        return "TYPE_ALIAS";
        case SLAst_TYPE_ANON_STRUCT:  return "TYPE_ANON_STRUCT";
        case SLAst_TYPE_ANON_UNION:   return "TYPE_ANON_UNION";
        case SLAst_STRUCT:            return "STRUCT";
        case SLAst_UNION:             return "UNION";
        case SLAst_ENUM:              return "ENUM";
        case SLAst_FIELD:             return "FIELD";
        case SLAst_BLOCK:             return "BLOCK";
        case SLAst_VAR:               return "VAR";
        case SLAst_CONST:             return "CONST";
        case SLAst_IF:                return "IF";
        case SLAst_FOR:               return "FOR";
        case SLAst_SWITCH:            return "SWITCH";
        case SLAst_CASE:              return "CASE";
        case SLAst_DEFAULT:           return "DEFAULT";
        case SLAst_RETURN:            return "RETURN";
        case SLAst_BREAK:             return "BREAK";
        case SLAst_CONTINUE:          return "CONTINUE";
        case SLAst_DEFER:             return "DEFER";
        case SLAst_ASSERT:            return "ASSERT";
        case SLAst_EXPR_STMT:         return "EXPR_STMT";
        case SLAst_IDENT:             return "IDENT";
        case SLAst_INT:               return "INT";
        case SLAst_FLOAT:             return "FLOAT";
        case SLAst_STRING:            return "STRING";
        case SLAst_BOOL:              return "BOOL";
        case SLAst_UNARY:             return "UNARY";
        case SLAst_BINARY:            return "BINARY";
        case SLAst_CALL:              return "CALL";
        case SLAst_CALL_WITH_CONTEXT: return "CALL_WITH_CONTEXT";
        case SLAst_CONTEXT_OVERLAY:   return "CONTEXT_OVERLAY";
        case SLAst_CONTEXT_BIND:      return "CONTEXT_BIND";
        case SLAst_COMPOUND_LIT:      return "COMPOUND_LIT";
        case SLAst_COMPOUND_FIELD:    return "COMPOUND_FIELD";
        case SLAst_INDEX:             return "INDEX";
        case SLAst_FIELD_EXPR:        return "FIELD_EXPR";
        case SLAst_CAST:              return "CAST";
        case SLAst_SIZEOF:            return "SIZEOF";
        case SLAst_NEW:               return "NEW";
        case SLAst_NULL:              return "NULL";
        case SLAst_UNWRAP:            return "UNWRAP";
    }
    return "UNKNOWN";
}

static int SLPIsSpaceButNotNewline(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static int SLPHasCodeOnLineBefore(const char* src, uint32_t lineStart, uint32_t pos) {
    uint32_t i;
    for (i = lineStart; i < pos; i++) {
        char c = src[i];
        if (!SLPIsSpaceButNotNewline(c)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t SLPFindLineStart(const char* src, uint32_t pos) {
    while (pos > 0) {
        if (src[pos - 1] == '\n') {
            break;
        }
        pos--;
    }
    return pos;
}

static int32_t SLPFindPrevNodeByEnd(const SLAst* ast, uint32_t pos) {
    int32_t  best = -1;
    uint32_t bestEnd = 0;
    uint32_t bestSpan = 0;
    uint32_t i;
    for (i = 0; i < ast->len; i++) {
        const SLAstNode* n;
        uint32_t         span;
        if ((int32_t)i == ast->root) {
            continue;
        }
        n = &ast->nodes[i];
        if (n->end > pos) {
            continue;
        }
        span = n->end >= n->start ? (n->end - n->start) : 0;
        if (best < 0 || n->end > bestEnd || (n->end == bestEnd && span > bestSpan)) {
            best = (int32_t)i;
            bestEnd = n->end;
            bestSpan = span;
        }
    }
    return best;
}

static int32_t SLPFindNextNodeByStart(const SLAst* ast, uint32_t pos) {
    int32_t  best = -1;
    uint32_t bestStart = 0;
    uint32_t bestSpan = 0;
    uint32_t i;
    for (i = 0; i < ast->len; i++) {
        const SLAstNode* n;
        uint32_t         span;
        if ((int32_t)i == ast->root) {
            continue;
        }
        n = &ast->nodes[i];
        if (n->start < pos) {
            continue;
        }
        span = n->end >= n->start ? (n->end - n->start) : 0;
        if (best < 0 || n->start < bestStart || (n->start == bestStart && span > bestSpan)) {
            best = (int32_t)i;
            bestStart = n->start;
            bestSpan = span;
        }
    }
    return best;
}

static int32_t SLPFindContainerNode(const SLAst* ast, uint32_t pos) {
    int32_t nodeId = ast->root;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    for (;;) {
        int32_t  bestChild = -1;
        int32_t  child = ast->nodes[nodeId].firstChild;
        uint32_t bestSpan = 0;
        while (child >= 0) {
            const SLAstNode* n = &ast->nodes[child];
            if (n->start <= pos && pos <= n->end) {
                uint32_t span = n->end >= n->start ? (n->end - n->start) : 0;
                if (bestChild < 0 || span < bestSpan) {
                    bestChild = child;
                    bestSpan = span;
                }
            }
            child = ast->nodes[child].nextSibling;
        }
        if (bestChild < 0) {
            break;
        }
        nodeId = bestChild;
    }
    return nodeId;
}

static int SLPNextCommentRange(
    SLStrView src, uint32_t* ioPos, uint32_t* outStart, uint32_t* outEnd) {
    uint32_t pos = *ioPos;
    while (pos < src.len) {
        unsigned char c = (unsigned char)src.ptr[pos];
        if (c == (unsigned char)'"') {
            pos++;
            while (pos < src.len) {
                c = (unsigned char)src.ptr[pos];
                if (c == (unsigned char)'"') {
                    pos++;
                    break;
                }
                if (c == (unsigned char)'\\') {
                    pos++;
                    if (pos >= src.len) {
                        break;
                    }
                    if ((unsigned char)src.ptr[pos] == (unsigned char)'\r' && pos + 1u < src.len
                        && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'\n')
                    {
                        pos += 2u;
                    } else {
                        pos++;
                    }
                    continue;
                }
                pos++;
            }
            continue;
        }
        if (c == (unsigned char)'`') {
            pos++;
            while (pos < src.len) {
                c = (unsigned char)src.ptr[pos];
                if (c == (unsigned char)'\\' && pos + 1u < src.len
                    && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'`')
                {
                    pos += 2u;
                    continue;
                }
                if (c == (unsigned char)'`') {
                    pos++;
                    break;
                }
                pos++;
            }
            continue;
        }
        if (c == (unsigned char)'/' && pos + 1u < src.len
            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'/')
        {
            uint32_t start = pos;
            pos += 2u;
            while (pos < src.len && (unsigned char)src.ptr[pos] != (unsigned char)'\n') {
                pos++;
            }
            *ioPos = pos;
            *outStart = start;
            *outEnd = pos;
            return 1;
        }
        pos++;
    }
    *ioPos = pos;
    return 0;
}

static int SLPCollectFormattingData(
    SLArena*       arena,
    SLStrView      src,
    const SLAst*   ast,
    SLParseExtras* outExtras,
    SLDiag* _Nullable diag) {
    SLComment* comments;
    uint32_t   count = 0;
    uint32_t   pos = 0;
    for (;;) {
        uint32_t start;
        uint32_t end;
        if (!SLPNextCommentRange(src, &pos, &start, &end)) {
            break;
        }
        count++;
    }
    if (count == 0) {
        outExtras->comments = NULL;
        outExtras->commentLen = 0;
        return 0;
    }

    comments = (SLComment*)SLArenaAlloc(
        arena, count * (uint32_t)sizeof(SLComment), (uint32_t)_Alignof(SLComment));
    if (comments == NULL) {
        SLPSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    pos = 0;
    count = 0;
    for (;;) {
        uint32_t start;
        uint32_t end;
        uint32_t lineStart;
        int32_t  prevNode;
        int32_t  nextNode;
        int32_t  containerNode;
        if (!SLPNextCommentRange(src, &pos, &start, &end)) {
            break;
        }
        lineStart = SLPFindLineStart(src.ptr, start);
        prevNode = SLPFindPrevNodeByEnd(ast, start);
        nextNode = SLPFindNextNodeByStart(ast, end);
        containerNode = SLPFindContainerNode(ast, start);

        comments[count].start = start;
        comments[count].end = end;
        comments[count].textStart = start + 2u <= end ? start + 2u : end;
        comments[count].textEnd = end;
        comments[count].containerNode = containerNode;
        comments[count].anchorNode = -1;
        comments[count].attachment = SLCommentAttachment_FLOATING;
        comments[count]._reserved[0] = 0;
        comments[count]._reserved[1] = 0;
        comments[count]._reserved[2] = 0;

        if (SLPHasCodeOnLineBefore(src.ptr, lineStart, start)) {
            comments[count].attachment = SLCommentAttachment_TRAILING;
            comments[count].anchorNode = prevNode >= 0 ? prevNode : containerNode;
        } else if (nextNode >= 0) {
            comments[count].attachment = SLCommentAttachment_LEADING;
            comments[count].anchorNode = nextNode;
        } else if (prevNode >= 0) {
            comments[count].attachment = SLCommentAttachment_TRAILING;
            comments[count].anchorNode = prevNode;
        } else {
            comments[count].attachment = SLCommentAttachment_FLOATING;
            comments[count].anchorNode = containerNode;
        }
        count++;
    }

    outExtras->comments = comments;
    outExtras->commentLen = count;
    return 0;
}

int SLParse(
    SLArena*  arena,
    SLStrView src,
    const SLParseOptions* _Nullable options,
    SLAst* out,
    SLParseExtras* _Nullable outExtras,
    SLDiag* diag) {
    SLTokenStream ts;
    SLParser      p;
    int32_t       root;
    uint32_t      parseFlags = options != NULL ? options->flags : 0;

    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (outExtras != NULL) {
        outExtras->comments = NULL;
        outExtras->commentLen = 0;
    }
    out->nodes = NULL;
    out->len = 0;
    out->root = -1;
    out->features = SLFeature_NONE;

    if (SLLex(arena, src, &ts, diag) != 0) {
        return -1;
    }

    p.src = src;
    p.arena = arena;
    p.tok = ts.v;
    p.tokLen = ts.len;
    p.pos = 0;
    p.nodeLen = 0;
    p.nodeCap = ts.len * 4u + 16u;
    p.diag = diag;
    p.features = SLFeature_NONE;
    p.nodes = (SLAstNode*)SLArenaAlloc(
        arena, p.nodeCap * (uint32_t)sizeof(SLAstNode), (uint32_t)_Alignof(SLAstNode));
    if (p.nodes == NULL) {
        SLPSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    root = SLPNewNode(&p, SLAst_FILE, 0, src.len);
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
    out->features = p.features;
    if ((parseFlags & SLParseFlag_COLLECT_FORMATTING) != 0 && outExtras != NULL
        && SLPCollectFormattingData(arena, src, out, outExtras, diag) != 0)
    {
        return -1;
    }
    return 0;
}

SL_API_END
