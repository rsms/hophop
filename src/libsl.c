#include "libsl.h"

typedef struct {
    SLToken* v;
    uint32_t len;
    uint32_t cap;
} SLTokenBuf;

static void SLSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->start = start;
    diag->end = end;
}

void SLDiagClear(SLDiag* diag) {
    if (diag == NULL) {
        return;
    }
    diag->code = SLDiag_NONE;
    diag->start = 0;
    diag->end = 0;
}

const char* SLDiagMessage(SLDiagCode code) {
    switch (code) {
        case SLDiag_NONE:                return "no error";
        case SLDiag_ARENA_OOM:           return "arena out of memory";
        case SLDiag_UNEXPECTED_CHAR:     return "unexpected character";
        case SLDiag_UNTERMINATED_STRING: return "unterminated string literal";
        case SLDiag_INVALID_NUMBER:      return "invalid number literal";
        case SLDiag_UNEXPECTED_TOKEN:    return "unexpected token";
        case SLDiag_EXPECTED_DECL:       return "expected declaration";
        case SLDiag_EXPECTED_EXPR:       return "expected expression";
        case SLDiag_EXPECTED_TYPE:       return "expected type";
        case SLDiag_DUPLICATE_SYMBOL:    return "duplicate symbol";
        case SLDiag_UNKNOWN_SYMBOL:      return "unknown symbol";
        case SLDiag_UNKNOWN_TYPE:        return "unknown type";
        case SLDiag_TYPE_MISMATCH:       return "type mismatch";
        case SLDiag_ARITY_MISMATCH:      return "call arity mismatch";
        case SLDiag_NOT_CALLABLE:        return "expression is not callable";
        case SLDiag_EXPECTED_BOOL:       return "expected bool expression";
    }
    return "unknown diagnostic";
}

void SLArenaInit(SLArena* arena, void* storage, uint32_t storageSize) {
    arena->mem = (uint8_t*)storage;
    arena->cap = storageSize;
    arena->len = 0;
}

void SLArenaReset(SLArena* arena) {
    arena->len = 0;
}

void* _Nullable SLArenaAlloc(SLArena* arena, uint32_t size, uint32_t align) {
    uint64_t aligned;
    uint64_t end;

    if (align == 0) {
        align = 1;
    }
    if ((align & (align - 1u)) != 0) {
        return NULL;
    }

    aligned = ((uint64_t)arena->len + (uint64_t)(align - 1u)) & ~((uint64_t)align - 1u);
    end = aligned + (uint64_t)size;
    if (end > (uint64_t)arena->cap) {
        return NULL;
    }

    arena->len = (uint32_t)end;
    return arena->mem + aligned;
}

