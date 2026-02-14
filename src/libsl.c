#include "libsl.h"

typedef struct {
    sl_token* v;
    uint32_t len;
    uint32_t cap;
} sl_token_buf;

static void sl_set_diag(sl_diag* diag, sl_diag_code code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->start = start;
    diag->end = end;
}

void sl_diag_clear(sl_diag* diag) {
    if (diag == NULL) {
        return;
    }
    diag->code = SL_DIAG_NONE;
    diag->start = 0;
    diag->end = 0;
}

const char* sl_diag_message(sl_diag_code code) {
    switch (code) {
        case SL_DIAG_NONE:
            return "no error";
        case SL_DIAG_ARENA_OOM:
            return "arena out of memory";
        case SL_DIAG_UNEXPECTED_CHAR:
            return "unexpected character";
        case SL_DIAG_UNTERMINATED_STRING:
            return "unterminated string literal";
        case SL_DIAG_INVALID_NUMBER:
            return "invalid number literal";
        case SL_DIAG_UNEXPECTED_TOKEN:
            return "unexpected token";
        case SL_DIAG_EXPECTED_DECL:
            return "expected declaration";
        case SL_DIAG_EXPECTED_EXPR:
            return "expected expression";
        case SL_DIAG_EXPECTED_TYPE:
            return "expected type";
    }
    return "unknown diagnostic";
}

void sl_arena_init(sl_arena* arena, void* storage, uint32_t storage_size) {
    arena->mem = (uint8_t*)storage;
    arena->cap = storage_size;
    arena->len = 0;
}

void sl_arena_reset(sl_arena* arena) {
    arena->len = 0;
}

