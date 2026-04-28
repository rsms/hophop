/* libhop version ${version} <https://github.com/rsms/hophop>
////////////////////////////////////////////////////////////////////////////////
${license}
////////////////////////////////////////////////////////////////////////////////
*/
#ifndef H2_LIBHOP_H
#define H2_LIBHOP_H

#pragma once

#include <stddef.h>
#include <stdint.h>

////////////////////////////////////////////////////////////////////////////////////////////////////

// H2_VERSION is incremented for every release
#define H2_VERSION 1
#ifndef H2_SOURCE_HASH
    #define H2_SOURCE_HASH "src"
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

#ifndef __has_include
    #define __has_include(...) 0
#endif
#ifndef __has_extension
    #define __has_extension(...) 0
#endif
#ifndef __has_feature
    #define __has_feature(...) 0
#endif
#ifndef __has_attribute
    #define __has_attribute(...) 0
#endif

#ifndef _Noreturn
    #define _Noreturn __attribute__((noreturn))
#endif

#if defined(__clang__)
    #define H2_ASSUME_NONNULL_BEGIN                                                    \
        _Pragma("clang diagnostic push");                                              \
        _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"");            \
        _Pragma("clang diagnostic ignored \"-Wnullability-inferred-on-nested-type\""); \
        _Pragma("clang assume_nonnull begin"); // assume T* means "T* _Nonnull"
    #define H2_ASSUME_NONNULL_END        \
        _Pragma("clang diagnostic pop"); \
        _Pragma("clang assume_nonnull end");
#else
    #define H2_ASSUME_NONNULL_BEGIN
    #define H2_ASSUME_NONNULL_END
#endif

#ifdef __cplusplus
    #define H2_API_BEGIN H2_ASSUME_NONNULL_BEGIN extern "C" {
    #define H2_API_END \
        }              \
        H2_ASSUME_NONNULL_END
#else
    #define H2_API_BEGIN H2_ASSUME_NONNULL_BEGIN
    #define H2_API_END   H2_ASSUME_NONNULL_END
#endif

#if !__has_feature(nullability)
    #define _Nullable
    #define _Nonnull
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

typedef enum {
#include "diagnostics_enum.inc"
    H2Diag__COUNT,
} H2DiagCode;

H2_API_BEGIN

typedef enum {
    H2DiagType_ERROR = 0,
    H2DiagType_WARNING = 1,
} H2DiagType;

typedef enum {
    H2DiagPhase_UNKNOWN = 0,
    H2DiagPhase_LEX,
    H2DiagPhase_PARSE,
    H2DiagPhase_RESOLVE,
    H2DiagPhase_TYPECHECK,
    H2DiagPhase_CONSTEVAL,
    H2DiagPhase_MIR,
    H2DiagPhase_CODEGEN_C,
    H2DiagPhase_CODEGEN_WASM,
    H2DiagPhase_COMPILER,
} H2DiagPhase;

typedef enum {
    H2DiagNoteKind_RELATED = 0,
    H2DiagNoteKind_PREVIOUS_DEFINITION,
    H2DiagNoteKind_REQUIRED_BY,
    H2DiagNoteKind_INFERRED_FROM,
    H2DiagNoteKind_INSTANTIATED_FROM,
    H2DiagNoteKind_CALLED_FROM,
    H2DiagNoteKind_IMPORTED_HERE,
    H2DiagNoteKind_CANDIDATE,
    H2DiagNoteKind_BECAUSE_OF,
} H2DiagNoteKind;

typedef enum {
    H2DiagFixItKind_REPLACE = 0,
    H2DiagFixItKind_INSERT,
    H2DiagFixItKind_DELETE,
} H2DiagFixItKind;

typedef enum {
    H2DiagExpectationKind_TOKEN = 0,
    H2DiagExpectationKind_DECL_FORM,
    H2DiagExpectationKind_EXPR_FORM,
    H2DiagExpectationKind_TYPE_KIND,
    H2DiagExpectationKind_SYMBOL_KIND,
    H2DiagExpectationKind_ARG_SHAPE,
} H2DiagExpectationKind;

