/* libhop version ${version} <https://github.com/rsms/hophop>
////////////////////////////////////////////////////////////////////////////////
${license}
////////////////////////////////////////////////////////////////////////////////
*/
#ifndef HOP_LIBHOP_H
#define HOP_LIBHOP_H

#pragma once

#include <stddef.h>
#include <stdint.h>

////////////////////////////////////////////////////////////////////////////////////////////////////

// HOP_VERSION is incremented for every release
#define HOP_VERSION 1
#ifndef HOP_SOURCE_HASH
    #define HOP_SOURCE_HASH "src"
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
    #define HOP_ASSUME_NONNULL_BEGIN                                                   \
        _Pragma("clang diagnostic push");                                              \
        _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"");            \
        _Pragma("clang diagnostic ignored \"-Wnullability-inferred-on-nested-type\""); \
        _Pragma("clang assume_nonnull begin"); // assume T* means "T* _Nonnull"
    #define HOP_ASSUME_NONNULL_END       \
        _Pragma("clang diagnostic pop"); \
        _Pragma("clang assume_nonnull end");
#else
    #define HOP_ASSUME_NONNULL_BEGIN
    #define HOP_ASSUME_NONNULL_END
#endif

#ifdef __cplusplus
    #define HOP_API_BEGIN HOP_ASSUME_NONNULL_BEGIN extern "C" {
    #define HOP_API_END \
        }               \
        HOP_ASSUME_NONNULL_END
#else
    #define HOP_API_BEGIN HOP_ASSUME_NONNULL_BEGIN
    #define HOP_API_END   HOP_ASSUME_NONNULL_END
#endif

#if !__has_feature(nullability)
    #define _Nullable
    #define _Nonnull
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

typedef enum {
#include "diagnostics_enum.inc"
    HOPDiag__COUNT,
} HOPDiagCode;

HOP_API_BEGIN

typedef enum {
    HOPDiagType_ERROR = 0,
    HOPDiagType_WARNING = 1,
} HOPDiagType;

typedef struct {
    const char* _Nullable ptr;
    uint32_t len;
} HOPStrView;

typedef struct {
    void* _Nullable ctx;
    void (*write)(void* _Nullable ctx, const char* data, uint32_t len);
} HOPWriter;

typedef void* _Nullable (*HOPArenaGrowFn)(
    void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize);
typedef void (*HOPArenaFreeFn)(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize);

typedef struct HOPArenaBlock HOPArenaBlock;
struct HOPArenaBlock {
    uint8_t* _Nullable mem;
    uint32_t cap;
    uint32_t len;
    uint32_t allocSize;
    HOPArenaBlock* _Nullable next;
    uint8_t owned;
};

typedef struct {
    void* _Nullable allocatorCtx;
    HOPArenaGrowFn _Nullable grow;
    HOPArenaFreeFn _Nullable free;
    HOPArenaBlock inlineBlock;
    HOPArenaBlock* _Nullable first;
    HOPArenaBlock* _Nullable current;
} HOPArena;

void HOPArenaInit(HOPArena* arena, void* storage, uint32_t storageSize);
void HOPArenaInitEx(
    HOPArena* arena,
    void* _Nullable storage,
    uint32_t storageSize,
    void* _Nullable allocatorCtx,
    HOPArenaGrowFn _Nullable growFn,
    HOPArenaFreeFn _Nullable freeFn);
void HOPArenaSetAllocator(
    HOPArena* arena,
    void* _Nullable allocatorCtx,
    HOPArenaGrowFn _Nullable growFn,
    HOPArenaFreeFn _Nullable freeFn);
void HOPArenaReset(HOPArena* arena);
void HOPArenaDispose(HOPArena* arena);
void* _Nullable HOPArenaAlloc(HOPArena* arena, uint32_t size, uint32_t align);

typedef struct {
    HOPDiagCode code;
    HOPDiagType type;
    uint32_t    start;
    uint32_t    end;
    uint32_t    argStart;
    uint32_t    argEnd;
    uint32_t    relatedStart;
    uint32_t    relatedEnd;
    const char* _Nullable detail;
    const char* _Nullable hintOverride;
} HOPDiag;