void* _Nullable sl_arena_alloc(sl_arena* arena, uint32_t size, uint32_t align) {
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

static int sl_is_alpha(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
           (c >= (unsigned char)'a' && c <= (unsigned char)'z');
}

static int sl_is_digit(unsigned char c) {
    return c >= (unsigned char)'0' && c <= (unsigned char)'9';
}

static int sl_is_alnum(unsigned char c) {
    return sl_is_alpha(c) || sl_is_digit(c);
}

static int sl_is_hex_digit(unsigned char c) {
    return sl_is_digit(c) ||
           (c >= (unsigned char)'a' && c <= (unsigned char)'f') ||
           (c >= (unsigned char)'A' && c <= (unsigned char)'F');
}

static int sl_str_eq(const char* a, uint32_t a_len, const char* b) {
    uint32_t i = 0;
    while (i < a_len) {
        if (b[i] == '\0' || a[i] != b[i]) {
            return 0;
        }
        i++;
    }
    return b[i] == '\0';
}

static sl_token_kind sl_keyword_kind(const char* s, uint32_t len) {
    if (len == 2) {
        if (sl_str_eq(s, len, "as")) {
            return SL_TOK_AS;
        }
        if (sl_str_eq(s, len, "if")) {
            return SL_TOK_IF;
        }
    } else if (len == 3) {
        if (sl_str_eq(s, len, "for")) {
            return SL_TOK_FOR;
        }
        if (sl_str_eq(s, len, "fun")) {
            return SL_TOK_FUN;
        }
        if (sl_str_eq(s, len, "var")) {
            return SL_TOK_VAR;
        }
        if (sl_str_eq(s, len, "pub")) {
            return SL_TOK_PUB;
        }
    } else if (len == 4) {
        if (sl_str_eq(s, len, "enum")) {
            return SL_TOK_ENUM;
        }
        if (sl_str_eq(s, len, "else")) {
            return SL_TOK_ELSE;
        }
        if (sl_str_eq(s, len, "case")) {
            return SL_TOK_CASE;
        }
        if (sl_str_eq(s, len, "true")) {
            return SL_TOK_TRUE;
        }
    } else if (len == 5) {
        if (sl_str_eq(s, len, "break")) {
            return SL_TOK_BREAK;
        }
        if (sl_str_eq(s, len, "const")) {
            return SL_TOK_CONST;
        }
        if (sl_str_eq(s, len, "defer")) {
            return SL_TOK_DEFER;
        }
        if (sl_str_eq(s, len, "false")) {
            return SL_TOK_FALSE;
        }
        if (sl_str_eq(s, len, "union")) {
            return SL_TOK_UNION;
        }
        if (sl_str_eq(s, len, "assert")) {
            return SL_TOK_ASSERT;
        }
    } else if (len == 6) {
        if (sl_str_eq(s, len, "import")) {
            return SL_TOK_IMPORT;
        }
        if (sl_str_eq(s, len, "return")) {
            return SL_TOK_RETURN;
        }
        if (sl_str_eq(s, len, "switch")) {
            return SL_TOK_SWITCH;
        }
        if (sl_str_eq(s, len, "struct")) {
            return SL_TOK_STRUCT;
        }
    } else if (len == 7) {
        if (sl_str_eq(s, len, "package")) {
            return SL_TOK_PACKAGE;
        }
        if (sl_str_eq(s, len, "default")) {
            return SL_TOK_DEFAULT;
        }
    } else if (len == 8) {
        if (sl_str_eq(s, len, "continue")) {
            return SL_TOK_CONTINUE;
        }
    }
    return SL_TOK_IDENT;
}

static int sl_token_can_end_stmt(sl_token_kind kind) {
    switch (kind) {
        case SL_TOK_IDENT:
        case SL_TOK_INT:
        case SL_TOK_FLOAT:
        case SL_TOK_STRING:
        case SL_TOK_TRUE:
        case SL_TOK_FALSE:
        case SL_TOK_BREAK:
        case SL_TOK_CONTINUE:
        case SL_TOK_RETURN:
        case SL_TOK_RPAREN:
        case SL_TOK_RBRACK:
        case SL_TOK_RBRACE:
            return 1;
        default:
            return 0;
    }
}

static int sl_push_token(sl_token_buf* out, sl_diag* diag, sl_token_kind kind, uint32_t start,
                         uint32_t end) {
    if (out->len >= out->cap) {
        sl_set_diag(diag, SL_DIAG_ARENA_OOM, start, end);
        return -1;
    }

    out->v[out->len].kind = kind;
    out->v[out->len].start = start;
    out->v[out->len].end = end;
    out->len++;
    return 0;
}

const char* sl_token_kind_name(sl_token_kind kind) {
    switch (kind) {
        case SL_TOK_INVALID:
            return "INVALID";
        case SL_TOK_EOF:
            return "EOF";
        case SL_TOK_IDENT:
            return "IDENT";
        case SL_TOK_INT:
            return "INT";
        case SL_TOK_FLOAT:
            return "FLOAT";
        case SL_TOK_STRING:
            return "STRING";
        case SL_TOK_PACKAGE:
            return "PACKAGE";
        case SL_TOK_IMPORT:
            return "IMPORT";
        case SL_TOK_PUB:
            return "PUB";
        case SL_TOK_STRUCT:
            return "STRUCT";
        case SL_TOK_UNION:
            return "UNION";
        case SL_TOK_ENUM:
            return "ENUM";
        case SL_TOK_FUN:
            return "FUN";
        case SL_TOK_VAR:
            return "VAR";
        case SL_TOK_CONST:
            return "CONST";
        case SL_TOK_IF:
            return "IF";
        case SL_TOK_ELSE:
            return "ELSE";
        case SL_TOK_FOR:
            return "FOR";
        case SL_TOK_SWITCH:
            return "SWITCH";
        case SL_TOK_CASE:
            return "CASE";
        case SL_TOK_DEFAULT:
            return "DEFAULT";
        case SL_TOK_BREAK:
            return "BREAK";
        case SL_TOK_CONTINUE:
            return "CONTINUE";
        case SL_TOK_RETURN:
            return "RETURN";
        case SL_TOK_DEFER:
            return "DEFER";
        case SL_TOK_ASSERT:
            return "ASSERT";
        case SL_TOK_TRUE:
            return "TRUE";
        case SL_TOK_FALSE:
            return "FALSE";
        case SL_TOK_AS:
            return "AS";
        case SL_TOK_LPAREN:
            return "LPAREN";
        case SL_TOK_RPAREN:
            return "RPAREN";
        case SL_TOK_LBRACE:
            return "LBRACE";
        case SL_TOK_RBRACE:
            return "RBRACE";
        case SL_TOK_LBRACK:
            return "LBRACK";
        case SL_TOK_RBRACK:
            return "RBRACK";
        case SL_TOK_COMMA:
            return "COMMA";
        case SL_TOK_DOT:
            return "DOT";
        case SL_TOK_SEMICOLON:
            return "SEMICOLON";
        case SL_TOK_COLON:
            return "COLON";
        case SL_TOK_ASSIGN:
            return "ASSIGN";
        case SL_TOK_ADD:
            return "ADD";
        case SL_TOK_SUB:
            return "SUB";
        case SL_TOK_MUL:
            return "MUL";
        case SL_TOK_DIV:
            return "DIV";
        case SL_TOK_MOD:
            return "MOD";
        case SL_TOK_AND:
            return "AND";
        case SL_TOK_OR:
            return "OR";
        case SL_TOK_XOR:
            return "XOR";
        case SL_TOK_NOT:
            return "NOT";
        case SL_TOK_LSHIFT:
            return "LSHIFT";
        case SL_TOK_RSHIFT:
            return "RSHIFT";
        case SL_TOK_EQ:
            return "EQ";
        case SL_TOK_NEQ:
            return "NEQ";
        case SL_TOK_LT:
            return "LT";
        case SL_TOK_GT:
            return "GT";
        case SL_TOK_LTE:
            return "LTE";
        case SL_TOK_GTE:
            return "GTE";
        case SL_TOK_LOGICAL_AND:
            return "LOGICAL_AND";
        case SL_TOK_LOGICAL_OR:
            return "LOGICAL_OR";
        case SL_TOK_ADD_ASSIGN:
            return "ADD_ASSIGN";
        case SL_TOK_SUB_ASSIGN:
            return "SUB_ASSIGN";
        case SL_TOK_MUL_ASSIGN:
            return "MUL_ASSIGN";
        case SL_TOK_DIV_ASSIGN:
            return "DIV_ASSIGN";
        case SL_TOK_MOD_ASSIGN:
            return "MOD_ASSIGN";
        case SL_TOK_AND_ASSIGN:
            return "AND_ASSIGN";
        case SL_TOK_OR_ASSIGN:
            return "OR_ASSIGN";
        case SL_TOK_XOR_ASSIGN:
            return "XOR_ASSIGN";
        case SL_TOK_LSHIFT_ASSIGN:
            return "LSHIFT_ASSIGN";
        case SL_TOK_RSHIFT_ASSIGN:
            return "RSHIFT_ASSIGN";
    }
    return "UNKNOWN";
}

int sl_lex(sl_arena* arena, sl_strview src, sl_token_stream* out, sl_diag* diag) {
    sl_token_buf tokbuf;
    uint32_t pos = 0;
    int inserted_eof_semicolon = 0;
    sl_token_kind prev_kind = SL_TOK_INVALID;

    sl_diag_clear(diag);
    out->v = NULL;
    out->len = 0;

    tokbuf.len = 0;
    tokbuf.cap = src.len + 2;
    if (tokbuf.cap < 8) {
        tokbuf.cap = 8;
    }
    tokbuf.v = (sl_token*)sl_arena_alloc(arena, tokbuf.cap * (uint32_t)sizeof(sl_token),
                                         (uint32_t)_Alignof(sl_token));
    if (tokbuf.v == NULL) {
        sl_set_diag(diag, SL_DIAG_ARENA_OOM, 0, 0);
        return -1;
    }

    for (;;) {
        int saw_newline = 0;
        uint32_t newline_pos = 0;

        for (;;) {
            unsigned char c;
            if (pos >= src.len) {
                break;
            }

            c = (unsigned char)src.ptr[pos];
            if (c == (unsigned char)' ' || c == (unsigned char)'\t' || c == (unsigned char)'\r' ||
                c == (unsigned char)'\f' || c == (unsigned char)'\v') {
                pos++;
                continue;
            }

            if (c == (unsigned char)'\n') {
                if (!saw_newline) {
                    saw_newline = 1;
                    newline_pos = pos;
                }
                pos++;
                continue;
            }

            if (c == (unsigned char)'/' && pos + 1 < src.len &&
                (unsigned char)src.ptr[pos + 1] == (unsigned char)'/') {
                pos += 2;
                while (pos < src.len && (unsigned char)src.ptr[pos] != (unsigned char)'\n') {
                    pos++;
                }
                continue;
            }
            break;
        }

        if (saw_newline && sl_token_can_end_stmt(prev_kind)) {
            if (sl_push_token(&tokbuf, diag, SL_TOK_SEMICOLON, newline_pos, newline_pos) != 0) {
                return -1;
            }
            prev_kind = SL_TOK_SEMICOLON;
            continue;
        }

        if (pos >= src.len) {
            if (!inserted_eof_semicolon && sl_token_can_end_stmt(prev_kind)) {
                if (sl_push_token(&tokbuf, diag, SL_TOK_SEMICOLON, src.len, src.len) != 0) {
                    return -1;
                }
                prev_kind = SL_TOK_SEMICOLON;
                inserted_eof_semicolon = 1;
                continue;
            }
            if (sl_push_token(&tokbuf, diag, SL_TOK_EOF, src.len, src.len) != 0) {
                return -1;
            }
            break;
        }

        {
            sl_token_kind kind = SL_TOK_INVALID;
            uint32_t start = pos;
            unsigned char c = (unsigned char)src.ptr[pos];

            if (sl_is_alpha(c) || c == (unsigned char)'_') {
                pos++;
                while (pos < src.len) {
                    c = (unsigned char)src.ptr[pos];
                    if (!sl_is_alnum(c) && c != (unsigned char)'_') {
                        break;
                    }
                    pos++;
                }
                kind = sl_keyword_kind(src.ptr + start, pos - start);
            } else if (sl_is_digit(c)) {
                kind = SL_TOK_INT;

                if (c == (unsigned char)'0' && pos + 1 < src.len &&
                    ((unsigned char)src.ptr[pos + 1] == (unsigned char)'x' ||
                     (unsigned char)src.ptr[pos + 1] == (unsigned char)'X')) {
                    uint32_t digits_start;
                    pos += 2;
                    digits_start = pos;
                    while (pos < src.len && sl_is_hex_digit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }
                    if (pos == digits_start) {
                        sl_set_diag(diag, SL_DIAG_INVALID_NUMBER, start, pos);
                        return -1;
                    }
                } else {
                    while (pos < src.len && sl_is_digit((unsigned char)src.ptr[pos])) {
                        pos++;
                    }

                    if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'.') {
                        kind = SL_TOK_FLOAT;
                        pos++;
                        while (pos < src.len && sl_is_digit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                    }

                    if (pos < src.len && ((unsigned char)src.ptr[pos] == (unsigned char)'e' ||
                                          (unsigned char)src.ptr[pos] == (unsigned char)'E')) {
                        uint32_t exp_start;
                        kind = SL_TOK_FLOAT;
                        pos++;
                        if (pos < src.len && ((unsigned char)src.ptr[pos] == (unsigned char)'+' ||
                                              (unsigned char)src.ptr[pos] == (unsigned char)'-')) {
                            pos++;
                        }
                        exp_start = pos;
                        while (pos < src.len && sl_is_digit((unsigned char)src.ptr[pos])) {
                            pos++;
                        }
                        if (pos == exp_start) {
                            sl_set_diag(diag, SL_DIAG_INVALID_NUMBER, start, pos);
                            return -1;
                        }
                    }
                }
            } else if (c == (unsigned char)'"') {
                kind = SL_TOK_STRING;
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
                            sl_set_diag(diag, SL_DIAG_UNTERMINATED_STRING, start, pos);
                            return -1;
                        }
                        pos++;
                        continue;
                    }
                    if (c == (unsigned char)'\n' || c == (unsigned char)'\r') {
                        sl_set_diag(diag, SL_DIAG_UNTERMINATED_STRING, start, pos);
                        return -1;
                    }
                    pos++;
                }
                if (pos > src.len || src.ptr[pos - 1] != '"') {
                    sl_set_diag(diag, SL_DIAG_UNTERMINATED_STRING, start, pos);
                    return -1;
                }
            } else {
                pos++;
                switch (c) {
                    case (unsigned char)'(':
                        kind = SL_TOK_LPAREN;
                        break;
                    case (unsigned char)')':
                        kind = SL_TOK_RPAREN;
                        break;
                    case (unsigned char)'{':
                        kind = SL_TOK_LBRACE;
                        break;
                    case (unsigned char)'}':
                        kind = SL_TOK_RBRACE;
                        break;
                    case (unsigned char)'[':
                        kind = SL_TOK_LBRACK;
                        break;
                    case (unsigned char)']':
                        kind = SL_TOK_RBRACK;
                        break;
                    case (unsigned char)',':
                        kind = SL_TOK_COMMA;
                        break;
                    case (unsigned char)'.':
                        kind = SL_TOK_DOT;
                        break;
                    case (unsigned char)';':
                        kind = SL_TOK_SEMICOLON;
                        break;
                    case (unsigned char)':':
                        kind = SL_TOK_COLON;
                        break;

                    case (unsigned char)'+':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_ADD_ASSIGN;
                        } else {
                            kind = SL_TOK_ADD;
                        }
                        break;
                    case (unsigned char)'-':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_SUB_ASSIGN;
                        } else {
                            kind = SL_TOK_SUB;
                        }
                        break;
                    case (unsigned char)'*':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_MUL_ASSIGN;
                        } else {
                            kind = SL_TOK_MUL;
                        }
                        break;
                    case (unsigned char)'/':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_DIV_ASSIGN;
                        } else {
                            kind = SL_TOK_DIV;
                        }
                        break;
                    case (unsigned char)'%':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_MOD_ASSIGN;
                        } else {
                            kind = SL_TOK_MOD;
                        }
                        break;
                    case (unsigned char)'&':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'&') {
                            pos++;
                            kind = SL_TOK_LOGICAL_AND;
                        } else if (pos < src.len &&
                                   (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_AND_ASSIGN;
                        } else {
                            kind = SL_TOK_AND;
                        }
                        break;
                    case (unsigned char)'|':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'|') {
                            pos++;
                            kind = SL_TOK_LOGICAL_OR;
                        } else if (pos < src.len &&
                                   (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_OR_ASSIGN;
                        } else {
                            kind = SL_TOK_OR;
                        }
                        break;
                    case (unsigned char)'^':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_XOR_ASSIGN;
                        } else {
                            kind = SL_TOK_XOR;
                        }
                        break;
                    case (unsigned char)'!':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_NEQ;
                        } else {
                            kind = SL_TOK_NOT;
                        }
                        break;
                    case (unsigned char)'=':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_EQ;
                        } else {
                            kind = SL_TOK_ASSIGN;
                        }
                        break;
                    case (unsigned char)'<':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'<') {
                            pos++;
                            if (pos < src.len &&
                                (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                                pos++;
                                kind = SL_TOK_LSHIFT_ASSIGN;
                            } else {
                                kind = SL_TOK_LSHIFT;
                            }
                        } else if (pos < src.len &&
                                   (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_LTE;
                        } else {
                            kind = SL_TOK_LT;
                        }
                        break;
                    case (unsigned char)'>':
                        if (pos < src.len && (unsigned char)src.ptr[pos] == (unsigned char)'>') {
                            pos++;
                            if (pos < src.len &&
                                (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                                pos++;
                                kind = SL_TOK_RSHIFT_ASSIGN;
                            } else {
                                kind = SL_TOK_RSHIFT;
                            }
                        } else if (pos < src.len &&
                                   (unsigned char)src.ptr[pos] == (unsigned char)'=') {
                            pos++;
                            kind = SL_TOK_GTE;
                        } else {
                            kind = SL_TOK_GT;
                        }
                        break;

                    default:
                        sl_set_diag(diag, SL_DIAG_UNEXPECTED_CHAR, start, pos);
                        return -1;
                }
            }

            if (sl_push_token(&tokbuf, diag, kind, start, pos) != 0) {
                return -1;
            }
            prev_kind = kind;
        }
    }

    out->v = tokbuf.v;
    out->len = tokbuf.len;
    return 0;
}

