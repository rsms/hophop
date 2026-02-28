/* libsl version ${version} <https://github.com/rsms/slang>
////////////////////////////////////////////////////////////////////////////////
${license}
////////////////////////////////////////////////////////////////////////////////
*/
#ifndef SL_LIBSL_H
#define SL_LIBSL_H

#pragma once

#include <stddef.h>
#include <stdint.h>

////////////////////////////////////////////////////////////////////////////////////////////////////

// SL_VERSION is incremented for every release
#define SL_VERSION 1
#ifndef SL_SOURCE_HASH
    #define SL_SOURCE_HASH "src"
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
    #define SL_ASSUME_NONNULL_BEGIN                                                    \
        _Pragma("clang diagnostic push");                                              \
        _Pragma("clang diagnostic ignored \"-Wnullability-completeness\"");            \
        _Pragma("clang diagnostic ignored \"-Wnullability-inferred-on-nested-type\""); \
        _Pragma("clang assume_nonnull begin"); // assume T* means "T* _Nonnull"
    #define SL_ASSUME_NONNULL_END        \
        _Pragma("clang diagnostic pop"); \
        _Pragma("clang assume_nonnull end");
#else
    #define SL_ASSUME_NONNULL_BEGIN
    #define SL_ASSUME_NONNULL_END
#endif

#ifdef __cplusplus
    #define SL_API_BEGIN SL_ASSUME_NONNULL_BEGIN extern "C" {
    #define SL_API_END \
        }              \
        SL_ASSUME_NONNULL_END
#else
    #define SL_API_BEGIN SL_ASSUME_NONNULL_BEGIN
    #define SL_API_END   SL_ASSUME_NONNULL_END
#endif

#if !__has_feature(nullability)
    #define _Nullable
    #define _Nonnull
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

typedef enum {
#include "diagnostics_enum.inc"
    SLDiag__COUNT,
} SLDiagCode;

SL_API_BEGIN

typedef enum {
    SLDiagType_ERROR = 0,
    SLDiagType_WARNING = 1,
} SLDiagType;

typedef struct {
    const char* ptr;
    uint32_t    len;
} SLStrView;

typedef struct {
    void* ctx;
    void (*write)(void* ctx, const char* data, uint32_t len);
} SLWriter;

typedef void* _Nullable (*SLArenaGrowFn)(
    void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize);
typedef void (*SLArenaFreeFn)(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize);

typedef struct SLArenaBlock SLArenaBlock;
struct SLArenaBlock {
    uint8_t* _Nullable mem;
    uint32_t cap;
    uint32_t len;
    uint32_t allocSize;
    SLArenaBlock* _Nullable next;
    uint8_t owned;
};

typedef struct {
    void* _Nullable allocatorCtx;
    SLArenaGrowFn _Nullable grow;
    SLArenaFreeFn _Nullable free;
    SLArenaBlock inlineBlock;
    SLArenaBlock* _Nullable first;
    SLArenaBlock* _Nullable current;
} SLArena;

void SLArenaInit(SLArena* arena, void* storage, uint32_t storageSize);
void SLArenaInitEx(
    SLArena* arena,
    void* _Nullable storage,
    uint32_t storageSize,
    void* _Nullable allocatorCtx,
    SLArenaGrowFn _Nullable growFn,
    SLArenaFreeFn _Nullable freeFn);
void SLArenaSetAllocator(
    SLArena* arena,
    void* _Nullable allocatorCtx,
    SLArenaGrowFn _Nullable growFn,
    SLArenaFreeFn _Nullable freeFn);
void SLArenaReset(SLArena* arena);
void SLArenaDispose(SLArena* arena);
void* _Nullable SLArenaAlloc(SLArena* arena, uint32_t size, uint32_t align);

typedef struct {
    SLDiagCode code;
    SLDiagType type;
    uint32_t   start;
    uint32_t   end;
    uint32_t   argStart;
    uint32_t   argEnd;
    const char* _Nullable detail;
    const char* _Nullable hintOverride;
} SLDiag;

