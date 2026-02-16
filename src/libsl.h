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

// SL_VERSION is incremented for every release.
// SL_VERSION_API is the major version of the API.
#define SL_VERSION     1
#define SL_VERSION_API 1
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

#if __has_attribute(__aligned__)
    #define SL_ATTR_ALIGNED(N) __attribute__((__aligned__(N)))
#else
    #define SL_ATTR_ALIGNED(N)
#endif

#if !defined(nullable)
    #define nullable _Nullable
    #define __SL_DEFINED_NULLABLE
#endif

// SL_ENUM_TYPE_SUPPORTED is defined if the compiler supports ": Type" in an enum declaration.
// C++: standard since C++11. MSVC historically supported it, but __cplusplus may lie
// unless /Zc:__cplusplus is enabled, so also gate on _MSC_VER.
// C: standard since C23; also supported as an extension in clang, and in GCC 13+.
#ifndef SL_ENUM_TYPE_SUPPORTED
    #if defined(__cplusplus) \
        && ((__cplusplus >= 201103L) || (defined(_MSC_VER) && _MSC_VER >= 1700))
        #define SL_ENUM_TYPE_SUPPORTED
    #elif (defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 202311L)) || defined(__clang__) \
        || defined(__GNUC__)
        #define SL_ENUM_TYPE_SUPPORTED
    #endif
#endif

// SL_ENUM_TYPE usage example:
//     enum Foo SL_ENUM_TYPE(uint16_t) { Foo_A, Foo_B };
#ifndef SL_ENUM_TYPE
    #ifdef SL_ENUM_TYPE_SUPPORTED
        #define SL_ENUM_TYPE(T) : T
    #else
        #define SL_ENUM_TYPE(T)
        #ifndef SL_ENUM_TYPE_SUPPORTED_DISABLE_WARNING
            #warning Enum types not supported; sizeof some structures may report incorrect value
        #endif
    #endif
#endif

#ifdef __wasm__
    #define SL_SYS_IMPORT_AS(name) __attribute__((__import_module__("sl"), __import_name__(#name)))
    #define SL_SYS_EXPORT_AS(name) __attribute__((export_name(#name)))
#else
    #define SL_SYS_IMPORT_AS(name)
    #define SL_SYS_EXPORT_AS(name) __attribute__((visibility("internal")))
#endif

////////////////////////////////////////////////////////////////////////////////////////////////////

SL_API_BEGIN

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
    SLDiag_DUPLICATE_SYMBOL,
    SLDiag_UNKNOWN_SYMBOL,
    SLDiag_UNKNOWN_TYPE,
    SLDiag_TYPE_MISMATCH,
    SLDiag_ARITY_MISMATCH,
    SLDiag_NOT_CALLABLE,
    SLDiag_EXPECTED_BOOL,
} SLDiagCode;

typedef struct {
    SLDiagCode code;
    uint32_t   start;
    uint32_t   end;
} SLDiag;

void        SLDiagClear(SLDiag* diag);
const char* SLDiagMessage(SLDiagCode code);

typedef enum {
    SLTok_INVALID = 0,
    SLTok_EOF,
    SLTok_IDENT,
    SLTok_INT,
    SLTok_FLOAT,
    SLTok_STRING,

    SLTok_IMPORT,
    SLTok_PUB,
    SLTok_STRUCT,
    SLTok_UNION,
    SLTok_ENUM,
    SLTok_FN,
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
    SLTok_SIZEOF,
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
    uint32_t    start;
    uint32_t    end;
} SLToken;

typedef struct {
    const SLToken* v;
    uint32_t       len;
} SLTokenStream;

typedef enum {
    SLAST_FILE = 0,
    SLAST_IMPORT,
    SLAST_PUB,
    SLAST_FN,
    SLAST_PARAM,
    SLAST_TYPE_NAME,
    SLAST_TYPE_PTR,
    SLAST_TYPE_ARRAY,
    SLAST_TYPE_VARRAY,
    SLAST_STRUCT,
    SLAST_UNION,
    SLAST_ENUM,
    SLAST_FIELD,
    SLAST_BLOCK,
    SLAST_VAR,
    SLAST_CONST,
    SLAST_IF,
    SLAST_FOR,
    SLAST_SWITCH,
    SLAST_CASE,
    SLAST_DEFAULT,
    SLAST_RETURN,
    SLAST_BREAK,
    SLAST_CONTINUE,
    SLAST_DEFER,
    SLAST_ASSERT,
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
    SLAST_SIZEOF,
} SLASTKind;

typedef struct {
    SLASTKind kind;
    uint32_t  start;
    uint32_t  end;
    int32_t   firstChild;
    int32_t   nextSibling;
    uint32_t  dataStart;
    uint32_t  dataEnd;
    uint16_t  op;
    uint16_t  flags;
} SLASTNode;

typedef struct {
    const SLASTNode* nodes;
    uint32_t         len;
    int32_t          root;
} SLAST;

const char* SLTokenKindName(SLTokenKind kind);
const char* SLASTKindName(SLASTKind kind);

// Tokenize src into arena memory and return a view over tokens.
// Returns 0 on success, -1 on failure. On failure, diag is set.
int SLLex(SLArena* arena, SLStrView src, SLTokenStream* out, SLDiag* diag);
int SLParse(SLArena* arena, SLStrView src, SLAST* out, SLDiag* diag);
int SLTypeCheck(SLArena* arena, const SLAST* ast, SLStrView src, SLDiag* diag);
int SLASTDump(const SLAST* ast, SLStrView src, SLWriter* w, SLDiag* diag);

SL_API_END

#endif /* SL_LIBSL_H */
