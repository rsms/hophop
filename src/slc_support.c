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
#include "slc_internal.h"

SL_API_BEGIN

int ASTFirstChild(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

int ASTNextSibling(const SLAst* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].nextSibling;
}

const char* DisplayPath(const char* path) {
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

int Errorf(
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
    fprintf(stderr, "%s:%u:%u: error: %s: ", DisplayPath(file), line, col, "SL0000");
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
    fprintf(stderr, "%s:%u:%u: error: %s: ", DisplayPath(file), line, col, DiagIdOrFallback(code));
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

int SliceEqCStr(const char* s, uint32_t start, uint32_t end, const char* cstr) {
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

int IsFnReturnTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE;
}

int SliceEqSlice(
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

char* _Nullable SLCDupCStr(const char* s) {
    size_t n = strlen(s);
    char*  out = (char*)malloc(n + 1u);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, n + 1u);
    return out;
}

char* _Nullable SLCDupSlice(const char* s, uint32_t start, uint32_t end) {
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
    const char* severity = diag->type == SLDiagType_WARNING ? "warning" : "error";
    const char* hint;
    uint32_t    sourceLen = source != NULL ? (uint32_t)strlen(source) : 0u;
    uint32_t    spanStart = diag->start;
    uint32_t    spanEnd = diag->end;
    uint32_t    argStart = diag->argStart;
    uint32_t    argEnd = diag->argEnd;
    uint32_t    locA = diag->start;
    uint32_t    locB = diag->end;
    uint32_t    hintLocA = diag->start;
    uint32_t    hintLocB = diag->end;
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
        if (diag->relatedEnd > diag->relatedStart) {
            DiagOffsetToLineCol(source, diag->relatedStart, &hintLocA, &hintLocB);
        } else {
            hintLocA = locA;
            hintLocB = locB;
        }
    }

    fprintf(stderr, "%s:%u:%u: %s: %s: ", DisplayPath(filename), locA, locB, severity, diagId);

    if (useIdentifierWording && diag->code == SLDiag_UNKNOWN_SYMBOL && source != NULL
        && spanEnd > spanStart)
    {
        char* ident = SLCDupSlice(source, spanStart, spanEnd);
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
            arg = SLCDupSlice(source, argStart, argEnd);
        } else {
            arg = SLCDupCStr("");
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

    (void)includeHint;
    hint = (diag->hintOverride != NULL && diag->hintOverride[0] != '\0')
             ? diag->hintOverride
             : SLDiagHint(diag->code);
    if (hint != NULL) {
        fprintf(
            stderr,
            "%s:%u:%u: hint: %s: %s\n",
            DisplayPath(filename),
            hintLocA,
            hintLocB,
            diagId,
            hint);
    }
    return diag->type == SLDiagType_WARNING ? 0 : -1;
}

int PrintSLDiag(
    const char* filename, const char* _Nullable source, const SLDiag* diag, int includeHint) {
    return PrintSLDiagEx(filename, source, diag, includeHint, 1, 1);
}

int PrintSLDiagLineCol(
    const char* filename, const char* _Nullable source, const SLDiag* diag, int includeHint) {
    return PrintSLDiagEx(filename, source, diag, includeHint, 1, 1);
}

uint32_t ArenaBytesUsed(const SLArena* arena) {
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

uint32_t ArenaBytesCapacity(const SLArena* arena) {
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

int ArenaDebugEnabled(void) {
    const char* s = getenv("SL_ARENA_DEBUG");
    return s != NULL && s[0] != '\0' && s[0] != '0';
}

int CompactAstInArena(SLArena* arena, SLAst* ast) {
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

int EnsureCap(void** ptr, uint32_t* cap, uint32_t need, size_t elemSize) {
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

void CombinedSourceMapFree(SLCombinedSourceMap* map) {
    if (map == NULL) {
        return;
    }
    free(map->spans);
    map->spans = NULL;
    map->len = 0;
    map->cap = 0;
}

int CombinedSourceMapAdd(
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

int RemapCombinedOffset(
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

void RemapCombinedDiag(
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
                || diagOut->code == SLDiag_LOCAL_PTR_REF_UNINIT
                || diagOut->code == SLDiag_LOCAL_PTR_REF_MAYBE_UNINIT
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

int SBReserve(SLStringBuilder* b, uint32_t extra) {
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

int SBAppend(SLStringBuilder* b, const char* s, uint32_t len) {
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

int SBAppendCStr(SLStringBuilder* b, const char* _Nullable s) {
    if (s == NULL) {
        return -1;
    }
    return SBAppend(b, s, (uint32_t)strlen(s));
}

int SBAppendSlice(SLStringBuilder* b, const char* s, uint32_t start, uint32_t end) {
    if (end < start) {
        return -1;
    }
    return SBAppend(b, s + start, end - start);
}

char* _Nullable SBFinish(SLStringBuilder* b, uint32_t* _Nullable outLen) {
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

char* _Nullable JoinPath(const char* _Nullable a, const char* _Nullable b) {
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

char* _Nullable DirNameDup(const char* path) {
    const char* slash = strrchr(path, '/');
    char*       out;
    size_t      len;
    if (slash == NULL) {
        return SLCDupCStr(".");
    }
    len = (size_t)(slash - path);
    if (len == 0) {
        return SLCDupCStr("/");
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
char* _Nullable GetExeDir(void) {
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

uint64_t StatMtimeNs(const struct stat* st) {
#if defined(__APPLE__)
    return (uint64_t)st->st_mtimespec.tv_sec * 1000000000ull + (uint64_t)st->st_mtimespec.tv_nsec;
#elif defined(__linux__)
    return (uint64_t)st->st_mtim.tv_sec * 1000000000ull + (uint64_t)st->st_mtim.tv_nsec;
#else
    return (uint64_t)st->st_mtime * 1000000000ull;
#endif
}

int GetFileMtimeNs(const char* path, uint64_t* outMtimeNs) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return -1;
    }
    *outMtimeNs = StatMtimeNs(&st);
    return 0;
}

int EnsureDirPath(const char* path) {
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

int EnsureDirRecursive(const char* path) {
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

char* _Nullable MakeAbsolutePathDup(const char* path) {
    char cwd[PATH_MAX];
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    if (path[0] == '/') {
        return SLCDupCStr(path);
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        return NULL;
    }
    return JoinPath(cwd, path);
}

uint64_t HashFNV1a64(const char* s) {
    uint64_t h = 1469598103934665603ull;
    size_t   i;
    for (i = 0; s[i] != '\0'; i++) {
        __uint128_t x = (((__uint128_t)h) ^ (uint8_t)s[i]) * 1099511628211ull;
        h = (uint64_t)x;
    }
    return h;
}

char* _Nullable BuildSanitizedIdent(const char* s, const char* fallback) {
    size_t i;
    size_t len;
    char*  out;
    if (s == NULL || s[0] == '\0') {
        return SLCDupCStr(fallback);
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
        return SLCDupCStr(fallback);
    }
    return out;
}

int HasSuffix(const char* s, const char* suffix) {
    size_t sLen = strlen(s);
    size_t suffixLen = strlen(suffix);
    if (sLen < suffixLen) {
        return 0;
    }
    return memcmp(s + (sLen - suffixLen), suffix, suffixLen) == 0;
}

int IsValidIdentifier(const char* s) {
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

int IsValidPlatformTargetName(const char* s) {
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

int IsReservedSLPrefixName(const char* s) {
    return s != NULL && strncmp(s, "__sl_", 5u) == 0;
}

char* _Nullable BaseNameDup(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return SLCDupCStr(path);
    }
    return SLCDupCStr(slash + 1);
}

char* _Nullable LastPathComponentDup(const char* path) {
    size_t      len = strlen(path);
    const char* end = path + len;
    const char* start = path;
    while (end > path && end[-1] == '/') {
        end--;
    }
    if (end == path) {
        return SLCDupCStr("");
    }
    start = end;
    while (start > path && start[-1] != '/') {
        start--;
    }
    len = (size_t)(end - start);
    if (len == 0) {
        return SLCDupCStr("");
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

char* _Nullable StripSLExtensionDup(const char* filename) {
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
    return SLCDupCStr(filename);
}

int CompareStringPtrs(const void* a, const void* b) {
    const char* const* sa = (const char* const*)a;
    const char* const* sb = (const char* const*)b;
    return strcmp(*sa, *sb);
}

int ListSLFiles(const char* dirPath, char*** outFiles, uint32_t* outLen) {
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

int ReadFile(const char* filename, char** outData, uint32_t* outLen) {
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

int ListTopLevelSLFilesForFmt(const char* dirPath, char*** outFiles, uint32_t* outLen) {
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

int WriteFileAtomic(const char* filename, const char* data, uint32_t len) {
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

typedef struct {
    const SLPackageLoader* loader;
    const SLPackage*       pkg;
    const SLParsedFile*    file;
} SLFmtLiteralCastCtx;

typedef struct {
    const SLPackage*    pkg;
    const SLParsedFile* file;
    int32_t             nodeId;
} SLFmtFnCandidate;

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    int32_t  typeNodeId;
} SLFmtFnParam;

static char* _Nullable CanonicalizeFmtPath(const char* path) {
    return realpath(path, NULL);
}

static int32_t SLFmtFindParentNode(const SLAst* ast, int32_t childNodeId) {
    uint32_t i;
    if (childNodeId < 0 || (uint32_t)childNodeId >= ast->len) {
        return -1;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t cur = ast->nodes[i].firstChild;
        while (cur >= 0) {
            if (cur == childNodeId) {
                return (int32_t)i;
            }
            cur = ast->nodes[cur].nextSibling;
        }
    }
    return -1;
}

static int IsPlatformImportPathFmt(const char* _Nullable path) {
    return path != NULL && (StrEq(path, "platform") || strncmp(path, "platform/", 9u) == 0);
}

static int IsSelectedPlatformImportPathFmt(
    const SLPackageLoader* loader, const char* _Nullable path) {
    size_t prefixLen = 9u;
    if (loader == NULL || loader->platformTarget == NULL || path == NULL) {
        return 0;
    }
    if (StrEq(path, "platform")) {
        return 1;
    }
    return strncmp(path, "platform/", prefixLen) == 0
        && StrEq(path + prefixLen, loader->platformTarget);
}

static const SLPackage* _Nullable EffectiveFmtImportTargetPackage(
    const SLPackageLoader* loader, const SLImportRef* imp) {
    if (imp == NULL) {
        return NULL;
    }
    if (loader != NULL && loader->selectedPlatformPkg != NULL
        && IsSelectedPlatformImportPathFmt(loader, imp->path))
    {
        return loader->selectedPlatformPkg;
    }
    return imp->target;
}

static const SLImportSymbolRef* _Nullable FindImportFunctionSymbolBySliceFmt(
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

static int ASTHasCallLiteralCast(const SLAst* ast) {
    uint32_t i;
    if (ast == NULL || ast->nodes == NULL) {
        return 0;
    }
    for (i = 0; i < ast->len; i++) {
        int32_t parentNodeId;
        if (ast->nodes[i].kind != SLAst_CAST) {
            continue;
        }
        parentNodeId = SLFmtFindParentNode(ast, (int32_t)i);
        if (parentNodeId < 0) {
            continue;
        }
        if (ast->nodes[parentNodeId].kind == SLAst_CALL) {
            return 1;
        }
        if (ast->nodes[parentNodeId].kind == SLAst_CALL_ARG) {
            int32_t callNodeId = SLFmtFindParentNode(ast, parentNodeId);
            if (callNodeId >= 0 && ast->nodes[callNodeId].kind == SLAst_CALL) {
                return 1;
            }
        }
    }
    return 0;
}

static int SourceNeedsPackageLiteralCastRewrite(
    SLArena* arena, const char* source, uint32_t sourceLen) {
    SLAst          ast = { 0 };
    SLParseOptions parseOptions = { 0 };
    if (arena == NULL || source == NULL) {
        return 0;
    }
    if (SLParse(arena, (SLStrView){ source, sourceLen }, &parseOptions, &ast, NULL, NULL) != 0) {
        return 0;
    }
    return ASTHasCallLiteralCast(&ast);
}

static int StrSliceEqSlice(
    const char* a, uint32_t aStart, uint32_t aEnd, const char* b, uint32_t bStart, uint32_t bEnd) {
    uint32_t len = aEnd - aStart;
    return len == (bEnd - bStart) && (len == 0 || memcmp(a + aStart, b + bStart, len) == 0);
}

static int SLFmtFindFileByPath(
    const SLPackageLoader* loader,
    const char*            canonicalPath,
    const SLPackage**      outPkg,
    const SLParsedFile**   outFile) {
    uint32_t pkgIndex;
    if (outPkg != NULL) {
        *outPkg = NULL;
    }
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (loader == NULL || canonicalPath == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (pkg->files[fileIndex].path != NULL
                && StrEq(pkg->files[fileIndex].path, canonicalPath))
            {
                if (outPkg != NULL) {
                    *outPkg = pkg;
                }
                if (outFile != NULL) {
                    *outFile = &pkg->files[fileIndex];
                }
                return 1;
            }
        }
    }
    return 0;
}

static int SLFmtAddFnCandidate(
    SLFmtFnCandidate*   candidates,
    uint32_t            cap,
    uint32_t*           len,
    const SLPackage*    pkg,
    const SLParsedFile* file,
    int32_t             nodeId) {
    uint32_t i;
    if (candidates == NULL || len == NULL || pkg == NULL || file == NULL || nodeId < 0) {
        return -1;
    }
    for (i = 0; i < *len; i++) {
        if (candidates[i].file == file && candidates[i].nodeId == nodeId) {
            return 0;
        }
    }
    if (*len >= cap) {
        return -1;
    }
    candidates[*len].pkg = pkg;
    candidates[*len].file = file;
    candidates[*len].nodeId = nodeId;
    (*len)++;
    return 0;
}

static int SLFmtCollectFunctionCandidatesFromDecls(
    const SLPackage*    pkg,
    const SLSymbolDecl* decls,
    uint32_t            declLen,
    const char*         name,
    SLFmtFnCandidate*   candidates,
    uint32_t            cap,
    uint32_t*           len) {
    uint32_t i;
    if (pkg == NULL || decls == NULL || name == NULL) {
        return 0;
    }
    for (i = 0; i < declLen; i++) {
        const SLSymbolDecl* decl = &decls[i];
        if (decl->kind != SLAst_FN || !StrEq(decl->name, name) || decl->fileIndex >= pkg->fileLen) {
            continue;
        }
        if (SLFmtAddFnCandidate(
                candidates, cap, len, pkg, &pkg->files[decl->fileIndex], decl->nodeId)
            != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int SLFmtCollectFunctionCandidatesBySlice(
    const SLPackage*    pkg,
    const SLSymbolDecl* decls,
    uint32_t            declLen,
    const char*         src,
    uint32_t            start,
    uint32_t            end,
    SLFmtFnCandidate*   candidates,
    uint32_t            cap,
    uint32_t*           len) {
    uint32_t i;
    for (i = 0; i < declLen; i++) {
        const SLSymbolDecl* decl = &decls[i];
        if (decl->kind != SLAst_FN || decl->fileIndex >= pkg->fileLen) {
            continue;
        }
        if (strlen(decl->name) != (size_t)(end - start)
            || memcmp(decl->name, src + start, (size_t)(end - start)) != 0)
        {
            continue;
        }
        if (SLFmtAddFnCandidate(
                candidates, cap, len, pkg, &pkg->files[decl->fileIndex], decl->nodeId)
            != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int SLFmtCollectFnParams(
    const SLParsedFile* file,
    int32_t             fnNodeId,
    SLFmtFnParam*       params,
    uint32_t            cap,
    uint32_t*           len,
    int*                outHasVariadic) {
    int32_t child;
    if (len != NULL) {
        *len = 0;
    }
    if (outHasVariadic != NULL) {
        *outHasVariadic = 0;
    }
    if (file == NULL || params == NULL || len == NULL || fnNodeId < 0
        || (uint32_t)fnNodeId >= file->ast.len || file->ast.nodes[fnNodeId].kind != SLAst_FN)
    {
        return 0;
    }
    child = ASTFirstChild(&file->ast, fnNodeId);
    while (child >= 0 && file->ast.nodes[child].kind == SLAst_PARAM) {
        const SLAstNode* paramNode = &file->ast.nodes[child];
        int32_t          typeNodeId = ASTFirstChild(&file->ast, child);
        if (*len >= cap || typeNodeId < 0) {
            return 0;
        }
        params[*len].nameStart = paramNode->dataStart;
        params[*len].nameEnd = paramNode->dataEnd;
        params[*len].typeNodeId = typeNodeId;
        if (outHasVariadic != NULL && (paramNode->flags & SLAstFlag_PARAM_VARIADIC) != 0) {
            *outHasVariadic = 1;
        }
        (*len)++;
        child = ASTNextSibling(&file->ast, child);
    }
    return 1;
}

static int SLFmtMapArgToParamTypeNode(
    const SLParsedFile* callFile,
    int32_t             callNodeId,
    int32_t             targetArgNodeId,
    const SLParsedFile* fnFile,
    int32_t             fnNodeId,
    int32_t*            outTypeNodeId) {
    SLFmtFnParam params[128];
    int32_t      argNodes[128];
    uint32_t     paramCount = 0;
    uint32_t     argCount = 0;
    uint32_t     targetArgIndex = UINT32_MAX;
    uint32_t     firstNamedIndex = UINT32_MAX;
    uint32_t     i;
    int          hasVariadic = 0;
    int32_t      cur;
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = -1;
    }
    if (callFile == NULL || fnFile == NULL || outTypeNodeId == NULL || callNodeId < 0
        || (uint32_t)callNodeId >= callFile->ast.len
        || !SLFmtCollectFnParams(fnFile, fnNodeId, params, 128u, &paramCount, &hasVariadic)
        || hasVariadic)
    {
        return 0;
    }
    cur = ASTFirstChild(&callFile->ast, callNodeId);
    cur = cur >= 0 ? ASTNextSibling(&callFile->ast, cur) : -1;
    while (cur >= 0) {
        const SLAstNode* argNode = &callFile->ast.nodes[cur];
        if (argCount >= 128u || (argNode->flags & SLAstFlag_CALL_ARG_SPREAD) != 0) {
            return 0;
        }
        argNodes[argCount] = cur;
        if (cur == targetArgNodeId) {
            targetArgIndex = argCount;
        }
        if (firstNamedIndex == UINT32_MAX && argNode->kind == SLAst_CALL_ARG
            && argNode->dataEnd > argNode->dataStart)
        {
            firstNamedIndex = argCount;
        }
        argCount++;
        cur = ASTNextSibling(&callFile->ast, cur);
    }
    if (targetArgIndex == UINT32_MAX || argCount != paramCount) {
        return 0;
    }
    if (firstNamedIndex == UINT32_MAX || targetArgIndex < firstNamedIndex) {
        *outTypeNodeId = params[targetArgIndex].typeNodeId;
        return 1;
    }
    for (i = firstNamedIndex; i < argCount; i++) {
        const SLAstNode* argNode = &callFile->ast.nodes[argNodes[i]];
        uint32_t         p;
        if (argNode->kind != SLAst_CALL_ARG || argNode->dataEnd <= argNode->dataStart) {
            return 0;
        }
        for (p = firstNamedIndex; p < paramCount; p++) {
            if (StrSliceEqSlice(
                    callFile->source,
                    argNode->dataStart,
                    argNode->dataEnd,
                    fnFile->source,
                    params[p].nameStart,
                    params[p].nameEnd))
            {
                if (i == targetArgIndex) {
                    *outTypeNodeId = params[p].typeNodeId;
                    return 1;
                }
                break;
            }
        }
        if (p == paramCount) {
            return 0;
        }
    }
    return 0;
}

static int SLFmtResolveCallTarget(
    const SLFmtLiteralCastCtx* ctx,
    const SLParsedFile*        callFile,
    int32_t                    callNodeId,
    char*                      nameBuf,
    size_t                     nameBufCap,
    const SLPackage**          outTargetPkg,
    uint32_t*                  outNameStart,
    uint32_t*                  outNameEnd,
    const char**               outNameCStr) {
    const SLAstNode* calleeNode;
    int32_t          calleeNodeId;
    if (outTargetPkg != NULL) {
        *outTargetPkg = NULL;
    }
    if (outNameStart != NULL) {
        *outNameStart = 0;
    }
    if (outNameEnd != NULL) {
        *outNameEnd = 0;
    }
    if (outNameCStr != NULL) {
        *outNameCStr = NULL;
    }
    if (ctx == NULL || ctx->pkg == NULL || callFile == NULL || ctx->loader == NULL || callNodeId < 0
        || (uint32_t)callNodeId >= callFile->ast.len)
    {
        return 0;
    }
    calleeNodeId = ASTFirstChild(&callFile->ast, callNodeId);
    if (calleeNodeId < 0 || (uint32_t)calleeNodeId >= callFile->ast.len) {
        return 0;
    }
    calleeNode = &callFile->ast.nodes[calleeNodeId];
    if (calleeNode->kind == SLAst_IDENT) {
        const SLImportSymbolRef* sym = FindImportFunctionSymbolBySliceFmt(
            ctx->pkg, callFile->source, calleeNode->dataStart, calleeNode->dataEnd);
        if (sym != NULL && sym->importIndex < ctx->pkg->importLen) {
            const SLPackage* targetPkg = EffectiveFmtImportTargetPackage(
                ctx->loader, &ctx->pkg->imports[sym->importIndex]);
            if (targetPkg == NULL || outTargetPkg == NULL || outNameCStr == NULL) {
                return 0;
            }
            *outTargetPkg = targetPkg;
            *outNameCStr = sym->sourceName;
            return 1;
        }
        if (outTargetPkg == NULL || outNameStart == NULL || outNameEnd == NULL) {
            return 0;
        }
        *outTargetPkg = ctx->pkg;
        *outNameStart = calleeNode->dataStart;
        *outNameEnd = calleeNode->dataEnd;
        return 1;
    }
    if (calleeNode->kind == SLAst_FIELD_EXPR) {
        int32_t recvNodeId = ASTFirstChild(&callFile->ast, calleeNodeId);
        if (recvNodeId >= 0 && (uint32_t)recvNodeId < callFile->ast.len
            && callFile->ast.nodes[recvNodeId].kind == SLAst_IDENT)
        {
            const SLAstNode*   recvNode = &callFile->ast.nodes[recvNodeId];
            const SLImportRef* imp = FindImportByAliasSlice(
                ctx->pkg, callFile->source, recvNode->dataStart, recvNode->dataEnd);
            size_t len;
            if (imp == NULL || outTargetPkg == NULL || outNameCStr == NULL || nameBuf == NULL
                || nameBufCap == 0)
            {
                return 0;
            }
            len = (size_t)(calleeNode->dataEnd - calleeNode->dataStart);
            if (len + 1u > nameBufCap) {
                return 0;
            }
            memcpy(nameBuf, callFile->source + calleeNode->dataStart, len);
            nameBuf[len] = '\0';
            *outTargetPkg = EffectiveFmtImportTargetPackage(ctx->loader, imp);
            *outNameCStr = nameBuf;
            return *outTargetPkg != NULL;
        }
    }
    return 0;
}

static int SLFmtCastIsNumericLiteral(const SLAst* ast, int32_t castNodeId, int32_t* outTypeNodeId) {
    int32_t exprNodeId;
    int32_t typeNodeId;
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = -1;
    }
    if (ast == NULL || castNodeId < 0 || (uint32_t)castNodeId >= ast->len
        || ast->nodes[castNodeId].kind != SLAst_CAST)
    {
        return 0;
    }
    exprNodeId = ASTFirstChild(ast, castNodeId);
    typeNodeId = exprNodeId >= 0 ? ASTNextSibling(ast, exprNodeId) : -1;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= ast->len || exprNodeId < 0
        || (uint32_t)exprNodeId >= ast->len)
    {
        return 0;
    }
    if (ast->nodes[exprNodeId].kind != SLAst_INT && ast->nodes[exprNodeId].kind != SLAst_FLOAT) {
        return 0;
    }
    if (outTypeNodeId != NULL) {
        *outTypeNodeId = typeNodeId;
    }
    return 1;
}

static int SLFmtTypeNodesEqualAcrossFiles(
    const SLAst*        ast,
    SLStrView           src,
    int32_t             castTypeNodeId,
    const SLParsedFile* file,
    int32_t             otherTypeNodeId) {
    const SLAstNode* castTypeNode;
    const SLAstNode* otherTypeNode;
    uint32_t         castLen;
    uint32_t         otherLen;
    if (ast == NULL || file == NULL || castTypeNodeId < 0 || otherTypeNodeId < 0
        || (uint32_t)castTypeNodeId >= ast->len || (uint32_t)otherTypeNodeId >= file->ast.len)
    {
        return 0;
    }
    castTypeNode = &ast->nodes[castTypeNodeId];
    otherTypeNode = &file->ast.nodes[otherTypeNodeId];
    castLen = castTypeNode->dataEnd - castTypeNode->dataStart;
    otherLen = otherTypeNode->dataEnd - otherTypeNode->dataStart;
    return castLen == otherLen
        && memcmp(
               src.ptr + castTypeNode->dataStart, file->source + otherTypeNode->dataStart, castLen)
               == 0;
}

static int SLFmtCanDropLiteralCastViaPackage(
    void* _Nullable opaqueCtx, const SLAst* ast, SLStrView src, int32_t castNodeId) {
    const SLFmtLiteralCastCtx* ctx = (const SLFmtLiteralCastCtx*)opaqueCtx;
    SLParsedFile               callFile = { 0 };
    SLFmtFnCandidate           candidates[64];
    uint32_t                   candidateLen = 0;
    const SLPackage*           targetPkg = NULL;
    const char*                targetNameCStr = NULL;
    uint32_t                   targetNameStart = 0;
    uint32_t                   targetNameEnd = 0;
    char                       targetNameBuf[256];
    int32_t                    parentNodeId;
    int32_t                    callNodeId;
    int32_t                    argNodeId;
    int32_t                    castTypeNodeId = -1;
    uint32_t                   i;
    int                        sawMappedCandidate = 0;
    if (ctx == NULL || ctx->pkg == NULL || ctx->loader == NULL || ast == NULL || src.ptr == NULL) {
        return 0;
    }
    if (!SLFmtCastIsNumericLiteral(ast, castNodeId, &castTypeNodeId)) {
        return 0;
    }
    callFile.source = (char*)src.ptr;
    callFile.sourceLen = src.len;
    callFile.ast = *ast;
    parentNodeId = SLFmtFindParentNode(ast, castNodeId);
    if (parentNodeId < 0) {
        return 0;
    }
    if (ast->nodes[parentNodeId].kind == SLAst_CALL_ARG) {
        argNodeId = parentNodeId;
        callNodeId = SLFmtFindParentNode(ast, parentNodeId);
    } else if (ast->nodes[parentNodeId].kind == SLAst_CALL) {
        argNodeId = castNodeId;
        callNodeId = parentNodeId;
    } else {
        return 0;
    }
    if (callNodeId < 0 || ast->nodes[callNodeId].kind != SLAst_CALL) {
        return 0;
    }
    if (!SLFmtResolveCallTarget(
            ctx,
            &callFile,
            callNodeId,
            targetNameBuf,
            sizeof(targetNameBuf),
            &targetPkg,
            &targetNameStart,
            &targetNameEnd,
            &targetNameCStr))
    {
        return 0;
    }
    if (targetNameCStr != NULL) {
        if (SLFmtCollectFunctionCandidatesFromDecls(
                targetPkg,
                targetPkg->pubDecls,
                targetPkg->pubDeclLen,
                targetNameCStr,
                candidates,
                64u,
                &candidateLen)
            != 0)
        {
            return 0;
        }
        if (targetPkg == ctx->pkg
            && SLFmtCollectFunctionCandidatesFromDecls(
                   targetPkg,
                   targetPkg->decls,
                   targetPkg->declLen,
                   targetNameCStr,
                   candidates,
                   64u,
                   &candidateLen)
                   != 0)
        {
            return 0;
        }
    } else {
        if (SLFmtCollectFunctionCandidatesBySlice(
                targetPkg,
                targetPkg->pubDecls,
                targetPkg->pubDeclLen,
                callFile.source,
                targetNameStart,
                targetNameEnd,
                candidates,
                64u,
                &candidateLen)
            != 0)
        {
            return 0;
        }
        if (targetPkg == ctx->pkg
            && SLFmtCollectFunctionCandidatesBySlice(
                   targetPkg,
                   targetPkg->decls,
                   targetPkg->declLen,
                   callFile.source,
                   targetNameStart,
                   targetNameEnd,
                   candidates,
                   64u,
                   &candidateLen)
                   != 0)
        {
            return 0;
        }
    }
    for (i = 0; i < candidateLen; i++) {
        int32_t paramTypeNodeId = -1;
        if (!SLFmtMapArgToParamTypeNode(
                &callFile,
                callNodeId,
                argNodeId,
                candidates[i].file,
                candidates[i].nodeId,
                &paramTypeNodeId))
        {
            continue;
        }
        sawMappedCandidate = 1;
        if (!SLFmtTypeNodesEqualAcrossFiles(
                ast, src, castTypeNodeId, candidates[i].file, paramTypeNodeId))
        {
            return 0;
        }
    }
    return sawMappedCandidate;
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
    char*               source = NULL;
    uint32_t            sourceLen = 0;
    uint64_t            arenaCap64;
    size_t              arenaCap;
    void*               arenaMem = NULL;
    SLArena             arena;
    SLDiag              diag = { 0 };
    SLStrView           formatted = { 0 };
    SLFormatOptions     formatOptions = { 0 };
    SLFmtLiteralCastCtx fmtCastCtx = { 0 };
    SLPackageLoader     fmtLoader = { 0 };
    char*               canonicalPath = NULL;
    char*               packagePath = NULL;
    SLPackage*          fmtEntryPkg = NULL;
    const SLPackage*    fmtPkg = NULL;
    const SLParsedFile* fmtFile = NULL;
    int                 changed = 0;
    int                 rc = -1;

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

    if (SourceNeedsPackageLiteralCastRewrite(&arena, source, sourceLen)) {
        canonicalPath = CanonicalizeFmtPath(filename);
        packagePath = canonicalPath != NULL ? DirNameDup(canonicalPath) : NULL;
        if (packagePath != NULL
            && LoadPackageForFmt(packagePath, SL_DEFAULT_PLATFORM_TARGET, &fmtLoader, &fmtEntryPkg)
                   == 0
            && fmtEntryPkg != NULL
            && SLFmtFindFileByPath(&fmtLoader, canonicalPath, &fmtPkg, &fmtFile))
        {
            fmtCastCtx.loader = &fmtLoader;
            fmtCastCtx.pkg = fmtPkg;
            fmtCastCtx.file = fmtFile;
            formatOptions.ctx = &fmtCastCtx;
            formatOptions.canDropLiteralCast = SLFmtCanDropLiteralCastViaPackage;
        } else {
            FreeLoader(&fmtLoader);
        }
    }

    if (SLFormat(
            &arena,
            (SLStrView){ source, sourceLen },
            formatOptions.canDropLiteralCast != NULL ? &formatOptions : NULL,
            &formatted,
            &diag)
        != 0)
    {
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
    FreeLoader(&fmtLoader);
    free(packagePath);
    free(canonicalPath);
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
    dup = SLCDupCStr(path);
    if (dup == NULL) {
        return -1;
    }
    (*outFiles)[(*outLen)++] = dup;
    return 0;
}

int RunFmtCommand(int argc, const char* const* argv) {
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

void StdoutWrite(void* ctx, const char* data, uint32_t len) {
    (void)ctx;
    if (len == 0) {
        return;
    }
    fwrite(data, 1u, (size_t)len, stdout);
}

int DumpTokens(const char* filename, const char* source, uint32_t sourceLen) {
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

int DumpAST(const char* filename, const char* source, uint32_t sourceLen) {
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

SL_API_END