typedef void (*HOPDiagSinkFn)(void* _Nullable ctx, const HOPDiag* _Nonnull diag);

typedef struct {
    void* _Nullable ctx;
    HOPDiagSinkFn _Nullable onDiag;
    uint32_t flags;
} HOPTypeCheckOptions;

void        HOPDiagClear(HOPDiag* _Nullable diag);
const char* HOPDiagId(HOPDiagCode code);
const char* HOPDiagMessage(HOPDiagCode code);
const char* _Nullable HOPDiagHint(HOPDiagCode code);
HOPDiagType HOPDiagTypeOfCode(HOPDiagCode code);
uint8_t     HOPDiagArgCount(HOPDiagCode code);

typedef enum {
    HOPTok_INVALID = 0,
    HOPTok_EOF,
    HOPTok_IDENT,
    HOPTok_INT,
    HOPTok_FLOAT,
    HOPTok_STRING,
    HOPTok_RUNE,

    HOPTok_IMPORT,
    HOPTok_PUB,
    HOPTok_STRUCT,
    HOPTok_UNION,
    HOPTok_ENUM,
    HOPTok_FN,
    HOPTok_VAR,
    HOPTok_CONST,
    HOPTok_TYPE,
    HOPTok_MUT,
    HOPTok_IF,
    HOPTok_ELSE,
    HOPTok_FOR,
    HOPTok_SWITCH,
    HOPTok_CASE,
    HOPTok_DEFAULT,
    HOPTok_BREAK,
    HOPTok_CONTINUE,
    HOPTok_RETURN,
    HOPTok_DEFER,
    HOPTok_ASSERT,
    HOPTok_SIZEOF,
    HOPTok_NEW,
    HOPTok_DEL,
    HOPTok_TRUE,
    HOPTok_FALSE,
    HOPTok_IN,
    HOPTok_AS,
    HOPTok_CONTEXT,
    HOPTok_ANYTYPE,

    HOPTok_LPAREN,
    HOPTok_RPAREN,
    HOPTok_LBRACE,
    HOPTok_RBRACE,
    HOPTok_LBRACK,
    HOPTok_RBRACK,
    HOPTok_COMMA,
    HOPTok_DOT,
    HOPTok_ELLIPSIS,
    HOPTok_SEMICOLON,
    HOPTok_COLON,
    HOPTok_AT,

    HOPTok_SHORT_ASSIGN,
    HOPTok_ASSIGN,
    HOPTok_ADD,
    HOPTok_SUB,
    HOPTok_MUL,
    HOPTok_DIV,
    HOPTok_MOD,
    HOPTok_AND,
    HOPTok_OR,
    HOPTok_XOR,
    HOPTok_NOT,
    HOPTok_LSHIFT,
    HOPTok_RSHIFT,

    HOPTok_EQ,
    HOPTok_NEQ,
    HOPTok_LT,
    HOPTok_GT,
    HOPTok_LTE,
    HOPTok_GTE,
    HOPTok_LOGICAL_AND,
    HOPTok_LOGICAL_OR,

    HOPTok_ADD_ASSIGN,
    HOPTok_SUB_ASSIGN,
    HOPTok_MUL_ASSIGN,
    HOPTok_DIV_ASSIGN,
    HOPTok_MOD_ASSIGN,
    HOPTok_AND_ASSIGN,
    HOPTok_OR_ASSIGN,
    HOPTok_XOR_ASSIGN,
    HOPTok_LSHIFT_ASSIGN,
    HOPTok_RSHIFT_ASSIGN,

    HOPTok_QUESTION,
    HOPTok_NULL,
} HOPTokenKind;

typedef struct {
    HOPTokenKind kind;
    uint32_t     start;
    uint32_t     end;
} HOPToken;

typedef struct {
    const HOPToken* _Nullable v;
    uint32_t len;
} HOPTokenStream;