typedef struct {
    const char* _Nullable ptr;
    uint32_t len;
} H2StrView;

typedef struct {
    void* _Nullable ctx;
    void (*write)(void* _Nullable ctx, const char* data, uint32_t len);
} H2Writer;

typedef struct {
    H2DiagNoteKind kind;
    uint32_t       start;
    uint32_t       end;
    const char* _Nullable message;
    const char* _Nullable path;
    const char* _Nullable source;
} H2DiagNote;

typedef struct {
    H2DiagFixItKind kind;
    uint32_t        start;
    uint32_t        end;
    const char* _Nullable text;
} H2DiagFixIt;

typedef struct {
    H2DiagExpectationKind kind;
    const char* _Nullable text;
} H2DiagExpectation;

typedef void* _Nullable (*H2ArenaGrowFn)(
    void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize);
typedef void (*H2ArenaFreeFn)(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize);

typedef struct H2ArenaBlock H2ArenaBlock;
struct H2ArenaBlock {
    uint8_t* _Nullable mem;
    uint32_t cap;
    uint32_t len;
    uint32_t allocSize;
    H2ArenaBlock* _Nullable next;
    uint8_t owned;
};

typedef struct {
    void* _Nullable allocatorCtx;
    H2ArenaGrowFn _Nullable grow;
    H2ArenaFreeFn _Nullable free;
    H2ArenaBlock inlineBlock;
    H2ArenaBlock* _Nullable first;
    H2ArenaBlock* _Nullable current;
} H2Arena;

void H2ArenaInit(H2Arena* arena, void* storage, uint32_t storageSize);
void H2ArenaInitEx(
    H2Arena* arena,
    void* _Nullable storage,
    uint32_t storageSize,
    void* _Nullable allocatorCtx,
    H2ArenaGrowFn _Nullable growFn,
    H2ArenaFreeFn _Nullable freeFn);
void H2ArenaSetAllocator(
    H2Arena* arena,
    void* _Nullable allocatorCtx,
    H2ArenaGrowFn _Nullable growFn,
    H2ArenaFreeFn _Nullable freeFn);
void H2ArenaReset(H2Arena* arena);
void H2ArenaDispose(H2Arena* arena);
void* _Nullable H2ArenaAlloc(H2Arena* arena, uint32_t size, uint32_t align);

typedef struct {
    H2DiagCode code;
    H2DiagType type;
    uint32_t   start;
    uint32_t   end;
    uint32_t   argStart;
    uint32_t   argEnd;
    const char* _Nullable argText;
    uint32_t argTextLen;
    uint32_t arg2Start;
    uint32_t arg2End;
    const char* _Nullable arg2Text;
    uint32_t arg2TextLen;
    uint32_t relatedStart;
    uint32_t relatedEnd;
    const char* _Nullable detail;
    const char* _Nullable hintOverride;
    H2DiagPhase phase;
    uint32_t    groupId;
    uint8_t     isPrimary;
    uint8_t     _reserved[3];
    const H2DiagNote* _Nullable notes;
    uint32_t notesLen;
    const H2DiagFixIt* _Nullable fixIts;
    uint32_t fixItsLen;
    const H2DiagExpectation* _Nullable expectations;
    uint32_t expectationsLen;
} H2Diag;

typedef void (*H2DiagSinkFn)(void* _Nullable ctx, const H2Diag* _Nonnull diag);

typedef struct {
    void* _Nullable ctx;
    H2DiagSinkFn _Nullable onDiag;
    uint32_t flags;
    const char* _Nullable filePath;
} H2TypeCheckOptions;

void        H2DiagReset(H2Diag* _Nullable diag, H2DiagCode code);
void        H2DiagClear(H2Diag* _Nullable diag);
const char* H2DiagId(H2DiagCode code);
const char* H2DiagMessage(H2DiagCode code);
const char* _Nullable H2DiagHint(H2DiagCode code);
H2DiagType H2DiagTypeOfCode(H2DiagCode code);
uint8_t    H2DiagArgCount(H2DiagCode code);
int        H2DiagAddNote(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagNoteKind kind,
    uint32_t       start,
    uint32_t       end,
    const char* _Nullable message);
