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
#include "ctfe_exec.h"
#include "libsl-impl.h"

SL_API_BEGIN

typedef struct {
    char*    path;
    char*    source;
    uint32_t sourceLen;
    void*    arenaMem;
    SLAst    ast;
} SLParsedFile;

struct SLPackage;

typedef struct {
    char* alias; /* internal mangle prefix */
    char* _Nullable bindName;
    char*             path;
    struct SLPackage* target;
    uint32_t          fileIndex;
    uint32_t          start;
    uint32_t          end;
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
    char*      rootDir;
    char*      platformTarget;
    SLPackage* packages;
    uint32_t   packageLen;
    uint32_t   packageCap;
} SLPackageLoader;

typedef struct {
    const char* name;
    const char* replacement;
} SLIdentMap;

typedef struct {
    char*    v;
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
    SLCombinedSourceSpan* spans;
    uint32_t              len;
    uint32_t              cap;
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
    const char* aliasedDeclText,
    const char* localName,
    const char* qualifiedName,
    char**      outShapeKey,
    char**      outWrapperDeclText);
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
static void     FreePackageArtifacts(SLPackageArtifact* artifacts, uint32_t artifactLen);
static uint32_t AstListCount(const SLAst* ast, int32_t listNode);
static int32_t  AstListItemAt(const SLAst* ast, int32_t listNode, uint32_t index);

#define SL_DEFAULT_PLATFORM_TARGET "cli-libc"
#define SL_EVAL_PLATFORM_TARGET    "cli-eval"
#define SL_EVAL_CALL_MAX_DEPTH     128u

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