typedef enum {
    HOPAst_FILE = 0,
    HOPAst_IMPORT,
    HOPAst_IMPORT_SYMBOL,
    HOPAst_DIRECTIVE,
    HOPAst_PUB,
    HOPAst_FN,
    HOPAst_PARAM,
    HOPAst_TYPE_PARAM,
    HOPAst_CONTEXT_CLAUSE,
    HOPAst_TYPE_NAME,
    HOPAst_TYPE_PTR,
    HOPAst_TYPE_REF,
    HOPAst_TYPE_MUTREF,
    HOPAst_TYPE_ARRAY,
    HOPAst_TYPE_VARRAY,
    HOPAst_TYPE_SLICE,
    HOPAst_TYPE_MUTSLICE,
    HOPAst_TYPE_OPTIONAL,
    HOPAst_TYPE_FN,
    HOPAst_TYPE_ALIAS,
    HOPAst_TYPE_ANON_STRUCT,
    HOPAst_TYPE_ANON_UNION,
    HOPAst_TYPE_TUPLE,
    HOPAst_STRUCT,
    HOPAst_UNION,
    HOPAst_ENUM,
    HOPAst_FIELD,
    HOPAst_BLOCK,
    HOPAst_VAR,
    HOPAst_CONST,
    HOPAst_CONST_BLOCK,
    HOPAst_IF,
    HOPAst_FOR,
    HOPAst_SWITCH,
    HOPAst_CASE,
    HOPAst_CASE_PATTERN,
    HOPAst_DEFAULT,
    HOPAst_RETURN,
    HOPAst_BREAK,
    HOPAst_CONTINUE,
    HOPAst_DEFER,
    HOPAst_ASSERT,
    HOPAst_DEL,
    HOPAst_EXPR_STMT,
    HOPAst_MULTI_ASSIGN,
    HOPAst_SHORT_ASSIGN,
    HOPAst_NAME_LIST,
    HOPAst_EXPR_LIST,
    HOPAst_TUPLE_EXPR,
    HOPAst_TYPE_VALUE,
    HOPAst_IDENT,
    HOPAst_INT,
    HOPAst_FLOAT,
    HOPAst_STRING,
    HOPAst_RUNE,
    HOPAst_BOOL,
    HOPAst_UNARY,
    HOPAst_BINARY,
    HOPAst_CALL,
    HOPAst_CALL_ARG,
    HOPAst_CALL_WITH_CONTEXT,
    HOPAst_CONTEXT_OVERLAY,
    HOPAst_CONTEXT_BIND,
    HOPAst_COMPOUND_LIT,
    HOPAst_COMPOUND_FIELD,
    HOPAst_INDEX,
    HOPAst_FIELD_EXPR,
    HOPAst_CAST,
    HOPAst_SIZEOF,
    HOPAst_NEW,
    HOPAst_NULL,
    HOPAst_UNWRAP,
} HOPAstKind;

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
} HOPAstNode;

enum {
    HOPAstFlag_PUB = 0x8000u,
    HOPAstFlag_INDEX_SLICE = 0x0001u,
    HOPAstFlag_INDEX_HAS_START = 0x0002u,
    HOPAstFlag_INDEX_HAS_END = 0x0004u,
    HOPAstFlag_INDEX_RUNTIME_BOUNDS = 0x0008u,
    HOPAstFlag_FIELD_EMBEDDED = 0x0010u,
    HOPAstFlag_CALL_WITH_CONTEXT_PASSTHROUGH = 0x0020u,
    HOPAstFlag_NEW_HAS_COUNT = 0x0040u,
    HOPAstFlag_NEW_HAS_ALLOC = 0x0080u,
    HOPAstFlag_NEW_HAS_INIT = 0x0100u,
    HOPAstFlag_PAREN = 0x0200u,
    HOPAstFlag_COMPOUND_FIELD_SHORTHAND = 0x0400u,
    HOPAstFlag_CONTEXT_BIND_SHORTHAND = 0x0800u,
    HOPAstFlag_PARAM_VARIADIC = 0x1000u,
    HOPAstFlag_CALL_ARG_SPREAD = 0x2000u,
    HOPAstFlag_PARAM_CONST = 0x4000u,
    HOPAstFlag_FOR_IN = 0x00010000u,
    HOPAstFlag_FOR_IN_HAS_KEY = 0x00020000u,
    HOPAstFlag_FOR_IN_KEY_REF = 0x00040000u,
    HOPAstFlag_FOR_IN_VALUE_REF = 0x00080000u,
    HOPAstFlag_FOR_IN_VALUE_DISCARD = 0x00200000u,
    HOPAstFlag_DEL_HAS_ALLOC = 0x00400000u,
};

