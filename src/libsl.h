/* libsl version ${version} <https://github.com/rsms/slang>
////////////////////////////////////////////////////////////////////////////////
${license}
////////////////////////////////////////////////////////////////////////////////
*/
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "compiler.h"
#include "version.h"

SL_API_BEGIN

typedef struct {
    const char* ptr;
    uint32_t len;
} sl_strview;

typedef struct {
    uint8_t* mem;
    uint32_t cap;
    uint32_t len;
} sl_arena;

void sl_arena_init(sl_arena* arena, void* storage, uint32_t storage_size);
void sl_arena_reset(sl_arena* arena);
void* _Nullable sl_arena_alloc(sl_arena* arena, uint32_t size, uint32_t align);

typedef enum {
    SL_DIAG_NONE = 0,
    SL_DIAG_ARENA_OOM,
    SL_DIAG_UNEXPECTED_CHAR,
    SL_DIAG_UNTERMINATED_STRING,
    SL_DIAG_INVALID_NUMBER,
} sl_diag_code;

typedef struct {
    sl_diag_code code;
    uint32_t start;
    uint32_t end;
} sl_diag;

void sl_diag_clear(sl_diag* diag);
const char* sl_diag_message(sl_diag_code code);

typedef enum {
    SL_TOK_INVALID = 0,
    SL_TOK_EOF,
    SL_TOK_IDENT,
    SL_TOK_INT,
    SL_TOK_FLOAT,
    SL_TOK_STRING,

    SL_TOK_PACKAGE,
    SL_TOK_IMPORT,
    SL_TOK_PUB,
    SL_TOK_STRUCT,
    SL_TOK_UNION,
    SL_TOK_ENUM,
    SL_TOK_FUN,
    SL_TOK_VAR,
    SL_TOK_CONST,
    SL_TOK_IF,
    SL_TOK_ELSE,
    SL_TOK_FOR,
    SL_TOK_SWITCH,
    SL_TOK_CASE,
    SL_TOK_DEFAULT,
    SL_TOK_BREAK,
    SL_TOK_CONTINUE,
    SL_TOK_RETURN,
    SL_TOK_DEFER,
    SL_TOK_ASSERT,
    SL_TOK_TRUE,
    SL_TOK_FALSE,

    SL_TOK_LPAREN,
    SL_TOK_RPAREN,
    SL_TOK_LBRACE,
    SL_TOK_RBRACE,
    SL_TOK_LBRACK,
    SL_TOK_RBRACK,
    SL_TOK_COMMA,
    SL_TOK_DOT,
    SL_TOK_SEMICOLON,
    SL_TOK_COLON,

    SL_TOK_ASSIGN,
    SL_TOK_ADD,
    SL_TOK_SUB,
    SL_TOK_MUL,
    SL_TOK_DIV,
    SL_TOK_MOD,
    SL_TOK_AND,
    SL_TOK_OR,
    SL_TOK_XOR,
    SL_TOK_NOT,
    SL_TOK_LSHIFT,
    SL_TOK_RSHIFT,

    SL_TOK_EQ,
    SL_TOK_NEQ,
    SL_TOK_LT,
    SL_TOK_GT,
    SL_TOK_LTE,
    SL_TOK_GTE,
    SL_TOK_LOGICAL_AND,
    SL_TOK_LOGICAL_OR,

    SL_TOK_ADD_ASSIGN,
    SL_TOK_SUB_ASSIGN,
    SL_TOK_MUL_ASSIGN,
    SL_TOK_DIV_ASSIGN,
    SL_TOK_MOD_ASSIGN,
    SL_TOK_AND_ASSIGN,
    SL_TOK_OR_ASSIGN,
    SL_TOK_XOR_ASSIGN,
    SL_TOK_LSHIFT_ASSIGN,
    SL_TOK_RSHIFT_ASSIGN,
} sl_token_kind;

typedef struct {
    sl_token_kind kind;
    uint32_t start;
    uint32_t end;
} sl_token;

typedef struct {
    const sl_token* v;
    uint32_t len;
} sl_token_stream;

const char* sl_token_kind_name(sl_token_kind kind);

// Tokenize src into arena memory and return a view over tokens.
// Returns 0 on success, -1 on failure. On failure, diag is set.
int sl_lex(sl_arena* arena, sl_strview src, sl_token_stream* out, sl_diag* diag);

SL_API_END