static int ErrorDiagf(
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

static int ErrorSimple(const char* fmt, ...) {
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
    if (source == NULL || outStart == NULL || outEnd == NULL || e <= s) {
        return 0;
    }
    while (source[s] != '\0' && s < e && !IsIdentifierChar(source[s])) {
        s++;
    }
    while (e > s && !IsIdentifierChar(source[e - 1u])) {
        e--;
    }
    if (e <= s || source[s] == '\0') {
        return 0;
    }
    while (s > 0 && IsIdentifierChar(source[s - 1u])) {
        s--;
    }
    while (source[e] != '\0' && IsIdentifierChar(source[e])) {
        e++;
    }
    if (e <= s || !IsIdentifierStartChar(source[s])) {
        return 0;
    }
    *outStart = s;
    *outEnd = e;
    return 1;
}

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
    uint32_t    locA = diag->start;
    uint32_t    locB = diag->end;
    if (useLineCol && source != NULL) {
        DiagOffsetToLineCol(source, diag->start, &locA, &locB);
    }

    fprintf(stderr, "%s:%u:%u: %s: ", DisplayPath(filename), locA, locB, diagId);

    if (useIdentifierWording && diag->code == SLDiag_UNKNOWN_SYMBOL && source != NULL
        && diag->end > diag->start)
    {
        char* ident = DupSlice(source, diag->start, diag->end);
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
        if (source != NULL && diag->argEnd > diag->argStart) {
            arg = DupSlice(source, diag->argStart, diag->argEnd);
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

static int PrintSLDiag(
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

static void* _Nullable CodegenArenaGrow(
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

static void CodegenArenaFree(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize) {
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
    const char* _Nullable source) {
    uint32_t fileIndexStart = 0;
    uint32_t fileIndexEnd = 0;
    int      startMapped;
    int      endMapped;
    *diagOut = *diagIn;
    if (outFileIndex != NULL) {
        *outFileIndex = 0;
    }
    if (map == NULL || map->len == 0) {
        return;
    }
    startMapped = RemapCombinedOffset(map, diagIn->start, &diagOut->start, &fileIndexStart);
    endMapped = RemapCombinedOffset(map, diagIn->end, &diagOut->end, &fileIndexEnd);
    if (startMapped && outFileIndex != NULL) {
        *outFileIndex = fileIndexStart;
    } else if (endMapped && outFileIndex != NULL) {
        *outFileIndex = fileIndexEnd;
    }
    if (diagIn->argEnd > diagIn->argStart) {
        uint32_t argFileIndex = 0;
        if (!RemapCombinedOffset(map, diagIn->argStart, &diagOut->argStart, &argFileIndex)) {
            diagOut->argStart = diagIn->argStart;
        }
        if (!RemapCombinedOffset(map, diagIn->argEnd, &diagOut->argEnd, &argFileIndex)) {
            diagOut->argEnd = diagIn->argEnd;
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
    if (len == 0) {
        return 0;
    }
    if (SBReserve(b, len) != 0) {
        return -1;
    }
    memcpy(b->v + b->len, s, len);
    b->len += len;
    b->v[b->len] = '\0';
    return 0;
}

static int SBAppendCStr(SLStringBuilder* b, const char* s) {
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

static char* _Nullable JoinPath(const char* a, const char* b) {
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
            } else if (SLFmtCheckLineEqNoWhitespace(
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
    SLDiag    diag = {};
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
    SLDiag        diag = {};
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
    SLDiag   diag = {};
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
    SLDiag   diag = {};

    *outArenaMem = NULL;
    if (outArena != NULL) {
        memset(outArena, 0, sizeof(*outArena));
    }
    outAst->nodes = NULL;
    outAst->len = 0;
    outAst->root = -1;

    arenaCap64 = (uint64_t)(sourceLen + 128u) * (uint64_t)sizeof(SLAstNode) + 65536u;
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
    int                        useLineColDiag;
    const SLCombinedSourceMap* remapMap;
    const char*                remapSource;
} SLTypecheckDiagSinkCtx;

static void TypecheckDiagSink(void* ctx, const SLDiag* diag) {
    SLTypecheckDiagSinkCtx* sink = (SLTypecheckDiagSinkCtx*)ctx;
    SLDiag                  remapped;
    uint32_t                remappedFileIndex = 0;
    const SLDiag*           toPrint = diag;
    const char*             source;
    if (sink == NULL || diag == NULL) {
        return;
    }
    source = sink->source;
    if (sink->remapMap != NULL && sink->remapSource != NULL) {
        RemapCombinedDiag(sink->remapMap, diag, &remapped, &remappedFileIndex, sink->remapSource);
        (void)remappedFileIndex;
        toPrint = &remapped;
        source = sink->remapSource;
    }
    (void)(sink->useLineColDiag
               ? PrintSLDiagLineCol(sink->filename, source, toPrint, 0)
               : PrintSLDiag(sink->filename, source, toPrint, 0));
}

static int CheckSourceEx(
    const char* filename, const char* source, uint32_t sourceLen, int useLineColDiag) {
    void*                  arenaMem;
    SLArena                arena;
    SLAst                  ast;
    SLDiag                 diag = {};
    uint32_t               beforeTypecheckUsed;
    uint32_t               beforeTypecheckCap;
    uint32_t               afterTypecheckUsed;
    uint32_t               afterTypecheckCap;
    SLTypecheckDiagSinkCtx sinkCtx = {
        .filename = filename,
        .source = source,
        .useLineColDiag = useLineColDiag,
        .remapMap = NULL,
        .remapSource = NULL,
    };
    SLTypeCheckOptions checkOptions = {
        .ctx = &sinkCtx,
        .onDiag = TypecheckDiagSink,
        .flags = 0,
    };

    if (ParseSourceEx(filename, source, sourceLen, &ast, &arenaMem, &arena, useLineColDiag) != 0) {
        return -1;
    }

    WarnUnknownFeatureImports(filename, source, &ast);
    beforeTypecheckUsed = ArenaBytesUsed(&arena);
    beforeTypecheckCap = ArenaBytesCapacity(&arena);

    if (SLTypeCheckEx(&arena, &ast, (SLStrView){ source, sourceLen }, &checkOptions, &diag) != 0) {
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
        int diagStatus =
            useLineColDiag
                ? PrintSLDiagLineCol(filename, source, &diag, 1)
                : PrintSLDiag(filename, source, &diag, 1);
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

static int CheckSourceExWithSingleFileRemap(
    const char*                filename,
    const char*                source,
    uint32_t                   sourceLen,
    int                        useLineColDiag,
    const char*                remapSource,
    const SLCombinedSourceMap* remapMap) {
    void*                  arenaMem;
    uint64_t               arenaCap64;
    size_t                 arenaCap;
    SLArena                arena;
    SLAst                  ast;
    SLDiag                 diag = {};
    uint32_t               beforeTypecheckUsed;
    uint32_t               beforeTypecheckCap;
    uint32_t               afterTypecheckUsed;
    uint32_t               afterTypecheckCap;
    SLTypecheckDiagSinkCtx sinkCtx = {
        .filename = filename,
        .source = source,
        .useLineColDiag = useLineColDiag,
        .remapMap = remapMap,
        .remapSource = remapSource,
    };
    SLTypeCheckOptions checkOptions = {
        .ctx = &sinkCtx,
        .onDiag = TypecheckDiagSink,
        .flags = 0,
    };

    arenaCap64 = (uint64_t)(sourceLen + 128u) * (uint64_t)sizeof(SLAstNode) + 65536u;
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
    if (SLParse(&arena, (SLStrView){ source, sourceLen }, NULL, &ast, NULL, &diag) != 0) {
        SLDiag   remappedDiag;
        uint32_t remappedFileIndex = 0;
        RemapCombinedDiag(remapMap, &diag, &remappedDiag, &remappedFileIndex, remapSource);
        (void)remappedFileIndex;
        (void)(useLineColDiag ? PrintSLDiagLineCol(filename, remapSource, &remappedDiag, 0)
                              : PrintSLDiag(filename, remapSource, &remappedDiag, 0));
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
        (void)(useLineColDiag ? PrintSLDiagLineCol(filename, remapSource, &oomDiag, 0)
                              : PrintSLDiag(filename, remapSource, &oomDiag, 0));
        SLArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }

    WarnUnknownFeatureImports(filename, source, &ast);
    beforeTypecheckUsed = ArenaBytesUsed(&arena);
    beforeTypecheckCap = ArenaBytesCapacity(&arena);

    if (SLTypeCheckEx(&arena, &ast, (SLStrView){ source, sourceLen }, &checkOptions, &diag) != 0) {
        SLDiag   remappedDiag;
        uint32_t remappedFileIndex = 0;
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
        RemapCombinedDiag(remapMap, &diag, &remappedDiag, &remappedFileIndex, remapSource);
        (void)remappedFileIndex;
        {
            int diagStatus =
                useLineColDiag
                    ? PrintSLDiagLineCol(filename, remapSource, &remappedDiag, 1)
                    : PrintSLDiag(filename, remapSource, &remappedDiag, 1);
            SLArenaDispose(&arena);
            free(arenaMem);
            return diagStatus;
        }
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

static int CheckSource(const char* filename, const char* source, uint32_t sourceLen) {
    return CheckSourceEx(filename, source, sourceLen, 0);
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

    if (n->kind == SLAst_FN && FnNodeHasBody(&file->ast, nodeId)) {
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
        || SliceEqCStr(src, start, end, "type") || SliceEqCStr(src, start, end, "u8")
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
            uint32_t i;
            uint32_t dotPos = 0;
            int      hasDot = 0;
            for (i = n->dataStart; i < n->dataEnd; i++) {
                if (file->source[i] == '.') {
                    dotPos = i;
                    hasDot = 1;
                    break;
                }
            }
            if (hasDot) {
                const SLImportRef* imp = FindImportByAliasSlice(
                    pkg, file->source, n->dataStart, dotPos);
                if (imp == NULL) {
                    return Errorf(
                        file->path,
                        file->source,
                        n->start,
                        n->end,
                        "public API %s references unknown import alias",
                        contextMsg);
                }
                if (imp->target == NULL) {
                    return 0;
                }
                if (PackageHasExportedTypeSlice(imp->target, file->source, dotPos + 1u, n->dataEnd))
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
                "public API %s references non-exported type",
                contextMsg);
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
        uint32_t i;
        for (i = n->dataStart; i < n->dataEnd; i++) {
            if (file->source[i] == '.') {
                const SLImportRef* imp = FindImportByAliasSlice(pkg, file->source, n->dataStart, i);
                if (imp == NULL) {
                    if (PackageHasEnumDeclBySlice(pkg, file->source, n->dataStart, i)) {
                        break;
                    }
                    return Errorf(
                        file->path, file->source, n->start, n->end, "unknown import alias");
                }
                if (!PackageHasExportSlice(imp->target, file->source, i + 1u, n->dataEnd)) {
                    return Errorf(
                        file->path, file->source, n->start, n->end, "unknown imported symbol");
                }
                break;
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
    if (IsDirectoryPath(candidate)) {
        return candidate;
    }
    free(candidate);
    return NULL;
}

static int IsLibImportPath(const char* importPath) {
    return StrEq(importPath, "core") || StrEq(importPath, "reflect") || StrEq(importPath, "mem")
        || StrEq(importPath, "platform") || StrEq(importPath, "compiler")
        || strncmp(importPath, "std/", 4u) == 0 || strncmp(importPath, "platform/", 9u) == 0;
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

static int IsCorePackage(const SLPackage* pkg) {
    const char* base;
    if (pkg == NULL || pkg->dirPath == NULL) {
        return 0;
    }
    base = strrchr(pkg->dirPath, '/');
    base = base != NULL ? base + 1 : pkg->dirPath;
    return StrEq(base, "core");
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

static int EnsureImplicitCoreImport(SLPackage* pkg) {
    char*    alias = NULL;
    char*    importPath = NULL;
    uint32_t importIndex = 0;
    if (IsCorePackage(pkg)) {
        return 0;
    }
    if (FindImportIndexByPath(pkg, "core") >= 0) {
        return 0;
    }
    alias = MakeUniqueImportAlias(pkg, "core");
    importPath = DupCStr("core");
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
    if (IsCorePackage(pkg) || IsReflectPackage(pkg)) {
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

static int EnsureImplicitCoreImportSymbols(SLPackage* pkg) {
    int coreImportIndex = FindImportIndexByPath(pkg, "core");
    if (coreImportIndex < 0) {
        return 0;
    }
    {
        const SLPackage* dep = pkg->imports[(uint32_t)coreImportIndex].target;
        uint32_t         i;
        if (dep == NULL) {
            return ErrorSimple("internal error: unresolved core import");
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
                if (sym->importIndex == (uint32_t)coreImportIndex
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
                        pkg, (uint32_t)coreImportIndex, sourceName, localName, 0, 0, 0)
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
    if (EnsureImplicitCoreImport(pkg) != 0) {
        return -1;
    }
    if (EnsureImplicitReflectImport(pkg) != 0) {
        return -1;
    }
    for (i = 0; i < pkg->importLen; i++) {
        char* resolvedDir;
        resolvedDir = JoinPath(loader->rootDir, pkg->imports[i].path);
        if (resolvedDir != NULL && IsLibImportPath(pkg->imports[i].path)
            && !IsDirectoryPath(resolvedDir))
        {
            char* libResolved = ResolveLibImportDir(pkg->dirPath, pkg->imports[i].path);
            if (libResolved != NULL) {
                free(resolvedDir);
                resolvedDir = libResolved;
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

    if (EnsureImplicitCoreImportSymbols(pkg) != 0) {
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
        uint32_t i;
        int      hasDot = 0;
        for (i = n->dataStart; i < n->dataEnd; i++) {
            if (file->source[i] == '.') {
                hasDot = 1;
                break;
            }
        }
        if (!hasDot) {
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
        case SLAst_UNARY:
        case SLAst_BINARY:
        case SLAst_CALL:
        case SLAst_CALL_WITH_CONTEXT:
        case SLAst_CONTEXT_OVERLAY:
        case SLAst_CONTEXT_BIND:
        case SLAst_INDEX:
        case SLAst_CAST:
        case SLAst_SIZEOF:
        case SLAst_NEW:
        case SLAst_UNWRAP:
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
    uint8_t* shadowCounts, uint32_t* shadowStack, uint32_t* shadowLen, uint32_t mark) {
    while (*shadowLen > mark) {
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
            int32_t  parts[4];
            uint32_t partCount = 0;
            uint32_t mark = *shadowLen;
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0 && partCount < 4u) {
                parts[partCount++] = child;
                child = ASTNextSibling(&file->ast, child);
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
    const char*    text,
    uint32_t       textLen,
    uint32_t       baseStart,
    SLTextRewrite* rewrites,
    uint32_t       rewriteLen,
    char**         outText) {
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
    SLDiag          diag = {};
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
    maps = (SLIdentMap*)malloc(sizeof(SLIdentMap) * sourcePkg->pubDeclLen);
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
    const char* aliasedDeclText,
    const char* localName,
    const char* qualifiedName,
    char**      outShapeKey,
    char**      outWrapperDeclText) {
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
    uint32_t importLen) {
    SLIdentMap* maps = NULL;
    uint32_t    j;
    int         rc = -1;

    if (sourcePkg->pubDeclLen == 0) {
        return 0;
    }
    maps = (SLIdentMap*)malloc(sizeof(SLIdentMap) * sourcePkg->pubDeclLen);
    if (maps == NULL) {
        return ErrorSimple("out of memory");
    }
    for (j = 0; j < sourcePkg->pubDeclLen; j++) {
        maps[j].name = sourcePkg->pubDecls[j].name;
        maps[j].replacement = NULL;
        if (BuildPrefixedName(alias, sourcePkg->pubDecls[j].name, (char**)&maps[j].replacement)
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
            sourcePkg->pubDeclLen,
            &rewritten);
        if (rc != 0) {
            goto done;
        }
        if (SBAppendCStr(b, rewritten) != 0 || SBAppendCStr(b, "\n") != 0) {
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
        for (j = 0; j < sourcePkg->pubDeclLen; j++) {
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
    const SLEmittedImportSurface* arr, uint32_t len, const SLPackage* pkg, const char* alias) {
    uint32_t i;
    for (i = 0; i < len; i++) {
        if (arr[i].pkg == pkg && StrEq(arr[i].alias, alias)) {
            return 1;
        }
    }
    return 0;
}

static int AppendImportedPackageSurface(
    SLStringBuilder*         b,
    const SLPackage*         dep,
    const char*              alias,
    SLEmittedImportSurface** emitted,
    uint32_t*                emittedLen,
    uint32_t*                emittedCap) {
    uint32_t j;
    if (dep == NULL) {
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
            && AppendAliasedPubDecls(b, subDep, dep->imports[j].alias, NULL, 0) != 0)
        {
            return -1;
        }
        if (!HasEmittedImportSurface(*emitted, *emittedLen, subDep, dep->imports[j].alias)) {
            if (EnsureEmittedImportSurfaceCap(emitted, emittedCap, *emittedLen + 1u) != 0) {
                return ErrorSimple("out of memory");
            }
            (*emitted)[*emittedLen].pkg = subDep;
            (*emitted)[*emittedLen].alias = dep->imports[j].alias;
            (*emittedLen)++;
        }
    }
    if (AppendAliasedPubDecls(b, dep, alias, dep->imports, dep->importLen) != 0) {
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
                &b, dep, pkg->imports[i].alias, &emitted, &emittedLen, &emittedCap)
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

static int CheckLoadedPackage(SLPackageLoader* loader, SLPackage* pkg) {
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
            loader, pkg, &source, &sourceLen, useSingleFileRemap ? &sourceMap : NULL, NULL)
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
                  &sourceMap)
                  != 0
            : CheckSourceEx(checkPath, checkSource, checkSourceLen, lineColDiag) != 0)
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

static void FreeLoader(SLPackageLoader* loader) {
    uint32_t i;
    for (i = 0; i < loader->packageLen; i++) {
        FreePackage(&loader->packages[i]);
    }
    free(loader->packages);
    free(loader->rootDir);
    free(loader->platformTarget);
    memset(loader, 0, sizeof(*loader));
}

static int LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    SLPackageLoader* outLoader,
    SLPackage**      outEntryPkg) {
    char*           canonical = CanonicalizePath(entryPath);
    struct stat     st;
    char*           pkgDir = NULL;
    char*           rootDir;
    SLPackageLoader loader;
    SLPackage*      entryPkg;
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
    (void)entryPkg;

    if (LoadSelectedPlatformTargetPackage(&loader, entryPkg->dirPath, NULL) != 0) {
        free(canonical);
        FreeLoader(&loader);
        return -1;
    }

    for (i = 0; i < loader.packageLen; i++) {
        if (CheckLoadedPackage(&loader, &loader.packages[i]) != 0) {
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

static int ValidateEntryMainSignature(const SLPackage* entryPkg) {
    uint32_t fileIndex;
    int      hasMainDefinition = 0;

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
    SLPackage*      entryPkg;
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
        argv0);
}

static void PrintVersion(void) {
    fprintf(stdout, "SL compiler version %d (%s)\n", SL_VERSION, SL_SOURCE_HASH);
}

static int GeneratePackage(
    const char* entryPath,
    const char* backendName,
    const char* _Nullable outFilename,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg) {
    SLPackageLoader         loader;
    SLPackage*              entryPkg;
    char*                   source = NULL;
    uint32_t                sourceLen = 0;
    char*                   outHeader = NULL;
    SLDiag                  diag = {};
    SLCodegenUnit           unit;
    const SLCodegenBackend* backend;
    (void)cacheDirArg;

    if (LoadAndCheckPackage(entryPath, platformTarget, &loader, &entryPkg) != 0) {
        return -1;
    }

    if (BuildCombinedPackageSource(&loader, entryPkg, &source, &sourceLen, NULL, NULL) != 0) {
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
        return ErrorSimple("unknown backend: %s", backendName);
    }

    unit.packageName = entryPkg->name;
    unit.source = source;
    unit.sourceLen = sourceLen;

    SLCodegenOptions codegenOptions = { 0 };
    codegenOptions.arenaGrow = CodegenArenaGrow;
    codegenOptions.arenaFree = CodegenArenaFree;

    if (backend->emit(backend, &unit, &codegenOptions, &outHeader, &diag) != 0) {
        if (diag.code != SLDiag_NONE) {
            int diagStatus;
            if (entryPkg->fileLen == 1 && entryPkg->importLen == 0) {
                diagStatus = PrintSLDiagLineCol(
                    entryPkg->files[0].path, entryPkg->files[0].source, &diag, 1);
            } else {
                diagStatus = PrintSLDiag(entryPkg->dirPath, source, &diag, 1);
            }
            if (!(diagStatus == 0 && outHeader != NULL)) {
                free(source);
                FreeLoader(&loader);
                return -1;
            }
        } else {
            fprintf(stderr, "error: codegen failed\n");
            free(source);
            FreeLoader(&loader);
            return -1;
        }
    }

    if (WriteOutput(outFilename, outHeader, (uint32_t)strlen(outHeader)) != 0) {
        fprintf(stderr, "error: failed to write output\n");
        free(outHeader);
        free(source);
        FreeLoader(&loader);
        return -1;
    }

    free(outHeader);
    free(source);
    FreeLoader(&loader);
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

static void FreePackageArtifacts(SLPackageArtifact* artifacts, uint32_t artifactLen) {
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

static int ResolvePlatformPath(
    const char* libDir, const char* platformTarget, char** outPlatformPath) {
    char* platformDir = NULL;
    char* platformTargetDir = NULL;
    char* platformPath = NULL;
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

static int ResolveCacheRoot(
    const SLPackageLoader* loader, const char* _Nullable cacheDirArg, char** outCacheRoot) {
    char* cacheRoot;
    *outCacheRoot = NULL;
    if (cacheDirArg != NULL) {
        cacheRoot = MakeAbsolutePathDup(cacheDirArg);
    } else {
        cacheRoot = JoinPath(loader->rootDir, ".sl-cache");
    }
    if (cacheRoot == NULL) {
        return ErrorSimple("out of memory");
    }
    *outCacheRoot = cacheRoot;
    return 0;
}

static int FindPackageIndex(const SLPackageLoader* loader, const SLPackage* pkg) {
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
    SLPackageArtifact* artifacts, uint32_t artifactLen, const SLPackage* pkg) {
    uint32_t i;
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
    SLAliasOverride*        aliasOverrides,
    uint32_t                aliasOverrideLen,
    SLImportSymbolOverride* symbolOverrides,
    uint32_t                symbolOverrideLen) {
    uint32_t i;
    for (i = 0; i < aliasOverrideLen; i++) {
        aliasOverrides[i].imp->alias = aliasOverrides[i].oldAlias;
        free(aliasOverrides[i].newAlias);
    }
    for (i = 0; i < symbolOverrideLen; i++) {
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
    char*            source = NULL;
    uint32_t         sourceLen = 0;
    uint32_t         ownDeclStartOffset = 0;
    SLCodegenUnit    unit;
    SLCodegenOptions codegenOptions = { 0 };
    SLDiag           diag = {};
    char*            outHeader = NULL;
    char*            headerGuard = NULL;
    char*            implMacro = NULL;
    SLStringBuilder  cBuilder = { 0 };
    char*            cSource = NULL;
    const char*      ccArgv[16];
    uint32_t         i;
    int              rc = -1;

    if (PackageHasUnsupportedImportedPubGlobals(pkg) != 0) {
        return -1;
    }
    if (BuildCombinedPackageSource(loader, pkg, &source, &sourceLen, NULL, &ownDeclStartOffset)
        != 0)
    {
        return -1;
    }

    unit.packageName = artifact->linkPrefix;
    unit.source = source;
    unit.sourceLen = sourceLen;
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

    if (backend->emit(backend, &unit, &codegenOptions, &outHeader, &diag) != 0) {
        if (diag.code != SLDiag_NONE) {
            int diagStatus;
            if (pkg->fileLen == 1 && pkg->importLen == 0) {
                diagStatus = PrintSLDiagLineCol(pkg->files[0].path, pkg->files[0].source, &diag, 1);
            } else {
                diagStatus = PrintSLDiag(pkg->dirPath, source, &diag, 1);
            }
            if (!(diagStatus == 0 && outHeader != NULL)) {
                goto end;
            }
        } else {
            ErrorSimple("codegen failed");
            goto end;
        }
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
    if (SBAppendCStr(&cBuilder, outHeader) != 0) {
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
    free(outHeader);
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
    const char* platformPath,
    const char* toolchainSignature,
    char**      outPlatformObjPath) {
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

    if (SBAppendCStr(&wrapperBuilder, "#include <core/core.h>\n\nvoid ") != 0
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
    if (GetFileMtimeNs(platformObjPath, &inputMtimeNs) != 0 || inputMtimeNs > outMtimeNs) {
        return 0;
    }
    for (i = 0; i < artifactLen; i++) {
        if (StrEq(artifacts[i].pkg->name, "core") || StrEq(artifacts[i].pkg->name, "platform")) {
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
    char*              platformObjPath = NULL;
    char*              wrapperObjPath = NULL;
    char*              toolchainSignature = NULL;
    SLPackageArtifact* artifacts = NULL;
    uint32_t           artifactLen = 0;
    SLPackageArtifact* entryArtifact;
    const char**       ccLinkArgv = NULL;
    uint32_t           i;
    int                rc = -1;

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

    if (IsLinkedOutputUpToDate(outExe, wrapperObjPath, platformObjPath, artifacts, artifactLen)) {
        rc = 0;
        goto end;
    }

    ccLinkArgv = (const char**)calloc((size_t)artifactLen + 11u, sizeof(char*));
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
            if (StrEq(artifacts[j].pkg->name, "core") || StrEq(artifacts[j].pkg->name, "platform"))
            {
                continue;
            }
            ccLinkArgv[i++] = artifacts[j].oPath;
        }
    }
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
    free(toolchainSignature);
    free(platformObjPath);
    free(platformPath);
    free(libDir);
    FreePackageArtifacts(artifacts, artifactLen);
    if (loaderReady) {
        FreeLoader(&loader);
    }
    return rc;
}

typedef struct {
    const SLParsedFile* file;
    int32_t             fnNode;
    int32_t             bodyNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint32_t            paramCount;
    uint8_t             hasReturnType;
    uint8_t             hasContextClause;
    uint8_t             _reserved[2];
} SLEvalFunction;

typedef struct {
    SLArena* _Nonnull arena;
    const SLPackage*    entryPkg;
    const SLParsedFile* currentFile;
    SLCTFEExecCtx*      currentExecCtx;
    SLEvalFunction*     funcs;
    uint32_t            funcLen;
    uint32_t            funcCap;
    uint32_t            callDepth;
    uint32_t            callStack[SL_EVAL_CALL_MAX_DEPTH];
    int                 exitCalled;
    int                 exitCode;
} SLEvalProgram;

static void SLEvalValueSetNull(SLCTFEValue* value) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_NULL;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static int SLEvalProgramAppendFunction(SLEvalProgram* p, const SLEvalFunction* fn) {
    SLEvalFunction* newFuncs;
    uint32_t        newCap;
    if (p == NULL || fn == NULL) {
        return -1;
    }
    if (p->funcLen >= p->funcCap) {
        newCap = p->funcCap < 8u ? 8u : p->funcCap * 2u;
        newFuncs = (SLEvalFunction*)realloc(p->funcs, sizeof(SLEvalFunction) * newCap);
        if (newFuncs == NULL) {
            return ErrorSimple("out of memory");
        }
        p->funcs = newFuncs;
        p->funcCap = newCap;
    }
    p->funcs[p->funcLen++] = *fn;
    return 0;
}

static int SLEvalCollectFunctions(SLEvalProgram* p) {
    uint32_t fileIndex;
    if (p == NULL || p->entryPkg == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < p->entryPkg->fileLen; fileIndex++) {
        const SLParsedFile* file = &p->entryPkg->files[fileIndex];
        const SLAst*        ast = &file->ast;
        int32_t             nodeId = ASTFirstChild(ast, ast->root);
        while (nodeId >= 0) {
            const SLAstNode* n = &ast->nodes[nodeId];
            if (n->kind == SLAst_FN) {
                SLEvalFunction fn;
                int32_t        child = ASTFirstChild(ast, nodeId);
                int32_t        bodyNode = -1;
                uint32_t       paramCount = 0;
                uint8_t        hasReturnType = 0;
                uint8_t        hasContextClause = 0;

                while (child >= 0) {
                    const SLAstNode* ch = &ast->nodes[child];
                    if (ch->kind == SLAst_PARAM) {
                        paramCount++;
                    } else if (ch->kind == SLAst_CONTEXT_CLAUSE) {
                        hasContextClause = 1;
                    } else if (IsFnReturnTypeNodeKind(ch->kind) && ch->flags == 1) {
                        hasReturnType = 1;
                    } else if (ch->kind == SLAst_BLOCK) {
                        if (bodyNode >= 0) {
                            return Errorf(
                                file->path,
                                file->source,
                                ch->start,
                                ch->end,
                                "evaluator backend does not support function body shape");
                        }
                        bodyNode = child;
                    }
                    child = ASTNextSibling(ast, child);
                }

                if (bodyNode >= 0) {
                    fn.file = file;
                    fn.fnNode = nodeId;
                    fn.bodyNode = bodyNode;
                    fn.nameStart = n->dataStart;
                    fn.nameEnd = n->dataEnd;
                    fn.paramCount = paramCount;
                    fn.hasReturnType = hasReturnType;
                    fn.hasContextClause = hasContextClause;
                    fn._reserved[0] = 0;
                    fn._reserved[1] = 0;
                    if (SLEvalProgramAppendFunction(p, &fn) != 0) {
                        return -1;
                    }
                }
            }
            nodeId = ASTNextSibling(ast, nodeId);
        }
    }
    return 0;
}

static int32_t SLEvalFindFunctionBySlice(
    const SLEvalProgram* p,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t             argCount) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        if (fn->paramCount != argCount) {
            continue;
        }
        if (!SliceEqSlice(
                callerFile->source,
                nameStart,
                nameEnd,
                fn->file->source,
                fn->nameStart,
                fn->nameEnd))
        {
            continue;
        }
        if (found >= 0) {
            return -2;
        }
        found = (int32_t)i;
    }
    return found;
}

static int SLEvalExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);

static int SLEvalInvokeFunction(
    SLEvalProgram*     p,
    int32_t            fnIndex,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outDidReturn) {
    const SLEvalFunction* fn;
    const SLAst*          ast;
    SLCTFEExecBinding*    paramBindings = NULL;
    SLCTFEExecEnv         paramFrame;
    SLCTFEExecCtx         execCtx;
    const SLParsedFile*   savedFile;
    SLCTFEExecCtx*        savedExecCtx;
    int                   isConst = 0;
    int                   rc;
    int32_t               child;
    uint32_t              paramIndex = 0;

    if (p == NULL || outValue == NULL || outDidReturn == NULL || fnIndex < 0
        || (uint32_t)fnIndex >= p->funcLen)
    {
        return -1;
    }
    fn = &p->funcs[fnIndex];
    ast = &fn->file->ast;
    if (argCount != fn->paramCount) {
        return Errorf(
            fn->file->path,
            fn->file->source,
            ast->nodes[fn->fnNode].start,
            ast->nodes[fn->fnNode].end,
            "evaluator backend call arity mismatch");
    }

    if (argCount > 0) {
        paramBindings = (SLCTFEExecBinding*)SLArenaAlloc(
            p->arena, sizeof(SLCTFEExecBinding) * argCount, (uint32_t)_Alignof(SLCTFEExecBinding));
        if (paramBindings == NULL) {
            return ErrorSimple("out of memory");
        }
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            paramBindings[paramIndex].nameStart = n->dataStart;
            paramBindings[paramIndex].nameEnd = n->dataEnd;
            paramBindings[paramIndex].typeId = -1;
            paramBindings[paramIndex].mutable = 1;
            paramBindings[paramIndex]._reserved[0] = 0;
            paramBindings[paramIndex]._reserved[1] = 0;
            paramBindings[paramIndex]._reserved[2] = 0;
            paramBindings[paramIndex].value = args[paramIndex];
            paramIndex++;
        }
        child = ASTNextSibling(ast, child);
    }

    paramFrame.parent = NULL;
    paramFrame.bindings = paramBindings;
    paramFrame.bindingLen = argCount;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = p->arena;
    execCtx.ast = ast;
    execCtx.src.ptr = fn->file->source;
    execCtx.src.len = fn->file->sourceLen;
    execCtx.env = &paramFrame;
    execCtx.evalExpr = SLEvalExecExprCb;
    execCtx.evalExprCtx = p;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = SLCTFE_EXEC_DEFAULT_FOR_LIMIT;
    SLCTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = fn->file;
    p->currentExecCtx = &execCtx;
    p->callStack[p->callDepth++] = (uint32_t)fnIndex;

    rc = SLCTFEExecEvalBlock(&execCtx, fn->bodyNode, outValue, outDidReturn, &isConst);

    p->callDepth--;
    p->currentExecCtx = savedExecCtx;
    p->currentFile = savedFile;

    if (rc != 0) {
        return -1;
    }
    if (!isConst) {
        uint32_t    errStart = execCtx.nonConstStart;
        uint32_t    errEnd = execCtx.nonConstEnd;
        const char* reason = execCtx.nonConstReason;
        if (reason == NULL || reason[0] == '\0') {
            reason = "statement is not supported by evaluator backend";
        }
        if (errEnd <= errStart) {
            errStart = ast->nodes[fn->fnNode].start;
            errEnd = ast->nodes[fn->fnNode].end;
        }
        return Errorf(
            fn->file->path, fn->file->source, errStart, errEnd, "evaluator backend: %s", reason);
    }
    if (!*outDidReturn) {
        SLEvalValueSetNull(outValue);
    }
    return 0;
}

static int SLEvalResolveIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL || p->currentExecCtx == NULL
        || p->currentFile == NULL)
    {
        return -1;
    }
    if (SLCTFEExecEnvLookup(p->currentExecCtx, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    SLCTFEExecSetReason(
        p->currentExecCtx, nameStart, nameEnd, "identifier is not available in evaluator backend");
    *outIsConst = 0;
    return 0;
}

static int SLEvalResolveCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*        p = (SLEvalProgram*)ctx;
    int32_t               fnIndex;
    const SLEvalFunction* fn;
    int                   didReturn = 0;
    (void)diag;

    if (p == NULL || p->currentFile == NULL || p->currentExecCtx == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }

    fnIndex = SLEvalFindFunctionBySlice(p, p->currentFile, nameStart, nameEnd, argCount);
    if (fnIndex == -2) {
        SLCTFEExecSetReason(
            p->currentExecCtx, nameStart, nameEnd, "call target is ambiguous in evaluator backend");
        *outIsConst = 0;
        return 0;
    }
    if (fnIndex < 0) {
        SLCTFEExecSetReason(
            p->currentExecCtx,
            nameStart,
            nameEnd,
            "call target is not supported by evaluator backend");
        *outIsConst = 0;
        return 0;
    }
    fn = &p->funcs[fnIndex];
    if (fn->hasContextClause) {
        SLCTFEExecSetReason(
            p->currentExecCtx,
            nameStart,
            nameEnd,
            "context functions are not supported by evaluator backend");
        *outIsConst = 0;
        return 0;
    }

    if (p->callDepth >= SL_EVAL_CALL_MAX_DEPTH) {
        SLCTFEExecSetReason(
            p->currentExecCtx, nameStart, nameEnd, "evaluator backend call depth limit exceeded");
        *outIsConst = 0;
        return 0;
    }
    {
        uint32_t i;
        for (i = 0; i < p->callDepth; i++) {
            if (p->callStack[i] == (uint32_t)fnIndex) {
                SLCTFEExecSetReason(
                    p->currentExecCtx,
                    nameStart,
                    nameEnd,
                    "recursive calls are not supported by evaluator backend");
                *outIsConst = 0;
                return 0;
            }
        }
    }

    if (SLEvalInvokeFunction(p, fnIndex, args, argCount, outValue, &didReturn) != 0) {
        return -1;
    }
    if (!didReturn) {
        if (fn->hasReturnType) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "function returned without a value in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
        SLEvalValueSetNull(outValue);
    }
    *outIsConst = 1;
    return 0;
}

static int SLEvalExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLEvalProgram*   p = (SLEvalProgram*)ctx;
    const SLAst*     ast;
    const SLAstNode* n;
    SLDiag           diag = {};
    int              rc;

    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return -1;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == SLAst_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return -1;
        }
        n = &ast->nodes[exprNode];
    }

    if (n->kind == SLAst_CALL) {
        int32_t calleeNode = n->firstChild;
        if (calleeNode >= 0 && ast->nodes[calleeNode].kind == SLAst_FIELD_EXPR) {
            const SLAstNode* callee = &ast->nodes[calleeNode];
            int32_t          baseNode = callee->firstChild;
            int32_t          argNode = ast->nodes[calleeNode].nextSibling;
            if (baseNode >= 0 && argNode >= 0 && ast->nodes[argNode].nextSibling < 0
                && ast->nodes[baseNode].kind == SLAst_IDENT
                && SliceEqCStr(p->currentFile->source, callee->dataStart, callee->dataEnd, "exit")
                && SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "platform"))
            {
                SLCTFEValue argValue;
                int         argIsConst = 0;
                int64_t     exitCode = 0;
                if (SLEvalExecExprCb(p, argNode, &argValue, &argIsConst) != 0) {
                    return -1;
                }
                if (!argIsConst || SLCTFEValueToInt64(&argValue, &exitCode) != 0) {
                    if (p->currentExecCtx != NULL) {
                        SLCTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[argNode].start,
                            ast->nodes[argNode].end,
                            "platform.exit argument must be an integer expression");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                p->exitCalled = 1;
                p->exitCode = (int)(exitCode & 255);
                SLEvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
        }
    }

    rc = SLCTFEEvalExpr(
        p->arena,
        ast,
        (SLStrView){ p->currentFile->source, p->currentFile->sourceLen },
        exprNode,
        SLEvalResolveIdent,
        SLEvalResolveCall,
        p,
        outValue,
        outIsConst,
        &diag);
    if (rc != 0) {
        if (diag.code != SLDiag_NONE) {
            PrintSLDiag(p->currentFile->path, p->currentFile->source, &diag, 1);
        } else {
            ErrorSimple("evaluator backend failed to evaluate expression");
        }
        return -1;
    }
    if (!*outIsConst && p->currentExecCtx != NULL && p->currentExecCtx->nonConstReason == NULL) {
        SLCTFEExecSetReasonNode(
            p->currentExecCtx, exprNode, "expression is not supported by evaluator backend");
    }
    return 0;
}

static int RunProgramEval(const char* entryPath, const char* _Nullable platformTarget) {
    SLPackageLoader loader;
    SLPackage*      entryPkg;
    SLEvalProgram   program;
    SLEvalFunction* mainFn = NULL;
    uint32_t        i;
    int32_t         mainIndex = -1;
    uint8_t         arenaMem[32 * 1024];
    SLArena         arena;
    SLCTFEValue     retValue;
    SLCTFEValue     noArgsValue;
    int             didReturn = 0;
    int             rc = -1;

    if (LoadAndCheckPackage(entryPath, platformTarget, &loader, &entryPkg) != 0) {
        return -1;
    }
    if (ValidateEntryMainSignature(entryPkg) != 0) {
        FreeLoader(&loader);
        return -1;
    }

    SLArenaInit(&arena, arenaMem, (uint32_t)sizeof(arenaMem));
    SLArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    memset(&program, 0, sizeof(program));
    program.arena = &arena;
    program.entryPkg = entryPkg;
    if (SLEvalCollectFunctions(&program) != 0) {
        goto end;
    }
    for (i = 0; i < program.funcLen; i++) {
        SLEvalFunction* fn = &program.funcs[i];
        if (fn->paramCount == 0
            && SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, "main"))
        {
            if (mainIndex >= 0) {
                rc = ErrorSimple("entry package has multiple fn main() definitions");
                goto end;
            }
            mainIndex = (int32_t)i;
        }
    }
    if (mainIndex < 0) {
        rc = ErrorSimple("entry package is missing fn main() definition");
        goto end;
    }
    mainFn = &program.funcs[mainIndex];
    if (mainFn->hasContextClause) {
        rc = Errorf(
            mainFn->file->path,
            mainFn->file->source,
            mainFn->file->ast.nodes[mainFn->fnNode].start,
            mainFn->file->ast.nodes[mainFn->fnNode].end,
            "entrypoint cannot have a context clause in evaluator backend");
        goto end;
    }

    SLEvalValueSetNull(&noArgsValue);
    if (SLEvalInvokeFunction(&program, mainIndex, &noArgsValue, 0, &retValue, &didReturn) != 0) {
        goto end;
    }
    rc = program.exitCalled ? program.exitCode : 0;

end:
    free(program.funcs);
    SLArenaDispose(&arena);
    FreeLoader(&loader);
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

static int RunProgram(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable cacheDirArg) {
    if (IsEvalPlatformTarget(platformTarget)) {
        return RunProgramEval(entryPath, platformTarget);
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