typedef struct {
    sl_strview src;
    const sl_token* tok;
    uint32_t tok_len;
    uint32_t pos;
    sl_ast_node* nodes;
    uint32_t node_len;
    uint32_t node_cap;
    sl_diag* diag;
} sl_parser;

static const sl_token* sl_p_peek(sl_parser* p) {
    if (p->pos >= p->tok_len) {
        return &p->tok[p->tok_len - 1];
    }
    return &p->tok[p->pos];
}

static const sl_token* sl_p_prev(sl_parser* p) {
    if (p->pos == 0) {
        return &p->tok[0];
    }
    return &p->tok[p->pos - 1];
}

static int sl_p_at(sl_parser* p, sl_token_kind kind) {
    return sl_p_peek(p)->kind == kind;
}

static int sl_p_match(sl_parser* p, sl_token_kind kind) {
    if (!sl_p_at(p, kind)) {
        return 0;
    }
    p->pos++;
    return 1;
}

static int sl_p_fail(sl_parser* p, sl_diag_code code) {
    const sl_token* t = sl_p_peek(p);
    sl_set_diag(p->diag, code, t->start, t->end);
    return -1;
}

static int sl_p_expect(sl_parser* p, sl_token_kind kind, sl_diag_code code, const sl_token** out) {
    if (!sl_p_at(p, kind)) {
        return sl_p_fail(p, code);
    }
    *out = sl_p_peek(p);
    p->pos++;
    return 0;
}

static int32_t sl_p_new_node(sl_parser* p, sl_ast_kind kind, uint32_t start, uint32_t end) {
    int32_t idx;
    if (p->node_len >= p->node_cap) {
        sl_set_diag(p->diag, SL_DIAG_ARENA_OOM, start, end);
        return -1;
    }
    idx = (int32_t)p->node_len++;
    p->nodes[idx].kind = kind;
    p->nodes[idx].start = start;
    p->nodes[idx].end = end;
    p->nodes[idx].first_child = -1;
    p->nodes[idx].next_sibling = -1;
    p->nodes[idx].data_start = 0;
    p->nodes[idx].data_end = 0;
    p->nodes[idx].op = 0;
    p->nodes[idx].flags = 0;
    return idx;
}

static int sl_p_add_child(sl_parser* p, int32_t parent, int32_t child) {
    int32_t n;
    if (parent < 0 || child < 0) {
        return -1;
    }
    if (p->nodes[parent].first_child < 0) {
        p->nodes[parent].first_child = child;
        return 0;
    }
    n = p->nodes[parent].first_child;
    while (p->nodes[n].next_sibling >= 0) {
        n = p->nodes[n].next_sibling;
    }
    p->nodes[n].next_sibling = child;
    return 0;
}

