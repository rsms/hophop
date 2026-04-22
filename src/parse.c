#include "libhop-impl.h"

HOP_API_BEGIN

static void HOPPSetDiag(HOPDiag* _Nullable diag, HOPDiagCode code, uint32_t start, uint32_t end) {
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

static void HOPPSetDiagWithArg(
    HOPDiag* _Nullable diag,
    HOPDiagCode code,
    uint32_t    start,
    uint32_t    end,
    uint32_t    argStart,
    uint32_t    argEnd) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = argStart;
    diag->argEnd = argEnd;
}

typedef struct {
    HOPStrView      src;
    HOPArena*       arena;
    const HOPToken* tok;
    uint32_t        tokLen;
    uint32_t        pos;
    HOPAstNode* _Nullable nodes;
    uint32_t nodeLen;
    uint32_t nodeCap;
    HOPDiag* _Nullable diag;
    HOPFeatures features;
} HOPParser;

static int HOPPParseExpr(HOPParser* p, int minPrec, int32_t* out);
static int HOPPParseType(HOPParser* p, int32_t* out);

static const HOPToken* HOPPPeek(HOPParser* p) {
    if (p->pos >= p->tokLen) {
        return &p->tok[p->tokLen - 1];
    }
    return &p->tok[p->pos];
}

static const HOPToken* HOPPPrev(HOPParser* p) {
    if (p->pos == 0) {
        return &p->tok[0];
    }
    return &p->tok[p->pos - 1];
}

static int HOPPAt(HOPParser* p, HOPTokenKind kind) {
    return HOPPPeek(p)->kind == kind;
}

static int HOPPMatch(HOPParser* p, HOPTokenKind kind) {
    if (!HOPPAt(p, kind)) {
        return 0;
    }
    p->pos++;
    return 1;
}

static int HOPPFail(HOPParser* p, HOPDiagCode code) {
    const HOPToken* t = HOPPPeek(p);
    HOPPSetDiag(p->diag, code, t->start, t->end);
    return -1;
}

static int HOPPExpect(HOPParser* p, HOPTokenKind kind, HOPDiagCode code, const HOPToken** out) {
    if (!HOPPAt(p, kind)) {
        return HOPPFail(p, code);
    }
    *out = HOPPPeek(p);
    p->pos++;
    return 0;
}

static int HOPPReservedName(const HOPParser* p, const HOPToken* tok) {
    static const char reservedPrefix[6] = { '_', '_', 'h', 'o', 'p', '_' };
    uint32_t          n = tok->end - tok->start;
    return n >= 6u && memcmp(p->src.ptr + tok->start, reservedPrefix, 6u) == 0;
}

static int HOPPIsHoleName(const HOPParser* p, const HOPToken* tok) {
    return tok->kind == HOPTok_IDENT && tok->end == tok->start + 1u
        && p->src.ptr[tok->start] == '_';
}

static int HOPPFailReservedName(HOPParser* p, const HOPToken* tok) {
    HOPPSetDiagWithArg(p->diag, HOPDiag_RESERVED_NAME, tok->start, tok->end, tok->start, tok->end);
    return -1;
}

static int HOPPExpectDeclName(HOPParser* p, const HOPToken** out, int allowHole) {
    const HOPToken* tok;
    if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_UNEXPECTED_TOKEN, &tok) != 0) {
        return -1;
    }
    if (!allowHole && HOPPIsHoleName(p, tok)) {
        return HOPPFailReservedName(p, tok);
    }
    if (HOPPReservedName(p, tok)) {
        HOPPSetDiag(p->diag, HOPDiag_RESERVED_HOP_PREFIX, tok->start, tok->end);
        return -1;
    }
    *out = tok;
    return 0;
}

static int HOPPExpectFnName(HOPParser* p, const HOPToken** out) {
    const HOPToken* tok = HOPPPeek(p);
    if (tok->kind == HOPTok_SIZEOF) {
        *out = tok;
        p->pos++;
        return 0;
    }
    return HOPPExpectDeclName(p, out, 0);
}

static int HOPPIsFieldSeparator(HOPTokenKind kind) {
    return kind == HOPTok_SEMICOLON || kind == HOPTok_COMMA || kind == HOPTok_RBRACE
        || kind == HOPTok_ASSIGN || kind == HOPTok_EOF;
}

static int HOPPAnonymousFieldLookahead(HOPParser* p, const HOPToken** outLastIdent) {
    uint32_t        i = p->pos;
    const HOPToken* last;

    if (i >= p->tokLen || p->tok[i].kind != HOPTok_IDENT) {
        return 0;
    }
    last = &p->tok[i];
    i++;
    while (
        (i + 1u) < p->tokLen && p->tok[i].kind == HOPTok_DOT && p->tok[i + 1u].kind == HOPTok_IDENT
        && p->tok[i].start == last->end && p->tok[i + 1u].start == p->tok[i].end)
    {
        last = &p->tok[i + 1u];
        i += 2u;
    }
    if (i >= p->tokLen || !HOPPIsFieldSeparator(p->tok[i].kind)) {
        return 0;
    }
    if (outLastIdent != NULL) {
        *outLastIdent = last;
    }
    return 1;
}