void        SLDiagClear(SLDiag* diag);
const char* SLDiagId(SLDiagCode code);
const char* SLDiagMessage(SLDiagCode code);
const char* _Nullable SLDiagHint(SLDiagCode code);
SLDiagType SLDiagTypeOfCode(SLDiagCode code);
uint8_t    SLDiagArgCount(SLDiagCode code);

typedef enum {
    SLTok_INVALID = 0,
    SLTok_EOF,
    SLTok_IDENT,
    SLTok_INT,
    SLTok_FLOAT,
    SLTok_STRING,
    SLTok_RUNE,

    SLTok_IMPORT,
    SLTok_PUB,
    SLTok_STRUCT,
    SLTok_UNION,
    SLTok_ENUM,
    SLTok_FN,
    SLTok_VAR,
    SLTok_CONST,
    SLTok_TYPE,
    SLTok_MUT,
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
    SLTok_SIZEOF,
    SLTok_NEW,
    SLTok_TRUE,
    SLTok_FALSE,
    SLTok_AS,
    SLTok_CONTEXT,
    SLTok_WITH,

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

    SLTok_QUESTION,
    SLTok_NULL,
} SLTokenKind;

typedef struct {
    SLTokenKind kind;
    uint32_t    start;
    uint32_t    end;
} SLToken;

typedef struct {
    const SLToken* v;
    uint32_t       len;
} SLTokenStream;

typedef enum {
    SLAst_FILE = 0,
    SLAst_IMPORT,
    SLAst_IMPORT_SYMBOL,
    SLAst_PUB,
    SLAst_FN,
    SLAst_PARAM,
    SLAst_CONTEXT_CLAUSE,
    SLAst_TYPE_NAME,
    SLAst_TYPE_PTR,
    SLAst_TYPE_REF,
    SLAst_TYPE_MUTREF,
    SLAst_TYPE_ARRAY,
    SLAst_TYPE_VARRAY,
    SLAst_TYPE_SLICE,
    SLAst_TYPE_MUTSLICE,
    SLAst_TYPE_OPTIONAL,
    SLAst_TYPE_FN,
    SLAst_TYPE_ALIAS,
    SLAst_TYPE_ANON_STRUCT,
    SLAst_TYPE_ANON_UNION,
    SLAst_TYPE_TUPLE,
    SLAst_STRUCT,
    SLAst_UNION,
    SLAst_ENUM,
    SLAst_FIELD,
    SLAst_BLOCK,
    SLAst_VAR,
    SLAst_CONST,
    SLAst_IF,
    SLAst_FOR,
    SLAst_SWITCH,
    SLAst_CASE,
    SLAst_DEFAULT,
    SLAst_RETURN,
    SLAst_BREAK,
    SLAst_CONTINUE,
    SLAst_DEFER,
    SLAst_ASSERT,
    SLAst_EXPR_STMT,
    SLAst_MULTI_ASSIGN,
    SLAst_NAME_LIST,
    SLAst_EXPR_LIST,
    SLAst_TUPLE_EXPR,
    SLAst_IDENT,
    SLAst_INT,
    SLAst_FLOAT,
    SLAst_STRING,
    SLAst_RUNE,
    SLAst_BOOL,
    SLAst_UNARY,
    SLAst_BINARY,
    SLAst_CALL,
    SLAst_CALL_ARG,
    SLAst_CALL_WITH_CONTEXT,
    SLAst_CONTEXT_OVERLAY,
    SLAst_CONTEXT_BIND,
    SLAst_COMPOUND_LIT,
    SLAst_COMPOUND_FIELD,
    SLAst_INDEX,
    SLAst_FIELD_EXPR,
    SLAst_CAST,
    SLAst_SIZEOF,
    SLAst_NEW,
    SLAst_NULL,
    SLAst_UNWRAP,
} SLAstKind;

