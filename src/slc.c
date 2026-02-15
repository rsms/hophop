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

#include "libsl-impl.h"
#include "slc_codegen.h"

SL_API_BEGIN

typedef struct {
    char*    path;
    char*    source;
    uint32_t sourceLen;
    void*    arenaMem;
    SLAST    ast;
} SLParsedFile;

struct SLPackage;

typedef struct {
    char*             alias;
    char*             path;
    struct SLPackage* target;
    uint32_t          fileIndex;
    uint32_t          start;
    uint32_t          end;
} SLImportRef;

typedef struct {
    SLASTKind kind;
    char*     name;
    char*     declText;
    int       hasBody;
    uint32_t  fileIndex;
    int32_t   nodeId;
} SLSymbolDecl;

typedef struct {
    char*    text;
    uint32_t fileIndex;
} SLDeclText;

typedef struct SLPackage {
    char* dirPath;
    char* name;
    int   loadState; /* 0=new, 1=loading, 2=loaded */
    int   checked;

    SLParsedFile* files;
    uint32_t      fileLen;
    uint32_t      fileCap;

    SLImportRef* imports;
    uint32_t     importLen;
    uint32_t     importCap;

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

static int ASTFirstChild(const SLAST* ast, int32_t nodeId) {
    if (nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    return ast->nodes[nodeId].firstChild;
}

static int ASTNextSibling(const SLAST* ast, int32_t nodeId) {
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

static int Errorf(const char* file, uint32_t start, uint32_t end, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%u:%u: error: ", DisplayPath(file), start, end);
    va_start(ap, fmt);
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
    SLDiag        diag;
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
        fprintf(
            stderr,
            "%s:%u:%u: error: %s\n",
            filename,
            diag.start,
            diag.end,
            SLDiagMessage(diag.code));
        free(arenaMem);
        return -1;
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
    SLAST    ast;
    SLDiag   diag;
    SLWriter writer;

    arenaCap64 = (uint64_t)(sourceLen + 64u) * (uint64_t)sizeof(SLASTNode) + 32768u;
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
    if (SLParse(&arena, (SLStrView){ source, sourceLen }, &ast, &diag) != 0) {
        fprintf(
            stderr,
            "%s:%u:%u: error: %s\n",
            filename,
            diag.start,
            diag.end,
            SLDiagMessage(diag.code));
        free(arenaMem);
        return -1;
    }

    writer.ctx = NULL;
    writer.write = StdoutWrite;
    if (SLASTDump(&ast, (SLStrView){ source, sourceLen }, &writer, &diag) != 0) {
        fprintf(
            stderr,
            "%s:%u:%u: error: %s\n",
            filename,
            diag.start,
            diag.end,
            SLDiagMessage(diag.code));
        free(arenaMem);
        return -1;
    }

    free(arenaMem);
    return 0;
}

static int ParseSource(
    const char* filename,
    const char* source,
    uint32_t    sourceLen,
    SLAST*      outAst,
    void**      outArenaMem,
    SLArena* _Nullable outArena) {
    void*    arenaMem;
    uint64_t arenaCap64;
    size_t   arenaCap;
    SLArena  arena;
    SLDiag   diag;

    *outArenaMem = NULL;
    if (outArena != NULL) {
        memset(outArena, 0, sizeof(*outArena));
    }
    outAst->nodes = NULL;
    outAst->len = 0;
    outAst->root = -1;

    arenaCap64 = (uint64_t)(sourceLen + 128u) * (uint64_t)sizeof(SLASTNode) + 65536u;
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
    if (SLParse(&arena, (SLStrView){ source, sourceLen }, outAst, &diag) != 0) {
        fprintf(
            stderr,
            "%s:%u:%u: error: %s\n",
            DisplayPath(filename),
            diag.start,
            diag.end,
            SLDiagMessage(diag.code));
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

static int CheckSource(const char* filename, const char* source, uint32_t sourceLen) {
    void*   arenaMem;
    SLArena arena;
    SLAST   ast;
    SLDiag  diag;

    if (ParseSource(filename, source, sourceLen, &ast, &arenaMem, &arena) != 0) {
        return -1;
    }

    if (SLTypeCheck(&arena, &ast, (SLStrView){ source, sourceLen }, &diag) != 0) {
        fprintf(
            stderr,
            "%s:%u:%u: error: %s\n",
            DisplayPath(filename),
            diag.start,
            diag.end,
            SLDiagMessage(diag.code));
        free(arenaMem);
        return -1;
    }

    free(arenaMem);
    return 0;
}

static int IsDeclKind(SLASTKind kind) {
    return kind == SLAST_FN || kind == SLAST_STRUCT || kind == SLAST_UNION || kind == SLAST_ENUM
        || kind == SLAST_CONST;
}

static int FnNodeHasBody(const SLAST* ast, int32_t nodeId) {
    int32_t child = ASTFirstChild(ast, nodeId);
    while (child >= 0) {
        if (ast->nodes[child].kind == SLAST_BLOCK) {
            return 1;
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

static int DecodeHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static char* _Nullable DecodeStringLiteral(const char* src, uint32_t start, uint32_t end) {
    SLStringBuilder b = { 0 };
    uint32_t        i;
    if (end <= start + 1u || src[start] != '"' || src[end - 1u] != '"') {
        return NULL;
    }
    i = start + 1u;
    while (i < end - 1u) {
        char c = src[i++];
        if (c != '\\') {
            if (SBAppend(&b, &c, 1) != 0) {
                free(b.v);
                return NULL;
            }
            continue;
        }
        if (i >= end - 1u) {
            free(b.v);
            return NULL;
        }
        c = src[i++];
        switch (c) {
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case '\\': break;
            case '"':  break;
            case 'x':  {
                int  hi;
                int  lo;
                char v;
                if (i + 1u >= end - 1u) {
                    free(b.v);
                    return NULL;
                }
                hi = DecodeHexDigit(src[i++]);
                lo = DecodeHexDigit(src[i++]);
                if (hi < 0 || lo < 0) {
                    free(b.v);
                    return NULL;
                }
                v = (char)((hi << 4) | lo);
                if (SBAppend(&b, &v, 1) != 0) {
                    free(b.v);
                    return NULL;
                }
                continue;
            }
            default: break;
        }
        if (SBAppend(&b, &c, 1) != 0) {
            free(b.v);
            return NULL;
        }
    }
    return SBFinish(&b, NULL);
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

static int AddPackageFile(
    SLPackage*  pkg,
    const char* filePath,
    char*       source,
    uint32_t    sourceLen,
    SLAST       ast,
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

static int AddDeclText(SLPackage* pkg, char* text, uint32_t fileIndex) {
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
    return 0;
}

static int AddSymbolDecl(
    SLSymbolDecl** arr,
    uint32_t*      len,
    uint32_t*      cap,
    SLASTKind      kind,
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
    char*      importPath,
    uint32_t   fileIndex,
    uint32_t   start,
    uint32_t   end) {
    SLImportRef* imp;
    uint32_t     i;
    for (i = 0; i < pkg->importLen; i++) {
        if (StrEq(pkg->imports[i].alias, alias)) {
            if (StrEq(pkg->imports[i].path, importPath)) {
                free(alias);
                free(importPath);
                return 0;
            }
            free(alias);
            free(importPath);
            return -1;
        }
    }
    if (EnsureCap((void**)&pkg->imports, &pkg->importCap, pkg->importLen + 1u, sizeof(SLImportRef))
        != 0)
    {
        return -1;
    }
    imp = &pkg->imports[pkg->importLen++];
    imp->alias = alias;
    imp->path = importPath;
    imp->target = NULL;
    imp->fileIndex = fileIndex;
    imp->start = start;
    imp->end = end;
    return 0;
}

static int AddDeclFromNode(
    SLPackage* pkg, const SLParsedFile* file, uint32_t fileIndex, int32_t nodeId, int isPub) {
    const SLASTNode* n = &file->ast.nodes[nodeId];
    char*            name;
    char*            declText;

    if (!IsDeclKind(n->kind)) {
        return 0;
    }
    if (n->dataEnd <= n->dataStart || n->end < n->start || n->end > file->sourceLen) {
        return Errorf(file->path, n->start, n->end, "invalid declaration");
    }
    name = DupSlice(file->source, n->dataStart, n->dataEnd);
    declText = DupSlice(file->source, n->start, n->end);
    if (name == NULL || declText == NULL) {
        free(name);
        free(declText);
        return ErrorSimple("out of memory");
    }

    if (isPub) {
        uint32_t i;
        for (i = 0; i < pkg->pubDeclLen; i++) {
            if (pkg->pubDecls[i].kind == n->kind && StrEq(pkg->pubDecls[i].name, name)) {
                free(name);
                free(declText);
                return Errorf(file->path, n->dataStart, n->dataEnd, "duplicate public symbol");
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

static int ProcessParsedFile(SLPackage* pkg, uint32_t fileIndex) {
    const SLParsedFile* file = &pkg->files[fileIndex];
    const SLAST*        ast = &file->ast;
    int32_t             child = ASTFirstChild(ast, ast->root);

    while (child >= 0) {
        const SLASTNode* n = &ast->nodes[child];
        if (n->kind == SLAST_IMPORT) {
            int32_t aliasNode = ASTFirstChild(ast, child);
            char*   importPath = DecodeStringLiteral(file->source, n->dataStart, n->dataEnd);
            char*   alias = NULL;
            if (importPath == NULL) {
                return Errorf(file->path, n->dataStart, n->dataEnd, "invalid import path literal");
            }
            if (aliasNode >= 0) {
                const SLASTNode* a = &ast->nodes[aliasNode];
                alias = DupSlice(file->source, a->dataStart, a->dataEnd);
            } else {
                alias = DefaultImportAlias(importPath);
            }
            if (alias == NULL) {
                int rc = Errorf(
                    file->path,
                    n->start,
                    n->end,
                    "import path requires explicit alias (e.g. import name \"%s\")",
                    importPath);
                free(importPath);
                return rc;
            }
            if (AddImportRef(pkg, alias, importPath, fileIndex, n->start, n->end) != 0) {
                return Errorf(file->path, n->start, n->end, "duplicate import alias");
            }
        } else {
            char* declText = DupSlice(file->source, n->start, n->end);
            if (declText == NULL) {
                return ErrorSimple("out of memory");
            }
            if (AddDeclText(pkg, declText, fileIndex) != 0) {
                free(declText);
                return ErrorSimple("out of memory");
            }
            if (n->kind == SLAST_PUB) {
                int32_t pubChild = ASTFirstChild(ast, child);
                while (pubChild >= 0) {
                    if (AddDeclFromNode(pkg, file, fileIndex, pubChild, 1) != 0) {
                        return -1;
                    }
                    pubChild = ASTNextSibling(ast, pubChild);
                }
            } else {
                if (AddDeclFromNode(pkg, file, fileIndex, child, 0) != 0) {
                    return -1;
                }
            }
        }
        child = ASTNextSibling(ast, child);
    }
    return 0;
}

static int IsBuiltinTypeName(const char* src, uint32_t start, uint32_t end) {
    return SliceEqCStr(src, start, end, "void") || SliceEqCStr(src, start, end, "bool")
        || SliceEqCStr(src, start, end, "str") || SliceEqCStr(src, start, end, "u8")
        || SliceEqCStr(src, start, end, "u16") || SliceEqCStr(src, start, end, "u32")
        || SliceEqCStr(src, start, end, "u64") || SliceEqCStr(src, start, end, "i8")
        || SliceEqCStr(src, start, end, "i16") || SliceEqCStr(src, start, end, "i32")
        || SliceEqCStr(src, start, end, "i64") || SliceEqCStr(src, start, end, "usize")
        || SliceEqCStr(src, start, end, "isize") || SliceEqCStr(src, start, end, "f32")
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
        if (!(pkg->pubDecls[i].kind == SLAST_STRUCT || pkg->pubDecls[i].kind == SLAST_UNION
              || pkg->pubDecls[i].kind == SLAST_ENUM))
        {
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

static int ValidatePubTypeNode(
    const SLPackage* pkg, const SLParsedFile* file, int32_t typeNodeId, const char* contextMsg) {
    const SLASTNode* n;
    if (typeNodeId < 0 || (uint32_t)typeNodeId >= file->ast.len) {
        return ErrorSimple("invalid type node");
    }
    n = &file->ast.nodes[typeNodeId];
    switch (n->kind) {
        case SLAST_TYPE_NAME: {
            uint32_t i;
            for (i = n->dataStart; i < n->dataEnd; i++) {
                if (file->source[i] == '.') {
                    return Errorf(
                        file->path,
                        n->start,
                        n->end,
                        "public API %s must not reference imported types",
                        contextMsg);
                }
            }
            if (IsBuiltinTypeName(file->source, n->dataStart, n->dataEnd)) {
                return 0;
            }
            if (PackageHasExportedTypeSlice(pkg, file->source, n->dataStart, n->dataEnd)) {
                return 0;
            }
            return Errorf(
                file->path,
                n->start,
                n->end,
                "public API %s references non-exported type",
                contextMsg);
        }
        case SLAST_TYPE_PTR:
        case SLAST_TYPE_ARRAY: {
            int32_t child = ASTFirstChild(&file->ast, typeNodeId);
            return ValidatePubTypeNode(pkg, file, child, contextMsg);
        }
        default: return Errorf(file->path, n->start, n->end, "invalid type in public API");
    }
}

static int ValidatePubClosure(const SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->pubDeclLen; i++) {
        const SLSymbolDecl* pubDecl = &pkg->pubDecls[i];
        const SLParsedFile* file = &pkg->files[pubDecl->fileIndex];
        int32_t             child = ASTFirstChild(&file->ast, pubDecl->nodeId);
        if (pubDecl->kind == SLAST_FN) {
            while (child >= 0) {
                const SLASTNode* n = &file->ast.nodes[child];
                if (n->kind == SLAST_PARAM) {
                    int32_t typeNode = ASTFirstChild(&file->ast, child);
                    if (ValidatePubTypeNode(pkg, file, typeNode, "function parameter") != 0) {
                        return -1;
                    }
                } else if (
                    (n->kind == SLAST_TYPE_NAME || n->kind == SLAST_TYPE_PTR
                     || n->kind == SLAST_TYPE_ARRAY)
                    && n->flags == 1)
                {
                    if (ValidatePubTypeNode(pkg, file, child, "function return type") != 0) {
                        return -1;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
        } else if (pubDecl->kind == SLAST_STRUCT || pubDecl->kind == SLAST_UNION) {
            while (child >= 0) {
                const SLASTNode* n = &file->ast.nodes[child];
                if (n->kind == SLAST_FIELD) {
                    int32_t typeNode = ASTFirstChild(&file->ast, child);
                    if (ValidatePubTypeNode(pkg, file, typeNode, "field type") != 0) {
                        return -1;
                    }
                }
                child = ASTNextSibling(&file->ast, child);
            }
        } else if (pubDecl->kind == SLAST_ENUM) {
            if (child >= 0) {
                const SLASTNode* n = &file->ast.nodes[child];
                if (n->kind == SLAST_TYPE_NAME || n->kind == SLAST_TYPE_PTR
                    || n->kind == SLAST_TYPE_ARRAY)
                {
                    if (ValidatePubTypeNode(pkg, file, child, "enum base type") != 0) {
                        return -1;
                    }
                }
            }
        } else if (pubDecl->kind == SLAST_CONST) {
            if (child >= 0) {
                if (ValidatePubTypeNode(pkg, file, child, "constant type") != 0) {
                    return -1;
                }
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
        int                 found = 0;
        if (pubDecl->kind != SLAST_FN) {
            continue;
        }
        for (j = 0; j < pkg->declLen; j++) {
            const SLSymbolDecl* decl = &pkg->decls[j];
            if (decl->kind == SLAST_FN && StrEq(decl->name, pubDecl->name) && decl->hasBody) {
                found = 1;
                break;
            }
        }
        if (!found) {
            const SLParsedFile* file = &pkg->files[pubDecl->fileIndex];
            const SLASTNode*    n = &file->ast.nodes[pubDecl->nodeId];
            return Errorf(
                file->path,
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
        if (strlen(pkg->imports[i].alias) == (size_t)(aliasEnd - aliasStart)
            && memcmp(pkg->imports[i].alias, src + aliasStart, (size_t)(aliasEnd - aliasStart))
                   == 0)
        {
            return &pkg->imports[i];
        }
    }
    return NULL;
}

static int ValidateSelectorsNode(const SLPackage* pkg, const SLParsedFile* file, int32_t nodeId) {
    const SLASTNode* n;
    int32_t          child;
    if (nodeId < 0 || (uint32_t)nodeId >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[nodeId];

    if (n->kind == SLAST_TYPE_NAME) {
        uint32_t i;
        for (i = n->dataStart; i < n->dataEnd; i++) {
            if (file->source[i] == '.') {
                const SLImportRef* imp = FindImportByAliasSlice(pkg, file->source, n->dataStart, i);
                if (imp == NULL) {
                    return Errorf(file->path, n->start, n->end, "unknown import alias");
                }
                if (!PackageHasExportSlice(imp->target, file->source, i + 1u, n->dataEnd)) {
                    return Errorf(file->path, n->start, n->end, "unknown imported symbol");
                }
                break;
            }
        }
    } else if (n->kind == SLAST_FIELD_EXPR) {
        int32_t recvNode = ASTFirstChild(&file->ast, nodeId);
        if (recvNode >= 0 && file->ast.nodes[recvNode].kind == SLAST_IDENT) {
            const SLASTNode*   recv = &file->ast.nodes[recvNode];
            const SLImportRef* imp = FindImportByAliasSlice(
                pkg, file->source, recv->dataStart, recv->dataEnd);
            if (imp != NULL) {
                if (!PackageHasExportSlice(imp->target, file->source, n->dataStart, n->dataEnd)) {
                    return Errorf(file->path, n->start, n->end, "unknown imported symbol");
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

static int LoadPackageRecursive(SLPackageLoader* loader, const char* dirPath, SLPackage** outPkg);

static int ResolvePackageImportsAndSelectors(SLPackageLoader* loader, SLPackage* pkg) {
    uint32_t i;
    for (i = 0; i < pkg->importLen; i++) {
        char* resolvedDir;
        if (pkg->imports[i].path[0] == '/') {
            resolvedDir = DupCStr(pkg->imports[i].path);
        } else {
            resolvedDir = JoinPath(loader->rootDir, pkg->imports[i].path);
        }
        if (resolvedDir == NULL) {
            return ErrorSimple("out of memory");
        }
        if (LoadPackageRecursive(loader, resolvedDir, &pkg->imports[i].target) != 0) {
            const SLParsedFile* file = &pkg->files[pkg->imports[i].fileIndex];
            free(resolvedDir);
            return Errorf(
                file->path,
                pkg->imports[i].start,
                pkg->imports[i].end,
                "failed to resolve import %s",
                pkg->imports[i].path);
        }
        free(resolvedDir);
    }

    if (ValidatePackageSelectors(pkg) != 0) {
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
        SLAST    ast;
        void*    arenaMem = NULL;
        if (ReadFile(filePaths[i], &source, &sourceLen) != 0) {
            return -1;
        }
        if (ParseSource(filePaths[i], source, sourceLen, &ast, &arenaMem, NULL) != 0) {
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
    if (ValidatePubClosure(pkg) != 0) {
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
    SLAST      ast;
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
    if (ParseSource(filePath, source, sourceLen, &ast, &arenaMem, NULL) != 0) {
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
    if (ValidatePubClosure(pkg) != 0) {
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
    SLDiag          diag;
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
                if (strlen(imports[j].alias) == (size_t)(t->end - t->start)
                    && memcmp(imports[j].alias, src + t->start, (size_t)(t->end - t->start)) == 0
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

static int BuildCombinedPackageSource(const SLPackage* pkg, char** outSource, uint32_t* outLen) {
    SLStringBuilder b = { 0 };
    uint32_t        i;
    *outSource = NULL;
    *outLen = 0;

    for (i = 0; i < pkg->importLen; i++) {
        const SLPackage* dep = pkg->imports[i].target;
        SLIdentMap*      maps;
        uint32_t         j;
        if (dep == NULL) {
            free(b.v);
            return ErrorSimple("internal error: unresolved import");
        }
        maps =
            dep->pubDeclLen == 0 ? NULL : (SLIdentMap*)malloc(sizeof(SLIdentMap) * dep->pubDeclLen);
        if (dep->pubDeclLen > 0 && maps == NULL) {
            free(b.v);
            return ErrorSimple("out of memory");
        }
        for (j = 0; j < dep->pubDeclLen; j++) {
            maps[j].name = dep->pubDecls[j].name;
            maps[j].replacement = NULL;
            if (BuildPrefixedName(
                    pkg->imports[i].alias, dep->pubDecls[j].name, (char**)&maps[j].replacement)
                != 0)
            {
                uint32_t k;
                for (k = 0; k < j; k++) {
                    free((void*)maps[k].replacement);
                }
                free(maps);
                free(b.v);
                return ErrorSimple("out of memory");
            }
        }

        for (j = 0; j < dep->pubDeclLen; j++) {
            char* rewritten = NULL;
            if (RewriteText(
                    dep->pubDecls[j].declText,
                    (uint32_t)strlen(dep->pubDecls[j].declText),
                    NULL,
                    0,
                    maps,
                    dep->pubDeclLen,
                    &rewritten)
                != 0)
            {
                uint32_t k;
                for (k = 0; k < dep->pubDeclLen; k++) {
                    free((void*)maps[k].replacement);
                }
                free(maps);
                free(b.v);
                return -1;
            }
            if (SBAppendCStr(&b, rewritten) != 0 || SBAppendCStr(&b, "\n") != 0) {
                uint32_t k;
                free(rewritten);
                for (k = 0; k < dep->pubDeclLen; k++) {
                    free((void*)maps[k].replacement);
                }
                free(maps);
                free(b.v);
                return ErrorSimple("out of memory");
            }
            free(rewritten);
        }
        if (dep->pubDeclLen > 0) {
            if (SBAppendCStr(&b, "\n") != 0) {
                uint32_t k;
                for (k = 0; k < dep->pubDeclLen; k++) {
                    free((void*)maps[k].replacement);
                }
                free(maps);
                free(b.v);
                return ErrorSimple("out of memory");
            }
        }
        for (j = 0; j < dep->pubDeclLen; j++) {
            free((void*)maps[j].replacement);
        }
        free(maps);
    }

    for (i = 0; i < pkg->declTextLen; i++) {
        char* rewritten = NULL;
        if (RewriteText(
                pkg->declTexts[i].text,
                (uint32_t)strlen(pkg->declTexts[i].text),
                pkg->imports,
                pkg->importLen,
                NULL,
                0,
                &rewritten)
            != 0)
        {
            free(b.v);
            return -1;
        }
        if (SBAppendCStr(&b, rewritten) != 0 || SBAppendCStr(&b, "\n") != 0) {
            free(rewritten);
            free(b.v);
            return ErrorSimple("out of memory");
        }
        free(rewritten);
    }

    *outSource = SBFinish(&b, outLen);
    if (*outSource == NULL) {
        return ErrorSimple("out of memory");
    }
    return 0;
}

static int CheckLoadedPackage(SLPackage* pkg) {
    char*    source = NULL;
    uint32_t sourceLen = 0;
    if (pkg->checked) {
        return 0;
    }
    if (BuildCombinedPackageSource(pkg, &source, &sourceLen) != 0) {
        return -1;
    }
    if (CheckSource(pkg->dirPath, source, sourceLen) != 0) {
        free(source);
        return -1;
    }
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
        free(pkg->imports[i].path);
    }
    free(pkg->imports);
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
    memset(loader, 0, sizeof(*loader));
}

static int LoadAndCheckPackage(
    const char* entryPath, SLPackageLoader* outLoader, SLPackage** outEntryPkg) {
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

    for (i = 0; i < loader.packageLen; i++) {
        if (CheckLoadedPackage(&loader.packages[i]) != 0) {
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

static int CheckPackageDir(const char* entryPath) {
    SLPackageLoader loader;
    SLPackage*      entryPkg;
    if (LoadAndCheckPackage(entryPath, &loader, &entryPkg) != 0) {
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
        "usage: %s [lex|ast|check] <file.sl>\n"
        "       %s checkpkg <package-dir|file.sl>\n"
        "       %s genpkg[:backend] <package-dir|file.sl> [out.h]\n"
        "       %s compile <package-dir|file.sl> -o <output>\n"
        "       %s run <package-dir|file.sl>\n",
        argv0,
        argv0,
        argv0,
        argv0,
        argv0);
}

static int GeneratePackage(
    const char* entryPath, const char* backendName, const char* _Nullable outFilename) {
    SLPackageLoader         loader;
    SLPackage*              entryPkg;
    char*                   source = NULL;
    uint32_t                sourceLen = 0;
    char*                   outHeader = NULL;
    SLDiag                  diag;
    SLCodegenUnit           unit;
    const SLCodegenBackend* backend;

    if (LoadAndCheckPackage(entryPath, &loader, &entryPkg) != 0) {
        return -1;
    }

    if (BuildCombinedPackageSource(entryPkg, &source, &sourceLen) != 0) {
        FreeLoader(&loader);
        return -1;
    }

    if (!IsValidIdentifier(entryPkg->name)) {
        free(source);
        FreeLoader(&loader);
        return ErrorSimple(
            "entry package name \"%s\" is not a valid identifier (inferred from path)",
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

    SLDiagClear(&diag);
    if (backend->emit(backend, &unit, &codegenOptions, &outHeader, &diag) != 0) {
        if (diag.code != SLDiag_NONE) {
            fprintf(
                stderr,
                "%s:%u:%u: error: %s\n",
                DisplayPath(entryPkg->dirPath),
                diag.start,
                diag.end,
                SLDiagMessage(diag.code));
        } else {
            fprintf(stderr, "error: codegen failed\n");
        }
        free(source);
        FreeLoader(&loader);
        return -1;
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

static int CreateTempDir(char* outPath, size_t outPathCap, const char* tag) {
    const char* tmpBase = getenv("TMPDIR");
    int         n;
    char*       dir;
    if (tmpBase == NULL || tmpBase[0] == '\0') {
        tmpBase = "/tmp";
    }
    n = snprintf(outPath, outPathCap, "%s/slc-%s.XXXXXX", tmpBase, tag);
    if (n <= 0 || (size_t)n >= outPathCap) {
        return -1;
    }
    dir = mkdtemp(outPath);
    return dir != NULL ? 0 : -1;
}

static int CompileProgram(const char* entryPath, const char* outExe) {
    SLPackageLoader         loader = { 0 };
    int                     loaderReady = 0;
    SLPackage*              entryPkg;
    char*                   source = NULL;
    uint32_t                sourceLen = 0;
    char*                   outHeader = NULL;
    SLDiag                  diag;
    SLCodegenUnit           unit;
    const SLCodegenBackend* backend;
    SLCodegenOptions        codegenOptions = { 0 };
    char                    tmpDir[PATH_MAX];
    char*                   headerPath = NULL;
    char*                   sourcePath = NULL;
    SLStringBuilder         cBuilder = { 0 };
    char*                   cSource = NULL;
    const char*             ccArgv[8];
    int                     rc = -1;

    tmpDir[0] = '\0';

    if (LoadAndCheckPackage(entryPath, &loader, &entryPkg) != 0) {
        goto end;
    }
    loaderReady = 1;
    if (BuildCombinedPackageSource(entryPkg, &source, &sourceLen) != 0) {
        goto end;
    }
    if (!IsValidIdentifier(entryPkg->name)) {
        ErrorSimple(
            "entry package name \"%s\" is not a valid identifier (inferred from path)",
            entryPkg->name);
        goto end;
    }

    backend = SLCodegenFindBackend("c");
    if (backend == NULL) {
        ErrorSimple("unknown backend: c");
        goto end;
    }

    unit.packageName = entryPkg->name;
    unit.source = source;
    unit.sourceLen = sourceLen;
    codegenOptions.implMacro = "SLC_IMPL";
    codegenOptions.headerGuard = "SLC_PROGRAM_H";

    SLDiagClear(&diag);
    if (backend->emit(backend, &unit, &codegenOptions, &outHeader, &diag) != 0) {
        if (diag.code != SLDiag_NONE) {
            ErrorSimple(
                "%s:%u:%u: %s",
                DisplayPath(entryPkg->dirPath),
                diag.start,
                diag.end,
                SLDiagMessage(diag.code));
        } else {
            ErrorSimple("codegen failed");
        }
        goto end;
    }

    if (CreateTempDir(tmpDir, sizeof(tmpDir), "compile") != 0) {
        ErrorSimple("failed to create temporary directory");
        goto end;
    }

    headerPath = JoinPath(tmpDir, "program.h");
    sourcePath = JoinPath(tmpDir, "program.c");
    if (headerPath == NULL || sourcePath == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }
    if (WriteOutput(headerPath, outHeader, (uint32_t)strlen(outHeader)) != 0) {
        ErrorSimple("failed to write generated header");
        goto end;
    }

    if (SBAppendCStr(&cBuilder, "#define SLC_IMPL\n#include \"") != 0
        || SBAppendCStr(&cBuilder, headerPath) != 0 || SBAppendCStr(&cBuilder, "\"\n\n") != 0
        || SBAppendCStr(&cBuilder, "int main(void) { return (int)") != 0
        || SBAppendCStr(&cBuilder, entryPkg->name) != 0
        || SBAppendCStr(&cBuilder, "__main(); }\n") != 0)
    {
        ErrorSimple("out of memory");
        goto end;
    }
    cSource = SBFinish(&cBuilder, NULL);
    if (cSource == NULL) {
        ErrorSimple("out of memory");
        goto end;
    }
    if (WriteOutput(sourcePath, cSource, (uint32_t)strlen(cSource)) != 0) {
        ErrorSimple("failed to write generated C source");
        goto end;
    }

    ccArgv[0] = "cc";
    ccArgv[1] = "-std=c11";
    ccArgv[2] = "-g";
    ccArgv[3] = "-o";
    ccArgv[4] = outExe;
    ccArgv[5] = sourcePath;
    ccArgv[6] = NULL;

    if (RunCommand(ccArgv) != 0) {
        ErrorSimple("C compilation failed");
        goto end;
    }

    rc = 0;

end:
    if (headerPath != NULL) {
        unlink(headerPath);
    }
    if (sourcePath != NULL) {
        unlink(sourcePath);
    }
    if (tmpDir[0] != '\0') {
        rmdir(tmpDir);
    }
    free(headerPath);
    free(sourcePath);
    free(outHeader);
    free(cSource);
    free(cBuilder.v);
    free(source);
    if (loaderReady) {
        FreeLoader(&loader);
    }
    return rc;
}

static int RunProgram(const char* entryPath) {
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

    if (CompileProgram(entryPath, exeTemplate) != 0) {
        unlink(exeTemplate);
        return -1;
    }

    execv(exeTemplate, execArgv);
    unlink(exeTemplate);
    return ErrorSimple("failed to execute compiled program");
}

int main(int argc, char* argv[]) {
    const char* mode = "lex";
    const char* filename = NULL;
    const char* outFilename = NULL;
    char        backendName[32];
    int         genpkgMode;
    char*       source;
    uint32_t    sourceLen;

    if (argc >= 2 && StrEq(argv[1], "compile")) {
        if (argc != 5 || !StrEq(argv[3], "-o")) {
            PrintUsage(argv[0]);
            return 2;
        }
        return CompileProgram(argv[2], argv[4]) == 0 ? 0 : 1;
    }
    if (argc >= 2 && StrEq(argv[1], "run")) {
        if (argc != 3) {
            PrintUsage(argv[0]);
            return 2;
        }
        return RunProgram(argv[2]) == 0 ? 0 : 1;
    }

    if (argc == 2) {
        filename = argv[1];
    } else if (argc == 3) {
        mode = argv[1];
        filename = argv[2];
    } else if (argc == 4) {
        mode = argv[1];
        filename = argv[2];
        outFilename = argv[3];
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
        return GeneratePackage(filename, backendName, outFilename) == 0 ? 0 : 1;
    }

    if (mode[0] == 'c' && mode[1] == 'h' && mode[2] == 'e' && mode[3] == 'c' && mode[4] == 'k'
        && mode[5] == 'p' && mode[6] == 'k' && mode[7] == 'g' && mode[8] == '\0')
    {
        if (outFilename != NULL) {
            fprintf(stderr, "unexpected output argument for mode checkpkg\n");
            return 2;
        }
        return CheckPackageDir(filename) == 0 ? 0 : 1;
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