int H2DiagAddNoteEx(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagNoteKind kind,
    uint32_t       start,
    uint32_t       end,
    const char* _Nullable message,
    const char* _Nullable path,
    const char* _Nullable source);
int H2DiagAddFixIt(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagFixItKind kind,
    uint32_t        start,
    uint32_t        end,
    const char* _Nullable text);
int H2DiagAddExpectation(
    H2Arena* _Nullable arena,
    H2Diag* _Nullable diag,
    H2DiagExpectationKind kind,
    const char* _Nullable text);

typedef enum {
    H2Tok_INVALID = 0,
    H2Tok_EOF,
    H2Tok_IDENT,
    H2Tok_INT,
    H2Tok_FLOAT,
    H2Tok_STRING,
    H2Tok_RUNE,

    H2Tok_IMPORT,
    H2Tok_PUB,
    H2Tok_STRUCT,
    H2Tok_UNION,
    H2Tok_ENUM,
    H2Tok_FN,
    H2Tok_VAR,
    H2Tok_CONST,
    H2Tok_TYPE,
    H2Tok_MUT,
    H2Tok_IF,
    H2Tok_ELSE,
    H2Tok_FOR,
    H2Tok_SWITCH,
    H2Tok_CASE,
    H2Tok_DEFAULT,
    H2Tok_BREAK,
    H2Tok_CONTINUE,
    H2Tok_RETURN,
    H2Tok_DEFER,
    H2Tok_ASSERT,
    H2Tok_SIZEOF,
    H2Tok_NEW,
    H2Tok_DEL,
    H2Tok_TRUE,
    H2Tok_FALSE,
    H2Tok_IN,
    H2Tok_AS,
    H2Tok_CONTEXT,
    H2Tok_ANYTYPE,

    H2Tok_LPAREN,
    H2Tok_RPAREN,
    H2Tok_LBRACE,
    H2Tok_RBRACE,
    H2Tok_LBRACK,
    H2Tok_RBRACK,
    H2Tok_COMMA,
    H2Tok_DOT,
    H2Tok_ELLIPSIS,
    H2Tok_SEMICOLON,
    H2Tok_COLON,
    H2Tok_AT,

    H2Tok_SHORT_ASSIGN,
    H2Tok_ASSIGN,
    H2Tok_ADD,
    H2Tok_SUB,
    H2Tok_MUL,
    H2Tok_DIV,
    H2Tok_MOD,
    H2Tok_AND,
    H2Tok_OR,
    H2Tok_XOR,
    H2Tok_NOT,
    H2Tok_LSHIFT,
    H2Tok_RSHIFT,

    H2Tok_EQ,
    H2Tok_NEQ,
    H2Tok_LT,
    H2Tok_GT,
    H2Tok_LTE,
    H2Tok_GTE,
    H2Tok_LOGICAL_AND,
    H2Tok_LOGICAL_OR,

    H2Tok_ADD_ASSIGN,
    H2Tok_SUB_ASSIGN,
    H2Tok_MUL_ASSIGN,
    H2Tok_DIV_ASSIGN,
    H2Tok_MOD_ASSIGN,
    H2Tok_AND_ASSIGN,
    H2Tok_OR_ASSIGN,
    H2Tok_XOR_ASSIGN,
    H2Tok_LSHIFT_ASSIGN,
    H2Tok_RSHIFT_ASSIGN,

    H2Tok_QUESTION,
    H2Tok_NULL,
} H2TokenKind;

typedef struct {
    H2TokenKind kind;
    uint32_t    start;
    uint32_t    end;
} H2Token;

typedef struct {
    const H2Token* _Nullable v;
    uint32_t len;
} H2TokenStream;

