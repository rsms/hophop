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