static int sl_is_assignment_op(sl_token_kind kind) {
    switch (kind) {
        case SL_TOK_ASSIGN:
        case SL_TOK_ADD_ASSIGN:
        case SL_TOK_SUB_ASSIGN:
        case SL_TOK_MUL_ASSIGN:
        case SL_TOK_DIV_ASSIGN:
        case SL_TOK_MOD_ASSIGN:
        case SL_TOK_AND_ASSIGN:
        case SL_TOK_OR_ASSIGN:
        case SL_TOK_XOR_ASSIGN:
        case SL_TOK_LSHIFT_ASSIGN:
        case SL_TOK_RSHIFT_ASSIGN:
            return 1;
        default:
            return 0;
    }
}

static int sl_bin_prec(sl_token_kind kind) {
    if (sl_is_assignment_op(kind)) {
        return 1;
    }
    switch (kind) {
        case SL_TOK_LOGICAL_OR:
            return 2;
        case SL_TOK_LOGICAL_AND:
            return 3;
        case SL_TOK_OR:
            return 4;
        case SL_TOK_XOR:
            return 5;
        case SL_TOK_AND:
            return 6;
        case SL_TOK_EQ:
        case SL_TOK_NEQ:
            return 7;
        case SL_TOK_LT:
        case SL_TOK_GT:
        case SL_TOK_LTE:
        case SL_TOK_GTE:
            return 8;
        case SL_TOK_LSHIFT:
        case SL_TOK_RSHIFT:
            return 9;
        case SL_TOK_ADD:
        case SL_TOK_SUB:
            return 10;
        case SL_TOK_MUL:
        case SL_TOK_DIV:
        case SL_TOK_MOD:
            return 11;
        default:
            return 0;
    }
}

static int sl_p_parse_type(sl_parser* p, int32_t* out);
static int sl_p_parse_expr(sl_parser* p, int min_prec, int32_t* out);
static int sl_p_parse_stmt(sl_parser* p, int32_t* out);
static int sl_p_parse_decl(sl_parser* p, int allow_body, int32_t* out);

static int sl_p_parse_type_name(sl_parser* p, int32_t* out) {
    const sl_token* first;
    const sl_token* last;
    int32_t n;

    if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_EXPECTED_TYPE, &first) != 0) {
        return -1;
    }
    last = first;
    while (sl_p_match(p, SL_TOK_DOT)) {
        if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_EXPECTED_TYPE, &last) != 0) {
            return -1;
        }
    }

    n = sl_p_new_node(p, SL_AST_TYPE_NAME, first->start, last->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].data_start = first->start;
    p->nodes[n].data_end = last->end;
    *out = n;
    return 0;
}

static int sl_p_parse_type(sl_parser* p, int32_t* out) {
    const sl_token* t;
    int32_t type_node;
    int32_t child;

    if (sl_p_match(p, SL_TOK_MUL)) {
        t = sl_p_prev(p);
        type_node = sl_p_new_node(p, SL_AST_TYPE_PTR, t->start, t->end);
        if (type_node < 0) {
            return -1;
        }
        if (sl_p_parse_type(p, &child) != 0) {
            return -1;
        }
        p->nodes[type_node].end = p->nodes[child].end;
        return sl_p_add_child(p, type_node, child) == 0 ? (*out = type_node, 0) : -1;
    }

    if (sl_p_match(p, SL_TOK_LBRACK)) {
        const sl_token* n_tok;
        const sl_token* rb;
        type_node = sl_p_new_node(p, SL_AST_TYPE_ARRAY, sl_p_prev(p)->start, sl_p_prev(p)->end);
        if (type_node < 0) {
            return -1;
        }
        if (sl_p_expect(p, SL_TOK_INT, SL_DIAG_EXPECTED_TYPE, &n_tok) != 0) {
            return -1;
        }
        p->nodes[type_node].data_start = n_tok->start;
        p->nodes[type_node].data_end = n_tok->end;
        if (sl_p_expect(p, SL_TOK_RBRACK, SL_DIAG_EXPECTED_TYPE, &rb) != 0) {
            return -1;
        }
        if (sl_p_parse_type(p, &child) != 0) {
            return -1;
        }
        p->nodes[type_node].end = p->nodes[child].end;
        return sl_p_add_child(p, type_node, child) == 0 ? (*out = type_node, 0) : -1;
    }

    return sl_p_parse_type_name(p, out);
}

static int sl_p_parse_primary(sl_parser* p, int32_t* out) {
    const sl_token* t = sl_p_peek(p);
    int32_t n;

    if (sl_p_match(p, SL_TOK_IDENT)) {
        n = sl_p_new_node(p, SL_AST_IDENT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].data_start = t->start;
        p->nodes[n].data_end = t->end;
        *out = n;
        return 0;
    }

    if (sl_p_match(p, SL_TOK_INT)) {
        n = sl_p_new_node(p, SL_AST_INT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].data_start = t->start;
        p->nodes[n].data_end = t->end;
        *out = n;
        return 0;
    }

    if (sl_p_match(p, SL_TOK_FLOAT)) {
        n = sl_p_new_node(p, SL_AST_FLOAT, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].data_start = t->start;
        p->nodes[n].data_end = t->end;
        *out = n;
        return 0;
    }

    if (sl_p_match(p, SL_TOK_STRING)) {
        n = sl_p_new_node(p, SL_AST_STRING, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].data_start = t->start;
        p->nodes[n].data_end = t->end;
        *out = n;
        return 0;
    }

    if (sl_p_match(p, SL_TOK_TRUE) || sl_p_match(p, SL_TOK_FALSE)) {
        t = sl_p_prev(p);
        n = sl_p_new_node(p, SL_AST_BOOL, t->start, t->end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].data_start = t->start;
        p->nodes[n].data_end = t->end;
        *out = n;
        return 0;
    }

    if (sl_p_match(p, SL_TOK_LPAREN)) {
        if (sl_p_parse_expr(p, 1, out) != 0) {
            return -1;
        }
        if (sl_p_expect(p, SL_TOK_RPAREN, SL_DIAG_EXPECTED_EXPR, &t) != 0) {
            return -1;
        }
        return 0;
    }

    return sl_p_fail(p, SL_DIAG_EXPECTED_EXPR);
}

