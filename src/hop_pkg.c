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

HOP_API_BEGIN

static int ParseSource(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    HOPAst*     outAst,
    void**      outArenaMem,
    HOPArena* _Nullable outArena);

static int ParseSourceEx(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    HOPAst*     outAst,
    void**      outArenaMem,
    HOPArena* _Nullable outArena,
    int useLineColDiag) {
    void*    arenaMem;
    uint64_t arenaCap64;
    size_t   arenaCap;
    HOPArena arena;
    HOPDiag  diag = { 0 };

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
        arenaCap64 = arenaNodeCap * (uint64_t)sizeof(HOPAstNode) + 65536u;
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

    HOPArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (outArena != NULL) {
        HOPArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    }
    if (HOPParse(&arena, (HOPStrView){ source, sourceLen }, NULL, outAst, NULL, &diag) != 0) {
        (void)(useLineColDiag ? PrintHOPDiagLineCol(filename, source, &diag, 0)
                              : PrintHOPDiag(filename, source, &diag, 0));
        HOPArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }
    if (CompactAstInArena(&arena, outAst) != 0) {
        HOPDiag oomDiag = { 0 };
        oomDiag.code = HOPDiag_ARENA_OOM;
        oomDiag.type = HOPDiagTypeOfCode(HOPDiag_ARENA_OOM);
        oomDiag.start = 0;
        oomDiag.end = 0;
        oomDiag.argStart = ArenaBytesUsed(&arena);
        oomDiag.argEnd = ArenaBytesCapacity(&arena);
        (void)(useLineColDiag ? PrintHOPDiagLineCol(filename, source, &oomDiag, 0)
                              : PrintHOPDiag(filename, source, &oomDiag, 0));
        HOPArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }

    *outArenaMem = arenaMem;
    if (outArena != NULL) {
        HOPArenaBlock* oldInline = &arena.inlineBlock;
        int            currentIsInline = arena.current == oldInline;
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
    HOPAst*     outAst,
    void**      outArenaMem,
    HOPArena* _Nullable outArena) {
    return ParseSourceEx(filename, source, sourceLen, outAst, outArenaMem, outArena, 0);
}

static void WarnUnknownFeatureImports(const char* filename, const char* source, const HOPAst* ast);

typedef struct {
    const char*                 filename;
    const char*                 source;
    uint32_t                    sourceLen;
    int                         useLineColDiag;
    const HOPCombinedSourceMap* remapMap;
    const char*                 remapSource;
    int                         suppressUnusedWarnings;
} HOPCheckRunSpec;

static int IsUnusedWarningDiag(HOPDiagCode code) {
    return code == HOPDiag_UNUSED_FUNCTION || code == HOPDiag_UNUSED_VARIABLE
        || code == HOPDiag_UNUSED_VARIABLE_NEVER_READ || code == HOPDiag_UNUSED_PARAMETER
        || code == HOPDiag_UNUSED_PARAMETER_NEVER_READ;
}

static int CheckRunHasRemap(const HOPCheckRunSpec* spec) {
    return spec != NULL && spec->remapMap != NULL && spec->remapSource != NULL;
}

static int EmitCheckDiag(
    const HOPCheckRunSpec* spec,
    const HOPDiag*         diag,
    int                    includeHint,
    int                    dropUnmappedUnusedWarnings) {
    const char*        displaySource;
    const char*        displayFilename;
    const HOPDiag*     toPrint = diag;
    HOPDiag            remappedDiag;
    HOPRemapDiagStatus remapStatus = { 0 };
    uint32_t           remappedFileIndex = 0;

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
             ? PrintHOPDiagLineCol(displayFilename, displaySource, toPrint, includeHint)
             : PrintHOPDiag(displayFilename, displaySource, toPrint, includeHint);
}

static void TypecheckDiagSink(void* ctx, const HOPDiag* diag) {
    HOPCheckRunSpec* spec = (HOPCheckRunSpec*)ctx;
    if (spec == NULL || diag == NULL) {
        return;
    }
    if (spec->suppressUnusedWarnings && IsUnusedWarningDiag(diag->code)) {
        return;
    }
    (void)EmitCheckDiag(spec, diag, 1, 1);
}

static int CheckSourceWithSpec(const HOPCheckRunSpec* spec) {
    void*               arenaMem;
    uint64_t            arenaCap64;
    size_t              arenaCap;
    HOPArena            arena;
    HOPAst              ast;
    HOPDiag             diag = { 0 };
    uint32_t            beforeTypecheckUsed;
    uint32_t            beforeTypecheckCap;
    uint32_t            afterTypecheckUsed;
    uint32_t            afterTypecheckCap;
    HOPTypeCheckOptions checkOptions = {
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
    arenaCap64 = (uint64_t)(spec->sourceLen + 128u) * (uint64_t)sizeof(HOPAstNode) + 65536u;
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
    HOPArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    HOPArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    if (HOPParse(&arena, (HOPStrView){ spec->source, spec->sourceLen }, NULL, &ast, NULL, &diag)
        != 0)
    {
        (void)EmitCheckDiag(spec, &diag, 0, 0);
        HOPArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }
    if (CompactAstInArena(&arena, &ast) != 0) {
        HOPDiag oomDiag = { 0 };
        oomDiag.code = HOPDiag_ARENA_OOM;
        oomDiag.type = HOPDiagTypeOfCode(HOPDiag_ARENA_OOM);
        oomDiag.start = 0;
        oomDiag.end = 0;
        oomDiag.argStart = ArenaBytesUsed(&arena);
        oomDiag.argEnd = ArenaBytesCapacity(&arena);
        (void)EmitCheckDiag(spec, &oomDiag, 0, 0);
        HOPArenaDispose(&arena);
        free(arenaMem);
        return -1;
    }

    WarnUnknownFeatureImports(spec->filename, spec->source, &ast);
    beforeTypecheckUsed = ArenaBytesUsed(&arena);
    beforeTypecheckCap = ArenaBytesCapacity(&arena);

    if (HOPTypeCheckEx(
            &arena, &ast, (HOPStrView){ spec->source, spec->sourceLen }, &checkOptions, &diag)
        != 0)
    {
        if (diag.code == HOPDiag_ARENA_OOM) {
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
        HOPArenaDispose(&arena);
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
            ast.len <= UINT32_MAX / (uint32_t)sizeof(HOPAstNode)
                ? ast.len * (uint32_t)sizeof(HOPAstNode)
                : UINT32_MAX,
            beforeTypecheckUsed,
            beforeTypecheckCap,
            afterTypecheckUsed,
            afterTypecheckCap);
    }

    HOPArenaDispose(&arena);
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
    HOPCheckRunSpec spec = {
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
    const HOPCombinedSourceMap* remapMap,
    int                         suppressUnusedWarnings) {
    if (filename == NULL || source == NULL) {
        return -1;
    }
    HOPCheckRunSpec spec = {
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

static int IsDeclKind(HOPAstKind kind) {
    return kind == HOPAst_FN || kind == HOPAst_STRUCT || kind == HOPAst_UNION || kind == HOPAst_ENUM
        || kind == HOPAst_TYPE_ALIAS || kind == HOPAst_VAR || kind == HOPAst_CONST;
}

static int IsPubDeclNode(const HOPAstNode* n) {
    return (n->flags & HOPAstFlag_PUB) != 0;
}

static int FnNodeHasBody(const HOPAst* ast, int32_t nodeId) {
    int32_t child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_BLOCK) {
            return 1;
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

static int FnNodeHasAnytypeParam(const HOPParsedFile* file, int32_t nodeId) {
    int32_t child = ASTFirstChild(&file->ast, nodeId);
    while (child >= 0) {
        const HOPAstNode* n = &file->ast.nodes[child];
        if (n->kind == HOPAst_PARAM) {
            int32_t           typeNode = ASTFirstChild(&file->ast, child);
            const HOPAstNode* t =
                (typeNode >= 0 && (uint32_t)typeNode < file->ast.len)
                    ? &file->ast.nodes[typeNode]
                    : NULL;
            if (t != NULL && t->kind == HOPAst_TYPE_NAME && t->dataEnd > t->dataStart
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

static int FnNodeHasContextClause(const HOPAst* ast, int32_t nodeId) {
    int32_t child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_CONTEXT_CLAUSE) {
            return 1;
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

int DirectiveNameEq(const HOPParsedFile* file, int32_t nodeId, const char* name) {
    const HOPAstNode* n =
        nodeId >= 0 && (uint32_t)nodeId < file->ast.len ? &file->ast.nodes[nodeId] : NULL;
    size_t len = strlen(name);
    return n != NULL && n->kind == HOPAst_DIRECTIVE && n->dataEnd >= n->dataStart
        && (size_t)(n->dataEnd - n->dataStart) == len
        && memcmp(file->source + n->dataStart, name, len) == 0;
}

static uint32_t DirectiveArgCount(const HOPAst* ast, int32_t nodeId) {
    uint32_t count = 0;
    int32_t  child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        count++;
        child = ASTNextSibling(ast, child);
    }
    return count;
}

int32_t DirectiveArgAt(const HOPAst* ast, int32_t nodeId, uint32_t index) {
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
    const HOPAst* ast, int32_t declNodeId, int32_t* outFirstDirective, int32_t* outLastDirective) {
    int32_t child;
    int32_t first = -1;
    int32_t last = -1;
    if (ast == NULL || outFirstDirective == NULL || outLastDirective == NULL) {
        return -1;
    }
    child = ASTFirstChild(ast, ast->root);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (n->kind == HOPAst_DIRECTIVE) {
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

static uint32_t DeclTextSourceStart(const HOPParsedFile* file, int32_t nodeId) {
    int32_t firstDirective = -1;
    int32_t lastDirective = -1;
    if (FindAttachedDirectiveRun(&file->ast, nodeId, &firstDirective, &lastDirective) == 0
        && firstDirective >= 0)
    {
        return file->ast.nodes[firstDirective].start;
    }
    return file->ast.nodes[nodeId].start;
}

static int AppendDirectiveRunForDecl(
    HOPStringBuilder* b, const HOPParsedFile* file, int32_t nodeId, int omitExportDirective) {
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
        const HOPAstNode* n = &file->ast.nodes[child];
        if (n->kind != HOPAst_DIRECTIVE) {
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
    const HOPParsedFile* file, int32_t nodeId, int isPubSurface) {
    const HOPAstNode* n = &file->ast.nodes[nodeId];
    uint32_t          declStart = n->start;
    uint32_t          declEnd = n->end;
    HOPStringBuilder  b = { 0 };

    if (!isPubSurface) {
        return HOPCDupSlice(file->source, DeclTextSourceStart(file, nodeId), n->end);
    }

    if (AppendDirectiveRunForDecl(&b, file, nodeId, 1) != 0) {
        free(b.v);
        return NULL;
    }
    if ((n->flags & HOPAstFlag_PUB) != 0 && declStart + 3u <= declEnd
        && memcmp(file->source + declStart, "pub", 3u) == 0)
    {
        declStart += 3u;
        while (declStart < declEnd && IsAsciiSpaceChar(file->source[declStart])) {
            declStart++;
        }
    }
    if (n->kind == HOPAst_FN && FnNodeHasBody(&file->ast, nodeId)
        && !FnNodeHasAnytypeParam(file, nodeId))
    {
        int32_t body = ASTFirstChild(&file->ast, nodeId);
        while (body >= 0) {
            if (file->ast.nodes[body].kind == HOPAst_BLOCK) {
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
    const HOPPackage* pkg, const HOPParsedFile* file, int32_t nodeId) {
    const HOPAstNode* target = &file->ast.nodes[nodeId];
    uint32_t          fileIndex;
    uint32_t          count = 0;
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const HOPParsedFile* scanFile = &pkg->files[fileIndex];
        int32_t              child = ASTFirstChild(&scanFile->ast, scanFile->ast.root);
        while (child >= 0) {
            const HOPAstNode* n = &scanFile->ast.nodes[child];
            if (n->kind == HOPAst_FN && n->dataEnd > n->dataStart
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

static int VarLikeNodeHasInitializer(const HOPAst* ast, int32_t nodeId) {
    int32_t firstChild = ASTFirstChild(ast, nodeId);
    int32_t afterNames =
        firstChild >= 0 && ast->nodes[firstChild].kind == HOPAst_NAME_LIST
            ? ASTNextSibling(ast, firstChild)
            : firstChild;
    if (afterNames < 0) {
        return 0;
    }
    if ((ast->nodes[afterNames].kind == HOPAst_TYPE_NAME
         || ast->nodes[afterNames].kind == HOPAst_TYPE_PTR
         || ast->nodes[afterNames].kind == HOPAst_TYPE_REF
         || ast->nodes[afterNames].kind == HOPAst_TYPE_MUTREF
         || ast->nodes[afterNames].kind == HOPAst_TYPE_ARRAY
         || ast->nodes[afterNames].kind == HOPAst_TYPE_VARRAY
         || ast->nodes[afterNames].kind == HOPAst_TYPE_SLICE
         || ast->nodes[afterNames].kind == HOPAst_TYPE_MUTSLICE
         || ast->nodes[afterNames].kind == HOPAst_TYPE_OPTIONAL
         || ast->nodes[afterNames].kind == HOPAst_TYPE_FN
         || ast->nodes[afterNames].kind == HOPAst_TYPE_ANON_STRUCT
         || ast->nodes[afterNames].kind == HOPAst_TYPE_ANON_UNION
         || ast->nodes[afterNames].kind == HOPAst_TYPE_TUPLE))
    {
        return ASTNextSibling(ast, afterNames) >= 0;
    }
    return 1;
}

static int VarLikeNodeUsesGroupedNames(const HOPAst* ast, int32_t nodeId) {
    int32_t firstChild = ASTFirstChild(ast, nodeId);
    return firstChild >= 0 && ast->nodes[firstChild].kind == HOPAst_NAME_LIST;
}

static int ValidateDirectiveRunOnDecl(
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    const char* _Nullable platformTarget,
    int32_t firstDirective,
    int32_t lastDirective,
    int32_t declNodeId) {
    const HOPAstNode* decl = &file->ast.nodes[declNodeId];
    int32_t           child = firstDirective;
    int32_t           cImportNode = -1;
    int32_t           wasmImportNode = -1;
    int32_t           exportNode = -1;
    int               hasForeignFnDirective = 0;

    while (child >= 0) {
        const HOPAstNode* dir = &file->ast.nodes[child];
        uint32_t          argCount = DirectiveArgCount(&file->ast, child);
        int32_t           arg0;
        int32_t           arg1;
        if (dir->kind != HOPAst_DIRECTIVE) {
            break;
        }
        if (DirectiveNameEq(file, child, "c_import")) {
            if (cImportNode >= 0 || wasmImportNode >= 0 || exportNode >= 0) {
                return Errorf(
                    file->path, file->source, dir->start, dir->end, "duplicate foreign directive");
            }
            if (decl->kind != HOPAst_FN && decl->kind != HOPAst_VAR && decl->kind != HOPAst_CONST) {
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
            if (arg0 < 0 || file->ast.nodes[arg0].kind != HOPAst_STRING) {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@c_import expects string arguments");
            }
            cImportNode = child;
            hasForeignFnDirective = decl->kind == HOPAst_FN;
        } else if (DirectiveNameEq(file, child, "wasm_import")) {
            if (cImportNode >= 0 || wasmImportNode >= 0 || exportNode >= 0) {
                return Errorf(
                    file->path, file->source, dir->start, dir->end, "duplicate foreign directive");
            }
            if (decl->kind != HOPAst_FN && decl->kind != HOPAst_VAR && decl->kind != HOPAst_CONST) {
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
            if (arg0 < 0 || arg1 < 0 || file->ast.nodes[arg0].kind != HOPAst_STRING
                || file->ast.nodes[arg1].kind != HOPAst_STRING)
            {
                return Errorf(
                    file->path,
                    file->source,
                    dir->start,
                    dir->end,
                    "@wasm_import expects string arguments");
            }
            wasmImportNode = child;
            hasForeignFnDirective = decl->kind == HOPAst_FN;
        } else if (DirectiveNameEq(file, child, "export")) {
            if (cImportNode >= 0 || wasmImportNode >= 0 || exportNode >= 0) {
                return Errorf(
                    file->path, file->source, dir->start, dir->end, "duplicate foreign directive");
            }
            if (decl->kind != HOPAst_FN || !IsPubDeclNode(decl)
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
            if (arg0 < 0 || file->ast.nodes[arg0].kind != HOPAst_STRING) {
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

    if ((cImportNode >= 0 || wasmImportNode >= 0) && decl->kind == HOPAst_FN
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
    if ((cImportNode >= 0 || wasmImportNode >= 0) && decl->kind == HOPAst_FN
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
        && (decl->kind == HOPAst_VAR || decl->kind == HOPAst_CONST))
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
        && StrEq(platformTarget, HOP_EVAL_PLATFORM_TARGET))
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
    const HOPPackage* pkg, const char* _Nullable platformTarget) {
    uint32_t fileIndex;
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const HOPParsedFile* file = &pkg->files[fileIndex];
        int32_t              child = ASTFirstChild(&file->ast, file->ast.root);
        int32_t              firstDirective = -1;
        int32_t              lastDirective = -1;
        while (child >= 0) {
            const HOPAstNode* n = &file->ast.nodes[child];
            if (n->kind == HOPAst_DIRECTIVE) {
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

static int PackageUsesWasmImportDirective(const HOPPackage* pkg) {
    uint32_t fileIndex;
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const HOPParsedFile* file = &pkg->files[fileIndex];
        int32_t              child = ASTFirstChild(&file->ast, file->ast.root);
        while (child >= 0) {
            if (DirectiveNameEq(file, child, "wasm_import")) {
                return 1;
            }
            child = ASTNextSibling(&file->ast, child);
        }
    }
    return 0;
}

int LoaderUsesWasmImportDirective(const HOPPackageLoader* loader) {
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
    return HOPCDupCStr(name);
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

static int IsImportAliasUsed(const HOPPackage* pkg, const char* alias) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        if (StrEq(pkg->imports[i].alias, alias)) {
            return 1;
        }
    }
    return 0;
}

static char* _Nullable MakeUniqueImportAlias(const HOPPackage* pkg, const char* preferred) {
    char*    alias;
    uint32_t n;
    if (preferred != NULL && preferred[0] != '\0' && IsValidIdentifier(preferred)
        && !IsImportAliasUsed(pkg, preferred))
    {
        return HOPCDupCStr(preferred);
    }
    if (preferred == NULL || preferred[0] == '\0' || !IsValidIdentifier(preferred)) {
        preferred = "imp";
    }
    alias = HOPCDupCStr(preferred);
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

static int StringLiteralHasDirectOffsetMapping(const char* source, const HOPAstNode* n) {
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

static void WarnUnknownFeatureImports(const char* filename, const char* source, const HOPAst* ast) {
    int32_t child = ASTFirstChild(ast, ast->root);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (n->kind == HOPAst_IMPORT) {
            uint8_t* decoded = NULL;
            uint32_t decodedLen = 0;
            if (HOPDecodeStringLiteralMalloc(
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
                        HOPDiag diag = {
                            .code = HOPDiag_UNKNOWN_FEATURE,
                            .type = HOPDiagTypeOfCode(HOPDiag_UNKNOWN_FEATURE),
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
    HOPPackage* pkg,
    const char* filePath,
    char*       source,
    uint32_t    sourceLen,
    HOPAst      ast,
    void*       arenaMem) {
    HOPParsedFile* f;
    if (EnsureCap((void**)&pkg->files, &pkg->fileCap, pkg->fileLen + 1u, sizeof(HOPParsedFile))
        != 0)
    {
        return -1;
    }
    f = &pkg->files[pkg->fileLen++];
    memset(f, 0, sizeof(*f));
    f->path = HOPCDupCStr(filePath);
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
    HOPPackage* pkg,
    char*       text,
    uint32_t    fileIndex,
    int32_t     nodeId,
    uint32_t    sourceStart,
    uint32_t    sourceEnd) {
    HOPDeclText* t;
    if (EnsureCap(
            (void**)&pkg->declTexts, &pkg->declTextCap, pkg->declTextLen + 1u, sizeof(HOPDeclText))
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
    HOPSymbolDecl** arr,
    uint32_t*       len,
    uint32_t*       cap,
    HOPAstKind      kind,
    char*           name,
    char*           declText,
    int             hasBody,
    uint32_t        fileIndex,
    int32_t         nodeId) {
    HOPSymbolDecl* d;
    if (EnsureCap((void**)arr, cap, *len + 1u, sizeof(HOPSymbolDecl)) != 0) {
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
    HOPPackage* pkg,
    char*       alias,
    char* _Nullable bindName,
    char*     importPath,
    uint32_t  fileIndex,
    uint32_t  start,
    uint32_t  end,
    uint32_t* outIndex) {
    HOPImportRef* imp;
    if (EnsureCap((void**)&pkg->imports, &pkg->importCap, pkg->importLen + 1u, sizeof(HOPImportRef))
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
    HOPPackage* pkg,
    uint32_t    importIndex,
    char*       sourceName,
    char*       localName,
    uint32_t    fileIndex,
    uint32_t    start,
    uint32_t    end) {
    HOPImportSymbolRef* sym;
    if (EnsureCap(
            (void**)&pkg->importSymbols,
            &pkg->importSymbolCap,
            pkg->importSymbolLen + 1u,
            sizeof(HOPImportSymbolRef))
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

static char* _Nullable DupPubDeclText(const HOPParsedFile* file, int32_t nodeId) {
    const HOPAstNode* n = &file->ast.nodes[nodeId];
    uint32_t          start = n->start;
    uint32_t          end = n->end;

    if (start + 3u <= end && memcmp(file->source + start, "pub", 3u) == 0) {
        start += 3u;
        while (start < end && IsAsciiSpaceChar(file->source[start])) {
            start++;
        }
    }

    if (n->kind == HOPAst_FN && FnNodeHasBody(&file->ast, nodeId)
        && !FnNodeHasAnytypeParam(file, nodeId))
    {
        int32_t body = ASTFirstChild(&file->ast, nodeId);
        while (body >= 0) {
            if (file->ast.nodes[body].kind == HOPAst_BLOCK) {
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

    return HOPCDupSlice(file->source, start, end);
}

static int AddDeclFromNode(
    HOPPackage* pkg, const HOPParsedFile* file, uint32_t fileIndex, int32_t nodeId, int isPub) {
    const HOPAstNode* n = &file->ast.nodes[nodeId];
    int32_t           firstChild;

    if (!IsDeclKind(n->kind)) {
        return 0;
    }
    if (n->end < n->start || n->end > file->sourceLen) {
        return Errorf(file->path, file->source, n->start, n->end, "invalid declaration");
    }
    firstChild = ASTFirstChild(&file->ast, nodeId);
    if ((n->kind == HOPAst_VAR || n->kind == HOPAst_CONST) && firstChild >= 0
        && file->ast.nodes[firstChild].kind == HOPAst_NAME_LIST)
    {
        uint32_t i;
        uint32_t nameCount = AstListCount(&file->ast, firstChild);
        for (i = 0; i < nameCount; i++) {
            int32_t           nameNode = AstListItemAt(&file->ast, firstChild, i);
            const HOPAstNode* nameAst =
                (nameNode >= 0 && (uint32_t)nameNode < file->ast.len)
                    ? &file->ast.nodes[nameNode]
                    : NULL;
            char* name;
            char* declText;
            if (nameAst == NULL || nameAst->dataEnd <= nameAst->dataStart) {
                return Errorf(file->path, file->source, n->start, n->end, "invalid declaration");
            }
            name = HOPCDupSlice(file->source, nameAst->dataStart, nameAst->dataEnd);
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
                        && n->kind != HOPAst_FN)
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
        char* name = HOPCDupSlice(file->source, n->dataStart, n->dataEnd);
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
                    && n->kind != HOPAst_FN)
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

static int ProcessParsedFile(HOPPackage* pkg, uint32_t fileIndex) {
    const HOPParsedFile* file = &pkg->files[fileIndex];
    const HOPAst*        ast = &file->ast;
    int32_t              child = ASTFirstChild(ast, ast->root);

    /* Accumulate feature flags from this file into the package. */
    pkg->features |= ast->features;

    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (n->kind == HOPAst_IMPORT) {
            int32_t           importChild = ASTFirstChild(ast, child);
            const HOPAstNode* aliasNode = NULL;
            int               hasSymbols = 0;
            uint8_t*          decodedPathBytes = NULL;
            const char*       pathErr = NULL;
            char*             decodedPath = NULL;
            char*             importPath = NULL;
            uint32_t          decodedPathLen = 0;
            char* _Nullable bindName = NULL;
            int aliasIsUnderscore = 0;
            char* _Nullable mangleAlias = NULL;
            uint32_t importIndex = 0;

            while (importChild >= 0) {
                const HOPAstNode* ch = &ast->nodes[importChild];
                if (ch->kind == HOPAst_IDENT) {
                    if (aliasNode != NULL) {
                        return Errorf(
                            file->path,
                            file->source,
                            n->start,
                            n->end,
                            "invalid import declaration");
                    }
                    aliasNode = ch;
                } else if (ch->kind == HOPAst_IMPORT_SYMBOL) {
                    hasSymbols = 1;
                } else {
                    return Errorf(
                        file->path, file->source, n->start, n->end, "invalid import declaration");
                }
                importChild = ASTNextSibling(ast, importChild);
            }

            if (HOPDecodeStringLiteralMalloc(
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
                            HOPDiag_IMPORT_INVALID_PATH,
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
            if (HOPNormalizeImportPath(decodedPath, importPath, decodedPathLen + 1u, &pathErr) != 0)
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
                        HOPDiag_IMPORT_INVALID_PATH,
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
                        HOPDiag_IMPORT_FEATURE_IMPORT_EXTRAS);
                    free(importPath);
                    return rc;
                }
                free(importPath);
                child = ASTNextSibling(ast, child);
                continue;
            }

            if (aliasNode != NULL) {
                bindName = HOPCDupSlice(file->source, aliasNode->dataStart, aliasNode->dataEnd);
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
                    HOPDiag_IMPORT_SIDE_EFFECT_ALIAS_WITH_SYMBOLS);
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
                        HOPDiag_IMPORT_ALIAS_INFERENCE_FAILED,
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
                const HOPAstNode* ch = &ast->nodes[importChild];
                if (ch->kind == HOPAst_IMPORT_SYMBOL) {
                    int32_t localAliasNode = ASTFirstChild(ast, importChild);
                    char*   sourceName = HOPCDupSlice(file->source, ch->dataStart, ch->dataEnd);
                    char*   localName;
                    if (sourceName == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    if (localAliasNode >= 0 && ast->nodes[localAliasNode].kind == HOPAst_IDENT) {
                        const HOPAstNode* ln = &ast->nodes[localAliasNode];
                        localName = HOPCDupSlice(file->source, ln->dataStart, ln->dataEnd);
                    } else {
                        localName = HOPCDupCStr(sourceName);
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
                            HOPDiag_IMPORT_SYMBOL_ALIAS_INVALID);
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
        } else if (n->kind != HOPAst_DIRECTIVE) {
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

static int PackageHasExport(const HOPPackage* pkg, const char* name) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (StrEq(pkg->pubDecls[i].name, name)) {
            return 1;
        }
    }
    return 0;
}

static int PackageHasExportSlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
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
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
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
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                    nameLen;
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
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end) {
    uint32_t i;
    if (pkg == NULL) {
        return 0;
    }
    for (i = 0; i < pkg->importLen; i++) {
        const HOPImportRef* imp = &pkg->imports[i];
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

const HOPImportRef* _Nullable FindImportByAliasSlice(
    const HOPPackage* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd);
static int FindImportIndexByPath(const HOPPackage* pkg, const char* importPath);
static int IsBuiltinPackage(const HOPPackage* pkg);

static int ValidatePubTypeNode(
    const HOPPackage* pkg, const HOPParsedFile* file, int32_t typeNodeId, const char* contextMsg) {
    const HOPAstNode* n;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= file->ast.len) {
        return ErrorSimple("invalid type node");
    }
    n = &file->ast.nodes[typeNodeId];
    switch (n->kind) {
        case HOPAst_TYPE_NAME: {
            uint32_t dotPos = FindSliceDot(file->source, n->dataStart, n->dataEnd);
            if (dotPos < n->dataEnd) {
                const HOPImportRef* imp = FindImportByAliasSlice(
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
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF:
        case HOPAst_TYPE_ARRAY:
        case HOPAst_TYPE_SLICE:
        case HOPAst_TYPE_MUTSLICE:
        case HOPAst_TYPE_VARRAY:
        case HOPAst_TYPE_OPTIONAL: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            return ValidatePubTypeNode(pkg, file, child, contextMsg);
        }
        case HOPAst_TYPE_TUPLE: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            while (child >= 0) {
                if (ValidatePubTypeNode(pkg, file, child, contextMsg) != 0) {
                    return -1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
            return 0;
        }
        case HOPAst_TYPE_FN: {
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

static int ValidatePubClosure(const HOPPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        const HOPSymbolDecl* pubDecl = &pkg->pubDecls[i];
        const HOPParsedFile* file = &pkg->files[pubDecl->fileIndex];
        int32_t              child = ASTFirstChild(&file->ast, pubDecl->nodeId);
        if (pubDecl->kind == HOPAst_FN) {
            while (child >= 0) {
                const HOPAstNode* n = &file->ast.nodes[child];
                if (n->kind == HOPAst_PARAM) {
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
        } else if (pubDecl->kind == HOPAst_STRUCT || pubDecl->kind == HOPAst_UNION) {
            while (child >= 0) {
                const HOPAstNode* n = &file->ast.nodes[child];
                if (n->kind == HOPAst_FIELD) {
                    int32_t typeNode = ASTFirstChild(&file->ast, child);
                    if (ValidatePubTypeNode(pkg, file, typeNode, "field type") != 0) {
                        return -1;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
        } else if (pubDecl->kind == HOPAst_ENUM) {
            if (child >= 0) {
                const HOPAstNode* n = &file->ast.nodes[child];
                if (n->kind == HOPAst_TYPE_NAME || n->kind == HOPAst_TYPE_PTR
                    || n->kind == HOPAst_TYPE_REF || n->kind == HOPAst_TYPE_MUTREF
                    || n->kind == HOPAst_TYPE_ARRAY || n->kind == HOPAst_TYPE_VARRAY
                    || n->kind == HOPAst_TYPE_SLICE || n->kind == HOPAst_TYPE_MUTSLICE
                    || n->kind == HOPAst_TYPE_FN || n->kind == HOPAst_TYPE_TUPLE)
                {
                    if (ValidatePubTypeNode(pkg, file, child, "enum base type") != 0) {
                        return -1;
                    }
                }
            }
        } else if (pubDecl->kind == HOPAst_VAR || pubDecl->kind == HOPAst_CONST) {
            if (child >= 0 && IsFnReturnTypeNodeKind(file->ast.nodes[child].kind)) {
                if (ValidatePubTypeNode(
                        pkg,
                        file,
                        child,
                        pubDecl->kind == HOPAst_VAR ? "variable type" : "constant type")
                    != 0)
                {
                    return -1;
                }
            }
        } else if (pubDecl->kind == HOPAst_TYPE_ALIAS) {
            const HOPAstNode* aliasNode = &file->ast.nodes[pubDecl->nodeId];
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

static int ValidatePubFnDefinitions(const HOPPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        const HOPSymbolDecl* pubDecl = &pkg->pubDecls[i];
        uint32_t             j;
        int                  found = pubDecl->hasBody;
        if (pubDecl->kind != HOPAst_FN) {
            continue;
        }
        for (j = 0; j < pkg->declLen; j++) {
            const HOPSymbolDecl* decl = &pkg->decls[j];
            if (decl->kind == HOPAst_FN && StrEq(decl->name, pubDecl->name) && decl->hasBody) {
                found = 1;
                break;
            }
        }
        if (!found) {
            const HOPParsedFile* file = &pkg->files[pubDecl->fileIndex];
            const HOPAstNode*    n = &file->ast.nodes[pubDecl->nodeId];
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

const HOPImportRef* _Nullable FindImportByAliasSlice(
    const HOPPackage* pkg, const char* src, uint32_t aliasStart, uint32_t aliasEnd) {
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
    const HOPPackage* pkg, const char* src, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    size_t   n = (size_t)(nameEnd - nameStart);
    for (i = 0; i < pkg->declLen; i++) {
        if (pkg->decls[i].kind == HOPAst_ENUM && strlen(pkg->decls[i].name) == n
            && memcmp(pkg->decls[i].name, src + nameStart, n) == 0)
        {
            return 1;
        }
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (pkg->pubDecls[i].kind == HOPAst_ENUM && strlen(pkg->pubDecls[i].name) == n
            && memcmp(pkg->pubDecls[i].name, src + nameStart, n) == 0)
        {
            return 1;
        }
    }
    return 0;
}

static int ValidateSelectorsNode(const HOPPackage* pkg, const HOPParsedFile* file, int32_t nodeId) {
    const HOPAstNode* n;
    int32_t           child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];

    if (n->kind == HOPAst_TYPE_NAME) {
        uint32_t dot = FindSliceDot(file->source, n->dataStart, n->dataEnd);
        if (dot < n->dataEnd) {
            const HOPImportRef* imp = FindImportByAliasSlice(pkg, file->source, n->dataStart, dot);
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
    } else if (n->kind == HOPAst_FIELD_EXPR) {
        int32_t recvNode = ASTFirstChild(&file->ast, nodeId);
        if (recvNode >= 0 && file->ast.nodes[recvNode].kind == HOPAst_IDENT) {
            const HOPAstNode*   recv = &file->ast.nodes[recvNode];
            const HOPImportRef* imp = FindImportByAliasSlice(
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

static int ValidatePackageSelectors(const HOPPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->fileLen; i++) {
        const HOPParsedFile* file = &pkg->files[i];
        if (ValidateSelectorsNode(pkg, file, file->ast.root) != 0) {
            return -1;
        }
    }
    return 0;
}

static int PackageHasAnyDeclName(const HOPPackage* pkg, const char* name) {
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
    const HOPPackage* pkg, const HOPSymbolDecl* decl, uint32_t* outStart, uint32_t* outEnd) {
    const HOPParsedFile* file;
    const HOPAstNode*    n;
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
    if ((n->kind == HOPAst_VAR || n->kind == HOPAst_CONST) && n->firstChild >= 0
        && (uint32_t)n->firstChild < file->ast.len
        && file->ast.nodes[n->firstChild].kind == HOPAst_NAME_LIST)
    {
        int32_t child = file->ast.nodes[n->firstChild].firstChild;
        while (child >= 0) {
            const HOPAstNode* nameNode;
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
    const HOPPackage*    pkg,
    const HOPSymbolDecl* decl,
    const HOPPackage*    builtinPkg,
    const HOPSymbolDecl* builtinDecl) {
    const HOPParsedFile* file;
    const HOPParsedFile* builtinFile = NULL;
    uint32_t             start = 0;
    uint32_t             end = 0;
    uint32_t             line = 0;
    uint32_t             col = 0;
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

static const HOPSymbolDecl* _Nullable FindBuiltinPubDeclByName(
    const HOPPackage* builtinPkg, const char* name) {
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

static const HOPSymbolDecl* _Nullable FindBuiltinNonFunctionPubDeclByName(
    const HOPPackage* builtinPkg, const char* name) {
    uint32_t i;
    if (builtinPkg == NULL || name == NULL) {
        return NULL;
    }
    for (i = 0; i < builtinPkg->pubDeclLen; i++) {
        if (builtinPkg->pubDecls[i].kind != HOPAst_FN && StrEq(builtinPkg->pubDecls[i].name, name))
        {
            return &builtinPkg->pubDecls[i];
        }
    }
    return NULL;
}

static int ValidateBuiltinNameConflictsForDecls(
    const HOPPackage*    pkg,
    const HOPPackage*    builtinPkg,
    const HOPSymbolDecl* decls,
    uint32_t             declLen) {
    uint32_t i;
    for (i = 0; i < declLen; i++) {
        const HOPSymbolDecl* decl = &decls[i];
        const HOPSymbolDecl* builtinDecl;
        if (decl->kind == HOPAst_FN) {
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

static int ValidateBuiltinNameConflicts(HOPPackage* pkg) {
    int               builtinImportIndex;
    const HOPPackage* builtinPkg;
    uint32_t          i;
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
        const HOPImportRef* imp = &pkg->imports[i];
        if (imp->bindName != NULL && FindBuiltinPubDeclByName(builtinPkg, imp->bindName) != NULL) {
            const HOPParsedFile* file = &pkg->files[imp->fileIndex];
            return Errorf(
                file->path, file->source, imp->start, imp->end, "import binding conflict");
        }
    }
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        const HOPSymbolDecl* builtinDecl = FindBuiltinPubDeclByName(builtinPkg, sym->localName);
        if (builtinDecl == NULL) {
            continue;
        }
        if (!(sym->isFunction && builtinDecl->kind == HOPAst_FN)) {
            const HOPParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "import binding conflict");
        }
    }
    return 0;
}

static int ValidateImportBindingConflicts(HOPPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        const HOPImportRef* imp = &pkg->imports[i];
        uint32_t            j;
        if (imp->bindName == NULL) {
            continue;
        }
        if (PackageHasAnyDeclName(pkg, imp->bindName)) {
            const HOPParsedFile* file = &pkg->files[imp->fileIndex];
            return Errorf(
                file->path, file->source, imp->start, imp->end, "import binding conflict");
        }
        for (j = i + 1u; j < pkg->importLen; j++) {
            if (pkg->imports[j].bindName != NULL && StrEq(pkg->imports[j].bindName, imp->bindName))
            {
                const HOPParsedFile* file = &pkg->files[pkg->imports[j].fileIndex];
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
                const HOPParsedFile* file = &pkg->files[pkg->importSymbols[j].fileIndex];
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
        HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        uint32_t            j;
        if (PackageHasAnyDeclName(pkg, sym->localName)) {
            const HOPParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "import binding conflict");
        }
        for (j = i + 1u; j < pkg->importSymbolLen; j++) {
            HOPImportSymbolRef* other = &pkg->importSymbols[j];
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
                const HOPParsedFile* file = &pkg->files[other->fileIndex];
                return Errorf(
                    file->path, file->source, other->start, other->end, "import binding conflict");
            }
        }
    }
    return 0;
}

static int ValidateAndFinalizeImportSymbols(HOPPackage* pkg) {
    uint32_t baseLen = pkg->importSymbolLen;
    uint32_t i;
    for (i = 0; i < baseLen; i++) {
        HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        const HOPImportRef* imp;
        const HOPPackage*   dep;
        uint32_t            j;
        uint32_t            matchCount = 0;
        if (sym->importIndex >= pkg->importLen) {
            return ErrorSimple("internal error: invalid import symbol mapping");
        }
        imp = &pkg->imports[sym->importIndex];
        dep = imp->target;
        if (dep == NULL) {
            return ErrorSimple("internal error: unresolved import");
        }
        for (j = 0; j < dep->pubDeclLen; j++) {
            const HOPSymbolDecl* exportDecl = &dep->pubDecls[j];
            HOPImportSymbolRef*  dstSym = sym;
            char*                rewrittenDecl = NULL;
            char*                shapeKey = NULL;
            char*                wrapperDecl = NULL;
            if (!StrEq(exportDecl->name, sym->sourceName)) {
                continue;
            }
            if (matchCount > 0) {
                char* sourceName = HOPCDupCStr(sym->sourceName);
                char* localName = HOPCDupCStr(sym->localName);
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
            dstSym->isFunction = exportDecl->kind == HOPAst_FN ? 1u : 0u;
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
            const HOPParsedFile* file = &pkg->files[sym->fileIndex];
            return Errorf(
                file->path, file->source, sym->start, sym->end, "unknown imported symbol");
        }
    }
    return 0;
}

static HOPPackage* _Nullable FindPackageByDir(const HOPPackageLoader* loader, const char* dirPath) {
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

static int AddPackageSlot(HOPPackageLoader* loader, const char* dirPath, HOPPackage** outPkg) {
    HOPPackage* pkg;
    uint32_t    needCap;
    if (loader == NULL || outPkg == NULL) {
        return -1;
    }
    needCap = loader->packageLen + 1u;
    if (loader->packageCap == 0u && needCap < 256u) {
        needCap = 256u;
    }
    if (EnsureCap((void**)&loader->packages, &loader->packageCap, needCap, sizeof(HOPPackage)) != 0)
    {
        return -1;
    }
    pkg = &loader->packages[loader->packageLen++];
    memset(pkg, 0, sizeof(*pkg));
    pkg->dirPath = HOPCDupCStr(dirPath);
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
    dir = HOPCDupCStr(startDir);
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

static int LoadPackageRecursive(HOPPackageLoader* loader, const char* dirPath, HOPPackage** outPkg);

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
    const HOPPackageLoader* loader, const char* tagStart, size_t tagLen) {
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
    const HOPPackageLoader* loader, const char* filePath, int* outMatch) {
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
    const HOPPackageLoader* loader, const char* dirPath, char** filePaths, uint32_t* fileCount) {
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

static int FindImportIndexByPath(const HOPPackage* pkg, const char* importPath) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        if (StrEq(pkg->imports[i].path, importPath)) {
            return (int)i;
        }
    }
    return -1;
}

static int IsBuiltinPackage(const HOPPackage* pkg) {
    const char* base;
    if (pkg == NULL || pkg->dirPath == NULL) {
        return 0;
    }
    base = strrchr(pkg->dirPath, '/');
    base = base != NULL ? base + 1 : pkg->dirPath;
    return StrEq(base, "builtin");
}

static int IsReflectPackage(const HOPPackage* pkg) {
    const char* base;
    if (pkg == NULL || pkg->dirPath == NULL) {
        return 0;
    }
    base = strrchr(pkg->dirPath, '/');
    base = base != NULL ? base + 1 : pkg->dirPath;
    return StrEq(base, "reflect");
}

static int EnsureImplicitBuiltinImport(HOPPackage* pkg) {
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
    importPath = HOPCDupCStr("builtin");
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

static int EnsureImplicitReflectImport(HOPPackage* pkg) {
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
    importPath = HOPCDupCStr("reflect");
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
    HOPPackageLoader* loader, const char* startDir, HOPPackage** outPkg) {
    char* importPath = NULL;
    char* resolvedDir = NULL;
    int   rc = -1;

    if (loader->platformTarget == NULL || loader->platformTarget[0] == '\0') {
        return ErrorSimple("internal error: missing platform target");
    }

    {
        HOPStringBuilder b = { 0 };
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
        HOPPackage*  tmpPkg = NULL;
        HOPPackage** outPtr = outPkg != NULL ? outPkg : &tmpPkg;
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
    const HOPPackageLoader* loader, const char* _Nullable importPath) {
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

static int ResolvePackageImportsAndSelectors(HOPPackageLoader* loader, HOPPackage* pkg) {
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
            const HOPParsedFile* file = &pkg->files[pkg->imports[i].fileIndex];
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

static int LoadPackageRecursive(
    HOPPackageLoader* loader, const char* dirPath, HOPPackage** outPkg) {
    char*       canonical = CanonicalizePath(dirPath);
    HOPPackage* pkg;
    char**      filePaths = NULL;
    uint32_t    fileCount = 0;
    uint32_t    i;

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
        HOPAst   ast;
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
    HOPPackageLoader* loader, const char* filePath, HOPPackage** outPkg) {
    char*       dirPath = DirNameDup(filePath);
    HOPPackage* pkg;
    char*       source = NULL;
    uint32_t    sourceLen = 0;
    HOPAst      ast;
    void*       arenaMem = NULL;
    uint32_t    i;

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
    const HOPIdentMap* _Nullable maps,
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
} HOPTextRewrite;

static int AddTextRewrite(
    HOPTextRewrite** rewrites,
    uint32_t*        len,
    uint32_t*        cap,
    uint32_t         start,
    uint32_t         end,
    const char*      replacement) {
    if (EnsureCap((void**)rewrites, cap, *len + 1u, sizeof(HOPTextRewrite)) != 0) {
        return -1;
    }
    (*rewrites)[*len].start = start;
    (*rewrites)[*len].end = end;
    (*rewrites)[*len].replacement = replacement;
    (*len)++;
    return 0;
}

static int FindImportSymbolBindingIndexBySlice(
    const HOPPackage* pkg, const char* src, uint32_t start, uint32_t end, int wantType) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                    nameLen;
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
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    int32_t              nodeId,
    HOPTextRewrite**     rewrites,
    uint32_t*            rewriteLen,
    uint32_t*            rewriteCap) {
    const HOPAstNode* n;
    int32_t           child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    if (n->kind == HOPAst_TYPE_NAME) {
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
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    int32_t              nodeId,
    uint8_t*             shadowCounts,
    HOPTextRewrite**     rewrites,
    uint32_t*            rewriteLen,
    uint32_t*            rewriteCap) {
    const HOPAstNode* n;
    int32_t           child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    switch (n->kind) {
        case HOPAst_IDENT: {
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
        case HOPAst_CALL:
        case HOPAst_CALL_WITH_CONTEXT: {
            int32_t callee = ASTFirstChild(&file->ast, nodeId);
            if (callee >= 0 && (uint32_t)callee < file->ast.len
                && file->ast.nodes[callee].kind == HOPAst_FIELD_EXPR)
            {
                int32_t             recv = ASTFirstChild(&file->ast, callee);
                const HOPAstNode*   fieldExpr = &file->ast.nodes[callee];
                const HOPImportRef* imp = NULL;
                if (recv >= 0 && (uint32_t)recv < file->ast.len
                    && file->ast.nodes[recv].kind == HOPAst_IDENT)
                {
                    const HOPAstNode* recvNode = &file->ast.nodes[recv];
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
        case HOPAst_UNARY:
        case HOPAst_BINARY:
        case HOPAst_CONTEXT_OVERLAY:
        case HOPAst_CONTEXT_BIND:
        case HOPAst_INDEX:
        case HOPAst_CAST:
        case HOPAst_SIZEOF:
        case HOPAst_NEW:
        case HOPAst_UNWRAP:
        case HOPAst_CALL_ARG:
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
        case HOPAst_FIELD_EXPR: {
            int32_t recv = ASTFirstChild(&file->ast, nodeId);
            if (recv >= 0 && (uint32_t)recv < file->ast.len
                && file->ast.nodes[recv].kind == HOPAst_IDENT)
            {
                const HOPAstNode* recvNode = &file->ast.nodes[recv];
                int               idx = FindImportSymbolBindingIndexBySlice(
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
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    uint32_t             start,
    uint32_t             end,
    uint8_t*             shadowCounts,
    uint32_t**           shadowStack,
    uint32_t*            shadowLen,
    uint32_t*            shadowCap) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
        size_t                    nameLen = strlen(sym->localName);
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
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    int32_t              nodeId,
    uint8_t*             shadowCounts,
    uint32_t**           shadowStack,
    uint32_t*            shadowLen,
    uint32_t*            shadowCap,
    HOPTextRewrite**     rewrites,
    uint32_t*            rewriteLen,
    uint32_t*            rewriteCap);

static int CollectBlockImportRewritesNode(
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    int32_t              blockNodeId,
    uint8_t*             shadowCounts,
    uint32_t**           shadowStack,
    uint32_t*            shadowLen,
    uint32_t*            shadowCap,
    HOPTextRewrite**     rewrites,
    uint32_t*            rewriteLen,
    uint32_t*            rewriteCap) {
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

static int32_t VarLikeInitNode(const HOPParsedFile* file, int32_t varLikeNodeId) {
    int32_t firstChild = ASTFirstChild(&file->ast, varLikeNodeId);
    int32_t afterNames;
    if (firstChild < 0) {
        return -1;
    }
    if (file->ast.nodes[firstChild].kind == HOPAst_NAME_LIST) {
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

uint32_t AstListCount(const HOPAst* ast, int32_t listNode) {
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

int32_t AstListItemAt(const HOPAst* ast, int32_t listNode, uint32_t index) {
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
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    int32_t              nodeId,
    uint8_t*             shadowCounts,
    uint32_t**           shadowStack,
    uint32_t*            shadowLen,
    uint32_t*            shadowCap,
    HOPTextRewrite**     rewrites,
    uint32_t*            rewriteLen,
    uint32_t*            rewriteCap) {
    const HOPAstNode* n;
    int32_t           child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];
    switch (n->kind) {
        case HOPAst_BLOCK:
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
        case HOPAst_VAR:
        case HOPAst_CONST: {
            int32_t initNode = VarLikeInitNode(file, nodeId);
            int32_t firstChild = ASTFirstChild(&file->ast, nodeId);
            if (firstChild >= 0 && file->ast.nodes[firstChild].kind == HOPAst_NAME_LIST) {
                uint32_t i;
                uint32_t nameCount = AstListCount(&file->ast, firstChild);
                if (initNode >= 0) {
                    if (file->ast.nodes[initNode].kind == HOPAst_EXPR_LIST) {
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
                    int32_t           nameNode = AstListItemAt(&file->ast, firstChild, i);
                    const HOPAstNode* name = nameNode >= 0 ? &file->ast.nodes[nameNode] : NULL;
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
        case HOPAst_IF: {
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
        case HOPAst_FOR: {
            int32_t           parts[4];
            uint32_t          partCount = 0;
            uint32_t          mark = *shadowLen;
            const HOPAstNode* forNode = &file->ast.nodes[nodeId];
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0 && partCount < 4u) {
                parts[partCount++] = child;
                child = ASTNextSibling(&file->ast, child);
            }
            if ((forNode->flags & HOPAstFlag_FOR_IN) != 0) {
                int      hasKey = (forNode->flags & HOPAstFlag_FOR_IN_HAS_KEY) != 0;
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
                    const HOPAstNode* key = &file->ast.nodes[keyNode];
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
                if (valueNode >= 0 && (forNode->flags & HOPAstFlag_FOR_IN_VALUE_DISCARD) == 0) {
                    const HOPAstNode* value = &file->ast.nodes[valueNode];
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
                if (bodyNode >= 0 && file->ast.nodes[bodyNode].kind == HOPAst_BLOCK) {
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
                if (partCount >= 2u && file->ast.nodes[parts[0]].kind == HOPAst_VAR) {
                    int32_t initNode = VarLikeInitNode(file, parts[0]);
                    int32_t firstChild = ASTFirstChild(&file->ast, parts[0]);
                    if (firstChild >= 0 && file->ast.nodes[firstChild].kind == HOPAst_NAME_LIST) {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(&file->ast, firstChild);
                        if (initNode >= 0) {
                            if (file->ast.nodes[initNode].kind == HOPAst_EXPR_LIST) {
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
                            int32_t           nameNode = AstListItemAt(&file->ast, firstChild, i);
                            const HOPAstNode* name =
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
                } else if (partCount >= 2u && file->ast.nodes[parts[0]].kind != HOPAst_BLOCK) {
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
                    if (file->ast.nodes[parts[idx]].kind != HOPAst_BLOCK
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
                if (file->ast.nodes[parts[last]].kind == HOPAst_BLOCK) {
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
        case HOPAst_SWITCH: {
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const HOPAstNode* c = &file->ast.nodes[child];
                if (c->kind == HOPAst_CASE) {
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
                } else if (c->kind == HOPAst_DEFAULT) {
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
        case HOPAst_RETURN:
        case HOPAst_ASSERT:
        case HOPAst_DEL:
        case HOPAst_EXPR_STMT: {
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
        case HOPAst_DEFER: {
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
    const HOPTextRewrite* ra = (const HOPTextRewrite*)a;
    const HOPTextRewrite* rb = (const HOPTextRewrite*)b;
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
    HOPTextRewrite* _Nullable rewrites,
    uint32_t rewriteLen,
    char**   outText) {
    HOPStringBuilder b = { 0 };
    uint32_t         i;
    uint32_t         copyPos = 0;
    *outText = NULL;

    if (rewriteLen == 0) {
        *outText = HOPCDupSlice(text, 0, textLen);
        return *outText == NULL ? -1 : 0;
    }
    qsort(rewrites, rewriteLen, sizeof(HOPTextRewrite), CompareTextRewrite);

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
    const HOPPackage*    pkg,
    const HOPParsedFile* file,
    int32_t              nodeId,
    const char*          text,
    char**               outText) {
    const HOPAstNode* n;
    HOPTextRewrite*   rewrites = NULL;
    uint32_t          rewriteLen = 0;
    uint32_t          rewriteCap = 0;
    uint8_t*          shadowCounts = NULL;
    uint32_t*         shadowStack = NULL;
    uint32_t          shadowLen = 0;
    uint32_t          shadowCap = 0;
    uint32_t          mark = 0;
    uint32_t          baseStart;
    int32_t           child;
    int               rc = -1;

    *outText = NULL;
    if (pkg->importSymbolLen == 0) {
        *outText = HOPCDupCStr(text);
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
        case HOPAst_FN:
            mark = shadowLen;
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const HOPAstNode* ch = &file->ast.nodes[child];
                if (ch->kind == HOPAst_PARAM) {
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
                } else if (ch->kind == HOPAst_BLOCK) {
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
        case HOPAst_CONST: {
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
        case HOPAst_ENUM: {
            child = ASTFirstChild(&file->ast, nodeId);
            while (child >= 0) {
                const HOPAstNode* c = &file->ast.nodes[child];
                if (c->kind == HOPAst_FIELD) {
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
    const HOPImportRef* _Nullable imports,
    uint32_t importLen,
    const HOPIdentMap* _Nullable maps,
    uint32_t mapLen,
    char**   outText) {
    void*            arenaMem = NULL;
    size_t           arenaCap;
    uint64_t         arenaCap64;
    HOPArena         arena;
    HOPTokenStream   stream;
    HOPDiag          diag = { 0 };
    HOPStringBuilder b = { 0 };
    uint32_t         i;
    uint32_t         copyPos = 0;

    *outText = NULL;
    if ((importLen > 0 && imports == NULL) || (mapLen > 0 && maps == NULL)) {
        return ErrorSimple("internal error: missing rewrite mappings");
    }
    arenaCap64 = (uint64_t)(srcLen + 16u) * (uint64_t)sizeof(HOPToken) + 4096u;
    if (arenaCap64 > (uint64_t)SIZE_MAX) {
        return ErrorSimple("arena too large");
    }
    arenaCap = (size_t)arenaCap64;
    arenaMem = malloc(arenaCap);
    if (arenaMem == NULL) {
        return ErrorSimple("out of memory");
    }

    HOPArenaInit(&arena, arenaMem, (uint32_t)arenaCap);
    if (HOPLex(&arena, (HOPStrView){ src, srcLen }, &stream, &diag) != 0) {
        free(arenaMem);
        return ErrorSimple("rewrite lex failed");
    }

    for (i = 0; i < stream.len; i++) {
        const HOPToken* t = &stream.v[i];
        if (t->kind == HOPTok_EOF) {
            break;
        }

        if (importLen > 0 && i + 2u < stream.len && t->kind == HOPTok_IDENT
            && stream.v[i + 1u].kind == HOPTok_DOT && stream.v[i + 2u].kind == HOPTok_IDENT)
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
        if (t->kind == HOPTok_IDENT && mapLen > 0) {
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
    HOPStringBuilder b = { 0 };
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
    const HOPPackage* sourcePkg, const HOPSymbolDecl* pubDecl, const char* alias, char** outText) {
    HOPIdentMap* maps = NULL;
    uint32_t     i;
    int          rc = -1;
    *outText = NULL;
    if (sourcePkg->pubDeclLen == 0) {
        return ErrorSimple("internal error: empty public declaration set");
    }
    maps = (HOPIdentMap*)calloc(sourcePkg->pubDeclLen, sizeof(HOPIdentMap));
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
    HOPAst           ast = { 0 };
    void*            arenaMem = NULL;
    int32_t          fnNode = -1;
    int32_t          child;
    int32_t          returnTypeNode = -1;
    int32_t          contextTypeNode = -1;
    HOPStringBuilder shape = { 0 };
    HOPStringBuilder wrapper = { 0 };
    HOPStringBuilder callArgs = { 0 };
    uint32_t         paramIndex = 0;

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
    if (fnNode < 0 || (uint32_t)fnNode >= ast.len || ast.nodes[fnNode].kind != HOPAst_FN) {
        free(arenaMem);
        return ErrorSimple("internal error: expected function declaration in rewritten import");
    }

    if (SBAppendCStr(&shape, "ctx:") != 0) {
        free(arenaMem);
        goto oom;
    }
    child = ASTFirstChild(&ast, fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &ast.nodes[child];
        if (n->kind == HOPAst_PARAM) {
            /* handled later */
        } else if (n->kind == HOPAst_CONTEXT_CLAUSE) {
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
        const HOPAstNode* n = &ast.nodes[child];
        if (n->kind == HOPAst_PARAM) {
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

static int AppendAliasedPubDecls(
    HOPStringBuilder* b,
    const HOPPackage* sourcePkg,
    const char*       alias,
    const HOPImportRef* _Nullable imports,
    uint32_t importLen,
    int      includePrivateDecls,
    int      forceFunctionDeclsOnly) {
    HOPIdentMap* maps = NULL;
    uint32_t     mapLen = 0;
    uint32_t     declLen = includePrivateDecls ? sourcePkg->declLen : 0u;
    uint32_t     j;
    int          rc = -1;

    if (declLen == 0 && sourcePkg->pubDeclLen == 0) {
        return 0;
    }
    mapLen = declLen + sourcePkg->pubDeclLen;
    maps = (HOPIdentMap*)calloc(mapLen, sizeof(HOPIdentMap));
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
        char*       declText = sourcePkg->pubDecls[j].declText;
        char*       declTextCopy = NULL;
        const char* rewriteSource = declText;
        uint32_t    rewriteSourceLen;
        char*       rewritten = NULL;
        if (forceFunctionDeclsOnly && sourcePkg->pubDecls[j].kind == HOPAst_FN) {
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
        if (rewritten == NULL || SBAppendCStr(b, rewritten) != 0 || SBAppendCStr(b, "\n") != 0) {
            free(rewritten);
            goto done;
        }
        free(rewritten);
    }
    for (j = 0; j < declLen; j++) {
        const HOPSymbolDecl* decl = &sourcePkg->decls[j];
        const HOPParsedFile* file = &sourcePkg->files[decl->fileIndex];
        char*                namedRewritten = NULL;
        char*                rewritten = NULL;
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
        if (rewritten == NULL || SBAppendCStr(b, rewritten) != 0 || SBAppendCStr(b, "\n") != 0) {
            free(namedRewritten);
            free(rewritten);
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
    const HOPPackage* pkg;
    const char*       alias;
} HOPEmittedImportSurface;

static int EnsureEmittedImportSurfaceCap(
    HOPEmittedImportSurface** outArr, uint32_t* outCap, uint32_t needLen) {
    if (needLen <= *outCap) {
        return 0;
    }
    {
        uint32_t                 nextCap = *outCap == 0 ? 8u : *outCap;
        HOPEmittedImportSurface* p;
        while (nextCap < needLen) {
            if (nextCap > UINT32_MAX / 2u) {
                return -1;
            }
            nextCap *= 2u;
        }
        p = (HOPEmittedImportSurface*)realloc(*outArr, (size_t)nextCap * sizeof(**outArr));
        if (p == NULL) {
            return -1;
        }
        *outArr = p;
        *outCap = nextCap;
    }
    return 0;
}

static int HasEmittedImportSurface(
    const HOPEmittedImportSurface* _Nullable arr,
    uint32_t len,
    const HOPPackage* _Nullable pkg,
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

static int PackageNeedsPrivateDeclSurface(const HOPPackage* pkg) {
    uint32_t i;
    if (pkg->name != NULL && StrEq(pkg->name, "builtin")) {
        return 0;
    }
    for (i = 0; i < pkg->pubDeclLen; i++) {
        if (pkg->pubDecls[i].kind == HOPAst_FN) {
            return 1;
        }
    }
    return 0;
}

static int AppendImportedPackageSurface(
    HOPStringBuilder*         b,
    const HOPPackageLoader*   loader,
    const HOPPackage*         dep,
    const char*               alias,
    int                       includePrivateImportDecls,
    HOPEmittedImportSurface** emitted,
    uint32_t*                 emittedLen,
    uint32_t*                 emittedCap) {
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
        const HOPPackage* subDep = dep->imports[j].target;
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
                   IsSelectedPlatformImportPath(loader, dep->imports[j].path))
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
            loader != NULL && dep == loader->selectedPlatformPkg)
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

static int AppendImportFunctionWrappers(HOPStringBuilder* b, const HOPPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importSymbolLen; i++) {
        const HOPImportSymbolRef* sym = &pkg->importSymbols[i];
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
    HOPPackageLoader* loader,
    const HOPPackage* pkg,
    int               includePrivateImportDecls,
    char**            outSource,
    uint32_t*         outLen,
    HOPCombinedSourceMap* _Nullable sourceMap,
    uint32_t* _Nullable outOwnDeclStartOffset) {
    HOPStringBuilder         b = { 0 };
    uint32_t                 i;
    HOPEmittedImportSurface* emitted = NULL;
    uint32_t                 emittedLen = 0;
    uint32_t                 emittedCap = 0;
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
        const HOPPackage* dep = pkg->imports[i].target;
        if (AppendImportedPackageSurface(
                &b,
                loader,
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
        const HOPDeclText*   decl = &pkg->declTexts[i];
        const HOPParsedFile* file = &pkg->files[decl->fileIndex];
        char*                namedRewritten = NULL;
        char*                rewritten = NULL;
        uint32_t             combinedStart;
        uint32_t             combinedEnd;
        uint32_t             sourceStart = decl->sourceStart;
        uint32_t             sourceEnd = decl->sourceEnd;
        uint32_t             rewrittenLen;
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

static int CheckLoadedPackage(
    HOPPackageLoader* loader, HOPPackage* pkg, int suppressUnusedWarnings) {
    char*                source = NULL;
    uint32_t             sourceLen = 0;
    int                  lineColDiag = 0;
    const char*          checkPath = pkg->dirPath;
    const char*          checkSource = NULL;
    uint32_t             checkSourceLen = 0;
    HOPCombinedSourceMap sourceMap = { 0 };
    int                  useSingleFileRemap = (pkg->fileLen == 1 && pkg->importLen > 0);
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
        uint64_t            tcArenaCap64;
        uint32_t            tcArenaCap;
        HOPTypeCheckCtx*    tcCtx;
        HOPDiag             tcDiag = { 0 };
        HOPTypeCheckOptions tcOptions = { 0 };
        tcArenaCap64 = (uint64_t)pkg->files[0].sourceLen * 256u + 1024u * 1024u;
        if (tcArenaCap64 > UINT32_MAX) {
            return ErrorSimple("typecheck arena too large");
        }
        tcArenaCap = (uint32_t)tcArenaCap64;
        pkg->files[0].typecheckArenaMem = malloc(tcArenaCap);
        if (pkg->files[0].typecheckArenaMem == NULL) {
            return ErrorSimple("out of memory");
        }
        HOPArenaInit(&pkg->files[0].typecheckArena, pkg->files[0].typecheckArenaMem, tcArenaCap);
        HOPArenaSetAllocator(
            &pkg->files[0].typecheckArena, NULL, CodegenArenaGrow, CodegenArenaFree);
        tcCtx = (HOPTypeCheckCtx*)HOPArenaAlloc(
            &pkg->files[0].typecheckArena,
            sizeof(HOPTypeCheckCtx),
            (uint32_t)_Alignof(HOPTypeCheckCtx));
        if (tcCtx == NULL) {
            return ErrorSimple("out of memory");
        }
        if (HOPTCBuildCheckedContext(
                &pkg->files[0].typecheckArena,
                &pkg->files[0].ast,
                (HOPStrView){ pkg->files[0].source, pkg->files[0].sourceLen },
                &tcOptions,
                &tcDiag,
                tcCtx)
            != 0)
        {
            HOPArenaDispose(&pkg->files[0].typecheckArena);
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

static void FreePackage(HOPPackage* pkg) {
    uint32_t i;
    free(pkg->dirPath);
    free(pkg->name);
    for (i = 0; i < pkg->fileLen; i++) {
        free(pkg->files[i].path);
        free(pkg->files[i].source);
        free(pkg->files[i].arenaMem);
        if (pkg->files[i].typecheckArenaMem != NULL) {
            HOPArenaDispose(&pkg->files[i].typecheckArena);
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

void FreeLoader(HOPPackageLoader* loader) {
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
    HOPPackageLoader* outLoader,
    HOPPackage**      outEntryPkg) {
    char*            canonical = CanonicalizePath(entryPath);
    struct stat      st;
    char*            pkgDir = NULL;
    char*            rootDir;
    HOPPackageLoader loader;
    HOPPackage*      entryPkg = NULL;
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
    loader.platformTarget = HOPCDupCStr(
        (platformTarget != NULL && platformTarget[0] != '\0')
            ? platformTarget
            : HOP_DEFAULT_PLATFORM_TARGET);
    loader.archTarget = HOPCDupCStr(HOP_DEFAULT_ARCH_TARGET);
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
    int               testingBuild,
    HOPPackageLoader* outLoader,
    HOPPackage**      outEntryPkg) {
    char*            canonical = CanonicalizePath(entryPath);
    struct stat      st;
    char*            pkgDir = NULL;
    char*            rootDir;
    HOPPackageLoader loader;
    HOPPackage*      entryPkg = NULL;
    uint32_t         i;
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
    loader.platformTarget = HOPCDupCStr(
        (platformTarget != NULL && platformTarget[0] != '\0')
            ? platformTarget
            : HOP_DEFAULT_PLATFORM_TARGET);
    loader.archTarget = HOPCDupCStr(
        (archTarget != NULL && archTarget[0] != '\0') ? archTarget : HOP_DEFAULT_ARCH_TARGET);
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

int ValidateEntryMainSignature(const HOPPackage* _Nullable entryPkg) {
    uint32_t fileIndex;
    int      hasMainDefinition = 0;

    if (entryPkg == NULL) {
        return ErrorSimple("internal error: missing entry package");
    }

    for (fileIndex = 0; fileIndex < entryPkg->fileLen; fileIndex++) {
        const HOPParsedFile* file = &entryPkg->files[fileIndex];
        const HOPAst*        ast = &file->ast;
        int32_t              nodeId = ASTFirstChild(ast, ast->root);

        while (nodeId >= 0) {
            const HOPAstNode* n = &ast->nodes[nodeId];
            if (n->kind == HOPAst_FN && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "main"))
            {
                int32_t child = ASTFirstChild(ast, nodeId);
                int     paramCount = 0;
                int     hasReturnType = 0;
                int     hasBody = 0;

                while (child >= 0) {
                    const HOPAstNode* ch = &ast->nodes[child];
                    if (ch->kind == HOPAst_PARAM) {
                        paramCount++;
                    } else if (IsFnReturnTypeNodeKind(ch->kind) && ch->flags == 1) {
                        hasReturnType = 1;
                    } else if (ch->kind == HOPAst_BLOCK) {
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
    HOPPackageLoader loader;
    HOPPackage*      entryPkg = NULL;
    if (LoadAndCheckPackage(entryPath, platformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        return -1;
    }
    (void)entryPkg;
    FreeLoader(&loader);
    return 0;
}

HOP_API_END
