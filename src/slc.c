#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
    #include <mach-o/dyld.h>
#endif

#include "codegen.h"
#include "ctfe.h"
#include "evaluator.h"
#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"

SL_API_BEGIN

#ifndef SL_WITH_C_BACKEND
    #define SL_WITH_C_BACKEND 1
#endif

typedef struct {
    char* _Nullable path;
    char* _Nullable source;
    uint32_t sourceLen;
    void* _Nullable arenaMem;
    SLAst ast;
} SLParsedFile;

struct SLPackage;

typedef struct {
    char* alias; /* internal mangle prefix */
    char* _Nullable bindName;
    char* path;
    struct SLPackage* _Nullable target;
    uint32_t fileIndex;
    uint32_t start;
    uint32_t end;
} SLImportRef;

typedef struct {
    uint32_t importIndex;
    char*    sourceName;
    char*    localName;
    char* _Nullable qualifiedName;
    uint8_t  isType;
    uint8_t  isFunction;
    uint8_t  useWrapper;
    uint32_t exportFileIndex;
    int32_t  exportNodeId;
    char* _Nullable fnShapeKey;
    char* _Nullable wrapperDeclText;
    uint32_t fileIndex;
    uint32_t start;
    uint32_t end;
} SLImportSymbolRef;

typedef struct {
    SLAstKind kind;
    char*     name;
    char*     declText;
    int       hasBody;
    uint32_t  fileIndex;
    int32_t   nodeId;
} SLSymbolDecl;

typedef struct {
    char*    text;
    uint32_t fileIndex;
    int32_t  nodeId;
} SLDeclText;

static int EnsureMirFunctionRefTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outTypeRef);

typedef struct SLPackage {
    char*      dirPath;
    char*      name;
    int        loadState; /* 0=new, 1=loading, 2=loaded */
    int        checked;
    SLFeatures features; /* accumulated from all parsed files */

    SLParsedFile* files;
    uint32_t      fileLen;
    uint32_t      fileCap;

    SLImportRef* imports;
    uint32_t     importLen;
    uint32_t     importCap;

    SLImportSymbolRef* importSymbols;
    uint32_t           importSymbolLen;
    uint32_t           importSymbolCap;

    SLSymbolDecl* decls;
    uint32_t      declLen;
    uint32_t      declCap;

    SLSymbolDecl* pubDecls;
    uint32_t      pubDeclLen;
    uint32_t      pubDeclCap;

    SLDeclText* declTexts;
    uint32_t    declTextLen;
    uint32_t    declTextCap;
} SLPackage;

typedef struct {
    char* _Nullable rootDir;
    char* _Nullable platformTarget;
    SLPackage* _Nullable packages;
    uint32_t packageLen;
    uint32_t packageCap;
} SLPackageLoader;

typedef struct {
    const char* name;
    const char* _Nullable replacement;
} SLIdentMap;

typedef struct {
    char* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLStringBuilder;

typedef struct {
    uint32_t combinedStart;
    uint32_t combinedEnd;
    uint32_t sourceStart;
    uint32_t sourceEnd;
    uint32_t fileIndex;
    int32_t  nodeId;
} SLCombinedSourceSpan;

typedef struct {
    SLCombinedSourceSpan* _Nullable spans;
    uint32_t len;
    uint32_t cap;
} SLCombinedSourceMap;

typedef struct {
    const SLPackage* pkg;
    uint32_t         pkgIndex;
    char*            key;
    char*            linkPrefix;
    char*            cacheDir;
    char*            cPath;
    char*            oPath;
    char*            sigPath;
    uint64_t         objMtimeNs;
} SLPackageArtifact;

typedef struct {
    SLImportRef* imp;
    char*        oldAlias;
    char*        newAlias;
} SLAliasOverride;

typedef struct {
    SLImportSymbolRef* sym;
    char*              oldQualifiedName;
    char*              newQualifiedName;
} SLImportSymbolOverride;

static int BuildPrefixedName(const char* alias, const char* name, char** outName);
static int RewriteAliasedPubDeclText(
    const SLPackage* sourcePkg, const SLSymbolDecl* pubDecl, const char* alias, char** outText);
static int BuildFnImportShapeAndWrapper(
    const char* _Nullable aliasedDeclText,
    const char* _Nullable localName,
    const char* _Nullable qualifiedName,
    char** _Nullable outShapeKey,
    char** _Nullable outWrapperDeclText);
static const SLImportRef* _Nullable FindImportByAliasSlice(
    const SLPackage* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd);
static int IsAsciiSpaceChar(char c);
static int IsIdentStartChar(unsigned char c);
static int IsIdentContinueChar(unsigned char c);
static int ResolveLibDir(char** outLibDir);
static int BuildCachedPackageArtifacts(
    SLPackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char*         libDir,
    SLPackageArtifact** outArtifacts,
    uint32_t*           outArtifactLen);
static int IsTypeDeclKind(SLAstKind kind);
static const SLParsedFile* _Nullable FindLoaderFileByMirSource(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    uint32_t               sourceRef,
    const SLPackage** _Nullable outPkg);
void FreeLoader(SLPackageLoader* loader);
int  LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    SLPackageLoader* outLoader,
    SLPackage**      outEntryPkg);
int             FindPackageIndex(const SLPackageLoader* loader, const SLPackage* pkg);
static void     FreePackageArtifacts(SLPackageArtifact* _Nullable artifacts, uint32_t artifactLen);
static uint32_t AstListCount(const SLAst* ast, int32_t listNode);
static int32_t  AstListItemAt(const SLAst* ast, int32_t listNode, uint32_t index);

#define SL_DEFAULT_PLATFORM_TARGET  "cli-libc"
#define SL_EVAL_PLATFORM_TARGET     "cli-eval"
#define SL_WASM_MIN_PLATFORM_TARGET "wasm-min"
#define SL_EVAL_CALL_MAX_DEPTH      128u

static int ASTFirstChild(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int ASTNextSibling(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

static const char* DisplayPath(const char* path) {
    static char   cwd[PATH_MAX];
    static size_t cwdLen = 0;
    static int    init = 0;
    if (!init) {
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            cwdLen = strlen(cwd);
        }
        init = 1;
    }
    if (cwdLen > 0 && strncmp(path, cwd, cwdLen) == 0 && path[cwdLen] == '/') {
        return path + cwdLen + 1u;
    }
    return path;
}

static void DiagOffsetToLineCol(
    const char* source, uint32_t offset, uint32_t* outLine, uint32_t* outCol);

static const char* DiagIdOrFallback(SLDiagCode code) {
    const char* id = SLDiagId(code);
    if (id == NULL || id[0] == '\0') {
        return "SL0000";
    }
    return id;
}

static int Errorf(
    const char* file,
    const char* _Nullable source,
    uint32_t    start,
    uint32_t    end,
    const char* fmt,
    ...) {
    va_list  ap;
    uint32_t line = start;
    uint32_t col = end;
    if (source != NULL) {
        DiagOffsetToLineCol(source, start, &line, &col);
    }
    fprintf(stderr, "%s:%u:%u: %s: ", DisplayPath(file), line, col, "SL0000");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

int ErrorDiagf(
    const char* file,
    const char* _Nullable source,
    uint32_t   start,
    uint32_t   end,
    SLDiagCode code,
    ...) {
    va_list     ap;
    const char* fmt = SLDiagMessage(code);
    uint32_t    line = start;
    uint32_t    col = end;
    if (source != NULL) {
        DiagOffsetToLineCol(source, start, &line, &col);
    }
    fprintf(stderr, "%s:%u:%u: %s: ", DisplayPath(file), line, col, DiagIdOrFallback(code));
    va_start(ap, code);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

int ErrorSimple(const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "error: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    return -1;
}

static int StrEq(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

static int SliceEqCStr(const char* s, uint32_t start, uint32_t end, const char* cstr) {
    uint32_t i = 0;
    if (end < start) {
        return 0;
    }
    while (start + i < end) {
        if (cstr[i] == '\0' || s[start + i] != cstr[i]) {
            return 0;
        }
        i++;
    }
    return cstr[i] == '\0';
}

static int IsFnReturnTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE;
}

static int SliceEqSlice(
    const char* a, uint32_t aStart, uint32_t aEnd, const char* b, uint32_t bStart, uint32_t bEnd) {
    uint32_t i;
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != (bEnd - bStart)) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (a[aStart + i] != b[bStart + i]) {
            return 0;
        }
    }
    return 1;
}

static char* _Nullable DupCStr(const char* s) {
    size_t n = strlen(s);
    char*  out = (char*)malloc(n + 1u);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1u);
    return out;
}

static char* _Nullable DupSlice(const char* s, uint32_t start, uint32_t end) {
    uint32_t len;
    char*    out;
    if (end < start) {
        return NULL;
    }
    len = end - start;
    out = (char*)malloc((size_t)len + 1u);
    if (out == NULL) {
        return NULL;
    }
    if (len > 0) {
        memcpy(out, s + start, len);
    }
    out[len] = '\0';
    return out;
}

static int IsIdentifierChar(char c) {
    unsigned char uc = (unsigned char)c;
    return c == '_' || isalnum((int)uc);
}

static int IsIdentifierStartChar(char c) {
    unsigned char uc = (unsigned char)c;
    return c == '_' || isalpha((int)uc);
}

static int NormalizeUnknownIdentifierSpan(
    const char* source, uint32_t inStart, uint32_t inEnd, uint32_t* outStart, uint32_t* outEnd) {
    uint32_t s = inStart;
    uint32_t e = inEnd;
    uint32_t srcLen;
    if (source == NULL || outStart == NULL || outEnd == NULL || e <= s) {
        return 0;
    }
    srcLen = (uint32_t)strlen(source);
    if (s > srcLen) {
        s = srcLen;
    }
    if (e > srcLen) {
        e = srcLen;
    }
    if (e <= s) {
        return 0;
    }
    while (s < e && s < srcLen && !IsIdentifierChar(source[s])) {
        s++;
    }
    while (e > s && !IsIdentifierChar(source[e - 1u])) {
        e--;
    }
    if (e <= s || s >= srcLen) {
        return 0;
    }
    while (s > 0 && IsIdentifierChar(source[s - 1u])) {
        s--;
    }
    while (e < srcLen && IsIdentifierChar(source[e])) {
        e++;
    }
    if (e <= s || !IsIdentifierStartChar(source[s])) {
        return 0;
    }
    *outStart = s;
    *outEnd = e;
    return 1;
}

static int NormalizeIdentifierTokenSpan(
    const char* source, uint32_t inStart, uint32_t inEnd, uint32_t* outStart, uint32_t* outEnd) {
    uint32_t s = inStart;
    uint32_t e = inEnd;
    uint32_t srcLen;
    uint32_t anchor;
    uint32_t tokenStart;
    uint32_t tokenEnd;
    if (source == NULL || outStart == NULL || outEnd == NULL || e <= s) {
        return 0;
    }
    srcLen = (uint32_t)strlen(source);
    if (s > srcLen) {
        s = srcLen;
    }
    if (e > srcLen) {
        e = srcLen;
    }
    if (e <= s) {
        return 0;
    }
    while (e > s && !IsIdentifierChar(source[e - 1u])) {
        e--;
    }
    if (e <= s) {
        return 0;
    }
    anchor = e - 1u;
    while (anchor > s && !IsIdentifierChar(source[anchor])) {
        anchor--;
    }
    if (!IsIdentifierChar(source[anchor])) {
        return 0;
    }
    tokenStart = anchor;
    tokenEnd = anchor + 1u;
    while (tokenStart > 0 && IsIdentifierChar(source[tokenStart - 1u])) {
        tokenStart--;
    }
    while (tokenEnd < srcLen && IsIdentifierChar(source[tokenEnd])) {
        tokenEnd++;
    }
    if (tokenEnd <= tokenStart || !IsIdentifierStartChar(source[tokenStart])) {
        return 0;
    }
    *outStart = tokenStart;
    *outEnd = tokenEnd;
    return 1;
}

static int NormalizeIdentifierAdjacentSpan(
    const char* source, uint32_t inStart, uint32_t inEnd, uint32_t* outStart, uint32_t* outEnd) {
    uint32_t s = inStart;
    uint32_t srcLen;
    uint32_t anchor = UINT32_MAX;
    uint32_t tokenStart;
    uint32_t tokenEnd;
    uint32_t i;
    if (source == NULL || outStart == NULL || outEnd == NULL) {
        return 0;
    }
    srcLen = (uint32_t)strlen(source);
    if (s > srcLen) {
        s = srcLen;
    }
    for (i = s; i < srcLen; i++) {
        if (IsIdentifierChar(source[i])) {
            anchor = i;
            break;
        }
        if (source[i] == '\n') {
            break;
        }
    }
    if (anchor == UINT32_MAX && s > 0) {
        i = s;
        while (i > 0) {
            i--;
            if (IsIdentifierChar(source[i])) {
                anchor = i;
                break;
            }
            if (source[i] == '\n') {
                break;
            }
        }
    }
    if (anchor == UINT32_MAX) {
        return 0;
    }
    tokenStart = anchor;
    tokenEnd = anchor + 1u;
    while (tokenStart > 0 && IsIdentifierChar(source[tokenStart - 1u])) {
        tokenStart--;
    }
    while (tokenEnd < srcLen && IsIdentifierChar(source[tokenEnd])) {
        tokenEnd++;
    }
    if (tokenEnd <= tokenStart || !IsIdentifierStartChar(source[tokenStart])) {
        return 0;
    }
    *outStart = tokenStart;
    *outEnd = tokenEnd;
    return 1;
}

typedef struct {
    uint8_t startMapped;
    uint8_t endMapped;
    uint8_t argStartMapped;
    uint8_t argEndMapped;
} SLRemapDiagStatus;

static void DiagOffsetToLineCol(
    const char* source, uint32_t offset, uint32_t* outLine, uint32_t* outCol) {
    uint32_t i = 0;
    uint32_t line = 1;
    uint32_t col = 1;
    while (source[i] != '\0' && i < offset) {
        if (source[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
        i++;
    }
    *outLine = line;
    *outCol = col;
}

static int PrintSLDiagEx(
    const char* filename,
    const char* _Nullable source,
    const SLDiag* diag,
    int           includeHint,
    int           useLineCol,
    int           useIdentifierWording) {
    const char* msg = SLDiagMessage(diag->code);
    uint8_t     argCount = SLDiagArgCount(diag->code);
    const char* diagId = DiagIdOrFallback(diag->code);
    uint32_t    sourceLen = source != NULL ? (uint32_t)strlen(source) : 0u;
    uint32_t    spanStart = diag->start;
    uint32_t    spanEnd = diag->end;
    uint32_t    argStart = diag->argStart;
    uint32_t    argEnd = diag->argEnd;
    uint32_t    locA = diag->start;
    uint32_t    locB = diag->end;
    if (source != NULL) {
        if (spanStart > sourceLen) {
            spanStart = sourceLen;
        }
        if (spanEnd > sourceLen) {
            spanEnd = sourceLen;
        }
        if (spanEnd < spanStart) {
            spanEnd = spanStart;
        }
        if (argStart > sourceLen) {
            argStart = sourceLen;
        }
        if (argEnd > sourceLen) {
            argEnd = sourceLen;
        }
        if (argEnd < argStart) {
            argEnd = argStart;
        }
    }
    if (useLineCol && source != NULL) {
        DiagOffsetToLineCol(source, diag->start, &locA, &locB);
    }

    fprintf(stderr, "%s:%u:%u: %s: ", DisplayPath(filename), locA, locB, diagId);

    if (useIdentifierWording && diag->code == SLDiag_UNKNOWN_SYMBOL && source != NULL
        && spanEnd > spanStart)
    {
        char* ident = DupSlice(source, spanStart, spanEnd);
        if (ident != NULL) {
            fprintf(stderr, "unknown identifier '%s'", ident);
            free(ident);
        } else {
            fputs("unknown identifier", stderr);
        }
    } else if (argCount == 0) {
        fputs(msg, stderr);
    } else if (argCount == 1) {
        char* arg = NULL;
        if (source != NULL && argEnd > argStart) {
            arg = DupSlice(source, argStart, argEnd);
        } else {
            arg = DupCStr("");
        }
        if (arg == NULL) {
            fputs(msg, stderr);
        } else {
            fprintf(stderr, msg, arg);
            free(arg);
        }
    } else {
        fputs(msg, stderr);
    }
    if (diag->detail != NULL && diag->detail[0] != '\0') {
        if (diag->code == SLDiag_SWITCH_MISSING_CASES) {
            fprintf(stderr, " %s", diag->detail);
        } else {
            fprintf(stderr, ": %s", diag->detail);
        }
    }
    if (diag->code == SLDiag_ARENA_OOM && diag->argEnd > 0) {
        fprintf(
            stderr,
            " (used %u / %u bytes, %.1f%%)",
            diag->argStart,
            diag->argEnd,
            diag->argEnd > 0 ? (100.0 * (double)diag->argStart / (double)diag->argEnd) : 0.0);
    }
    fputc('\n', stderr);

    if (includeHint) {
        const char* hint =
            (diag->hintOverride != NULL && diag->hintOverride[0] != '\0')
                ? diag->hintOverride
                : SLDiagHint(diag->code);
        if (hint != NULL) {
            fprintf(stderr, "  tip: %s\n", hint);
        }
    }
    return diag->type == SLDiagType_WARNING ? 0 : -1;
}

int PrintSLDiag(
    const char* filename, const char* _Nullable source, const SLDiag* diag, int includeHint) {
    return PrintSLDiagEx(filename, source, diag, includeHint, 1, 1);
}

static int PrintSLDiagLineCol(
    const char* filename, const char* _Nullable source, const SLDiag* diag, int includeHint) {
    return PrintSLDiagEx(filename, source, diag, includeHint, 1, 1);
}

static uint32_t ArenaBytesUsed(const SLArena* arena) {
    const SLArenaBlock* block;
    uint64_t            sum = 0;
    if (arena == NULL) {
        return 0;
    }
    block = arena->first;
    while (block != NULL) {
        sum += block->len;
        block = block->next;
    }
    return sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

static uint32_t ArenaBytesCapacity(const SLArena* arena) {
    const SLArenaBlock* block;
    uint64_t            sum = 0;
    if (arena == NULL) {
        return 0;
    }
    block = arena->first;
    while (block != NULL) {
        sum += block->cap;
        block = block->next;
    }
    return sum > UINT32_MAX ? UINT32_MAX : (uint32_t)sum;
}

static int ArenaDebugEnabled(void) {
    const char* s = getenv("SL_ARENA_DEBUG");
    return s != NULL && s[0] != '\0' && s[0] != '0';
}

static int CompactAstInArena(SLArena* arena, SLAst* ast) {
    uint32_t   bytes;
    SLAstNode* compactNodes;
    SLAstNode* temp;
    if (arena == NULL || ast == NULL || ast->nodes == NULL || ast->len == 0) {
        return 0;
    }
    if (ast->len > UINT32_MAX / (uint32_t)sizeof(SLAstNode)) {
        return -1;
    }
    bytes = ast->len * (uint32_t)sizeof(SLAstNode);
    temp = (SLAstNode*)malloc(bytes);
    if (temp == NULL) {
        return -1;
    }
    memcpy(temp, ast->nodes, bytes);
    SLArenaReset(arena);
    compactNodes = (SLAstNode*)SLArenaAlloc(arena, bytes, (uint32_t)_Alignof(SLAstNode));
    if (compactNodes == NULL) {
        free(temp);
        return -1;
    }
    memcpy(compactNodes, temp, bytes);
    free(temp);
    ast->nodes = compactNodes;
    return 0;
}

void* _Nullable CodegenArenaGrow(
    void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize) {
    void* p;
    (void)ctx;
    p = malloc((size_t)minSize);
    if (p == NULL) {
        return NULL;
    }
    *outSize = minSize;
    return p;
}

void CodegenArenaFree(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize) {
    (void)ctx;
    (void)blockSize;
    free(block);
}

static int EnsureCap(void** ptr, uint32_t* cap, uint32_t need, size_t elemSize) {
    uint32_t newCap;
    void*    newPtr;
    if (need <= *cap) {
        return 0;
    }
    newCap = *cap == 0 ? 8u : *cap;
    while (newCap < need) {
        if (newCap > UINT32_MAX / 2u) {
            newCap = need;
            break;
        }
        newCap *= 2u;
    }
    newPtr = realloc(*ptr, (size_t)newCap * elemSize);
    if (newPtr == NULL) {
        return -1;
    }
    *ptr = newPtr;
    *cap = newCap;
    return 0;
}

static void CombinedSourceMapFree(SLCombinedSourceMap* map) {
    if (map == NULL) {
        return;
    }
    free(map->spans);
    map->spans = NULL;
    map->len = 0;
    map->cap = 0;
}

static int CombinedSourceMapAdd(
    SLCombinedSourceMap* map,
    uint32_t             combinedStart,
    uint32_t             combinedEnd,
    uint32_t             sourceStart,
    uint32_t             sourceEnd,
    uint32_t             fileIndex,
    int32_t              nodeId) {
    if (map == NULL) {
        return 0;
    }
    if (EnsureCap((void**)&map->spans, &map->cap, map->len + 1u, sizeof(SLCombinedSourceSpan)) != 0)
    {
        return -1;
    }
    map->spans[map->len].combinedStart = combinedStart;
    map->spans[map->len].combinedEnd = combinedEnd;
    map->spans[map->len].sourceStart = sourceStart;
    map->spans[map->len].sourceEnd = sourceEnd;
    map->spans[map->len].fileIndex = fileIndex;
    map->spans[map->len].nodeId = nodeId;
    map->len++;
    return 0;
}

static int RemapCombinedOffset(
    const SLCombinedSourceMap* map, uint32_t offset, uint32_t* outOffset, uint32_t* outFileIndex) {
    uint32_t i;
    if (map == NULL || outOffset == NULL || outFileIndex == NULL) {
        return 0;
    }
    for (i = 0; i < map->len; i++) {
        const SLCombinedSourceSpan* s = &map->spans[i];
        uint32_t                    combinedLen;
        uint32_t                    sourceLen;
        uint32_t                    rel;
        uint32_t                    mapped;
        if (offset < s->combinedStart || offset > s->combinedEnd) {
            continue;
        }
        combinedLen = s->combinedEnd >= s->combinedStart ? (s->combinedEnd - s->combinedStart) : 0;
        sourceLen = s->sourceEnd >= s->sourceStart ? (s->sourceEnd - s->sourceStart) : 0;
        rel = offset - s->combinedStart;
        if (sourceLen == 0 || combinedLen == 0) {
            mapped = s->sourceStart;
        } else if (rel >= combinedLen) {
            mapped = s->sourceEnd;
        } else {
            mapped = s->sourceStart
                   + (uint32_t)(((uint64_t)rel * (uint64_t)sourceLen) / (uint64_t)combinedLen);
        }
        if (mapped < s->sourceStart) {
            mapped = s->sourceStart;
        }
        if (mapped > s->sourceEnd) {
            mapped = s->sourceEnd;
        }
        *outOffset = mapped;
        *outFileIndex = s->fileIndex;
        return 1;
    }
    return 0;
}

static void RemapCombinedDiag(
    const SLCombinedSourceMap* map,
    const SLDiag*              diagIn,
    SLDiag*                    diagOut,
    uint32_t*                  outFileIndex,
    const char* _Nullable source,
    SLRemapDiagStatus* _Nullable outStatus) {
    uint32_t fileIndexStart = 0;
    uint32_t fileIndexEnd = 0;
    int      startMapped;
    int      endMapped;
    int      argStartMapped = 0;
    int      argEndMapped = 0;
    *diagOut = *diagIn;
    if (outFileIndex != NULL) {
        *outFileIndex = 0;
    }
    if (outStatus != NULL) {
        outStatus->startMapped = 0;
        outStatus->endMapped = 0;
        outStatus->argStartMapped = 0;
        outStatus->argEndMapped = 0;
    }
    if (map == NULL || map->len == 0) {
        return;
    }
    startMapped = RemapCombinedOffset(map, diagIn->start, &diagOut->start, &fileIndexStart);
    endMapped = RemapCombinedOffset(map, diagIn->end, &diagOut->end, &fileIndexEnd);
    if (outStatus != NULL) {
        outStatus->startMapped = startMapped ? 1u : 0u;
        outStatus->endMapped = endMapped ? 1u : 0u;
    }
    if (startMapped && outFileIndex != NULL) {
        *outFileIndex = fileIndexStart;
    } else if (endMapped && outFileIndex != NULL) {
        *outFileIndex = fileIndexEnd;
    }
    if (diagIn->argEnd > diagIn->argStart) {
        uint32_t argFileIndex = 0;
        argStartMapped = RemapCombinedOffset(
            map, diagIn->argStart, &diagOut->argStart, &argFileIndex);
        argEndMapped = RemapCombinedOffset(map, diagIn->argEnd, &diagOut->argEnd, &argFileIndex);
        if (outStatus != NULL) {
            outStatus->argStartMapped = argStartMapped ? 1u : 0u;
            outStatus->argEndMapped = argEndMapped ? 1u : 0u;
        }
        if (!argStartMapped) {
            diagOut->argStart = diagIn->argStart;
        }
        if (!argEndMapped) {
            diagOut->argEnd = diagIn->argEnd;
        }
        if (source != NULL
            && (diagOut->code == SLDiag_UNUSED_FUNCTION || diagOut->code == SLDiag_UNUSED_VARIABLE
                || diagOut->code == SLDiag_UNUSED_VARIABLE_NEVER_READ
                || diagOut->code == SLDiag_UNUSED_PARAMETER
                || diagOut->code == SLDiag_UNUSED_PARAMETER_NEVER_READ
                || diagOut->code == SLDiag_CONST_PARAM_ARG_NOT_CONST
                || diagOut->code == SLDiag_CONST_PARAM_SPREAD_NOT_CONST)
            && diagOut->argEnd > diagOut->argStart)
        {
            int normalized = NormalizeIdentifierTokenSpan(
                source, diagOut->argStart, diagOut->argEnd, &diagOut->argStart, &diagOut->argEnd);
            if (!normalized && diagOut->code == SLDiag_UNUSED_FUNCTION) {
                (void)NormalizeIdentifierAdjacentSpan(
                    source,
                    diagOut->argStart,
                    diagOut->argEnd,
                    &diagOut->argStart,
                    &diagOut->argEnd);
            }
        }
    }
    if (source != NULL && diagOut->code == SLDiag_UNKNOWN_SYMBOL && diagOut->end > diagOut->start) {
        (void)NormalizeUnknownIdentifierSpan(
            source, diagOut->start, diagOut->end, &diagOut->start, &diagOut->end);
    }
}

static int SBReserve(SLStringBuilder* b, uint32_t extra) {
    uint32_t need;
    char*    newPtr;
    if (UINT32_MAX - b->len < extra + 1u) {
        return -1;
    }
    need = b->len + extra + 1u;
    if (need <= b->cap) {
        return 0;
    }
    if (EnsureCap((void**)&b->v, &b->cap, need, sizeof(char)) != 0) {
        return -1;
    }
    newPtr = b->v;
    if (newPtr == NULL) {
        return -1;
    }
    b->v = newPtr;
    return 0;
}

static int SBAppend(SLStringBuilder* b, const char* s, uint32_t len) {
    if (b == NULL || len == 0) {
        return 0;
    }
    if (SBReserve(b, len) != 0) {
        return -1;
    }
    if (b->v == NULL || s == NULL) {
        return -1;
    }
    memcpy(b->v + b->len, s, len);
    b->len += len;
    b->v[b->len] = '\0';
    return 0;
}

static int SBAppendCStr(SLStringBuilder* b, const char* _Nullable s) {
    if (s == NULL) {
        return -1;
    }
    return SBAppend(b, s, (uint32_t)strlen(s));
}

static int SBAppendSlice(SLStringBuilder* b, const char* s, uint32_t start, uint32_t end) {
    if (end < start) {
        return -1;
    }
    return SBAppend(b, s + start, end - start);
}

static char* _Nullable SBFinish(SLStringBuilder* b, uint32_t* _Nullable outLen) {
    char* out;
    if (b->v == NULL) {
        out = (char*)malloc(1u);
        if (out == NULL) {
            return NULL;
        }
        out[0] = '\0';
        if (outLen != NULL) {
            *outLen = 0;
        }
        return out;
    }
    out = b->v;
    if (outLen != NULL) {
        *outLen = b->len;
    }
    b->v = NULL;
    b->len = 0;
    b->cap = 0;
    return out;
}

static char* _Nullable JoinPath(const char* _Nullable a, const char* _Nullable b) {
    if (a == NULL || b == NULL) {
        return NULL;
    }
    size_t aLen = strlen(a);
    size_t bLen = strlen(b);
    int    needSlash = 1;
    char*  out;
    if (aLen > 0 && a[aLen - 1] == '/') {
        needSlash = 0;
    }
    out = (char*)malloc(aLen + (size_t)needSlash + bLen + 1u);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, a, aLen);
    if (needSlash) {
        out[aLen] = '/';
        memcpy(out + aLen + 1u, b, bLen);
        out[aLen + 1u + bLen] = '\0';
    } else {
        memcpy(out + aLen, b, bLen);
        out[aLen + bLen] = '\0';
    }
    return out;
}

static char* _Nullable DirNameDup(const char* path) {
    const char* slash = strrchr(path, '/');
    char*       out;
    size_t      len;
    if (slash == NULL) {
        return DupCStr(".");
    }
    len = (size_t)(slash - path);
    if (len == 0) {
        return DupCStr("/");
    }
    out = (char*)malloc(len + 1u);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, path, len);
    out[len] = '\0';
    return out;
}

/* Returns a malloc'd string with the directory containing the running
 * executable, or NULL on failure. Caller must free. */
static char* _Nullable GetExeDir(void) {
#if defined(__APPLE__)
    char     buf[PATH_MAX];
    char     resolved[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return NULL;
    }
    if (realpath(buf, resolved) == NULL) {
        return NULL;
    }
    return DirNameDup(resolved);
#elif defined(__linux__)
    char    buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return NULL;
    }
    buf[n] = '\0';
    return DirNameDup(buf);
#else
    return NULL;
#endif
}

static uint64_t StatMtimeNs(const struct stat* st) {
#if defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st->st_mtimespec.tv_nsec;
#elif defined(__linux__)
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
#else
    return (uint64_t)st->st_mtime * 1000000000ull;
#endif
}

static int GetFileMtimeNs(const char* path, uint64_t* outMtimeNs) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    *outMtimeNs = StatMtimeNs(&st);
    return 0;
}

static int EnsureDirPath(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return 0;
        }
        return -1;
    }
    if (mkdir(path, 0777) == 0) {
        return 0;
    }
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 0;
    }
    return -1;
}

static int EnsureDirRecursive(const char* path) {
    char   tmp[PATH_MAX];
    char*  p;
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1u);
    for (p = tmp + 1; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (EnsureDirPath(tmp) != 0) {
            return -1;
        }
        *p = '/';
    }
    return EnsureDirPath(tmp);
}

static char* _Nullable MakeAbsolutePathDup(const char* path) {
    char cwd[PATH_MAX];
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    if (path[0] == '/') {
        return DupCStr(path);
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return NULL;
    }
    return JoinPath(cwd, path);
}

static uint64_t HashFNV1a64(const char* s) {
    uint64_t h = 1469598103934665603ull;
    size_t   i;
    for (i = 0; s[i] != '\0'; i++) {
        __uint128_t x = (((__uint128_t)h) ^ (uint8_t)s[i]) * 1099511628211ull;
        h = (uint64_t)x;
    }
    return h;
}

static char* _Nullable BuildSanitizedIdent(const char* s, const char* fallback) {
    size_t i;
    size_t len;
    char*  out;
    if (s == NULL || s[0] == '\0') {
        return DupCStr(fallback);
    }
    len = strlen(s);
    out = (char*)malloc(len + 2u);
    if (out == NULL) {
        return NULL;
    }
    out[0] = '\0';
    if (!IsIdentStartChar((unsigned char)s[0])) {
        out[0] = 'p';
        out[1] = '\0';
    }
    for (i = 0; s[i] != '\0'; i++) {
        char ch = s[i];
        if (!IsIdentContinueChar((unsigned char)ch)) {
            ch = '_';
        }
        {
            size_t outLen = strlen(out);
            out[outLen] = ch;
            out[outLen + 1u] = '\0';
        }
    }
    if (out[0] == '\0') {
        free(out);
        return DupCStr(fallback);
    }
    return out;
}

static int HasSuffix(const char* s, const char* suffix) {
    size_t sLen = strlen(s);
    size_t suffixLen = strlen(suffix);
    if (sLen < suffixLen) {
        return 0;
    }
    return memcmp(s + (sLen - suffixLen), suffix, suffixLen) == 0;
}

static int IsIdentStartChar(unsigned char c) {
    return (c >= (unsigned char)'A' && c <= (unsigned char)'Z')
        || (c >= (unsigned char)'a' && c <= (unsigned char)'z') || c == (unsigned char)'_';
}

static int IsIdentContinueChar(unsigned char c) {
    return IsIdentStartChar(c) || (c >= (unsigned char)'0' && c <= (unsigned char)'9');
}

static int IsValidIdentifier(const char* s) {
    size_t i = 0;
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    if (!IsIdentStartChar((unsigned char)s[0])) {
        return 0;
    }
    i = 1;
    while (s[i] != '\0') {
        if (!IsIdentContinueChar((unsigned char)s[i])) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int IsValidPlatformTargetName(const char* s) {
    size_t i = 0;
    if (s == NULL || s[0] == '\0') {
        return 0;
    }
    while (s[i] != '\0') {
        unsigned char c = (unsigned char)s[i];
        if (!(IsIdentContinueChar(c) || c == '-' || c == '.')) {
            return 0;
        }
        i++;
    }
    return 1;
}

static int IsReservedSLPrefixName(const char* s) {
    return s != NULL && strncmp(s, "__sl_", 5u) == 0;
}

static char* _Nullable BaseNameDup(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return DupCStr(path);
    }
    return DupCStr(slash + 1);
}

static char* _Nullable LastPathComponentDup(const char* path) {
    size_t      len = strlen(path);
    const char* end = path + len;
    const char* start = path;
    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return DupCStr("");
    }
    start = end;
    while (start > path && start[-1] != '/') {
        start--;
    }
    len = (size_t)(end - start);
    if (len == 0) {
        return DupCStr("");
    }
    {
        char* out = (char*)malloc(len + 1u);
        if (out == NULL) {
            return NULL;
        }
        memcpy(out, start, len);
        out[len] = '\0';
        return out;
    }
}

static char* _Nullable StripSLExtensionDup(const char* filename) {
    size_t len = strlen(filename);
    if (len > 3 && strcmp(filename + len - 3u, ".sl") == 0) {
        char* out = (char*)malloc(len - 1u);
        if (out == NULL) {
            return NULL;
        }
        memcpy(out, filename, len - 3u);
        out[len - 3u] = '\0';
        return out;
    }
    return DupCStr(filename);
}

static int CompareStringPtrs(const void* a, const void* b) {
    const char* const* sa = (const char* const*)a;
    const char* const* sb = (const char* const*)b;
    return strcmp(*sa, *sb);
}

static int ListSLFiles(const char* dirPath, char*** outFiles, uint32_t* outLen) {
    DIR*           dir = opendir(dirPath);
    struct dirent* ent;
    char**         files = NULL;
    uint32_t       len = 0;
    uint32_t       cap = 0;

    *outFiles = NULL;
    *outLen = 0;

    if (dir == NULL) {
        return ErrorSimple("failed to open package directory %s", dirPath);
    }

    for (;;) {
        char*       filePath;
        struct stat st;
        if ((ent = readdir(dir)) == NULL) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!HasSuffix(ent->d_name, ".sl")) {
            continue;
        }
        filePath = JoinPath(dirPath, ent->d_name);
        if (filePath == NULL) {
            closedir(dir);
            return ErrorSimple("out of memory");
        }
        if (stat(filePath, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(filePath);
            continue;
        }
        if (EnsureCap((void**)&files, &cap, len + 1u, sizeof(char*)) != 0) {
            free(filePath);
            closedir(dir);
            return ErrorSimple("out of memory");
        }
        files[len++] = filePath;
    }
    closedir(dir);

    if (len == 0) {
        free(files);
        return ErrorSimple("no .sl files found in %s", dirPath);
    }

    qsort(files, (size_t)len, sizeof(char*), CompareStringPtrs);
    *outFiles = files;
    *outLen = len;
    return 0;
}

static int ReadFile(const char* filename, char** outData, uint32_t* outLen) {
    FILE*  f;
    long   size;
    char*  data;
    size_t nread;

    *outData = NULL;
    *outLen = 0;

    f = fopen(filename, "rb");
    if (f == NULL) {
        fprintf(stderr, "failed to open %s\n", filename);
        return -1;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        fprintf(stderr, "failed to seek %s\n", filename);
        return -1;
    }

    size = ftell(f);
    if (size < 0 || (unsigned long)size > UINT32_MAX) {
        fclose(f);
        fprintf(stderr, "file too large: %s\n", filename);
        return -1;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        fprintf(stderr, "failed to seek %s\n", filename);
        return -1;
    }

    data = (char*)malloc((size_t)size + 1u);
    if (data == NULL) {
        fclose(f);
        fprintf(stderr, "out of memory while reading %s\n", filename);
        return -1;
    }

    nread = fread(data, 1u, (size_t)size, f);
    fclose(f);
    if (nread != (size_t)size) {
        free(data);
        fprintf(stderr, "failed to read %s\n", filename);
        return -1;
    }

    data[size] = '\0';
    *outData = data;
    *outLen = (uint32_t)size;
    return 0;
}

static int ListTopLevelSLFilesForFmt(const char* dirPath, char*** outFiles, uint32_t* outLen) {
    DIR*           dir = opendir(dirPath);
    struct dirent* ent;
    char**         files = NULL;
    uint32_t       len = 0;
    uint32_t       cap = 0;

    *outFiles = NULL;
    *outLen = 0;

    if (dir == NULL) {
        return ErrorSimple("failed to open directory %s", dirPath);
    }

    for (;;) {
        char*       filePath;
        struct stat st;
        if ((ent = readdir(dir)) == NULL) {
            break;
        }
        if (ent->d_name[0] == '.') {
            continue;
        }
        if (!HasSuffix(ent->d_name, ".sl")) {
            continue;
        }
        filePath = JoinPath(dirPath, ent->d_name);
        if (filePath == NULL) {
            uint32_t i;
            for (i = 0; i < len; i++) {
                free(files[i]);
            }
            free(files);
            closedir(dir);
            return ErrorSimple("out of memory");
        }
        if (stat(filePath, &st) != 0 || !S_ISREG(st.st_mode)) {
            free(filePath);
            continue;
        }
        if (EnsureCap((void**)&files, &cap, len + 1u, sizeof(char*)) != 0) {
            uint32_t i;
            free(filePath);
            for (i = 0; i < len; i++) {
                free(files[i]);
            }
            free(files);
            closedir(dir);
            return ErrorSimple("out of memory");
        }
        files[len++] = filePath;
    }
    closedir(dir);

    if (len > 0) {
        qsort(files, (size_t)len, sizeof(char*), CompareStringPtrs);
    }
    *outFiles = files;
    *outLen = len;
    return 0;
}

static int WriteFileAtomic(const char* filename, const char* data, uint32_t len) {
    size_t  filenameLen = strlen(filename);
    size_t  tmpCap = filenameLen + 32u;
    char*   tmpPath;
    int     fd;
    ssize_t nwritten;
    int     rc = -1;

    tmpPath = (char*)malloc(tmpCap);
    if (tmpPath == NULL) {
        return -1;
    }
    snprintf(tmpPath, tmpCap, "%s.tmp.XXXXXX", filename);
    fd = mkstemp(tmpPath);
    if (fd < 0) {
        free(tmpPath);
        return -1;
    }

    nwritten = write(fd, data, (size_t)len);
    if (nwritten != (ssize_t)len) {
        close(fd);
        unlink(tmpPath);
        free(tmpPath);
        return -1;
    }
    if (close(fd) != 0) {
        unlink(tmpPath);
        free(tmpPath);
        return -1;
    }
    if (rename(tmpPath, filename) == 0) {
        rc = 0;
    } else {
        unlink(tmpPath);
    }
    free(tmpPath);
    return rc;
}

typedef struct {
    uint32_t start;
    uint32_t end;
} SLFmtCheckLine;

static int SLFmtCheckBuildLines(
    const char* src, uint32_t srcLen, SLFmtCheckLine** outLines, uint32_t* outLineLen) {
    SLFmtCheckLine* lines = NULL;
    uint32_t        lineCap = 1u;
    uint32_t        lineLen = 0u;
    uint32_t        i;
    uint32_t        start;

    for (i = 0; i < srcLen; i++) {
        if (src[i] == '\n') {
            lineCap++;
        }
    }
    lines = (SLFmtCheckLine*)malloc((size_t)lineCap * sizeof(SLFmtCheckLine));
    if (lines == NULL) {
        return -1;
    }

    start = 0u;
    while (start <= srcLen) {
        uint32_t end = start;
        while (end < srcLen && src[end] != '\n') {
            end++;
        }
        lines[lineLen].start = start;
        lines[lineLen].end = end;
        lineLen++;
        if (end >= srcLen) {
            break;
        }
        start = end + 1u;
    }

    *outLines = lines;
    *outLineLen = lineLen;
    return 0;
}

static int SLFmtCheckLineEq(
    const char* a, const SLFmtCheckLine* al, const char* b, const SLFmtCheckLine* bl) {
    uint32_t aLen = al->end - al->start;
    uint32_t bLen = bl->end - bl->start;
    if (aLen != bLen) {
        return 0;
    }
    if (aLen == 0) {
        return 1;
    }
    return memcmp(a + al->start, b + bl->start, aLen) == 0;
}

static uint32_t SLFmtCheckTrimLeft(const char* s, uint32_t start, uint32_t end) {
    while (start < end && isspace((unsigned char)s[start])) {
        start++;
    }
    return start;
}

static uint32_t SLFmtCheckTrimRight(const char* s, uint32_t start, uint32_t end) {
    while (end > start && isspace((unsigned char)s[end - 1u])) {
        end--;
    }
    return end;
}

static int SLFmtCheckLineEqTrimmed(
    const char* a, const SLFmtCheckLine* al, const char* b, const SLFmtCheckLine* bl) {
    uint32_t aStart = SLFmtCheckTrimLeft(a, al->start, al->end);
    uint32_t aEnd = SLFmtCheckTrimRight(a, aStart, al->end);
    uint32_t bStart = SLFmtCheckTrimLeft(b, bl->start, bl->end);
    uint32_t bEnd = SLFmtCheckTrimRight(b, bStart, bl->end);
    uint32_t aLen = aEnd - aStart;
    uint32_t bLen = bEnd - bStart;
    if (aLen != bLen) {
        return 0;
    }
    if (aLen == 0) {
        return 1;
    }
    return memcmp(a + aStart, b + bStart, aLen) == 0;
}

static int SLFmtCheckLineEqNoWhitespace(
    const char* a, const SLFmtCheckLine* al, const char* b, const SLFmtCheckLine* bl) {
    uint32_t ai = al->start;
    uint32_t bi = bl->start;
    while (ai < al->end || bi < bl->end) {
        while (ai < al->end && isspace((unsigned char)a[ai])) {
            ai++;
        }
        while (bi < bl->end && isspace((unsigned char)b[bi])) {
            bi++;
        }
        if (ai >= al->end || bi >= bl->end) {
            break;
        }
        if (a[ai] != b[bi]) {
            return 0;
        }
        ai++;
        bi++;
    }
    while (ai < al->end && isspace((unsigned char)a[ai])) {
        ai++;
    }
    while (bi < bl->end && isspace((unsigned char)b[bi])) {
        bi++;
    }
    return ai == al->end && bi == bl->end;
}

static void SLFmtCheckPrintEscapedLine(const char* s, uint32_t start, uint32_t end) {
    uint32_t i;
    fputc('"', stdout);
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '\t': fputs("\\t", stdout); break;
            case '\r': fputs("\\r", stdout); break;
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            default:
                if (ch >= 0x20 && ch <= 0x7e) {
                    fputc((int)ch, stdout);
                } else {
                    fprintf(stdout, "\\x%02x", (unsigned)ch);
                }
                break;
        }
    }
    fputc('"', stdout);
}

static void SLFmtCheckPrintIssue(
    const char*           filename,
    uint32_t              lineNo,
    const char*           reason,
    const char*           current,
    const SLFmtCheckLine* currentLine,
    const char*           expected,
    const SLFmtCheckLine* expectedLine) {
    fprintf(stdout, "%s:%u:1: %s\n", filename, lineNo, reason);
    fputs("  current : ", stdout);
    SLFmtCheckPrintEscapedLine(current, currentLine->start, currentLine->end);
    fputc('\n', stdout);
    fputs("  expected: ", stdout);
    SLFmtCheckPrintEscapedLine(expected, expectedLine->start, expectedLine->end);
    fputc('\n', stdout);
}

static void SLFmtCheckReport(
    const char* filename,
    const char* current,
    uint32_t    currentLen,
    const char* expected,
    uint32_t    expectedLen) {
    SLFmtCheckLine* currentLines = NULL;
    SLFmtCheckLine* expectedLines = NULL;
    uint32_t        currentLineLen = 0;
    uint32_t        expectedLineLen = 0;
    uint32_t        i = 0;
    uint32_t        j = 0;
    uint32_t        issues = 0;

    if (SLFmtCheckBuildLines(current, currentLen, &currentLines, &currentLineLen) != 0
        || SLFmtCheckBuildLines(expected, expectedLen, &expectedLines, &expectedLineLen) != 0)
    {
        fputs("  note: unable to allocate detailed formatter mismatch report\n", stdout);
        free(currentLines);
        free(expectedLines);
        return;
    }

    while (i < currentLineLen || j < expectedLineLen) {
        SLFmtCheckLine empty = { 0, 0 };
        if (i < currentLineLen && j < expectedLineLen
            && SLFmtCheckLineEq(current, &currentLines[i], expected, &expectedLines[j]))
        {
            i++;
            j++;
            continue;
        }

        if (i + 1u < currentLineLen && j < expectedLineLen
            && SLFmtCheckLineEq(current, &currentLines[i + 1u], expected, &expectedLines[j]))
        {
            SLFmtCheckPrintIssue(
                filename,
                i + 1u,
                "line should be removed",
                current,
                &currentLines[i],
                expected,
                &empty);
            issues++;
            i++;
            continue;
        }
        if (i < currentLineLen && j + 1u < expectedLineLen
            && SLFmtCheckLineEq(current, &currentLines[i], expected, &expectedLines[j + 1u]))
        {
            SLFmtCheckPrintIssue(
                filename,
                i + 1u,
                "line should be inserted",
                current,
                &empty,
                expected,
                &expectedLines[j]);
            issues++;
            j++;
            continue;
        }

        if (i < currentLineLen && j < expectedLineLen) {
            const char* reason = "line content differs";
            if (SLFmtCheckLineEqTrimmed(current, &currentLines[i], expected, &expectedLines[j])) {
                reason = "leading or trailing whitespace differs";
            } else if (
                SLFmtCheckLineEqNoWhitespace(
                    current, &currentLines[i], expected, &expectedLines[j]))
            {
                reason = "internal whitespace differs";
            }
            SLFmtCheckPrintIssue(
                filename, i + 1u, reason, current, &currentLines[i], expected, &expectedLines[j]);
            issues++;
            i++;
            j++;
            continue;
        }

        if (i < currentLineLen) {
            SLFmtCheckPrintIssue(
                filename,
                i + 1u,
                "line should be removed",
                current,
                &currentLines[i],
                expected,
                &empty);
            issues++;
            i++;
            continue;
        }
        SLFmtCheckPrintIssue(
            filename,
            currentLineLen + 1u,
            "line should be inserted",
            current,
            &empty,
            expected,
            &expectedLines[j]);
        issues++;
        j++;
    }

    fprintf(stdout, "  issues: %u\n", issues);

    free(currentLines);
    free(expectedLines);
}

static int FormatOneFile(const char* filename, int checkOnly, int* outChanged) {
    char*     source = NULL;
    uint32_t  sourceLen = 0;
    uint64_t  arenaCap64;
    size_t    arenaCap;
    void*     arenaMem = NULL;
    SLArena   arena;
    SLDiag    diag = { 0 };
    SLStrView formatted = { 0 };
    int       changed = 0;
    int       rc = -1;

    *outChanged = 0;
    if (ReadFile(filename, &source, &sourceLen) != 0) {
        return -1;
    }

    arenaCap64 = (uint64_t)(sourceLen + 128u) * (uint64_t)sizeof(SLAstNode) + 65536u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "arena too large\n");
        goto done;
    }
    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        fprintf(stderr, "failed to allocate arena\n");
        goto done;
    }
    SLArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    SLArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);

    if (SLFormat(&arena, (SLStrView){ source, sourceLen }, NULL, &formatted, &diag) != 0) {
        (void)PrintSLDiagLineCol(filename, source, &diag, 0);
        goto done;
    }

    changed = sourceLen != formatted.len
           || memcmp(source, formatted.ptr, sourceLen < formatted.len ? sourceLen : formatted.len)
                  != 0;
    if (changed) {
        if (checkOnly) {
            fprintf(stdout, "%s\n", filename);
            SLFmtCheckReport(filename, source, sourceLen, formatted.ptr, formatted.len);
        } else if (WriteFileAtomic(filename, formatted.ptr, formatted.len) != 0) {
            fprintf(stderr, "error: failed to write %s\n", filename);
            goto done;
        }
    }

    *outChanged = changed;
    rc = 0;

done:
    if (arenaMem != NULL) {
        SLArenaDispose(&arena);
        free(arenaMem);
    }
    free(source);
    return rc;
}

static int AddFmtPath(char*** outFiles, uint32_t* outLen, uint32_t* outCap, const char* path) {
    char* dup;
    if (EnsureCap((void**)outFiles, outCap, *outLen + 1u, sizeof(char*)) != 0) {
        return -1;
    }
    dup = DupCStr(path);
    if (dup == NULL) {
        return -1;
    }
    (*outFiles)[(*outLen)++] = dup;
    return 0;
}

static int RunFmtCommand(int argc, const char* const* argv) {
    int      checkOnly = 0;
    char**   files = NULL;
    uint32_t fileLen = 0;
    uint32_t fileCap = 0;
    uint32_t i;
    int      hadMismatch = 0;
    int      hadError = 0;

    for (i = 0; i < (uint32_t)argc; i++) {
        const char* arg = argv[i];
        if (StrEq(arg, "--check")) {
            checkOnly = 1;
            continue;
        }
        {
            struct stat st;
            if (stat(arg, &st) != 0) {
                fprintf(stderr, "error: path does not exist: %s\n", arg);
                hadError = 1;
                continue;
            }
            if (S_ISDIR(st.st_mode)) {
                char**   dirFiles = NULL;
                uint32_t dirLen = 0;
                uint32_t j;
                if (ListTopLevelSLFilesForFmt(arg, &dirFiles, &dirLen) != 0) {
                    hadError = 1;
                    continue;
                }
                for (j = 0; j < dirLen; j++) {
                    if (AddFmtPath(&files, &fileLen, &fileCap, dirFiles[j]) != 0) {
                        hadError = 1;
                    }
                    free(dirFiles[j]);
                }
                free(dirFiles);
                continue;
            }
            if (!S_ISREG(st.st_mode)) {
                fprintf(stderr, "error: not a regular file or directory: %s\n", arg);
                hadError = 1;
                continue;
            }
            if (!HasSuffix(arg, ".sl")) {
                fprintf(stderr, "error: not an .sl file: %s\n", arg);
                hadError = 1;
                continue;
            }
            if (AddFmtPath(&files, &fileLen, &fileCap, arg) != 0) {
                fprintf(stderr, "error: out of memory\n");
                hadError = 1;
                continue;
            }
        }
    }

    if (argc == 0) {
        char**   dirFiles = NULL;
        uint32_t dirLen = 0;
        uint32_t j;
        if (ListTopLevelSLFilesForFmt(".", &dirFiles, &dirLen) != 0) {
            hadError = 1;
        } else {
            for (j = 0; j < dirLen; j++) {
                if (AddFmtPath(&files, &fileLen, &fileCap, dirFiles[j]) != 0) {
                    hadError = 1;
                }
                free(dirFiles[j]);
            }
            free(dirFiles);
        }
    }

    if (fileLen > 1u) {
        qsort(files, (size_t)fileLen, sizeof(char*), CompareStringPtrs);
    }
    for (i = 0; i < fileLen; i++) {
        int changed = 0;
        if (FormatOneFile(files[i], checkOnly, &changed) != 0) {
            hadError = 1;
            continue;
        }
        if (changed) {
            hadMismatch = 1;
        }
    }

    for (i = 0; i < fileLen; i++) {
        free(files[i]);
    }
    free(files);

    if (hadError) {
        return 1;
    }
    if (checkOnly && hadMismatch) {
        return 1;
    }
    return 0;
}

static void PrintEscaped(FILE* out, const char* s, uint32_t start, uint32_t end) {
    uint32_t i;

    fputc('"', out);
    for (i = start; i < end; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (c >= 0x20 && c <= 0x7e) {
                    fputc((int)c, out);
                } else {
                    fprintf(out, "\\x%02x", (unsigned)c);
                }
                break;
        }
    }
    fputc('"', out);
}

static void StdoutWrite(void* ctx, const char* data, uint32_t len) {
    (void)ctx;
    if (len == 0) {
        return;
    }
    fwrite(data, 1u, (size_t)len, stdout);
}

static int DumpTokens(const char* filename, const char* source, uint32_t sourceLen) {
    void*         arenaMem;
    uint64_t      arenaCap64;
    size_t        arenaCap;
    SLArena       arena;
    SLTokenStream stream;
    SLDiag        diag = { 0 };
    uint32_t      i;

    arenaCap64 = (uint64_t)(sourceLen + 16u) * (uint64_t)sizeof(SLToken) + 4096u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "arena too large\n");
        return -1;
    }

    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        fprintf(stderr, "failed to allocate arena\n");
        return -1;
    }

    SLArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (SLLex(&arena, (SLStrView){ source, sourceLen }, &stream, &diag) != 0) {
        int diagStatus = PrintSLDiag(filename, source, &diag, 0);
        free(arenaMem);
        return diagStatus;
    }

    for (i = 0; i < stream.len; i++) {
        const SLToken* t = &stream.v[i];
        printf("%s %u %u ", SLTokenKindName(t->kind), t->start, t->end);
        if (t->kind == SLTok_EOF) {
            printf("<eof>");
        } else if (t->kind == SLTok_SEMICOLON && t->start == t->end) {
            printf("<auto>");
        } else {
            PrintEscaped(stdout, source, t->start, t->end);
        }
        fputc('\n', stdout);
    }

    free(arenaMem);
    return 0;
}

static int DumpAST(const char* filename, const char* source, uint32_t sourceLen) {
    void*    arenaMem;
    uint64_t arenaCap64;
    size_t   arenaCap;
    SLArena  arena;
    SLAst    ast;
    SLDiag   diag = { 0 };
    SLWriter writer;

    arenaCap64 = (uint64_t)(sourceLen + 64u) * (uint64_t)sizeof(SLAstNode) + 32768u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "arena too large\n");
        return -1;
    }

    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        fprintf(stderr, "failed to allocate arena\n");
        return -1;
    }

    SLArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (SLParse(&arena, (SLStrView){ source, sourceLen }, NULL, &ast, NULL, &diag) != 0) {
        int diagStatus = PrintSLDiag(filename, source, &diag, 0);
        free(arenaMem);
        return diagStatus;
    }

    writer.ctx = NULL;
    writer.write = StdoutWrite;
    if (SLAstDump(&ast, (SLStrView){ source, sourceLen }, &writer, &diag) != 0) {
        int diagStatus = PrintSLDiag(filename, source, &diag, 0);
        free(arenaMem);
        return diagStatus;
    }

    free(arenaMem);
    return 0;
}

static int FindFunctionBodyNode(const SLParsedFile* file, int32_t fnNode) {
    int32_t child = ASTFirstChild(&file->ast, fnNode);
    while (child >= 0) {
        if (file->ast.nodes[child].kind == SLAst_BLOCK) {
            return child;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return -1;
}

static int ErrorMirUnsupported(
    const SLParsedFile* file,
    const SLAstNode*    node,
    const char*         kind,
    const SLDiag* _Nullable diag) {
    uint32_t    start = node != NULL ? node->start : 0u;
    uint32_t    end = node != NULL ? node->end : 0u;
    const char* detail = diag != NULL && diag->detail != NULL ? diag->detail : "not supported";
    if (diag != NULL && diag->end > diag->start) {
        start = diag->start;
        end = diag->end;
    }
    return Errorf(
        file->path,
        file->source,
        start,
        end,
        "MIR lowering does not yet support %s: %s",
        kind,
        detail);
}

typedef enum {
    SLMirDeclKind_NONE = 0,
    SLMirDeclKind_FN,
    SLMirDeclKind_CONST,
    SLMirDeclKind_VAR,
} SLMirDeclKind;

typedef struct {
    const SLPackage* pkg;
    const char*      src;
    uint32_t         nameStart;
    uint32_t         nameEnd;
    uint32_t         functionIndex;
    uint8_t          kind;
} SLMirResolvedDecl;

typedef struct {
    SLMirResolvedDecl* _Nullable v;
    uint32_t len;
    uint32_t cap;
} SLMirResolvedDeclMap;

static int AddMirResolvedDecl(
    SLMirResolvedDeclMap* map,
    const SLPackage*      pkg,
    const char*           src,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    uint32_t              functionIndex,
    SLMirDeclKind         kind) {
    if (map == NULL || pkg == NULL || src == NULL || nameEnd <= nameStart
        || kind == SLMirDeclKind_NONE)
    {
        return -1;
    }
    if (EnsureCap((void**)&map->v, &map->cap, map->len + 1u, sizeof(SLMirResolvedDecl)) != 0) {
        return -1;
    }
    map->v[map->len++] = (SLMirResolvedDecl){
        .pkg = pkg,
        .src = src,
        .nameStart = nameStart,
        .nameEnd = nameEnd,
        .functionIndex = functionIndex,
        .kind = (uint8_t)kind,
    };
    return 0;
}

static const SLMirResolvedDecl* _Nullable FindMirResolvedDeclBySlice(
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    nameStart,
    uint32_t                    nameEnd,
    SLMirDeclKind               kind) {
    uint32_t i;
    if (map == NULL || pkg == NULL || src == NULL || nameEnd <= nameStart) {
        return NULL;
    }
    for (i = 0; i < map->len; i++) {
        const SLMirResolvedDecl* entry = &map->v[i];
        if (entry->pkg != pkg || entry->kind != (uint8_t)kind) {
            continue;
        }
        if (SliceEqSlice(entry->src, entry->nameStart, entry->nameEnd, src, nameStart, nameEnd)) {
            return entry;
        }
    }
    return NULL;
}

static const SLMirResolvedDecl* _Nullable FindMirResolvedDeclByCStr(
    const SLMirResolvedDeclMap* map, const SLPackage* pkg, const char* name, SLMirDeclKind kind) {
    uint32_t i;
    size_t   nameLen;
    if (map == NULL || pkg == NULL || name == NULL) {
        return NULL;
    }
    nameLen = strlen(name);
    for (i = 0; i < map->len; i++) {
        const SLMirResolvedDecl* entry = &map->v[i];
        if (entry->pkg != pkg || entry->kind != (uint8_t)kind) {
            continue;
        }
        if ((size_t)(entry->nameEnd - entry->nameStart) == nameLen
            && memcmp(entry->src + entry->nameStart, name, nameLen) == 0)
        {
            return entry;
        }
    }
    return NULL;
}

static const SLMirResolvedDecl* _Nullable FindMirResolvedValueBySlice(
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    nameStart,
    uint32_t                    nameEnd) {
    const SLMirResolvedDecl* entry = FindMirResolvedDeclBySlice(
        map, pkg, src, nameStart, nameEnd, SLMirDeclKind_CONST);
    if (entry != NULL) {
        return entry;
    }
    return FindMirResolvedDeclBySlice(map, pkg, src, nameStart, nameEnd, SLMirDeclKind_VAR);
}

static const SLMirResolvedDecl* _Nullable FindMirResolvedValueByCStr(
    const SLMirResolvedDeclMap* map, const SLPackage* pkg, const char* name) {
    const SLMirResolvedDecl* entry = FindMirResolvedDeclByCStr(map, pkg, name, SLMirDeclKind_CONST);
    if (entry != NULL) {
        return entry;
    }
    return FindMirResolvedDeclByCStr(map, pkg, name, SLMirDeclKind_VAR);
}

static int AppendMirDeclsFromFile(
    SLMirProgramBuilder* builder,
    SLArena*             arena,
    const SLPackage*     pkg,
    const SLParsedFile*  file,
    SLMirResolvedDeclMap* _Nullable declMap) {
    int32_t child = ASTFirstChild(&file->ast, file->ast.root);
    while (child >= 0) {
        const SLAstNode* n = &file->ast.nodes[child];
        if (n->kind == SLAst_FN) {
            uint32_t     outFunctionIndex = UINT32_MAX;
            int32_t      bodyNode;
            SLDiag       diag = { 0 };
            int          supported = 0;
            SLStrView    src = { file->source, file->sourceLen };
            const SLAst* ast = &file->ast;
            bodyNode = FindFunctionBodyNode(file, child);
            if (bodyNode >= 0) {
                if (SLMirLowerAppendSimpleFunction(
                        builder,
                        arena,
                        ast,
                        src,
                        child,
                        bodyNode,
                        &outFunctionIndex,
                        &supported,
                        &diag)
                    != 0)
                {
                    return PrintSLDiagLineCol(file->path, file->source, &diag, 0);
                }
                if (!supported) {
                    return ErrorMirUnsupported(file, &ast->nodes[child], "function body", &diag);
                }
                if (declMap != NULL
                    && AddMirResolvedDecl(
                           declMap,
                           pkg,
                           file->source,
                           n->dataStart,
                           n->dataEnd,
                           outFunctionIndex,
                           SLMirDeclKind_FN)
                           != 0)
                {
                    return ErrorSimple("out of memory");
                }
            }
        } else if (n->kind == SLAst_VAR || n->kind == SLAst_CONST) {
            const SLAst* ast = &file->ast;
            const char*  kindName = n->kind == SLAst_CONST ? "top-level const" : "top-level var";
            int32_t      firstChild = ASTFirstChild(ast, child);
            SLStrView    src = { file->source, file->sourceLen };
            SLDiag       diag = { 0 };
            int          supported = 0;
            if (firstChild >= 0 && ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
                uint32_t i;
                uint32_t nameCount = AstListCount(ast, firstChild);
                for (i = 0; i < nameCount; i++) {
                    uint32_t         outFunctionIndex = UINT32_MAX;
                    int32_t          nameNode = AstListItemAt(ast, firstChild, i);
                    const SLAstNode* nameAst =
                        (nameNode >= 0 && (uint32_t)nameNode < ast->len)
                            ? &ast->nodes[nameNode]
                            : NULL;
                    if (nameAst == NULL) {
                        return ErrorMirUnsupported(file, n, kindName, NULL);
                    }
                    diag = (SLDiag){ 0 };
                    supported = 0;
                    if (SLMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
                            builder,
                            arena,
                            ast,
                            src,
                            child,
                            nameAst->dataStart,
                            nameAst->dataEnd,
                            &outFunctionIndex,
                            &supported,
                            &diag)
                        != 0)
                    {
                        return PrintSLDiagLineCol(file->path, file->source, &diag, 0);
                    }
                    if (!supported) {
                        return ErrorMirUnsupported(file, nameAst, kindName, &diag);
                    }
                    if (declMap != NULL
                        && AddMirResolvedDecl(
                               declMap,
                               pkg,
                               file->source,
                               nameAst->dataStart,
                               nameAst->dataEnd,
                               outFunctionIndex,
                               n->kind == SLAst_CONST ? SLMirDeclKind_CONST : SLMirDeclKind_VAR)
                               != 0)
                    {
                        return ErrorSimple("out of memory");
                    }
                }
            } else {
                uint32_t outFunctionIndex = UINT32_MAX;
                if (SLMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
                        builder,
                        arena,
                        ast,
                        src,
                        child,
                        n->dataStart,
                        n->dataEnd,
                        &outFunctionIndex,
                        &supported,
                        &diag)
                    != 0)
                {
                    return PrintSLDiagLineCol(file->path, file->source, &diag, 0);
                }
                if (!supported) {
                    return ErrorMirUnsupported(file, n, kindName, &diag);
                }
                if (declMap != NULL
                    && AddMirResolvedDecl(
                           declMap,
                           pkg,
                           file->source,
                           n->dataStart,
                           n->dataEnd,
                           outFunctionIndex,
                           n->kind == SLAst_CONST ? SLMirDeclKind_CONST : SLMirDeclKind_VAR)
                           != 0)
                {
                    return ErrorSimple("out of memory");
                }
            }
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return 0;
}

static int IsPlatformImportPath(const char* _Nullable path) {
    return path != NULL && (StrEq(path, "platform") || strncmp(path, "platform/", 9u) == 0);
}

static int ShouldSkipPackageMirImportPath(const char* _Nullable path) {
    return IsPlatformImportPath(path)
        || (path != NULL
            && (StrEq(path, "builtin") || StrEq(path, "reflect")
                || strncmp(path, "builtin/", 8u) == 0 || strncmp(path, "reflect/", 8u) == 0));
}

static int BuildEntryPackageMirOrderVisit(
    const SLPackageLoader* loader,
    uint32_t               pkgIndex,
    uint8_t*               state,
    uint32_t*              order,
    uint32_t*              orderLen) {
    const SLPackage* pkg = &loader->packages[pkgIndex];
    uint32_t         i;
    if (state[pkgIndex] == 2u) {
        return 0;
    }
    if (state[pkgIndex] == 1u) {
        return ErrorSimple("import cycle detected");
    }
    state[pkgIndex] = 1u;
    for (i = 0; i < pkg->importLen; i++) {
        int depIndex;
        if (ShouldSkipPackageMirImportPath(pkg->imports[i].path)) {
            continue;
        }
        depIndex = FindPackageIndex(loader, pkg->imports[i].target);
        if (depIndex < 0) {
            return ErrorSimple("internal error: unresolved import");
        }
        if (BuildEntryPackageMirOrderVisit(loader, (uint32_t)depIndex, state, order, orderLen) != 0)
        {
            return -1;
        }
    }
    state[pkgIndex] = 2u;
    order[(*orderLen)++] = pkgIndex;
    return 0;
}

static int BuildEntryPackageMirOrder(
    const SLPackageLoader* loader,
    const SLPackage*       entryPkg,
    uint32_t*              outOrder,
    uint32_t               outOrderCap,
    uint32_t*              outOrderLen) {
    uint8_t* state = NULL;
    int      entryPkgIndex;
    int      rc = -1;
    *outOrderLen = 0;
    if (loader == NULL || entryPkg == NULL || outOrder == NULL || outOrderLen == NULL
        || outOrderCap < loader->packageLen)
    {
        return -1;
    }
    entryPkgIndex = FindPackageIndex(loader, entryPkg);
    if (entryPkgIndex < 0) {
        return ErrorSimple("internal error: entry package missing from loader");
    }
    state = (uint8_t*)calloc(loader->packageLen, sizeof(uint8_t));
    if (state == NULL) {
        return ErrorSimple("out of memory");
    }
    rc = BuildEntryPackageMirOrderVisit(
        loader, (uint32_t)entryPkgIndex, state, outOrder, outOrderLen);
    free(state);
    return rc;
}

static const SLParsedFile* _Nullable FindLoaderFileByMirSource(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    uint32_t               sourceRef,
    const SLPackage** _Nullable outPkg) {
    uint32_t pkgIndex;
    if (outPkg != NULL) {
        *outPkg = NULL;
    }
    if (loader == NULL || program == NULL || sourceRef >= program->sourceLen) {
        return NULL;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (pkg->files[fileIndex].source == program->sources[sourceRef].src.ptr
                && pkg->files[fileIndex].sourceLen == program->sources[sourceRef].src.len)
            {
                if (outPkg != NULL) {
                    *outPkg = pkg;
                }
                return &pkg->files[fileIndex];
            }
        }
    }
    return NULL;
}

static int DecodeNewExprNodes(
    const SLParsedFile* file,
    int32_t             nodeId,
    int32_t*            outTypeNode,
    int32_t*            outCountNode,
    int32_t*            outInitNode,
    int32_t*            outAllocNode) {
    const SLAstNode* n;
    int32_t          nextNode;
    int              hasCount;
    int              hasInit;
    int              hasAlloc;
    if (outTypeNode != NULL) {
        *outTypeNode = -1;
    }
    if (outCountNode != NULL) {
        *outCountNode = -1;
    }
    if (outInitNode != NULL) {
        *outInitNode = -1;
    }
    if (outAllocNode != NULL) {
        *outAllocNode = -1;
    }
    if (file == NULL || nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    if (n->kind != SLAst_NEW) {
        return 0;
    }
    hasCount = (n->flags & SLAstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & SLAstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & SLAstFlag_NEW_HAS_ALLOC) != 0;
    if (outTypeNode == NULL || outCountNode == NULL || outInitNode == NULL || outAllocNode == NULL)
    {
        return 0;
    }
    *outTypeNode = ASTFirstChild(&file->ast, nodeId);
    if (*outTypeNode < 0) {
        return 0;
    }
    nextNode = ASTNextSibling(&file->ast, *outTypeNode);
    if (hasCount) {
        *outCountNode = nextNode;
        if (*outCountNode < 0) {
            return 0;
        }
        nextNode = ASTNextSibling(&file->ast, *outCountNode);
    }
    if (hasInit) {
        *outInitNode = nextNode;
        if (*outInitNode < 0) {
            return 0;
        }
        nextNode = ASTNextSibling(&file->ast, *outInitNode);
    }
    if (hasAlloc) {
        *outAllocNode = nextNode;
        if (*outAllocNode < 0) {
            return 0;
        }
        nextNode = ASTNextSibling(&file->ast, *outAllocNode);
    }
    return nextNode < 0;
}

static int ParseMirIntLiteral(const char* src, uint32_t start, uint32_t end, int64_t* out) {
    uint64_t value = 0;
    uint32_t i;
    uint32_t base = 10u;
    if (src == NULL || out == NULL || end <= start) {
        return 0;
    }
    if (end - start >= 3u && src[start] == '0'
        && (src[start + 1u] == 'x' || src[start + 1u] == 'X'))
    {
        base = 16u;
        start += 2u;
        if (end <= start) {
            return 0;
        }
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)src[i];
        uint32_t      digit;
        if (ch >= (unsigned char)'0' && ch <= (unsigned char)'9') {
            digit = (uint32_t)(ch - (unsigned char)'0');
        } else if (base == 16u && ch >= (unsigned char)'a' && ch <= (unsigned char)'f') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'a');
        } else if (base == 16u && ch >= (unsigned char)'A' && ch <= (unsigned char)'F') {
            digit = 10u + (uint32_t)(ch - (unsigned char)'A');
        } else {
            return 0;
        }
        if (digit >= base) {
            return 0;
        }
        if (value > (uint64_t)INT64_MAX / (uint64_t)base
            || (value == (uint64_t)INT64_MAX / (uint64_t)base
                && (uint64_t)digit > (uint64_t)INT64_MAX % (uint64_t)base))
        {
            return 0;
        }
        value = value * (uint64_t)base + (uint64_t)digit;
    }
    *out = (int64_t)value;
    return 1;
}

static uint32_t FindMirSourceRefByText(
    const SLMirProgram* program, const char* src, uint32_t srcLen);
static int FindMirTypeRefByAstNode(
    const SLMirProgram* program, uint32_t sourceRef, int32_t astNode, uint32_t* outTypeRef);
static int AppendMirInst(
    SLMirInst* outInsts, uint32_t outCap, uint32_t* outLen, const SLMirInst* inst);
static int MirTypeNodeKind(SLAstKind kind);
static int ResolveMirAggregateTypeRefForTypeNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLParsedFile*    file,
    int32_t                typeNode,
    uint32_t*              outTypeRef);
static int EnsureMirAstTypeRef(
    SLArena*               arena,
    const SLPackageLoader* loader,
    SLMirProgram*          program,
    uint32_t               astNode,
    uint32_t               sourceRef,
    uint32_t* _Nonnull outTypeRef);
static int EnsureMirScalarTypeRef(
    SLArena* arena, SLMirProgram* program, SLMirTypeScalar scalar, uint32_t* _Nonnull outTypeRef);

static uint32_t MirIntKindByteWidth(SLMirIntKind intKind) {
    switch (intKind) {
        case SLMirIntKind_U8:
        case SLMirIntKind_I8:
        case SLMirIntKind_BOOL: return 1u;
        case SLMirIntKind_U16:
        case SLMirIntKind_I16:  return 2u;
        case SLMirIntKind_U32:
        case SLMirIntKind_I32:  return 4u;
        default:                return 0u;
    }
}

static int ResolveMirAllocNewPointeeTypeRef(
    const SLPackageLoader* loader,
    SLArena*               arena,
    SLMirProgram*          program,
    const SLParsedFile*    file,
    const SLMirInst*       allocInst,
    uint32_t*              outTypeRef) {
    int32_t  typeNode = -1;
    int32_t  countNode = -1;
    int32_t  initNode = -1;
    int32_t  allocNode = -1;
    uint32_t sourceRef;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (loader == NULL || arena == NULL || program == NULL || file == NULL || allocInst == NULL
        || outTypeRef == NULL
        || !DecodeNewExprNodes(
            file, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode)
        || typeNode < 0)
    {
        return 0;
    }
    sourceRef = FindMirSourceRefByText(program, file->source, file->sourceLen);
    if (sourceRef == UINT32_MAX) {
        return 0;
    }
    if (ResolveMirAggregateTypeRefForTypeNode(loader, program, file, typeNode, outTypeRef)) {
        return 1;
    }
    if (FindMirTypeRefByAstNode(program, sourceRef, typeNode, outTypeRef)) {
        return 1;
    }
    return EnsureMirAstTypeRef(arena, loader, program, (uint32_t)typeNode, sourceRef, outTypeRef)
        == 0;
}

static int EnsureMirIntConst(
    SLArena* arena, SLMirProgram* program, int64_t value, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_INT && (int64_t)program->consts[i].bits == value)
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (SLMirConst){
        .kind = SLMirConst_INT,
        .aux = 0u,
        .bits = (uint64_t)value,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirNullConst(SLArena* arena, SLMirProgram* program, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_NULL) {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] =
        (SLMirConst){ .kind = SLMirConst_NULL, .aux = 0u, .bits = 0u, .bytes = { 0 } };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirBoolConst(
    SLArena* arena, SLMirProgram* program, bool value, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_BOOL
            && ((program->consts[i].bits != 0u) == value))
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (SLMirConst){
        .kind = SLMirConst_BOOL,
        .aux = 0u,
        .bits = value ? 1u : 0u,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirStringConst(
    SLArena* arena, SLMirProgram* program, SLStrView value, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_STRING
            && program->consts[i].bytes.len == value.len
            && (value.len == 0u || memcmp(program->consts[i].bytes.ptr, value.ptr, value.len) == 0))
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] =
        (SLMirConst){ .kind = SLMirConst_STRING, .aux = 0u, .bits = 0u, .bytes = value };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static int EnsureMirFunctionConst(
    SLArena* arena, SLMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outIndex) {
    SLMirConst* newConsts;
    uint32_t    i;
    if (arena == NULL || program == NULL || outIndex == NULL || functionIndex >= program->funcLen) {
        return -1;
    }
    for (i = 0; i < program->constLen; i++) {
        if (program->consts[i].kind == SLMirConst_FUNCTION
            && program->consts[i].aux == functionIndex)
        {
            *outIndex = i;
            return 0;
        }
    }
    newConsts = (SLMirConst*)SLArenaAlloc(
        arena, sizeof(SLMirConst) * (program->constLen + 1u), (uint32_t)_Alignof(SLMirConst));
    if (newConsts == NULL) {
        return -1;
    }
    if (program->constLen != 0u) {
        memcpy(newConsts, program->consts, sizeof(SLMirConst) * program->constLen);
    }
    newConsts[program->constLen] = (SLMirConst){
        .kind = SLMirConst_FUNCTION,
        .aux = functionIndex,
        .bits = functionIndex,
        .bytes = { 0 },
    };
    program->consts = newConsts;
    *outIndex = program->constLen++;
    return 0;
}

static bool MirSourceSliceEq(
    const SLMirProgram* program,
    uint32_t            sourceRefA,
    uint32_t            startA,
    uint32_t            endA,
    uint32_t            sourceRefB,
    uint32_t            startB,
    uint32_t            endB) {
    uint32_t len;
    if (program == NULL || sourceRefA >= program->sourceLen || sourceRefB >= program->sourceLen
        || endA < startA || endB < startB)
    {
        return false;
    }
    len = endA - startA;
    if (len != endB - startB || program->sources[sourceRefA].src.ptr == NULL
        || program->sources[sourceRefB].src.ptr == NULL)
    {
        return false;
    }
    return len == 0u
        || memcmp(
               program->sources[sourceRefA].src.ptr + startA,
               program->sources[sourceRefB].src.ptr + startB,
               len)
               == 0;
}

static uint32_t FindMirSourceRefByText(
    const SLMirProgram* program, const char* src, uint32_t srcLen) {
    uint32_t i;
    if (program == NULL || src == NULL) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->sourceLen; i++) {
        if (program->sources[i].src.len == srcLen
            && memcmp(program->sources[i].src.ptr, src, srcLen) == 0)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static int FindMirTypeRefByAstNode(
    const SLMirProgram* program, uint32_t sourceRef, int32_t astNode, uint32_t* outTypeRef) {
    uint32_t i;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (program == NULL || outTypeRef == NULL || astNode < 0) {
        return 0;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].sourceRef == sourceRef
            && program->types[i].astNode == (uint32_t)astNode)
        {
            *outTypeRef = i;
            return 1;
        }
    }
    return 0;
}

static int FindMirFieldByOwnerAndSlice(
    const SLMirProgram* program,
    uint32_t            ownerTypeRef,
    uint32_t            sourceRef,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    uint32_t*           outFieldIndex) {
    uint32_t i;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (program == NULL || outFieldIndex == NULL || nameEnd < nameStart) {
        return 0;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef != ownerTypeRef) {
            continue;
        }
        if (MirSourceSliceEq(
                program,
                program->fields[i].sourceRef,
                program->fields[i].nameStart,
                program->fields[i].nameEnd,
                sourceRef,
                nameStart,
                nameEnd))
        {
            *outFieldIndex = i;
            return 1;
        }
    }
    return 0;
}

static int FindMirPseudoFieldByName(
    const SLMirProgram* program, const char* name, uint32_t* outFieldIndex) {
    uint32_t i;
    size_t   nameLen = 0u;
    if (outFieldIndex != NULL) {
        *outFieldIndex = UINT32_MAX;
    }
    if (program == NULL || name == NULL || outFieldIndex == NULL) {
        return 0;
    }
    while (name[nameLen] != '\0') {
        nameLen++;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef != UINT32_MAX
            || program->fields[i].sourceRef >= program->sourceLen
            || program->fields[i].nameEnd < program->fields[i].nameStart
            || (size_t)(program->fields[i].nameEnd - program->fields[i].nameStart) != nameLen)
        {
            continue;
        }
        if (memcmp(
                program->sources[program->fields[i].sourceRef].src.ptr
                    + program->fields[i].nameStart,
                name,
                nameLen)
            == 0)
        {
            *outFieldIndex = i;
            return 1;
        }
    }
    return 0;
}

static int FindMirFunctionLocalBySlice(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    const char*          src,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t*            outLocalIndex) {
    uint32_t i;
    uint32_t nameLen;
    if (outLocalIndex != NULL) {
        *outLocalIndex = UINT32_MAX;
    }
    if (program == NULL || fn == NULL || src == NULL || outLocalIndex == NULL
        || nameEnd < nameStart)
    {
        return 0;
    }
    nameLen = nameEnd - nameStart;
    for (i = 0; i < fn->localCount; i++) {
        const SLMirLocal* local = &program->locals[fn->localStart + i];
        if (local->nameEnd >= local->nameStart && local->nameEnd - local->nameStart == nameLen
            && memcmp(src + local->nameStart, src + nameStart, nameLen) == 0)
        {
            *outLocalIndex = i;
            return 1;
        }
    }
    return 0;
}

static int CountMirAllocNewCountExprInsts(
    const SLParsedFile* file, int32_t exprNode, uint32_t* _Nonnull outCount) {
    const SLAstNode* n;
    uint32_t         leftCount = 0u;
    uint32_t         rightCount = 0u;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (file == NULL || outCount == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case SLAst_INT:
        case SLAst_IDENT: *outCount = 1u; return 1;
        case SLAst_UNARY:
            if (!CountMirAllocNewCountExprInsts(file, n->firstChild, &leftCount)) {
                return 0;
            }
            *outCount = leftCount + 1u;
            return 1;
        case SLAst_BINARY: {
            int32_t rhsNode = ASTNextSibling(&file->ast, n->firstChild);
            if (!CountMirAllocNewCountExprInsts(file, n->firstChild, &leftCount) || rhsNode < 0
                || !CountMirAllocNewCountExprInsts(file, rhsNode, &rightCount))
            {
                return 0;
            }
            *outCount = leftCount + rightCount + 1u;
            return 1;
        }
        default: return 0;
    }
}

static int CountMirAllocNewAllocExprInsts(
    const SLParsedFile* file, int32_t exprNode, uint32_t* _Nonnull outCount) {
    const SLAstNode* n;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (file == NULL || outCount == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case SLAst_IDENT:
        case SLAst_NULL:  *outCount = 1u; return 1;
        case SLAst_CAST:  {
            int32_t lhsNode = n->firstChild;
            if (lhsNode < 0 || (uint32_t)lhsNode >= file->ast.len) {
                return 0;
            }
            return CountMirAllocNewAllocExprInsts(file, lhsNode, outCount);
        }
        case SLAst_FIELD_EXPR: {
            int32_t baseNode = n->firstChild;
            if (baseNode < 0 || (uint32_t)baseNode >= file->ast.len) {
                return 0;
            }
            if (file->ast.nodes[baseNode].kind == SLAst_IDENT
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[baseNode].dataStart,
                    file->ast.nodes[baseNode].dataEnd,
                    "context")
                && (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "mem")
                    || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "temp_mem")))
            {
                *outCount = 1u;
                return 1;
            }
            return 0;
        }
        default: return 0;
    }
}

static int LowerMirAllocNewCountExpr(
    const SLMirProgram*  program,
    SLMirProgram*        mutableProgram,
    const SLMirFunction* fn,
    const SLParsedFile*  file,
    int32_t              exprNode,
    SLArena*             arena,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen) {
    const SLAstNode* n;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (program == NULL || mutableProgram == NULL || fn == NULL || file == NULL || arena == NULL
        || outInsts == NULL || outLen == NULL || exprNode < 0
        || (uint32_t)exprNode >= file->ast.len)
    {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case SLAst_INT: {
            int64_t  value = 0;
            uint32_t constIndex = UINT32_MAX;
            if (outCap < 1u || !ParseMirIntLiteral(file->source, n->dataStart, n->dataEnd, &value)
                || EnsureMirIntConst(arena, mutableProgram, value, &constIndex) != 0)
            {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_PUSH_CONST,
                .tok = 0u,
                ._reserved = 0u,
                .aux = constIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_IDENT: {
            uint32_t localIndex = UINT32_MAX;
            if (outCap < 1u
                || !FindMirFunctionLocalBySlice(
                    program, fn, file->source, n->dataStart, n->dataEnd, &localIndex))
            {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_LOCAL_LOAD,
                .tok = 0u,
                ._reserved = 0u,
                .aux = localIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_UNARY: {
            uint32_t innerLen = 0u;
            if (!LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    n->firstChild,
                    arena,
                    outInsts,
                    outCap > 0u ? outCap - 1u : 0u,
                    &innerLen)
                || innerLen + 1u > outCap)
            {
                return 0;
            }
            outInsts[innerLen] = (SLMirInst){
                .op = SLMirOp_UNARY,
                .tok = (uint16_t)n->op,
                ._reserved = 0u,
                .aux = 0u,
                .start = n->start,
                .end = n->end,
            };
            *outLen = innerLen + 1u;
            return 1;
        }
        case SLAst_BINARY: {
            uint32_t leftLen = 0u;
            uint32_t rightLen = 0u;
            int32_t  rhsNode = ASTNextSibling(&file->ast, n->firstChild);
            if (rhsNode < 0
                || !LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    n->firstChild,
                    arena,
                    outInsts,
                    outCap,
                    &leftLen)
                || !LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    rhsNode,
                    arena,
                    outInsts + leftLen,
                    outCap >= leftLen ? outCap - leftLen : 0u,
                    &rightLen)
                || leftLen + rightLen + 1u > outCap)
            {
                return 0;
            }
            outInsts[leftLen + rightLen] = (SLMirInst){
                .op = SLMirOp_BINARY,
                .tok = (uint16_t)n->op,
                ._reserved = 0u,
                .aux = 0u,
                .start = n->start,
                .end = n->end,
            };
            *outLen = leftLen + rightLen + 1u;
            return 1;
        }
        default: return 0;
    }
}

static int FindCompoundFieldValueNodeBySlice(
    const SLParsedFile* file,
    int32_t             initNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    int32_t*            outValueNode) {
    int32_t child;
    if (outValueNode != NULL) {
        *outValueNode = -1;
    }
    if (file == NULL || outValueNode == NULL || initNode < 0 || (uint32_t)initNode >= file->ast.len
        || nameEnd < nameStart)
    {
        return 0;
    }
    child = file->ast.nodes[initNode].firstChild;
    if (child >= 0 && (uint32_t)child < file->ast.len
        && MirTypeNodeKind(file->ast.nodes[child].kind))
    {
        child = file->ast.nodes[child].nextSibling;
    }
    while (child >= 0) {
        const SLAstNode* field = &file->ast.nodes[child];
        if (field->kind == SLAst_COMPOUND_FIELD && field->dataEnd >= field->dataStart
            && field->dataEnd - field->dataStart == nameEnd - nameStart
            && memcmp(
                   file->source + field->dataStart, file->source + nameStart, nameEnd - nameStart)
                   == 0)
        {
            *outValueNode = field->firstChild;
            return *outValueNode >= 0 || (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u;
        }
        child = field->nextSibling;
    }
    return 0;
}

static int FindCompoundFieldValueNodeByText(
    const SLParsedFile* file, int32_t initNode, const char* name, int32_t* outValueNode) {
    int32_t child;
    size_t  nameLen = 0u;
    if (outValueNode != NULL) {
        *outValueNode = -1;
    }
    if (file == NULL || name == NULL || outValueNode == NULL || initNode < 0
        || (uint32_t)initNode >= file->ast.len)
    {
        return 0;
    }
    while (name[nameLen] != '\0') {
        nameLen++;
    }
    child = file->ast.nodes[initNode].firstChild;
    if (child >= 0 && (uint32_t)child < file->ast.len
        && MirTypeNodeKind(file->ast.nodes[child].kind))
    {
        child = file->ast.nodes[child].nextSibling;
    }
    while (child >= 0) {
        const SLAstNode* field = &file->ast.nodes[child];
        if (field->kind == SLAst_COMPOUND_FIELD && field->dataEnd >= field->dataStart
            && (size_t)(field->dataEnd - field->dataStart) == nameLen
            && memcmp(file->source + field->dataStart, name, nameLen) == 0)
        {
            *outValueNode = field->firstChild;
            return *outValueNode >= 0 || (field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u;
        }
        child = field->nextSibling;
    }
    return 0;
}

static int AppendMirBinaryInst(
    SLMirInst* outInsts, uint32_t outCap, uint32_t* outLen, SLTokenKind tok) {
    return AppendMirInst(
        outInsts,
        outCap,
        outLen,
        &(SLMirInst){
            .op = SLMirOp_BINARY,
            .tok = (uint16_t)tok,
            ._reserved = 0u,
            .aux = 0u,
            .start = 0u,
            .end = 0u,
        });
}

static int AppendMirIntConstInst(
    SLArena*      arena,
    SLMirProgram* program,
    SLMirInst*    outInsts,
    uint32_t      outCap,
    uint32_t*     outLen,
    int64_t       value) {
    uint32_t constIndex = UINT32_MAX;
    return EnsureMirIntConst(arena, program, value, &constIndex) == 0
        && AppendMirInst(
               outInsts,
               outCap,
               outLen,
               &(SLMirInst){
                   .op = SLMirOp_PUSH_CONST,
                   .tok = 0u,
                   ._reserved = 0u,
                   .aux = constIndex,
                   .start = 0u,
                   .end = 0u,
               });
}

static int MirStaticTypeByteSize(const SLMirProgram* program, uint32_t typeRefIndex) {
    const SLMirTypeRef* typeRef;
    uint32_t            i;
    uint32_t            offset = 0u;
    uint32_t            maxAlign = 1u;
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return -1;
    }
    typeRef = &program->types[typeRefIndex];
    if (SLMirTypeRefIsAggregate(typeRef)) {
        for (i = 0; i < program->fieldLen; i++) {
            int fieldSize;
            int fieldAlign;
            if (program->fields[i].ownerTypeRef != typeRefIndex) {
                continue;
            }
            if (program->fields[i].typeRef >= program->typeLen) {
                return -1;
            }
            if (SLMirTypeRefIsVArrayView(&program->types[program->fields[i].typeRef])) {
                fieldSize = 0;
                fieldAlign = 1;
            } else if (SLMirTypeRefIsStrObj(&program->types[program->fields[i].typeRef])) {
                fieldSize = 8;
                fieldAlign = 4;
            } else {
                fieldSize = MirStaticTypeByteSize(program, program->fields[i].typeRef);
                fieldAlign = fieldSize >= 4 ? 4 : fieldSize;
            }
            if (fieldSize < 0 || fieldAlign <= 0) {
                return -1;
            }
            if ((uint32_t)fieldAlign > maxAlign) {
                maxAlign = (uint32_t)fieldAlign;
            }
            offset = (offset + ((uint32_t)fieldAlign - 1u)) & ~((uint32_t)fieldAlign - 1u);
            offset += (uint32_t)fieldSize;
        }
        return (int)(maxAlign > 1u ? ((offset + (maxAlign - 1u)) & ~(maxAlign - 1u)) : offset);
    }
    if (SLMirTypeRefIsStrObj(typeRef) || SLMirTypeRefIsStrRef(typeRef)
        || SLMirTypeRefIsSliceView(typeRef) || SLMirTypeRefIsAggSliceView(typeRef))
    {
        return 8;
    }
    if (SLMirTypeRefIsFixedArray(typeRef)) {
        return (int)(MirIntKindByteWidth(SLMirTypeRefIntKind(typeRef))
                     * SLMirTypeRefFixedArrayCount(typeRef));
    }
    if (SLMirTypeRefIsFixedArrayView(typeRef) || SLMirTypeRefIsStrPtr(typeRef)
        || SLMirTypeRefIsOpaquePtr(typeRef) || SLMirTypeRefIsU8Ptr(typeRef)
        || SLMirTypeRefIsI8Ptr(typeRef) || SLMirTypeRefIsU16Ptr(typeRef)
        || SLMirTypeRefIsI16Ptr(typeRef) || SLMirTypeRefIsU32Ptr(typeRef)
        || SLMirTypeRefIsI32Ptr(typeRef) || SLMirTypeRefIsFuncRef(typeRef))
    {
        return 4;
    }
    if (SLMirTypeRefScalarKind(typeRef) == SLMirTypeScalar_I32) {
        return (int)MirIntKindByteWidth(SLMirTypeRefIntKind(typeRef));
    }
    return -1;
}

static int LowerMirVarSizeAllocNewSizeExpr(
    const SLMirProgram*  program,
    SLMirProgram*        mutableProgram,
    const SLMirFunction* fn,
    const SLParsedFile*  file,
    const SLMirInst*     allocInst,
    uint32_t             pointeeTypeRef,
    SLArena*             arena,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen) {
    int32_t             typeNode = -1;
    int32_t             countNode = -1;
    int32_t             initNode = -1;
    int32_t             allocNode = -1;
    uint32_t            len = 0u;
    uint32_t            i;
    const SLMirTypeRef* pointee;
    int                 hasDynamic = 0;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (program == NULL || mutableProgram == NULL || fn == NULL || file == NULL || allocInst == NULL
        || arena == NULL || outInsts == NULL || outLen == NULL || pointeeTypeRef >= program->typeLen
        || !DecodeNewExprNodes(
            file, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    pointee = &program->types[pointeeTypeRef];
    if (SLMirTypeRefIsStrObj(pointee)) {
        int32_t lenNode = -1;
        if (initNode < 0 || !FindCompoundFieldValueNodeByText(file, initNode, "len", &lenNode)) {
            /* caller handles unsupported shape */
            return 0;
        }
        if (!LowerMirAllocNewCountExpr(
                program, mutableProgram, fn, file, lenNode, arena, outInsts, outCap, &len)
            || !AppendMirIntConstInst(arena, mutableProgram, outInsts, outCap, &len, 9)
            || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD))
        {
            return 0;
        }
        *outLen = len;
        return 1;
    }
    if (!SLMirTypeRefIsAggregate(pointee)) {
        return 0;
    }
    if (!AppendMirIntConstInst(
            arena,
            mutableProgram,
            outInsts,
            outCap,
            &len,
            MirStaticTypeByteSize(program, pointeeTypeRef)))
    {
        return 0;
    }
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField*   fieldRef;
        const SLMirTypeRef* fieldType;
        int32_t             valueNode = -1;
        uint32_t            elemSize;
        uint32_t            align;
        if (program->fields[i].ownerTypeRef != pointeeTypeRef) {
            continue;
        }
        fieldRef = &program->fields[i];
        if (fieldRef->typeRef >= program->typeLen) {
            return 0;
        }
        fieldType = &program->types[fieldRef->typeRef];
        if (SLMirTypeRefIsStrObj(fieldType)) {
            char     pathBuf[128];
            uint32_t baseLen;
            int32_t  valueNode = -1;
            if (fieldRef->sourceRef >= program->sourceLen || fieldRef->nameEnd < fieldRef->nameStart
                || initNode < 0)
            {
                return 0;
            }
            baseLen = fieldRef->nameEnd - fieldRef->nameStart;
            if (baseLen + 4u >= sizeof(pathBuf)) {
                return 0;
            }
            memcpy(
                pathBuf,
                program->sources[fieldRef->sourceRef].src.ptr + fieldRef->nameStart,
                (size_t)baseLen);
            memcpy(pathBuf + baseLen, ".len", 4u);
            pathBuf[baseLen + 4u] = '\0';
            hasDynamic = 1;
            (void)FindCompoundFieldValueNodeByText(file, initNode, pathBuf, &valueNode);
            if (valueNode >= 0) {
                uint32_t valueLen = 0u;
                if (!LowerMirAllocNewCountExpr(
                        program,
                        mutableProgram,
                        fn,
                        file,
                        valueNode,
                        arena,
                        outInsts + len,
                        outCap >= len ? outCap - len : 0u,
                        &valueLen))
                {
                    return 0;
                }
                len += valueLen;
            } else if (!AppendMirIntConstInst(arena, mutableProgram, outInsts, outCap, &len, 0)) {
                return 0;
            }
            if (!AppendMirIntConstInst(arena, mutableProgram, outInsts, outCap, &len, 1)
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD)
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD))
            {
                return 0;
            }
            continue;
        }
        if (!SLMirTypeRefIsVArrayView(fieldType)) {
            continue;
        }
        hasDynamic = 1;
        elemSize = MirIntKindByteWidth(SLMirTypeRefIntKind(fieldType));
        align = elemSize >= 4u ? 4u : elemSize;
        if (align > 1u
            && (!AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, (int64_t)(align - 1u))
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD)
                || !AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, -(int64_t)align)
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_AND)))
        {
            return 0;
        }
        if (SLMirTypeRefVArrayCountField(fieldType) == UINT32_MAX
            || SLMirTypeRefVArrayCountField(fieldType) >= program->fieldLen)
        {
            return 0;
        }
        {
            const SLMirField* countField =
                &program->fields[SLMirTypeRefVArrayCountField(fieldType)];
            int32_t  countValueNode = -1;
            uint32_t countExprLen = 0u;
            if (!FindCompoundFieldValueNodeBySlice(
                    file, initNode, countField->nameStart, countField->nameEnd, &countValueNode)
                || !LowerMirAllocNewCountExpr(
                    program,
                    mutableProgram,
                    fn,
                    file,
                    countValueNode,
                    arena,
                    outInsts + len,
                    outCap >= len ? outCap - len : 0u,
                    &countExprLen))
            {
                return 0;
            }
            len += countExprLen;
        }
        if (elemSize > 1u
            && (!AppendMirIntConstInst(
                    arena, mutableProgram, outInsts, outCap, &len, (int64_t)elemSize)
                || !AppendMirBinaryInst(outInsts, outCap, &len, SLTok_MUL)))
        {
            return 0;
        }
        if (!AppendMirBinaryInst(outInsts, outCap, &len, SLTok_ADD)) {
            return 0;
        }
    }
    if (!hasDynamic) {
        return 0;
    }
    *outLen = len;
    return 1;
}

static int CountMirVarSizeAllocNewSizeExpr(
    const SLMirProgram* program,
    const SLParsedFile* file,
    const SLMirInst*    allocInst,
    uint32_t            pointeeTypeRef,
    uint32_t*           outCount) {
    int32_t             typeNode = -1;
    int32_t             countNode = -1;
    int32_t             initNode = -1;
    int32_t             allocNode = -1;
    uint32_t            count = 0u;
    uint32_t            i;
    int                 hasDynamic = 0;
    const SLMirTypeRef* pointee;
    if (outCount != NULL) {
        *outCount = 0u;
    }
    if (program == NULL || file == NULL || allocInst == NULL || outCount == NULL
        || pointeeTypeRef >= program->typeLen
        || !DecodeNewExprNodes(
            file, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    pointee = &program->types[pointeeTypeRef];
    if (SLMirTypeRefIsStrObj(pointee)) {
        int32_t  lenNode = -1;
        uint32_t exprCount = 0u;
        if (initNode < 0 || !FindCompoundFieldValueNodeByText(file, initNode, "len", &lenNode)
            || !CountMirAllocNewCountExprInsts(file, lenNode, &exprCount))
        {
            return 0;
        }
        *outCount = exprCount + 2u;
        return 1;
    }
    if (!SLMirTypeRefIsAggregate(pointee)) {
        return 0;
    }
    count = 1u;
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField*   fieldRef;
        const SLMirTypeRef* fieldType;
        if (program->fields[i].ownerTypeRef != pointeeTypeRef) {
            continue;
        }
        fieldRef = &program->fields[i];
        if (fieldRef->typeRef >= program->typeLen) {
            return 0;
        }
        fieldType = &program->types[fieldRef->typeRef];
        if (SLMirTypeRefIsStrObj(fieldType)) {
            char     pathBuf[128];
            uint32_t baseLen;
            int32_t  valueNode = -1;
            uint32_t exprCount = 0u;
            if (fieldRef->sourceRef >= program->sourceLen || fieldRef->nameEnd < fieldRef->nameStart
                || initNode < 0)
            {
                return 0;
            }
            baseLen = fieldRef->nameEnd - fieldRef->nameStart;
            if (baseLen + 4u >= sizeof(pathBuf)) {
                return 0;
            }
            memcpy(
                pathBuf,
                program->sources[fieldRef->sourceRef].src.ptr + fieldRef->nameStart,
                (size_t)baseLen);
            memcpy(pathBuf + baseLen, ".len", 4u);
            pathBuf[baseLen + 4u] = '\0';
            hasDynamic = 1;
            if (FindCompoundFieldValueNodeByText(file, initNode, pathBuf, &valueNode)) {
                if (valueNode < 0 || !CountMirAllocNewCountExprInsts(file, valueNode, &exprCount)) {
                    return 0;
                }
                count += exprCount;
            } else {
                count += 1u;
            }
            count += 3u;
            continue;
        }
        if (SLMirTypeRefIsVArrayView(fieldType)) {
            const SLMirField* countField;
            int32_t           countValueNode = -1;
            uint32_t          exprCount = 0u;
            uint32_t          elemSize = MirIntKindByteWidth(SLMirTypeRefIntKind(fieldType));
            uint32_t          countFieldRef = SLMirTypeRefVArrayCountField(fieldType);
            hasDynamic = 1;
            if (countFieldRef == UINT32_MAX || countFieldRef >= program->fieldLen || initNode < 0) {
                return 0;
            }
            countField = &program->fields[countFieldRef];
            if (!FindCompoundFieldValueNodeBySlice(
                    file, initNode, countField->nameStart, countField->nameEnd, &countValueNode)
                || countValueNode < 0
                || !CountMirAllocNewCountExprInsts(file, countValueNode, &exprCount))
            {
                return 0;
            }
            if (elemSize >= 4u) {
                count += 4u;
            } else if (elemSize == 2u) {
                count += 4u;
            }
            count += exprCount;
            if (elemSize > 1u) {
                count += 2u;
            }
            count += 1u;
        }
    }
    if (!hasDynamic) {
        return 0;
    }
    *outCount = count;
    return 1;
}

static int LowerMirAllocNewAllocExpr(
    const SLMirProgram*  program,
    SLMirProgram*        mutableProgram,
    const SLMirFunction* fn,
    const SLParsedFile*  file,
    int32_t              exprNode,
    SLArena*             arena,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen) {
    const SLAstNode* n;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (program == NULL || mutableProgram == NULL || fn == NULL || file == NULL || arena == NULL
        || outInsts == NULL || outLen == NULL || exprNode < 0 || (uint32_t)exprNode >= file->ast.len
        || outCap == 0u)
    {
        return 0;
    }
    n = &file->ast.nodes[exprNode];
    switch (n->kind) {
        case SLAst_IDENT: {
            uint32_t localIndex = UINT32_MAX;
            if (!FindMirFunctionLocalBySlice(
                    program, fn, file->source, n->dataStart, n->dataEnd, &localIndex))
            {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_LOCAL_LOAD,
                .tok = 0u,
                ._reserved = 0u,
                .aux = localIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_NULL: {
            uint32_t constIndex = UINT32_MAX;
            if (EnsureMirNullConst(arena, mutableProgram, &constIndex) != 0) {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_PUSH_CONST,
                .tok = 0u,
                ._reserved = 0u,
                .aux = constIndex,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        case SLAst_CAST:
            return LowerMirAllocNewAllocExpr(
                program, mutableProgram, fn, file, n->firstChild, arena, outInsts, outCap, outLen);
        case SLAst_FIELD_EXPR: {
            int32_t  baseNode = n->firstChild;
            uint32_t field = SLMirContextField_INVALID;
            if (baseNode < 0 || (uint32_t)baseNode >= file->ast.len
                || file->ast.nodes[baseNode].kind != SLAst_IDENT
                || !SliceEqCStr(
                    file->source,
                    file->ast.nodes[baseNode].dataStart,
                    file->ast.nodes[baseNode].dataEnd,
                    "context"))
            {
                return 0;
            }
            if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "mem")) {
                field = SLMirContextField_MEM;
            } else if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "temp_mem")) {
                field = SLMirContextField_TEMP_MEM;
            } else {
                return 0;
            }
            outInsts[0] = (SLMirInst){
                .op = SLMirOp_CTX_GET,
                .tok = 0u,
                ._reserved = 0u,
                .aux = field,
                .start = n->start,
                .end = n->end,
            };
            *outLen = 1u;
            return 1;
        }
        default: return 0;
    }
}

static int AppendMirInst(
    SLMirInst* outInsts, uint32_t outCap, uint32_t* _Nonnull ioLen, const SLMirInst* inst) {
    if (outInsts == NULL || ioLen == NULL || inst == NULL || *ioLen >= outCap) {
        return 0;
    }
    outInsts[*ioLen] = *inst;
    (*ioLen)++;
    return 1;
}

static uint32_t MirInitOwnerTypeRefForType(const SLMirProgram* program, uint32_t typeRef) {
    if (program == NULL || typeRef >= program->typeLen) {
        return UINT32_MAX;
    }
    if (SLMirTypeRefIsAggregate(&program->types[typeRef])) {
        return typeRef;
    }
    if (SLMirTypeRefIsOpaquePtr(&program->types[typeRef])) {
        return SLMirTypeRefOpaquePointeeTypeRef(&program->types[typeRef]);
    }
    return UINT32_MAX;
}

static int MirTypeNodeKind(SLAstKind kind) {
    return IsFnReturnTypeNodeKind(kind) || kind == SLAst_TYPE_ANON_STRUCT
        || kind == SLAst_TYPE_ANON_UNION;
}

static int LowerMirHeapInitValueExpr(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    exprFile,
    int32_t                exprNode,
    SLArena*               arena,
    uint32_t               currentLocalIndex,
    uint32_t               currentOwnerTypeRef,
    uint32_t               expectedTypeRef,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    uint32_t*              outTypeRef);

static const SLAstNode* _Nullable ResolveMirAggregateDeclNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirTypeRef*    typeRef,
    const SLParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef);

static int EnsureMirAggregateFieldRef(
    SLArena*      arena,
    SLMirProgram* program,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    uint32_t      sourceRef,
    uint32_t      ownerTypeRef,
    uint32_t      typeRef,
    uint32_t* _Nonnull outFieldRef);

static const SLSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end);

static int ResolveMirAggregateTypeRefForTypeNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLParsedFile*    file,
    int32_t                typeNode,
    uint32_t*              outTypeRef) {
    const SLPackage*    pkg = NULL;
    const SLAstNode*    node;
    const SLSymbolDecl* decl;
    const SLParsedFile* declFile;
    uint32_t            sourceRef;
    uint32_t            declSourceRef;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || file == NULL || outTypeRef == NULL || typeNode < 0
        || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    sourceRef = FindMirSourceRefByText(program, file->source, file->sourceLen);
    if (sourceRef == UINT32_MAX) {
        return 0;
    }
    if (FindMirTypeRefByAstNode(program, sourceRef, typeNode, outTypeRef)
        && *outTypeRef < program->typeLen && SLMirTypeRefIsAggregate(&program->types[*outTypeRef]))
    {
        return 1;
    }
    node = &file->ast.nodes[typeNode];
    if (node->kind != SLAst_TYPE_NAME) {
        return 0;
    }
    if (FindLoaderFileByMirSource(loader, program, sourceRef, &pkg) == NULL || pkg == NULL) {
        return 0;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->nodeId < 0 || (uint32_t)decl->fileIndex >= pkg->fileLen) {
        return 0;
    }
    declFile = &pkg->files[decl->fileIndex];
    declSourceRef = FindMirSourceRefByText(program, declFile->source, declFile->sourceLen);
    if (declSourceRef == UINT32_MAX) {
        return 0;
    }
    if (!FindMirTypeRefByAstNode(program, declSourceRef, decl->nodeId, outTypeRef)
        || *outTypeRef >= program->typeLen
        || !SLMirTypeRefIsAggregate(&program->types[*outTypeRef]))
    {
        return 0;
    }
    return 1;
}

static int LowerMirHeapInitValueBySlice(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    const SLParsedFile*  fnFile,
    const SLParsedFile*  exprFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t             start,
    uint32_t             end,
    uint32_t             currentLocalIndex,
    uint32_t             currentOwnerTypeRef,
    SLMirInst*           outInsts,
    uint32_t             outCap,
    uint32_t*            outLen,
    uint32_t*            outTypeRef) {
    uint32_t localIndex = UINT32_MAX;
    uint32_t fieldIndex = UINT32_MAX;
    if (outLen == NULL || outTypeRef == NULL || program == NULL || fn == NULL || exprFile == NULL
        || nameEnd < nameStart)
    {
        return 0;
    }
    *outTypeRef = UINT32_MAX;
    if (fnFile != NULL && fnFile == exprFile
        && FindMirFunctionLocalBySlice(
            program, fn, fnFile->source, nameStart, nameEnd, &localIndex))
    {
        if (!AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(SLMirInst){
                    .op = SLMirOp_LOCAL_LOAD,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = localIndex,
                    .start = start,
                    .end = end,
                }))
        {
            return 0;
        }
        *outTypeRef = program->locals[fn->localStart + localIndex].typeRef;
        return 1;
    }
    if (currentOwnerTypeRef != UINT32_MAX
        && FindMirFieldByOwnerAndSlice(
            program,
            currentOwnerTypeRef,
            FindMirSourceRefByText(program, exprFile->source, exprFile->sourceLen),
            nameStart,
            nameEnd,
            &fieldIndex))
    {
        if (!AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(SLMirInst){
                    .op = SLMirOp_LOCAL_LOAD,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = currentLocalIndex,
                    .start = start,
                    .end = end,
                })
            || !AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(SLMirInst){
                    .op = SLMirOp_AGG_GET,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = fieldIndex,
                    .start = start,
                    .end = end,
                }))
        {
            return 0;
        }
        *outTypeRef = program->fields[fieldIndex].typeRef;
        return 1;
    }
    return 0;
}

static int LowerMirHeapInitCompoundLiteral(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    exprFile,
    int32_t                exprNode,
    SLArena*               arena,
    uint32_t               currentLocalIndex,
    uint32_t               currentOwnerTypeRef,
    uint32_t               expectedTypeRef,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    uint32_t*              outTypeRef) {
    const SLAstNode* lit;
    int32_t          child;
    int32_t          typeNode = -1;
    uint32_t         ownerTypeRef = expectedTypeRef;
    uint32_t         exprSourceRef;
    if (outLen == NULL || outTypeRef == NULL || program == NULL || exprFile == NULL || exprNode < 0
        || (uint32_t)exprNode >= exprFile->ast.len)
    {
        return 0;
    }
    lit = &exprFile->ast.nodes[exprNode];
    if (lit->kind != SLAst_COMPOUND_LIT) {
        return 0;
    }
    child = lit->firstChild;
    exprSourceRef = FindMirSourceRefByText(program, exprFile->source, exprFile->sourceLen);
    if (child >= 0 && (uint32_t)child < exprFile->ast.len
        && MirTypeNodeKind(exprFile->ast.nodes[child].kind))
    {
        typeNode = child;
        child = exprFile->ast.nodes[child].nextSibling;
        if (!ResolveMirAggregateTypeRefForTypeNode(
                loader, program, exprFile, typeNode, &ownerTypeRef))
        {
            return 0;
        }
    }
    if (ownerTypeRef >= program->typeLen || !SLMirTypeRefIsAggregate(&program->types[ownerTypeRef]))
    {
        return 0;
    }
    if (!AppendMirInst(
            outInsts,
            outCap,
            outLen,
            &(SLMirInst){
                .op = SLMirOp_AGG_ZERO,
                .tok = 0u,
                ._reserved = 0u,
                .aux = ownerTypeRef,
                .start = lit->start,
                .end = lit->end,
            }))
    {
        return 0;
    }
    while (child >= 0) {
        const SLAstNode* field = &exprFile->ast.nodes[child];
        int32_t          valueNode = field->firstChild;
        uint32_t         fieldIndex = UINT32_MAX;
        uint32_t         fieldTypeRef = UINT32_MAX;
        uint32_t         valueTypeRef = UINT32_MAX;
        uint32_t         beforeLen;
        if (field->kind != SLAst_COMPOUND_FIELD || field->dataEnd < field->dataStart
            || field->nextSibling == child)
        {
            return 0;
        }
        if (memchr(exprFile->source + field->dataStart, '.', field->dataEnd - field->dataStart)
                != NULL
            || !FindMirFieldByOwnerAndSlice(
                program,
                ownerTypeRef,
                exprSourceRef,
                field->dataStart,
                field->dataEnd,
                &fieldIndex))
        {
            return 0;
        }
        fieldTypeRef = program->fields[fieldIndex].typeRef;
        beforeLen = *outLen;
        if (valueNode >= 0) {
            if (!LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    valueNode,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    fieldTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &valueTypeRef))
            {
                return 0;
            }
        } else if ((field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
            if (!LowerMirHeapInitValueBySlice(
                    program,
                    fn,
                    fnFile,
                    exprFile,
                    field->dataStart,
                    field->dataEnd,
                    field->start,
                    field->end,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &valueTypeRef))
            {
                return 0;
            }
        } else {
            return 0;
        }
        if (!AppendMirInst(
                outInsts,
                outCap,
                outLen,
                &(SLMirInst){
                    .op = SLMirOp_AGG_SET,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = fieldIndex,
                    .start = field->start,
                    .end = field->end,
                }))
        {
            *outLen = beforeLen;
            return 0;
        }
        child = field->nextSibling;
    }
    *outTypeRef = ownerTypeRef;
    return 1;
}

static int LowerMirHeapInitValueExpr(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    exprFile,
    int32_t                exprNode,
    SLArena*               arena,
    uint32_t               currentLocalIndex,
    uint32_t               currentOwnerTypeRef,
    uint32_t               expectedTypeRef,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    uint32_t*              outTypeRef) {
    const SLAstNode* n;
    if (outTypeRef != NULL) {
        *outTypeRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || mutableProgram == NULL || fn == NULL
        || exprFile == NULL || arena == NULL || outInsts == NULL || outLen == NULL
        || outTypeRef == NULL || exprNode < 0 || (uint32_t)exprNode >= exprFile->ast.len)
    {
        return 0;
    }
    n = &exprFile->ast.nodes[exprNode];
    switch (n->kind) {
        case SLAst_INT: {
            int64_t  value = 0;
            uint32_t constIndex = UINT32_MAX;
            if (!ParseMirIntLiteral(exprFile->source, n->dataStart, n->dataEnd, &value)
                || EnsureMirIntConst(arena, mutableProgram, value, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case SLAst_BOOL: {
            uint32_t constIndex = UINT32_MAX;
            bool     value = SliceEqCStr(exprFile->source, n->dataStart, n->dataEnd, "true");
            if ((!value && !SliceEqCStr(exprFile->source, n->dataStart, n->dataEnd, "false"))
                || EnsureMirBoolConst(arena, mutableProgram, value, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case SLAst_STRING: {
            uint8_t*       bytes = NULL;
            uint32_t       len = 0u;
            SLStringLitErr litErr = { 0 };
            uint32_t       constIndex = UINT32_MAX;
            if (SLDecodeStringLiteralArena(
                    arena, exprFile->source, n->dataStart, n->dataEnd, &bytes, &len, &litErr)
                    != 0
                || EnsureMirStringConst(
                       arena,
                       mutableProgram,
                       (SLStrView){ .ptr = (const char*)bytes, .len = len },
                       &constIndex)
                       != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case SLAst_NULL: {
            uint32_t constIndex = UINT32_MAX;
            if (EnsureMirNullConst(arena, mutableProgram, &constIndex) != 0
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_PUSH_CONST,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = constIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            return 1;
        }
        case SLAst_IDENT:
            return LowerMirHeapInitValueBySlice(
                program,
                fn,
                fnFile,
                exprFile,
                n->dataStart,
                n->dataEnd,
                n->start,
                n->end,
                currentLocalIndex,
                currentOwnerTypeRef,
                outInsts,
                outCap,
                outLen,
                outTypeRef);
        case SLAst_UNARY: {
            if (!LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    n->firstChild,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    outTypeRef)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_UNARY,
                        .tok = (uint16_t)n->op,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            *outTypeRef = UINT32_MAX;
            return 1;
        }
        case SLAst_BINARY: {
            int32_t  rhsNode = ASTNextSibling(&exprFile->ast, n->firstChild);
            uint32_t rhsTypeRef = UINT32_MAX;
            if (rhsNode < 0
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    n->firstChild,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    outTypeRef)
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    rhsNode,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &rhsTypeRef)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_BINARY,
                        .tok = (uint16_t)n->op,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            *outTypeRef = UINT32_MAX;
            return 1;
        }
        case SLAst_FIELD_EXPR: {
            uint32_t baseTypeRef = UINT32_MAX;
            uint32_t ownerTypeRef = UINT32_MAX;
            uint32_t fieldIndex = UINT32_MAX;
            int32_t  baseNode = n->firstChild;
            if (baseNode < 0
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    exprFile,
                    baseNode,
                    arena,
                    currentLocalIndex,
                    currentOwnerTypeRef,
                    expectedTypeRef,
                    outInsts,
                    outCap,
                    outLen,
                    &baseTypeRef))
            {
                return 0;
            }
            ownerTypeRef = MirInitOwnerTypeRefForType(program, baseTypeRef);
            if (ownerTypeRef == UINT32_MAX
                || !FindMirFieldByOwnerAndSlice(
                    program,
                    ownerTypeRef,
                    FindMirSourceRefByText(program, exprFile->source, exprFile->sourceLen),
                    n->dataStart,
                    n->dataEnd,
                    &fieldIndex)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    outLen,
                    &(SLMirInst){
                        .op = SLMirOp_AGG_GET,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = fieldIndex,
                        .start = n->start,
                        .end = n->end,
                    }))
            {
                return 0;
            }
            *outTypeRef = program->fields[fieldIndex].typeRef;
            return 1;
        }
        case SLAst_COMPOUND_LIT:
            return LowerMirHeapInitCompoundLiteral(
                loader,
                program,
                mutableProgram,
                fn,
                fnFile,
                exprFile,
                exprNode,
                arena,
                currentLocalIndex,
                currentOwnerTypeRef,
                expectedTypeRef,
                outInsts,
                outCap,
                outLen,
                outTypeRef);
        default: return 0;
    }
}

static int LowerMirAllocNewPostInitInsts(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    SLMirProgram*          mutableProgram,
    const SLMirFunction*   fn,
    const SLParsedFile*    fnFile,
    const SLParsedFile*    typeFile,
    const SLParsedFile*    newFile,
    const SLMirInst*       allocInst,
    const SLMirInst*       storeInst,
    uint32_t               pointeeTypeRef,
    SLArena*               arena,
    SLMirInst*             outInsts,
    uint32_t               outCap,
    uint32_t*              outLen,
    bool*                  outClearedInitFlag) {
    int32_t          typeNode = -1;
    int32_t          countNode = -1;
    int32_t          initNode = -1;
    int32_t          allocNode = -1;
    const SLAstNode* structNode;
    const SLAstNode* astNode;
    uint32_t         fieldSourceRef;
    uint32_t         typeSourceRef = UINT32_MAX;
    uint32_t         emittedLen = 0u;
    uint32_t         localIndex = storeInst->aux;
    bool             clearedInitFlag = false;
    bool             explicitDirect[256] = { 0 };
    uint32_t         directFieldIndices[256];
    uint32_t         directFieldCount = 0u;
    uint32_t         i;
    if (outLen != NULL) {
        *outLen = 0u;
    }
    if (outClearedInitFlag != NULL) {
        *outClearedInitFlag = false;
    }
    if (loader == NULL || program == NULL || mutableProgram == NULL || fn == NULL || newFile == NULL
        || typeFile == NULL || allocInst == NULL || storeInst == NULL || outInsts == NULL
        || outLen == NULL || outClearedInitFlag == NULL || pointeeTypeRef >= program->typeLen)
    {
        return 0;
    }
    if (!DecodeNewExprNodes(
            newFile, (int32_t)allocInst->aux, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    astNode = &newFile->ast.nodes[allocInst->aux];
    fieldSourceRef = FindMirSourceRefByText(program, newFile->source, newFile->sourceLen);
    if (astNode->kind != SLAst_NEW || fieldSourceRef == UINT32_MAX) {
        return 0;
    }
    if ((astNode->flags & SLAstFlag_NEW_HAS_INIT) != 0u) {
        const SLAstNode* initLit;
        int32_t          child;
        if (initNode < 0 || (uint32_t)initNode >= newFile->ast.len) {
            return 0;
        }
        initLit = &newFile->ast.nodes[initNode];
        if (initLit->kind != SLAst_COMPOUND_LIT) {
            return 0;
        }
        child = initLit->firstChild;
        if (child >= 0 && (uint32_t)child < newFile->ast.len
            && MirTypeNodeKind(newFile->ast.nodes[child].kind))
        {
            child = newFile->ast.nodes[child].nextSibling;
        }
        while (child >= 0) {
            const SLAstNode* field = &newFile->ast.nodes[child];
            int32_t          valueNode = field->firstChild;
            uint32_t         fieldIndex = UINT32_MAX;
            uint32_t         fieldTypeRef = UINT32_MAX;
            uint32_t         valueTypeRef = UINT32_MAX;
            const char*      dot;
            if (field->kind != SLAst_COMPOUND_FIELD || field->dataEnd < field->dataStart) {
                return 0;
            }
            dot = memchr(
                newFile->source + field->dataStart, '.', field->dataEnd - field->dataStart);
            if (SLMirTypeRefIsStrObj(&program->types[pointeeTypeRef])) {
                uint32_t pseudoFieldRef = UINT32_MAX;
                uint32_t expectedTypeRef = UINT32_MAX;
                if (dot != NULL) {
                    return 0;
                }
                if (SliceEqCStr(newFile->source, field->dataStart, field->dataEnd, "len")) {
                    if (EnsureMirScalarTypeRef(
                            arena, mutableProgram, SLMirTypeScalar_I32, &expectedTypeRef)
                        != 0)
                    {
                        return 0;
                    }
                    if (!FindMirPseudoFieldByName(program, "len", &pseudoFieldRef)
                        && EnsureMirAggregateFieldRef(
                               arena,
                               mutableProgram,
                               field->dataStart,
                               field->dataEnd,
                               fieldSourceRef,
                               UINT32_MAX,
                               UINT32_MAX,
                               &pseudoFieldRef)
                               != 0)
                    {
                        return 0;
                    }
                } else {
                    return 0;
                }
                if (!AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(SLMirInst){
                            .op = SLMirOp_LOCAL_LOAD,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = localIndex,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !LowerMirHeapInitValueExpr(
                        loader,
                        program,
                        mutableProgram,
                        fn,
                        fnFile,
                        newFile,
                        valueNode,
                        arena,
                        localIndex,
                        pointeeTypeRef,
                        expectedTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef)
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(SLMirInst){
                            .op = SLMirOp_AGG_SET,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = pseudoFieldRef,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(SLMirInst){
                            .op = SLMirOp_DROP,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = 0u,
                            .start = field->start,
                            .end = field->end,
                        }))
                {
                    return 0;
                }
                child = field->nextSibling;
                clearedInitFlag = true;
                continue;
            }
            if (dot != NULL) {
                uint32_t baseFieldRef = UINT32_MAX;
                uint32_t pseudoFieldRef = UINT32_MAX;
                uint32_t expectedTypeRef = UINT32_MAX;
                if (!FindMirFieldByOwnerAndSlice(
                        program,
                        pointeeTypeRef,
                        fieldSourceRef,
                        field->dataStart,
                        (uint32_t)(dot - newFile->source),
                        &baseFieldRef))
                {
                    return 0;
                }
                if (directFieldCount
                    >= (uint32_t)(sizeof(directFieldIndices) / sizeof(directFieldIndices[0])))
                {
                    return 0;
                }
                directFieldIndices[directFieldCount] = baseFieldRef;
                explicitDirect[directFieldCount] = true;
                directFieldCount++;
                if (program->fields[baseFieldRef].typeRef >= program->typeLen
                    || !SLMirTypeRefIsStrObj(
                        &program->types[program->fields[baseFieldRef].typeRef]))
                {
                    return 0;
                }
                if (SliceEqCStr(
                        newFile->source,
                        (uint32_t)(dot - newFile->source) + 1u,
                        field->dataEnd,
                        "len"))
                {
                    if (EnsureMirScalarTypeRef(
                            arena, mutableProgram, SLMirTypeScalar_I32, &expectedTypeRef)
                        != 0)
                    {
                        return 0;
                    }
                    if (!FindMirPseudoFieldByName(program, "len", &pseudoFieldRef)
                        && EnsureMirAggregateFieldRef(
                               arena,
                               mutableProgram,
                               (uint32_t)(dot - newFile->source) + 1u,
                               field->dataEnd,
                               fieldSourceRef,
                               UINT32_MAX,
                               UINT32_MAX,
                               &pseudoFieldRef)
                               != 0)
                    {
                        return 0;
                    }
                } else {
                    return 0;
                }
                if (!AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(SLMirInst){
                            .op = SLMirOp_LOCAL_LOAD,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = localIndex,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(SLMirInst){
                            .op = SLMirOp_AGG_GET,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = baseFieldRef,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !LowerMirHeapInitValueExpr(
                        loader,
                        program,
                        mutableProgram,
                        fn,
                        fnFile,
                        newFile,
                        valueNode,
                        arena,
                        localIndex,
                        pointeeTypeRef,
                        expectedTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef)
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(SLMirInst){
                            .op = SLMirOp_AGG_SET,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = pseudoFieldRef,
                            .start = field->start,
                            .end = field->end,
                        })
                    || !AppendMirInst(
                        outInsts,
                        outCap,
                        &emittedLen,
                        &(SLMirInst){
                            .op = SLMirOp_DROP,
                            .tok = 0u,
                            ._reserved = 0u,
                            .aux = 0u,
                            .start = field->start,
                            .end = field->end,
                        }))
                {
                    return 0;
                }
                child = field->nextSibling;
                clearedInitFlag = true;
                continue;
            }
            if (!FindMirFieldByOwnerAndSlice(
                    program,
                    pointeeTypeRef,
                    fieldSourceRef,
                    field->dataStart,
                    field->dataEnd,
                    &fieldIndex))
            {
                return 0;
            }
            if (directFieldCount
                >= (uint32_t)(sizeof(directFieldIndices) / sizeof(directFieldIndices[0])))
            {
                return 0;
            }
            directFieldIndices[directFieldCount] = fieldIndex;
            explicitDirect[directFieldCount] = true;
            directFieldCount++;
            fieldTypeRef = program->fields[fieldIndex].typeRef;
            if (!AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(SLMirInst){
                        .op = SLMirOp_LOCAL_LOAD,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = localIndex,
                        .start = field->start,
                        .end = field->end,
                    }))
            {
                return 0;
            }
            if (valueNode >= 0) {
                if (!LowerMirHeapInitValueExpr(
                        loader,
                        program,
                        mutableProgram,
                        fn,
                        fnFile,
                        newFile,
                        valueNode,
                        arena,
                        localIndex,
                        pointeeTypeRef,
                        fieldTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef))
                {
                    return 0;
                }
            } else if ((field->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0u) {
                if (!LowerMirHeapInitValueBySlice(
                        program,
                        fn,
                        fnFile,
                        newFile,
                        field->dataStart,
                        field->dataEnd,
                        field->start,
                        field->end,
                        localIndex,
                        pointeeTypeRef,
                        outInsts,
                        outCap,
                        &emittedLen,
                        &valueTypeRef))
                {
                    return 0;
                }
            } else {
                return 0;
            }
            if (!AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(SLMirInst){
                        .op = SLMirOp_AGG_SET,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = fieldIndex,
                        .start = field->start,
                        .end = field->end,
                    })
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(SLMirInst){
                        .op = SLMirOp_DROP,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = field->start,
                        .end = field->end,
                    }))
            {
                return 0;
            }
            child = field->nextSibling;
        }
        clearedInitFlag = true;
    }
    if (!SLMirTypeRefIsAggregate(&program->types[pointeeTypeRef])) {
        *outLen = emittedLen;
        *outClearedInitFlag = clearedInitFlag;
        return emittedLen != 0u || clearedInitFlag;
    }
    structNode = ResolveMirAggregateDeclNode(
        loader, program, &program->types[pointeeTypeRef], &typeFile, &typeSourceRef);
    if (structNode == NULL || structNode->kind != SLAst_STRUCT) {
        *outLen = emittedLen;
        *outClearedInitFlag = clearedInitFlag;
        return emittedLen != 0u || clearedInitFlag;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].ownerTypeRef == pointeeTypeRef) {
            directFieldIndices[directFieldCount++] = i;
            if (directFieldCount
                >= (uint32_t)(sizeof(directFieldIndices) / sizeof(directFieldIndices[0])))
            {
                break;
            }
        }
    }
    {
        int32_t fieldNode = structNode->firstChild;
        while (fieldNode >= 0) {
            const SLAstNode* fieldDecl = &typeFile->ast.nodes[fieldNode];
            int32_t          typeChild;
            int32_t          defaultExprNode;
            uint32_t         fieldIndex = UINT32_MAX;
            uint32_t         fieldTypeRef = UINT32_MAX;
            uint32_t         valueTypeRef = UINT32_MAX;
            bool             alreadyExplicit = false;
            uint32_t         directIndex;
            if (fieldDecl->kind != SLAst_FIELD) {
                fieldNode = fieldDecl->nextSibling;
                continue;
            }
            typeChild = fieldDecl->firstChild;
            if (typeChild < 0 || (uint32_t)typeChild >= typeFile->ast.len) {
                return 0;
            }
            defaultExprNode = typeFile->ast.nodes[typeChild].nextSibling;
            if (defaultExprNode < 0) {
                fieldNode = fieldDecl->nextSibling;
                continue;
            }
            if (!FindMirFieldByOwnerAndSlice(
                    program,
                    pointeeTypeRef,
                    typeSourceRef,
                    fieldDecl->dataStart,
                    fieldDecl->dataEnd,
                    &fieldIndex))
            {
                return 0;
            }
            for (directIndex = 0; directIndex < directFieldCount; directIndex++) {
                if (directFieldIndices[directIndex] == fieldIndex && explicitDirect[directIndex]) {
                    alreadyExplicit = true;
                    break;
                }
            }
            if (alreadyExplicit) {
                fieldNode = fieldDecl->nextSibling;
                continue;
            }
            fieldTypeRef = program->fields[fieldIndex].typeRef;
            if (!AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(SLMirInst){
                        .op = SLMirOp_LOCAL_LOAD,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = localIndex,
                        .start = fieldDecl->start,
                        .end = fieldDecl->end,
                    })
                || !LowerMirHeapInitValueExpr(
                    loader,
                    program,
                    mutableProgram,
                    fn,
                    fnFile,
                    typeFile,
                    defaultExprNode,
                    arena,
                    localIndex,
                    pointeeTypeRef,
                    fieldTypeRef,
                    outInsts,
                    outCap,
                    &emittedLen,
                    &valueTypeRef)
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(SLMirInst){
                        .op = SLMirOp_AGG_SET,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = fieldIndex,
                        .start = fieldDecl->start,
                        .end = fieldDecl->end,
                    })
                || !AppendMirInst(
                    outInsts,
                    outCap,
                    &emittedLen,
                    &(SLMirInst){
                        .op = SLMirOp_DROP,
                        .tok = 0u,
                        ._reserved = 0u,
                        .aux = 0u,
                        .start = fieldDecl->start,
                        .end = fieldDecl->end,
                    }))
            {
                return 0;
            }
            fieldNode = fieldDecl->nextSibling;
        }
    }
    *outLen = emittedLen;
    *outClearedInitFlag = clearedInitFlag;
    return emittedLen != 0u || clearedInitFlag;
}

static int RewriteMirAllocNewInitExprs(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  fnFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (fnFile == NULL) {
            continue;
        }
        for (pc = 0u; pc + 1u < fn->instLen; pc++) {
            const SLMirInst*    allocInst = &program->insts[fn->instStart + pc];
            const SLMirInst*    storeInst = &program->insts[fn->instStart + pc + 1u];
            const SLMirLocal*   local;
            uint32_t            pointeeTypeRef;
            const SLParsedFile* typeFile;
            SLMirInst*          tempInsts;
            uint32_t            tempLen = 0u;
            bool                clearedInit = false;
            uint32_t            tempCap;
            if (allocInst->op != SLMirOp_ALLOC_NEW || storeInst->op != SLMirOp_LOCAL_STORE
                || storeInst->aux >= fn->localCount)
            {
                continue;
            }
            local = &program->locals[fn->localStart + storeInst->aux];
            pointeeTypeRef = MirInitOwnerTypeRefForType(program, local->typeRef);
            if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                && !ResolveMirAllocNewPointeeTypeRef(
                    loader, arena, program, fnFile, allocInst, &pointeeTypeRef))
            {
                continue;
            }
            if (pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen) {
                continue;
            }
            typeFile = FindLoaderFileByMirSource(
                loader, program, program->types[pointeeTypeRef].sourceRef, NULL);
            if (typeFile == NULL) {
                continue;
            }
            tempCap = fnFile->ast.len * 8u + typeFile->ast.len * 4u + 64u;
            tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
            if (tempInsts == NULL) {
                return -1;
            }
            if (!LowerMirAllocNewPostInitInsts(
                    loader,
                    program,
                    program,
                    fn,
                    fnFile,
                    typeFile,
                    fnFile,
                    allocInst,
                    storeInst,
                    pointeeTypeRef,
                    arena,
                    tempInsts,
                    tempCap,
                    &tempLen,
                    &clearedInit))
            {
                free(tempInsts);
                continue;
            }
            free(tempInsts);
            totalExtraLen += tempLen;
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  fnFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint8_t*  clearInit = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            clearInit = (uint8_t*)calloc(fn->instLen, sizeof(uint8_t));
            if (insertCounts == NULL || pcMap == NULL || clearInit == NULL) {
                free(insertCounts);
                free(pcMap);
                free(clearInit);
                return -1;
            }
        }
        if (fnFile != NULL) {
            for (pc = 0u; pc + 1u < fn->instLen; pc++) {
                const SLMirInst*    allocInst = &program->insts[fn->instStart + pc];
                const SLMirInst*    storeInst = &program->insts[fn->instStart + pc + 1u];
                const SLMirLocal*   local;
                uint32_t            pointeeTypeRef;
                const SLParsedFile* typeFile;
                SLMirInst*          tempInsts;
                uint32_t            tempLen = 0u;
                bool                cleared = false;
                uint32_t            tempCap;
                if (allocInst->op != SLMirOp_ALLOC_NEW || storeInst->op != SLMirOp_LOCAL_STORE
                    || storeInst->aux >= fn->localCount)
                {
                    continue;
                }
                local = &program->locals[fn->localStart + storeInst->aux];
                pointeeTypeRef = MirInitOwnerTypeRefForType(program, local->typeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, fnFile, allocInst, &pointeeTypeRef))
                {
                    continue;
                }
                if (pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen) {
                    continue;
                }
                typeFile = FindLoaderFileByMirSource(
                    loader, program, program->types[pointeeTypeRef].sourceRef, NULL);
                if (typeFile == NULL) {
                    continue;
                }
                tempCap = fnFile->ast.len * 8u + typeFile->ast.len * 4u + 64u;
                tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
                if (tempInsts == NULL) {
                    free(insertCounts);
                    free(pcMap);
                    free(clearInit);
                    return -1;
                }
                if (!LowerMirAllocNewPostInitInsts(
                        loader,
                        program,
                        program,
                        fn,
                        fnFile,
                        typeFile,
                        fnFile,
                        allocInst,
                        storeInst,
                        pointeeTypeRef,
                        arena,
                        tempInsts,
                        tempCap,
                        &tempLen,
                        &cleared))
                {
                    free(tempInsts);
                    continue;
                }
                free(tempInsts);
                insertCounts[pc + 1u] = tempLen;
                clearInit[pc] = cleared ? 1u : 0u;
                extraLen += tempLen;
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
            insts[instOutLen] = *srcInst;
            if (clearInit != NULL && clearInit[pc] && insts[instOutLen].op == SLMirOp_ALLOC_NEW) {
                insts[instOutLen].tok &= (uint16_t)~SLAstFlag_NEW_HAS_INIT;
            }
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                const SLMirInst*    allocInst = &program->insts[fn->instStart + pc - 1u];
                const SLMirInst*    storeInst = &program->insts[fn->instStart + pc];
                const SLMirLocal*   local;
                uint32_t            pointeeTypeRef;
                const SLParsedFile* typeFile;
                uint32_t            emittedLen = 0u;
                SLMirInst*          tempInsts = insts + instOutLen;
                local = &program->locals[fn->localStart + storeInst->aux];
                pointeeTypeRef = MirInitOwnerTypeRefForType(program, local->typeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, fnFile, allocInst, &pointeeTypeRef))
                {
                    free(insertCounts);
                    free(pcMap);
                    free(clearInit);
                    return -1;
                }
                typeFile = FindLoaderFileByMirSource(
                    loader, program, program->types[pointeeTypeRef].sourceRef, NULL);
                if (typeFile == NULL
                    || !LowerMirAllocNewPostInitInsts(
                        loader,
                        program,
                        program,
                        fn,
                        fnFile,
                        typeFile,
                        fnFile,
                        allocInst,
                        storeInst,
                        pointeeTypeRef,
                        arena,
                        tempInsts,
                        insertCounts[pc],
                        &emittedLen,
                        &(bool){ false })
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    free(clearInit);
                    return -1;
                }
                instOutLen += emittedLen;
            }
        }
        free(insertCounts);
        free(pcMap);
        free(clearInit);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static int RewriteMirDynamicSliceAllocCounts(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile != NULL) {
            for (pc = 0u; pc < fn->instLen; pc++) {
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                const SLMirInst* nextInst;
                uint32_t         localTypeRef;
                uint32_t         countInstLen = 0u;
                int32_t          typeNode = -1;
                int32_t          countNode = -1;
                int32_t          initNode = -1;
                int32_t          allocNode = -1;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) == 0u
                    || pc + 1u >= fn->instLen || ownerFile->source == NULL)
                {
                    continue;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                if (localTypeRef >= program->typeLen
                    || (!SLMirTypeRefIsSliceView(&program->types[localTypeRef])
                        && !SLMirTypeRefIsAggSliceView(&program->types[localTypeRef])))
                {
                    continue;
                }
                if (!DecodeNewExprNodes(
                        ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                    || countNode < 0)
                {
                    continue;
                }
                if (!CountMirAllocNewCountExprInsts(ownerFile, countNode, &countInstLen)) {
                    continue;
                }
                totalExtraLen += countInstLen;
            }
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    if (program->instLen + totalExtraLen < program->instLen) {
        return -1;
    }
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            if (insertCounts == NULL || pcMap == NULL) {
                free(insertCounts);
                free(pcMap);
                return -1;
            }
        }
        if (ownerFile != NULL) {
            for (pc = 0u; pc < fn->instLen; pc++) {
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                const SLMirInst* nextInst;
                uint32_t         localTypeRef;
                int32_t          typeNode = -1;
                int32_t          countNode = -1;
                int32_t          initNode = -1;
                int32_t          allocNode = -1;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) == 0u
                    || pc + 1u >= fn->instLen || ownerFile->source == NULL)
                {
                    continue;
                }
                nextInst = &program->insts[fn->instStart + pc + 1u];
                if (nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount) {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                if (localTypeRef >= program->typeLen
                    || (!SLMirTypeRefIsSliceView(&program->types[localTypeRef])
                        && !SLMirTypeRefIsAggSliceView(&program->types[localTypeRef])))
                {
                    continue;
                }
                if (!DecodeNewExprNodes(
                        ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                    || countNode < 0
                    || !CountMirAllocNewCountExprInsts(ownerFile, countNode, &insertCounts[pc]))
                {
                    insertCounts[pc] = 0u;
                    continue;
                }
                extraLen += insertCounts[pc];
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                int32_t  typeNode = -1;
                int32_t  countNode = -1;
                int32_t  initNode = -1;
                int32_t  allocNode = -1;
                uint32_t emittedLen = 0u;
                if (!DecodeNewExprNodes(
                        ownerFile,
                        (int32_t)srcInst->aux,
                        &typeNode,
                        &countNode,
                        &initNode,
                        &allocNode)
                    || countNode < 0
                    || !LowerMirAllocNewCountExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        countNode,
                        arena,
                        &insts[instOutLen],
                        insertCounts[pc],
                        &emittedLen)
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    return -1;
                }
                instOutLen += emittedLen;
            }
            insts[instOutLen] = *srcInst;
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
        }
        free(insertCounts);
        free(pcMap);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static int RewriteMirVarSizeAllocCounts(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile == NULL || ownerFile->source == NULL) {
            continue;
        }
        for (pc = 0u; pc + 1u < fn->instLen; pc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            const SLMirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
            uint32_t         localTypeRef;
            uint32_t         pointeeTypeRef = UINT32_MAX;
            uint32_t         countInstLen = 0u;
            uint32_t         tempCap;
            SLMirInst*       tempInsts = NULL;
            if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) != 0u
                || nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount)
            {
                continue;
            }
            localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
            pointeeTypeRef = MirInitOwnerTypeRefForType(program, localTypeRef);
            if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                && !ResolveMirAllocNewPointeeTypeRef(
                    loader, arena, program, ownerFile, inst, &pointeeTypeRef))
            {
                continue;
            }
            tempCap = ownerFile->ast.len * 8u + 64u;
            tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
            if (tempInsts == NULL) {
                return -1;
            }
            if (!LowerMirVarSizeAllocNewSizeExpr(
                    program,
                    program,
                    fn,
                    ownerFile,
                    inst,
                    pointeeTypeRef,
                    arena,
                    tempInsts,
                    tempCap,
                    &countInstLen))
            {
                free(tempInsts);
                continue;
            }
            free(tempInsts);
            totalExtraLen += countInstLen;
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    if (program->instLen + totalExtraLen < program->instLen) {
        return -1;
    }
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint8_t*  setCount = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            setCount = (uint8_t*)calloc(fn->instLen, sizeof(uint8_t));
            if (insertCounts == NULL || pcMap == NULL || setCount == NULL) {
                free(insertCounts);
                free(pcMap);
                free(setCount);
                return -1;
            }
        }
        if (ownerFile != NULL && ownerFile->source != NULL) {
            for (pc = 0u; pc + 1u < fn->instLen; pc++) {
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                const SLMirInst* nextInst = &program->insts[fn->instStart + pc + 1u];
                uint32_t         localTypeRef;
                uint32_t         pointeeTypeRef = UINT32_MAX;
                uint32_t         tempCap;
                SLMirInst*       tempInsts = NULL;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_COUNT) != 0u
                    || nextInst->op != SLMirOp_LOCAL_STORE || nextInst->aux >= fn->localCount)
                {
                    continue;
                }
                localTypeRef = program->locals[fn->localStart + nextInst->aux].typeRef;
                pointeeTypeRef = MirInitOwnerTypeRefForType(program, localTypeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, ownerFile, inst, &pointeeTypeRef))
                {
                    continue;
                }
                tempCap = ownerFile->ast.len * 8u + 64u;
                tempInsts = tempCap != 0u ? (SLMirInst*)malloc(sizeof(SLMirInst) * tempCap) : NULL;
                if (tempInsts == NULL) {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                if (!LowerMirVarSizeAllocNewSizeExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        inst,
                        pointeeTypeRef,
                        arena,
                        tempInsts,
                        tempCap,
                        &insertCounts[pc]))
                {
                    free(tempInsts);
                    insertCounts[pc] = 0u;
                    continue;
                }
                free(tempInsts);
                setCount[pc] = 1u;
                extraLen += insertCounts[pc];
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                const SLMirInst* nextInst =
                    pc + 1u < fn->instLen ? &program->insts[fn->instStart + pc + 1u] : NULL;
                uint32_t pointeeTypeRef = UINT32_MAX;
                uint32_t emittedLen = 0u;
                if (nextInst == NULL || nextInst->op != SLMirOp_LOCAL_STORE
                    || nextInst->aux >= fn->localCount)
                {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                pointeeTypeRef = MirInitOwnerTypeRefForType(
                    program, program->locals[fn->localStart + nextInst->aux].typeRef);
                if ((pointeeTypeRef == UINT32_MAX || pointeeTypeRef >= program->typeLen)
                    && !ResolveMirAllocNewPointeeTypeRef(
                        loader, arena, program, ownerFile, srcInst, &pointeeTypeRef))
                {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                if (!LowerMirVarSizeAllocNewSizeExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        srcInst,
                        pointeeTypeRef,
                        arena,
                        &insts[instOutLen],
                        insertCounts[pc],
                        &emittedLen)
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    free(setCount);
                    return -1;
                }
                instOutLen += emittedLen;
            }
            insts[instOutLen] = *srcInst;
            if (setCount != NULL && setCount[pc] != 0u) {
                insts[instOutLen].tok |= SLAstFlag_NEW_HAS_COUNT;
            }
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
        }
        free(insertCounts);
        free(pcMap);
        free(setCount);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static int RewriteMirAllocNewAllocExprs(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t pc;
        if (ownerFile == NULL || ownerFile->source == NULL) {
            continue;
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            uint32_t         allocInstLen = 0u;
            int32_t          typeNode = -1;
            int32_t          countNode = -1;
            int32_t          initNode = -1;
            int32_t          allocNode = -1;
            if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_ALLOC) == 0u) {
                continue;
            }
            if (!DecodeNewExprNodes(
                    ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                || allocNode < 0
                || !CountMirAllocNewAllocExprInsts(ownerFile, allocNode, &allocInstLen))
            {
                continue;
            }
            totalExtraLen += allocInstLen;
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    if (program->instLen + totalExtraLen < program->instLen) {
        return -1;
    }
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        uint32_t* insertCounts = NULL;
        uint32_t* pcMap = NULL;
        uint32_t  extraLen = 0u;
        uint32_t  delta = 0u;
        uint32_t  pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            if (insertCounts == NULL || pcMap == NULL) {
                free(insertCounts);
                free(pcMap);
                return -1;
            }
        }
        if (ownerFile != NULL && ownerFile->source != NULL) {
            for (pc = 0u; pc < fn->instLen; pc++) {
                const SLMirInst* inst = &program->insts[fn->instStart + pc];
                int32_t          typeNode = -1;
                int32_t          countNode = -1;
                int32_t          initNode = -1;
                int32_t          allocNode = -1;
                if (inst->op != SLMirOp_ALLOC_NEW || (inst->tok & SLAstFlag_NEW_HAS_ALLOC) == 0u) {
                    continue;
                }
                if (!DecodeNewExprNodes(
                        ownerFile, (int32_t)inst->aux, &typeNode, &countNode, &initNode, &allocNode)
                    || allocNode < 0
                    || !CountMirAllocNewAllocExprInsts(ownerFile, allocNode, &insertCounts[pc]))
                {
                    insertCounts[pc] = 0u;
                    continue;
                }
                extraLen += insertCounts[pc];
            }
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            const SLMirInst* srcInst = &program->insts[fn->instStart + pc];
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                int32_t  typeNode = -1;
                int32_t  countNode = -1;
                int32_t  initNode = -1;
                int32_t  allocNode = -1;
                uint32_t emittedLen = 0u;
                if (!DecodeNewExprNodes(
                        ownerFile,
                        (int32_t)srcInst->aux,
                        &typeNode,
                        &countNode,
                        &initNode,
                        &allocNode)
                    || allocNode < 0
                    || !LowerMirAllocNewAllocExpr(
                        program,
                        program,
                        fn,
                        ownerFile,
                        allocNode,
                        arena,
                        &insts[instOutLen],
                        insertCounts[pc],
                        &emittedLen)
                    || emittedLen != insertCounts[pc])
                {
                    free(insertCounts);
                    free(pcMap);
                    return -1;
                }
                instOutLen += emittedLen;
            }
            insts[instOutLen] = *srcInst;
            if ((srcInst->op == SLMirOp_JUMP || srcInst->op == SLMirOp_JUMP_IF_FALSE)
                && srcInst->aux < fn->instLen)
            {
                insts[instOutLen].aux = pcMap[srcInst->aux];
            }
            instOutLen++;
        }
        free(insertCounts);
        free(pcMap);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static const SLImportSymbolRef* _Nullable FindImportValueSymbolBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL || end <= start) {
        return NULL;
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                   nameLen;
        if (sym->isType || sym->isFunction) {
            continue;
        }
        nameLen = strlen(sym->localName);
        if (nameLen == (size_t)(end - start) && memcmp(sym->localName, src + start, nameLen) == 0) {
            return sym;
        }
    }
    return NULL;
}

static const SLImportSymbolRef* _Nullable FindImportFunctionSymbolBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL || src == NULL || end <= start) {
        return NULL;
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                   nameLen;
        if (!sym->isFunction || sym->useWrapper) {
            continue;
        }
        nameLen = strlen(sym->localName);
        if (nameLen == (size_t)(end - start) && memcmp(sym->localName, src + start, nameLen) == 0) {
            return sym;
        }
    }
    return NULL;
}

static const SLMirResolvedDecl* _Nullable FindResolvedImportValueBySlice(
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    start,
    uint32_t                    end) {
    const SLImportSymbolRef* sym = FindImportValueSymbolBySlice(pkg, src, start, end);
    const SLPackage*         depPkg;
    if (sym == NULL || sym->importIndex >= pkg->importLen) {
        return NULL;
    }
    depPkg = pkg->imports[sym->importIndex].target;
    return depPkg != NULL ? FindMirResolvedValueByCStr(map, depPkg, sym->sourceName) : NULL;
}

static const SLMirResolvedDecl* _Nullable FindResolvedImportFunctionBySlice(
    const SLMirResolvedDeclMap* map,
    const SLPackage*            pkg,
    const char*                 src,
    uint32_t                    start,
    uint32_t                    end) {
    const SLImportSymbolRef* sym = FindImportFunctionSymbolBySlice(pkg, src, start, end);
    const SLPackage*         depPkg;
    if (sym == NULL || sym->importIndex >= pkg->importLen) {
        return NULL;
    }
    depPkg = pkg->imports[sym->importIndex].target;
    return depPkg != NULL
             ? FindMirResolvedDeclByCStr(map, depPkg, sym->sourceName, SLMirDeclKind_FN)
             : NULL;
}

static int MirExprInstStackDelta(const SLMirInst* inst, int32_t* outDelta) {
    uint32_t elemCount = 0;
    if (inst == NULL || outDelta == NULL) {
        return 0;
    }
    switch (inst->op) {
        case SLMirOp_PUSH_CONST:
        case SLMirOp_PUSH_INT:
        case SLMirOp_PUSH_FLOAT:
        case SLMirOp_PUSH_BOOL:
        case SLMirOp_PUSH_STRING:
        case SLMirOp_PUSH_NULL:
        case SLMirOp_LOAD_IDENT:
        case SLMirOp_LOCAL_LOAD:
        case SLMirOp_LOCAL_ADDR:
        case SLMirOp_ADDR_OF:
        case SLMirOp_AGG_ZERO:
        case SLMirOp_ARRAY_ZERO:
        case SLMirOp_CTX_GET:         *outDelta = 1; return 1;
        case SLMirOp_UNARY:
        case SLMirOp_CAST:
        case SLMirOp_COERCE:
        case SLMirOp_SEQ_LEN:
        case SLMirOp_STR_CSTR:
        case SLMirOp_OPTIONAL_WRAP:
        case SLMirOp_OPTIONAL_UNWRAP:
        case SLMirOp_DEREF_LOAD:
        case SLMirOp_AGG_GET:
        case SLMirOp_AGG_ADDR:
        case SLMirOp_ARRAY_GET:
        case SLMirOp_ARRAY_ADDR:
        case SLMirOp_TAGGED_TAG:
        case SLMirOp_TAGGED_PAYLOAD:  *outDelta = 0; return 1;
        case SLMirOp_BINARY:
        case SLMirOp_INDEX:
        case SLMirOp_LOCAL_STORE:
        case SLMirOp_STORE_IDENT:
        case SLMirOp_DROP:
        case SLMirOp_ASSERT:
        case SLMirOp_CTX_SET:
        case SLMirOp_DEREF_STORE:
        case SLMirOp_ARRAY_SET:
        case SLMirOp_AGG_SET:         *outDelta = -1; return 1;
        case SLMirOp_CALL:
        case SLMirOp_CALL_FN:
        case SLMirOp_CALL_HOST:
        case SLMirOp_CALL_INDIRECT:
            *outDelta = 1 - (int32_t)SLMirCallArgCountFromTok(inst->tok);
            return 1;
        case SLMirOp_TUPLE_MAKE:
        case SLMirOp_AGG_MAKE:
            elemCount = (uint32_t)inst->tok;
            *outDelta = 1 - (int32_t)elemCount;
            return 1;
        case SLMirOp_SLICE_MAKE:
            *outDelta = 0 - (((inst->tok & SLAstFlag_INDEX_HAS_START) != 0u) ? 1 : 0)
                      - (((inst->tok & SLAstFlag_INDEX_HAS_END) != 0u) ? 1 : 0);
            return 1;
        case SLMirOp_TAGGED_MAKE:   *outDelta = 0; return 1;
        case SLMirOp_RETURN:
        case SLMirOp_JUMP_IF_FALSE: *outDelta = -1; return 1;
        case SLMirOp_RETURN_VOID:
        case SLMirOp_LOCAL_ZERO:
        case SLMirOp_JUMP:          return ((*outDelta = 0), 1);
        default:                    return 0;
    }
}

static int FindCallArgStartInFunction(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    uint32_t             callIndex,
    uint32_t             argCount,
    uint32_t*            outArgStart) {
    int32_t  need = 0;
    uint32_t i;
    if (outArgStart != NULL) {
        *outArgStart = UINT32_MAX;
    }
    if (program == NULL || fn == NULL || outArgStart == NULL || argCount == 0u
        || callIndex < fn->instStart || callIndex >= fn->instStart + fn->instLen)
    {
        return 0;
    }
    need = (int32_t)argCount;
    i = callIndex;
    while (i > fn->instStart) {
        int32_t delta = 0;
        i--;
        if (!MirExprInstStackDelta(&program->insts[i], &delta)) {
            return 0;
        }
        need -= delta;
        if (need == 0) {
            *outArgStart = i;
            return 1;
        }
    }
    return 0;
}

static int FindFirstCallArgEndInFunction(
    const SLMirProgram*  program,
    const SLMirFunction* fn,
    uint32_t             callIndex,
    uint32_t             argCount,
    uint32_t*            outArgEnd) {
    uint32_t argStart = UINT32_MAX;
    uint32_t i;
    int32_t  depth = 0;
    if (outArgEnd != NULL) {
        *outArgEnd = UINT32_MAX;
    }
    if (program == NULL || fn == NULL || outArgEnd == NULL || argCount == 0u
        || !FindCallArgStartInFunction(program, fn, callIndex, argCount, &argStart)
        || argStart < fn->instStart || argStart >= callIndex)
    {
        return 0;
    }
    for (i = argStart; i < callIndex; i++) {
        int32_t delta = 0;
        if (!MirExprInstStackDelta(&program->insts[i], &delta)) {
            return 0;
        }
        depth += delta;
        if (depth == 1) {
            *outArgEnd = i + 1u;
            return 1;
        }
    }
    return 0;
}

static uint32_t MirInstResultTypeRef(
    const SLMirProgram* program, const SLMirFunction* fn, const SLMirInst* inst) {
    if (program == NULL || fn == NULL || inst == NULL) {
        return UINT32_MAX;
    }
    switch (inst->op) {
        case SLMirOp_LOCAL_LOAD:
            return inst->aux < fn->localCount
                     ? program->locals[fn->localStart + inst->aux].typeRef
                     : UINT32_MAX;
        case SLMirOp_AGG_GET:
        case SLMirOp_AGG_ADDR:
            return inst->aux < program->fieldLen ? program->fields[inst->aux].typeRef : UINT32_MAX;
        default: return UINT32_MAX;
    }
}

static uint32_t MirAggregateOwnerTypeRef(const SLMirProgram* program, uint32_t typeRef) {
    if (program == NULL || typeRef >= program->typeLen) {
        return UINT32_MAX;
    }
    if (SLMirTypeRefIsAggregate(&program->types[typeRef])) {
        return typeRef;
    }
    if (SLMirTypeRefIsOpaquePtr(&program->types[typeRef])) {
        return SLMirTypeRefOpaquePointeeTypeRef(&program->types[typeRef]);
    }
    return UINT32_MAX;
}

static uint32_t FindMirFieldNamed(
    const SLMirProgram* program,
    uint32_t            ownerTypeRef,
    const char*         src,
    uint32_t            start,
    uint32_t            end) {
    uint32_t i;
    uint32_t nameLen;
    if (program == NULL || src == NULL || ownerTypeRef >= program->typeLen || end < start) {
        return UINT32_MAX;
    }
    nameLen = end - start;
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField* field = &program->fields[i];
        if (field->ownerTypeRef != ownerTypeRef || field->sourceRef >= program->sourceLen) {
            continue;
        }
        if (field->nameEnd - field->nameStart == nameLen
            && memcmp(
                   program->sources[field->sourceRef].src.ptr + field->nameStart,
                   src + start,
                   nameLen)
                   == 0)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static uint32_t FindMirFieldNamedAny(
    const SLMirProgram* program, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    uint32_t nameLen;
    if (program == NULL || src == NULL || end < start) {
        return UINT32_MAX;
    }
    nameLen = end - start;
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField* field = &program->fields[i];
        if (field->sourceRef >= program->sourceLen) {
            continue;
        }
        if (field->nameEnd - field->nameStart == nameLen
            && memcmp(
                   program->sources[field->sourceRef].src.ptr + field->nameStart,
                   src + start,
                   nameLen)
                   == 0)
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static int EnsureMirHostRef(
    SLArena*      arena,
    SLMirProgram* program,
    SLMirHostKind kind,
    uint32_t      flags,
    uint32_t      target,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    uint32_t* _Nonnull outIndex) {
    uint32_t      i;
    SLMirHostRef* newHosts;
    if (arena == NULL || program == NULL || outIndex == NULL) {
        return -1;
    }
    for (i = 0; i < program->hostLen; i++) {
        if (program->hosts[i].kind == kind && program->hosts[i].flags == flags
            && program->hosts[i].target == target)
        {
            *outIndex = i;
            return 0;
        }
    }
    newHosts = (SLMirHostRef*)SLArenaAlloc(
        arena, sizeof(SLMirHostRef) * (program->hostLen + 1u), (uint32_t)_Alignof(SLMirHostRef));
    if (newHosts == NULL) {
        return -1;
    }
    if (program->hostLen != 0u) {
        memcpy(newHosts, program->hosts, sizeof(SLMirHostRef) * program->hostLen);
    }
    newHosts[program->hostLen] = (SLMirHostRef){
        .nameStart = nameStart,
        .nameEnd = nameEnd,
        .kind = kind,
        .flags = flags,
        .target = target,
    };
    program->hosts = newHosts;
    *outIndex = program->hostLen++;
    return 0;
}

static int ResolvePackageMirProgram(
    const SLPackageLoader*      loader,
    const SLMirResolvedDeclMap* declMap,
    SLArena*                    arena,
    const SLMirProgram*         program,
    SLMirProgram*               outProgram) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0;
    uint32_t       funcIndex;
    int            allowWasmMinPlatform = 0;
    if (loader == NULL || declMap == NULL || arena == NULL || program == NULL || outProgram == NULL)
    {
        return -1;
    }
    allowWasmMinPlatform =
        loader->platformTarget != NULL
        && StrEq(loader->platformTarget, SL_WASM_MIN_PLATFORM_TARGET);
    *outProgram = *program;
    insts = (SLMirInst*)SLArenaAlloc(
        arena, sizeof(SLMirInst) * program->instLen, (uint32_t)_Alignof(SLMirInst));
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    if ((program->instLen != 0u && insts == NULL) || (program->funcLen != 0u && funcs == NULL)) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        const SLPackage*     ownerPkg = NULL;
        const SLParsedFile*  ownerFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, &ownerPkg);
        uint8_t* omit = NULL;
        uint32_t localIndex;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            omit = (uint8_t*)calloc(fn->instLen, sizeof(uint8_t));
            if (omit == NULL) {
                return -1;
            }
        }
        if (ownerPkg != NULL && ownerFile != NULL) {
            for (localIndex = 0; localIndex < fn->instLen; localIndex++) {
                uint32_t         instIndex = fn->instStart + localIndex;
                const SLMirInst* inst = &program->insts[instIndex];
                if (inst->op == SLMirOp_CALL && inst->aux < program->symbolLen) {
                    const SLMirSymbolRef*    sym = &program->symbols[inst->aux];
                    const SLMirResolvedDecl* target = NULL;
                    if (sym->kind != SLMirSymbol_CALL) {
                        continue;
                    }
                    if ((sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u) {
                        uint32_t argc = SLMirCallArgCountFromTok(inst->tok);
                        uint32_t argStart = UINT32_MAX;
                        if (argc == 0u
                            || !FindCallArgStartInFunction(program, fn, instIndex, argc, &argStart)
                            || argStart < fn->instStart || argStart >= fn->instStart + fn->instLen
                            || program->insts[argStart].op != SLMirOp_LOAD_IDENT)
                        {
                            continue;
                        }
                        {
                            const SLMirInst*   recvInst = &program->insts[argStart];
                            const SLImportRef* imp = FindImportByAliasSlice(
                                ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                            if (allowWasmMinPlatform && imp != NULL && imp->target != NULL
                                && StrEq(imp->target->name, "platform")
                                && (SliceEqCStr(ownerFile->source, inst->start, inst->end, "exit")
                                    || SliceEqCStr(
                                        ownerFile->source, inst->start, inst->end, "console_log")))
                            {
                                omit[argStart - fn->instStart] = 1u;
                                continue;
                            }
                            if (imp == NULL || imp->target == NULL) {
                                continue;
                            }
                            target = FindMirResolvedDeclBySlice(
                                declMap,
                                imp->target,
                                ownerFile->source,
                                inst->start,
                                inst->end,
                                SLMirDeclKind_FN);
                            if (target == NULL) {
                                continue;
                            }
                            omit[argStart - fn->instStart] = 1u;
                        }
                    } else {
                        target = FindMirResolvedDeclBySlice(
                            declMap,
                            ownerPkg,
                            ownerFile->source,
                            inst->start,
                            inst->end,
                            SLMirDeclKind_FN);
                        if (target == NULL) {
                            target = FindResolvedImportFunctionBySlice(
                                declMap, ownerPkg, ownerFile->source, inst->start, inst->end);
                        }
                        if (target == NULL) {
                            continue;
                        }
                    }
                } else if (inst->op == SLMirOp_AGG_GET && localIndex > 0u) {
                    const SLMirInst*         recvInst = &program->insts[instIndex - 1u];
                    const SLImportRef*       imp;
                    const SLMirResolvedDecl* target;
                    if (recvInst->op != SLMirOp_LOAD_IDENT) {
                        continue;
                    }
                    imp = FindImportByAliasSlice(
                        ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                    if (imp == NULL || imp->target == NULL) {
                        continue;
                    }
                    target = FindMirResolvedValueBySlice(
                        declMap, imp->target, ownerFile->source, inst->start, inst->end);
                    if (target == NULL) {
                        continue;
                    }
                    omit[localIndex - 1u] = 1u;
                }
            }
        }
        for (localIndex = 0; localIndex < fn->instLen; localIndex++) {
            uint32_t  instIndex = fn->instStart + localIndex;
            SLMirInst inst = program->insts[instIndex];
            if (omit != NULL && omit[localIndex] != 0u) {
                continue;
            }
            if (ownerPkg != NULL && ownerFile != NULL) {
                if (inst.op == SLMirOp_LOAD_IDENT) {
                    const SLMirResolvedDecl* target = FindMirResolvedValueBySlice(
                        declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                    if (target == NULL) {
                        target = FindResolvedImportValueBySlice(
                            declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                    }
                    if (target != NULL) {
                        inst.op = SLMirOp_CALL_FN;
                        inst.tok = 0;
                        inst.aux = target->functionIndex;
                    } else {
                        target = FindMirResolvedDeclBySlice(
                            declMap,
                            ownerPkg,
                            ownerFile->source,
                            inst.start,
                            inst.end,
                            SLMirDeclKind_FN);
                        if (target == NULL) {
                            target = FindResolvedImportFunctionBySlice(
                                declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                        }
                        if (target != NULL) {
                            uint32_t constIndex = UINT32_MAX;
                            if (EnsureMirFunctionConst(
                                    arena, outProgram, target->functionIndex, &constIndex)
                                != 0)
                            {
                                free(omit);
                                return -1;
                            }
                            inst.op = SLMirOp_PUSH_CONST;
                            inst.tok = 0u;
                            inst.aux = constIndex;
                        }
                    }
                } else if (inst.op == SLMirOp_CALL && inst.aux < program->symbolLen) {
                    const SLMirSymbolRef*    sym = &program->symbols[inst.aux];
                    const SLMirResolvedDecl* target = NULL;
                    if (sym->kind == SLMirSymbol_CALL) {
                        if ((sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u) {
                            uint32_t argc = SLMirCallArgCountFromTok(inst.tok);
                            uint32_t argStart = UINT32_MAX;
                            if (argc != 0u
                                && FindCallArgStartInFunction(
                                    program, fn, instIndex, argc, &argStart)
                                && argStart < instIndex
                                && program->insts[argStart].op == SLMirOp_LOAD_IDENT)
                            {
                                const SLMirInst*   recvInst = &program->insts[argStart];
                                const SLImportRef* imp = FindImportByAliasSlice(
                                    ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                                if (allowWasmMinPlatform && imp != NULL && imp->target != NULL
                                    && StrEq(imp->target->name, "platform"))
                                {
                                    uint32_t hostTarget = SLMirHostTarget_INVALID;
                                    uint32_t hostIndex = UINT32_MAX;
                                    if (SliceEqCStr(
                                            ownerFile->source, inst.start, inst.end, "exit"))
                                    {
                                        hostTarget = SLMirHostTarget_PLATFORM_EXIT;
                                    } else if (
                                        SliceEqCStr(
                                            ownerFile->source, inst.start, inst.end, "console_log"))
                                    {
                                        hostTarget = SLMirHostTarget_PLATFORM_CONSOLE_LOG;
                                    }
                                    if (hostTarget != SLMirHostTarget_INVALID
                                        && EnsureMirHostRef(
                                               arena,
                                               outProgram,
                                               SLMirHost_GENERIC,
                                               0u,
                                               hostTarget,
                                               inst.start,
                                               inst.end,
                                               &hostIndex)
                                               != 0)
                                    {
                                        return -1;
                                    }
                                    if (hostTarget != SLMirHostTarget_INVALID) {
                                        inst.op = SLMirOp_CALL_HOST;
                                        inst.tok =
                                            (uint16_t)((SLMirCallArgCountFromTok(inst.tok) - 1u)
                                                       | (inst.tok & SLMirCallArgFlag_SPREAD_LAST));
                                        inst.aux = hostIndex;
                                        insts[instOutLen++] = inst;
                                        continue;
                                    }
                                }
                                if (imp != NULL && imp->target != NULL) {
                                    target = FindMirResolvedDeclBySlice(
                                        declMap,
                                        imp->target,
                                        ownerFile->source,
                                        inst.start,
                                        inst.end,
                                        SLMirDeclKind_FN);
                                    if (target != NULL) {
                                        inst.op = SLMirOp_CALL_FN;
                                        inst.tok =
                                            (uint16_t)((SLMirCallArgCountFromTok(inst.tok) - 1u)
                                                       | (inst.tok & SLMirCallArgFlag_SPREAD_LAST));
                                        inst.aux = target->functionIndex;
                                    }
                                }
                            }
                        } else {
                            target = FindMirResolvedDeclBySlice(
                                declMap,
                                ownerPkg,
                                ownerFile->source,
                                inst.start,
                                inst.end,
                                SLMirDeclKind_FN);
                            if (target == NULL) {
                                target = FindResolvedImportFunctionBySlice(
                                    declMap, ownerPkg, ownerFile->source, inst.start, inst.end);
                            }
                            if (target != NULL) {
                                inst.op = SLMirOp_CALL_FN;
                                inst.aux = target->functionIndex;
                            }
                        }
                    }
                } else if (inst.op == SLMirOp_AGG_GET && localIndex > 0u) {
                    const SLMirInst*         recvInst = &program->insts[instIndex - 1u];
                    const SLImportRef*       imp;
                    const SLMirResolvedDecl* target;
                    if (recvInst->op == SLMirOp_LOAD_IDENT) {
                        imp = FindImportByAliasSlice(
                            ownerPkg, ownerFile->source, recvInst->start, recvInst->end);
                        if (imp != NULL && imp->target != NULL) {
                            target = FindMirResolvedValueBySlice(
                                declMap, imp->target, ownerFile->source, inst.start, inst.end);
                            if (target != NULL) {
                                inst.op = SLMirOp_CALL_FN;
                                inst.tok = 0;
                                inst.aux = target->functionIndex;
                            }
                        }
                    }
                }
            }
            insts[instOutLen++] = inst;
        }
        funcs[funcIndex].instLen = instOutLen - funcs[funcIndex].instStart;
        free(omit);
    }
    outProgram->insts = insts;
    outProgram->instLen = instOutLen;
    outProgram->funcs = funcs;
    return 0;
}

static uint32_t FindMirFuncRefFieldByName(
    const SLMirProgram* program, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (program == NULL || src == NULL) {
        return UINT32_MAX;
    }
    for (i = 0; i < program->fieldLen; i++) {
        const SLMirField* field = &program->fields[i];
        if (field->typeRef < program->typeLen
            && SLMirTypeRefIsFuncRef(&program->types[field->typeRef])
            && SliceEqSlice(src, field->nameStart, field->nameEnd, src, start, end))
        {
            return i;
        }
    }
    return UINT32_MAX;
}

static int ClassifyMirFuncFieldCall(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirFunction*   fn,
    uint32_t               localIndex,
    uint32_t* _Nullable outInsertAfterPc,
    uint32_t* _Nullable outImplFieldRef) {
    const SLParsedFile*   ownerFile;
    const SLMirInst*      inst;
    const SLMirSymbolRef* sym;
    uint32_t              argc;
    uint32_t              argStartPc = UINT32_MAX;
    uint32_t              recvEndPc = UINT32_MAX;
    uint32_t              fieldRef = UINT32_MAX;
    if (outInsertAfterPc != NULL) {
        *outInsertAfterPc = UINT32_MAX;
    }
    if (outImplFieldRef != NULL) {
        *outImplFieldRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || fn == NULL || localIndex >= fn->instLen) {
        return 0;
    }
    ownerFile = FindLoaderFileByMirSource(loader, program, fn->sourceRef, NULL);
    inst = &program->insts[fn->instStart + localIndex];
    if (ownerFile == NULL || ownerFile->source == NULL || inst->op != SLMirOp_CALL
        || inst->aux >= program->symbolLen)
    {
        return 0;
    }
    sym = &program->symbols[inst->aux];
    argc = SLMirCallArgCountFromTok(inst->tok);
    if (sym->kind != SLMirSymbol_CALL || argc < 2u
        || (sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) == 0u
        || !FindCallArgStartInFunction(program, fn, fn->instStart + localIndex, argc, &argStartPc)
        || !FindCallArgStartInFunction(
            program, fn, fn->instStart + localIndex, argc - 1u, &recvEndPc)
        || argStartPc < fn->instStart || argStartPc >= recvEndPc
        || recvEndPc > fn->instStart + localIndex)
    {
        return 0;
    }
    fieldRef = FindMirFuncRefFieldByName(program, ownerFile->source, inst->start, inst->end);
    if (fieldRef >= program->fieldLen) {
        fieldRef = FindMirFieldNamedAny(program, ownerFile->source, inst->start, inst->end);
    }
    if (fieldRef >= program->fieldLen) {
        return 0;
    }
    if (outInsertAfterPc != NULL) {
        *outInsertAfterPc = recvEndPc - fn->instStart - 1u;
    }
    if (outImplFieldRef != NULL) {
        *outImplFieldRef = fieldRef;
    }
    return 1;
}

static int RewriteMirFuncFieldCalls(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    SLMirInst*     insts = NULL;
    SLMirFunction* funcs = NULL;
    uint32_t       instOutLen = 0u;
    uint32_t       totalExtraLen = 0u;
    uint32_t       funcIndex;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 0u; pc < fn->instLen; pc++) {
            if (ClassifyMirFuncFieldCall(loader, program, fn, pc, NULL, NULL)) {
                totalExtraLen++;
            }
        }
    }
    if (totalExtraLen == 0u) {
        return 0;
    }
    funcs = (SLMirFunction*)SLArenaAlloc(
        arena, sizeof(SLMirFunction) * program->funcLen, (uint32_t)_Alignof(SLMirFunction));
    insts = (SLMirInst*)SLArenaAlloc(
        arena,
        sizeof(SLMirInst) * (program->instLen + totalExtraLen),
        (uint32_t)_Alignof(SLMirInst));
    if ((funcs == NULL && program->funcLen != 0u)
        || (insts == NULL && program->instLen + totalExtraLen != 0u))
    {
        return -1;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t*            insertCounts = NULL;
        uint32_t*            insertFieldRefs = NULL;
        uint32_t*            pcMap = NULL;
        uint32_t             extraLen = 0u;
        uint32_t             delta = 0u;
        uint32_t             pc;
        funcs[funcIndex] = *fn;
        funcs[funcIndex].instStart = instOutLen;
        if (fn->instLen != 0u) {
            insertCounts = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            insertFieldRefs = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            pcMap = (uint32_t*)calloc(fn->instLen, sizeof(uint32_t));
            if (insertCounts == NULL || insertFieldRefs == NULL || pcMap == NULL) {
                free(insertCounts);
                free(insertFieldRefs);
                free(pcMap);
                return -1;
            }
            for (pc = 0u; pc < fn->instLen; pc++) {
                insertFieldRefs[pc] = UINT32_MAX;
            }
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            uint32_t insertAfterPc = UINT32_MAX;
            uint32_t implFieldRef = UINT32_MAX;
            if (!ClassifyMirFuncFieldCall(loader, program, fn, pc, &insertAfterPc, &implFieldRef)) {
                continue;
            }
            insertCounts[insertAfterPc] = 1u;
            insertFieldRefs[insertAfterPc] = implFieldRef;
            extraLen++;
        }
        funcs[funcIndex].instLen = fn->instLen + extraLen;
        for (pc = 0u; pc < fn->instLen; pc++) {
            pcMap[pc] = pc + delta;
            delta += insertCounts[pc];
        }
        for (pc = 0u; pc < fn->instLen; pc++) {
            SLMirInst inst = program->insts[fn->instStart + pc];
            if ((inst.op == SLMirOp_JUMP || inst.op == SLMirOp_JUMP_IF_FALSE)
                && inst.aux < fn->instLen)
            {
                inst.aux = pcMap[inst.aux];
            } else if (inst.op == SLMirOp_CALL && inst.aux < program->symbolLen) {
                const SLMirSymbolRef* sym = &program->symbols[inst.aux];
                if (sym->kind == SLMirSymbol_CALL
                    && (sym->flags & SLMirSymbolFlag_CALL_RECEIVER_ARG0) != 0u
                    && SLMirCallArgCountFromTok(inst.tok) > 0u)
                {
                    inst.op = SLMirOp_CALL_INDIRECT;
                    inst.tok = (uint16_t)((SLMirCallArgCountFromTok(inst.tok) - 1u)
                                          | (inst.tok & SLMirCallArgFlag_SPREAD_LAST));
                    inst.aux = 0u;
                }
            }
            insts[instOutLen++] = inst;
            if (insertCounts != NULL && insertCounts[pc] != 0u) {
                insts[instOutLen++] = (SLMirInst){
                    .op = SLMirOp_AGG_GET,
                    .tok = 0u,
                    ._reserved = 0u,
                    .aux = insertFieldRefs[pc],
                    .start = inst.start,
                    .end = inst.end,
                };
            }
        }
        free(insertCounts);
        free(insertFieldRefs);
        free(pcMap);
    }
    program->insts = insts;
    program->instLen = instOutLen;
    program->funcs = funcs;
    return 0;
}

static void SpecializeMirDirectFunctionFieldStores(SLArena* arena, SLMirProgram* program) {
    uint32_t funcIndex;
    if (arena == NULL || program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 3u; pc < fn->instLen; pc++) {
            const SLMirInst*  storeInst = &program->insts[fn->instStart + pc];
            const SLMirInst*  addrInst = &program->insts[fn->instStart + pc - 1u];
            const SLMirInst*  valueInst = &program->insts[fn->instStart + pc - 3u];
            const SLMirField* fieldRef;
            SLMirField*       field = NULL;
            uint32_t          typeRef = UINT32_MAX;
            if (storeInst->op != SLMirOp_DEREF_STORE || addrInst->op != SLMirOp_AGG_ADDR
                || addrInst->aux >= program->fieldLen || valueInst->op != SLMirOp_PUSH_CONST
                || valueInst->aux >= program->constLen
                || program->consts[valueInst->aux].kind != SLMirConst_FUNCTION)
            {
                continue;
            }
            fieldRef = &program->fields[addrInst->aux];
            if (fieldRef->sourceRef >= program->sourceLen) {
                continue;
            }
            {
                uint32_t fieldIndex = FindMirFuncRefFieldByName(
                    program,
                    program->sources[fieldRef->sourceRef].src.ptr,
                    fieldRef->nameStart,
                    fieldRef->nameEnd);
                if (fieldIndex >= program->fieldLen) {
                    continue;
                }
                field = (SLMirField*)&program->fields[fieldIndex];
            }
            if (field->typeRef >= program->typeLen
                || !SLMirTypeRefIsFuncRef(&program->types[field->typeRef])
                || SLMirTypeRefFuncRefFunctionIndex(&program->types[field->typeRef]) != UINT32_MAX)
            {
                continue;
            }
            if (EnsureMirFunctionRefTypeRef(
                    arena, program, program->consts[valueInst->aux].aux, &typeRef)
                != 0)
            {
                continue;
            }
            field->typeRef = typeRef;
        }
    }
}

static bool MirTypeNodesEquivalent(
    const SLParsedFile* fileA, int32_t nodeA, const SLParsedFile* fileB, int32_t nodeB) {
    const SLAstNode* astNodeA;
    const SLAstNode* astNodeB;
    int32_t          childA;
    int32_t          childB;
    if (fileA == NULL || fileB == NULL || nodeA < 0 || nodeB < 0
        || (uint32_t)nodeA >= fileA->ast.len || (uint32_t)nodeB >= fileB->ast.len)
    {
        return false;
    }
    astNodeA = &fileA->ast.nodes[nodeA];
    astNodeB = &fileB->ast.nodes[nodeB];
    if (astNodeA->kind != astNodeB->kind || astNodeA->flags != astNodeB->flags
        || !SliceEqSlice(
            fileA->source,
            astNodeA->dataStart,
            astNodeA->dataEnd,
            fileB->source,
            astNodeB->dataStart,
            astNodeB->dataEnd))
    {
        return false;
    }
    childA = ASTFirstChild(&fileA->ast, nodeA);
    childB = ASTFirstChild(&fileB->ast, nodeB);
    while (childA >= 0 && childB >= 0) {
        if (!MirTypeNodesEquivalent(fileA, childA, fileB, childB)) {
            return false;
        }
        childA = ASTNextSibling(&fileA->ast, childA);
        childB = ASTNextSibling(&fileB->ast, childB);
    }
    return childA < 0 && childB < 0;
}

static int32_t FindMirFunctionDeclNode(const SLParsedFile* file, const SLMirFunction* fn) {
    int32_t child;
    if (file == NULL || fn == NULL) {
        return -1;
    }
    child = ASTFirstChild(&file->ast, file->ast.root);
    while (child >= 0) {
        const SLAstNode* n = &file->ast.nodes[child];
        if (n->kind == SLAst_FN
            && SliceEqSlice(
                file->source, n->dataStart, n->dataEnd, file->source, fn->nameStart, fn->nameEnd))
        {
            return child;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return -1;
}

static bool MirFunctionTypeMatchesDecl(
    const SLParsedFile* typeFile, int32_t typeNode, const SLParsedFile* fnFile, int32_t fnNode) {
    int32_t typeChild;
    int32_t fnChild;
    int32_t typeReturnNode = -1;
    int32_t fnReturnNode = -1;
    if (typeFile == NULL || fnFile == NULL || typeNode < 0 || fnNode < 0
        || (uint32_t)typeNode >= typeFile->ast.len || (uint32_t)fnNode >= fnFile->ast.len
        || typeFile->ast.nodes[typeNode].kind != SLAst_TYPE_FN
        || fnFile->ast.nodes[fnNode].kind != SLAst_FN)
    {
        return false;
    }
    typeChild = ASTFirstChild(&typeFile->ast, typeNode);
    fnChild = ASTFirstChild(&fnFile->ast, fnNode);
    while (fnChild >= 0 && fnFile->ast.nodes[fnChild].kind == SLAst_PARAM) {
        int32_t fnParamType = ASTFirstChild(&fnFile->ast, fnChild);
        if (typeChild < 0 || fnParamType < 0
            || !MirTypeNodesEquivalent(typeFile, typeChild, fnFile, fnParamType))
        {
            return false;
        }
        typeChild = ASTNextSibling(&typeFile->ast, typeChild);
        fnChild = ASTNextSibling(&fnFile->ast, fnChild);
    }
    if (typeChild >= 0 && IsFnReturnTypeNodeKind(typeFile->ast.nodes[typeChild].kind)
        && typeFile->ast.nodes[typeChild].flags == 1u)
    {
        typeReturnNode = typeChild;
        typeChild = ASTNextSibling(&typeFile->ast, typeChild);
    }
    if (fnChild >= 0 && IsFnReturnTypeNodeKind(fnFile->ast.nodes[fnChild].kind)
        && fnFile->ast.nodes[fnChild].flags == 1u)
    {
        fnReturnNode = fnChild;
    }
    if (typeChild >= 0) {
        return false;
    }
    if (typeReturnNode < 0 || fnReturnNode < 0) {
        return typeReturnNode < 0 && fnReturnNode < 0;
    }
    return MirTypeNodesEquivalent(typeFile, typeReturnNode, fnFile, fnReturnNode);
}

static uint32_t FindMirRepresentativeFunctionForFuncType(
    const SLPackageLoader* loader, const SLMirProgram* program, const SLMirTypeRef* typeRef) {
    const SLParsedFile* typeFile;
    uint32_t            functionIndex;
    if (loader == NULL || program == NULL || typeRef == NULL || !SLMirTypeRefIsFuncRef(typeRef)
        || typeRef->astNode == UINT32_MAX)
    {
        return UINT32_MAX;
    }
    typeFile = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, NULL);
    if (typeFile == NULL) {
        return UINT32_MAX;
    }
    for (functionIndex = 0; functionIndex < program->funcLen; functionIndex++) {
        const SLMirFunction* fn = &program->funcs[functionIndex];
        const SLParsedFile*  fnFile = FindLoaderFileByMirSource(
            loader, program, fn->sourceRef, NULL);
        int32_t fnNode;
        if (fnFile == NULL) {
            continue;
        }
        fnNode = FindMirFunctionDeclNode(fnFile, fn);
        if (fnNode >= 0
            && MirFunctionTypeMatchesDecl(typeFile, (int32_t)typeRef->astNode, fnFile, fnNode))
        {
            return functionIndex;
        }
    }
    return UINT32_MAX;
}

static void EnrichMirFunctionRefRepresentatives(
    const SLPackageLoader* loader, SLMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL) {
        return;
    }
    for (i = 0; i < program->typeLen; i++) {
        SLMirTypeRef* typeRef = (SLMirTypeRef*)&program->types[i];
        uint32_t      functionIndex;
        if (!SLMirTypeRefIsFuncRef(typeRef) || typeRef->aux != 0u || typeRef->astNode == UINT32_MAX)
        {
            continue;
        }
        functionIndex = FindMirRepresentativeFunctionForFuncType(loader, program, typeRef);
        if (functionIndex < program->funcLen) {
            typeRef->aux = functionIndex + 1u;
        }
    }
}

static int PackageUsesPlatformImport(const SLPackageLoader* loader) {
    uint32_t pkgIndex;
    if (loader == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &loader->packages[pkgIndex];
        uint32_t         importIndex;
        for (importIndex = 0; importIndex < pkg->importLen; importIndex++) {
            const char* path = pkg->imports[importIndex].path;
            if (path == NULL) {
                continue;
            }
            if (StrEq(path, "platform") || strncmp(path, "platform/", 9u) == 0) {
                return 1;
            }
        }
    }
    return 0;
}

static SLMirTypeScalar ClassifyMirScalarType(
    const SLParsedFile* file, const SLMirTypeRef* typeRef) {
    const SLAstNode* node;
    const SLAstNode* child;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return SLMirTypeScalar_NONE;
    }
    node = &file->ast.nodes[typeRef->astNode];
    switch (node->kind) {
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_MUTREF: return SLMirTypeScalar_I32;
        case SLAst_TYPE_REF:
            if (node->firstChild >= 0 && (uint32_t)node->firstChild < file->ast.len) {
                child = &file->ast.nodes[node->firstChild];
                if (child->kind == SLAst_TYPE_NAME
                    && SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str"))
                {
                    return SLMirTypeScalar_NONE;
                }
            }
            return SLMirTypeScalar_I32;
        case SLAst_TYPE_NAME:
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "rawptr")) {
                return SLMirTypeScalar_NONE;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "bool")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u8")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u16")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u32")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i8")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i16")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i32")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "uint")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "int"))
            {
                return SLMirTypeScalar_I32;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u64")
                || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i64"))
            {
                return SLMirTypeScalar_I64;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "f32")) {
                return SLMirTypeScalar_F32;
            }
            if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "f64")) {
                return SLMirTypeScalar_F64;
            }
            return SLMirTypeScalar_NONE;
        default: return SLMirTypeScalar_NONE;
    }
}

static SLMirIntKind ClassifyMirIntKindFromTypeNode(
    const SLParsedFile* file, const SLAstNode* node) {
    if (file == NULL || node == NULL) {
        return SLMirIntKind_NONE;
    }
    if (node->kind != SLAst_TYPE_NAME) {
        return SLMirIntKind_NONE;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "bool")) {
        return SLMirIntKind_BOOL;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u8")) {
        return SLMirIntKind_U8;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i8")) {
        return SLMirIntKind_I8;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u16")) {
        return SLMirIntKind_U16;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i16")) {
        return SLMirIntKind_I16;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "u32")) {
        return SLMirIntKind_U32;
    }
    if (SliceEqCStr(file->source, node->dataStart, node->dataEnd, "i32")
        || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "uint")
        || SliceEqCStr(file->source, node->dataStart, node->dataEnd, "int"))
    {
        return SLMirIntKind_I32;
    }
    return SLMirIntKind_NONE;
}

static uint32_t ParseMirArrayLen(const SLParsedFile* file, const SLAstNode* node) {
    uint32_t value = 0;
    uint32_t i;
    if (file == NULL || node == NULL || node->dataEnd <= node->dataStart) {
        return 0;
    }
    for (i = node->dataStart; i < node->dataEnd; i++) {
        char ch = file->source[i];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        value = value * 10u + (uint32_t)(ch - '0');
    }
    return value;
}

static uint32_t FindMirSourceRefByFile(
    const SLMirProgram* program, const SLParsedFile* file, uint32_t defaultSourceRef) {
    uint32_t i;
    if (program == NULL || file == NULL) {
        return defaultSourceRef;
    }
    for (i = 0; i < program->sourceLen; i++) {
        if (program->sources[i].src.ptr == file->source
            && program->sources[i].src.len == file->sourceLen)
        {
            return i;
        }
    }
    return defaultSourceRef;
}

static const SLSymbolDecl* _Nullable FindPackageTypeDeclBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    size_t   nameLen;
    if (pkg == NULL || src == NULL || end < start) {
        return NULL;
    }
    nameLen = (size_t)(end - start);
    for (i = 0; i < pkg->declLen; i++) {
        if (IsTypeDeclKind(pkg->decls[i].kind) && strlen(pkg->decls[i].name) == nameLen
            && memcmp(pkg->decls[i].name, src + start, nameLen) == 0)
        {
            return &pkg->decls[i];
        }
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (IsTypeDeclKind(pkg->pubDecls[i].kind) && strlen(pkg->pubDecls[i].name) == nameLen
            && memcmp(pkg->pubDecls[i].name, src + start, nameLen) == 0)
        {
            return &pkg->pubDecls[i];
        }
    }
    return NULL;
}

static const SLAstNode* _Nullable ResolveMirAggregateDeclNode(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLMirTypeRef*    typeRef,
    const SLParsedFile** _Nullable outFile,
    uint32_t* _Nullable outSourceRef) {
    const SLPackage*    pkg = NULL;
    const SLParsedFile* file;
    const SLAstNode*    node;
    const SLSymbolDecl* decl;
    const SLParsedFile* declFile;
    uint32_t            sourceRef;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outSourceRef != NULL) {
        *outSourceRef = UINT32_MAX;
    }
    if (loader == NULL || program == NULL || typeRef == NULL) {
        return NULL;
    }
    file = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, &pkg);
    if (file == NULL || pkg == NULL || typeRef->astNode >= file->ast.len) {
        return NULL;
    }
    node = &file->ast.nodes[typeRef->astNode];
    if (node->kind == SLAst_TYPE_ANON_STRUCT || node->kind == SLAst_TYPE_ANON_UNION) {
        if (outFile != NULL) {
            *outFile = file;
        }
        if (outSourceRef != NULL) {
            *outSourceRef = typeRef->sourceRef;
        }
        return node;
    }
    if (node->kind != SLAst_TYPE_NAME) {
        return NULL;
    }
    decl = FindPackageTypeDeclBySlice(pkg, file->source, node->dataStart, node->dataEnd);
    if (decl == NULL || decl->nodeId < 0 || (uint32_t)decl->fileIndex >= pkg->fileLen) {
        return NULL;
    }
    declFile = &pkg->files[decl->fileIndex];
    if ((decl->kind != SLAst_STRUCT && decl->kind != SLAst_UNION)
        || (uint32_t)decl->nodeId >= declFile->ast.len)
    {
        return NULL;
    }
    sourceRef = FindMirSourceRefByFile(program, declFile, typeRef->sourceRef);
    if (outFile != NULL) {
        *outFile = declFile;
    }
    if (outSourceRef != NULL) {
        *outSourceRef = sourceRef;
    }
    return &declFile->ast.nodes[decl->nodeId];
}

static uint32_t ClassifyMirTypeFlags(
    const SLPackageLoader* loader,
    const SLMirProgram*    program,
    const SLParsedFile*    file,
    const SLMirTypeRef*    typeRef) {
    uint32_t         flags = ClassifyMirScalarType(file, typeRef);
    const SLAstNode* node;
    const SLAstNode* child;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return flags;
    }
    node = &file->ast.nodes[typeRef->astNode];
    if (node->kind == SLAst_TYPE_OPTIONAL && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        SLMirTypeRef childTypeRef = {
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = typeRef->sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        return SLMirTypeFlag_OPTIONAL | ClassifyMirTypeFlags(loader, program, file, &childTypeRef);
    }
    if ((node->kind == SLAst_TYPE_REF || node->kind == SLAst_TYPE_PTR) && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        SLMirTypeRef childTypeRef = {
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = typeRef->sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        child = &file->ast.nodes[node->firstChild];
        if (child->kind == SLAst_TYPE_NAME) {
            if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str")) {
                flags |=
                    node->kind == SLAst_TYPE_REF ? SLMirTypeFlag_STR_REF : SLMirTypeFlag_STR_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u8")) {
                flags |= SLMirTypeFlag_U8_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i8")) {
                flags |= SLMirTypeFlag_I8_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u16")) {
                flags |= SLMirTypeFlag_U16_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i16")) {
                flags |= SLMirTypeFlag_I16_PTR;
            } else if (SliceEqCStr(file->source, child->dataStart, child->dataEnd, "u32")) {
                flags |= SLMirTypeFlag_U32_PTR;
            } else if (
                SliceEqCStr(file->source, child->dataStart, child->dataEnd, "bool")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "i32")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "uint")
                || SliceEqCStr(file->source, child->dataStart, child->dataEnd, "int"))
            {
                flags |= SLMirTypeFlag_I32_PTR;
            } else if (
                loader != NULL && program != NULL
                && ResolveMirAggregateDeclNode(loader, program, &childTypeRef, NULL, NULL) != NULL)
            {
                flags |= SLMirTypeFlag_OPAQUE_PTR;
            }
        } else if (
            child->kind == SLAst_TYPE_ARRAY
            && ClassifyMirIntKindFromTypeNode(
                   file,
                   child->firstChild >= 0 && (uint32_t)child->firstChild < file->ast.len
                       ? &file->ast.nodes[child->firstChild]
                       : NULL)
                   != SLMirIntKind_NONE
            && ParseMirArrayLen(file, child) != 0u)
        {
            flags |= SLMirTypeFlag_FIXED_ARRAY_VIEW;
        } else if (
            child->kind == SLAst_TYPE_SLICE && child->firstChild >= 0
            && (uint32_t)child->firstChild < file->ast.len
            && (ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                    != SLMirIntKind_NONE
                || (loader != NULL && program != NULL
                    && ResolveMirAggregateDeclNode(
                           loader,
                           program,
                           &(SLMirTypeRef){
                               .astNode = (uint32_t)child->firstChild,
                               .sourceRef = typeRef->sourceRef,
                               .flags = 0u,
                               .aux = 0u,
                           },
                           NULL,
                           NULL)
                           != NULL)))
        {
            flags |= ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                          != SLMirIntKind_NONE
                       ? SLMirTypeFlag_SLICE_VIEW
                       : SLMirTypeFlag_AGG_SLICE_VIEW;
        } else if (
            child->kind == SLAst_TYPE_VARRAY && child->firstChild >= 0
            && (uint32_t)child->firstChild < file->ast.len
            && (ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                    != SLMirIntKind_NONE
                || (loader != NULL && program != NULL
                    && ResolveMirAggregateDeclNode(
                           loader,
                           program,
                           &(SLMirTypeRef){
                               .astNode = (uint32_t)child->firstChild,
                               .sourceRef = typeRef->sourceRef,
                               .flags = 0u,
                               .aux = 0u,
                           },
                           NULL,
                           NULL)
                           != NULL)))
        {
            flags |= ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild])
                          != SLMirIntKind_NONE
                       ? SLMirTypeFlag_VARRAY_VIEW
                       : SLMirTypeFlag_AGG_SLICE_VIEW;
        }
    } else if (
        node->kind == SLAst_TYPE_ARRAY && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len)
    {
        child = &file->ast.nodes[node->firstChild];
        if (ClassifyMirIntKindFromTypeNode(file, child) != SLMirIntKind_NONE
            && ParseMirArrayLen(file, node) != 0u)
        {
            flags |= SLMirTypeFlag_FIXED_ARRAY;
        }
    } else if (node->kind == SLAst_TYPE_FN) {
        flags |= SLMirTypeFlag_FUNC_REF;
    } else if (
        node->kind == SLAst_TYPE_VARRAY && node->firstChild >= 0
        && (uint32_t)node->firstChild < file->ast.len
        && ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[node->firstChild])
               != SLMirIntKind_NONE)
    {
        flags |= SLMirTypeFlag_VARRAY_VIEW;
    } else if (
        node->kind == SLAst_TYPE_NAME
        && SliceEqCStr(file->source, node->dataStart, node->dataEnd, "str"))
    {
        flags |= SLMirTypeFlag_STR_OBJ;
    } else if (
        node->kind == SLAst_TYPE_NAME
        && SliceEqCStr(file->source, node->dataStart, node->dataEnd, "rawptr"))
    {
        flags |= SLMirTypeFlag_OPAQUE_PTR;
    } else if (
        loader != NULL && program != NULL
        && ResolveMirAggregateDeclNode(loader, program, typeRef, NULL, NULL) != NULL)
    {
        flags |= SLMirTypeFlag_AGGREGATE;
    }
    return flags;
}

static uint32_t ClassifyMirTypeAux(const SLParsedFile* file, const SLMirTypeRef* typeRef) {
    const SLAstNode* node;
    const SLAstNode* child;
    SLMirIntKind     intKind;
    uint32_t         arrayCount;
    if (file == NULL || typeRef == NULL || typeRef->astNode >= file->ast.len) {
        return 0u;
    }
    node = &file->ast.nodes[typeRef->astNode];
    switch (node->kind) {
        case SLAst_TYPE_OPTIONAL:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            return ClassifyMirTypeAux(
                file,
                &(SLMirTypeRef){
                    .astNode = (uint32_t)node->firstChild,
                    .sourceRef = typeRef->sourceRef,
                    .flags = 0u,
                    .aux = 0u,
                });
        case SLAst_TYPE_NAME:
            intKind = ClassifyMirIntKindFromTypeNode(file, node);
            return intKind != SLMirIntKind_NONE ? SLMirTypeAuxMakeScalarInt(intKind) : 0u;
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            child = &file->ast.nodes[node->firstChild];
            if (child->kind == SLAst_TYPE_ARRAY && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                arrayCount = ParseMirArrayLen(file, child);
                if (intKind != SLMirIntKind_NONE && arrayCount != 0u) {
                    return SLMirTypeAuxMakeFixedArray(intKind, arrayCount);
                }
                return 0u;
            }
            if (child->kind == SLAst_TYPE_SLICE && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                if (intKind != SLMirIntKind_NONE) {
                    return SLMirTypeAuxMakeScalarInt(intKind);
                }
                return SLMirTypeAuxMakeAggSliceView(UINT32_MAX);
            }
            if (child->kind == SLAst_TYPE_VARRAY && child->firstChild >= 0
                && (uint32_t)child->firstChild < file->ast.len)
            {
                intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[child->firstChild]);
                if (intKind != SLMirIntKind_NONE) {
                    return SLMirTypeAuxMakeScalarInt(intKind);
                }
                return SLMirTypeAuxMakeAggSliceView(UINT32_MAX);
            }
            intKind = ClassifyMirIntKindFromTypeNode(file, child);
            return intKind != SLMirIntKind_NONE ? SLMirTypeAuxMakeScalarInt(intKind) : 0u;
        case SLAst_TYPE_ARRAY:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            child = &file->ast.nodes[node->firstChild];
            intKind = ClassifyMirIntKindFromTypeNode(file, child);
            arrayCount = ParseMirArrayLen(file, node);
            if (intKind == SLMirIntKind_NONE || arrayCount == 0u) {
                return 0u;
            }
            return SLMirTypeAuxMakeFixedArray(intKind, arrayCount);
        case SLAst_TYPE_VARRAY:
            if (node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len) {
                return 0u;
            }
            intKind = ClassifyMirIntKindFromTypeNode(file, &file->ast.nodes[node->firstChild]);
            return intKind != SLMirIntKind_NONE ? SLMirTypeAuxMakeScalarInt(intKind) : 0u;
        default: return 0u;
    }
}

static void EnrichMirTypeFlags(const SLPackageLoader* loader, SLMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL || program->types == NULL) {
        return;
    }
    for (i = 0; i < program->typeLen; i++) {
        SLMirTypeRef*       typeRef = (SLMirTypeRef*)&program->types[i];
        const SLParsedFile* file = FindLoaderFileByMirSource(
            loader, program, typeRef->sourceRef, NULL);
        typeRef->flags =
            (typeRef->flags
             & ~(SLMirTypeFlag_SCALAR_MASK | SLMirTypeFlag_STR_REF | SLMirTypeFlag_STR_PTR
                 | SLMirTypeFlag_STR_OBJ | SLMirTypeFlag_U8_PTR | SLMirTypeFlag_I32_PTR
                 | SLMirTypeFlag_I8_PTR | SLMirTypeFlag_U16_PTR | SLMirTypeFlag_I16_PTR
                 | SLMirTypeFlag_U32_PTR | SLMirTypeFlag_FIXED_ARRAY
                 | SLMirTypeFlag_FIXED_ARRAY_VIEW | SLMirTypeFlag_SLICE_VIEW
                 | SLMirTypeFlag_VARRAY_VIEW | SLMirTypeFlag_AGG_SLICE_VIEW
                 | SLMirTypeFlag_AGGREGATE | SLMirTypeFlag_OPAQUE_PTR | SLMirTypeFlag_OPTIONAL
                 | SLMirTypeFlag_FUNC_REF))
            | ClassifyMirTypeFlags(loader, program, file, typeRef);
        typeRef->aux = ClassifyMirTypeAux(file, typeRef);
    }
}

static int EnsureMirAstTypeRef(
    SLArena*               arena,
    const SLPackageLoader* loader,
    SLMirProgram*          program,
    uint32_t               astNode,
    uint32_t               sourceRef,
    uint32_t* _Nonnull outTypeRef) {
    uint32_t            i;
    SLMirTypeRef*       newTypes;
    const SLParsedFile* file;
    if (arena == NULL || loader == NULL || program == NULL || outTypeRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].astNode == astNode && program->types[i].sourceRef == sourceRef) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] =
        (SLMirTypeRef){ .astNode = astNode, .sourceRef = sourceRef, .flags = 0u, .aux = 0u };
    file = FindLoaderFileByMirSource(loader, program, sourceRef, NULL);
    newTypes[program->typeLen].flags = ClassifyMirTypeFlags(
        loader, program, file, &newTypes[program->typeLen]);
    newTypes[program->typeLen].aux = ClassifyMirTypeAux(file, &newTypes[program->typeLen]);
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirAggregateFieldRef(
    SLArena*      arena,
    SLMirProgram* program,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    uint32_t      sourceRef,
    uint32_t      ownerTypeRef,
    uint32_t      typeRef,
    uint32_t* _Nonnull outFieldRef) {
    uint32_t    i;
    SLMirField* newFields;
    if (arena == NULL || program == NULL || outFieldRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->fieldLen; i++) {
        if (program->fields[i].nameStart == nameStart && program->fields[i].nameEnd == nameEnd
            && program->fields[i].sourceRef == sourceRef
            && program->fields[i].ownerTypeRef == ownerTypeRef
            && program->fields[i].typeRef == typeRef)
        {
            *outFieldRef = i;
            return 0;
        }
    }
    newFields = (SLMirField*)SLArenaAlloc(
        arena, sizeof(SLMirField) * (program->fieldLen + 1u), (uint32_t)_Alignof(SLMirField));
    if (newFields == NULL) {
        return -1;
    }
    if (program->fieldLen != 0u) {
        memcpy(newFields, program->fields, sizeof(SLMirField) * program->fieldLen);
    }
    newFields[program->fieldLen] = (SLMirField){
        .nameStart = nameStart,
        .nameEnd = nameEnd,
        .sourceRef = sourceRef,
        .ownerTypeRef = ownerTypeRef,
        .typeRef = typeRef,
    };
    program->fields = newFields;
    *outFieldRef = program->fieldLen++;
    return 0;
}

static int EnsureMirAggregateFieldsForType(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program, uint32_t ownerTypeRef) {
    const SLParsedFile* file = NULL;
    const SLAstNode*    declNode;
    uint32_t            ownerSourceRef = UINT32_MAX;
    int32_t             childNode;
    if (loader == NULL || arena == NULL || program == NULL || ownerTypeRef >= program->typeLen) {
        return -1;
    }
    declNode = ResolveMirAggregateDeclNode(
        loader, program, &program->types[ownerTypeRef], &file, &ownerSourceRef);
    if (declNode == NULL || file == NULL) {
        return 0;
    }
    childNode = declNode->firstChild;
    while (childNode >= 0 && (uint32_t)childNode < file->ast.len) {
        const SLAstNode* fieldNode = &file->ast.nodes[childNode];
        uint32_t         fieldTypeRef = UINT32_MAX;
        uint32_t         fieldRef = UINT32_MAX;
        int32_t          typeNode = fieldNode->firstChild;
        if (fieldNode->kind != SLAst_FIELD) {
            childNode = fieldNode->nextSibling;
            continue;
        }
        if (typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
            return -1;
        }
        if (EnsureMirAstTypeRef(
                arena, loader, program, (uint32_t)typeNode, ownerSourceRef, &fieldTypeRef)
            != 0)
        {
            return -1;
        }
        if (EnsureMirAggregateFieldRef(
                arena,
                program,
                fieldNode->dataStart,
                fieldNode->dataEnd,
                ownerSourceRef,
                ownerTypeRef,
                fieldTypeRef,
                &fieldRef)
            != 0)
        {
            return -1;
        }
        (void)fieldRef;
        childNode = fieldNode->nextSibling;
    }
    return 0;
}

static int EnrichMirAggregateFields(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    uint32_t i;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (!SLMirTypeRefIsAggregate(&program->types[i])) {
            continue;
        }
        if (EnsureMirAggregateFieldsForType(loader, arena, program, i) != 0) {
            return -1;
        }
    }
    return 0;
}

static int EnrichMirVArrayCountFields(const SLPackageLoader* loader, SLMirProgram* program) {
    uint32_t i;
    if (loader == NULL || program == NULL) {
        return -1;
    }
    for (i = 0; i < program->fieldLen; i++) {
        SLMirTypeRef*       typeRef;
        const SLParsedFile* file;
        const SLAstNode*    typeNode;
        uint32_t            countFieldRef = UINT32_MAX;
        if (program->fields[i].typeRef >= program->typeLen) {
            continue;
        }
        typeRef = (SLMirTypeRef*)&program->types[program->fields[i].typeRef];
        if (!SLMirTypeRefIsVArrayView(typeRef)) {
            continue;
        }
        file = FindLoaderFileByMirSource(loader, program, typeRef->sourceRef, NULL);
        if (file == NULL || typeRef->astNode >= file->ast.len) {
            continue;
        }
        typeNode = &file->ast.nodes[typeRef->astNode];
        if (typeNode->kind != SLAst_TYPE_VARRAY
            || !FindMirFieldByOwnerAndSlice(
                program,
                program->fields[i].ownerTypeRef,
                typeRef->sourceRef,
                typeNode->dataStart,
                typeNode->dataEnd,
                &countFieldRef))
        {
            return -1;
        }
        typeRef->aux = SLMirTypeAuxMakeVArrayView(SLMirTypeRefIntKind(typeRef), countFieldRef);
    }
    return 0;
}

static int EnrichMirAggSliceElemTypes(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    uint32_t i = 0;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    while (i < program->typeLen) {
        SLMirTypeRef*       typeRef = (SLMirTypeRef*)&program->types[i];
        const SLParsedFile* file;
        const SLAstNode*    node;
        const SLAstNode*    child;
        uint32_t            elemTypeRef = UINT32_MAX;
        uint32_t            sourceRef;
        if (!SLMirTypeRefIsAggSliceView(typeRef)) {
            i++;
            continue;
        }
        sourceRef = typeRef->sourceRef;
        file = FindLoaderFileByMirSource(loader, program, sourceRef, NULL);
        if (file == NULL || typeRef->astNode >= file->ast.len) {
            i++;
            continue;
        }
        node = &file->ast.nodes[typeRef->astNode];
        if ((node->kind != SLAst_TYPE_PTR && node->kind != SLAst_TYPE_REF
             && node->kind != SLAst_TYPE_MUTREF)
            || node->firstChild < 0 || (uint32_t)node->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        child = &file->ast.nodes[node->firstChild];
        if ((child->kind != SLAst_TYPE_SLICE && child->kind != SLAst_TYPE_VARRAY)
            || child->firstChild < 0 || (uint32_t)child->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        if (EnsureMirAstTypeRef(
                arena, loader, program, (uint32_t)child->firstChild, sourceRef, &elemTypeRef)
            != 0)
        {
            return -1;
        }
        typeRef = (SLMirTypeRef*)&program->types[i];
        typeRef->aux = SLMirTypeAuxMakeAggSliceView(elemTypeRef);
        if (EnsureMirAggregateFieldsForType(loader, arena, program, elemTypeRef) != 0) {
            return -1;
        }
        i++;
    }
    return 0;
}

static int EnrichMirOpaquePtrPointees(
    const SLPackageLoader* loader, SLArena* arena, SLMirProgram* program) {
    uint32_t i = 0;
    if (loader == NULL || arena == NULL || program == NULL) {
        return -1;
    }
    while (i < program->typeLen) {
        SLMirTypeRef*       typeRef = (SLMirTypeRef*)&program->types[i];
        const SLParsedFile* file;
        const SLAstNode*    node;
        SLMirTypeRef        childTypeRef;
        uint32_t            pointeeTypeRef = UINT32_MAX;
        uint32_t            sourceRef;
        if (!SLMirTypeRefIsOpaquePtr(typeRef)) {
            i++;
            continue;
        }
        sourceRef = typeRef->sourceRef;
        file = FindLoaderFileByMirSource(loader, program, sourceRef, NULL);
        if (file == NULL || typeRef->astNode >= file->ast.len) {
            i++;
            continue;
        }
        node = &file->ast.nodes[typeRef->astNode];
        if (node->kind != SLAst_TYPE_PTR || node->firstChild < 0
            || (uint32_t)node->firstChild >= file->ast.len)
        {
            i++;
            continue;
        }
        childTypeRef = (SLMirTypeRef){
            .astNode = (uint32_t)node->firstChild,
            .sourceRef = sourceRef,
            .flags = 0u,
            .aux = 0u,
        };
        if (ResolveMirAggregateDeclNode(loader, program, &childTypeRef, NULL, NULL) == NULL) {
            i++;
            continue;
        }
        if (EnsureMirAstTypeRef(
                arena, loader, program, (uint32_t)node->firstChild, sourceRef, &pointeeTypeRef)
            != 0)
        {
            return -1;
        }
        typeRef = (SLMirTypeRef*)&program->types[i];
        typeRef->aux = pointeeTypeRef;
        if (EnsureMirAggregateFieldsForType(loader, arena, program, pointeeTypeRef) != 0) {
            return -1;
        }
        i++;
    }
    return 0;
}

static int EnsureMirScalarTypeRef(
    SLArena* arena, SLMirProgram* program, SLMirTypeScalar scalar, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL || scalar == SLMirTypeScalar_NONE) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (SLMirTypeRefScalarKind(&program->types[i]) == scalar) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (SLMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = (uint32_t)scalar,
        .aux = 0,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

typedef enum {
    MirInferredType_NONE = 0,
    MirInferredType_I32,
    MirInferredType_I64,
    MirInferredType_F32,
    MirInferredType_F64,
    MirInferredType_STR_REF,
    MirInferredType_STR_PTR,
    MirInferredType_U8_PTR,
    MirInferredType_I8_PTR,
    MirInferredType_U16_PTR,
    MirInferredType_I16_PTR,
    MirInferredType_U32_PTR,
    MirInferredType_I32_PTR,
    MirInferredType_OPAQUE_PTR,
    MirInferredType_ARRAY_U8,
    MirInferredType_ARRAY_I8,
    MirInferredType_ARRAY_U16,
    MirInferredType_ARRAY_I16,
    MirInferredType_ARRAY_U32,
    MirInferredType_ARRAY_I32,
    MirInferredType_SLICE_U8,
    MirInferredType_SLICE_I8,
    MirInferredType_SLICE_U16,
    MirInferredType_SLICE_I16,
    MirInferredType_SLICE_U32,
    MirInferredType_SLICE_I32,
    MirInferredType_SLICE_AGG,
    MirInferredType_AGG,
    MirInferredType_FUNC_REF,
} MirInferredType;

static MirInferredType MirInferredTypeFromTypeRef(const SLMirTypeRef* typeRef) {
    uint32_t flags;
    if (typeRef == NULL) {
        return MirInferredType_NONE;
    }
    flags = typeRef->flags;
    if ((flags & SLMirTypeFlag_STR_REF) != 0) {
        return MirInferredType_STR_REF;
    }
    if ((flags & SLMirTypeFlag_STR_PTR) != 0) {
        return MirInferredType_STR_PTR;
    }
    if ((flags & SLMirTypeFlag_STR_OBJ) != 0) {
        return MirInferredType_STR_PTR;
    }
    if ((flags & SLMirTypeFlag_U8_PTR) != 0) {
        return MirInferredType_U8_PTR;
    }
    if ((flags & SLMirTypeFlag_I8_PTR) != 0) {
        return MirInferredType_I8_PTR;
    }
    if ((flags & SLMirTypeFlag_U16_PTR) != 0) {
        return MirInferredType_U16_PTR;
    }
    if ((flags & SLMirTypeFlag_I16_PTR) != 0) {
        return MirInferredType_I16_PTR;
    }
    if ((flags & SLMirTypeFlag_U32_PTR) != 0) {
        return MirInferredType_U32_PTR;
    }
    if ((flags & SLMirTypeFlag_I32_PTR) != 0) {
        return MirInferredType_I32_PTR;
    }
    if ((flags & SLMirTypeFlag_OPAQUE_PTR) != 0) {
        return MirInferredType_OPAQUE_PTR;
    }
    if ((flags & SLMirTypeFlag_FUNC_REF) != 0) {
        return MirInferredType_FUNC_REF;
    }
    if ((flags & (SLMirTypeFlag_FIXED_ARRAY | SLMirTypeFlag_FIXED_ARRAY_VIEW)) != 0) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   return MirInferredType_ARRAY_U8;
            case SLMirIntKind_I8:   return MirInferredType_ARRAY_I8;
            case SLMirIntKind_U16:  return MirInferredType_ARRAY_U16;
            case SLMirIntKind_I16:  return MirInferredType_ARRAY_I16;
            case SLMirIntKind_U32:  return MirInferredType_ARRAY_U32;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  return MirInferredType_ARRAY_I32;
            default:                return MirInferredType_NONE;
        }
    }
    if ((flags & (SLMirTypeFlag_SLICE_VIEW | SLMirTypeFlag_VARRAY_VIEW)) != 0) {
        switch (SLMirTypeRefIntKind(typeRef)) {
            case SLMirIntKind_U8:   return MirInferredType_SLICE_U8;
            case SLMirIntKind_I8:   return MirInferredType_SLICE_I8;
            case SLMirIntKind_U16:  return MirInferredType_SLICE_U16;
            case SLMirIntKind_I16:  return MirInferredType_SLICE_I16;
            case SLMirIntKind_U32:  return MirInferredType_SLICE_U32;
            case SLMirIntKind_BOOL:
            case SLMirIntKind_I32:  return MirInferredType_SLICE_I32;
            default:                return MirInferredType_NONE;
        }
    }
    if ((flags & SLMirTypeFlag_AGG_SLICE_VIEW) != 0) {
        return MirInferredType_SLICE_AGG;
    }
    if ((flags & SLMirTypeFlag_AGGREGATE) != 0) {
        return MirInferredType_AGG;
    }
    switch ((SLMirTypeScalar)(flags & SLMirTypeFlag_SCALAR_MASK)) {
        case SLMirTypeScalar_I32: return MirInferredType_I32;
        case SLMirTypeScalar_I64: return MirInferredType_I64;
        case SLMirTypeScalar_F32: return MirInferredType_F32;
        case SLMirTypeScalar_F64: return MirInferredType_F64;
        default:                  return MirInferredType_NONE;
    }
}

static MirInferredType MirProgramTypeKind(const SLMirProgram* program, uint32_t typeRefIndex) {
    if (program == NULL || typeRefIndex == UINT32_MAX || typeRefIndex >= program->typeLen) {
        return MirInferredType_NONE;
    }
    return MirInferredTypeFromTypeRef(&program->types[typeRefIndex]);
}

static MirInferredType MirConstTypeKind(const SLMirConst* value) {
    if (value == NULL) {
        return MirInferredType_NONE;
    }
    switch (value->kind) {
        case SLMirConst_INT:
        case SLMirConst_BOOL:
        case SLMirConst_NULL:     return MirInferredType_I32;
        case SLMirConst_STRING:   return MirInferredType_STR_REF;
        case SLMirConst_FUNCTION: return MirInferredType_FUNC_REF;
        default:                  return MirInferredType_NONE;
    }
}

static MirInferredType MirFunctionResultTypeKind(
    const SLMirProgram* program, uint32_t functionIndex) {
    if (program == NULL || functionIndex >= program->funcLen) {
        return MirInferredType_NONE;
    }
    return MirProgramTypeKind(program, program->funcs[functionIndex].typeRef);
}

static int EnsureMirStrRefTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (SLMirTypeRefIsStrRef(&program->types[i])) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (SLMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = SLMirTypeFlag_STR_REF,
        .aux = 0,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirFlaggedTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t flags, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    if (arena == NULL || program == NULL || outTypeRef == NULL || flags == 0u) {
        return -1;
    }
    for (i = 0; i < program->typeLen; i++) {
        if (program->types[i].flags == flags) {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] =
        (SLMirTypeRef){ .astNode = UINT32_MAX, .sourceRef = 0, .flags = flags, .aux = 0 };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirFunctionRefTypeRef(
    SLArena* arena, SLMirProgram* program, uint32_t functionIndex, uint32_t* _Nonnull outTypeRef) {
    uint32_t      i;
    SLMirTypeRef* newTypes;
    uint32_t      aux;
    if (arena == NULL || program == NULL || outTypeRef == NULL || functionIndex >= program->funcLen)
    {
        return -1;
    }
    aux = functionIndex + 1u;
    for (i = 0; i < program->typeLen; i++) {
        if ((program->types[i].flags & SLMirTypeFlag_FUNC_REF) != 0u
            && program->types[i].aux == aux)
        {
            *outTypeRef = i;
            return 0;
        }
    }
    newTypes = (SLMirTypeRef*)SLArenaAlloc(
        arena, sizeof(SLMirTypeRef) * (program->typeLen + 1u), (uint32_t)_Alignof(SLMirTypeRef));
    if (newTypes == NULL) {
        return -1;
    }
    if (program->typeLen != 0u) {
        memcpy(newTypes, program->types, sizeof(SLMirTypeRef) * program->typeLen);
    }
    newTypes[program->typeLen] = (SLMirTypeRef){
        .astNode = UINT32_MAX,
        .sourceRef = 0,
        .flags = SLMirTypeFlag_FUNC_REF,
        .aux = aux,
    };
    program->types = newTypes;
    *outTypeRef = program->typeLen++;
    return 0;
}

static int EnsureMirInferredTypeRef(
    SLArena* arena, SLMirProgram* program, MirInferredType type, uint32_t* _Nonnull outTypeRef) {
    switch (type) {
        case MirInferredType_I32:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_I32, outTypeRef);
        case MirInferredType_I64:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_I64, outTypeRef);
        case MirInferredType_F32:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_F32, outTypeRef);
        case MirInferredType_F64:
            return EnsureMirScalarTypeRef(arena, program, SLMirTypeScalar_F64, outTypeRef);
        case MirInferredType_STR_REF: return EnsureMirStrRefTypeRef(arena, program, outTypeRef);
        case MirInferredType_STR_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_STR_PTR, outTypeRef);
        case MirInferredType_U8_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_U8_PTR, outTypeRef);
        case MirInferredType_I8_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_I8_PTR, outTypeRef);
        case MirInferredType_U16_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_U16_PTR, outTypeRef);
        case MirInferredType_I16_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_I16_PTR, outTypeRef);
        case MirInferredType_U32_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_U32_PTR, outTypeRef);
        case MirInferredType_I32_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_I32_PTR, outTypeRef);
        case MirInferredType_OPAQUE_PTR:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_OPAQUE_PTR, outTypeRef);
        case MirInferredType_FUNC_REF:
            return EnsureMirFlaggedTypeRef(arena, program, SLMirTypeFlag_FUNC_REF, outTypeRef);
        default: return -1;
    }
}

static bool MirCanStoreInferredType(MirInferredType dstType, MirInferredType srcType) {
    if (dstType == MirInferredType_NONE || dstType == srcType) {
        return true;
    }
    if (dstType == MirInferredType_STR_PTR && srcType == MirInferredType_STR_REF) {
        return true;
    }
    return false;
}

static void RewriteMirAggregateMake(SLMirProgram* program) {
    uint32_t funcIndex;
    if (program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        uint32_t             pc;
        for (pc = 0; pc < fn->instLen; pc++) {
            SLMirInst* inst = (SLMirInst*)&program->insts[fn->instStart + pc];
            uint32_t   typeRef = UINT32_MAX;
            uint32_t   scanPc;
            int32_t    depth = 1;
            if (inst->op != SLMirOp_AGG_MAKE) {
                continue;
            }
            for (scanPc = pc + 1u; scanPc < fn->instLen && depth > 0; scanPc++) {
                const SLMirInst* next = &program->insts[fn->instStart + scanPc];
                int32_t          delta = 0;
                if (depth == 1) {
                    if (next->op == SLMirOp_COERCE && next->aux < program->typeLen
                        && SLMirTypeRefIsAggregate(&program->types[next->aux]))
                    {
                        typeRef = next->aux;
                        break;
                    }
                    if (next->op == SLMirOp_LOCAL_STORE && next->aux < fn->localCount) {
                        uint32_t localTypeRef = program->locals[fn->localStart + next->aux].typeRef;
                        if (localTypeRef < program->typeLen
                            && SLMirTypeRefIsAggregate(&program->types[localTypeRef]))
                        {
                            typeRef = localTypeRef;
                            break;
                        }
                    }
                }
                if (!MirExprInstStackDelta(next, &delta)) {
                    break;
                }
                if (delta < 0 && (uint32_t)(-delta) > (uint32_t)depth) {
                    break;
                }
                depth += delta;
            }
            if (typeRef != UINT32_MAX) {
                inst->op = SLMirOp_AGG_ZERO;
                inst->tok = 0u;
                inst->aux = typeRef;
            }
        }
    }
}

static void InferMirStraightLineLocalTypes(SLArena* arena, SLMirProgram* program) {
    uint32_t funcIndex;
    if (arena == NULL || program == NULL) {
        return;
    }
    for (funcIndex = 0; funcIndex < program->funcLen; funcIndex++) {
        const SLMirFunction* fn = &program->funcs[funcIndex];
        MirInferredType      localTypes[256] = { 0 };
        MirInferredType      stackTypes[512] = { 0 };
        uint32_t             localTypeRefs[256];
        uint32_t             stackTypeRefs[512];
        uint32_t             stackLen = 0;
        uint32_t             localIndex;
        uint32_t             pc;
        int                  supported = 1;
        if (fn->localCount > 256u) {
            continue;
        }
        for (localIndex = 0; localIndex < 256u; localIndex++) {
            localTypeRefs[localIndex] = UINT32_MAX;
        }
        for (localIndex = 0; localIndex < 512u; localIndex++) {
            stackTypeRefs[localIndex] = UINT32_MAX;
        }
        for (localIndex = 0; localIndex < fn->localCount; localIndex++) {
            const SLMirLocal* local = &program->locals[fn->localStart + localIndex];
            localTypes[localIndex] = MirProgramTypeKind(program, local->typeRef);
            localTypeRefs[localIndex] = local->typeRef;
        }
        for (pc = 0; pc < fn->instLen && supported; pc++) {
            const SLMirInst* inst = &program->insts[fn->instStart + pc];
            switch (inst->op) {
                case SLMirOp_PUSH_CONST:
                    if (inst->aux >= program->constLen || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = MirConstTypeKind(&program->consts[inst->aux]);
                    stackTypeRefs[stackLen] = UINT32_MAX;
                    if (program->consts[inst->aux].kind == SLMirConst_FUNCTION
                        && EnsureMirFunctionRefTypeRef(
                               arena,
                               program,
                               program->consts[inst->aux].aux,
                               &stackTypeRefs[stackLen])
                               != 0)
                    {
                        supported = 0;
                        break;
                    }
                    stackLen++;
                    break;
                case SLMirOp_AGG_ZERO:
                    if (inst->aux >= program->typeLen || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = MirInferredType_AGG;
                    stackTypeRefs[stackLen++] = inst->aux;
                    break;
                case SLMirOp_LOCAL_ZERO:
                    if (inst->aux >= fn->localCount) {
                        supported = 0;
                    }
                    break;
                case SLMirOp_CTX_GET:
                    if (stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] =
                        (inst->aux == SLMirContextField_MEM
                         || inst->aux == SLMirContextField_TEMP_MEM)
                            ? MirInferredType_OPAQUE_PTR
                            : MirInferredType_NONE;
                    stackTypeRefs[stackLen++] = UINT32_MAX;
                    break;
                case SLMirOp_LOCAL_LOAD:
                    if (inst->aux >= fn->localCount || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = localTypes[inst->aux];
                    stackTypeRefs[stackLen++] = localTypeRefs[inst->aux];
                    break;
                case SLMirOp_LOCAL_ADDR:
                    if (inst->aux >= fn->localCount || stackLen >= 512u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen] = localTypes[inst->aux];
                    stackTypeRefs[stackLen++] = localTypeRefs[inst->aux];
                    break;
                case SLMirOp_LOCAL_STORE: {
                    SLMirLocal*     local;
                    MirInferredType srcType;
                    uint32_t        typeRef = UINT32_MAX;
                    if (inst->aux >= fn->localCount || stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    srcType = stackTypes[stackLen - 1u];
                    typeRef = stackTypeRefs[stackLen - 1u];
                    stackLen--;
                    if (srcType == MirInferredType_NONE) {
                        break;
                    }
                    if (srcType == MirInferredType_AGG) {
                        if ((localTypes[inst->aux] != MirInferredType_NONE
                             && localTypes[inst->aux] != MirInferredType_AGG)
                            || (localTypeRefs[inst->aux] != UINT32_MAX && typeRef != UINT32_MAX
                                && localTypeRefs[inst->aux] != typeRef))
                        {
                            supported = 0;
                            break;
                        }
                        localTypes[inst->aux] = MirInferredType_AGG;
                        if (localTypeRefs[inst->aux] == UINT32_MAX) {
                            localTypeRefs[inst->aux] = typeRef;
                        }
                        local = (SLMirLocal*)&program->locals[fn->localStart + inst->aux];
                        if (local->typeRef == UINT32_MAX && typeRef != UINT32_MAX) {
                            local->typeRef = typeRef;
                        }
                        break;
                    }
                    if (!MirCanStoreInferredType(localTypes[inst->aux], srcType)) {
                        supported = 0;
                        break;
                    }
                    if (localTypes[inst->aux] == MirInferredType_NONE) {
                        localTypes[inst->aux] = srcType;
                    }
                    local = (SLMirLocal*)&program->locals[fn->localStart + inst->aux];
                    if (srcType == MirInferredType_FUNC_REF && typeRef != UINT32_MAX
                        && local->typeRef < program->typeLen
                        && SLMirTypeRefIsFuncRef(&program->types[local->typeRef])
                        && SLMirTypeRefFuncRefFunctionIndex(&program->types[local->typeRef])
                               == UINT32_MAX)
                    {
                        local->typeRef = typeRef;
                        localTypeRefs[inst->aux] = typeRef;
                    }
                    if (local->typeRef == UINT32_MAX) {
                        if (typeRef != UINT32_MAX) {
                            local->typeRef = typeRef;
                            localTypeRefs[inst->aux] = typeRef;
                        } else if (EnsureMirInferredTypeRef(arena, program, srcType, &typeRef) == 0)
                        {
                            local->typeRef = typeRef;
                            localTypeRefs[inst->aux] = typeRef;
                        }
                    }
                    break;
                }
                case SLMirOp_AGG_SET:
                    if (stackLen < 2u || stackTypes[stackLen - 2u] != MirInferredType_AGG) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    break;
                case SLMirOp_UNARY:
                    if (stackLen == 0u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirInferredType_I32;
                    break;
                case SLMirOp_BINARY:
                    if (stackLen < 2u || stackTypes[stackLen - 1u] != MirInferredType_I32
                        || stackTypes[stackLen - 2u] != MirInferredType_I32)
                    {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    stackTypes[stackLen - 1u] = MirInferredType_I32;
                    break;
                case SLMirOp_CAST:
                case SLMirOp_COERCE:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirProgramTypeKind(program, inst->aux);
                    stackTypeRefs[stackLen - 1u] = inst->aux;
                    break;
                case SLMirOp_SEQ_LEN:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirInferredType_I32;
                    stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    break;
                case SLMirOp_STR_CSTR:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackTypes[stackLen - 1u] = MirInferredType_U8_PTR;
                    stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    break;
                case SLMirOp_CALL_FN: {
                    uint32_t argc = SLMirCallArgCountFromTok(inst->tok);
                    if (inst->aux >= program->funcLen || stackLen < argc) {
                        supported = 0;
                        break;
                    }
                    stackLen -= argc;
                    {
                        MirInferredType resultType = MirFunctionResultTypeKind(program, inst->aux);
                        if (resultType != MirInferredType_NONE) {
                            if (stackLen >= 512u) {
                                supported = 0;
                                break;
                            }
                            stackTypes[stackLen] = resultType;
                            stackTypeRefs[stackLen++] = program->funcs[inst->aux].typeRef;
                        }
                    }
                    break;
                }
                case SLMirOp_ARRAY_ADDR:
                    if (stackLen < 2u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    switch (stackTypes[stackLen - 1u]) {
                        case MirInferredType_ARRAY_U8:
                            stackTypes[stackLen - 1u] = MirInferredType_U8_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I8:
                            stackTypes[stackLen - 1u] = MirInferredType_I8_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U16:
                            stackTypes[stackLen - 1u] = MirInferredType_U16_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I16:
                            stackTypes[stackLen - 1u] = MirInferredType_I16_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U32:
                            stackTypes[stackLen - 1u] = MirInferredType_U32_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I32:
                            stackTypes[stackLen - 1u] = MirInferredType_I32_PTR;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_SLICE_AGG:
                            stackTypes[stackLen - 1u] = MirInferredType_OPAQUE_PTR;
                            stackTypeRefs[stackLen - 1u] =
                                stackTypeRefs[stackLen - 1u] < program->typeLen
                                    ? SLMirTypeRefAggSliceElemTypeRef(
                                          &program->types[stackTypeRefs[stackLen - 1u]])
                                    : UINT32_MAX;
                            if (stackTypeRefs[stackLen - 1u] == UINT32_MAX) {
                                supported = 0;
                            }
                            break;
                        default: supported = 0; break;
                    }
                    break;
                case SLMirOp_DEREF_LOAD:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    switch (stackTypes[stackLen - 1u]) {
                        case MirInferredType_STR_PTR:
                            stackTypes[stackLen - 1u] = MirInferredType_STR_REF;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_U8_PTR:
                        case MirInferredType_I8_PTR:
                        case MirInferredType_U16_PTR:
                        case MirInferredType_I16_PTR:
                        case MirInferredType_U32_PTR:
                        case MirInferredType_I32_PTR:
                            stackTypes[stackLen - 1u] = MirInferredType_I32;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        default: supported = 0; break;
                    }
                    break;
                case SLMirOp_DEREF_STORE:
                    if (stackLen < 2u) {
                        supported = 0;
                        break;
                    }
                    stackLen -= 2u;
                    break;
                case SLMirOp_SLICE_MAKE:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    if ((inst->tok & SLAstFlag_INDEX_HAS_END) != 0u) {
                        if (stackLen == 0u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                            supported = 0;
                            break;
                        }
                        stackLen--;
                    }
                    if ((inst->tok & SLAstFlag_INDEX_HAS_START) != 0u) {
                        if (stackLen == 0u || stackTypes[stackLen - 1u] != MirInferredType_I32) {
                            supported = 0;
                            break;
                        }
                        stackLen--;
                    }
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    switch (stackTypes[stackLen - 1u]) {
                        case MirInferredType_ARRAY_U8:
                        case MirInferredType_SLICE_U8:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_U8;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I8:
                        case MirInferredType_SLICE_I8:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_I8;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U16:
                        case MirInferredType_SLICE_U16:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_U16;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I16:
                        case MirInferredType_SLICE_I16:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_I16;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_U32:
                        case MirInferredType_SLICE_U32:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_U32;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        case MirInferredType_ARRAY_I32:
                        case MirInferredType_SLICE_I32:
                            stackTypes[stackLen - 1u] = MirInferredType_SLICE_I32;
                            stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                            break;
                        default: supported = 0; break;
                    }
                    break;
                case SLMirOp_INDEX:
                    if (stackLen < 2u) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    if (stackTypes[stackLen - 1u] == MirInferredType_SLICE_AGG) {
                        stackTypes[stackLen - 1u] = MirInferredType_OPAQUE_PTR;
                        stackTypeRefs[stackLen - 1u] =
                            stackTypeRefs[stackLen - 1u] < program->typeLen
                                ? SLMirTypeRefAggSliceElemTypeRef(
                                      &program->types[stackTypeRefs[stackLen - 1u]])
                                : UINT32_MAX;
                        if (stackTypeRefs[stackLen - 1u] == UINT32_MAX) {
                            supported = 0;
                        }
                    } else {
                        stackTypes[stackLen - 1u] = MirInferredType_I32;
                        stackTypeRefs[stackLen - 1u] = UINT32_MAX;
                    }
                    break;
                case SLMirOp_DROP:
                case SLMirOp_ASSERT:
                case SLMirOp_RETURN:
                    if (stackLen == 0u) {
                        supported = 0;
                        break;
                    }
                    stackLen--;
                    break;
                case SLMirOp_RETURN_VOID: break;
                default:                  supported = 0; break;
            }
        }
    }
}

static int BuildPackageMirProgram(
    const SLPackageLoader* loader,
    const SLPackage*       entryPkg,
    SLArena*               arena,
    SLMirProgram*          outProgram,
    SLDiag* _Nullable diag) {
    SLMirProgramBuilder  builder;
    SLMirResolvedDeclMap declMap = { 0 };
    uint32_t*            topoOrder = NULL;
    uint32_t             topoOrderLen = 0;
    uint32_t             orderIndex;
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (loader == NULL || entryPkg == NULL || arena == NULL || outProgram == NULL) {
        return -1;
    }
    topoOrder = (uint32_t*)calloc(loader->packageLen, sizeof(uint32_t));
    if (topoOrder == NULL && loader->packageLen != 0u) {
        return -1;
    }
    if (BuildEntryPackageMirOrder(loader, entryPkg, topoOrder, loader->packageLen, &topoOrderLen)
        != 0)
    {
        free(topoOrder);
        return -1;
    }
    SLMirProgramBuilderInit(&builder, arena);
    for (orderIndex = 0; orderIndex < topoOrderLen; orderIndex++) {
        const SLPackage* pkg = &loader->packages[topoOrder[orderIndex]];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (AppendMirDeclsFromFile(&builder, arena, pkg, &pkg->files[fileIndex], &declMap) != 0)
            {
                free(declMap.v);
                free(topoOrder);
                return -1;
            }
        }
    }
    SLMirProgramBuilderFinish(&builder, outProgram);
    EnrichMirTypeFlags(loader, outProgram);
    if (EnrichMirOpaquePtrPointees(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (EnrichMirAggSliceElemTypes(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (EnrichMirAggregateFields(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (EnrichMirVArrayCountFields(loader, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (ResolvePackageMirProgram(loader, &declMap, arena, outProgram, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirFuncFieldCalls(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirVarSizeAllocCounts(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirDynamicSliceAllocCounts(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirAllocNewAllocExprs(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    if (RewriteMirAllocNewInitExprs(loader, arena, outProgram) != 0) {
        free(declMap.v);
        free(topoOrder);
        return -1;
    }
    RewriteMirAggregateMake(outProgram);
    InferMirStraightLineLocalTypes(arena, outProgram);
    SpecializeMirDirectFunctionFieldStores(arena, outProgram);
    EnrichMirFunctionRefRepresentatives(loader, outProgram);
    free(declMap.v);
    free(topoOrder);
    return SLMirValidateProgram(outProgram, diag);
}

static int DumpMIR(const char* entryPath, const char* _Nullable platformTarget) {
    uint8_t         arenaStorage[4096];
    SLArena         arena;
    SLPackageLoader loader = { 0 };
    SLPackage*      entryPkg = NULL;
    SLMirProgram    program = { 0 };
    SLDiag          diag = { 0 };
    SLWriter        writer;
    SLStrView       fallbackSrc = { 0 };

    SLArenaInit(&arena, arenaStorage, sizeof(arenaStorage));
    SLArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);

    if (LoadAndCheckPackage(entryPath, platformTarget, &loader, &entryPkg) != 0) {
        SLArenaDispose(&arena);
        return -1;
    }
    if (BuildPackageMirProgram(&loader, entryPkg, &arena, &program, &diag) != 0) {
        if (diag.code != SLDiag_NONE && entryPkg->fileLen == 1 && entryPkg->files[0].source != NULL)
        {
            (void)PrintSLDiagLineCol(entryPkg->files[0].path, entryPkg->files[0].source, &diag, 0);
        } else if (diag.code != SLDiag_NONE) {
            (void)ErrorSimple("invalid MIR program");
        }
        FreeLoader(&loader);
        SLArenaDispose(&arena);
        return -1;
    }

    if (entryPkg->fileLen == 1 && entryPkg->files[0].source != NULL) {
        fallbackSrc.ptr = entryPkg->files[0].source;
        fallbackSrc.len = entryPkg->files[0].sourceLen;
    }

    writer.ctx = NULL;
    writer.write = StdoutWrite;
    if (SLMirDumpProgram(&program, fallbackSrc, &writer, &diag) != 0) {
        FreeLoader(&loader);
        SLArenaDispose(&arena);
        return -1;
    }

    FreeLoader(&loader);
    SLArenaDispose(&arena);
    return 0;
}

static int ParseSource(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    SLAst*      outAst,
    void**      outArenaMem,
    SLArena* _Nullable outArena);

static int ParseSourceEx(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    SLAst*      outAst,
    void**      outArenaMem,
    SLArena* _Nullable outArena,
    int useLineColDiag) {
    void*    arenaMem;
    uint64_t arenaCap64;
    size_t   arenaCap;
    SLArena  arena;
    SLDiag   diag = { 0 };

    *outArenaMem = NULL;
    if (outArena != NULL) {
        memset(outArena, 0, sizeof(*outArena));
    }
    outAst->nodes = NULL;
    outAst->len = 0;
    outAst->root = -1;

    {
        uint64_t arenaNodeCap = (uint64_t)sourceLen + 128u;
        if (outArena == NULL) {
            arenaNodeCap *= 3u;
        }
        arenaCap64 = arenaNodeCap * (uint64_t)sizeof(SLAstNode) + 65536u;
    }
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "arena too large\n");
        return -1;
    }

    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        fprintf(stderr, "failed to allocate arena\n");
        return -1;
    }

    SLArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (outArena != NULL) {
        SLArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    }
    if (SLParse(&arena, (SLStrView){ source, sourceLen }, NULL, outAst, NULL, &diag) != 0) {
        (void)(useLineColDiag ? PrintSLDiagLineCol(filename, source, &diag, 0)
                              : PrintSLDiag(filename, source, &diag, 0));
        SLArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }
    if (CompactAstInArena(&arena, outAst) != 0) {
        SLDiag oomDiag = { 0 };
        oomDiag.code = SLDiag_ARENA_OOM;
        oomDiag.type = SLDiagTypeOfCode(SLDiag_ARENA_OOM);
        oomDiag.start = 0;
        oomDiag.end = 0;
        oomDiag.argStart = ArenaBytesUsed(&arena);
        oomDiag.argEnd = ArenaBytesCapacity(&arena);
        (void)(useLineColDiag ? PrintSLDiagLineCol(filename, source, &oomDiag, 0)
                              : PrintSLDiag(filename, source, &oomDiag, 0));
        SLArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }

    *outArenaMem = arenaMem;
    if (outArena != NULL) {
        SLArenaBlock* oldInline = &arena.inlineBlock;
        int           currentIsInline = arena.current == oldInline;
        *outArena = arena;
        outArena->first = &outArena->inlineBlock;
        if (currentIsInline || outArena->current == NULL) {
            outArena->current = &outArena->inlineBlock;
        }
    }
    return 0;
}

static int ParseSource(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    SLAst*      outAst,
    void**      outArenaMem,
    SLArena* _Nullable outArena) {
    return ParseSourceEx(filename, source, sourceLen, outAst, outArenaMem, outArena, 0);
}

static void WarnUnknownFeatureImports(const char* filename, const char* source, const SLAst* ast);

typedef struct {
    const char*                filename;
    const char*                source;
    uint32_t                   sourceLen;
    int                        useLineColDiag;
    const SLCombinedSourceMap* remapMap;
    const char*                remapSource;
    int                        suppressUnusedWarnings;
} SLCheckRunSpec;

static int IsUnusedWarningDiag(SLDiagCode code) {
    return code == SLDiag_UNUSED_FUNCTION || code == SLDiag_UNUSED_VARIABLE
        || code == SLDiag_UNUSED_VARIABLE_NEVER_READ || code == SLDiag_UNUSED_PARAMETER
        || code == SLDiag_UNUSED_PARAMETER_NEVER_READ;
}

static int CheckRunHasRemap(const SLCheckRunSpec* spec) {
    return spec != NULL && spec->remapMap != NULL && spec->remapSource != NULL;
}

static int EmitCheckDiag(
    const SLCheckRunSpec* spec,
    const SLDiag*         diag,
    int                   includeHint,
    int                   dropUnmappedUnusedWarnings) {
    const char*       displaySource;
    const char*       displayFilename;
    const SLDiag*     toPrint = diag;
    SLDiag            remappedDiag;
    SLRemapDiagStatus remapStatus = { 0 };
    uint32_t          remappedFileIndex = 0;

    if (spec == NULL || diag == NULL) {
        return -1;
    }
    displaySource = spec->source;
    displayFilename = spec->filename;
    if (CheckRunHasRemap(spec)) {
        RemapCombinedDiag(
            spec->remapMap,
            diag,
            &remappedDiag,
            &remappedFileIndex,
            spec->remapSource,
            &remapStatus);
        (void)remappedFileIndex;
        if (dropUnmappedUnusedWarnings && IsUnusedWarningDiag(diag->code)
            && !remapStatus.startMapped && !remapStatus.endMapped)
        {
            return 0;
        }
        if (remapStatus.startMapped) {
            toPrint = &remappedDiag;
            displaySource = spec->remapSource;
        } else {
            toPrint = diag;
            displaySource = spec->source;
            displayFilename = "<combined>";
        }
    }
    return spec->useLineColDiag
             ? PrintSLDiagLineCol(displayFilename, displaySource, toPrint, includeHint)
             : PrintSLDiag(displayFilename, displaySource, toPrint, includeHint);
}

static void TypecheckDiagSink(void* ctx, const SLDiag* diag) {
    SLCheckRunSpec* spec = (SLCheckRunSpec*)ctx;
    if (spec == NULL || diag == NULL) {
        return;
    }
    if (spec->suppressUnusedWarnings && IsUnusedWarningDiag(diag->code)) {
        return;
    }
    (void)EmitCheckDiag(spec, diag, 0, 1);
}

static int CheckSourceWithSpec(const SLCheckRunSpec* spec) {
    void*              arenaMem;
    uint64_t           arenaCap64;
    size_t             arenaCap;
    SLArena            arena;
    SLAst              ast;
    SLDiag             diag = { 0 };
    uint32_t           beforeTypecheckUsed;
    uint32_t           beforeTypecheckCap;
    uint32_t           afterTypecheckUsed;
    uint32_t           afterTypecheckCap;
    SLTypeCheckOptions checkOptions = {
        .ctx = (void*)spec,
        .onDiag = TypecheckDiagSink,
        .flags = 0,
    };

    if (spec == NULL) {
        return -1;
    }
    ast.nodes = NULL;
    ast.len = 0;
    ast.root = -1;
    ast.features = 0;
    arenaCap64 = (uint64_t)(spec->sourceLen + 128u) * (uint64_t)sizeof(SLAstNode) + 65536u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        fprintf(stderr, "arena too large\n");
        return -1;
    }
    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        fprintf(stderr, "failed to allocate arena\n");
        return -1;
    }
    SLArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    SLArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    if (SLParse(&arena, (SLStrView){ spec->source, spec->sourceLen }, NULL, &ast, NULL, &diag) != 0)
    {
        (void)EmitCheckDiag(spec, &diag, 0, 0);
        SLArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }
    if (CompactAstInArena(&arena, &ast) != 0) {
        SLDiag oomDiag = { 0 };
        oomDiag.code = SLDiag_ARENA_OOM;
        oomDiag.type = SLDiagTypeOfCode(SLDiag_ARENA_OOM);
        oomDiag.start = 0;
        oomDiag.end = 0;
        oomDiag.argStart = ArenaBytesUsed(&arena);
        oomDiag.argEnd = ArenaBytesCapacity(&arena);
        (void)EmitCheckDiag(spec, &oomDiag, 0, 0);
        SLArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }

    WarnUnknownFeatureImports(spec->filename, spec->source, &ast);
    beforeTypecheckUsed = ArenaBytesUsed(&arena);
    beforeTypecheckCap = ArenaBytesCapacity(&arena);

    if (SLTypeCheckEx(
            &arena, &ast, (SLStrView){ spec->source, spec->sourceLen }, &checkOptions, &diag)
        != 0)
    {
        if (diag.code == SLDiag_ARENA_OOM) {
            uint32_t afterUsed = ArenaBytesUsed(&arena);
            uint32_t afterCap = ArenaBytesCapacity(&arena);
            diag.argStart = afterUsed;
            diag.argEnd = afterCap;
            fprintf(
                stderr,
                "  note: typecheck arena delta %u bytes (before: %u/%u, after: %u/%u)\n",
                afterUsed >= beforeTypecheckUsed ? afterUsed - beforeTypecheckUsed : 0u,
                beforeTypecheckUsed,
                beforeTypecheckCap,
                afterUsed,
                afterCap);
        }
        int diagStatus = EmitCheckDiag(spec, &diag, 1, 0);
        SLArenaDispose(&arena);
        free(arenaMem);
        return diagStatus;
    }

    afterTypecheckUsed = ArenaBytesUsed(&arena);
    afterTypecheckCap = ArenaBytesCapacity(&arena);
    if (ArenaDebugEnabled()) {
        fprintf(
            stderr,
            "arena debug: ast=%u nodes (%u bytes), before check=%u/%u, after check=%u/%u\n",
            ast.len,
            ast.len <= UINT32_MAX / (uint32_t)sizeof(SLAstNode)
                ? ast.len * (uint32_t)sizeof(SLAstNode)
                : UINT32_MAX,
            beforeTypecheckUsed,
            beforeTypecheckCap,
            afterTypecheckUsed,
            afterTypecheckCap);
    }

    SLArenaDispose(&arena);
    free(arenaMem);
    return 0;
}

static int CheckSourceEx(
    const char* filename,
    const char* _Nullable source,
    uint32_t sourceLen,
    int      useLineColDiag,
    int      suppressUnusedWarnings) {
    if (filename == NULL || source == NULL) {
        return -1;
    }
    SLCheckRunSpec spec = {
        .filename = filename,
        .source = source,
        .sourceLen = sourceLen,
        .useLineColDiag = useLineColDiag,
        .remapMap = NULL,
        .remapSource = NULL,
        .suppressUnusedWarnings = suppressUnusedWarnings,
    };
    return CheckSourceWithSpec(&spec);
}

static int CheckSourceExWithSingleFileRemap(
    const char* filename,
    const char* _Nullable source,
    uint32_t sourceLen,
    int      useLineColDiag,
    const char* _Nullable remapSource,
    const SLCombinedSourceMap* remapMap,
    int                        suppressUnusedWarnings) {
    if (filename == NULL || source == NULL) {
        return -1;
    }
    SLCheckRunSpec spec = {
        .filename = filename,
        .source = source,
        .sourceLen = sourceLen,
        .useLineColDiag = useLineColDiag,
        .remapMap = remapMap,
        .remapSource = remapSource,
        .suppressUnusedWarnings = suppressUnusedWarnings,
    };
    return CheckSourceWithSpec(&spec);
}

static int CheckSource(const char* filename, const char* source, uint32_t sourceLen) {
    return CheckSourceEx(filename, source, sourceLen, 0, 0);
}

static int IsDeclKind(SLAstKind kind) {
    return kind == SLAst_FN || kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS || kind == SLAst_VAR || kind == SLAst_CONST;
}

static int IsTypeDeclKind(SLAstKind kind) {
    return kind == SLAst_STRUCT || kind == SLAst_UNION || kind == SLAst_ENUM
        || kind == SLAst_TYPE_ALIAS;
}

static int IsPubDeclNode(const SLAstNode* n) {
    return (n->flags & SLAstFlag_PUB) != 0;
}

static int FnNodeHasBody(const SLAst* ast, int32_t nodeId) {
    int32_t child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAst_BLOCK) {
            return 1;
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

static int FnNodeHasAnytypeParam(const SLParsedFile* file, int32_t nodeId) {
    int32_t child = ASTFirstChild(&file->ast, nodeId);
    while (child >= 0) {
        const SLAstNode* n = &file->ast.nodes[child];
        if (n->kind == SLAst_PARAM) {
            int32_t          typeNode = ASTFirstChild(&file->ast, child);
            const SLAstNode* t =
                (typeNode >= 0 && (uint32_t)typeNode < file->ast.len)
                    ? &file->ast.nodes[typeNode]
                    : NULL;
            if (t != NULL && t->kind == SLAst_TYPE_NAME && t->dataEnd > t->dataStart
                && (size_t)(t->dataEnd - t->dataStart) == strlen("anytype")
                && memcmp(file->source + t->dataStart, "anytype", strlen("anytype")) == 0)
            {
                return 1;
            }
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return 0;
}

static char* _Nullable DefaultImportAlias(const char* importPath) {
    const char* slash = strrchr(importPath, '/');
    const char* name = importPath;
    if (slash != NULL && slash[1] != '\0') {
        name = slash + 1;
    }
    if (!IsValidIdentifier(name)) {
        return NULL;
    }
    return DupCStr(name);
}

static const char* LastPathSegment(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return path;
    }
    return slash + 1;
}

static int IsFeatureImportPath(const char* importPath) {
    return strncmp(importPath, "slang/feature/", 14u) == 0
        || strncmp(importPath, "feature/", 8u) == 0;
}

static int IsImportAliasUsed(const SLPackage* pkg, const char* alias) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        if (StrEq(pkg->imports[i].alias, alias)) {
            return 1;
        }
    }
    return 0;
}

static char* _Nullable MakeUniqueImportAlias(const SLPackage* pkg, const char* preferred) {
    char*    alias;
    uint32_t n;
    if (preferred != NULL && preferred[0] != '\0' && IsValidIdentifier(preferred)
        && !IsImportAliasUsed(pkg, preferred))
    {
        return DupCStr(preferred);
    }
    if (preferred == NULL || preferred[0] == '\0' || !IsValidIdentifier(preferred)) {
        preferred = "imp";
    }
    alias = DupCStr(preferred);
    if (alias == NULL) {
        return NULL;
    }
    if (!IsImportAliasUsed(pkg, alias)) {
        return alias;
    }
    free(alias);
    for (n = 2; n < 1000000u; n++) {
        int   m = snprintf(NULL, 0, "%s_%u", preferred, n);
        char* cand;
        if (m <= 0) {
            return NULL;
        }
        cand = (char*)malloc((size_t)m + 1u);
        if (cand == NULL) {
            return NULL;
        }
        snprintf(cand, (size_t)m + 1u, "%s_%u", preferred, n);
        if (!IsImportAliasUsed(pkg, cand)) {
            return cand;
        }
        free(cand);
    }
    return NULL;
}

static int StringLiteralHasDirectOffsetMapping(const char* source, const SLAstNode* n) {
    uint32_t      i;
    unsigned char delim;
    if (n->dataEnd <= n->dataStart + 1u) {
        return 0;
    }
    delim = (unsigned char)source[n->dataStart];
    if ((delim != (unsigned char)'"' && delim != (unsigned char)'`')
        || (unsigned char)source[n->dataEnd - 1u] != delim)
    {
        return 0;
    }
    for (i = n->dataStart + 1u; i < n->dataEnd - 1u; i++) {
        unsigned char c = (unsigned char)source[i];
        if (c == (unsigned char)'\r') {
            return 0;
        }
        if (delim == (unsigned char)'"' && c == (unsigned char)'\\') {
            return 0;
        }
        if (delim == (unsigned char)'`' && c == (unsigned char)'\\' && i + 1u < n->dataEnd - 1u
            && (unsigned char)source[i + 1u] == (unsigned char)'`')
        {
            return 0;
        }
    }
    return 1;
}

static void WarnUnknownFeatureImports(const char* filename, const char* source, const SLAst* ast) {
    int32_t child = ASTFirstChild(ast, ast->root);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (n->kind == SLAst_IMPORT) {
            uint8_t* decoded = NULL;
            uint32_t decodedLen = 0;
            if (SLDecodeStringLiteralMalloc(
                    source, n->dataStart, n->dataEnd, &decoded, &decodedLen, NULL)
                != 0)
            {
                child = ASTNextSibling(ast, child);
                continue;
            }
            {
                uint32_t featureStart = 0;
                if (decodedLen > 14u && memcmp(decoded, "slang/feature/", 14u) == 0) {
                    featureStart = 14u;
                } else if (decodedLen > 8u && memcmp(decoded, "feature/", 8u) == 0) {
                    featureStart = 8u;
                }
                if (featureStart != 0) {
                    uint32_t featureLen = decodedLen - featureStart;
                    if (!(featureLen == 8u && memcmp(decoded + featureStart, "optional", 8u) == 0))
                    {
                        uint32_t argStart = n->dataStart;
                        uint32_t argEnd = n->dataEnd;
                        if (n->dataEnd > n->dataStart + 1u) {
                            argStart = n->dataStart + 1u;
                            argEnd = n->dataEnd - 1u;
                            if (StringLiteralHasDirectOffsetMapping(source, n)) {
                                argStart += featureStart;
                            }
                        }
                        SLDiag diag = {
                            .code = SLDiag_UNKNOWN_FEATURE,
                            .type = SLDiagTypeOfCode(SLDiag_UNKNOWN_FEATURE),
                            .start = n->start,
                            .end = n->end,
                            .argStart = argStart,
                            .argEnd = argEnd,
                        };
                        (void)PrintSLDiag(filename, source, &diag, 0);
                    }
                }
            }
            free(decoded);
        }
        child = ASTNextSibling(ast, child);
    }
}

static int AddPackageFile(
    SLPackage*  pkg,
    const char* filePath,
    char*       source,
    uint32_t    sourceLen,
    SLAst       ast,
    void*       arenaMem) {
    SLParsedFile* f;
    if (EnsureCap((void**)&pkg->files, &pkg->fileCap, pkg->fileLen + 1u, sizeof(SLParsedFile)) != 0)
    {
        return -1;
    }
    f = &pkg->files[pkg->fileLen++];
    f->path = DupCStr(filePath);
    f->source = source;
    f->sourceLen = sourceLen;
    f->arenaMem = arenaMem;
    f->ast = ast;
    if (f->path == NULL) {
        return -1;
    }
    return 0;
}

static int AddDeclText(SLPackage* pkg, char* text, uint32_t fileIndex, int32_t nodeId) {
    SLDeclText* t;
    if (EnsureCap(
            (void**)&pkg->declTexts, &pkg->declTextCap, pkg->declTextLen + 1u, sizeof(SLDeclText))
        != 0)
    {
        return -1;
    }
    t = &pkg->declTexts[pkg->declTextLen++];
    t->text = text;
    t->fileIndex = fileIndex;
    t->nodeId = nodeId;
    return 0;
}

static int AddSymbolDecl(
    SLSymbolDecl** arr,
    uint32_t*      len,
    uint32_t*      cap,
    SLAstKind      kind,
    char*          name,
    char*          declText,
    int            hasBody,
    uint32_t       fileIndex,
    int32_t        nodeId) {
    SLSymbolDecl* d;
    if (EnsureCap((void**)arr, cap, *len + 1u, sizeof(SLSymbolDecl)) != 0) {
        return -1;
    }
    d = &(*arr)[(*len)++];
    d->kind = kind;
    d->name = name;
    d->declText = declText;
    d->hasBody = hasBody;
    d->fileIndex = fileIndex;
    d->nodeId = nodeId;
    return 0;
}

static int AddImportRef(
    SLPackage* pkg,
    char*      alias,
    char* _Nullable bindName,
    char*     importPath,
    uint32_t  fileIndex,
    uint32_t  start,
    uint32_t  end,
    uint32_t* outIndex) {
    SLImportRef* imp;
    if (EnsureCap((void**)&pkg->imports, &pkg->importCap, pkg->importLen + 1u, sizeof(SLImportRef))
        != 0)
    {
        return -1;
    }
    *outIndex = pkg->importLen;
    imp = &pkg->imports[pkg->importLen++];
    imp->alias = alias;
    imp->bindName = bindName;
    imp->path = importPath;
    imp->target = NULL;
    imp->fileIndex = fileIndex;
    imp->start = start;
    imp->end = end;
    return 0;
}

static int AddImportSymbolRef(
    SLPackage* pkg,
    uint32_t   importIndex,
    char*      sourceName,
    char*      localName,
    uint32_t   fileIndex,
    uint32_t   start,
    uint32_t   end) {
    SLImportSymbolRef* sym;
    if (EnsureCap(
            (void**)&pkg->importSymbols,
            &pkg->importSymbolCap,
            pkg->importSymbolLen + 1u,
            sizeof(SLImportSymbolRef))
        != 0)
    {
        return -1;
    }
    sym = &pkg->importSymbols[pkg->importSymbolLen++];
    sym->importIndex = importIndex;
    sym->sourceName = sourceName;
    sym->localName = localName;
    sym->qualifiedName = NULL;
    sym->isType = 0;
    sym->isFunction = 0;
    sym->useWrapper = 0;
    sym->exportFileIndex = 0;
    sym->exportNodeId = -1;
    sym->fnShapeKey = NULL;
    sym->wrapperDeclText = NULL;
    sym->fileIndex = fileIndex;
    sym->start = start;
    sym->end = end;
    return 0;
}

static int IsAsciiSpaceChar(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static char* _Nullable DupPubDeclText(const SLParsedFile* file, int32_t nodeId) {
    const SLAstNode* n = &file->ast.nodes[nodeId];
    uint32_t         start = n->start;
    uint32_t         end = n->end;

    if (start + 3u <= end && memcmp(file->source + start, "pub", 3u) == 0) {
        start += 3u;
        while (start < end && IsAsciiSpaceChar(file->source[start])) {
            start++;
        }
    }

    if (n->kind == SLAst_FN && FnNodeHasBody(&file->ast, nodeId)
        && !FnNodeHasAnytypeParam(file, nodeId))
    {
        int32_t body = ASTFirstChild(&file->ast, nodeId);
        while (body >= 0) {
            if (file->ast.nodes[body].kind == SLAst_BLOCK) {
                end = file->ast.nodes[body].start;
                break;
            }
            body = ASTNextSibling(&file->ast, body);
        }
        while (end > start && IsAsciiSpaceChar(file->source[end - 1u])) {
            end--;
        }
        if (end >= start) {
            char* out = (char*)malloc((size_t)(end - start) + 2u);
            if (out == NULL) {
                return NULL;
            }
            memcpy(out, file->source + start, (size_t)(end - start));
            out[end - start] = ';';
            out[end - start + 1u] = '\0';
            return out;
        }
    }

    return DupSlice(file->source, start, end);
}

static int AddDeclFromNode(
    SLPackage* pkg, const SLParsedFile* file, uint32_t fileIndex, int32_t nodeId, int isPub) {
    const SLAstNode* n = &file->ast.nodes[nodeId];
    int32_t          firstChild;

    if (!IsDeclKind(n->kind)) {
        return 0;
    }
    if (n->end < n->start || n->end > file->sourceLen) {
        return Errorf(file->path, file->source, n->start, n->end, "invalid declaration");
    }
    firstChild = ASTFirstChild(&file->ast, nodeId);
    if ((n->kind == SLAst_VAR || n->kind == SLAst_CONST) && firstChild >= 0
        && file->ast.nodes[firstChild].kind == SLAst_NAME_LIST)
    {
        uint32_t i;
        uint32_t nameCount = AstListCount(&file->ast, firstChild);
        for (i = 0; i < nameCount; i++) {
            int32_t          nameNode = AstListItemAt(&file->ast, firstChild, i);
            const SLAstNode* nameAst =
                (nameNode >= 0 && (uint32_t)nameNode < file->ast.len)
                    ? &file->ast.nodes[nameNode]
                    : NULL;
            char* name;
            char* declText;
            if (nameAst == NULL || nameAst->dataEnd <= nameAst->dataStart) {
                return Errorf(file->path, file->source, n->start, n->end, "invalid declaration");
            }
            name = DupSlice(file->source, nameAst->dataStart, nameAst->dataEnd);
            declText =
                isPub ? DupPubDeclText(file, nodeId) : DupSlice(file->source, n->start, n->end);
            if (name == NULL || declText == NULL) {
                free(name);
                free(declText);
                return ErrorSimple("out of memory");
            }
            if (isPub) {
                uint32_t j;
                for (j = 0; j < pkg->pubDeclLen; j++) {
                    if (pkg->pubDecls[j].kind == n->kind && StrEq(pkg->pubDecls[j].name, name)
                        && n->kind != SLAst_FN)
                    {
                        free(name);
                        free(declText);
                        return Errorf(
                            file->path,
                            file->source,
                            nameAst->dataStart,
                            nameAst->dataEnd,
                            "duplicate public symbol");
                    }
                }
                if (AddSymbolDecl(
                        &pkg->pubDecls,
                        &pkg->pubDeclLen,
                        &pkg->pubDeclCap,
                        n->kind,
                        name,
                        declText,
                        FnNodeHasBody(&file->ast, nodeId),
                        fileIndex,
                        nodeId)
                    != 0)
                {
                    free(name);
                    free(declText);
                    return ErrorSimple("out of memory");
                }
            } else if (
                AddSymbolDecl(
                    &pkg->decls,
                    &pkg->declLen,
                    &pkg->declCap,
                    n->kind,
                    name,
                    declText,
                    FnNodeHasBody(&file->ast, nodeId),
                    fileIndex,
                    nodeId)
                != 0)
            {
                free(name);
                free(declText);
                return ErrorSimple("out of memory");
            }
        }
        return 0;
    }

    if (n->dataEnd <= n->dataStart) {
        return Errorf(file->path, file->source, n->start, n->end, "invalid declaration");
    }
    {
        char* name = DupSlice(file->source, n->dataStart, n->dataEnd);
        char* declText =
            isPub ? DupPubDeclText(file, nodeId) : DupSlice(file->source, n->start, n->end);
        if (name == NULL || declText == NULL) {
            free(name);
            free(declText);
            return ErrorSimple("out of memory");
        }
        if (isPub) {
            uint32_t i;
            for (i = 0; i < pkg->pubDeclLen; i++) {
                if (pkg->pubDecls[i].kind == n->kind && StrEq(pkg->pubDecls[i].name, name)
                    && n->kind != SLAst_FN)
                {
                    free(name);
                    free(declText);
                    return Errorf(
                        file->path,
                        file->source,
                        n->dataStart,
                        n->dataEnd,
                        "duplicate public symbol");
                }
            }
            if (AddSymbolDecl(
                    &pkg->pubDecls,
                    &pkg->pubDeclLen,
                    &pkg->pubDeclCap,
                    n->kind,
                    name,
                    declText,
                    FnNodeHasBody(&file->ast, nodeId),
                    fileIndex,
                    nodeId)
                != 0)
            {
                free(name);
                free(declText);
                return ErrorSimple("out of memory");
            }
            return 0;
        }
        if (AddSymbolDecl(
                &pkg->decls,
                &pkg->declLen,
                &pkg->declCap,
                n->kind,
                name,
                declText,
                FnNodeHasBody(&file->ast, nodeId),
                fileIndex,
                nodeId)
            != 0)
        {
            free(name);
            free(declText);
            return ErrorSimple("out of memory");
        }
        return 0;
    }
}

static int ProcessParsedFile(SLPackage* pkg, uint32_t fileIndex) {
    const SLParsedFile* file = &pkg->files[fileIndex];
    const SLAst*        ast = &file->ast;
    int32_t             child = ASTFirstChild(ast, ast->root);

    /* Accumulate feature flags from this file into the package. */
    pkg->features |= ast->features;

    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (n->kind == SLAst_IMPORT) {
            int32_t          importChild = ASTFirstChild(ast, child);
            const SLAstNode* aliasNode = NULL;
            int              hasSymbols = 0;
            uint8_t*         decodedPathBytes = NULL;
            const char*      pathErr = NULL;
            char*            decodedPath = NULL;
            char*            importPath = NULL;
            uint32_t         decodedPathLen = 0;
            char* _Nullable bindName = NULL;
            int aliasIsUnderscore = 0;
            char* _Nullable mangleAlias = NULL;
            uint32_t importIndex = 0;

            while (importChild >= 0) {
                const SLAstNode* ch = &ast->nodes[importChild];
                if (ch->kind == SLAst_IDENT) {
                    if (aliasNode != NULL) {
                        return Errorf(
                            file->path,
                            file->source,
                            n->start,
                            n->end,
                            "invalid import declaration");
                    }
                    aliasNode = ch;
                } else if (ch->kind == SLAst_IMPORT_SYMBOL) {
                    hasSymbols = 1;
                } else {
                    return Errorf(
                        file->path, file->source, n->start, n->end, "invalid import declaration");
                }
                importChild = ASTNextSibling(ast, importChild);
            }

            if (SLDecodeStringLiteralMalloc(
                    file->source,
                    n->dataStart,
                    n->dataEnd,
                    &decodedPathBytes,
                    &decodedPathLen,
                    NULL)
                != 0)
            {
                return Errorf(
                    file->path,
                    file->source,
                    n->dataStart,
                    n->dataEnd,
                    "invalid import path literal");
            }

            {
                uint32_t i;
                for (i = 0; i < decodedPathLen; i++) {
                    if (decodedPathBytes[i] == (uint8_t)'\0') {
                        free(decodedPathBytes);
                        return ErrorDiagf(
                            file->path,
                            file->source,
                            n->start,
                            n->end,
                            SLDiag_IMPORT_INVALID_PATH,
                            "contains NUL byte");
                    }
                }
            }
            decodedPath = (char*)malloc((size_t)decodedPathLen + 1u);
            if (decodedPath == NULL) {
                free(decodedPathBytes);
                return ErrorSimple("out of memory");
            }
            memcpy(decodedPath, decodedPathBytes, (size_t)decodedPathLen);
            decodedPath[decodedPathLen] = '\0';
            free(decodedPathBytes);
            decodedPathBytes = NULL;

            importPath = (char*)malloc((size_t)decodedPathLen + 1u);
            if (importPath == NULL) {
                free(decodedPath);
                return ErrorSimple("out of memory");
            }
            if (SLNormalizeImportPath(decodedPath, importPath, decodedPathLen + 1u, &pathErr) != 0)
            {
                free(importPath);
                importPath = NULL;
            }
            free(decodedPath);
            if (importPath == NULL) {
                if (pathErr != NULL) {
                    return ErrorDiagf(
                        file->path,
                        file->source,
                        n->start,
                        n->end,
                        SLDiag_IMPORT_INVALID_PATH,
                        pathErr);
                }
                return ErrorSimple("out of memory");
            }

            if (IsFeatureImportPath(importPath)) {
                if (aliasNode != NULL || hasSymbols) {
                    int rc = ErrorDiagf(
                        file->path,
                        file->source,
                        n->start,
                        n->end,
                        SLDiag_IMPORT_FEATURE_IMPORT_EXTRAS);
                    free(importPath);
                    return rc;
                }
                free(importPath);
                child = ASTNextSibling(ast, child);
                continue;
            }

            if (aliasNode != NULL) {
                bindName = DupSlice(file->source, aliasNode->dataStart, aliasNode->dataEnd);
                if (bindName == NULL) {
                    free(importPath);
                    return ErrorSimple("out of memory");
                }
                aliasIsUnderscore = StrEq(bindName, "_");
                if (aliasIsUnderscore) {
                    free(bindName);
                    bindName = NULL;
                }
            }

            if (aliasIsUnderscore && hasSymbols) {
                int rc = ErrorDiagf(
                    file->path,
                    file->source,
                    n->start,
                    n->end,
                    SLDiag_IMPORT_SIDE_EFFECT_ALIAS_WITH_SYMBOLS);
                free(importPath);
                return rc;
            }

            if (bindName == NULL && !hasSymbols) {
                bindName = DefaultImportAlias(importPath);
                if (bindName == NULL) {
                    int rc = ErrorDiagf(
                        file->path,
                        file->source,
                        n->start,
                        n->end,
                        SLDiag_IMPORT_ALIAS_INFERENCE_FAILED,
                        importPath);
                    free(importPath);
                    return rc;
                }
            }

            if (bindName != NULL && IsReservedSLPrefixName(bindName)) {
                int rc = Errorf(
                    file->path,
                    file->source,
                    n->start,
                    n->end,
                    "identifier prefix '__sl_' is reserved");
                free(bindName);
                free(importPath);
                return rc;
            }

            mangleAlias = MakeUniqueImportAlias(
                pkg, bindName != NULL ? bindName : LastPathSegment(importPath));
            if (mangleAlias == NULL) {
                free(bindName);
                free(importPath);
                return ErrorSimple("out of memory");
            }
            if (AddImportRef(
                    pkg,
                    mangleAlias,
                    bindName,
                    importPath,
                    fileIndex,
                    n->start,
                    n->end,
                    &importIndex)
                != 0)
            {
                free(mangleAlias);
                free(bindName);
                free(importPath);
                return ErrorSimple("out of memory");
            }

            importChild = ASTFirstChild(ast, child);
            while (importChild >= 0) {
                const SLAstNode* ch = &ast->nodes[importChild];
                if (ch->kind == SLAst_IMPORT_SYMBOL) {
                    int32_t localAliasNode = ASTFirstChild(ast, importChild);
                    char*   sourceName = DupSlice(file->source, ch->dataStart, ch->dataEnd);
                    char*   localName;
                    if (sourceName == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    if (localAliasNode >= 0 && ast->nodes[localAliasNode].kind == SLAst_IDENT) {
                        const SLAstNode* ln = &ast->nodes[localAliasNode];
                        localName = DupSlice(file->source, ln->dataStart, ln->dataEnd);
                    } else {
                        localName = DupCStr(sourceName);
                    }
                    if (localName == NULL) {
                        free(sourceName);
                        return ErrorSimple("out of memory");
                    }
                    if (StrEq(localName, "_")) {
                        int rc = ErrorDiagf(
                            file->path,
                            file->source,
                            ch->start,
                            ch->end,
                            SLDiag_IMPORT_SYMBOL_ALIAS_INVALID);
                        free(sourceName);
                        free(localName);
                        return rc;
                    }
                    if (AddImportSymbolRef(
                            pkg, importIndex, sourceName, localName, fileIndex, ch->start, ch->end)
                        != 0)
                    {
                        free(sourceName);
                        free(localName);
                        return ErrorSimple("out of memory");
                    }
                }
                importChild = ASTNextSibling(ast, importChild);
            }
        } else {
            char* declText = DupSlice(file->source, n->start, n->end);
            if (declText == NULL) {
                return ErrorSimple("out of memory");
            }
            if (AddDeclText(pkg, declText, fileIndex, child) != 0) {
                free(declText);
                return ErrorSimple("out of memory");
            }
            if (AddDeclFromNode(pkg, file, fileIndex, child, IsPubDeclNode(n)) != 0) {
                return -1;
            }
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

static int IsBuiltinTypeName(const char* src, uint32_t start, uint32_t end) {
    return SliceEqCStr(src, start, end, "bool") || SliceEqCStr(src, start, end, "str")
        || SliceEqCStr(src, start, end, "rawptr") || SliceEqCStr(src, start, end, "type")
        || SliceEqCStr(src, start, end, "anytype") || SliceEqCStr(src, start, end, "u8")
        || SliceEqCStr(src, start, end, "u16") || SliceEqCStr(src, start, end, "u32")
        || SliceEqCStr(src, start, end, "u64") || SliceEqCStr(src, start, end, "i8")
        || SliceEqCStr(src, start, end, "i16") || SliceEqCStr(src, start, end, "i32")
        || SliceEqCStr(src, start, end, "i64") || SliceEqCStr(src, start, end, "uint")
        || SliceEqCStr(src, start, end, "int") || SliceEqCStr(src, start, end, "f32")
        || SliceEqCStr(src, start, end, "f64");
}

static int PackageHasExport(const SLPackage* pkg, const char* name) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (StrEq(pkg->pubDecls[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

static int PackageHasExportSlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        size_t nameLen = strlen(pkg->pubDecls[i].name);
        if (nameLen == (size_t)(end - start)
            && memcmp(pkg->pubDecls[i].name, src + start, nameLen) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int PackageHasExportedTypeSlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (!IsTypeDeclKind(pkg->pubDecls[i].kind)) {
            continue;
        }
        if (strlen(pkg->pubDecls[i].name) == (size_t)(end - start)
            && memcmp(pkg->pubDecls[i].name, src + start, (size_t)(end - start)) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int PackageHasImportedTypeSymbolSlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                   nameLen;
        if (!sym->isType) {
            continue;
        }
        nameLen = strlen(sym->localName);
        if (nameLen == (size_t)(end - start) && memcmp(sym->localName, src + start, nameLen) == 0) {
            return 1;
        }
    }
    return 0;
}

static uint32_t FindSliceDot(const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = start; i < end; i++) {
        if (src[i] == '.') {
            return i;
        }
    }
    return end;
}

static const SLImportRef* _Nullable FindImportByAliasSlice(
    const SLPackage* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd);

static int ValidatePubTypeNode(
    const SLPackage* pkg, const SLParsedFile* file, int32_t typeNodeId, const char* contextMsg) {
    const SLAstNode* n;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= file->ast.len) {
        return ErrorSimple("invalid type node");
    }
    n = &file->ast.nodes[typeNodeId];
    switch (n->kind) {
        case SLAst_TYPE_NAME: {
            uint32_t dotPos = FindSliceDot(file->source, n->dataStart, n->dataEnd);
            if (dotPos < n->dataEnd) {
                const SLImportRef* imp = FindImportByAliasSlice(
                    pkg, file->source, n->dataStart, dotPos);
                if (imp == NULL) {
                    if (PackageHasExportedTypeSlice(pkg, file->source, n->dataStart, dotPos)
                        || PackageHasImportedTypeSymbolSlice(
                            pkg, file->source, n->dataStart, dotPos))
                    {
                        return 0;
                    }
                    return Errorf(
                        file->path,
                        file->source,
                        n->start,
                        n->end,
                        "public API %s references non-exported type",
                        contextMsg);
                }
                if (imp->target == NULL) {
                    return 0;
                }
                {
                    uint32_t memberRootStart = dotPos + 1u;
                    uint32_t memberRootEnd = FindSliceDot(
                        file->source, memberRootStart, n->dataEnd);
                    if (PackageHasExportedTypeSlice(
                            imp->target, file->source, memberRootStart, memberRootEnd))
                    {
                        return 0;
                    }
                }
                return Errorf(
                    file->path,
                    file->source,
                    n->start,
                    n->end,
                    "public API %s references non-exported type",
                    contextMsg);
            }
            if (IsBuiltinTypeName(file->source, n->dataStart, n->dataEnd)) {
                return 0;
            }
            if (PackageHasExportedTypeSlice(pkg, file->source, n->dataStart, n->dataEnd)) {
                return 0;
            }
            if (PackageHasImportedTypeSymbolSlice(pkg, file->source, n->dataStart, n->dataEnd)) {
                return 0;
            }
            return Errorf(
                file->path,
                file->source,
                n->start,
                n->end,
                "public API %s references non-exported type '%.*s'",
                contextMsg,
                (int)(n->dataEnd - n->dataStart),
                file->source + n->dataStart);
        }
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
        case SLAst_TYPE_ARRAY:
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE:
        case SLAst_TYPE_VARRAY:
        case SLAst_TYPE_OPTIONAL: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            return ValidatePubTypeNode(pkg, file, child, contextMsg);
        }
        case SLAst_TYPE_TUPLE: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            while (child >= 0) {
                if (ValidatePubTypeNode(pkg, file, child, contextMsg) != 0) {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        case SLAst_TYPE_FN: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            while (child >= 0) {
                if (ValidatePubTypeNode(pkg, file, child, contextMsg) != 0) {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        default:
            return Errorf(file->path, file->source, n->start, n->end, "invalid type in public API");
    }
}

static int ValidatePubClosure(const SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        const SLSymbolDecl* pubDecl = &pkg->pubDecls[i];
        const SLParsedFile* file = &pkg->files[pubDecl->fileIndex];
        int32_t             child = ASTFirstChild(&file->ast, pubDecl->nodeId);
        if (pubDecl->kind == SLAst_FN) {
            while (child >= 0) {
                const SLAstNode* n = &file->ast.nodes[child];
                if (n->kind == SLAst_PARAM) {
                    int32_t typeNode = ASTFirstChild(&file->ast, child);
                    if (ValidatePubTypeNode(pkg, file, typeNode, "function parameter") != 0) {
                        return -1;
                    }
                } else if (IsFnReturnTypeNodeKind(n->kind) && n->flags == 1) {
                    if (ValidatePubTypeNode(pkg, file, child, "function return type") != 0) {
                        return -1;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
        } else if (pubDecl->kind == SLAst_STRUCT || pubDecl->kind == SLAst_UNION) {
            while (child >= 0) {
                const SLAstNode* n = &file->ast.nodes[child];
                if (n->kind == SLAst_FIELD) {
                    int32_t typeNode = ASTFirstChild(&file->ast, child);
                    if (ValidatePubTypeNode(pkg, file, typeNode, "field type") != 0) {
                        return -1;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
        } else if (pubDecl->kind == SLAst_ENUM) {
            if (child >= 0) {
                const SLAstNode* n = &file->ast.nodes[child];
                if (n->kind == SLAst_TYPE_NAME || n->kind == SLAst_TYPE_PTR
                    || n->kind == SLAst_TYPE_REF || n->kind == SLAst_TYPE_MUTREF
                    || n->kind == SLAst_TYPE_ARRAY || n->kind == SLAst_TYPE_VARRAY
                    || n->kind == SLAst_TYPE_SLICE || n->kind == SLAst_TYPE_MUTSLICE
                    || n->kind == SLAst_TYPE_FN || n->kind == SLAst_TYPE_TUPLE)
                {
                    if (ValidatePubTypeNode(pkg, file, child, "enum base type") != 0) {
                        return -1;
                    }
                }
            }
        } else if (pubDecl->kind == SLAst_VAR || pubDecl->kind == SLAst_CONST) {
            if (child >= 0 && IsFnReturnTypeNodeKind(file->ast.nodes[child].kind)) {
                if (ValidatePubTypeNode(
                        pkg,
                        file,
                        child,
                        pubDecl->kind == SLAst_VAR ? "variable type" : "constant type")
                    != 0)
                {
                    return -1;
                }
            }
        } else if (pubDecl->kind == SLAst_TYPE_ALIAS) {
            const SLAstNode* aliasNode = &file->ast.nodes[pubDecl->nodeId];
            if (child < 0) {
                return Errorf(
                    file->path,
                    file->source,
                    aliasNode->start,
                    aliasNode->end,
                    "missing alias target type");
            }
            if (ValidatePubTypeNode(pkg, file, child, "type alias target") != 0) {
                return -1;
            }
        }
    }
    return 0;
}

static int ValidatePubFnDefinitions(const SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        const SLSymbolDecl* pubDecl = &pkg->pubDecls[i];
        uint32_t            j;
        int                 found = pubDecl->hasBody;
        if (pubDecl->kind != SLAst_FN) {
            continue;
        }
        for (j = 0; j < pkg->declLen; j++) {
            const SLSymbolDecl* decl = &pkg->decls[j];
            if (decl->kind == SLAst_FN && StrEq(decl->name, pubDecl->name) && decl->hasBody) {
                found = 1;
                break;
            }
        }
        if (!found) {
            const SLParsedFile* file = &pkg->files[pubDecl->fileIndex];
            const SLAstNode*    n = &file->ast.nodes[pubDecl->nodeId];
            return Errorf(
                file->path,
                file->source,
                n->dataStart,
                n->dataEnd,
                "missing definition for exported function %s",
                pubDecl->name);
        }
    }
    return 0;
}

static const SLImportRef* _Nullable FindImportByAliasSlice(
    const SLPackage* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        if (pkg->imports[i].bindName != NULL
            && strlen(pkg->imports[i].bindName) == (size_t)(aliasEnd - aliasStart)
            && memcmp(pkg->imports[i].bindName, src + aliasStart, (size_t)(aliasEnd - aliasStart))
                   == 0)
        {
            return &pkg->imports[i];
        }
    }
    return NULL;
}

static int PackageHasEnumDeclBySlice(
    const SLPackage* pkg, const char* src, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    size_t   n = (size_t)(nameEnd - nameStart);
    for (i = 0; i < pkg->declLen; i++) {
        if (pkg->decls[i].kind == SLAst_ENUM && strlen(pkg->decls[i].name) == n
            && memcmp(pkg->decls[i].name, src + nameStart, n) == 0)
        {
            return 1;
        }
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (pkg->pubDecls[i].kind == SLAst_ENUM && strlen(pkg->pubDecls[i].name) == n
            && memcmp(pkg->pubDecls[i].name, src + nameStart, n) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int ValidateSelectorsNode(const SLPackage* pkg, const SLParsedFile* file, int32_t nodeId) {
    const SLAstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];

    if (n->kind == SLAst_TYPE_NAME) {
        uint32_t dot = FindSliceDot(file->source, n->dataStart, n->dataEnd);
        if (dot < n->dataEnd) {
            const SLImportRef* imp = FindImportByAliasSlice(pkg, file->source, n->dataStart, dot);
            if (imp == NULL) {
                if (!PackageHasEnumDeclBySlice(pkg, file->source, n->dataStart, dot)
                    && !PackageHasExportedTypeSlice(pkg, file->source, n->dataStart, dot)
                    && !PackageHasImportedTypeSymbolSlice(pkg, file->source, n->dataStart, dot))
                {
                    return Errorf(
                        file->path, file->source, n->start, n->end, "unknown import alias");
                }
            } else {
                uint32_t memberRootStart = dot + 1u;
                uint32_t memberRootEnd = FindSliceDot(file->source, memberRootStart, n->dataEnd);
                if (!PackageHasExportSlice(
                        imp->target, file->source, memberRootStart, memberRootEnd))
                {
                    return Errorf(
                        file->path, file->source, n->start, n->end, "unknown imported symbol");
                }
            }
        }
    } else if (n->kind == SLAst_FIELD_EXPR) {
        int32_t recvNode = ASTFirstChild(&file->ast, nodeId);
        if (recvNode >= 0 && file->ast.nodes[recvNode].kind == SLAst_IDENT) {
            const SLAstNode*   recv = &file->ast.nodes[recvNode];
            const SLImportRef* imp = FindImportByAliasSlice(
                pkg, file->source, recv->dataStart, recv->dataEnd);
            if (imp != NULL) {
                if (!PackageHasExportSlice(imp->target, file->source, n->dataStart, n->dataEnd)) {
                    return Errorf(
                        file->path, file->source, n->start, n->end, "unknown imported symbol");
                }
            }
        }
    }

    child = ASTFirstChild(&file->ast, nodeId);
    while (child >= 0) {
        if (ValidateSelectorsNode(pkg, file, child) != 0) {
            return -1;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return 0;
}

static int ValidatePackageSelectors(const SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->fileLen; i++) {
        const SLParsedFile* file = &pkg->files[i];
        if (ValidateSelectorsNode(pkg, file, file->ast.root) != 0) {
            return -1;
        }
    }
    return 0;
}

static int PackageHasAnyDeclName(const SLPackage* pkg, const char* name) {
    uint32_t i;
    for (i = 0; i < pkg->declLen; i++) {
        if (StrEq(pkg->decls[i].name, name)) {
            return 1;
        }
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (StrEq(pkg->pubDecls[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

static int PackageHasImportSymbolLocalName(const SLPackage* pkg, const char* name) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        if (StrEq(pkg->importSymbols[i].localName, name)) {
            return 1;
        }
    }
    return 0;
}

static int ValidateImportBindingConflicts(SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        const SLImportRef* imp = &pkg->imports[i];
        uint32_t           j;
        if (imp->bindName == NULL) {
            continue;
        }
        if (PackageHasAnyDeclName(pkg, imp->bindName)) {
            const SLParsedFile* file = &pkg->files[imp->fileIndex];
            return Errorf(
                file->path, file->source, imp->start, imp->end, "import binding conflict");
        }
        for (j = i + 1u; j < pkg->importLen; j++) {
            if (pkg->imports[j].bindName != NULL && StrEq(pkg->imports[j].bindName, imp->bindName))
            {
                const SLParsedFile* file = &pkg->files[pkg->imports[j].fileIndex];
                return Errorf(
                    file->path,
                    file->source,
                    pkg->imports[j].start,
                    pkg->imports[j].end,
                    "import binding conflict");
            }
        }
        for (j = 0; j < pkg->importSymbolLen; j++) {
            if (StrEq(pkg->importSymbols[j].localName, imp->bindName)) {
                const SLParsedFile* file = &pkg->files[pkg->importSymbols[j].fileIndex];
                return Errorf(
                    file->path,
                    file->source,
                    pkg->importSymbols[j].start,
                    pkg->importSymbols[j].end,
                    "import binding conflict");
            }
        }
    }

    for (i = 0; i < pkg->importSymbolLen; i++) {
        SLImportSymbolRef* sym = &pkg->importSymbols[i];
        uint32_t           j;
        if (PackageHasAnyDeclName(pkg, sym->localName)) {
            const SLParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "import binding conflict");
        }
        for (j = i + 1u; j < pkg->importSymbolLen; j++) {
            SLImportSymbolRef* other = &pkg->importSymbols[j];
            if (!StrEq(other->localName, sym->localName)) {
                continue;
            }
            if (sym->isFunction && other->isFunction) {
                if (sym->fnShapeKey == NULL || other->fnShapeKey == NULL) {
                    return ErrorSimple("internal error: missing import function shape");
                }
                if (!StrEq(sym->fnShapeKey, other->fnShapeKey)) {
                    sym->useWrapper = 1;
                    other->useWrapper = 1;
                    continue;
                }
            }
            {
                const SLParsedFile* file = &pkg->files[other->fileIndex];
                return Errorf(
                    file->path, file->source, other->start, other->end, "import binding conflict");
            }
        }
    }
    return 0;
}

static int ValidateAndFinalizeImportSymbols(SLPackage* pkg) {
    uint32_t baseLen = pkg->importSymbolLen;
    uint32_t i;
    for (i = 0; i < baseLen; i++) {
        SLImportSymbolRef* sym = &pkg->importSymbols[i];
        const SLImportRef* imp;
        const SLPackage*   dep;
        uint32_t           j;
        uint32_t           matchCount = 0;
        if (sym->importIndex >= pkg->importLen) {
            return ErrorSimple("internal error: invalid import symbol mapping");
        }
        imp = &pkg->imports[sym->importIndex];
        dep = imp->target;
        if (dep == NULL) {
            return ErrorSimple("internal error: unresolved import");
        }
        for (j = 0; j < dep->pubDeclLen; j++) {
            const SLSymbolDecl* exportDecl = &dep->pubDecls[j];
            SLImportSymbolRef*  dstSym = sym;
            char*               rewrittenDecl = NULL;
            char*               shapeKey = NULL;
            char*               wrapperDecl = NULL;
            if (!StrEq(exportDecl->name, sym->sourceName)) {
                continue;
            }
            if (matchCount > 0) {
                char* sourceName = DupCStr(sym->sourceName);
                char* localName = DupCStr(sym->localName);
                if (sourceName == NULL || localName == NULL) {
                    free(sourceName);
                    free(localName);
                    return ErrorSimple("out of memory");
                }
                if (AddImportSymbolRef(
                        pkg,
                        sym->importIndex,
                        sourceName,
                        localName,
                        sym->fileIndex,
                        sym->start,
                        sym->end)
                    != 0)
                {
                    free(sourceName);
                    free(localName);
                    return ErrorSimple("out of memory");
                }
                dstSym = &pkg->importSymbols[pkg->importSymbolLen - 1u];
            }
            dstSym->isType = IsTypeDeclKind(exportDecl->kind) ? 1u : 0u;
            dstSym->isFunction = exportDecl->kind == SLAst_FN ? 1u : 0u;
            dstSym->useWrapper = 0;
            dstSym->exportFileIndex = exportDecl->fileIndex;
            dstSym->exportNodeId = exportDecl->nodeId;
            if (dstSym->qualifiedName == NULL
                && BuildPrefixedName(imp->alias, dstSym->sourceName, &dstSym->qualifiedName) != 0)
            {
                return ErrorSimple("out of memory");
            }
            if (dstSym->isFunction) {
                if (RewriteAliasedPubDeclText(dep, exportDecl, imp->alias, &rewrittenDecl) != 0) {
                    return -1;
                }
                if (BuildFnImportShapeAndWrapper(
                        rewrittenDecl,
                        dstSym->localName,
                        dstSym->qualifiedName,
                        &shapeKey,
                        &wrapperDecl)
                    != 0)
                {
                    free(rewrittenDecl);
                    return -1;
                }
                free(rewrittenDecl);
                dstSym->fnShapeKey = shapeKey;
                dstSym->wrapperDeclText = wrapperDecl;
            }
            matchCount++;
        }
        if (matchCount == 0) {
            const SLParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "unknown imported symbol");
        }
    }
    return 0;
}

static SLPackage* _Nullable FindPackageByDir(const SLPackageLoader* loader, const char* dirPath) {
    uint32_t i;
    for (i = 0; i < loader->packageLen; i++) {
        if (StrEq(loader->packages[i].dirPath, dirPath)) {
            return &loader->packages[i];
        }
    }
    return NULL;
}

static char* _Nullable InferPackageNameFromDirPath(const char* dirPath) {
    return LastPathComponentDup(dirPath);
}

static char* _Nullable InferPackageNameFromSingleFile(const char* filePath) {
    char* dirPath = DirNameDup(filePath);
    char* dirName = NULL;
    char* fileName = NULL;
    char* baseName = NULL;
    char* outName = NULL;
    if (dirPath == NULL) {
        return NULL;
    }
    dirName = LastPathComponentDup(dirPath);
    if (dirName != NULL && IsValidIdentifier(dirName)) {
        outName = dirName;
        free(dirPath);
        return outName;
    }
    free(dirName);
    fileName = BaseNameDup(filePath);
    if (fileName == NULL) {
        free(dirPath);
        return NULL;
    }
    baseName = StripSLExtensionDup(fileName);
    free(fileName);
    free(dirPath);
    if (baseName == NULL) {
        return NULL;
    }
    if (IsValidIdentifier(baseName)) {
        return baseName;
    }
    free(baseName);
    return NULL;
}

static int AddPackageSlot(SLPackageLoader* loader, const char* dirPath, SLPackage** outPkg) {
    SLPackage* pkg;
    if (EnsureCap(
            (void**)&loader->packages,
            &loader->packageCap,
            loader->packageLen + 1u,
            sizeof(SLPackage))
        != 0)
    {
        return -1;
    }
    pkg = &loader->packages[loader->packageLen++];
    memset(pkg, 0, sizeof(*pkg));
    pkg->dirPath = DupCStr(dirPath);
    if (pkg->dirPath == NULL) {
        return -1;
    }
    pkg->name = InferPackageNameFromDirPath(dirPath);
    if (pkg->name == NULL) {
        free(pkg->dirPath);
        pkg->dirPath = NULL;
        return -1;
    }
    *outPkg = pkg;
    return 0;
}

static char* _Nullable CanonicalizePath(const char* path) {
    char* out = realpath(path, NULL);
    return out;
}

static int IsDirectoryPath(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return S_ISDIR(st.st_mode);
}

static int DirectoryHasSLFiles(const char* dirPath) {
    DIR*           dir = opendir(dirPath);
    struct dirent* ent;
    if (dir == NULL) {
        return 0;
    }
    while ((ent = readdir(dir)) != NULL) {
        const char* name = ent->d_name;
        size_t      len;
        if (name[0] == '.') {
            continue;
        }
        len = strlen(name);
        if (len >= 3 && strcmp(name + len - 3, ".sl") == 0) {
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

static char* _Nullable ResolveLibImportDirInRoot(const char* rootDir, const char* importPath) {
    char* libDir = JoinPath(rootDir, "lib");
    char* candidate;
    if (libDir == NULL) {
        return NULL;
    }
    candidate = JoinPath(libDir, importPath);
    free(libDir);
    if (candidate == NULL) {
        return NULL;
    }
    if (IsDirectoryPath(candidate) && DirectoryHasSLFiles(candidate)) {
        return candidate;
    }
    free(candidate);
    return NULL;
}

static int IsLibImportPath(const char* importPath) {
    return StrEq(importPath, "builtin") || StrEq(importPath, "reflect") || StrEq(importPath, "mem")
        || StrEq(importPath, "platform") || StrEq(importPath, "compiler")
        || StrEq(importPath, "str") || strncmp(importPath, "builtin/", 8u) == 0
        || strncmp(importPath, "reflect/", 8u) == 0 || strncmp(importPath, "mem/", 4u) == 0
        || strncmp(importPath, "compiler/", 9u) == 0 || strncmp(importPath, "std/", 4u) == 0
        || strncmp(importPath, "platform/", 9u) == 0 || strncmp(importPath, "str/", 4u) == 0;
}

static char* _Nullable ResolveLibImportDir(const char* startDir, const char* importPath) {
    char* dir;
    if (!IsLibImportPath(importPath)) {
        return NULL;
    }
    dir = DupCStr(startDir);
    if (dir == NULL) {
        return NULL;
    }
    for (;;) {
        char* candidate = ResolveLibImportDirInRoot(dir, importPath);
        if (candidate != NULL) {
            free(dir);
            return candidate;
        }
        {
            char* parent = DirNameDup(dir);
            if (parent == NULL || StrEq(parent, dir)) {
                free(parent);
                break;
            }
            free(dir);
            dir = parent;
        }
    }
    free(dir);
    return NULL;
}

static char* _Nullable ResolveLibImportDirFromExe(const char* importPath) {
    char* exeDir = NULL;
    char* resolved = NULL;
    if (!IsLibImportPath(importPath)) {
        return NULL;
    }
    exeDir = GetExeDir();
    if (exeDir == NULL) {
        return NULL;
    }
    resolved = ResolveLibImportDir(exeDir, importPath);
    free(exeDir);
    return resolved;
}

static int LoadPackageRecursive(SLPackageLoader* loader, const char* dirPath, SLPackage** outPkg);

static int FindImportIndexByPath(const SLPackage* pkg, const char* importPath) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        if (StrEq(pkg->imports[i].path, importPath)) {
            return (int)i;
        }
    }
    return -1;
}

static int IsBuiltinPackage(const SLPackage* pkg) {
    const char* base;
    if (pkg == NULL || pkg->dirPath == NULL) {
        return 0;
    }
    base = strrchr(pkg->dirPath, '/');
    base = base != NULL ? base + 1 : pkg->dirPath;
    return StrEq(base, "builtin");
}

static int IsReflectPackage(const SLPackage* pkg) {
    const char* base;
    if (pkg == NULL || pkg->dirPath == NULL) {
        return 0;
    }
    base = strrchr(pkg->dirPath, '/');
    base = base != NULL ? base + 1 : pkg->dirPath;
    return StrEq(base, "reflect");
}

static int EnsureImplicitBuiltinImport(SLPackage* pkg) {
    char*    alias = NULL;
    char*    importPath = NULL;
    uint32_t importIndex = 0;
    if (IsBuiltinPackage(pkg)) {
        return 0;
    }
    if (FindImportIndexByPath(pkg, "builtin") >= 0) {
        return 0;
    }
    alias = MakeUniqueImportAlias(pkg, "builtin");
    importPath = DupCStr("builtin");
    if (alias == NULL || importPath == NULL) {
        free(alias);
        free(importPath);
        return ErrorSimple("out of memory");
    }
    if (AddImportRef(pkg, alias, NULL, importPath, 0, 0, 0, &importIndex) != 0) {
        free(alias);
        free(importPath);
        return ErrorSimple("out of memory");
    }
    return 0;
}

static int EnsureImplicitReflectImport(SLPackage* pkg) {
    char*    alias = NULL;
    char*    importPath = NULL;
    uint32_t importIndex = 0;
    if (IsBuiltinPackage(pkg) || IsReflectPackage(pkg)) {
        return 0;
    }
    if (FindImportIndexByPath(pkg, "reflect") >= 0) {
        return 0;
    }
    alias = MakeUniqueImportAlias(pkg, "reflect");
    importPath = DupCStr("reflect");
    if (alias == NULL || importPath == NULL) {
        free(alias);
        free(importPath);
        return ErrorSimple("out of memory");
    }
    if (AddImportRef(pkg, alias, NULL, importPath, 0, 0, 0, &importIndex) != 0) {
        free(alias);
        free(importPath);
        return ErrorSimple("out of memory");
    }
    return 0;
}

static int EnsureImplicitBuiltinImportSymbols(SLPackage* pkg) {
    int builtinImportIndex = FindImportIndexByPath(pkg, "builtin");
    if (builtinImportIndex < 0) {
        return 0;
    }
    {
        const SLPackage* dep = pkg->imports[(uint32_t)builtinImportIndex].target;
        uint32_t         i;
        if (dep == NULL) {
            return ErrorSimple("internal error: unresolved builtin import");
        }
        for (i = 0; i < dep->pubDeclLen; i++) {
            uint32_t j;
            int      alreadyMapped = 0;
            if (PackageHasAnyDeclName(pkg, dep->pubDecls[i].name)
                || PackageHasImportSymbolLocalName(pkg, dep->pubDecls[i].name))
            {
                continue;
            }
            for (j = 0; j < pkg->importSymbolLen; j++) {
                const SLImportSymbolRef* sym = &pkg->importSymbols[j];
                if (sym->importIndex == (uint32_t)builtinImportIndex
                    && StrEq(sym->sourceName, dep->pubDecls[i].name)
                    && StrEq(sym->localName, dep->pubDecls[i].name))
                {
                    alreadyMapped = 1;
                    break;
                }
            }
            if (!alreadyMapped) {
                char* sourceName = DupCStr(dep->pubDecls[i].name);
                char* localName = DupCStr(dep->pubDecls[i].name);
                if (sourceName == NULL || localName == NULL) {
                    free(sourceName);
                    free(localName);
                    return ErrorSimple("out of memory");
                }
                if (AddImportSymbolRef(
                        pkg, (uint32_t)builtinImportIndex, sourceName, localName, 0, 0, 0)
                    != 0)
                {
                    free(sourceName);
                    free(localName);
                    return ErrorSimple("out of memory");
                }
            }
        }
    }
    return 0;
}

static int LoadSelectedPlatformTargetPackage(
    SLPackageLoader* loader, const char* startDir, SLPackage** outPkg) {
    char* importPath = NULL;
    char* resolvedDir = NULL;
    int   rc = -1;

    if (loader->platformTarget == NULL || loader->platformTarget[0] == '\0') {
        return ErrorSimple("internal error: missing platform target");
    }

    {
        SLStringBuilder b = { 0 };
        if (SBAppendCStr(&b, "platform/") != 0 || SBAppendCStr(&b, loader->platformTarget) != 0) {
            free(b.v);
            return ErrorSimple("out of memory");
        }
        importPath = SBFinish(&b, NULL);
        if (importPath == NULL) {
            return ErrorSimple("out of memory");
        }
    }

    resolvedDir = JoinPath(loader->rootDir, importPath);
    if (resolvedDir != NULL && !IsDirectoryPath(resolvedDir)) {
        char* libResolved = ResolveLibImportDir(startDir, importPath);
        if (libResolved == NULL) {
            libResolved = ResolveLibImportDirFromExe(importPath);
        }
        if (libResolved != NULL) {
            free(resolvedDir);
            resolvedDir = libResolved;
        }
    }
    if (resolvedDir == NULL) {
        free(importPath);
        return ErrorSimple("out of memory");
    }
    {
        SLPackage*  tmpPkg = NULL;
        SLPackage** outPtr = outPkg != NULL ? outPkg : &tmpPkg;
        if (LoadPackageRecursive(loader, resolvedDir, outPtr) != 0) {
            rc = ErrorSimple("failed to resolve platform target package %s", importPath);
        } else {
            rc = 0;
        }
    }
    free(importPath);
    free(resolvedDir);
    return rc;
}

static int ResolvePackageImportsAndSelectors(SLPackageLoader* loader, SLPackage* pkg) {
    uint32_t i;
    if (EnsureImplicitBuiltinImport(pkg) != 0) {
        return -1;
    }
    if (EnsureImplicitReflectImport(pkg) != 0) {
        return -1;
    }
    for (i = 0; i < pkg->importLen; i++) {
        char* resolvedDir;
        resolvedDir = JoinPath(loader->rootDir, pkg->imports[i].path);
        if (resolvedDir != NULL && !IsDirectoryPath(resolvedDir)) {
            char* localResolved = JoinPath(pkg->dirPath, pkg->imports[i].path);
            if (localResolved == NULL) {
                free(resolvedDir);
                return ErrorSimple("out of memory");
            }
            if (IsDirectoryPath(localResolved)) {
                free(resolvedDir);
                resolvedDir = localResolved;
            } else {
                free(localResolved);
                if (IsLibImportPath(pkg->imports[i].path)) {
                    char* libResolved = ResolveLibImportDir(pkg->dirPath, pkg->imports[i].path);
                    if (libResolved == NULL) {
                        libResolved = ResolveLibImportDirFromExe(pkg->imports[i].path);
                    }
                    if (libResolved != NULL) {
                        free(resolvedDir);
                        resolvedDir = libResolved;
                    }
                }
            }
        }
        if (resolvedDir == NULL) {
            return ErrorSimple("out of memory");
        }
        if (LoadPackageRecursive(loader, resolvedDir, &pkg->imports[i].target) != 0) {
            const SLParsedFile* file = &pkg->files[pkg->imports[i].fileIndex];
            free(resolvedDir);
            return Errorf(
                file->path,
                file->source,
                pkg->imports[i].start,
                pkg->imports[i].end,
                "failed to resolve import %s",
                pkg->imports[i].path);
        }
        free(resolvedDir);
    }

    if (EnsureImplicitBuiltinImportSymbols(pkg) != 0) {
        return -1;
    }
    if (ValidateAndFinalizeImportSymbols(pkg) != 0) {
        return -1;
    }
    if (ValidateImportBindingConflicts(pkg) != 0) {
        return -1;
    }
    if (ValidatePackageSelectors(pkg) != 0) {
        return -1;
    }
    if (ValidatePubClosure(pkg) != 0) {
        return -1;
    }
    pkg->loadState = 2;
    return 0;
}

static int LoadPackageRecursive(SLPackageLoader* loader, const char* dirPath, SLPackage** outPkg) {
    char*      canonical = CanonicalizePath(dirPath);
    SLPackage* pkg;
    char**     filePaths = NULL;
    uint32_t   fileCount = 0;
    uint32_t   i;

    if (canonical == NULL) {
        return ErrorSimple("failed to resolve package path %s", dirPath);
    }

    pkg = FindPackageByDir(loader, canonical);
    if (pkg != NULL) {
        if (pkg->loadState == 1) {
            free(canonical);
            return ErrorSimple("import cycle detected");
        }
        *outPkg = pkg;
        free(canonical);
        return 0;
    }

    if (AddPackageSlot(loader, canonical, &pkg) != 0) {
        free(canonical);
        return ErrorSimple("out of memory");
    }
    free(canonical);
    pkg->loadState = 1;

    if (ListSLFiles(pkg->dirPath, &filePaths, &fileCount) != 0) {
        return -1;
    }

    for (i = 0; i < fileCount; i++) {
        char*    source = NULL;
        uint32_t sourceLen = 0;
        SLAst    ast;
        void*    arenaMem = NULL;
        if (ReadFile(filePaths[i], &source, &sourceLen) != 0) {
            return -1;
        }
        if (ParseSourceEx(filePaths[i], source, sourceLen, &ast, &arenaMem, NULL, 1) != 0) {
            free(source);
            return -1;
        }
        if (AddPackageFile(pkg, filePaths[i], source, sourceLen, ast, arenaMem) != 0) {
            free(source);
            free(arenaMem);
            return ErrorSimple("out of memory");
        }
    }

    for (i = 0; i < fileCount; i++) {
        free(filePaths[i]);
    }
    free(filePaths);

    for (i = 0; i < pkg->fileLen; i++) {
        if (ProcessParsedFile(pkg, i) != 0) {
            return -1;
        }
    }

    if (ValidatePubFnDefinitions(pkg) != 0) {
        return -1;
    }
    if (ResolvePackageImportsAndSelectors(loader, pkg) != 0) {
        return -1;
    }
    *outPkg = pkg;
    return 0;
}

static int LoadSingleFilePackage(
    SLPackageLoader* loader, const char* filePath, SLPackage** outPkg) {
    char*      dirPath = DirNameDup(filePath);
    SLPackage* pkg;
    char*      source = NULL;
    uint32_t   sourceLen = 0;
    SLAst      ast;
    void*      arenaMem = NULL;
    uint32_t   i;

    if (dirPath == NULL) {
        return ErrorSimple("out of memory");
    }
    if (AddPackageSlot(loader, dirPath, &pkg) != 0) {
        free(dirPath);
        return ErrorSimple("out of memory");
    }
    free(dirPath);

    free(pkg->name);
    pkg->name = InferPackageNameFromSingleFile(filePath);
    if (pkg->name == NULL) {
        return ErrorSimple("failed to infer package name from %s", filePath);
    }

    pkg->loadState = 1;
    if (ReadFile(filePath, &source, &sourceLen) != 0) {
        return -1;
    }
    if (ParseSourceEx(filePath, source, sourceLen, &ast, &arenaMem, NULL, 1) != 0) {
        free(source);
        return -1;
    }
    if (AddPackageFile(pkg, filePath, source, sourceLen, ast, arenaMem) != 0) {
        free(source);
        free(arenaMem);
        return ErrorSimple("out of memory");
    }

    for (i = 0; i < pkg->fileLen; i++) {
        if (ProcessParsedFile(pkg, i) != 0) {
            return -1;
        }
    }

    if (ValidatePubFnDefinitions(pkg) != 0) {
        return -1;
    }
    if (ResolvePackageImportsAndSelectors(loader, pkg) != 0) {
        return -1;
    }
    *outPkg = pkg;
    return 0;
}

static const char* _Nullable FindIdentReplacement(
    const SLIdentMap* _Nullable maps,
    uint32_t    mapLen,
    const char* src,
    uint32_t    start,
    uint32_t    end) {
    uint32_t i;
    if (maps == NULL || mapLen == 0) {
        return NULL;
    }
    for (i = 0; i < mapLen; i++) {
        size_t len = strlen(maps[i].name);
        if (len == (size_t)(end - start) && memcmp(maps[i].name, src + start, len) == 0) {
            return maps[i].replacement;
        }
    }
    return NULL;
}

typedef struct {
    uint32_t    start;
    uint32_t    end;
    const char* replacement;
} SLTextRewrite;

static int AddTextRewrite(
    SLTextRewrite** rewrites,
    uint32_t*       len,
    uint32_t*       cap,
    uint32_t        start,
    uint32_t        end,
    const char*     replacement) {
    if (EnsureCap((void**)rewrites, cap, *len + 1u, sizeof(SLTextRewrite)) != 0) {
        return -1;
    }
    (*rewrites)[*len].start = start;
    (*rewrites)[*len].end = end;
    (*rewrites)[*len].replacement = replacement;
    (*len)++;
    return 0;
}

static int FindImportSymbolBindingIndexBySlice(
    const SLPackage* pkg, const char* src, uint32_t start, uint32_t end, int wantType) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                   nameLen;
        if ((sym->isType ? 1 : 0) != (wantType ? 1 : 0)) {
            continue;
        }
        if (!wantType && sym->useWrapper) {
            continue;
        }
        nameLen = strlen(sym->localName);
        if (nameLen == (size_t)(end - start) && memcmp(sym->localName, src + start, nameLen) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int CollectTypeNameImportRewritesNode(
    const SLPackage*    pkg,
    const SLParsedFile* file,
    int32_t             nodeId,
    SLTextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap) {
    const SLAstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    if (n->kind == SLAst_TYPE_NAME) {
        uint32_t dot = FindSliceDot(file->source, n->dataStart, n->dataEnd);
        if (dot >= n->dataEnd) {
            int idx = FindImportSymbolBindingIndexBySlice(
                pkg, file->source, n->dataStart, n->dataEnd, 1);
            if (idx >= 0) {
                if (AddTextRewrite(
                        rewrites,
                        rewriteLen,
                        rewriteCap,
                        n->dataStart,
                        n->dataEnd,
                        pkg->importSymbols[(uint32_t)idx].qualifiedName)
                    != 0)
                {
                    return -1;
                }
            }
        } else {
            int idx = FindImportSymbolBindingIndexBySlice(pkg, file->source, n->dataStart, dot, 1);
            if (idx >= 0) {
                if (AddTextRewrite(
                        rewrites,
                        rewriteLen,
                        rewriteCap,
                        n->dataStart,
                        dot,
                        pkg->importSymbols[(uint32_t)idx].qualifiedName)
                    != 0)
                {
                    return -1;
                }
            }
        }
    }
    child = ASTFirstChild(&file->ast, nodeId);
    while (child >= 0) {
        if (CollectTypeNameImportRewritesNode(pkg, file, child, rewrites, rewriteLen, rewriteCap)
            != 0)
        {
            return -1;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    return 0;
}

static int CollectExprImportRewritesNode(
    const SLPackage*    pkg,
    const SLParsedFile* file,
    int32_t             nodeId,
    uint8_t*            shadowCounts,
    SLTextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap) {
    const SLAstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    switch (n->kind) {
        case SLAst_IDENT: {
            int idx = FindImportSymbolBindingIndexBySlice(
                pkg, file->source, n->dataStart, n->dataEnd, 0);
            if (idx >= 0 && shadowCounts[(uint32_t)idx] == 0) {
                if (AddTextRewrite(
                        rewrites,
                        rewriteLen,
                        rewriteCap,
                        n->dataStart,
                        n->dataEnd,
                        pkg->importSymbols[(uint32_t)idx].qualifiedName)
                    != 0)
                {
                    return -1;
                }
            }
            return 0;
        }
        case SLAst_CALL:
        case SLAst_CALL_WITH_CONTEXT: {
            int32_t callee = ASTFirstChild(&file->ast, nodeId);
            if (callee >= 0 && (uint32_t)callee < file->ast.len
                && file->ast.nodes[callee].kind == SLAst_FIELD_EXPR)
            {
                int32_t            recv = ASTFirstChild(&file->ast, callee);
                const SLAstNode*   fieldExpr = &file->ast.nodes[callee];
                const SLImportRef* imp = NULL;
                if (recv >= 0 && (uint32_t)recv < file->ast.len
                    && file->ast.nodes[recv].kind == SLAst_IDENT)
                {
                    const SLAstNode* recvNode = &file->ast.nodes[recv];
                    imp = FindImportByAliasSlice(
                        pkg, file->source, recvNode->dataStart, recvNode->dataEnd);
                }
                if (imp == NULL) {
                    int idx = FindImportSymbolBindingIndexBySlice(
                        pkg, file->source, fieldExpr->dataStart, fieldExpr->dataEnd, 0);
                    if (idx >= 0 && shadowCounts[(uint32_t)idx] == 0) {
                        if (AddTextRewrite(
                                rewrites,
                                rewriteLen,
                                rewriteCap,
                                fieldExpr->dataStart,
                                fieldExpr->dataEnd,
                                pkg->importSymbols[(uint32_t)idx].qualifiedName)
                            != 0)
                        {
                            return -1;
                        }
                    }
                }
            }
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                if (CollectExprImportRewritesNode(
                        pkg, file, child, shadowCounts, rewrites, rewriteLen, rewriteCap)
                    != 0)
                {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        case SLAst_UNARY:
        case SLAst_BINARY:
        case SLAst_CONTEXT_OVERLAY:
        case SLAst_CONTEXT_BIND:
        case SLAst_INDEX:
        case SLAst_CAST:
        case SLAst_SIZEOF:
        case SLAst_NEW:
        case SLAst_UNWRAP:
        case SLAst_CALL_ARG:
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                if (CollectExprImportRewritesNode(
                        pkg, file, child, shadowCounts, rewrites, rewriteLen, rewriteCap)
                    != 0)
                {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        case SLAst_FIELD_EXPR: {
            int32_t recv = ASTFirstChild(&file->ast, nodeId);
            if (recv >= 0 && (uint32_t)recv < file->ast.len
                && file->ast.nodes[recv].kind == SLAst_IDENT)
            {
                const SLAstNode* recvNode = &file->ast.nodes[recv];
                int              idx = FindImportSymbolBindingIndexBySlice(
                    pkg, file->source, recvNode->dataStart, recvNode->dataEnd, 1);
                if (idx >= 0 && shadowCounts[(uint32_t)idx] == 0) {
                    if (AddTextRewrite(
                            rewrites,
                            rewriteLen,
                            rewriteCap,
                            recvNode->dataStart,
                            recvNode->dataEnd,
                            pkg->importSymbols[(uint32_t)idx].qualifiedName)
                        != 0)
                    {
                        return -1;
                    }
                }
            }
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                if (CollectExprImportRewritesNode(
                        pkg, file, child, shadowCounts, rewrites, rewriteLen, rewriteCap)
                    != 0)
                {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        default: return 0;
    }
}

static int PushShadowIfValueImportName(
    const SLPackage*    pkg,
    const SLParsedFile* file,
    uint32_t            start,
    uint32_t            end,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                   nameLen = strlen(sym->localName);
        if (nameLen != (size_t)(end - start)
            || memcmp(sym->localName, file->source + start, nameLen) != 0)
        {
            continue;
        }
        if (EnsureCap((void**)shadowStack, shadowCap, *shadowLen + 1u, sizeof(uint32_t)) != 0) {
            return -1;
        }
        shadowCounts[i]++;
        (*shadowStack)[(*shadowLen)++] = i;
    }
    return 0;
}

static void PopShadowToMark(
    uint8_t* shadowCounts, uint32_t* _Nullable shadowStack, uint32_t* shadowLen, uint32_t mark) {
    if (shadowCounts == NULL || shadowLen == NULL) {
        return;
    }
    while (*shadowLen > mark) {
        if (shadowStack == NULL) {
            return;
        }
        uint32_t idx = shadowStack[--(*shadowLen)];
        if (shadowCounts[idx] > 0) {
            shadowCounts[idx]--;
        }
    }
}

static int CollectStmtImportRewritesNode(
    const SLPackage*    pkg,
    const SLParsedFile* file,
    int32_t             nodeId,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap,
    SLTextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap);

static int CollectBlockImportRewritesNode(
    const SLPackage*    pkg,
    const SLParsedFile* file,
    int32_t             blockNodeId,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap,
    SLTextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap) {
    uint32_t mark = *shadowLen;
    int32_t  child = ASTFirstChild(&file->ast, blockNodeId);
    while (child >= 0) {
        if (CollectStmtImportRewritesNode(
                pkg,
                file,
                child,
                shadowCounts,
                shadowStack,
                shadowLen,
                shadowCap,
                rewrites,
                rewriteLen,
                rewriteCap)
            != 0)
        {
            PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
            return -1;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
    return 0;
}

static int32_t VarLikeInitNode(const SLParsedFile* file, int32_t varLikeNodeId) {
    int32_t firstChild = ASTFirstChild(&file->ast, varLikeNodeId);
    int32_t afterNames;
    if (firstChild < 0) {
        return -1;
    }
    if (file->ast.nodes[firstChild].kind == SLAst_NAME_LIST) {
        afterNames = ASTNextSibling(&file->ast, firstChild);
        if (afterNames >= 0 && IsFnReturnTypeNodeKind(file->ast.nodes[afterNames].kind)) {
            return ASTNextSibling(&file->ast, afterNames);
        }
        return afterNames;
    }
    if (IsFnReturnTypeNodeKind(file->ast.nodes[firstChild].kind)) {
        return ASTNextSibling(&file->ast, firstChild);
    }
    return firstChild;
}

static uint32_t AstListCount(const SLAst* ast, int32_t listNode) {
    uint32_t count = 0;
    int32_t  child;
    if (listNode < 0 || (uint32_t)listNode >= ast->len) {
        return 0;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        count++;
        child = ast->nodes[child].nextSibling;
    }
    return count;
}

static int32_t AstListItemAt(const SLAst* ast, int32_t listNode, uint32_t index) {
    uint32_t i = 0;
    int32_t  child;
    if (listNode < 0 || (uint32_t)listNode >= ast->len) {
        return -1;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        if (i == index) {
            return child;
        }
        i++;
        child = ast->nodes[child].nextSibling;
    }
    return -1;
}

static int CollectStmtImportRewritesNode(
    const SLPackage*    pkg,
    const SLParsedFile* file,
    int32_t             nodeId,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap,
    SLTextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap) {
    const SLAstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    switch (n->kind) {
        case SLAst_BLOCK:
            return CollectBlockImportRewritesNode(
                pkg,
                file,
                nodeId,
                shadowCounts,
                shadowStack,
                shadowLen,
                shadowCap,
                rewrites,
                rewriteLen,
                rewriteCap);
        case SLAst_VAR:
        case SLAst_CONST: {
            int32_t initNode = VarLikeInitNode(file, nodeId);
            int32_t firstChild = ASTFirstChild(&file->ast, nodeId);
            if (firstChild >= 0 && file->ast.nodes[firstChild].kind == SLAst_NAME_LIST) {
                uint32_t i;
                uint32_t nameCount = AstListCount(&file->ast, firstChild);
                if (initNode >= 0) {
                    if (file->ast.nodes[initNode].kind == SLAst_EXPR_LIST) {
                        uint32_t initCount = AstListCount(&file->ast, initNode);
                        for (i = 0; i < initCount; i++) {
                            int32_t exprNode = AstListItemAt(&file->ast, initNode, i);
                            if (exprNode >= 0
                                && CollectExprImportRewritesNode(
                                       pkg,
                                       file,
                                       exprNode,
                                       shadowCounts,
                                       rewrites,
                                       rewriteLen,
                                       rewriteCap)
                                       != 0)
                            {
                                return -1;
                            }
                        }
                    } else if (
                        CollectExprImportRewritesNode(
                            pkg, file, initNode, shadowCounts, rewrites, rewriteLen, rewriteCap)
                        != 0)
                    {
                        return -1;
                    }
                }
                for (i = 0; i < nameCount; i++) {
                    int32_t          nameNode = AstListItemAt(&file->ast, firstChild, i);
                    const SLAstNode* name = nameNode >= 0 ? &file->ast.nodes[nameNode] : NULL;
                    if (name == NULL) {
                        continue;
                    }
                    if (PushShadowIfValueImportName(
                            pkg,
                            file,
                            name->dataStart,
                            name->dataEnd,
                            shadowCounts,
                            shadowStack,
                            shadowLen,
                            shadowCap)
                        != 0)
                    {
                        return -1;
                    }
                }
                return 0;
            }
            if (initNode >= 0
                && CollectExprImportRewritesNode(
                       pkg, file, initNode, shadowCounts, rewrites, rewriteLen, rewriteCap)
                       != 0)
            {
                return -1;
            }
            return PushShadowIfValueImportName(
                pkg,
                file,
                n->dataStart,
                n->dataEnd,
                shadowCounts,
                shadowStack,
                shadowLen,
                shadowCap);
        }
        case SLAst_IF: {
            int32_t cond = ASTFirstChild(&file->ast, nodeId);
            int32_t thenNode = cond >= 0 ? ASTNextSibling(&file->ast, cond) : -1;
            int32_t elseNode = thenNode >= 0 ? ASTNextSibling(&file->ast, thenNode) : -1;
            if (cond >= 0
                && CollectExprImportRewritesNode(
                       pkg, file, cond, shadowCounts, rewrites, rewriteLen, rewriteCap)
                       != 0)
            {
                return -1;
            }
            if (thenNode >= 0
                && CollectStmtImportRewritesNode(
                       pkg,
                       file,
                       thenNode,
                       shadowCounts,
                       shadowStack,
                       shadowLen,
                       shadowCap,
                       rewrites,
                       rewriteLen,
                       rewriteCap)
                       != 0)
            {
                return -1;
            }
            if (elseNode >= 0
                && CollectStmtImportRewritesNode(
                       pkg,
                       file,
                       elseNode,
                       shadowCounts,
                       shadowStack,
                       shadowLen,
                       shadowCap,
                       rewrites,
                       rewriteLen,
                       rewriteCap)
                       != 0)
            {
                return -1;
            }
            return 0;
        }
        case SLAst_FOR: {
            int32_t          parts[4];
            uint32_t         partCount = 0;
            uint32_t         mark = *shadowLen;
            const SLAstNode* forNode = &file->ast.nodes[nodeId];
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0 && partCount < 4u) {
                parts[partCount++] = child;
                child = ASTNextSibling(&file->ast, child);
            }
            if ((forNode->flags & SLAstFlag_FOR_IN) != 0) {
                int      hasKey = (forNode->flags & SLAstFlag_FOR_IN_HAS_KEY) != 0;
                int32_t  keyNode = -1;
                int32_t  valueNode = -1;
                int32_t  sourceNode = -1;
                int32_t  bodyNode = -1;
                uint32_t bodyMark;
                if ((!hasKey && partCount != 3u) || (hasKey && partCount != 4u)) {
                    PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                    return 0;
                }
                if (hasKey) {
                    keyNode = parts[0];
                    valueNode = parts[1];
                    sourceNode = parts[2];
                    bodyNode = parts[3];
                } else {
                    valueNode = parts[0];
                    sourceNode = parts[1];
                    bodyNode = parts[2];
                }
                if (sourceNode >= 0
                    && CollectExprImportRewritesNode(
                           pkg, file, sourceNode, shadowCounts, rewrites, rewriteLen, rewriteCap)
                           != 0)
                {
                    PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                    return -1;
                }
                bodyMark = *shadowLen;
                if (hasKey && keyNode >= 0) {
                    const SLAstNode* key = &file->ast.nodes[keyNode];
                    if (PushShadowIfValueImportName(
                            pkg,
                            file,
                            key->dataStart,
                            key->dataEnd,
                            shadowCounts,
                            shadowStack,
                            shadowLen,
                            shadowCap)
                        != 0)
                    {
                        PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                        return -1;
                    }
                }
                if (valueNode >= 0 && (forNode->flags & SLAstFlag_FOR_IN_VALUE_DISCARD) == 0) {
                    const SLAstNode* value = &file->ast.nodes[valueNode];
                    if (PushShadowIfValueImportName(
                            pkg,
                            file,
                            value->dataStart,
                            value->dataEnd,
                            shadowCounts,
                            shadowStack,
                            shadowLen,
                            shadowCap)
                        != 0)
                    {
                        PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                        return -1;
                    }
                }
                if (bodyNode >= 0 && file->ast.nodes[bodyNode].kind == SLAst_BLOCK) {
                    if (CollectBlockImportRewritesNode(
                            pkg,
                            file,
                            bodyNode,
                            shadowCounts,
                            shadowStack,
                            shadowLen,
                            shadowCap,
                            rewrites,
                            rewriteLen,
                            rewriteCap)
                        != 0)
                    {
                        PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                        return -1;
                    }
                }
                PopShadowToMark(shadowCounts, *shadowStack, shadowLen, bodyMark);
                PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                return 0;
            }
            if (partCount > 0) {
                uint32_t last = partCount - 1u;
                uint32_t idx = 0;
                if (partCount >= 2u && file->ast.nodes[parts[0]].kind == SLAst_VAR) {
                    int32_t initNode = VarLikeInitNode(file, parts[0]);
                    int32_t firstChild = ASTFirstChild(&file->ast, parts[0]);
                    if (firstChild >= 0 && file->ast.nodes[firstChild].kind == SLAst_NAME_LIST) {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(&file->ast, firstChild);
                        if (initNode >= 0) {
                            if (file->ast.nodes[initNode].kind == SLAst_EXPR_LIST) {
                                uint32_t initCount = AstListCount(&file->ast, initNode);
                                for (i = 0; i < initCount; i++) {
                                    int32_t exprNode = AstListItemAt(&file->ast, initNode, i);
                                    if (exprNode >= 0
                                        && CollectExprImportRewritesNode(
                                               pkg,
                                               file,
                                               exprNode,
                                               shadowCounts,
                                               rewrites,
                                               rewriteLen,
                                               rewriteCap)
                                               != 0)
                                    {
                                        PopShadowToMark(
                                            shadowCounts, *shadowStack, shadowLen, mark);
                                        return -1;
                                    }
                                }
                            } else if (
                                CollectExprImportRewritesNode(
                                    pkg,
                                    file,
                                    initNode,
                                    shadowCounts,
                                    rewrites,
                                    rewriteLen,
                                    rewriteCap)
                                != 0)
                            {
                                PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                                return -1;
                            }
                        }
                        for (i = 0; i < nameCount; i++) {
                            int32_t          nameNode = AstListItemAt(&file->ast, firstChild, i);
                            const SLAstNode* name =
                                nameNode >= 0 ? &file->ast.nodes[nameNode] : NULL;
                            if (name == NULL) {
                                continue;
                            }
                            if (PushShadowIfValueImportName(
                                    pkg,
                                    file,
                                    name->dataStart,
                                    name->dataEnd,
                                    shadowCounts,
                                    shadowStack,
                                    shadowLen,
                                    shadowCap)
                                != 0)
                            {
                                PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                                return -1;
                            }
                        }
                    } else {
                        if (initNode >= 0
                            && CollectExprImportRewritesNode(
                                   pkg,
                                   file,
                                   initNode,
                                   shadowCounts,
                                   rewrites,
                                   rewriteLen,
                                   rewriteCap)
                                   != 0)
                        {
                            PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                            return -1;
                        }
                        if (PushShadowIfValueImportName(
                                pkg,
                                file,
                                file->ast.nodes[parts[0]].dataStart,
                                file->ast.nodes[parts[0]].dataEnd,
                                shadowCounts,
                                shadowStack,
                                shadowLen,
                                shadowCap)
                            != 0)
                        {
                            PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                            return -1;
                        }
                    }
                    idx = 1;
                } else if (partCount >= 2u && file->ast.nodes[parts[0]].kind != SLAst_BLOCK) {
                    if (CollectExprImportRewritesNode(
                            pkg, file, parts[0], shadowCounts, rewrites, rewriteLen, rewriteCap)
                        != 0)
                    {
                        PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                        return -1;
                    }
                    idx = 1;
                }
                while (idx < last) {
                    if (file->ast.nodes[parts[idx]].kind != SLAst_BLOCK
                        && CollectExprImportRewritesNode(
                               pkg,
                               file,
                               parts[idx],
                               shadowCounts,
                               rewrites,
                               rewriteLen,
                               rewriteCap)
                               != 0)
                    {
                        PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                        return -1;
                    }
                    idx++;
                }
                if (file->ast.nodes[parts[last]].kind == SLAst_BLOCK) {
                    if (CollectBlockImportRewritesNode(
                            pkg,
                            file,
                            parts[last],
                            shadowCounts,
                            shadowStack,
                            shadowLen,
                            shadowCap,
                            rewrites,
                            rewriteLen,
                            rewriteCap)
                        != 0)
                    {
                        PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
                        return -1;
                    }
                }
            }
            PopShadowToMark(shadowCounts, *shadowStack, shadowLen, mark);
            return 0;
        }
        case SLAst_SWITCH: {
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const SLAstNode* c = &file->ast.nodes[child];
                if (c->kind == SLAst_CASE) {
                    int32_t k = ASTFirstChild(&file->ast, child);
                    int32_t last = -1;
                    while (k >= 0) {
                        last = k;
                        k = ASTNextSibling(&file->ast, k);
                    }
                    k = ASTFirstChild(&file->ast, child);
                    while (k >= 0) {
                        if (k != last
                            && CollectExprImportRewritesNode(
                                   pkg, file, k, shadowCounts, rewrites, rewriteLen, rewriteCap)
                                   != 0)
                        {
                            return -1;
                        }
                        if (k == last) {
                            break;
                        }
                        k = ASTNextSibling(&file->ast, k);
                    }
                    if (last >= 0
                        && CollectStmtImportRewritesNode(
                               pkg,
                               file,
                               last,
                               shadowCounts,
                               shadowStack,
                               shadowLen,
                               shadowCap,
                               rewrites,
                               rewriteLen,
                               rewriteCap)
                               != 0)
                    {
                        return -1;
                    }
                } else if (c->kind == SLAst_DEFAULT) {
                    int32_t blk = ASTFirstChild(&file->ast, child);
                    if (blk >= 0
                        && CollectStmtImportRewritesNode(
                               pkg,
                               file,
                               blk,
                               shadowCounts,
                               shadowStack,
                               shadowLen,
                               shadowCap,
                               rewrites,
                               rewriteLen,
                               rewriteCap)
                               != 0)
                    {
                        return -1;
                    }
                } else {
                    if (CollectExprImportRewritesNode(
                            pkg, file, child, shadowCounts, rewrites, rewriteLen, rewriteCap)
                        != 0)
                    {
                        return -1;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        case SLAst_RETURN:
        case SLAst_ASSERT:
        case SLAst_EXPR_STMT: {
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                if (CollectExprImportRewritesNode(
                        pkg, file, child, shadowCounts, rewrites, rewriteLen, rewriteCap)
                    != 0)
                {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        case SLAst_DEFER: {
            int32_t d = ASTFirstChild(&file->ast, nodeId);
            if (d >= 0) {
                return CollectStmtImportRewritesNode(
                    pkg,
                    file,
                    d,
                    shadowCounts,
                    shadowStack,
                    shadowLen,
                    shadowCap,
                    rewrites,
                    rewriteLen,
                    rewriteCap);
            }
            return 0;
        }
        default: return 0;
    }
}

static int CompareTextRewrite(const void* a, const void* b) {
    const SLTextRewrite* ra = (const SLTextRewrite*)a;
    const SLTextRewrite* rb = (const SLTextRewrite*)b;
    if (ra->start < rb->start) {
        return -1;
    }
    if (ra->start > rb->start) {
        return 1;
    }
    if (ra->end < rb->end) {
        return -1;
    }
    if (ra->end > rb->end) {
        return 1;
    }
    return 0;
}

static int ApplyTextRewrites(
    const char* text,
    uint32_t    textLen,
    uint32_t    baseStart,
    SLTextRewrite* _Nullable rewrites,
    uint32_t rewriteLen,
    char**   outText) {
    SLStringBuilder b = { 0 };
    uint32_t        i;
    uint32_t        copyPos = 0;
    *outText = NULL;

    if (rewriteLen == 0) {
        *outText = DupSlice(text, 0, textLen);
        return *outText == NULL ? -1 : 0;
    }
    qsort(rewrites, rewriteLen, sizeof(SLTextRewrite), CompareTextRewrite);

    for (i = 0; i < rewriteLen; i++) {
        uint32_t relStart;
        uint32_t relEnd;
        if (rewrites[i].start < baseStart || rewrites[i].end < rewrites[i].start
            || rewrites[i].end > baseStart + textLen)
        {
            free(b.v);
            return -1;
        }
        relStart = rewrites[i].start - baseStart;
        relEnd = rewrites[i].end - baseStart;
        if (relStart < copyPos) {
            continue;
        }
        if (SBAppendSlice(&b, text, copyPos, relStart) != 0
            || SBAppendCStr(&b, rewrites[i].replacement) != 0)
        {
            free(b.v);
            return -1;
        }
        copyPos = relEnd;
    }
    if (SBAppendSlice(&b, text, copyPos, textLen) != 0) {
        free(b.v);
        return -1;
    }
    *outText = SBFinish(&b, NULL);
    return *outText == NULL ? -1 : 0;
}

static int RewriteDeclTextForNamedImports(
    const SLPackage*    pkg,
    const SLParsedFile* file,
    int32_t             nodeId,
    const char*         text,
    char**              outText) {
    const SLAstNode* n;
    SLTextRewrite*   rewrites = NULL;
    uint32_t         rewriteLen = 0;
    uint32_t         rewriteCap = 0;
    uint8_t*         shadowCounts = NULL;
    uint32_t*        shadowStack = NULL;
    uint32_t         shadowLen = 0;
    uint32_t         shadowCap = 0;
    uint32_t         mark = 0;
    int32_t          child;
    int              rc = -1;

    *outText = NULL;
    if (pkg->importSymbolLen == 0) {
        *outText = DupCStr(text);
        return *outText == NULL ? -1 : 0;
    }
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return -1;
    }
    n = &file->ast.nodes[nodeId];

    shadowCounts = (uint8_t*)calloc(pkg->importSymbolLen, sizeof(uint8_t));
    if (shadowCounts == NULL) {
        goto done;
    }

    if (CollectTypeNameImportRewritesNode(pkg, file, nodeId, &rewrites, &rewriteLen, &rewriteCap)
        != 0)
    {
        goto done;
    }

    switch (n->kind) {
        case SLAst_FN:
            mark = shadowLen;
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const SLAstNode* ch = &file->ast.nodes[child];
                if (ch->kind == SLAst_PARAM) {
                    if (PushShadowIfValueImportName(
                            pkg,
                            file,
                            ch->dataStart,
                            ch->dataEnd,
                            shadowCounts,
                            &shadowStack,
                            &shadowLen,
                            &shadowCap)
                        != 0)
                    {
                        goto done;
                    }
                } else if (ch->kind == SLAst_BLOCK) {
                    if (CollectBlockImportRewritesNode(
                            pkg,
                            file,
                            child,
                            shadowCounts,
                            &shadowStack,
                            &shadowLen,
                            &shadowCap,
                            &rewrites,
                            &rewriteLen,
                            &rewriteCap)
                        != 0)
                    {
                        goto done;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
            PopShadowToMark(shadowCounts, shadowStack, &shadowLen, mark);
            break;
        case SLAst_CONST: {
            int32_t initNode = VarLikeInitNode(file, nodeId);
            if (initNode >= 0
                && CollectExprImportRewritesNode(
                       pkg, file, initNode, shadowCounts, &rewrites, &rewriteLen, &rewriteCap)
                       != 0)
            {
                goto done;
            }
            break;
        }
        case SLAst_ENUM: {
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const SLAstNode* c = &file->ast.nodes[child];
                if (c->kind == SLAst_FIELD) {
                    int32_t expr = ASTFirstChild(&file->ast, child);
                    if (expr >= 0
                        && CollectExprImportRewritesNode(
                               pkg, file, expr, shadowCounts, &rewrites, &rewriteLen, &rewriteCap)
                               != 0)
                    {
                        goto done;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
            break;
        }
        default: break;
    }

    if (ApplyTextRewrites(
            text,
            (uint32_t)strlen(text),
            file->ast.nodes[nodeId].start,
            rewrites,
            rewriteLen,
            outText)
        != 0)
    {
        goto done;
    }
    rc = 0;

done:
    free(rewrites);
    free(shadowCounts);
    free(shadowStack);
    return rc;
}

static int RewriteText(
    const char* src,
    uint32_t    srcLen,
    const SLImportRef* _Nullable imports,
    uint32_t importLen,
    const SLIdentMap* _Nullable maps,
    uint32_t mapLen,
    char**   outText) {
    void*           arenaMem = NULL;
    size_t          arenaCap;
    uint64_t        arenaCap64;
    SLArena         arena;
    SLTokenStream   stream;
    SLDiag          diag = { 0 };
    SLStringBuilder b = { 0 };
    uint32_t        i;
    uint32_t        copyPos = 0;

    *outText = NULL;
    if ((importLen > 0 && imports == NULL) || (mapLen > 0 && maps == NULL)) {
        return ErrorSimple("internal error: missing rewrite mappings");
    }
    arenaCap64 = (uint64_t)(srcLen + 16u) * (uint64_t)sizeof(SLToken) + 4096u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        return ErrorSimple("arena too large");
    }
    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        return ErrorSimple("out of memory");
    }

    SLArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (SLLex(&arena, (SLStrView){ src, srcLen }, &stream, &diag) != 0) {
        free(arenaMem);
        return ErrorSimple("rewrite lex failed");
    }

    for (i = 0; i < stream.len; i++) {
        const SLToken* t = &stream.v[i];
        if (t->kind == SLTok_EOF) {
            break;
        }

        if (importLen > 0 && i + 2u < stream.len && t->kind == SLTok_IDENT
            && stream.v[i + 1u].kind == SLTok_DOT && stream.v[i + 2u].kind == SLTok_IDENT)
        {
            uint32_t j;
            for (j = 0; j < importLen; j++) {
                if (imports[j].bindName != NULL
                    && strlen(imports[j].bindName) == (size_t)(t->end - t->start)
                    && memcmp(imports[j].bindName, src + t->start, (size_t)(t->end - t->start)) == 0
                    && PackageHasExportSlice(
                        imports[j].target, src, stream.v[i + 2u].start, stream.v[i + 2u].end))
                {
                    if (SBAppendSlice(&b, src, copyPos, t->start) != 0
                        || SBAppendCStr(&b, imports[j].alias) != 0 || SBAppendCStr(&b, "__") != 0
                        || SBAppendSlice(&b, src, stream.v[i + 2u].start, stream.v[i + 2u].end)
                               != 0)
                    {
                        free(b.v);
                        free(arenaMem);
                        return ErrorSimple("out of memory");
                    }
                    copyPos = stream.v[i + 2u].end;
                    i += 2u;
                    t = NULL;
                    break;
                }
            }
            if (t == NULL) {
                continue;
            }
        }

        if (SBAppendSlice(&b, src, copyPos, t->start) != 0) {
            free(b.v);
            free(arenaMem);
            return ErrorSimple("out of memory");
        }
        if (t->kind == SLTok_IDENT && mapLen > 0) {
            const char* repl = FindIdentReplacement(maps, mapLen, src, t->start, t->end);
            if (repl != NULL) {
                if (SBAppendCStr(&b, repl) != 0) {
                    free(b.v);
                    free(arenaMem);
                    return ErrorSimple("out of memory");
                }
                copyPos = t->end;
                continue;
            }
        }
        if (SBAppendSlice(&b, src, t->start, t->end) != 0) {
            free(b.v);
            free(arenaMem);
            return ErrorSimple("out of memory");
        }
        copyPos = t->end;
    }

    if (SBAppendSlice(&b, src, copyPos, srcLen) != 0) {
        free(b.v);
        free(arenaMem);
        return ErrorSimple("out of memory");
    }

    *outText = SBFinish(&b, NULL);
    free(arenaMem);
    if (*outText == NULL) {
        return ErrorSimple("out of memory");
    }
    return 0;
}

static int BuildPrefixedName(const char* alias, const char* name, char** outName) {
    SLStringBuilder b = { 0 };
    *outName = NULL;
    if (SBAppendCStr(&b, alias) != 0 || SBAppendCStr(&b, "__") != 0 || SBAppendCStr(&b, name) != 0)
    {
        free(b.v);
        return -1;
    }
    *outName = SBFinish(&b, NULL);
    return *outName == NULL ? -1 : 0;
}

static int RewriteAliasedPubDeclText(
    const SLPackage* sourcePkg, const SLSymbolDecl* pubDecl, const char* alias, char** outText) {
    SLIdentMap* maps = NULL;
    uint32_t    i;
    int         rc = -1;
    *outText = NULL;
    if (sourcePkg->pubDeclLen == 0) {
        return ErrorSimple("internal error: empty public declaration set");
    }
    maps = (SLIdentMap*)calloc(sourcePkg->pubDeclLen, sizeof(SLIdentMap));
    if (maps == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < sourcePkg->pubDeclLen; i++) {
        maps[i].name = sourcePkg->pubDecls[i].name;
        maps[i].replacement = NULL;
        if (BuildPrefixedName(alias, sourcePkg->pubDecls[i].name, (char**)&maps[i].replacement)
            != 0)
        {
            goto done;
        }
    }
    rc = RewriteText(
        pubDecl->declText,
        (uint32_t)strlen(pubDecl->declText),
        sourcePkg->imports,
        sourcePkg->importLen,
        maps,
        sourcePkg->pubDeclLen,
        outText);

done:
    if (maps != NULL) {
        for (i = 0; i < sourcePkg->pubDeclLen; i++) {
            free((void*)maps[i].replacement);
        }
    }
    free(maps);
    return rc;
}

static int BuildFnImportShapeAndWrapper(
    const char* _Nullable aliasedDeclText,
    const char* _Nullable localName,
    const char* _Nullable qualifiedName,
    char** _Nullable outShapeKey,
    char** _Nullable outWrapperDeclText) {
    SLAst           ast = { 0 };
    void*           arenaMem = NULL;
    int32_t         fnNode = -1;
    int32_t         child;
    int32_t         returnTypeNode = -1;
    int32_t         contextTypeNode = -1;
    SLStringBuilder shape = { 0 };
    SLStringBuilder wrapper = { 0 };
    SLStringBuilder callArgs = { 0 };
    uint32_t        paramIndex = 0;

    if (aliasedDeclText == NULL || localName == NULL || qualifiedName == NULL || outShapeKey == NULL
        || outWrapperDeclText == NULL)
    {
        return ErrorSimple("internal error: invalid generated import wrapper input");
    }
    *outShapeKey = NULL;
    *outWrapperDeclText = NULL;

    if (ParseSourceEx(
            "<generated-import-fn>",
            aliasedDeclText,
            (uint32_t)strlen(aliasedDeclText),
            &ast,
            &arenaMem,
            NULL,
            0)
        != 0)
    {
        return ErrorSimple("internal error: failed to parse rewritten import function declaration");
    }

    fnNode = ASTFirstChild(&ast, ast.root);
    if (fnNode < 0 || (uint32_t)fnNode >= ast.len || ast.nodes[fnNode].kind != SLAst_FN) {
        free(arenaMem);
        return ErrorSimple("internal error: expected function declaration in rewritten import");
    }

    if (SBAppendCStr(&shape, "ctx:") != 0) {
        free(arenaMem);
        goto oom;
    }
    child = ASTFirstChild(&ast, fnNode);
    while (child >= 0) {
        const SLAstNode* n = &ast.nodes[child];
        if (n->kind == SLAst_PARAM) {
            /* handled later */
        } else if (n->kind == SLAst_CONTEXT_CLAUSE) {
            contextTypeNode = ASTFirstChild(&ast, child);
        } else if (IsFnReturnTypeNodeKind(n->kind) && n->flags == 1u) {
            returnTypeNode = child;
        }
        child = ASTNextSibling(&ast, child);
    }
    if (contextTypeNode >= 0) {
        if (SBAppendSlice(
                &shape,
                aliasedDeclText,
                ast.nodes[contextTypeNode].start,
                ast.nodes[contextTypeNode].end)
            != 0)
        {
            free(arenaMem);
            goto oom;
        }
    } else if (SBAppendCStr(&shape, "-") != 0) {
        free(arenaMem);
        goto oom;
    }
    if (SBAppendCStr(&shape, "|params:") != 0) {
        free(arenaMem);
        goto oom;
    }

    if (SBAppendCStr(&wrapper, "fn ") != 0 || SBAppendCStr(&wrapper, localName) != 0
        || SBAppendCStr(&wrapper, "(") != 0)
    {
        free(arenaMem);
        goto oom;
    }

    child = ASTFirstChild(&ast, fnNode);
    while (child >= 0) {
        const SLAstNode* n = &ast.nodes[child];
        if (n->kind == SLAst_PARAM) {
            int32_t     typeNode = ASTFirstChild(&ast, child);
            uint32_t    nameStart = n->dataStart;
            uint32_t    nameEnd = n->dataEnd;
            const char* tempName = NULL;
            char        tempNameBuf[32];
            if (typeNode < 0 || (uint32_t)typeNode >= ast.len) {
                free(arenaMem);
                return ErrorSimple(
                    "internal error: malformed function parameter in rewritten import");
            }
            if (paramIndex > 0) {
                if (SBAppendCStr(&shape, ";") != 0 || SBAppendCStr(&wrapper, ", ") != 0
                    || SBAppendCStr(&callArgs, ", ") != 0)
                {
                    free(arenaMem);
                    goto oom;
                }
            }
            if (SBAppendSlice(
                    &shape, aliasedDeclText, ast.nodes[typeNode].start, ast.nodes[typeNode].end)
                != 0)
            {
                free(arenaMem);
                goto oom;
            }
            if (nameEnd <= nameStart
                || (nameEnd == nameStart + 1u && aliasedDeclText[nameStart] == '_'))
            {
                int nbytes = snprintf(
                    tempNameBuf, sizeof(tempNameBuf), "__sl_arg%u", (unsigned)paramIndex);
                if (nbytes <= 0 || (size_t)nbytes >= sizeof(tempNameBuf)) {
                    free(arenaMem);
                    return ErrorSimple(
                        "internal error: generated import wrapper arg name overflow");
                }
                tempName = tempNameBuf;
                if (SBAppendCStr(&wrapper, tempName) != 0 || SBAppendCStr(&callArgs, tempName) != 0)
                {
                    free(arenaMem);
                    goto oom;
                }
            } else {
                if (SBAppendSlice(&wrapper, aliasedDeclText, nameStart, nameEnd) != 0
                    || SBAppendSlice(&callArgs, aliasedDeclText, nameStart, nameEnd) != 0)
                {
                    free(arenaMem);
                    goto oom;
                }
            }
            if (SBAppendCStr(&wrapper, " ") != 0
                || SBAppendSlice(
                       &wrapper,
                       aliasedDeclText,
                       ast.nodes[typeNode].start,
                       ast.nodes[typeNode].end)
                       != 0)
            {
                free(arenaMem);
                goto oom;
            }
            paramIndex++;
        }
        child = ASTNextSibling(&ast, child);
    }

    if (SBAppendCStr(&wrapper, ")") != 0) {
        free(arenaMem);
        goto oom;
    }
    if (returnTypeNode >= 0
        && (SBAppendCStr(&wrapper, " ") != 0
            || SBAppendSlice(
                   &wrapper,
                   aliasedDeclText,
                   ast.nodes[returnTypeNode].start,
                   ast.nodes[returnTypeNode].end)
                   != 0))
    {
        free(arenaMem);
        goto oom;
    }
    if (contextTypeNode >= 0
        && (SBAppendCStr(&wrapper, " context ") != 0
            || SBAppendSlice(
                   &wrapper,
                   aliasedDeclText,
                   ast.nodes[contextTypeNode].start,
                   ast.nodes[contextTypeNode].end)
                   != 0))
    {
        free(arenaMem);
        goto oom;
    }
    if (SBAppendCStr(&wrapper, " { ") != 0) {
        free(arenaMem);
        goto oom;
    }
    if (returnTypeNode >= 0 && SBAppendCStr(&wrapper, "return ") != 0) {
        free(arenaMem);
        goto oom;
    }
    if (SBAppendCStr(&wrapper, qualifiedName) != 0 || SBAppendCStr(&wrapper, "(") != 0
        || SBAppendCStr(&wrapper, callArgs.v != NULL ? callArgs.v : "") != 0
        || SBAppendCStr(&wrapper, "); }\n") != 0)
    {
        free(arenaMem);
        goto oom;
    }

    *outShapeKey = SBFinish(&shape, NULL);
    *outWrapperDeclText = SBFinish(&wrapper, NULL);
    free(callArgs.v);
    free(arenaMem);
    if (*outShapeKey == NULL || *outWrapperDeclText == NULL) {
        free(*outShapeKey);
        free(*outWrapperDeclText);
        *outShapeKey = NULL;
        *outWrapperDeclText = NULL;
        return ErrorSimple("out of memory");
    }
    return 0;

oom:
    free(shape.v);
    free(wrapper.v);
    free(callArgs.v);
    return ErrorSimple("out of memory");
}

static int AppendAliasedPubDecls(
    SLStringBuilder* b,
    const SLPackage* sourcePkg,
    const char*      alias,
    const SLImportRef* _Nullable imports,
    uint32_t importLen,
    int      includePrivateDecls) {
    SLIdentMap* maps = NULL;
    uint32_t    mapLen = 0;
    uint32_t    declLen = includePrivateDecls ? sourcePkg->declLen : 0u;
    uint32_t    j;
    int         rc = -1;

    if (declLen == 0 && sourcePkg->pubDeclLen == 0) {
        return 0;
    }
    mapLen = declLen + sourcePkg->pubDeclLen;
    maps = (SLIdentMap*)calloc(mapLen, sizeof(SLIdentMap));
    if (maps == NULL) {
        return ErrorSimple("out of memory");
    }
    for (j = 0; j < declLen; j++) {
        maps[j].name = sourcePkg->decls[j].name;
        maps[j].replacement = NULL;
        if (BuildPrefixedName(alias, sourcePkg->decls[j].name, (char**)&maps[j].replacement) != 0) {
            goto done;
        }
    }
    for (j = 0; j < sourcePkg->pubDeclLen; j++) {
        maps[declLen + j].name = sourcePkg->pubDecls[j].name;
        maps[declLen + j].replacement = NULL;
        if (BuildPrefixedName(
                alias, sourcePkg->pubDecls[j].name, (char**)&maps[declLen + j].replacement)
            != 0)
        {
            goto done;
        }
    }

    for (j = 0; j < sourcePkg->pubDeclLen; j++) {
        char* rewritten = NULL;
        rc = RewriteText(
            sourcePkg->pubDecls[j].declText,
            (uint32_t)strlen(sourcePkg->pubDecls[j].declText),
            imports,
            importLen,
            maps,
            mapLen,
            &rewritten);
        if (rc != 0) {
            goto done;
        }
        if (rewritten == NULL || SBAppendCStr(b, rewritten) != 0 || SBAppendCStr(b, "\n") != 0) {
            free(rewritten);
            goto done;
        }
        free(rewritten);
    }
    for (j = 0; j < declLen; j++) {
        char* rewritten = NULL;
        rc = RewriteText(
            sourcePkg->decls[j].declText,
            (uint32_t)strlen(sourcePkg->decls[j].declText),
            imports,
            importLen,
            maps,
            mapLen,
            &rewritten);
        if (rc != 0) {
            goto done;
        }
        if (rewritten == NULL || SBAppendCStr(b, rewritten) != 0 || SBAppendCStr(b, "\n") != 0) {
            free(rewritten);
            goto done;
        }
        free(rewritten);
    }
    if (SBAppendCStr(b, "\n") != 0) {
        goto done;
    }
    rc = 0;

done:
    if (maps != NULL) {
        for (j = 0; j < mapLen; j++) {
            free((void*)maps[j].replacement);
        }
    }
    free(maps);
    return rc;
}

typedef struct {
    const SLPackage* pkg;
    const char*      alias;
} SLEmittedImportSurface;

static int EnsureEmittedImportSurfaceCap(
    SLEmittedImportSurface** outArr, uint32_t* outCap, uint32_t needLen) {
    if (needLen <= *outCap) {
        return 0;
    }
    {
        uint32_t                nextCap = *outCap == 0 ? 8u : *outCap;
        SLEmittedImportSurface* p;
        while (nextCap < needLen) {
            if (nextCap > UINT32_MAX / 2u) {
                return -1;
            }
            nextCap *= 2u;
        }
        p = (SLEmittedImportSurface*)realloc(*outArr, (size_t)nextCap * sizeof(**outArr));
        if (p == NULL) {
            return -1;
        }
        *outArr = p;
        *outCap = nextCap;
    }
    return 0;
}

static int HasEmittedImportSurface(
    const SLEmittedImportSurface* _Nullable arr,
    uint32_t len,
    const SLPackage* _Nullable pkg,
    const char* _Nullable alias) {
    uint32_t i;
    if (arr == NULL || pkg == NULL || alias == NULL) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        if (arr[i].pkg == pkg && StrEq(arr[i].alias, alias)) {
            return 1;
        }
    }
    return 0;
}

static int PackageNeedsPrivateDeclSurface(const SLPackage* pkg) {
    uint32_t i;
    if (pkg->name != NULL && StrEq(pkg->name, "builtin")) {
        return 0;
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (pkg->pubDecls[i].kind == SLAst_FN) {
            return 1;
        }
    }
    return 0;
}

static int AppendImportedPackageSurface(
    SLStringBuilder*         b,
    const SLPackage*         dep,
    const char*              alias,
    int                      includePrivateImportDecls,
    SLEmittedImportSurface** emitted,
    uint32_t*                emittedLen,
    uint32_t*                emittedCap) {
    uint32_t j;
    int      includePrivateDecls;
    if (b == NULL || dep == NULL || alias == NULL || emitted == NULL || emittedLen == NULL
        || emittedCap == NULL)
    {
        return ErrorSimple("internal error: unresolved import");
    }
    if (HasEmittedImportSurface(*emitted, *emittedLen, dep, alias)) {
        return 0;
    }

    /* Make direct imported exported types/symbols from dep available when dep's
     * own exported API references them. */
    for (j = 0; j < dep->importLen; j++) {
        const SLPackage* subDep = dep->imports[j].target;
        if (subDep == NULL) {
            return ErrorSimple("internal error: unresolved import");
        }
        if (!HasEmittedImportSurface(*emitted, *emittedLen, subDep, dep->imports[j].alias)
            && AppendAliasedPubDecls(b, subDep, dep->imports[j].alias, NULL, 0, 0) != 0)
        {
            return -1;
        }
        if (!HasEmittedImportSurface(*emitted, *emittedLen, subDep, dep->imports[j].alias)) {
            if (EnsureEmittedImportSurfaceCap(emitted, emittedCap, *emittedLen + 1u) != 0) {
                return ErrorSimple("out of memory");
            }
            if (*emitted == NULL) {
                return ErrorSimple("out of memory");
            }
            (*emitted)[*emittedLen].pkg = subDep;
            (*emitted)[*emittedLen].alias = dep->imports[j].alias;
            (*emittedLen)++;
        }
    }
    includePrivateDecls = includePrivateImportDecls ? PackageNeedsPrivateDeclSurface(dep) : 0;
    if (AppendAliasedPubDecls(b, dep, alias, dep->imports, dep->importLen, includePrivateDecls)
        != 0)
    {
        return -1;
    }
    if (EnsureEmittedImportSurfaceCap(emitted, emittedCap, *emittedLen + 1u) != 0) {
        return ErrorSimple("out of memory");
    }
    (*emitted)[*emittedLen].pkg = dep;
    (*emitted)[*emittedLen].alias = alias;
    (*emittedLen)++;
    return 0;
}

static int AppendImportFunctionWrappers(SLStringBuilder* b, const SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const SLImportSymbolRef* sym = &pkg->importSymbols[i];
        if (!sym->isFunction || !sym->useWrapper || sym->wrapperDeclText == NULL) {
            continue;
        }
        if (SBAppendCStr(b, sym->wrapperDeclText) != 0 || SBAppendCStr(b, "\n") != 0) {
            return ErrorSimple("out of memory");
        }
    }
    return 0;
}

static int BuildCombinedPackageSource(
    SLPackageLoader* loader,
    const SLPackage* pkg,
    int              includePrivateImportDecls,
    char**           outSource,
    uint32_t*        outLen,
    SLCombinedSourceMap* _Nullable sourceMap,
    uint32_t* _Nullable outOwnDeclStartOffset) {
    SLStringBuilder         b = { 0 };
    uint32_t                i;
    SLEmittedImportSurface* emitted = NULL;
    uint32_t                emittedLen = 0;
    uint32_t                emittedCap = 0;
    (void)loader;
    *outSource = NULL;
    *outLen = 0;
    if (outOwnDeclStartOffset != NULL) {
        *outOwnDeclStartOffset = 0;
    }
    if (sourceMap != NULL) {
        CombinedSourceMapFree(sourceMap);
    }

    for (i = 0; i < pkg->importLen; i++) {
        const SLPackage* dep = pkg->imports[i].target;
        if (AppendImportedPackageSurface(
                &b,
                dep,
                pkg->imports[i].alias,
                includePrivateImportDecls,
                &emitted,
                &emittedLen,
                &emittedCap)
            != 0)
        {
            free(b.v);
            free(emitted);
            return -1;
        }
    }
    if (AppendImportFunctionWrappers(&b, pkg) != 0) {
        free(b.v);
        free(emitted);
        return -1;
    }
    if (outOwnDeclStartOffset != NULL) {
        *outOwnDeclStartOffset = b.len;
    }

    for (i = 0; i < pkg->declTextLen; i++) {
        const SLDeclText*   decl = &pkg->declTexts[i];
        const SLParsedFile* file = &pkg->files[decl->fileIndex];
        char*               namedRewritten = NULL;
        char*               rewritten = NULL;
        uint32_t            combinedStart;
        uint32_t            combinedEnd;
        uint32_t            sourceStart = 0;
        uint32_t            sourceEnd = 0;
        uint32_t            rewrittenLen;
        if (decl->nodeId >= 0 && (uint32_t)decl->nodeId < file->ast.len) {
            const SLAstNode* n = &file->ast.nodes[decl->nodeId];
            sourceStart = n->start;
            sourceEnd = n->end;
        }
        if (RewriteDeclTextForNamedImports(pkg, file, decl->nodeId, decl->text, &namedRewritten)
            != 0)
        {
            free(b.v);
            free(emitted);
            return ErrorSimple("out of memory");
        }
        if (RewriteText(
                namedRewritten,
                (uint32_t)strlen(namedRewritten),
                pkg->imports,
                pkg->importLen,
                NULL,
                0,
                &rewritten)
            != 0)
        {
            free(namedRewritten);
            free(b.v);
            free(emitted);
            return -1;
        }
        if (rewritten == NULL) {
            free(namedRewritten);
            free(b.v);
            free(emitted);
            return -1;
        }
        rewrittenLen = (uint32_t)strlen(rewritten);
        combinedStart = b.len;
        combinedEnd = combinedStart + rewrittenLen;
        if (SBAppendCStr(&b, rewritten) != 0 || SBAppendCStr(&b, "\n") != 0) {
            free(namedRewritten);
            free(rewritten);
            free(b.v);
            free(emitted);
            if (sourceMap != NULL) {
                CombinedSourceMapFree(sourceMap);
            }
            return ErrorSimple("out of memory");
        }
        if (sourceMap != NULL
            && CombinedSourceMapAdd(
                   sourceMap,
                   combinedStart,
                   combinedEnd,
                   sourceStart,
                   sourceEnd,
                   decl->fileIndex,
                   decl->nodeId)
                   != 0)
        {
            free(namedRewritten);
            free(rewritten);
            free(b.v);
            free(emitted);
            CombinedSourceMapFree(sourceMap);
            return ErrorSimple("out of memory");
        }
        free(namedRewritten);
        free(rewritten);
    }

    *outSource = SBFinish(&b, outLen);
    free(emitted);
    if (*outSource == NULL) {
        if (sourceMap != NULL) {
            CombinedSourceMapFree(sourceMap);
        }
        return ErrorSimple("out of memory");
    }
    return 0;
}

static int CheckLoadedPackage(SLPackageLoader* loader, SLPackage* pkg, int suppressUnusedWarnings) {
    char*               source = NULL;
    uint32_t            sourceLen = 0;
    int                 lineColDiag = 0;
    const char*         checkPath = pkg->dirPath;
    const char*         checkSource = NULL;
    uint32_t            checkSourceLen = 0;
    SLCombinedSourceMap sourceMap = { 0 };
    int                 useSingleFileRemap = (pkg->fileLen == 1 && pkg->importLen > 0);
    if (pkg->checked) {
        return 0;
    }
    if (BuildCombinedPackageSource(
            loader, pkg, 1, &source, &sourceLen, useSingleFileRemap ? &sourceMap : NULL, NULL)
        != 0)
    {
        return -1;
    }
    checkSource = source;
    checkSourceLen = sourceLen;
    if (pkg->fileLen == 1) {
        checkPath = pkg->files[0].path;
        lineColDiag = 1;
        if (pkg->importLen == 0) {
            checkSource = pkg->files[0].source;
            checkSourceLen = pkg->files[0].sourceLen;
        }
    }
    if (useSingleFileRemap
            ? CheckSourceExWithSingleFileRemap(
                  checkPath,
                  checkSource,
                  checkSourceLen,
                  lineColDiag,
                  pkg->files[0].source,
                  &sourceMap,
                  suppressUnusedWarnings)
                  != 0
            : CheckSourceEx(
                  checkPath, checkSource, checkSourceLen, lineColDiag, suppressUnusedWarnings)
                  != 0)
    {
        CombinedSourceMapFree(&sourceMap);
        free(source);
        return -1;
    }
    CombinedSourceMapFree(&sourceMap);
    free(source);
    pkg->checked = 1;
    return 0;
}

static void FreePackage(SLPackage* pkg) {
    uint32_t i;
    free(pkg->dirPath);
    free(pkg->name);
    for (i = 0; i < pkg->fileLen; i++) {
        free(pkg->files[i].path);
        free(pkg->files[i].source);
        free(pkg->files[i].arenaMem);
    }
    free(pkg->files);
    for (i = 0; i < pkg->importLen; i++) {
        free(pkg->imports[i].alias);
        free(pkg->imports[i].bindName);
        free(pkg->imports[i].path);
    }
    free(pkg->imports);
    for (i = 0; i < pkg->importSymbolLen; i++) {
        free(pkg->importSymbols[i].sourceName);
        free(pkg->importSymbols[i].localName);
        free(pkg->importSymbols[i].qualifiedName);
        free(pkg->importSymbols[i].fnShapeKey);
        free(pkg->importSymbols[i].wrapperDeclText);
    }
    free(pkg->importSymbols);
    for (i = 0; i < pkg->declLen; i++) {
        free(pkg->decls[i].name);
        free(pkg->decls[i].declText);
    }
    free(pkg->decls);
    for (i = 0; i < pkg->pubDeclLen; i++) {
        free(pkg->pubDecls[i].name);
        free(pkg->pubDecls[i].declText);
    }
    free(pkg->pubDecls);
    for (i = 0; i < pkg->declTextLen; i++) {
        free(pkg->declTexts[i].text);
    }
    free(pkg->declTexts);
    memset(pkg, 0, sizeof(*pkg));
}

void FreeLoader(SLPackageLoader* loader) {
    uint32_t i;
    for (i = 0; i < loader->packageLen; i++) {
        FreePackage(&loader->packages[i]);
    }
    free(loader->packages);
    free(loader->rootDir);
    free(loader->platformTarget);
    memset(loader, 0, sizeof(*loader));
}

int LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    SLPackageLoader* outLoader,
    SLPackage**      outEntryPkg) {
    char*           canonical = CanonicalizePath(entryPath);
    struct stat     st;
    char*           pkgDir = NULL;
    char*           rootDir;
    SLPackageLoader loader;
    SLPackage*      entryPkg = NULL;
    uint32_t        i;
    memset(outLoader, 0, sizeof(*outLoader));
    *outEntryPkg = NULL;
    if (canonical == NULL) {
        return ErrorSimple("failed to resolve package path %s", entryPath);
    }

    if (stat(canonical, &st) != 0) {
        free(canonical);
        return ErrorSimple("failed to access %s", entryPath);
    }

    if (S_ISDIR(st.st_mode)) {
        rootDir = DirNameDup(canonical);
    } else if (S_ISREG(st.st_mode) && HasSuffix(canonical, ".sl")) {
        pkgDir = DirNameDup(canonical);
        if (pkgDir == NULL) {
            free(canonical);
            return ErrorSimple("out of memory");
        }
        rootDir = DirNameDup(pkgDir);
    } else {
        free(canonical);
        return ErrorSimple("expected package directory or .sl file: %s", entryPath);
    }
    if (rootDir == NULL) {
        free(pkgDir);
        free(canonical);
        return ErrorSimple("out of memory");
    }

    memset(&loader, 0, sizeof(loader));
    loader.rootDir = rootDir;
    loader.platformTarget = DupCStr(
        (platformTarget != NULL && platformTarget[0] != '\0')
            ? platformTarget
            : SL_DEFAULT_PLATFORM_TARGET);
    if (loader.platformTarget == NULL) {
        free(pkgDir);
        free(canonical);
        FreeLoader(&loader);
        return ErrorSimple("out of memory");
    }

    if (pkgDir != NULL) {
        if (LoadSingleFilePackage(&loader, canonical, &entryPkg) != 0) {
            free(pkgDir);
            free(canonical);
            FreeLoader(&loader);
            return -1;
        }
    } else if (LoadPackageRecursive(&loader, canonical, &entryPkg) != 0) {
        free(canonical);
        FreeLoader(&loader);
        return -1;
    }
    free(pkgDir);
    if (entryPkg == NULL || entryPkg->dirPath == NULL) {
        free(canonical);
        FreeLoader(&loader);
        return ErrorSimple("internal error: failed to load entry package");
    }

    if (LoadSelectedPlatformTargetPackage(&loader, entryPkg->dirPath, NULL) != 0) {
        free(canonical);
        FreeLoader(&loader);
        return -1;
    }

    for (i = 0; i < loader.packageLen; i++) {
        int suppressUnusedWarnings = (&loader.packages[i] != entryPkg);
        if (CheckLoadedPackage(&loader, &loader.packages[i], suppressUnusedWarnings) != 0) {
            free(canonical);
            FreeLoader(&loader);
            return -1;
        }
    }

    free(canonical);
    *outLoader = loader;
    *outEntryPkg = entryPkg;
    return 0;
}

int ValidateEntryMainSignature(const SLPackage* _Nullable entryPkg) {
    uint32_t fileIndex;
    int      hasMainDefinition = 0;

    if (entryPkg == NULL) {
        return ErrorSimple("internal error: missing entry package");
    }

    for (fileIndex = 0; fileIndex < entryPkg->fileLen; fileIndex++) {
        const SLParsedFile* file = &entryPkg->files[fileIndex];
        const SLAst*        ast = &file->ast;
        int32_t             nodeId = ASTFirstChild(ast, ast->root);

        while (nodeId >= 0) {
            const SLAstNode* n = &ast->nodes[nodeId];
            if (n->kind == SLAst_FN && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "main"))
            {
                int32_t child = ASTFirstChild(ast, nodeId);
                int     paramCount = 0;
                int     hasReturnType = 0;
                int     hasBody = 0;

                while (child >= 0) {
                    const SLAstNode* ch = &ast->nodes[child];
                    if (ch->kind == SLAst_PARAM) {
                        paramCount++;
                    } else if (IsFnReturnTypeNodeKind(ch->kind) && ch->flags == 1) {
                        hasReturnType = 1;
                    } else if (ch->kind == SLAst_BLOCK) {
                        hasBody = 1;
                    }
                    child = ASTNextSibling(ast, child);
                }

                if (paramCount != 0 || hasReturnType) {
                    return Errorf(
                        file->path,
                        file->source,
                        n->start,
                        n->end,
                        "entrypoint must have signature: fn main()");
                }
                if (hasBody) {
                    hasMainDefinition = 1;
                }
            }
            nodeId = ASTNextSibling(ast, nodeId);
        }
    }

    if (!hasMainDefinition) {
        return ErrorSimple("entry package is missing fn main() definition");
    }

    return 0;
}

static int CheckPackageDir(const char* entryPath, const char* _Nullable platformTarget) {
    SLPackageLoader loader;
    SLPackage*      entryPkg = NULL;
    if (LoadAndCheckPackage(entryPath, platformTarget, &loader, &entryPkg) != 0) {
        return -1;
    }
    (void)entryPkg;
    FreeLoader(&loader);
    return 0;
}

static int WriteOutput(const char* _Nullable outFilename, const char* data, uint32_t len) {
    FILE*  out;
    size_t nwritten;
    if (outFilename == NULL) {
        if (len == 0) {
            return 0;
        }
        nwritten = fwrite(data, 1u, (size_t)len, stdout);
        return nwritten == (size_t)len ? 0 : -1;
    }
    out = fopen(outFilename, "wb");
    if (out == NULL) {
        return -1;
    }
    nwritten = fwrite(data, 1u, (size_t)len, out);
    fclose(out);
    return nwritten == (size_t)len ? 0 : -1;
}

static int ParseGenpkgMode(const char* mode, char* outBackend, uint32_t outBackendCap) {
    uint32_t i;
    if (mode[0] != 'g' || mode[1] != 'e' || mode[2] != 'n' || mode[3] != 'p' || mode[4] != 'k'
        || mode[5] != 'g')
    {
        return 0;
    }
    if (mode[6] == '\0') {
        if (outBackendCap < 2) {
            return -1;
        }
        outBackend[0] = 'c';
        outBackend[1] = '\0';
        return 1;
    }
    if (mode[6] != ':') {
        return -1;
    }
    i = 0;
    while (mode[7u + i] != '\0') {
        if (i + 1u >= outBackendCap) {
            return -1;
        }
        outBackend[i] = mode[7u + i];
        i++;
    }
    if (i == 0) {
        return -1;
    }
    outBackend[i] = '\0';
    return 1;
}

static void PrintUsage(const char* argv0) {
    fprintf(
        stderr,
        "usage: %s --version\n"
        "       %s [lex|ast|check] <file.sl>\n"
        "       %s [--platform <target>] [--cache-dir <dir>] mir <package-dir|file.sl>\n"
        "       %s fmt [--check] [<file-or-dir> ...]\n"
        "       %s [--platform <target>] [--cache-dir <dir>] checkpkg <package-dir|file.sl>\n"
        "       %s [--platform <target>] [--cache-dir <dir>] genpkg[:backend] "
        "<package-dir|file.sl> [out.h]\n"
        "       %s [--platform <target>] [--cache-dir <dir>] compile <package-dir|file.sl> "
        "[-o <output>]\n"
        "       %s [--platform <target>] [--cache-dir <dir>] run <package-dir|file.sl>\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0,
        argv0);
}

static void PrintVersion(void) {
    fprintf(
        stdout,
        "SL compiler version %d (%s) eval%s\n",
        SL_VERSION,
        SL_SOURCE_HASH,
        SL_WITH_C_BACKEND ? " c11" : "");
}

static int HasCBackendBuild(void) {
#if SL_WITH_C_BACKEND
    return 1;
#else
    return 0;
#endif
}

static int HasWasmBackendBuild(void) {
#if SL_WITH_WASM_BACKEND
    return 1;
#else
    return 0;
#endif
}

static int ErrorCBackendDisabled(void) {
    return ErrorSimple("this slc build was compiled without the C backend");
}

static int GeneratePackage(
    const char* entryPath,
    const char* backendName,
    const char* _Nullable outFilename,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg) {
    uint8_t                 mirArenaStorage[4096];
    SLArena                 mirArena;
    SLPackageLoader         loader;
    SLPackage*              entryPkg = NULL;
    char*                   source = NULL;
    uint32_t                sourceLen = 0;
    SLMirProgram            mirProgram = { 0 };
    SLCodegenArtifact       artifact = { 0 };
    SLDiag                  diag = { 0 };
    SLCodegenUnit           unit;
    const SLCodegenBackend* backend;
    int                     needsMir = 0;
    (void)cacheDirArg;

    memset(&mirArena, 0, sizeof(mirArena));

    if (LoadAndCheckPackage(entryPath, platformTarget, &loader, &entryPkg) != 0) {
        return -1;
    }

    if (BuildCombinedPackageSource(&loader, entryPkg, 1, &source, &sourceLen, NULL, NULL) != 0) {
        FreeLoader(&loader);
        return -1;
    }

    if (!IsValidIdentifier(entryPkg->name)) {
        free(source);
        FreeLoader(&loader);
        return ErrorSimple(
            "entry package name \"%s\" is not a valid identifier "
            "(inferred from path)",
            entryPkg->name);
    }

    backend = SLCodegenFindBackend(backendName);
    if (backend == NULL) {
        free(source);
        FreeLoader(&loader);
        if (!HasCBackendBuild()) {
            return ErrorCBackendDisabled();
        }
        return ErrorSimple("unknown backend: %s", backendName);
    }
    if (source == NULL) {
        FreeLoader(&loader);
        return ErrorSimple("out of memory");
    }

    unit.packageName = entryPkg->name;
    unit.source = source;
    unit.sourceLen = sourceLen;
    unit.platformTarget = platformTarget;
    unit.mirProgram = NULL;
    unit.usesPlatform = 0;

    needsMir = StrEq(backendName, "wasm");
    if (needsMir) {
        SLArenaInit(&mirArena, mirArenaStorage, sizeof(mirArenaStorage));
        SLArenaSetAllocator(&mirArena, NULL, CodegenArenaGrow, CodegenArenaFree);
        if (BuildPackageMirProgram(&loader, entryPkg, &mirArena, &mirProgram, &diag) != 0) {
            if (diag.code != SLDiag_NONE && entryPkg->fileLen == 1
                && entryPkg->files[0].source != NULL)
            {
                (void)PrintSLDiagLineCol(
                    entryPkg->files[0].path, entryPkg->files[0].source, &diag, 0);
            } else if (diag.code != SLDiag_NONE) {
                (void)ErrorSimple("invalid MIR program");
            }
            free(source);
            FreeLoader(&loader);
            SLArenaDispose(&mirArena);
            return -1;
        }
        unit.mirProgram = &mirProgram;
        unit.usesPlatform = PackageUsesPlatformImport(&loader) ? 1u : 0u;
    }

    SLCodegenOptions codegenOptions = { 0 };
    codegenOptions.arenaGrow = CodegenArenaGrow;
    codegenOptions.arenaFree = CodegenArenaFree;

    if (backend->emit(backend, &unit, &codegenOptions, &artifact, &diag) != 0) {
        if (diag.code != SLDiag_NONE) {
            int diagStatus;
            if (entryPkg->fileLen == 1 && entryPkg->importLen == 0) {
                diagStatus = PrintSLDiagLineCol(
                    entryPkg->files[0].path, entryPkg->files[0].source, &diag, 1);
            } else {
                diagStatus = PrintSLDiag(entryPkg->dirPath, source, &diag, 1);
            }
            if (!(diagStatus == 0 && artifact.data != NULL)) {
                free(source);
                FreeLoader(&loader);
                SLArenaDispose(&mirArena);
                return -1;
            }
        } else {
            fprintf(stderr, "error: codegen failed\n");
            free(source);
            FreeLoader(&loader);
            SLArenaDispose(&mirArena);
            return -1;
        }
    }

    if (WriteOutput(outFilename, (const char*)artifact.data, artifact.len) != 0) {
        fprintf(stderr, "error: failed to write output\n");
        free(artifact.data);
        free(source);
        FreeLoader(&loader);
        SLArenaDispose(&mirArena);
        return -1;
    }

    free(artifact.data);
    free(source);
    FreeLoader(&loader);
    SLArenaDispose(&mirArena);
    return 0;
}

static int RunCommand(const char* const* argv) {
    pid_t pid = fork();
    int   status;
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char* const*)argv);
        perror(argv[0]);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return 0;
    }
    return -1;
}

static int RunCommandExitCode(const char* const* argv, int* outExitCode) {
    pid_t pid = fork();
    int   status;
    if (outExitCode == NULL) {
        return -1;
    }
    *outExitCode = -1;
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        execvp(argv[0], (char* const*)argv);
        perror(argv[0]);
        _exit(127);
    }
    if (waitpid(pid, &status, 0) < 0) {
        return -1;
    }
    if (WIFEXITED(status)) {
        *outExitCode = WEXITSTATUS(status);
        return 0;
    }
    if (WIFSIGNALED(status)) {
        *outExitCode = 128 + WTERMSIG(status);
        return 0;
    }
    return -1;
}

static void FreePackageArtifacts(SLPackageArtifact* _Nullable artifacts, uint32_t artifactLen) {
    uint32_t i;
    if (artifacts == NULL) {
        return;
    }
    for (i = 0; i < artifactLen; i++) {
        free(artifacts[i].key);
        free(artifacts[i].linkPrefix);
        free(artifacts[i].cacheDir);
        free(artifacts[i].cPath);
        free(artifacts[i].oPath);
        free(artifacts[i].sigPath);
    }
    free(artifacts);
}

static int ResolveLibDir(char** outLibDir) {
    char* exeDir = GetExeDir();
    char* libDir = NULL;
    *outLibDir = NULL;
    if (exeDir != NULL) {
        libDir = JoinPath(exeDir, "lib");
        free(exeDir);
    }
    if (libDir == NULL) {
        return ErrorSimple("cannot locate lib directory (dirname of executable)");
    }
    *outLibDir = libDir;
    return 0;
}

static int ResolveRepoToolPath(const char* relPath, char** outPath) {
    char* exeDir = NULL;
    char* buildDir = NULL;
    char* repoDir = NULL;
    char* toolPath = NULL;
    if (relPath == NULL || outPath == NULL) {
        return -1;
    }
    *outPath = NULL;
    exeDir = GetExeDir();
    if (exeDir == NULL) {
        return ErrorSimple("cannot locate executable directory");
    }
    buildDir = DirNameDup(exeDir);
    free(exeDir);
    if (buildDir == NULL) {
        return ErrorSimple("cannot locate build directory");
    }
    repoDir = DirNameDup(buildDir);
    free(buildDir);
    if (repoDir == NULL) {
        return ErrorSimple("cannot locate repository root");
    }
    toolPath = JoinPath(repoDir, relPath);
    free(repoDir);
    if (toolPath == NULL) {
        return ErrorSimple("out of memory");
    }
    if (access(toolPath, R_OK) != 0) {
        free(toolPath);
        return ErrorSimple("cannot locate %s", relPath);
    }
    *outPath = toolPath;
    return 0;
}

static int ResolvePlatformPath(
    const char* _Nullable libDir, const char* _Nullable platformTarget, char** outPlatformPath) {
    char* platformDir = NULL;
    char* platformTargetDir = NULL;
    char* platformPath = NULL;
    if (libDir == NULL || platformTarget == NULL || outPlatformPath == NULL) {
        return -1;
    }
    *outPlatformPath = NULL;
    platformDir = JoinPath(libDir, "platform");
    if (platformDir == NULL) {
        return ErrorSimple("out of memory");
    }
    platformTargetDir = JoinPath(platformDir, platformTarget);
    if (platformTargetDir == NULL) {
        free(platformDir);
        return ErrorSimple("out of memory");
    }
    platformPath = JoinPath(platformTargetDir, "platform.c");
    free(platformTargetDir);
    free(platformDir);
    if (platformPath == NULL) {
        return ErrorSimple("out of memory");
    }
    *outPlatformPath = platformPath;
    return 0;
}

static int ResolveBuiltinPath(
    const char* _Nullable libDir, char** outBuiltinPath, char** outBuiltinHeaderPath) {
    char* builtinDir = NULL;
    char* builtinPath = NULL;
    char* builtinHeaderPath = NULL;
    if (libDir == NULL || outBuiltinPath == NULL || outBuiltinHeaderPath == NULL) {
        return -1;
    }
    *outBuiltinPath = NULL;
    *outBuiltinHeaderPath = NULL;
    builtinDir = JoinPath(libDir, "builtin");
    if (builtinDir == NULL) {
        return ErrorSimple("out of memory");
    }
    builtinPath = JoinPath(builtinDir, "builtin.c");
    builtinHeaderPath = JoinPath(builtinDir, "builtin.h");
    free(builtinDir);
    if (builtinPath == NULL || builtinHeaderPath == NULL) {
        free(builtinPath);
        free(builtinHeaderPath);
        return ErrorSimple("out of memory");
    }
    *outBuiltinPath = builtinPath;
    *outBuiltinHeaderPath = builtinHeaderPath;
    return 0;
}

static int ResolveCacheRoot(
    const SLPackageLoader* _Nullable loader,
    const char* _Nullable cacheDirArg,
    char** outCacheRoot) {
    char* cacheRoot;
    if (outCacheRoot == NULL) {
        return -1;
    }
    *outCacheRoot = NULL;
    if (cacheDirArg != NULL) {
        cacheRoot = MakeAbsolutePathDup(cacheDirArg);
    } else {
        if (loader == NULL || loader->rootDir == NULL) {
            return -1;
        }
        cacheRoot = JoinPath(loader->rootDir, ".sl-cache");
    }
    if (cacheRoot == NULL) {
        return ErrorSimple("out of memory");
    }
    *outCacheRoot = cacheRoot;
    return 0;
}

int FindPackageIndex(const SLPackageLoader* loader, const SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < loader->packageLen; i++) {
        if (&loader->packages[i] == pkg) {
            return (int)i;
        }
    }
    return -1;
}

static const char* _Nullable FindPreferredImportPath(
    const SLPackageLoader* loader, const SLPackage* pkg) {
    const char* best = NULL;
    uint32_t    i;
    for (i = 0; i < loader->packageLen; i++) {
        const SLPackage* src = &loader->packages[i];
        uint32_t         j;
        for (j = 0; j < src->importLen; j++) {
            if (src->imports[j].target != pkg) {
                continue;
            }
            if (best == NULL || strlen(src->imports[j].path) < strlen(best)
                || (strlen(src->imports[j].path) == strlen(best)
                    && strcmp(src->imports[j].path, best) < 0))
            {
                best = src->imports[j].path;
            }
        }
    }
    return best;
}

static char* _Nullable BuildPackageKey(const SLPackageLoader* loader, const SLPackage* pkg) {
    const char* hashSource = pkg->dirPath;
    const char* keyHint = FindPreferredImportPath(loader, pkg);
    char*       keyBase = NULL;
    char*       out = NULL;
    uint64_t    h;
    int         n;
    if (pkg->fileLen == 1) {
        hashSource = pkg->files[0].path;
    }
    keyBase = BuildSanitizedIdent(keyHint != NULL ? keyHint : pkg->name, "pkg");
    if (keyBase == NULL) {
        return NULL;
    }
    h = HashFNV1a64(hashSource);
    out = (char*)malloc(strlen(keyBase) + 1u + 16u + 1u);
    if (out == NULL) {
        free(keyBase);
        return NULL;
    }
    n = snprintf(
        out, strlen(keyBase) + 1u + 16u + 1u, "%s-%016llx", keyBase, (unsigned long long)h);
    free(keyBase);
    if (n <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

static char* _Nullable BuildPackageLinkPrefix(const SLPackage* pkg) {
    (void)pkg;
    return BuildSanitizedIdent(pkg->name, "pkg");
}

static char* _Nullable BuildPackageMacro(const char* key, const char* suffix) {
    char*  sanitized = BuildSanitizedIdent(key, "pkg");
    size_t keyLen;
    size_t suffixLen;
    char*  out;
    size_t i;
    if (sanitized == NULL) {
        return NULL;
    }
    keyLen = strlen(sanitized);
    suffixLen = strlen(suffix);
    out = (char*)malloc(keyLen + suffixLen + 1u);
    if (out == NULL) {
        free(sanitized);
        return NULL;
    }
    for (i = 0; i < keyLen; i++) {
        char ch = sanitized[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (char)('A' + (ch - 'a'));
        }
        out[i] = ch;
    }
    memcpy(out + keyLen, suffix, suffixLen + 1u);
    free(sanitized);
    return out;
}

static SLPackageArtifact* _Nullable FindArtifactByPkg(
    SLPackageArtifact* _Nullable artifacts, uint32_t artifactLen, const SLPackage* _Nullable pkg) {
    uint32_t i;
    if (artifacts == NULL || pkg == NULL) {
        return NULL;
    }
    for (i = 0; i < artifactLen; i++) {
        if (artifacts[i].pkg == pkg) {
            return &artifacts[i];
        }
    }
    return NULL;
}

static int BuildToolchainSignature(
    const SLPackageLoader* loader, const char* libDir, char** outSignature) {
    SLStringBuilder b = { 0 };
    char*           sig;
    char            tmp[64];
    int             n;
    *outSignature = NULL;
    n = snprintf(tmp, sizeof(tmp), "%d", SL_VERSION);
    if (n <= 0) {
        return -1;
    }
    if (SBAppendCStr(&b, "sl_version=") != 0 || SBAppend(&b, tmp, (uint32_t)n) != 0) {
        free(b.v);
        return -1;
    }
    if (SBAppendCStr(&b, ";source_hash=") != 0 || SBAppendCStr(&b, SL_SOURCE_HASH) != 0
        || SBAppendCStr(&b, ";backend=c;platform=") != 0
        || SBAppendCStr(&b, loader->platformTarget) != 0
        || SBAppendCStr(&b, ";cc=cc;-std=c11;-g;-w;lib=") != 0 || SBAppendCStr(&b, libDir) != 0)
    {
        free(b.v);
        return -1;
    }
    sig = SBFinish(&b, NULL);
    if (sig == NULL) {
        return -1;
    }
    *outSignature = sig;
    return 0;
}

static int ToolchainSignatureMatches(const char* sigPath, const char* signature) {
    FILE*    f;
    long     flen;
    char*    actual;
    size_t   nread;
    uint32_t expectedLen = (uint32_t)strlen(signature);
    f = fopen(sigPath, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    flen = ftell(f);
    if (flen < 0 || (uint64_t)flen > (uint64_t)UINT32_MAX) {
        fclose(f);
        return 0;
    }
    if ((uint32_t)flen != expectedLen) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    actual = (char*)malloc((size_t)expectedLen);
    if (actual == NULL) {
        fclose(f);
        return 0;
    }
    nread = fread(actual, 1u, (size_t)expectedLen, f);
    fclose(f);
    if (nread != (size_t)expectedLen || memcmp(actual, signature, (size_t)expectedLen) != 0) {
        free(actual);
        return 0;
    }
    free(actual);
    return 1;
}

static int PackageNewestSourceMtime(const SLPackage* pkg, uint64_t* outMtimeNs) {
    uint64_t maxMtime = 0;
    uint64_t mt;
    uint32_t i;
    if (GetFileMtimeNs(pkg->dirPath, &maxMtime) != 0) {
        return -1;
    }
    for (i = 0; i < pkg->fileLen; i++) {
        if (GetFileMtimeNs(pkg->files[i].path, &mt) != 0) {
            return -1;
        }
        if (mt > maxMtime) {
            maxMtime = mt;
        }
    }
    *outMtimeNs = maxMtime;
    return 0;
}

static int PackageHasUnsupportedImportedPubGlobals(const SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        const SLImportRef* imp = &pkg->imports[i];
        const SLPackage*   dep = imp->target;
        uint32_t           j;
        if (dep == NULL) {
            return ErrorSimple("internal error: unresolved import");
        }
        for (j = 0; j < dep->pubDeclLen; j++) {
            if (dep->pubDecls[j].kind == SLAst_VAR || dep->pubDecls[j].kind == SLAst_CONST) {
                const SLParsedFile* file = &pkg->files[imp->fileIndex];
                return Errorf(
                    file->path,
                    file->source,
                    imp->start,
                    imp->end,
                    "imported public globals are not supported in cached multi-object mode");
            }
        }
    }
    return 0;
}

static int IsPackageArtifactUpToDate(
    const SLPackage*   pkg,
    SLPackageArtifact* artifact,
    SLPackageArtifact* artifacts,
    uint32_t           artifactLen,
    const char*        toolchainSignature) {
    struct stat st;
    uint64_t    objMtime;
    uint64_t    srcMtime;
    uint32_t    i;
    if (stat(artifact->cPath, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    if (stat(artifact->oPath, &st) != 0 || !S_ISREG(st.st_mode)) {
        return 0;
    }
    objMtime = StatMtimeNs(&st);
    if (!ToolchainSignatureMatches(artifact->sigPath, toolchainSignature)) {
        return 0;
    }
    if (PackageNewestSourceMtime(pkg, &srcMtime) != 0 || srcMtime > objMtime) {
        return 0;
    }
    for (i = 0; i < pkg->importLen; i++) {
        SLPackageArtifact* depArtifact = FindArtifactByPkg(
            artifacts, artifactLen, pkg->imports[i].target);
        uint64_t depMtime = 0;
        if (depArtifact == NULL) {
            return 0;
        }
        if (depArtifact->objMtimeNs > 0) {
            depMtime = depArtifact->objMtimeNs;
        } else if (GetFileMtimeNs(depArtifact->oPath, &depMtime) != 0) {
            return 0;
        }
        if (depMtime > objMtime) {
            return 0;
        }
    }
    artifact->objMtimeNs = objMtime;
    return 1;
}

static void RestoreImportOverrides(
    SLAliasOverride* _Nullable aliasOverrides,
    uint32_t aliasOverrideLen,
    SLImportSymbolOverride* _Nullable symbolOverrides,
    uint32_t symbolOverrideLen) {
    uint32_t i;
    for (i = 0; aliasOverrides != NULL && i < aliasOverrideLen; i++) {
        aliasOverrides[i].imp->alias = aliasOverrides[i].oldAlias;
        free(aliasOverrides[i].newAlias);
    }
    for (i = 0; symbolOverrides != NULL && i < symbolOverrideLen; i++) {
        symbolOverrides[i].sym->qualifiedName = symbolOverrides[i].oldQualifiedName;
        free(symbolOverrides[i].newQualifiedName);
    }
    free(aliasOverrides);
    free(symbolOverrides);
}

static int ApplyLinkPrefixImportOverrides(
    SLPackageLoader*         loader,
    SLPackageArtifact*       artifacts,
    uint32_t                 artifactLen,
    SLAliasOverride**        outAliasOverrides,
    uint32_t*                outAliasOverrideLen,
    SLImportSymbolOverride** outSymbolOverrides,
    uint32_t*                outSymbolOverrideLen) {
    SLAliasOverride*        aliasOverrides = NULL;
    SLImportSymbolOverride* symbolOverrides = NULL;
    uint32_t                aliasCap = 0;
    uint32_t                symbolCap = 0;
    uint32_t                aliasLen = 0;
    uint32_t                symbolLen = 0;
    uint32_t                i;
    for (i = 0; i < loader->packageLen; i++) {
        aliasCap += loader->packages[i].importLen;
        symbolCap += loader->packages[i].importSymbolLen;
    }
    if (aliasCap > 0) {
        aliasOverrides = (SLAliasOverride*)calloc(aliasCap, sizeof(SLAliasOverride));
        if (aliasOverrides == NULL) {
            return ErrorSimple("out of memory");
        }
    }
    if (symbolCap > 0) {
        symbolOverrides = (SLImportSymbolOverride*)calloc(
            symbolCap, sizeof(SLImportSymbolOverride));
        if (symbolOverrides == NULL) {
            free(aliasOverrides);
            return ErrorSimple("out of memory");
        }
    }

    for (i = 0; i < loader->packageLen; i++) {
        SLPackage* pkg = &loader->packages[i];
        uint32_t   j;
        for (j = 0; j < pkg->importLen; j++) {
            SLImportRef*       imp = &pkg->imports[j];
            SLPackageArtifact* depArtifact = FindArtifactByPkg(artifacts, artifactLen, imp->target);
            char*              newAlias;
            if (depArtifact == NULL) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("internal error: unresolved import artifact");
            }
            newAlias = DupCStr(depArtifact->linkPrefix);
            if (newAlias == NULL) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            if (aliasOverrides == NULL) {
                free(newAlias);
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            aliasOverrides[aliasLen].imp = imp;
            aliasOverrides[aliasLen].oldAlias = imp->alias;
            aliasOverrides[aliasLen].newAlias = newAlias;
            imp->alias = newAlias;
            aliasLen++;
        }
    }

    for (i = 0; i < loader->packageLen; i++) {
        SLPackage* pkg = &loader->packages[i];
        uint32_t   j;
        for (j = 0; j < pkg->importSymbolLen; j++) {
            SLImportSymbolRef* sym = &pkg->importSymbols[j];
            SLImportRef*       imp;
            char*              newQualifiedName = NULL;
            if (sym->importIndex >= pkg->importLen) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("internal error: invalid import symbol mapping");
            }
            imp = &pkg->imports[sym->importIndex];
            if (BuildPrefixedName(imp->alias, sym->sourceName, &newQualifiedName) != 0) {
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            if (symbolOverrides == NULL) {
                free(newQualifiedName);
                RestoreImportOverrides(aliasOverrides, aliasLen, symbolOverrides, symbolLen);
                return ErrorSimple("out of memory");
            }
            symbolOverrides[symbolLen].sym = sym;
            symbolOverrides[symbolLen].oldQualifiedName = sym->qualifiedName;
            symbolOverrides[symbolLen].newQualifiedName = newQualifiedName;
            sym->qualifiedName = newQualifiedName;
            symbolLen++;
        }
    }

    *outAliasOverrides = aliasOverrides;
    *outAliasOverrideLen = aliasLen;
    *outSymbolOverrides = symbolOverrides;
    *outSymbolOverrideLen = symbolLen;
    return 0;
}

static int EmitPackageArtifact(
    SLPackageLoader*        loader,
    const SLPackage*        pkg,
    SLPackageArtifact*      artifact,
    SLPackageArtifact*      artifacts,
    uint32_t                artifactLen,
    const char*             libDir,
    const char*             cachePkgDir,
    const char*             toolchainSignature,
    const SLCodegenBackend* backend) {
    char*             source = NULL;
    uint32_t          sourceLen = 0;
    uint32_t          ownDeclStartOffset = 0;
    SLCodegenUnit     unit;
    SLCodegenOptions  codegenOptions = { 0 };
    SLDiag            diag = { 0 };
    SLCodegenArtifact outArtifact = { 0 };
    char*             headerGuard = NULL;
    char*             implMacro = NULL;
    SLStringBuilder   cBuilder = { 0 };
    char*             cSource = NULL;
    const char*       ccArgv[16];
    uint32_t          i;
    int               rc = -1;

    if (PackageHasUnsupportedImportedPubGlobals(pkg) != 0) {
        return -1;
    }
    if (BuildCombinedPackageSource(loader, pkg, 1, &source, &sourceLen, NULL, &ownDeclStartOffset)
        != 0)
    {
        return -1;
    }
    if (source == NULL) {
        return ErrorSimple("out of memory");
    }

    unit.packageName = artifact->linkPrefix;
    unit.source = source;
    unit.sourceLen = sourceLen;
    unit.platformTarget = loader->platformTarget;
    unit.mirProgram = NULL;
    unit.usesPlatform = 0;
    headerGuard = BuildPackageMacro(artifact->key, "_H");
    implMacro = BuildPackageMacro(artifact->key, "_IMPL");
    if (headerGuard == NULL || implMacro == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }
    codegenOptions.headerGuard = headerGuard;
    codegenOptions.implMacro = implMacro;
    codegenOptions.emitNodeStartOffset = ownDeclStartOffset;
    codegenOptions.emitNodeStartOffsetEnabled = 1;
    codegenOptions.arenaGrow = CodegenArenaGrow;
    codegenOptions.arenaFree = CodegenArenaFree;

    if (backend->emit(backend, &unit, &codegenOptions, &outArtifact, &diag) != 0) {
        if (diag.code != SLDiag_NONE) {
            int diagStatus;
            if (pkg->fileLen == 1 && pkg->importLen == 0) {
                diagStatus = PrintSLDiagLineCol(pkg->files[0].path, pkg->files[0].source, &diag, 1);
            } else {
                diagStatus = PrintSLDiag(pkg->dirPath, source, &diag, 1);
            }
            if (!(diagStatus == 0 && outArtifact.data != NULL)) {
                goto end;
            }
        } else {
            ErrorSimple("codegen failed");
            goto end;
        }
    }
    if (outArtifact.isBinary) {
        ErrorSimple("internal error: compile pipeline received binary codegen artifact");
        goto end;
    }

    for (i = 0; i < pkg->importLen; i++) {
        const SLPackage*   dep = pkg->imports[i].target;
        SLPackageArtifact* depArtifact;
        uint32_t           j;
        int                alreadyIncluded = 0;
        if (dep == NULL || dep->pubDeclLen == 0) {
            continue;
        }
        depArtifact = FindArtifactByPkg(artifacts, artifactLen, dep);
        if (depArtifact == NULL) {
            ErrorSimple("internal error: unresolved import artifact");
            goto end;
        }
        for (j = 0; j < i; j++) {
            const SLPackage* prevDep = pkg->imports[j].target;
            if (prevDep != NULL && prevDep == dep) {
                alreadyIncluded = 1;
                break;
            }
        }
        if (alreadyIncluded) {
            continue;
        }
        if (SBAppendCStr(&cBuilder, "#include <") != 0
            || SBAppendCStr(&cBuilder, depArtifact->key) != 0
            || SBAppendCStr(&cBuilder, "/pkg.c>\n") != 0)
        {
            ErrorSimple("out of memory");
            goto end;
        }
    }
    if (cBuilder.len > 0 && SBAppendCStr(&cBuilder, "\n") != 0) {
        ErrorSimple("out of memory");
        goto end;
    }
    if (SBAppend(&cBuilder, (const char*)outArtifact.data, outArtifact.len) != 0) {
        ErrorSimple("out of memory");
        goto end;
    }
    cSource = SBFinish(&cBuilder, NULL);
    if (cSource == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }

    if (EnsureDirRecursive(artifact->cacheDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto end;
    }
    if (WriteOutput(artifact->cPath, cSource, (uint32_t)strlen(cSource)) != 0) {
        ErrorSimple("failed to write cached C source");
        goto end;
    }

    ccArgv[0] = "cc";
    ccArgv[1] = "-std=c11";
    ccArgv[2] = "-g";
    ccArgv[3] = "-w";
    ccArgv[4] = "-isystem";
    ccArgv[5] = libDir;
    ccArgv[6] = "-I";
    ccArgv[7] = cachePkgDir;
    ccArgv[8] = "-D";
    ccArgv[9] = implMacro;
    ccArgv[10] = "-c";
    ccArgv[11] = artifact->cPath;
    ccArgv[12] = "-o";
    ccArgv[13] = artifact->oPath;
    ccArgv[14] = NULL;
    if (RunCommand(ccArgv) != 0) {
        ErrorSimple("C compilation failed");
        goto end;
    }

    if (WriteOutput(artifact->sigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
        != 0)
    {
        ErrorSimple("failed to write cache toolchain signature");
        goto end;
    }
    if (GetFileMtimeNs(artifact->oPath, &artifact->objMtimeNs) != 0) {
        ErrorSimple("failed to stat cached object");
        goto end;
    }
    rc = 0;

end:
    free(headerGuard);
    free(implMacro);
    free(outArtifact.data);
    free(source);
    free(cSource);
    free(cBuilder.v);
    return rc;
}

static int VisitTopoPackage(
    const SLPackageLoader* loader,
    uint32_t               pkgIndex,
    uint8_t*               state,
    uint32_t*              order,
    uint32_t*              orderLen) {
    const SLPackage* pkg = &loader->packages[pkgIndex];
    uint32_t         i;
    if (state[pkgIndex] == 2) {
        return 0;
    }
    if (state[pkgIndex] == 1) {
        return ErrorSimple("import cycle detected");
    }
    state[pkgIndex] = 1;
    for (i = 0; i < pkg->importLen; i++) {
        int depIndex = FindPackageIndex(loader, pkg->imports[i].target);
        if (depIndex < 0) {
            return ErrorSimple("internal error: unresolved import");
        }
        if (VisitTopoPackage(loader, (uint32_t)depIndex, state, order, orderLen) != 0) {
            return -1;
        }
    }
    state[pkgIndex] = 2;
    order[(*orderLen)++] = pkgIndex;
    return 0;
}

static int BuildPackageTopologicalOrder(
    const SLPackageLoader* loader,
    uint32_t*              outOrder,
    uint32_t               outOrderCap,
    uint32_t*              outOrderLen) {
    uint8_t* state = NULL;
    uint32_t i;
    *outOrderLen = 0;
    if (outOrderCap < loader->packageLen) {
        return -1;
    }
    state = (uint8_t*)calloc(loader->packageLen, sizeof(uint8_t));
    if (state == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < loader->packageLen; i++) {
        if (VisitTopoPackage(loader, i, state, outOrder, outOrderLen) != 0) {
            free(state);
            return -1;
        }
    }
    free(state);
    return 0;
}

static int BuildCachedPackageArtifacts(
    SLPackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char*         libDir,
    SLPackageArtifact** outArtifacts,
    uint32_t*           outArtifactLen) {
    SLPackageArtifact*      artifacts = NULL;
    uint32_t                artifactLen = loader->packageLen;
    char*                   cacheRoot = NULL;
    char*                   cacheV1Dir = NULL;
    char*                   cachePkgDir = NULL;
    char*                   toolchainSignature = NULL;
    const SLCodegenBackend* backend;
    uint32_t*               topoOrder = NULL;
    uint32_t                topoOrderLen = 0;
    SLAliasOverride*        aliasOverrides = NULL;
    uint32_t                aliasOverrideLen = 0;
    SLImportSymbolOverride* symbolOverrides = NULL;
    uint32_t                symbolOverrideLen = 0;
    uint32_t                i;

    *outArtifacts = NULL;
    *outArtifactLen = 0;

    backend = SLCodegenFindBackend("c");
    if (backend == NULL) {
        return ErrorSimple("unknown backend: c");
    }

    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cachePkgDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "pkg") : NULL;
    if (cacheV1Dir == NULL || cachePkgDir == NULL) {
        free(cachePkgDir);
        free(cacheV1Dir);
        free(cacheRoot);
        return ErrorSimple("out of memory");
    }
    if (EnsureDirRecursive(cachePkgDir) != 0) {
        free(cachePkgDir);
        free(cacheV1Dir);
        free(cacheRoot);
        return ErrorSimple("failed to create cache directory");
    }

    artifacts = (SLPackageArtifact*)calloc(artifactLen, sizeof(SLPackageArtifact));
    topoOrder = (uint32_t*)calloc(artifactLen, sizeof(uint32_t));
    if ((artifactLen > 0 && artifacts == NULL) || (artifactLen > 0 && topoOrder == NULL)) {
        free(topoOrder);
        free(artifacts);
        free(cachePkgDir);
        free(cacheV1Dir);
        free(cacheRoot);
        return ErrorSimple("out of memory");
    }

    for (i = 0; i < artifactLen; i++) {
        SLPackageArtifact* a = &artifacts[i];
        a->pkg = &loader->packages[i];
        a->pkgIndex = i;
        a->key = BuildPackageKey(loader, a->pkg);
        a->linkPrefix = BuildPackageLinkPrefix(a->pkg);
        if (a->key == NULL || a->linkPrefix == NULL) {
            goto fail;
        }
        a->cacheDir = JoinPath(cachePkgDir, a->key);
        a->cPath = a->cacheDir != NULL ? JoinPath(a->cacheDir, "pkg.c") : NULL;
        a->oPath = a->cacheDir != NULL ? JoinPath(a->cacheDir, "pkg.o") : NULL;
        a->sigPath = a->cacheDir != NULL ? JoinPath(a->cacheDir, "toolchain.sig") : NULL;
        if (a->cacheDir == NULL || a->cPath == NULL || a->oPath == NULL || a->sigPath == NULL) {
            goto fail;
        }
    }

    if (BuildToolchainSignature(loader, libDir, &toolchainSignature) != 0) {
        goto fail;
    }
    if (BuildPackageTopologicalOrder(loader, topoOrder, artifactLen, &topoOrderLen) != 0) {
        goto fail;
    }
    if (ApplyLinkPrefixImportOverrides(
            loader,
            artifacts,
            artifactLen,
            &aliasOverrides,
            &aliasOverrideLen,
            &symbolOverrides,
            &symbolOverrideLen)
        != 0)
    {
        goto fail;
    }

    for (i = 0; i < topoOrderLen; i++) {
        uint32_t           pkgIndex = topoOrder[i];
        SLPackageArtifact* artifact = &artifacts[pkgIndex];
        if (IsPackageArtifactUpToDate(
                artifact->pkg, artifact, artifacts, artifactLen, toolchainSignature))
        {
            continue;
        }
        if (EmitPackageArtifact(
                loader,
                artifact->pkg,
                artifact,
                artifacts,
                artifactLen,
                libDir,
                cachePkgDir,
                toolchainSignature,
                backend)
            != 0)
        {
            goto fail;
        }
    }

    RestoreImportOverrides(aliasOverrides, aliasOverrideLen, symbolOverrides, symbolOverrideLen);
    free(topoOrder);
    free(toolchainSignature);
    free(cachePkgDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outArtifacts = artifacts;
    *outArtifactLen = artifactLen;
    return 0;

fail:
    RestoreImportOverrides(aliasOverrides, aliasOverrideLen, symbolOverrides, symbolOverrideLen);
    free(topoOrder);
    free(toolchainSignature);
    free(cachePkgDir);
    free(cacheV1Dir);
    free(cacheRoot);
    FreePackageArtifacts(artifacts, artifactLen);
    return -1;
}

static int BuildCachedPlatformObject(
    const SLPackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char* libDir,
    const char* _Nullable platformPath,
    const char* _Nullable toolchainSignature,
    char** _Nullable outPlatformObjPath) {
    char*       cacheRoot = NULL;
    char*       cacheV1Dir = NULL;
    char*       cachePlatformDir = NULL;
    char*       cachePlatformTargetDir = NULL;
    char*       platformObjPath = NULL;
    char*       platformSigPath = NULL;
    uint64_t    srcMtimeNs = 0;
    uint64_t    objMtimeNs = 0;
    const char* ccArgv[12];
    int         isUpToDate = 0;

    if (loader == NULL || loader->platformTarget == NULL || libDir == NULL || platformPath == NULL
        || toolchainSignature == NULL || outPlatformObjPath == NULL)
    {
        return -1;
    }
    *outPlatformObjPath = NULL;
    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cachePlatformDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "platform") : NULL;
    cachePlatformTargetDir =
        cachePlatformDir != NULL ? JoinPath(cachePlatformDir, loader->platformTarget) : NULL;
    platformObjPath =
        cachePlatformTargetDir != NULL ? JoinPath(cachePlatformTargetDir, "platform.o") : NULL;
    platformSigPath =
        cachePlatformTargetDir != NULL ? JoinPath(cachePlatformTargetDir, "toolchain.sig") : NULL;
    if (cacheV1Dir == NULL || cachePlatformDir == NULL || cachePlatformTargetDir == NULL
        || platformObjPath == NULL || platformSigPath == NULL)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    if (EnsureDirRecursive(cachePlatformTargetDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto fail;
    }

    if (ToolchainSignatureMatches(platformSigPath, toolchainSignature)
        && GetFileMtimeNs(platformPath, &srcMtimeNs) == 0
        && GetFileMtimeNs(platformObjPath, &objMtimeNs) == 0 && srcMtimeNs <= objMtimeNs)
    {
        isUpToDate = 1;
    }

    if (!isUpToDate) {
        ccArgv[0] = "cc";
        ccArgv[1] = "-std=c11";
        ccArgv[2] = "-g";
        ccArgv[3] = "-w";
        ccArgv[4] = "-isystem";
        ccArgv[5] = libDir;
        ccArgv[6] = "-c";
        ccArgv[7] = platformPath;
        ccArgv[8] = "-o";
        ccArgv[9] = platformObjPath;
        ccArgv[10] = NULL;
        if (RunCommand(ccArgv) != 0) {
            ErrorSimple("C compilation failed");
            goto fail;
        }
        if (WriteOutput(platformSigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
            != 0)
        {
            ErrorSimple("failed to write cache toolchain signature");
            goto fail;
        }
    }

    free(platformSigPath);
    free(cachePlatformTargetDir);
    free(cachePlatformDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outPlatformObjPath = platformObjPath;
    return 0;

fail:
    free(platformObjPath);
    free(platformSigPath);
    free(cachePlatformTargetDir);
    free(cachePlatformDir);
    free(cacheV1Dir);
    free(cacheRoot);
    return -1;
}

static int BuildCachedBuiltinObject(
    const SLPackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char* libDir,
    const char* _Nullable builtinPath,
    const char* _Nullable builtinHeaderPath,
    const char* _Nullable toolchainSignature,
    char** _Nullable outBuiltinObjPath) {
    char*       cacheRoot = NULL;
    char*       cacheV1Dir = NULL;
    char*       cacheBuiltinDir = NULL;
    char*       cacheBuiltinTargetDir = NULL;
    char*       builtinObjPath = NULL;
    char*       builtinSigPath = NULL;
    uint64_t    srcMtimeNs = 0;
    uint64_t    headerMtimeNs = 0;
    uint64_t    objMtimeNs = 0;
    const char* ccArgv[12];
    int         isUpToDate = 0;

    if (loader == NULL || loader->platformTarget == NULL || libDir == NULL || builtinPath == NULL
        || builtinHeaderPath == NULL || toolchainSignature == NULL || outBuiltinObjPath == NULL)
    {
        return -1;
    }
    *outBuiltinObjPath = NULL;
    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cacheBuiltinDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "builtin") : NULL;
    cacheBuiltinTargetDir =
        cacheBuiltinDir != NULL ? JoinPath(cacheBuiltinDir, loader->platformTarget) : NULL;
    builtinObjPath =
        cacheBuiltinTargetDir != NULL ? JoinPath(cacheBuiltinTargetDir, "builtin.o") : NULL;
    builtinSigPath =
        cacheBuiltinTargetDir != NULL ? JoinPath(cacheBuiltinTargetDir, "toolchain.sig") : NULL;
    if (cacheV1Dir == NULL || cacheBuiltinDir == NULL || cacheBuiltinTargetDir == NULL
        || builtinObjPath == NULL || builtinSigPath == NULL)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    if (EnsureDirRecursive(cacheBuiltinTargetDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto fail;
    }

    if (ToolchainSignatureMatches(builtinSigPath, toolchainSignature)
        && GetFileMtimeNs(builtinPath, &srcMtimeNs) == 0
        && GetFileMtimeNs(builtinHeaderPath, &headerMtimeNs) == 0
        && GetFileMtimeNs(builtinObjPath, &objMtimeNs) == 0 && srcMtimeNs <= objMtimeNs
        && headerMtimeNs <= objMtimeNs)
    {
        isUpToDate = 1;
    }

    if (!isUpToDate) {
        ccArgv[0] = "cc";
        ccArgv[1] = "-std=c11";
        ccArgv[2] = "-g";
        ccArgv[3] = "-w";
        ccArgv[4] = "-isystem";
        ccArgv[5] = libDir;
        ccArgv[6] = "-c";
        ccArgv[7] = builtinPath;
        ccArgv[8] = "-o";
        ccArgv[9] = builtinObjPath;
        ccArgv[10] = NULL;
        if (RunCommand(ccArgv) != 0) {
            ErrorSimple("C compilation failed");
            goto fail;
        }
        if (WriteOutput(builtinSigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
            != 0)
        {
            ErrorSimple("failed to write cache toolchain signature");
            goto fail;
        }
    }

    free(builtinSigPath);
    free(cacheBuiltinTargetDir);
    free(cacheBuiltinDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outBuiltinObjPath = builtinObjPath;
    return 0;

fail:
    free(builtinObjPath);
    free(builtinSigPath);
    free(cacheBuiltinTargetDir);
    free(cacheBuiltinDir);
    free(cacheV1Dir);
    free(cacheRoot);
    return -1;
}

static int BuildCachedWrapperObject(
    const SLPackageLoader* loader,
    const char* _Nullable cacheDirArg,
    const char* libDir,
    const char* wrapperLinkPrefix,
    const char* toolchainSignature,
    char**      outWrapperObjPath) {
    char*           cacheRoot = NULL;
    char*           cacheV1Dir = NULL;
    char*           cacheWrapperDir = NULL;
    char*           cacheWrapperTargetDir = NULL;
    char*           wrapperKey = NULL;
    char*           cacheWrapperEntryDir = NULL;
    char*           wrapperCPath = NULL;
    char*           wrapperOPath = NULL;
    char*           wrapperSigPath = NULL;
    SLStringBuilder wrapperBuilder = { 0 };
    char*           wrapperSource = NULL;
    uint32_t        wrapperSourceLen = 0;
    char*           existingSource = NULL;
    FILE*           existingSourceFile = NULL;
    long            existingSourceFileLen = 0;
    size_t          nread = 0;
    uint64_t        srcMtimeNs = 0;
    uint64_t        objMtimeNs = 0;
    const char*     ccArgv[12];
    int             sourceMatches = 0;
    int             isUpToDate = 0;

    *outWrapperObjPath = NULL;
    if (ResolveCacheRoot(loader, cacheDirArg, &cacheRoot) != 0) {
        return -1;
    }
    cacheV1Dir = JoinPath(cacheRoot, "v1");
    cacheWrapperDir = cacheV1Dir != NULL ? JoinPath(cacheV1Dir, "wrapper") : NULL;
    cacheWrapperTargetDir =
        cacheWrapperDir != NULL ? JoinPath(cacheWrapperDir, loader->platformTarget) : NULL;
    wrapperKey = BuildSanitizedIdent(wrapperLinkPrefix, "entry");
    cacheWrapperEntryDir =
        (cacheWrapperTargetDir != NULL && wrapperKey != NULL)
            ? JoinPath(cacheWrapperTargetDir, wrapperKey)
            : NULL;
    wrapperCPath =
        cacheWrapperEntryDir != NULL ? JoinPath(cacheWrapperEntryDir, "wrapper.c") : NULL;
    wrapperOPath =
        cacheWrapperEntryDir != NULL ? JoinPath(cacheWrapperEntryDir, "wrapper.o") : NULL;
    wrapperSigPath =
        cacheWrapperEntryDir != NULL ? JoinPath(cacheWrapperEntryDir, "toolchain.sig") : NULL;
    if (cacheV1Dir == NULL || cacheWrapperDir == NULL || cacheWrapperTargetDir == NULL
        || wrapperKey == NULL || cacheWrapperEntryDir == NULL || wrapperCPath == NULL
        || wrapperOPath == NULL || wrapperSigPath == NULL)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    if (EnsureDirRecursive(cacheWrapperEntryDir) != 0) {
        ErrorSimple("failed to create cache directory");
        goto fail;
    }

    if (SBAppendCStr(&wrapperBuilder, "#include <builtin/builtin.h>\n\nvoid ") != 0
        || SBAppendCStr(&wrapperBuilder, wrapperLinkPrefix) != 0
        || SBAppendCStr(&wrapperBuilder, "__main(__sl_Context *context);\n\n") != 0
        || SBAppendCStr(&wrapperBuilder, "int sl_main(__sl_Context *context) { ") != 0
        || SBAppendCStr(&wrapperBuilder, wrapperLinkPrefix) != 0
        || SBAppendCStr(&wrapperBuilder, "__main(context); return 0; }\n") != 0)
    {
        ErrorSimple("out of memory");
        goto fail;
    }
    wrapperSource = SBFinish(&wrapperBuilder, NULL);
    wrapperBuilder.v = NULL;
    wrapperBuilder.len = 0;
    wrapperBuilder.cap = 0;
    if (wrapperSource == NULL) {
        ErrorSimple("out of memory");
        goto fail;
    }
    wrapperSourceLen = (uint32_t)strlen(wrapperSource);

    existingSourceFile = fopen(wrapperCPath, "rb");
    if (existingSourceFile != NULL && fseek(existingSourceFile, 0, SEEK_END) == 0) {
        existingSourceFileLen = ftell(existingSourceFile);
        if (existingSourceFileLen >= 0 && (uint64_t)existingSourceFileLen <= (uint64_t)UINT32_MAX
            && (uint32_t)existingSourceFileLen == wrapperSourceLen
            && fseek(existingSourceFile, 0, SEEK_SET) == 0)
        {
            if (wrapperSourceLen == 0) {
                sourceMatches = 1;
            } else {
                existingSource = (char*)malloc((size_t)wrapperSourceLen);
                if (existingSource != NULL) {
                    nread = fread(existingSource, 1u, (size_t)wrapperSourceLen, existingSourceFile);
                    if (nread == (size_t)wrapperSourceLen
                        && memcmp(existingSource, wrapperSource, wrapperSourceLen) == 0)
                    {
                        sourceMatches = 1;
                    }
                }
            }
        }
    }
    if (existingSourceFile != NULL) {
        fclose(existingSourceFile);
    }
    free(existingSource);
    existingSource = NULL;

    if (!sourceMatches) {
        if (WriteOutput(wrapperCPath, wrapperSource, wrapperSourceLen) != 0) {
            ErrorSimple("failed to write wrapper source");
            goto fail;
        }
    }

    if (sourceMatches && ToolchainSignatureMatches(wrapperSigPath, toolchainSignature)
        && GetFileMtimeNs(wrapperCPath, &srcMtimeNs) == 0
        && GetFileMtimeNs(wrapperOPath, &objMtimeNs) == 0 && srcMtimeNs <= objMtimeNs)
    {
        isUpToDate = 1;
    }

    if (!isUpToDate) {
        ccArgv[0] = "cc";
        ccArgv[1] = "-std=c11";
        ccArgv[2] = "-g";
        ccArgv[3] = "-w";
        ccArgv[4] = "-isystem";
        ccArgv[5] = libDir;
        ccArgv[6] = "-c";
        ccArgv[7] = wrapperCPath;
        ccArgv[8] = "-o";
        ccArgv[9] = wrapperOPath;
        ccArgv[10] = NULL;
        if (RunCommand(ccArgv) != 0) {
            ErrorSimple("C compilation failed");
            goto fail;
        }
        if (WriteOutput(wrapperSigPath, toolchainSignature, (uint32_t)strlen(toolchainSignature))
            != 0)
        {
            ErrorSimple("failed to write cache toolchain signature");
            goto fail;
        }
    }

    free(wrapperSource);
    free(wrapperSigPath);
    free(wrapperCPath);
    free(cacheWrapperEntryDir);
    free(wrapperKey);
    free(cacheWrapperTargetDir);
    free(cacheWrapperDir);
    free(cacheV1Dir);
    free(cacheRoot);
    *outWrapperObjPath = wrapperOPath;
    return 0;

fail:
    free(existingSource);
    free(wrapperSource);
    free(wrapperBuilder.v);
    free(wrapperSigPath);
    free(wrapperOPath);
    free(wrapperCPath);
    free(cacheWrapperEntryDir);
    free(wrapperKey);
    free(cacheWrapperTargetDir);
    free(cacheWrapperDir);
    free(cacheV1Dir);
    free(cacheRoot);
    return -1;
}

static int IsLinkedOutputUpToDate(
    const char*              outExe,
    const char*              wrapperObjPath,
    const char*              builtinObjPath,
    const char*              platformObjPath,
    const SLPackageArtifact* artifacts,
    uint32_t                 artifactLen) {
    struct stat outSt;
    uint64_t    outMtimeNs;
    uint64_t    inputMtimeNs;
    uint32_t    i;

    if (stat(outExe, &outSt) != 0 || !S_ISREG(outSt.st_mode)) {
        return 0;
    }
    outMtimeNs = StatMtimeNs(&outSt);

    if (GetFileMtimeNs(wrapperObjPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
        return 0;
    }
    if (GetFileMtimeNs(builtinObjPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
        return 0;
    }
    if (GetFileMtimeNs(platformObjPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
        return 0;
    }
    for (i = 0; i < artifactLen; i++) {
        if (StrEq(artifacts[i].pkg->name, "platform")) {
            continue;
        }
        if (GetFileMtimeNs(artifacts[i].oPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
            return 0;
        }
    }
    return 1;
}

static int IsEvalPlatformTarget(const char* _Nullable platformTarget) {
    return platformTarget != NULL && StrEq(platformTarget, SL_EVAL_PLATFORM_TARGET);
}

static int IsWasmMinPlatformTarget(const char* _Nullable platformTarget) {
    return platformTarget != NULL && StrEq(platformTarget, SL_WASM_MIN_PLATFORM_TARGET);
}

/* Embedded cli-libc platform source — compiled alongside the generated SL
 * package. Provides runtime platform functions via libc and defines main()
 * which calls sl_main(). */
static int CompileProgram(
    const char* entryPath,
    const char* outExe,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg) {
    SLPackageLoader    loader = { 0 };
    SLPackage*         entryPkg = NULL;
    int                loaderReady = 0;
    char*              libDir = NULL;
    char*              platformPath = NULL;
    char*              builtinPath = NULL;
    char*              builtinHeaderPath = NULL;
    char*              platformObjPath = NULL;
    char*              builtinObjPath = NULL;
    char*              wrapperObjPath = NULL;
    char*              toolchainSignature = NULL;
    SLPackageArtifact* artifacts = NULL;
    uint32_t           artifactLen = 0;
    SLPackageArtifact* entryArtifact;
    const char**       ccLinkArgv = NULL;
    uint32_t           i;
    int                rc = -1;

    if (IsWasmMinPlatformTarget(platformTarget)) {
        return ErrorSimple(
            "platform target %s is run-only; use `slc run --platform %s`",
            SL_WASM_MIN_PLATFORM_TARGET,
            SL_WASM_MIN_PLATFORM_TARGET);
    }
    if (!HasCBackendBuild()) {
        return ErrorCBackendDisabled();
    }

    if (LoadAndCheckPackage(entryPath, platformTarget, &loader, &entryPkg) != 0) {
        goto end;
    }
    loaderReady = 1;
    if (ValidateEntryMainSignature(entryPkg) != 0) {
        goto end;
    }
    if (IsEvalPlatformTarget(loader.platformTarget)) {
        ErrorSimple(
            "platform target %s is evaluator-only; use `slc run --platform %s`",
            loader.platformTarget,
            loader.platformTarget);
        goto end;
    }
    if (ResolveLibDir(&libDir) != 0) {
        goto end;
    }
    if (ResolvePlatformPath(libDir, loader.platformTarget, &platformPath) != 0) {
        goto end;
    }
    if (ResolveBuiltinPath(libDir, &builtinPath, &builtinHeaderPath) != 0) {
        goto end;
    }
    if (BuildToolchainSignature(&loader, libDir, &toolchainSignature) != 0) {
        ErrorSimple("out of memory");
        goto end;
    }
    if (BuildCachedPlatformObject(
            &loader, cacheDirArg, libDir, platformPath, toolchainSignature, &platformObjPath)
        != 0)
    {
        goto end;
    }
    if (BuildCachedBuiltinObject(
            &loader,
            cacheDirArg,
            libDir,
            builtinPath,
            builtinHeaderPath,
            toolchainSignature,
            &builtinObjPath)
        != 0)
    {
        goto end;
    }
    if (BuildCachedPackageArtifacts(&loader, cacheDirArg, libDir, &artifacts, &artifactLen) != 0) {
        goto end;
    }
    entryArtifact = FindArtifactByPkg(artifacts, artifactLen, entryPkg);
    if (entryArtifact == NULL) {
        ErrorSimple("internal error: entry package artifact missing");
        goto end;
    }
    if (BuildCachedWrapperObject(
            &loader,
            cacheDirArg,
            libDir,
            entryArtifact->linkPrefix,
            toolchainSignature,
            &wrapperObjPath)
        != 0)
    {
        goto end;
    }

    if (IsLinkedOutputUpToDate(
            outExe, wrapperObjPath, builtinObjPath, platformObjPath, artifacts, artifactLen))
    {
        rc = 0;
        goto end;
    }

    ccLinkArgv = (const char**)calloc((size_t)artifactLen + 12u, sizeof(char*));
    if (ccLinkArgv == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }
    i = 0;
    ccLinkArgv[i++] = "cc";
    ccLinkArgv[i++] = "-std=c11";
    ccLinkArgv[i++] = "-g";
    ccLinkArgv[i++] = "-w";
    ccLinkArgv[i++] = "-isystem";
    ccLinkArgv[i++] = libDir;
    ccLinkArgv[i++] = "-o";
    ccLinkArgv[i++] = outExe;
    ccLinkArgv[i++] = wrapperObjPath;
    {
        uint32_t j;
        for (j = 0; j < artifactLen; j++) {
            if (StrEq(artifacts[j].pkg->name, "platform")) {
                continue;
            }
            ccLinkArgv[i++] = artifacts[j].oPath;
        }
    }
    ccLinkArgv[i++] = builtinObjPath;
    ccLinkArgv[i++] = platformObjPath;
    ccLinkArgv[i] = NULL;
    if (RunCommand(ccLinkArgv) != 0) {
        ErrorSimple("C link failed");
        goto end;
    }

    rc = 0;

end:
    free(ccLinkArgv);
    free(wrapperObjPath);
    free(builtinObjPath);
    free(toolchainSignature);
    free(platformObjPath);
    free(builtinHeaderPath);
    free(builtinPath);
    free(platformPath);
    free(libDir);
    FreePackageArtifacts(artifacts, artifactLen);
    if (loaderReady) {
        FreeLoader(&loader);
    }
    return rc;
}

static int RunProgramC(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg) {
    const char* tmpBase = getenv("TMPDIR");
    char        exeTemplate[PATH_MAX];
    int         n;
    int         fd;
    char* const execArgv[2] = { exeTemplate, NULL };

    if (tmpBase == NULL || tmpBase[0] == '\0') {
        tmpBase = "/tmp";
    }
    n = snprintf(exeTemplate, sizeof(exeTemplate), "%s/slc-run.XXXXXX", tmpBase);
    if (n <= 0 || (size_t)n >= sizeof(exeTemplate)) {
        return ErrorSimple("temporary path too long");
    }
    fd = mkstemp(exeTemplate);
    if (fd < 0) {
        return ErrorSimple("failed to create temporary output file");
    }
    close(fd);
    unlink(exeTemplate);

    if (CompileProgram(entryPath, exeTemplate, platformTarget, cacheDirArg) != 0) {
        unlink(exeTemplate);
        return -1;
    }

    execv(exeTemplate, execArgv);
    unlink(exeTemplate);
    return ErrorSimple("failed to execute compiled program");
}

static int RunProgramWasmMin(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg) {
    const char* tmpBase = getenv("TMPDIR");
    char        wasmTemplate[PATH_MAX];
    int         n;
    int         fd;
    char*       runnerPath = NULL;
    const char* runArgv[4];
    int         exitCode = 0;

    if (!HasWasmBackendBuild()) {
        return ErrorSimple("this slc build was compiled without the Wasm backend");
    }
    if (tmpBase == NULL || tmpBase[0] == '\0') {
        tmpBase = "/tmp";
    }
    n = snprintf(wasmTemplate, sizeof(wasmTemplate), "%s/slc-run-wasm.XXXXXX", tmpBase);
    if (n <= 0 || (size_t)n >= sizeof(wasmTemplate)) {
        return ErrorSimple("temporary path too long");
    }
    fd = mkstemp(wasmTemplate);
    if (fd < 0) {
        return ErrorSimple("failed to create temporary wasm output file");
    }
    close(fd);

    if (GeneratePackage(entryPath, "wasm", wasmTemplate, platformTarget, cacheDirArg) != 0) {
        unlink(wasmTemplate);
        return -1;
    }
    if (ResolveRepoToolPath("tools/wasm_min_runner.js", &runnerPath) != 0) {
        unlink(wasmTemplate);
        return -1;
    }

    runArgv[0] = "node";
    runArgv[1] = runnerPath;
    runArgv[2] = wasmTemplate;
    runArgv[3] = NULL;
    if (RunCommandExitCode(runArgv, &exitCode) != 0) {
        free(runnerPath);
        unlink(wasmTemplate);
        return ErrorSimple("failed to execute wasm-min runner");
    }

    free(runnerPath);
    unlink(wasmTemplate);
    return exitCode;
}

static int RunProgram(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg) {
    if (IsEvalPlatformTarget(platformTarget)) {
        return RunProgramEval(entryPath, platformTarget);
    }
    if (IsWasmMinPlatformTarget(platformTarget)) {
        return RunProgramWasmMin(entryPath, platformTarget, cacheDirArg);
    }
    if (!HasCBackendBuild()) {
        return ErrorCBackendDisabled();
    }
    return RunProgramC(entryPath, platformTarget, cacheDirArg);
}

int main(int argc, char* argv[]) {
    const char* mode = "lex";
    const char* filename = NULL;
    const char* outFilename = NULL;
    const char* platformTarget = SL_DEFAULT_PLATFORM_TARGET;
    const char* cacheDirArg = NULL;
    char        backendName[32];
    int         genpkgMode;
    char*       source;
    uint32_t    sourceLen;
    int         argi = 1;

    if (argc == 2 && StrEq(argv[1], "--version")) {
        PrintVersion();
        return 0;
    }

    while (argi < argc) {
        if (StrEq(argv[argi], "--platform")) {
            if (argi + 1 >= argc) {
                PrintUsage(argv[0]);
                return 2;
            }
            platformTarget = argv[argi + 1];
            argi += 2;
            continue;
        }
        if (StrEq(argv[argi], "--cache-dir")) {
            if (argi + 1 >= argc) {
                PrintUsage(argv[0]);
                return 2;
            }
            cacheDirArg = argv[argi + 1];
            argi += 2;
            continue;
        }
        break;
    }

    if (!IsValidPlatformTargetName(platformTarget)) {
        fprintf(stderr, "invalid platform target: %s\n", platformTarget);
        return 2;
    }

    if (argc - argi >= 1 && StrEq(argv[argi], "compile")) {
        if (argc - argi == 2) {
            return CompileProgram(argv[argi + 1], "a.out", platformTarget, cacheDirArg) == 0
                     ? 0
                     : 1;
        }
        if (argc - argi != 4 || !StrEq(argv[argi + 2], "-o")) {
            PrintUsage(argv[0]);
            return 2;
        }
        return CompileProgram(argv[argi + 1], argv[argi + 3], platformTarget, cacheDirArg) == 0
                 ? 0
                 : 1;
    }
    if (argc - argi >= 1 && StrEq(argv[argi], "run")) {
        if (argc - argi != 2) {
            PrintUsage(argv[0]);
            return 2;
        }
        {
            int runRc = RunProgram(argv[argi + 1], platformTarget, cacheDirArg);
            if (runRc < 0) {
                return 1;
            }
            return runRc;
        }
    }
    if (argc - argi >= 1 && StrEq(argv[argi], "fmt")) {
        return RunFmtCommand(argc - argi - 1, (const char* const*)&argv[argi + 1]);
    }

    if (argc - argi == 1) {
        filename = argv[argi];
    } else if (argc - argi == 2) {
        mode = argv[argi];
        filename = argv[argi + 1];
    } else if (argc - argi == 3) {
        mode = argv[argi];
        filename = argv[argi + 1];
        outFilename = argv[argi + 2];
    } else {
        PrintUsage(argv[0]);
        return 2;
    }

    genpkgMode = ParseGenpkgMode(mode, backendName, sizeof(backendName));
    if (genpkgMode < 0) {
        fprintf(stderr, "unknown mode: %s\n", mode);
        return 2;
    }
    if (genpkgMode == 1) {
        return GeneratePackage(filename, backendName, outFilename, platformTarget, cacheDirArg) == 0
                 ? 0
                 : 1;
    }

    if (mode[0] == 'c' && mode[1] == 'h' && mode[2] == 'e' && mode[3] == 'c' && mode[4] == 'k'
        && mode[5] == 'p' && mode[6] == 'k' && mode[7] == 'g' && mode[8] == '\0')
    {
        if (outFilename != NULL) {
            fprintf(stderr, "unexpected output argument for mode checkpkg\n");
            return 2;
        }
        return CheckPackageDir(filename, platformTarget) == 0 ? 0 : 1;
    }
    if (mode[0] == 'm' && mode[1] == 'i' && mode[2] == 'r' && mode[3] == '\0') {
        if (outFilename != NULL) {
            fprintf(stderr, "unexpected output argument for mode mir\n");
            return 2;
        }
        return DumpMIR(filename, platformTarget) == 0 ? 0 : 1;
    }

    if (outFilename != NULL) {
        fprintf(stderr, "unexpected output argument for mode %s\n", mode);
        return 2;
    }

    if (ReadFile(filename, &source, &sourceLen) != 0) {
        return 1;
    }

    if (mode[0] == 'l' && mode[1] == 'e' && mode[2] == 'x' && mode[3] == '\0') {
        if (DumpTokens(filename, source, sourceLen) != 0) {
            free(source);
            return 1;
        }
    } else if (mode[0] == 'a' && mode[1] == 's' && mode[2] == 't' && mode[3] == '\0') {
        if (DumpAST(filename, source, sourceLen) != 0) {
            free(source);
            return 1;
        }
    } else if (
        mode[0] == 'c' && mode[1] == 'h' && mode[2] == 'e' && mode[3] == 'c' && mode[4] == 'k'
        && mode[5] == '\0')
    {
        if (CheckSource(filename, source, sourceLen) != 0) {
            free(source);
            return 1;
        }
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode);
        free(source);
        return 2;
    }

    free(source);
    return 0;
}

SL_API_END