static int sl_p_parse_postfix(sl_parser* p, int32_t* expr) {
    for (;;) {
        int32_t n;
        const sl_token* t;

        if (sl_p_match(p, SL_TOK_LPAREN)) {
            n = sl_p_new_node(p, SL_AST_CALL, p->nodes[*expr].start, sl_p_prev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (sl_p_add_child(p, n, *expr) != 0) {
                return -1;
            }
            if (!sl_p_at(p, SL_TOK_RPAREN)) {
                for (;;) {
                    int32_t arg;
                    if (sl_p_parse_expr(p, 1, &arg) != 0) {
                        return -1;
                    }
                    if (sl_p_add_child(p, n, arg) != 0) {
                        return -1;
                    }
                    if (!sl_p_match(p, SL_TOK_COMMA)) {
                        break;
                    }
                }
            }
            if (sl_p_expect(p, SL_TOK_RPAREN, SL_DIAG_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (sl_p_match(p, SL_TOK_LBRACK)) {
            int32_t idx_expr;
            n = sl_p_new_node(p, SL_AST_INDEX, p->nodes[*expr].start, sl_p_prev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (sl_p_add_child(p, n, *expr) != 0) {
                return -1;
            }
            if (sl_p_parse_expr(p, 1, &idx_expr) != 0) {
                return -1;
            }
            if (sl_p_add_child(p, n, idx_expr) != 0) {
                return -1;
            }
            if (sl_p_expect(p, SL_TOK_RBRACK, SL_DIAG_EXPECTED_EXPR, &t) != 0) {
                return -1;
            }
            p->nodes[n].end = t->end;
            *expr = n;
            continue;
        }

        if (sl_p_match(p, SL_TOK_DOT)) {
            const sl_token* field_tok;
            n = sl_p_new_node(p, SL_AST_FIELD_EXPR, p->nodes[*expr].start, sl_p_prev(p)->end);
            if (n < 0) {
                return -1;
            }
            if (sl_p_add_child(p, n, *expr) != 0) {
                return -1;
            }
            if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_EXPECTED_EXPR, &field_tok) != 0) {
                return -1;
            }
            p->nodes[n].data_start = field_tok->start;
            p->nodes[n].data_end = field_tok->end;
            p->nodes[n].end = field_tok->end;
            *expr = n;
            continue;
        }

        if (sl_p_match(p, SL_TOK_AS)) {
            int32_t type_node;
            n = sl_p_new_node(p, SL_AST_CAST, p->nodes[*expr].start, sl_p_prev(p)->end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)SL_TOK_AS;
            if (sl_p_add_child(p, n, *expr) != 0) {
                return -1;
            }
            if (sl_p_parse_type(p, &type_node) != 0) {
                return -1;
            }
            if (sl_p_add_child(p, n, type_node) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[type_node].end;
            *expr = n;
            continue;
        }

        break;
    }
    return 0;
}

static int sl_p_parse_prefix(sl_parser* p, int32_t* out) {
    sl_token_kind op = sl_p_peek(p)->kind;
    int32_t rhs;
    int32_t n;
    const sl_token* t = sl_p_peek(p);

    switch (op) {
        case SL_TOK_ADD:
        case SL_TOK_SUB:
        case SL_TOK_NOT:
        case SL_TOK_MUL:
        case SL_TOK_AND:
            p->pos++;
            if (sl_p_parse_prefix(p, &rhs) != 0) {
                return -1;
            }
            n = sl_p_new_node(p, SL_AST_UNARY, t->start, p->nodes[rhs].end);
            if (n < 0) {
                return -1;
            }
            p->nodes[n].op = (uint16_t)op;
            if (sl_p_add_child(p, n, rhs) != 0) {
                return -1;
            }
            *out = n;
            return 0;
        default:
            if (sl_p_parse_primary(p, out) != 0) {
                return -1;
            }
            return sl_p_parse_postfix(p, out);
    }
}

static int sl_p_parse_expr(sl_parser* p, int min_prec, int32_t* out) {
    int32_t lhs;
    if (sl_p_parse_prefix(p, &lhs) != 0) {
        return -1;
    }

    for (;;) {
        sl_token_kind op = sl_p_peek(p)->kind;
        int prec = sl_bin_prec(op);
        int right_assoc = sl_is_assignment_op(op);
        int32_t rhs;
        int32_t n;

        if (prec < min_prec || prec == 0) {
            break;
        }
        p->pos++;
        if (sl_p_parse_expr(p, right_assoc ? prec : prec + 1, &rhs) != 0) {
            return -1;
        }
        n = sl_p_new_node(p, SL_AST_BINARY, p->nodes[lhs].start, p->nodes[rhs].end);
        if (n < 0) {
            return -1;
        }
        p->nodes[n].op = (uint16_t)op;
        if (sl_p_add_child(p, n, lhs) != 0 || sl_p_add_child(p, n, rhs) != 0) {
            return -1;
        }
        lhs = n;
    }

    *out = lhs;
    return 0;
}

static int sl_p_parse_param(sl_parser* p, int32_t* out) {
    const sl_token* name;
    int32_t param;
    int32_t type;
    if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    if (sl_p_parse_type(p, &type) != 0) {
        return -1;
    }
    param = sl_p_new_node(p, SL_AST_PARAM, name->start, p->nodes[type].end);
    if (param < 0) {
        return -1;
    }
    p->nodes[param].data_start = name->start;
    p->nodes[param].data_end = name->end;
    if (sl_p_add_child(p, param, type) != 0) {
        return -1;
    }
    *out = param;
    return 0;
}

static int sl_p_parse_block(sl_parser* p, int32_t* out) {
    const sl_token* lb;
    const sl_token* rb;
    int32_t block;

    if (sl_p_expect(p, SL_TOK_LBRACE, SL_DIAG_UNEXPECTED_TOKEN, &lb) != 0) {
        return -1;
    }
    block = sl_p_new_node(p, SL_AST_BLOCK, lb->start, lb->end);
    if (block < 0) {
        return -1;
    }

    while (!sl_p_at(p, SL_TOK_RBRACE) && !sl_p_at(p, SL_TOK_EOF)) {
        int32_t stmt = -1;
        if (sl_p_at(p, SL_TOK_SEMICOLON)) {
            p->pos++;
            continue;
        }
        if (sl_p_parse_stmt(p, &stmt) != 0) {
            return -1;
        }
        if (stmt >= 0 && sl_p_add_child(p, block, stmt) != 0) {
            return -1;
        }
    }

    if (sl_p_expect(p, SL_TOK_RBRACE, SL_DIAG_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[block].end = rb->end;
    *out = block;
    return 0;
}

static int sl_p_parse_var_like_stmt(sl_parser* p, sl_ast_kind kind, int require_semi, int32_t* out) {
    const sl_token* kw = sl_p_peek(p);
    const sl_token* name;
    int32_t n;
    int32_t type;

    p->pos++;
    if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    if (sl_p_parse_type(p, &type) != 0) {
        return -1;
    }

    n = sl_p_new_node(p, kind, kw->start, p->nodes[type].end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].data_start = name->start;
    p->nodes[n].data_end = name->end;
    if (sl_p_add_child(p, n, type) != 0) {
        return -1;
    }

    if (sl_p_match(p, SL_TOK_ASSIGN)) {
        int32_t init;
        if (sl_p_parse_expr(p, 1, &init) != 0) {
            return -1;
        }
        if (sl_p_add_child(p, n, init) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[init].end;
    }

    if (require_semi) {
        if (sl_p_expect(p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
            return -1;
        }
        p->nodes[n].end = kw->end;
    }

    *out = n;
    return 0;
}

static int sl_p_parse_if_stmt(sl_parser* p, int32_t* out) {
    const sl_token* kw = sl_p_peek(p);
    int32_t n;
    int32_t cond;
    int32_t then_block;

    p->pos++;
    if (sl_p_parse_expr(p, 1, &cond) != 0) {
        return -1;
    }
    if (sl_p_parse_block(p, &then_block) != 0) {
        return -1;
    }

    n = sl_p_new_node(p, SL_AST_IF, kw->start, p->nodes[then_block].end);
    if (n < 0) {
        return -1;
    }
    if (sl_p_add_child(p, n, cond) != 0 || sl_p_add_child(p, n, then_block) != 0) {
        return -1;
    }

    if (sl_p_match(p, SL_TOK_SEMICOLON) && sl_p_at(p, SL_TOK_ELSE)) {
        /* Allow newline between `}` and `else`. */
    } else if (p->pos > 0 && sl_p_prev(p)->kind == SL_TOK_SEMICOLON && !sl_p_at(p, SL_TOK_ELSE)) {
        p->pos--;
    }

    if (sl_p_match(p, SL_TOK_ELSE)) {
        int32_t else_node;
        if (sl_p_at(p, SL_TOK_IF)) {
            if (sl_p_parse_if_stmt(p, &else_node) != 0) {
                return -1;
            }
        } else {
            if (sl_p_parse_block(p, &else_node) != 0) {
                return -1;
            }
        }
        if (sl_p_add_child(p, n, else_node) != 0) {
            return -1;
        }
        p->nodes[n].end = p->nodes[else_node].end;
    }

    *out = n;
    return 0;
}

static int sl_p_parse_for_stmt(sl_parser* p, int32_t* out) {
    const sl_token* kw = sl_p_peek(p);
    int32_t n;
    int32_t body;
    int32_t init = -1;
    int32_t cond = -1;
    int32_t post = -1;

    p->pos++;
    n = sl_p_new_node(p, SL_AST_FOR, kw->start, kw->end);
    if (n < 0) {
        return -1;
    }

    if (sl_p_at(p, SL_TOK_LBRACE)) {
        if (sl_p_parse_block(p, &body) != 0) {
            return -1;
        }
    } else {
        if (sl_p_at(p, SL_TOK_SEMICOLON)) {
            p->pos++;
        } else if (sl_p_at(p, SL_TOK_VAR)) {
            if (sl_p_parse_var_like_stmt(p, SL_AST_VAR, 0, &init) != 0) {
                return -1;
            }
        } else {
            if (sl_p_parse_expr(p, 1, &init) != 0) {
                return -1;
            }
        }

        if (sl_p_match(p, SL_TOK_SEMICOLON)) {
            if (!sl_p_at(p, SL_TOK_SEMICOLON)) {
                if (sl_p_parse_expr(p, 1, &cond) != 0) {
                    return -1;
                }
            }
            if (!sl_p_match(p, SL_TOK_SEMICOLON)) {
                return sl_p_fail(p, SL_DIAG_UNEXPECTED_TOKEN);
            }
            if (!sl_p_at(p, SL_TOK_LBRACE)) {
                if (sl_p_parse_expr(p, 1, &post) != 0) {
                    return -1;
                }
            }
        } else {
            cond = init;
            init = -1;
        }

        if (sl_p_parse_block(p, &body) != 0) {
            return -1;
        }
    }

    if (init >= 0 && sl_p_add_child(p, n, init) != 0) {
        return -1;
    }
    if (cond >= 0 && sl_p_add_child(p, n, cond) != 0) {
        return -1;
    }
    if (post >= 0 && sl_p_add_child(p, n, post) != 0) {
        return -1;
    }
    if (sl_p_add_child(p, n, body) != 0) {
        return -1;
    }
    p->nodes[n].end = p->nodes[body].end;
    *out = n;
    return 0;
}

static int sl_p_parse_stmt(sl_parser* p, int32_t* out) {
    const sl_token* kw;
    int32_t n;
    int32_t expr;
    int32_t block;

    switch (sl_p_peek(p)->kind) {
        case SL_TOK_VAR:
            return sl_p_parse_var_like_stmt(p, SL_AST_VAR, 1, out);
        case SL_TOK_CONST:
            return sl_p_parse_var_like_stmt(p, SL_AST_CONST, 1, out);
        case SL_TOK_IF:
            return sl_p_parse_if_stmt(p, out);
        case SL_TOK_FOR:
            return sl_p_parse_for_stmt(p, out);
        case SL_TOK_RETURN:
            kw = sl_p_peek(p);
            p->pos++;
            n = sl_p_new_node(p, SL_AST_RETURN, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (!sl_p_at(p, SL_TOK_SEMICOLON)) {
                if (sl_p_parse_expr(p, 1, &expr) != 0) {
                    return -1;
                }
                if (sl_p_add_child(p, n, expr) != 0) {
                    return -1;
                }
                p->nodes[n].end = p->nodes[expr].end;
            }
            if (sl_p_expect(p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            p->nodes[n].end = kw->end;
            *out = n;
            return 0;
        case SL_TOK_BREAK:
            kw = sl_p_peek(p);
            p->pos++;
            n = sl_p_new_node(p, SL_AST_BREAK, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (sl_p_expect(p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            p->nodes[n].end = kw->end;
            *out = n;
            return 0;
        case SL_TOK_CONTINUE:
            kw = sl_p_peek(p);
            p->pos++;
            n = sl_p_new_node(p, SL_AST_CONTINUE, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (sl_p_expect(p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            p->nodes[n].end = kw->end;
            *out = n;
            return 0;
        case SL_TOK_DEFER:
            kw = sl_p_peek(p);
            p->pos++;
            n = sl_p_new_node(p, SL_AST_DEFER, kw->start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (sl_p_at(p, SL_TOK_LBRACE)) {
                if (sl_p_parse_block(p, &block) != 0) {
                    return -1;
                }
            } else {
                if (sl_p_parse_stmt(p, &block) != 0) {
                    return -1;
                }
            }
            if (sl_p_add_child(p, n, block) != 0) {
                return -1;
            }
            p->nodes[n].end = p->nodes[block].end;
            *out = n;
            return 0;
        case SL_TOK_LBRACE:
            return sl_p_parse_block(p, out);
        default:
            if (sl_p_parse_expr(p, 1, &expr) != 0) {
                return -1;
            }
            if (sl_p_expect(p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
                return -1;
            }
            n = sl_p_new_node(p, SL_AST_EXPR_STMT, p->nodes[expr].start, kw->end);
            if (n < 0) {
                return -1;
            }
            if (sl_p_add_child(p, n, expr) != 0) {
                return -1;
            }
            *out = n;
            return 0;
    }
}

static int sl_p_parse_field_list(sl_parser* p, int32_t agg) {
    while (!sl_p_at(p, SL_TOK_RBRACE) && !sl_p_at(p, SL_TOK_EOF)) {
        const sl_token* name;
        int32_t field;
        int32_t type;
        if (sl_p_at(p, SL_TOK_SEMICOLON) || sl_p_at(p, SL_TOK_COMMA)) {
            p->pos++;
            continue;
        }
        if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_UNEXPECTED_TOKEN, &name) != 0) {
            return -1;
        }
        if (sl_p_parse_type(p, &type) != 0) {
            return -1;
        }
        field = sl_p_new_node(p, SL_AST_FIELD, name->start, p->nodes[type].end);
        if (field < 0) {
            return -1;
        }
        p->nodes[field].data_start = name->start;
        p->nodes[field].data_end = name->end;
        if (sl_p_add_child(p, field, type) != 0) {
            return -1;
        }
        if (sl_p_add_child(p, agg, field) != 0) {
            return -1;
        }
        if (sl_p_match(p, SL_TOK_SEMICOLON) || sl_p_match(p, SL_TOK_COMMA)) {
            continue;
        }
    }
    return 0;
}

static int sl_p_parse_aggregate_decl(sl_parser* p, int32_t* out) {
    const sl_token* kw = sl_p_peek(p);
    const sl_token* name;
    const sl_token* rb;
    sl_ast_kind kind = SL_AST_STRUCT;
    int32_t n;

    if (kw->kind == SL_TOK_UNION) {
        kind = SL_AST_UNION;
    } else if (kw->kind == SL_TOK_ENUM) {
        kind = SL_AST_ENUM;
    }

    p->pos++;
    if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    n = sl_p_new_node(p, kind, kw->start, name->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].data_start = name->start;
    p->nodes[n].data_end = name->end;

    if (kw->kind == SL_TOK_ENUM) {
        int32_t under_type;
        if (sl_p_parse_type(p, &under_type) != 0) {
            return -1;
        }
        if (sl_p_add_child(p, n, under_type) != 0) {
            return -1;
        }
    }

    if (sl_p_expect(p, SL_TOK_LBRACE, SL_DIAG_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    if (kw->kind == SL_TOK_ENUM) {
        while (!sl_p_at(p, SL_TOK_RBRACE) && !sl_p_at(p, SL_TOK_EOF)) {
            const sl_token* item_name;
            int32_t item;
            if (sl_p_at(p, SL_TOK_COMMA) || sl_p_at(p, SL_TOK_SEMICOLON)) {
                p->pos++;
                continue;
            }
            if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_UNEXPECTED_TOKEN, &item_name) != 0) {
                return -1;
            }
            item = sl_p_new_node(p, SL_AST_FIELD, item_name->start, item_name->end);
            if (item < 0) {
                return -1;
            }
            p->nodes[item].data_start = item_name->start;
            p->nodes[item].data_end = item_name->end;
            if (sl_p_match(p, SL_TOK_ASSIGN)) {
                int32_t vexpr;
                if (sl_p_parse_expr(p, 1, &vexpr) != 0) {
                    return -1;
                }
                if (sl_p_add_child(p, item, vexpr) != 0) {
                    return -1;
                }
                p->nodes[item].end = p->nodes[vexpr].end;
            }
            if (sl_p_add_child(p, n, item) != 0) {
                return -1;
            }
            if (sl_p_match(p, SL_TOK_COMMA) || sl_p_match(p, SL_TOK_SEMICOLON)) {
                continue;
            }
        }
    } else {
        if (sl_p_parse_field_list(p, n) != 0) {
            return -1;
        }
    }

    if (sl_p_expect(p, SL_TOK_RBRACE, SL_DIAG_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[n].end = rb->end;
    *out = n;
    return 0;
}

static int sl_p_parse_fun_decl(sl_parser* p, int allow_body, int32_t* out) {
    const sl_token* kw = sl_p_peek(p);
    const sl_token* name;
    const sl_token* t;
    int32_t fn;

    p->pos++;
    if (sl_p_expect(p, SL_TOK_IDENT, SL_DIAG_UNEXPECTED_TOKEN, &name) != 0) {
        return -1;
    }
    if (sl_p_expect(p, SL_TOK_LPAREN, SL_DIAG_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }

    fn = sl_p_new_node(p, SL_AST_FUN, kw->start, name->end);
    if (fn < 0) {
        return -1;
    }
    p->nodes[fn].data_start = name->start;
    p->nodes[fn].data_end = name->end;

    if (!sl_p_at(p, SL_TOK_RPAREN)) {
        for (;;) {
            int32_t param;
            if (sl_p_parse_param(p, &param) != 0) {
                return -1;
            }
            if (sl_p_add_child(p, fn, param) != 0) {
                return -1;
            }
            if (!sl_p_match(p, SL_TOK_COMMA)) {
                break;
            }
        }
    }

    if (sl_p_expect(p, SL_TOK_RPAREN, SL_DIAG_UNEXPECTED_TOKEN, &t) != 0) {
        return -1;
    }
    p->nodes[fn].end = t->end;

    if (!sl_p_at(p, SL_TOK_LBRACE) && !sl_p_at(p, SL_TOK_SEMICOLON)) {
        int32_t ret_type;
        if (sl_p_parse_type(p, &ret_type) != 0) {
            return -1;
        }
        p->nodes[ret_type].flags = 1;
        if (sl_p_add_child(p, fn, ret_type) != 0) {
            return -1;
        }
        p->nodes[fn].end = p->nodes[ret_type].end;
    }

    if (sl_p_at(p, SL_TOK_LBRACE)) {
        int32_t body;
        if (!allow_body) {
            return sl_p_fail(p, SL_DIAG_UNEXPECTED_TOKEN);
        }
        if (sl_p_parse_block(p, &body) != 0) {
            return -1;
        }
        if (sl_p_add_child(p, fn, body) != 0) {
            return -1;
        }
        p->nodes[fn].end = p->nodes[body].end;
    } else {
        if (sl_p_expect(p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &t) != 0) {
            return -1;
        }
        p->nodes[fn].end = t->end;
    }

    *out = fn;
    return 0;
}

static int sl_p_parse_import(sl_parser* p, int32_t* out) {
    const sl_token* kw = sl_p_peek(p);
    const sl_token* alias = NULL;
    const sl_token* path;
    int32_t n;
    p->pos++;

    if (sl_p_at(p, SL_TOK_IDENT)) {
        if ((p->pos + 1u) < p->tok_len && p->tok[p->pos + 1u].kind == SL_TOK_STRING) {
            alias = sl_p_peek(p);
            p->pos++;
        }
    }

    if (sl_p_expect(p, SL_TOK_STRING, SL_DIAG_UNEXPECTED_TOKEN, &path) != 0) {
        return -1;
    }

    n = sl_p_new_node(p, SL_AST_IMPORT, kw->start, path->end);
    if (n < 0) {
        return -1;
    }
    p->nodes[n].data_start = path->start;
    p->nodes[n].data_end = path->end;

    if (alias != NULL) {
        int32_t alias_node = sl_p_new_node(p, SL_AST_IDENT, alias->start, alias->end);
        if (alias_node < 0) {
            return -1;
        }
        p->nodes[alias_node].data_start = alias->start;
        p->nodes[alias_node].data_end = alias->end;
        if (sl_p_add_child(p, n, alias_node) != 0) {
            return -1;
        }
    }
    if (sl_p_expect(p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
        return -1;
    }
    p->nodes[n].end = kw->end;
    *out = n;
    return 0;
}

static int sl_p_parse_pub_block(sl_parser* p, int32_t* out) {
    const sl_token* kw = sl_p_peek(p);
    const sl_token* rb;
    int32_t n;
    p->pos++;
    if (sl_p_expect(p, SL_TOK_LBRACE, SL_DIAG_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    n = sl_p_new_node(p, SL_AST_PUB, kw->start, rb->end);
    if (n < 0) {
        return -1;
    }

    while (!sl_p_at(p, SL_TOK_RBRACE) && !sl_p_at(p, SL_TOK_EOF)) {
        int32_t decl;
        if (sl_p_at(p, SL_TOK_SEMICOLON)) {
            p->pos++;
            continue;
        }
        if (sl_p_parse_decl(p, 0, &decl) != 0) {
            return -1;
        }
        if (sl_p_add_child(p, n, decl) != 0) {
            return -1;
        }
    }

    if (sl_p_expect(p, SL_TOK_RBRACE, SL_DIAG_UNEXPECTED_TOKEN, &rb) != 0) {
        return -1;
    }
    p->nodes[n].end = rb->end;
    *out = n;
    return 0;
}

static int sl_p_parse_decl(sl_parser* p, int allow_body, int32_t* out) {
    switch (sl_p_peek(p)->kind) {
        case SL_TOK_FUN:
            return sl_p_parse_fun_decl(p, allow_body, out);
        case SL_TOK_STRUCT:
        case SL_TOK_UNION:
        case SL_TOK_ENUM:
            if (sl_p_parse_aggregate_decl(p, out) != 0) {
                return -1;
            }
            if (sl_p_match(p, SL_TOK_SEMICOLON)) {
                p->nodes[*out].end = sl_p_prev(p)->end;
            }
            return 0;
        case SL_TOK_CONST:
            return sl_p_parse_var_like_stmt(p, SL_AST_CONST, 1, out);
        case SL_TOK_PUB:
            return sl_p_parse_pub_block(p, out);
        default:
            return sl_p_fail(p, SL_DIAG_EXPECTED_DECL);
    }
}

const char* sl_ast_kind_name(sl_ast_kind kind) {
    switch (kind) {
        case SL_AST_FILE:
            return "FILE";
        case SL_AST_PACKAGE:
            return "PACKAGE";
        case SL_AST_IMPORT:
            return "IMPORT";
        case SL_AST_PUB:
            return "PUB";
        case SL_AST_FUN:
            return "FUN";
        case SL_AST_PARAM:
            return "PARAM";
        case SL_AST_TYPE_NAME:
            return "TYPE_NAME";
        case SL_AST_TYPE_PTR:
            return "TYPE_PTR";
        case SL_AST_TYPE_ARRAY:
            return "TYPE_ARRAY";
        case SL_AST_STRUCT:
            return "STRUCT";
        case SL_AST_UNION:
            return "UNION";
        case SL_AST_ENUM:
            return "ENUM";
        case SL_AST_FIELD:
            return "FIELD";
        case SL_AST_BLOCK:
            return "BLOCK";
        case SL_AST_VAR:
            return "VAR";
        case SL_AST_CONST:
            return "CONST";
        case SL_AST_IF:
            return "IF";
        case SL_AST_FOR:
            return "FOR";
        case SL_AST_RETURN:
            return "RETURN";
        case SL_AST_BREAK:
            return "BREAK";
        case SL_AST_CONTINUE:
            return "CONTINUE";
        case SL_AST_DEFER:
            return "DEFER";
        case SL_AST_EXPR_STMT:
            return "EXPR_STMT";
        case SL_AST_IDENT:
            return "IDENT";
        case SL_AST_INT:
            return "INT";
        case SL_AST_FLOAT:
            return "FLOAT";
        case SL_AST_STRING:
            return "STRING";
        case SL_AST_BOOL:
            return "BOOL";
        case SL_AST_UNARY:
            return "UNARY";
        case SL_AST_BINARY:
            return "BINARY";
        case SL_AST_CALL:
            return "CALL";
        case SL_AST_INDEX:
            return "INDEX";
        case SL_AST_FIELD_EXPR:
            return "FIELD_EXPR";
        case SL_AST_CAST:
            return "CAST";
    }
    return "UNKNOWN";
}

int sl_parse(sl_arena* arena, sl_strview src, sl_ast* out, sl_diag* diag) {
    sl_token_stream ts;
    sl_parser p;
    int32_t root;
    const sl_token* kw;
    const sl_token* pkg_name;

    sl_diag_clear(diag);
    out->nodes = NULL;
    out->len = 0;
    out->root = -1;

    if (sl_lex(arena, src, &ts, diag) != 0) {
        return -1;
    }

    p.src = src;
    p.tok = ts.v;
    p.tok_len = ts.len;
    p.pos = 0;
    p.node_len = 0;
    p.node_cap = ts.len * 4u + 16u;
    p.diag = diag;
    p.nodes = (sl_ast_node*)sl_arena_alloc(arena, p.node_cap * (uint32_t)sizeof(sl_ast_node),
                                           (uint32_t)_Alignof(sl_ast_node));
    if (p.nodes == NULL) {
        sl_set_diag(diag, SL_DIAG_ARENA_OOM, 0, 0);
        return -1;
    }

    root = sl_p_new_node(&p, SL_AST_FILE, 0, src.len);
    if (root < 0) {
        return -1;
    }

    if (sl_p_expect(&p, SL_TOK_PACKAGE, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
        return -1;
    }
    if (sl_p_expect(&p, SL_TOK_IDENT, SL_DIAG_UNEXPECTED_TOKEN, &pkg_name) != 0) {
        return -1;
    }
    {
        int32_t pkg = sl_p_new_node(&p, SL_AST_PACKAGE, kw->start, pkg_name->end);
        if (pkg < 0) {
            return -1;
        }
        p.nodes[pkg].data_start = pkg_name->start;
        p.nodes[pkg].data_end = pkg_name->end;
        if (sl_p_add_child(&p, root, pkg) != 0) {
            return -1;
        }
    }
    if (sl_p_expect(&p, SL_TOK_SEMICOLON, SL_DIAG_UNEXPECTED_TOKEN, &kw) != 0) {
        return -1;
    }

    while (sl_p_at(&p, SL_TOK_IMPORT)) {
        int32_t imp;
        if (sl_p_parse_import(&p, &imp) != 0) {
            return -1;
        }
        if (sl_p_add_child(&p, root, imp) != 0) {
            return -1;
        }
    }

    while (!sl_p_at(&p, SL_TOK_EOF)) {
        int32_t decl;
        if (sl_p_at(&p, SL_TOK_SEMICOLON)) {
            p.pos++;
            continue;
        }
        if (sl_p_parse_decl(&p, 1, &decl) != 0) {
            return -1;
        }
        if (sl_p_add_child(&p, root, decl) != 0) {
            return -1;
        }
    }

    out->nodes = p.nodes;
    out->len = p.node_len;
    out->root = root;
    return 0;
}

static void sl_w_write(sl_writer* w, const char* s, uint32_t len) {
    w->write(w->ctx, s, len);
}

static void sl_w_cstr(sl_writer* w, const char* s) {
    uint32_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    sl_w_write(w, s, n);
}

static void sl_w_u32(sl_writer* w, uint32_t v) {
    char buf[16];
    uint32_t n = 0;
    if (v == 0) {
        sl_w_write(w, "0", 1);
        return;
    }
    while (v > 0 && n < (uint32_t)sizeof(buf)) {
        buf[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        n--;
        sl_w_write(w, &buf[n], 1);
    }
}

static void sl_w_indent(sl_writer* w, uint32_t depth) {
    uint32_t i;
    for (i = 0; i < depth; i++) {
        sl_w_write(w, "  ", 2);
    }
}

static void sl_w_escaped(sl_writer* w, sl_strview src, uint32_t start, uint32_t end) {
    uint32_t i;
    sl_w_write(w, "\"", 1);
    for (i = start; i < end && i < src.len; i++) {
        unsigned char c = (unsigned char)src.ptr[i];
        switch (c) {
            case '\"':
                sl_w_write(w, "\\\"", 2);
                break;
            case '\\':
                sl_w_write(w, "\\\\", 2);
                break;
            case '\n':
                sl_w_write(w, "\\n", 2);
                break;
            case '\r':
                sl_w_write(w, "\\r", 2);
                break;
            case '\t':
                sl_w_write(w, "\\t", 2);
                break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    sl_w_write(w, (const char*)&src.ptr[i], 1);
                } else {
                    char hex[4];
                    static const char* digits = "0123456789abcdef";
                    hex[0] = '\\';
                    hex[1] = 'x';
                    hex[2] = digits[(c >> 4) & 0x0f];
                    hex[3] = digits[c & 0x0f];
                    sl_w_write(w, hex, 4);
                }
                break;
        }
    }
    sl_w_write(w, "\"", 1);
}

static int sl_ast_dump_node(const sl_ast* ast, int32_t idx, uint32_t depth, sl_strview src,
                            sl_writer* w) {
    const sl_ast_node* n;
    int32_t c;
    if (idx < 0 || (uint32_t)idx >= ast->len) {
        return -1;
    }
    n = &ast->nodes[idx];
    sl_w_indent(w, depth);
    sl_w_cstr(w, sl_ast_kind_name(n->kind));

    if (n->op != 0) {
        sl_w_cstr(w, " op=");
        sl_w_cstr(w, sl_token_kind_name((sl_token_kind)n->op));
    }
    if (n->flags != 0) {
        sl_w_cstr(w, " flags=");
        sl_w_u32(w, n->flags);
    }
    if (n->data_end > n->data_start) {
        sl_w_cstr(w, " ");
        sl_w_escaped(w, src, n->data_start, n->data_end);
    }
    sl_w_cstr(w, " [");
    sl_w_u32(w, n->start);
    sl_w_cstr(w, ",");
    sl_w_u32(w, n->end);
    sl_w_cstr(w, "]\n");

    c = n->first_child;
    while (c >= 0) {
        if (sl_ast_dump_node(ast, c, depth + 1, src, w) != 0) {
            return -1;
        }
        c = ast->nodes[c].next_sibling;
    }
    return 0;
}

int sl_ast_dump(const sl_ast* ast, sl_strview src, sl_writer* w, sl_diag* diag) {
    sl_diag_clear(diag);
    if (ast == NULL || w == NULL || w->write == NULL || ast->nodes == NULL || ast->root < 0) {
        sl_set_diag(diag, SL_DIAG_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    return sl_ast_dump_node(ast, ast->root, 0, src, w);
}