typedef uint32_t HOPFeatures;
#define HOPFeature_NONE     ((HOPFeatures)0)
#define HOPFeature_OPTIONAL ((HOPFeatures)(1u << 0))

typedef enum {
    HOPCommentAttachment_FLOATING = 0,
    HOPCommentAttachment_LEADING = 1,
    HOPCommentAttachment_TRAILING = 2,
} HOPCommentAttachment;

typedef struct {
    uint32_t             start;
    uint32_t             end;
    uint32_t             textStart;
    uint32_t             textEnd;
    int32_t              anchorNode;
    int32_t              containerNode;
    HOPCommentAttachment attachment;
    uint8_t              _reserved[3];
} HOPComment;

typedef struct {
    const HOPAstNode* _Nullable nodes;
    uint32_t    len;
    int32_t     root;
    HOPFeatures features;
} HOPAst;

typedef enum {
    HOPParseFlag_NONE = 0,
    HOPParseFlag_COLLECT_FORMATTING = 1u << 0,
} HOPParseFlag;

typedef struct {
    uint32_t flags;
} HOPParseOptions;

typedef struct {
    const HOPComment* _Nullable comments;
    uint32_t commentLen;
} HOPParseExtras;

typedef int (*HOPFormatCanDropLiteralCastFn)(
    void* _Nullable ctx, const HOPAst* ast, HOPStrView src, int32_t castNodeId);

typedef struct {
    void* _Nullable ctx;
    HOPFormatCanDropLiteralCastFn _Nullable canDropLiteralCast;
    uint32_t flags;
    uint32_t indentWidth;
} HOPFormatOptions;

const char* HOPTokenKindName(HOPTokenKind kind);
const char* HOPAstKindName(HOPAstKind kind);

// Normalize an import path.
// Returns 0 on success and writes the normalized path to `out` (NUL-terminated).
// Returns -1 on failure and sets `*outErrReason` when provided.
// Example reasons: "empty path", "absolute path", "invalid character".
int HOPNormalizeImportPath(
    const char* importPath,
    char*       out,
    uint32_t    outCap,
    const char* _Nullable* _Nullable outErrReason);

// Tokenize src into arena memory and return a view over tokens.
// Returns 0 on success, -1 on failure. On failure, diag is set.
int HOPLex(HOPArena* arena, HOPStrView src, HOPTokenStream* out, HOPDiag* _Nullable diag);
int HOPParse(
    HOPArena*  arena,
    HOPStrView src,
    const HOPParseOptions* _Nullable options,
    HOPAst* out,
    HOPParseExtras* _Nullable outExtras,
    HOPDiag* _Nullable diag);
int HOPFormat(
    HOPArena*  arena,
    HOPStrView src,
    const HOPFormatOptions* _Nullable options,
    HOPStrView* out,
    HOPDiag* _Nullable diag);
int HOPTypeCheckEx(
    HOPArena*     arena,
    const HOPAst* ast,
    HOPStrView    src,
    const HOPTypeCheckOptions* _Nullable options,
    HOPDiag* _Nullable diag);
int HOPTypeCheck(HOPArena* arena, const HOPAst* ast, HOPStrView src, HOPDiag* _Nullable diag);
int HOPAstDump(const HOPAst* ast, HOPStrView src, HOPWriter* w, HOPDiag* _Nullable diag);

HOP_API_END

#endif /* HOP_LIBHOP_H */