static int SLIsAlpha(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

static int SLIsDigit(unsigned char c) {
    return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int SLIsAlnum(unsigned char c) {
    return SLIsAlpha(c) || SLIsDigit(c);
}

static int SLIsHexDigit(unsigned char c) {
    return SLIsDigit(c) || (c >= (unsigned char)'a' && c <= (unsigned char)'f')
        || (c >= (unsigned char)'A' && c <= (unsigned char)'F');
}

static int SLStrEq(const char* a, uint32_t aLen, const char* b) {
    uint32_t i = 0;
    while (i < aLen) {
        if (b[i] == '\0' || a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return b[i] == '\0';
}

static SLTokenKind SLKeywordKind(const char* s, uint32_t len) {
    if (len == 2) {
        if (SLStrEq(s, len, "as")) {
            return SLTok_AS;
        }
        if (SLStrEq(s, len, "fn")) {
            return SLTok_FN;
        }
        if (SLStrEq(s, len, "if")) {
            return SLTok_IF;
        }
    } else if (len == 3) {
        if (SLStrEq(s, len, "for")) {
            return SLTok_FOR;
        }
        if (SLStrEq(s, len, "var")) {
            return SLTok_VAR;
        }
        if (SLStrEq(s, len, "pub")) {
            return SLTok_PUB;
        }
    } else if (len == 4) {
        if (SLStrEq(s, len, "enum")) {
            return SLTok_ENUM;
        }
        if (SLStrEq(s, len, "else")) {
            return SLTok_ELSE;
        }
        if (SLStrEq(s, len, "case")) {
            return SLTok_CASE;
        }
        if (SLStrEq(s, len, "true")) {
            return SLTok_TRUE;
        }
    } else if (len == 5) {
        if (SLStrEq(s, len, "break")) {
            return SLTok_BREAK;
        }
        if (SLStrEq(s, len, "const")) {
            return SLTok_CONST;
        }
        if (SLStrEq(s, len, "defer")) {
            return SLTok_DEFER;
        }
        if (SLStrEq(s, len, "false")) {
            return SLTok_FALSE;
        }
        if (SLStrEq(s, len, "union")) {
            return SLTok_UNION;
        }
        if (SLStrEq(s, len, "assert")) {
            return SLTok_ASSERT;
        }
    } else if (len == 6) {
        if (SLStrEq(s, len, "import")) {
            return SLTok_IMPORT;
        }
        if (SLStrEq(s, len, "return")) {
            return SLTok_RETURN;
        }
        if (SLStrEq(s, len, "switch")) {
            return SLTok_SWITCH;
        }
        if (SLStrEq(s, len, "struct")) {
            return SLTok_STRUCT;
        }
    } else if (len == 7) {
        if (SLStrEq(s, len, "package")) {
            return SLTok_PACKAGE;
        }
        if (SLStrEq(s, len, "default")) {
            return SLTok_DEFAULT;
        }
    } else if (len == 8) {
        if (SLStrEq(s, len, "continue")) {
            return SLTok_CONTINUE;
        }
    }
    return SLTok_IDENT;
}

static int SLTokenCanEndStmt(SLTokenKind kind) {
    switch (kind) {
        case SLTok_IDENT:
        case SLTok_INT:
        case SLTok_FLOAT:
        case SLTok_STRING:
        case SLTok_TRUE:
        case SLTok_FALSE:
        case SLTok_BREAK:
        case SLTok_CONTINUE:
        case SLTok_RETURN:
        case SLTok_RPAREN:
        case SLTok_RBRACK:
        case SLTok_RBRACE:   return 1;
        default:             return 0;
    }
}

static int SLPushToken(
    SLTokenBuf* out, SLDiag* diag, SLTokenKind kind, uint32_t start, uint32_t end) {
    if (out->len >= out->cap) {
        SLSetDiag(diag, SLDiag_ARENA_OOM, start, end);
        return -1;
    }

    out->v[out->len].kind = kind;
    out->v[out->len].start = start;
    out->v[out->len].end = end;
    out->len++;
    return 0;
}

const char* SLTokenKindName(SLTokenKind kind) {
    switch (kind) {
        case SLTok_INVALID:       return "INVALID";
        case SLTok_EOF:           return "EOF";
        case SLTok_IDENT:         return "IDENT";
        case SLTok_INT:           return "INT";
        case SLTok_FLOAT:         return "FLOAT";
        case SLTok_STRING:        return "STRING";
        case SLTok_PACKAGE:       return "PACKAGE";
        case SLTok_IMPORT:        return "IMPORT";
        case SLTok_PUB:           return "PUB";
        case SLTok_STRUCT:        return "STRUCT";
        case SLTok_UNION:         return "UNION";
        case SLTok_ENUM:          return "ENUM";
        case SLTok_FN:            return "FN";
        case SLTok_VAR:           return "VAR";
        case SLTok_CONST:         return "CONST";
        case SLTok_IF:            return "IF";
        case SLTok_ELSE:          return "ELSE";
        case SLTok_FOR:           return "FOR";
        case SLTok_SWITCH:        return "SWITCH";
        case SLTok_CASE:          return "CASE";
        case SLTok_DEFAULT:       return "DEFAULT";
        case SLTok_BREAK:         return "BREAK";
        case SLTok_CONTINUE:      return "CONTINUE";
        case SLTok_RETURN:        return "RETURN";
        case SLTok_DEFER:         return "DEFER";
        case SLTok_ASSERT:        return "ASSERT";
        case SLTok_TRUE:          return "TRUE";
        case SLTok_FALSE:         return "FALSE";
        case SLTok_AS:            return "AS";
        case SLTok_LPAREN:        return "LPAREN";
        case SLTok_RPAREN:        return "RPAREN";
        case SLTok_LBRACE:        return "LBRACE";
        case SLTok_RBRACE:        return "RBRACE";
        case SLTok_LBRACK:        return "LBRACK";
        case SLTok_RBRACK:        return "RBRACK";
        case SLTok_COMMA:         return "COMMA";
        case SLTok_DOT:           return "DOT";
        case SLTok_SEMICOLON:     return "SEMICOLON";
        case SLTok_COLON:         return "COLON";
        case SLTok_ASSIGN:        return "ASSIGN";
        case SLTok_ADD:           return "ADD";
        case SLTok_SUB:           return "SUB";
        case SLTok_MUL:           return "MUL";
        case SLTok_DIV:           return "DIV";
        case SLTok_MOD:           return "MOD";
        case SLTok_AND:           return "AND";
        case SLTok_OR:            return "OR";
        case SLTok_XOR:           return "XOR";
        case SLTok_NOT:           return "NOT";
        case SLTok_LSHIFT:        return "LSHIFT";
        case SLTok_RSHIFT:        return "RSHIFT";
        case SLTok_EQ:            return "EQ";
        case SLTok_NEQ:           return "NEQ";
        case SLTok_LT:            return "LT";
        case SLTok_GT:            return "GT";
        case SLTok_LTE:           return "LTE";
        case SLTok_GTE:           return "GTE";
        case SLTok_LOGICAL_AND:   return "LOGICAL_AND";
        case SLTok_LOGICAL_OR:    return "LOGICAL_OR";
        case SLTok_ADD_ASSIGN:    return "ADD_ASSIGN";
        case SLTok_SUB_ASSIGN:    return "SUB_ASSIGN";
        case SLTok_MUL_ASSIGN:    return "MUL_ASSIGN";
        case SLTok_DIV_ASSIGN:    return "DIV_ASSIGN";
        case SLTok_MOD_ASSIGN:    return "MOD_ASSIGN";
        case SLTok_AND_ASSIGN:    return "AND_ASSIGN";
        case SLTok_OR_ASSIGN:     return "OR_ASSIGN";
        case SLTok_XOR_ASSIGN:    return "XOR_ASSIGN";
        case SLTok_LSHIFT_ASSIGN: return "LSHIFT_ASSIGN";
        case SLTok_RSHIFT_ASSIGN: return "RSHIFT_ASSIGN";
    }
    return "UNKNOWN";
}

int SLLex(SLArena* arena, SLStrView src, SLTokenStream* out, SLDiag* diag) {
    SLTokenBuf  tokbuf;
    uint32_t    pos = 0;
    int         insertedEOFSemicolon = 0;
    SLTokenKind prevKind = SLTok_INVALID;

    SLDiagClear(diag);
    out->v = NULL;
    out->len = 0;

    tokbuf.len = 0;
    tokbuf.cap = src.len + 2;
    if (tokbuf.cap < 8) {
        tokbuf.cap = 8;
    }
    tokbuf.v = (SLToken*)SLArenaAlloc(
        arena, tokbuf.cap * (uint32_t)sizeof(SLToken), (uint32_t)_Alignof(SLToken));
    if (tokbuf.v == NULL) {
        SLSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    for (;;) {
        int      sawNewline = 0;
        uint32_t newlinePos = 0;

        for (;;) {
            unsigned char c;
            if (pos >= src.len) {
                break;
            }

            c = (unsigned char)src.ptr[pos];
            if (c == (unsigned char)' ' || c == (unsigned char)'\t' || c == (unsigned char)'\r'
                || c == (unsigned char)'\f' || c == (unsigned char)'\v')
            {
                pos++;
                continue;
            }

            if (c == (unsigned char)'\n') {
                if (!sawNewline) {
                    sawNewline = 1;
                    newlinePos = pos;
                }
                pos++;
                continue;
            }

            if (c == (unsigned char)'/' && pos + 1 < src.len
                && (unsigned char)src.ptr[pos + 1] == (unsigned char)'/')
            {
                pos += 2;
                while (pos < src.len && (unsigned char)src.ptr[pos] != (unsigned char)'\n') {
                    pos++;
                }
                continue;
            }
            break;
        }

        if (sawNewline && SLTokenCanEndStmt(prevKind)) {
            if (SLPushToken(&tokbuf, diag, SLTok_SEMICOLON, newlinePos, newlinePos) != 0) {
                return -1;
            }
            prevKind = SLTok_SEMICOLON;
            continue;
        }

        if (pos >= src.len) {
            if (!insertedEOFSemicolon && SLTokenCanEndStmt(prevKind)) {
                if (SLPushToken(&tokbuf, diag, SLTok_SEMICOLON, src.len, src.len) != 0) {
                    return -1;
                }
                prevKind = SLTok_SEMICOLON;
                insertedEOFSemicolon = 1;
                continue;
            }
            if (SLPushToken(&tokbuf, diag, SLTok_EOF, src.len, src.len) != 0) {
                return -1;
            }
            break;
        }

        {
            SLTokenKind   kind = SLTok_INVALID;
            uint32_t      start = pos;
            unsigned char c = (unsigned char)src.ptr[pos];

            if (SLIsAlpha(c) || c == (unsigned char)'_') {
                pos++;
                while (pos < src.len) {
                    c = (unsigned char)src.ptr[pos];
                    if (!SLIsAlnum(c) && c != (unsigned char)'_') {
                        break;
                    }
                    pos++;
                }
                kind = SLKeywordKind(src.ptr + start, pos - start);
            } else if (SLIsDigit(c)) {
                kind = SLTok_INT;

                if (c == (unsigned char)'0' && pos + 1 < src.len
                    && ((unsigned char)src.ptr[pos + 1] == (unsigned char)'x'
                        || (unsigned char)src.ptr[pos + 1] == (unsigned char)'X'))
                {
                    uint32_t digitsStart;
                    pos += 2;
                    digitsStart = pos;
                    while (pos < src.len && SLIsHexDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }
                    if (pos == digitsStart) {
                        SLSetDiag(diag, SLDiag_INVALID_NUMBER, start, pos);
                        return -1;
                    }
                } else {
                    while (pos < src.len && SLIsDigit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }

                    if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.') {
                        kind = SLTok_FLOAT;
                        pos++;
                        while (pos < src.len && SLIsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                    }

                    if (pos < src.len
                        && ((unsigned char)src.ptr[pos] == (unsigned char)'e'
                            || (unsigned char)src.ptr[pos] == (unsigned char)'E'))
                    {
                        uint32_t expStart;
                        kind = SLTok_FLOAT;
                        pos++;
                        if (pos < src.len
                            && ((unsigned char)src.ptr[pos] == (unsigned char)'+'
                                || (unsigned char)src.ptr[pos] == (unsigned char)'-'))
                        {
                            pos++;
                        }
                        expStart = pos;
                        while (pos < src.len && SLIsDigit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                        if (pos == expStart) {
                            SLSetDiag(diag, SLDiag_INVALID_NUMBER, start, pos);
                            return -1;
                        }
                    }
                }
            } else if (c == (unsigned char)'"') {
                kind = SLTok_STRING;
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
                            SLSetDiag(diag, SLDiag_UNTERMINATED_STRING, start, pos);
                            return -1;
                        }
                        pos++;
                        continue;
                    }
                    if (c == (unsigned char)'\n' || c == (unsigned char)'\r') {
                        SLSetDiag(diag, SLDiag_UNTERMINATED_STRING, start, pos);
                        return -1;
                    }
                    pos++;
                }
                if (pos > src.len || src.ptr[pos - 1] != '"') {
                    SLSetDiag(diag, SLDiag_UNTERMINATED_STRING, start, pos);
                    return -1;
                }
            } else {
                pos++;
                switch (c) {
                    case (unsigned char)'(': kind = SLTok_LPAREN; break;
                    case (unsigned char)')': kind = SLTok_RPAREN; break;
                    case (unsigned char)'{': kind = SLTok_LBRACE; break;
                    case (unsigned char)'}': kind = SLTok_RBRACE; break;
                    case (unsigned char)'[': kind = SLTok_LBRACK; break;
                    case (unsigned char)']': kind = SLTok_RBRACK; break;
                    case (unsigned char)',': kind = SLTok_COMMA; break;
                    case (unsigned char)'.': kind = SLTok_DOT; break;
                    case (unsigned char)';': kind = SLTok_SEMICOLON; break;
                    case (unsigned char)':': kind = SLTok_COLON; break;

                    case (unsigned char)'+':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_ADD_ASSIGN;
                        } else {
                            kind = SLTok_ADD;
                        }
                        break;
                    case (unsigned char)'-':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_SUB_ASSIGN;
                        } else {
                            kind = SLTok_SUB;
                        }
                        break;
                    case (unsigned char)'*':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_MUL_ASSIGN;
                        } else {
                            kind = SLTok_MUL;
                        }
                        break;
                    case (unsigned char)'/':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_DIV_ASSIGN;
                        } else {
                            kind = SLTok_DIV;
                        }
                        break;
                    case (unsigned char)'%':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_MOD_ASSIGN;
                        } else {
                            kind = SLTok_MOD;
                        }
                        break;
                    case (unsigned char)'&':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'&') {
                            pos++;
                            kind = SLTok_LOGICAL_AND;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_AND_ASSIGN;
                        } else {
                            kind = SLTok_AND;
                        }
                        break;
                    case (unsigned char)'|':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'|') {
                            pos++;
                            kind = SLTok_LOGICAL_OR;
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_OR_ASSIGN;
                        } else {
                            kind = SLTok_OR;
                        }
                        break;
                    case (unsigned char)'^':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_XOR_ASSIGN;
                        } else {
                            kind = SLTok_XOR;
                        }
                        break;
                    case (unsigned char)'!':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_NEQ;
                        } else {
                            kind = SLTok_NOT;
                        }
                        break;
                    case (unsigned char)'=':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SLTok_EQ;
                        } else {
                            kind = SLTok_ASSIGN;
                        }
                        break;
                    case (unsigned char)'<':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'<') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = SLTok_LSHIFT_ASSIGN;
                            } else {
                                kind = SLTok_LSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_LTE;
                        } else {
                            kind = SLTok_LT;
                        }
                        break;
                    case (unsigned char)'>':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'>') {
                            pos++;
                            if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                            {
                                pos++;
                                kind = SLTok_RSHIFT_ASSIGN;
                            } else {
                                kind = SLTok_RSHIFT;
                            }
                        } else if (
                            pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=')
                        {
                            pos++;
                            kind = SLTok_GTE;
                        } else {
                            kind = SLTok_GT;
                        }
                        break;

                    default: SLSetDiag(diag, SLDiag_UNEXPECTED_CHAR, start, pos); return -1;
                }
            }

            if (SLPushToken(&tokbuf, diag, kind, start, pos) != 0) {
                return -1;
            }
            prevKind = kind;
        }
    }

    out->v = tokbuf.v;
    out->len = tokbuf.len;
    return 0;
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
    SLSetDiag(p->diag, code, t->start, t->end);
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
        SLSetDiag(p->diag, SLDiag_ARENA_OOM, start, end);
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