typedef enum {
    H2Ast_FILE = 0,
    H2Ast_IMPORT,
    H2Ast_IMPORT_SYMBOL,
    H2Ast_DIRECTIVE,
    H2Ast_PUB,
    H2Ast_FN,
    H2Ast_PARAM,
    H2Ast_TYPE_PARAM,
    H2Ast_CONTEXT_CLAUSE,
    H2Ast_TYPE_NAME,
    H2Ast_TYPE_PTR,
    H2Ast_TYPE_REF,
    H2Ast_TYPE_MUTREF,
    H2Ast_TYPE_ARRAY,
    H2Ast_TYPE_VARRAY,
    H2Ast_TYPE_SLICE,
    H2Ast_TYPE_MUTSLICE,
    H2Ast_TYPE_OPTIONAL,
    H2Ast_TYPE_FN,
    H2Ast_TYPE_ALIAS,
    H2Ast_TYPE_ANON_STRUCT,
    H2Ast_TYPE_ANON_UNION,
    H2Ast_TYPE_TUPLE,
    H2Ast_STRUCT,
    H2Ast_UNION,
    H2Ast_ENUM,
    H2Ast_FIELD,
    H2Ast_BLOCK,
    H2Ast_VAR,
    H2Ast_CONST,
    H2Ast_CONST_BLOCK,
    H2Ast_IF,
    H2Ast_FOR,
    H2Ast_SWITCH,
    H2Ast_CASE,
    H2Ast_CASE_PATTERN,
    H2Ast_DEFAULT,
    H2Ast_RETURN,
    H2Ast_BREAK,
    H2Ast_CONTINUE,
    H2Ast_DEFER,
    H2Ast_ASSERT,
    H2Ast_DEL,
    H2Ast_EXPR_STMT,
    H2Ast_MULTI_ASSIGN,
    H2Ast_SHORT_ASSIGN,
    H2Ast_NAME_LIST,
    H2Ast_EXPR_LIST,
    H2Ast_TUPLE_EXPR,
    H2Ast_TYPE_VALUE,
    H2Ast_IDENT,
    H2Ast_INT,
    H2Ast_FLOAT,
    H2Ast_STRING,
    H2Ast_RUNE,
    H2Ast_BOOL,
    H2Ast_UNARY,
    H2Ast_BINARY,
    H2Ast_CALL,
    H2Ast_CALL_ARG,
    H2Ast_CALL_WITH_CONTEXT,
    H2Ast_CONTEXT_OVERLAY,
    H2Ast_CONTEXT_BIND,
    H2Ast_COMPOUND_LIT,
    H2Ast_COMPOUND_FIELD,
    H2Ast_ARRAY_LIT,
    H2Ast_INDEX,
    H2Ast_FIELD_EXPR,
    H2Ast_CAST,
    H2Ast_SIZEOF,
    H2Ast_NEW,
    H2Ast_NULL,
    H2Ast_UNWRAP,
} H2AstKind;

typedef struct {
    uint32_t start;
    uint32_t end;
    int32_t  firstChild;
    int32_t  nextSibling;
    uint32_t dataStart;
    uint32_t dataEnd;
    uint16_t op;
    uint16_t kind;
    uint32_t flags;
} H2AstNode;

enum {
    H2AstFlag_PUB = 0x8000u,
    H2AstFlag_INDEX_SLICE = 0x0001u,
    H2AstFlag_INDEX_HAS_START = 0x0002u,
    H2AstFlag_INDEX_HAS_END = 0x0004u,
    H2AstFlag_INDEX_RUNTIME_BOUNDS = 0x0008u,
    H2AstFlag_FIELD_EMBEDDED = 0x0010u,
    H2AstFlag_CALL_WITH_CONTEXT_PASSTHROUGH = 0x0020u,
    H2AstFlag_NEW_HAS_COUNT = 0x0040u,
    H2AstFlag_NEW_HAS_ALLOC = 0x0080u,
    H2AstFlag_NEW_HAS_INIT = 0x0100u,
    H2AstFlag_PAREN = 0x0200u,
    H2AstFlag_COMPOUND_FIELD_SHORTHAND = 0x0400u,
    H2AstFlag_CONTEXT_BIND_SHORTHAND = 0x0800u,
    H2AstFlag_PARAM_VARIADIC = 0x1000u,
    H2AstFlag_CALL_ARG_SPREAD = 0x2000u,
    H2AstFlag_PARAM_CONST = 0x4000u,
    H2AstFlag_FOR_IN = 0x00010000u,
    H2AstFlag_FOR_IN_HAS_KEY = 0x00020000u,
    H2AstFlag_FOR_IN_KEY_REF = 0x00040000u,
    H2AstFlag_FOR_IN_VALUE_REF = 0x00080000u,
    H2AstFlag_FOR_IN_VALUE_DISCARD = 0x00200000u,
    H2AstFlag_DEL_HAS_ALLOC = 0x00400000u,
    H2AstFlag_NEW_HAS_ARRAY_LIT = 0x00800000u,
};