typedef struct {
    SLAstKind kind;
    uint32_t  start;
    uint32_t  end;
    int32_t   firstChild;
    int32_t   nextSibling;
    uint32_t  dataStart;
    uint32_t  dataEnd;
    uint16_t  op;
    uint16_t  flags;
} SLAstNode;

enum {
    SLAstFlag_PUB = 0x8000u,
    SLAstFlag_INDEX_SLICE = 0x0001u,
    SLAstFlag_INDEX_HAS_START = 0x0002u,
    SLAstFlag_INDEX_HAS_END = 0x0004u,
    SLAstFlag_INDEX_RUNTIME_BOUNDS = 0x0008u,
    SLAstFlag_FIELD_EMBEDDED = 0x0010u,
    SLAstFlag_CALL_WITH_CONTEXT_PASSTHROUGH = 0x0020u,
    SLAstFlag_NEW_HAS_COUNT = 0x0040u,
    SLAstFlag_NEW_HAS_ALLOC = 0x0080u,
    SLAstFlag_NEW_HAS_INIT = 0x0100u,
    SLAstFlag_PAREN = 0x0200u,
    SLAstFlag_COMPOUND_FIELD_SHORTHAND = 0x0400u,
};

typedef uint32_t SLFeatures;
#define SLFeature_NONE     ((SLFeatures)0)
#define SLFeature_OPTIONAL ((SLFeatures)(1u << 0))

typedef enum {
    SLCommentAttachment_FLOATING = 0,
    SLCommentAttachment_LEADING = 1,
    SLCommentAttachment_TRAILING = 2,
} SLCommentAttachment;

typedef struct {
    uint32_t            start;
    uint32_t            end;
    uint32_t            textStart;
    uint32_t            textEnd;
    int32_t             anchorNode;
    int32_t             containerNode;
    SLCommentAttachment attachment;
    uint8_t             _reserved[3];
} SLComment;

typedef struct {
    const SLAstNode* nodes;
    uint32_t         len;
    int32_t          root;
    SLFeatures       features;
} SLAst;

typedef enum {
    SLParseFlag_NONE = 0,
    SLParseFlag_COLLECT_FORMATTING = 1u << 0,
} SLParseFlag;

typedef struct {
    uint32_t flags;
} SLParseOptions;

typedef struct {
    const SLComment* comments;
    uint32_t         commentLen;
} SLParseExtras;

typedef struct {
    uint32_t flags;
    uint32_t indentWidth;
} SLFormatOptions;

const char* SLTokenKindName(SLTokenKind kind);
const char* SLAstKindName(SLAstKind kind);

// Normalize an import path.
// Returns 0 on success and writes the normalized path to `out` (NUL-terminated).
// Returns -1 on failure and sets `*outErrReason` when provided.
// Example reasons: "empty path", "absolute path", "invalid character".
int SLNormalizeImportPath(
    const char* importPath,
    char*       out,
    uint32_t    outCap,
    const char* _Nullable* _Nullable outErrReason);

// Tokenize src into arena memory and return a view over tokens.
// Returns 0 on success, -1 on failure. On failure, diag is set.
int SLLex(SLArena* arena, SLStrView src, SLTokenStream* out, SLDiag* diag);
int SLParse(
    SLArena*  arena,
    SLStrView src,
    const SLParseOptions* _Nullable options,
    SLAst* out,
    SLParseExtras* _Nullable outExtras,
    SLDiag* diag);
int SLFormat(
    SLArena*  arena,
    SLStrView src,
    const SLFormatOptions* _Nullable options,
    SLStrView* out,
    SLDiag*    diag);
int SLTypeCheck(SLArena* arena, const SLAst* ast, SLStrView src, SLDiag* diag);
int SLAstDump(const SLAst* ast, SLStrView src, SLWriter* w, SLDiag* diag);

SL_API_END

#endif /* SL_LIBSL_H */
