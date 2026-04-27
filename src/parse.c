#include "libhop-impl.h"

H2_API_BEGIN

static void H2PSetDiag(H2Diag* _Nullable diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = H2DiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->argText = NULL;
    diag->argTextLen = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
    diag->phase = H2DiagPhase_PARSE;
    diag->groupId = 0;
    diag->isPrimary = 1;
    diag->_reserved[0] = 0;
    diag->_reserved[1] = 0;
    diag->_reserved[2] = 0;
    diag->notes = NULL;
    diag->notesLen = 0;
    diag->fixIts = NULL;
    diag->fixItsLen = 0;
    diag->expectations = NULL;
    diag->expectationsLen = 0;
}

static void H2PSetDiagWithArg(
    H2Diag* _Nullable diag,
    H2DiagCode code,
    uint32_t   start,
    uint32_t   end,
    uint32_t   argStart,
    uint32_t   argEnd) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = H2DiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = argStart;
    diag->argEnd = argEnd;
    diag->argText = NULL;
    diag->argTextLen = 0;
    diag->relatedStart = 0;
    diag->relatedEnd = 0;
    diag->detail = NULL;
    diag->hintOverride = NULL;
    diag->phase = H2DiagPhase_PARSE;
    diag->groupId = 0;
    diag->isPrimary = 1;
    diag->_reserved[0] = 0;
    diag->_reserved[1] = 0;
    diag->_reserved[2] = 0;
    diag->notes = NULL;
    diag->notesLen = 0;
    diag->fixIts = NULL;
    diag->fixItsLen = 0;
    diag->expectations = NULL;
    diag->expectationsLen = 0;
}

typedef struct {
    H2StrView      src;
    H2Arena*       arena;
    const H2Token* tok;
    uint32_t       tokLen;
    uint32_t       pos;
    H2AstNode* _Nullable nodes;
    uint32_t nodeLen;
    uint32_t nodeCap;
    H2Diag* _Nullable diag;
    H2Features features;
} H2Parser;

static int H2PParseExpr(H2Parser* p, int minPrec, int32_t* out);
static int H2PParseType(H2Parser* p, int32_t* out);

static const H2Token* H2PPeek(H2Parser* p) {
    if (p->pos >= p->tokLen) {
        return &p->tok[p->tokLen - 1];
    }
    return &p->tok[p->pos];
}

static const H2Token* H2PPrev(H2Parser* p) {
    if (p->pos == 0) {
        return &p->tok[0];
    }
    return &p->tok[p->pos - 1];
}

static int H2PAt(H2Parser* p, H2TokenKind kind) {
    return H2PPeek(p)->kind == kind;
}

static int H2PMatch(H2Parser* p, H2TokenKind kind) {
    if (!H2PAt(p, kind)) {
        return 0;
    }
    p->pos++;
    return 1;
}

static const char* _Nullable H2PInsertionText(H2TokenKind kind) {
    switch (kind) {
        case H2Tok_COMMA:     return "\",\"";
        case H2Tok_SEMICOLON: return "\";\"";
        case H2Tok_RPAREN:    return "\")\"";
        case H2Tok_RBRACK:    return "\"]\"";
        case H2Tok_RBRACE:    return "\"}\"";
        case H2Tok_LPAREN:    return "\"(\"";
        case H2Tok_LBRACK:    return "\"[\"";
        case H2Tok_LBRACE:    return "\"{\"";
        default:              return NULL;
    }
}

static void H2PAttachGenericExpectation(H2Parser* p, H2DiagCode code) {
    if (p == NULL || p->diag == NULL) {
        return;
    }
    switch (code) {
        case H2Diag_EXPECTED_DECL:
            (void)H2DiagAddExpectation(
                p->arena, p->diag, H2DiagExpectationKind_DECL_FORM, "declaration");
            break;
        case H2Diag_EXPECTED_EXPR:
            (void)H2DiagAddExpectation(
                p->arena, p->diag, H2DiagExpectationKind_EXPR_FORM, "expression");
            break;
        case H2Diag_EXPECTED_TYPE:
            (void)H2DiagAddExpectation(p->arena, p->diag, H2DiagExpectationKind_TYPE_KIND, "type");
            break;
        default: break;
    }
}

static int H2PFail(H2Parser* p, H2DiagCode code) {
    const H2Token* t = H2PPeek(p);
    H2PSetDiag(p->diag, code, t->start, t->end);
    H2PAttachGenericExpectation(p, code);
    return -1;
}

static int H2PExpect(H2Parser* p, H2TokenKind kind, H2DiagCode code, const H2Token** out) {
    if (!H2PAt(p, kind)) {
        const H2Token* t = H2PPeek(p);
        H2PSetDiag(p->diag, code, t->start, t->end);
        H2PAttachGenericExpectation(p, code);
        if (p->diag != NULL) {
            (void)H2DiagAddExpectation(
                p->arena, p->diag, H2DiagExpectationKind_TOKEN, H2TokenKindName(kind));
            if (H2PInsertionText(kind) != NULL) {
                (void)H2DiagAddFixIt(
                    p->arena,
                    p->diag,
                    H2DiagFixItKind_INSERT,
                    t->start,
                    t->start,
                    H2PInsertionText(kind));
            }
        }
        return -1;
    }
    *out = H2PPeek(p);
    p->pos++;
    return 0;
}

static int H2PReservedName(const H2Parser* p, const H2Token* tok) {
    static const char reservedPrefix[6] = { '_', '_', 'h', 'o', 'p', '_' };
    uint32_t          n = tok->end - tok->start;
    return n >= 6u && memcmp(p->src.ptr + tok->start, reservedPrefix, 6u) == 0;
}

static int H2PIsHoleName(const H2Parser* p, const H2Token* tok) {
    return tok->kind == H2Tok_IDENT && tok->end == tok->start + 1u && p->src.ptr[tok->start] == '_';
}

static int H2PFailReservedName(H2Parser* p, const H2Token* tok) {
    H2PSetDiagWithArg(p->diag, H2Diag_RESERVED_NAME, tok->start, tok->end, tok->start, tok->end);
    return -1;
}

static int H2PExpectDeclName(H2Parser* p, const H2Token** out, int allowHole) {
    const H2Token* tok;
    if (H2PExpect(p, H2Tok_IDENT, H2Diag_UNEXPECTED_TOKEN, &tok) != 0) {
        return -1;
    }
    if (!allowHole && H2PIsHoleName(p, tok)) {
        return H2PFailReservedName(p, tok);
    }
    if (H2PReservedName(p, tok)) {
        H2PSetDiag(p->diag, H2Diag_RESERVED_HOP_PREFIX, tok->start, tok->end);
        return -1;
    }
    *out = tok;
    return 0;
}

static int H2PExpectFnName(H2Parser* p, const H2Token** out) {
    const H2Token* tok = H2PPeek(p);
    if (tok->kind == H2Tok_SIZEOF) {
        *out = tok;
        p->pos++;
        return 0;
    }
    return H2PExpectDeclName(p, out, 0);
}

static int H2PIsFieldSeparator(H2TokenKind kind) {
    return kind == H2Tok_SEMICOLON || kind == H2Tok_COMMA || kind == H2Tok_RBRACE
        || kind == H2Tok_ASSIGN || kind == H2Tok_EOF;
}

static int H2PAnonymousFieldLookahead(H2Parser* p, const H2Token** outLastIdent) {
    uint32_t       i = p->pos;
    const H2Token* last;

    if (i >= p->tokLen || p->tok[i].kind != H2Tok_IDENT) {
        return 0;
    }
    last = &p->tok[i];
    i++;
    while ((i + 1u) < p->tokLen && p->tok[i].kind == H2Tok_DOT && p->tok[i + 1u].kind == H2Tok_IDENT
           && p->tok[i].start == last->end && p->tok[i + 1u].start == p->tok[i].end)
    {
        last = &p->tok[i + 1u];
        i += 2u;
    }
    if (i >= p->tokLen || !H2PIsFieldSeparator(p->tok[i].kind)) {
        return 0;
    }
    if (outLastIdent != NULL) {
        *outLastIdent = last;
    }
    return 1;
}

