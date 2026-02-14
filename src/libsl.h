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
} SLStrView;

typedef struct {
    void* ctx;
    void (*write)(void* ctx, const char* data, uint32_t len);
} SLWriter;

typedef struct {
    uint8_t* mem;
    uint32_t cap;
    uint32_t len;
} SLArena;

void SLArenaInit(SLArena* arena, void* storage, uint32_t storageSize);
void SLArenaReset(SLArena* arena);
void* _Nullable SLArenaAlloc(SLArena* arena, uint32_t size, uint32_t align);

typedef enum {
    SLDiag_NONE = 0,
    SLDiag_ARENA_OOM,
    SLDiag_UNEXPECTED_CHAR,
    SLDiag_UNTERMINATED_STRING,
    SLDiag_INVALID_NUMBER,
    SLDiag_UNEXPECTED_TOKEN,
    SLDiag_EXPECTED_DECL,
    SLDiag_EXPECTED_EXPR,
    SLDiag_EXPECTED_TYPE,
} SLDiagCode;

typedef struct {
    SLDiagCode code;
    uint32_t start;
    uint32_t end;
} SLDiag;

void SLDiagClear(SLDiag* diag);
const char* SLDiagMessage(SLDiagCode code);

typedef enum {
    SLTok_INVALID = 0,
    SLTok_EOF,
    SLTok_IDENT,
    SLTok_INT,
    SLTok_FLOAT,
    SLTok_STRING,

    SLTok_PACKAGE,
    SLTok_IMPORT,
    SLTok_PUB,
    SLTok_STRUCT,
    SLTok_UNION,
    SLTok_ENUM,
    SLTok_FUN,
    SLTok_VAR,
    SLTok_CONST,
    SLTok_IF,
    SLTok_ELSE,
    SLTok_FOR,
    SLTok_SWITCH,
    SLTok_CASE,
    SLTok_DEFAULT,
    SLTok_BREAK,
    SLTok_CONTINUE,
    SLTok_RETURN,
    SLTok_DEFER,
    SLTok_ASSERT,
    SLTok_TRUE,
    SLTok_FALSE,
    SLTok_AS,

    SLTok_LPAREN,
    SLTok_RPAREN,
    SLTok_LBRACE,
    SLTok_RBRACE,
    SLTok_LBRACK,
    SLTok_RBRACK,
    SLTok_COMMA,
    SLTok_DOT,
    SLTok_SEMICOLON,
    SLTok_COLON,

    SLTok_ASSIGN,
    SLTok_ADD,
    SLTok_SUB,
    SLTok_MUL,
    SLTok_DIV,
    SLTok_MOD,
    SLTok_AND,
    SLTok_OR,
    SLTok_XOR,
    SLTok_NOT,
    SLTok_LSHIFT,
    SLTok_RSHIFT,

    SLTok_EQ,
    SLTok_NEQ,
    SLTok_LT,
    SLTok_GT,
    SLTok_LTE,
    SLTok_GTE,
    SLTok_LOGICAL_AND,
    SLTok_LOGICAL_OR,

    SLTok_ADD_ASSIGN,
    SLTok_SUB_ASSIGN,
    SLTok_MUL_ASSIGN,
    SLTok_DIV_ASSIGN,
    SLTok_MOD_ASSIGN,
    SLTok_AND_ASSIGN,
    SLTok_OR_ASSIGN,
    SLTok_XOR_ASSIGN,
    SLTok_LSHIFT_ASSIGN,
    SLTok_RSHIFT_ASSIGN,
} SLTokenKind;

typedef struct {
    SLTokenKind kind;
    uint32_t start;
    uint32_t end;
} SLToken;

typedef struct {
    const SLToken* v;
    uint32_t len;
} SLTokenStream;

typedef enum {
    SLAST_FILE = 0,
    SLAST_PACKAGE,
    SLAST_IMPORT,
    SLAST_PUB,
    SLAST_FUN,
    SLAST_PARAM,
    SLAST_TYPE_NAME,
    SLAST_TYPE_PTR,
    SLAST_TYPE_ARRAY,
    SLAST_STRUCT,
    SLAST_UNION,
    SLAST_ENUM,
    SLAST_FIELD,
    SLAST_BLOCK,
    SLAST_VAR,
    SLAST_CONST,
    SLAST_IF,
    SLAST_FOR,
    SLAST_RETURN,
    SLAST_BREAK,
    SLAST_CONTINUE,
    SLAST_DEFER,
    SLAST_EXPR_STMT,
    SLAST_IDENT,
    SLAST_INT,
    SLAST_FLOAT,
    SLAST_STRING,
    SLAST_BOOL,
    SLAST_UNARY,
    SLAST_BINARY,
    SLAST_CALL,
    SLAST_INDEX,
    SLAST_FIELD_EXPR,
    SLAST_CAST,
} SLASTKind;

typedef struct {
    SLASTKind kind;
    uint32_t start;
    uint32_t end;
    int32_t firstChild;
    int32_t nextSibling;
    uint32_t dataStart;
    uint32_t dataEnd;
    uint16_t op;
    uint16_t flags;
} SLASTNode;

typedef struct {
    const SLASTNode* nodes;
    uint32_t len;
    int32_t root;
} SLAST;

const char* SLTokenKindName(SLTokenKind kind);
const char* SLASTKindName(SLASTKind kind);

// Tokenize src into arena memory and return a view over tokens.
// Returns 0 on success, -1 on failure. On failure, diag is set.
int SLLex(SLArena* arena, SLStrView src, SLTokenStream* out, SLDiag* diag);
int SLParse(SLArena* arena, SLStrView src, SLAST* out, SLDiag* diag);
int SLASTDump(const SLAST* ast, SLStrView src, SLWriter* w, SLDiag* diag);

SL_API_END