static int32_t HOPPNewNode(HOPParser* p, HOPAstKind kind, uint32_t start, uint32_t end) {
    int32_t idx;
    if (p->nodeLen >= p->nodeCap) {
        HOPPSetDiag(p->diag, HOPDiag_ARENA_OOM, start, end);
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

static int HOPPAddChild(HOPParser* p, int32_t parent, int32_t child) {
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

static int HOPIsAssignmentOp(HOPTokenKind kind) {
    switch (kind) {
        case HOPTok_ASSIGN:
        case HOPTok_ADD_ASSIGN:
        case HOPTok_SUB_ASSIGN:
        case HOPTok_MUL_ASSIGN:
        case HOPTok_DIV_ASSIGN:
        case HOPTok_MOD_ASSIGN:
        case HOPTok_AND_ASSIGN:
        case HOPTok_OR_ASSIGN:
        case HOPTok_XOR_ASSIGN:
        case HOPTok_LSHIFT_ASSIGN:
        case HOPTok_RSHIFT_ASSIGN: return 1;
        default:                   return 0;
    }
}

static int HOPBinPrec(HOPTokenKind kind) {
    if (HOPIsAssignmentOp(kind)) {
        return 1;
    }
    switch (kind) {
        case HOPTok_LOGICAL_OR:  return 2;
        case HOPTok_LOGICAL_AND: return 3;
        case HOPTok_EQ:
        case HOPTok_NEQ:
        case HOPTok_LT:
        case HOPTok_GT:
        case HOPTok_LTE:
        case HOPTok_GTE:         return 4;
        case HOPTok_OR:
        case HOPTok_XOR:
        case HOPTok_ADD:
        case HOPTok_SUB:         return 5;
        case HOPTok_AND:
        case HOPTok_LSHIFT:
        case HOPTok_RSHIFT:
        case HOPTok_MUL:
        case HOPTok_DIV:
        case HOPTok_MOD:         return 6;
        default:                 return 0;
    }
}

static int HOPPParseType(HOPParser* p, int32_t* out);
static int HOPPParseFnType(HOPParser* p, int32_t* out);
static int HOPPParseTupleType(HOPParser* p, int32_t* out);
static int HOPPParseExpr(HOPParser* p, int minPrec, int32_t* out);
static int HOPPParseStmt(HOPParser* p, int32_t* out);
static int HOPPParseDecl(HOPParser* p, int allowBody, int32_t* out);
static int HOPPParseDeclInner(HOPParser* p, int allowBody, int32_t* out);
static int HOPPParseDirective(HOPParser* p, int32_t* out);
static int HOPPParseSwitchStmt(HOPParser* p, int32_t* out);
static int HOPPParseAggregateDecl(HOPParser* p, int32_t* out);
static int HOPPParseTypeAliasDecl(HOPParser* p, int32_t* out);

static int HOPPIsTypeStart(HOPTokenKind kind) {
    switch (kind) {
        case HOPTok_IDENT:
        case HOPTok_ANYTYPE:
        case HOPTok_TYPE:
        case HOPTok_STRUCT:
        case HOPTok_UNION:
        case HOPTok_MUL:
        case HOPTok_AND:
        case HOPTok_MUT:
        case HOPTok_LBRACE:
        case HOPTok_LBRACK:
        case HOPTok_LPAREN:
        case HOPTok_QUESTION: return 1;
        case HOPTok_FN:       return 1;
        default:              return 0;
    }
}

static int HOPPCloneSubtree(HOPParser* p, int32_t nodeId, int32_t* out) {
    const HOPAstNode* src;
    int32_t           clone;
    int32_t           child;
    if (nodeId < 0 || (uint32_t)nodeId >= p->nodeLen) {
        return -1;
    }
    src = &p->nodes[nodeId];
    clone = HOPPNewNode(p, src->kind, src->start, src->end);
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
        if (HOPPCloneSubtree(p, child, &childClone) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, clone, childClone) != 0) {
            return -1;
        }
        child = p->nodes[child].nextSibling;
    }
    *out = clone;
    return 0;
}

static int HOPPParseTypeName(HOPParser* p, int32_t* out) {
    const HOPToken* first = NULL;
    const HOPToken* last;
    int32_t         n;

    if (HOPPAt(p, HOPTok_IDENT) || HOPPAt(p, HOPTok_TYPE) || HOPPAt(p, HOPTok_ANYTYPE)) {
        p->pos++;
        first = HOPPPrev(p);
    } else {
        return HOPPFail(p, HOPDiag_EXPECTED_TYPE);
    }
    last = first;
    while ((p->pos + 1u) < p->tokLen && p->tok[p->pos].kind == HOPTok_DOT
           && p->tok[p->pos + 1u].kind == HOPTok_IDENT && p->tok[p->pos].start == last->end
           && p->tok[p->pos + 1u].start == p->tok[p->pos].end)
    {
        p->pos++;
        last = &p->tok[p->pos];
        p->pos++;
    }

    n = HOPPNewNode(p, HOPAst_TYPE_NAME, first->start, last->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = first->start;
    p->nodes[n].dataEnd = last->end;
    if (HOPPMatch(p, HOPTok_LBRACK)) {
        const HOPToken* lb = HOPPPrev(p);
        const HOPToken* rb;
        p->nodes[n].end = lb->end;
        for (;;) {
            int32_t argType;
            if (HOPPParseType(p, &argType) != 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, argType) != 0) {
                return -1;
            }
            if (!HOPPMatch(p, HOPTok_COMMA)) {
                break;
            }
        }
        if (HOPPExpect(p, HOPTok_RBRACK, HOPDiag_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        p->nodes[n].end = rb->end;
    }
    *out = n;
    return 0;
}

static int HOPPParseTypeParamList(HOPParser* p, int32_t ownerNode) {
    uint32_t        savedPos = p->pos;
    uint32_t        savedNodeLen = p->nodeLen;
    int32_t         lastChild = -1;
    const HOPToken* rb;
    int             sawAny = 0;
    if (ownerNode >= 0 && (uint32_t)ownerNode < p->nodeLen) {
        int32_t child = p->nodes[ownerNode].firstChild;
        while (child >= 0) {
            lastChild = child;
            child = p->nodes[child].nextSibling;
        }
    }
    if (!HOPPMatch(p, HOPTok_LBRACK)) {
        return 0;
    }
    for (;;) {
        const HOPToken* name;
        int32_t         paramNode;
        if (!HOPPAt(p, HOPTok_IDENT)) {
            p->pos = savedPos;
            p->nodeLen = savedNodeLen;
            if (lastChild >= 0) {
                p->nodes[lastChild].nextSibling = -1;
            } else if (ownerNode >= 0 && (uint32_t)ownerNode < p->nodeLen) {
                p->nodes[ownerNode].firstChild = -1;
            }
            return 0;
        }
        sawAny = 1;
        if (HOPPExpectDeclName(p, &name, 0) != 0) {
            return -1;
        }
        paramNode = HOPPNewNode(p, HOPAst_TYPE_PARAM, name->start, name->end);
        if (paramNode < 0) {
            return -1;
        }
        p->nodes[paramNode].dataStart = name->start;
        p->nodes[paramNode].dataEnd = name->end;
        if (HOPPAddChild(p, ownerNode, paramNode) != 0) {
            return -1;
        }
        if (!HOPPMatch(p, HOPTok_COMMA)) {
            break;
        }
    }
    if (!sawAny || HOPPExpect(p, HOPTok_RBRACK, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        p->pos = savedPos;
        p->nodeLen = savedNodeLen;
        if (lastChild >= 0) {
            p->nodes[lastChild].nextSibling = -1;
        } else if (ownerNode >= 0 && (uint32_t)ownerNode < p->nodeLen) {
            p->nodes[ownerNode].firstChild = -1;
        }
        return 0;
    }
    p->nodes[ownerNode].end = rb->end;
    return 0;
}

static int HOPPTryParseFnTypeNamedParamGroup(
    HOPParser* p, int32_t fnTypeNode, int* outConsumedGroup) {
    uint32_t savedPos = p->pos;
    uint32_t savedNodeLen = p->nodeLen;
    HOPDiag  savedDiag = { 0 };
    uint32_t nameCount = 0;
    int32_t  typeNode = -1;
    uint32_t i;

    if (p->diag != NULL) {
        savedDiag = *p->diag;
    }
    if (!HOPPAt(p, HOPTok_IDENT)) {
        *outConsumedGroup = 0;
        return 0;
    }

    for (;;) {
        const HOPToken* name;
        if (HOPPExpectDeclName(p, &name, 0) != 0) {
            goto not_group;
        }
        nameCount++;
        if (!HOPPMatch(p, HOPTok_COMMA)) {
            break;
        }
        if (!HOPPAt(p, HOPTok_IDENT)) {
            goto not_group;
        }
    }

    if (nameCount == 0 || !HOPPIsTypeStart(HOPPPeek(p)->kind)) {
        goto not_group;
    }
    if (HOPPParseType(p, &typeNode) != 0) {
        goto not_group;
    }

    for (i = 0; i < nameCount; i++) {
        int32_t paramType = -1;
        if (i == 0) {
            paramType = typeNode;
        } else if (HOPPCloneSubtree(p, typeNode, &paramType) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, fnTypeNode, paramType) != 0) {
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

static int HOPPParseFnType(HOPParser* p, int32_t* out) {
    const HOPToken* fnTok;
    const HOPToken* rp;
    int32_t         fnTypeNode;
    int             sawVariadic = 0;

    if (HOPPExpect(p, HOPTok_FN, HOPDiag_EXPECTED_TYPE, &fnTok) != 0) {
        return -1;
    }
    if (HOPPExpect(p, HOPTok_LPAREN, HOPDiag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }

    fnTypeNode = HOPPNewNode(p, HOPAst_TYPE_FN, fnTok->start, rp->end);
    if (fnTypeNode < 0) {
        return -1;
    }

    if (!HOPPAt(p, HOPTok_RPAREN)) {
        for (;;) {
            int consumedGroup = 0;
            int isConstParam = HOPPMatch(p, HOPTok_CONST);
            if (isConstParam) {
                uint32_t savedPos = p->pos;
                uint32_t savedNodeLen = p->nodeLen;
                HOPDiag  savedDiag = { 0 };
                int32_t  paramType = -1;
                int      isVariadicParam = 0;
                if (p->diag != NULL) {
                    savedDiag = *p->diag;
                }
                if (HOPPMatch(p, HOPTok_ELLIPSIS)) {
                    isVariadicParam = 1;
                    if (sawVariadic) {
                        return HOPPFail(p, HOPDiag_VARIADIC_PARAM_DUPLICATE);
                    }
                    if (HOPPParseType(p, &paramType) != 0) {
                        return -1;
                    }
                } else if (HOPPAt(p, HOPTok_IDENT)) {
                    const HOPToken* nameTok = NULL;
                    if (HOPPExpectDeclName(p, &nameTok, 0) != 0) {
                        return -1;
                    }
                    if (HOPPAt(p, HOPTok_COMMA)) {
                        p->pos = savedPos;
                        p->nodeLen = savedNodeLen;
                        if (p->diag != NULL) {
                            *p->diag = savedDiag;
                        }
                    }
                    if (paramType < 0 && HOPPMatch(p, HOPTok_ELLIPSIS)) {
                        isVariadicParam = 1;
                        if (sawVariadic) {
                            return HOPPFail(p, HOPDiag_VARIADIC_PARAM_DUPLICATE);
                        }
                        if (HOPPParseType(p, &paramType) != 0) {
                            return -1;
                        }
                    } else if (paramType < 0 && HOPPIsTypeStart(HOPPPeek(p)->kind)) {
                        if (HOPPParseType(p, &paramType) != 0) {
                            return -1;
                        }
                    } else if (paramType < 0) {
                        p->pos = savedPos;
                        p->nodeLen = savedNodeLen;
                        if (p->diag != NULL) {
                            *p->diag = savedDiag;
                        }
                    }
                }
                if (paramType < 0) {
                    if (HOPPParseType(p, &paramType) != 0) {
                        return -1;
                    }
                }
                if (isVariadicParam) {
                    p->nodes[paramType].flags |= HOPAstFlag_PARAM_VARIADIC;
                    sawVariadic = 1;
                }
                p->nodes[paramType].flags |= HOPAstFlag_PARAM_CONST;
                if (HOPPAddChild(p, fnTypeNode, paramType) != 0) {
                    return -1;
                }
            } else if (HOPPMatch(p, HOPTok_ELLIPSIS)) {
                int32_t paramType = -1;
                if (sawVariadic) {
                    return HOPPFail(p, HOPDiag_VARIADIC_PARAM_DUPLICATE);
                }
                if (HOPPParseType(p, &paramType) != 0) {
                    return -1;
                }
                p->nodes[paramType].flags |= HOPAstFlag_PARAM_VARIADIC;
                if (HOPPAddChild(p, fnTypeNode, paramType) != 0) {
                    return -1;
                }
                sawVariadic = 1;
            } else if (
                HOPPAt(p, HOPTok_IDENT)
                && HOPPTryParseFnTypeNamedParamGroup(p, fnTypeNode, &consumedGroup) != 0)
            {
                return -1;
            }
            if (!consumedGroup && !isConstParam) {
                int32_t paramType = -1;
                if (HOPPParseType(p, &paramType) != 0) {
                    return -1;
                }
                if (HOPPAddChild(p, fnTypeNode, paramType) != 0) {
                    return -1;
                }
            }
            if (!HOPPMatch(p, HOPTok_COMMA)) {
                break;
            }
            if (sawVariadic) {
                return HOPPFail(p, HOPDiag_VARIADIC_PARAM_NOT_LAST);
            }
        }
    }

    if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }
    p->nodes[fnTypeNode].end = rp->end;

    if (HOPPIsTypeStart(HOPPPeek(p)->kind)) {
        int32_t resultType = -1;
        if (HOPPParseType(p, &resultType) != 0) {
            return -1;
        }
        p->nodes[resultType].flags = 1;
        if (HOPPAddChild(p, fnTypeNode, resultType) != 0) {
            return -1;
        }
        p->nodes[fnTypeNode].end = p->nodes[resultType].end;
    }

    *out = fnTypeNode;
    return 0;
}

static int HOPPParseTupleType(HOPParser* p, int32_t* out) {
    const HOPToken* lp = NULL;
    const HOPToken* rp = NULL;
    int32_t         items[256];
    uint32_t        itemCount = 0;
    int32_t         tupleNode;
    uint32_t        i;

    if (HOPPExpect(p, HOPTok_LPAREN, HOPDiag_EXPECTED_TYPE, &lp) != 0) {
        return -1;
    }
    if (HOPPParseType(p, &items[itemCount]) != 0) {
        return -1;
    }
    itemCount++;
    if (!HOPPMatch(p, HOPTok_COMMA)) {
        return HOPPFail(p, HOPDiag_EXPECTED_TYPE);
    }
    for (;;) {
        if (itemCount >= (uint32_t)(sizeof(items) / sizeof(items[0]))) {
            return HOPPFail(p, HOPDiag_ARENA_OOM);
        }
        if (HOPPParseType(p, &items[itemCount]) != 0) {
            return -1;
        }
        itemCount++;
        if (!HOPPMatch(p, HOPTok_COMMA)) {
            break;
        }
    }
    if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }

    tupleNode = HOPPNewNode(p, HOPAst_TYPE_TUPLE, lp->start, rp->end);
    if (tupleNode < 0) {
        return -1;
    }
    for (i = 0; i < itemCount; i++) {
        if (HOPPAddChild(p, tupleNode, items[i]) != 0) {
            return -1;
        }
    }
    *out = tupleNode;
    return 0;
}

static int HOPPParseFnResultClause(HOPParser* p, int32_t fnNode) {
    const HOPToken* lp = NULL;
    const HOPToken* rp = NULL;
    int32_t         resultTypes[256];
    uint32_t        resultCount = 0;
    int32_t         resultTypeNode = -1;
    uint32_t        i;

    if (HOPPExpect(p, HOPTok_LPAREN, HOPDiag_EXPECTED_TYPE, &lp) != 0) {
        return -1;
    }
    if (HOPPAt(p, HOPTok_RPAREN)) {
        return HOPPFail(p, HOPDiag_EXPECTED_TYPE);
    }

    for (;;) {
        int consumedGroup = 0;
        if (HOPPAt(p, HOPTok_IDENT)) {
            uint32_t savedPos = p->pos;
            uint32_t savedNodeLen = p->nodeLen;
            HOPDiag  savedDiag = { 0 };
            uint32_t nameCount = 0;
            int32_t  groupType = -1;
            if (p->diag != NULL) {
                savedDiag = *p->diag;
            }
            for (;;) {
                const HOPToken* name;
                if (HOPPExpectDeclName(p, &name, 0) != 0) {
                    break;
                }
                (void)name;
                nameCount++;
                if (!HOPPMatch(p, HOPTok_COMMA)) {
                    break;
                }
                if (!HOPPAt(p, HOPTok_IDENT)) {
                    break;
                }
            }
            if (nameCount > 0 && HOPPIsTypeStart(HOPPPeek(p)->kind)
                && HOPPParseType(p, &groupType) == 0)
            {
                for (i = 0; i < nameCount; i++) {
                    int32_t itemType = -1;
                    if (resultCount >= (uint32_t)(sizeof(resultTypes) / sizeof(resultTypes[0]))) {
                        return HOPPFail(p, HOPDiag_ARENA_OOM);
                    }
                    if (i == 0) {
                        itemType = groupType;
                    } else if (HOPPCloneSubtree(p, groupType, &itemType) != 0) {
                        return -1;
                    }
                    resultTypes[resultCount++] = itemType;
                }
                consumedGroup = 1;
            } else {
                p->pos = savedPos;
                p->nodeLen = savedNodeLen;
                if (p->diag != NULL) {
                    *p->diag = savedDiag;
                }
            }
        }
        if (!consumedGroup) {
            if (resultCount >= (uint32_t)(sizeof(resultTypes) / sizeof(resultTypes[0]))) {
                return HOPPFail(p, HOPDiag_ARENA_OOM);
            }
            if (HOPPParseType(p, &resultTypes[resultCount]) != 0) {
                return -1;
            }
            resultCount++;
        }
        if (!HOPPMatch(p, HOPTok_COMMA)) {
            break;
        }
    }

    if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }
    if (resultCount == 0) {
        return HOPPFail(p, HOPDiag_EXPECTED_TYPE);
    }

    if (resultCount == 1) {
        resultTypeNode = resultTypes[0];
    } else {
        resultTypeNode = HOPPNewNode(p, HOPAst_TYPE_TUPLE, lp->start, rp->end);
        if (resultTypeNode < 0) {
            return -1;
        }
        for (i = 0; i < resultCount; i++) {
            if (HOPPAddChild(p, resultTypeNode, resultTypes[i]) != 0) {
                return -1;
            }
        }
    }

    p->nodes[resultTypeNode].flags = 1;
    if (HOPPAddChild(p, fnNode, resultTypeNode) != 0) {
        return -1;
    }
    p->nodes[fnNode].end = p->nodes[resultTypeNode].end;
    return 0;
}

static int HOPPParseAnonymousAggregateFieldDeclList(HOPParser* p, int32_t aggTypeNode) {
    while (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
        const HOPToken* names[256];
        uint32_t        nameCount = 0;
        int32_t         typeNode = -1;
        int32_t         defaultExpr = -1;
        uint32_t        i;

        if (HOPPMatch(p, HOPTok_SEMICOLON)) {
            continue;
        }
        if (HOPPExpectDeclName(p, &names[nameCount], 0) != 0) {
            return -1;
        }
        nameCount++;
        while (HOPPMatch(p, HOPTok_COMMA)) {
            if (!HOPPAt(p, HOPTok_IDENT)) {
                return HOPPFail(p, HOPDiag_EXPECTED_TYPE);
            }
            if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                return HOPPFail(p, HOPDiag_ARENA_OOM);
            }
            if (HOPPExpectDeclName(p, &names[nameCount], 0) != 0) {
                return -1;
            }
            nameCount++;
        }

        if (HOPPParseType(p, &typeNode) != 0) {
            return -1;
        }
        if (HOPPMatch(p, HOPTok_ASSIGN)) {
            if (nameCount > 1) {
                const HOPToken* eq = HOPPPrev(p);
                HOPPSetDiag(p->diag, HOPDiag_UNEXPECTED_TOKEN, eq->start, eq->end);
                return -1;
            }
            if (HOPPParseExpr(p, 1, &defaultExpr) != 0) {
                return -1;
            }
        }
        for (i = 0; i < nameCount; i++) {
            int32_t fieldNode;
            int32_t fieldTypeNode;
            if (i == 0) {
                fieldTypeNode = typeNode;
            } else {
                if (HOPPCloneSubtree(p, typeNode, &fieldTypeNode) != 0) {
                    return -1;
                }
            }
            fieldNode = HOPPNewNode(p, HOPAst_FIELD, names[i]->start, p->nodes[fieldTypeNode].end);
            if (fieldNode < 0) {
                return -1;
            }
            p->nodes[fieldNode].dataStart = names[i]->start;
            p->nodes[fieldNode].dataEnd = names[i]->end;
            if (HOPPAddChild(p, fieldNode, fieldTypeNode) != 0) {
                return -1;
            }
            if (i == 0 && defaultExpr >= 0) {
                p->nodes[fieldNode].end = p->nodes[defaultExpr].end;
                if (HOPPAddChild(p, fieldNode, defaultExpr) != 0) {
                    return -1;
                }
            }
            if (HOPPAddChild(p, aggTypeNode, fieldNode) != 0) {
                return -1;
            }
        }

        if (HOPPMatch(p, HOPTok_SEMICOLON)) {
            continue;
        }
        if (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
    }
    return 0;
}

static int HOPPParseAnonymousAggregateType(HOPParser* p, int32_t* out) {
    const HOPToken* kw = NULL;
    const HOPToken* lb;
    const HOPToken* rb;
    HOPAstKind      kind = HOPAst_TYPE_ANON_STRUCT;
    int32_t         typeNode;

    if (HOPPAt(p, HOPTok_STRUCT) || HOPPAt(p, HOPTok_UNION)) {
        p->pos++;
        kw = HOPPPrev(p);
        if (kw->kind == HOPTok_UNION) {
            kind = HOPAst_TYPE_ANON_UNION;
        }
    }

    if (HOPPExpect(p, HOPTok_LBRACE, HOPDiag_EXPECTED_TYPE, &lb) != 0) {
        return -1;
    }
    typeNode = HOPPNewNode(p, kind, kw != NULL ? kw->start : lb->start, lb->end);
    if (typeNode < 0) {
        return -1;
    }
    if (HOPPParseAnonymousAggregateFieldDeclList(p, typeNode) != 0) {
        return -1;
    }
    if (HOPPExpect(p, HOPTok_RBRACE, HOPDiag_EXPECTED_TYPE, &rb) != 0) {
        return -1;
    }
    p->nodes[typeNode].end = rb->end;
    *out = typeNode;
    return 0;
}

static int HOPPParseType(HOPParser* p, int32_t* out) {
    const HOPToken* t;
    int32_t         typeNode;
    int32_t         child;

    /* Prefix '?' optional type. */
    if (HOPPMatch(p, HOPTok_QUESTION)) {
        t = HOPPPrev(p);
        typeNode = HOPPNewNode(p, HOPAst_TYPE_OPTIONAL, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (HOPPParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return HOPPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (HOPPMatch(p, HOPTok_MUL)) {
        t = HOPPPrev(p);
        typeNode = HOPPNewNode(p, HOPAst_TYPE_PTR, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (HOPPParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return HOPPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (HOPPMatch(p, HOPTok_AND)) {
        t = HOPPPrev(p);
        typeNode = HOPPNewNode(p, HOPAst_TYPE_REF, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (HOPPParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return HOPPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (HOPPMatch(p, HOPTok_MUT)) {
        (void)HOPPPrev(p);
        return HOPPFail(p, HOPDiag_EXPECTED_TYPE);
    }

    if (HOPPAt(p, HOPTok_LPAREN)) {
        return HOPPParseTupleType(p, out);
    }

    if (HOPPMatch(p, HOPTok_LBRACK)) {
        const HOPToken* lb = HOPPPrev(p);
        const HOPToken* nTok = NULL;
        int32_t         lenExpr = -1;
        const HOPToken* rb;
        HOPAstKind      kind;

        if (HOPPParseType(p, &child) != 0) {
            return -1;
        }

        if (HOPPMatch(p, HOPTok_RBRACK)) {
            rb = HOPPPrev(p);
            typeNode = HOPPNewNode(p, HOPAst_TYPE_SLICE, lb->start, rb->end);
            if (typeNode < 0) {
                return -1;
            }
            return HOPPAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
        }

        if (HOPPMatch(p, HOPTok_DOT)) {
            kind = HOPAst_TYPE_VARRAY;
            if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_EXPECTED_TYPE, &nTok) != 0) {
                return -1;
            }
        } else {
            kind = HOPAst_TYPE_ARRAY;
            if (HOPPParseExpr(p, 1, &lenExpr) != 0) {
                return -1;
            }
        }

        typeNode = HOPPNewNode(p, kind, lb->start, lb->end);
        if (typeNode < 0) {
            return -1;
        }
        if (nTok != NULL) {
            p->nodes[typeNode].dataStart = nTok->start;
            p->nodes[typeNode].dataEnd = nTok->end;
        }
        if (HOPPExpect(p, HOPTok_RBRACK, HOPDiag_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = rb->end;
        if (HOPPAddChild(p, typeNode, child) != 0) {
            return -1;
        }
        if (kind == HOPAst_TYPE_ARRAY) {
            const HOPAstNode* lenNode = &p->nodes[lenExpr];
            if ((lenNode->kind == HOPAst_INT && lenNode->dataEnd > lenNode->dataStart)
                && (lenNode->flags & HOPAstFlag_PAREN) == 0)
            {
                p->nodes[typeNode].dataStart = lenNode->dataStart;
                p->nodes[typeNode].dataEnd = lenNode->dataEnd;
            } else {
                p->nodes[typeNode].dataStart = lenNode->start;
                p->nodes[typeNode].dataEnd = lenNode->end;
                if (HOPPAddChild(p, typeNode, lenExpr) != 0) {
                    return -1;
                }
            }
        }
        *out = typeNode;
        return 0;
    }

    if (HOPPAt(p, HOPTok_FN)) {
        return HOPPParseFnType(p, out);
    }

    if (HOPPAt(p, HOPTok_LBRACE) || HOPPAt(p, HOPTok_STRUCT) || HOPPAt(p, HOPTok_UNION)) {
        return HOPPParseAnonymousAggregateType(p, out);
    }

    if (HOPPParseTypeName(p, out) != 0) {
        return -1;
    }
    return 0;
}

static int HOPPParseCompoundLiteralTail(HOPParser* p, int32_t typeNode, int32_t* out) {
    const HOPToken* lb;
    const HOPToken* rb;
    int32_t         lit;

    if (HOPPExpect(p, HOPTok_LBRACE, HOPDiag_EXPECTED_EXPR, &lb) != 0) {
        return -1;
    }

    lit = HOPPNewNode(
        p, HOPAst_COMPOUND_LIT, typeNode >= 0 ? p->nodes[typeNode].start : lb->start, lb->end);
    if (lit < 0) {
        return -1;
    }
    if (typeNode >= 0 && HOPPAddChild(p, lit, typeNode) != 0) {
        return -1;
    }

    while (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
        const HOPToken* fieldName;
        int32_t         field;
        int32_t         expr = -1;
        int             hasDottedPath = 0;

        if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_UNEXPECTED_TOKEN, &fieldName) != 0) {
            return -1;
        }
        field = HOPPNewNode(p, HOPAst_COMPOUND_FIELD, fieldName->start, fieldName->end);
        if (field < 0) {
            return -1;
        }
        p->nodes[field].dataStart = fieldName->start;
        p->nodes[field].dataEnd = fieldName->end;
        while (HOPPMatch(p, HOPTok_DOT)) {
            const HOPToken* seg;
            if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_UNEXPECTED_TOKEN, &seg) != 0) {
                return -1;
            }
            p->nodes[field].dataEnd = seg->end;
            hasDottedPath = 1;
        }
        if (HOPPMatch(p, HOPTok_COLON)) {
            if (HOPPParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (HOPPAddChild(p, field, expr) != 0) {
                return -1;
            }
            p->nodes[field].end = p->nodes[expr].end;
        } else {
            if (hasDottedPath) {
                return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
            }
            p->nodes[field].flags |= HOPAstFlag_COMPOUND_FIELD_SHORTHAND;
        }
        if (HOPPAddChild(p, lit, field) != 0) {
            return -1;
        }

        if (HOPPMatch(p, HOPTok_COMMA) || HOPPMatch(p, HOPTok_SEMICOLON)) {
            if (HOPPAt(p, HOPTok_RBRACE)) {
                break;
            }
            continue;
        }
        break;
    }

    if (HOPPExpect(p, HOPTok_RBRACE, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[lit].end = rb->end;
    *out = lit;
    return 0;
}

static int HOPPParseNewExpr(HOPParser* p, int32_t* out) {
    const HOPToken* kw = HOPPPrev(p);
    const HOPToken* rb;
    int32_t         n;
    int32_t         typeNode;
    int32_t         countNode = -1;
    int32_t         initNode = -1;
    int32_t         allocNode = -1;

    n = HOPPNewNode(p, HOPAst_NEW, kw->start, kw->end);
    if (n < 0) {
        return -1;
    }

    if (HOPPMatch(p, HOPTok_LBRACK)) {
        if (HOPPParseType(p, &typeNode) != 0) {
            return -1;
        }
        if (HOPPParseExpr(p, 1, &countNode) != 0) {
            return -1;
        }
        if (HOPPExpect(p, HOPTok_RBRACK, HOPDiag_EXPECTED_EXPR, &rb) != 0) {
            return -1;
        }
        p->nodes[n].flags |= HOPAstFlag_NEW_HAS_COUNT;
        p->nodes[n].end = rb->end;
    } else {
        if (HOPPParseType(p, &typeNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[typeNode].end;
        if (HOPPAt(p, HOPTok_LBRACE)) {
            if (HOPPParseCompoundLiteralTail(p, -1, &initNode) != 0) {
                return -1;
            }
            p->nodes[n].flags |= HOPAstFlag_NEW_HAS_INIT;
            p->nodes[n].end = p->nodes[initNode].end;
        }
    }

    if (HOPPMatch(p, HOPTok_IN)) {
        if (HOPPParseExpr(p, 1, &allocNode) != 0) {
            return -1;
        }
        p->nodes[n].flags |= HOPAstFlag_NEW_HAS_ALLOC;
        p->nodes[n].end = p->nodes[allocNode].end;
    }

    if (HOPPAddChild(p, n, typeNode) != 0) {
        return -1;
    }
    if (countNode >= 0) {
        if (HOPPAddChild(p, n, countNode) != 0) {
            return -1;
        }
    }
    if (initNode >= 0) {
        if (HOPPAddChild(p, n, initNode) != 0) {
            return -1;
        }
    }
    if (allocNode >= 0) {
        if (HOPPAddChild(p, n, allocNode) != 0) {
            return -1;
        }
    }

    *out = n;
    return 0;
}

static int HOPPParsePrimary(HOPParser* p, int32_t* out) {
    const HOPToken* t = HOPPPeek(p);
    int32_t         n;

    if (HOPPAt(p, HOPTok_IDENT)) {
        uint32_t savedPos = p->pos;
        uint32_t savedNodeLen = p->nodeLen;
        HOPDiag  savedDiag = { 0 };
        int32_t  typeNode;
        if (p->diag != NULL) {
            savedDiag = *p->diag;
        }
        if (HOPPParseTypeName(p, &typeNode) == 0 && HOPPAt(p, HOPTok_LBRACE)
            && HOPPPeek(p)->start == p->nodes[typeNode].end)
        {
            return HOPPParseCompoundLiteralTail(p, typeNode, out);
        }
        p->pos = savedPos;
        p->nodeLen = savedNodeLen;
        if (p->diag != NULL) {
            *p->diag = savedDiag;
        }
    }

    if (HOPPAt(p, HOPTok_LBRACE)) {
        return HOPPParseCompoundLiteralTail(p, -1, out);
    }

    if (HOPPAt(p, HOPTok_TYPE) && (p->pos + 1u) < p->tokLen
        && HOPPIsTypeStart(p->tok[p->pos + 1u].kind))
    {
        int32_t typeNode;
        p->pos++;
        t = HOPPPrev(p);
        n = HOPPNewNode(p, HOPAst_TYPE_VALUE, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        if (HOPPParseType(p, &typeNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[typeNode].end;
        if (HOPPAddChild(p, n, typeNode) != 0) {
            return -1;
        }
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_IDENT) || HOPPMatch(p, HOPTok_TYPE)) {
        t = HOPPPrev(p);
        n = HOPPNewNode(p, HOPAst_IDENT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_INT)) {
        n = HOPPNewNode(p, HOPAst_INT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_FLOAT)) {
        n = HOPPNewNode(p, HOPAst_FLOAT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_STRING)) {
        n = HOPPNewNode(p, HOPAst_STRING, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_RUNE)) {
        n = HOPPNewNode(p, HOPAst_RUNE, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_TRUE) || HOPPMatch(p, HOPTok_FALSE)) {
        t = HOPPPrev(p);
        n = HOPPNewNode(p, HOPAst_BOOL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_NULL)) {
        t = HOPPPrev(p);
        n = HOPPNewNode(p, HOPAst_NULL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_LPAREN)) {
        const HOPToken* lp = HOPPPrev(p);
        int32_t         firstExpr = -1;
        int32_t         exprItems[256];
        uint32_t        exprCount = 0;
        int32_t         tupleExpr = -1;
        uint32_t        i;
        if (HOPPParseExpr(p, 1, &firstExpr) != 0) {
            return -1;
        }
        if (!HOPPMatch(p, HOPTok_COMMA)) {
            *out = firstExpr;
            if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            if (*out >= 0) {
                p->nodes[*out].flags |= HOPAstFlag_PAREN;
            }
            return 0;
        }

        exprItems[exprCount++] = firstExpr;
        for (;;) {
            if (exprCount >= (uint32_t)(sizeof(exprItems) / sizeof(exprItems[0]))) {
                return HOPPFail(p, HOPDiag_ARENA_OOM);
            }
            if (HOPPParseExpr(p, 1, &exprItems[exprCount]) != 0) {
                return -1;
            }
            exprCount++;
            if (!HOPPMatch(p, HOPTok_COMMA)) {
                break;
            }
        }
        if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_EXPR, &t) != 0) {
            return -1;
        }
        tupleExpr = HOPPNewNode(p, HOPAst_TUPLE_EXPR, lp->start, t->end);
        if (tupleExpr < 0) {
            return -1;
        }
        for (i = 0; i < exprCount; i++) {
            if (HOPPAddChild(p, tupleExpr, exprItems[i]) != 0) {
                return -1;
            }
        }
        *out = tupleExpr;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_SIZEOF)) {
        const HOPToken* kw = HOPPPrev(p);
        const HOPToken* rp;
        int32_t         inner;
        uint32_t        savePos;
        uint32_t        saveNodeLen;
        if (HOPPExpect(p, HOPTok_LPAREN, HOPDiag_EXPECTED_EXPR, &t) != 0) {
            return -1;
        }
        n = HOPPNewNode(p, HOPAst_SIZEOF, kw->start, t->end);
        if (n < 0) {
            return -1;
        }

        savePos = p->pos;
        saveNodeLen = p->nodeLen;
        if (HOPPParseType(p, &inner) == 0 && HOPPAt(p, HOPTok_RPAREN)) {
            p->nodes[n].flags = 1;
            if (HOPPAddChild(p, n, inner) != 0) {
                return -1;
            }
            if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_EXPR, &rp) != 0) {
                return -1;
            }
            p->nodes[n].end = rp->end;
            *out = n;
            return 0;
        }
        p->pos = savePos;
        p->nodeLen = saveNodeLen;
        if (p->diag != NULL) {
            *p->diag = (HOPDiag){ 0 };
        }

        if (HOPPParseExpr(p, 1, &inner) != 0) {
            return -1;
        }
        p->nodes[n].flags = 0;
        if (HOPPAddChild(p, n, inner) != 0) {
            return -1;
        }
        if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_EXPR, &rp) != 0) {
            return -1;
        }
        p->nodes[n].end = rp->end;
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_NEW)) {
        return HOPPParseNewExpr(p, out);
    }

    return HOPPFail(p, HOPDiag_EXPECTED_EXPR);
}

static int HOPPParsePostfix(HOPParser* p, int32_t* expr) {
    for (;;) {
        int32_t         n;
        const HOPToken* t;

        if (HOPPMatch(p, HOPTok_LPAREN)) {
            n = HOPPNewNode(p, HOPAst_CALL, p->nodes[*expr].start, HOPPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (!HOPPAt(p, HOPTok_RPAREN)) {
                for (;;) {
                    int32_t         arg;
                    int32_t         argNode;
                    const HOPToken* labelTok = NULL;
                    const HOPToken* spreadTok = NULL;
                    uint32_t        argStart = HOPPPeek(p)->start;
                    if (HOPPAt(p, HOPTok_IDENT) && (p->pos + 1u) < p->tokLen
                        && p->tok[p->pos + 1u].kind == HOPTok_COLON)
                    {
                        labelTok = HOPPPeek(p);
                        p->pos += 2u;
                    }
                    if (HOPPParseExpr(p, 1, &arg) != 0) {
                        return -1;
                    }
                    if (HOPPMatch(p, HOPTok_ELLIPSIS)) {
                        spreadTok = HOPPPrev(p);
                    }
                    argNode = HOPPNewNode(
                        p,
                        HOPAst_CALL_ARG,
                        labelTok != NULL ? labelTok->start : argStart,
                        spreadTok != NULL ? spreadTok->end : p->nodes[arg].end);
                    if (argNode < 0) {
                        return -1;
                    }
                    if (labelTok != NULL) {
                        p->nodes[argNode].dataStart = labelTok->start;
                        p->nodes[argNode].dataEnd = labelTok->end;
                    }
                    if (spreadTok != NULL) {
                        p->nodes[argNode].flags |= HOPAstFlag_CALL_ARG_SPREAD;
                    }
                    if (HOPPAddChild(p, argNode, arg) != 0 || HOPPAddChild(p, n, argNode) != 0) {
                        return -1;
                    }
                    if (!HOPPMatch(p, HOPTok_COMMA)) {
                        break;
                    }
                }
            }
            if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (HOPPMatch(p, HOPTok_LBRACK)) {
            int32_t firstExpr = -1;
            n = HOPPNewNode(p, HOPAst_INDEX, p->nodes[*expr].start, HOPPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, *expr) != 0) {
                return -1;
            }

            if (HOPPMatch(p, HOPTok_COLON)) {
                p->nodes[n].flags |= HOPAstFlag_INDEX_SLICE;
                if (!HOPPAt(p, HOPTok_RBRACK)) {
                    if (HOPPParseExpr(p, 1, &firstExpr) != 0) {
                        return -1;
                    }
                    p->nodes[n].flags |= HOPAstFlag_INDEX_HAS_END;
                    if (HOPPAddChild(p, n, firstExpr) != 0) {
                        return -1;
                    }
                }
                if (HOPPExpect(p, HOPTok_RBRACK, HOPDiag_EXPECTED_EXPR, &t) != 0) {
                    return -1;
                }
                p->nodes[n].end = t->end;
                *expr = n;
                continue;
            }

            if (HOPPParseExpr(p, 1, &firstExpr) != 0) {
                return -1;
            }
            if (HOPPMatch(p, HOPTok_COLON)) {
                int32_t endExpr = -1;
                p->nodes[n].flags |= HOPAstFlag_INDEX_SLICE;
                p->nodes[n].flags |= HOPAstFlag_INDEX_HAS_START;
                if (HOPPAddChild(p, n, firstExpr) != 0) {
                    return -1;
                }
                if (!HOPPAt(p, HOPTok_RBRACK)) {
                    if (HOPPParseExpr(p, 1, &endExpr) != 0) {
                        return -1;
                    }
                    p->nodes[n].flags |= HOPAstFlag_INDEX_HAS_END;
                    if (HOPPAddChild(p, n, endExpr) != 0) {
                        return -1;
                    }
                }
                if (HOPPExpect(p, HOPTok_RBRACK, HOPDiag_EXPECTED_EXPR, &t) != 0) {
                    return -1;
                }
                p->nodes[n].end = t->end;
                *expr = n;
                continue;
            }

            if (HOPPAddChild(p, n, firstExpr) != 0) {
                return -1;
            }
            if (HOPPExpect(p, HOPTok_RBRACK, HOPDiag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (HOPPMatch(p, HOPTok_DOT)) {
            const HOPToken* fieldTok;
            n = HOPPNewNode(p, HOPAst_FIELD_EXPR, p->nodes[*expr].start, HOPPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_EXPECTED_EXPR, &fieldTok) != 0) {
                return -1;
            }
            p->nodes[n].dataStart = fieldTok->start;
            p->nodes[n].dataEnd = fieldTok->end;
            p->nodes[n].end = fieldTok->end;
            *expr = n;
            continue;
        }

        if (HOPPMatch(p, HOPTok_AS)) {
            int32_t typeNode;
            n = HOPPNewNode(p, HOPAst_CAST, p->nodes[*expr].start, HOPPPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)HOPTok_AS;
            if (HOPPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (HOPPParseType(p, &typeNode) != 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, typeNode) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[typeNode].end;
            *expr = n;
            continue;
        }

        if (HOPPMatch(p, HOPTok_NOT)) {
            t = HOPPPrev(p);
            n = HOPPNewNode(p, HOPAst_UNWRAP, p->nodes[*expr].start, t->end);
            if (n < 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, *expr) != 0) {
                return -1;
            }
            *expr = n;
            continue;
        }

        break;
    }
    return 0;
}

static int HOPPParsePrefix(HOPParser* p, int32_t* out) {
    HOPTokenKind    op = HOPPPeek(p)->kind;
    int32_t         rhs;
    int32_t         n;
    const HOPToken* t = HOPPPeek(p);

    switch (op) {
        case HOPTok_ADD:
        case HOPTok_SUB:
        case HOPTok_NOT:
        case HOPTok_MUL:
        case HOPTok_AND:
            p->pos++;
            if (HOPPParsePrefix(p, &rhs) != 0) {
                return -1;
            }
            n = HOPPNewNode(p, HOPAst_UNARY, t->start, p->nodes[rhs].end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)op;
            if (HOPPAddChild(p, n, rhs) != 0) {
                return -1;
            }
            *out = n;
            return 0;
        default:
            if (HOPPParsePrimary(p, out) != 0) {
                return -1;
            }
            return HOPPParsePostfix(p, out);
    }
}

static int HOPPParseExpr(HOPParser* p, int minPrec, int32_t* out) {
    int32_t lhs;
    if (HOPPParsePrefix(p, &lhs) != 0) {
        return -1;
    }

    for (;;) {
        HOPTokenKind op = HOPPPeek(p)->kind;
        int          prec = HOPBinPrec(op);
        int          rightAssoc = HOPIsAssignmentOp(op);
        int32_t      rhs;
        int32_t      n;

        if (prec < minPrec || prec == 0) {
            break;
        }
        p->pos++;
        if (HOPPParseExpr(p, rightAssoc ? prec : prec + 1, &rhs) != 0) {
            return -1;
        }
        n = HOPPNewNode(p, HOPAst_BINARY, p->nodes[lhs].start, p->nodes[rhs].end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].op = (uint16_t)op;
        if (HOPPAddChild(p, n, lhs) != 0 || HOPPAddChild(p, n, rhs) != 0) {
            return -1;
        }
        lhs = n;
    }

    *out = lhs;
    return 0;
}

static int HOPPBuildListNode(
    HOPParser* p, HOPAstKind kind, int32_t* items, uint32_t itemCount, int32_t* out) {
    int32_t  n;
    uint32_t i;
    if (itemCount == 0) {
        return HOPPFail(p, HOPDiag_EXPECTED_EXPR);
    }
    n = HOPPNewNode(p, kind, p->nodes[items[0]].start, p->nodes[items[itemCount - 1u]].end);
    if (n < 0) {
        return -1;
    }
    for (i = 0; i < itemCount; i++) {
        if (HOPPAddChild(p, n, items[i]) != 0) {
            return -1;
        }
    }
    *out = n;
    return 0;
}

static int HOPPParseExprList(HOPParser* p, int32_t* out) {
    int32_t  items[256];
    uint32_t itemCount = 0;
    if (HOPPParseExpr(p, 1, &items[itemCount]) != 0) {
        return -1;
    }
    itemCount++;
    while (HOPPMatch(p, HOPTok_COMMA)) {
        if (itemCount >= (uint32_t)(sizeof(items) / sizeof(items[0]))) {
            return HOPPFail(p, HOPDiag_ARENA_OOM);
        }
        if (HOPPParseExpr(p, 1, &items[itemCount]) != 0) {
            return -1;
        }
        itemCount++;
    }
    return HOPPBuildListNode(p, HOPAst_EXPR_LIST, items, itemCount, out);
}

static int HOPPParseDirectiveLiteral(HOPParser* p, int32_t* out) {
    const HOPToken* t = HOPPPeek(p);
    int32_t         n;

    switch (t->kind) {
        case HOPTok_INT:
            p->pos++;
            n = HOPPNewNode(p, HOPAst_INT, t->start, t->end);
            break;
        case HOPTok_FLOAT:
            p->pos++;
            n = HOPPNewNode(p, HOPAst_FLOAT, t->start, t->end);
            break;
        case HOPTok_STRING:
            p->pos++;
            n = HOPPNewNode(p, HOPAst_STRING, t->start, t->end);
            break;
        case HOPTok_RUNE:
            p->pos++;
            n = HOPPNewNode(p, HOPAst_RUNE, t->start, t->end);
            break;
        case HOPTok_TRUE:
        case HOPTok_FALSE:
            p->pos++;
            n = HOPPNewNode(p, HOPAst_BOOL, t->start, t->end);
            break;
        case HOPTok_NULL:
            p->pos++;
            n = HOPPNewNode(p, HOPAst_NULL, t->start, t->end);
            break;
        default: return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
    }
    if (n < 0) {
        return -1;
    }
    if (p->nodes[n].kind != HOPAst_NULL) {
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
    }
    *out = n;
    return 0;
}

static int HOPPParseDirective(HOPParser* p, int32_t* out) {
    const HOPToken* atTok = NULL;
    const HOPToken* nameTok = NULL;
    const HOPToken* rp = NULL;
    int32_t         directive;

    if (HOPPExpect(p, HOPTok_AT, HOPDiag_UNEXPECTED_TOKEN, &atTok) != 0) {
        return -1;
    }
    if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_UNEXPECTED_TOKEN, &nameTok) != 0) {
        return -1;
    }
    directive = HOPPNewNode(p, HOPAst_DIRECTIVE, atTok->start, nameTok->end);
    if (directive < 0) {
        return -1;
    }
    p->nodes[directive].dataStart = nameTok->start;
    p->nodes[directive].dataEnd = nameTok->end;

    if (HOPPMatch(p, HOPTok_LPAREN)) {
        if (!HOPPAt(p, HOPTok_RPAREN)) {
            for (;;) {
                int32_t argNode = -1;
                if (HOPPParseDirectiveLiteral(p, &argNode) != 0) {
                    return -1;
                }
                if (HOPPAddChild(p, directive, argNode) != 0) {
                    return -1;
                }
                if (!HOPPMatch(p, HOPTok_COMMA)) {
                    break;
                }
                if (HOPPAt(p, HOPTok_RPAREN)) {
                    break;
                }
            }
        }
        if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_UNEXPECTED_TOKEN, &rp) != 0) {
            return -1;
        }
        p->nodes[directive].end = rp->end;
    }
    *out = directive;
    return 0;
}

static int HOPPParseDeclNameList(
    HOPParser*       p,
    int              allowHole,
    const HOPToken** names,
    uint32_t         namesCap,
    uint32_t*        outNameCount,
    const HOPToken** outFirstHole) {
    uint32_t        nameCount = 0;
    const HOPToken* hole = NULL;
    if (HOPPExpectDeclName(p, &names[nameCount], allowHole) != 0) {
        return -1;
    }
    if (hole == NULL && HOPPIsHoleName(p, names[nameCount])) {
        hole = names[nameCount];
    }
    nameCount++;
    while (HOPPMatch(p, HOPTok_COMMA)) {
        if (nameCount >= namesCap) {
            return HOPPFail(p, HOPDiag_ARENA_OOM);
        }
        if (!HOPPAt(p, HOPTok_IDENT)) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
        if (HOPPExpectDeclName(p, &names[nameCount], allowHole) != 0) {
            return -1;
        }
        if (hole == NULL && HOPPIsHoleName(p, names[nameCount])) {
            hole = names[nameCount];
        }
        nameCount++;
    }
    *outNameCount = nameCount;
    *outFirstHole = hole;
    return 0;
}

static int HOPPBuildNameListNode(
    HOPParser* p, const HOPToken** names, uint32_t nameCount, int32_t* outNameList) {
    int32_t  items[256];
    uint32_t i;
    for (i = 0; i < nameCount; i++) {
        items[i] = HOPPNewNode(p, HOPAst_IDENT, names[i]->start, names[i]->end);
        if (items[i] < 0) {
            return -1;
        }
        p->nodes[items[i]].dataStart = names[i]->start;
        p->nodes[items[i]].dataEnd = names[i]->end;
    }
    return HOPPBuildListNode(p, HOPAst_NAME_LIST, items, nameCount, outNameList);
}

static int HOPPFnHasVariadicParam(HOPParser* p, int32_t fnNode) {
    int32_t child = p->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (p->nodes[child].kind == HOPAst_PARAM
            && (p->nodes[child].flags & HOPAstFlag_PARAM_VARIADIC) != 0)
        {
            return 1;
        }
        child = p->nodes[child].nextSibling;
    }
    return 0;
}

static int HOPPParseParamGroup(HOPParser* p, int32_t fnNode, int* outIsVariadic) {
    const HOPToken* lastName = NULL;
    int32_t         firstParam = -1;
    int32_t         lastParam = -1;
    int32_t         type = -1;
    int             isVariadic = 0;
    int             isConstGroup = HOPPMatch(p, HOPTok_CONST);
    int             hasExistingVariadic = HOPPFnHasVariadicParam(p, fnNode);

    for (;;) {
        const HOPToken* name;
        int32_t         param;
        if (HOPPExpectDeclName(p, &name, 1) != 0) {
            return -1;
        }
        lastName = name;
        param = HOPPNewNode(p, HOPAst_PARAM, name->start, name->end);
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

        if (!HOPPMatch(p, HOPTok_COMMA)) {
            break;
        }
        if (isConstGroup) {
            return HOPPFail(p, HOPDiag_CONST_PARAM_GROUPED_NAME_INVALID);
        }
        if (!HOPPAt(p, HOPTok_IDENT)) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
    }

    if (HOPPMatch(p, HOPTok_ELLIPSIS)) {
        if (firstParam != lastParam) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
        if (hasExistingVariadic) {
            return HOPPFail(p, HOPDiag_VARIADIC_PARAM_DUPLICATE);
        }
        isVariadic = 1;
    } else if (hasExistingVariadic) {
        return HOPPFail(p, HOPDiag_VARIADIC_PARAM_NOT_LAST);
    }

    if (!HOPPIsTypeStart(HOPPPeek(p)->kind)) {
        if (lastName != NULL) {
            HOPPSetDiagWithArg(
                p->diag,
                HOPDiag_PARAM_MISSING_TYPE,
                lastName->start,
                lastName->end,
                lastName->start,
                lastName->end);
        } else {
            HOPPSetDiag(p->diag, HOPDiag_PARAM_MISSING_TYPE, HOPPPeek(p)->start, HOPPPeek(p)->end);
        }
        return -1;
    }

    if (HOPPParseType(p, &type) != 0) {
        return -1;
    }

    {
        int32_t param = firstParam;
        while (param >= 0) {
            int32_t nextParam = p->nodes[param].nextSibling;
            int32_t typeNode = -1;
            if (param == firstParam) {
                typeNode = type;
            } else if (HOPPCloneSubtree(p, type, &typeNode) != 0) {
                return -1;
            }
            if (HOPPAddChild(p, param, typeNode) != 0) {
                return -1;
            }
            if (isVariadic) {
                p->nodes[param].flags |= HOPAstFlag_PARAM_VARIADIC;
            }
            if (isConstGroup) {
                p->nodes[param].flags |= HOPAstFlag_PARAM_CONST;
            }
            p->nodes[param].end = p->nodes[typeNode].end;
            param = nextParam;
        }
    }

    if (HOPPAddChild(p, fnNode, firstParam) != 0) {
        return -1;
    }
    if (outIsVariadic != NULL) {
        *outIsVariadic = isVariadic;
    }
    return 0;
}

static int HOPPParseBlock(HOPParser* p, int32_t* out) {
    const HOPToken* lb;
    const HOPToken* rb;
    int32_t         block;

    if (HOPPExpect(p, HOPTok_LBRACE, HOPDiag_UNEXPECTED_TOKEN, &lb) != 0) {
        return -1;
    }
    block = HOPPNewNode(p, HOPAst_BLOCK, lb->start, lb->end);
    if (block < 0) {
        return -1;
    }

    while (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
        int32_t stmt = -1;
        if (HOPPAt(p, HOPTok_SEMICOLON)) {
            p->pos++;
            continue;
        }
        if (HOPPParseStmt(p, &stmt) != 0) {
            return -1;
        }
        if (stmt >= 0 && HOPPAddChild(p, block, stmt) != 0) {
            return -1;
        }
    }

    if (HOPPExpect(p, HOPTok_RBRACE, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[block].end = rb->end;
    *out = block;
    return 0;
}

/* Statement separators may omit trailing semicolon before closing `}`. */
static int HOPPConsumeStmtTerminator(HOPParser* p, const HOPToken** semiTok) {
    if (HOPPAt(p, HOPTok_SEMICOLON)) {
        if (semiTok != NULL) {
            *semiTok = HOPPPeek(p);
        }
        p->pos++;
        return 1;
    }
    if (HOPPAt(p, HOPTok_RBRACE)) {
        if (semiTok != NULL) {
            *semiTok = NULL;
        }
        return 0;
    }
    return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
}

static int HOPPParseVarLikeStmt(
    HOPParser* p, HOPAstKind kind, int requireSemi, int allowHole, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    const HOPToken* names[256];
    const HOPToken* firstHole = NULL;
    uint32_t        stmtStart = kw->start;
    uint32_t        nameCount = 0;
    int32_t         n;
    int32_t         nameList = -1;
    int32_t         type = -1;
    int32_t         init = -1;

    p->pos++;
    if (HOPPParseDeclNameList(
            p,
            allowHole,
            names,
            (uint32_t)(sizeof(names) / sizeof(names[0])),
            &nameCount,
            &firstHole)
        != 0)
    {
        return -1;
    }

    if (nameCount == 1u && firstHole != NULL) {
        int hasSemi;
        if (!HOPPMatch(p, HOPTok_ASSIGN)) {
            return HOPPFailReservedName(p, firstHole);
        }
        if (HOPPParseExpr(p, 1, &init) != 0) {
            return -1;
        }
        if (!requireSemi) {
            *out = init;
            return 0;
        }
        hasSemi = HOPPConsumeStmtTerminator(p, &kw);
        if (hasSemi < 0) {
            return -1;
        }
        n = HOPPNewNode(p, HOPAst_EXPR_STMT, stmtStart, hasSemi ? kw->end : p->nodes[init].end);
        if (n < 0) {
            return -1;
        }
        if (HOPPAddChild(p, n, init) != 0) {
            return -1;
        }
        *out = n;
        return 0;
    }

    if (HOPPMatch(p, HOPTok_ASSIGN)) {
        if (nameCount == 1u) {
            if (HOPPParseExpr(p, 1, &init) != 0) {
                return -1;
            }
        } else {
            if (HOPPParseExprList(p, &init) != 0) {
                return -1;
            }
        }
    } else {
        if (firstHole != NULL) {
            return HOPPFailReservedName(p, firstHole);
        }
        if (HOPPParseType(p, &type) != 0) {
            return -1;
        }
        if (HOPPMatch(p, HOPTok_ASSIGN)) {
            if (nameCount == 1u) {
                if (HOPPParseExpr(p, 1, &init) != 0) {
                    return -1;
                }
            } else if (HOPPParseExprList(p, &init) != 0) {
                return -1;
            }
        }
    }

    n = HOPPNewNode(
        p,
        kind,
        kw->start,
        init >= 0 ? p->nodes[init].end
                  : (type >= 0 ? p->nodes[type].end : names[nameCount - 1u]->end));
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = names[0]->start;
    p->nodes[n].dataEnd = names[0]->end;
    if (nameCount > 1u) {
        if (HOPPBuildNameListNode(p, names, nameCount, &nameList) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, n, nameList) != 0) {
            return -1;
        }
    }
    if (type >= 0) {
        if (HOPPAddChild(p, n, type) != 0) {
            return -1;
        }
    }
    if (init >= 0) {
        if (HOPPAddChild(p, n, init) != 0) {
            return -1;
        }
    }

    if (requireSemi) {
        int hasSemi = HOPPConsumeStmtTerminator(p, &kw);
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

static int HOPPIsShortAssignStart(HOPParser* p) {
    uint32_t i;
    if (!HOPPAt(p, HOPTok_IDENT)) {
        return 0;
    }
    i = p->pos + 1u;
    while (
        i + 1u < p->tokLen && p->tok[i].kind == HOPTok_COMMA && p->tok[i + 1u].kind == HOPTok_IDENT)
    {
        i += 2u;
    }
    return i < p->tokLen && p->tok[i].kind == HOPTok_SHORT_ASSIGN;
}

static int HOPPParseShortAssignStmt(HOPParser* p, int requireSemi, int32_t* out) {
    const HOPToken* names[256];
    const HOPToken* firstHole = NULL;
    const HOPToken* semiTok = NULL;
    uint32_t        nameCount = 0;
    int32_t         nameList = -1;
    int32_t         rhsList = -1;
    int32_t         n;

    if (HOPPParseDeclNameList(
            p, 1, names, (uint32_t)(sizeof(names) / sizeof(names[0])), &nameCount, &firstHole)
        != 0)
    {
        return -1;
    }
    (void)firstHole;
    if (HOPPExpect(p, HOPTok_SHORT_ASSIGN, HOPDiag_UNEXPECTED_TOKEN, &semiTok) != 0) {
        return -1;
    }
    if (HOPPParseExprList(p, &rhsList) != 0) {
        return -1;
    }
    if (HOPPBuildNameListNode(p, names, nameCount, &nameList) != 0) {
        return -1;
    }
    n = HOPPNewNode(p, HOPAst_SHORT_ASSIGN, p->nodes[nameList].start, p->nodes[rhsList].end);
    if (n < 0) {
        return -1;
    }
    if (HOPPAddChild(p, n, nameList) != 0 || HOPPAddChild(p, n, rhsList) != 0) {
        return -1;
    }
    if (requireSemi) {
        int hasSemi = HOPPConsumeStmtTerminator(p, &semiTok);
        if (hasSemi < 0) {
            return -1;
        }
        if (hasSemi) {
            p->nodes[n].end = semiTok->end;
        }
    }
    *out = n;
    return 0;
}

static int HOPPParseIfStmt(HOPParser* p, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    int32_t         n;
    int32_t         cond;
    int32_t         thenBlock;

    p->pos++;
    if (HOPPParseExpr(p, 1, &cond) != 0) {
        return -1;
    }
    if (HOPPParseBlock(p, &thenBlock) != 0) {
        return -1;
    }

    n = HOPPNewNode(p, HOPAst_IF, kw->start, p->nodes[thenBlock].end);
    if (n < 0) {
        return -1;
    }
    if (HOPPAddChild(p, n, cond) != 0 || HOPPAddChild(p, n, thenBlock) != 0) {
        return -1;
    }

    if (HOPPMatch(p, HOPTok_SEMICOLON) && HOPPAt(p, HOPTok_ELSE)) {
        /* Allow newline between `}` and `else`. */
    } else if (p->pos > 0 && HOPPPrev(p)->kind == HOPTok_SEMICOLON && !HOPPAt(p, HOPTok_ELSE)) {
        p->pos--;
    }

    if (HOPPMatch(p, HOPTok_ELSE)) {
        int32_t elseNode;
        if (HOPPAt(p, HOPTok_IF)) {
            if (HOPPParseIfStmt(p, &elseNode) != 0) {
                return -1;
            }
        } else {
            if (HOPPParseBlock(p, &elseNode) != 0) {
                return -1;
            }
        }
        if (HOPPAddChild(p, n, elseNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[elseNode].end;
    }

    *out = n;
    return 0;
}

static int HOPPForStmtHeadHasInToken(HOPParser* p) {
    uint32_t i = p->pos;
    while (i < p->tokLen) {
        HOPTokenKind k = p->tok[i].kind;
        if (k == HOPTok_IN) {
            return 1;
        }
        if (k == HOPTok_LBRACE || k == HOPTok_SEMICOLON || k == HOPTok_EOF) {
            break;
        }
        i++;
    }
    return 0;
}

static int HOPPNewIdentNodeFromToken(HOPParser* p, const HOPToken* tok, int32_t* out) {
    int32_t n = HOPPNewNode(p, HOPAst_IDENT, tok->start, tok->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = tok->start;
    p->nodes[n].dataEnd = tok->end;
    *out = n;
    return 0;
}

static int HOPPParseForInKeyBinding(HOPParser* p, int32_t* outIdent, uint32_t* outFlags) {
    const HOPToken* name = NULL;
    int             keyRef = 0;
    if (HOPPMatch(p, HOPTok_AND)) {
        keyRef = 1;
    }
    if (HOPPExpectDeclName(p, &name, 0) != 0) {
        return -1;
    }
    if (HOPPNewIdentNodeFromToken(p, name, outIdent) != 0) {
        return -1;
    }
    if (keyRef) {
        *outFlags |= HOPAstFlag_FOR_IN_KEY_REF;
    }
    return 0;
}

static int HOPPParseForInValueBinding(HOPParser* p, int32_t* outIdent, uint32_t* outFlags) {
    const HOPToken* name = NULL;
    int             byRef = 0;
    if (HOPPMatch(p, HOPTok_AND)) {
        byRef = 1;
    }
    if (HOPPExpectDeclName(p, &name, 1) != 0) {
        return -1;
    }
    if (HOPPIsHoleName(p, name)) {
        if (byRef) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
        *outFlags |= HOPAstFlag_FOR_IN_VALUE_DISCARD;
    } else if (byRef) {
        *outFlags |= HOPAstFlag_FOR_IN_VALUE_REF;
    }
    return HOPPNewIdentNodeFromToken(p, name, outIdent);
}

static int HOPPParseForInClause(
    HOPParser* p,
    int32_t*   outKeyBinding,
    int32_t*   outValueBinding,
    int32_t*   outSourceExpr,
    int32_t*   outBody,
    uint32_t*  outFlags) {
    uint32_t savedPos = p->pos;
    uint32_t savedNodeLen = p->nodeLen;
    HOPDiag  savedDiag = { 0 };
    int32_t  keyBinding = -1;
    int32_t  valueBinding = -1;
    int32_t  sourceExpr = -1;
    uint32_t forFlags = HOPAstFlag_FOR_IN;
    int32_t  body = -1;

    if (p->diag != NULL) {
        savedDiag = *p->diag;
    }

    if (HOPPParseForInKeyBinding(p, &keyBinding, &forFlags) == 0 && HOPPMatch(p, HOPTok_COMMA)) {
        forFlags |= HOPAstFlag_FOR_IN_HAS_KEY;
        if (HOPPParseForInValueBinding(p, &valueBinding, &forFlags) != 0) {
            return -1;
        }
        if (!HOPPMatch(p, HOPTok_IN)) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
        if (HOPPParseExpr(p, 1, &sourceExpr) != 0) {
            return -1;
        }
        if (HOPPParseBlock(p, &body) != 0) {
            return -1;
        }
        *outKeyBinding = keyBinding;
        *outValueBinding = valueBinding;
        *outSourceExpr = sourceExpr;
        *outBody = body;
        *outFlags = forFlags;
        return 0;
    }

    p->pos = savedPos;
    p->nodeLen = savedNodeLen;
    if (p->diag != NULL) {
        *p->diag = savedDiag;
    }

    valueBinding = -1;
    sourceExpr = -1;
    forFlags = HOPAstFlag_FOR_IN;
    if (HOPPParseForInValueBinding(p, &valueBinding, &forFlags) != 0) {
        return -1;
    }
    if (!HOPPMatch(p, HOPTok_IN)) {
        return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
    }
    if (HOPPParseExpr(p, 1, &sourceExpr) != 0) {
        return -1;
    }
    if (HOPPParseBlock(p, &body) != 0) {
        return -1;
    }
    *outKeyBinding = -1;
    *outValueBinding = valueBinding;
    *outSourceExpr = sourceExpr;
    *outBody = body;
    *outFlags = forFlags;
    return 0;
}

static int HOPPParseForStmt(HOPParser* p, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    int32_t         n;
    int32_t         body;
    int32_t         init = -1;
    int32_t         cond = -1;
    int32_t         post = -1;
    int32_t         keyBinding = -1;
    int32_t         valueBinding = -1;
    int32_t         sourceExpr = -1;
    uint32_t        forInFlags = 0;
    int             isForIn = 0;

    p->pos++;
    n = HOPPNewNode(p, HOPAst_FOR, kw->start, kw->end);
    if (n < 0) {
        return -1;
    }

    if (HOPPAt(p, HOPTok_LBRACE)) {
        if (HOPPParseBlock(p, &body) != 0) {
            return -1;
        }
    } else if (HOPPForStmtHeadHasInToken(p)) {
        if (HOPPParseForInClause(p, &keyBinding, &valueBinding, &sourceExpr, &body, &forInFlags)
            != 0)
        {
            return -1;
        }
        isForIn = 1;
        p->nodes[n].flags |= forInFlags;
    } else {
        if (HOPPAt(p, HOPTok_SEMICOLON)) {
            p->pos++;
        } else if (HOPPAt(p, HOPTok_VAR)) {
            if (HOPPParseVarLikeStmt(p, HOPAst_VAR, 0, 1, &init) != 0) {
                return -1;
            }
        } else if (HOPPIsShortAssignStart(p)) {
            if (HOPPParseShortAssignStmt(p, 0, &init) != 0) {
                return -1;
            }
        } else {
            if (HOPPParseExpr(p, 1, &init) != 0) {
                return -1;
            }
        }

        if (HOPPMatch(p, HOPTok_SEMICOLON)) {
            if (!HOPPAt(p, HOPTok_SEMICOLON)) {
                if (HOPPParseExpr(p, 1, &cond) != 0) {
                    return -1;
                }
            }
            if (!HOPPMatch(p, HOPTok_SEMICOLON)) {
                return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
            }
            if (!HOPPAt(p, HOPTok_LBRACE)) {
                if (HOPPParseExpr(p, 1, &post) != 0) {
                    return -1;
                }
            }
        } else {
            cond = init;
            init = -1;
        }

        if (HOPPParseBlock(p, &body) != 0) {
            return -1;
        }
    }

    if (isForIn) {
        if (keyBinding >= 0 && HOPPAddChild(p, n, keyBinding) != 0) {
            return -1;
        }
        if (valueBinding >= 0 && HOPPAddChild(p, n, valueBinding) != 0) {
            return -1;
        }
        if (sourceExpr >= 0 && HOPPAddChild(p, n, sourceExpr) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, n, body) != 0) {
            return -1;
        }
    } else {
        if (init >= 0 && HOPPAddChild(p, n, init) != 0) {
            return -1;
        }
        if (cond >= 0 && HOPPAddChild(p, n, cond) != 0) {
            return -1;
        }
        if (post >= 0 && HOPPAddChild(p, n, post) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, n, body) != 0) {
            return -1;
        }
    }
    p->nodes[n].end = p->nodes[body].end;
    *out = n;
    return 0;
}

static int HOPPParseSwitchStmt(HOPParser* p, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    const HOPToken* rb;
    int32_t         sw;
    int             sawDefault = 0;

    p->pos++;
    sw = HOPPNewNode(p, HOPAst_SWITCH, kw->start, kw->end);
    if (sw < 0) {
        return -1;
    }

    // Expression switch: switch <expr> { ... }
    // Condition switch:  switch { ... }
    if (!HOPPAt(p, HOPTok_LBRACE)) {
        int32_t subject;
        if (HOPPParseExpr(p, 1, &subject) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, sw, subject) != 0) {
            return -1;
        }
        p->nodes[sw].flags = 1;
    }

    if (HOPPExpect(p, HOPTok_LBRACE, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }

    while (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
        if (HOPPMatch(p, HOPTok_SEMICOLON)) {
            continue;
        }

        if (HOPPMatch(p, HOPTok_CASE)) {
            int32_t caseNode = HOPPNewNode(p, HOPAst_CASE, HOPPPrev(p)->start, HOPPPrev(p)->end);
            int32_t body;
            if (caseNode < 0) {
                return -1;
            }

            for (;;) {
                uint32_t savedPos = p->pos;
                uint32_t savedNodeLen = p->nodeLen;
                HOPDiag  savedDiag = { 0 };
                int32_t  patternExpr = -1;
                int32_t  patternNode = -1;
                int32_t  aliasNode = -1;
                if (p->diag != NULL) {
                    savedDiag = *p->diag;
                }

                /*
                 * Variant pattern fast-path:
                 *   case Type.Variant [as alias]
                 * This avoids interpreting `as alias` as a cast in case labels.
                 */
                if (HOPPAt(p, HOPTok_IDENT)) {
                    const HOPToken* firstSeg = NULL;
                    const HOPToken* seg = NULL;
                    int             sawDot = 0;
                    if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_EXPECTED_EXPR, &firstSeg) == 0) {
                        patternExpr = HOPPNewNode(p, HOPAst_IDENT, firstSeg->start, firstSeg->end);
                        if (patternExpr < 0) {
                            return -1;
                        }
                        p->nodes[patternExpr].dataStart = firstSeg->start;
                        p->nodes[patternExpr].dataEnd = firstSeg->end;
                        while (HOPPMatch(p, HOPTok_DOT)) {
                            int32_t fieldExpr;
                            if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_EXPECTED_EXPR, &seg) != 0) {
                                goto parse_case_label_fallback;
                            }
                            sawDot = 1;
                            fieldExpr = HOPPNewNode(
                                p, HOPAst_FIELD_EXPR, p->nodes[patternExpr].start, seg->end);
                            if (fieldExpr < 0) {
                                return -1;
                            }
                            if (HOPPAddChild(p, fieldExpr, patternExpr) != 0) {
                                return -1;
                            }
                            p->nodes[fieldExpr].dataStart = seg->start;
                            p->nodes[fieldExpr].dataEnd = seg->end;
                            patternExpr = fieldExpr;
                        }
                        if (sawDot) {
                            if (HOPPMatch(p, HOPTok_AS)) {
                                const HOPToken* aliasTok;
                                if (HOPPExpectDeclName(p, &aliasTok, 0) != 0) {
                                    goto parse_case_label_fallback;
                                }
                                aliasNode = HOPPNewNode(
                                    p, HOPAst_IDENT, aliasTok->start, aliasTok->end);
                                if (aliasNode < 0) {
                                    return -1;
                                }
                                p->nodes[aliasNode].dataStart = aliasTok->start;
                                p->nodes[aliasNode].dataEnd = aliasTok->end;
                            }
                            if (HOPPAt(p, HOPTok_COMMA) || HOPPAt(p, HOPTok_LBRACE)) {
                                patternNode = HOPPNewNode(
                                    p,
                                    HOPAst_CASE_PATTERN,
                                    p->nodes[patternExpr].start,
                                    aliasNode >= 0 ? p->nodes[aliasNode].end
                                                   : p->nodes[patternExpr].end);
                                if (patternNode < 0) {
                                    return -1;
                                }
                                if (HOPPAddChild(p, patternNode, patternExpr) != 0) {
                                    return -1;
                                }
                                if (aliasNode >= 0 && HOPPAddChild(p, patternNode, aliasNode) != 0)
                                {
                                    return -1;
                                }
                            }
                        }
                    }
                }

            parse_case_label_fallback:
                if (patternNode < 0) {
                    p->pos = savedPos;
                    p->nodeLen = savedNodeLen;
                    if (p->diag != NULL) {
                        *p->diag = savedDiag;
                    }
                    if (HOPPParseExpr(p, 1, &patternExpr) != 0) {
                        return -1;
                    }
                    patternNode = HOPPNewNode(
                        p,
                        HOPAst_CASE_PATTERN,
                        p->nodes[patternExpr].start,
                        p->nodes[patternExpr].end);
                    if (patternNode < 0) {
                        return -1;
                    }
                    if (HOPPAddChild(p, patternNode, patternExpr) != 0) {
                        return -1;
                    }
                }

                if (HOPPAddChild(p, caseNode, patternNode) != 0) {
                    return -1;
                }
                if (!HOPPMatch(p, HOPTok_COMMA)) {
                    break;
                }
            }

            if (HOPPParseBlock(p, &body) != 0) {
                return -1;
            }
            if (HOPPAddChild(p, caseNode, body) != 0) {
                return -1;
            }
            p->nodes[caseNode].end = p->nodes[body].end;
            if (HOPPAddChild(p, sw, caseNode) != 0) {
                return -1;
            }
            continue;
        }

        if (HOPPMatch(p, HOPTok_DEFAULT)) {
            int32_t defNode;
            int32_t body;
            if (sawDefault) {
                return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
            }
            sawDefault = 1;
            defNode = HOPPNewNode(p, HOPAst_DEFAULT, HOPPPrev(p)->start, HOPPPrev(p)->end);
            if (defNode < 0) {
                return -1;
            }
            if (HOPPParseBlock(p, &body) != 0) {
                return -1;
            }
            if (HOPPAddChild(p, defNode, body) != 0) {
                return -1;
            }
            p->nodes[defNode].end = p->nodes[body].end;
            if (HOPPAddChild(p, sw, defNode) != 0) {
                return -1;
            }
            continue;
        }

        return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
    }

    if (HOPPExpect(p, HOPTok_RBRACE, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[sw].end = rb->end;
    *out = sw;
    return 0;
}

static int HOPPParseStmt(HOPParser* p, int32_t* out) {
    const HOPToken* kw;
    int32_t         n;
    int32_t         expr;
    int32_t         block;

    switch (HOPPPeek(p)->kind) {
        case HOPTok_VAR: return HOPPParseVarLikeStmt(p, HOPAst_VAR, 1, 1, out);
        case HOPTok_CONST:
            kw = HOPPPeek(p);
            p->pos++;
            if (HOPPAt(p, HOPTok_LBRACE)) {
                n = HOPPNewNode(p, HOPAst_CONST_BLOCK, kw->start, kw->end);
                if (n < 0) {
                    return -1;
                }
                if (HOPPParseBlock(p, &block) != 0) {
                    return -1;
                }
                if (HOPPAddChild(p, n, block) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[block].end;
                *out = n;
                return 0;
            }
            p->pos--;
            return HOPPParseVarLikeStmt(p, HOPAst_CONST, 1, 1, out);
        case HOPTok_IF:     return HOPPParseIfStmt(p, out);
        case HOPTok_FOR:    return HOPPParseForStmt(p, out);
        case HOPTok_SWITCH: return HOPPParseSwitchStmt(p, out);
        case HOPTok_RETURN:
            kw = HOPPPeek(p);
            p->pos++;
            n = HOPPNewNode(p, HOPAst_RETURN, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (!HOPPAt(p, HOPTok_SEMICOLON)) {
                if (HOPPParseExpr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (HOPPMatch(p, HOPTok_COMMA)) {
                    int32_t  items[256];
                    uint32_t itemCount = 0;
                    int32_t  exprList;
                    items[itemCount++] = expr;
                    for (;;) {
                        if (itemCount >= (uint32_t)(sizeof(items) / sizeof(items[0]))) {
                            return HOPPFail(p, HOPDiag_ARENA_OOM);
                        }
                        if (HOPPParseExpr(p, 1, &items[itemCount]) != 0) {
                            return -1;
                        }
                        itemCount++;
                        if (!HOPPMatch(p, HOPTok_COMMA)) {
                            break;
                        }
                    }
                    if (HOPPBuildListNode(p, HOPAst_EXPR_LIST, items, itemCount, &exprList) != 0) {
                        return -1;
                    }
                    expr = exprList;
                }
                if (HOPPAddChild(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[expr].end;
            }
            {
                int hasSemi = HOPPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case HOPTok_BREAK:
            kw = HOPPPeek(p);
            p->pos++;
            n = HOPPNewNode(p, HOPAst_BREAK, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            {
                int hasSemi = HOPPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case HOPTok_CONTINUE:
            kw = HOPPPeek(p);
            p->pos++;
            n = HOPPNewNode(p, HOPAst_CONTINUE, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            {
                int hasSemi = HOPPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case HOPTok_DEFER:
            kw = HOPPPeek(p);
            p->pos++;
            n = HOPPNewNode(p, HOPAst_DEFER, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (HOPPAt(p, HOPTok_LBRACE)) {
                if (HOPPParseBlock(p, &block) != 0) {
                    return -1;
                }
            } else {
                if (HOPPParseStmt(p, &block) != 0) {
                    return -1;
                }
            }
            if (HOPPAddChild(p, n, block) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[block].end;
            *out = n;
            return 0;
        case HOPTok_ASSERT:
            kw = HOPPPeek(p);
            p->pos++;
            n = HOPPNewNode(p, HOPAst_ASSERT, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (HOPPParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, expr) != 0) {
                return -1;
            }
            while (HOPPMatch(p, HOPTok_COMMA)) {
                int32_t arg;
                if (HOPPParseExpr(p, 1, &arg) != 0) {
                    return -1;
                }
                if (HOPPAddChild(p, n, arg) != 0) {
                    return -1;
                }
            }
            {
                int hasSemi = HOPPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case HOPTok_DEL:
            kw = HOPPPeek(p);
            p->pos++;
            n = HOPPNewNode(p, HOPAst_DEL, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            for (;;) {
                if (HOPPParseExpr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (HOPPAddChild(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[expr].end;
                if (!HOPPMatch(p, HOPTok_COMMA)) {
                    break;
                }
            }
            if (HOPPMatch(p, HOPTok_IN)) {
                if (HOPPParseExpr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (HOPPAddChild(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].flags |= HOPAstFlag_DEL_HAS_ALLOC;
                p->nodes[n].end = p->nodes[expr].end;
            }
            {
                int hasSemi = HOPPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case HOPTok_LBRACE: return HOPPParseBlock(p, out);
        default:            {
            int hasSemi;
            if (HOPPIsShortAssignStart(p)) {
                return HOPPParseShortAssignStmt(p, 1, out);
            }
            if (HOPPParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (HOPPMatch(p, HOPTok_COMMA)) {
                int32_t  lhsExprs[256];
                int32_t  rhsExprs[256];
                uint32_t lhsCount = 0;
                uint32_t rhsCount = 0;
                int32_t  lhsList;
                int32_t  rhsList;
                lhsExprs[lhsCount++] = expr;
                if (HOPPParseExpr(p, 2, &lhsExprs[lhsCount]) != 0) {
                    return -1;
                }
                lhsCount++;
                while (HOPPMatch(p, HOPTok_COMMA)) {
                    if (lhsCount >= (uint32_t)(sizeof(lhsExprs) / sizeof(lhsExprs[0]))) {
                        return HOPPFail(p, HOPDiag_ARENA_OOM);
                    }
                    if (HOPPParseExpr(p, 2, &lhsExprs[lhsCount]) != 0) {
                        return -1;
                    }
                    lhsCount++;
                }
                if (!HOPPMatch(p, HOPTok_ASSIGN)) {
                    return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
                }
                if (HOPPParseExpr(p, 1, &rhsExprs[rhsCount]) != 0) {
                    return -1;
                }
                rhsCount++;
                while (HOPPMatch(p, HOPTok_COMMA)) {
                    if (rhsCount >= (uint32_t)(sizeof(rhsExprs) / sizeof(rhsExprs[0]))) {
                        return HOPPFail(p, HOPDiag_ARENA_OOM);
                    }
                    if (HOPPParseExpr(p, 1, &rhsExprs[rhsCount]) != 0) {
                        return -1;
                    }
                    rhsCount++;
                }
                if (HOPPBuildListNode(p, HOPAst_EXPR_LIST, lhsExprs, lhsCount, &lhsList) != 0
                    || HOPPBuildListNode(p, HOPAst_EXPR_LIST, rhsExprs, rhsCount, &rhsList) != 0)
                {
                    return -1;
                }
                n = HOPPNewNode(
                    p, HOPAst_MULTI_ASSIGN, p->nodes[lhsList].start, p->nodes[rhsList].end);
                if (n < 0) {
                    return -1;
                }
                if (HOPPAddChild(p, n, lhsList) != 0 || HOPPAddChild(p, n, rhsList) != 0) {
                    return -1;
                }
                hasSemi = HOPPConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
                *out = n;
                return 0;
            }
            hasSemi = HOPPConsumeStmtTerminator(p, &kw);
            if (hasSemi < 0) {
                return -1;
            }
            n = HOPPNewNode(
                p, HOPAst_EXPR_STMT, p->nodes[expr].start, hasSemi ? kw->end : p->nodes[expr].end);
            if (n < 0) {
                return -1;
            }
            if (HOPPAddChild(p, n, expr) != 0) {
                return -1;
            }
            *out = n;
            return 0;
        }
    }
}

static int HOPPParseFieldList(HOPParser* p, int32_t agg) {
    while (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
        const HOPToken* names[256];
        uint32_t        nameCount = 0;
        const HOPToken* embeddedTypeName = NULL;
        int32_t         type = -1;
        int32_t         defaultExpr = -1;
        uint32_t        i;
        int             isEmbedded = 0;
        if (HOPPAt(p, HOPTok_SEMICOLON) || HOPPAt(p, HOPTok_COMMA)) {
            p->pos++;
            continue;
        }
        if (HOPPAnonymousFieldLookahead(p, &embeddedTypeName)
            && !(
                p->pos + 2u < p->tokLen && p->tok[p->pos].kind == HOPTok_IDENT
                && p->tok[p->pos + 1u].kind == HOPTok_COMMA
                && p->tok[p->pos + 2u].kind == HOPTok_IDENT))
        {
            if (HOPPParseTypeName(p, &type) != 0) {
                return -1;
            }
            isEmbedded = 1;
            nameCount = 1;
        } else {
            if (HOPPExpectDeclName(p, &names[nameCount], 0) != 0) {
                return -1;
            }
            nameCount++;
            while (HOPPMatch(p, HOPTok_COMMA)) {
                if (!HOPPAt(p, HOPTok_IDENT)) {
                    return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
                }
                if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                    return HOPPFail(p, HOPDiag_ARENA_OOM);
                }
                if (HOPPExpectDeclName(p, &names[nameCount], 0) != 0) {
                    return -1;
                }
                nameCount++;
            }
            if (HOPPParseType(p, &type) != 0) {
                return -1;
            }
        }
        if (HOPPMatch(p, HOPTok_ASSIGN)) {
            if (!isEmbedded && nameCount > 1) {
                const HOPToken* eq = HOPPPrev(p);
                HOPPSetDiag(p->diag, HOPDiag_UNEXPECTED_TOKEN, eq->start, eq->end);
                return -1;
            }
            if (HOPPParseExpr(p, 1, &defaultExpr) != 0) {
                return -1;
            }
        }
        for (i = 0; i < nameCount; i++) {
            int32_t field;
            int32_t fieldType;
            if (isEmbedded) {
                fieldType = type;
                if (i != 0) {
                    return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
                }
            } else if (i == 0) {
                fieldType = type;
            } else if (HOPPCloneSubtree(p, type, &fieldType) != 0) {
                return -1;
            }
            field = HOPPNewNode(
                p,
                HOPAst_FIELD,
                isEmbedded ? p->nodes[fieldType].start : names[i]->start,
                p->nodes[fieldType].end);
            if (field < 0) {
                return -1;
            }
            if (isEmbedded) {
                p->nodes[field].dataStart = embeddedTypeName->start;
                p->nodes[field].dataEnd = embeddedTypeName->end;
                p->nodes[field].flags |= HOPAstFlag_FIELD_EMBEDDED;
            } else {
                p->nodes[field].dataStart = names[i]->start;
                p->nodes[field].dataEnd = names[i]->end;
            }
            if (HOPPAddChild(p, field, fieldType) != 0) {
                return -1;
            }
            if (i == 0 && defaultExpr >= 0) {
                p->nodes[field].end = p->nodes[defaultExpr].end;
                if (HOPPAddChild(p, field, defaultExpr) != 0) {
                    return -1;
                }
            }
            if (HOPPAddChild(p, agg, field) != 0) {
                return -1;
            }
        }
        if (HOPPMatch(p, HOPTok_SEMICOLON) || HOPPMatch(p, HOPTok_COMMA)) {
            continue;
        }
        if (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
    }
    return 0;
}

static int HOPPParseAggregateMemberList(HOPParser* p, int32_t agg) {
    while (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
        if (HOPPAt(p, HOPTok_SEMICOLON) || HOPPAt(p, HOPTok_COMMA)) {
            p->pos++;
            continue;
        }

        if (HOPPAt(p, HOPTok_STRUCT) || HOPPAt(p, HOPTok_UNION) || HOPPAt(p, HOPTok_ENUM)
            || HOPPAt(p, HOPTok_TYPE))
        {
            int32_t declNode = -1;
            if (HOPPAt(p, HOPTok_TYPE)) {
                if (HOPPParseTypeAliasDecl(p, &declNode) != 0) {
                    return -1;
                }
            } else {
                if (HOPPParseAggregateDecl(p, &declNode) != 0) {
                    return -1;
                }
            }
            if (HOPPAddChild(p, agg, declNode) != 0) {
                return -1;
            }
            if (HOPPMatch(p, HOPTok_SEMICOLON) || HOPPMatch(p, HOPTok_COMMA)) {
                continue;
            }
            if (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
                return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
            }
            continue;
        }

        if (HOPPAt(p, HOPTok_FN) || HOPPAt(p, HOPTok_CONST) || HOPPAt(p, HOPTok_VAR)
            || HOPPAt(p, HOPTok_PUB))
        {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }

        {
            const HOPToken* names[256];
            uint32_t        nameCount = 0;
            const HOPToken* embeddedTypeName = NULL;
            int32_t         type = -1;
            int32_t         defaultExpr = -1;
            uint32_t        i;
            int             isEmbedded = 0;

            if (HOPPAnonymousFieldLookahead(p, &embeddedTypeName)
                && !(
                    p->pos + 2u < p->tokLen && p->tok[p->pos].kind == HOPTok_IDENT
                    && p->tok[p->pos + 1u].kind == HOPTok_COMMA
                    && p->tok[p->pos + 2u].kind == HOPTok_IDENT))
            {
                if (HOPPParseTypeName(p, &type) != 0) {
                    return -1;
                }
                isEmbedded = 1;
                nameCount = 1;
            } else {
                if (HOPPExpectDeclName(p, &names[nameCount], 0) != 0) {
                    return -1;
                }
                nameCount++;
                while (HOPPMatch(p, HOPTok_COMMA)) {
                    if (!HOPPAt(p, HOPTok_IDENT)) {
                        return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
                    }
                    if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                        return HOPPFail(p, HOPDiag_ARENA_OOM);
                    }
                    if (HOPPExpectDeclName(p, &names[nameCount], 0) != 0) {
                        return -1;
                    }
                    nameCount++;
                }
                if (HOPPParseType(p, &type) != 0) {
                    return -1;
                }
            }

            if (HOPPMatch(p, HOPTok_ASSIGN)) {
                if (!isEmbedded && nameCount > 1) {
                    const HOPToken* eq = HOPPPrev(p);
                    HOPPSetDiag(p->diag, HOPDiag_UNEXPECTED_TOKEN, eq->start, eq->end);
                    return -1;
                }
                if (HOPPParseExpr(p, 1, &defaultExpr) != 0) {
                    return -1;
                }
            }

            for (i = 0; i < nameCount; i++) {
                int32_t field;
                int32_t fieldType;
                if (isEmbedded) {
                    fieldType = type;
                    if (i != 0) {
                        return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
                    }
                } else if (i == 0) {
                    fieldType = type;
                } else if (HOPPCloneSubtree(p, type, &fieldType) != 0) {
                    return -1;
                }
                field = HOPPNewNode(
                    p,
                    HOPAst_FIELD,
                    isEmbedded ? p->nodes[fieldType].start : names[i]->start,
                    p->nodes[fieldType].end);
                if (field < 0) {
                    return -1;
                }
                if (isEmbedded) {
                    p->nodes[field].dataStart = embeddedTypeName->start;
                    p->nodes[field].dataEnd = embeddedTypeName->end;
                    p->nodes[field].flags |= HOPAstFlag_FIELD_EMBEDDED;
                } else {
                    p->nodes[field].dataStart = names[i]->start;
                    p->nodes[field].dataEnd = names[i]->end;
                }
                if (HOPPAddChild(p, field, fieldType) != 0) {
                    return -1;
                }
                if (i == 0 && defaultExpr >= 0) {
                    p->nodes[field].end = p->nodes[defaultExpr].end;
                    if (HOPPAddChild(p, field, defaultExpr) != 0) {
                        return -1;
                    }
                }
                if (HOPPAddChild(p, agg, field) != 0) {
                    return -1;
                }
            }
        }
        if (HOPPMatch(p, HOPTok_SEMICOLON) || HOPPMatch(p, HOPTok_COMMA)) {
            continue;
        }
        if (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
    }
    return 0;
}

static int HOPPParseAggregateDecl(HOPParser* p, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    const HOPToken* name;
    const HOPToken* rb;
    HOPAstKind      kind = HOPAst_STRUCT;
    int32_t         n;

    if (kw->kind == HOPTok_UNION) {
        kind = HOPAst_UNION;
    } else if (kw->kind == HOPTok_ENUM) {
        kind = HOPAst_ENUM;
    }

    p->pos++;
    if (HOPPExpectDeclName(p, &name, 0) != 0) {
        return -1;
    }
    n = HOPPNewNode(p, kind, kw->start, name->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;
    if (HOPPParseTypeParamList(p, n) != 0) {
        return -1;
    }

    if (kw->kind == HOPTok_ENUM) {
        int32_t underType;
        if (HOPPParseType(p, &underType) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, n, underType) != 0) {
            return -1;
        }
    }

    if (HOPPExpect(p, HOPTok_LBRACE, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    if (kw->kind == HOPTok_ENUM) {
        while (!HOPPAt(p, HOPTok_RBRACE) && !HOPPAt(p, HOPTok_EOF)) {
            const HOPToken* itemName;
            int32_t         item;
            if (HOPPAt(p, HOPTok_COMMA) || HOPPAt(p, HOPTok_SEMICOLON)) {
                p->pos++;
                continue;
            }
            if (HOPPExpectDeclName(p, &itemName, 0) != 0) {
                return -1;
            }
            item = HOPPNewNode(p, HOPAst_FIELD, itemName->start, itemName->end);
            if (item < 0) {
                return -1;
            }
            p->nodes[item].dataStart = itemName->start;
            p->nodes[item].dataEnd = itemName->end;
            if (HOPPMatch(p, HOPTok_LBRACE)) {
                if (HOPPParseFieldList(p, item) != 0) {
                    return -1;
                }
                if (HOPPExpect(p, HOPTok_RBRACE, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
                    return -1;
                }
                p->nodes[item].end = rb->end;
            }
            if (HOPPMatch(p, HOPTok_ASSIGN)) {
                int32_t vexpr;
                if (HOPPParseExpr(p, 1, &vexpr) != 0) {
                    return -1;
                }
                if (HOPPAddChild(p, item, vexpr) != 0) {
                    return -1;
                }
                p->nodes[item].end = p->nodes[vexpr].end;
            }
            if (HOPPAddChild(p, n, item) != 0) {
                return -1;
            }
            if (HOPPMatch(p, HOPTok_COMMA) || HOPPMatch(p, HOPTok_SEMICOLON)) {
                continue;
            }
        }
    } else {
        if (HOPPParseAggregateMemberList(p, n) != 0) {
            return -1;
        }
    }

    if (HOPPExpect(p, HOPTok_RBRACE, HOPDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[n].end = rb->end;
    *out = n;
    return 0;
}

static int HOPPParseFunDecl(HOPParser* p, int allowBody, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    const HOPToken* name;
    const HOPToken* t;
    int32_t         fn;

    p->pos++;
    if (HOPPExpectFnName(p, &name) != 0) {
        return -1;
    }
    fn = HOPPNewNode(p, HOPAst_FN, kw->start, name->end);
    if (fn < 0) {
        return -1;
    }
    p->nodes[fn].dataStart = name->start;
    p->nodes[fn].dataEnd = name->end;
    if (HOPPParseTypeParamList(p, fn) != 0) {
        return -1;
    }
    if (HOPPExpect(p, HOPTok_LPAREN, HOPDiag_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }

    if (!HOPPAt(p, HOPTok_RPAREN)) {
        for (;;) {
            int isVariadic = 0;
            if (HOPPParseParamGroup(p, fn, &isVariadic) != 0) {
                return -1;
            }
            if (!HOPPMatch(p, HOPTok_COMMA)) {
                break;
            }
        }
    }

    if (HOPPExpect(p, HOPTok_RPAREN, HOPDiag_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }
    p->nodes[fn].end = t->end;

    if (!HOPPAt(p, HOPTok_LBRACE) && !HOPPAt(p, HOPTok_SEMICOLON)) {
        if (HOPPAt(p, HOPTok_LPAREN)) {
            if (HOPPParseFnResultClause(p, fn) != 0) {
                return -1;
            }
        } else {
            int32_t retType;
            if (HOPPParseType(p, &retType) != 0) {
                return -1;
            }
            p->nodes[retType].flags = 1;
            if (HOPPAddChild(p, fn, retType) != 0) {
                return -1;
            }
            p->nodes[fn].end = p->nodes[retType].end;
        }
    }

    if (HOPPAt(p, HOPTok_LBRACE)) {
        int32_t body;
        if (!allowBody) {
            return HOPPFail(p, HOPDiag_UNEXPECTED_TOKEN);
        }
        if (HOPPParseBlock(p, &body) != 0) {
            return -1;
        }
        if (HOPPAddChild(p, fn, body) != 0) {
            return -1;
        }
        p->nodes[fn].end = p->nodes[body].end;
    } else {
        if (HOPPExpect(p, HOPTok_SEMICOLON, HOPDiag_UNEXPECTED_TOKEN, &t) != 0) {
            return -1;
        }
        p->nodes[fn].end = t->end;
    }

    *out = fn;
    return 0;
}

static int HOPPParseTypeAliasDecl(HOPParser* p, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    const HOPToken* name;
    const HOPToken* semi;
    int32_t         n;
    int32_t         targetType;

    p->pos++;
    if (HOPPExpectDeclName(p, &name, 0) != 0) {
        return -1;
    }
    n = HOPPNewNode(p, HOPAst_TYPE_ALIAS, kw->start, name->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;
    if (HOPPParseTypeParamList(p, n) != 0) {
        return -1;
    }
    if (HOPPParseType(p, &targetType) != 0) {
        return -1;
    }
    p->nodes[n].end = p->nodes[targetType].end;
    if (HOPPAddChild(p, n, targetType) != 0) {
        return -1;
    }

    if (HOPPExpect(p, HOPTok_SEMICOLON, HOPDiag_UNEXPECTED_TOKEN, &semi) != 0) {
        return -1;
    }
    p->nodes[n].end = semi->end;
    *out = n;
    return 0;
}

static int HOPPParseImport(HOPParser* p, int32_t* out) {
    const HOPToken* kw = HOPPPeek(p);
    const HOPToken* path;
    const HOPToken* alias = NULL;
    int32_t         n;
    p->pos++;

    if (HOPPExpect(p, HOPTok_STRING, HOPDiag_UNEXPECTED_TOKEN, &path) != 0) {
        return -1;
    }

    n = HOPPNewNode(p, HOPAst_IMPORT, kw->start, path->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = path->start;
    p->nodes[n].dataEnd = path->end;

    if (HOPPMatch(p, HOPTok_AS)) {
        int32_t aliasNode;
        if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_UNEXPECTED_TOKEN, &alias) != 0) {
            return -1;
        }
        if (HOPPReservedName(p, alias)) {
            HOPPSetDiag(p->diag, HOPDiag_RESERVED_HOP_PREFIX, alias->start, alias->end);
            return -1;
        }
        aliasNode = HOPPNewNode(p, HOPAst_IDENT, alias->start, alias->end);
        if (aliasNode < 0) {
            return -1;
        }
        p->nodes[aliasNode].dataStart = alias->start;
        p->nodes[aliasNode].dataEnd = alias->end;
        if (HOPPAddChild(p, n, aliasNode) != 0) {
            return -1;
        }
    }

    if (HOPPMatch(p, HOPTok_LBRACE)) {
        if (!HOPPAt(p, HOPTok_RBRACE)) {
            for (;;) {
                const HOPToken* symName = NULL;
                const HOPToken* symAlias = NULL;
                int32_t         symNode;

                if (HOPPAt(p, HOPTok_MUL)) {
                    const HOPToken* starTok = HOPPPeek(p);
                    HOPPSetDiag(
                        p->diag,
                        HOPDiag_IMPORT_WILDCARD_NOT_SUPPORTED,
                        starTok->start,
                        starTok->end);
                    return -1;
                }
                if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_UNEXPECTED_TOKEN, &symName) != 0) {
                    return -1;
                }
                symNode = HOPPNewNode(p, HOPAst_IMPORT_SYMBOL, symName->start, symName->end);
                if (symNode < 0) {
                    return -1;
                }
                p->nodes[symNode].dataStart = symName->start;
                p->nodes[symNode].dataEnd = symName->end;

                if (HOPPMatch(p, HOPTok_AS)) {
                    int32_t symAliasNode;
                    if (HOPPExpect(p, HOPTok_IDENT, HOPDiag_UNEXPECTED_TOKEN, &symAlias) != 0) {
                        return -1;
                    }
                    if (HOPPReservedName(p, symAlias)) {
                        HOPPSetDiag(
                            p->diag, HOPDiag_RESERVED_HOP_PREFIX, symAlias->start, symAlias->end);
                        return -1;
                    }
                    symAliasNode = HOPPNewNode(p, HOPAst_IDENT, symAlias->start, symAlias->end);
                    if (symAliasNode < 0) {
                        return -1;
                    }
                    p->nodes[symAliasNode].dataStart = symAlias->start;
                    p->nodes[symAliasNode].dataEnd = symAlias->end;
                    if (HOPPAddChild(p, symNode, symAliasNode) != 0) {
                        return -1;
                    }
                    p->nodes[symNode].end = symAlias->end;
                }

                if (HOPPAddChild(p, n, symNode) != 0) {
                    return -1;
                }

                if (!HOPPMatch(p, HOPTok_COMMA) && !HOPPMatch(p, HOPTok_SEMICOLON)) {
                    break;
                }
                while (HOPPMatch(p, HOPTok_COMMA) || HOPPMatch(p, HOPTok_SEMICOLON)) {}
                if (HOPPAt(p, HOPTok_RBRACE)) {
                    break;
                }
            }
        }
        if (HOPPExpect(p, HOPTok_RBRACE, HOPDiag_UNEXPECTED_TOKEN, &kw) != 0) {
            return -1;
        }
        p->nodes[n].end = kw->end;
    }

    /* Detect feature imports and set feature flags using decoded import path bytes. */
    {
        HOPStringLitErr litErr = { 0 };
        uint8_t*        decoded = NULL;
        uint32_t        decodedLen = 0;
        const uint8_t*  name = NULL;
        uint32_t        nameLen = 0;
        if (HOPDecodeStringLiteralArena(
                p->arena, p->src.ptr, path->start, path->end, &decoded, &decodedLen, &litErr)
            != 0)
        {
            HOPPSetDiag(p->diag, HOPStringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
            return -1;
        }
        if (decodedLen > 15u && memcmp(decoded, "hophop/feature/", 15u) == 0) {
            name = decoded + 15u;
            nameLen = decodedLen - 15u;
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
                p->features |= HOPFeature_OPTIONAL;
            }
            /* Unknown feature names: silently ignored here; CLI layer warns later. */
        }
    }

    if (HOPPExpect(p, HOPTok_SEMICOLON, HOPDiag_UNEXPECTED_TOKEN, &kw) != 0) {
        return -1;
    }
    p->nodes[n].end = kw->end;
    *out = n;
    return 0;
}

static int HOPPParseDeclInner(HOPParser* p, int allowBody, int32_t* out) {
    switch (HOPPPeek(p)->kind) {
        case HOPTok_FN:   return HOPPParseFunDecl(p, allowBody, out);
        case HOPTok_TYPE: return HOPPParseTypeAliasDecl(p, out);
        case HOPTok_STRUCT:
        case HOPTok_UNION:
        case HOPTok_ENUM:
            if (HOPPParseAggregateDecl(p, out) != 0) {
                return -1;
            }
            if (HOPPMatch(p, HOPTok_SEMICOLON)) {
                p->nodes[*out].end = HOPPPrev(p)->end;
            }
            return 0;
        case HOPTok_VAR:   return HOPPParseVarLikeStmt(p, HOPAst_VAR, 1, 0, out);
        case HOPTok_CONST: return HOPPParseVarLikeStmt(p, HOPAst_CONST, 1, 0, out);
        default:           return HOPPFail(p, HOPDiag_EXPECTED_DECL);
    }
}

static int HOPPParseDecl(HOPParser* p, int allowBody, int32_t* out) {
    int      isPub = 0;
    uint32_t pubStart = 0;
    if (HOPPMatch(p, HOPTok_PUB)) {
        isPub = 1;
        pubStart = HOPPPrev(p)->start;
    }
    if (HOPPParseDeclInner(p, allowBody, out) != 0) {
        return -1;
    }
    if (isPub) {
        p->nodes[*out].start = pubStart;
        p->nodes[*out].flags |= HOPAstFlag_PUB;
    }
    return 0;
}

const char* HOPAstKindName(HOPAstKind kind) {
    switch (kind) {
        case HOPAst_FILE:              return "FILE";
        case HOPAst_IMPORT:            return "IMPORT";
        case HOPAst_IMPORT_SYMBOL:     return "IMPORT_SYMBOL";
        case HOPAst_DIRECTIVE:         return "DIRECTIVE";
        case HOPAst_PUB:               return "PUB";
        case HOPAst_FN:                return "FN";
        case HOPAst_PARAM:             return "PARAM";
        case HOPAst_TYPE_PARAM:        return "TYPE_PARAM";
        case HOPAst_CONTEXT_CLAUSE:    return "CONTEXT_CLAUSE";
        case HOPAst_TYPE_NAME:         return "TYPE_NAME";
        case HOPAst_TYPE_PTR:          return "TYPE_PTR";
        case HOPAst_TYPE_REF:          return "TYPE_REF";
        case HOPAst_TYPE_MUTREF:       return "TYPE_MUTREF";
        case HOPAst_TYPE_ARRAY:        return "TYPE_ARRAY";
        case HOPAst_TYPE_VARRAY:       return "TYPE_VARRAY";
        case HOPAst_TYPE_SLICE:        return "TYPE_SLICE";
        case HOPAst_TYPE_MUTSLICE:     return "TYPE_MUTSLICE";
        case HOPAst_TYPE_OPTIONAL:     return "TYPE_OPTIONAL";
        case HOPAst_TYPE_FN:           return "TYPE_FN";
        case HOPAst_TYPE_ALIAS:        return "TYPE_ALIAS";
        case HOPAst_TYPE_ANON_STRUCT:  return "TYPE_ANON_STRUCT";
        case HOPAst_TYPE_ANON_UNION:   return "TYPE_ANON_UNION";
        case HOPAst_TYPE_TUPLE:        return "TYPE_TUPLE";
        case HOPAst_STRUCT:            return "STRUCT";
        case HOPAst_UNION:             return "UNION";
        case HOPAst_ENUM:              return "ENUM";
        case HOPAst_FIELD:             return "FIELD";
        case HOPAst_BLOCK:             return "BLOCK";
        case HOPAst_VAR:               return "VAR";
        case HOPAst_CONST:             return "CONST";
        case HOPAst_CONST_BLOCK:       return "CONST_BLOCK";
        case HOPAst_IF:                return "IF";
        case HOPAst_FOR:               return "FOR";
        case HOPAst_SWITCH:            return "SWITCH";
        case HOPAst_CASE:              return "CASE";
        case HOPAst_CASE_PATTERN:      return "CASE_PATTERN";
        case HOPAst_DEFAULT:           return "DEFAULT";
        case HOPAst_RETURN:            return "RETURN";
        case HOPAst_BREAK:             return "BREAK";
        case HOPAst_CONTINUE:          return "CONTINUE";
        case HOPAst_DEFER:             return "DEFER";
        case HOPAst_ASSERT:            return "ASSERT";
        case HOPAst_DEL:               return "DEL";
        case HOPAst_EXPR_STMT:         return "EXPR_STMT";
        case HOPAst_MULTI_ASSIGN:      return "MULTI_ASSIGN";
        case HOPAst_SHORT_ASSIGN:      return "SHORT_ASSIGN";
        case HOPAst_NAME_LIST:         return "NAME_LIST";
        case HOPAst_EXPR_LIST:         return "EXPR_LIST";
        case HOPAst_TUPLE_EXPR:        return "TUPLE_EXPR";
        case HOPAst_TYPE_VALUE:        return "TYPE_VALUE";
        case HOPAst_IDENT:             return "IDENT";
        case HOPAst_INT:               return "INT";
        case HOPAst_FLOAT:             return "FLOAT";
        case HOPAst_STRING:            return "STRING";
        case HOPAst_RUNE:              return "RUNE";
        case HOPAst_BOOL:              return "BOOL";
        case HOPAst_UNARY:             return "UNARY";
        case HOPAst_BINARY:            return "BINARY";
        case HOPAst_CALL:              return "CALL";
        case HOPAst_CALL_ARG:          return "CALL_ARG";
        case HOPAst_CALL_WITH_CONTEXT: return "CALL_WITH_CONTEXT";
        case HOPAst_CONTEXT_OVERLAY:   return "CONTEXT_OVERLAY";
        case HOPAst_CONTEXT_BIND:      return "CONTEXT_BIND";
        case HOPAst_COMPOUND_LIT:      return "COMPOUND_LIT";
        case HOPAst_COMPOUND_FIELD:    return "COMPOUND_FIELD";
        case HOPAst_INDEX:             return "INDEX";
        case HOPAst_FIELD_EXPR:        return "FIELD_EXPR";
        case HOPAst_CAST:              return "CAST";
        case HOPAst_SIZEOF:            return "SIZEOF";
        case HOPAst_NEW:               return "NEW";
        case HOPAst_NULL:              return "NULL";
        case HOPAst_UNWRAP:            return "UNWRAP";
    }
    return "UNKNOWN";
}

static int HOPPIsSpaceButNotNewline(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static int HOPPHasCodeOnLineBefore(const char* src, uint32_t lineStart, uint32_t pos) {
    uint32_t i;
    for (i = lineStart; i < pos; i++) {
        char c = src[i];
        if (!HOPPIsSpaceButNotNewline(c)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t HOPPFindLineStart(const char* src, uint32_t pos) {
    while (pos > 0) {
        if (src[pos - 1] == '\n') {
            break;
        }
        pos--;
    }
    return pos;
}

static int32_t HOPPFindPrevNodeByEnd(const HOPAst* ast, uint32_t pos) {
    int32_t  best = -1;
    uint32_t bestEnd = 0;
    uint32_t bestSpan = 0;
    uint32_t i;
    for (i = 0; i < ast->len; i++) {
        const HOPAstNode* n;
        uint32_t          span;
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

static int32_t HOPPFindNextNodeByStart(const HOPAst* ast, uint32_t pos) {
    int32_t  best = -1;
    uint32_t bestStart = 0;
    uint32_t bestSpan = 0;
    uint32_t i;
    for (i = 0; i < ast->len; i++) {
        const HOPAstNode* n;
        uint32_t          span;
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

static int32_t HOPPFindContainerNode(const HOPAst* ast, uint32_t pos) {
    int32_t nodeId = ast->root;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    for (;;) {
        int32_t  bestChild = -1;
        int32_t  child = ast->nodes[nodeId].firstChild;
        uint32_t bestSpan = 0;
        while (child >= 0) {
            const HOPAstNode* n = &ast->nodes[child];
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

static int HOPPNextCommentRange(
    HOPStrView src, uint32_t* ioPos, uint32_t* outStart, uint32_t* outEnd) {
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
        if (c == (unsigned char)'/' && pos + 1u < src.len
            && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'*')
        {
            uint32_t start = pos;
            uint32_t depth = 1u;
            pos += 2u;
            while (pos < src.len) {
                c = (unsigned char)src.ptr[pos];
                if (c == (unsigned char)'/' && pos + 1u < src.len
                    && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'*')
                {
                    depth++;
                    pos += 2u;
                    continue;
                }
                if (c == (unsigned char)'*' && pos + 1u < src.len
                    && (unsigned char)src.ptr[pos + 1u] == (unsigned char)'/')
                {
                    depth--;
                    pos += 2u;
                    if (depth == 0u) {
                        break;
                    }
                    continue;
                }
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

static int HOPPCollectFormattingData(
    HOPArena*       arena,
    HOPStrView      src,
    const HOPAst*   ast,
    HOPParseExtras* outExtras,
    HOPDiag* _Nullable diag) {
    HOPComment* comments;
    uint32_t    count = 0;
    uint32_t    pos = 0;
    for (;;) {
        uint32_t start;
        uint32_t end;
        if (!HOPPNextCommentRange(src, &pos, &start, &end)) {
            break;
        }
        count++;
    }
    if (count == 0) {
        outExtras->comments = NULL;
        outExtras->commentLen = 0;
        return 0;
    }

    comments = (HOPComment*)HOPArenaAlloc(
        arena, count * (uint32_t)sizeof(HOPComment), (uint32_t)_Alignof(HOPComment));
    if (comments == NULL) {
        HOPPSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
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
        if (!HOPPNextCommentRange(src, &pos, &start, &end)) {
            break;
        }
        lineStart = HOPPFindLineStart(src.ptr, start);
        prevNode = HOPPFindPrevNodeByEnd(ast, start);
        nextNode = HOPPFindNextNodeByStart(ast, end);
        containerNode = HOPPFindContainerNode(ast, start);

        comments[count].start = start;
        comments[count].end = end;
        comments[count].textStart = start + 2u <= end ? start + 2u : end;
        comments[count].textEnd = end;
        if (start + 1u < end && (unsigned char)src.ptr[start + 1u] == (unsigned char)'*'
            && end >= 2u && (unsigned char)src.ptr[end - 2u] == (unsigned char)'*'
            && (unsigned char)src.ptr[end - 1u] == (unsigned char)'/')
        {
            comments[count].textEnd = end >= 2u ? end - 2u : end;
            if (comments[count].textEnd < comments[count].textStart) {
                comments[count].textEnd = comments[count].textStart;
            }
        }
        comments[count].containerNode = containerNode;
        comments[count].anchorNode = -1;
        comments[count].attachment = HOPCommentAttachment_FLOATING;
        comments[count]._reserved[0] = 0;
        comments[count]._reserved[1] = 0;
        comments[count]._reserved[2] = 0;

        if (HOPPHasCodeOnLineBefore(src.ptr, lineStart, start)) {
            comments[count].attachment = HOPCommentAttachment_TRAILING;
            comments[count].anchorNode = prevNode >= 0 ? prevNode : containerNode;
        } else if (nextNode >= 0) {
            comments[count].attachment = HOPCommentAttachment_LEADING;
            comments[count].anchorNode = nextNode;
        } else if (prevNode >= 0) {
            comments[count].attachment = HOPCommentAttachment_TRAILING;
            comments[count].anchorNode = prevNode;
        } else {
            comments[count].attachment = HOPCommentAttachment_FLOATING;
            comments[count].anchorNode = containerNode;
        }
        count++;
    }

    outExtras->comments = comments;
    outExtras->commentLen = count;
    return 0;
}

int HOPParse(
    HOPArena*  arena,
    HOPStrView src,
    const HOPParseOptions* _Nullable options,
    HOPAst* out,
    HOPParseExtras* _Nullable outExtras,
    HOPDiag* _Nullable diag) {
    HOPTokenStream ts;
    HOPParser      p;
    int32_t        root;
    uint32_t       parseFlags = options != NULL ? options->flags : 0;

    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (outExtras != NULL) {
        outExtras->comments = NULL;
        outExtras->commentLen = 0;
    }
    out->nodes = NULL;
    out->len = 0;
    out->root = -1;
    out->features = HOPFeature_NONE;

    if (HOPLex(arena, src, &ts, diag) != 0) {
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
    p.features = HOPFeature_NONE;
    p.nodes = (HOPAstNode*)HOPArenaAlloc(
        arena, p.nodeCap * (uint32_t)sizeof(HOPAstNode), (uint32_t)_Alignof(HOPAstNode));
    if (p.nodes == NULL) {
        HOPPSetDiag(diag, HOPDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    root = HOPPNewNode(&p, HOPAst_FILE, 0, src.len);
    if (root < 0) {
        return -1;
    }

    while (HOPPAt(&p, HOPTok_IMPORT)) {
        int32_t imp;
        if (HOPPParseImport(&p, &imp) != 0) {
            return -1;
        }
        if (HOPPAddChild(&p, root, imp) != 0) {
            return -1;
        }
    }

    while (!HOPPAt(&p, HOPTok_EOF)) {
        int32_t decl;
        if (HOPPAt(&p, HOPTok_SEMICOLON)) {
            p.pos++;
            continue;
        }
        for (;;) {
            if (HOPPAt(&p, HOPTok_SEMICOLON)) {
                p.pos++;
                continue;
            }
            if (HOPPAt(&p, HOPTok_AT)) {
                int32_t directive;
                if (HOPPParseDirective(&p, &directive) != 0) {
                    return -1;
                }
                if (HOPPAddChild(&p, root, directive) != 0) {
                    return -1;
                }
                continue;
            }
            break;
        }
        if (HOPPParseDecl(&p, 1, &decl) != 0) {
            return -1;
        }
        if (HOPPAddChild(&p, root, decl) != 0) {
            return -1;
        }
    }

    out->nodes = p.nodes;
    out->len = p.nodeLen;
    out->root = root;
    out->features = p.features;
    if ((parseFlags & HOPParseFlag_COLLECT_FORMATTING) != 0 && outExtras != NULL
        && HOPPCollectFormattingData(arena, src, out, outExtras, diag) != 0)
    {
        return -1;
    }
    return 0;
}

HOP_API_END