static int32_t H2PNewNode(H2Parser* p, H2AstKind kind, uint32_t start, uint32_t end) {
    int32_t idx;
    if (p->nodeLen >= p->nodeCap) {
        H2PSetDiag(p->diag, H2Diag_ARENA_OOM, start, end);
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

static int H2PAddChild(H2Parser* p, int32_t parent, int32_t child) {
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

static int H2IsAssignmentOp(H2TokenKind kind) {
    switch (kind) {
        case H2Tok_ASSIGN:
        case H2Tok_ADD_ASSIGN:
        case H2Tok_SUB_ASSIGN:
        case H2Tok_MUL_ASSIGN:
        case H2Tok_DIV_ASSIGN:
        case H2Tok_MOD_ASSIGN:
        case H2Tok_AND_ASSIGN:
        case H2Tok_OR_ASSIGN:
        case H2Tok_XOR_ASSIGN:
        case H2Tok_LSHIFT_ASSIGN:
        case H2Tok_RSHIFT_ASSIGN: return 1;
        default:                  return 0;
    }
}

static int H2BinPrec(H2TokenKind kind) {
    if (H2IsAssignmentOp(kind)) {
        return 1;
    }
    switch (kind) {
        case H2Tok_LOGICAL_OR:  return 2;
        case H2Tok_LOGICAL_AND: return 3;
        case H2Tok_EQ:
        case H2Tok_NEQ:
        case H2Tok_LT:
        case H2Tok_GT:
        case H2Tok_LTE:
        case H2Tok_GTE:         return 4;
        case H2Tok_OR:
        case H2Tok_XOR:
        case H2Tok_ADD:
        case H2Tok_SUB:         return 5;
        case H2Tok_AND:
        case H2Tok_LSHIFT:
        case H2Tok_RSHIFT:
        case H2Tok_MUL:
        case H2Tok_DIV:
        case H2Tok_MOD:         return 6;
        default:                return 0;
    }
}

static int H2PParseType(H2Parser* p, int32_t* out);
static int H2PParseFnType(H2Parser* p, int32_t* out);
static int H2PParseTupleType(H2Parser* p, int32_t* out);
static int H2PParseExpr(H2Parser* p, int minPrec, int32_t* out);
static int H2PParseStmt(H2Parser* p, int32_t* out);
static int H2PParseDecl(H2Parser* p, int allowBody, int32_t* out);
static int H2PParseDeclInner(H2Parser* p, int allowBody, int32_t* out);
static int H2PParseDirective(H2Parser* p, int32_t* out);
static int H2PParseSwitchStmt(H2Parser* p, int32_t* out);
static int H2PParseAggregateDecl(H2Parser* p, int32_t* out);
static int H2PParseTypeAliasDecl(H2Parser* p, int32_t* out);

static int H2PIsTypeStart(H2TokenKind kind) {
    switch (kind) {
        case H2Tok_IDENT:
        case H2Tok_ANYTYPE:
        case H2Tok_TYPE:
        case H2Tok_STRUCT:
        case H2Tok_UNION:
        case H2Tok_MUL:
        case H2Tok_AND:
        case H2Tok_MUT:
        case H2Tok_LBRACE:
        case H2Tok_LBRACK:
        case H2Tok_LPAREN:
        case H2Tok_QUESTION: return 1;
        case H2Tok_FN:       return 1;
        default:             return 0;
    }
}

static int H2PCloneSubtree(H2Parser* p, int32_t nodeId, int32_t* out) {
    const H2AstNode* src;
    int32_t          clone;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= p->nodeLen) {
        return -1;
    }
    src = &p->nodes[nodeId];
    clone = H2PNewNode(p, src->kind, src->start, src->end);
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
        if (H2PCloneSubtree(p, child, &childClone) != 0) {
            return -1;
        }
        if (H2PAddChild(p, clone, childClone) != 0) {
            return -1;
        }
        child = p->nodes[child].nextSibling;
    }
    *out = clone;
    return 0;
}

static int H2PParseTypeName(H2Parser* p, int32_t* out) {
    const H2Token* first = NULL;
    const H2Token* last;
    int32_t        n;

    if (H2PAt(p, H2Tok_IDENT) || H2PAt(p, H2Tok_TYPE) || H2PAt(p, H2Tok_ANYTYPE)) {
        p->pos++;
        first = H2PPrev(p);
    } else {
        return H2PFail(p, H2Diag_EXPECTED_TYPE);
    }
    last = first;
    while ((p->pos + 1u) < p->tokLen && p->tok[p->pos].kind == H2Tok_DOT
           && p->tok[p->pos + 1u].kind == H2Tok_IDENT && p->tok[p->pos].start == last->end
           && p->tok[p->pos + 1u].start == p->tok[p->pos].end)
    {
        p->pos++;
        last = &p->tok[p->pos];
        p->pos++;
    }

    n = H2PNewNode(p, H2Ast_TYPE_NAME, first->start, last->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = first->start;
    p->nodes[n].dataEnd = last->end;
    if (H2PMatch(p, H2Tok_LBRACK)) {
        const H2Token* lb = H2PPrev(p);
        const H2Token* rb;
        p->nodes[n].end = lb->end;
        for (;;) {
            int32_t argType;
            if (H2PParseType(p, &argType) != 0) {
                return -1;
            }
            if (H2PAddChild(p, n, argType) != 0) {
                return -1;
            }
            if (!H2PMatch(p, H2Tok_COMMA)) {
                break;
            }
        }
        if (H2PExpect(p, H2Tok_RBRACK, H2Diag_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        p->nodes[n].end = rb->end;
    }
    *out = n;
    return 0;
}

static int H2PParseTypeParamList(H2Parser* p, int32_t ownerNode) {
    uint32_t       savedPos = p->pos;
    uint32_t       savedNodeLen = p->nodeLen;
    int32_t        lastChild = -1;
    const H2Token* rb;
    int            sawAny = 0;
    if (ownerNode >= 0 && (uint32_t)ownerNode < p->nodeLen) {
        int32_t child = p->nodes[ownerNode].firstChild;
        while (child >= 0) {
            lastChild = child;
            child = p->nodes[child].nextSibling;
        }
    }
    if (!H2PMatch(p, H2Tok_LBRACK)) {
        return 0;
    }
    for (;;) {
        const H2Token* name;
        int32_t        paramNode;
        if (!H2PAt(p, H2Tok_IDENT)) {
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
        if (H2PExpectDeclName(p, &name, 0) != 0) {
            return -1;
        }
        paramNode = H2PNewNode(p, H2Ast_TYPE_PARAM, name->start, name->end);
        if (paramNode < 0) {
            return -1;
        }
        p->nodes[paramNode].dataStart = name->start;
        p->nodes[paramNode].dataEnd = name->end;
        if (H2PAddChild(p, ownerNode, paramNode) != 0) {
            return -1;
        }
        if (!H2PMatch(p, H2Tok_COMMA)) {
            break;
        }
    }
    if (!sawAny || H2PExpect(p, H2Tok_RBRACK, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
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

static int H2PTryParseFnTypeNamedParamGroup(
    H2Parser* p, int32_t fnTypeNode, int* outConsumedGroup) {
    uint32_t savedPos = p->pos;
    uint32_t savedNodeLen = p->nodeLen;
    H2Diag   savedDiag = { 0 };
    uint32_t nameCount = 0;
    int32_t  typeNode = -1;
    uint32_t i;

    if (p->diag != NULL) {
        savedDiag = *p->diag;
    }
    if (!H2PAt(p, H2Tok_IDENT)) {
        *outConsumedGroup = 0;
        return 0;
    }

    for (;;) {
        const H2Token* name;
        if (H2PExpectDeclName(p, &name, 0) != 0) {
            goto not_group;
        }
        nameCount++;
        if (!H2PMatch(p, H2Tok_COMMA)) {
            break;
        }
        if (!H2PAt(p, H2Tok_IDENT)) {
            goto not_group;
        }
    }

    if (nameCount == 0 || !H2PIsTypeStart(H2PPeek(p)->kind)) {
        goto not_group;
    }
    if (H2PParseType(p, &typeNode) != 0) {
        goto not_group;
    }

    for (i = 0; i < nameCount; i++) {
        int32_t paramType = -1;
        if (i == 0) {
            paramType = typeNode;
        } else if (H2PCloneSubtree(p, typeNode, &paramType) != 0) {
            return -1;
        }
        if (H2PAddChild(p, fnTypeNode, paramType) != 0) {
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

static int H2PParseFnType(H2Parser* p, int32_t* out) {
    const H2Token* fnTok;
    const H2Token* rp;
    int32_t        fnTypeNode;
    int            sawVariadic = 0;

    if (H2PExpect(p, H2Tok_FN, H2Diag_EXPECTED_TYPE, &fnTok) != 0) {
        return -1;
    }
    if (H2PExpect(p, H2Tok_LPAREN, H2Diag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }

    fnTypeNode = H2PNewNode(p, H2Ast_TYPE_FN, fnTok->start, rp->end);
    if (fnTypeNode < 0) {
        return -1;
    }

    if (!H2PAt(p, H2Tok_RPAREN)) {
        for (;;) {
            int consumedGroup = 0;
            int isConstParam = H2PMatch(p, H2Tok_CONST);
            if (isConstParam) {
                uint32_t savedPos = p->pos;
                uint32_t savedNodeLen = p->nodeLen;
                H2Diag   savedDiag = { 0 };
                int32_t  paramType = -1;
                int      isVariadicParam = 0;
                if (p->diag != NULL) {
                    savedDiag = *p->diag;
                }
                if (H2PMatch(p, H2Tok_ELLIPSIS)) {
                    isVariadicParam = 1;
                    if (sawVariadic) {
                        return H2PFail(p, H2Diag_VARIADIC_PARAM_DUPLICATE);
                    }
                    if (H2PParseType(p, &paramType) != 0) {
                        return -1;
                    }
                } else if (H2PAt(p, H2Tok_IDENT)) {
                    const H2Token* nameTok = NULL;
                    if (H2PExpectDeclName(p, &nameTok, 0) != 0) {
                        return -1;
                    }
                    if (H2PAt(p, H2Tok_COMMA)) {
                        p->pos = savedPos;
                        p->nodeLen = savedNodeLen;
                        if (p->diag != NULL) {
                            *p->diag = savedDiag;
                        }
                    }
                    if (paramType < 0 && H2PMatch(p, H2Tok_ELLIPSIS)) {
                        isVariadicParam = 1;
                        if (sawVariadic) {
                            return H2PFail(p, H2Diag_VARIADIC_PARAM_DUPLICATE);
                        }
                        if (H2PParseType(p, &paramType) != 0) {
                            return -1;
                        }
                    } else if (paramType < 0 && H2PIsTypeStart(H2PPeek(p)->kind)) {
                        if (H2PParseType(p, &paramType) != 0) {
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
                    if (H2PParseType(p, &paramType) != 0) {
                        return -1;
                    }
                }
                if (isVariadicParam) {
                    p->nodes[paramType].flags |= H2AstFlag_PARAM_VARIADIC;
                    sawVariadic = 1;
                }
                p->nodes[paramType].flags |= H2AstFlag_PARAM_CONST;
                if (H2PAddChild(p, fnTypeNode, paramType) != 0) {
                    return -1;
                }
            } else if (H2PMatch(p, H2Tok_ELLIPSIS)) {
                int32_t paramType = -1;
                if (sawVariadic) {
                    return H2PFail(p, H2Diag_VARIADIC_PARAM_DUPLICATE);
                }
                if (H2PParseType(p, &paramType) != 0) {
                    return -1;
                }
                p->nodes[paramType].flags |= H2AstFlag_PARAM_VARIADIC;
                if (H2PAddChild(p, fnTypeNode, paramType) != 0) {
                    return -1;
                }
                sawVariadic = 1;
            } else if (
                H2PAt(p, H2Tok_IDENT)
                && H2PTryParseFnTypeNamedParamGroup(p, fnTypeNode, &consumedGroup) != 0)
            {
                return -1;
            }
            if (!consumedGroup && !isConstParam) {
                int32_t paramType = -1;
                if (H2PParseType(p, &paramType) != 0) {
                    return -1;
                }
                if (H2PAddChild(p, fnTypeNode, paramType) != 0) {
                    return -1;
                }
            }
            if (!H2PMatch(p, H2Tok_COMMA)) {
                break;
            }
            if (sawVariadic) {
                return H2PFail(p, H2Diag_VARIADIC_PARAM_NOT_LAST);
            }
        }
    }

    if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }
    p->nodes[fnTypeNode].end = rp->end;

    if (H2PIsTypeStart(H2PPeek(p)->kind)) {
        int32_t resultType = -1;
        if (H2PParseType(p, &resultType) != 0) {
            return -1;
        }
        p->nodes[resultType].flags = 1;
        if (H2PAddChild(p, fnTypeNode, resultType) != 0) {
            return -1;
        }
        p->nodes[fnTypeNode].end = p->nodes[resultType].end;
    }

    *out = fnTypeNode;
    return 0;
}

static int H2PParseTupleType(H2Parser* p, int32_t* out) {
    const H2Token* lp = NULL;
    const H2Token* rp = NULL;
    int32_t        items[256];
    uint32_t       itemCount = 0;
    int32_t        tupleNode;
    uint32_t       i;

    if (H2PExpect(p, H2Tok_LPAREN, H2Diag_EXPECTED_TYPE, &lp) != 0) {
        return -1;
    }
    if (H2PParseType(p, &items[itemCount]) != 0) {
        return -1;
    }
    itemCount++;
    if (!H2PMatch(p, H2Tok_COMMA)) {
        return H2PFail(p, H2Diag_EXPECTED_TYPE);
    }
    for (;;) {
        if (itemCount >= (uint32_t)(sizeof(items) / sizeof(items[0]))) {
            return H2PFail(p, H2Diag_ARENA_OOM);
        }
        if (H2PParseType(p, &items[itemCount]) != 0) {
            return -1;
        }
        itemCount++;
        if (!H2PMatch(p, H2Tok_COMMA)) {
            break;
        }
    }
    if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }

    tupleNode = H2PNewNode(p, H2Ast_TYPE_TUPLE, lp->start, rp->end);
    if (tupleNode < 0) {
        return -1;
    }
    for (i = 0; i < itemCount; i++) {
        if (H2PAddChild(p, tupleNode, items[i]) != 0) {
            return -1;
        }
    }
    *out = tupleNode;
    return 0;
}

static int H2PParseFnResultClause(H2Parser* p, int32_t fnNode) {
    const H2Token* lp = NULL;
    const H2Token* rp = NULL;
    int32_t        resultTypes[256];
    uint32_t       resultCount = 0;
    int32_t        resultTypeNode = -1;
    uint32_t       i;

    if (H2PExpect(p, H2Tok_LPAREN, H2Diag_EXPECTED_TYPE, &lp) != 0) {
        return -1;
    }
    if (H2PAt(p, H2Tok_RPAREN)) {
        return H2PFail(p, H2Diag_EXPECTED_TYPE);
    }

    for (;;) {
        int consumedGroup = 0;
        if (H2PAt(p, H2Tok_IDENT)) {
            uint32_t savedPos = p->pos;
            uint32_t savedNodeLen = p->nodeLen;
            H2Diag   savedDiag = { 0 };
            uint32_t nameCount = 0;
            int32_t  groupType = -1;
            if (p->diag != NULL) {
                savedDiag = *p->diag;
            }
            for (;;) {
                const H2Token* name;
                if (H2PExpectDeclName(p, &name, 0) != 0) {
                    break;
                }
                (void)name;
                nameCount++;
                if (!H2PMatch(p, H2Tok_COMMA)) {
                    break;
                }
                if (!H2PAt(p, H2Tok_IDENT)) {
                    break;
                }
            }
            if (nameCount > 0 && H2PIsTypeStart(H2PPeek(p)->kind)
                && H2PParseType(p, &groupType) == 0)
            {
                for (i = 0; i < nameCount; i++) {
                    int32_t itemType = -1;
                    if (resultCount >= (uint32_t)(sizeof(resultTypes) / sizeof(resultTypes[0]))) {
                        return H2PFail(p, H2Diag_ARENA_OOM);
                    }
                    if (i == 0) {
                        itemType = groupType;
                    } else if (H2PCloneSubtree(p, groupType, &itemType) != 0) {
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
                return H2PFail(p, H2Diag_ARENA_OOM);
            }
            if (H2PParseType(p, &resultTypes[resultCount]) != 0) {
                return -1;
            }
            resultCount++;
        }
        if (!H2PMatch(p, H2Tok_COMMA)) {
            break;
        }
    }

    if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_TYPE, &rp) != 0) {
        return -1;
    }
    if (resultCount == 0) {
        return H2PFail(p, H2Diag_EXPECTED_TYPE);
    }

    if (resultCount == 1) {
        resultTypeNode = resultTypes[0];
    } else {
        resultTypeNode = H2PNewNode(p, H2Ast_TYPE_TUPLE, lp->start, rp->end);
        if (resultTypeNode < 0) {
            return -1;
        }
        for (i = 0; i < resultCount; i++) {
            if (H2PAddChild(p, resultTypeNode, resultTypes[i]) != 0) {
                return -1;
            }
        }
    }

    p->nodes[resultTypeNode].flags = 1;
    if (H2PAddChild(p, fnNode, resultTypeNode) != 0) {
        return -1;
    }
    p->nodes[fnNode].end = p->nodes[resultTypeNode].end;
    return 0;
}

static int H2PParseAnonymousAggregateFieldDeclList(H2Parser* p, int32_t aggTypeNode) {
    while (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
        const H2Token* names[256];
        uint32_t       nameCount = 0;
        int32_t        typeNode = -1;
        int32_t        defaultExpr = -1;
        uint32_t       i;

        if (H2PMatch(p, H2Tok_SEMICOLON)) {
            continue;
        }
        if (H2PExpectDeclName(p, &names[nameCount], 0) != 0) {
            return -1;
        }
        nameCount++;
        while (H2PMatch(p, H2Tok_COMMA)) {
            if (!H2PAt(p, H2Tok_IDENT)) {
                return H2PFail(p, H2Diag_EXPECTED_TYPE);
            }
            if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                return H2PFail(p, H2Diag_ARENA_OOM);
            }
            if (H2PExpectDeclName(p, &names[nameCount], 0) != 0) {
                return -1;
            }
            nameCount++;
        }

        if (H2PParseType(p, &typeNode) != 0) {
            return -1;
        }
        if (H2PMatch(p, H2Tok_ASSIGN)) {
            if (nameCount > 1) {
                const H2Token* eq = H2PPrev(p);
                H2PSetDiag(p->diag, H2Diag_UNEXPECTED_TOKEN, eq->start, eq->end);
                return -1;
            }
            if (H2PParseExpr(p, 1, &defaultExpr) != 0) {
                return -1;
            }
        }
        for (i = 0; i < nameCount; i++) {
            int32_t fieldNode;
            int32_t fieldTypeNode;
            if (i == 0) {
                fieldTypeNode = typeNode;
            } else {
                if (H2PCloneSubtree(p, typeNode, &fieldTypeNode) != 0) {
                    return -1;
                }
            }
            fieldNode = H2PNewNode(p, H2Ast_FIELD, names[i]->start, p->nodes[fieldTypeNode].end);
            if (fieldNode < 0) {
                return -1;
            }
            p->nodes[fieldNode].dataStart = names[i]->start;
            p->nodes[fieldNode].dataEnd = names[i]->end;
            if (H2PAddChild(p, fieldNode, fieldTypeNode) != 0) {
                return -1;
            }
            if (i == 0 && defaultExpr >= 0) {
                p->nodes[fieldNode].end = p->nodes[defaultExpr].end;
                if (H2PAddChild(p, fieldNode, defaultExpr) != 0) {
                    return -1;
                }
            }
            if (H2PAddChild(p, aggTypeNode, fieldNode) != 0) {
                return -1;
            }
        }

        if (H2PMatch(p, H2Tok_SEMICOLON)) {
            continue;
        }
        if (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
    }
    return 0;
}

static int H2PParseAnonymousAggregateType(H2Parser* p, int32_t* out) {
    const H2Token* kw = NULL;
    const H2Token* lb;
    const H2Token* rb;
    H2AstKind      kind = H2Ast_TYPE_ANON_STRUCT;
    int32_t        typeNode;

    if (H2PAt(p, H2Tok_STRUCT) || H2PAt(p, H2Tok_UNION)) {
        p->pos++;
        kw = H2PPrev(p);
        if (kw->kind == H2Tok_UNION) {
            kind = H2Ast_TYPE_ANON_UNION;
        }
    }

    if (H2PExpect(p, H2Tok_LBRACE, H2Diag_EXPECTED_TYPE, &lb) != 0) {
        return -1;
    }
    typeNode = H2PNewNode(p, kind, kw != NULL ? kw->start : lb->start, lb->end);
    if (typeNode < 0) {
        return -1;
    }
    if (H2PParseAnonymousAggregateFieldDeclList(p, typeNode) != 0) {
        return -1;
    }
    if (H2PExpect(p, H2Tok_RBRACE, H2Diag_EXPECTED_TYPE, &rb) != 0) {
        return -1;
    }
    p->nodes[typeNode].end = rb->end;
    *out = typeNode;
    return 0;
}

static int H2PParseType(H2Parser* p, int32_t* out) {
    const H2Token* t;
    int32_t        typeNode;
    int32_t        child;

    /* Prefix '?' optional type. */
    if (H2PMatch(p, H2Tok_QUESTION)) {
        t = H2PPrev(p);
        typeNode = H2PNewNode(p, H2Ast_TYPE_OPTIONAL, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (H2PParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return H2PAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (H2PMatch(p, H2Tok_MUL)) {
        t = H2PPrev(p);
        typeNode = H2PNewNode(p, H2Ast_TYPE_PTR, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (H2PParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return H2PAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (H2PMatch(p, H2Tok_AND)) {
        t = H2PPrev(p);
        typeNode = H2PNewNode(p, H2Ast_TYPE_REF, t->start, t->end);
        if (typeNode < 0) {
            return -1;
        }
        if (H2PParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
        return H2PAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
    }

    if (H2PMatch(p, H2Tok_MUT)) {
        (void)H2PPrev(p);
        return H2PFail(p, H2Diag_EXPECTED_TYPE);
    }

    if (H2PAt(p, H2Tok_LPAREN)) {
        return H2PParseTupleType(p, out);
    }

    if (H2PMatch(p, H2Tok_LBRACK)) {
        const H2Token* lb = H2PPrev(p);
        const H2Token* nTok = NULL;
        int32_t        lenExpr = -1;
        const H2Token* rb;
        H2AstKind      kind;

        if (H2PParseType(p, &child) != 0) {
            return -1;
        }

        if (H2PMatch(p, H2Tok_RBRACK)) {
            rb = H2PPrev(p);
            typeNode = H2PNewNode(p, H2Ast_TYPE_SLICE, lb->start, rb->end);
            if (typeNode < 0) {
                return -1;
            }
            return H2PAddChild(p, typeNode, child) == 0 ? (*out = typeNode, 0) : -1;
        }

        if (H2PMatch(p, H2Tok_DOT)) {
            kind = H2Ast_TYPE_VARRAY;
            if (H2PExpect(p, H2Tok_IDENT, H2Diag_EXPECTED_TYPE, &nTok) != 0) {
                return -1;
            }
        } else {
            kind = H2Ast_TYPE_ARRAY;
            if (H2PParseExpr(p, 1, &lenExpr) != 0) {
                return -1;
            }
        }

        typeNode = H2PNewNode(p, kind, lb->start, lb->end);
        if (typeNode < 0) {
            return -1;
        }
        if (nTok != NULL) {
            p->nodes[typeNode].dataStart = nTok->start;
            p->nodes[typeNode].dataEnd = nTok->end;
        }
        if (H2PExpect(p, H2Tok_RBRACK, H2Diag_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = rb->end;
        if (H2PAddChild(p, typeNode, child) != 0) {
            return -1;
        }
        if (kind == H2Ast_TYPE_ARRAY) {
            const H2AstNode* lenNode = &p->nodes[lenExpr];
            if ((lenNode->kind == H2Ast_INT && lenNode->dataEnd > lenNode->dataStart)
                && (lenNode->flags & H2AstFlag_PAREN) == 0)
            {
                p->nodes[typeNode].dataStart = lenNode->dataStart;
                p->nodes[typeNode].dataEnd = lenNode->dataEnd;
            } else {
                p->nodes[typeNode].dataStart = lenNode->start;
                p->nodes[typeNode].dataEnd = lenNode->end;
                if (H2PAddChild(p, typeNode, lenExpr) != 0) {
                    return -1;
                }
            }
        }
        *out = typeNode;
        return 0;
    }

    if (H2PAt(p, H2Tok_FN)) {
        return H2PParseFnType(p, out);
    }

    if (H2PAt(p, H2Tok_LBRACE) || H2PAt(p, H2Tok_STRUCT) || H2PAt(p, H2Tok_UNION)) {
        return H2PParseAnonymousAggregateType(p, out);
    }

    if (H2PParseTypeName(p, out) != 0) {
        return -1;
    }
    return 0;
}

static int H2PParseCompoundLiteralTail(H2Parser* p, int32_t typeNode, int32_t* out) {
    const H2Token* lb;
    const H2Token* rb;
    int32_t        lit;

    if (H2PExpect(p, H2Tok_LBRACE, H2Diag_EXPECTED_EXPR, &lb) != 0) {
        return -1;
    }

    lit = H2PNewNode(
        p, H2Ast_COMPOUND_LIT, typeNode >= 0 ? p->nodes[typeNode].start : lb->start, lb->end);
    if (lit < 0) {
        return -1;
    }
    if (typeNode >= 0 && H2PAddChild(p, lit, typeNode) != 0) {
        return -1;
    }

    while (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
        const H2Token* fieldName;
        int32_t        field;
        int32_t        expr = -1;
        int            hasDottedPath = 0;

        if (H2PExpect(p, H2Tok_IDENT, H2Diag_UNEXPECTED_TOKEN, &fieldName) != 0) {
            return -1;
        }
        field = H2PNewNode(p, H2Ast_COMPOUND_FIELD, fieldName->start, fieldName->end);
        if (field < 0) {
            return -1;
        }
        p->nodes[field].dataStart = fieldName->start;
        p->nodes[field].dataEnd = fieldName->end;
        while (H2PMatch(p, H2Tok_DOT)) {
            const H2Token* seg;
            if (H2PExpect(p, H2Tok_IDENT, H2Diag_UNEXPECTED_TOKEN, &seg) != 0) {
                return -1;
            }
            p->nodes[field].dataEnd = seg->end;
            hasDottedPath = 1;
        }
        if (H2PMatch(p, H2Tok_COLON)) {
            if (H2PParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (H2PAddChild(p, field, expr) != 0) {
                return -1;
            }
            p->nodes[field].end = p->nodes[expr].end;
        } else {
            if (hasDottedPath) {
                return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
            }
            p->nodes[field].flags |= H2AstFlag_COMPOUND_FIELD_SHORTHAND;
        }
        if (H2PAddChild(p, lit, field) != 0) {
            return -1;
        }

        if (H2PMatch(p, H2Tok_COMMA) || H2PMatch(p, H2Tok_SEMICOLON)) {
            if (H2PAt(p, H2Tok_RBRACE)) {
                break;
            }
            continue;
        }
        break;
    }

    if (H2PExpect(p, H2Tok_RBRACE, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[lit].end = rb->end;
    *out = lit;
    return 0;
}

static int H2PParseArrayLiteral(H2Parser* p, int32_t* out) {
    const H2Token* lb;
    const H2Token* rb;
    int32_t        lit;

    if (H2PExpect(p, H2Tok_LBRACK, H2Diag_EXPECTED_EXPR, &lb) != 0) {
        return -1;
    }
    lit = H2PNewNode(p, H2Ast_ARRAY_LIT, lb->start, lb->end);
    if (lit < 0) {
        return -1;
    }

    while (!H2PAt(p, H2Tok_RBRACK) && !H2PAt(p, H2Tok_EOF)) {
        int32_t elem;
        if (H2PParseExpr(p, 1, &elem) != 0) {
            return -1;
        }
        if (H2PAddChild(p, lit, elem) != 0) {
            return -1;
        }
        if (H2PMatch(p, H2Tok_COMMA) || H2PMatch(p, H2Tok_SEMICOLON)) {
            if (H2PAt(p, H2Tok_RBRACK)) {
                break;
            }
            continue;
        }
        break;
    }

    if (H2PExpect(p, H2Tok_RBRACK, H2Diag_EXPECTED_EXPR, &rb) != 0) {
        return -1;
    }
    p->nodes[lit].end = rb->end;
    *out = lit;
    return 0;
}

static int H2PParseNewExpr(H2Parser* p, int32_t* out) {
    const H2Token* kw = H2PPrev(p);
    const H2Token* rb;
    int32_t        n;
    int32_t        typeNode = -1;
    int32_t        countNode = -1;
    int32_t        initNode = -1;
    int32_t        allocNode = -1;

    n = H2PNewNode(p, H2Ast_NEW, kw->start, kw->end);
    if (n < 0) {
        return -1;
    }

    if (H2PMatch(p, H2Tok_LBRACK)) {
        uint32_t savedPos = p->pos;
        uint32_t savedNodeLen = p->nodeLen;
        H2Diag   savedDiag = { 0 };
        int      parsedCount = 0;
        if (p->diag != NULL) {
            savedDiag = *p->diag;
        }
        if (H2PParseType(p, &typeNode) == 0 && H2PParseExpr(p, 1, &countNode) == 0
            && H2PExpect(p, H2Tok_RBRACK, H2Diag_EXPECTED_EXPR, &rb) == 0)
        {
            parsedCount = 1;
        }
        if (parsedCount) {
            p->nodes[n].flags |= H2AstFlag_NEW_HAS_COUNT;
            p->nodes[n].end = rb->end;
        } else {
            p->pos = savedPos - 1u;
            p->nodeLen = savedNodeLen;
            if (p->diag != NULL) {
                *p->diag = savedDiag;
            }
            if (H2PParseArrayLiteral(p, &initNode) != 0) {
                return -1;
            }
            p->nodes[n].flags |= H2AstFlag_NEW_HAS_ARRAY_LIT;
            p->nodes[n].end = p->nodes[initNode].end;
            typeNode = -1;
            countNode = -1;
        }
    } else {
        if (H2PParseType(p, &typeNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[typeNode].end;
        if (H2PAt(p, H2Tok_LBRACE)) {
            if (H2PParseCompoundLiteralTail(p, -1, &initNode) != 0) {
                return -1;
            }
            p->nodes[n].flags |= H2AstFlag_NEW_HAS_INIT;
            p->nodes[n].end = p->nodes[initNode].end;
        }
    }

    if (H2PMatch(p, H2Tok_IN)) {
        if (H2PParseExpr(p, 1, &allocNode) != 0) {
            return -1;
        }
        p->nodes[n].flags |= H2AstFlag_NEW_HAS_ALLOC;
        p->nodes[n].end = p->nodes[allocNode].end;
    }

    if (typeNode >= 0 && H2PAddChild(p, n, typeNode) != 0) {
        return -1;
    }
    if (countNode >= 0) {
        if (H2PAddChild(p, n, countNode) != 0) {
            return -1;
        }
    }
    if (initNode >= 0) {
        if (H2PAddChild(p, n, initNode) != 0) {
            return -1;
        }
    }
    if (allocNode >= 0) {
        if (H2PAddChild(p, n, allocNode) != 0) {
            return -1;
        }
    }

    *out = n;
    return 0;
}

static int H2PParsePrimary(H2Parser* p, int32_t* out) {
    const H2Token* t = H2PPeek(p);
    int32_t        n;

    if (H2PAt(p, H2Tok_IDENT)) {
        uint32_t savedPos = p->pos;
        uint32_t savedNodeLen = p->nodeLen;
        H2Diag   savedDiag = { 0 };
        int32_t  typeNode;
        if (p->diag != NULL) {
            savedDiag = *p->diag;
        }
        if (H2PParseTypeName(p, &typeNode) == 0 && H2PAt(p, H2Tok_LBRACE)
            && H2PPeek(p)->start == p->nodes[typeNode].end)
        {
            return H2PParseCompoundLiteralTail(p, typeNode, out);
        }
        p->pos = savedPos;
        p->nodeLen = savedNodeLen;
        if (p->diag != NULL) {
            *p->diag = savedDiag;
        }
    }

    if (H2PAt(p, H2Tok_LBRACE)) {
        return H2PParseCompoundLiteralTail(p, -1, out);
    }

    if (H2PAt(p, H2Tok_TYPE) && (p->pos + 1u) < p->tokLen
        && H2PIsTypeStart(p->tok[p->pos + 1u].kind))
    {
        int32_t typeNode;
        p->pos++;
        t = H2PPrev(p);
        n = H2PNewNode(p, H2Ast_TYPE_VALUE, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        if (H2PParseType(p, &typeNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[typeNode].end;
        if (H2PAddChild(p, n, typeNode) != 0) {
            return -1;
        }
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_IDENT) || H2PMatch(p, H2Tok_TYPE)) {
        t = H2PPrev(p);
        n = H2PNewNode(p, H2Ast_IDENT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_INT)) {
        n = H2PNewNode(p, H2Ast_INT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_FLOAT)) {
        n = H2PNewNode(p, H2Ast_FLOAT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_STRING)) {
        n = H2PNewNode(p, H2Ast_STRING, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_RUNE)) {
        n = H2PNewNode(p, H2Ast_RUNE, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_TRUE) || H2PMatch(p, H2Tok_FALSE)) {
        t = H2PPrev(p);
        n = H2PNewNode(p, H2Ast_BOOL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_NULL)) {
        t = H2PPrev(p);
        n = H2PNewNode(p, H2Ast_NULL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_LPAREN)) {
        const H2Token* lp = H2PPrev(p);
        int32_t        firstExpr = -1;
        int32_t        exprItems[256];
        uint32_t       exprCount = 0;
        int32_t        tupleExpr = -1;
        uint32_t       i;
        if (H2PParseExpr(p, 1, &firstExpr) != 0) {
            return -1;
        }
        if (!H2PMatch(p, H2Tok_COMMA)) {
            *out = firstExpr;
            if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            if (*out >= 0) {
                p->nodes[*out].flags |= H2AstFlag_PAREN;
            }
            return 0;
        }

        exprItems[exprCount++] = firstExpr;
        for (;;) {
            if (exprCount >= (uint32_t)(sizeof(exprItems) / sizeof(exprItems[0]))) {
                return H2PFail(p, H2Diag_ARENA_OOM);
            }
            if (H2PParseExpr(p, 1, &exprItems[exprCount]) != 0) {
                return -1;
            }
            exprCount++;
            if (!H2PMatch(p, H2Tok_COMMA)) {
                break;
            }
        }
        if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_EXPR, &t) != 0) {
            return -1;
        }
        tupleExpr = H2PNewNode(p, H2Ast_TUPLE_EXPR, lp->start, t->end);
        if (tupleExpr < 0) {
            return -1;
        }
        for (i = 0; i < exprCount; i++) {
            if (H2PAddChild(p, tupleExpr, exprItems[i]) != 0) {
                return -1;
            }
        }
        *out = tupleExpr;
        return 0;
    }

    if (H2PAt(p, H2Tok_LBRACK)) {
        return H2PParseArrayLiteral(p, out);
    }

    if (H2PMatch(p, H2Tok_SIZEOF)) {
        const H2Token* kw = H2PPrev(p);
        const H2Token* rp;
        int32_t        inner;
        uint32_t       savePos;
        uint32_t       saveNodeLen;
        if (H2PExpect(p, H2Tok_LPAREN, H2Diag_EXPECTED_EXPR, &t) != 0) {
            return -1;
        }
        n = H2PNewNode(p, H2Ast_SIZEOF, kw->start, t->end);
        if (n < 0) {
            return -1;
        }

        savePos = p->pos;
        saveNodeLen = p->nodeLen;
        if (H2PParseType(p, &inner) == 0 && H2PAt(p, H2Tok_RPAREN)) {
            p->nodes[n].flags = 1;
            if (H2PAddChild(p, n, inner) != 0) {
                return -1;
            }
            if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_EXPR, &rp) != 0) {
                return -1;
            }
            p->nodes[n].end = rp->end;
            *out = n;
            return 0;
        }
        p->pos = savePos;
        p->nodeLen = saveNodeLen;
        if (p->diag != NULL) {
            *p->diag = (H2Diag){ 0 };
        }

        if (H2PParseExpr(p, 1, &inner) != 0) {
            return -1;
        }
        p->nodes[n].flags = 0;
        if (H2PAddChild(p, n, inner) != 0) {
            return -1;
        }
        if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_EXPR, &rp) != 0) {
            return -1;
        }
        p->nodes[n].end = rp->end;
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_NEW)) {
        return H2PParseNewExpr(p, out);
    }

    return H2PFail(p, H2Diag_EXPECTED_EXPR);
}

static int H2PParsePostfix(H2Parser* p, int32_t* expr) {
    for (;;) {
        int32_t        n;
        const H2Token* t;

        if (H2PMatch(p, H2Tok_LPAREN)) {
            n = H2PNewNode(p, H2Ast_CALL, p->nodes[*expr].start, H2PPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (H2PAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (!H2PAt(p, H2Tok_RPAREN)) {
                for (;;) {
                    int32_t        arg;
                    int32_t        argNode;
                    const H2Token* labelTok = NULL;
                    const H2Token* spreadTok = NULL;
                    uint32_t       argStart = H2PPeek(p)->start;
                    if (H2PAt(p, H2Tok_IDENT) && (p->pos + 1u) < p->tokLen
                        && p->tok[p->pos + 1u].kind == H2Tok_COLON)
                    {
                        labelTok = H2PPeek(p);
                        p->pos += 2u;
                    }
                    if (H2PParseExpr(p, 1, &arg) != 0) {
                        return -1;
                    }
                    if (H2PMatch(p, H2Tok_ELLIPSIS)) {
                        spreadTok = H2PPrev(p);
                    }
                    argNode = H2PNewNode(
                        p,
                        H2Ast_CALL_ARG,
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
                        p->nodes[argNode].flags |= H2AstFlag_CALL_ARG_SPREAD;
                    }
                    if (H2PAddChild(p, argNode, arg) != 0 || H2PAddChild(p, n, argNode) != 0) {
                        return -1;
                    }
                    if (!H2PMatch(p, H2Tok_COMMA)) {
                        break;
                    }
                }
            }
            if (H2PExpect(p, H2Tok_RPAREN, H2Diag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (H2PMatch(p, H2Tok_LBRACK)) {
            int32_t firstExpr = -1;
            n = H2PNewNode(p, H2Ast_INDEX, p->nodes[*expr].start, H2PPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (H2PAddChild(p, n, *expr) != 0) {
                return -1;
            }

            if (H2PMatch(p, H2Tok_COLON)) {
                p->nodes[n].flags |= H2AstFlag_INDEX_SLICE;
                if (!H2PAt(p, H2Tok_RBRACK)) {
                    if (H2PParseExpr(p, 1, &firstExpr) != 0) {
                        return -1;
                    }
                    p->nodes[n].flags |= H2AstFlag_INDEX_HAS_END;
                    if (H2PAddChild(p, n, firstExpr) != 0) {
                        return -1;
                    }
                }
                if (H2PExpect(p, H2Tok_RBRACK, H2Diag_EXPECTED_EXPR, &t) != 0) {
                    return -1;
                }
                p->nodes[n].end = t->end;
                *expr = n;
                continue;
            }

            if (H2PParseExpr(p, 1, &firstExpr) != 0) {
                return -1;
            }
            if (H2PMatch(p, H2Tok_COLON)) {
                int32_t endExpr = -1;
                p->nodes[n].flags |= H2AstFlag_INDEX_SLICE;
                p->nodes[n].flags |= H2AstFlag_INDEX_HAS_START;
                if (H2PAddChild(p, n, firstExpr) != 0) {
                    return -1;
                }
                if (!H2PAt(p, H2Tok_RBRACK)) {
                    if (H2PParseExpr(p, 1, &endExpr) != 0) {
                        return -1;
                    }
                    p->nodes[n].flags |= H2AstFlag_INDEX_HAS_END;
                    if (H2PAddChild(p, n, endExpr) != 0) {
                        return -1;
                    }
                }
                if (H2PExpect(p, H2Tok_RBRACK, H2Diag_EXPECTED_EXPR, &t) != 0) {
                    return -1;
                }
                p->nodes[n].end = t->end;
                *expr = n;
                continue;
            }

            if (H2PAddChild(p, n, firstExpr) != 0) {
                return -1;
            }
            if (H2PExpect(p, H2Tok_RBRACK, H2Diag_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (H2PMatch(p, H2Tok_DOT)) {
            const H2Token* fieldTok;
            n = H2PNewNode(p, H2Ast_FIELD_EXPR, p->nodes[*expr].start, H2PPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (H2PAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (H2PExpect(p, H2Tok_IDENT, H2Diag_EXPECTED_EXPR, &fieldTok) != 0) {
                return -1;
            }
            p->nodes[n].dataStart = fieldTok->start;
            p->nodes[n].dataEnd = fieldTok->end;
            p->nodes[n].end = fieldTok->end;
            *expr = n;
            continue;
        }

        if (H2PMatch(p, H2Tok_AS)) {
            int32_t typeNode;
            n = H2PNewNode(p, H2Ast_CAST, p->nodes[*expr].start, H2PPrev(p)->end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)H2Tok_AS;
            if (H2PAddChild(p, n, *expr) != 0) {
                return -1;
            }
            if (H2PParseType(p, &typeNode) != 0) {
                return -1;
            }
            if (H2PAddChild(p, n, typeNode) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[typeNode].end;
            *expr = n;
            continue;
        }

        if (H2PMatch(p, H2Tok_NOT)) {
            t = H2PPrev(p);
            n = H2PNewNode(p, H2Ast_UNWRAP, p->nodes[*expr].start, t->end);
            if (n < 0) {
                return -1;
            }
            if (H2PAddChild(p, n, *expr) != 0) {
                return -1;
            }
            *expr = n;
            continue;
        }

        break;
    }
    return 0;
}

static int H2PParsePrefix(H2Parser* p, int32_t* out) {
    H2TokenKind    op = H2PPeek(p)->kind;
    int32_t        rhs;
    int32_t        n;
    const H2Token* t = H2PPeek(p);

    switch (op) {
        case H2Tok_ADD:
        case H2Tok_SUB:
        case H2Tok_NOT:
        case H2Tok_MUL:
        case H2Tok_AND:
            p->pos++;
            if (H2PParsePrefix(p, &rhs) != 0) {
                return -1;
            }
            n = H2PNewNode(p, H2Ast_UNARY, t->start, p->nodes[rhs].end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)op;
            if (H2PAddChild(p, n, rhs) != 0) {
                return -1;
            }
            *out = n;
            return 0;
        default:
            if (H2PParsePrimary(p, out) != 0) {
                return -1;
            }
            return H2PParsePostfix(p, out);
    }
}

static int H2PParseExpr(H2Parser* p, int minPrec, int32_t* out) {
    int32_t lhs;
    if (H2PParsePrefix(p, &lhs) != 0) {
        return -1;
    }

    for (;;) {
        H2TokenKind op = H2PPeek(p)->kind;
        int         prec = H2BinPrec(op);
        int         rightAssoc = H2IsAssignmentOp(op);
        int32_t     rhs;
        int32_t     n;

        if (prec < minPrec || prec == 0) {
            break;
        }
        p->pos++;
        if (H2PParseExpr(p, rightAssoc ? prec : prec + 1, &rhs) != 0) {
            return -1;
        }
        n = H2PNewNode(p, H2Ast_BINARY, p->nodes[lhs].start, p->nodes[rhs].end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].op = (uint16_t)op;
        if (H2PAddChild(p, n, lhs) != 0 || H2PAddChild(p, n, rhs) != 0) {
            return -1;
        }
        lhs = n;
    }

    *out = lhs;
    return 0;
}

static int H2PBuildListNode(
    H2Parser* p, H2AstKind kind, int32_t* items, uint32_t itemCount, int32_t* out) {
    int32_t  n;
    uint32_t i;
    if (itemCount == 0) {
        return H2PFail(p, H2Diag_EXPECTED_EXPR);
    }
    n = H2PNewNode(p, kind, p->nodes[items[0]].start, p->nodes[items[itemCount - 1u]].end);
    if (n < 0) {
        return -1;
    }
    for (i = 0; i < itemCount; i++) {
        if (H2PAddChild(p, n, items[i]) != 0) {
            return -1;
        }
    }
    *out = n;
    return 0;
}

static int H2PParseExprList(H2Parser* p, int32_t* out) {
    int32_t  items[256];
    uint32_t itemCount = 0;
    if (H2PParseExpr(p, 1, &items[itemCount]) != 0) {
        return -1;
    }
    itemCount++;
    while (H2PMatch(p, H2Tok_COMMA)) {
        if (itemCount >= (uint32_t)(sizeof(items) / sizeof(items[0]))) {
            return H2PFail(p, H2Diag_ARENA_OOM);
        }
        if (H2PParseExpr(p, 1, &items[itemCount]) != 0) {
            return -1;
        }
        itemCount++;
    }
    return H2PBuildListNode(p, H2Ast_EXPR_LIST, items, itemCount, out);
}

static int H2PParseDirectiveLiteral(H2Parser* p, int32_t* out) {
    const H2Token* t = H2PPeek(p);
    int32_t        n;

    switch (t->kind) {
        case H2Tok_INT:
            p->pos++;
            n = H2PNewNode(p, H2Ast_INT, t->start, t->end);
            break;
        case H2Tok_FLOAT:
            p->pos++;
            n = H2PNewNode(p, H2Ast_FLOAT, t->start, t->end);
            break;
        case H2Tok_STRING:
            p->pos++;
            n = H2PNewNode(p, H2Ast_STRING, t->start, t->end);
            break;
        case H2Tok_RUNE:
            p->pos++;
            n = H2PNewNode(p, H2Ast_RUNE, t->start, t->end);
            break;
        case H2Tok_TRUE:
        case H2Tok_FALSE:
            p->pos++;
            n = H2PNewNode(p, H2Ast_BOOL, t->start, t->end);
            break;
        case H2Tok_NULL:
            p->pos++;
            n = H2PNewNode(p, H2Ast_NULL, t->start, t->end);
            break;
        default: return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
    }
    if (n < 0) {
        return -1;
    }
    if (p->nodes[n].kind != H2Ast_NULL) {
        p->nodes[n].dataStart = t->start;
        p->nodes[n].dataEnd = t->end;
    }
    *out = n;
    return 0;
}

static int H2PParseDirective(H2Parser* p, int32_t* out) {
    const H2Token* atTok = NULL;
    const H2Token* nameTok = NULL;
    const H2Token* rp = NULL;
    int32_t        directive;

    if (H2PExpect(p, H2Tok_AT, H2Diag_UNEXPECTED_TOKEN, &atTok) != 0) {
        return -1;
    }
    if (H2PExpect(p, H2Tok_IDENT, H2Diag_UNEXPECTED_TOKEN, &nameTok) != 0) {
        return -1;
    }
    directive = H2PNewNode(p, H2Ast_DIRECTIVE, atTok->start, nameTok->end);
    if (directive < 0) {
        return -1;
    }
    p->nodes[directive].dataStart = nameTok->start;
    p->nodes[directive].dataEnd = nameTok->end;

    if (H2PMatch(p, H2Tok_LPAREN)) {
        if (!H2PAt(p, H2Tok_RPAREN)) {
            for (;;) {
                int32_t argNode = -1;
                if (H2PParseDirectiveLiteral(p, &argNode) != 0) {
                    return -1;
                }
                if (H2PAddChild(p, directive, argNode) != 0) {
                    return -1;
                }
                if (!H2PMatch(p, H2Tok_COMMA)) {
                    break;
                }
                if (H2PAt(p, H2Tok_RPAREN)) {
                    break;
                }
            }
        }
        if (H2PExpect(p, H2Tok_RPAREN, H2Diag_UNEXPECTED_TOKEN, &rp) != 0) {
            return -1;
        }
        p->nodes[directive].end = rp->end;
    }
    *out = directive;
    return 0;
}

static int H2PParseDeclNameList(
    H2Parser*       p,
    int             allowHole,
    const H2Token** names,
    uint32_t        namesCap,
    uint32_t*       outNameCount,
    const H2Token** outFirstHole) {
    uint32_t       nameCount = 0;
    const H2Token* hole = NULL;
    if (H2PExpectDeclName(p, &names[nameCount], allowHole) != 0) {
        return -1;
    }
    if (hole == NULL && H2PIsHoleName(p, names[nameCount])) {
        hole = names[nameCount];
    }
    nameCount++;
    while (H2PMatch(p, H2Tok_COMMA)) {
        if (nameCount >= namesCap) {
            return H2PFail(p, H2Diag_ARENA_OOM);
        }
        if (!H2PAt(p, H2Tok_IDENT)) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
        if (H2PExpectDeclName(p, &names[nameCount], allowHole) != 0) {
            return -1;
        }
        if (hole == NULL && H2PIsHoleName(p, names[nameCount])) {
            hole = names[nameCount];
        }
        nameCount++;
    }
    *outNameCount = nameCount;
    *outFirstHole = hole;
    return 0;
}

static int H2PBuildNameListNode(
    H2Parser* p, const H2Token** names, uint32_t nameCount, int32_t* outNameList) {
    int32_t  items[256];
    uint32_t i;
    for (i = 0; i < nameCount; i++) {
        items[i] = H2PNewNode(p, H2Ast_IDENT, names[i]->start, names[i]->end);
        if (items[i] < 0) {
            return -1;
        }
        p->nodes[items[i]].dataStart = names[i]->start;
        p->nodes[items[i]].dataEnd = names[i]->end;
    }
    return H2PBuildListNode(p, H2Ast_NAME_LIST, items, nameCount, outNameList);
}

static int H2PFnHasVariadicParam(H2Parser* p, int32_t fnNode) {
    int32_t child = p->nodes[fnNode].firstChild;
    while (child >= 0) {
        if (p->nodes[child].kind == H2Ast_PARAM
            && (p->nodes[child].flags & H2AstFlag_PARAM_VARIADIC) != 0)
        {
            return 1;
        }
        child = p->nodes[child].nextSibling;
    }
    return 0;
}

static int H2PParseParamGroup(H2Parser* p, int32_t fnNode, int* outIsVariadic) {
    const H2Token* lastName = NULL;
    int32_t        firstParam = -1;
    int32_t        lastParam = -1;
    int32_t        type = -1;
    int            isVariadic = 0;
    int            isConstGroup = H2PMatch(p, H2Tok_CONST);
    int            hasExistingVariadic = H2PFnHasVariadicParam(p, fnNode);

    for (;;) {
        const H2Token* name;
        int32_t        param;
        if (H2PExpectDeclName(p, &name, 1) != 0) {
            return -1;
        }
        lastName = name;
        param = H2PNewNode(p, H2Ast_PARAM, name->start, name->end);
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

        if (!H2PMatch(p, H2Tok_COMMA)) {
            break;
        }
        if (isConstGroup) {
            return H2PFail(p, H2Diag_CONST_PARAM_GROUPED_NAME_INVALID);
        }
        if (!H2PAt(p, H2Tok_IDENT)) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
    }

    if (H2PMatch(p, H2Tok_ELLIPSIS)) {
        if (firstParam != lastParam) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
        if (hasExistingVariadic) {
            return H2PFail(p, H2Diag_VARIADIC_PARAM_DUPLICATE);
        }
        isVariadic = 1;
    } else if (hasExistingVariadic) {
        return H2PFail(p, H2Diag_VARIADIC_PARAM_NOT_LAST);
    }

    if (!H2PIsTypeStart(H2PPeek(p)->kind)) {
        if (lastName != NULL) {
            H2PSetDiagWithArg(
                p->diag,
                H2Diag_PARAM_MISSING_TYPE,
                lastName->start,
                lastName->end,
                lastName->start,
                lastName->end);
        } else {
            H2PSetDiag(p->diag, H2Diag_PARAM_MISSING_TYPE, H2PPeek(p)->start, H2PPeek(p)->end);
        }
        return -1;
    }

    if (H2PParseType(p, &type) != 0) {
        return -1;
    }

    {
        int32_t param = firstParam;
        while (param >= 0) {
            int32_t nextParam = p->nodes[param].nextSibling;
            int32_t typeNode = -1;
            if (param == firstParam) {
                typeNode = type;
            } else if (H2PCloneSubtree(p, type, &typeNode) != 0) {
                return -1;
            }
            if (H2PAddChild(p, param, typeNode) != 0) {
                return -1;
            }
            if (isVariadic) {
                p->nodes[param].flags |= H2AstFlag_PARAM_VARIADIC;
            }
            if (isConstGroup) {
                p->nodes[param].flags |= H2AstFlag_PARAM_CONST;
            }
            p->nodes[param].end = p->nodes[typeNode].end;
            param = nextParam;
        }
    }

    if (H2PAddChild(p, fnNode, firstParam) != 0) {
        return -1;
    }
    if (outIsVariadic != NULL) {
        *outIsVariadic = isVariadic;
    }
    return 0;
}

static int H2PParseBlock(H2Parser* p, int32_t* out) {
    const H2Token* lb;
    const H2Token* rb;
    int32_t        block;

    if (H2PExpect(p, H2Tok_LBRACE, H2Diag_UNEXPECTED_TOKEN, &lb) != 0) {
        return -1;
    }
    block = H2PNewNode(p, H2Ast_BLOCK, lb->start, lb->end);
    if (block < 0) {
        return -1;
    }

    while (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
        int32_t stmt = -1;
        if (H2PAt(p, H2Tok_SEMICOLON)) {
            p->pos++;
            continue;
        }
        if (H2PParseStmt(p, &stmt) != 0) {
            return -1;
        }
        if (stmt >= 0 && H2PAddChild(p, block, stmt) != 0) {
            return -1;
        }
    }

    if (H2PExpect(p, H2Tok_RBRACE, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[block].end = rb->end;
    *out = block;
    return 0;
}

/* Statement separators may omit trailing semicolon before closing `}`. */
static int H2PConsumeStmtTerminator(H2Parser* p, const H2Token** semiTok) {
    if (H2PAt(p, H2Tok_SEMICOLON)) {
        if (semiTok != NULL) {
            *semiTok = H2PPeek(p);
        }
        p->pos++;
        return 1;
    }
    if (H2PAt(p, H2Tok_RBRACE)) {
        if (semiTok != NULL) {
            *semiTok = NULL;
        }
        return 0;
    }
    return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
}

static int H2PParseVarLikeStmt(
    H2Parser* p, H2AstKind kind, int requireSemi, int allowHole, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    const H2Token* names[256];
    const H2Token* firstHole = NULL;
    uint32_t       stmtStart = kw->start;
    uint32_t       nameCount = 0;
    int32_t        n;
    int32_t        nameList = -1;
    int32_t        type = -1;
    int32_t        init = -1;

    p->pos++;
    if (H2PParseDeclNameList(
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
        if (!H2PMatch(p, H2Tok_ASSIGN)) {
            return H2PFailReservedName(p, firstHole);
        }
        if (H2PParseExpr(p, 1, &init) != 0) {
            return -1;
        }
        if (!requireSemi) {
            *out = init;
            return 0;
        }
        hasSemi = H2PConsumeStmtTerminator(p, &kw);
        if (hasSemi < 0) {
            return -1;
        }
        n = H2PNewNode(p, H2Ast_EXPR_STMT, stmtStart, hasSemi ? kw->end : p->nodes[init].end);
        if (n < 0) {
            return -1;
        }
        if (H2PAddChild(p, n, init) != 0) {
            return -1;
        }
        *out = n;
        return 0;
    }

    if (H2PMatch(p, H2Tok_ASSIGN)) {
        if (nameCount == 1u) {
            if (H2PParseExpr(p, 1, &init) != 0) {
                return -1;
            }
        } else {
            if (H2PParseExprList(p, &init) != 0) {
                return -1;
            }
        }
    } else {
        if (firstHole != NULL) {
            return H2PFailReservedName(p, firstHole);
        }
        if (H2PParseType(p, &type) != 0) {
            return -1;
        }
        if (H2PMatch(p, H2Tok_ASSIGN)) {
            if (nameCount == 1u) {
                if (H2PParseExpr(p, 1, &init) != 0) {
                    return -1;
                }
            } else if (H2PParseExprList(p, &init) != 0) {
                return -1;
            }
        }
    }

    n = H2PNewNode(
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
        if (H2PBuildNameListNode(p, names, nameCount, &nameList) != 0) {
            return -1;
        }
        if (H2PAddChild(p, n, nameList) != 0) {
            return -1;
        }
    }
    if (type >= 0) {
        if (H2PAddChild(p, n, type) != 0) {
            return -1;
        }
    }
    if (init >= 0) {
        if (H2PAddChild(p, n, init) != 0) {
            return -1;
        }
    }

    if (requireSemi) {
        int hasSemi = H2PConsumeStmtTerminator(p, &kw);
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

static int H2PIsShortAssignStart(H2Parser* p) {
    uint32_t i;
    if (!H2PAt(p, H2Tok_IDENT)) {
        return 0;
    }
    i = p->pos + 1u;
    while (
        i + 1u < p->tokLen && p->tok[i].kind == H2Tok_COMMA && p->tok[i + 1u].kind == H2Tok_IDENT)
    {
        i += 2u;
    }
    return i < p->tokLen && p->tok[i].kind == H2Tok_SHORT_ASSIGN;
}

static int H2PParseShortAssignStmt(H2Parser* p, int requireSemi, int32_t* out) {
    const H2Token* names[256];
    const H2Token* firstHole = NULL;
    const H2Token* semiTok = NULL;
    uint32_t       nameCount = 0;
    int32_t        nameList = -1;
    int32_t        rhsList = -1;
    int32_t        n;

    if (H2PParseDeclNameList(
            p, 1, names, (uint32_t)(sizeof(names) / sizeof(names[0])), &nameCount, &firstHole)
        != 0)
    {
        return -1;
    }
    (void)firstHole;
    if (H2PExpect(p, H2Tok_SHORT_ASSIGN, H2Diag_UNEXPECTED_TOKEN, &semiTok) != 0) {
        return -1;
    }
    if (H2PParseExprList(p, &rhsList) != 0) {
        return -1;
    }
    if (H2PBuildNameListNode(p, names, nameCount, &nameList) != 0) {
        return -1;
    }
    n = H2PNewNode(p, H2Ast_SHORT_ASSIGN, p->nodes[nameList].start, p->nodes[rhsList].end);
    if (n < 0) {
        return -1;
    }
    if (H2PAddChild(p, n, nameList) != 0 || H2PAddChild(p, n, rhsList) != 0) {
        return -1;
    }
    if (requireSemi) {
        int hasSemi = H2PConsumeStmtTerminator(p, &semiTok);
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

static int H2PParseIfStmt(H2Parser* p, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    int32_t        n;
    int32_t        cond;
    int32_t        thenBlock;

    p->pos++;
    if (H2PParseExpr(p, 1, &cond) != 0) {
        return -1;
    }
    if (H2PParseBlock(p, &thenBlock) != 0) {
        return -1;
    }

    n = H2PNewNode(p, H2Ast_IF, kw->start, p->nodes[thenBlock].end);
    if (n < 0) {
        return -1;
    }
    if (H2PAddChild(p, n, cond) != 0 || H2PAddChild(p, n, thenBlock) != 0) {
        return -1;
    }

    if (H2PMatch(p, H2Tok_SEMICOLON) && H2PAt(p, H2Tok_ELSE)) {
        /* Allow newline between `}` and `else`. */
    } else if (p->pos > 0 && H2PPrev(p)->kind == H2Tok_SEMICOLON && !H2PAt(p, H2Tok_ELSE)) {
        p->pos--;
    }

    if (H2PMatch(p, H2Tok_ELSE)) {
        int32_t elseNode;
        if (H2PAt(p, H2Tok_IF)) {
            if (H2PParseIfStmt(p, &elseNode) != 0) {
                return -1;
            }
        } else {
            if (H2PParseBlock(p, &elseNode) != 0) {
                return -1;
            }
        }
        if (H2PAddChild(p, n, elseNode) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[elseNode].end;
    }

    *out = n;
    return 0;
}

static int H2PForStmtHeadHasInToken(H2Parser* p) {
    uint32_t i = p->pos;
    while (i < p->tokLen) {
        H2TokenKind k = p->tok[i].kind;
        if (k == H2Tok_IN) {
            return 1;
        }
        if (k == H2Tok_LBRACE || k == H2Tok_SEMICOLON || k == H2Tok_EOF) {
            break;
        }
        i++;
    }
    return 0;
}

static int H2PNewIdentNodeFromToken(H2Parser* p, const H2Token* tok, int32_t* out) {
    int32_t n = H2PNewNode(p, H2Ast_IDENT, tok->start, tok->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = tok->start;
    p->nodes[n].dataEnd = tok->end;
    *out = n;
    return 0;
}

static int H2PParseForInKeyBinding(H2Parser* p, int32_t* outIdent, uint32_t* outFlags) {
    const H2Token* name = NULL;
    int            keyRef = 0;
    if (H2PMatch(p, H2Tok_AND)) {
        keyRef = 1;
    }
    if (H2PExpectDeclName(p, &name, 0) != 0) {
        return -1;
    }
    if (H2PNewIdentNodeFromToken(p, name, outIdent) != 0) {
        return -1;
    }
    if (keyRef) {
        *outFlags |= H2AstFlag_FOR_IN_KEY_REF;
    }
    return 0;
}

static int H2PParseForInValueBinding(H2Parser* p, int32_t* outIdent, uint32_t* outFlags) {
    const H2Token* name = NULL;
    int            byRef = 0;
    if (H2PMatch(p, H2Tok_AND)) {
        byRef = 1;
    }
    if (H2PExpectDeclName(p, &name, 1) != 0) {
        return -1;
    }
    if (H2PIsHoleName(p, name)) {
        if (byRef) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
        *outFlags |= H2AstFlag_FOR_IN_VALUE_DISCARD;
    } else if (byRef) {
        *outFlags |= H2AstFlag_FOR_IN_VALUE_REF;
    }
    return H2PNewIdentNodeFromToken(p, name, outIdent);
}

static int H2PParseForInClause(
    H2Parser* p,
    int32_t*  outKeyBinding,
    int32_t*  outValueBinding,
    int32_t*  outSourceExpr,
    int32_t*  outBody,
    uint32_t* outFlags) {
    uint32_t savedPos = p->pos;
    uint32_t savedNodeLen = p->nodeLen;
    H2Diag   savedDiag = { 0 };
    int32_t  keyBinding = -1;
    int32_t  valueBinding = -1;
    int32_t  sourceExpr = -1;
    uint32_t forFlags = H2AstFlag_FOR_IN;
    int32_t  body = -1;

    if (p->diag != NULL) {
        savedDiag = *p->diag;
    }

    if (H2PParseForInKeyBinding(p, &keyBinding, &forFlags) == 0 && H2PMatch(p, H2Tok_COMMA)) {
        forFlags |= H2AstFlag_FOR_IN_HAS_KEY;
        if (H2PParseForInValueBinding(p, &valueBinding, &forFlags) != 0) {
            return -1;
        }
        if (!H2PMatch(p, H2Tok_IN)) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
        if (H2PParseExpr(p, 1, &sourceExpr) != 0) {
            return -1;
        }
        if (H2PParseBlock(p, &body) != 0) {
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
    forFlags = H2AstFlag_FOR_IN;
    if (H2PParseForInValueBinding(p, &valueBinding, &forFlags) != 0) {
        return -1;
    }
    if (!H2PMatch(p, H2Tok_IN)) {
        return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
    }
    if (H2PParseExpr(p, 1, &sourceExpr) != 0) {
        return -1;
    }
    if (H2PParseBlock(p, &body) != 0) {
        return -1;
    }
    *outKeyBinding = -1;
    *outValueBinding = valueBinding;
    *outSourceExpr = sourceExpr;
    *outBody = body;
    *outFlags = forFlags;
    return 0;
}

static int H2PParseForStmt(H2Parser* p, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    int32_t        n;
    int32_t        body;
    int32_t        init = -1;
    int32_t        cond = -1;
    int32_t        post = -1;
    int32_t        keyBinding = -1;
    int32_t        valueBinding = -1;
    int32_t        sourceExpr = -1;
    uint32_t       forInFlags = 0;
    int            isForIn = 0;

    p->pos++;
    n = H2PNewNode(p, H2Ast_FOR, kw->start, kw->end);
    if (n < 0) {
        return -1;
    }

    if (H2PAt(p, H2Tok_LBRACE)) {
        if (H2PParseBlock(p, &body) != 0) {
            return -1;
        }
    } else if (H2PForStmtHeadHasInToken(p)) {
        if (H2PParseForInClause(p, &keyBinding, &valueBinding, &sourceExpr, &body, &forInFlags)
            != 0)
        {
            return -1;
        }
        isForIn = 1;
        p->nodes[n].flags |= forInFlags;
    } else {
        if (H2PAt(p, H2Tok_SEMICOLON)) {
            p->pos++;
        } else if (H2PAt(p, H2Tok_VAR)) {
            if (H2PParseVarLikeStmt(p, H2Ast_VAR, 0, 1, &init) != 0) {
                return -1;
            }
        } else if (H2PIsShortAssignStart(p)) {
            if (H2PParseShortAssignStmt(p, 0, &init) != 0) {
                return -1;
            }
        } else {
            if (H2PParseExpr(p, 1, &init) != 0) {
                return -1;
            }
        }

        if (H2PMatch(p, H2Tok_SEMICOLON)) {
            if (!H2PAt(p, H2Tok_SEMICOLON)) {
                if (H2PParseExpr(p, 1, &cond) != 0) {
                    return -1;
                }
            }
            if (!H2PMatch(p, H2Tok_SEMICOLON)) {
                return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
            }
            if (!H2PAt(p, H2Tok_LBRACE)) {
                if (H2PParseExpr(p, 1, &post) != 0) {
                    return -1;
                }
            }
        } else {
            cond = init;
            init = -1;
        }

        if (H2PParseBlock(p, &body) != 0) {
            return -1;
        }
    }

    if (isForIn) {
        if (keyBinding >= 0 && H2PAddChild(p, n, keyBinding) != 0) {
            return -1;
        }
        if (valueBinding >= 0 && H2PAddChild(p, n, valueBinding) != 0) {
            return -1;
        }
        if (sourceExpr >= 0 && H2PAddChild(p, n, sourceExpr) != 0) {
            return -1;
        }
        if (H2PAddChild(p, n, body) != 0) {
            return -1;
        }
    } else {
        if (init >= 0 && H2PAddChild(p, n, init) != 0) {
            return -1;
        }
        if (cond >= 0 && H2PAddChild(p, n, cond) != 0) {
            return -1;
        }
        if (post >= 0 && H2PAddChild(p, n, post) != 0) {
            return -1;
        }
        if (H2PAddChild(p, n, body) != 0) {
            return -1;
        }
    }
    p->nodes[n].end = p->nodes[body].end;
    *out = n;
    return 0;
}

static int H2PParseSwitchStmt(H2Parser* p, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    const H2Token* rb;
    int32_t        sw;
    int            sawDefault = 0;

    p->pos++;
    sw = H2PNewNode(p, H2Ast_SWITCH, kw->start, kw->end);
    if (sw < 0) {
        return -1;
    }

    // Expression switch: switch <expr> { ... }
    // Condition switch:  switch { ... }
    if (!H2PAt(p, H2Tok_LBRACE)) {
        int32_t subject;
        if (H2PParseExpr(p, 1, &subject) != 0) {
            return -1;
        }
        if (H2PAddChild(p, sw, subject) != 0) {
            return -1;
        }
        p->nodes[sw].flags = 1;
    }

    if (H2PExpect(p, H2Tok_LBRACE, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }

    while (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
        if (H2PMatch(p, H2Tok_SEMICOLON)) {
            continue;
        }

        if (H2PMatch(p, H2Tok_CASE)) {
            int32_t caseNode = H2PNewNode(p, H2Ast_CASE, H2PPrev(p)->start, H2PPrev(p)->end);
            int32_t body;
            if (caseNode < 0) {
                return -1;
            }

            for (;;) {
                uint32_t savedPos = p->pos;
                uint32_t savedNodeLen = p->nodeLen;
                H2Diag   savedDiag = { 0 };
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
                if (H2PAt(p, H2Tok_IDENT)) {
                    const H2Token* firstSeg = NULL;
                    const H2Token* seg = NULL;
                    int            sawDot = 0;
                    if (H2PExpect(p, H2Tok_IDENT, H2Diag_EXPECTED_EXPR, &firstSeg) == 0) {
                        patternExpr = H2PNewNode(p, H2Ast_IDENT, firstSeg->start, firstSeg->end);
                        if (patternExpr < 0) {
                            return -1;
                        }
                        p->nodes[patternExpr].dataStart = firstSeg->start;
                        p->nodes[patternExpr].dataEnd = firstSeg->end;
                        while (H2PMatch(p, H2Tok_DOT)) {
                            int32_t fieldExpr;
                            if (H2PExpect(p, H2Tok_IDENT, H2Diag_EXPECTED_EXPR, &seg) != 0) {
                                goto parse_case_label_fallback;
                            }
                            sawDot = 1;
                            fieldExpr = H2PNewNode(
                                p, H2Ast_FIELD_EXPR, p->nodes[patternExpr].start, seg->end);
                            if (fieldExpr < 0) {
                                return -1;
                            }
                            if (H2PAddChild(p, fieldExpr, patternExpr) != 0) {
                                return -1;
                            }
                            p->nodes[fieldExpr].dataStart = seg->start;
                            p->nodes[fieldExpr].dataEnd = seg->end;
                            patternExpr = fieldExpr;
                        }
                        if (sawDot) {
                            if (H2PMatch(p, H2Tok_AS)) {
                                const H2Token* aliasTok;
                                if (H2PExpectDeclName(p, &aliasTok, 0) != 0) {
                                    goto parse_case_label_fallback;
                                }
                                aliasNode = H2PNewNode(
                                    p, H2Ast_IDENT, aliasTok->start, aliasTok->end);
                                if (aliasNode < 0) {
                                    return -1;
                                }
                                p->nodes[aliasNode].dataStart = aliasTok->start;
                                p->nodes[aliasNode].dataEnd = aliasTok->end;
                            }
                            if (H2PAt(p, H2Tok_COMMA) || H2PAt(p, H2Tok_LBRACE)) {
                                patternNode = H2PNewNode(
                                    p,
                                    H2Ast_CASE_PATTERN,
                                    p->nodes[patternExpr].start,
                                    aliasNode >= 0 ? p->nodes[aliasNode].end
                                                   : p->nodes[patternExpr].end);
                                if (patternNode < 0) {
                                    return -1;
                                }
                                if (H2PAddChild(p, patternNode, patternExpr) != 0) {
                                    return -1;
                                }
                                if (aliasNode >= 0 && H2PAddChild(p, patternNode, aliasNode) != 0) {
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
                    if (H2PParseExpr(p, 1, &patternExpr) != 0) {
                        return -1;
                    }
                    patternNode = H2PNewNode(
                        p,
                        H2Ast_CASE_PATTERN,
                        p->nodes[patternExpr].start,
                        p->nodes[patternExpr].end);
                    if (patternNode < 0) {
                        return -1;
                    }
                    if (H2PAddChild(p, patternNode, patternExpr) != 0) {
                        return -1;
                    }
                }

                if (H2PAddChild(p, caseNode, patternNode) != 0) {
                    return -1;
                }
                if (!H2PMatch(p, H2Tok_COMMA)) {
                    break;
                }
            }

            if (H2PParseBlock(p, &body) != 0) {
                return -1;
            }
            if (H2PAddChild(p, caseNode, body) != 0) {
                return -1;
            }
            p->nodes[caseNode].end = p->nodes[body].end;
            if (H2PAddChild(p, sw, caseNode) != 0) {
                return -1;
            }
            continue;
        }

        if (H2PMatch(p, H2Tok_DEFAULT)) {
            int32_t defNode;
            int32_t body;
            if (sawDefault) {
                return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
            }
            sawDefault = 1;
            defNode = H2PNewNode(p, H2Ast_DEFAULT, H2PPrev(p)->start, H2PPrev(p)->end);
            if (defNode < 0) {
                return -1;
            }
            if (H2PParseBlock(p, &body) != 0) {
                return -1;
            }
            if (H2PAddChild(p, defNode, body) != 0) {
                return -1;
            }
            p->nodes[defNode].end = p->nodes[body].end;
            if (H2PAddChild(p, sw, defNode) != 0) {
                return -1;
            }
            continue;
        }

        return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
    }

    if (H2PExpect(p, H2Tok_RBRACE, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[sw].end = rb->end;
    *out = sw;
    return 0;
}

static int H2PParseStmt(H2Parser* p, int32_t* out) {
    const H2Token* kw;
    int32_t        n;
    int32_t        expr;
    int32_t        block;

    switch (H2PPeek(p)->kind) {
        case H2Tok_VAR: return H2PParseVarLikeStmt(p, H2Ast_VAR, 1, 1, out);
        case H2Tok_CONST:
            kw = H2PPeek(p);
            p->pos++;
            if (H2PAt(p, H2Tok_LBRACE)) {
                n = H2PNewNode(p, H2Ast_CONST_BLOCK, kw->start, kw->end);
                if (n < 0) {
                    return -1;
                }
                if (H2PParseBlock(p, &block) != 0) {
                    return -1;
                }
                if (H2PAddChild(p, n, block) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[block].end;
                *out = n;
                return 0;
            }
            p->pos--;
            return H2PParseVarLikeStmt(p, H2Ast_CONST, 1, 1, out);
        case H2Tok_IF:     return H2PParseIfStmt(p, out);
        case H2Tok_FOR:    return H2PParseForStmt(p, out);
        case H2Tok_SWITCH: return H2PParseSwitchStmt(p, out);
        case H2Tok_RETURN:
            kw = H2PPeek(p);
            p->pos++;
            n = H2PNewNode(p, H2Ast_RETURN, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (!H2PAt(p, H2Tok_SEMICOLON)) {
                if (H2PParseExpr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (H2PMatch(p, H2Tok_COMMA)) {
                    int32_t  items[256];
                    uint32_t itemCount = 0;
                    int32_t  exprList;
                    items[itemCount++] = expr;
                    for (;;) {
                        if (itemCount >= (uint32_t)(sizeof(items) / sizeof(items[0]))) {
                            return H2PFail(p, H2Diag_ARENA_OOM);
                        }
                        if (H2PParseExpr(p, 1, &items[itemCount]) != 0) {
                            return -1;
                        }
                        itemCount++;
                        if (!H2PMatch(p, H2Tok_COMMA)) {
                            break;
                        }
                    }
                    if (H2PBuildListNode(p, H2Ast_EXPR_LIST, items, itemCount, &exprList) != 0) {
                        return -1;
                    }
                    expr = exprList;
                }
                if (H2PAddChild(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[expr].end;
            }
            {
                int hasSemi = H2PConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case H2Tok_BREAK:
            kw = H2PPeek(p);
            p->pos++;
            n = H2PNewNode(p, H2Ast_BREAK, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            {
                int hasSemi = H2PConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case H2Tok_CONTINUE:
            kw = H2PPeek(p);
            p->pos++;
            n = H2PNewNode(p, H2Ast_CONTINUE, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            {
                int hasSemi = H2PConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case H2Tok_DEFER:
            kw = H2PPeek(p);
            p->pos++;
            n = H2PNewNode(p, H2Ast_DEFER, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (H2PAt(p, H2Tok_LBRACE)) {
                if (H2PParseBlock(p, &block) != 0) {
                    return -1;
                }
            } else {
                if (H2PParseStmt(p, &block) != 0) {
                    return -1;
                }
            }
            if (H2PAddChild(p, n, block) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[block].end;
            *out = n;
            return 0;
        case H2Tok_ASSERT:
            kw = H2PPeek(p);
            p->pos++;
            n = H2PNewNode(p, H2Ast_ASSERT, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (H2PParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (H2PAddChild(p, n, expr) != 0) {
                return -1;
            }
            while (H2PMatch(p, H2Tok_COMMA)) {
                int32_t arg;
                if (H2PParseExpr(p, 1, &arg) != 0) {
                    return -1;
                }
                if (H2PAddChild(p, n, arg) != 0) {
                    return -1;
                }
            }
            {
                int hasSemi = H2PConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case H2Tok_DEL:
            kw = H2PPeek(p);
            p->pos++;
            n = H2PNewNode(p, H2Ast_DEL, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            for (;;) {
                if (H2PParseExpr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (H2PAddChild(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[expr].end;
                if (!H2PMatch(p, H2Tok_COMMA)) {
                    break;
                }
            }
            if (H2PMatch(p, H2Tok_IN)) {
                if (H2PParseExpr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (H2PAddChild(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].flags |= H2AstFlag_DEL_HAS_ALLOC;
                p->nodes[n].end = p->nodes[expr].end;
            }
            {
                int hasSemi = H2PConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
            }
            *out = n;
            return 0;
        case H2Tok_LBRACE: return H2PParseBlock(p, out);
        default:           {
            int hasSemi;
            if (H2PIsShortAssignStart(p)) {
                return H2PParseShortAssignStmt(p, 1, out);
            }
            if (H2PParseExpr(p, 1, &expr) != 0) {
                return -1;
            }
            if (H2PMatch(p, H2Tok_COMMA)) {
                int32_t  lhsExprs[256];
                int32_t  rhsExprs[256];
                uint32_t lhsCount = 0;
                uint32_t rhsCount = 0;
                int32_t  lhsList;
                int32_t  rhsList;
                lhsExprs[lhsCount++] = expr;
                if (H2PParseExpr(p, 2, &lhsExprs[lhsCount]) != 0) {
                    return -1;
                }
                lhsCount++;
                while (H2PMatch(p, H2Tok_COMMA)) {
                    if (lhsCount >= (uint32_t)(sizeof(lhsExprs) / sizeof(lhsExprs[0]))) {
                        return H2PFail(p, H2Diag_ARENA_OOM);
                    }
                    if (H2PParseExpr(p, 2, &lhsExprs[lhsCount]) != 0) {
                        return -1;
                    }
                    lhsCount++;
                }
                if (!H2PMatch(p, H2Tok_ASSIGN)) {
                    return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
                }
                if (H2PParseExpr(p, 1, &rhsExprs[rhsCount]) != 0) {
                    return -1;
                }
                rhsCount++;
                while (H2PMatch(p, H2Tok_COMMA)) {
                    if (rhsCount >= (uint32_t)(sizeof(rhsExprs) / sizeof(rhsExprs[0]))) {
                        return H2PFail(p, H2Diag_ARENA_OOM);
                    }
                    if (H2PParseExpr(p, 1, &rhsExprs[rhsCount]) != 0) {
                        return -1;
                    }
                    rhsCount++;
                }
                if (H2PBuildListNode(p, H2Ast_EXPR_LIST, lhsExprs, lhsCount, &lhsList) != 0
                    || H2PBuildListNode(p, H2Ast_EXPR_LIST, rhsExprs, rhsCount, &rhsList) != 0)
                {
                    return -1;
                }
                n = H2PNewNode(
                    p, H2Ast_MULTI_ASSIGN, p->nodes[lhsList].start, p->nodes[rhsList].end);
                if (n < 0) {
                    return -1;
                }
                if (H2PAddChild(p, n, lhsList) != 0 || H2PAddChild(p, n, rhsList) != 0) {
                    return -1;
                }
                hasSemi = H2PConsumeStmtTerminator(p, &kw);
                if (hasSemi < 0) {
                    return -1;
                }
                if (hasSemi) {
                    p->nodes[n].end = kw->end;
                }
                *out = n;
                return 0;
            }
            hasSemi = H2PConsumeStmtTerminator(p, &kw);
            if (hasSemi < 0) {
                return -1;
            }
            n = H2PNewNode(
                p, H2Ast_EXPR_STMT, p->nodes[expr].start, hasSemi ? kw->end : p->nodes[expr].end);
            if (n < 0) {
                return -1;
            }
            if (H2PAddChild(p, n, expr) != 0) {
                return -1;
            }
            *out = n;
            return 0;
        }
    }
}

static int H2PParseFieldList(H2Parser* p, int32_t agg) {
    while (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
        const H2Token* names[256];
        uint32_t       nameCount = 0;
        const H2Token* embeddedTypeName = NULL;
        int32_t        type = -1;
        int32_t        defaultExpr = -1;
        uint32_t       i;
        int            isEmbedded = 0;
        if (H2PAt(p, H2Tok_SEMICOLON) || H2PAt(p, H2Tok_COMMA)) {
            p->pos++;
            continue;
        }
        if (H2PAnonymousFieldLookahead(p, &embeddedTypeName)
            && !(
                p->pos + 2u < p->tokLen && p->tok[p->pos].kind == H2Tok_IDENT
                && p->tok[p->pos + 1u].kind == H2Tok_COMMA
                && p->tok[p->pos + 2u].kind == H2Tok_IDENT))
        {
            if (H2PParseTypeName(p, &type) != 0) {
                return -1;
            }
            isEmbedded = 1;
            nameCount = 1;
        } else {
            if (H2PExpectDeclName(p, &names[nameCount], 0) != 0) {
                return -1;
            }
            nameCount++;
            while (H2PMatch(p, H2Tok_COMMA)) {
                if (!H2PAt(p, H2Tok_IDENT)) {
                    return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
                }
                if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                    return H2PFail(p, H2Diag_ARENA_OOM);
                }
                if (H2PExpectDeclName(p, &names[nameCount], 0) != 0) {
                    return -1;
                }
                nameCount++;
            }
            if (H2PParseType(p, &type) != 0) {
                return -1;
            }
        }
        if (H2PMatch(p, H2Tok_ASSIGN)) {
            if (!isEmbedded && nameCount > 1) {
                const H2Token* eq = H2PPrev(p);
                H2PSetDiag(p->diag, H2Diag_UNEXPECTED_TOKEN, eq->start, eq->end);
                return -1;
            }
            if (H2PParseExpr(p, 1, &defaultExpr) != 0) {
                return -1;
            }
        }
        for (i = 0; i < nameCount; i++) {
            int32_t field;
            int32_t fieldType;
            if (isEmbedded) {
                fieldType = type;
                if (i != 0) {
                    return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
                }
            } else if (i == 0) {
                fieldType = type;
            } else if (H2PCloneSubtree(p, type, &fieldType) != 0) {
                return -1;
            }
            field = H2PNewNode(
                p,
                H2Ast_FIELD,
                isEmbedded ? p->nodes[fieldType].start : names[i]->start,
                p->nodes[fieldType].end);
            if (field < 0) {
                return -1;
            }
            if (isEmbedded) {
                p->nodes[field].dataStart = embeddedTypeName->start;
                p->nodes[field].dataEnd = embeddedTypeName->end;
                p->nodes[field].flags |= H2AstFlag_FIELD_EMBEDDED;
            } else {
                p->nodes[field].dataStart = names[i]->start;
                p->nodes[field].dataEnd = names[i]->end;
            }
            if (H2PAddChild(p, field, fieldType) != 0) {
                return -1;
            }
            if (i == 0 && defaultExpr >= 0) {
                p->nodes[field].end = p->nodes[defaultExpr].end;
                if (H2PAddChild(p, field, defaultExpr) != 0) {
                    return -1;
                }
            }
            if (H2PAddChild(p, agg, field) != 0) {
                return -1;
            }
        }
        if (H2PMatch(p, H2Tok_SEMICOLON) || H2PMatch(p, H2Tok_COMMA)) {
            continue;
        }
        if (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
    }
    return 0;
}

static int H2PParseAggregateMemberList(H2Parser* p, int32_t agg) {
    while (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
        if (H2PAt(p, H2Tok_SEMICOLON) || H2PAt(p, H2Tok_COMMA)) {
            p->pos++;
            continue;
        }

        if (H2PAt(p, H2Tok_STRUCT) || H2PAt(p, H2Tok_UNION) || H2PAt(p, H2Tok_ENUM)
            || H2PAt(p, H2Tok_TYPE))
        {
            int32_t declNode = -1;
            if (H2PAt(p, H2Tok_TYPE)) {
                if (H2PParseTypeAliasDecl(p, &declNode) != 0) {
                    return -1;
                }
            } else {
                if (H2PParseAggregateDecl(p, &declNode) != 0) {
                    return -1;
                }
            }
            if (H2PAddChild(p, agg, declNode) != 0) {
                return -1;
            }
            if (H2PMatch(p, H2Tok_SEMICOLON) || H2PMatch(p, H2Tok_COMMA)) {
                continue;
            }
            if (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
                return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
            }
            continue;
        }

        if (H2PAt(p, H2Tok_FN) || H2PAt(p, H2Tok_CONST) || H2PAt(p, H2Tok_VAR)
            || H2PAt(p, H2Tok_PUB))
        {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }

        {
            const H2Token* names[256];
            uint32_t       nameCount = 0;
            const H2Token* embeddedTypeName = NULL;
            int32_t        type = -1;
            int32_t        defaultExpr = -1;
            uint32_t       i;
            int            isEmbedded = 0;

            if (H2PAnonymousFieldLookahead(p, &embeddedTypeName)
                && !(
                    p->pos + 2u < p->tokLen && p->tok[p->pos].kind == H2Tok_IDENT
                    && p->tok[p->pos + 1u].kind == H2Tok_COMMA
                    && p->tok[p->pos + 2u].kind == H2Tok_IDENT))
            {
                if (H2PParseTypeName(p, &type) != 0) {
                    return -1;
                }
                isEmbedded = 1;
                nameCount = 1;
            } else {
                if (H2PExpectDeclName(p, &names[nameCount], 0) != 0) {
                    return -1;
                }
                nameCount++;
                while (H2PMatch(p, H2Tok_COMMA)) {
                    if (!H2PAt(p, H2Tok_IDENT)) {
                        return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
                    }
                    if (nameCount >= (uint32_t)(sizeof(names) / sizeof(names[0]))) {
                        return H2PFail(p, H2Diag_ARENA_OOM);
                    }
                    if (H2PExpectDeclName(p, &names[nameCount], 0) != 0) {
                        return -1;
                    }
                    nameCount++;
                }
                if (H2PParseType(p, &type) != 0) {
                    return -1;
                }
            }

            if (H2PMatch(p, H2Tok_ASSIGN)) {
                if (!isEmbedded && nameCount > 1) {
                    const H2Token* eq = H2PPrev(p);
                    H2PSetDiag(p->diag, H2Diag_UNEXPECTED_TOKEN, eq->start, eq->end);
                    return -1;
                }
                if (H2PParseExpr(p, 1, &defaultExpr) != 0) {
                    return -1;
                }
            }

            for (i = 0; i < nameCount; i++) {
                int32_t field;
                int32_t fieldType;
                if (isEmbedded) {
                    fieldType = type;
                    if (i != 0) {
                        return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
                    }
                } else if (i == 0) {
                    fieldType = type;
                } else if (H2PCloneSubtree(p, type, &fieldType) != 0) {
                    return -1;
                }
                field = H2PNewNode(
                    p,
                    H2Ast_FIELD,
                    isEmbedded ? p->nodes[fieldType].start : names[i]->start,
                    p->nodes[fieldType].end);
                if (field < 0) {
                    return -1;
                }
                if (isEmbedded) {
                    p->nodes[field].dataStart = embeddedTypeName->start;
                    p->nodes[field].dataEnd = embeddedTypeName->end;
                    p->nodes[field].flags |= H2AstFlag_FIELD_EMBEDDED;
                } else {
                    p->nodes[field].dataStart = names[i]->start;
                    p->nodes[field].dataEnd = names[i]->end;
                }
                if (H2PAddChild(p, field, fieldType) != 0) {
                    return -1;
                }
                if (i == 0 && defaultExpr >= 0) {
                    p->nodes[field].end = p->nodes[defaultExpr].end;
                    if (H2PAddChild(p, field, defaultExpr) != 0) {
                        return -1;
                    }
                }
                if (H2PAddChild(p, agg, field) != 0) {
                    return -1;
                }
            }
        }
        if (H2PMatch(p, H2Tok_SEMICOLON) || H2PMatch(p, H2Tok_COMMA)) {
            continue;
        }
        if (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
    }
    return 0;
}

static int H2PParseAggregateDecl(H2Parser* p, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    const H2Token* name;
    const H2Token* rb;
    H2AstKind      kind = H2Ast_STRUCT;
    int32_t        n;

    if (kw->kind == H2Tok_UNION) {
        kind = H2Ast_UNION;
    } else if (kw->kind == H2Tok_ENUM) {
        kind = H2Ast_ENUM;
    }

    p->pos++;
    if (H2PExpectDeclName(p, &name, 0) != 0) {
        return -1;
    }
    n = H2PNewNode(p, kind, kw->start, name->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;
    if (H2PParseTypeParamList(p, n) != 0) {
        return -1;
    }

    if (kw->kind == H2Tok_ENUM) {
        int32_t underType;
        if (H2PParseType(p, &underType) != 0) {
            return -1;
        }
        if (H2PAddChild(p, n, underType) != 0) {
            return -1;
        }
    }

    if (H2PExpect(p, H2Tok_LBRACE, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    if (kw->kind == H2Tok_ENUM) {
        while (!H2PAt(p, H2Tok_RBRACE) && !H2PAt(p, H2Tok_EOF)) {
            const H2Token* itemName;
            int32_t        item;
            if (H2PAt(p, H2Tok_COMMA) || H2PAt(p, H2Tok_SEMICOLON)) {
                p->pos++;
                continue;
            }
            if (H2PExpectDeclName(p, &itemName, 0) != 0) {
                return -1;
            }
            item = H2PNewNode(p, H2Ast_FIELD, itemName->start, itemName->end);
            if (item < 0) {
                return -1;
            }
            p->nodes[item].dataStart = itemName->start;
            p->nodes[item].dataEnd = itemName->end;
            if (H2PMatch(p, H2Tok_LBRACE)) {
                if (H2PParseFieldList(p, item) != 0) {
                    return -1;
                }
                if (H2PExpect(p, H2Tok_RBRACE, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
                    return -1;
                }
                p->nodes[item].end = rb->end;
            }
            if (H2PMatch(p, H2Tok_ASSIGN)) {
                int32_t vexpr;
                if (H2PParseExpr(p, 1, &vexpr) != 0) {
                    return -1;
                }
                if (H2PAddChild(p, item, vexpr) != 0) {
                    return -1;
                }
                p->nodes[item].end = p->nodes[vexpr].end;
            }
            if (H2PAddChild(p, n, item) != 0) {
                return -1;
            }
            if (H2PMatch(p, H2Tok_COMMA) || H2PMatch(p, H2Tok_SEMICOLON)) {
                continue;
            }
        }
    } else {
        if (H2PParseAggregateMemberList(p, n) != 0) {
            return -1;
        }
    }

    if (H2PExpect(p, H2Tok_RBRACE, H2Diag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[n].end = rb->end;
    *out = n;
    return 0;
}

static int H2PParseFunDecl(H2Parser* p, int allowBody, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    const H2Token* name;
    const H2Token* t;
    int32_t        fn;

    p->pos++;
    if (H2PExpectFnName(p, &name) != 0) {
        return -1;
    }
    fn = H2PNewNode(p, H2Ast_FN, kw->start, name->end);
    if (fn < 0) {
        return -1;
    }
    p->nodes[fn].dataStart = name->start;
    p->nodes[fn].dataEnd = name->end;
    if (H2PParseTypeParamList(p, fn) != 0) {
        return -1;
    }
    if (H2PExpect(p, H2Tok_LPAREN, H2Diag_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }

    if (!H2PAt(p, H2Tok_RPAREN)) {
        for (;;) {
            int isVariadic = 0;
            if (H2PParseParamGroup(p, fn, &isVariadic) != 0) {
                return -1;
            }
            if (!H2PMatch(p, H2Tok_COMMA)) {
                break;
            }
        }
    }

    if (H2PExpect(p, H2Tok_RPAREN, H2Diag_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }
    p->nodes[fn].end = t->end;

    if (!H2PAt(p, H2Tok_LBRACE) && !H2PAt(p, H2Tok_SEMICOLON)) {
        if (H2PAt(p, H2Tok_LPAREN)) {
            if (H2PParseFnResultClause(p, fn) != 0) {
                return -1;
            }
        } else {
            int32_t retType;
            if (H2PParseType(p, &retType) != 0) {
                return -1;
            }
            p->nodes[retType].flags = 1;
            if (H2PAddChild(p, fn, retType) != 0) {
                return -1;
            }
            p->nodes[fn].end = p->nodes[retType].end;
        }
    }

    if (H2PAt(p, H2Tok_LBRACE)) {
        int32_t body;
        if (!allowBody) {
            return H2PFail(p, H2Diag_UNEXPECTED_TOKEN);
        }
        if (H2PParseBlock(p, &body) != 0) {
            return -1;
        }
        if (H2PAddChild(p, fn, body) != 0) {
            return -1;
        }
        p->nodes[fn].end = p->nodes[body].end;
    } else {
        if (H2PExpect(p, H2Tok_SEMICOLON, H2Diag_UNEXPECTED_TOKEN, &t) != 0) {
            return -1;
        }
        p->nodes[fn].end = t->end;
    }

    *out = fn;
    return 0;
}

static int H2PParseTypeAliasDecl(H2Parser* p, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    const H2Token* name;
    const H2Token* semi;
    int32_t        n;
    int32_t        targetType;

    p->pos++;
    if (H2PExpectDeclName(p, &name, 0) != 0) {
        return -1;
    }
    n = H2PNewNode(p, H2Ast_TYPE_ALIAS, kw->start, name->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = name->start;
    p->nodes[n].dataEnd = name->end;
    if (H2PParseTypeParamList(p, n) != 0) {
        return -1;
    }
    if (H2PParseType(p, &targetType) != 0) {
        return -1;
    }
    p->nodes[n].end = p->nodes[targetType].end;
    if (H2PAddChild(p, n, targetType) != 0) {
        return -1;
    }

    if (H2PExpect(p, H2Tok_SEMICOLON, H2Diag_UNEXPECTED_TOKEN, &semi) != 0) {
        return -1;
    }
    p->nodes[n].end = semi->end;
    *out = n;
    return 0;
}

static int H2PParseImport(H2Parser* p, int32_t* out) {
    const H2Token* kw = H2PPeek(p);
    const H2Token* path;
    const H2Token* alias = NULL;
    int32_t        n;
    p->pos++;

    if (H2PExpect(p, H2Tok_STRING, H2Diag_UNEXPECTED_TOKEN, &path) != 0) {
        return -1;
    }

    n = H2PNewNode(p, H2Ast_IMPORT, kw->start, path->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].dataStart = path->start;
    p->nodes[n].dataEnd = path->end;

    if (H2PMatch(p, H2Tok_AS)) {
        int32_t aliasNode;
        if (H2PExpect(p, H2Tok_IDENT, H2Diag_UNEXPECTED_TOKEN, &alias) != 0) {
            return -1;
        }
        if (H2PReservedName(p, alias)) {
            H2PSetDiag(p->diag, H2Diag_RESERVED_HOP_PREFIX, alias->start, alias->end);
            return -1;
        }
        aliasNode = H2PNewNode(p, H2Ast_IDENT, alias->start, alias->end);
        if (aliasNode < 0) {
            return -1;
        }
        p->nodes[aliasNode].dataStart = alias->start;
        p->nodes[aliasNode].dataEnd = alias->end;
        if (H2PAddChild(p, n, aliasNode) != 0) {
            return -1;
        }
    }

    if (H2PMatch(p, H2Tok_LBRACE)) {
        if (!H2PAt(p, H2Tok_RBRACE)) {
            for (;;) {
                const H2Token* symName = NULL;
                const H2Token* symAlias = NULL;
                int32_t        symNode;

                if (H2PAt(p, H2Tok_MUL)) {
                    const H2Token* starTok = H2PPeek(p);
                    H2PSetDiag(
                        p->diag,
                        H2Diag_IMPORT_WILDCARD_NOT_SUPPORTED,
                        starTok->start,
                        starTok->end);
                    return -1;
                }
                if (H2PExpect(p, H2Tok_IDENT, H2Diag_UNEXPECTED_TOKEN, &symName) != 0) {
                    return -1;
                }
                symNode = H2PNewNode(p, H2Ast_IMPORT_SYMBOL, symName->start, symName->end);
                if (symNode < 0) {
                    return -1;
                }
                p->nodes[symNode].dataStart = symName->start;
                p->nodes[symNode].dataEnd = symName->end;

                if (H2PMatch(p, H2Tok_AS)) {
                    int32_t symAliasNode;
                    if (H2PExpect(p, H2Tok_IDENT, H2Diag_UNEXPECTED_TOKEN, &symAlias) != 0) {
                        return -1;
                    }
                    if (H2PReservedName(p, symAlias)) {
                        H2PSetDiag(
                            p->diag, H2Diag_RESERVED_HOP_PREFIX, symAlias->start, symAlias->end);
                        return -1;
                    }
                    symAliasNode = H2PNewNode(p, H2Ast_IDENT, symAlias->start, symAlias->end);
                    if (symAliasNode < 0) {
                        return -1;
                    }
                    p->nodes[symAliasNode].dataStart = symAlias->start;
                    p->nodes[symAliasNode].dataEnd = symAlias->end;
                    if (H2PAddChild(p, symNode, symAliasNode) != 0) {
                        return -1;
                    }
                    p->nodes[symNode].end = symAlias->end;
                }

                if (H2PAddChild(p, n, symNode) != 0) {
                    return -1;
                }

                if (!H2PMatch(p, H2Tok_COMMA) && !H2PMatch(p, H2Tok_SEMICOLON)) {
                    break;
                }
                while (H2PMatch(p, H2Tok_COMMA) || H2PMatch(p, H2Tok_SEMICOLON)) {}
                if (H2PAt(p, H2Tok_RBRACE)) {
                    break;
                }
            }
        }
        if (H2PExpect(p, H2Tok_RBRACE, H2Diag_UNEXPECTED_TOKEN, &kw) != 0) {
            return -1;
        }
        p->nodes[n].end = kw->end;
    }

    /* Detect feature imports and set feature flags using decoded import path bytes. */
    {
        H2StringLitErr litErr = { 0 };
        uint8_t*       decoded = NULL;
        uint32_t       decodedLen = 0;
        const uint8_t* name = NULL;
        uint32_t       nameLen = 0;
        if (H2DecodeStringLiteralArena(
                p->arena, p->src.ptr, path->start, path->end, &decoded, &decodedLen, &litErr)
            != 0)
        {
            H2PSetDiag(p->diag, H2StringLitErrDiagCode(litErr.kind), litErr.start, litErr.end);
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
                p->features |= H2Feature_OPTIONAL;
            }
            /* Unknown feature names: silently ignored here; CLI layer warns later. */
        }
    }

    if (H2PExpect(p, H2Tok_SEMICOLON, H2Diag_UNEXPECTED_TOKEN, &kw) != 0) {
        return -1;
    }
    p->nodes[n].end = kw->end;
    *out = n;
    return 0;
}

static int H2PParseDeclInner(H2Parser* p, int allowBody, int32_t* out) {
    switch (H2PPeek(p)->kind) {
        case H2Tok_FN:   return H2PParseFunDecl(p, allowBody, out);
        case H2Tok_TYPE: return H2PParseTypeAliasDecl(p, out);
        case H2Tok_STRUCT:
        case H2Tok_UNION:
        case H2Tok_ENUM:
            if (H2PParseAggregateDecl(p, out) != 0) {
                return -1;
            }
            if (H2PMatch(p, H2Tok_SEMICOLON)) {
                p->nodes[*out].end = H2PPrev(p)->end;
            }
            return 0;
        case H2Tok_VAR:   return H2PParseVarLikeStmt(p, H2Ast_VAR, 1, 0, out);
        case H2Tok_CONST: return H2PParseVarLikeStmt(p, H2Ast_CONST, 1, 0, out);
        default:          return H2PFail(p, H2Diag_EXPECTED_DECL);
    }
}

static int H2PParseDecl(H2Parser* p, int allowBody, int32_t* out) {
    int      isPub = 0;
    uint32_t pubStart = 0;
    if (H2PMatch(p, H2Tok_PUB)) {
        isPub = 1;
        pubStart = H2PPrev(p)->start;
    }
    if (H2PParseDeclInner(p, allowBody, out) != 0) {
        return -1;
    }
    if (isPub) {
        p->nodes[*out].start = pubStart;
        p->nodes[*out].flags |= H2AstFlag_PUB;
    }
    return 0;
}

const char* H2AstKindName(H2AstKind kind) {
    switch (kind) {
        case H2Ast_FILE:              return "FILE";
        case H2Ast_IMPORT:            return "IMPORT";
        case H2Ast_IMPORT_SYMBOL:     return "IMPORT_SYMBOL";
        case H2Ast_DIRECTIVE:         return "DIRECTIVE";
        case H2Ast_PUB:               return "PUB";
        case H2Ast_FN:                return "FN";
        case H2Ast_PARAM:             return "PARAM";
        case H2Ast_TYPE_PARAM:        return "TYPE_PARAM";
        case H2Ast_CONTEXT_CLAUSE:    return "CONTEXT_CLAUSE";
        case H2Ast_TYPE_NAME:         return "TYPE_NAME";
        case H2Ast_TYPE_PTR:          return "TYPE_PTR";
        case H2Ast_TYPE_REF:          return "TYPE_REF";
        case H2Ast_TYPE_MUTREF:       return "TYPE_MUTREF";
        case H2Ast_TYPE_ARRAY:        return "TYPE_ARRAY";
        case H2Ast_TYPE_VARRAY:       return "TYPE_VARRAY";
        case H2Ast_TYPE_SLICE:        return "TYPE_SLICE";
        case H2Ast_TYPE_MUTSLICE:     return "TYPE_MUTSLICE";
        case H2Ast_TYPE_OPTIONAL:     return "TYPE_OPTIONAL";
        case H2Ast_TYPE_FN:           return "TYPE_FN";
        case H2Ast_TYPE_ALIAS:        return "TYPE_ALIAS";
        case H2Ast_TYPE_ANON_STRUCT:  return "TYPE_ANON_STRUCT";
        case H2Ast_TYPE_ANON_UNION:   return "TYPE_ANON_UNION";
        case H2Ast_TYPE_TUPLE:        return "TYPE_TUPLE";
        case H2Ast_STRUCT:            return "STRUCT";
        case H2Ast_UNION:             return "UNION";
        case H2Ast_ENUM:              return "ENUM";
        case H2Ast_FIELD:             return "FIELD";
        case H2Ast_BLOCK:             return "BLOCK";
        case H2Ast_VAR:               return "VAR";
        case H2Ast_CONST:             return "CONST";
        case H2Ast_CONST_BLOCK:       return "CONST_BLOCK";
        case H2Ast_IF:                return "IF";
        case H2Ast_FOR:               return "FOR";
        case H2Ast_SWITCH:            return "SWITCH";
        case H2Ast_CASE:              return "CASE";
        case H2Ast_CASE_PATTERN:      return "CASE_PATTERN";
        case H2Ast_DEFAULT:           return "DEFAULT";
        case H2Ast_RETURN:            return "RETURN";
        case H2Ast_BREAK:             return "BREAK";
        case H2Ast_CONTINUE:          return "CONTINUE";
        case H2Ast_DEFER:             return "DEFER";
        case H2Ast_ASSERT:            return "ASSERT";
        case H2Ast_DEL:               return "DEL";
        case H2Ast_EXPR_STMT:         return "EXPR_STMT";
        case H2Ast_MULTI_ASSIGN:      return "MULTI_ASSIGN";
        case H2Ast_SHORT_ASSIGN:      return "SHORT_ASSIGN";
        case H2Ast_NAME_LIST:         return "NAME_LIST";
        case H2Ast_EXPR_LIST:         return "EXPR_LIST";
        case H2Ast_TUPLE_EXPR:        return "TUPLE_EXPR";
        case H2Ast_TYPE_VALUE:        return "TYPE_VALUE";
        case H2Ast_IDENT:             return "IDENT";
        case H2Ast_INT:               return "INT";
        case H2Ast_FLOAT:             return "FLOAT";
        case H2Ast_STRING:            return "STRING";
        case H2Ast_RUNE:              return "RUNE";
        case H2Ast_BOOL:              return "BOOL";
        case H2Ast_UNARY:             return "UNARY";
        case H2Ast_BINARY:            return "BINARY";
        case H2Ast_CALL:              return "CALL";
        case H2Ast_CALL_ARG:          return "CALL_ARG";
        case H2Ast_CALL_WITH_CONTEXT: return "CALL_WITH_CONTEXT";
        case H2Ast_CONTEXT_OVERLAY:   return "CONTEXT_OVERLAY";
        case H2Ast_CONTEXT_BIND:      return "CONTEXT_BIND";
        case H2Ast_COMPOUND_LIT:      return "COMPOUND_LIT";
        case H2Ast_COMPOUND_FIELD:    return "COMPOUND_FIELD";
        case H2Ast_ARRAY_LIT:         return "ARRAY_LIT";
        case H2Ast_INDEX:             return "INDEX";
        case H2Ast_FIELD_EXPR:        return "FIELD_EXPR";
        case H2Ast_CAST:              return "CAST";
        case H2Ast_SIZEOF:            return "SIZEOF";
        case H2Ast_NEW:               return "NEW";
        case H2Ast_NULL:              return "NULL";
        case H2Ast_UNWRAP:            return "UNWRAP";
    }
    return "UNKNOWN";
}

static int H2PIsSpaceButNotNewline(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v';
}

static int H2PHasCodeOnLineBefore(const char* src, uint32_t lineStart, uint32_t pos) {
    uint32_t i;
    for (i = lineStart; i < pos; i++) {
        char c = src[i];
        if (!H2PIsSpaceButNotNewline(c)) {
            return 1;
        }
    }
    return 0;
}

static uint32_t H2PFindLineStart(const char* src, uint32_t pos) {
    while (pos > 0) {
        if (src[pos - 1] == '\n') {
            break;
        }
        pos--;
    }
    return pos;
}

static int32_t H2PFindPrevNodeByEnd(const H2Ast* ast, uint32_t pos) {
    int32_t  best = -1;
    uint32_t bestEnd = 0;
    uint32_t bestSpan = 0;
    uint32_t i;
    for (i = 0; i < ast->len; i++) {
        const H2AstNode* n;
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

static int32_t H2PFindNextNodeByStart(const H2Ast* ast, uint32_t pos) {
    int32_t  best = -1;
    uint32_t bestStart = 0;
    uint32_t bestSpan = 0;
    uint32_t i;
    for (i = 0; i < ast->len; i++) {
        const H2AstNode* n;
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

static int32_t H2PFindContainerNode(const H2Ast* ast, uint32_t pos) {
    int32_t nodeId = ast->root;
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    for (;;) {
        int32_t  bestChild = -1;
        int32_t  child = ast->nodes[nodeId].firstChild;
        uint32_t bestSpan = 0;
        while (child >= 0) {
            const H2AstNode* n = &ast->nodes[child];
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

static int H2PNextCommentRange(
    H2StrView src, uint32_t* ioPos, uint32_t* outStart, uint32_t* outEnd) {
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

static int H2PCollectFormattingData(
    H2Arena*       arena,
    H2StrView      src,
    const H2Ast*   ast,
    H2ParseExtras* outExtras,
    H2Diag* _Nullable diag) {
    H2Comment* comments;
    uint32_t   count = 0;
    uint32_t   pos = 0;
    for (;;) {
        uint32_t start;
        uint32_t end;
        if (!H2PNextCommentRange(src, &pos, &start, &end)) {
            break;
        }
        count++;
    }
    if (count == 0) {
        outExtras->comments = NULL;
        outExtras->commentLen = 0;
        return 0;
    }

    comments = (H2Comment*)H2ArenaAlloc(
        arena, count * (uint32_t)sizeof(H2Comment), (uint32_t)_Alignof(H2Comment));
    if (comments == NULL) {
        H2PSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
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
        if (!H2PNextCommentRange(src, &pos, &start, &end)) {
            break;
        }
        lineStart = H2PFindLineStart(src.ptr, start);
        prevNode = H2PFindPrevNodeByEnd(ast, start);
        nextNode = H2PFindNextNodeByStart(ast, end);
        containerNode = H2PFindContainerNode(ast, start);

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
        comments[count].attachment = H2CommentAttachment_FLOATING;
        comments[count]._reserved[0] = 0;
        comments[count]._reserved[1] = 0;
        comments[count]._reserved[2] = 0;

        if (H2PHasCodeOnLineBefore(src.ptr, lineStart, start)) {
            comments[count].attachment = H2CommentAttachment_TRAILING;
            comments[count].anchorNode = prevNode >= 0 ? prevNode : containerNode;
        } else if (nextNode >= 0) {
            comments[count].attachment = H2CommentAttachment_LEADING;
            comments[count].anchorNode = nextNode;
        } else if (prevNode >= 0) {
            comments[count].attachment = H2CommentAttachment_TRAILING;
            comments[count].anchorNode = prevNode;
        } else {
            comments[count].attachment = H2CommentAttachment_FLOATING;
            comments[count].anchorNode = containerNode;
        }
        count++;
    }

    outExtras->comments = comments;
    outExtras->commentLen = count;
    return 0;
}

int H2Parse(
    H2Arena*  arena,
    H2StrView src,
    const H2ParseOptions* _Nullable options,
    H2Ast* out,
    H2ParseExtras* _Nullable outExtras,
    H2Diag* _Nullable diag) {
    H2TokenStream ts;
    H2Parser      p;
    int32_t       root;
    uint32_t      parseFlags = options != NULL ? options->flags : 0;

    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (outExtras != NULL) {
        outExtras->comments = NULL;
        outExtras->commentLen = 0;
    }
    out->nodes = NULL;
    out->len = 0;
    out->root = -1;
    out->features = H2Feature_NONE;

    if (H2Lex(arena, src, &ts, diag) != 0) {
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
    p.features = H2Feature_NONE;
    p.nodes = (H2AstNode*)H2ArenaAlloc(
        arena, p.nodeCap * (uint32_t)sizeof(H2AstNode), (uint32_t)_Alignof(H2AstNode));
    if (p.nodes == NULL) {
        H2PSetDiag(diag, H2Diag_ARENA_OOM, 0, 0);
        return -1;
    }

    root = H2PNewNode(&p, H2Ast_FILE, 0, src.len);
    if (root < 0) {
        return -1;
    }

    while (H2PAt(&p, H2Tok_IMPORT)) {
        int32_t imp;
        if (H2PParseImport(&p, &imp) != 0) {
            return -1;
        }
        if (H2PAddChild(&p, root, imp) != 0) {
            return -1;
        }
    }

    while (!H2PAt(&p, H2Tok_EOF)) {
        int32_t decl;
        if (H2PAt(&p, H2Tok_SEMICOLON)) {
            p.pos++;
            continue;
        }
        for (;;) {
            if (H2PAt(&p, H2Tok_SEMICOLON)) {
                p.pos++;
                continue;
            }
            if (H2PAt(&p, H2Tok_AT)) {
                int32_t directive;
                if (H2PParseDirective(&p, &directive) != 0) {
                    return -1;
                }
                if (H2PAddChild(&p, root, directive) != 0) {
                    return -1;
                }
                continue;
            }
            break;
        }
        if (H2PParseDecl(&p, 1, &decl) != 0) {
            return -1;
        }
        if (H2PAddChild(&p, root, decl) != 0) {
            return -1;
        }
    }

    out->nodes = p.nodes;
    out->len = p.nodeLen;
    out->root = root;
    out->features = p.features;
    if ((parseFlags & H2ParseFlag_COLLECT_FORMATTING) != 0 && outExtras != NULL
        && H2PCollectFormattingData(arena, src, out, outExtras, diag) != 0)
    {
        return -1;
    }
    return 0;
}

H2_API_END
