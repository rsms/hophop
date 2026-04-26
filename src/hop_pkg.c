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
#include "libhop-impl.h"
#include "mir.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"
#include "hop_internal.h"
#include "typecheck/internal.h"

H2_API_BEGIN

static int ParseSource(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    H2Ast*      outAst,
    void**      outArenaMem,
    H2Arena* _Nullable outArena);

static int ParseSourceEx(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    H2Ast*      outAst,
    void**      outArenaMem,
    H2Arena* _Nullable outArena,
    int useLineColDiag) {
    void*    arenaMem;
    uint64_t arenaCap64;
    size_t   arenaCap;
    H2Arena  arena;
    H2Diag   diag = { 0 };

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
        arenaCap64 = arenaNodeCap * (uint64_t)sizeof(H2AstNode) + 65536u;
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

    H2ArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (outArena != NULL) {
        H2ArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    }
    if (H2Parse(&arena, (H2StrView){ source, sourceLen }, NULL, outAst, NULL, &diag) != 0) {
        (void)(useLineColDiag ? PrintHOPDiagLineCol(filename, source, &diag, 0)
                              : PrintHOPDiag(filename, source, &diag, 0));
        H2ArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }
    if (CompactAstInArena(&arena, outAst) != 0) {
        H2Diag oomDiag = { 0 };
        oomDiag.code = H2Diag_ARENA_OOM;
        oomDiag.type = H2DiagTypeOfCode(H2Diag_ARENA_OOM);
        oomDiag.start = 0;
        oomDiag.end = 0;
        oomDiag.argStart = ArenaBytesUsed(&arena);
        oomDiag.argEnd = ArenaBytesCapacity(&arena);
        (void)(useLineColDiag ? PrintHOPDiagLineCol(filename, source, &oomDiag, 0)
                              : PrintHOPDiag(filename, source, &oomDiag, 0));
        H2ArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }

    *outArenaMem = arenaMem;
    if (outArena != NULL) {
        H2ArenaBlock* oldInline = &arena.inlineBlock;
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
    H2Ast*      outAst,
    void**      outArenaMem,
    H2Arena* _Nullable outArena) {
    return ParseSourceEx(filename, source, sourceLen, outAst, outArenaMem, outArena, 0);
}

static void WarnUnknownFeatureImports(const char* filename, const char* source, const H2Ast* ast);

typedef struct {
    const char*                filename;
    const char*                source;
    uint32_t                   sourceLen;
    int                        useLineColDiag;
    const H2CombinedSourceMap* remapMap;
    const char*                remapSource;
    int                        suppressUnusedWarnings;
} H2CheckRunSpec;

static int IsUnusedWarningDiag(H2DiagCode code) {
    return code == H2Diag_UNUSED_FUNCTION || code == H2Diag_UNUSED_VARIABLE
        || code == H2Diag_UNUSED_VARIABLE_NEVER_READ || code == H2Diag_UNUSED_PARAMETER
        || code == H2Diag_UNUSED_PARAMETER_NEVER_READ;
}

static int CheckRunHasRemap(const H2CheckRunSpec* spec) {
    return spec != NULL && spec->remapMap != NULL && spec->remapSource != NULL;
}

static void RemapSpanOffsetToLineCol(
    const char* source,
    uint32_t    spanStart,
    uint32_t    spanEnd,
    uint32_t    offset,
    uint32_t*   outLine,
    uint32_t*   outCol) {
    uint32_t i;
    uint32_t line = 1;
    uint32_t col = 1;
    if (outLine != NULL) {
        *outLine = 1;
    }
    if (outCol != NULL) {
        *outCol = 1;
    }
    if (source == NULL || spanEnd <= spanStart) {
        return;
    }
    if (offset < spanStart) {
        offset = spanStart;
    } else if (offset > spanEnd) {
        offset = spanEnd;
    }
    i = spanStart;
    while (i < offset && source[i] != '\0') {
        if (source[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
        i++;
    }
    if (outLine != NULL) {
        *outLine = line;
    }
    if (outCol != NULL) {
        *outCol = col;
    }
}

static uint32_t RemapSpanLineColToOffset(
    const char* source,
    uint32_t    spanStart,
    uint32_t    spanEnd,
    uint32_t    targetLine,
    uint32_t    targetCol) {
    uint32_t i = spanStart;
    uint32_t line = 1;
    uint32_t col = 1;
    if (source == NULL || spanEnd <= spanStart) {
        return spanStart;
    }
    if (targetLine == 0) {
        targetLine = 1;
    }
    if (targetCol == 0) {
        targetCol = 1;
    }
    while (i < spanEnd && source[i] != '\0') {
        if (line == targetLine && col == targetCol) {
            return i;
        }
        if (source[i] == '\n') {
            if (line == targetLine) {
                return i;
            }
            line++;
            col = 1;
        } else {
            col++;
        }
        i++;
    }
    return spanEnd;
}

static int RemapCombinedOffsetForNote(
    const H2CheckRunSpec* spec,
    uint32_t              offset,
    uint32_t*             outOffset,
    const char**          outPath,
    const char**          outSource) {
    uint32_t i;
    if (outOffset != NULL) {
        *outOffset = offset;
    }
    if (outPath != NULL) {
        *outPath = NULL;
    }
    if (outSource != NULL) {
        *outSource = NULL;
    }
    if (spec == NULL || spec->remapMap == NULL || spec->source == NULL || outOffset == NULL) {
        return 0;
    }
    for (i = 0; i < spec->remapMap->len; i++) {
        const H2CombinedSourceSpan* s = &spec->remapMap->spans[i];
        uint32_t                    line = 1;
        uint32_t                    col = 1;
        if (offset < s->combinedStart || offset > s->combinedEnd) {
            continue;
        }
        if (offset >= s->combinedEnd) {
            *outOffset = s->sourceEnd;
        } else if (s->source != NULL) {
            RemapSpanOffsetToLineCol(
                spec->source, s->combinedStart, s->combinedEnd, offset, &line, &col);
            *outOffset = RemapSpanLineColToOffset(
                s->source, s->sourceStart, s->sourceEnd, line, col);
        } else {
            uint32_t fileIndex = 0;
            if (!RemapCombinedOffset(
                    spec->remapMap, offset, outOffset, &fileIndex, outPath, outSource))
            {
                return 0;
            }
        }
        if (outPath != NULL) {
            *outPath = s->path;
        }
        if (outSource != NULL) {
            *outSource = s->source;
        }
        return 1;
    }
    return 0;
}

static void RemapCombinedDiagNotes(
    const H2CheckRunSpec* spec, const H2Diag* diagIn, H2Diag* diagOut) {
    H2DiagNote* notes;
    uint32_t    i;
    if (spec == NULL || diagIn == NULL || diagOut == NULL || diagIn->notesLen == 0
        || spec->remapMap == NULL)
    {
        return;
    }
    notes = (H2DiagNote*)calloc(diagIn->notesLen, sizeof(H2DiagNote));
    if (notes == NULL) {
        return;
    }
    for (i = 0; i < diagIn->notesLen; i++) {
        uint32_t    mappedStart = diagIn->notes[i].start;
        uint32_t    mappedEnd = diagIn->notes[i].end;
        const char* path = NULL;
        const char* source = NULL;
        int         startMapped = RemapCombinedOffsetForNote(
            spec, diagIn->notes[i].start, &mappedStart, &path, &source);
        int endMapped = RemapCombinedOffsetForNote(
            spec, diagIn->notes[i].end, &mappedEnd, &path, &source);
        notes[i] = diagIn->notes[i];
        if (startMapped) {
            notes[i].start = mappedStart;
        }
        if (endMapped) {
            notes[i].end = mappedEnd;
        }
        if (startMapped || endMapped) {
            notes[i].path = path;
            notes[i].source = source;
        } else if (!startMapped && !endMapped) {
            notes[i].path = "<combined>";
            notes[i].source = spec->source;
        }
    }
    diagOut->notes = notes;
}

static int EmitCheckDiag(
    const H2CheckRunSpec* spec,
    const H2Diag*         diag,
    int                   includeHint,
    int                   dropUnmappedUnusedWarnings) {
    const char*       displaySource;
    const char*       displayFilename;
    const H2Diag*     toPrint = diag;
    H2Diag            remappedDiag;
    H2RemapDiagStatus remapStatus = { 0 };
    uint32_t          remappedFileIndex = 0;
    H2DiagNote*       ownedNotes = NULL;
    int               rc;

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
        if (diag->argEnd > diag->argStart
            && (diag->code == H2Diag_CONST_PARAM_ARG_NOT_CONST
                || diag->code == H2Diag_CONST_PARAM_SPREAD_NOT_CONST)
            && spec->source != NULL)
        {
            uint32_t sourceLen = (uint32_t)strlen(spec->source);
            if (diag->argEnd <= sourceLen) {
                remappedDiag.argText = spec->source + diag->argStart;
                remappedDiag.argTextLen = diag->argEnd - diag->argStart;
            }
        }
        (void)remappedFileIndex;
        if (dropUnmappedUnusedWarnings && IsUnusedWarningDiag(diag->code)
            && !remapStatus.startMapped && !remapStatus.endMapped)
        {
            return 0;
        }
        if (remapStatus.startMapped) {
            RemapCombinedDiagNotes(spec, diag, &remappedDiag);
            ownedNotes = (H2DiagNote*)remappedDiag.notes;
            toPrint = &remappedDiag;
            displaySource = spec->remapSource;
        } else {
            toPrint = diag;
            displaySource = spec->source;
            displayFilename = "<combined>";
        }
    }
    rc = spec->useLineColDiag
           ? PrintHOPDiagLineCol(displayFilename, displaySource, toPrint, includeHint)
           : PrintHOPDiag(displayFilename, displaySource, toPrint, includeHint);
    free(ownedNotes);
    return rc;
}

static void TypecheckDiagSink(void* ctx, const H2Diag* diag) {
    H2CheckRunSpec* spec = (H2CheckRunSpec*)ctx;
    if (spec == NULL || diag == NULL) {
        return;
    }
    if (spec->suppressUnusedWarnings && IsUnusedWarningDiag(diag->code)) {
        return;
    }
    (void)EmitCheckDiag(spec, diag, 1, 1);
}

static int CheckSourceWithSpec(const H2CheckRunSpec* spec) {
    void*              arenaMem;
    uint64_t           arenaCap64;
    size_t             arenaCap;
    H2Arena            arena;
    H2Ast              ast;
    H2Diag             diag = { 0 };
    uint32_t           beforeTypecheckUsed;
    uint32_t           beforeTypecheckCap;
    uint32_t           afterTypecheckUsed;
    uint32_t           afterTypecheckCap;
    H2TypeCheckOptions checkOptions = {
        .ctx = (void*)spec,
        .onDiag = TypecheckDiagSink,
        .flags = 0,
        .filePath = spec != NULL ? spec->filename : NULL,
    };

    if (spec == NULL) {
        return -1;
    }
    ast.nodes = NULL;
    ast.len = 0;
    ast.root = -1;
    ast.features = 0;
    arenaCap64 = (uint64_t)(spec->sourceLen + 128u) * (uint64_t)sizeof(H2AstNode) + 65536u;
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
    H2ArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    H2ArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    if (H2Parse(&arena, (H2StrView){ spec->source, spec->sourceLen }, NULL, &ast, NULL, &diag) != 0)
    {
        (void)EmitCheckDiag(spec, &diag, 0, 0);
        H2ArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }
    if (CompactAstInArena(&arena, &ast) != 0) {
        H2Diag oomDiag = { 0 };
        oomDiag.code = H2Diag_ARENA_OOM;
        oomDiag.type = H2DiagTypeOfCode(H2Diag_ARENA_OOM);
        oomDiag.start = 0;
        oomDiag.end = 0;
        oomDiag.argStart = ArenaBytesUsed(&arena);
        oomDiag.argEnd = ArenaBytesCapacity(&arena);
        (void)EmitCheckDiag(spec, &oomDiag, 0, 0);
        H2ArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }

    WarnUnknownFeatureImports(spec->filename, spec->source, &ast);
    beforeTypecheckUsed = ArenaBytesUsed(&arena);
    beforeTypecheckCap = ArenaBytesCapacity(&arena);

    if (H2TypeCheckEx(
            &arena, &ast, (H2StrView){ spec->source, spec->sourceLen }, &checkOptions, &diag)
        != 0)
    {
        if (diag.code == H2Diag_ARENA_OOM) {
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
        H2ArenaDispose(&arena);
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
            ast.len <= UINT32_MAX / (uint32_t)sizeof(H2AstNode)
                ? ast.len * (uint32_t)sizeof(H2AstNode)
                : UINT32_MAX,
            beforeTypecheckUsed,
            beforeTypecheckCap,
            afterTypecheckUsed,
            afterTypecheckCap);
    }

    H2ArenaDispose(&arena);
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
    H2CheckRunSpec spec = {
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
    const H2CombinedSourceMap* remapMap,
    int                        suppressUnusedWarnings) {
    if (filename == NULL || source == NULL) {
        return -1;
    }
    H2CheckRunSpec spec = {
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

int CheckSource(const char* filename, const char* source, uint32_t sourceLen) {
    return CheckSourceEx(filename, source, sourceLen, 0, 0);
}

static int IsDeclKind(H2AstKind kind) {
    return kind == H2Ast_FN || kind == H2Ast_STRUCT || kind == H2Ast_UNION || kind == H2Ast_ENUM
        || kind == H2Ast_TYPE_ALIAS || kind == H2Ast_VAR || kind == H2Ast_CONST;
}

static int IsPubDeclNode(const H2AstNode* n) {
    return (n->flags & H2AstFlag_PUB) != 0;
}

static int FnNodeHasBody(const H2Ast* ast, int32_t nodeId) {
    int32_t child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == H2Ast_BLOCK) {
            return 1;
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

static int FnNodeHasAnytypeParam(const H2ParsedFile* file, int32_t nodeId) {
    int32_t child = ASTFirstChild(&file->ast, nodeId);
    while (child >= 0) {
        const H2AstNode* n = &file->ast.nodes[child];
        if (n->kind == H2Ast_PARAM) {
            int32_t          typeNode = ASTFirstChild(&file->ast, child);
            const H2AstNode* t =
                (typeNode >= 0 && (uint32_t)typeNode < file->ast.len)
                    ? &file->ast.nodes[typeNode]
                    : NULL;
            if (t != NULL && t->kind == H2Ast_TYPE_NAME && t->dataEnd > t->dataStart
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

static int FnNodeHasContextClause(const H2Ast* ast, int32_t nodeId) {
    int32_t child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == H2Ast_CONTEXT_CLAUSE) {
            return 1;
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

int DirectiveNameEq(const H2ParsedFile* file, int32_t nodeId, const char* name) {
    const H2AstNode* n =
        nodeId >= 0 && (uint32_t)nodeId < file->ast.len ? &file->ast.nodes[nodeId] : NULL;
    size_t len = strlen(name);
    return n != NULL && n->kind == H2Ast_DIRECTIVE && n->dataEnd >= n->dataStart
        && (size_t)(n->dataEnd - n->dataStart) == len
        && memcmp(file->source + n->dataStart, name, len) == 0;
}

static uint32_t DirectiveArgCount(const H2Ast* ast, int32_t nodeId) {
    uint32_t count = 0;
    int32_t  child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        count++;
        child = ASTNextSibling(ast, child);
    }
    return count;
}

int32_t DirectiveArgAt(const H2Ast* ast, int32_t nodeId, uint32_t index) {
    uint32_t i = 0;
    int32_t  child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        if (i == index) {
            return child;
        }
        i++;
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

int FindAttachedDirectiveRun(
    const H2Ast* ast, int32_t declNodeId, int32_t* outFirstDirective, int32_t* outLastDirective) {
    int32_t child;
    int32_t first = -1;
    int32_t last = -1;
    if (ast == NULL || outFirstDirective == NULL || outLastDirective == NULL) {
        return -1;
    }
    child = ASTFirstChild(ast, ast->root);
    while (child >= 0) {
        const H2AstNode* n = &ast->nodes[child];
        if (n->kind == H2Ast_DIRECTIVE) {
            if (first < 0) {
                first = child;
            }
            last = child;
        } else {
            if (child == declNodeId) {
                *outFirstDirective = first;
                *outLastDirective = last;
                return 0;
            }
            first = -1;
            last = -1;
        }
        child = ASTNextSibling(ast, child);
    }
    *outFirstDirective = -1;
    *outLastDirective = -1;
    return -1;
}

static uint32_t DeclTextSourceStart(const H2ParsedFile* file, int32_t nodeId) {
    int32_t firstDirective = -1;
    int32_t lastDirective = -1;
    if (FindAttachedDirectiveRun(&file->ast, nodeId, &firstDirective, &lastDirective) == 0
        && firstDirective >= 0)
    {
        return file->ast.nodes[firstDirective].start;
    }
    return file->ast.nodes[nodeId].start;
}

static uint32_t PubDeclTextSourceStart(const H2ParsedFile* file, int32_t nodeId) {
    uint32_t         start;
    uint32_t         end;
    const H2AstNode* n;
    if (file == NULL || nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    start = n->start;
    end = n->end;
    if ((n->flags & H2AstFlag_PUB) != 0 && start + 3u <= end
        && memcmp(file->source + start, "pub", 3u) == 0)
    {
        start += 3u;
        while (start < end && IsAsciiSpaceChar(file->source[start])) {
            start++;
        }
    }
    return start;
}

static int AppendDirectiveRunForDecl(
    H2StringBuilder* b, const H2ParsedFile* file, int32_t nodeId, int omitExportDirective) {
    int32_t firstDirective = -1;
    int32_t lastDirective = -1;
    int32_t child;
    int     emittedAny = 0;
    if (FindAttachedDirectiveRun(&file->ast, nodeId, &firstDirective, &lastDirective) != 0
        || firstDirective < 0)
    {
        return 0;
    }
    child = firstDirective;
    while (child >= 0) {
        const H2AstNode* n = &file->ast.nodes[child];
        if (n->kind != H2Ast_DIRECTIVE) {
            break;
        }
        if (!(omitExportDirective && DirectiveNameEq(file, child, "export"))) {
            if (SBAppendSlice(b, file->source, n->start, n->end) != 0 || SBAppendCStr(b, "\n") != 0)
            {
                return -1;
            }
            emittedAny = 1;
        }
        if (child == lastDirective) {
            break;
        }
        child = ASTNextSibling(&file->ast, child);
    }
    (void)emittedAny;
    return 0;
}

static char* _Nullable BuildDeclTextForNode(
    const H2ParsedFile* file, int32_t nodeId, int isPubSurface) {
    const H2AstNode* n = &file->ast.nodes[nodeId];
    uint32_t         declStart = n->start;
    uint32_t         declEnd = n->end;
    H2StringBuilder  b = { 0 };

    if (!isPubSurface) {
        return H2CDupSlice(file->source, DeclTextSourceStart(file, nodeId), n->end);
    }

    if (AppendDirectiveRunForDecl(&b, file, nodeId, 1) != 0) {
        free(b.v);
        return NULL;
    }
    if ((n->flags & H2AstFlag_PUB) != 0 && declStart + 3u <= declEnd
        && memcmp(file->source + declStart, "pub", 3u) == 0)
    {
        declStart += 3u;
        while (declStart < declEnd && IsAsciiSpaceChar(file->source[declStart])) {
            declStart++;
        }
    }
    if (n->kind == H2Ast_FN && FnNodeHasBody(&file->ast, nodeId)
        && !FnNodeHasAnytypeParam(file, nodeId))
    {
        int32_t body = ASTFirstChild(&file->ast, nodeId);
        while (body >= 0) {
            if (file->ast.nodes[body].kind == H2Ast_BLOCK) {
                declEnd = file->ast.nodes[body].start;
                break;
            }
            body = ASTNextSibling(&file->ast, body);
        }
        while (declEnd > declStart && IsAsciiSpaceChar(file->source[declEnd - 1u])) {
            declEnd--;
        }
        if (SBAppendSlice(&b, file->source, declStart, declEnd) != 0 || SBAppendCStr(&b, ";") != 0)
        {
            free(b.v);
            return NULL;
        }
    } else if (SBAppendSlice(&b, file->source, declStart, declEnd) != 0) {
        free(b.v);
        return NULL;
    }
    return SBFinish(&b, NULL);
}

static int PackageFnDeclCountByNode(
    const H2Package* pkg, const H2ParsedFile* file, int32_t nodeId) {
    const H2AstNode* target = &file->ast.nodes[nodeId];
    uint32_t         fileIndex;
    uint32_t         count = 0;
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const H2ParsedFile* scanFile = &pkg->files[fileIndex];
        int32_t             child = ASTFirstChild(&scanFile->ast, scanFile->ast.root);
        while (child >= 0) {
            const H2AstNode* n = &scanFile->ast.nodes[child];
            if (n->kind == H2Ast_FN && n->dataEnd > n->dataStart
                && SliceEqSlice(
                    scanFile->source,
                    n->dataStart,
                    n->dataEnd,
                    file->source,
                    target->dataStart,
                    target->dataEnd))
            {
                count++;
            }
            child = ASTNextSibling(&scanFile->ast, child);
        }
    }
    return (int)count;
}

static int VarLikeNodeHasInitializer(const H2Ast* ast, int32_t nodeId) {
    int32_t firstChild = ASTFirstChild(ast, nodeId);
    int32_t afterNames =
        firstChild >= 0 && ast->nodes[firstChild].kind == H2Ast_NAME_LIST
            ? ASTNextSibling(ast, firstChild)
            : firstChild;
    if (afterNames < 0) {
        return 0;
    }
    if ((ast->nodes[afterNames].kind == H2Ast_TYPE_NAME
         || ast->nodes[afterNames].kind == H2Ast_TYPE_PTR
         || ast->nodes[afterNames].kind == H2Ast_TYPE_REF
         || ast->nodes[afterNames].kind == H2Ast_TYPE_MUTREF
         || ast->nodes[afterNames].kind == H2Ast_TYPE_ARRAY
         || ast->nodes[afterNames].kind == H2Ast_TYPE_VARRAY
         || ast->nodes[afterNames].kind == H2Ast_TYPE_SLICE
         || ast->nodes[afterNames].kind == H2Ast_TYPE_MUTSLICE
         || ast->nodes[afterNames].kind == H2Ast_TYPE_OPTIONAL
         || ast->nodes[afterNames].kind == H2Ast_TYPE_FN
         || ast->nodes[afterNames].kind == H2Ast_TYPE_ANON_STRUCT
         || ast->nodes[afterNames].kind == H2Ast_TYPE_ANON_UNION
         || ast->nodes[afterNames].kind == H2Ast_TYPE_TUPLE))
    {
        return ASTNextSibling(ast, afterNames) >= 0;
    }
    return 1;
}

static int VarLikeNodeUsesGroupedNames(const H2Ast* ast, int32_t nodeId) {
    int32_t firstChild = ASTFirstChild(ast, nodeId);
    return firstChild >= 0 && ast->nodes[firstChild].kind == H2Ast_NAME_LIST;
}

static int ValidateDirectiveRunOnDecl(
    const H2Package*    pkg,
    const H2ParsedFile* file,
    const char* _Nullable platformTarget,
    int32_t firstDirective,
    int32_t lastDirective,
    int32_t declNodeId) {
    const H2AstNode* decl = &file->ast.nodes[declNodeId];
    int32_t          child = firstDirective;
    int32_t          cImportNode = -1;
    int32_t          wasmImportNode = -1;
    int32_t          exportNode = -1;
    int              hasForeignFnDirective = 0;

    while (child >= 0) {
        const H2AstNode* dir = &file->ast.nodes[child];
        uint32_t         argCount = DirectiveArgCount(&file->ast, child);
        int32_t          arg0;
        int32_t          arg1;
        if (dir->kind != H2Ast_DIRECTIVE) {
            break;
        }
        if (DirectiveNameEq(file, child, "c_import")) {
            if (cImportNode >= 0 || wasmImportNode >= 0 || exportNode >= 0) {
                return Errorf(
                    file->path, file->source, dir->start, dir->end, "duplicate foreign directive");
            }
            if (decl->kind != H2Ast_FN && decl->kind != H2Ast_VAR && decl->kind != H2Ast_CONST) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@c_import applies only to top-level fn, var, or const declarations");
            }
            if (argCount != 1u) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@c_import expects 1 string argument");
            }
            arg0 = DirectiveArgAt(&file->ast, child, 0u);
            if (arg0 < 0 || file->ast.nodes[arg0].kind != H2Ast_STRING) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@c_import expects string arguments");
            }
            cImportNode = child;
            hasForeignFnDirective = decl->kind == H2Ast_FN;
        } else if (DirectiveNameEq(file, child, "wasm_import")) {
            if (cImportNode >= 0 || wasmImportNode >= 0 || exportNode >= 0) {
                return Errorf(
                    file->path, file->source, dir->start, dir->end, "duplicate foreign directive");
            }
            if (decl->kind != H2Ast_FN && decl->kind != H2Ast_VAR && decl->kind != H2Ast_CONST) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@wasm_import applies only to top-level fn, var, or const declarations");
            }
            if (argCount != 2u) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@wasm_import expects 2 string arguments");
            }
            arg0 = DirectiveArgAt(&file->ast, child, 0u);
            arg1 = DirectiveArgAt(&file->ast, child, 1u);
            if (arg0 < 0 || arg1 < 0 || file->ast.nodes[arg0].kind != H2Ast_STRING
                || file->ast.nodes[arg1].kind != H2Ast_STRING)
            {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@wasm_import expects string arguments");
            }
            wasmImportNode = child;
            hasForeignFnDirective = decl->kind == H2Ast_FN;
        } else if (DirectiveNameEq(file, child, "export")) {
            if (cImportNode >= 0 || wasmImportNode >= 0 || exportNode >= 0) {
                return Errorf(
                    file->path, file->source, dir->start, dir->end, "duplicate foreign directive");
            }
            if (decl->kind != H2Ast_FN || !IsPubDeclNode(decl)
                || !FnNodeHasBody(&file->ast, declNodeId))
            {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@export applies only to pub fn definitions");
            }
            if (argCount != 1u) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@export expects 1 string argument");
            }
            arg0 = DirectiveArgAt(&file->ast, child, 0u);
            if (arg0 < 0 || file->ast.nodes[arg0].kind != H2Ast_STRING) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@export expects a string argument");
            }
            exportNode = child;
            hasForeignFnDirective = 1;
        } else {
            return Errorf(file->path, file->source, dir->start, dir->end, "unknown directive");
        }
        if (child == lastDirective) {
            break;
        }
        child = ASTNextSibling(&file->ast, child);
    }

    if ((cImportNode >= 0 || wasmImportNode >= 0) && decl->kind == H2Ast_FN
        && PackageFnDeclCountByNode(pkg, file, declNodeId) != 1)
    {
        return Errorf(
            file->path,
            file->source,
            decl->dataStart,
            decl->dataEnd,
            "foreign-linkage directives are invalid on overloaded functions");
    }
    if (exportNode >= 0 && PackageFnDeclCountByNode(pkg, file, declNodeId) != 1) {
        return Errorf(
            file->path,
            file->source,
            decl->dataStart,
            decl->dataEnd,
            "foreign-linkage directives are invalid on overloaded functions");
    }
    if (hasForeignFnDirective && FnNodeHasContextClause(&file->ast, declNodeId)) {
        return Errorf(
            file->path,
            file->source,
            decl->start,
            decl->end,
            "context is not supported on foreign-linkage functions");
    }
    if ((cImportNode >= 0 || wasmImportNode >= 0) && decl->kind == H2Ast_FN
        && FnNodeHasBody(&file->ast, declNodeId))
    {
        return Errorf(
            file->path,
            file->source,
            decl->start,
            decl->end,
            "foreign imports must be declarations only");
    }
    if ((cImportNode >= 0 || wasmImportNode >= 0)
        && (decl->kind == H2Ast_VAR || decl->kind == H2Ast_CONST))
    {
        if (VarLikeNodeUsesGroupedNames(&file->ast, declNodeId)) {
            return Errorf(
                file->path,
                file->source,
                decl->start,
                decl->end,
                "foreign imports do not support grouped var/const declarations");
        }
        if (VarLikeNodeHasInitializer(&file->ast, declNodeId)) {
            return Errorf(
                file->path,
                file->source,
                decl->start,
                decl->end,
                "foreign imports must be declarations only");
        }
    }
    if ((cImportNode >= 0 || wasmImportNode >= 0) && platformTarget != NULL
        && StrEq(platformTarget, H2_EVAL_PLATFORM_TARGET))
    {
        return Errorf(
            file->path,
            file->source,
            decl->start,
            decl->end,
            "foreign imports are not supported for platform %s",
            platformTarget);
    }
    return 0;
}

static int ValidatePackageForeignDirectives(
    const H2Package* pkg, const char* _Nullable platformTarget) {
    uint32_t fileIndex;
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const H2ParsedFile* file = &pkg->files[fileIndex];
        int32_t             child = ASTFirstChild(&file->ast, file->ast.root);
        int32_t             firstDirective = -1;
        int32_t             lastDirective = -1;
        while (child >= 0) {
            const H2AstNode* n = &file->ast.nodes[child];
            if (n->kind == H2Ast_DIRECTIVE) {
                if (firstDirective < 0) {
                    firstDirective = child;
                }
                lastDirective = child;
            } else {
                if (firstDirective >= 0
                    && ValidateDirectiveRunOnDecl(
                           pkg, file, platformTarget, firstDirective, lastDirective, child)
                           != 0)
                {
                    return -1;
                }
                firstDirective = -1;
                lastDirective = -1;
            }
            child = ASTNextSibling(&file->ast, child);
        }
    }
    return 0;
}

static int PackageUsesWasmImportDirective(const H2Package* pkg) {
    uint32_t fileIndex;
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const H2ParsedFile* file = &pkg->files[fileIndex];
        int32_t             child = ASTFirstChild(&file->ast, file->ast.root);
        while (child >= 0) {
            if (DirectiveNameEq(file, child, "wasm_import")) {
                return 1;
            }
            child = ASTNextSibling(&file->ast, child);
        }
    }
    return 0;
}

int LoaderUsesWasmImportDirective(const H2PackageLoader* loader) {
    uint32_t i;
    for (i = 0; i < loader->packageLen; i++) {
        if (PackageUsesWasmImportDirective(&loader->packages[i])) {
            return 1;
        }
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
    return H2CDupCStr(name);
}

static const char* LastPathSegment(const char* path) {
    const char* slash = strrchr(path, '/');
    if (slash == NULL || slash[1] == '\0') {
        return path;
    }
    return slash + 1;
}

static int IsFeatureImportPath(const char* importPath) {
    return strncmp(importPath, "hophop/feature/", 15u) == 0
        || strncmp(importPath, "feature/", 8u) == 0;
}

static int IsImportAliasUsed(const H2Package* pkg, const char* alias) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        if (StrEq(pkg->imports[i].alias, alias)) {
            return 1;
        }
    }
    return 0;
}

static char* _Nullable MakeUniqueImportAlias(const H2Package* pkg, const char* preferred) {
    char*    alias;
    uint32_t n;
    if (preferred != NULL && preferred[0] != '\0' && IsValidIdentifier(preferred)
        && !IsImportAliasUsed(pkg, preferred))
    {
        return H2CDupCStr(preferred);
    }
    if (preferred == NULL || preferred[0] == '\0' || !IsValidIdentifier(preferred)) {
        preferred = "imp";
    }
    alias = H2CDupCStr(preferred);
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

static int StringLiteralHasDirectOffsetMapping(const char* source, const H2AstNode* n) {
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

static void WarnUnknownFeatureImports(const char* filename, const char* source, const H2Ast* ast) {
    int32_t child = ASTFirstChild(ast, ast->root);
    while (child >= 0) {
        const H2AstNode* n = &ast->nodes[child];
        if (n->kind == H2Ast_IMPORT) {
            uint8_t* decoded = NULL;
            uint32_t decodedLen = 0;
            if (H2DecodeStringLiteralMalloc(
                    source, n->dataStart, n->dataEnd, &decoded, &decodedLen, NULL)
                != 0)
            {
                child = ASTNextSibling(ast, child);
                continue;
            }
            {
                uint32_t featureStart = 0;
                if (decodedLen > 15u && memcmp(decoded, "hophop/feature/", 15u) == 0) {
                    featureStart = 15u;
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
                        H2Diag diag = {
                            .code = H2Diag_UNKNOWN_FEATURE,
                            .type = H2DiagTypeOfCode(H2Diag_UNKNOWN_FEATURE),
                            .start = n->start,
                            .end = n->end,
                            .argStart = argStart,
                            .argEnd = argEnd,
                        };
                        (void)PrintHOPDiag(filename, source, &diag, 0);
                    }
                }
            }
            free(decoded);
        }
        child = ASTNextSibling(ast, child);
    }
}

static int AddPackageFile(
    H2Package*  pkg,
    const char* filePath,
    char*       source,
    uint32_t    sourceLen,
    H2Ast       ast,
    void*       arenaMem) {
    H2ParsedFile* f;
    if (EnsureCap((void**)&pkg->files, &pkg->fileCap, pkg->fileLen + 1u, sizeof(H2ParsedFile)) != 0)
    {
        return -1;
    }
    f = &pkg->files[pkg->fileLen++];
    memset(f, 0, sizeof(*f));
    f->path = H2CDupCStr(filePath);
    f->source = source;
    f->sourceLen = sourceLen;
    f->arenaMem = arenaMem;
    f->ast = ast;
    if (f->path == NULL) {
        return -1;
    }
    return 0;
}

static int AddDeclText(
    H2Package* pkg,
    char*      text,
    uint32_t   fileIndex,
    int32_t    nodeId,
    uint32_t   sourceStart,
    uint32_t   sourceEnd) {
    H2DeclText* t;
    if (EnsureCap(
            (void**)&pkg->declTexts, &pkg->declTextCap, pkg->declTextLen + 1u, sizeof(H2DeclText))
        != 0)
    {
        return -1;
    }
    t = &pkg->declTexts[pkg->declTextLen++];
    t->text = text;
    t->fileIndex = fileIndex;
    t->nodeId = nodeId;
    t->sourceStart = sourceStart;
    t->sourceEnd = sourceEnd;
    return 0;
}

static int AddSymbolDecl(
    H2SymbolDecl** arr,
    uint32_t*      len,
    uint32_t*      cap,
    H2AstKind      kind,
    char*          name,
    char*          declText,
    int            hasBody,
    uint32_t       fileIndex,
    int32_t        nodeId) {
    H2SymbolDecl* d;
    if (EnsureCap((void**)arr, cap, *len + 1u, sizeof(H2SymbolDecl)) != 0) {
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
    H2Package* pkg,
    char*      alias,
    char* _Nullable bindName,
    char*     importPath,
    uint32_t  fileIndex,
    uint32_t  start,
    uint32_t  end,
    uint32_t* outIndex) {
    H2ImportRef* imp;
    if (EnsureCap((void**)&pkg->imports, &pkg->importCap, pkg->importLen + 1u, sizeof(H2ImportRef))
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
    H2Package* pkg,
    uint32_t   importIndex,
    char*      sourceName,
    char*      localName,
    uint32_t   fileIndex,
    uint32_t   start,
    uint32_t   end) {
    H2ImportSymbolRef* sym;
    if (EnsureCap(
            (void**)&pkg->importSymbols,
            &pkg->importSymbolCap,
            pkg->importSymbolLen + 1u,
            sizeof(H2ImportSymbolRef))
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

static char* _Nullable DupPubDeclText(const H2ParsedFile* file, int32_t nodeId) {
    const H2AstNode* n = &file->ast.nodes[nodeId];
    uint32_t         start = n->start;
    uint32_t         end = n->end;

    if (start + 3u <= end && memcmp(file->source + start, "pub", 3u) == 0) {
        start += 3u;
        while (start < end && IsAsciiSpaceChar(file->source[start])) {
            start++;
        }
    }

    if (n->kind == H2Ast_FN && FnNodeHasBody(&file->ast, nodeId)
        && !FnNodeHasAnytypeParam(file, nodeId))
    {
        int32_t body = ASTFirstChild(&file->ast, nodeId);
        while (body >= 0) {
            if (file->ast.nodes[body].kind == H2Ast_BLOCK) {
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

    return H2CDupSlice(file->source, start, end);
}

static int AddDeclFromNode(
    H2Package* pkg, const H2ParsedFile* file, uint32_t fileIndex, int32_t nodeId, int isPub) {
    const H2AstNode* n = &file->ast.nodes[nodeId];
    int32_t          firstChild;

    if (!IsDeclKind(n->kind)) {
        return 0;
    }
    if (n->end < n->start || n->end > file->sourceLen) {
        return Errorf(file->path, file->source, n->start, n->end, "invalid declaration");
    }
    firstChild = ASTFirstChild(&file->ast, nodeId);
    if ((n->kind == H2Ast_VAR || n->kind == H2Ast_CONST) && firstChild >= 0
        && file->ast.nodes[firstChild].kind == H2Ast_NAME_LIST)
    {
        uint32_t i;
        uint32_t nameCount = AstListCount(&file->ast, firstChild);
        for (i = 0; i < nameCount; i++) {
            int32_t          nameNode = AstListItemAt(&file->ast, firstChild, i);
            const H2AstNode* nameAst =
                (nameNode >= 0 && (uint32_t)nameNode < file->ast.len)
                    ? &file->ast.nodes[nameNode]
                    : NULL;
            char* name;
            char* declText;
            if (nameAst == NULL || nameAst->dataEnd <= nameAst->dataStart) {
                return Errorf(file->path, file->source, n->start, n->end, "invalid declaration");
            }
            name = H2CDupSlice(file->source, nameAst->dataStart, nameAst->dataEnd);
            declText = BuildDeclTextForNode(file, nodeId, isPub);
            if (name == NULL || declText == NULL) {
                free(name);
                free(declText);
                return ErrorSimple("out of memory");
            }
            if (isPub) {
                uint32_t j;
                for (j = 0; j < pkg->pubDeclLen; j++) {
                    if (pkg->pubDecls[j].kind == n->kind && StrEq(pkg->pubDecls[j].name, name)
                        && n->kind != H2Ast_FN)
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
        char* name = H2CDupSlice(file->source, n->dataStart, n->dataEnd);
        char* declText = BuildDeclTextForNode(file, nodeId, isPub);
        if (name == NULL || declText == NULL) {
            free(name);
            free(declText);
            return ErrorSimple("out of memory");
        }
        if (isPub) {
            uint32_t i;
            for (i = 0; i < pkg->pubDeclLen; i++) {
                if (pkg->pubDecls[i].kind == n->kind && StrEq(pkg->pubDecls[i].name, name)
                    && n->kind != H2Ast_FN)
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

static int ProcessParsedFile(H2Package* pkg, uint32_t fileIndex) {
    const H2ParsedFile* file = &pkg->files[fileIndex];
    const H2Ast*        ast = &file->ast;
    int32_t             child = ASTFirstChild(ast, ast->root);

    /* Accumulate feature flags from this file into the package. */
    pkg->features |= ast->features;

    while (child >= 0) {
        const H2AstNode* n = &ast->nodes[child];
        if (n->kind == H2Ast_IMPORT) {
            int32_t          importChild = ASTFirstChild(ast, child);
            const H2AstNode* aliasNode = NULL;
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
                const H2AstNode* ch = &ast->nodes[importChild];
                if (ch->kind == H2Ast_IDENT) {
                    if (aliasNode != NULL) {
                        return Errorf(
                            file->path,
                            file->source,
                            n->start,
                            n->end,
                            "invalid import declaration");
                    }
                    aliasNode = ch;
                } else if (ch->kind == H2Ast_IMPORT_SYMBOL) {
                    hasSymbols = 1;
                } else {
                    return Errorf(
                        file->path, file->source, n->start, n->end, "invalid import declaration");
                }
                importChild = ASTNextSibling(ast, importChild);
            }

            if (H2DecodeStringLiteralMalloc(
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
                            H2Diag_IMPORT_INVALID_PATH,
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
            if (H2NormalizeImportPath(decodedPath, importPath, decodedPathLen + 1u, &pathErr) != 0)
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
                        H2Diag_IMPORT_INVALID_PATH,
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
                        H2Diag_IMPORT_FEATURE_IMPORT_EXTRAS);
                    free(importPath);
                    return rc;
                }
                free(importPath);
                child = ASTNextSibling(ast, child);
                continue;
            }

            if (aliasNode != NULL) {
                bindName = H2CDupSlice(file->source, aliasNode->dataStart, aliasNode->dataEnd);
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
                    H2Diag_IMPORT_SIDE_EFFECT_ALIAS_WITH_SYMBOLS);
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
                        H2Diag_IMPORT_ALIAS_INFERENCE_FAILED,
                        importPath);
                    free(importPath);
                    return rc;
                }
            }

            if (bindName != NULL && IsReservedHOPPrefixName(bindName)) {
                int rc = Errorf(
                    file->path,
                    file->source,
                    n->start,
                    n->end,
                    "identifier prefix '__hop_' is reserved");
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
                const H2AstNode* ch = &ast->nodes[importChild];
                if (ch->kind == H2Ast_IMPORT_SYMBOL) {
                    int32_t localAliasNode = ASTFirstChild(ast, importChild);
                    char*   sourceName = H2CDupSlice(file->source, ch->dataStart, ch->dataEnd);
                    char*   localName;
                    if (sourceName == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    if (localAliasNode >= 0 && ast->nodes[localAliasNode].kind == H2Ast_IDENT) {
                        const H2AstNode* ln = &ast->nodes[localAliasNode];
                        localName = H2CDupSlice(file->source, ln->dataStart, ln->dataEnd);
                    } else {
                        localName = H2CDupCStr(sourceName);
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
                            H2Diag_IMPORT_SYMBOL_ALIAS_INVALID);
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
        } else if (n->kind != H2Ast_DIRECTIVE) {
            char*    declText = BuildDeclTextForNode(file, child, 0);
            uint32_t sourceStart = DeclTextSourceStart(file, child);
            if (declText == NULL) {
                return ErrorSimple("out of memory");
            }
            if (AddDeclText(pkg, declText, fileIndex, child, sourceStart, n->end) != 0) {
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

static int PackageHasExport(const H2Package* pkg, const char* name) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (StrEq(pkg->pubDecls[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

static int ReflectPackageHasIntrinsicExportSlice(
    const H2Package* pkg, const char* src, uint32_t start, uint32_t end) {
    if (pkg == NULL || pkg->name == NULL || !StrEq(pkg->name, "reflect")) {
        return 0;
    }
    return SliceEqCStr(src, start, end, "kind") || SliceEqCStr(src, start, end, "base")
        || SliceEqCStr(src, start, end, "is_alias") || SliceEqCStr(src, start, end, "is_const")
        || SliceEqCStr(src, start, end, "type_name");
}

static int PackageHasExportSlice(
    const H2Package* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (ReflectPackageHasIntrinsicExportSlice(pkg, src, start, end)) {
        return 1;
    }
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
    const H2Package* pkg, const char* src, uint32_t start, uint32_t end) {
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
    const H2Package* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const H2ImportSymbolRef* sym = &pkg->importSymbols[i];
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

static int PackageHasBuiltinExportedTypeSlice(
    const H2Package* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL) {
        return 0;
    }
    for (i = 0; i < pkg->importLen; i++) {
        const H2ImportRef* imp = &pkg->imports[i];
        if (StrEq(imp->path, "builtin") && imp->target != NULL) {
            return PackageHasExportedTypeSlice(imp->target, src, start, end);
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

const H2ImportRef* _Nullable FindImportByAliasSlice(
    const H2Package* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd);
static int FindImportIndexByPath(const H2Package* pkg, const char* importPath);
static int IsBuiltinPackage(const H2Package* pkg);

static int ValidatePubTypeNode(
    const H2Package* pkg, const H2ParsedFile* file, int32_t typeNodeId, const char* contextMsg) {
    const H2AstNode* n;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= file->ast.len) {
        return ErrorSimple("invalid type node");
    }
    n = &file->ast.nodes[typeNodeId];
    switch (n->kind) {
        case H2Ast_TYPE_NAME: {
            uint32_t dotPos = FindSliceDot(file->source, n->dataStart, n->dataEnd);
            if (dotPos < n->dataEnd) {
                const H2ImportRef* imp = FindImportByAliasSlice(
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
            if (PackageHasBuiltinExportedTypeSlice(pkg, file->source, n->dataStart, n->dataEnd)) {
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
        case H2Ast_TYPE_PTR:
        case H2Ast_TYPE_REF:
        case H2Ast_TYPE_MUTREF:
        case H2Ast_TYPE_ARRAY:
        case H2Ast_TYPE_SLICE:
        case H2Ast_TYPE_MUTSLICE:
        case H2Ast_TYPE_VARRAY:
        case H2Ast_TYPE_OPTIONAL: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            return ValidatePubTypeNode(pkg, file, child, contextMsg);
        }
        case H2Ast_TYPE_TUPLE: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            while (child >= 0) {
                if (ValidatePubTypeNode(pkg, file, child, contextMsg) != 0) {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        case H2Ast_TYPE_FN: {
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

static int ValidatePubClosure(const H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        const H2SymbolDecl* pubDecl = &pkg->pubDecls[i];
        const H2ParsedFile* file = &pkg->files[pubDecl->fileIndex];
        int32_t             child = ASTFirstChild(&file->ast, pubDecl->nodeId);
        if (pubDecl->kind == H2Ast_FN) {
            while (child >= 0) {
                const H2AstNode* n = &file->ast.nodes[child];
                if (n->kind == H2Ast_PARAM) {
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
        } else if (pubDecl->kind == H2Ast_STRUCT || pubDecl->kind == H2Ast_UNION) {
            while (child >= 0) {
                const H2AstNode* n = &file->ast.nodes[child];
                if (n->kind == H2Ast_FIELD) {
                    int32_t typeNode = ASTFirstChild(&file->ast, child);
                    if (ValidatePubTypeNode(pkg, file, typeNode, "field type") != 0) {
                        return -1;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
        } else if (pubDecl->kind == H2Ast_ENUM) {
            if (child >= 0) {
                const H2AstNode* n = &file->ast.nodes[child];
                if (n->kind == H2Ast_TYPE_NAME || n->kind == H2Ast_TYPE_PTR
                    || n->kind == H2Ast_TYPE_REF || n->kind == H2Ast_TYPE_MUTREF
                    || n->kind == H2Ast_TYPE_ARRAY || n->kind == H2Ast_TYPE_VARRAY
                    || n->kind == H2Ast_TYPE_SLICE || n->kind == H2Ast_TYPE_MUTSLICE
                    || n->kind == H2Ast_TYPE_FN || n->kind == H2Ast_TYPE_TUPLE)
                {
                    if (ValidatePubTypeNode(pkg, file, child, "enum base type") != 0) {
                        return -1;
                    }
                }
            }
        } else if (pubDecl->kind == H2Ast_VAR || pubDecl->kind == H2Ast_CONST) {
            if (child >= 0 && IsFnReturnTypeNodeKind(file->ast.nodes[child].kind)) {
                if (ValidatePubTypeNode(
                        pkg,
                        file,
                        child,
                        pubDecl->kind == H2Ast_VAR ? "variable type" : "constant type")
                    != 0)
                {
                    return -1;
                }
            }
        } else if (pubDecl->kind == H2Ast_TYPE_ALIAS) {
            const H2AstNode* aliasNode = &file->ast.nodes[pubDecl->nodeId];
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

static int ValidatePubFnDefinitions(const H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        const H2SymbolDecl* pubDecl = &pkg->pubDecls[i];
        uint32_t            j;
        int                 found = pubDecl->hasBody;
        if (pubDecl->kind != H2Ast_FN) {
            continue;
        }
        for (j = 0; j < pkg->declLen; j++) {
            const H2SymbolDecl* decl = &pkg->decls[j];
            if (decl->kind == H2Ast_FN && StrEq(decl->name, pubDecl->name) && decl->hasBody) {
                found = 1;
                break;
            }
        }
        if (!found) {
            const H2ParsedFile* file = &pkg->files[pubDecl->fileIndex];
            const H2AstNode*    n = &file->ast.nodes[pubDecl->nodeId];
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

const H2ImportRef* _Nullable FindImportByAliasSlice(
    const H2Package* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd) {
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
    const H2Package* pkg, const char* src, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    size_t   n = (size_t)(nameEnd - nameStart);
    for (i = 0; i < pkg->declLen; i++) {
        if (pkg->decls[i].kind == H2Ast_ENUM && strlen(pkg->decls[i].name) == n
            && memcmp(pkg->decls[i].name, src + nameStart, n) == 0)
        {
            return 1;
        }
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (pkg->pubDecls[i].kind == H2Ast_ENUM && strlen(pkg->pubDecls[i].name) == n
            && memcmp(pkg->pubDecls[i].name, src + nameStart, n) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int ValidateSelectorsNode(const H2Package* pkg, const H2ParsedFile* file, int32_t nodeId) {
    const H2AstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];

    if (n->kind == H2Ast_TYPE_NAME) {
        uint32_t dot = FindSliceDot(file->source, n->dataStart, n->dataEnd);
        if (dot < n->dataEnd) {
            const H2ImportRef* imp = FindImportByAliasSlice(pkg, file->source, n->dataStart, dot);
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
    } else if (n->kind == H2Ast_FIELD_EXPR) {
        int32_t recvNode = ASTFirstChild(&file->ast, nodeId);
        if (recvNode >= 0 && file->ast.nodes[recvNode].kind == H2Ast_IDENT) {
            const H2AstNode*   recv = &file->ast.nodes[recvNode];
            const H2ImportRef* imp = FindImportByAliasSlice(
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

static int ValidatePackageSelectors(const H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->fileLen; i++) {
        const H2ParsedFile* file = &pkg->files[i];
        if (ValidateSelectorsNode(pkg, file, file->ast.root) != 0) {
            return -1;
        }
    }
    return 0;
}

static int PackageHasAnyDeclName(const H2Package* pkg, const char* name) {
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

static void PackageDiagOffsetToLineCol(
    const char* source, uint32_t offset, uint32_t* outLine, uint32_t* outCol) {
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t i;
    if (source == NULL) {
        *outLine = offset;
        *outCol = offset;
        return;
    }
    for (i = 0; source[i] != '\0' && i < offset; i++) {
        if (source[i] == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    *outLine = line;
    *outCol = col;
}

static int FindSymbolDeclNameSpan(
    const H2Package* pkg, const H2SymbolDecl* decl, uint32_t* outStart, uint32_t* outEnd) {
    const H2ParsedFile* file;
    const H2AstNode*    n;
    if (outStart != NULL) {
        *outStart = 0;
    }
    if (outEnd != NULL) {
        *outEnd = 0;
    }
    if (pkg == NULL || decl == NULL || decl->fileIndex >= pkg->fileLen || decl->nodeId < 0) {
        return 0;
    }
    file = &pkg->files[decl->fileIndex];
    if ((uint32_t)decl->nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[decl->nodeId];
    if ((n->kind == H2Ast_VAR || n->kind == H2Ast_CONST) && n->firstChild >= 0
        && (uint32_t)n->firstChild < file->ast.len
        && file->ast.nodes[n->firstChild].kind == H2Ast_NAME_LIST)
    {
        int32_t child = file->ast.nodes[n->firstChild].firstChild;
        while (child >= 0) {
            const H2AstNode* nameNode;
            if ((uint32_t)child >= file->ast.len) {
                break;
            }
            nameNode = &file->ast.nodes[child];
            if (strlen(decl->name) == (size_t)(nameNode->dataEnd - nameNode->dataStart)
                && memcmp(
                       decl->name,
                       file->source + nameNode->dataStart,
                       (size_t)(nameNode->dataEnd - nameNode->dataStart))
                       == 0)
            {
                if (outStart != NULL) {
                    *outStart = nameNode->dataStart;
                }
                if (outEnd != NULL) {
                    *outEnd = nameNode->dataEnd;
                }
                return 1;
            }
            child = nameNode->nextSibling;
        }
    }
    if (n->dataEnd > n->dataStart) {
        if (outStart != NULL) {
            *outStart = n->dataStart;
        }
        if (outEnd != NULL) {
            *outEnd = n->dataEnd;
        }
        return 1;
    }
    return 0;
}

static int ErrorDuplicateBuiltinDecl(
    const H2Package*    pkg,
    const H2SymbolDecl* decl,
    const H2Package*    builtinPkg,
    const H2SymbolDecl* builtinDecl) {
    const H2ParsedFile* file;
    const H2ParsedFile* builtinFile = NULL;
    uint32_t            start = 0;
    uint32_t            end = 0;
    uint32_t            line = 0;
    uint32_t            col = 0;
    if (pkg == NULL || decl == NULL || decl->fileIndex >= pkg->fileLen) {
        return ErrorSimple("internal error: invalid declaration");
    }
    file = &pkg->files[decl->fileIndex];
    if (!FindSymbolDeclNameSpan(pkg, decl, &start, &end)) {
        if (decl->nodeId >= 0 && (uint32_t)decl->nodeId < file->ast.len) {
            start = file->ast.nodes[decl->nodeId].start;
            end = file->ast.nodes[decl->nodeId].end;
        }
    }
    PackageDiagOffsetToLineCol(file->source, start, &line, &col);
    fprintf(
        stderr,
        "%s:%u:%u: error: HOP2001: duplicate definition '%s'\n",
        DisplayPath(file->path),
        line,
        col,
        decl->name);
    if (builtinPkg != NULL && builtinDecl != NULL && builtinDecl->fileIndex < builtinPkg->fileLen) {
        uint32_t otherStart = 0;
        uint32_t otherEnd = 0;
        uint32_t otherLine = 0;
        uint32_t otherCol = 0;
        builtinFile = &builtinPkg->files[builtinDecl->fileIndex];
        if (FindSymbolDeclNameSpan(builtinPkg, builtinDecl, &otherStart, &otherEnd)) {
            PackageDiagOffsetToLineCol(builtinFile->source, otherStart, &otherLine, &otherCol);
            fprintf(
                stderr,
                "%s:%u:%u: hint: HOP2001: other declaration of '%s'\n",
                DisplayPath(builtinFile->path),
                otherLine,
                otherCol,
                decl->name);
            (void)otherEnd;
        }
    }
    (void)end;
    return -1;
}

static const H2SymbolDecl* _Nullable FindBuiltinPubDeclByName(
    const H2Package* builtinPkg, const char* name) {
    uint32_t i;
    if (builtinPkg == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < builtinPkg->pubDeclLen; i++) {
        if (StrEq(builtinPkg->pubDecls[i].name, name)) {
            return &builtinPkg->pubDecls[i];
        }
    }
    return NULL;
}

static const H2SymbolDecl* _Nullable FindBuiltinNonFunctionPubDeclByName(
    const H2Package* builtinPkg, const char* name) {
    uint32_t i;
    if (builtinPkg == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < builtinPkg->pubDeclLen; i++) {
        if (builtinPkg->pubDecls[i].kind != H2Ast_FN && StrEq(builtinPkg->pubDecls[i].name, name)) {
            return &builtinPkg->pubDecls[i];
        }
    }
    return NULL;
}

static int ValidateBuiltinNameConflictsForDecls(
    const H2Package*    pkg,
    const H2Package*    builtinPkg,
    const H2SymbolDecl* decls,
    uint32_t            declLen) {
    uint32_t i;
    for (i = 0; i < declLen; i++) {
        const H2SymbolDecl* decl = &decls[i];
        const H2SymbolDecl* builtinDecl;
        if (decl->kind == H2Ast_FN) {
            builtinDecl = FindBuiltinNonFunctionPubDeclByName(builtinPkg, decl->name);
        } else {
            builtinDecl = FindBuiltinPubDeclByName(builtinPkg, decl->name);
        }
        if (builtinDecl != NULL) {
            return ErrorDuplicateBuiltinDecl(pkg, decl, builtinPkg, builtinDecl);
        }
    }
    return 0;
}

static int ValidateBuiltinNameConflicts(H2Package* pkg) {
    int              builtinImportIndex;
    const H2Package* builtinPkg;
    uint32_t         i;
    if (IsBuiltinPackage(pkg)) {
        return 0;
    }
    builtinImportIndex = FindImportIndexByPath(pkg, "builtin");
    if (builtinImportIndex < 0) {
        return 0;
    }
    builtinPkg = pkg->imports[(uint32_t)builtinImportIndex].target;
    if (builtinPkg == NULL) {
        return ErrorSimple("internal error: unresolved builtin import");
    }
    if (ValidateBuiltinNameConflictsForDecls(pkg, builtinPkg, pkg->decls, pkg->declLen) != 0
        || ValidateBuiltinNameConflictsForDecls(pkg, builtinPkg, pkg->pubDecls, pkg->pubDeclLen)
               != 0)
    {
        return -1;
    }
    for (i = 0; i < pkg->importLen; i++) {
        const H2ImportRef* imp = &pkg->imports[i];
        if (imp->bindName != NULL && FindBuiltinPubDeclByName(builtinPkg, imp->bindName) != NULL) {
            const H2ParsedFile* file = &pkg->files[imp->fileIndex];
            return Errorf(
                file->path, file->source, imp->start, imp->end, "import binding conflict");
        }
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const H2ImportSymbolRef* sym = &pkg->importSymbols[i];
        const H2SymbolDecl*      builtinDecl = FindBuiltinPubDeclByName(builtinPkg, sym->localName);
        if (builtinDecl == NULL) {
            continue;
        }
        if (!(sym->isFunction && builtinDecl->kind == H2Ast_FN)) {
            const H2ParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "import binding conflict");
        }
    }
    return 0;
}

static int ValidateImportBindingConflicts(H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        const H2ImportRef* imp = &pkg->imports[i];
        uint32_t           j;
        if (imp->bindName == NULL) {
            continue;
        }
        if (PackageHasAnyDeclName(pkg, imp->bindName)) {
            const H2ParsedFile* file = &pkg->files[imp->fileIndex];
            return Errorf(
                file->path, file->source, imp->start, imp->end, "import binding conflict");
        }
        for (j = i + 1u; j < pkg->importLen; j++) {
            if (pkg->imports[j].bindName != NULL && StrEq(pkg->imports[j].bindName, imp->bindName))
            {
                const H2ParsedFile* file = &pkg->files[pkg->imports[j].fileIndex];
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
                const H2ParsedFile* file = &pkg->files[pkg->importSymbols[j].fileIndex];
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
        H2ImportSymbolRef* sym = &pkg->importSymbols[i];
        uint32_t           j;
        if (PackageHasAnyDeclName(pkg, sym->localName)) {
            const H2ParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "import binding conflict");
        }
        for (j = i + 1u; j < pkg->importSymbolLen; j++) {
            H2ImportSymbolRef* other = &pkg->importSymbols[j];
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
                const H2ParsedFile* file = &pkg->files[other->fileIndex];
                return Errorf(
                    file->path, file->source, other->start, other->end, "import binding conflict");
            }
        }
    }
    return 0;
}

static int ValidateAndFinalizeImportSymbols(H2Package* pkg) {
    uint32_t baseLen = pkg->importSymbolLen;
    uint32_t i;
    for (i = 0; i < baseLen; i++) {
        H2ImportSymbolRef* sym = &pkg->importSymbols[i];
        const H2ImportRef* imp;
        const H2Package*   dep;
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
            const H2SymbolDecl* exportDecl = &dep->pubDecls[j];
            H2ImportSymbolRef*  dstSym = sym;
            char*               rewrittenDecl = NULL;
            char*               shapeKey = NULL;
            char*               wrapperDecl = NULL;
            if (!StrEq(exportDecl->name, sym->sourceName)) {
                continue;
            }
            if (matchCount > 0) {
                char* sourceName = H2CDupCStr(sym->sourceName);
                char* localName = H2CDupCStr(sym->localName);
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
            dstSym->isFunction = exportDecl->kind == H2Ast_FN ? 1u : 0u;
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
            const H2ParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "unknown imported symbol");
        }
    }
    return 0;
}

static H2Package* _Nullable FindPackageByDir(const H2PackageLoader* loader, const char* dirPath) {
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
    baseName = StripHOPExtensionDup(fileName);
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

static int AddPackageSlot(H2PackageLoader* loader, const char* dirPath, H2Package** outPkg) {
    H2Package* pkg;
    uint32_t   needCap;
    if (loader == NULL || outPkg == NULL) {
        return -1;
    }
    needCap = loader->packageLen + 1u;
    if (loader->packageCap == 0u && needCap < 256u) {
        needCap = 256u;
    }
    if (EnsureCap((void**)&loader->packages, &loader->packageCap, needCap, sizeof(H2Package)) != 0)
    {
        return -1;
    }
    pkg = &loader->packages[loader->packageLen++];
    memset(pkg, 0, sizeof(*pkg));
    pkg->dirPath = H2CDupCStr(dirPath);
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

static int DirectoryHasHOPFiles(const char* dirPath) {
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
        if (len >= 4 && strcmp(name + len - 4, ".hop") == 0) {
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
    if (IsDirectoryPath(candidate) && DirectoryHasHOPFiles(candidate)) {
        return candidate;
    }
    free(candidate);
    return NULL;
}

static int IsLibImportPath(const char* importPath) {
    return StrEq(importPath, "builtin") || StrEq(importPath, "reflect") || StrEq(importPath, "mem")
        || StrEq(importPath, "platform") || StrEq(importPath, "compiler")
        || StrEq(importPath, "playbit") || StrEq(importPath, "str") || StrEq(importPath, "testing")
        || strncmp(importPath, "builtin/", 8u) == 0 || strncmp(importPath, "reflect/", 8u) == 0
        || strncmp(importPath, "mem/", 4u) == 0 || strncmp(importPath, "compiler/", 9u) == 0
        || strncmp(importPath, "playbit/", 8u) == 0 || strncmp(importPath, "std/", 4u) == 0
        || strncmp(importPath, "platform/", 9u) == 0 || strncmp(importPath, "str/", 4u) == 0;
}

static char* _Nullable ResolveLibImportDir(const char* startDir, const char* importPath) {
    char* dir;
    if (!IsLibImportPath(importPath)) {
        return NULL;
    }
    dir = H2CDupCStr(startDir);
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

static int LoadPackageRecursive(H2PackageLoader* loader, const char* dirPath, H2Package** outPkg);

static void FreeStringList(char** _Nullable items, uint32_t len) {
    uint32_t i;
    if (items == NULL) {
        return;
    }
    for (i = 0; i < len; i++) {
        free(items[i]);
    }
    free(items);
}

static int IsBuildTagChar(unsigned char c) {
    return IsIdentContinueChar(c) || c == '-' || c == '.';
}

static int BuildTagSliceIsActive(
    const H2PackageLoader* loader, const char* tagStart, size_t tagLen) {
    if (loader == NULL || tagStart == NULL || tagLen == 0) {
        return 0;
    }
    if (loader->archTarget != NULL && strlen(loader->archTarget) == tagLen
        && memcmp(loader->archTarget, tagStart, tagLen) == 0)
    {
        return 1;
    }
    if (loader->platformTarget != NULL && strlen(loader->platformTarget) == tagLen
        && memcmp(loader->platformTarget, tagStart, tagLen) == 0)
    {
        return 1;
    }
    return loader->testingBuild && tagLen == 7u && memcmp(tagStart, "testing", 7u) == 0;
}

static int FilenameBuildTagsMatch(
    const H2PackageLoader* loader, const char* filePath, int* outMatch) {
    const char* base = strrchr(filePath, '/');
    size_t      baseLen;
    size_t      stemLen;
    size_t      openIndex = (size_t)-1;
    size_t      i;
    size_t      tagStart;
    size_t      tagEnd;

    *outMatch = 0;
    base = base != NULL ? base + 1 : filePath;
    baseLen = strlen(base);
    if (baseLen < 4u || !HasSuffix(base, ".hop")) {
        return ErrorSimple(
            "invalid filename build tag in %s: expected .hop file", DisplayPath(filePath));
    }
    stemLen = baseLen - 4u;

    for (i = 0; i < stemLen; i++) {
        if (base[i] == '[') {
            if (openIndex != (size_t)-1) {
                return ErrorSimple(
                    "invalid filename build tag in %s: nested '['", DisplayPath(filePath));
            }
            openIndex = i;
        } else if (base[i] == ']') {
            if (openIndex == (size_t)-1) {
                return ErrorSimple(
                    "invalid filename build tag in %s: unexpected ']'", DisplayPath(filePath));
            }
            if (i != stemLen - 1u) {
                return ErrorSimple(
                    "invalid filename build tag in %s: tag suffix must precede .hop",
                    DisplayPath(filePath));
            }
        }
    }

    if (openIndex == (size_t)-1) {
        *outMatch = 1;
        return 0;
    }
    if (stemLen == 0 || base[stemLen - 1u] != ']') {
        return ErrorSimple(
            "invalid filename build tag in %s: missing closing ']'", DisplayPath(filePath));
    }

    tagStart = openIndex + 1u;
    tagEnd = stemLen - 1u;
    if (tagStart == tagEnd) {
        return ErrorSimple(
            "invalid filename build tag in %s: empty tag list", DisplayPath(filePath));
    }

    i = tagStart;
    *outMatch = 1;
    while (i < tagEnd) {
        size_t atomStart = i;
        size_t atomEnd;
        int    negated = 0;
        int    active;
        if (base[i] == '!') {
            negated = 1;
            i++;
            atomStart = i;
            if (i < tagEnd && base[i] == '!') {
                return ErrorSimple(
                    "invalid filename build tag in %s: duplicate '!'", DisplayPath(filePath));
            }
        }
        atomEnd = i;
        while (atomEnd < tagEnd && base[atomEnd] != ',') {
            atomEnd++;
        }
        if (atomStart == atomEnd) {
            return ErrorSimple(
                "invalid filename build tag in %s: empty tag", DisplayPath(filePath));
        }
        for (i = atomStart; i < atomEnd; i++) {
            unsigned char c = (unsigned char)base[i];
            if (c == '[' || c == ']') {
                return ErrorSimple(
                    "invalid filename build tag in %s: nested bracket", DisplayPath(filePath));
            }
            if (!IsBuildTagChar(c)) {
                return ErrorSimple(
                    "invalid filename build tag in %s: invalid tag character",
                    DisplayPath(filePath));
            }
        }
        active = BuildTagSliceIsActive(loader, base + atomStart, atomEnd - atomStart);
        if ((active && negated) || (!active && !negated)) {
            *outMatch = 0;
        }
        if (atomEnd < tagEnd) {
            i = atomEnd + 1u;
            if (i == tagEnd) {
                return ErrorSimple(
                    "invalid filename build tag in %s: empty tag", DisplayPath(filePath));
            }
        } else {
            i = atomEnd;
        }
    }

    return 0;
}

static int FilterPackageFilesByBuildTags(
    const H2PackageLoader* loader, const char* dirPath, char** filePaths, uint32_t* fileCount) {
    uint32_t readIndex;
    uint32_t writeIndex = 0;
    for (readIndex = 0; readIndex < *fileCount; readIndex++) {
        int match = 0;
        if (FilenameBuildTagsMatch(loader, filePaths[readIndex], &match) != 0) {
            return -1;
        }
        if (match) {
            filePaths[writeIndex] = filePaths[readIndex];
            if (writeIndex != readIndex) {
                filePaths[readIndex] = NULL;
            }
            writeIndex++;
        } else {
            free(filePaths[readIndex]);
            filePaths[readIndex] = NULL;
        }
    }
    if (writeIndex == 0) {
        return ErrorSimple("no matching .hop files found in %s", DisplayPath(dirPath));
    }
    *fileCount = writeIndex;
    return 0;
}

static int FindImportIndexByPath(const H2Package* pkg, const char* importPath) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        if (StrEq(pkg->imports[i].path, importPath)) {
            return (int)i;
        }
    }
    return -1;
}

static int IsBuiltinPackage(const H2Package* pkg) {
    const char* base;
    if (pkg == NULL || pkg->dirPath == NULL) {
        return 0;
    }
    base = strrchr(pkg->dirPath, '/');
    base = base != NULL ? base + 1 : pkg->dirPath;
    return StrEq(base, "builtin");
}

static int IsReflectPackage(const H2Package* pkg) {
    const char* base;
    if (pkg == NULL || pkg->dirPath == NULL) {
        return 0;
    }
    base = strrchr(pkg->dirPath, '/');
    base = base != NULL ? base + 1 : pkg->dirPath;
    return StrEq(base, "reflect");
}

static int EnsureImplicitBuiltinImport(H2Package* pkg) {
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
    importPath = H2CDupCStr("builtin");
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

static int EnsureImplicitReflectImport(H2Package* pkg) {
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
    importPath = H2CDupCStr("reflect");
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

static int LoadSelectedPlatformTargetPackage(
    H2PackageLoader* loader, const char* startDir, H2Package** outPkg) {
    char* importPath = NULL;
    char* resolvedDir = NULL;
    int   rc = -1;

    if (loader->platformTarget == NULL || loader->platformTarget[0] == '\0') {
        return ErrorSimple("internal error: missing platform target");
    }

    {
        H2StringBuilder b = { 0 };
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
        H2Package*  tmpPkg = NULL;
        H2Package** outPtr = outPkg != NULL ? outPkg : &tmpPkg;
        if (LoadPackageRecursive(loader, resolvedDir, outPtr) != 0) {
            rc = ErrorSimple("failed to resolve platform target package %s", importPath);
        } else {
            loader->selectedPlatformPkg = *outPtr;
            rc = 0;
        }
    }
    free(importPath);
    free(resolvedDir);
    return rc;
}

static int IsSelectedPlatformImportPath(
    const H2PackageLoader* loader, const char* _Nullable importPath) {
    size_t prefixLen = 9u;
    if (loader == NULL || loader->platformTarget == NULL || importPath == NULL) {
        return 0;
    }
    if (StrEq(importPath, "platform")) {
        return 1;
    }
    return strncmp(importPath, "platform/", prefixLen) == 0
        && StrEq(importPath + prefixLen, loader->platformTarget);
}

static int ResolvePackageImportsAndSelectors(H2PackageLoader* loader, H2Package* pkg) {
    uint32_t i;
    int      pkgIndex;
    if (loader == NULL || pkg == NULL) {
        return -1;
    }
    pkgIndex = FindPackageIndex(loader, pkg);
    if (pkgIndex < 0) {
        return ErrorSimple("internal error: package missing from loader");
    }
    if (EnsureImplicitBuiltinImport(pkg) != 0) {
        return -1;
    }
    if (EnsureImplicitReflectImport(pkg) != 0) {
        return -1;
    }
    for (i = 0; i < pkg->importLen; i++) {
        char* resolvedDir;
        if (loader->selectedPlatformPkg != NULL
            && IsSelectedPlatformImportPath(loader, pkg->imports[i].path))
        {
            pkg->imports[i].target = loader->selectedPlatformPkg;
            continue;
        }
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
            const H2ParsedFile* file = &pkg->files[pkg->imports[i].fileIndex];
            free(resolvedDir);
            return Errorf(
                file->path,
                file->source,
                pkg->imports[i].start,
                pkg->imports[i].end,
                "failed to resolve import %s",
                pkg->imports[i].path);
        }
        pkg = &loader->packages[(uint32_t)pkgIndex];
        free(resolvedDir);
    }

    if (ValidateAndFinalizeImportSymbols(pkg) != 0) {
        return -1;
    }
    if (ValidateImportBindingConflicts(pkg) != 0) {
        return -1;
    }
    if (ValidateBuiltinNameConflicts(pkg) != 0) {
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

static int LoadPackageRecursive(H2PackageLoader* loader, const char* dirPath, H2Package** outPkg) {
    char*      canonical = CanonicalizePath(dirPath);
    H2Package* pkg;
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

    if (ListHOPFiles(pkg->dirPath, &filePaths, &fileCount) != 0) {
        return -1;
    }
    if (FilterPackageFilesByBuildTags(loader, pkg->dirPath, filePaths, &fileCount) != 0) {
        FreeStringList(filePaths, fileCount);
        return -1;
    }

    for (i = 0; i < fileCount; i++) {
        char*    source = NULL;
        uint32_t sourceLen = 0;
        H2Ast    ast;
        void*    arenaMem = NULL;
        if (ReadFile(filePaths[i], &source, &sourceLen) != 0) {
            FreeStringList(filePaths, fileCount);
            return -1;
        }
        if (ParseSourceEx(filePaths[i], source, sourceLen, &ast, &arenaMem, NULL, 1) != 0) {
            free(source);
            FreeStringList(filePaths, fileCount);
            return -1;
        }
        if (AddPackageFile(pkg, filePaths[i], source, sourceLen, ast, arenaMem) != 0) {
            free(source);
            free(arenaMem);
            FreeStringList(filePaths, fileCount);
            return ErrorSimple("out of memory");
        }
    }

    FreeStringList(filePaths, fileCount);

    for (i = 0; i < pkg->fileLen; i++) {
        if (ProcessParsedFile(pkg, i) != 0) {
            return -1;
        }
    }

    if (ValidatePackageForeignDirectives(pkg, loader->platformTarget) != 0) {
        return -1;
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
    H2PackageLoader* loader, const char* filePath, H2Package** outPkg) {
    char*      dirPath = DirNameDup(filePath);
    H2Package* pkg;
    char*      source = NULL;
    uint32_t   sourceLen = 0;
    H2Ast      ast;
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

    if (ValidatePackageForeignDirectives(pkg, loader->platformTarget) != 0) {
        return -1;
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
    const H2IdentMap* _Nullable maps,
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
} H2TextRewrite;

static int AddTextRewrite(
    H2TextRewrite** rewrites,
    uint32_t*       len,
    uint32_t*       cap,
    uint32_t        start,
    uint32_t        end,
    const char*     replacement) {
    if (EnsureCap((void**)rewrites, cap, *len + 1u, sizeof(H2TextRewrite)) != 0) {
        return -1;
    }
    (*rewrites)[*len].start = start;
    (*rewrites)[*len].end = end;
    (*rewrites)[*len].replacement = replacement;
    (*len)++;
    return 0;
}

static int FindImportSymbolBindingIndexBySlice(
    const H2Package* pkg, const char* src, uint32_t start, uint32_t end, int wantType) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const H2ImportSymbolRef* sym = &pkg->importSymbols[i];
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
    const H2Package*    pkg,
    const H2ParsedFile* file,
    int32_t             nodeId,
    H2TextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap) {
    const H2AstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    if (n->kind == H2Ast_TYPE_NAME) {
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
    const H2Package*    pkg,
    const H2ParsedFile* file,
    int32_t             nodeId,
    uint8_t*            shadowCounts,
    H2TextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap) {
    const H2AstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    switch (n->kind) {
        case H2Ast_IDENT: {
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
        case H2Ast_CALL:
        case H2Ast_CALL_WITH_CONTEXT: {
            int32_t callee = ASTFirstChild(&file->ast, nodeId);
            if (callee >= 0 && (uint32_t)callee < file->ast.len
                && file->ast.nodes[callee].kind == H2Ast_FIELD_EXPR)
            {
                int32_t            recv = ASTFirstChild(&file->ast, callee);
                const H2AstNode*   fieldExpr = &file->ast.nodes[callee];
                const H2ImportRef* imp = NULL;
                if (recv >= 0 && (uint32_t)recv < file->ast.len
                    && file->ast.nodes[recv].kind == H2Ast_IDENT)
                {
                    const H2AstNode* recvNode = &file->ast.nodes[recv];
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
        case H2Ast_UNARY:
        case H2Ast_BINARY:
        case H2Ast_CONTEXT_OVERLAY:
        case H2Ast_CONTEXT_BIND:
        case H2Ast_INDEX:
        case H2Ast_CAST:
        case H2Ast_SIZEOF:
        case H2Ast_NEW:
        case H2Ast_UNWRAP:
        case H2Ast_CALL_ARG:
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
        case H2Ast_FIELD_EXPR: {
            int32_t recv = ASTFirstChild(&file->ast, nodeId);
            if (recv >= 0 && (uint32_t)recv < file->ast.len
                && file->ast.nodes[recv].kind == H2Ast_IDENT)
            {
                const H2AstNode* recvNode = &file->ast.nodes[recv];
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
    const H2Package*    pkg,
    const H2ParsedFile* file,
    uint32_t            start,
    uint32_t            end,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const H2ImportSymbolRef* sym = &pkg->importSymbols[i];
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
    const H2Package*    pkg,
    const H2ParsedFile* file,
    int32_t             nodeId,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap,
    H2TextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap);

static int CollectBlockImportRewritesNode(
    const H2Package*    pkg,
    const H2ParsedFile* file,
    int32_t             blockNodeId,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap,
    H2TextRewrite**     rewrites,
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

static int32_t VarLikeInitNode(const H2ParsedFile* file, int32_t varLikeNodeId) {
    int32_t firstChild = ASTFirstChild(&file->ast, varLikeNodeId);
    int32_t afterNames;
    if (firstChild < 0) {
        return -1;
    }
    if (file->ast.nodes[firstChild].kind == H2Ast_NAME_LIST) {
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

uint32_t AstListCount(const H2Ast* ast, int32_t listNode) {
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

int32_t AstListItemAt(const H2Ast* ast, int32_t listNode, uint32_t index) {
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
    const H2Package*    pkg,
    const H2ParsedFile* file,
    int32_t             nodeId,
    uint8_t*            shadowCounts,
    uint32_t**          shadowStack,
    uint32_t*           shadowLen,
    uint32_t*           shadowCap,
    H2TextRewrite**     rewrites,
    uint32_t*           rewriteLen,
    uint32_t*           rewriteCap) {
    const H2AstNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    switch (n->kind) {
        case H2Ast_BLOCK:
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
        case H2Ast_VAR:
        case H2Ast_CONST: {
            int32_t initNode = VarLikeInitNode(file, nodeId);
            int32_t firstChild = ASTFirstChild(&file->ast, nodeId);
            if (firstChild >= 0 && file->ast.nodes[firstChild].kind == H2Ast_NAME_LIST) {
                uint32_t i;
                uint32_t nameCount = AstListCount(&file->ast, firstChild);
                if (initNode >= 0) {
                    if (file->ast.nodes[initNode].kind == H2Ast_EXPR_LIST) {
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
                    const H2AstNode* name = nameNode >= 0 ? &file->ast.nodes[nameNode] : NULL;
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
        case H2Ast_IF: {
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
        case H2Ast_FOR: {
            int32_t          parts[4];
            uint32_t         partCount = 0;
            uint32_t         mark = *shadowLen;
            const H2AstNode* forNode = &file->ast.nodes[nodeId];
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0 && partCount < 4u) {
                parts[partCount++] = child;
                child = ASTNextSibling(&file->ast, child);
            }
            if ((forNode->flags & H2AstFlag_FOR_IN) != 0) {
                int      hasKey = (forNode->flags & H2AstFlag_FOR_IN_HAS_KEY) != 0;
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
                    const H2AstNode* key = &file->ast.nodes[keyNode];
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
                if (valueNode >= 0 && (forNode->flags & H2AstFlag_FOR_IN_VALUE_DISCARD) == 0) {
                    const H2AstNode* value = &file->ast.nodes[valueNode];
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
                if (bodyNode >= 0 && file->ast.nodes[bodyNode].kind == H2Ast_BLOCK) {
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
                if (partCount >= 2u && file->ast.nodes[parts[0]].kind == H2Ast_VAR) {
                    int32_t initNode = VarLikeInitNode(file, parts[0]);
                    int32_t firstChild = ASTFirstChild(&file->ast, parts[0]);
                    if (firstChild >= 0 && file->ast.nodes[firstChild].kind == H2Ast_NAME_LIST) {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(&file->ast, firstChild);
                        if (initNode >= 0) {
                            if (file->ast.nodes[initNode].kind == H2Ast_EXPR_LIST) {
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
                            const H2AstNode* name =
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
                } else if (partCount >= 2u && file->ast.nodes[parts[0]].kind != H2Ast_BLOCK) {
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
                    if (file->ast.nodes[parts[idx]].kind != H2Ast_BLOCK
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
                if (file->ast.nodes[parts[last]].kind == H2Ast_BLOCK) {
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
        case H2Ast_SWITCH: {
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const H2AstNode* c = &file->ast.nodes[child];
                if (c->kind == H2Ast_CASE) {
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
                } else if (c->kind == H2Ast_DEFAULT) {
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
        case H2Ast_RETURN:
        case H2Ast_ASSERT:
        case H2Ast_DEL:
        case H2Ast_EXPR_STMT: {
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
        case H2Ast_DEFER: {
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
    const H2TextRewrite* ra = (const H2TextRewrite*)a;
    const H2TextRewrite* rb = (const H2TextRewrite*)b;
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
    H2TextRewrite* _Nullable rewrites,
    uint32_t rewriteLen,
    char**   outText) {
    H2StringBuilder b = { 0 };
    uint32_t        i;
    uint32_t        copyPos = 0;
    *outText = NULL;

    if (rewriteLen == 0) {
        *outText = H2CDupSlice(text, 0, textLen);
        return *outText == NULL ? -1 : 0;
    }
    qsort(rewrites, rewriteLen, sizeof(H2TextRewrite), CompareTextRewrite);

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
    const H2Package*    pkg,
    const H2ParsedFile* file,
    int32_t             nodeId,
    const char*         text,
    char**              outText) {
    const H2AstNode* n;
    H2TextRewrite*   rewrites = NULL;
    uint32_t         rewriteLen = 0;
    uint32_t         rewriteCap = 0;
    uint8_t*         shadowCounts = NULL;
    uint32_t*        shadowStack = NULL;
    uint32_t         shadowLen = 0;
    uint32_t         shadowCap = 0;
    uint32_t         mark = 0;
    uint32_t         baseStart;
    int32_t          child;
    int              rc = -1;

    *outText = NULL;
    if (pkg->importSymbolLen == 0) {
        *outText = H2CDupCStr(text);
        return *outText == NULL ? -1 : 0;
    }
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return -1;
    }
    baseStart = DeclTextSourceStart(file, nodeId);
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
        case H2Ast_FN:
            mark = shadowLen;
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const H2AstNode* ch = &file->ast.nodes[child];
                if (ch->kind == H2Ast_PARAM) {
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
                } else if (ch->kind == H2Ast_BLOCK) {
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
        case H2Ast_CONST: {
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
        case H2Ast_ENUM: {
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const H2AstNode* c = &file->ast.nodes[child];
                if (c->kind == H2Ast_FIELD) {
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

    if (ApplyTextRewrites(text, (uint32_t)strlen(text), baseStart, rewrites, rewriteLen, outText)
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
    const H2ImportRef* _Nullable imports,
    uint32_t importLen,
    const H2IdentMap* _Nullable maps,
    uint32_t mapLen,
    char**   outText) {
    void*           arenaMem = NULL;
    size_t          arenaCap;
    uint64_t        arenaCap64;
    H2Arena         arena;
    H2TokenStream   stream;
    H2Diag          diag = { 0 };
    H2StringBuilder b = { 0 };
    uint32_t        i;
    uint32_t        copyPos = 0;

    *outText = NULL;
    if ((importLen > 0 && imports == NULL) || (mapLen > 0 && maps == NULL)) {
        return ErrorSimple("internal error: missing rewrite mappings");
    }
    arenaCap64 = (uint64_t)(srcLen + 16u) * (uint64_t)sizeof(H2Token) + 4096u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        return ErrorSimple("arena too large");
    }
    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        return ErrorSimple("out of memory");
    }

    H2ArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (H2Lex(&arena, (H2StrView){ src, srcLen }, &stream, &diag) != 0) {
        free(arenaMem);
        return ErrorSimple("rewrite lex failed");
    }

    for (i = 0; i < stream.len; i++) {
        const H2Token* t = &stream.v[i];
        if (t->kind == H2Tok_EOF) {
            break;
        }

        if (importLen > 0 && i + 2u < stream.len && t->kind == H2Tok_IDENT
            && stream.v[i + 1u].kind == H2Tok_DOT && stream.v[i + 2u].kind == H2Tok_IDENT)
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
        if (t->kind == H2Tok_IDENT && mapLen > 0) {
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

int BuildPrefixedName(const char* alias, const char* name, char** outName) {
    H2StringBuilder b = { 0 };
    *outName = NULL;
    if (SBAppendCStr(&b, alias) != 0 || SBAppendCStr(&b, "__") != 0 || SBAppendCStr(&b, name) != 0)
    {
        free(b.v);
        return -1;
    }
    *outName = SBFinish(&b, NULL);
    return *outName == NULL ? -1 : 0;
}

int RewriteAliasedPubDeclText(
    const H2Package* sourcePkg, const H2SymbolDecl* pubDecl, const char* alias, char** outText) {
    H2IdentMap* maps = NULL;
    uint32_t    i;
    int         rc = -1;
    *outText = NULL;
    if (sourcePkg->pubDeclLen == 0) {
        return ErrorSimple("internal error: empty public declaration set");
    }
    maps = (H2IdentMap*)calloc(sourcePkg->pubDeclLen, sizeof(H2IdentMap));
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

int BuildFnImportShapeAndWrapper(
    const char* _Nullable aliasedDeclText,
    const char* _Nullable localName,
    const char* _Nullable qualifiedName,
    char** _Nullable outShapeKey,
    char** _Nullable outWrapperDeclText) {
    H2Ast           ast = { 0 };
    void*           arenaMem = NULL;
    int32_t         fnNode = -1;
    int32_t         child;
    int32_t         returnTypeNode = -1;
    int32_t         contextTypeNode = -1;
    H2StringBuilder shape = { 0 };
    H2StringBuilder wrapper = { 0 };
    H2StringBuilder callArgs = { 0 };
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
    if (fnNode < 0 || (uint32_t)fnNode >= ast.len || ast.nodes[fnNode].kind != H2Ast_FN) {
        free(arenaMem);
        return ErrorSimple("internal error: expected function declaration in rewritten import");
    }

    if (SBAppendCStr(&shape, "ctx:") != 0) {
        free(arenaMem);
        goto oom;
    }
    child = ASTFirstChild(&ast, fnNode);
    while (child >= 0) {
        const H2AstNode* n = &ast.nodes[child];
        if (n->kind == H2Ast_PARAM) {
            /* handled later */
        } else if (n->kind == H2Ast_CONTEXT_CLAUSE) {
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
        const H2AstNode* n = &ast.nodes[child];
        if (n->kind == H2Ast_PARAM) {
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
                    tempNameBuf, sizeof(tempNameBuf), "__hop_arg%u", (unsigned)paramIndex);
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

static int PackagePrivateDeclIncludedInSurface(const H2Package* pkg, const H2SymbolDecl* decl);

static int AppendAliasedPubDecls(
    H2StringBuilder* b,
    const H2Package* sourcePkg,
    const char*      alias,
    const H2ImportRef* _Nullable imports,
    uint32_t importLen,
    int      includePrivateDecls,
    int      forceFunctionDeclsOnly,
    H2CombinedSourceMap* _Nullable sourceMap) {
    H2IdentMap* maps = NULL;
    uint32_t    mapLen = 0;
    uint32_t    declLen = includePrivateDecls ? sourcePkg->declLen : 0u;
    uint32_t    selectedDeclLen = 0;
    uint32_t    j;
    uint32_t    mapIndex = 0;
    int         rc = -1;

    if (includePrivateDecls) {
        for (j = 0; j < declLen; j++) {
            if (PackagePrivateDeclIncludedInSurface(sourcePkg, &sourcePkg->decls[j])) {
                selectedDeclLen++;
            }
        }
    }
    if (selectedDeclLen == 0 && sourcePkg->pubDeclLen == 0) {
        return 0;
    }
    mapLen = selectedDeclLen + sourcePkg->pubDeclLen;
    maps = (H2IdentMap*)calloc(mapLen, sizeof(H2IdentMap));
    if (maps == NULL) {
        return ErrorSimple("out of memory");
    }
    for (j = 0; j < declLen; j++) {
        if (!PackagePrivateDeclIncludedInSurface(sourcePkg, &sourcePkg->decls[j])) {
            continue;
        }
        maps[mapIndex].name = sourcePkg->decls[j].name;
        maps[mapIndex].replacement = NULL;
        if (BuildPrefixedName(alias, sourcePkg->decls[j].name, (char**)&maps[mapIndex].replacement)
            != 0)
        {
            goto done;
        }
        mapIndex++;
    }
    for (j = 0; j < sourcePkg->pubDeclLen; j++) {
        maps[mapIndex].name = sourcePkg->pubDecls[j].name;
        maps[mapIndex].replacement = NULL;
        if (BuildPrefixedName(
                alias, sourcePkg->pubDecls[j].name, (char**)&maps[mapIndex].replacement)
            != 0)
        {
            goto done;
        }
        mapIndex++;
    }

    for (j = 0; j < sourcePkg->pubDeclLen; j++) {
        const H2SymbolDecl* decl = &sourcePkg->pubDecls[j];
        const H2ParsedFile* file = &sourcePkg->files[decl->fileIndex];
        char*               declText = sourcePkg->pubDecls[j].declText;
        char*               declTextCopy = NULL;
        const char*         rewriteSource = declText;
        uint32_t            rewriteSourceLen;
        char*               rewritten = NULL;
        uint32_t            combinedStart;
        uint32_t            combinedEnd;
        uint32_t            sourceStart = PubDeclTextSourceStart(file, decl->nodeId);
        uint32_t            sourceEnd = file->ast.nodes[decl->nodeId].end;
        if (forceFunctionDeclsOnly && sourcePkg->pubDecls[j].kind == H2Ast_FN) {
            const char* bodyStart = strchr(declText, '{');
            if (bodyStart != NULL) {
                size_t declLenNoBody = (size_t)(bodyStart - declText);
                while (declLenNoBody > 0u && IsAsciiSpaceChar(declText[declLenNoBody - 1u])) {
                    declLenNoBody--;
                }
                declTextCopy = (char*)malloc(declLenNoBody + 2u);
                if (declTextCopy == NULL) {
                    rc = ErrorSimple("out of memory");
                    goto done;
                }
                memcpy(declTextCopy, declText, declLenNoBody);
                declTextCopy[declLenNoBody] = ';';
                declTextCopy[declLenNoBody + 1u] = '\0';
                rewriteSource = declTextCopy;
            }
        }
        rewriteSourceLen = (uint32_t)strlen(rewriteSource);
        rc = RewriteText(
            rewriteSource, rewriteSourceLen, imports, importLen, maps, mapLen, &rewritten);
        free(declTextCopy);
        if (rc != 0) {
            goto done;
        }
        combinedStart = b->len;
        combinedEnd = combinedStart + (rewritten != NULL ? (uint32_t)strlen(rewritten) : 0u);
        if (rewritten == NULL || SBAppendCStr(b, rewritten) != 0 || SBAppendCStr(b, "\n") != 0) {
            free(rewritten);
            goto done;
        }
        if (sourceMap != NULL
            && CombinedSourceMapAdd(
                   sourceMap,
                   combinedStart,
                   combinedEnd,
                   sourceStart,
                   sourceEnd,
                   decl->fileIndex,
                   decl->nodeId,
                   file->path,
                   file->source)
                   != 0)
        {
            free(rewritten);
            rc = ErrorSimple("out of memory");
            goto done;
        }
        free(rewritten);
    }
    for (j = 0; j < declLen; j++) {
        const H2SymbolDecl* decl = &sourcePkg->decls[j];
        const H2ParsedFile* file = &sourcePkg->files[decl->fileIndex];
        char*               namedRewritten = NULL;
        char*               rewritten = NULL;
        uint32_t            combinedStart;
        uint32_t            combinedEnd;
        uint32_t            sourceStart = DeclTextSourceStart(file, decl->nodeId);
        uint32_t            sourceEnd = file->ast.nodes[decl->nodeId].end;
        if (!PackagePrivateDeclIncludedInSurface(sourcePkg, decl)) {
            continue;
        }
        rc = RewriteDeclTextForNamedImports(
            sourcePkg, file, decl->nodeId, decl->declText, &namedRewritten);
        if (rc != 0) {
            goto done;
        }
        rc = RewriteText(
            namedRewritten,
            (uint32_t)strlen(namedRewritten),
            imports,
            importLen,
            maps,
            mapLen,
            &rewritten);
        if (rc != 0) {
            free(namedRewritten);
            goto done;
        }
        combinedStart = b->len;
        combinedEnd = combinedStart + (rewritten != NULL ? (uint32_t)strlen(rewritten) : 0u);
        if (rewritten == NULL || SBAppendCStr(b, rewritten) != 0 || SBAppendCStr(b, "\n") != 0) {
            free(namedRewritten);
            free(rewritten);
            goto done;
        }
        if (sourceMap != NULL
            && CombinedSourceMapAdd(
                   sourceMap,
                   combinedStart,
                   combinedEnd,
                   sourceStart,
                   sourceEnd,
                   decl->fileIndex,
                   decl->nodeId,
                   file->path,
                   file->source)
                   != 0)
        {
            free(namedRewritten);
            free(rewritten);
            rc = ErrorSimple("out of memory");
            goto done;
        }
        free(namedRewritten);
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
    const H2Package* pkg;
    const char*      alias;
} H2EmittedImportSurface;

static int EnsureEmittedImportSurfaceCap(
    H2EmittedImportSurface** outArr, uint32_t* outCap, uint32_t needLen) {
    if (needLen <= *outCap) {
        return 0;
    }
    {
        uint32_t                nextCap = *outCap == 0 ? 8u : *outCap;
        H2EmittedImportSurface* p;
        while (nextCap < needLen) {
            if (nextCap > UINT32_MAX / 2u) {
                return -1;
            }
            nextCap *= 2u;
        }
        p = (H2EmittedImportSurface*)realloc(*outArr, (size_t)nextCap * sizeof(**outArr));
        if (p == NULL) {
            return -1;
        }
        *outArr = p;
        *outCap = nextCap;
    }
    return 0;
}

static int HasEmittedImportSurface(
    const H2EmittedImportSurface* _Nullable arr,
    uint32_t len,
    const H2Package* _Nullable pkg,
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

static int PackageNeedsPrivateDeclSurface(const H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (pkg->pubDecls[i].kind == H2Ast_FN) {
            return 1;
        }
    }
    return 0;
}

static int PackagePrivateDeclIncludedInSurface(const H2Package* pkg, const H2SymbolDecl* decl) {
    if (pkg == NULL || decl == NULL || decl->name == NULL) {
        return 0;
    }
    if (pkg->name == NULL || !StrEq(pkg->name, "builtin")) {
        return 1;
    }
    return strncmp(decl->name, "fmt_", 4u) == 0;
}

static int AppendImportedPackageSurface(
    H2StringBuilder*         b,
    const H2PackageLoader*   loader,
    const H2Package*         dep,
    const char*              alias,
    int                      includePrivateImportDecls,
    H2EmittedImportSurface** emitted,
    uint32_t*                emittedLen,
    uint32_t*                emittedCap,
    H2CombinedSourceMap* _Nullable sourceMap) {
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
        const H2Package* subDep = dep->imports[j].target;
        if (subDep == NULL) {
            return ErrorSimple("internal error: unresolved import");
        }
        if (!HasEmittedImportSurface(*emitted, *emittedLen, subDep, dep->imports[j].alias)
            && AppendAliasedPubDecls(
                   b,
                   subDep,
                   dep->imports[j].alias,
                   NULL,
                   0,
                   0,
                   IsSelectedPlatformImportPath(loader, dep->imports[j].path),
                   sourceMap)
                   != 0)
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
    includePrivateDecls =
        (loader != NULL && dep == loader->selectedPlatformPkg)
            ? 0
            : (includePrivateImportDecls ? PackageNeedsPrivateDeclSurface(dep) : 0);
    if (AppendAliasedPubDecls(
            b,
            dep,
            alias,
            dep->imports,
            dep->importLen,
            includePrivateDecls,
            loader != NULL && dep == loader->selectedPlatformPkg,
            sourceMap)
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

static int AppendImportFunctionWrappers(H2StringBuilder* b, const H2Package* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const H2ImportSymbolRef* sym = &pkg->importSymbols[i];
        if (!sym->isFunction || !sym->useWrapper || sym->wrapperDeclText == NULL) {
            continue;
        }
        if (SBAppendCStr(b, sym->wrapperDeclText) != 0 || SBAppendCStr(b, "\n") != 0) {
            return ErrorSimple("out of memory");
        }
    }
    return 0;
}

int BuildCombinedPackageSource(
    H2PackageLoader* loader,
    const H2Package* pkg,
    int              includePrivateImportDecls,
    char**           outSource,
    uint32_t*        outLen,
    H2CombinedSourceMap* _Nullable sourceMap,
    uint32_t* _Nullable outOwnDeclStartOffset) {
    H2StringBuilder         b = { 0 };
    uint32_t                i;
    H2EmittedImportSurface* emitted = NULL;
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
        const H2Package* dep = pkg->imports[i].target;
        if (AppendImportedPackageSurface(
                &b,
                loader,
                dep,
                pkg->imports[i].alias,
                includePrivateImportDecls,
                &emitted,
                &emittedLen,
                &emittedCap,
                sourceMap)
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
        const H2DeclText*   decl = &pkg->declTexts[i];
        const H2ParsedFile* file = &pkg->files[decl->fileIndex];
        char*               namedRewritten = NULL;
        char*               rewritten = NULL;
        uint32_t            combinedStart;
        uint32_t            combinedEnd;
        uint32_t            sourceStart = decl->sourceStart;
        uint32_t            sourceEnd = decl->sourceEnd;
        uint32_t            rewrittenLen;
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
                   decl->nodeId,
                   file->path,
                   file->source)
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

static int CheckLoadedPackage(H2PackageLoader* loader, H2Package* pkg, int suppressUnusedWarnings) {
    char*               source = NULL;
    uint32_t            sourceLen = 0;
    int                 lineColDiag = 0;
    const char*         checkPath = pkg->dirPath;
    const char*         checkSource = NULL;
    uint32_t            checkSourceLen = 0;
    H2CombinedSourceMap sourceMap = { 0 };
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
    if (pkg->fileLen == 1 && pkg->files[0].source != NULL) {
        uint64_t           tcArenaCap64;
        uint32_t           tcArenaCap;
        H2TypeCheckCtx*    tcCtx;
        H2Diag             tcDiag = { 0 };
        H2TypeCheckOptions tcOptions = { 0 };
        tcOptions.filePath = pkg->files[0].path;
        tcArenaCap64 = (uint64_t)pkg->files[0].sourceLen * 256u + 1024u * 1024u;
        if (tcArenaCap64 > UINT32_MAX) {
            return ErrorSimple("typecheck arena too large");
        }
        tcArenaCap = (uint32_t)tcArenaCap64;
        pkg->files[0].typecheckArenaMem = malloc(tcArenaCap);
        if (pkg->files[0].typecheckArenaMem == NULL) {
            return ErrorSimple("out of memory");
        }
        H2ArenaInit(&pkg->files[0].typecheckArena, pkg->files[0].typecheckArenaMem, tcArenaCap);
        H2ArenaSetAllocator(
            &pkg->files[0].typecheckArena, NULL, CodegenArenaGrow, CodegenArenaFree);
        tcCtx = (H2TypeCheckCtx*)H2ArenaAlloc(
            &pkg->files[0].typecheckArena,
            sizeof(H2TypeCheckCtx),
            (uint32_t)_Alignof(H2TypeCheckCtx));
        if (tcCtx == NULL) {
            return ErrorSimple("out of memory");
        }
        if (H2TCBuildCheckedContext(
                &pkg->files[0].typecheckArena,
                &pkg->files[0].ast,
                (H2StrView){ pkg->files[0].source, pkg->files[0].sourceLen },
                &tcOptions,
                &tcDiag,
                tcCtx)
            != 0)
        {
            H2ArenaDispose(&pkg->files[0].typecheckArena);
            free(pkg->files[0].typecheckArenaMem);
            pkg->files[0].typecheckArenaMem = NULL;
            pkg->files[0].typecheckCtx = NULL;
            pkg->files[0].hasTypecheckCtx = 0;
        } else {
            pkg->files[0].typecheckCtx = tcCtx;
            pkg->files[0].hasTypecheckCtx = 1;
        }
    }
    pkg->checked = 1;
    return 0;
}

static void FreePackage(H2Package* pkg) {
    uint32_t i;
    free(pkg->dirPath);
    free(pkg->name);
    for (i = 0; i < pkg->fileLen; i++) {
        free(pkg->files[i].path);
        free(pkg->files[i].source);
        free(pkg->files[i].arenaMem);
        if (pkg->files[i].typecheckArenaMem != NULL) {
            H2ArenaDispose(&pkg->files[i].typecheckArena);
        }
        free(pkg->files[i].typecheckArenaMem);
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

void FreeLoader(H2PackageLoader* loader) {
    uint32_t i;
    for (i = 0; i < loader->packageLen; i++) {
        FreePackage(&loader->packages[i]);
    }
    free(loader->packages);
    free(loader->rootDir);
    free(loader->platformTarget);
    free(loader->archTarget);
    memset(loader, 0, sizeof(*loader));
}

int LoadPackageForFmt(
    const char* entryPath,
    const char* _Nullable platformTarget,
    H2PackageLoader* outLoader,
    H2Package**      outEntryPkg) {
    char*           canonical = CanonicalizePath(entryPath);
    struct stat     st;
    char*           pkgDir = NULL;
    char*           rootDir;
    H2PackageLoader loader;
    H2Package*      entryPkg = NULL;
    memset(outLoader, 0, sizeof(*outLoader));
    *outEntryPkg = NULL;
    if (canonical == NULL) {
        return -1;
    }
    if (stat(canonical, &st) != 0) {
        free(canonical);
        return -1;
    }
    if (S_ISDIR(st.st_mode)) {
        rootDir = DirNameDup(canonical);
    } else if (S_ISREG(st.st_mode) && HasSuffix(canonical, ".hop")) {
        pkgDir = DirNameDup(canonical);
        if (pkgDir == NULL) {
            free(canonical);
            return -1;
        }
        rootDir = DirNameDup(pkgDir);
    } else {
        free(canonical);
        return -1;
    }
    if (rootDir == NULL) {
        free(pkgDir);
        free(canonical);
        return -1;
    }
    memset(&loader, 0, sizeof(loader));
    loader.rootDir = rootDir;
    loader.platformTarget = H2CDupCStr(
        (platformTarget != NULL && platformTarget[0] != '\0')
            ? platformTarget
            : H2_DEFAULT_PLATFORM_TARGET);
    loader.archTarget = H2CDupCStr(H2_DEFAULT_ARCH_TARGET);
    if (loader.platformTarget == NULL || loader.archTarget == NULL) {
        free(pkgDir);
        free(canonical);
        FreeLoader(&loader);
        return -1;
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
        return -1;
    }
    if (LoadSelectedPlatformTargetPackage(&loader, entryPkg->dirPath, NULL) != 0) {
        free(canonical);
        FreeLoader(&loader);
        return -1;
    }
    free(canonical);
    *outLoader = loader;
    *outEntryPkg = entryPkg;
    return 0;
}

int LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int              testingBuild,
    H2PackageLoader* outLoader,
    H2Package**      outEntryPkg) {
    char*           canonical = CanonicalizePath(entryPath);
    struct stat     st;
    char*           pkgDir = NULL;
    char*           rootDir;
    H2PackageLoader loader;
    H2Package*      entryPkg = NULL;
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
    } else if (S_ISREG(st.st_mode) && HasSuffix(canonical, ".hop")) {
        pkgDir = DirNameDup(canonical);
        if (pkgDir == NULL) {
            free(canonical);
            return ErrorSimple("out of memory");
        }
        rootDir = DirNameDup(pkgDir);
    } else {
        free(canonical);
        return ErrorSimple("expected package directory or .hop file: %s", entryPath);
    }
    if (rootDir == NULL) {
        free(pkgDir);
        free(canonical);
        return ErrorSimple("out of memory");
    }

    memset(&loader, 0, sizeof(loader));
    loader.rootDir = rootDir;
    loader.platformTarget = H2CDupCStr(
        (platformTarget != NULL && platformTarget[0] != '\0')
            ? platformTarget
            : H2_DEFAULT_PLATFORM_TARGET);
    loader.archTarget = H2CDupCStr(
        (archTarget != NULL && archTarget[0] != '\0') ? archTarget : H2_DEFAULT_ARCH_TARGET);
    loader.testingBuild = testingBuild;
    if (loader.platformTarget == NULL || loader.archTarget == NULL) {
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

int ValidateEntryMainSignature(const H2Package* _Nullable entryPkg) {
    uint32_t fileIndex;
    int      hasMainDefinition = 0;

    if (entryPkg == NULL) {
        return ErrorSimple("internal error: missing entry package");
    }

    for (fileIndex = 0; fileIndex < entryPkg->fileLen; fileIndex++) {
        const H2ParsedFile* file = &entryPkg->files[fileIndex];
        const H2Ast*        ast = &file->ast;
        int32_t             nodeId = ASTFirstChild(ast, ast->root);

        while (nodeId >= 0) {
            const H2AstNode* n = &ast->nodes[nodeId];
            if (n->kind == H2Ast_FN && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "main"))
            {
                int32_t child = ASTFirstChild(ast, nodeId);
                int     paramCount = 0;
                int     hasReturnType = 0;
                int     hasBody = 0;

                while (child >= 0) {
                    const H2AstNode* ch = &ast->nodes[child];
                    if (ch->kind == H2Ast_PARAM) {
                        paramCount++;
                    } else if (IsFnReturnTypeNodeKind(ch->kind) && ch->flags == 1) {
                        hasReturnType = 1;
                    } else if (ch->kind == H2Ast_BLOCK) {
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

int CheckPackageDir(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild) {
    H2PackageLoader loader;
    H2Package*      entryPkg = NULL;
    if (LoadAndCheckPackage(entryPath, platformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        return -1;
    }
    (void)entryPkg;
    FreeLoader(&loader);
    return 0;
}

H2_API_END