typedef uint32_t H2Features;
#define H2Feature_NONE     ((H2Features)0)
#define H2Feature_OPTIONAL ((H2Features)(1u << 0))

typedef enum {
    H2CommentAttachment_FLOATING = 0,
    H2CommentAttachment_LEADING = 1,
    H2CommentAttachment_TRAILING = 2,
} H2CommentAttachment;

typedef struct {
    uint32_t            start;
    uint32_t            end;
    uint32_t            textStart;
    uint32_t            textEnd;
    int32_t             anchorNode;
    int32_t             containerNode;
    H2CommentAttachment attachment;
    uint8_t             _reserved[3];
} H2Comment;

typedef struct {
    const H2AstNode* _Nullable nodes;
    uint32_t   len;
    int32_t    root;
    H2Features features;
} H2Ast;

typedef enum {
    H2ParseFlag_NONE = 0,
    H2ParseFlag_COLLECT_FORMATTING = 1u << 0,
} H2ParseFlag;

typedef struct {
    uint32_t flags;
} H2ParseOptions;

typedef struct {
    const H2Comment* _Nullable comments;
    uint32_t commentLen;
} H2ParseExtras;

typedef int (*H2FormatCanDropLiteralCastFn)(
    void* _Nullable ctx, const H2Ast* ast, H2StrView src, int32_t castNodeId);

typedef struct {
    void* _Nullable ctx;
    H2FormatCanDropLiteralCastFn _Nullable canDropLiteralCast;
    uint32_t flags;
    uint32_t indentWidth;
} H2FormatOptions;

const char* H2TokenKindName(H2TokenKind kind);
const char* H2AstKindName(H2AstKind kind);

// Normalize an import path.
// Returns 0 on success and writes the normalized path to `out` (NUL-terminated).
// Returns -1 on failure and sets `*outErrReason` when provided.
// Example reasons: "empty path", "absolute path", "invalid character".
int H2NormalizeImportPath(
    const char* importPath,
    char*       out,
    uint32_t    outCap,
    const char* _Nullable* _Nullable outErrReason);

// Tokenize src into arena memory and return a view over tokens.
// Returns 0 on success, -1 on failure. On failure, diag is set.
int H2Lex(H2Arena* arena, H2StrView src, H2TokenStream* out, H2Diag* _Nullable diag);
int H2Parse(
    H2Arena*  arena,
    H2StrView src,
    const H2ParseOptions* _Nullable options,
    H2Ast* out,
    H2ParseExtras* _Nullable outExtras,
    H2Diag* _Nullable diag);
int H2Format(
    H2Arena*  arena,
    H2StrView src,
    const H2FormatOptions* _Nullable options,
    H2StrView* out,
    H2Diag* _Nullable diag);
int H2TypeCheckEx(
    H2Arena*     arena,
    const H2Ast* ast,
    H2StrView    src,
    const H2TypeCheckOptions* _Nullable options,
    H2Diag* _Nullable diag);
int H2TypeCheck(H2Arena* arena, const H2Ast* ast, H2StrView src, H2Diag* _Nullable diag);
int H2AstDump(const H2Ast* ast, H2StrView src, H2Writer* w, H2Diag* _Nullable diag);

H2_API_END

#endif /* H2_LIBHOP_H */