static int SLPParseTypeName(SLParser* p, int32_t* out) {
    const SLToken* first;
    const SLToken* last;
    int32_t        n;

    if (SLPExpect(p, SLTok_IDENT, SLDiag_EXPECTED_TYPE, &first) != 0) {
        return -1;
    }
    last = first;
    while (SLPMatch(p, SLTok_DOT)) {
        if (SLPExpect(p, SLTok_IDENT, SLDiag_EXPECTED_TYPE, &last) != 0) {
            return -1;
        }
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

    if (SLPMatch(p, SLTok_LBRACK)) {
        const SLToken* nTok;
        const SLToken* rb;
        typeNode = SLPNewNode(p, SLAST_TYPE_ARRAY, SLPPrev(p)->start, SLPPrev(p)->end);
        if (typeNode < 0) {
            return -1;
        }
        if (SLPExpect(p, SLTok_INT, SLDiag_EXPECTED_TYPE, &nTok) != 0) {
            return -1;
        }
        p->nodes[typeNode].dataStart = nTok->start;
        p->nodes[typeNode].dataEnd = nTok->end;
        if (SLPExpect(p, SLTok_RBRACK, SLDiag_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        if (SLPParseType(p, &child) != 0) {
            return -1;
        }
        p->nodes[typeNode].end = p->nodes[child].end;
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

static int SLPParseStmt(SLParser* p, int32_t* out) {
    const SLToken* kw;
    int32_t        n;
    int32_t        expr;
    int32_t        block;

    switch (SLPPeek(p)->kind) {
        case SLTok_VAR:   return SLPParseVarLikeStmt(p, SLAST_VAR, 1, out);
        case SLTok_CONST: return SLPParseVarLikeStmt(p, SLAST_CONST, 1, out);
        case SLTok_IF:    return SLPParseIfStmt(p, out);
        case SLTok_FOR:   return SLPParseForStmt(p, out);
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

static int SLPParsePubBlock(SLParser* p, int32_t* out) {
    const SLToken* kw = SLPPeek(p);
    const SLToken* rb;
    int32_t        n;
    p->pos++;
    if (SLPExpect(p, SLTok_LBRACE, SLDiag_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    n = SLPNewNode(p, SLAST_PUB, kw->start, rb->end);
    if (n < 0) {
        return -1;
    }

    while (!SLPAt(p, SLTok_RBRACE) && !SLPAt(p, SLTok_EOF)) {
        int32_t decl;
        if (SLPAt(p, SLTok_SEMICOLON)) {
            p->pos++;
            continue;
        }
        if (SLPParseDecl(p, 0, &decl) != 0) {
            return -1;
        }
        if (SLPAddChild(p, n, decl) != 0) {
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

static int SLPParseDecl(SLParser* p, int allowBody, int32_t* out) {
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
        case SLTok_PUB:   return SLPParsePubBlock(p, out);
        default:          return SLPFail(p, SLDiag_EXPECTED_DECL);
    }
}

const char* SLASTKindName(SLASTKind kind) {
    switch (kind) {
        case SLAST_FILE:       return "FILE";
        case SLAST_PACKAGE:    return "PACKAGE";
        case SLAST_IMPORT:     return "IMPORT";
        case SLAST_PUB:        return "PUB";
        case SLAST_FN:         return "FN";
        case SLAST_PARAM:      return "PARAM";
        case SLAST_TYPE_NAME:  return "TYPE_NAME";
        case SLAST_TYPE_PTR:   return "TYPE_PTR";
        case SLAST_TYPE_ARRAY: return "TYPE_ARRAY";
        case SLAST_STRUCT:     return "STRUCT";
        case SLAST_UNION:      return "UNION";
        case SLAST_ENUM:       return "ENUM";
        case SLAST_FIELD:      return "FIELD";
        case SLAST_BLOCK:      return "BLOCK";
        case SLAST_VAR:        return "VAR";
        case SLAST_CONST:      return "CONST";
        case SLAST_IF:         return "IF";
        case SLAST_FOR:        return "FOR";
        case SLAST_RETURN:     return "RETURN";
        case SLAST_BREAK:      return "BREAK";
        case SLAST_CONTINUE:   return "CONTINUE";
        case SLAST_DEFER:      return "DEFER";
        case SLAST_EXPR_STMT:  return "EXPR_STMT";
        case SLAST_IDENT:      return "IDENT";
        case SLAST_INT:        return "INT";
        case SLAST_FLOAT:      return "FLOAT";
        case SLAST_STRING:     return "STRING";
        case SLAST_BOOL:       return "BOOL";
        case SLAST_UNARY:      return "UNARY";
        case SLAST_BINARY:     return "BINARY";
        case SLAST_CALL:       return "CALL";
        case SLAST_INDEX:      return "INDEX";
        case SLAST_FIELD_EXPR: return "FIELD_EXPR";
        case SLAST_CAST:       return "CAST";
    }
    return "UNKNOWN";
}

int SLParse(SLArena* arena, SLStrView src, SLAST* out, SLDiag* diag) {
    SLTokenStream  ts;
    SLParser       p;
    int32_t        root;
    const SLToken* kw;
    const SLToken* pkgName;

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
        SLSetDiag(diag, SLDiag_ARENA_OOM, 0, 0);
        return -1;
    }

    root = SLPNewNode(&p, SLAST_FILE, 0, src.len);
    if (root < 0) {
        return -1;
    }

    if (SLPExpect(&p, SLTok_PACKAGE, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
        return -1;
    }
    if (SLPExpect(&p, SLTok_IDENT, SLDiag_UNEXPECTED_TOKEN, &pkgName) != 0) {
        return -1;
    }
    {
        int32_t pkg = SLPNewNode(&p, SLAST_PACKAGE, kw->start, pkgName->end);
        if (pkg < 0) {
            return -1;
        }
        p.nodes[pkg].dataStart = pkgName->start;
        p.nodes[pkg].dataEnd = pkgName->end;
        if (SLPAddChild(&p, root, pkg) != 0) {
            return -1;
        }
    }
    if (SLPExpect(&p, SLTok_SEMICOLON, SLDiag_UNEXPECTED_TOKEN, &kw) != 0) {
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

static void SLWWrite(SLWriter* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void SLWCStr(SLWriter* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    SLWWrite(w, s, n);
}

static void SLWU32(SLWriter* w, uint32_t v) {
    char     buf[16];
    uint32_t n = 0;
    if (v == 0) {
        SLWWrite(w, "0", 1);
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        SLWWrite(w, &buf[n], 1);
    }
}

static void SLWIndent(SLWriter* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        SLWWrite(w, "  ", 2);
    }
}

static void SLWEscaped(SLWriter* w, SLStrView src, uint32_t start, uint32_t end) {
    uint32_t i;
    SLWWrite(w, "\"", 1);
    for (i = start; i < end && i < src.len; i++) {
        unsigned char c = (unsigned char)src.ptr[i];
        switch (c) {
            case '\"': SLWWrite(w, "\\\"", 2); break;
            case '\\': SLWWrite(w, "\\\\", 2); break;
            case '\n': SLWWrite(w, "\\n", 2); break;
            case '\r': SLWWrite(w, "\\r", 2); break;
            case '\t': SLWWrite(w, "\\t", 2); break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    SLWWrite(w, (const char*)&src.ptr[i], 1);
                } else {
                    char               hex[4];
                    static const char* digits = "0123456789abcdef";
                    hex[0] = '\\';
                    hex[1] = 'x';
                    hex[2] = digits[(c >> 4) & 0x0f];
                    hex[3] = digits[c & 0x0f];
                    SLWWrite(w, hex, 4);
                }
                break;
        }
    }
    SLWWrite(w, "\"", 1);
}

static int SLASTDumpNode(
    const SLAST* ast, int32_t idx, uint32_t depth, SLStrView src, SLWriter* w) {
    const SLASTNode* n;
    int32_t          c;
    if (idx < 0 || (uint32_t)idx >= ast->len) {
        return -1;
    }
    n = &ast->nodes[idx];
    SLWIndent(w, depth);
    SLWCStr(w, SLASTKindName(n->kind));

    if (n->op != 0) {
        SLWCStr(w, " op=");
        SLWCStr(w, SLTokenKindName((SLTokenKind)n->op));
    }
    if (n->flags != 0) {
        SLWCStr(w, " flags=");
        SLWU32(w, n->flags);
    }
    if (n->dataEnd > n->dataStart) {
        SLWCStr(w, " ");
        SLWEscaped(w, src, n->dataStart, n->dataEnd);
    }
    SLWCStr(w, " [");
    SLWU32(w, n->start);
    SLWCStr(w, ",");
    SLWU32(w, n->end);
    SLWCStr(w, "]\n");

    c = n->firstChild;
    while (c >= 0) {
        if (SLASTDumpNode(ast, c, depth + 1, src, w) != 0) {
            return -1;
        }
        c = ast->nodes[c].nextSibling;
    }
    return 0;
}

int SLASTDump(const SLAST* ast, SLStrView src, SLWriter* w, SLDiag* diag) {
    SLDiagClear(diag);
    if (ast == NULL || w == NULL || w->write == NULL || ast->nodes == NULL || ast->root < 0) {
        SLSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    return SLASTDumpNode(ast, ast->root, 0, src, w);
}
