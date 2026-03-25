#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctfe.h"
#include "ctfe_exec.h"
#include "evaluator.h"
#include "libsl-impl.h"
#include "mir_exec.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"

SL_API_BEGIN

typedef struct {
    char* _Nullable path;
    char* _Nullable source;
    uint32_t       sourceLen;
    void* _Nullable arenaMem;
    SLAst    ast;
} SLParsedFile;

struct SLPackage;

typedef struct {
    char* alias;
    char* _Nullable bindName;
    char*                 path;
    struct SLPackage* _Nullable target;
    uint32_t              fileIndex;
    uint32_t              start;
    uint32_t              end;
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
    int        loadState;
    int        checked;
    SLFeatures features;

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

#define SL_EVAL_CALL_MAX_DEPTH 128u

enum {
    SL_EVAL_MIR_HOST_INVALID = SLMirHostTarget_INVALID,
    SL_EVAL_MIR_HOST_PRINT = SLMirHostTarget_PRINT,
    SL_EVAL_MIR_HOST_PLATFORM_EXIT = SLMirHostTarget_PLATFORM_EXIT,
    SL_EVAL_MIR_HOST_FREE = SLMirHostTarget_FREE,
    SL_EVAL_MIR_HOST_CONCAT = SLMirHostTarget_CONCAT,
    SL_EVAL_MIR_HOST_COPY = SLMirHostTarget_COPY,
};

enum {
    SL_EVAL_MIR_ITER_KIND_INVALID = 0,
    SL_EVAL_MIR_ITER_KIND_SEQUENCE = 1,
    SL_EVAL_MIR_ITER_KIND_PROTOCOL = 2,
    SL_EVAL_MIR_ITER_MAGIC = 0x534c4954u,
};

static int SliceEqCStr(const char* s, uint32_t start, uint32_t end, const char* cstr);

static int SLEvalNameEqLiteralOrPkgBuiltin(
    const char* _Nullable src,
    uint32_t start,
    uint32_t end,
    const char* _Nonnull lit,
    const char* _Nonnull pkgPrefix) {
    size_t litLen = 0;
    size_t pkgLen = 0;
    size_t i;
    if (src == NULL || lit == NULL || pkgPrefix == NULL || end < start) {
        return 0;
    }
    while (lit[litLen] != '\0') {
        litLen++;
    }
    while (pkgPrefix[pkgLen] != '\0') {
        pkgLen++;
    }
    if ((size_t)(end - start) == litLen && memcmp(src + start, lit, litLen) == 0) {
        return 1;
    }
    if ((size_t)(end - start) != pkgLen + 2u + litLen) {
        return 0;
    }
    for (i = 0; i < pkgLen; i++) {
        if (src[start + i] != pkgPrefix[i]) {
            return 0;
        }
    }
    if (src[start + pkgLen] != '_' || src[start + pkgLen + 1u] != '_') {
        return 0;
    }
    return memcmp(src + start + pkgLen + 2u, lit, litLen) == 0;
}

static int SLEvalNameIsCompilerDiagBuiltin(
    const char* _Nullable src, uint32_t start, uint32_t end) {
    return SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "error", "compiler")
        || SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "error_at", "compiler")
        || SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "warn", "compiler")
        || SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "warn_at", "compiler");
}

static int SLEvalNameIsLazyTypeBuiltin(const char* _Nullable src, uint32_t start, uint32_t end) {
    return SliceEqCStr(src, start, end, "typeof")
        || SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "kind", "reflect")
        || SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "base", "reflect")
        || SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "is_alias", "reflect")
        || SLEvalNameEqLiteralOrPkgBuiltin(src, start, end, "type_name", "reflect")
        || SliceEqCStr(src, start, end, "ptr") || SliceEqCStr(src, start, end, "slice")
        || SliceEqCStr(src, start, end, "array");
}

typedef struct {
    uint32_t    magic;
    uint32_t    sourceNode;
    uint32_t    index;
    int32_t     iteratorFn;
    uint16_t    flags;
    uint8_t     kind;
    uint8_t     _reserved[1];
    SLCTFEValue sourceValue;
    SLCTFEValue iteratorValue;
} SLEvalMirIteratorState;

int ErrorDiagf(
    const char* file,
    const char* _Nullable source,
    uint32_t   start,
    uint32_t   end,
    SLDiagCode code,
    ...);
int ErrorSimple(const char* fmt, ...);
int PrintSLDiag(
    const char* filename, const char* _Nullable source, const SLDiag* diag, int includeHint);
void* _Nullable CodegenArenaGrow(void* _Nullable ctx, uint32_t minSize, uint32_t* _Nonnull outSize);
void CodegenArenaFree(void* _Nullable ctx, void* _Nullable block, uint32_t blockSize);
void FreeLoader(SLPackageLoader* loader);
int  LoadAndCheckPackage(
    const char* entryPath,
    const char* _Nullable platformTarget,
    SLPackageLoader* outLoader,
    SLPackage**      outEntryPkg);
int        ValidateEntryMainSignature(const SLPackage* entryPkg);
int        FindPackageIndex(const SLPackageLoader* loader, const SLPackage* pkg);
static int SLEvalStringValueFromArrayBytes(
    SLArena* arena, const SLCTFEValue* inValue, int32_t targetTypeCode, SLCTFEValue* outValue);

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

static int SLEvalTypeNodeIsAnytype(const SLParsedFile* file, int32_t typeNode);

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

static int IsFnReturnTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE || kind == SLAst_TYPE_ANON_STRUCT
        || kind == SLAst_TYPE_ANON_UNION;
}

static int ParseSliceU64(const char* s, uint32_t start, uint32_t end, uint64_t* outValue) {
    uint64_t v = 0;
    uint32_t i;
    if (outValue != NULL) {
        *outValue = 0;
    }
    if (s == NULL || outValue == NULL || end <= start) {
        return 0;
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch < '0' || ch > '9') {
            return 0;
        }
        if (v > (UINT64_MAX - (uint64_t)(ch - '0')) / 10u) {
            return 0;
        }
        v = v * 10u + (uint64_t)(ch - '0');
    }
    *outValue = v;
    return 1;
}

enum {
    SLEvalTypeCode_INVALID = 0,
    SLEvalTypeCode_BOOL = 1,
    SLEvalTypeCode_U8,
    SLEvalTypeCode_U16,
    SLEvalTypeCode_U32,
    SLEvalTypeCode_U64,
    SLEvalTypeCode_UINT,
    SLEvalTypeCode_I8,
    SLEvalTypeCode_I16,
    SLEvalTypeCode_I32,
    SLEvalTypeCode_I64,
    SLEvalTypeCode_INT,
    SLEvalTypeCode_F32,
    SLEvalTypeCode_F64,
    SLEvalTypeCode_TYPE,
    SLEvalTypeCode_STR_REF,
    SLEvalTypeCode_STR_PTR,
    SLEvalTypeCode_ANYTYPE,
};

typedef struct SLEvalProgram SLEvalProgram;
typedef struct SLEvalContext SLEvalContext;

static int     ASTNextSibling(const SLAst* ast, int32_t nodeId);
static void    SLEvalValueSetRuntimeTypeCode(SLCTFEValue* value, int32_t typeCode);
static int     SLEvalValueGetRuntimeTypeCode(const SLCTFEValue* value, int32_t* outTypeCode);
static int32_t SLEvalFindTopConstBySlice(
    const SLEvalProgram* p, const SLParsedFile* file, uint32_t nameStart, uint32_t nameEnd);
static int32_t SLEvalFindTopConstBySliceInPackage(
    const SLEvalProgram* p,
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd);
static int SLEvalEvalTopConst(
    SLEvalProgram* p, uint32_t topConstIndex, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalInvokeFunction(
    SLEvalProgram*       p,
    int32_t              fnIndex,
    const SLCTFEValue* _Nullable args,
    uint32_t             argCount,
    const SLEvalContext* callContext,
    SLCTFEValue*         outValue,
    int*                 outDidReturn);
static int SLEvalMirLookupLocalTypeNode(
    SLEvalProgram*       p,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const SLParsedFile** outFile,
    int32_t*             outTypeNode);
static int SLEvalMirLookupLocalValue(
    SLEvalProgram* p, uint32_t nameStart, uint32_t nameEnd, SLCTFEValue* outValue);
static int SLEvalFindVisibleLocalTypeNodeByName(
    const SLParsedFile* file,
    uint32_t            beforePos,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    int32_t*            outTypeNode);

static int SLEvalBuiltinTypeSize(
    const char* source, uint32_t nameStart, uint32_t nameEnd, uint64_t* outSize) {
    if (outSize != NULL) {
        *outSize = 0;
    }
    if (source == NULL || outSize == NULL) {
        return 0;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "bool")
        || SliceEqCStr(source, nameStart, nameEnd, "u8")
        || SliceEqCStr(source, nameStart, nameEnd, "i8"))
    {
        *outSize = 1u;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u16")
        || SliceEqCStr(source, nameStart, nameEnd, "i16"))
    {
        *outSize = 2u;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u32")
        || SliceEqCStr(source, nameStart, nameEnd, "i32")
        || SliceEqCStr(source, nameStart, nameEnd, "f32"))
    {
        *outSize = 4u;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u64")
        || SliceEqCStr(source, nameStart, nameEnd, "i64")
        || SliceEqCStr(source, nameStart, nameEnd, "f64")
        || SliceEqCStr(source, nameStart, nameEnd, "type"))
    {
        *outSize = 8u;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "usize")
        || SliceEqCStr(source, nameStart, nameEnd, "isize")
        || SliceEqCStr(source, nameStart, nameEnd, "uint")
        || SliceEqCStr(source, nameStart, nameEnd, "int"))
    {
        *outSize = (uint64_t)sizeof(void*);
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "str")) {
        *outSize = (uint64_t)(sizeof(void*) * 2u);
        return 1;
    }
    return 0;
}

static int SLEvalBuiltinTypeCode(
    const char* source, uint32_t nameStart, uint32_t nameEnd, int32_t* outTypeCode) {
    if (outTypeCode != NULL) {
        *outTypeCode = SLEvalTypeCode_INVALID;
    }
    if (source == NULL || outTypeCode == NULL) {
        return 0;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "bool")) {
        *outTypeCode = SLEvalTypeCode_BOOL;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u8")) {
        *outTypeCode = SLEvalTypeCode_U8;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u16")) {
        *outTypeCode = SLEvalTypeCode_U16;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u32")) {
        *outTypeCode = SLEvalTypeCode_U32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u64")) {
        *outTypeCode = SLEvalTypeCode_U64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "uint")) {
        *outTypeCode = SLEvalTypeCode_UINT;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i8")) {
        *outTypeCode = SLEvalTypeCode_I8;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i16")) {
        *outTypeCode = SLEvalTypeCode_I16;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i32")) {
        *outTypeCode = SLEvalTypeCode_I32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i64")) {
        *outTypeCode = SLEvalTypeCode_I64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "int")) {
        *outTypeCode = SLEvalTypeCode_INT;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "f32")) {
        *outTypeCode = SLEvalTypeCode_F32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "f64")) {
        *outTypeCode = SLEvalTypeCode_F64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "type")) {
        *outTypeCode = SLEvalTypeCode_TYPE;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "anytype")) {
        *outTypeCode = SLEvalTypeCode_ANYTYPE;
        return 1;
    }
    return 0;
}

static int SLEvalTypeCodeFromTypeNode(
    const SLParsedFile* file, int32_t typeNode, int32_t* outTypeCode) {
    const SLAstNode* n;
    if (outTypeCode != NULL) {
        *outTypeCode = SLEvalTypeCode_INVALID;
    }
    if (file == NULL || outTypeCode == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == SLAst_TYPE_NAME) {
        return SLEvalBuiltinTypeCode(file->source, n->dataStart, n->dataEnd, outTypeCode);
    }
    if ((n->kind == SLAst_TYPE_REF || n->kind == SLAst_TYPE_PTR) && n->firstChild >= 0
        && (uint32_t)n->firstChild < file->ast.len
        && file->ast.nodes[n->firstChild].kind == SLAst_TYPE_NAME
        && SliceEqCStr(
            file->source,
            file->ast.nodes[n->firstChild].dataStart,
            file->ast.nodes[n->firstChild].dataEnd,
            "str"))
    {
        *outTypeCode = n->kind == SLAst_TYPE_REF ? SLEvalTypeCode_STR_REF : SLEvalTypeCode_STR_PTR;
        return 1;
    }
    return 0;
}

static int SLEvalIsU8ElementTypeNode(const SLParsedFile* file, int32_t typeNode) {
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    return file->ast.nodes[typeNode].kind == SLAst_TYPE_NAME
        && SliceEqCStr(
               file->source,
               file->ast.nodes[typeNode].dataStart,
               file->ast.nodes[typeNode].dataEnd,
               "u8");
}

static int SLEvalStringViewTypeCodeFromTypeNode(
    const SLParsedFile* file, int32_t typeNode, int32_t* outTypeCode) {
    const SLAstNode* n;
    int32_t          childNode;
    if (outTypeCode != NULL) {
        *outTypeCode = SLEvalTypeCode_INVALID;
    }
    if (file == NULL || outTypeCode == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    if (SLEvalTypeCodeFromTypeNode(file, typeNode, outTypeCode)) {
        return *outTypeCode == SLEvalTypeCode_STR_REF || *outTypeCode == SLEvalTypeCode_STR_PTR;
    }
    n = &file->ast.nodes[typeNode];
    if ((n->kind != SLAst_TYPE_PTR && n->kind != SLAst_TYPE_REF) || n->firstChild < 0
        || (uint32_t)n->firstChild >= file->ast.len)
    {
        return 0;
    }
    childNode = n->firstChild;
    if ((file->ast.nodes[childNode].kind == SLAst_TYPE_VARRAY
         || file->ast.nodes[childNode].kind == SLAst_TYPE_ARRAY)
        && file->ast.nodes[childNode].firstChild >= 0
        && SLEvalIsU8ElementTypeNode(file, file->ast.nodes[childNode].firstChild))
    {
        *outTypeCode = n->kind == SLAst_TYPE_REF ? SLEvalTypeCode_STR_REF : SLEvalTypeCode_STR_PTR;
        return 1;
    }
    return 0;
}

static int SLEvalCloneStringValue(
    SLArena* arena, const SLCTFEValue* inValue, SLCTFEValue* outValue, int32_t typeCode) {
    uint8_t* copyBytes = NULL;
    if (arena == NULL || inValue == NULL || outValue == NULL || inValue->kind != SLCTFEValue_STRING)
    {
        return 0;
    }
    *outValue = *inValue;
    if (inValue->s.len > 0) {
        copyBytes = (uint8_t*)SLArenaAlloc(arena, inValue->s.len, (uint32_t)_Alignof(uint8_t));
        if (copyBytes == NULL) {
            return -1;
        }
        memcpy(copyBytes, inValue->s.bytes, inValue->s.len);
        outValue->s.bytes = copyBytes;
    }
    SLEvalValueSetRuntimeTypeCode(outValue, typeCode);
    return 1;
}

static int SLEvalAdaptStringValueForType(
    SLArena*            arena,
    const SLParsedFile* typeFile,
    int32_t             typeNode,
    const SLCTFEValue*  inValue,
    SLCTFEValue*        outValue) {
    int32_t targetTypeCode = SLEvalTypeCode_INVALID;
    int32_t currentTypeCode = SLEvalTypeCode_INVALID;
    if (arena == NULL || typeFile == NULL || inValue == NULL || outValue == NULL
        || inValue->kind != SLCTFEValue_STRING)
    {
        return 0;
    }
    if (!SLEvalStringViewTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)) {
        return 0;
    }
    if (targetTypeCode == SLEvalTypeCode_STR_PTR) {
        if (SLEvalValueGetRuntimeTypeCode(inValue, &currentTypeCode)
            && currentTypeCode == SLEvalTypeCode_STR_PTR)
        {
            *outValue = *inValue;
            return 1;
        }
        return SLEvalCloneStringValue(arena, inValue, outValue, targetTypeCode);
    }
    *outValue = *inValue;
    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
    return 1;
}

static int SLEvalTypeCodeFromValue(const SLCTFEValue* value, int32_t* outTypeCode) {
    if (outTypeCode != NULL) {
        *outTypeCode = SLEvalTypeCode_INVALID;
    }
    if (value == NULL || outTypeCode == NULL) {
        return 0;
    }
    if (SLEvalValueGetRuntimeTypeCode(value, outTypeCode)) {
        return 1;
    }
    if (value->kind == SLCTFEValue_BOOL) {
        *outTypeCode = SLEvalTypeCode_BOOL;
        return 1;
    }
    if (value->kind == SLCTFEValue_INT) {
        *outTypeCode = SLEvalTypeCode_INT;
        return 1;
    }
    if (value->kind == SLCTFEValue_FLOAT) {
        *outTypeCode = SLEvalTypeCode_F64;
        return 1;
    }
    if (value->kind == SLCTFEValue_STRING) {
        *outTypeCode = SLEvalTypeCode_STR_REF;
        return 1;
    }
    if (value->kind == SLCTFEValue_TYPE) {
        *outTypeCode = SLEvalTypeCode_TYPE;
        return 1;
    }
    return 0;
}

static void SLEvalAnnotateValueTypeFromExpr(
    const SLParsedFile* file, const SLAst* ast, int32_t exprNode, SLCTFEValue* value) {
    int32_t          typeCode = SLEvalTypeCode_INVALID;
    const SLAstNode* n;
    if (file == NULL || ast == NULL || value == NULL || exprNode < 0
        || (uint32_t)exprNode >= ast->len)
    {
        return;
    }
    if (SLEvalValueGetRuntimeTypeCode(value, &typeCode)) {
        return;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == SLAst_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return;
        }
        n = &ast->nodes[exprNode];
    }
    if (n->kind == SLAst_CAST) {
        int32_t typeNode = ASTNextSibling(ast, n->firstChild);
        if (typeNode >= 0 && SLEvalTypeCodeFromTypeNode(file, typeNode, &typeCode)) {
            SLEvalValueSetRuntimeTypeCode(value, typeCode);
            return;
        }
    }
    if (n->kind == SLAst_STRING) {
        SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_STR_REF);
        return;
    }
    if (n->kind == SLAst_BOOL) {
        SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_BOOL);
        return;
    }
    if (n->kind == SLAst_FLOAT) {
        SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_F64);
        return;
    }
    if (n->kind == SLAst_INT) {
        SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_INT);
        return;
    }
}

static int SLEvalTypeNodeSize(
    const SLParsedFile* file, int32_t typeNode, uint64_t* outSize, uint32_t depth) {
    const SLAstNode* n;
    if (outSize != NULL) {
        *outSize = 0;
    }
    if (file == NULL || outSize == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len
        || depth > file->ast.len)
    {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    switch (n->kind) {
        case SLAst_TYPE_NAME:
            return SLEvalBuiltinTypeSize(file->source, n->dataStart, n->dataEnd, outSize);
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
        case SLAst_TYPE_FN:       *outSize = (uint64_t)sizeof(void*); return 1;
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE: *outSize = (uint64_t)(sizeof(void*) * 2u); return 1;
        case SLAst_TYPE_OPTIONAL: {
            int32_t child = file->ast.nodes[typeNode].firstChild;
            return child >= 0 ? SLEvalTypeNodeSize(file, child, outSize, depth + 1u) : 0;
        }
        case SLAst_TYPE_ARRAY: {
            int32_t  elemTypeNode = file->ast.nodes[typeNode].firstChild;
            uint64_t elemSize = 0;
            uint64_t count = 0;
            if (elemTypeNode < 0 || !SLEvalTypeNodeSize(file, elemTypeNode, &elemSize, depth + 1u)
                || !ParseSliceU64(file->source, n->dataStart, n->dataEnd, &count))
            {
                return 0;
            }
            if (count > 0 && elemSize > UINT64_MAX / count) {
                return 0;
            }
            *outSize = elemSize * count;
            return 1;
        }
        default: return 0;
    }
}

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
typedef struct {
    const SLPackage*    pkg;
    const SLParsedFile* file;
    int32_t             fnNode;
    int32_t             bodyNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint32_t            paramCount;
    uint8_t             hasReturnType;
    uint8_t             hasContextClause;
    uint8_t             isBuiltinPackageFn;
    uint8_t             isVariadic;
} SLEvalFunction;

enum {
    SLEvalTopConstState_UNSEEN = 0,
    SLEvalTopConstState_VISITING = 1,
    SLEvalTopConstState_READY = 2,
    SLEvalTopConstState_FAILED = 3,
};

typedef struct {
    const SLParsedFile* file;
    int32_t             nodeId;
    int32_t             initExprNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint8_t             state;
    uint8_t             _reserved[3];
    SLCTFEValue         value;
} SLEvalTopConst;

typedef struct {
    const SLParsedFile* file;
    int32_t             nodeId;
    int32_t             initExprNode;
    int32_t             declTypeNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint8_t             state;
    uint8_t             _reserved[3];
    SLCTFEValue         value;
} SLEvalTopVar;

typedef struct {
    uint32_t    nameStart;
    uint32_t    nameEnd;
    uint16_t    flags;
    uint16_t    _reserved;
    int32_t     typeNode;
    int32_t     defaultExprNode;
    SLCTFEValue value;
} SLEvalAggregateField;

typedef struct {
    const SLParsedFile*   file;
    int32_t               nodeId;
    SLEvalAggregateField* fields;
    uint32_t              fieldLen;
} SLEvalAggregate;

typedef struct {
    const SLParsedFile* file;
    int32_t             typeNode;
    int32_t             elemTypeNode;
    SLCTFEValue* _Nullable elems;
    uint32_t            len;
} SLEvalArray;

struct SLEvalContext {
    SLCTFEValue mem;
    SLCTFEValue tempMem;
    SLCTFEValue log;
};

typedef struct {
    const SLParsedFile* file;
    int32_t             enumNode;
    uint32_t            variantNameStart;
    uint32_t            variantNameEnd;
    uint32_t            tagIndex;
    SLEvalAggregate* _Nullable payload;
} SLEvalTaggedEnum;

typedef struct SLEvalReflectedType SLEvalReflectedType;
struct SLEvalReflectedType {
    uint8_t             kind;
    uint8_t             namedKind;
    uint16_t            _reserved;
    const SLParsedFile* file;
    int32_t             nodeId;
    uint32_t            arrayLen;
    SLCTFEValue         elemType;
};

#define SL_EVAL_PACKAGE_REF_TAG_FLAG  (UINT64_C(1) << 63)
#define SL_EVAL_NULL_FIXED_LEN_TAG    (UINT64_C(1) << 62)
#define SL_EVAL_FUNCTION_REF_TAG_FLAG (UINT64_C(1) << 61)
#define SL_EVAL_TAGGED_ENUM_TAG_FLAG  (UINT64_C(1) << 60)
#define SL_EVAL_SIMPLE_TYPE_TAG_FLAG  (UINT64_C(1) << 59)
#define SL_EVAL_REFLECT_TYPE_TAG_FLAG (UINT64_C(1) << 58)
#define SL_EVAL_RUNTIME_TYPE_MAGIC    0x534c4556u

enum {
    SLEvalReflectType_NAMED = 1,
    SLEvalReflectType_PTR = 2,
    SLEvalReflectType_SLICE = 3,
    SLEvalReflectType_ARRAY = 4,
};

enum {
    SLEvalTypeKind_PRIMITIVE = 1,
    SLEvalTypeKind_ALIAS = 2,
    SLEvalTypeKind_STRUCT = 3,
    SLEvalTypeKind_UNION = 4,
    SLEvalTypeKind_ENUM = 5,
    SLEvalTypeKind_POINTER = 6,
    SLEvalTypeKind_REFERENCE = 7,
    SLEvalTypeKind_SLICE = 8,
    SLEvalTypeKind_ARRAY = 9,
    SLEvalTypeKind_OPTIONAL = 10,
    SLEvalTypeKind_FUNCTION = 11,
};

struct SLEvalProgram {
    SLArena* _Nonnull arena;
    const SLPackageLoader* loader;
    const SLPackage*       entryPkg;
    const SLParsedFile*    currentFile;
    SLCTFEExecCtx*         currentExecCtx;
    struct SLEvalMirExecCtx* _Nullable currentMirExecCtx;
    SLEvalFunction*      funcs;
    uint32_t             funcLen;
    uint32_t             funcCap;
    SLEvalTopConst*      topConsts;
    uint32_t             topConstLen;
    uint32_t             topConstCap;
    SLEvalTopVar*        topVars;
    uint32_t             topVarLen;
    uint32_t             topVarCap;
    uint32_t             callDepth;
    uint32_t             callStack[SL_EVAL_CALL_MAX_DEPTH];
    SLEvalContext        rootContext;
    const SLEvalContext* _Nullable currentContext;
    int                            exitCalled;
    int                            exitCode;
};

typedef struct SLEvalMirExecCtx {
    SLEvalProgram*           p;
    uint32_t*                evalToMir;
    uint32_t                 evalToMirLen;
    uint32_t*                mirToEval;
    uint32_t                 mirToEvalLen;
    const SLParsedFile**     sourceFiles;
    uint32_t                 sourceFileCap;
    const SLParsedFile*      savedFiles[SL_EVAL_CALL_MAX_DEPTH];
    uint8_t                  pushedFrames[SL_EVAL_CALL_MAX_DEPTH];
    struct SLEvalMirExecCtx* savedMirExecCtxs[SL_EVAL_CALL_MAX_DEPTH];
    uint32_t                 savedFileLen;
    uint32_t                 rootMirFnIndex;
    const SLMirProgram*      mirProgram;
    const SLMirFunction*     mirFunction;
    const SLCTFEValue*       mirLocals;
    uint32_t                 mirLocalCount;
    const SLMirProgram*      savedMirPrograms[SL_EVAL_CALL_MAX_DEPTH];
    const SLMirFunction*     savedMirFunctions[SL_EVAL_CALL_MAX_DEPTH];
    const SLCTFEValue*       savedMirLocals[SL_EVAL_CALL_MAX_DEPTH];
    uint32_t                 savedMirLocalCounts[SL_EVAL_CALL_MAX_DEPTH];
    uint32_t                 mirFrameDepth;
} SLEvalMirExecCtx;

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

static void SLEvalValueSetInt(SLCTFEValue* value, int64_t n) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_INT;
    value->i64 = n;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void SLEvalValueSetRuntimeTypeCode(SLCTFEValue* value, int32_t typeCode) {
    if (value == NULL) {
        return;
    }
    value->span.fileLen = 0;
    value->span.startLine = (uint32_t)typeCode;
    value->span.startColumn = SL_EVAL_RUNTIME_TYPE_MAGIC;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static int SLEvalValueGetRuntimeTypeCode(const SLCTFEValue* value, int32_t* outTypeCode) {
    if (value == NULL || outTypeCode == NULL || value->kind == SLCTFEValue_SPAN
        || value->span.startColumn != SL_EVAL_RUNTIME_TYPE_MAGIC)
    {
        return 0;
    }
    *outTypeCode = (int32_t)value->span.startLine;
    return 1;
}

static void SLEvalValueSetSimpleTypeValue(SLCTFEValue* value, int32_t typeCode) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = SL_EVAL_SIMPLE_TYPE_TAG_FLAG | (uint64_t)(uint32_t)typeCode;
    value->s.bytes = NULL;
    value->s.len = 0;
    value->span.fileBytes = NULL;
    value->span.fileLen = 0;
    value->span.startLine = 0;
    value->span.startColumn = 0;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static int SLEvalValueGetSimpleTypeCode(const SLCTFEValue* value, int32_t* outTypeCode) {
    if (value == NULL || outTypeCode == NULL || value->kind != SLCTFEValue_TYPE
        || (value->typeTag & SL_EVAL_SIMPLE_TYPE_TAG_FLAG) == 0)
    {
        return 0;
    }
    *outTypeCode = (int32_t)(uint32_t)(value->typeTag & ~SL_EVAL_SIMPLE_TYPE_TAG_FLAG);
    return 1;
}

static void SLEvalAnnotateUntypedLiteralValue(SLCTFEValue* value) {
    int32_t typeCode = SLEvalTypeCode_INVALID;
    if (value == NULL || SLEvalValueGetRuntimeTypeCode(value, &typeCode)) {
        return;
    }
    switch (value->kind) {
        case SLCTFEValue_BOOL:  SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_BOOL); break;
        case SLCTFEValue_INT:   SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_INT); break;
        case SLCTFEValue_FLOAT: SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_F64); break;
        case SLCTFEValue_STRING:
            SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_STR_REF);
            break;
        default: break;
    }
}

static uint64_t SLEvalHashMix64(uint64_t x) {
    x ^= x >> 21;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static uint64_t SLEvalReflectTypeTagBody(const SLEvalReflectedType* rt) {
    uint64_t tag = 0;
    if (rt == NULL) {
        return 0;
    }
    tag = ((uint64_t)rt->kind << 52) ^ ((uint64_t)rt->namedKind << 44);
    if (rt->kind == SLEvalReflectType_NAMED) {
        tag ^= SLEvalHashMix64((uint64_t)(uintptr_t)rt->file);
        tag ^= SLEvalHashMix64((uint64_t)(uint32_t)(rt->nodeId + 1));
    } else {
        tag ^= SLEvalHashMix64(rt->elemType.typeTag);
        tag ^= SLEvalHashMix64((uint64_t)rt->arrayLen);
    }
    return tag
         & ~(SL_EVAL_PACKAGE_REF_TAG_FLAG | SL_EVAL_NULL_FIXED_LEN_TAG
             | SL_EVAL_FUNCTION_REF_TAG_FLAG | SL_EVAL_TAGGED_ENUM_TAG_FLAG
             | SL_EVAL_SIMPLE_TYPE_TAG_FLAG | SL_EVAL_REFLECT_TYPE_TAG_FLAG);
}

static void SLEvalValueSetReflectedTypeValue(SLCTFEValue* value, SLEvalReflectedType* rt) {
    if (value == NULL || rt == NULL) {
        return;
    }
    value->kind = SLCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = SL_EVAL_REFLECT_TYPE_TAG_FLAG | SLEvalReflectTypeTagBody(rt);
    value->s.bytes = (const uint8_t*)rt;
    value->s.len = 0;
    value->span.fileBytes = NULL;
    value->span.fileLen = 0;
    value->span.startLine = 0;
    value->span.startColumn = 0;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static SLEvalReflectedType* _Nullable SLEvalValueAsReflectedType(const SLCTFEValue* value) {
    if (value == NULL || value->kind != SLCTFEValue_TYPE
        || (value->typeTag & SL_EVAL_REFLECT_TYPE_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (SLEvalReflectedType*)value->s.bytes;
}

static void SLEvalValueSetPackageRef(SLCTFEValue* value, uint32_t pkgIndex) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = SL_EVAL_PACKAGE_REF_TAG_FLAG | (uint64_t)pkgIndex;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void SLEvalValueSetFunctionRef(SLCTFEValue* value, uint32_t fnIndex) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = SL_EVAL_FUNCTION_REF_TAG_FLAG | (uint64_t)fnIndex;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static int SLEvalValueIsPackageRef(const SLCTFEValue* value, uint32_t* outPkgIndex) {
    if (value == NULL || value->kind != SLCTFEValue_TYPE
        || (value->typeTag & SL_EVAL_PACKAGE_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outPkgIndex != NULL) {
        *outPkgIndex = (uint32_t)(value->typeTag & ~SL_EVAL_PACKAGE_REF_TAG_FLAG);
    }
    return 1;
}

static int SLEvalValueIsFunctionRef(const SLCTFEValue* value, uint32_t* _Nullable outFnIndex) {
    if (value == NULL || value->kind != SLCTFEValue_TYPE
        || (value->typeTag & SL_EVAL_FUNCTION_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outFnIndex != NULL) {
        *outFnIndex = (uint32_t)(value->typeTag & ~SL_EVAL_FUNCTION_REF_TAG_FLAG);
    }
    return 1;
}

static int SLEvalValueIsInvokableFunctionRef(const SLCTFEValue* value) {
    return SLEvalValueIsFunctionRef(value, NULL) || SLMirValueAsFunctionRef(value, NULL);
}

static void SLEvalValueSetTaggedEnum(
    SLEvalProgram*      p,
    SLCTFEValue*        value,
    const SLParsedFile* file,
    int32_t             enumNode,
    uint32_t            variantNameStart,
    uint32_t            variantNameEnd,
    uint32_t            tagIndex,
    SLEvalAggregate* _Nullable payload) {
    SLEvalTaggedEnum* tagged;
    if (p == NULL || value == NULL || file == NULL) {
        return;
    }
    tagged = (SLEvalTaggedEnum*)SLArenaAlloc(
        p->arena, sizeof(SLEvalTaggedEnum), (uint32_t)_Alignof(SLEvalTaggedEnum));
    if (tagged == NULL) {
        SLEvalValueSetNull(value);
        return;
    }
    memset(tagged, 0, sizeof(*tagged));
    tagged->file = file;
    tagged->enumNode = enumNode;
    tagged->variantNameStart = variantNameStart;
    tagged->variantNameEnd = variantNameEnd;
    tagged->tagIndex = tagIndex;
    tagged->payload = payload;
    value->kind = SLCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = SL_EVAL_TAGGED_ENUM_TAG_FLAG
                   | (((uint64_t)(uintptr_t)file) & ~SL_EVAL_TAGGED_ENUM_TAG_FLAG);
    value->typeTag ^= (uint64_t)(uint32_t)(enumNode + 1) << 3;
    value->typeTag ^= (uint64_t)tagIndex;
    value->s.bytes = (const uint8_t*)tagged;
    value->s.len = 0;
}

static SLEvalTaggedEnum* _Nullable SLEvalValueAsTaggedEnum(const SLCTFEValue* value) {
    if (value == NULL || value->kind != SLCTFEValue_TYPE
        || (value->typeTag & SL_EVAL_TAGGED_ENUM_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (SLEvalTaggedEnum*)value->s.bytes;
}

static uint64_t SLEvalMakeAliasTag(const SLParsedFile* file, int32_t nodeId) {
    uint64_t tag = 0;
    if (file != NULL) {
        tag = (uint64_t)(uintptr_t)file;
    }
    tag ^= (uint64_t)(uint32_t)(nodeId + 1) << 1;
    return tag & ~SL_EVAL_PACKAGE_REF_TAG_FLAG;
}

static uint64_t SLEvalMakeNullFixedLenTag(uint32_t len) {
    return SL_EVAL_NULL_FIXED_LEN_TAG | (uint64_t)len;
}

static int SLEvalValueGetNullFixedLen(const SLCTFEValue* value, uint32_t* outLen) {
    if (value == NULL || value->kind != SLCTFEValue_NULL
        || (value->typeTag & SL_EVAL_NULL_FIXED_LEN_TAG) == 0)
    {
        return 0;
    }
    if (outLen != NULL) {
        *outLen = (uint32_t)(value->typeTag & ~SL_EVAL_NULL_FIXED_LEN_TAG);
    }
    return 1;
}

static int SLEvalParseUintSlice(
    const char* source, uint32_t start, uint32_t end, uint32_t* _Nullable outValue) {
    uint64_t value = 0;
    uint32_t i;
    if (source == NULL || end <= start) {
        return 0;
    }
    for (i = start; i < end; i++) {
        unsigned char ch = (unsigned char)source[i];
        if (ch < (unsigned char)'0' || ch > (unsigned char)'9') {
            return 0;
        }
        value = value * 10u + (uint64_t)(ch - (unsigned char)'0');
        if (value > UINT32_MAX) {
            return 0;
        }
    }
    if (outValue != NULL) {
        *outValue = (uint32_t)value;
    }
    return 1;
}

static int SLEvalResolveNullCastTypeTag(
    const SLParsedFile* file, int32_t typeNode, uint64_t* _Nullable outTypeTag) {
    const SLAstNode* n;
    int32_t          childNode;
    uint32_t         fixedLen = 0;
    if (outTypeTag != NULL) {
        *outTypeTag = 0;
    }
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind != SLAst_TYPE_PTR && n->kind != SLAst_TYPE_REF && n->kind != SLAst_TYPE_MUTREF) {
        return 0;
    }
    childNode = n->firstChild;
    if (childNode >= 0 && (uint32_t)childNode < file->ast.len
        && file->ast.nodes[childNode].kind == SLAst_TYPE_ARRAY
        && SLEvalParseUintSlice(
            file->source,
            file->ast.nodes[childNode].dataStart,
            file->ast.nodes[childNode].dataEnd,
            &fixedLen))
    {
        if (outTypeTag != NULL) {
            *outTypeTag = SLEvalMakeNullFixedLenTag(fixedLen);
        }
    }
    return 1;
}

static const SLPackage* _Nullable SLEvalFindPackageByFile(
    const SLEvalProgram* p, const SLParsedFile* file) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL || file == NULL) {
        return NULL;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &p->loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (&pkg->files[fileIndex] == file) {
                return pkg;
            }
        }
    }
    return NULL;
}

static void SLEvalValueSetStringSlice(
    SLCTFEValue* value, const char* source, uint32_t start, uint32_t end) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_STRING;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = source != NULL ? (const uint8_t*)(source + start) : NULL;
    value->s.len = end >= start ? end - start : 0;
}

static int32_t SLEvalResolveNamedTypeDeclInPackage(
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const SLParsedFile** outFile,
    uint8_t*             outNamedKind) {
    uint32_t fileIndex;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outNamedKind != NULL) {
        *outNamedKind = 0;
    }
    if (pkg == NULL || callerFile == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const SLParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const SLAstNode* n = &pkgFile->ast.nodes[nodeId];
            uint8_t          namedKind = 0;
            if (n->kind == SLAst_TYPE_ALIAS) {
                namedKind = SLEvalTypeKind_ALIAS;
            } else if (n->kind == SLAst_STRUCT) {
                namedKind = SLEvalTypeKind_STRUCT;
            } else if (n->kind == SLAst_UNION) {
                namedKind = SLEvalTypeKind_UNION;
            } else if (n->kind == SLAst_ENUM) {
                namedKind = SLEvalTypeKind_ENUM;
            }
            if (namedKind != 0
                && SliceEqSlice(
                    callerFile->source,
                    nameStart,
                    nameEnd,
                    pkgFile->source,
                    n->dataStart,
                    n->dataEnd))
            {
                if (outFile != NULL) {
                    *outFile = pkgFile;
                }
                if (outNamedKind != NULL) {
                    *outNamedKind = namedKind;
                }
                return nodeId;
            }
            nodeId = ASTNextSibling(&pkgFile->ast, nodeId);
        }
    }
    return -1;
}

static int SLEvalResolveTypePackageAndName(
    const SLEvalProgram* p,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const SLPackage**    outPkg,
    uint32_t*            outLookupStart,
    uint32_t*            outLookupEnd) {
    const SLPackage* currentPkg;
    uint32_t         dot = nameStart;
    if (outPkg != NULL) {
        *outPkg = NULL;
    }
    if (outLookupStart != NULL) {
        *outLookupStart = nameStart;
    }
    if (outLookupEnd != NULL) {
        *outLookupEnd = nameEnd;
    }
    if (p == NULL || callerFile == NULL) {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(p, callerFile);
    while (dot < nameEnd) {
        if (callerFile->source[dot] == '.') {
            break;
        }
        dot++;
    }
    if (dot < nameEnd && currentPkg != NULL) {
        uint32_t i;
        for (i = 0; i < currentPkg->importLen; i++) {
            const SLImportRef* imp = &currentPkg->imports[i];
            if (imp->bindName == NULL || imp->target == NULL) {
                continue;
            }
            if (strlen(imp->bindName) == (size_t)(dot - nameStart)
                && memcmp(imp->bindName, callerFile->source + nameStart, (size_t)(dot - nameStart))
                       == 0)
            {
                if (outPkg != NULL) {
                    *outPkg = imp->target;
                }
                if (outLookupStart != NULL) {
                    *outLookupStart = dot + 1u;
                }
                return 1;
            }
        }
    }
    if (outPkg != NULL) {
        *outPkg = currentPkg;
    }
    return currentPkg != NULL;
}

static int SLEvalMakeNamedTypeValue(
    SLEvalProgram*      p,
    const SLParsedFile* declFile,
    int32_t             declNode,
    uint8_t             namedKind,
    SLCTFEValue*        outValue) {
    SLEvalReflectedType* rt;
    if (p == NULL || declFile == NULL || outValue == NULL || declNode < 0) {
        return 0;
    }
    rt = (SLEvalReflectedType*)SLArenaAlloc(
        p->arena, sizeof(SLEvalReflectedType), (uint32_t)_Alignof(SLEvalReflectedType));
    if (rt == NULL) {
        return -1;
    }
    memset(rt, 0, sizeof(*rt));
    rt->kind = SLEvalReflectType_NAMED;
    rt->namedKind = namedKind;
    rt->file = declFile;
    rt->nodeId = declNode;
    SLEvalValueSetReflectedTypeValue(outValue, rt);
    return 1;
}

static int SLEvalResolveTypeValueName(
    SLEvalProgram*      p,
    const SLParsedFile* callerFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    SLCTFEValue*        outValue) {
    const SLPackage*    targetPkg = NULL;
    const SLParsedFile* declFile = NULL;
    uint32_t            lookupStart = nameStart;
    uint32_t            lookupEnd = nameEnd;
    uint8_t             namedKind = 0;
    int32_t             typeCode = SLEvalTypeCode_INVALID;
    int32_t             declNode = -1;
    uint32_t            pkgIndex;
    if (p == NULL || callerFile == NULL || outValue == NULL) {
        return 0;
    }
    if (SLEvalBuiltinTypeCode(callerFile->source, nameStart, nameEnd, &typeCode)) {
        SLEvalValueSetSimpleTypeValue(outValue, typeCode);
        return 1;
    }
    if (SLEvalResolveTypePackageAndName(
            p, callerFile, nameStart, nameEnd, &targetPkg, &lookupStart, &lookupEnd))
    {
        declNode = SLEvalResolveNamedTypeDeclInPackage(
            targetPkg, callerFile, lookupStart, lookupEnd, &declFile, &namedKind);
        if (declNode >= 0 && declFile != NULL) {
            return SLEvalMakeNamedTypeValue(p, declFile, declNode, namedKind, outValue);
        }
        {
            int32_t topConstIndex = SLEvalFindTopConstBySliceInPackage(
                p, targetPkg, callerFile, lookupStart, lookupEnd);
            if (topConstIndex >= 0) {
                int isConst = 0;
                if (SLEvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                    return -1;
                }
                if (isConst && outValue->kind == SLCTFEValue_TYPE) {
                    return 1;
                }
            }
        }
    }
    if (p->loader == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &p->loader->packages[pkgIndex];
        int32_t          topConstIndex;
        if (pkg == targetPkg) {
            continue;
        }
        declNode = SLEvalResolveNamedTypeDeclInPackage(
            pkg, callerFile, lookupStart, lookupEnd, &declFile, &namedKind);
        if (declNode >= 0 && declFile != NULL) {
            return SLEvalMakeNamedTypeValue(p, declFile, declNode, namedKind, outValue);
        }
        topConstIndex = SLEvalFindTopConstBySliceInPackage(
            p, pkg, callerFile, lookupStart, lookupEnd);
        if (topConstIndex >= 0) {
            int isConst = 0;
            if (SLEvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst && outValue->kind == SLCTFEValue_TYPE) {
                return 1;
            }
        }
    }
    return 0;
}

static uint64_t SLEvalMakeAggregateTag(const SLParsedFile* file, int32_t nodeId) {
    uint64_t tag = 0;
    if (file != NULL) {
        tag = (uint64_t)(uintptr_t)file;
    }
    tag ^= (uint64_t)(uint32_t)(nodeId + 1) << 2;
    return tag
         & ~(SL_EVAL_PACKAGE_REF_TAG_FLAG | SL_EVAL_NULL_FIXED_LEN_TAG
             | SLCTFEValueTag_AGG_PARTIAL);
}

static void SLEvalValueSetAggregate(
    SLCTFEValue* value, const SLParsedFile* file, int32_t nodeId, SLEvalAggregate* aggregate) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_AGGREGATE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = SLEvalMakeAggregateTag(file, nodeId);
    value->s.bytes = (const uint8_t*)aggregate;
    value->s.len = aggregate != NULL ? aggregate->fieldLen : 0;
}

static SLEvalAggregate* _Nullable SLEvalValueAsAggregate(const SLCTFEValue* value) {
    if (value == NULL || value->kind != SLCTFEValue_AGGREGATE || value->s.bytes == NULL) {
        return NULL;
    }
    return (SLEvalAggregate*)value->s.bytes;
}

static SLCTFEValue* _Nullable SLEvalValueReferenceTarget(const SLCTFEValue* value);
static void SLEvalValueSetReference(SLCTFEValue* value, SLCTFEValue* target);
static int  SLEvalExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int  SLEvalZeroInitTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    SLCTFEValue*         outValue,
    int*                 outIsConst);
static int SLEvalResolveAliasCastTargetNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    const SLParsedFile** outAliasFile,
    int32_t*             outAliasNode,
    int32_t*             outTargetNode);
static int SLEvalResolveAggregateDeclFromValue(
    const SLEvalProgram* p,
    const SLCTFEValue*   value,
    const SLParsedFile** outFile,
    int32_t*             outNode);
static int SLEvalAggregateSetFieldValue(
    SLEvalAggregate*   agg,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* inValue,
    SLCTFEValue* _Nullable outValue);
static int SLEvalExecExprInFileWithType(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    SLCTFEExecEnv*      env,
    int32_t             exprNode,
    const SLParsedFile* typeFile,
    int32_t             typeNode,
    SLCTFEValue*        outValue,
    int*                outIsConst);
static SLEvalAggregateField* _Nullable SLEvalAggregateFindDirectField(
    SLEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd);
static int SLEvalValueSetFieldPath(
    SLCTFEValue*       value,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* inValue);
static int SLEvalFinalizeAggregateVarArrays(SLEvalProgram* p, SLEvalAggregate* agg);
static int SLEvalForInIndexCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    const SLCTFEValue* sourceValue,
    uint32_t           index,
    int                byRef,
    SLCTFEValue*       outValue,
    int*               outIsConst);
static int SLEvalForInIterCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    int32_t            sourceNode,
    const SLCTFEValue* sourceValue,
    uint32_t           index,
    int                hasKey,
    int                keyRef,
    int                valueRef,
    int                valueDiscard,
    int*               outHasItem,
    SLCTFEValue*       outKey,
    int*               outKeyIsConst,
    SLCTFEValue*       outValue,
    int*               outValueIsConst);

static void SLEvalValueSetArray(
    SLCTFEValue* value, const SLParsedFile* file, int32_t typeNode, SLEvalArray* array) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_ARRAY;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = SLEvalMakeAggregateTag(file, typeNode);
    value->s.bytes = (const uint8_t*)array;
    value->s.len = array != NULL ? array->len : 0;
}

static SLEvalArray* _Nullable SLEvalValueAsArray(const SLCTFEValue* value) {
    if (value == NULL || value->kind != SLCTFEValue_ARRAY || value->s.bytes == NULL) {
        return NULL;
    }
    return (SLEvalArray*)value->s.bytes;
}

static SLEvalArray* _Nullable SLEvalAllocArrayView(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    int32_t              elemTypeNode,
    SLCTFEValue* _Nullable elems,
    uint32_t len) {
    SLEvalArray* array;
    if (p == NULL) {
        return NULL;
    }
    array = (SLEvalArray*)SLArenaAlloc(
        p->arena, sizeof(SLEvalArray), (uint32_t)_Alignof(SLEvalArray));
    if (array == NULL) {
        return NULL;
    }
    memset(array, 0, sizeof(*array));
    array->file = file;
    array->typeNode = typeNode;
    array->elemTypeNode = elemTypeNode;
    array->elems = elems;
    array->len = len;
    return array;
}

static int SLEvalAllocTupleValue(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    int32_t             typeNode,
    const SLCTFEValue*  elems,
    uint32_t            len,
    SLCTFEValue*        outValue,
    int*                outIsConst) {
    SLEvalArray* array;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || file == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    array = SLEvalAllocArrayView(p, file, typeNode, -1, NULL, len);
    if (array == NULL) {
        return ErrorSimple("out of memory");
    }
    if (len > 0) {
        array->elems = (SLCTFEValue*)SLArenaAlloc(
            p->arena, sizeof(SLCTFEValue) * len, (uint32_t)_Alignof(SLCTFEValue));
        if (array->elems == NULL) {
            return ErrorSimple("out of memory");
        }
        memcpy(array->elems, elems, sizeof(SLCTFEValue) * len);
    }
    SLEvalValueSetArray(outValue, file, typeNode, array);
    *outIsConst = 1;
    return 0;
}

static const SLCTFEValue* SLEvalValueTargetOrSelf(const SLCTFEValue* value) {
    const SLCTFEValue* target = SLEvalValueReferenceTarget(value);
    return target != NULL ? target : value;
}

static int SLEvalOptionalPayload(const SLCTFEValue* value, const SLCTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (value == NULL || value->kind != SLCTFEValue_OPTIONAL || outPayload == NULL) {
        return 0;
    }
    if (value->b == 0u) {
        return 1;
    }
    if (value->s.bytes == NULL) {
        return 0;
    }
    *outPayload = (const SLCTFEValue*)value->s.bytes;
    return 1;
}

static int SLEvalResolveSimpleAliasCastTarget(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    char*                outTargetKind,
    uint64_t* _Nullable outAliasTag);

static int SLEvalCoerceValueToTypeNode(
    SLEvalProgram* p, const SLParsedFile* typeFile, int32_t typeNode, SLCTFEValue* inOutValue) {
    const SLAstNode*   type;
    const SLCTFEValue* sourceValue;
    SLEvalAggregate*   sourceAgg;
    const SLCTFEValue* optionalPayload = NULL;
    int32_t            targetTypeCode = SLEvalTypeCode_INVALID;
    uint32_t           i;
    if (p == NULL || typeFile == NULL || typeNode < 0 || inOutValue == NULL
        || (uint32_t)typeNode >= typeFile->ast.len)
    {
        return -1;
    }
    if (SLEvalTypeNodeIsAnytype(typeFile, typeNode)) {
        SLEvalAnnotateUntypedLiteralValue(inOutValue);
        return 0;
    }
    type = &typeFile->ast.nodes[typeNode];
    sourceValue = SLEvalValueTargetOrSelf(inOutValue);
    if (type->kind == SLAst_TYPE_OPTIONAL) {
        int32_t payloadTypeNode = type->firstChild;
        if (sourceValue->kind == SLCTFEValue_OPTIONAL) {
            return 0;
        }
        if (sourceValue->kind == SLCTFEValue_NULL) {
            inOutValue->kind = SLCTFEValue_OPTIONAL;
            inOutValue->i64 = 0;
            inOutValue->f64 = 0.0;
            inOutValue->b = 0u;
            inOutValue->typeTag = 0;
            inOutValue->s.bytes = NULL;
            inOutValue->s.len = 0;
            return 0;
        }
        {
            SLCTFEValue  payloadValue = *inOutValue;
            SLCTFEValue* payloadCopy;
            if (payloadTypeNode >= 0
                && SLEvalCoerceValueToTypeNode(p, typeFile, payloadTypeNode, &payloadValue) != 0)
            {
                return -1;
            }
            payloadCopy = (SLCTFEValue*)SLArenaAlloc(
                p->arena, sizeof(SLCTFEValue), (uint32_t)_Alignof(SLCTFEValue));
            if (payloadCopy == NULL) {
                return ErrorSimple("out of memory");
            }
            *payloadCopy = payloadValue;
            inOutValue->kind = SLCTFEValue_OPTIONAL;
            inOutValue->i64 = 0;
            inOutValue->f64 = 0.0;
            inOutValue->b = 1u;
            inOutValue->typeTag = 0;
            inOutValue->s.bytes = (const uint8_t*)payloadCopy;
            inOutValue->s.len = 0;
            return 0;
        }
    }
    if (type->kind != SLAst_TYPE_OPTIONAL && sourceValue->kind == SLCTFEValue_OPTIONAL
        && SLEvalOptionalPayload(sourceValue, &optionalPayload))
    {
        if (sourceValue->b == 0u || optionalPayload == NULL) {
            return 0;
        }
        *inOutValue = *optionalPayload;
        sourceValue = inOutValue;
    }
    if (type->kind == SLAst_TYPE_NAME) {
        char     aliasTargetKind = '\0';
        uint64_t aliasTag = 0;
        if (SLEvalResolveSimpleAliasCastTarget(p, typeFile, typeNode, &aliasTargetKind, &aliasTag))
        {
            if ((aliasTargetKind == 'i' && sourceValue->kind == SLCTFEValue_INT)
                || (aliasTargetKind == 'f' && sourceValue->kind == SLCTFEValue_FLOAT)
                || (aliasTargetKind == 'b' && sourceValue->kind == SLCTFEValue_BOOL)
                || (aliasTargetKind == 's' && sourceValue->kind == SLCTFEValue_STRING))
            {
                *inOutValue = *sourceValue;
                inOutValue->typeTag = aliasTag;
                return 0;
            }
        }
    }
    sourceAgg = SLEvalValueAsAggregate(sourceValue);
    if ((type->kind == SLAst_TYPE_NAME || type->kind == SLAst_TYPE_ANON_STRUCT
         || type->kind == SLAst_TYPE_ANON_UNION)
        && sourceAgg != NULL)
    {
        SLCTFEValue        targetValue;
        int                targetIsConst = 0;
        SLEvalAggregate*   targetAgg;
        uint8_t*           explicitSet = NULL;
        SLCTFEExecBinding* fieldBindings = NULL;
        SLCTFEExecEnv      fieldFrame;
        int                finalizeRc = 0;
        int sourcePartial = (sourceValue->typeTag & SLCTFEValueTag_AGG_PARTIAL) != 0u;
        if (SLEvalZeroInitTypeNode(p, typeFile, typeNode, &targetValue, &targetIsConst) != 0) {
            return -1;
        }
        if (!targetIsConst) {
            return 0;
        }
        targetAgg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(&targetValue));
        if (targetAgg == NULL) {
            return 0;
        }
        if (targetAgg->fieldLen > 0) {
            explicitSet = (uint8_t*)SLArenaAlloc(
                p->arena, targetAgg->fieldLen, (uint32_t)_Alignof(uint8_t));
            fieldBindings = (SLCTFEExecBinding*)SLArenaAlloc(
                p->arena,
                sizeof(SLCTFEExecBinding) * targetAgg->fieldLen,
                (uint32_t)_Alignof(SLCTFEExecBinding));
            if (explicitSet == NULL || fieldBindings == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(explicitSet, 0, targetAgg->fieldLen);
            memset(fieldBindings, 0, sizeof(SLCTFEExecBinding) * targetAgg->fieldLen);
        }
        for (i = 0; i < sourceAgg->fieldLen; i++) {
            const SLEvalAggregateField* field = &sourceAgg->fields[i];
            SLCTFEValue                 fieldValue = field->value;
            if (sourcePartial && field->typeNode >= 0 && (field->_reserved & 1u) == 0u) {
                continue;
            }
            if (explicitSet != NULL) {
                uint32_t j;
                for (j = 0; j < targetAgg->fieldLen; j++) {
                    SLEvalAggregateField* targetField = &targetAgg->fields[j];
                    if (SliceEqSlice(
                            sourceAgg->file->source,
                            field->nameStart,
                            field->nameEnd,
                            targetAgg->file->source,
                            targetField->nameStart,
                            targetField->nameEnd))
                    {
                        if (!sourcePartial || (field->_reserved & 1u) != 0u || field->typeNode < 0)
                        {
                            explicitSet[j] = 1u;
                        }
                        if (targetField->typeNode >= 0
                            && SLEvalCoerceValueToTypeNode(
                                   p, targetAgg->file, targetField->typeNode, &fieldValue)
                                   != 0)
                        {
                            return -1;
                        }
                        break;
                    }
                }
            }
            if (!SLEvalValueSetFieldPath(
                    &targetValue,
                    sourceAgg->file->source,
                    field->nameStart,
                    field->nameEnd,
                    &fieldValue))
            {
                return 0;
            }
        }
        fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
        fieldFrame.bindings = fieldBindings;
        fieldFrame.bindingLen = 0;
        for (i = 0; i < targetAgg->fieldLen; i++) {
            SLEvalAggregateField* field = &targetAgg->fields[i];
            if ((explicitSet == NULL || explicitSet[i] == 0u) && field->defaultExprNode >= 0) {
                SLCTFEValue defaultValue;
                int         defaultIsConst = 0;
                if (SLEvalExecExprInFileWithType(
                        p,
                        targetAgg->file,
                        &fieldFrame,
                        field->defaultExprNode,
                        targetAgg->file,
                        field->typeNode,
                        &defaultValue,
                        &defaultIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!defaultIsConst) {
                    return 0;
                }
                field->value = defaultValue;
            }
            if (fieldBindings != NULL) {
                fieldBindings[fieldFrame.bindingLen].nameStart = field->nameStart;
                fieldBindings[fieldFrame.bindingLen].nameEnd = field->nameEnd;
                fieldBindings[fieldFrame.bindingLen].typeId = -1;
                fieldBindings[fieldFrame.bindingLen].typeNode = field->typeNode;
                fieldBindings[fieldFrame.bindingLen].mutable = 1u;
                fieldBindings[fieldFrame.bindingLen].value = field->value;
                fieldFrame.bindingLen++;
            }
            field->_reserved = 0;
        }
        finalizeRc = SLEvalFinalizeAggregateVarArrays(p, targetAgg);
        if (finalizeRc != 1) {
            return finalizeRc < 0 ? -1 : 0;
        }
        *inOutValue = targetValue;
    }
    if (SLEvalStringViewTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)
        && sourceValue->kind != SLCTFEValue_STRING)
    {
        SLCTFEValue stringValue;
        int         stringRc = SLEvalStringValueFromArrayBytes(
            p->arena, sourceValue, targetTypeCode, &stringValue);
        if (stringRc < 0) {
            return -1;
        }
        if (stringRc > 0) {
            *inOutValue = stringValue;
        }
    }
    if (SLEvalAdaptStringValueForType(p->arena, typeFile, typeNode, inOutValue, inOutValue) < 0) {
        return -1;
    }
    sourceValue = SLEvalValueTargetOrSelf(inOutValue);
    if (SLEvalTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)) {
        if (sourceValue->kind == SLCTFEValue_BOOL && targetTypeCode == SLEvalTypeCode_BOOL) {
            SLEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == SLCTFEValue_FLOAT
            && (targetTypeCode == SLEvalTypeCode_F32 || targetTypeCode == SLEvalTypeCode_F64))
        {
            SLEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == SLCTFEValue_INT
            && (targetTypeCode == SLEvalTypeCode_U8 || targetTypeCode == SLEvalTypeCode_U16
                || targetTypeCode == SLEvalTypeCode_U32 || targetTypeCode == SLEvalTypeCode_U64
                || targetTypeCode == SLEvalTypeCode_UINT || targetTypeCode == SLEvalTypeCode_I8
                || targetTypeCode == SLEvalTypeCode_I16 || targetTypeCode == SLEvalTypeCode_I32
                || targetTypeCode == SLEvalTypeCode_I64 || targetTypeCode == SLEvalTypeCode_INT))
        {
            SLEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == SLCTFEValue_STRING
            && (targetTypeCode == SLEvalTypeCode_STR_REF
                || targetTypeCode == SLEvalTypeCode_STR_PTR))
        {
            SLEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        }
    }
    return 0;
}

static int SLEvalForInIndexCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    const SLCTFEValue* sourceValue,
    uint32_t           index,
    int                byRef,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    const SLCTFEValue* targetValue;
    SLEvalArray*       array;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (ctx == NULL || execCtx == NULL || sourceValue == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    targetValue = SLEvalValueTargetOrSelf(sourceValue);
    if (targetValue->kind == SLCTFEValue_ARRAY) {
        array = SLEvalValueAsArray(targetValue);
        if (array == NULL || index >= array->len) {
            return 0;
        }
        if (byRef) {
            SLEvalValueSetReference(outValue, &array->elems[index]);
        } else {
            *outValue = array->elems[index];
        }
        *outIsConst = 1;
        return 0;
    }
    if (targetValue->kind == SLCTFEValue_STRING) {
        if (index >= targetValue->s.len) {
            return 0;
        }
        if (byRef) {
            SLCTFEValue* byteValue = (SLCTFEValue*)SLArenaAlloc(
                ((SLEvalProgram*)ctx)->arena, sizeof(SLCTFEValue), (uint32_t)_Alignof(SLCTFEValue));
            if (byteValue == NULL) {
                return ErrorSimple("out of memory");
            }
            SLEvalValueSetInt(byteValue, (int64_t)targetValue->s.bytes[index]);
            SLEvalValueSetRuntimeTypeCode(byteValue, SLEvalTypeCode_U8);
            SLEvalValueSetReference(outValue, byteValue);
            *outIsConst = 1;
            return 0;
        }
        SLEvalValueSetInt(outValue, (int64_t)targetValue->s.bytes[index]);
        SLEvalValueSetRuntimeTypeCode(outValue, SLEvalTypeCode_U8);
        *outIsConst = 1;
        return 0;
    }
    SLCTFEExecSetReason(execCtx, 0, 0, "for-in loop source is not supported in evaluator backend");
    return 0;
}

static int SLEvalCollectCallArgs(
    SLEvalProgram* p,
    const SLAst*   ast,
    int32_t        firstArgNode,
    SLCTFEValue**  outArgs,
    uint32_t*      outArgCount,
    int32_t* _Nullable outLastArgNode) {
    SLCTFEValue tempArgs[256];
    uint32_t    argCount = 0;
    int32_t     argNode = firstArgNode;
    if (outArgs != NULL) {
        *outArgs = NULL;
    }
    if (outArgCount != NULL) {
        *outArgCount = 0;
    }
    if (outLastArgNode != NULL) {
        *outLastArgNode = -1;
    }
    if (p == NULL || ast == NULL || outArgs == NULL || outArgCount == NULL) {
        return -1;
    }
    while (argNode >= 0) {
        int32_t     argExprNode = argNode;
        SLCTFEValue argValue;
        int         argIsConst = 0;
        if (ast->nodes[argNode].kind == SLAst_CALL_ARG) {
            argExprNode = ast->nodes[argNode].firstChild;
        }
        if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
            *outArgCount = 0;
            *outArgs = NULL;
            return 0;
        }
        if (SLEvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
            return -1;
        }
        if (!argIsConst) {
            *outArgCount = 0;
            *outArgs = NULL;
            return 0;
        }
        SLEvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
        if (ast->nodes[argNode].kind == SLAst_CALL_ARG
            && (ast->nodes[argNode].flags & SLAstFlag_CALL_ARG_SPREAD) != 0)
        {
            SLEvalArray* array = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&argValue));
            uint32_t     i;
            if (array == NULL) {
                *outArgCount = 0;
                *outArgs = NULL;
                return 0;
            }
            if (array->len > 256u - argCount) {
                return ErrorSimple("too many call arguments for evaluator backend");
            }
            for (i = 0; i < array->len; i++) {
                tempArgs[argCount++] = array->elems[i];
            }
        } else {
            if (argCount >= 256u) {
                return ErrorSimple("too many call arguments for evaluator backend");
            }
            tempArgs[argCount++] = argValue;
        }
        if (outLastArgNode != NULL) {
            *outLastArgNode = argNode;
        }
        argNode = ast->nodes[argNode].nextSibling;
    }
    if (argCount > 0) {
        *outArgs = (SLCTFEValue*)SLArenaAlloc(
            p->arena, sizeof(SLCTFEValue) * argCount, (uint32_t)_Alignof(SLCTFEValue));
        if (*outArgs == NULL) {
            return ErrorSimple("out of memory");
        }
        memcpy(*outArgs, tempArgs, sizeof(SLCTFEValue) * argCount);
    }
    *outArgCount = argCount;
    return 1;
}

static int SLEvalCurrentContextFieldByLiteral(
    const SLEvalProgram* p, const char* fieldName, SLCTFEValue* outValue) {
    const SLEvalContext* context;
    if (p == NULL || fieldName == NULL || outValue == NULL) {
        return 0;
    }
    context = p->currentContext != NULL ? p->currentContext : &p->rootContext;
    if (strcmp(fieldName, "mem") == 0) {
        *outValue = context->mem;
        return 1;
    }
    if (strcmp(fieldName, "temp_mem") == 0) {
        *outValue = context->tempMem;
        return 1;
    }
    if (strcmp(fieldName, "log") == 0) {
        *outValue = context->log;
        return 1;
    }
    return 0;
}

static int SLEvalCurrentContextField(
    const SLEvalProgram* p,
    const char*          source,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    SLCTFEValue*         outValue) {
    if (source == NULL) {
        return 0;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "mem")) {
        return SLEvalCurrentContextFieldByLiteral(p, "mem", outValue);
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "temp_mem")) {
        return SLEvalCurrentContextFieldByLiteral(p, "temp_mem", outValue);
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "log")) {
        return SLEvalCurrentContextFieldByLiteral(p, "log", outValue);
    }
    return 0;
}

static int SLEvalMirContextGet(
    void* _Nullable ctx,
    uint32_t        fieldId,
    SLMirExecValue* outValue,
    int*            outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    const char*    fieldName = NULL;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    switch ((SLMirContextField)fieldId) {
        case SLMirContextField_MEM:      fieldName = "mem"; break;
        case SLMirContextField_TEMP_MEM: fieldName = "temp_mem"; break;
        case SLMirContextField_LOG:      fieldName = "log"; break;
        default:                         return 0;
    }
    if (!SLEvalCurrentContextFieldByLiteral(p, fieldName, outValue)) {
        return 0;
    }
    *outIsConst = 1;
    return 1;
}

static int SLEvalBuildContextOverlay(
    SLEvalProgram* p, int32_t overlayNode, SLEvalContext* outContext, const SLParsedFile* file);

static int SLEvalMirEvalWithContext(
    void* _Nullable ctx,
    uint32_t        sourceNode,
    SLMirExecValue* outValue,
    int*            outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*       p = (SLEvalProgram*)ctx;
    const SLEvalContext* savedContext;
    SLEvalContext        overlayContext;
    const SLAstNode*     n;
    int32_t              exprNode;
    int32_t              overlayNode;
    int                  overlayRc;
    int                  rc;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL
        || sourceNode >= p->currentFile->ast.len)
    {
        return -1;
    }
    n = &p->currentFile->ast.nodes[sourceNode];
    if (n->kind != SLAst_CALL_WITH_CONTEXT) {
        return 0;
    }
    exprNode = n->firstChild;
    overlayNode = exprNode >= 0 ? p->currentFile->ast.nodes[exprNode].nextSibling : -1;
    if (exprNode < 0) {
        return 0;
    }
    overlayRc = SLEvalBuildContextOverlay(p, overlayNode, &overlayContext, p->currentFile);
    if (overlayRc != 1) {
        return overlayRc < 0 ? -1 : 0;
    }
    savedContext = p->currentContext;
    p->currentContext = &overlayContext;
    rc = SLEvalExecExprCb(p, exprNode, outValue, outIsConst);
    p->currentContext = savedContext;
    if (rc != 0) {
        return -1;
    }
    return *outIsConst ? 1 : 0;
}

static int SLEvalBuildContextOverlay(
    SLEvalProgram* p, int32_t overlayNode, SLEvalContext* outContext, const SLParsedFile* file) {
    const SLAst* ast;
    int32_t      bindNode;
    if (p == NULL || outContext == NULL || file == NULL || p->currentFile == NULL) {
        return -1;
    }
    ast = &file->ast;
    *outContext = p->currentContext != NULL ? *p->currentContext : p->rootContext;
    if (overlayNode < 0 || (uint32_t)overlayNode >= ast->len) {
        return 1;
    }
    bindNode = ASTFirstChild(ast, overlayNode);
    while (bindNode >= 0) {
        const SLAstNode* bind = &ast->nodes[bindNode];
        int32_t          exprNode = ASTFirstChild(ast, bindNode);
        SLCTFEValue      fieldValue;
        int              fieldIsConst = 0;
        if (bind->kind != SLAst_CONTEXT_BIND) {
            bindNode = ASTNextSibling(ast, bindNode);
            continue;
        }
        if (exprNode >= 0) {
            if (SLEvalExecExprCb(p, exprNode, &fieldValue, &fieldIsConst) != 0) {
                return -1;
            }
            if (!fieldIsConst) {
                return 0;
            }
        } else if (!SLEvalCurrentContextField(
                       p, file->source, bind->dataStart, bind->dataEnd, &fieldValue))
        {
            if (p->currentExecCtx != NULL) {
                SLCTFEExecSetReason(
                    p->currentExecCtx,
                    bind->dataStart,
                    bind->dataEnd,
                    "context field is not available in evaluator backend");
            }
            return 0;
        }
        if (SliceEqCStr(file->source, bind->dataStart, bind->dataEnd, "mem")) {
            outContext->mem = fieldValue;
        } else if (SliceEqCStr(file->source, bind->dataStart, bind->dataEnd, "temp_mem")) {
            outContext->tempMem = fieldValue;
        } else if (SliceEqCStr(file->source, bind->dataStart, bind->dataEnd, "log")) {
            outContext->log = fieldValue;
        }
        bindNode = ASTNextSibling(ast, bindNode);
    }
    return 1;
}

static void SLEvalValueSetReference(SLCTFEValue* value, SLCTFEValue* target) {
    if (value == NULL) {
        return;
    }
    value->kind = SLCTFEValue_REFERENCE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = (const uint8_t*)target;
    value->s.len = 0;
}

static SLCTFEValue* _Nullable SLEvalValueReferenceTarget(const SLCTFEValue* value) {
    if (value == NULL || value->kind != SLCTFEValue_REFERENCE || value->s.bytes == NULL) {
        return NULL;
    }
    return (SLCTFEValue*)value->s.bytes;
}

static int32_t SLEvalFindNamedAggregateDeclInPackage(
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    int32_t              typeNode,
    const SLParsedFile** outFile) {
    const SLAstNode* typeNameNode;
    uint32_t         lookupStart;
    uint32_t         lookupEnd;
    uint32_t         fileIndex;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (pkg == NULL || callerFile == NULL || typeNode < 0
        || (uint32_t)typeNode >= callerFile->ast.len)
    {
        return -1;
    }
    typeNameNode = &callerFile->ast.nodes[typeNode];
    if (typeNameNode->kind != SLAst_TYPE_NAME) {
        return -1;
    }
    lookupStart = typeNameNode->dataStart;
    lookupEnd = typeNameNode->dataEnd;
    {
        uint32_t i;
        for (i = lookupStart; i < lookupEnd; i++) {
            if (callerFile->source[i] == '.') {
                lookupStart = i + 1u;
            }
        }
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const SLParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const SLAstNode* n = &pkgFile->ast.nodes[nodeId];
            if ((n->kind == SLAst_STRUCT || n->kind == SLAst_UNION)
                && SliceEqSlice(
                    callerFile->source,
                    lookupStart,
                    lookupEnd,
                    pkgFile->source,
                    n->dataStart,
                    n->dataEnd))
            {
                if (outFile != NULL) {
                    *outFile = pkgFile;
                }
                return nodeId;
            }
            nodeId = ASTNextSibling(&pkgFile->ast, nodeId);
        }
    }
    return -1;
}

static int32_t SLEvalFindNamedAggregateDecl(
    const SLEvalProgram* p,
    const SLParsedFile*  callerFile,
    int32_t              typeNode,
    const SLParsedFile** outFile) {
    const SLPackage* currentPkg;
    uint32_t         pkgIndex;
    int32_t          nodeId;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = SLEvalFindPackageByFile(p, callerFile);
    if (currentPkg != NULL) {
        nodeId = SLEvalFindNamedAggregateDeclInPackage(currentPkg, callerFile, typeNode, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    if (p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &p->loader->packages[pkgIndex];
        if (pkg == currentPkg) {
            continue;
        }
        nodeId = SLEvalFindNamedAggregateDeclInPackage(pkg, callerFile, typeNode, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    return -1;
}

static int32_t SLEvalFindNamedEnumDeclInPackage(
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const SLParsedFile** outFile) {
    uint32_t fileIndex;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (pkg == NULL || callerFile == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const SLParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const SLAstNode* n = &pkgFile->ast.nodes[nodeId];
            if (n->kind == SLAst_ENUM
                && SliceEqSlice(
                    callerFile->source,
                    nameStart,
                    nameEnd,
                    pkgFile->source,
                    n->dataStart,
                    n->dataEnd))
            {
                if (outFile != NULL) {
                    *outFile = pkgFile;
                }
                return nodeId;
            }
            nodeId = ASTNextSibling(&pkgFile->ast, nodeId);
        }
    }
    return -1;
}

static int32_t SLEvalFindNamedEnumDecl(
    const SLEvalProgram* p,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const SLParsedFile** outFile) {
    const SLPackage* currentPkg;
    uint32_t         pkgIndex;
    int32_t          nodeId;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = SLEvalFindPackageByFile(p, callerFile);
    if (currentPkg != NULL) {
        nodeId = SLEvalFindNamedEnumDeclInPackage(
            currentPkg, callerFile, nameStart, nameEnd, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    if (p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &p->loader->packages[pkgIndex];
        if (pkg == currentPkg) {
            continue;
        }
        nodeId = SLEvalFindNamedEnumDeclInPackage(pkg, callerFile, nameStart, nameEnd, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    return -1;
}

static int SLEvalFindEnumVariant(
    const SLParsedFile* enumFile,
    int32_t             enumNode,
    const char*         source,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    int32_t*            outVariantNode,
    uint32_t*           outTagIndex) {
    int32_t  child;
    uint32_t tagIndex = 0;
    if (outVariantNode != NULL) {
        *outVariantNode = -1;
    }
    if (outTagIndex != NULL) {
        *outTagIndex = 0;
    }
    if (enumFile == NULL || source == NULL || enumNode < 0
        || (uint32_t)enumNode >= enumFile->ast.len)
    {
        return 0;
    }
    child = ASTFirstChild(&enumFile->ast, enumNode);
    if (child >= 0 && enumFile->ast.nodes[child].kind != SLAst_FIELD) {
        child = ASTNextSibling(&enumFile->ast, child);
    }
    while (child >= 0) {
        const SLAstNode* fieldNode = &enumFile->ast.nodes[child];
        if (fieldNode->kind == SLAst_FIELD) {
            if (SliceEqSlice(
                    source,
                    nameStart,
                    nameEnd,
                    enumFile->source,
                    fieldNode->dataStart,
                    fieldNode->dataEnd))
            {
                if (outVariantNode != NULL) {
                    *outVariantNode = child;
                }
                if (outTagIndex != NULL) {
                    *outTagIndex = tagIndex;
                }
                return 1;
            }
            tagIndex++;
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    return 0;
}

static int SLEvalEnumHasPayloadVariants(const SLParsedFile* enumFile, int32_t enumNode) {
    int32_t child;
    if (enumFile == NULL || enumNode < 0 || (uint32_t)enumNode >= enumFile->ast.len) {
        return 0;
    }
    child = ASTFirstChild(&enumFile->ast, enumNode);
    while (child >= 0) {
        if (enumFile->ast.nodes[child].kind == SLAst_FIELD) {
            int32_t valueNode = ASTFirstChild(&enumFile->ast, child);
            if (valueNode >= 0 && (uint32_t)valueNode < enumFile->ast.len
                && enumFile->ast.nodes[valueNode].kind == SLAst_FIELD)
            {
                return 1;
            }
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    return 0;
}

static int SLEvalZeroInitTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    SLCTFEValue*         outValue,
    int*                 outIsConst);
static int SLEvalResolveAggregateTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  typeFile,
    int32_t              typeNode,
    const SLParsedFile** outDeclFile,
    int32_t*             outDeclNode);
static int SLEvalExecExprWithTypeNode(
    SLEvalProgram*      p,
    int32_t             exprNode,
    const SLParsedFile* typeFile,
    int32_t             typeNode,
    SLCTFEValue*        outValue,
    int*                outIsConst);
static int SLEvalExecExprInFileWithType(
    SLEvalProgram*      p,
    const SLParsedFile* exprFile,
    SLCTFEExecEnv*      env,
    int32_t             exprNode,
    const SLParsedFile* typeFile,
    int32_t             typeNode,
    SLCTFEValue*        outValue,
    int*                outIsConst);
static int SLEvalExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalAssignExprCb(
    void* ctx, SLCTFEExecCtx* execCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalAssignValueExprCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    int32_t            lhsExprNode,
    const SLCTFEValue* inValue,
    SLCTFEValue*       outValue,
    int*               outIsConst);
static int SLEvalMatchPatternCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    const SLCTFEValue* subjectValue,
    int32_t            labelExprNode,
    int*               outMatched);
static int SLEvalZeroInitCb(void* ctx, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalMirZeroInitLocal(
    void*               ctx,
    const SLMirTypeRef* typeRef,
    SLCTFEValue*        outValue,
    int*                outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirCoerceValueForType(
    void* ctx, const SLMirTypeRef* typeRef, SLCTFEValue* inOutValue, SLDiag* _Nullable diag);
static int SLEvalMirIndexValue(
    void*              ctx,
    const SLCTFEValue* base,
    const SLCTFEValue* index,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirIndexAddr(
    void*              ctx,
    const SLCTFEValue* base,
    const SLCTFEValue* index,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirSliceValue(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull base,
    const SLCTFEValue* _Nullable start,
    const SLCTFEValue* _Nullable end,
    uint16_t flags,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirAggGetField(
    void*              ctx,
    const SLCTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirAggAddrField(
    void*              ctx,
    const SLCTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirAggSetField(
    void* _Nullable ctx,
    SLCTFEValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLCTFEValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirMakeTuple(
    void*              ctx,
    const SLCTFEValue* elems,
    uint32_t           elemCount,
    uint32_t           typeNodeHint,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirMakeVariadicPack(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirTypeRef* _Nullable paramTypeRef,
    uint16_t           callFlags,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirHostCall(
    void*              ctx,
    uint32_t           hostId,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalTryMirZeroInitType(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    int32_t             typeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    SLCTFEValue*        outValue,
    int*                outIsConst);
static int SLEvalTryMirEvalTopInit(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const SLParsedFile* _Nullable coerceTypeFile,
    int32_t      coerceTypeNode,
    SLCTFEValue* outValue,
    int*         outIsConst,
    int* _Nullable outSupported);
static int SLEvalMirBuildTopInitProgram(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    SLMirProgram*       outProgram,
    SLEvalMirExecCtx*   outExecCtx,
    uint32_t*           outRootMirFnIndex,
    int*                outSupported);
static void SLEvalMirAdaptOutValue(
    const SLEvalMirExecCtx* c, SLCTFEValue* _Nullable value, int* _Nullable inOutIsConst);
static int SLEvalTryMirEvalExprWithType(
    SLEvalProgram*      p,
    int32_t             exprNode,
    const SLParsedFile* exprFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const SLParsedFile* _Nullable typeFile,
    int32_t      typeNode,
    SLCTFEValue* outValue,
    int*         outIsConst,
    int*         outSupported);
static int SLEvalMirEnterFunction(
    void* ctx, uint32_t functionIndex, uint32_t sourceRef, SLDiag* _Nullable diag);
static void SLEvalMirLeaveFunction(void* ctx);
static int  SLEvalMirBindFrame(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLCTFEValue* _Nullable locals,
    uint32_t localCount,
    SLDiag* _Nullable diag);
static void SLEvalMirUnbindFrame(void* _Nullable ctx);
static void SLEvalMirInitExecEnv(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    SLMirExecEnv*       env,
    SLEvalMirExecCtx* _Nullable functionCtx);
static int SLEvalMirEvalBinary(
    void* _Nullable ctx,
    SLTokenKind op,
    const SLMirExecValue* _Nonnull lhs,
    const SLMirExecValue* _Nonnull rhs,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirAllocNew(
    void* _Nullable ctx,
    uint32_t sourceNode,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirContextGet(
    void* _Nullable ctx,
    uint32_t fieldId,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirEvalWithContext(
    void* _Nullable ctx,
    uint32_t sourceNode,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalResolveIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirAssignIdent(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* inValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalResolveCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* _Nullable args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalResolveCallMir(
    void* ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* _Nullable args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalResolveCallMirPre(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalEvalTopVar(
    SLEvalProgram* p, uint32_t topVarIndex, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalInvokeFunction(
    SLEvalProgram*       p,
    int32_t              fnIndex,
    const SLCTFEValue*   args,
    uint32_t             argCount,
    const SLEvalContext* callContext,
    SLCTFEValue*         outValue,
    int*                 outDidReturn);

static int32_t SLEvalAggregateLookupFieldIndex(
    const SLEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const SLEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return (int32_t)i;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const SLEvalAggregateField* field = &agg->fields[i];
        SLEvalAggregate*            embedded = NULL;
        if ((field->flags & SLAstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = SLEvalValueAsAggregate(&field->value);
        if (embedded != NULL) {
            int32_t nested = SLEvalAggregateLookupFieldIndex(embedded, source, nameStart, nameEnd);
            if (nested >= 0) {
                return (int32_t)i;
            }
        }
    }
    return -1;
}

static int SLEvalAggregateGetFieldValue(
    const SLEvalAggregate* agg,
    const char*            source,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    SLCTFEValue*           outValue) {
    int32_t fieldIndex;
    if (outValue == NULL) {
        return 0;
    }
    fieldIndex = SLEvalAggregateLookupFieldIndex(agg, source, nameStart, nameEnd);
    if (fieldIndex < 0) {
        return 0;
    }
    {
        const SLEvalAggregateField* field = &agg->fields[fieldIndex];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            *outValue = field->value;
            return 1;
        }
        if ((field->flags & SLAstFlag_FIELD_EMBEDDED) != 0) {
            SLEvalAggregate* embedded = SLEvalValueAsAggregate(&field->value);
            if (embedded != NULL) {
                return SLEvalAggregateGetFieldValue(embedded, source, nameStart, nameEnd, outValue);
            }
        }
    }
    return 0;
}

static SLCTFEValue* _Nullable SLEvalAggregateLookupFieldValuePtr(
    SLEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return &field->value;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        SLEvalAggregate*      embedded;
        SLCTFEValue*          nested;
        if ((field->flags & SLAstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = SLEvalValueAsAggregate(&field->value);
        if (embedded == NULL) {
            continue;
        }
        nested = SLEvalAggregateLookupFieldValuePtr(embedded, source, nameStart, nameEnd);
        if (nested != NULL) {
            return nested;
        }
    }
    return NULL;
}

static int SLEvalAggregateSetFieldValue(
    SLEvalAggregate*   agg,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* inValue,
    SLCTFEValue* _Nullable outValue) {
    uint32_t i;
    if (agg == NULL || source == NULL || inValue == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            field->value = *inValue;
            if (outValue != NULL) {
                *outValue = field->value;
            }
            return 1;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        SLEvalAggregate*      embedded = NULL;
        if ((field->flags & SLAstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = SLEvalValueAsAggregate(&field->value);
        if (embedded != NULL
            && SLEvalAggregateSetFieldValue(
                embedded, source, nameStart, nameEnd, inValue, outValue))
        {
            return 1;
        }
    }
    return 0;
}

static SLEvalAggregateField* _Nullable SLEvalAggregateFindDirectField(
    SLEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return field;
        }
    }
    return NULL;
}

static int SLEvalValueSetFieldPath(
    SLCTFEValue*       value,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* inValue) {
    uint32_t i;
    if (value == NULL || source == NULL || inValue == NULL) {
        return 0;
    }
    for (i = nameStart; i < nameEnd; i++) {
        if (source[i] == '.') {
            SLCTFEValue*          childValue = NULL;
            SLEvalAggregateField* field;
            SLEvalAggregate*      agg = SLEvalValueAsAggregate(value);
            SLEvalTaggedEnum*     tagged = SLEvalValueAsTaggedEnum(value);
            if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
                agg = tagged->payload;
            }
            if (agg == NULL) {
                agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(value));
            }
            if (agg == NULL) {
                return 0;
            }
            field = SLEvalAggregateFindDirectField(agg, source, nameStart, i);
            if (field == NULL) {
                return 0;
            }
            childValue = &field->value;
            return SLEvalValueSetFieldPath(childValue, source, i + 1u, nameEnd, inValue);
        }
    }
    if (value->kind == SLCTFEValue_STRING) {
        if (SliceEqCStr(source, nameStart, nameEnd, "len") && inValue->kind == SLCTFEValue_INT
            && inValue->i64 >= 0)
        {
            value->s.len = (uint32_t)inValue->i64;
            return 1;
        }
        return 0;
    }
    {
        SLEvalAggregate*  agg = SLEvalValueAsAggregate(value);
        SLEvalTaggedEnum* tagged = SLEvalValueAsTaggedEnum(value);
        if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
            agg = tagged->payload;
        }
        if (agg == NULL) {
            agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(value));
        }
        if (agg != NULL) {
            return SLEvalAggregateSetFieldValue(agg, source, nameStart, nameEnd, inValue, NULL);
        }
    }
    return 0;
}

static int SLEvalFinalizeAggregateVarArrays(SLEvalProgram* p, SLEvalAggregate* agg) {
    uint32_t i;
    if (p == NULL || agg == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        const SLAstNode*      typeNode;
        SLCTFEValue           lenValue;
        int64_t               len = 0;
        SLEvalArray*          array;
        uint32_t              j;
        if (field->typeNode < 0 || (uint32_t)field->typeNode >= agg->file->ast.len) {
            continue;
        }
        typeNode = &agg->file->ast.nodes[field->typeNode];
        if (typeNode->kind != SLAst_TYPE_VARRAY) {
            continue;
        }
        if (!SLEvalAggregateGetFieldValue(
                agg, agg->file->source, typeNode->dataStart, typeNode->dataEnd, &lenValue)
            || SLCTFEValueToInt64(&lenValue, &len) != 0 || len < 0)
        {
            return 0;
        }
        array = SLEvalAllocArrayView(
            p,
            agg->file,
            field->typeNode,
            ASTFirstChild(&agg->file->ast, field->typeNode),
            NULL,
            (uint32_t)len);
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        if (array->len > 0) {
            array->elems = (SLCTFEValue*)SLArenaAlloc(
                p->arena, sizeof(SLCTFEValue) * array->len, (uint32_t)_Alignof(SLCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(SLCTFEValue) * array->len);
            for (j = 0; j < array->len; j++) {
                int elemIsConst = 0;
                if (SLEvalZeroInitTypeNode(
                        p, agg->file, array->elemTypeNode, &array->elems[j], &elemIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!elemIsConst) {
                    return 0;
                }
            }
        }
        SLEvalValueSetArray(&field->value, agg->file, field->typeNode, array);
    }
    return 1;
}

static int SLEvalBuildTaggedEnumPayload(
    SLEvalProgram*      p,
    const SLParsedFile* enumFile,
    int32_t             variantNode,
    int32_t             compoundLitNode,
    SLCTFEValue*        outValue,
    int*                outIsConst) {
    uint32_t         fieldCount = 0;
    uint32_t         fieldIndex = 0;
    int32_t          child;
    SLEvalAggregate* agg;
    int32_t          fieldNode;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || enumFile == NULL || outValue == NULL || outIsConst == NULL || variantNode < 0
        || (uint32_t)variantNode >= enumFile->ast.len)
    {
        return -1;
    }
    child = ASTFirstChild(&enumFile->ast, variantNode);
    while (child >= 0) {
        if (enumFile->ast.nodes[child].kind == SLAst_FIELD) {
            fieldCount++;
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    agg = (SLEvalAggregate*)SLArenaAlloc(
        p->arena, sizeof(SLEvalAggregate), (uint32_t)_Alignof(SLEvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = enumFile;
    agg->nodeId = variantNode;
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (SLEvalAggregateField*)SLArenaAlloc(
            p->arena,
            sizeof(SLEvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(SLEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(SLEvalAggregateField) * fieldCount);
    }
    child = ASTFirstChild(&enumFile->ast, variantNode);
    while (child >= 0) {
        const SLAstNode* variantField = &enumFile->ast.nodes[child];
        if (variantField->kind == SLAst_FIELD) {
            int32_t               fieldTypeNode = ASTFirstChild(&enumFile->ast, child);
            int                   fieldIsConst = 0;
            SLEvalAggregateField* field = &agg->fields[fieldIndex++];
            field->nameStart = variantField->dataStart;
            field->nameEnd = variantField->dataEnd;
            field->typeNode = fieldTypeNode;
            if (SLEvalZeroInitTypeNode(p, enumFile, fieldTypeNode, &field->value, &fieldIsConst)
                != 0)
            {
                return -1;
            }
            if (!fieldIsConst) {
                return 0;
            }
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    fieldNode = -1;
    if (compoundLitNode >= 0 && p->currentFile != NULL
        && (uint32_t)compoundLitNode < p->currentFile->ast.len)
    {
        fieldNode = ASTFirstChild(&p->currentFile->ast, compoundLitNode);
        if (fieldNode >= 0 && IsFnReturnTypeNodeKind(p->currentFile->ast.nodes[fieldNode].kind)) {
            fieldNode = ASTNextSibling(&p->currentFile->ast, fieldNode);
        }
    }
    while (fieldNode >= 0) {
        const SLAstNode* compoundField = &p->currentFile->ast.nodes[fieldNode];
        int32_t          valueNode = ASTFirstChild(&p->currentFile->ast, fieldNode);
        SLCTFEValue      fieldValue;
        int              fieldIsConst = 0;
        if (compoundField->kind != SLAst_COMPOUND_FIELD || valueNode < 0) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
            return -1;
        }
        if (!fieldIsConst
            || !SLEvalAggregateSetFieldValue(
                agg,
                p->currentFile->source,
                compoundField->dataStart,
                compoundField->dataEnd,
                &fieldValue,
                NULL))
        {
            *outIsConst = 0;
            return 0;
        }
        fieldNode = ASTNextSibling(&p->currentFile->ast, fieldNode);
    }
    SLEvalValueSetAggregate(outValue, enumFile, variantNode, agg);
    *outIsConst = 1;
    return 0;
}

static int SLEvalZeroInitAggregateValue(
    const SLEvalProgram* p,
    const SLParsedFile*  declFile,
    int32_t              declNode,
    SLCTFEValue*         outValue,
    int*                 outIsConst) {
    const SLAstNode* aggregateDecl;
    SLEvalAggregate* agg;
    uint32_t         fieldCount = 0;
    uint32_t         fieldIndex = 0;
    int32_t          child;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue == NULL || p == NULL || declFile == NULL || declNode < 0
        || (uint32_t)declNode >= declFile->ast.len)
    {
        return -1;
    }
    aggregateDecl = &declFile->ast.nodes[declNode];
    if (aggregateDecl->kind != SLAst_STRUCT && aggregateDecl->kind != SLAst_UNION
        && aggregateDecl->kind != SLAst_TYPE_ANON_STRUCT
        && aggregateDecl->kind != SLAst_TYPE_ANON_UNION)
    {
        return 0;
    }
    child = ASTFirstChild(&declFile->ast, declNode);
    while (child >= 0) {
        if (declFile->ast.nodes[child].kind == SLAst_FIELD) {
            fieldCount++;
        }
        child = ASTNextSibling(&declFile->ast, child);
    }
    agg = (SLEvalAggregate*)SLArenaAlloc(
        p->arena, sizeof(SLEvalAggregate), (uint32_t)_Alignof(SLEvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = declFile;
    agg->nodeId = declNode;
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (SLEvalAggregateField*)SLArenaAlloc(
            p->arena,
            sizeof(SLEvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(SLEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(SLEvalAggregateField) * fieldCount);
    }
    child = ASTFirstChild(&declFile->ast, declNode);
    while (child >= 0) {
        const SLAstNode* fieldNode = &declFile->ast.nodes[child];
        if (fieldNode->kind == SLAst_FIELD) {
            int32_t fieldTypeNode = ASTFirstChild(&declFile->ast, child);
            int32_t fieldDefaultNode =
                fieldTypeNode >= 0 ? ASTNextSibling(&declFile->ast, fieldTypeNode) : -1;
            int                   fieldIsConst = 0;
            SLEvalAggregateField* field = &agg->fields[fieldIndex++];
            field->nameStart = fieldNode->dataStart;
            field->nameEnd = fieldNode->dataEnd;
            field->flags = (uint16_t)fieldNode->flags;
            field->typeNode = fieldTypeNode;
            field->defaultExprNode = fieldDefaultNode;
            if (SLEvalZeroInitTypeNode(p, declFile, fieldTypeNode, &field->value, &fieldIsConst)
                != 0)
            {
                return -1;
            }
            if (!fieldIsConst) {
                return 0;
            }
        }
        child = ASTNextSibling(&declFile->ast, child);
    }
    SLEvalValueSetAggregate(outValue, declFile, declNode, agg);
    if (outIsConst != NULL) {
        *outIsConst = 1;
    }
    return 0;
}

static int SLEvalTypeValueFromTypeNode(
    SLEvalProgram* p, const SLParsedFile* file, int32_t typeNode, SLCTFEValue* outValue) {
    const SLAstNode*     n;
    int32_t              childNode;
    SLEvalReflectedType* rt;
    uint32_t             arrayLen = 0;
    if (p == NULL || file == NULL || outValue == NULL || typeNode < 0
        || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == SLAst_TYPE_NAME) {
        return SLEvalResolveTypeValueName(p, file, n->dataStart, n->dataEnd, outValue);
    }
    if (n->kind != SLAst_TYPE_PTR && n->kind != SLAst_TYPE_REF && n->kind != SLAst_TYPE_ARRAY) {
        return 0;
    }
    childNode = ASTFirstChild(&file->ast, typeNode);
    if (childNode < 0 || !SLEvalTypeValueFromTypeNode(p, file, childNode, outValue)) {
        return 0;
    }
    rt = (SLEvalReflectedType*)SLArenaAlloc(
        p->arena, sizeof(SLEvalReflectedType), (uint32_t)_Alignof(SLEvalReflectedType));
    if (rt == NULL) {
        return -1;
    }
    memset(rt, 0, sizeof(*rt));
    if (n->kind == SLAst_TYPE_PTR) {
        rt->kind = SLEvalReflectType_PTR;
        rt->namedKind = SLEvalTypeKind_POINTER;
    } else if (n->kind == SLAst_TYPE_REF) {
        rt->kind = SLEvalReflectType_PTR;
        rt->namedKind = SLEvalTypeKind_REFERENCE;
    } else {
        if (!SLEvalParseUintSlice(file->source, n->dataStart, n->dataEnd, &arrayLen)) {
            return 0;
        }
        rt->kind = SLEvalReflectType_ARRAY;
        rt->namedKind = SLEvalTypeKind_ARRAY;
        rt->arrayLen = arrayLen;
    }
    rt->elemType = *outValue;
    SLEvalValueSetReflectedTypeValue(outValue, rt);
    return 1;
}

static SLCTFEExecBinding* _Nullable SLEvalFindBinding(
    const SLCTFEExecCtx* _Nullable execCtx,
    const SLParsedFile* file,
    uint32_t            nameStart,
    uint32_t            nameEnd);

static int SLEvalTypeValueFromExprNode(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    const SLAst*        ast,
    int32_t             exprNode,
    SLCTFEValue*        outValue) {
    const SLAstNode* n;
    if (p == NULL || file == NULL || ast == NULL || outValue == NULL || exprNode < 0
        || (uint32_t)exprNode >= ast->len)
    {
        return 0;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == SLAst_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return 0;
        }
        n = &ast->nodes[exprNode];
    }
    if (SLEvalTypeValueFromTypeNode(p, file, exprNode, outValue)) {
        return 1;
    }
    if (n->kind == SLAst_IDENT) {
        SLCTFEExecBinding* binding = SLEvalFindBinding(
            p->currentExecCtx, file, n->dataStart, n->dataEnd);
        const SLParsedFile* localTypeFile = NULL;
        int32_t             localTypeNode = -1;
        int32_t             visibleLocalTypeNode = -1;
        if (binding != NULL && binding->typeNode >= 0
            && !(
                file->ast.nodes[binding->typeNode].kind == SLAst_TYPE_NAME
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[binding->typeNode].dataStart,
                    file->ast.nodes[binding->typeNode].dataEnd,
                    "anytype"))
            && SLEvalTypeValueFromTypeNode(p, file, binding->typeNode, outValue))
        {
            return 1;
        }
        if (SLEvalFindVisibleLocalTypeNodeByName(
                file, n->start, n->dataStart, n->dataEnd, &visibleLocalTypeNode)
            && visibleLocalTypeNode >= 0
            && !(
                file->ast.nodes[visibleLocalTypeNode].kind == SLAst_TYPE_NAME
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[visibleLocalTypeNode].dataStart,
                    file->ast.nodes[visibleLocalTypeNode].dataEnd,
                    "anytype"))
            && SLEvalTypeValueFromTypeNode(p, file, visibleLocalTypeNode, outValue))
        {
            return 1;
        }
        if (SLEvalMirLookupLocalTypeNode(
                p, n->dataStart, n->dataEnd, &localTypeFile, &localTypeNode)
            && localTypeFile != NULL && localTypeNode >= 0
            && !(
                localTypeFile->ast.nodes[localTypeNode].kind == SLAst_TYPE_NAME
                && SliceEqCStr(
                    localTypeFile->source,
                    localTypeFile->ast.nodes[localTypeNode].dataStart,
                    localTypeFile->ast.nodes[localTypeNode].dataEnd,
                    "anytype"))
            && SLEvalTypeValueFromTypeNode(p, localTypeFile, localTypeNode, outValue))
        {
            return 1;
        }
        return SLEvalResolveTypeValueName(p, file, n->dataStart, n->dataEnd, outValue);
    }
    return 0;
}

static int SLEvalZeroInitTypeValue(
    const SLEvalProgram* p, const SLCTFEValue* typeValue, SLCTFEValue* outValue, int* outIsConst) {
    int32_t              typeCode = SLEvalTypeCode_INVALID;
    SLEvalReflectedType* rt;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || typeValue == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (SLEvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        switch (typeCode) {
            case SLEvalTypeCode_BOOL:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 0;
            case SLEvalTypeCode_F32:
            case SLEvalTypeCode_F64:
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 0;
            case SLEvalTypeCode_STR_REF:
            case SLEvalTypeCode_STR_PTR:
                outValue->kind = SLCTFEValue_STRING;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 0;
            case SLEvalTypeCode_U8:
            case SLEvalTypeCode_U16:
            case SLEvalTypeCode_U32:
            case SLEvalTypeCode_U64:
            case SLEvalTypeCode_UINT:
            case SLEvalTypeCode_I8:
            case SLEvalTypeCode_I16:
            case SLEvalTypeCode_I32:
            case SLEvalTypeCode_I64:
            case SLEvalTypeCode_INT:
                SLEvalValueSetInt(outValue, 0);
                *outIsConst = 1;
                return 0;
            default: return 0;
        }
    }
    rt = SLEvalValueAsReflectedType(typeValue);
    if (rt == NULL) {
        return 0;
    }
    if (rt->kind == SLEvalReflectType_NAMED) {
        const SLAstNode* declNode = NULL;
        if (rt->file == NULL || rt->nodeId < 0 || (uint32_t)rt->nodeId >= rt->file->ast.len) {
            return 0;
        }
        declNode = &rt->file->ast.nodes[rt->nodeId];
        if (rt->namedKind == SLEvalTypeKind_ALIAS) {
            int32_t     baseTypeNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            SLCTFEValue baseTypeValue;
            if (baseTypeNode < 0
                || !SLEvalTypeValueFromTypeNode(
                    (SLEvalProgram*)p, rt->file, baseTypeNode, &baseTypeValue))
            {
                return 0;
            }
            if (SLEvalZeroInitTypeValue(p, &baseTypeValue, outValue, outIsConst) != 0) {
                return -1;
            }
            if (*outIsConst) {
                outValue->typeTag = SLEvalMakeAliasTag(rt->file, rt->nodeId);
            }
            return 0;
        }
        if (rt->namedKind == SLEvalTypeKind_STRUCT || rt->namedKind == SLEvalTypeKind_UNION) {
            return SLEvalZeroInitAggregateValue(p, rt->file, rt->nodeId, outValue, outIsConst);
        }
        if (rt->namedKind == SLEvalTypeKind_ENUM && declNode != NULL) {
            int32_t  variantNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            uint32_t tagIndex = 0;
            while (variantNode >= 0) {
                if (rt->file->ast.nodes[variantNode].kind == SLAst_FIELD) {
                    SLEvalValueSetTaggedEnum(
                        (SLEvalProgram*)p,
                        outValue,
                        rt->file,
                        rt->nodeId,
                        rt->file->ast.nodes[variantNode].dataStart,
                        rt->file->ast.nodes[variantNode].dataEnd,
                        tagIndex,
                        NULL);
                    *outIsConst = 1;
                    return 0;
                }
                if (rt->file->ast.nodes[variantNode].kind == SLAst_FIELD) {
                    tagIndex++;
                }
                variantNode = ASTNextSibling(&rt->file->ast, variantNode);
            }
        }
        return 0;
    }
    if (rt->kind == SLEvalReflectType_PTR) {
        SLEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (rt->kind == SLEvalReflectType_ARRAY) {
        uint32_t     i;
        SLEvalArray* array = (SLEvalArray*)SLArenaAlloc(
            p->arena, sizeof(SLEvalArray), (uint32_t)_Alignof(SLEvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = rt->file;
        array->typeNode = rt->nodeId;
        array->len = rt->arrayLen;
        if (rt->arrayLen > 0) {
            array->elems = (SLCTFEValue*)SLArenaAlloc(
                p->arena, sizeof(SLCTFEValue) * rt->arrayLen, (uint32_t)_Alignof(SLCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(SLCTFEValue) * rt->arrayLen);
            for (i = 0; i < rt->arrayLen; i++) {
                int elemIsConst = 0;
                if (SLEvalZeroInitTypeValue(p, &rt->elemType, &array->elems[i], &elemIsConst) != 0)
                {
                    return -1;
                }
                if (!elemIsConst) {
                    return 0;
                }
            }
        }
        SLEvalValueSetArray(outValue, rt->file, rt->nodeId, array);
        *outIsConst = 1;
        return 0;
    }
    return 0;
}

static int SLEvalTypeKindOfValue(const SLCTFEValue* typeValue, int32_t* outKind) {
    int32_t              typeCode = SLEvalTypeCode_INVALID;
    SLEvalReflectedType* rt;
    if (outKind != NULL) {
        *outKind = 0;
    }
    if (typeValue == NULL || outKind == NULL || typeValue->kind != SLCTFEValue_TYPE) {
        return 0;
    }
    if (SLEvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        *outKind = SLEvalTypeKind_PRIMITIVE;
        return 1;
    }
    rt = SLEvalValueAsReflectedType(typeValue);
    if (rt == NULL) {
        return 0;
    }
    *outKind = (int32_t)rt->namedKind;
    return 1;
}

static int SLEvalTypeNameOfValue(SLCTFEValue* typeValue, SLCTFEValue* outValue) {
    int32_t              typeCode = SLEvalTypeCode_INVALID;
    SLEvalReflectedType* rt;
    if (typeValue == NULL || outValue == NULL || typeValue->kind != SLCTFEValue_TYPE) {
        return 0;
    }
    if (SLEvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        switch (typeCode) {
            case SLEvalTypeCode_BOOL: SLEvalValueSetStringSlice(outValue, "bool", 0, 4); return 1;
            case SLEvalTypeCode_U8:   SLEvalValueSetStringSlice(outValue, "u8", 0, 2); return 1;
            case SLEvalTypeCode_U16:  SLEvalValueSetStringSlice(outValue, "u16", 0, 3); return 1;
            case SLEvalTypeCode_U32:  SLEvalValueSetStringSlice(outValue, "u32", 0, 3); return 1;
            case SLEvalTypeCode_U64:  SLEvalValueSetStringSlice(outValue, "u64", 0, 3); return 1;
            case SLEvalTypeCode_UINT: SLEvalValueSetStringSlice(outValue, "uint", 0, 4); return 1;
            case SLEvalTypeCode_I8:   SLEvalValueSetStringSlice(outValue, "i8", 0, 2); return 1;
            case SLEvalTypeCode_I16:  SLEvalValueSetStringSlice(outValue, "i16", 0, 3); return 1;
            case SLEvalTypeCode_I32:  SLEvalValueSetStringSlice(outValue, "i32", 0, 3); return 1;
            case SLEvalTypeCode_I64:  SLEvalValueSetStringSlice(outValue, "i64", 0, 3); return 1;
            case SLEvalTypeCode_INT:  SLEvalValueSetStringSlice(outValue, "int", 0, 3); return 1;
            case SLEvalTypeCode_F32:  SLEvalValueSetStringSlice(outValue, "f32", 0, 3); return 1;
            case SLEvalTypeCode_F64:  SLEvalValueSetStringSlice(outValue, "f64", 0, 3); return 1;
            case SLEvalTypeCode_TYPE: SLEvalValueSetStringSlice(outValue, "type", 0, 4); return 1;
            case SLEvalTypeCode_ANYTYPE:
                SLEvalValueSetStringSlice(outValue, "anytype", 0, 7);
                return 1;
            default: return 0;
        }
    }
    rt = SLEvalValueAsReflectedType(typeValue);
    if (rt == NULL || rt->kind != SLEvalReflectType_NAMED || rt->file == NULL || rt->nodeId < 0
        || (uint32_t)rt->nodeId >= rt->file->ast.len)
    {
        return 0;
    }
    SLEvalValueSetStringSlice(
        outValue,
        rt->file->source,
        rt->file->ast.nodes[rt->nodeId].dataStart,
        rt->file->ast.nodes[rt->nodeId].dataEnd);
    return 1;
}

static int SLEvalZeroInitTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    SLCTFEValue*         outValue,
    int*                 outIsConst) {
    const SLAstNode*    typeNameNode;
    const SLPackage*    currentPkg;
    const SLParsedFile* aggregateFile = NULL;
    const SLParsedFile* aliasFile = NULL;
    int32_t             aggregateNode = -1;
    int32_t             aliasNode = -1;
    int32_t             aliasTargetNode = -1;
    uint64_t            nullTag = 0;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue == NULL || p == NULL || file == NULL || typeNode < 0
        || (uint32_t)typeNode >= file->ast.len)
    {
        return -1;
    }
    typeNameNode = &file->ast.nodes[typeNode];
    switch (typeNameNode->kind) {
        case SLAst_TYPE_NAME: {
            uint32_t            dot = typeNameNode->dataStart;
            const SLParsedFile* enumFile = NULL;
            int32_t             enumNode = -1;
            int32_t             variantNode = -1;
            uint32_t            tagIndex = 0;
            while (dot < typeNameNode->dataEnd && file->source[dot] != '.') {
                dot++;
            }
            if (dot < typeNameNode->dataEnd) {
                enumNode = SLEvalFindNamedEnumDecl(
                    p, file, typeNameNode->dataStart, dot, &enumFile);
                if (enumNode >= 0 && enumFile != NULL
                    && SLEvalFindEnumVariant(
                        enumFile,
                        enumNode,
                        file->source,
                        dot + 1u,
                        typeNameNode->dataEnd,
                        &variantNode,
                        &tagIndex))
                {
                    const SLAstNode* variantField = &enumFile->ast.nodes[variantNode];
                    SLCTFEValue      payloadValue;
                    SLEvalAggregate* payload = NULL;
                    int              payloadIsConst = 0;
                    if (SLEvalBuildTaggedEnumPayload(
                            (SLEvalProgram*)p,
                            enumFile,
                            variantNode,
                            -1,
                            &payloadValue,
                            &payloadIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (!payloadIsConst) {
                        return 0;
                    }
                    payload = SLEvalValueAsAggregate(&payloadValue);
                    SLEvalValueSetTaggedEnum(
                        (SLEvalProgram*)p,
                        outValue,
                        enumFile,
                        enumNode,
                        variantField->dataStart,
                        variantField->dataEnd,
                        tagIndex,
                        payload);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "bool")) {
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                SLEvalValueSetRuntimeTypeCode(outValue, SLEvalTypeCode_BOOL);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f32")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f64"))
            {
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                SLEvalValueSetRuntimeTypeCode(
                    outValue,
                    SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f32")
                        ? SLEvalTypeCode_F32
                        : SLEvalTypeCode_F64);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "string")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "str"))
            {
                outValue->kind = SLCTFEValue_STRING;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                SLEvalValueSetRuntimeTypeCode(outValue, SLEvalTypeCode_STR_REF);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "u8")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "u16")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "u32")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "u64")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "uint")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "i8")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "i16")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "i32")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "i64")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "int"))
            {
                int32_t typeCode = SLEvalTypeCode_INVALID;
                SLEvalValueSetInt(outValue, 0);
                if (SLEvalBuiltinTypeCode(
                        file->source, typeNameNode->dataStart, typeNameNode->dataEnd, &typeCode))
                {
                    SLEvalValueSetRuntimeTypeCode(outValue, typeCode);
                }
                *outIsConst = 1;
                return 0;
            }
            if (SLEvalResolveAliasCastTargetNode(
                    p, file, typeNode, &aliasFile, &aliasNode, &aliasTargetNode)
                && aliasFile != NULL && aliasNode >= 0 && aliasTargetNode >= 0)
            {
                if (SLEvalZeroInitTypeNode(p, aliasFile, aliasTargetNode, outValue, outIsConst)
                    != 0)
                {
                    return -1;
                }
                if (*outIsConst) {
                    outValue->typeTag = SLEvalMakeAliasTag(aliasFile, aliasNode);
                }
                return 0;
            }
            currentPkg = SLEvalFindPackageByFile(p, file);
            if (currentPkg == NULL) {
                return 0;
            }
            aggregateNode = SLEvalFindNamedAggregateDecl(p, file, typeNode, &aggregateFile);
            if (aggregateNode >= 0 && aggregateFile != NULL) {
                return SLEvalZeroInitAggregateValue(
                    p, aggregateFile, aggregateNode, outValue, outIsConst);
            }
            {
                SLCTFEValue typeValue;
                int32_t     topConstIndex = SLEvalFindTopConstBySlice(
                    p, file, typeNameNode->dataStart, typeNameNode->dataEnd);
                if (topConstIndex >= 0) {
                    int typeIsConst = 0;
                    if (SLEvalEvalTopConst(
                            (SLEvalProgram*)p, (uint32_t)topConstIndex, &typeValue, &typeIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (typeIsConst && typeValue.kind == SLCTFEValue_TYPE) {
                        return SLEvalZeroInitTypeValue(p, &typeValue, outValue, outIsConst);
                    }
                }
                if (SLEvalTypeValueFromTypeNode((SLEvalProgram*)p, file, typeNode, &typeValue)) {
                    return SLEvalZeroInitTypeValue(p, &typeValue, outValue, outIsConst);
                }
            }
            return 0;
        }
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
            SLEvalValueSetNull(outValue);
            if (SLEvalResolveNullCastTypeTag(file, typeNode, &nullTag)) {
                outValue->typeTag = nullTag;
            }
            *outIsConst = 1;
            return 0;
        case SLAst_TYPE_OPTIONAL:
            SLEvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        case SLAst_TYPE_ARRAY: {
            const SLAstNode* arrayTypeNode = &file->ast.nodes[typeNode];
            int32_t          elemTypeNode = ASTFirstChild(&file->ast, typeNode);
            uint32_t         len = 0;
            uint32_t         i;
            SLEvalArray*     array;
            if (elemTypeNode < 0
                || !SLEvalParseUintSlice(
                    file->source, arrayTypeNode->dataStart, arrayTypeNode->dataEnd, &len))
            {
                return 0;
            }
            array = (SLEvalArray*)SLArenaAlloc(
                p->arena, sizeof(SLEvalArray), (uint32_t)_Alignof(SLEvalArray));
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array, 0, sizeof(*array));
            array->file = file;
            array->typeNode = typeNode;
            array->elemTypeNode = elemTypeNode;
            array->len = len;
            if (len > 0) {
                array->elems = (SLCTFEValue*)SLArenaAlloc(
                    p->arena, sizeof(SLCTFEValue) * len, (uint32_t)_Alignof(SLCTFEValue));
                if (array->elems == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(array->elems, 0, sizeof(SLCTFEValue) * len);
                for (i = 0; i < len; i++) {
                    int elemIsConst = 0;
                    if (SLEvalZeroInitTypeNode(
                            p, file, elemTypeNode, &array->elems[i], &elemIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (!elemIsConst) {
                        return 0;
                    }
                }
            }
            SLEvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case SLAst_TYPE_VARRAY: {
            int32_t      elemTypeNode = ASTFirstChild(&file->ast, typeNode);
            SLEvalArray* array = SLEvalAllocArrayView(p, file, typeNode, elemTypeNode, NULL, 0);
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            SLEvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case SLAst_TYPE_TUPLE: {
            uint32_t     len = AstListCount(&file->ast, typeNode);
            uint32_t     i;
            SLEvalArray* array = (SLEvalArray*)SLArenaAlloc(
                p->arena, sizeof(SLEvalArray), (uint32_t)_Alignof(SLEvalArray));
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array, 0, sizeof(*array));
            array->file = file;
            array->typeNode = typeNode;
            array->len = len;
            if (len > 0) {
                array->elems = (SLCTFEValue*)SLArenaAlloc(
                    p->arena, sizeof(SLCTFEValue) * len, (uint32_t)_Alignof(SLCTFEValue));
                if (array->elems == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(array->elems, 0, sizeof(SLCTFEValue) * len);
                for (i = 0; i < len; i++) {
                    int32_t elemTypeNode = AstListItemAt(&file->ast, typeNode, i);
                    int     elemIsConst = 0;
                    if (elemTypeNode < 0
                        || SLEvalZeroInitTypeNode(
                               p, file, elemTypeNode, &array->elems[i], &elemIsConst)
                               != 0)
                    {
                        return elemTypeNode < 0 ? 0 : -1;
                    }
                    if (!elemIsConst) {
                        return 0;
                    }
                }
            }
            SLEvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case SLAst_TYPE_ANON_STRUCT:
        case SLAst_TYPE_ANON_UNION:
            return SLEvalZeroInitAggregateValue(p, file, typeNode, outValue, outIsConst);
        case SLAst_TYPE_FN:
            SLEvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        default: return 0;
    }
}

static int SLEvalResolveSimpleAliasCastTarget(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    char*                outTargetKind,
    uint64_t* _Nullable outAliasTag) {
    const SLPackage* currentPkg;
    const SLAstNode* typeNameNode;
    uint32_t         fileIndex;
    if (outTargetKind == NULL) {
        return 0;
    }
    *outTargetKind = '\0';
    if (outAliasTag != NULL) {
        *outAliasTag = 0;
    }
    if (p == NULL || file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    typeNameNode = &file->ast.nodes[typeNode];
    if (typeNameNode->kind != SLAst_TYPE_NAME) {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    for (fileIndex = 0; fileIndex < currentPkg->fileLen; fileIndex++) {
        const SLParsedFile* pkgFile = &currentPkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const SLAstNode* aliasNode = &pkgFile->ast.nodes[nodeId];
            if (aliasNode->kind == SLAst_TYPE_ALIAS
                && SliceEqSlice(
                    file->source,
                    typeNameNode->dataStart,
                    typeNameNode->dataEnd,
                    pkgFile->source,
                    aliasNode->dataStart,
                    aliasNode->dataEnd))
            {
                int32_t          targetNodeId = aliasNode->firstChild;
                const SLAstNode* targetNode;
                if (targetNodeId < 0 || (uint32_t)targetNodeId >= pkgFile->ast.len) {
                    return 0;
                }
                targetNode = &pkgFile->ast.nodes[targetNodeId];
                if (targetNode->kind != SLAst_TYPE_NAME) {
                    return 0;
                }
                if (SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "bool"))
                {
                    *outTargetKind = 'b';
                    if (outAliasTag != NULL) {
                        *outAliasTag = SLEvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                if (SliceEqCStr(pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "f32")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "f64"))
                {
                    *outTargetKind = 'f';
                    if (outAliasTag != NULL) {
                        *outAliasTag = SLEvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                if (SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "string"))
                {
                    *outTargetKind = 's';
                    if (outAliasTag != NULL) {
                        *outAliasTag = SLEvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                if (SliceEqCStr(pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "u8")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "u16")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "u32")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "u64")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "uint")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "i8")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "i16")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "i32")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "i64")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "int"))
                {
                    *outTargetKind = 'i';
                    if (outAliasTag != NULL) {
                        *outAliasTag = SLEvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                return 0;
            }
            nodeId = ASTNextSibling(&pkgFile->ast, nodeId);
        }
    }
    return 0;
}

static int SLEvalResolveAliasCastTargetNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    const SLParsedFile** outAliasFile,
    int32_t*             outAliasNode,
    int32_t*             outTargetNode) {
    const SLPackage* currentPkg;
    const SLAstNode* typeNameNode;
    uint32_t         fileIndex;
    if (outAliasFile != NULL) {
        *outAliasFile = NULL;
    }
    if (outAliasNode != NULL) {
        *outAliasNode = -1;
    }
    if (outTargetNode != NULL) {
        *outTargetNode = -1;
    }
    if (p == NULL || file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    typeNameNode = &file->ast.nodes[typeNode];
    if (typeNameNode->kind != SLAst_TYPE_NAME) {
        return 0;
    }
    currentPkg = SLEvalFindPackageByFile(p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    for (fileIndex = 0; fileIndex < currentPkg->fileLen; fileIndex++) {
        const SLParsedFile* pkgFile = &currentPkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const SLAstNode* aliasNode = &pkgFile->ast.nodes[nodeId];
            if (aliasNode->kind == SLAst_TYPE_ALIAS
                && SliceEqSlice(
                    file->source,
                    typeNameNode->dataStart,
                    typeNameNode->dataEnd,
                    pkgFile->source,
                    aliasNode->dataStart,
                    aliasNode->dataEnd))
            {
                int32_t targetNodeId = aliasNode->firstChild;
                if (targetNodeId < 0 || (uint32_t)targetNodeId >= pkgFile->ast.len) {
                    return 0;
                }
                if (outAliasFile != NULL) {
                    *outAliasFile = pkgFile;
                }
                if (outAliasNode != NULL) {
                    *outAliasNode = nodeId;
                }
                if (outTargetNode != NULL) {
                    *outTargetNode = targetNodeId;
                }
                return 1;
            }
            nodeId = ASTNextSibling(&pkgFile->ast, nodeId);
        }
    }
    return 0;
}

static int SLEvalDecodeNewExprNodes(
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

static int SLEvalAllocReferencedValue(
    SLEvalProgram* p, const SLCTFEValue* inValue, SLCTFEValue* outValue, int* outIsConst) {
    SLCTFEValue* target;
    if (p == NULL || inValue == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    target = (SLCTFEValue*)SLArenaAlloc(
        p->arena, sizeof(SLCTFEValue), (uint32_t)_Alignof(SLCTFEValue));
    if (target == NULL) {
        return ErrorSimple("out of memory");
    }
    *target = *inValue;
    SLEvalValueSetReference(outValue, target);
    *outIsConst = 1;
    return 0;
}

static int SLEvalEvalNewExpr(
    SLEvalProgram* p, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    int32_t     typeNode = -1;
    int32_t     countNode = -1;
    int32_t     initNode = -1;
    int32_t     allocNode = -1;
    SLCTFEValue allocValue;
    int         allocIsConst = 0;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!SLEvalDecodeNewExprNodes(
            p->currentFile, exprNode, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    if (allocNode >= 0) {
        if (SLEvalExecExprCb(p, allocNode, &allocValue, &allocIsConst) != 0) {
            return -1;
        }
        if (!allocIsConst) {
            return 0;
        }
    } else if (!SLEvalCurrentContextFieldByLiteral(p, "mem", &allocValue)) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReasonNode(
                p->currentExecCtx,
                exprNode,
                "allocator context is not available in evaluator backend");
        }
        return 0;
    } else {
        allocIsConst = 1;
    }
    if (SLEvalValueTargetOrSelf(&allocValue)->kind == SLCTFEValue_NULL) {
        *outValue = allocValue;
        *outIsConst = 1;
        return 0;
    }
    if (countNode >= 0) {
        SLCTFEValue  countValue;
        int          countIsConst = 0;
        int64_t      count = 0;
        SLEvalArray* array;
        SLCTFEValue  arrayValue;
        uint32_t     i;
        if (SLEvalExecExprCb(p, countNode, &countValue, &countIsConst) != 0) {
            return -1;
        }
        if (!countIsConst || SLCTFEValueToInt64(&countValue, &count) != 0 || count < 0) {
            return 0;
        }
        array = (SLEvalArray*)SLArenaAlloc(
            p->arena, sizeof(SLEvalArray), (uint32_t)_Alignof(SLEvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = p->currentFile;
        array->typeNode = exprNode;
        array->elemTypeNode = typeNode;
        array->len = (uint32_t)count;
        if (array->len > 0) {
            array->elems = (SLCTFEValue*)SLArenaAlloc(
                p->arena, sizeof(SLCTFEValue) * array->len, (uint32_t)_Alignof(SLCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(SLCTFEValue) * array->len);
            if (initNode >= 0) {
                SLCTFEValue initValue;
                int         initIsConst = 0;
                if (SLEvalExecExprWithTypeNode(
                        p, initNode, p->currentFile, typeNode, &initValue, &initIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!initIsConst) {
                    return 0;
                }
                for (i = 0; i < array->len; i++) {
                    array->elems[i] = initValue;
                }
            } else {
                for (i = 0; i < array->len; i++) {
                    int elemIsConst = 0;
                    if (SLEvalZeroInitTypeNode(
                            p, p->currentFile, typeNode, &array->elems[i], &elemIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (!elemIsConst) {
                        return 0;
                    }
                }
            }
        }
        SLEvalValueSetArray(&arrayValue, p->currentFile, exprNode, array);
        return SLEvalAllocReferencedValue(p, &arrayValue, outValue, outIsConst);
    }
    {
        SLCTFEValue value;
        int         valueIsConst = 0;
        if (initNode >= 0) {
            if (SLEvalExecExprWithTypeNode(
                    p, initNode, p->currentFile, typeNode, &value, &valueIsConst)
                != 0)
            {
                return -1;
            }
        } else if (SLEvalZeroInitTypeNode(p, p->currentFile, typeNode, &value, &valueIsConst) != 0)
        {
            return -1;
        }
        if (!valueIsConst) {
            return 0;
        }
        {
            SLEvalAggregate*   agg = SLEvalValueAsAggregate(&value);
            SLCTFEExecBinding* fieldBindings = NULL;
            SLCTFEExecEnv      fieldFrame;
            uint32_t           i;
            if (agg != NULL && agg->fieldLen > 0) {
                fieldBindings = (SLCTFEExecBinding*)SLArenaAlloc(
                    p->arena,
                    sizeof(SLCTFEExecBinding) * agg->fieldLen,
                    (uint32_t)_Alignof(SLCTFEExecBinding));
                if (fieldBindings == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(fieldBindings, 0, sizeof(SLCTFEExecBinding) * agg->fieldLen);
            }
            fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
            fieldFrame.bindings = fieldBindings;
            fieldFrame.bindingLen = 0;
            if (agg != NULL) {
                for (i = 0; i < agg->fieldLen; i++) {
                    SLEvalAggregateField* field = &agg->fields[i];
                    if (initNode < 0 && field->defaultExprNode >= 0) {
                        SLCTFEValue defaultValue;
                        int         defaultIsConst = 0;
                        if (SLEvalExecExprInFileWithType(
                                p,
                                agg->file,
                                &fieldFrame,
                                field->defaultExprNode,
                                agg->file,
                                field->typeNode,
                                &defaultValue,
                                &defaultIsConst)
                            != 0)
                        {
                            return -1;
                        }
                        if (!defaultIsConst) {
                            return 0;
                        }
                        field->value = defaultValue;
                    }
                    if (fieldBindings != NULL) {
                        fieldBindings[fieldFrame.bindingLen].nameStart = field->nameStart;
                        fieldBindings[fieldFrame.bindingLen].nameEnd = field->nameEnd;
                        fieldBindings[fieldFrame.bindingLen].typeId = -1;
                        fieldBindings[fieldFrame.bindingLen].typeNode = field->typeNode;
                        fieldBindings[fieldFrame.bindingLen].mutable = 1u;
                        fieldBindings[fieldFrame.bindingLen].value = field->value;
                        fieldFrame.bindingLen++;
                    }
                }
            }
        }
        return SLEvalAllocReferencedValue(p, &value, outValue, outIsConst);
    }
}

static int SLEvalValueConcatStrings(
    SLArena* arena, const SLCTFEValue* a, const SLCTFEValue* b, SLCTFEValue* outValue) {
    uint64_t totalLen64;
    uint32_t totalLen;
    uint8_t* bytes;
    if (arena == NULL || a == NULL || b == NULL || outValue == NULL) {
        return -1;
    }
    if (a->kind != SLCTFEValue_STRING || b->kind != SLCTFEValue_STRING) {
        return 0;
    }
    totalLen64 = (uint64_t)a->s.len + (uint64_t)b->s.len;
    if (totalLen64 > UINT32_MAX) {
        return 0;
    }
    totalLen = (uint32_t)totalLen64;
    outValue->kind = SLCTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    if (totalLen == 0) {
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        return 1;
    }
    bytes = (uint8_t*)SLArenaAlloc(arena, totalLen, (uint32_t)_Alignof(uint8_t));
    if (bytes == NULL) {
        return -1;
    }
    if (a->s.len > 0 && a->s.bytes != NULL) {
        memcpy(bytes, a->s.bytes, a->s.len);
    }
    if (b->s.len > 0 && b->s.bytes != NULL) {
        memcpy(bytes + a->s.len, b->s.bytes, b->s.len);
    }
    outValue->s.bytes = bytes;
    outValue->s.len = totalLen;
    return 1;
}

static void SLEvalValueSetUInt(SLCTFEValue* value, uint32_t n) {
    SLEvalValueSetInt(value, (int64_t)n);
    SLEvalValueSetRuntimeTypeCode(value, SLEvalTypeCode_UINT);
}

static int SLEvalValueCopyBuiltin(
    SLArena* arena, const SLCTFEValue* dstArg, const SLCTFEValue* srcArg, SLCTFEValue* outValue) {
    const SLCTFEValue* srcValue;
    SLCTFEValue*       dstValue;
    SLEvalArray*       dstArray;
    SLEvalArray*       srcArray;
    uint32_t           copyLen;
    if (arena == NULL || dstArg == NULL || srcArg == NULL || outValue == NULL) {
        return -1;
    }
    srcValue = SLEvalValueTargetOrSelf(srcArg);
    dstValue = SLEvalValueReferenceTarget(dstArg);
    if (dstValue == NULL) {
        dstValue = (SLCTFEValue*)SLEvalValueTargetOrSelf(dstArg);
    }
    if (dstValue == NULL || srcValue == NULL) {
        return 0;
    }
    dstArray = SLEvalValueAsArray(dstValue);
    srcArray = SLEvalValueAsArray(srcValue);
    if (dstArray != NULL && srcArray != NULL) {
        copyLen = dstArray->len < srcArray->len ? dstArray->len : srcArray->len;
        if (copyLen > 0) {
            SLCTFEValue* temp = (SLCTFEValue*)SLArenaAlloc(
                arena, sizeof(SLCTFEValue) * copyLen, (uint32_t)_Alignof(SLCTFEValue));
            if (temp == NULL) {
                return -1;
            }
            memcpy(temp, srcArray->elems, sizeof(SLCTFEValue) * copyLen);
            memcpy(dstArray->elems, temp, sizeof(SLCTFEValue) * copyLen);
        }
        SLEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstValue->kind == SLCTFEValue_STRING && srcValue->kind == SLCTFEValue_STRING) {
        int32_t dstTypeCode = SLEvalTypeCode_INVALID;
        if (!SLEvalValueGetRuntimeTypeCode(dstValue, &dstTypeCode)
            || dstTypeCode != SLEvalTypeCode_STR_PTR)
        {
            return 0;
        }
        copyLen = dstValue->s.len < srcValue->s.len ? dstValue->s.len : srcValue->s.len;
        if (copyLen > 0 && dstValue->s.bytes != NULL && srcValue->s.bytes != NULL) {
            memmove((uint8_t*)dstValue->s.bytes, srcValue->s.bytes, copyLen);
        }
        SLEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstArray != NULL && srcValue->kind == SLCTFEValue_STRING) {
        copyLen = dstArray->len < srcValue->s.len ? dstArray->len : srcValue->s.len;
        for (uint32_t i = 0; i < copyLen; i++) {
            SLEvalValueSetInt(&dstArray->elems[i], (int64_t)srcValue->s.bytes[i]);
            SLEvalValueSetRuntimeTypeCode(&dstArray->elems[i], SLEvalTypeCode_U8);
        }
        SLEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstValue->kind == SLCTFEValue_STRING && srcArray != NULL) {
        int32_t  dstTypeCode = SLEvalTypeCode_INVALID;
        uint8_t* tempBytes;
        if (!SLEvalValueGetRuntimeTypeCode(dstValue, &dstTypeCode)
            || dstTypeCode != SLEvalTypeCode_STR_PTR)
        {
            return 0;
        }
        copyLen = dstValue->s.len < srcArray->len ? dstValue->s.len : srcArray->len;
        tempBytes =
            copyLen > 0 ? (uint8_t*)SLArenaAlloc(arena, copyLen, (uint32_t)_Alignof(uint8_t))
                        : NULL;
        if (copyLen > 0 && tempBytes == NULL) {
            return -1;
        }
        for (uint32_t i = 0; i < copyLen; i++) {
            int64_t byteValue = 0;
            if (SLCTFEValueToInt64(&srcArray->elems[i], &byteValue) != 0 || byteValue < 0
                || byteValue > 255)
            {
                return 0;
            }
            tempBytes[i] = (uint8_t)byteValue;
        }
        if (copyLen > 0 && dstValue->s.bytes != NULL) {
            memmove((uint8_t*)dstValue->s.bytes, tempBytes, copyLen);
        }
        SLEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    return 0;
}

static int SLEvalStringValueFromArrayBytes(
    SLArena* arena, const SLCTFEValue* inValue, int32_t targetTypeCode, SLCTFEValue* outValue) {
    SLEvalArray* array;
    uint8_t*     bytes = NULL;
    uint32_t     i;
    if (arena == NULL || inValue == NULL || outValue == NULL) {
        return -1;
    }
    array = SLEvalValueAsArray(SLEvalValueTargetOrSelf(inValue));
    if (array == NULL) {
        return 0;
    }
    if (array->len > 0) {
        bytes = (uint8_t*)SLArenaAlloc(arena, array->len, (uint32_t)_Alignof(uint8_t));
        if (bytes == NULL) {
            return -1;
        }
        for (i = 0; i < array->len; i++) {
            int64_t byteValue = 0;
            if (SLCTFEValueToInt64(&array->elems[i], &byteValue) != 0 || byteValue < 0
                || byteValue > 255)
            {
                return 0;
            }
            bytes[i] = (uint8_t)byteValue;
        }
    }
    outValue->kind = SLCTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = bytes;
    outValue->s.len = array->len;
    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
    return 1;
}

static void SLEvalValueSetSpan(
    const SLParsedFile* file, uint32_t start, uint32_t end, SLCTFEValue* value) {
    uint32_t startLine = 0;
    uint32_t startCol = 0;
    uint32_t endLine = 0;
    uint32_t endCol = 0;
    if (file == NULL || value == NULL) {
        return;
    }
    DiagOffsetToLineCol(file->source, start, &startLine, &startCol);
    DiagOffsetToLineCol(file->source, end, &endLine, &endCol);
    value->kind = SLCTFEValue_SPAN;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = NULL;
    value->s.len = 0;
    value->span.fileBytes = (const uint8_t*)file->source;
    value->span.fileLen = file->sourceLen;
    value->span.startLine = startLine;
    value->span.startColumn = startCol;
    value->span.endLine = endLine;
    value->span.endColumn = endCol;
}

static int ErrorEvalUnsupported(
    const char* file, const char* _Nullable source, uint32_t start, uint32_t end, const char* msg) {
    const char* detail = msg;
    if (detail == NULL || detail[0] == '\0') {
        detail = "operation is not supported by evaluator backend";
    }
    return ErrorDiagf(file, source, start, end, SLDiag_EVAL_BACKEND_UNSUPPORTED, detail);
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

static int SLEvalProgramAppendTopConst(SLEvalProgram* p, const SLEvalTopConst* topConst) {
    SLEvalTopConst* newConsts;
    uint32_t        newCap;
    if (p == NULL || topConst == NULL) {
        return -1;
    }
    if (p->topConstLen >= p->topConstCap) {
        newCap = p->topConstCap < 8u ? 8u : p->topConstCap * 2u;
        newConsts = (SLEvalTopConst*)realloc(p->topConsts, sizeof(SLEvalTopConst) * newCap);
        if (newConsts == NULL) {
            return ErrorSimple("out of memory");
        }
        p->topConsts = newConsts;
        p->topConstCap = newCap;
    }
    p->topConsts[p->topConstLen++] = *topConst;
    return 0;
}

static int SLEvalProgramAppendTopVar(SLEvalProgram* p, const SLEvalTopVar* topVar) {
    SLEvalTopVar* newVars;
    uint32_t      newCap;
    if (p == NULL || topVar == NULL) {
        return -1;
    }
    if (p->topVarLen >= p->topVarCap) {
        newCap = p->topVarCap < 8u ? 8u : p->topVarCap * 2u;
        newVars = (SLEvalTopVar*)realloc(p->topVars, sizeof(SLEvalTopVar) * newCap);
        if (newVars == NULL) {
            return ErrorSimple("out of memory");
        }
        p->topVars = newVars;
        p->topVarCap = newCap;
    }
    p->topVars[p->topVarLen++] = *topVar;
    return 0;
}

static int32_t SLEvalVarLikeInitExprNodeAt(
    const SLParsedFile* file, int32_t varLikeNodeId, int32_t nameIndex) {
    int32_t firstChild;
    int32_t initNode;
    if (file == NULL || nameIndex < 0) {
        return -1;
    }
    firstChild = ASTFirstChild(&file->ast, varLikeNodeId);
    if (firstChild < 0 || (uint32_t)firstChild >= file->ast.len) {
        return -1;
    }
    initNode = VarLikeInitNode(file, varLikeNodeId);
    if (initNode < 0 || (uint32_t)initNode >= file->ast.len) {
        return -1;
    }
    if (file->ast.nodes[firstChild].kind != SLAst_NAME_LIST) {
        return nameIndex == 0 ? initNode : -1;
    }
    if (file->ast.nodes[initNode].kind != SLAst_EXPR_LIST) {
        return -1;
    }
    {
        uint32_t nameCount = AstListCount(&file->ast, firstChild);
        uint32_t initCount = AstListCount(&file->ast, initNode);
        if ((uint32_t)nameIndex >= nameCount) {
            return -1;
        }
        if (initCount == nameCount) {
            return AstListItemAt(&file->ast, initNode, (uint32_t)nameIndex);
        }
        if (initCount != 1u) {
            return -1;
        }
        {
            int32_t onlyInit = AstListItemAt(&file->ast, initNode, 0);
            if (onlyInit < 0 || (uint32_t)onlyInit >= file->ast.len
                || file->ast.nodes[onlyInit].kind != SLAst_TUPLE_EXPR)
            {
                return -1;
            }
            return AstListItemAt(&file->ast, onlyInit, (uint32_t)nameIndex);
        }
    }
}

static int32_t SLEvalVarLikeDeclTypeNode(const SLParsedFile* file, int32_t varLikeNodeId) {
    int32_t firstChild;
    int32_t afterNames;
    if (file == NULL || varLikeNodeId < 0 || (uint32_t)varLikeNodeId >= file->ast.len) {
        return -1;
    }
    firstChild = ASTFirstChild(&file->ast, varLikeNodeId);
    if (firstChild < 0 || (uint32_t)firstChild >= file->ast.len) {
        return -1;
    }
    if (file->ast.nodes[firstChild].kind == SLAst_NAME_LIST) {
        afterNames = ASTNextSibling(&file->ast, firstChild);
        if (afterNames >= 0 && IsFnReturnTypeNodeKind(file->ast.nodes[afterNames].kind)) {
            return afterNames;
        }
        return -1;
    }
    if (IsFnReturnTypeNodeKind(file->ast.nodes[firstChild].kind)) {
        return firstChild;
    }
    return -1;
}

static int SLEvalCollectTopConsts(SLEvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &p->loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const SLParsedFile* file = &pkg->files[fileIndex];
            const SLAst*        ast = &file->ast;
            int32_t             nodeId = ASTFirstChild(ast, ast->root);
            while (nodeId >= 0) {
                const SLAstNode* n = &ast->nodes[nodeId];
                if (n->kind == SLAst_CONST) {
                    int32_t firstChild = ASTFirstChild(ast, nodeId);
                    if (firstChild >= 0 && ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(ast, firstChild);
                        for (i = 0; i < nameCount; i++) {
                            int32_t          nameNode = AstListItemAt(ast, firstChild, i);
                            const SLAstNode* name = nameNode >= 0 ? &ast->nodes[nameNode] : NULL;
                            SLEvalTopConst   topConst;
                            if (name == NULL) {
                                continue;
                            }
                            memset(&topConst, 0, sizeof(topConst));
                            topConst.file = file;
                            topConst.nodeId = nodeId;
                            topConst.initExprNode = SLEvalVarLikeInitExprNodeAt(
                                file, nodeId, (int32_t)i);
                            topConst.nameStart = name->dataStart;
                            topConst.nameEnd = name->dataEnd;
                            topConst.state = SLEvalTopConstState_UNSEEN;
                            if (SLEvalProgramAppendTopConst(p, &topConst) != 0) {
                                return -1;
                            }
                        }
                    } else {
                        SLEvalTopConst topConst;
                        memset(&topConst, 0, sizeof(topConst));
                        topConst.file = file;
                        topConst.nodeId = nodeId;
                        topConst.initExprNode = SLEvalVarLikeInitExprNodeAt(file, nodeId, 0);
                        topConst.nameStart = n->dataStart;
                        topConst.nameEnd = n->dataEnd;
                        topConst.state = SLEvalTopConstState_UNSEEN;
                        if (SLEvalProgramAppendTopConst(p, &topConst) != 0) {
                            return -1;
                        }
                    }
                }
                nodeId = ASTNextSibling(ast, nodeId);
            }
        }
    }
    return 0;
}

static int SLEvalCollectTopVars(SLEvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &p->loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const SLParsedFile* file = &pkg->files[fileIndex];
            const SLAst*        ast = &file->ast;
            int32_t             nodeId = ASTFirstChild(ast, ast->root);
            while (nodeId >= 0) {
                const SLAstNode* n = &ast->nodes[nodeId];
                if (n->kind == SLAst_VAR) {
                    if (ASTFirstChild(ast, nodeId) >= 0
                        && ast->nodes[ASTFirstChild(ast, nodeId)].kind == SLAst_NAME_LIST)
                    {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(ast, ASTFirstChild(ast, nodeId));
                        for (i = 0; i < nameCount; i++) {
                            int32_t nameNode = AstListItemAt(ast, ASTFirstChild(ast, nodeId), i);
                            const SLAstNode* name = nameNode >= 0 ? &ast->nodes[nameNode] : NULL;
                            SLEvalTopVar     topVar;
                            if (name == NULL) {
                                continue;
                            }
                            memset(&topVar, 0, sizeof(topVar));
                            topVar.file = file;
                            topVar.nodeId = nodeId;
                            topVar.initExprNode = SLEvalVarLikeInitExprNodeAt(
                                file, nodeId, (int32_t)i);
                            topVar.declTypeNode = SLEvalVarLikeDeclTypeNode(file, nodeId);
                            topVar.nameStart = name->dataStart;
                            topVar.nameEnd = name->dataEnd;
                            topVar.state = SLEvalTopConstState_UNSEEN;
                            if (SLEvalProgramAppendTopVar(p, &topVar) != 0) {
                                return -1;
                            }
                        }
                    } else {
                        SLEvalTopVar topVar;
                        memset(&topVar, 0, sizeof(topVar));
                        topVar.file = file;
                        topVar.nodeId = nodeId;
                        topVar.initExprNode = SLEvalVarLikeInitExprNodeAt(file, nodeId, 0);
                        topVar.declTypeNode = SLEvalVarLikeDeclTypeNode(file, nodeId);
                        topVar.nameStart = n->dataStart;
                        topVar.nameEnd = n->dataEnd;
                        topVar.state = SLEvalTopConstState_UNSEEN;
                        if (SLEvalProgramAppendTopVar(p, &topVar) != 0) {
                            return -1;
                        }
                    }
                }
                nodeId = ASTNextSibling(ast, nodeId);
            }
        }
    }
    return 0;
}

static int32_t SLEvalFindTopConstBySlice(
    const SLEvalProgram* p, const SLParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->topConstLen; i++) {
        const SLEvalTopConst* topConst = &p->topConsts[i];
        if (SliceEqSlice(
                callerFile->source,
                nameStart,
                nameEnd,
                topConst->file->source,
                topConst->nameStart,
                topConst->nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLEvalFindTopConstBySliceInPackage(
    const SLEvalProgram* p,
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->topConstLen; i++) {
        const SLEvalTopConst* topConst = &p->topConsts[i];
        const SLPackage*      topConstPkg = SLEvalFindPackageByFile(p, topConst->file);
        if (topConstPkg != pkg) {
            continue;
        }
        if (!SliceEqSlice(
                callerFile->source,
                nameStart,
                nameEnd,
                topConst->file->source,
                topConst->nameStart,
                topConst->nameEnd))
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

static int32_t SLEvalFindTopVarBySliceInPackage(
    const SLEvalProgram* p,
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->topVarLen; i++) {
        const SLEvalTopVar* topVar = &p->topVars[i];
        const SLPackage*    topVarPkg = SLEvalFindPackageByFile(p, topVar->file);
        if (topVarPkg != pkg) {
            continue;
        }
        if (!SliceEqSlice(
                callerFile->source,
                nameStart,
                nameEnd,
                topVar->file->source,
                topVar->nameStart,
                topVar->nameEnd))
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

static int32_t SLEvalFindTopVarBySlice(
    const SLEvalProgram* p, const SLParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->topVarLen; i++) {
        const SLEvalTopVar* topVar = &p->topVars[i];
        if (SliceEqSlice(
                callerFile->source,
                nameStart,
                nameEnd,
                topVar->file->source,
                topVar->nameStart,
                topVar->nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLEvalFindCurrentTopVarBySlice(
    const SLEvalProgram* p, const SLParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    const SLPackage* currentPkg;
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = SLEvalFindPackageByFile(p, callerFile);
    return currentPkg != NULL
             ? SLEvalFindTopVarBySliceInPackage(p, currentPkg, callerFile, nameStart, nameEnd)
             : SLEvalFindTopVarBySlice(p, callerFile, nameStart, nameEnd);
}

static int SLEvalCollectFunctionsFromPackage(
    SLEvalProgram* p, const SLPackage* pkg, uint8_t isBuiltinPackageFn) {
    uint32_t fileIndex;
    if (p == NULL || pkg == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const SLParsedFile* file = &pkg->files[fileIndex];
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
                uint8_t        isVariadic = 0;

                while (child >= 0) {
                    const SLAstNode* ch = &ast->nodes[child];
                    if (ch->kind == SLAst_PARAM) {
                        paramCount++;
                        if ((ch->flags & SLAstFlag_PARAM_VARIADIC) != 0) {
                            isVariadic = 1;
                        }
                    } else if (ch->kind == SLAst_CONTEXT_CLAUSE) {
                        hasContextClause = 1;
                    } else if (IsFnReturnTypeNodeKind(ch->kind) && ch->flags == 1) {
                        hasReturnType = 1;
                    } else if (ch->kind == SLAst_BLOCK) {
                        if (bodyNode >= 0) {
                            return ErrorEvalUnsupported(
                                file->path,
                                file->source,
                                ch->start,
                                ch->end,
                                "function body shape is not supported by evaluator backend");
                        }
                        bodyNode = child;
                    }
                    child = ASTNextSibling(ast, child);
                }

                if (bodyNode >= 0) {
                    fn.file = file;
                    fn.pkg = pkg;
                    fn.fnNode = nodeId;
                    fn.bodyNode = bodyNode;
                    fn.nameStart = n->dataStart;
                    fn.nameEnd = n->dataEnd;
                    fn.paramCount = paramCount;
                    fn.hasReturnType = hasReturnType;
                    fn.hasContextClause = hasContextClause;
                    fn.isBuiltinPackageFn = isBuiltinPackageFn;
                    fn.isVariadic = isVariadic;
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

static int SLEvalCollectFunctions(SLEvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const SLPackage* pkg = &p->loader->packages[pkgIndex];
        uint8_t          isBuiltinPackageFn = StrEq(pkg->name, "builtin") ? 1u : 0u;
        if (SLEvalCollectFunctionsFromPackage(p, pkg, isBuiltinPackageFn) != 0) {
            return -1;
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

static int32_t SLEvalFindFunctionBySliceInPackage(
    const SLEvalProgram* p,
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t             argCount) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        if (fn->pkg != pkg || fn->paramCount != argCount) {
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

static int32_t SLEvalFindAnyFunctionBySliceInPackage(
    const SLEvalProgram* p,
    const SLPackage*     pkg,
    const SLParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        if (fn->pkg != pkg) {
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

static int SLEvalValueSimpleKind(
    const SLCTFEValue* value, char* outKind, uint64_t* _Nullable outAliasTag) {
    if (outKind == NULL) {
        return 0;
    }
    *outKind = '\0';
    if (outAliasTag != NULL) {
        *outAliasTag = 0;
    }
    if (value == NULL) {
        return 0;
    }
    switch (value->kind) {
        case SLCTFEValue_INT:    *outKind = 'i'; break;
        case SLCTFEValue_FLOAT:  *outKind = 'f'; break;
        case SLCTFEValue_BOOL:   *outKind = 'b'; break;
        case SLCTFEValue_STRING: *outKind = 's'; break;
        default:                 return 0;
    }
    if (outAliasTag != NULL) {
        *outAliasTag = value->typeTag;
    }
    return 1;
}

static int SLEvalClassifySimpleTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    char*                outKind,
    uint64_t* _Nullable outAliasTag);
static int SLEvalAggregateDistanceToType(
    const SLEvalProgram* p,
    const SLCTFEValue*   value,
    const SLParsedFile*  callerFile,
    int32_t              typeNode,
    uint32_t*            outDistance);

static int SLEvalTypeNodeIsAnytype(const SLParsedFile* file, int32_t typeNode) {
    const SLAstNode* n;
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    return n->kind == SLAst_TYPE_NAME
        && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "anytype");
}

static int SLEvalValueMatchesExpectedTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  typeFile,
    int32_t              typeNode,
    const SLCTFEValue*   value) {
    char               argKind = '\0';
    char               paramKind = '\0';
    uint64_t           argAliasTag = 0;
    uint64_t           paramAliasTag = 0;
    uint32_t           structDistance = 0;
    int32_t            typeCode = SLEvalTypeCode_INVALID;
    const SLCTFEValue* sourceValue = SLEvalValueTargetOrSelf(value);
    if (p == NULL || typeFile == NULL || value == NULL || typeNode < 0
        || (uint32_t)typeNode >= typeFile->ast.len)
    {
        return 0;
    }
    if (SLEvalTypeNodeIsAnytype(typeFile, typeNode)) {
        return 1;
    }
    if (SLEvalAggregateDistanceToType(p, value, typeFile, typeNode, &structDistance)) {
        return 1;
    }
    if (SLEvalValueSimpleKind(sourceValue, &argKind, &argAliasTag)
        && SLEvalClassifySimpleTypeNode(p, typeFile, typeNode, &paramKind, &paramAliasTag)
        && argKind == paramKind)
    {
        return paramAliasTag == 0 || argAliasTag == paramAliasTag;
    }
    if (!SLEvalTypeCodeFromTypeNode(typeFile, typeNode, &typeCode)) {
        return 0;
    }
    switch (typeCode) {
        case SLEvalTypeCode_BOOL:    return sourceValue->kind == SLCTFEValue_BOOL;
        case SLEvalTypeCode_F32:
        case SLEvalTypeCode_F64:     return sourceValue->kind == SLCTFEValue_FLOAT;
        case SLEvalTypeCode_U8:
        case SLEvalTypeCode_U16:
        case SLEvalTypeCode_U32:
        case SLEvalTypeCode_U64:
        case SLEvalTypeCode_UINT:
        case SLEvalTypeCode_I8:
        case SLEvalTypeCode_I16:
        case SLEvalTypeCode_I32:
        case SLEvalTypeCode_I64:
        case SLEvalTypeCode_INT:     return sourceValue->kind == SLCTFEValue_INT;
        case SLEvalTypeCode_STR_REF:
        case SLEvalTypeCode_STR_PTR: return sourceValue->kind == SLCTFEValue_STRING;
        case SLEvalTypeCode_TYPE:    return sourceValue->kind == SLCTFEValue_TYPE;
        default:                     return 0;
    }
}

static int32_t SLEvalFunctionParamTypeNodeAt(const SLEvalFunction* fn, uint32_t paramIndex) {
    const SLAst* ast;
    int32_t      child;
    uint32_t     i = 0;
    if (fn == NULL || fn->file == NULL) {
        return -1;
    }
    ast = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= ast->len) {
        return -1;
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            if (i == paramIndex) {
                return ASTFirstChild(ast, child);
            }
            i++;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static int32_t SLEvalFunctionParamIndexByName(
    const SLEvalFunction* fn, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    const SLAst* ast;
    int32_t      child;
    uint32_t     i = 0;
    if (fn == NULL || fn->file == NULL || source == NULL) {
        return -1;
    }
    ast = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= ast->len) {
        return -1;
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            if (SliceEqSlice(
                    source, nameStart, nameEnd, fn->file->source, n->dataStart, n->dataEnd))
            {
                return (int32_t)i;
            }
            i++;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static int SLEvalExprIsAnytypePackIndex(SLEvalProgram* p, const SLAst* ast, int32_t exprNode) {
    int32_t             baseNode;
    int32_t             idxNode;
    int32_t             extraNode;
    SLCTFEExecBinding*  binding;
    const SLCTFEValue*  bindingValue;
    const SLParsedFile* localTypeFile = NULL;
    int32_t             localTypeNode = -1;
    SLCTFEValue         localValue;
    while (ast != NULL && exprNode >= 0 && (uint32_t)exprNode < ast->len
           && ast->nodes[exprNode].kind == SLAst_CALL_ARG)
    {
        exprNode = ast->nodes[exprNode].firstChild;
    }
    if (p == NULL || p->currentFile == NULL || p->currentExecCtx == NULL || ast == NULL
        || exprNode < 0 || (uint32_t)exprNode >= ast->len
        || ast->nodes[exprNode].kind != SLAst_INDEX
        || (ast->nodes[exprNode].flags & SLAstFlag_INDEX_SLICE) != 0u)
    {
        return 0;
    }
    baseNode = ast->nodes[exprNode].firstChild;
    idxNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
    extraNode = idxNode >= 0 ? ast->nodes[idxNode].nextSibling : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0 || ast->nodes[baseNode].kind != SLAst_IDENT) {
        return 0;
    }
    binding = SLEvalFindBinding(
        p->currentExecCtx,
        p->currentFile,
        ast->nodes[baseNode].dataStart,
        ast->nodes[baseNode].dataEnd);
    if (binding != NULL && SLEvalTypeNodeIsAnytype(p->currentFile, binding->typeNode)) {
        bindingValue = SLEvalValueTargetOrSelf(&binding->value);
        return bindingValue->kind == SLCTFEValue_ARRAY;
    }
    if (!SLEvalMirLookupLocalTypeNode(
            p,
            ast->nodes[baseNode].dataStart,
            ast->nodes[baseNode].dataEnd,
            &localTypeFile,
            &localTypeNode)
        || localTypeFile == NULL || localTypeNode < 0
        || !SLEvalTypeNodeIsAnytype(localTypeFile, localTypeNode)
        || !SLEvalMirLookupLocalValue(
            p, ast->nodes[baseNode].dataStart, ast->nodes[baseNode].dataEnd, &localValue))
    {
        return 0;
    }
    bindingValue = SLEvalValueTargetOrSelf(&localValue);
    return bindingValue->kind == SLCTFEValue_ARRAY;
}

static int SLEvalReorderFixedCallArgsByName(
    SLEvalProgram*        p,
    const SLEvalFunction* fn,
    const SLAst*          callAst,
    int32_t               firstArgNode,
    SLCTFEValue*          args,
    uint32_t              argCount,
    uint32_t              paramOffset) {
    uint32_t     argNameStarts[256];
    uint32_t     argNameEnds[256];
    uint32_t     paramNameStarts[256];
    uint32_t     paramNameEnds[256];
    uint8_t      paramAssigned[256];
    SLCTFEValue  reorderedArgs[256];
    const SLAst* fnAst;
    const char*  callSource;
    int32_t      child;
    int32_t      argNode;
    uint32_t     i = 0;
    int          reordered = 0;

    if (p == NULL || fn == NULL || callAst == NULL || args == NULL || fn->file == NULL) {
        return 0;
    }
    if (argCount == 0 || argCount > 256u || fn->isVariadic
        || fn->paramCount != paramOffset + argCount)
    {
        return 0;
    }
    callSource = p->currentFile != NULL ? p->currentFile->source : NULL;
    if (callSource == NULL) {
        return 0;
    }

    memset(argNameStarts, 0, sizeof(argNameStarts));
    memset(argNameEnds, 0, sizeof(argNameEnds));
    argNode = firstArgNode;
    while (argNode >= 0) {
        const SLAstNode* arg = &callAst->nodes[argNode];
        int32_t          exprNode = argNode;
        if (i >= argCount) {
            return 0;
        }
        if (arg->kind == SLAst_CALL_ARG) {
            if ((arg->flags & SLAstFlag_CALL_ARG_SPREAD) != 0) {
                return 0;
            }
            exprNode = arg->firstChild;
            if (arg->dataEnd > arg->dataStart) {
                argNameStarts[i] = arg->dataStart;
                argNameEnds[i] = arg->dataEnd;
            }
        }
        if (exprNode < 0 || (uint32_t)exprNode >= callAst->len) {
            return 0;
        }
        if (argNameEnds[i] <= argNameStarts[i] && callAst->nodes[exprNode].kind == SLAst_IDENT) {
            SLCTFEValue ignoredTypeValue;
            if (SLEvalResolveTypeValueName(
                    p,
                    p->currentFile,
                    callAst->nodes[exprNode].dataStart,
                    callAst->nodes[exprNode].dataEnd,
                    &ignoredTypeValue)
                <= 0)
            {
                argNameStarts[i] = callAst->nodes[exprNode].dataStart;
                argNameEnds[i] = callAst->nodes[exprNode].dataEnd;
            }
        }
        if (argNameEnds[i] <= argNameStarts[i]) {
            return 0;
        }
        i++;
        argNode = ASTNextSibling(callAst, argNode);
    }
    if (i != argCount) {
        return 0;
    }

    fnAst = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= fnAst->len) {
        return 0;
    }
    memset(paramNameStarts, 0, sizeof(paramNameStarts));
    memset(paramNameEnds, 0, sizeof(paramNameEnds));
    memset(paramAssigned, 0, sizeof(paramAssigned));
    child = ASTFirstChild(fnAst, fn->fnNode);
    i = 0;
    while (child >= 0) {
        const SLAstNode* n = &fnAst->nodes[child];
        if (n->kind == SLAst_PARAM) {
            if (i < paramOffset) {
                i++;
                child = ASTNextSibling(fnAst, child);
                continue;
            }
            if (i - paramOffset >= argCount) {
                return 0;
            }
            paramNameStarts[i - paramOffset] = n->dataStart;
            paramNameEnds[i - paramOffset] = n->dataEnd;
            i++;
        }
        child = ASTNextSibling(fnAst, child);
    }
    if (i != paramOffset + argCount) {
        return 0;
    }

    for (i = 0; i < argCount; i++) {
        uint32_t j;
        uint32_t matchIndex = UINT32_MAX;
        for (j = 0; j < argCount; j++) {
            if (paramAssigned[j]) {
                continue;
            }
            if (SliceEqSlice(
                    callSource,
                    argNameStarts[i],
                    argNameEnds[i],
                    fn->file->source,
                    paramNameStarts[j],
                    paramNameEnds[j]))
            {
                matchIndex = j;
                break;
            }
        }
        if (matchIndex == UINT32_MAX) {
            return 0;
        }
        reorderedArgs[matchIndex] = args[i];
        paramAssigned[matchIndex] = 1;
        if (matchIndex != i) {
            reordered = 1;
        }
    }
    if (!reordered) {
        return 1;
    }
    memcpy(args, reorderedArgs, sizeof(SLCTFEValue) * argCount);
    return 1;
}

static int32_t SLEvalFunctionReturnTypeNode(const SLEvalFunction* fn) {
    const SLAst* ast;
    int32_t      child;
    if (fn == NULL || fn->file == NULL) {
        return -1;
    }
    ast = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= ast->len) {
        return -1;
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (IsFnReturnTypeNodeKind(n->kind)) {
            return child;
        }
        if (n->kind == SLAst_BLOCK) {
            break;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static int SLEvalClassifySimpleTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    char*                outKind,
    uint64_t* _Nullable outAliasTag) {
    const SLAstNode* n;
    char             aliasKind = '\0';
    uint64_t         aliasTag = 0;
    if (outKind == NULL) {
        return 0;
    }
    *outKind = '\0';
    if (outAliasTag != NULL) {
        *outAliasTag = 0;
    }
    if (p == NULL || file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind != SLAst_TYPE_NAME) {
        return 0;
    }
    if (SLEvalResolveSimpleAliasCastTarget(p, file, typeNode, &aliasKind, &aliasTag)) {
        *outKind = aliasKind;
        if (outAliasTag != NULL) {
            *outAliasTag = aliasTag;
        }
        return 1;
    }
    if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "bool")) {
        *outKind = 'b';
        return 1;
    }
    if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "f32")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "f64"))
    {
        *outKind = 'f';
        return 1;
    }
    if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "string")) {
        *outKind = 's';
        return 1;
    }
    if (SliceEqCStr(file->source, n->dataStart, n->dataEnd, "u8")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "u16")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "u32")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "u64")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "uint")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "i8")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "i16")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "i32")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "i64")
        || SliceEqCStr(file->source, n->dataStart, n->dataEnd, "int"))
    {
        *outKind = 'i';
        return 1;
    }
    return 0;
}

static int SLEvalStructEmbeddedBase(
    const SLEvalProgram* p,
    const SLParsedFile*  structFile,
    int32_t              structNode,
    const SLParsedFile** outBaseFile,
    int32_t*             outBaseNode) {
    int32_t          child;
    const SLPackage* pkg;
    if (outBaseFile != NULL) {
        *outBaseFile = NULL;
    }
    if (outBaseNode != NULL) {
        *outBaseNode = -1;
    }
    if (p == NULL || structFile == NULL || structNode < 0
        || (uint32_t)structNode >= structFile->ast.len)
    {
        return 0;
    }
    pkg = SLEvalFindPackageByFile(p, structFile);
    if (pkg == NULL) {
        return 0;
    }
    child = ASTFirstChild(&structFile->ast, structNode);
    while (child >= 0) {
        const SLAstNode* fieldNode = &structFile->ast.nodes[child];
        if (fieldNode->kind == SLAst_FIELD && (fieldNode->flags & SLAstFlag_FIELD_EMBEDDED) != 0) {
            int32_t             typeNode = ASTFirstChild(&structFile->ast, child);
            const SLParsedFile* baseFile = NULL;
            int32_t baseNode = SLEvalFindNamedAggregateDecl(p, structFile, typeNode, &baseFile);
            if (baseNode >= 0 && baseFile != NULL) {
                if (outBaseFile != NULL) {
                    *outBaseFile = baseFile;
                }
                if (outBaseNode != NULL) {
                    *outBaseNode = baseNode;
                }
                return 1;
            }
            return 0;
        }
        child = ASTNextSibling(&structFile->ast, child);
    }
    return 0;
}

static int SLEvalAggregateDistanceToType(
    const SLEvalProgram* p,
    const SLCTFEValue*   value,
    const SLParsedFile*  callerFile,
    int32_t              typeNode,
    uint32_t*            outDistance) {
    const SLPackage*    pkg;
    const SLParsedFile* targetFile = NULL;
    int32_t             targetNode = -1;
    const SLParsedFile* curFile = NULL;
    int32_t             curNode = -1;
    uint32_t            distance = 0;
    if (outDistance != NULL) {
        *outDistance = 0;
    }
    if (p == NULL || callerFile == NULL) {
        return 0;
    }
    pkg = SLEvalFindPackageByFile(p, callerFile);
    if (pkg == NULL) {
        return 0;
    }
    (void)pkg;
    if (!SLEvalResolveAggregateTypeNode(p, callerFile, typeNode, &targetFile, &targetNode)) {
        return 0;
    }
    if (!SLEvalResolveAggregateDeclFromValue(p, value, &curFile, &curNode)) {
        return 0;
    }
    while (curFile != NULL && curNode >= 0) {
        if (curFile == targetFile && curNode == targetNode) {
            if (outDistance != NULL) {
                *outDistance = distance;
            }
            return 1;
        }
        if (!SLEvalStructEmbeddedBase(p, curFile, curNode, &curFile, &curNode)) {
            break;
        }
        distance++;
    }
    return 0;
}

static int SLEvalScoreFunctionCandidate(
    const SLEvalProgram*  p,
    const SLEvalFunction* fn,
    const SLCTFEValue*    args,
    uint32_t              argCount,
    int*                  outScore) {
    uint32_t i;
    int      score = 0;
    uint32_t fixedCount;
    if (outScore == NULL) {
        return 0;
    }
    *outScore = 0;
    if (p == NULL || fn == NULL || args == NULL) {
        return 0;
    }
    fixedCount = fn->isVariadic && fn->paramCount > 0 ? fn->paramCount - 1u : fn->paramCount;
    if ((!fn->isVariadic && fn->paramCount != argCount)
        || (fn->isVariadic && argCount < fixedCount))
    {
        return 0;
    }
    for (i = 0; i < argCount; i++) {
        char     argKind = '\0';
        char     paramKind = '\0';
        uint64_t argAliasTag = 0;
        uint64_t paramAliasTag = 0;
        uint32_t structDistance = 0;
        int32_t  paramTypeNode = SLEvalFunctionParamTypeNodeAt(
            fn, fn->isVariadic && i >= fixedCount ? fixedCount : i);
        if (paramTypeNode < 0) {
            return 0;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind == SLAst_TYPE_NAME
            && SliceEqCStr(
                fn->file->source,
                fn->file->ast.nodes[paramTypeNode].dataStart,
                fn->file->ast.nodes[paramTypeNode].dataEnd,
                "anytype"))
        {
            continue;
        }
        if (SLEvalAggregateDistanceToType(p, &args[i], fn->file, paramTypeNode, &structDistance)) {
            score += structDistance == 0 ? 16 : (int)(16u - structDistance);
            continue;
        }
        if (!SLEvalValueSimpleKind(&args[i], &argKind, &argAliasTag)
            || !SLEvalClassifySimpleTypeNode(p, fn->file, paramTypeNode, &paramKind, &paramAliasTag)
            || argKind != paramKind)
        {
            return 0;
        }
        if (paramAliasTag != 0) {
            if (argAliasTag != paramAliasTag) {
                return 0;
            }
            score += 4;
        } else if (argAliasTag != 0) {
            score += 1;
        } else {
            score += 2;
        }
    }
    *outScore = score;
    return 1;
}

static int32_t SLEvalResolveFunctionBySlice(
    const SLEvalProgram* p,
    const SLPackage* _Nullable targetPkg,
    const SLParsedFile* callerFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const SLCTFEValue* _Nullable args,
    uint32_t argCount) {
    uint32_t i;
    int32_t  found = -1;
    int32_t  best = -1;
    int      bestScore = -1;
    int      ambiguous = 0;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        int                   score = 0;
        uint32_t              fixedCount =
            fn->isVariadic && fn->paramCount > 0 ? fn->paramCount - 1u : fn->paramCount;
        if ((!fn->isVariadic && fn->paramCount != argCount)
            || (fn->isVariadic && argCount < fixedCount))
        {
            continue;
        }
        if (targetPkg != NULL && fn->pkg != targetPkg) {
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
        if (found < 0) {
            found = (int32_t)i;
        } else {
            found = -2;
        }
        if (args != NULL && SLEvalScoreFunctionCandidate(p, fn, args, argCount, &score)) {
            if (score > bestScore) {
                best = (int32_t)i;
                bestScore = score;
                ambiguous = 0;
            } else if (score == bestScore) {
                ambiguous = 1;
            }
        }
    }
    if (best >= 0) {
        return ambiguous ? -2 : best;
    }
    return found;
}

static int32_t SLEvalFindAnyFunctionBySlice(
    const SLEvalProgram* p, const SLParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
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

static int SLEvalExprContainsFieldExpr(const SLAst* ast, int32_t nodeId) {
    int32_t child;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[nodeId].kind == SLAst_FIELD_EXPR) {
        return 1;
    }
    child = ast->nodes[nodeId].firstChild;
    while (child >= 0) {
        if (SLEvalExprContainsFieldExpr(ast, child)) {
            return 1;
        }
        child = ast->nodes[child].nextSibling;
    }
    return 0;
}

static int SLEvalEvalUnary(
    SLTokenKind op, const SLCTFEValue* inValue, SLCTFEValue* outValue, int* outIsConst) {
    if (outValue == NULL || outIsConst == NULL || inValue == NULL) {
        return -1;
    }
    switch (op) {
        case SLTok_NOT:
            if (inValue->kind != SLCTFEValue_BOOL) {
                return 0;
            }
            outValue->kind = SLCTFEValue_BOOL;
            outValue->b = inValue->b ? 0u : 1u;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            *outIsConst = 1;
            return 1;
        case SLTok_SUB:
            if (inValue->kind == SLCTFEValue_INT) {
                SLEvalValueSetInt(outValue, -inValue->i64);
                *outIsConst = 1;
                return 1;
            }
            if (inValue->kind == SLCTFEValue_FLOAT) {
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 = -inValue->f64;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 1;
            }
            return 0;
        default: return 0;
    }
}

static int SLEvalLexCompareStrings(const SLCTFEValue* lhs, const SLCTFEValue* rhs, int* outCmp) {
    uint32_t minLen;
    int      cmp = 0;
    if (lhs == NULL || rhs == NULL || outCmp == NULL) {
        return -1;
    }
    minLen = lhs->s.len < rhs->s.len ? lhs->s.len : rhs->s.len;
    if (minLen > 0 && lhs->s.bytes != NULL && rhs->s.bytes != NULL) {
        cmp = memcmp(lhs->s.bytes, rhs->s.bytes, minLen);
    }
    if (cmp == 0) {
        if (lhs->s.len < rhs->s.len) {
            cmp = -1;
        } else if (lhs->s.len > rhs->s.len) {
            cmp = 1;
        }
    }
    *outCmp = cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
    return 0;
}

static int32_t SLEvalResolveFunctionByLiteralArgs(
    const SLEvalProgram* p, const char* name, const SLCTFEValue* args, uint32_t argCount) {
    uint32_t i;
    int32_t  best = -1;
    int      bestScore = -1;
    int      ambiguous = 0;
    if (p == NULL || name == NULL) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        int                   score = 0;
        uint32_t              fixedCount =
            fn->isVariadic && fn->paramCount > 0 ? fn->paramCount - 1u : fn->paramCount;
        if ((!fn->isVariadic && fn->paramCount != argCount)
            || (fn->isVariadic && argCount < fixedCount))
        {
            continue;
        }
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)) {
            continue;
        }
        if (args != NULL && SLEvalScoreFunctionCandidate(p, fn, args, argCount, &score)) {
            if (score > bestScore) {
                best = (int32_t)i;
                bestScore = score;
                ambiguous = 0;
            } else if (score == bestScore) {
                ambiguous = 1;
            }
        } else if (args == NULL && best < 0) {
            best = (int32_t)i;
        }
    }
    return ambiguous ? -2 : best;
}

static int SLEvalCompareValues(
    SLEvalProgram* p, const SLCTFEValue* lhs, const SLCTFEValue* rhs, int* outCmp, int* outHandled);

static int SLEvalTaggedEnumPayloadEqual(
    SLEvalProgram* p, const SLEvalTaggedEnum* lhs, const SLEvalTaggedEnum* rhs, int* outEqual) {
    if (outEqual != NULL) {
        *outEqual = 0;
    }
    if (lhs == NULL || rhs == NULL || outEqual == NULL) {
        return -1;
    }
    if (lhs->tagIndex != rhs->tagIndex) {
        *outEqual = 0;
        return 0;
    }
    if (lhs->payload == NULL || rhs->payload == NULL) {
        *outEqual = 1;
        return 0;
    }
    if (lhs->payload->fieldLen != rhs->payload->fieldLen) {
        *outEqual = 0;
        return 0;
    }
    {
        uint32_t i;
        for (i = 0; i < lhs->payload->fieldLen; i++) {
            int cmp = 0;
            int handled = 0;
            if (SLEvalCompareValues(
                    p,
                    &lhs->payload->fields[i].value,
                    &rhs->payload->fields[i].value,
                    &cmp,
                    &handled)
                != 0)
            {
                return -1;
            }
            if (!handled || cmp != 0) {
                *outEqual = 0;
                return 0;
            }
        }
    }
    *outEqual = 1;
    return 0;
}

static int SLEvalCompareValues(
    SLEvalProgram*     p,
    const SLCTFEValue* lhs,
    const SLCTFEValue* rhs,
    int*               outCmp,
    int*               outHandled) {
    const SLCTFEValue* lhsValue = SLEvalValueTargetOrSelf(lhs);
    const SLCTFEValue* rhsValue = SLEvalValueTargetOrSelf(rhs);
    if (outCmp != NULL) {
        *outCmp = 0;
    }
    if (outHandled != NULL) {
        *outHandled = 0;
    }
    if (lhs == NULL || rhs == NULL || outCmp == NULL || outHandled == NULL) {
        return -1;
    }
    if (lhs->kind == SLCTFEValue_REFERENCE && rhs->kind == SLCTFEValue_REFERENCE) {
        uintptr_t la = (uintptr_t)lhs->s.bytes;
        uintptr_t ra = (uintptr_t)rhs->s.bytes;
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == SLCTFEValue_INT && rhsValue->kind == SLCTFEValue_INT) {
        *outCmp = lhsValue->i64 < rhsValue->i64 ? -1 : (lhsValue->i64 > rhsValue->i64 ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == SLCTFEValue_FLOAT && rhsValue->kind == SLCTFEValue_FLOAT) {
        *outCmp = lhsValue->f64 < rhsValue->f64 ? -1 : (lhsValue->f64 > rhsValue->f64 ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == SLCTFEValue_BOOL && rhsValue->kind == SLCTFEValue_BOOL) {
        *outCmp = lhsValue->b < rhsValue->b ? -1 : (lhsValue->b > rhsValue->b ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == SLCTFEValue_STRING && rhsValue->kind == SLCTFEValue_STRING) {
        if (SLEvalLexCompareStrings(lhsValue, rhsValue, outCmp) != 0) {
            return -1;
        }
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == SLCTFEValue_TYPE && rhsValue->kind == SLCTFEValue_TYPE) {
        int32_t lhsTypeCode = 0;
        int32_t rhsTypeCode = 0;
        if (SLEvalValueGetSimpleTypeCode(lhsValue, &lhsTypeCode)
            && SLEvalValueGetSimpleTypeCode(rhsValue, &rhsTypeCode))
        {
            *outCmp = lhsTypeCode < rhsTypeCode ? -1 : (lhsTypeCode > rhsTypeCode ? 1 : 0);
            *outHandled = 1;
            return 0;
        }
    }
    {
        SLEvalTaggedEnum* lhsTagged = SLEvalValueAsTaggedEnum(lhsValue);
        SLEvalTaggedEnum* rhsTagged = SLEvalValueAsTaggedEnum(rhsValue);
        if (lhsTagged != NULL && rhsTagged != NULL && lhsTagged->file == rhsTagged->file
            && lhsTagged->enumNode == rhsTagged->enumNode)
        {
            int equal = 0;
            if (SLEvalTaggedEnumPayloadEqual(p, lhsTagged, rhsTagged, &equal) != 0) {
                return -1;
            }
            *outCmp = equal ? 0 : 1;
            *outHandled = 1;
            return 0;
        }
    }
    {
        SLEvalArray* lhsArray = SLEvalValueAsArray(lhsValue);
        SLEvalArray* rhsArray = SLEvalValueAsArray(rhsValue);
        if (lhsArray != NULL && rhsArray != NULL) {
            uint32_t i;
            if (lhsArray->len != rhsArray->len) {
                *outCmp = lhsArray->len < rhsArray->len ? -1 : 1;
                *outHandled = 1;
                return 0;
            }
            for (i = 0; i < lhsArray->len; i++) {
                int cmp = 0;
                int handled = 0;
                if (SLEvalCompareValues(p, &lhsArray->elems[i], &rhsArray->elems[i], &cmp, &handled)
                    != 0)
                {
                    return -1;
                }
                if (!handled || cmp != 0) {
                    *outCmp = cmp != 0 ? cmp : 1;
                    *outHandled = 1;
                    return 0;
                }
            }
            *outCmp = 0;
            *outHandled = 1;
            return 0;
        }
    }
    {
        SLEvalAggregate* lhsAgg = SLEvalValueAsAggregate(lhsValue);
        SLEvalAggregate* rhsAgg = SLEvalValueAsAggregate(rhsValue);
        if (lhsAgg != NULL && rhsAgg != NULL) {
            SLCTFEValue args[2];
            args[0] = *lhsValue;
            args[1] = *rhsValue;
            {
                int32_t hookIndex = SLEvalResolveFunctionByLiteralArgs(p, "__order", args, 2);
                if (hookIndex >= 0) {
                    SLCTFEValue hookValue;
                    int         didReturn = 0;
                    int64_t     order = 0;
                    if (SLEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && SLCTFEValueToInt64(&hookValue, &order) == 0) {
                        *outCmp = order < 0 ? -1 : (order > 0 ? 1 : 0);
                        *outHandled = 1;
                        return 0;
                    }
                }
            }
            {
                int32_t hookIndex = SLEvalResolveFunctionByLiteralArgs(p, "__equal", args, 2);
                if (hookIndex >= 0) {
                    SLCTFEValue hookValue;
                    int         didReturn = 0;
                    if (SLEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && hookValue.kind == SLCTFEValue_BOOL) {
                        *outCmp = hookValue.b ? 0 : 1;
                        *outHandled = 1;
                        return 0;
                    }
                }
            }
            {
                uint32_t i;
                if (lhsAgg->fieldLen != rhsAgg->fieldLen) {
                    *outCmp = lhsAgg->fieldLen < rhsAgg->fieldLen ? -1 : 1;
                    *outHandled = 1;
                    return 0;
                }
                for (i = 0; i < lhsAgg->fieldLen; i++) {
                    int cmp = 0;
                    int handled = 0;
                    if (SLEvalCompareValues(
                            p, &lhsAgg->fields[i].value, &rhsAgg->fields[i].value, &cmp, &handled)
                        != 0)
                    {
                        return -1;
                    }
                    if (!handled || cmp != 0) {
                        *outCmp = cmp != 0 ? cmp : 1;
                        *outHandled = 1;
                        return 0;
                    }
                }
                *outCmp = 0;
                *outHandled = 1;
                return 0;
            }
        }
    }
    return 0;
}

static int SLEvalCompareOptionalEq(
    SLEvalProgram*     p,
    const SLCTFEValue* lhs,
    const SLCTFEValue* rhs,
    uint8_t*           outEqual,
    int*               outHandled) {
    const SLCTFEValue* lhsValue = SLEvalValueTargetOrSelf(lhs);
    const SLCTFEValue* rhsValue = SLEvalValueTargetOrSelf(rhs);
    const SLCTFEValue* lhsPayload = NULL;
    const SLCTFEValue* rhsPayload = NULL;
    int                lhsIsOptional = 0;
    int                rhsIsOptional = 0;
    if (outEqual != NULL) {
        *outEqual = 0;
    }
    if (outHandled != NULL) {
        *outHandled = 0;
    }
    if (lhs == NULL || rhs == NULL || outEqual == NULL || outHandled == NULL) {
        return -1;
    }
    lhsIsOptional =
        lhsValue->kind == SLCTFEValue_OPTIONAL && SLEvalOptionalPayload(lhsValue, &lhsPayload);
    rhsIsOptional =
        rhsValue->kind == SLCTFEValue_OPTIONAL && SLEvalOptionalPayload(rhsValue, &rhsPayload);
    if (!lhsIsOptional && !rhsIsOptional) {
        return 0;
    }
    if (lhsIsOptional && rhsIsOptional) {
        if (lhsValue->b == 0u || lhsPayload == NULL) {
            *outEqual = (rhsValue->b == 0u || rhsPayload == NULL) ? 1u : 0u;
            *outHandled = 1;
            return 0;
        }
        if (rhsValue->b == 0u || rhsPayload == NULL) {
            *outEqual = 0u;
            *outHandled = 1;
            return 0;
        }
        {
            int cmp = 0;
            int handled = 0;
            if (SLEvalCompareValues(p, lhsPayload, rhsPayload, &cmp, &handled) != 0) {
                return -1;
            }
            if (handled) {
                *outEqual = cmp == 0 ? 1u : 0u;
                *outHandled = 1;
            }
            return 0;
        }
    }
    if (lhsIsOptional) {
        if (lhsValue->b == 0u || lhsPayload == NULL) {
            *outEqual = rhsValue->kind == SLCTFEValue_NULL ? 1u : 0u;
            *outHandled = 1;
            return 0;
        }
        if (rhsValue->kind == SLCTFEValue_NULL) {
            *outEqual = 0u;
            *outHandled = 1;
            return 0;
        }
        {
            int cmp = 0;
            int handled = 0;
            if (SLEvalCompareValues(p, lhsPayload, rhsValue, &cmp, &handled) != 0) {
                return -1;
            }
            if (handled) {
                *outEqual = cmp == 0 ? 1u : 0u;
                *outHandled = 1;
            }
            return 0;
        }
    }
    if (rhsValue->b == 0u || rhsPayload == NULL) {
        *outEqual = lhsValue->kind == SLCTFEValue_NULL ? 1u : 0u;
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == SLCTFEValue_NULL) {
        *outEqual = 0u;
        *outHandled = 1;
        return 0;
    }
    {
        int cmp = 0;
        int handled = 0;
        if (SLEvalCompareValues(p, lhsValue, rhsPayload, &cmp, &handled) != 0) {
            return -1;
        }
        if (handled) {
            *outEqual = cmp == 0 ? 1u : 0u;
            *outHandled = 1;
        }
    }
    return 0;
}

static int SLEvalEvalBinary(
    SLEvalProgram*     p,
    SLTokenKind        op,
    const SLCTFEValue* lhs,
    const SLCTFEValue* rhs,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    int cmp = 0;
    int handled = 0;
    if (lhs == NULL || rhs == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if ((op == SLTok_EQ || op == SLTok_NEQ || op == SLTok_LT || op == SLTok_LTE || op == SLTok_GT
         || op == SLTok_GTE))
    {
        SLEvalAggregate* lhsAgg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(lhs));
        SLEvalAggregate* rhsAgg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(rhs));
        if (p != NULL && lhsAgg != NULL && rhsAgg != NULL) {
            SLCTFEValue args[2];
            int32_t     hookIndex = -1;
            SLCTFEValue hookValue;
            int         didReturn = 0;
            args[0] = *SLEvalValueTargetOrSelf(lhs);
            args[1] = *SLEvalValueTargetOrSelf(rhs);
            if (op == SLTok_EQ || op == SLTok_NEQ) {
                hookIndex = SLEvalResolveFunctionByLiteralArgs(p, "__equal", args, 2);
                if (hookIndex >= 0) {
                    if (SLEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && hookValue.kind == SLCTFEValue_BOOL) {
                        outValue->kind = SLCTFEValue_BOOL;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->typeTag = 0;
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->b = op == SLTok_EQ ? hookValue.b : (uint8_t)!hookValue.b;
                        *outIsConst = 1;
                        return 1;
                    }
                }
            } else {
                hookIndex = SLEvalResolveFunctionByLiteralArgs(p, "__order", args, 2);
                if (hookIndex >= 0) {
                    int64_t order = 0;
                    if (SLEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && SLCTFEValueToInt64(&hookValue, &order) == 0) {
                        outValue->kind = SLCTFEValue_BOOL;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->typeTag = 0;
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->b =
                            op == SLTok_LT    ? order < 0
                            : op == SLTok_LTE ? order <= 0
                            : op == SLTok_GT
                                ? order > 0
                                : order >= 0;
                        *outIsConst = 1;
                        return 1;
                    }
                }
            }
        }
    }
    if (op == SLTok_EQ || op == SLTok_NEQ || op == SLTok_LT || op == SLTok_LTE || op == SLTok_GT
        || op == SLTok_GTE)
    {
        SLEvalTaggedEnum* lhsTagged = SLEvalValueAsTaggedEnum(SLEvalValueTargetOrSelf(lhs));
        SLEvalTaggedEnum* rhsTagged = SLEvalValueAsTaggedEnum(SLEvalValueTargetOrSelf(rhs));
        if ((op == SLTok_LT || op == SLTok_LTE || op == SLTok_GT || op == SLTok_GTE)
            && lhsTagged != NULL && rhsTagged != NULL && lhsTagged->file == rhsTagged->file
            && lhsTagged->enumNode == rhsTagged->enumNode)
        {
            outValue->kind = SLCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            switch (op) {
                case SLTok_LT:  outValue->b = lhsTagged->tagIndex < rhsTagged->tagIndex; break;
                case SLTok_LTE: outValue->b = lhsTagged->tagIndex <= rhsTagged->tagIndex; break;
                case SLTok_GT:  outValue->b = lhsTagged->tagIndex > rhsTagged->tagIndex; break;
                case SLTok_GTE: outValue->b = lhsTagged->tagIndex >= rhsTagged->tagIndex; break;
                default:        outValue->b = 0; break;
            }
            *outIsConst = 1;
            return 1;
        }
        if (op == SLTok_EQ || op == SLTok_NEQ) {
            uint8_t equal = 0;
            if (SLEvalCompareOptionalEq(p, lhs, rhs, &equal, &handled) != 0) {
                return -1;
            }
            if (handled) {
                outValue->kind = SLCTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->b = op == SLTok_EQ ? equal : (uint8_t)!equal;
                *outIsConst = 1;
                return 1;
            }
        }
        if (SLEvalCompareValues(p, lhs, rhs, &cmp, &handled) != 0) {
            return -1;
        }
        if (handled) {
            outValue->kind = SLCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            switch (op) {
                case SLTok_EQ:  outValue->b = cmp == 0; break;
                case SLTok_NEQ: outValue->b = cmp != 0; break;
                case SLTok_LT:  outValue->b = cmp < 0; break;
                case SLTok_LTE: outValue->b = cmp <= 0; break;
                case SLTok_GT:  outValue->b = cmp > 0; break;
                case SLTok_GTE: outValue->b = cmp >= 0; break;
                default:        break;
            }
            *outIsConst = 1;
            return 1;
        }
    }
    if (lhs->kind == SLCTFEValue_INT && rhs->kind == SLCTFEValue_INT) {
        switch (op) {
            case SLTok_ADD: SLEvalValueSetInt(outValue, lhs->i64 + rhs->i64); break;
            case SLTok_SUB: SLEvalValueSetInt(outValue, lhs->i64 - rhs->i64); break;
            case SLTok_MUL: SLEvalValueSetInt(outValue, lhs->i64 * rhs->i64); break;
            case SLTok_DIV:
                if (rhs->i64 == 0) {
                    return 0;
                }
                SLEvalValueSetInt(outValue, lhs->i64 / rhs->i64);
                break;
            case SLTok_MOD:
                if (rhs->i64 == 0) {
                    return 0;
                }
                SLEvalValueSetInt(outValue, lhs->i64 % rhs->i64);
                break;
            default: return 0;
        }
        *outIsConst = 1;
        return 1;
    }
    if (lhs->kind == SLCTFEValue_BOOL && rhs->kind == SLCTFEValue_BOOL) {
        switch (op) {
            case SLTok_AND:
            case SLTok_OR:
            case SLTok_LOGICAL_AND:
            case SLTok_LOGICAL_OR:
                outValue->kind = SLCTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (op == SLTok_AND || op == SLTok_LOGICAL_AND) {
                    outValue->b = lhs->b && rhs->b;
                } else {
                    outValue->b = lhs->b || rhs->b;
                }
                *outIsConst = 1;
                return 1;
            default: return 0;
        }
    }
    if (lhs->kind == SLCTFEValue_FLOAT && rhs->kind == SLCTFEValue_FLOAT) {
        switch (op) {
            case SLTok_ADD:
            case SLTok_SUB:
            case SLTok_MUL:
            case SLTok_DIV:
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 =
                    op == SLTok_ADD   ? lhs->f64 + rhs->f64
                    : op == SLTok_SUB ? lhs->f64 - rhs->f64
                    : op == SLTok_MUL
                        ? lhs->f64 * rhs->f64
                        : lhs->f64 / rhs->f64;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 1;
            default: return 0;
        }
    }
    if (lhs->kind == SLCTFEValue_NULL && rhs->kind == SLCTFEValue_NULL) {
        if (op == SLTok_EQ || op == SLTok_NEQ) {
            outValue->kind = SLCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = op == SLTok_EQ ? 1u : 0u;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            *outIsConst = 1;
            return 1;
        }
        return 0;
    }
    return 0;
}

static int SLEvalResolveAggregateTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  typeFile,
    int32_t              typeNode,
    const SLParsedFile** outDeclFile,
    int32_t*             outDeclNode) {
    const SLAstNode*    n;
    const SLParsedFile* declFile = NULL;
    int32_t             declNode = -1;
    if (outDeclFile != NULL) {
        *outDeclFile = NULL;
    }
    if (outDeclNode != NULL) {
        *outDeclNode = -1;
    }
    if (p == NULL || typeFile == NULL || typeNode < 0 || (uint32_t)typeNode >= typeFile->ast.len) {
        return 0;
    }
    n = &typeFile->ast.nodes[typeNode];
    while (n->kind == SLAst_TYPE_REF || n->kind == SLAst_TYPE_MUTREF) {
        typeNode = n->firstChild;
        if (typeNode < 0 || (uint32_t)typeNode >= typeFile->ast.len) {
            return 0;
        }
        n = &typeFile->ast.nodes[typeNode];
    }
    if (n->kind == SLAst_TYPE_ANON_STRUCT || n->kind == SLAst_TYPE_ANON_UNION) {
        if (outDeclFile != NULL) {
            *outDeclFile = typeFile;
        }
        if (outDeclNode != NULL) {
            *outDeclNode = typeNode;
        }
        return 1;
    }
    if (n->kind != SLAst_TYPE_NAME) {
        return 0;
    }
    declNode = SLEvalFindNamedAggregateDecl(p, typeFile, typeNode, &declFile);
    if (declNode < 0 || declFile == NULL) {
        return 0;
    }
    if (outDeclFile != NULL) {
        *outDeclFile = declFile;
    }
    if (outDeclNode != NULL) {
        *outDeclNode = declNode;
    }
    return 1;
}

static int SLEvalExecExprForTypeCb(
    void* ctx, int32_t exprNode, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst);

static int SLEvalExecExprInFileWithType(
    SLEvalProgram*      p,
    const SLParsedFile* exprFile,
    SLCTFEExecEnv*      env,
    int32_t             exprNode,
    const SLParsedFile* typeFile,
    int32_t             typeNode,
    SLCTFEValue*        outValue,
    int*                outIsConst) {
    const SLParsedFile* savedFile;
    SLCTFEExecCtx*      savedExecCtx;
    SLCTFEExecCtx       tempExecCtx;
    int                 rc;
    if (p == NULL || exprFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    memset(&tempExecCtx, 0, sizeof(tempExecCtx));
    if (p->currentExecCtx != NULL) {
        tempExecCtx = *p->currentExecCtx;
    }
    tempExecCtx.ast = &exprFile->ast;
    tempExecCtx.src.ptr = exprFile->source;
    tempExecCtx.src.len = exprFile->sourceLen;
    tempExecCtx.env =
        env != NULL ? env : (p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL);
    tempExecCtx.evalExpr = SLEvalExecExprCb;
    tempExecCtx.evalExprCtx = p;
    tempExecCtx.evalExprForType = SLEvalExecExprForTypeCb;
    tempExecCtx.evalExprForTypeCtx = p;
    tempExecCtx.zeroInit = SLEvalZeroInitCb;
    tempExecCtx.zeroInitCtx = p;
    tempExecCtx.assignExpr = SLEvalAssignExprCb;
    tempExecCtx.assignExprCtx = p;
    tempExecCtx.assignValueExpr = SLEvalAssignValueExprCb;
    tempExecCtx.assignValueExprCtx = p;
    tempExecCtx.matchPattern = SLEvalMatchPatternCb;
    tempExecCtx.matchPatternCtx = p;
    tempExecCtx.forInIndex = SLEvalForInIndexCb;
    tempExecCtx.forInIndexCtx = p;
    tempExecCtx.forInIter = SLEvalForInIterCb;
    tempExecCtx.forInIterCtx = p;
    tempExecCtx.pendingReturnExprNode = -1;
    if (tempExecCtx.forIterLimit == 0) {
        tempExecCtx.forIterLimit = SLCTFE_EXEC_DEFAULT_FOR_LIMIT;
    }
    SLCTFEExecResetReason(&tempExecCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = exprFile;
    p->currentExecCtx = &tempExecCtx;
    rc = SLEvalExecExprWithTypeNode(p, exprNode, typeFile, typeNode, outValue, outIsConst);
    p->currentExecCtx = savedExecCtx;
    p->currentFile = savedFile;

    if (rc == 0 && !*outIsConst && savedExecCtx != NULL && tempExecCtx.nonConstReason != NULL) {
        SLCTFEExecSetReason(
            savedExecCtx,
            tempExecCtx.nonConstStart,
            tempExecCtx.nonConstEnd,
            tempExecCtx.nonConstReason);
    }
    return rc;
}

static int SLEvalEvalCompoundLiteral(
    SLEvalProgram*      p,
    int32_t             exprNode,
    const SLParsedFile* litFile,
    const SLParsedFile* expectedTypeFile,
    int32_t             expectedTypeNode,
    SLCTFEValue*        outValue,
    int*                outIsConst) {
    const SLAst*        ast;
    int32_t             child;
    int32_t             fieldNode;
    const SLParsedFile* declFile = NULL;
    int32_t             declNode = -1;
    const SLParsedFile* targetTypeFile = expectedTypeFile;
    int32_t             targetTypeNode = expectedTypeNode;
    SLCTFEValue         aggregateValue;
    int                 aggregateIsConst = 0;
    SLEvalAggregate*    agg;
    uint8_t*            explicitSet = NULL;
    SLCTFEValue*        explicitValues = NULL;
    SLCTFEExecBinding*  fieldBindings = NULL;
    SLCTFEExecEnv       fieldFrame;
    uint32_t            i;
    int                 rc = 0;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || litFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &litFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len
        || ast->nodes[exprNode].kind != SLAst_COMPOUND_LIT)
    {
        return 0;
    }
    child = ASTFirstChild(ast, exprNode);
    fieldNode = child;
    if (child >= 0 && IsFnReturnTypeNodeKind(ast->nodes[child].kind)) {
        targetTypeFile = litFile;
        targetTypeNode = child;
        fieldNode = ASTNextSibling(ast, child);
    }
    if (targetTypeFile != NULL && targetTypeNode >= 0
        && (uint32_t)targetTypeNode < targetTypeFile->ast.len
        && targetTypeFile->ast.nodes[targetTypeNode].kind == SLAst_TYPE_NAME)
    {
        const SLAstNode* typeNode = &targetTypeFile->ast.nodes[targetTypeNode];
        uint32_t         dot = typeNode->dataStart;
        while (dot < typeNode->dataEnd && targetTypeFile->source[dot] != '.') {
            dot++;
        }
        if (dot < typeNode->dataEnd) {
            const SLParsedFile* enumFile = NULL;
            int32_t             enumNode = SLEvalFindNamedEnumDecl(
                p, targetTypeFile, typeNode->dataStart, dot, &enumFile);
            int32_t  variantNode = -1;
            uint32_t tagIndex = 0;
            if (enumNode >= 0 && enumFile != NULL
                && SLEvalFindEnumVariant(
                    enumFile,
                    enumNode,
                    targetTypeFile->source,
                    dot + 1u,
                    typeNode->dataEnd,
                    &variantNode,
                    &tagIndex))
            {
                const SLAstNode* variantField = &enumFile->ast.nodes[variantNode];
                SLCTFEValue      payloadValue;
                int              payloadIsConst = 0;
                SLEvalAggregate* payload = NULL;
                if (SLEvalBuildTaggedEnumPayload(
                        p, enumFile, variantNode, exprNode, &payloadValue, &payloadIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!payloadIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                payload = SLEvalValueAsAggregate(&payloadValue);
                SLEvalValueSetTaggedEnum(
                    p,
                    outValue,
                    enumFile,
                    enumNode,
                    variantField->dataStart,
                    variantField->dataEnd,
                    tagIndex,
                    payload);
                *outIsConst = 1;
                return 0;
            }
        }
    }
    if (targetTypeFile != NULL && targetTypeNode >= 0
        && (uint32_t)targetTypeNode < targetTypeFile->ast.len
        && targetTypeFile->ast.nodes[targetTypeNode].kind == SLAst_TYPE_NAME
        && (SliceEqCStr(
                targetTypeFile->source,
                targetTypeFile->ast.nodes[targetTypeNode].dataStart,
                targetTypeFile->ast.nodes[targetTypeNode].dataEnd,
                "str")
            || SliceEqCStr(
                targetTypeFile->source,
                targetTypeFile->ast.nodes[targetTypeNode].dataStart,
                targetTypeFile->ast.nodes[targetTypeNode].dataEnd,
                "string")))
    {
        SLCTFEValue stringValue;
        int         stringIsConst = 0;
        if (SLEvalZeroInitTypeNode(p, targetTypeFile, targetTypeNode, &stringValue, &stringIsConst)
            != 0)
        {
            return -1;
        }
        if (!stringIsConst) {
            *outIsConst = 0;
            return 0;
        }
        while (fieldNode >= 0) {
            const SLAstNode* fieldAst = &ast->nodes[fieldNode];
            int32_t          valueNode = ASTFirstChild(ast, fieldNode);
            SLCTFEValue      fieldValue;
            int              fieldIsConst = 0;
            if (fieldAst->kind != SLAst_COMPOUND_FIELD || valueNode < 0) {
                *outIsConst = 0;
                return 0;
            }
            if (SLEvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
                return -1;
            }
            if (!fieldIsConst
                || !SLEvalValueSetFieldPath(
                    &stringValue,
                    litFile->source,
                    fieldAst->dataStart,
                    fieldAst->dataEnd,
                    &fieldValue))
            {
                *outIsConst = 0;
                return 0;
            }
            fieldNode = ASTNextSibling(ast, fieldNode);
        }
        *outValue = stringValue;
        *outIsConst = 1;
        return 0;
    }
    if (!SLEvalResolveAggregateTypeNode(
            p,
            targetTypeFile != NULL ? targetTypeFile : litFile,
            targetTypeNode,
            &declFile,
            &declNode))
    {
        uint32_t inferredFieldCount = 0;
        int32_t  scanNode = fieldNode;
        while (scanNode >= 0) {
            if (ast->nodes[scanNode].kind != SLAst_COMPOUND_FIELD) {
                *outIsConst = 0;
                return 0;
            }
            inferredFieldCount++;
            scanNode = ASTNextSibling(ast, scanNode);
        }
        if (inferredFieldCount == 0) {
            *outIsConst = 0;
            return 0;
        }
        agg = (SLEvalAggregate*)SLArenaAlloc(
            p->arena, sizeof(SLEvalAggregate), (uint32_t)_Alignof(SLEvalAggregate));
        if (agg == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg, 0, sizeof(*agg));
        agg->file = litFile;
        agg->nodeId = exprNode;
        agg->fieldLen = inferredFieldCount;
        agg->fields = (SLEvalAggregateField*)SLArenaAlloc(
            p->arena,
            sizeof(SLEvalAggregateField) * inferredFieldCount,
            (uint32_t)_Alignof(SLEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(SLEvalAggregateField) * inferredFieldCount);
        scanNode = fieldNode;
        for (i = 0; i < inferredFieldCount; i++) {
            const SLAstNode* fieldAst = &ast->nodes[scanNode];
            int32_t          valueNode = ASTFirstChild(ast, scanNode);
            int              fieldIsConst = 0;
            if (valueNode < 0
                || SLEvalExecExprCb(p, valueNode, &agg->fields[i].value, &fieldIsConst) != 0)
            {
                return valueNode < 0 ? 0 : -1;
            }
            if (!fieldIsConst) {
                *outIsConst = 0;
                return 0;
            }
            agg->fields[i].nameStart = fieldAst->dataStart;
            agg->fields[i].nameEnd = fieldAst->dataEnd;
            agg->fields[i].typeNode = -1;
            agg->fields[i].defaultExprNode = -1;
            scanNode = ASTNextSibling(ast, scanNode);
        }
        SLEvalValueSetAggregate(outValue, litFile, exprNode, agg);
        *outIsConst = 1;
        return 0;
    }
    if (SLEvalZeroInitAggregateValue(p, declFile, declNode, &aggregateValue, &aggregateIsConst)
        != 0)
    {
        return -1;
    }
    if (!aggregateIsConst) {
        *outIsConst = 0;
        return 0;
    }
    agg = SLEvalValueAsAggregate(&aggregateValue);
    if (agg == NULL) {
        *outIsConst = 0;
        return 0;
    }
    if (agg->fieldLen > 0) {
        explicitSet = (uint8_t*)SLArenaAlloc(p->arena, agg->fieldLen, (uint32_t)_Alignof(uint8_t));
        explicitValues = (SLCTFEValue*)SLArenaAlloc(
            p->arena, sizeof(SLCTFEValue) * agg->fieldLen, (uint32_t)_Alignof(SLCTFEValue));
        fieldBindings = (SLCTFEExecBinding*)SLArenaAlloc(
            p->arena,
            sizeof(SLCTFEExecBinding) * agg->fieldLen,
            (uint32_t)_Alignof(SLCTFEExecBinding));
        if (explicitSet == NULL || explicitValues == NULL || fieldBindings == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(explicitSet, 0, agg->fieldLen);
        memset(explicitValues, 0, sizeof(SLCTFEValue) * agg->fieldLen);
        memset(fieldBindings, 0, sizeof(SLCTFEExecBinding) * agg->fieldLen);
    }

    while (fieldNode >= 0) {
        const SLAstNode* fieldAst = &ast->nodes[fieldNode];
        int32_t          valueNode = ASTFirstChild(ast, fieldNode);
        int32_t          fieldIndex;
        SLCTFEValue      fieldValue;
        int              fieldIsConst = 0;
        if (fieldAst->kind != SLAst_COMPOUND_FIELD) {
            *outIsConst = 0;
            return 0;
        }
        fieldIndex = SLEvalAggregateLookupFieldIndex(
            agg, litFile->source, fieldAst->dataStart, fieldAst->dataEnd);
        if (valueNode >= 0 && fieldIndex >= 0) {
            if (SLEvalExecExprWithTypeNode(
                    p,
                    valueNode,
                    agg->file,
                    agg->fields[fieldIndex].typeNode,
                    &fieldValue,
                    &fieldIsConst)
                != 0)
            {
                return -1;
            }
        } else if ((fieldAst->flags & SLAstFlag_COMPOUND_FIELD_SHORTHAND) != 0) {
            if (SLEvalResolveIdent(
                    p, fieldAst->dataStart, fieldAst->dataEnd, &fieldValue, &fieldIsConst, NULL)
                != 0)
            {
                return -1;
            }
        } else if (valueNode >= 0) {
            if (SLEvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
                return -1;
            }
        } else {
            *outIsConst = 0;
            return 0;
        }
        if (!fieldIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (fieldIndex >= 0) {
            if (explicitSet == NULL || explicitValues == NULL) {
                return ErrorSimple("out of memory");
            }
            explicitSet[fieldIndex] = 1u;
            explicitValues[fieldIndex] = fieldValue;
        } else if (!SLEvalValueSetFieldPath(
                       &aggregateValue,
                       litFile->source,
                       fieldAst->dataStart,
                       fieldAst->dataEnd,
                       &fieldValue))
        {
            *outIsConst = 0;
            return 0;
        }
        fieldNode = ASTNextSibling(ast, fieldNode);
    }

    fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
    fieldFrame.bindings = fieldBindings;
    fieldFrame.bindingLen = 0;
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        if (explicitSet != NULL && explicitSet[i] != 0u) {
            field->value = explicitValues[i];
        } else if (field->defaultExprNode >= 0) {
            SLCTFEValue defaultValue;
            int         defaultIsConst = 0;
            if (SLEvalExecExprInFileWithType(
                    p,
                    agg->file,
                    &fieldFrame,
                    field->defaultExprNode,
                    agg->file,
                    field->typeNode,
                    &defaultValue,
                    &defaultIsConst)
                != 0)
            {
                return -1;
            }
            if (!defaultIsConst) {
                *outIsConst = 0;
                return 0;
            }
            field->value = defaultValue;
        }
        if (fieldBindings != NULL) {
            fieldBindings[fieldFrame.bindingLen].nameStart = field->nameStart;
            fieldBindings[fieldFrame.bindingLen].nameEnd = field->nameEnd;
            fieldBindings[fieldFrame.bindingLen].typeId = -1;
            fieldBindings[fieldFrame.bindingLen].typeNode = field->typeNode;
            fieldBindings[fieldFrame.bindingLen].mutable = 1u;
            fieldBindings[fieldFrame.bindingLen].value = field->value;
            fieldFrame.bindingLen++;
        }
    }
    rc = SLEvalFinalizeAggregateVarArrays(p, agg);
    if (rc != 1) {
        *outIsConst = 0;
        return rc < 0 ? -1 : 0;
    }
    *outValue = aggregateValue;
    *outIsConst = 1;
    return 0;
}

static int SLEvalExecExprWithTypeNode(
    SLEvalProgram*      p,
    int32_t             exprNode,
    const SLParsedFile* typeFile,
    int32_t             typeNode,
    SLCTFEValue*        outValue,
    int*                outIsConst) {
    const SLAst* ast;
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return -1;
    }
    if (ast->nodes[exprNode].kind == SLAst_CALL_ARG) {
        exprNode = ast->nodes[exprNode].firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
    }
    if (typeFile != NULL && typeNode >= 0 && (uint32_t)typeNode < typeFile->ast.len
        && typeFile->ast.nodes[typeNode].kind == SLAst_TYPE_TUPLE
        && ast->nodes[exprNode].kind == SLAst_TUPLE_EXPR)
    {
        SLCTFEValue elems[256];
        uint32_t    elemCount = AstListCount(ast, exprNode);
        uint32_t    i;
        if (elemCount == 0 || elemCount > 256u
            || AstListCount(&typeFile->ast, typeNode) != elemCount)
        {
            *outIsConst = 0;
            return 0;
        }
        for (i = 0; i < elemCount; i++) {
            int32_t valueNode = AstListItemAt(ast, exprNode, i);
            int32_t elemTypeNode = AstListItemAt(&typeFile->ast, typeNode, i);
            int     elemIsConst = 0;
            if (valueNode < 0 || elemTypeNode < 0
                || SLEvalExecExprWithTypeNode(
                       p, valueNode, typeFile, elemTypeNode, &elems[i], &elemIsConst)
                       != 0)
            {
                return valueNode < 0 || elemTypeNode < 0 ? 0 : -1;
            }
            if (!elemIsConst) {
                *outIsConst = 0;
                return 0;
            }
        }
        return SLEvalAllocTupleValue(p, typeFile, typeNode, elems, elemCount, outValue, outIsConst);
    }
    if (ast->nodes[exprNode].kind == SLAst_COMPOUND_LIT) {
        return SLEvalEvalCompoundLiteral(
            p,
            exprNode,
            p->currentFile,
            typeFile != NULL ? typeFile : p->currentFile,
            typeNode,
            outValue,
            outIsConst);
    }
    if (ast->nodes[exprNode].kind == SLAst_NEW) {
        return SLEvalEvalNewExpr(p, exprNode, outValue, outIsConst);
    }
    if (SLEvalExecExprCb(p, exprNode, outValue, outIsConst) != 0) {
        return -1;
    }
    if (*outIsConst && typeFile != NULL && typeNode >= 0
        && SLEvalCoerceValueToTypeNode(p, typeFile, typeNode, outValue) != 0)
    {
        return -1;
    }
    return 0;
}

static int SLEvalExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalAssignExprCb(
    void* ctx, SLCTFEExecCtx* execCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalZeroInitCb(void* ctx, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalMirMakeAggregate(
    void*        ctx,
    uint32_t     sourceNode,
    uint32_t     fieldCount,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalMirZeroInitLocal(
    void*               ctx,
    const SLMirTypeRef* typeRef,
    SLCTFEValue*        outValue,
    int*                outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalExecExprForTypeCb(
    void* ctx, int32_t exprNode, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    if (p == NULL) {
        return -1;
    }
    return SLEvalExecExprWithTypeNode(p, exprNode, p->currentFile, typeNode, outValue, outIsConst);
}
static int SLEvalResolveIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalResolveCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* _Nullable args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalResolveCallMir(
    void* ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* _Nullable args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalEvalTopConst(
    SLEvalProgram* p, uint32_t topConstIndex, SLCTFEValue* outValue, int* outIsConst);

static SLCTFEExecBinding* _Nullable SLEvalFindBinding(
    const SLCTFEExecCtx* _Nullable execCtx,
    const SLParsedFile* file,
    uint32_t            nameStart,
    uint32_t            nameEnd) {
    const SLCTFEExecEnv* frame;
    if (execCtx == NULL || file == NULL) {
        return NULL;
    }
    frame = execCtx->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            SLCTFEExecBinding* binding;
            i--;
            binding = &frame->bindings[i];
            if (SliceEqSlice(
                    file->source,
                    nameStart,
                    nameEnd,
                    execCtx->src.ptr,
                    binding->nameStart,
                    binding->nameEnd))
            {
                return binding;
            }
        }
        frame = frame->parent;
    }
    return NULL;
}

static int SLEvalTypeNodesEquivalent(
    const SLParsedFile* aFile, int32_t aNode, const SLParsedFile* bFile, int32_t bNode) {
    const SLAstNode* a;
    const SLAstNode* b;
    if (aFile == NULL || bFile == NULL || aNode < 0 || bNode < 0
        || (uint32_t)aNode >= aFile->ast.len || (uint32_t)bNode >= bFile->ast.len)
    {
        return 0;
    }
    a = &aFile->ast.nodes[aNode];
    b = &bFile->ast.nodes[bNode];
    if (a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
        case SLAst_TYPE_NAME:
            return SliceEqSlice(
                aFile->source, a->dataStart, a->dataEnd, bFile->source, b->dataStart, b->dataEnd);
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
        case SLAst_TYPE_OPTIONAL:
        case SLAst_TYPE_SLICE:
        case SLAst_TYPE_MUTSLICE:
        case SLAst_TYPE_VARRAY:
            return SLEvalTypeNodesEquivalent(aFile, a->firstChild, bFile, b->firstChild);
        case SLAst_TYPE_ARRAY:
            return SliceEqSlice(
                       aFile->source,
                       a->dataStart,
                       a->dataEnd,
                       bFile->source,
                       b->dataStart,
                       b->dataEnd)
                && SLEvalTypeNodesEquivalent(aFile, a->firstChild, bFile, b->firstChild);
        default: return 0;
    }
}

static int32_t SLEvalResolveFunctionByTypeNodeLiteral(
    const SLEvalProgram* p, const char* name, const SLParsedFile* typeFile, int32_t typeNode) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || name == NULL || typeFile == NULL || typeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = SLEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0
            || !SLEvalTypeNodesEquivalent(fn->file, paramTypeNode, typeFile, typeNode))
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

static int32_t SLEvalResolvePointerFunctionByPointeeTypeLiteral(
    const SLEvalProgram* p,
    const char*          name,
    const SLParsedFile*  typeFile,
    int32_t              pointeeTypeNode) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || name == NULL || typeFile == NULL || pointeeTypeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        int32_t               childTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = SLEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (childTypeNode < 0
            || !SLEvalTypeNodesEquivalent(fn->file, childTypeNode, typeFile, pointeeTypeNode))
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

static int SLEvalIsTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE || kind == SLAst_TYPE_ANON_STRUCT
        || kind == SLAst_TYPE_ANON_UNION;
}

static int SLEvalFindVisibleLocalTypeNodeByName(
    const SLParsedFile* file,
    uint32_t            beforePos,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    int32_t*            outTypeNode) {
    const SLAst* ast;
    uint32_t     i;
    int32_t      found = -1;
    if (outTypeNode != NULL) {
        *outTypeNode = -1;
    }
    if (file == NULL || outTypeNode == NULL) {
        return 0;
    }
    ast = &file->ast;
    for (i = 0; i < ast->len; i++) {
        const SLAstNode* n = &ast->nodes[i];
        int32_t          firstChild;
        int32_t          maybeTypeNode;
        int32_t          initNode;
        int              nameMatches = 0;
        if ((n->kind != SLAst_VAR && n->kind != SLAst_CONST) || n->start >= beforePos) {
            continue;
        }
        firstChild = n->firstChild;
        if (firstChild < 0 || (uint32_t)firstChild >= ast->len) {
            continue;
        }
        maybeTypeNode = -1;
        if (ast->nodes[firstChild].kind == SLAst_NAME_LIST) {
            int32_t afterNames;
            int32_t nameNode = ast->nodes[firstChild].firstChild;
            while (nameNode >= 0) {
                if ((uint32_t)nameNode < ast->len && ast->nodes[nameNode].kind == SLAst_IDENT
                    && SliceEqSlice(
                        file->source,
                        ast->nodes[nameNode].dataStart,
                        ast->nodes[nameNode].dataEnd,
                        file->source,
                        nameStart,
                        nameEnd))
                {
                    nameMatches = 1;
                    break;
                }
                nameNode = ast->nodes[nameNode].nextSibling;
            }
            afterNames = ast->nodes[firstChild].nextSibling;
            if (afterNames >= 0 && (uint32_t)afterNames < ast->len
                && SLEvalIsTypeNodeKind(ast->nodes[afterNames].kind))
            {
                maybeTypeNode = afterNames;
                initNode = ast->nodes[afterNames].nextSibling;
            } else {
                initNode = afterNames;
            }
        } else {
            nameMatches = SliceEqSlice(
                file->source, n->dataStart, n->dataEnd, file->source, nameStart, nameEnd);
            if (SLEvalIsTypeNodeKind(ast->nodes[firstChild].kind)) {
                maybeTypeNode = firstChild;
                initNode = ast->nodes[firstChild].nextSibling;
            } else {
                initNode = firstChild;
            }
        }
        if (!nameMatches) {
            continue;
        }
        if (maybeTypeNode >= 0) {
            found = maybeTypeNode;
            continue;
        }
        if (initNode >= 0 && (uint32_t)initNode < ast->len
            && ast->nodes[initNode].kind == SLAst_COMPOUND_LIT)
        {
            int32_t typeNode = ast->nodes[initNode].firstChild;
            if (typeNode >= 0 && (uint32_t)typeNode < ast->len
                && SLEvalIsTypeNodeKind(ast->nodes[typeNode].kind))
            {
                found = typeNode;
            }
        }
    }
    if (found < 0) {
        return 0;
    }
    *outTypeNode = found;
    return 1;
}

static int SLEvalResolveAggregateDeclFromValue(
    const SLEvalProgram* p,
    const SLCTFEValue*   value,
    const SLParsedFile** outFile,
    int32_t*             outNode) {
    const SLCTFEValue* valueTarget;
    SLEvalAggregate*   agg;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outNode != NULL) {
        *outNode = -1;
    }
    if (p == NULL || value == NULL || outFile == NULL || outNode == NULL) {
        return 0;
    }
    valueTarget = SLEvalValueTargetOrSelf(value);
    agg = SLEvalValueAsAggregate(valueTarget);
    if (agg == NULL || agg->file == NULL || agg->nodeId < 0) {
        return 0;
    }
    if ((uint32_t)agg->nodeId < agg->file->ast.len) {
        const SLAstNode* aggNode = &agg->file->ast.nodes[agg->nodeId];
        if (aggNode->kind == SLAst_COMPOUND_LIT) {
            int32_t             typeNode = aggNode->firstChild;
            const SLParsedFile* declFile = NULL;
            int32_t             declNode = -1;
            if (typeNode >= 0
                && SLEvalResolveAggregateTypeNode(p, agg->file, typeNode, &declFile, &declNode))
            {
                *outFile = declFile;
                *outNode = declNode;
                return 1;
            }
        }
    }
    *outFile = agg->file;
    *outNode = agg->nodeId;
    return 1;
}

static int32_t SLEvalResolvePointerAggregateFunctionByLiteral(
    const SLEvalProgram* p, const char* name, const SLCTFEValue* argValue) {
    const SLCTFEValue*  targetValue;
    SLEvalAggregate*    agg;
    const SLParsedFile* aggDeclFile = NULL;
    int32_t             aggDeclNode = -1;
    uint32_t            i;
    int32_t             found = -1;
    if (p == NULL || name == NULL || argValue == NULL) {
        return -1;
    }
    targetValue = SLEvalValueTargetOrSelf(argValue);
    agg = SLEvalValueAsAggregate(targetValue);
    if (agg == NULL) {
        return SLEvalResolveFunctionByLiteralArgs(p, name, argValue, 1);
    }
    if (!SLEvalResolveAggregateDeclFromValue(p, argValue, &aggDeclFile, &aggDeclNode)) {
        aggDeclFile = agg->file;
        aggDeclNode = agg->nodeId;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        int32_t               childTypeNode;
        const SLParsedFile*   declFile = NULL;
        int32_t               declNode = -1;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = SLEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (!SLEvalResolveAggregateTypeNode(p, fn->file, childTypeNode, &declFile, &declNode)
            || declFile != aggDeclFile || declNode != aggDeclNode)
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

static int32_t SLEvalResolveFunctionBySourceExprLiteral(
    SLEvalProgram*      p,
    SLCTFEExecCtx*      execCtx,
    const SLParsedFile* file,
    int32_t             exprNode,
    const char*         name) {
    const SLAstNode* expr;
    int32_t          bindingTypeNode = -1;
    if (p == NULL || execCtx == NULL || file == NULL || exprNode < 0
        || (uint32_t)exprNode >= file->ast.len || name == NULL)
    {
        return -1;
    }
    expr = &file->ast.nodes[exprNode];
    if (expr->kind == SLAst_IDENT) {
        SLCTFEExecBinding* binding = SLEvalFindBinding(
            execCtx, file, expr->dataStart, expr->dataEnd);
        if (binding != NULL && binding->typeNode >= 0) {
            return SLEvalResolveFunctionByTypeNodeLiteral(p, name, file, binding->typeNode);
        }
        if (SLEvalFindVisibleLocalTypeNodeByName(
                file, expr->start, expr->dataStart, expr->dataEnd, &bindingTypeNode))
        {
            return SLEvalResolveFunctionByTypeNodeLiteral(p, name, file, bindingTypeNode);
        }
        return -1;
    }
    if (expr->kind == SLAst_UNARY && (SLTokenKind)expr->op == SLTok_AND) {
        int32_t childNode = expr->firstChild;
        if (childNode >= 0 && (uint32_t)childNode < file->ast.len
            && file->ast.nodes[childNode].kind == SLAst_IDENT)
        {
            SLCTFEExecBinding* binding = SLEvalFindBinding(
                execCtx,
                file,
                file->ast.nodes[childNode].dataStart,
                file->ast.nodes[childNode].dataEnd);
            if (binding != NULL && binding->typeNode >= 0) {
                return SLEvalResolvePointerFunctionByPointeeTypeLiteral(
                    p, name, file, binding->typeNode);
            }
            if (SLEvalFindVisibleLocalTypeNodeByName(
                    file,
                    expr->start,
                    file->ast.nodes[childNode].dataStart,
                    file->ast.nodes[childNode].dataEnd,
                    &bindingTypeNode))
            {
                return SLEvalResolvePointerFunctionByPointeeTypeLiteral(
                    p, name, file, bindingTypeNode);
            }
        }
    }
    return -1;
}

static int32_t SLEvalResolveIteratorHookByReturnType(
    const SLEvalProgram* p, const char* name, int32_t iteratorFnIndex) {
    const SLEvalFunction* iteratorFn;
    int32_t               iteratorTypeNode;
    uint32_t              i;
    int32_t               found = -1;
    if (p == NULL || name == NULL || iteratorFnIndex < 0 || (uint32_t)iteratorFnIndex >= p->funcLen)
    {
        return -1;
    }
    iteratorFn = &p->funcs[iteratorFnIndex];
    iteratorTypeNode = SLEvalFunctionReturnTypeNode(iteratorFn);
    if (iteratorTypeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const SLEvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        int32_t               childTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = SLEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != SLAst_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (!SLEvalTypeNodesEquivalent(fn->file, childTypeNode, iteratorFn->file, iteratorTypeNode))
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

static void SLEvalAdaptForInValueBinding(
    const SLCTFEValue* inValue, int valueRef, SLCTFEValue* outValue) {
    const SLCTFEValue* target;
    if (outValue == NULL) {
        return;
    }
    if (inValue == NULL) {
        SLEvalValueSetNull(outValue);
        return;
    }
    if (valueRef) {
        *outValue = *inValue;
        return;
    }
    target = SLEvalValueReferenceTarget(inValue);
    *outValue = target != NULL ? *target : *inValue;
}

static int32_t SLEvalResolveForInIteratorFn(
    SLEvalProgram* p, SLCTFEExecCtx* execCtx, int32_t sourceNode, const SLCTFEValue* sourceValue) {
    SLCTFEValue         sourceArg;
    const SLParsedFile* sourceTypeFile = NULL;
    int32_t             sourceTypeNode = -1;
    int32_t             iteratorFn = -1;
    if (p == NULL || execCtx == NULL || sourceValue == NULL) {
        return -1;
    }
    sourceArg = *sourceValue;
    if (p->currentFile != NULL) {
        iteratorFn = SLEvalResolveFunctionBySourceExprLiteral(
            p, execCtx, p->currentFile, sourceNode, "__iterator");
        if (sourceNode >= 0 && (uint32_t)sourceNode < p->currentFile->ast.len
            && p->currentFile->ast.nodes[sourceNode].kind == SLAst_IDENT)
        {
            SLCTFEExecBinding* binding = SLEvalFindBinding(
                execCtx,
                p->currentFile,
                p->currentFile->ast.nodes[sourceNode].dataStart,
                p->currentFile->ast.nodes[sourceNode].dataEnd);
            if (binding != NULL && binding->typeNode >= 0) {
                sourceTypeFile = p->currentFile;
                sourceTypeNode = binding->typeNode;
            }
        }
    }
    if (iteratorFn < 0 && sourceTypeNode >= 0) {
        iteratorFn = SLEvalResolveFunctionByTypeNodeLiteral(
            p, "__iterator", sourceTypeFile, sourceTypeNode);
    }
    if (iteratorFn < 0) {
        iteratorFn = SLEvalResolvePointerAggregateFunctionByLiteral(p, "__iterator", sourceValue);
    }
    if (iteratorFn < 0) {
        iteratorFn = SLEvalResolveFunctionByLiteralArgs(p, "__iterator", &sourceArg, 1);
    }
    return iteratorFn;
}

static int SLEvalAdvanceForInIterator(
    SLEvalProgram* p,
    SLCTFEExecCtx* execCtx,
    int32_t        iteratorFn,
    SLCTFEValue*   iteratorValue,
    int            hasKey,
    int            keyRef,
    int            valueRef,
    int            valueDiscard,
    int*           outHasItem,
    SLCTFEValue*   outKey,
    int*           outKeyIsConst,
    SLCTFEValue*   outValue,
    int*           outValueIsConst) {
    SLCTFEValue        iterRef;
    SLCTFEValue        callResult;
    const SLCTFEValue* payload = NULL;
    int32_t            nextFn = -1;
    int32_t            nextReturnTypeNode = -1;
    int                didReturn = 0;
    int                usePair = 0;
    if (outHasItem != NULL) {
        *outHasItem = 1;
    }
    if (outKeyIsConst != NULL) {
        *outKeyIsConst = 0;
    }
    if (outValueIsConst != NULL) {
        *outValueIsConst = 0;
    }
    if (outKey != NULL) {
        SLEvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        SLEvalValueSetNull(outValue);
    }
    if (p == NULL || execCtx == NULL || iteratorValue == NULL || outHasItem == NULL
        || outKey == NULL || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    if (keyRef) {
        SLCTFEExecSetReason(
            execCtx, 0, 0, "for-in key reference is not supported in evaluator backend");
        return 0;
    }
    SLEvalValueSetReference(&iterRef, iteratorValue);
    if (hasKey) {
        if (!valueDiscard) {
            nextFn = SLEvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
            usePair = 1;
        } else {
            nextFn = SLEvalResolveIteratorHookByReturnType(p, "next_key", iteratorFn);
            usePair = 0;
            if (nextFn < 0) {
                nextFn = SLEvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
                usePair = nextFn >= 0 ? 1 : 0;
            }
        }
    } else {
        nextFn = SLEvalResolveIteratorHookByReturnType(p, "next_value", iteratorFn);
        usePair = 0;
        if (nextFn < 0) {
            nextFn = SLEvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
            usePair = nextFn >= 0 ? 1 : 0;
        }
    }
    if (nextFn < 0) {
        SLCTFEExecSetReason(
            execCtx, 0, 0, "for-in iterator hooks are not supported in evaluator backend");
        return 0;
    }
    nextReturnTypeNode = SLEvalFunctionReturnTypeNode(&p->funcs[nextFn]);
    if (nextReturnTypeNode < 0) {
        SLCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook has unsupported return type");
        return 0;
    }
    if (SLEvalInvokeFunction(p, nextFn, &iterRef, 1, p->currentContext, &callResult, &didReturn)
        != 0)
    {
        return -1;
    }
    if (!didReturn) {
        SLCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook returned unsupported value");
        return 0;
    }
    if (p->funcs[nextFn].file->ast.nodes[nextReturnTypeNode].kind == SLAst_TYPE_OPTIONAL) {
        if (callResult.kind == SLCTFEValue_OPTIONAL) {
            if (!SLEvalOptionalPayload(&callResult, &payload)) {
                SLCTFEExecSetReason(
                    execCtx, 0, 0, "for-in iterator hook returned unsupported value");
                return 0;
            }
            if (callResult.b == 0u || payload == NULL) {
                *outHasItem = 0;
                *outKeyIsConst = 1;
                *outValueIsConst = 1;
                return 0;
            }
        } else if (callResult.kind == SLCTFEValue_NULL) {
            *outHasItem = 0;
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        } else {
            payload = &callResult;
        }
    } else {
        SLCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook has unsupported return type");
        return 0;
    }
    if (payload == NULL) {
        *outHasItem = 0;
        *outKeyIsConst = 1;
        *outValueIsConst = 1;
        return 0;
    }
    if (usePair) {
        const SLCTFEValue* pairValue = SLEvalValueTargetOrSelf(payload);
        SLEvalArray*       tuple = SLEvalValueAsArray(pairValue);
        if (tuple == NULL || tuple->len != 2u) {
            SLCTFEExecSetReason(execCtx, 0, 0, "for-in pair iterator returned malformed tuple");
            return 0;
        }
        if (hasKey) {
            *outKey = tuple->elems[0];
            *outKeyIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
        if (!valueDiscard) {
            SLEvalAdaptForInValueBinding(&tuple->elems[1], valueRef, outValue);
            *outValueIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
    } else if (hasKey) {
        *outKey = *payload;
        *outKeyIsConst = 1;
        *outValueIsConst = 1;
    } else {
        SLEvalAdaptForInValueBinding(payload, valueRef, outValue);
        *outValueIsConst = 1;
        *outKeyIsConst = 1;
    }
    return 0;
}

static int SLEvalForInIterCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    int32_t            sourceNode,
    const SLCTFEValue* sourceValue,
    uint32_t           index,
    int                hasKey,
    int                keyRef,
    int                valueRef,
    int                valueDiscard,
    int*               outHasItem,
    SLCTFEValue*       outKey,
    int*               outKeyIsConst,
    SLCTFEValue*       outValue,
    int*               outValueIsConst) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    SLCTFEValue    iterValue;
    int32_t        iteratorFn = -1;
    uint32_t       step;
    int            didReturn = 0;
    if (outHasItem != NULL) {
        *outHasItem = 1;
    }
    if (outKeyIsConst != NULL) {
        *outKeyIsConst = 0;
    }
    if (outValueIsConst != NULL) {
        *outValueIsConst = 0;
    }
    if (outKey != NULL) {
        SLEvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        SLEvalValueSetNull(outValue);
    }
    if (p == NULL || execCtx == NULL || sourceValue == NULL || outHasItem == NULL || outKey == NULL
        || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    if (keyRef) {
        SLCTFEExecSetReason(
            execCtx, 0, 0, "for-in key reference is not supported in evaluator backend");
        return 0;
    }
    iteratorFn = SLEvalResolveForInIteratorFn(p, execCtx, sourceNode, sourceValue);
    if (iteratorFn < 0) {
        SLCTFEExecSetReason(
            execCtx, 0, 0, "for-in loop source is not supported in evaluator backend");
        return 0;
    }
    if (SLEvalInvokeFunction(
            p, iteratorFn, sourceValue, 1, p->currentContext, &iterValue, &didReturn)
        != 0)
    {
        return -1;
    }
    if (!didReturn) {
        SLCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook did not return a value");
        return 0;
    }
    for (step = 0; step <= index; step++) {
        if (SLEvalAdvanceForInIterator(
                p,
                execCtx,
                iteratorFn,
                &iterValue,
                hasKey,
                keyRef,
                valueRef,
                valueDiscard,
                outHasItem,
                outKey,
                outKeyIsConst,
                outValue,
                outValueIsConst)
            != 0)
        {
            return -1;
        }
        if (*outHasItem == 0 || step == index) {
            return 0;
        }
    }
    return 0;
}

static int SLEvalZeroInitCb(void* ctx, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL) {
        return -1;
    }
    return SLEvalZeroInitTypeNode(p, p->currentFile, typeNode, outValue, outIsConst);
}

static int SLEvalMirMakeAggregate(
    void*        ctx,
    uint32_t     sourceNode,
    uint32_t     fieldCount,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*   p = (SLEvalProgram*)ctx;
    SLEvalAggregate* agg;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    agg = (SLEvalAggregate*)SLArenaAlloc(
        p->arena, sizeof(SLEvalAggregate), (uint32_t)_Alignof(SLEvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = p->currentFile;
    agg->nodeId = (int32_t)sourceNode;
    if (sourceNode < p->currentFile->ast.len) {
        const SLAstNode*    sourceAst = &p->currentFile->ast.nodes[sourceNode];
        const SLParsedFile* declFile = NULL;
        int32_t             declNode = -1;
        if (sourceAst->kind == SLAst_COMPOUND_LIT && sourceAst->firstChild >= 0
            && SLEvalResolveAggregateTypeNode(
                p, p->currentFile, sourceAst->firstChild, &declFile, &declNode))
        {
            agg->file = declFile;
            agg->nodeId = declNode;
        }
    }
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (SLEvalAggregateField*)SLArenaAlloc(
            p->arena,
            sizeof(SLEvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(SLEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(SLEvalAggregateField) * fieldCount);
        {
            uint32_t i;
            for (i = 0; i < fieldCount; i++) {
                agg->fields[i].typeNode = -1;
                agg->fields[i].defaultExprNode = -1;
            }
        }
    }
    SLEvalValueSetAggregate(outValue, agg->file, agg->nodeId, agg);
    outValue->typeTag |= SLCTFEValueTag_AGG_PARTIAL;
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirFindCallNodeBySpan(
    const SLParsedFile* file,
    uint32_t            callStart,
    uint32_t            callEnd,
    uint32_t            argCount,
    int32_t*            outCallNode) {
    uint32_t i;
    int32_t  found = -1;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (file == NULL || outCallNode == NULL) {
        return 0;
    }
    for (i = 0; i < file->ast.len; i++) {
        const SLAstNode* call = &file->ast.nodes[i];
        const SLAstNode* callee;
        int32_t          calleeNode;
        uint32_t         curArgCount = 0;
        int32_t          argNode;
        uint32_t         curStart;
        uint32_t         curEnd;
        if (call->kind != SLAst_CALL) {
            continue;
        }
        calleeNode = call->firstChild;
        if (calleeNode < 0 || (uint32_t)calleeNode >= file->ast.len) {
            continue;
        }
        callee = &file->ast.nodes[calleeNode];
        if (callee->kind != SLAst_IDENT && callee->kind != SLAst_FIELD_EXPR) {
            continue;
        }
        curStart = callee->dataStart;
        curEnd = callee->dataEnd;
        if (curStart != callStart || curEnd != callEnd) {
            continue;
        }
        argNode = callee->nextSibling;
        while (argNode >= 0) {
            curArgCount++;
            argNode = file->ast.nodes[argNode].nextSibling;
        }
        if (curArgCount != argCount) {
            continue;
        }
        if (found >= 0) {
            return 0;
        }
        found = (int32_t)i;
    }
    if (found < 0) {
        return 0;
    }
    *outCallNode = found;
    return 1;
}

static int SLEvalMirAdjustCallArgs(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t        calleeFunctionIndex,
    SLMirExecValue* args,
    uint32_t        argCount,
    SLDiag* _Nullable diag) {
    SLEvalProgram*        p = (SLEvalProgram*)ctx;
    SLEvalMirExecCtx*     execCtx;
    uint32_t              evalFnIndex;
    uint32_t              receiverArgCount;
    int32_t               callNode = -1;
    int32_t               calleeNode;
    int32_t               argNode;
    int32_t               firstArgNode;
    const SLEvalFunction* calleeFn;
    (void)program;
    (void)function;
    (void)diag;
    if (p == NULL || inst == NULL || args == NULL || p->currentFile == NULL) {
        return 0;
    }
    execCtx = p->currentMirExecCtx;
    if (execCtx == NULL || execCtx->mirToEval == NULL
        || calleeFunctionIndex >= execCtx->mirToEvalLen)
    {
        return 0;
    }
    evalFnIndex = execCtx->mirToEval[calleeFunctionIndex];
    if (evalFnIndex == UINT32_MAX || evalFnIndex >= p->funcLen) {
        return 0;
    }
    receiverArgCount = SLMirCallTokDropsReceiverArg0(inst->tok) ? 1u : 0u;
    if (argCount < receiverArgCount) {
        return 0;
    }
    if (!SLEvalMirFindCallNodeBySpan(
            p->currentFile, inst->start, inst->end, argCount - receiverArgCount, &callNode))
    {
        return 0;
    }
    calleeNode = p->currentFile->ast.nodes[callNode].firstChild;
    if (calleeNode < 0 || (uint32_t)calleeNode >= p->currentFile->ast.len) {
        return 0;
    }
    calleeFn = &p->funcs[evalFnIndex];
    firstArgNode = p->currentFile->ast.nodes[calleeNode].nextSibling;
    argNode = firstArgNode;
    if (argCount > receiverArgCount && argNode >= 0) {
        uint32_t argIndex = 0;
        while (argNode >= 0 && argIndex + receiverArgCount < argCount) {
            const SLAstNode* arg = &p->currentFile->ast.nodes[argNode];
            int32_t          exprNode = argNode;
            int32_t          paramTypeNode;
            int32_t          paramIndex = (int32_t)(argIndex + receiverArgCount);
            if (arg->kind == SLAst_CALL_ARG) {
                if ((arg->flags & SLAstFlag_CALL_ARG_SPREAD) != 0) {
                    break;
                }
                exprNode = arg->firstChild;
                if (arg->dataEnd > arg->dataStart) {
                    paramIndex = SLEvalFunctionParamIndexByName(
                        calleeFn, p->currentFile->source, arg->dataStart, arg->dataEnd);
                    if (paramIndex < 0) {
                        break;
                    }
                }
            }
            if (exprNode < 0 || (uint32_t)exprNode >= p->currentFile->ast.len) {
                break;
            }
            paramTypeNode = SLEvalFunctionParamTypeNodeAt(calleeFn, (uint32_t)paramIndex);
            if (paramTypeNode >= 0
                && SLEvalExprIsAnytypePackIndex(p, &p->currentFile->ast, exprNode)
                && !SLEvalValueMatchesExpectedTypeNode(
                    p, calleeFn->file, paramTypeNode, &args[argIndex + receiverArgCount]))
            {
                if (p->currentExecCtx != NULL) {
                    SLCTFEExecSetReasonNode(
                        p->currentExecCtx, exprNode, "anytype pack element type mismatch");
                }
                return 1;
            }
            argIndex++;
            argNode = p->currentFile->ast.nodes[argNode].nextSibling;
        }
    }
    (void)SLEvalReorderFixedCallArgsByName(
        p,
        calleeFn,
        &p->currentFile->ast,
        firstArgNode,
        args + receiverArgCount,
        argCount - receiverArgCount,
        receiverArgCount);
    return 0;
}

static int SLEvalMirAssignIdent(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* inValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    int32_t        topVarIndex;
    SLCTFEValue    value;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || inValue == NULL || outIsConst == NULL) {
        return -1;
    }
    topVarIndex = SLEvalFindCurrentTopVarBySlice(p, p->currentFile, nameStart, nameEnd);
    if (topVarIndex < 0) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "identifier assignment is not supported by evaluator backend");
        }
        return 0;
    }
    value = *inValue;
    if (p->topVars[(uint32_t)topVarIndex].declTypeNode >= 0
        && SLEvalCoerceValueToTypeNode(
               p,
               p->topVars[(uint32_t)topVarIndex].file,
               p->topVars[(uint32_t)topVarIndex].declTypeNode,
               &value)
               != 0)
    {
        return -1;
    }
    p->topVars[(uint32_t)topVarIndex].value = value;
    p->topVars[(uint32_t)topVarIndex].state = SLEvalTopConstState_READY;
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirZeroInitLocal(
    void*               ctx,
    const SLMirTypeRef* typeRef,
    SLCTFEValue*        outValue,
    int*                outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL || typeRef == NULL || typeRef->astNode == UINT32_MAX
        || typeRef->astNode >= p->currentFile->ast.len)
    {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    return SLEvalZeroInitTypeNode(
        p, p->currentFile, (int32_t)typeRef->astNode, outValue, outIsConst);
}

static int SLEvalMirCoerceValueForType(
    void* ctx, const SLMirTypeRef* typeRef, SLCTFEValue* inOutValue, SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL || typeRef == NULL || inOutValue == NULL
        || typeRef->astNode == UINT32_MAX || typeRef->astNode >= p->currentFile->ast.len)
    {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    return SLEvalCoerceValueToTypeNode(p, p->currentFile, (int32_t)typeRef->astNode, inOutValue);
}

static int SLEvalMirIndexValue(
    void*              ctx,
    const SLCTFEValue* base,
    const SLCTFEValue* index,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*     p = (SLEvalProgram*)ctx;
    const SLCTFEValue* baseValue;
    SLEvalArray*       array;
    int64_t            indexInt = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = SLEvalValueTargetOrSelf(base);
    if (SLCTFEValueToInt64(index, &indexInt) != 0 || indexInt < 0) {
        return 0;
    }
    array = SLEvalValueAsArray(baseValue);
    if (array == NULL) {
        return 0;
    }
    if ((uint64_t)indexInt >= (uint64_t)array->len) {
        return 0;
    }
    *outValue = array->elems[(uint32_t)indexInt];
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirIndexAddr(
    void*              ctx,
    const SLCTFEValue* base,
    const SLCTFEValue* index,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*     p = (SLEvalProgram*)ctx;
    const SLCTFEValue* baseValue;
    SLEvalArray*       array;
    int64_t            indexInt = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = SLEvalValueTargetOrSelf(base);
    if (SLCTFEValueToInt64(index, &indexInt) != 0 || indexInt < 0) {
        return 0;
    }
    array = SLEvalValueAsArray(baseValue);
    if (array == NULL || (uint64_t)indexInt >= (uint64_t)array->len) {
        SLCTFEValue* targetValue = SLEvalValueReferenceTarget(base);
        int32_t      baseTypeCode = SLEvalTypeCode_INVALID;
        if (targetValue == NULL) {
            targetValue = (SLCTFEValue*)baseValue;
        }
        if (targetValue != NULL && targetValue->kind == SLCTFEValue_STRING
            && SLEvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
            && baseTypeCode == SLEvalTypeCode_STR_PTR
            && (uint64_t)indexInt < (uint64_t)targetValue->s.len)
        {
            SLCTFEValue* byteProxy = (SLCTFEValue*)SLArenaAlloc(
                p->arena, sizeof(SLCTFEValue), (uint32_t)_Alignof(SLCTFEValue));
            if (byteProxy == NULL) {
                return ErrorSimple("out of memory");
            }
            SLMirValueSetByteRefProxy(byteProxy, (uint8_t*)targetValue->s.bytes + indexInt);
            SLEvalValueSetRuntimeTypeCode(byteProxy, SLEvalTypeCode_U8);
            SLEvalValueSetReference(outValue, byteProxy);
            *outIsConst = 1;
            return 0;
        }
        return 0;
    }
    SLEvalValueSetReference(outValue, &array->elems[(uint32_t)indexInt]);
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirSliceValue(
    void* _Nullable ctx,
    const SLCTFEValue* _Nonnull base,
    const SLCTFEValue* _Nullable start,
    const SLCTFEValue* _Nullable end,
    uint16_t flags,
    SLCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*     p = (SLEvalProgram*)ctx;
    const SLCTFEValue* baseValue;
    SLEvalArray*       array;
    SLEvalArray*       view;
    int64_t            startInt = 0;
    int64_t            endInt = -1;
    uint32_t           startIndex = 0;
    uint32_t           endIndex = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = SLEvalValueTargetOrSelf(base);
    if ((flags & SLAstFlag_INDEX_HAS_START) != 0u) {
        if (start == NULL || SLCTFEValueToInt64(start, &startInt) != 0 || startInt < 0) {
            return 0;
        }
    }
    if ((flags & SLAstFlag_INDEX_HAS_END) != 0u) {
        if (end == NULL || SLCTFEValueToInt64(end, &endInt) != 0 || endInt < 0) {
            return 0;
        }
    }
    if (baseValue->kind == SLCTFEValue_STRING) {
        int32_t currentTypeCode = SLEvalTypeCode_INVALID;
        startIndex = (uint32_t)startInt;
        endIndex = endInt >= 0 ? (uint32_t)endInt : baseValue->s.len;
        if (startIndex > endIndex || endIndex > baseValue->s.len) {
            return 0;
        }
        *outValue = *baseValue;
        outValue->s.bytes = baseValue->s.bytes != NULL ? baseValue->s.bytes + startIndex : NULL;
        outValue->s.len = endIndex - startIndex;
        if (!SLEvalValueGetRuntimeTypeCode(baseValue, &currentTypeCode)) {
            currentTypeCode = SLEvalTypeCode_STR_REF;
        }
        SLEvalValueSetRuntimeTypeCode(outValue, currentTypeCode);
        *outIsConst = 1;
        return 0;
    }
    array = SLEvalValueAsArray(baseValue);
    if (array == NULL) {
        return 0;
    }
    startIndex = (uint32_t)startInt;
    endIndex = endInt >= 0 ? (uint32_t)endInt : array->len;
    if (startIndex > endIndex || endIndex > array->len) {
        return 0;
    }
    view = SLEvalAllocArrayView(
        p,
        baseValue->kind == SLCTFEValue_ARRAY ? array->file : p->currentFile,
        array->typeNode,
        array->elemTypeNode,
        array->elems + startIndex,
        endIndex - startIndex);
    if (view == NULL) {
        return ErrorSimple("out of memory");
    }
    {
        SLCTFEValue viewValue;
        SLEvalValueSetArray(&viewValue, p->currentFile, array->typeNode, view);
        return SLEvalAllocReferencedValue(p, &viewValue, outValue, outIsConst);
    }
}

static int SLEvalMirSequenceLen(
    void*              ctx,
    const SLCTFEValue* base,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*     p = (SLEvalProgram*)ctx;
    const SLCTFEValue* baseValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = SLEvalValueTargetOrSelf(base);
    if (baseValue->kind != SLCTFEValue_STRING && baseValue->kind != SLCTFEValue_ARRAY
        && baseValue->kind != SLCTFEValue_NULL)
    {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReason(
                p->currentExecCtx, 0, 0, "len argument is not supported by evaluator backend");
        }
        return 0;
    }
    SLEvalValueSetInt(outValue, (int64_t)baseValue->s.len);
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirMakeTuple(
    void*              ctx,
    const SLCTFEValue* elems,
    uint32_t           elemCount,
    uint32_t           typeNodeHint,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*      p = (SLEvalProgram*)ctx;
    const SLParsedFile* file;
    int32_t             typeNode = -1;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    file = p->currentFile;
    if (file == NULL) {
        return 0;
    }
    if (typeNodeHint != UINT32_MAX && typeNodeHint < file->ast.len) {
        typeNode = (int32_t)typeNodeHint;
    }
    return SLEvalAllocTupleValue(p, file, typeNode, elems, elemCount, outValue, outIsConst);
}

static int SLEvalMirMakeVariadicPack(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirTypeRef* _Nullable paramTypeRef,
    uint16_t           callFlags,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*      p = (SLEvalProgram*)ctx;
    const SLParsedFile* file;
    const SLEvalArray*  spreadArray = NULL;
    SLEvalArray*        packArray;
    int32_t             typeNode = -1;
    uint32_t            packLen = argCount;
    uint32_t            prefixCount = argCount;
    uint32_t            i;
    (void)program;
    (void)function;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    file = p->currentFile;
    if (file == NULL) {
        return 0;
    }
    if (paramTypeRef != NULL && paramTypeRef->astNode < file->ast.len) {
        typeNode = (int32_t)paramTypeRef->astNode;
    }
    if (SLMirCallTokHasSpreadLast(callFlags)) {
        if (argCount == 0u) {
            return 0;
        }
        spreadArray = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&args[argCount - 1u]));
        if (spreadArray == NULL || spreadArray->len > UINT32_MAX - (argCount - 1u)) {
            return 0;
        }
        prefixCount = argCount - 1u;
        packLen = prefixCount + spreadArray->len;
    }
    packArray = SLEvalAllocArrayView(p, file, typeNode, typeNode, NULL, packLen);
    if (packArray == NULL) {
        return ErrorSimple("out of memory");
    }
    if (packLen > 0u) {
        packArray->elems = (SLCTFEValue*)SLArenaAlloc(
            p->arena, sizeof(SLCTFEValue) * packLen, (uint32_t)_Alignof(SLCTFEValue));
        if (packArray->elems == NULL) {
            return ErrorSimple("out of memory");
        }
        for (i = 0; i < prefixCount; i++) {
            packArray->elems[i] = args[i];
            SLEvalAnnotateUntypedLiteralValue(&packArray->elems[i]);
        }
        if (spreadArray != NULL) {
            uint32_t j;
            for (j = 0; j < spreadArray->len; j++) {
                packArray->elems[prefixCount + j] = spreadArray->elems[j];
                SLEvalAnnotateUntypedLiteralValue(&packArray->elems[prefixCount + j]);
            }
        } else {
            for (; i < packLen; i++) {
                packArray->elems[i] = args[i];
                SLEvalAnnotateUntypedLiteralValue(&packArray->elems[i]);
            }
        }
    }
    SLEvalValueSetArray(outValue, file, typeNode, packArray);
    *outIsConst = 1;
    return 0;
}

static SLEvalMirIteratorState* _Nullable SLEvalMirIteratorStateFromValue(
    const SLCTFEValue* iterValue) {
    SLCTFEValue* target;
    if (iterValue == NULL) {
        return NULL;
    }
    target = SLEvalValueReferenceTarget(iterValue);
    if (target == NULL || target->kind != SLCTFEValue_SPAN
        || target->typeTag != SL_EVAL_MIR_ITER_MAGIC || target->s.bytes == NULL)
    {
        return NULL;
    }
    return (SLEvalMirIteratorState*)target->s.bytes;
}

static int SLEvalMirIterInit(
    void*              ctx,
    uint32_t           sourceNode,
    const SLCTFEValue* source,
    uint16_t           flags,
    SLCTFEValue*       outIter,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*          p = (SLEvalProgram*)ctx;
    SLEvalMirIteratorState* state;
    SLCTFEValue*            target;
    const SLCTFEValue*      sourceValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || source == NULL || outIter == NULL || outIsConst == NULL) {
        return -1;
    }
    if ((flags & SLMirIterFlag_KEY_REF) != 0u) {
        return 0;
    }
    state = (SLEvalMirIteratorState*)SLArenaAlloc(
        p->arena, sizeof(*state), (uint32_t)_Alignof(SLEvalMirIteratorState));
    target = (SLCTFEValue*)SLArenaAlloc(p->arena, sizeof(*target), (uint32_t)_Alignof(SLCTFEValue));
    if (state == NULL || target == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(state, 0, sizeof(*state));
    state->magic = SL_EVAL_MIR_ITER_MAGIC;
    state->sourceNode = sourceNode;
    state->index = 0;
    state->iteratorFn = -1;
    state->flags = flags;
    state->sourceValue = *source;
    state->iteratorValue = (SLCTFEValue){ .kind = SLCTFEValue_INVALID };
    sourceValue = SLEvalValueTargetOrSelf(source);
    if (sourceValue->kind == SLCTFEValue_ARRAY || sourceValue->kind == SLCTFEValue_STRING
        || sourceValue->kind == SLCTFEValue_NULL)
    {
        state->kind = SL_EVAL_MIR_ITER_KIND_SEQUENCE;
    } else {
        int didReturn = 0;
        state->kind = SL_EVAL_MIR_ITER_KIND_PROTOCOL;
        if (p->currentExecCtx == NULL) {
            return 0;
        }
        state->iteratorFn = SLEvalResolveForInIteratorFn(
            p, p->currentExecCtx, (int32_t)sourceNode, source);
        if (state->iteratorFn < 0) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                0,
                0,
                "for-in loop source is not supported in evaluator backend");
            return 0;
        }
        if (SLEvalInvokeFunction(
                p,
                state->iteratorFn,
                source,
                1,
                p->currentContext,
                &state->iteratorValue,
                &didReturn)
            != 0)
        {
            return -1;
        }
        if (!didReturn) {
            SLCTFEExecSetReason(
                p->currentExecCtx, 0, 0, "for-in iterator hook did not return a value");
            return 0;
        }
    }
    target->kind = SLCTFEValue_SPAN;
    target->i64 = 0;
    target->f64 = 0.0;
    target->b = 0;
    target->typeTag = SL_EVAL_MIR_ITER_MAGIC;
    target->s.bytes = (const uint8_t*)state;
    target->s.len = 0;
    target->span = (SLCTFESpan){ 0 };
    SLEvalValueSetReference(outIter, target);
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirIterNext(
    void*              ctx,
    const SLCTFEValue* iter,
    uint16_t           flags,
    int*               outHasItem,
    SLCTFEValue*       outKey,
    int*               outKeyIsConst,
    SLCTFEValue*       outValue,
    int*               outValueIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*          p = (SLEvalProgram*)ctx;
    SLEvalMirIteratorState* state;
    const SLCTFEValue*      sourceValue;
    int                     hasKey;
    int                     keyRef;
    int                     valueRef;
    int                     valueDiscard;
    (void)diag;
    if (outHasItem != NULL) {
        *outHasItem = 0;
    }
    if (outKeyIsConst != NULL) {
        *outKeyIsConst = 0;
    }
    if (outValueIsConst != NULL) {
        *outValueIsConst = 0;
    }
    if (outKey != NULL) {
        SLEvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        SLEvalValueSetNull(outValue);
    }
    if (p == NULL || iter == NULL || outHasItem == NULL || outKey == NULL || outKeyIsConst == NULL
        || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    state = SLEvalMirIteratorStateFromValue(iter);
    if (state == NULL) {
        return 0;
    }
    hasKey = (flags & SLMirIterFlag_HAS_KEY) != 0u;
    keyRef = (flags & SLMirIterFlag_KEY_REF) != 0u;
    valueRef = (flags & SLMirIterFlag_VALUE_REF) != 0u;
    valueDiscard = (flags & SLMirIterFlag_VALUE_DISCARD) != 0u;
    if (state->kind == SL_EVAL_MIR_ITER_KIND_SEQUENCE) {
        sourceValue = SLEvalValueTargetOrSelf(&state->sourceValue);
        if (sourceValue->kind != SLCTFEValue_ARRAY && sourceValue->kind != SLCTFEValue_STRING
            && sourceValue->kind != SLCTFEValue_NULL)
        {
            return 0;
        }
        if (state->index >= sourceValue->s.len) {
            *outHasItem = 0;
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        }
        if (hasKey) {
            SLEvalValueSetInt(outKey, (int64_t)state->index);
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if (!valueDiscard) {
            if (p->currentExecCtx == NULL) {
                return 0;
            }
            if (SLEvalForInIndexCb(
                    p,
                    p->currentExecCtx,
                    &state->sourceValue,
                    state->index,
                    valueRef,
                    outValue,
                    outValueIsConst)
                != 0)
            {
                return -1;
            }
            if (!*outValueIsConst) {
                return 0;
            }
        } else {
            *outValueIsConst = 1;
        }
        *outHasItem = 1;
        state->index++;
        return 0;
    }
    if (state->kind == SL_EVAL_MIR_ITER_KIND_PROTOCOL) {
        if (p->currentExecCtx == NULL) {
            return 0;
        }
        if (SLEvalAdvanceForInIterator(
                p,
                p->currentExecCtx,
                state->iteratorFn,
                &state->iteratorValue,
                hasKey,
                keyRef,
                valueRef,
                valueDiscard,
                outHasItem,
                outKey,
                outKeyIsConst,
                outValue,
                outValueIsConst)
            != 0)
        {
            return -1;
        }
        if (*outHasItem) {
            state->index++;
        }
        return 0;
    }
    return 0;
}

static int SLEvalMirAggGetField(
    void*              ctx,
    const SLCTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*       p = (SLEvalProgram*)ctx;
    const SLCTFEValue*   baseValue;
    const SLCTFEValue*   payload = NULL;
    SLEvalAggregate*     agg = NULL;
    SLEvalReflectedType* rt;
    SLEvalTaggedEnum*    tagged;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || base == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    baseValue = SLEvalValueTargetOrSelf(base);
    tagged = SLEvalValueAsTaggedEnum(baseValue);
    if (tagged != NULL && tagged->payload != NULL
        && SLEvalAggregateGetFieldValue(
            tagged->payload, p->currentFile->source, nameStart, nameEnd, outValue))
    {
        *outIsConst = 1;
        return 0;
    }
    if (baseValue->kind == SLCTFEValue_OPTIONAL && SLEvalOptionalPayload(baseValue, &payload)
        && baseValue->b != 0u && payload != NULL)
    {
        baseValue = SLEvalValueTargetOrSelf(payload);
    }
    if (SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "len")
        && (baseValue->kind == SLCTFEValue_STRING || baseValue->kind == SLCTFEValue_ARRAY
            || baseValue->kind == SLCTFEValue_NULL))
    {
        SLEvalValueSetInt(outValue, (int64_t)baseValue->s.len);
        *outIsConst = 1;
        return 0;
    }
    if (SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "cstr")
        && baseValue->kind == SLCTFEValue_STRING)
    {
        *outValue = *baseValue;
        *outIsConst = 1;
        return 0;
    }
    rt = SLEvalValueAsReflectedType(baseValue);
    if (rt != NULL && rt->kind == SLEvalReflectType_NAMED && rt->namedKind == SLEvalTypeKind_ENUM
        && rt->file != NULL && rt->nodeId >= 0 && (uint32_t)rt->nodeId < rt->file->ast.len)
    {
        int32_t  variantNode = -1;
        uint32_t tagIndex = 0;
        if (SLEvalFindEnumVariant(
                rt->file,
                rt->nodeId,
                p->currentFile->source,
                nameStart,
                nameEnd,
                &variantNode,
                &tagIndex))
        {
            const SLAstNode* variantField = &rt->file->ast.nodes[variantNode];
            int32_t          valueNode = ASTFirstChild(&rt->file->ast, variantNode);
            if (valueNode >= 0 && rt->file->ast.nodes[valueNode].kind != SLAst_FIELD
                && !SLEvalEnumHasPayloadVariants(rt->file, rt->nodeId))
            {
                int valueIsConst = 0;
                if (SLEvalExecExprInFileWithType(
                        p,
                        rt->file,
                        p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL,
                        valueNode,
                        rt->file,
                        -1,
                        outValue,
                        &valueIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!valueIsConst) {
                    return 0;
                }
                *outIsConst = 1;
                return 0;
            }
            SLEvalValueSetTaggedEnum(
                p,
                outValue,
                rt->file,
                rt->nodeId,
                variantField->dataStart,
                variantField->dataEnd,
                tagIndex,
                NULL);
            *outIsConst = 1;
            return 0;
        }
    }
    agg = SLEvalValueAsAggregate(baseValue);
    if (agg == NULL) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field access is not supported by evaluator backend");
        }
        return 0;
    }
    if (!SLEvalAggregateGetFieldValue(agg, p->currentFile->source, nameStart, nameEnd, outValue)) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field access is not supported by evaluator backend");
        }
        return 0;
    }
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirAggAddrField(
    void*              ctx,
    const SLCTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*     p = (SLEvalProgram*)ctx;
    const SLCTFEValue* baseValue;
    const SLCTFEValue* payload = NULL;
    SLEvalAggregate*   agg = NULL;
    SLCTFEValue*       fieldValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || base == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    baseValue = SLEvalValueTargetOrSelf(base);
    if (baseValue->kind == SLCTFEValue_OPTIONAL && SLEvalOptionalPayload(baseValue, &payload)
        && baseValue->b != 0u && payload != NULL)
    {
        baseValue = SLEvalValueTargetOrSelf(payload);
    }
    agg = SLEvalValueAsAggregate(baseValue);
    if (agg == NULL) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field address is not supported by evaluator backend");
        }
        return 0;
    }
    fieldValue = SLEvalAggregateLookupFieldValuePtr(
        agg, p->currentFile->source, nameStart, nameEnd);
    if (fieldValue == NULL) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field address is not supported by evaluator backend");
        }
        return 0;
    }
    SLEvalValueSetReference(outValue, fieldValue);
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirAggSetField(
    void* _Nullable ctx,
    SLCTFEValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLCTFEValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || inOutBase == NULL || inValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    {
        SLCTFEValue*      value = inOutBase;
        SLEvalAggregate*  agg = SLEvalValueAsAggregate(value);
        SLEvalTaggedEnum* tagged = SLEvalValueAsTaggedEnum(value);
        uint32_t          i;
        uint32_t          dot = nameStart;
        while (dot < nameEnd && p->currentFile->source[dot] != '.') {
            dot++;
        }
        if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
            agg = tagged->payload;
        }
        if (agg == NULL) {
            value = (SLCTFEValue*)SLEvalValueTargetOrSelf(inOutBase);
            agg = SLEvalValueAsAggregate(value);
            tagged = SLEvalValueAsTaggedEnum(value);
            if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
                agg = tagged->payload;
            }
        }
        if (agg != NULL && dot == nameEnd) {
            if (value != NULL) {
                value->typeTag |= SLCTFEValueTag_AGG_PARTIAL;
            }
            SLEvalAggregateField* field = SLEvalAggregateFindDirectField(
                agg, p->currentFile->source, nameStart, nameEnd);
            if (field != NULL) {
                SLCTFEValue coercedValue = *inValue;
                if (field->typeNode >= 0
                    && SLEvalCoerceValueToTypeNode(p, agg->file, field->typeNode, &coercedValue)
                           != 0)
                {
                    return -1;
                }
                field->_reserved |= 1u;
                field->value = coercedValue;
                *outIsConst = 1;
                return 0;
            }
            for (i = 0; i < agg->fieldLen; i++) {
                field = &agg->fields[i];
                if (field->nameStart == 0 && field->nameEnd == 0 && field->typeNode < 0
                    && field->defaultExprNode < 0)
                {
                    field->nameStart = nameStart;
                    field->nameEnd = nameEnd;
                    field->flags = 0;
                    field->_reserved = 1u;
                    field->value = *inValue;
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (agg != NULL && value != NULL) {
            value->typeTag |= SLCTFEValueTag_AGG_PARTIAL;
        }
    }
    if (!SLEvalValueSetFieldPath(inOutBase, p->currentFile->source, nameStart, nameEnd, inValue)) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field assignment is not supported by evaluator backend");
        }
        return 0;
    }
    *outIsConst = 1;
    return 0;
}

static int SLEvalMirHostCall(
    void*              ctx,
    uint32_t           hostId,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (hostId == SL_EVAL_MIR_HOST_PRINT && argCount == 1u && args[0].kind == SLCTFEValue_STRING) {
        if (args[0].s.len > 0 && args[0].s.bytes != NULL) {
            if (fwrite(args[0].s.bytes, 1, args[0].s.len, stdout) != args[0].s.len) {
                return ErrorSimple("failed to write print output");
            }
        }
        fputc('\n', stdout);
        fflush(stdout);
        SLEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == SL_EVAL_MIR_HOST_CONCAT && argCount == 2u) {
        int concatRc = SLEvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
        if (concatRc < 0) {
            return -1;
        }
        if (concatRc > 0) {
            *outIsConst = 1;
            return 0;
        }
        *outIsConst = 0;
        return 0;
    }
    if (hostId == SL_EVAL_MIR_HOST_COPY && argCount == 2u) {
        int copyRc = SLEvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
        if (copyRc < 0) {
            return -1;
        }
        if (copyRc > 0) {
            *outIsConst = 1;
            return 0;
        }
        *outIsConst = 0;
        return 0;
    }
    if (hostId == SL_EVAL_MIR_HOST_FREE && (argCount == 1u || argCount == 2u)) {
        SLEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == SL_EVAL_MIR_HOST_PLATFORM_EXIT && argCount == 1u) {
        int64_t exitCode = 0;
        if (SLCTFEValueToInt64(&args[0], &exitCode) != 0) {
            *outIsConst = 0;
            return 0;
        }
        p->exitCalled = 1;
        p->exitCode = (int)(exitCode & 255);
        SLEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    *outIsConst = 0;
    return 0;
}

static void SLEvalMirSetReason(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* _Nonnull reason) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    if (p == NULL || p->currentExecCtx == NULL || reason[0] == '\0') {
        return;
    }
    SLCTFEExecSetReason(p->currentExecCtx, start, end, reason);
}

static int SLEvalMirEnterFunction(
    void* ctx, uint32_t functionIndex, uint32_t sourceRef, SLDiag* _Nullable diag) {
    SLEvalMirExecCtx* c = (SLEvalMirExecCtx*)ctx;
    uint8_t           pushed = 0;
    if (c == NULL || c->p == NULL || sourceRef >= c->sourceFileCap
        || c->savedFileLen >= SL_EVAL_CALL_MAX_DEPTH)
    {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    if (functionIndex != c->rootMirFnIndex) {
        uint32_t evalFnIndex;
        if (c->mirToEval == NULL || functionIndex >= c->mirToEvalLen) {
            if (diag != NULL) {
                diag->code = SLDiag_UNEXPECTED_TOKEN;
                diag->type = SLDiagTypeOfCode(diag->code);
                diag->start = 0;
                diag->end = 0;
                diag->argStart = 0;
                diag->argEnd = 0;
            }
            return -1;
        }
        evalFnIndex = c->mirToEval[functionIndex];
        if (evalFnIndex == UINT32_MAX) {
            pushed = 0;
        } else if (evalFnIndex >= c->p->funcLen || c->p->callDepth >= SL_EVAL_CALL_MAX_DEPTH) {
            if (diag != NULL) {
                diag->code = SLDiag_UNEXPECTED_TOKEN;
                diag->type = SLDiagTypeOfCode(diag->code);
                diag->start = 0;
                diag->end = 0;
                diag->argStart = 0;
                diag->argEnd = 0;
            }
            return -1;
        } else {
            c->p->callStack[c->p->callDepth++] = evalFnIndex;
            pushed = 1;
        }
    }
    c->savedMirExecCtxs[c->savedFileLen] = c->p->currentMirExecCtx;
    c->p->currentMirExecCtx = c;
    c->savedFiles[c->savedFileLen++] = c->p->currentFile;
    c->pushedFrames[c->savedFileLen - 1u] = pushed;
    if (c->sourceFiles[sourceRef] != NULL) {
        c->p->currentFile = c->sourceFiles[sourceRef];
    }
    return 0;
}

static void SLEvalMirLeaveFunction(void* ctx) {
    SLEvalMirExecCtx* c = (SLEvalMirExecCtx*)ctx;
    if (c == NULL || c->p == NULL || c->savedFileLen == 0) {
        return;
    }
    if (c->pushedFrames[c->savedFileLen - 1u] && c->p->callDepth > 0) {
        c->p->callDepth--;
    }
    c->p->currentMirExecCtx = c->savedMirExecCtxs[c->savedFileLen - 1u];
    c->p->currentFile = c->savedFiles[--c->savedFileLen];
}

static int SLEvalMirBindFrame(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLCTFEValue* _Nullable locals,
    uint32_t localCount,
    SLDiag* _Nullable diag) {
    SLEvalMirExecCtx* c = (SLEvalMirExecCtx*)ctx;
    if (c == NULL || c->mirFrameDepth >= SL_EVAL_CALL_MAX_DEPTH) {
        if (diag != NULL) {
            diag->code = SLDiag_UNEXPECTED_TOKEN;
            diag->type = SLDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    c->savedMirPrograms[c->mirFrameDepth] = c->mirProgram;
    c->savedMirFunctions[c->mirFrameDepth] = c->mirFunction;
    c->savedMirLocals[c->mirFrameDepth] = c->mirLocals;
    c->savedMirLocalCounts[c->mirFrameDepth] = c->mirLocalCount;
    c->mirFrameDepth++;
    c->mirProgram = program;
    c->mirFunction = function;
    c->mirLocals = locals;
    c->mirLocalCount = localCount;
    return 0;
}

static void SLEvalMirUnbindFrame(void* _Nullable ctx) {
    SLEvalMirExecCtx* c = (SLEvalMirExecCtx*)ctx;
    if (c == NULL || c->mirFrameDepth == 0) {
        return;
    }
    c->mirFrameDepth--;
    c->mirProgram = c->savedMirPrograms[c->mirFrameDepth];
    c->mirFunction = c->savedMirFunctions[c->mirFrameDepth];
    c->mirLocals = c->savedMirLocals[c->mirFrameDepth];
    c->mirLocalCount = c->savedMirLocalCounts[c->mirFrameDepth];
}

static int SLEvalMirResolveCallNode(
    const SLMirProgram* _Nullable program,
    const SLMirInst* _Nullable inst,
    int32_t* _Nonnull outCallNode) {
    const SLMirSymbolRef* symbol;
    *outCallNode = -1;
    if (program == NULL || inst == NULL || inst->op != SLMirOp_CALL
        || inst->aux >= program->symbolLen)
    {
        return 0;
    }
    symbol = &program->symbols[inst->aux];
    if (symbol->kind != SLMirSymbol_CALL || symbol->target == UINT32_MAX) {
        return 0;
    }
    *outCallNode = (int32_t)symbol->target;
    return 1;
}

static int SLEvalMirCallNodeIsLazyBuiltin(SLEvalProgram* p, int32_t callNode) {
    const SLAst*     ast;
    const SLAstNode* call;
    const SLAstNode* callee;
    int32_t          calleeNode;
    int32_t          recvNode;
    if (p == NULL || p->currentFile == NULL) {
        return 0;
    }
    ast = &p->currentFile->ast;
    if (callNode < 0 || (uint32_t)callNode >= ast->len) {
        return 0;
    }
    call = &ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &ast->nodes[calleeNode] : NULL;
    if (call->kind != SLAst_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == SLAst_IDENT) {
        return SLEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "span_of", "reflect")
            || SLEvalNameIsLazyTypeBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd)
            || SLEvalNameIsCompilerDiagBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd);
    }
    if (callee->kind != SLAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= ast->len || ast->nodes[recvNode].kind != SLAst_IDENT)
    {
        return 0;
    }
    if (SliceEqCStr(
            p->currentFile->source,
            ast->nodes[recvNode].dataStart,
            ast->nodes[recvNode].dataEnd,
            "reflect"))
    {
        return SLEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "span_of", "reflect")
            || SLEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "kind", "reflect")
            || SLEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "base", "reflect")
            || SLEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source,
                   callee->dataStart,
                   callee->dataEnd,
                   "is_alias",
                   "reflect")
            || SLEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source,
                   callee->dataStart,
                   callee->dataEnd,
                   "type_name",
                   "reflect");
    }
    if (!SliceEqCStr(
            p->currentFile->source,
            ast->nodes[recvNode].dataStart,
            ast->nodes[recvNode].dataEnd,
            "compiler"))
    {
        return 0;
    }
    return SLEvalNameIsCompilerDiagBuiltin(
        p->currentFile->source, callee->dataStart, callee->dataEnd);
}

static const SLPackage* _Nullable SLEvalMirResolveQualifiedImportCallTargetPkg(
    SLEvalProgram* p, int32_t callNode) {
    const SLPackage* currentPkg;
    const SLAst*     ast;
    const SLAstNode* call;
    const SLAstNode* callee;
    const SLAstNode* base;
    uint32_t         i;
    int32_t          calleeNode;
    int32_t          baseNode;
    if (p == NULL || p->currentFile == NULL) {
        return NULL;
    }
    currentPkg = SLEvalFindPackageByFile(p, p->currentFile);
    ast = &p->currentFile->ast;
    if (currentPkg == NULL || callNode < 0 || (uint32_t)callNode >= ast->len) {
        return NULL;
    }
    call = &ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &ast->nodes[calleeNode] : NULL;
    baseNode = calleeNode >= 0 ? ast->nodes[calleeNode].firstChild : -1;
    base = baseNode >= 0 ? &ast->nodes[baseNode] : NULL;
    if (call->kind != SLAst_CALL || callee == NULL || callee->kind != SLAst_FIELD_EXPR
        || base == NULL || base->kind != SLAst_IDENT)
    {
        return NULL;
    }
    for (i = 0; i < currentPkg->importLen; i++) {
        const SLImportRef* imp = &currentPkg->imports[i];
        if (imp->bindName == NULL || imp->target == NULL) {
            continue;
        }
        if (strlen(imp->bindName) == (size_t)(base->dataEnd - base->dataStart)
            && memcmp(
                   imp->bindName,
                   p->currentFile->source + base->dataStart,
                   (size_t)(base->dataEnd - base->dataStart))
                   == 0)
        {
            return imp->target;
        }
    }
    return NULL;
}

static int SLEvalMirLookupLocalValue(
    SLEvalProgram* p, uint32_t nameStart, uint32_t nameEnd, SLCTFEValue* outValue) {
    SLEvalMirExecCtx*   c;
    const SLParsedFile* file;
    if (p == NULL || outValue == NULL || p->currentFile == NULL) {
        return 0;
    }
    c = p->currentMirExecCtx;
    file = p->currentFile;
    while (c != NULL) {
        uint32_t i;
        if (c->mirProgram != NULL && c->mirFunction != NULL && c->mirLocals != NULL
            && c->mirFunction->localStart <= c->mirProgram->localLen
            && c->mirFunction->localCount <= c->mirProgram->localLen - c->mirFunction->localStart
            && c->mirLocalCount >= c->mirFunction->localCount)
        {
            for (i = c->mirFunction->localCount; i > 0; i--) {
                const SLMirLocal* local =
                    &c->mirProgram->locals[c->mirFunction->localStart + i - 1u];
                const SLCTFEValue* value = &c->mirLocals[i - 1u];
                if (local->nameEnd <= local->nameStart
                    || !SliceEqSlice(
                        file->source,
                        nameStart,
                        nameEnd,
                        file->source,
                        local->nameStart,
                        local->nameEnd))
                {
                    continue;
                }
                if (value->kind == SLCTFEValue_INVALID) {
                    continue;
                }
                *outValue = *value;
                if (local->typeRef != UINT32_MAX && local->typeRef < c->mirProgram->typeLen) {
                    int32_t typeNode = (int32_t)c->mirProgram->types[local->typeRef].astNode;
                    int32_t typeCode = SLEvalTypeCode_INVALID;
                    if (!SLEvalValueGetRuntimeTypeCode(outValue, &typeCode) && typeNode >= 0
                        && SLEvalTypeCodeFromTypeNode(file, typeNode, &typeCode))
                    {
                        SLEvalValueSetRuntimeTypeCode(outValue, typeCode);
                    }
                }
                return 1;
            }
        }
        if (c->savedFileLen == 0) {
            break;
        }
        if (c->savedMirExecCtxs[c->savedFileLen - 1u] == c) {
            break;
        }
        c = c->savedMirExecCtxs[c->savedFileLen - 1u];
    }
    return 0;
}

static int SLEvalMirLookupLocalTypeNode(
    SLEvalProgram*       p,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const SLParsedFile** outFile,
    int32_t*             outTypeNode) {
    SLEvalMirExecCtx*   c;
    const SLParsedFile* file;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outTypeNode != NULL) {
        *outTypeNode = -1;
    }
    if (p == NULL || outFile == NULL || outTypeNode == NULL || p->currentFile == NULL) {
        return 0;
    }
    c = p->currentMirExecCtx;
    file = p->currentFile;
    while (c != NULL) {
        uint32_t i;
        if (c->mirProgram != NULL && c->mirFunction != NULL && c->mirLocals != NULL
            && c->mirFunction->localStart <= c->mirProgram->localLen
            && c->mirFunction->localCount <= c->mirProgram->localLen - c->mirFunction->localStart
            && c->mirLocalCount >= c->mirFunction->localCount)
        {
            for (i = c->mirFunction->localCount; i > 0; i--) {
                const SLMirLocal* local =
                    &c->mirProgram->locals[c->mirFunction->localStart + i - 1u];
                if (local->nameEnd <= local->nameStart || local->typeRef == UINT32_MAX
                    || local->typeRef >= c->mirProgram->typeLen
                    || !SliceEqSlice(
                        file->source,
                        nameStart,
                        nameEnd,
                        file->source,
                        local->nameStart,
                        local->nameEnd))
                {
                    continue;
                }
                *outFile = file;
                *outTypeNode = (int32_t)c->mirProgram->types[local->typeRef].astNode;
                return *outTypeNode >= 0;
            }
        }
        if (c->savedFileLen == 0) {
            break;
        }
        if (c->savedMirExecCtxs[c->savedFileLen - 1u] == c) {
            break;
        }
        c = c->savedMirExecCtxs[c->savedFileLen - 1u];
    }
    return 0;
}

static void SLEvalMirInitExecEnv(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    SLMirExecEnv*       env,
    SLEvalMirExecCtx* _Nullable functionCtx) {
    if (p == NULL || file == NULL || env == NULL) {
        return;
    }
    memset(env, 0, sizeof(*env));
    env->src.ptr = file->source;
    env->src.len = file->sourceLen;
    env->resolveIdent = SLEvalResolveIdent;
    env->assignIdent = SLEvalMirAssignIdent;
    env->assignIdentCtx = p;
    env->resolveCallPre = SLEvalResolveCallMirPre;
    env->resolveCall = SLEvalResolveCallMir;
    env->adjustCallArgs = SLEvalMirAdjustCallArgs;
    env->resolveCtx = p;
    env->adjustCallArgsCtx = p;
    env->hostCall = SLEvalMirHostCall;
    env->hostCtx = p;
    env->zeroInitLocal = SLEvalMirZeroInitLocal;
    env->zeroInitCtx = p;
    env->coerceValueForType = SLEvalMirCoerceValueForType;
    env->coerceValueCtx = p;
    env->indexValue = SLEvalMirIndexValue;
    env->indexValueCtx = p;
    env->indexAddr = SLEvalMirIndexAddr;
    env->indexAddrCtx = p;
    env->sliceValue = SLEvalMirSliceValue;
    env->sliceValueCtx = p;
    env->sequenceLen = SLEvalMirSequenceLen;
    env->sequenceLenCtx = p;
    env->iterInit = SLEvalMirIterInit;
    env->iterInitCtx = p;
    env->iterNext = SLEvalMirIterNext;
    env->iterNextCtx = p;
    env->aggGetField = SLEvalMirAggGetField;
    env->aggGetFieldCtx = p;
    env->aggAddrField = SLEvalMirAggAddrField;
    env->aggAddrFieldCtx = p;
    env->aggSetField = SLEvalMirAggSetField;
    env->aggSetFieldCtx = p;
    env->makeAggregate = SLEvalMirMakeAggregate;
    env->makeAggregateCtx = p;
    env->makeTuple = SLEvalMirMakeTuple;
    env->makeTupleCtx = p;
    env->makeVariadicPack = SLEvalMirMakeVariadicPack;
    env->makeVariadicPackCtx = p;
    env->evalBinary = SLEvalMirEvalBinary;
    env->evalBinaryCtx = p;
    env->allocNew = SLEvalMirAllocNew;
    env->allocNewCtx = p;
    env->contextGet = SLEvalMirContextGet;
    env->contextGetCtx = p;
    env->evalWithContext = SLEvalMirEvalWithContext;
    env->evalWithContextCtx = p;
    env->setReason = SLEvalMirSetReason;
    env->setReasonCtx = p;
    env->backwardJumpLimit = p->currentExecCtx != NULL ? p->currentExecCtx->forIterLimit : 0;
    env->diag = p->currentExecCtx != NULL ? p->currentExecCtx->diag : NULL;
    if (functionCtx != NULL) {
        env->enterFunction = SLEvalMirEnterFunction;
        env->leaveFunction = SLEvalMirLeaveFunction;
        env->functionCtx = functionCtx;
        env->bindFrame = SLEvalMirBindFrame;
        env->unbindFrame = SLEvalMirUnbindFrame;
        env->frameCtx = functionCtx;
    }
}

static int SLEvalMirEvalBinary(
    void* _Nullable ctx,
    SLTokenKind           op,
    const SLMirExecValue* lhs,
    const SLMirExecValue* rhs,
    SLMirExecValue*       outValue,
    int*                  outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    (void)diag;
    return SLEvalEvalBinary(p, op, lhs, rhs, outValue, outIsConst);
}

static int SLEvalTryMirZeroInitType(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    int32_t             typeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    SLCTFEValue*        outValue,
    int*                outIsConst) {
    return SLEvalTryMirEvalTopInit(
        p, file, -1, typeNode, nameStart, nameEnd, NULL, -1, outValue, outIsConst, NULL);
}

static int SLEvalTryMirEvalTopInit(
    SLEvalProgram*      p,
    const SLParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const SLParsedFile* _Nullable coerceTypeFile,
    int32_t      coerceTypeNode,
    SLCTFEValue* outValue,
    int*         outIsConst,
    int* _Nullable outSupported) {
    SLMirProgram     program = { 0 };
    SLMirExecEnv     env = { 0 };
    SLEvalMirExecCtx functionCtx = { 0 };
    uint32_t         rootMirFnIndex = UINT32_MAX;
    int              supported = 0;
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (p == NULL || file == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (SLEvalMirBuildTopInitProgram(
            p,
            file,
            initExprNode,
            declTypeNode,
            nameStart,
            nameEnd,
            &program,
            &functionCtx,
            &rootMirFnIndex,
            &supported)
        != 0)
    {
        return -1;
    }
    if (!supported) {
        *outIsConst = 0;
        return 0;
    }
    SLEvalMirInitExecEnv(p, file, &env, &functionCtx);
    if (!SLMirProgramNeedsDynamicResolution(&program)) {
        SLMirExecEnvDisableDynamicResolution(&env);
    }
    if (SLMirEvalFunction(p->arena, &program, rootMirFnIndex, NULL, 0, &env, outValue, outIsConst)
        != 0)
    {
        return -1;
    }
    SLEvalMirAdaptOutValue(&functionCtx, outValue, outIsConst);
    if (*outIsConst && coerceTypeFile != NULL && coerceTypeNode >= 0
        && SLEvalAdaptStringValueForType(
               p->arena, coerceTypeFile, coerceTypeNode, outValue, outValue)
               < 0)
    {
        return -1;
    }
    if (outSupported != NULL) {
        *outSupported = 1;
    }
    return 0;
}

static int SLEvalTryMirEvalExprWithType(
    SLEvalProgram*      p,
    int32_t             exprNode,
    const SLParsedFile* exprFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const SLParsedFile* _Nullable typeFile,
    int32_t      typeNode,
    SLCTFEValue* outValue,
    int*         outIsConst,
    int*         outSupported) {
    return SLEvalTryMirEvalTopInit(
        p,
        exprFile,
        exprNode,
        -1,
        nameStart,
        nameEnd,
        typeFile,
        typeNode,
        outValue,
        outIsConst,
        outSupported);
}

static int SLEvalValueNeedsDefaultFieldEval(const SLCTFEValue* value) {
    SLEvalAggregate* agg;
    uint32_t         i;
    if (value == NULL) {
        return 0;
    }
    agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(value));
    if (agg == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        if (agg->fields[i].defaultExprNode >= 0) {
            return 1;
        }
    }
    return 0;
}

static int SLEvalFinalizeNewAggregateDefaults(SLEvalProgram* p, SLCTFEValue* inOutValue) {
    SLEvalAggregate*   agg = SLEvalValueAsAggregate(inOutValue);
    SLCTFEExecBinding* fieldBindings = NULL;
    SLCTFEExecEnv      fieldFrame;
    uint32_t           i;
    if (p == NULL || inOutValue == NULL || agg == NULL
        || !SLEvalValueNeedsDefaultFieldEval(inOutValue))
    {
        return 1;
    }
    if (agg->fieldLen > 0) {
        fieldBindings = (SLCTFEExecBinding*)SLArenaAlloc(
            p->arena,
            sizeof(SLCTFEExecBinding) * agg->fieldLen,
            (uint32_t)_Alignof(SLCTFEExecBinding));
        if (fieldBindings == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(fieldBindings, 0, sizeof(SLCTFEExecBinding) * agg->fieldLen);
    }
    fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
    fieldFrame.bindings = fieldBindings;
    fieldFrame.bindingLen = 0;
    for (i = 0; i < agg->fieldLen; i++) {
        SLEvalAggregateField* field = &agg->fields[i];
        if (field->defaultExprNode >= 0) {
            SLCTFEValue defaultValue;
            int         defaultIsConst = 0;
            if (SLEvalExecExprInFileWithType(
                    p,
                    agg->file,
                    &fieldFrame,
                    field->defaultExprNode,
                    agg->file,
                    field->typeNode,
                    &defaultValue,
                    &defaultIsConst)
                != 0)
            {
                return -1;
            }
            if (!defaultIsConst) {
                return 0;
            }
            field->value = defaultValue;
        }
        if (fieldBindings != NULL) {
            fieldBindings[fieldFrame.bindingLen].nameStart = field->nameStart;
            fieldBindings[fieldFrame.bindingLen].nameEnd = field->nameEnd;
            fieldBindings[fieldFrame.bindingLen].typeId = -1;
            fieldBindings[fieldFrame.bindingLen].typeNode = field->typeNode;
            fieldBindings[fieldFrame.bindingLen].mutable = 1u;
            fieldBindings[fieldFrame.bindingLen].value = field->value;
            fieldFrame.bindingLen++;
        }
    }
    return 1;
}

static int SLEvalMirAllocNew(
    void* _Nullable ctx,
    uint32_t        sourceNode,
    SLMirExecValue* outValue,
    int*            outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*      p = (SLEvalProgram*)ctx;
    const SLParsedFile* newFile;
    int32_t             exprNode = (int32_t)sourceNode;
    int32_t             typeNode = -1;
    int32_t             countNode = -1;
    int32_t             initNode = -1;
    int32_t             allocNode = -1;
    SLCTFEValue         allocValue;
    int                 allocIsConst = 0;
    int                 allocSupported = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    newFile = p->currentFile;
    if (!SLEvalDecodeNewExprNodes(newFile, exprNode, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    if (allocNode >= 0) {
        if (SLEvalTryMirEvalExprWithType(
                p, allocNode, newFile, 0, 0, NULL, -1, &allocValue, &allocIsConst, &allocSupported)
            != 0)
        {
            return -1;
        }
        if (!allocSupported || !allocIsConst) {
            return 0;
        }
    } else if (!SLEvalCurrentContextFieldByLiteral(p, "mem", &allocValue)) {
        if (p->currentExecCtx != NULL) {
            SLCTFEExecSetReasonNode(
                p->currentExecCtx,
                exprNode,
                "allocator context is not available in evaluator backend");
        }
        return 0;
    }
    if (SLEvalValueTargetOrSelf(&allocValue)->kind == SLCTFEValue_NULL) {
        *outValue = allocValue;
        *outIsConst = 1;
        return 1;
    }
    if (countNode >= 0) {
        SLCTFEValue  countValue;
        SLCTFEValue  arrayValue;
        SLEvalArray* array;
        int          countIsConst = 0;
        int          countSupported = 0;
        int64_t      count = 0;
        uint32_t     i;
        if (SLEvalTryMirEvalExprWithType(
                p, countNode, newFile, 0, 0, NULL, -1, &countValue, &countIsConst, &countSupported)
            != 0)
        {
            return -1;
        }
        if (!countSupported || !countIsConst || SLCTFEValueToInt64(&countValue, &count) != 0
            || count < 0)
        {
            return 0;
        }
        array = (SLEvalArray*)SLArenaAlloc(
            p->arena, sizeof(SLEvalArray), (uint32_t)_Alignof(SLEvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = newFile;
        array->typeNode = exprNode;
        array->elemTypeNode = typeNode;
        array->len = (uint32_t)count;
        if (array->len > 0) {
            array->elems = (SLCTFEValue*)SLArenaAlloc(
                p->arena, sizeof(SLCTFEValue) * array->len, (uint32_t)_Alignof(SLCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(SLCTFEValue) * array->len);
            if (initNode >= 0) {
                SLCTFEValue initValue;
                int         initIsConst = 0;
                if (SLEvalExecExprWithTypeNode(
                        p, initNode, newFile, typeNode, &initValue, &initIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!initIsConst) {
                    return 0;
                }
                if (SLEvalCoerceValueToTypeNode(p, newFile, typeNode, &initValue) != 0) {
                    return -1;
                }
                for (i = 0; i < array->len; i++) {
                    array->elems[i] = initValue;
                }
            } else {
                for (i = 0; i < array->len; i++) {
                    int elemIsConst = 0;
                    if (SLEvalZeroInitTypeNode(p, newFile, typeNode, &array->elems[i], &elemIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (!elemIsConst) {
                        return 0;
                    }
                    {
                        int finalizeRc = SLEvalFinalizeNewAggregateDefaults(p, &array->elems[i]);
                        if (finalizeRc <= 0) {
                            return finalizeRc < 0 ? -1 : 0;
                        }
                    }
                }
            }
        }
        SLEvalValueSetArray(&arrayValue, newFile, exprNode, array);
        return SLEvalAllocReferencedValue(p, &arrayValue, outValue, outIsConst) == 0 ? 1 : -1;
    }
    {
        SLCTFEValue value;
        int         valueIsConst = 0;
        if (initNode >= 0) {
            if (SLEvalExecExprWithTypeNode(p, initNode, newFile, typeNode, &value, &valueIsConst)
                != 0)
            {
                return -1;
            }
            if (!valueIsConst) {
                return 0;
            }
        } else {
            if (SLEvalZeroInitTypeNode(p, newFile, typeNode, &value, &valueIsConst) != 0) {
                return -1;
            }
            if (!valueIsConst) {
                return 0;
            }
        }
        if (initNode >= 0) {
            if (SLEvalCoerceValueToTypeNode(p, newFile, typeNode, &value) != 0) {
                return -1;
            }
        } else {
            int finalizeRc = SLEvalFinalizeNewAggregateDefaults(p, &value);
            if (finalizeRc <= 0) {
                return finalizeRc < 0 ? -1 : 0;
            }
        }
        return SLEvalAllocReferencedValue(p, &value, outValue, outIsConst) == 0 ? 1 : -1;
    }
}

static int SLEvalBinaryOpForAssignToken(SLTokenKind assignOp, SLTokenKind* outBinaryOp) {
    if (outBinaryOp == NULL) {
        return 0;
    }
    switch (assignOp) {
        case SLTok_ADD_ASSIGN: *outBinaryOp = SLTok_ADD; return 1;
        case SLTok_SUB_ASSIGN: *outBinaryOp = SLTok_SUB; return 1;
        case SLTok_MUL_ASSIGN: *outBinaryOp = SLTok_MUL; return 1;
        case SLTok_DIV_ASSIGN: *outBinaryOp = SLTok_DIV; return 1;
        case SLTok_MOD_ASSIGN: *outBinaryOp = SLTok_MOD; return 1;
        default:               *outBinaryOp = SLTok_INVALID; return 0;
    }
}

static int SLEvalAssignExprCb(
    void* ctx, SLCTFEExecCtx* execCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst) {
    SLEvalProgram*   p = (SLEvalProgram*)ctx;
    const SLAst*     ast;
    const SLAstNode* expr;
    int32_t          lhsNode;
    int32_t          rhsNode;
    SLCTFEValue      rhsValue;
    int              rhsIsConst = 0;
    if (p == NULL || p->currentFile == NULL || execCtx == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        *outIsConst = 0;
        return 0;
    }
    expr = &ast->nodes[exprNode];
    lhsNode = expr->firstChild;
    rhsNode = lhsNode >= 0 ? ast->nodes[lhsNode].nextSibling : -1;
    if (expr->kind != SLAst_BINARY || lhsNode < 0 || rhsNode < 0
        || ast->nodes[rhsNode].nextSibling >= 0)
    {
        *outIsConst = 0;
        return 0;
    }
    if (ast->nodes[lhsNode].kind == SLAst_IDENT) {
        if (SliceEqCStr(
                p->currentFile->source,
                ast->nodes[lhsNode].dataStart,
                ast->nodes[lhsNode].dataEnd,
                "_"))
        {
            if (SLEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
                return -1;
            }
            if (!rhsIsConst) {
                *outIsConst = 0;
                return 0;
            }
            *outValue = rhsValue;
            *outIsConst = 1;
            return 0;
        }
        int32_t topVarIndex = SLEvalFindCurrentTopVarBySlice(
            p, p->currentFile, ast->nodes[lhsNode].dataStart, ast->nodes[lhsNode].dataEnd);
        if (topVarIndex >= 0) {
            SLEvalTopVar* topVar = &p->topVars[(uint32_t)topVarIndex];
            if ((SLTokenKind)expr->op == SLTok_ASSIGN) {
                if (topVar->declTypeNode >= 0) {
                    if (SLEvalExecExprWithTypeNode(
                            p, rhsNode, topVar->file, topVar->declTypeNode, &rhsValue, &rhsIsConst)
                        != 0)
                    {
                        return -1;
                    }
                } else if (SLEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
                    return -1;
                }
                if (!rhsIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                topVar->value = rhsValue;
                topVar->state = SLEvalTopConstState_READY;
                *outValue = rhsValue;
                *outIsConst = 1;
                return 0;
            }
        }
    }
    if (ast->nodes[lhsNode].kind == SLAst_INDEX && (ast->nodes[lhsNode].flags & 0x7u) == 0u) {
        int32_t      baseNode = ast->nodes[lhsNode].firstChild;
        int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        SLCTFEValue  baseValue;
        SLCTFEValue  indexValue;
        SLEvalArray* array;
        SLTokenKind  binaryOp = SLTok_INVALID;
        int          baseIsConst = 0;
        int          indexIsConst = 0;
        int64_t      index = 0;
        if (SLEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (!rhsIsConst || baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0)
        {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || SLEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || SLCTFEValueToInt64(&indexValue, &index) != 0) {
            *outIsConst = 0;
            return 0;
        }
        array = SLEvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                int handled;
                if (!SLEvalBinaryOpForAssignToken((SLTokenKind)expr->op, &binaryOp)) {
                    *outIsConst = 0;
                    return 0;
                }
                handled = SLEvalEvalBinary(
                    p, binaryOp, &array->elems[(uint32_t)index], &rhsValue, &rhsValue, outIsConst);
                if (handled <= 0 || !*outIsConst) {
                    *outIsConst = 0;
                    return handled < 0 ? -1 : 0;
                }
            }
            array->elems[(uint32_t)index] = rhsValue;
            *outValue = rhsValue;
            *outIsConst = 1;
            return 0;
        }
        {
            SLCTFEValue* targetValue = SLEvalValueReferenceTarget(&baseValue);
            int32_t      baseTypeCode = SLEvalTypeCode_INVALID;
            int64_t      byteValue = 0;
            if (targetValue == NULL) {
                targetValue = (SLCTFEValue*)SLEvalValueTargetOrSelf(&baseValue);
            }
            if (targetValue != NULL && targetValue->kind == SLCTFEValue_STRING
                && SLEvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
                && baseTypeCode == SLEvalTypeCode_STR_PTR && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len
                && SLCTFEValueToInt64(&rhsValue, &byteValue) == 0 && byteValue >= 0
                && byteValue <= 255)
            {
                ((uint8_t*)targetValue->s.bytes)[(uint32_t)index] = (uint8_t)byteValue;
                SLEvalValueSetInt(outValue, byteValue);
                SLEvalValueSetRuntimeTypeCode(outValue, SLEvalTypeCode_U8);
                *outIsConst = 1;
                return 0;
            }
        }
        *outIsConst = 0;
        return 0;
    }
    if (ast->nodes[lhsNode].kind == SLAst_UNARY && (SLTokenKind)ast->nodes[lhsNode].op == SLTok_MUL)
    {
        int32_t      refNode = ast->nodes[lhsNode].firstChild;
        SLCTFEValue  refValue;
        SLCTFEValue* target;
        SLTokenKind  binaryOp = SLTok_INVALID;
        int          refIsConst = 0;
        if (SLEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (!rhsIsConst || refNode < 0) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, refNode, &refValue, &refIsConst) != 0) {
            return -1;
        }
        if (!refIsConst) {
            *outIsConst = 0;
            return 0;
        }
        target = SLEvalValueReferenceTarget(&refValue);
        if (target == NULL) {
            *outIsConst = 0;
            return 0;
        }
        if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
            int handled;
            if (!SLEvalBinaryOpForAssignToken((SLTokenKind)expr->op, &binaryOp)) {
                *outIsConst = 0;
                return 0;
            }
            handled = SLEvalEvalBinary(p, binaryOp, target, &rhsValue, &rhsValue, outIsConst);
            if (handled <= 0 || !*outIsConst) {
                *outIsConst = 0;
                return handled < 0 ? -1 : 0;
            }
        }
        *target = rhsValue;
        *outValue = rhsValue;
        *outIsConst = 1;
        return 0;
    }
    if (ast->nodes[lhsNode].kind != SLAst_FIELD_EXPR) {
        *outIsConst = 0;
        return 0;
    }
    if (SLEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
        return -1;
    }
    if (!rhsIsConst) {
        *outIsConst = 0;
        return 0;
    }
    {
        int32_t            curNode = lhsNode;
        SLCTFEExecBinding* binding = NULL;
        SLEvalAggregate*   agg = NULL;
        while (ast->nodes[curNode].kind == SLAst_FIELD_EXPR) {
            int32_t baseNode = ast->nodes[curNode].firstChild;
            if (baseNode < 0) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[baseNode].kind == SLAst_IDENT) {
                int allowIndirectMutation = 0;
                binding = SLEvalFindBinding(
                    execCtx,
                    p->currentFile,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd);
                if (binding == NULL) {
                    *outIsConst = 0;
                    return 0;
                }
                agg = SLEvalValueAsAggregate(&binding->value);
                if (agg != NULL) {
                    allowIndirectMutation = 0;
                } else {
                    agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(&binding->value));
                    allowIndirectMutation = agg != NULL;
                }
                if (!binding->mutable && !allowIndirectMutation) {
                    *outIsConst = 0;
                    return 0;
                }
                break;
            }
            if (ast->nodes[baseNode].kind == SLAst_FIELD_EXPR) {
                curNode = baseNode;
                continue;
            }
            *outIsConst = 0;
            return 0;
        }
        if (binding == NULL || agg == NULL) {
            *outIsConst = 0;
            return 0;
        }
        {
            int32_t  pathNodes[16];
            uint32_t pathLen = 0;
            curNode = lhsNode;
            while (curNode >= 0 && ast->nodes[curNode].kind == SLAst_FIELD_EXPR) {
                if (pathLen >= 16u) {
                    *outIsConst = 0;
                    return 0;
                }
                pathNodes[pathLen++] = curNode;
                curNode = ast->nodes[curNode].firstChild;
            }
            while (pathLen > 0) {
                const SLAstNode* fieldNode;
                pathLen--;
                fieldNode = &ast->nodes[pathNodes[pathLen]];
                if (pathLen == 0) {
                    if ((SLTokenKind)expr->op != SLTok_ASSIGN) {
                        SLCTFEValue curValue;
                        SLTokenKind binaryOp = SLTok_INVALID;
                        int         handled = 0;
                        if (!SLEvalAggregateGetFieldValue(
                                agg,
                                p->currentFile->source,
                                fieldNode->dataStart,
                                fieldNode->dataEnd,
                                &curValue))
                        {
                            *outIsConst = 0;
                            return 0;
                        }
                        if (!SLEvalBinaryOpForAssignToken((SLTokenKind)expr->op, &binaryOp)) {
                            *outIsConst = 0;
                            return 0;
                        }
                        handled = SLEvalEvalBinary(
                            p, binaryOp, &curValue, &rhsValue, &rhsValue, outIsConst);
                        if (handled <= 0 || !*outIsConst) {
                            *outIsConst = 0;
                            return handled < 0 ? -1 : 0;
                        }
                    }
                    if (SLEvalAggregateSetFieldValue(
                            agg,
                            p->currentFile->source,
                            fieldNode->dataStart,
                            fieldNode->dataEnd,
                            &rhsValue,
                            outValue))
                    {
                        *outIsConst = 1;
                        return 0;
                    }
                    *outIsConst = 0;
                    return 0;
                }
                {
                    SLCTFEValue nestedValue;
                    if (!SLEvalAggregateGetFieldValue(
                            agg,
                            p->currentFile->source,
                            fieldNode->dataStart,
                            fieldNode->dataEnd,
                            &nestedValue))
                    {
                        *outIsConst = 0;
                        return 0;
                    }
                    agg = SLEvalValueAsAggregate(&nestedValue);
                }
                if (agg == NULL) {
                    *outIsConst = 0;
                    return 0;
                }
            }
        }
    }
    *outIsConst = 0;
    return 0;
}

static SLCTFEValue* _Nullable SLEvalResolveFieldExprValuePtr(
    SLEvalProgram* p, SLCTFEExecCtx* execCtx, int32_t fieldExprNode) {
    const SLAst*     ast;
    const SLAstNode* fieldExpr;
    int32_t          baseNode;
    SLEvalAggregate* agg = NULL;
    if (p == NULL || p->currentFile == NULL || execCtx == NULL || fieldExprNode < 0) {
        return NULL;
    }
    ast = &p->currentFile->ast;
    if ((uint32_t)fieldExprNode >= ast->len) {
        return NULL;
    }
    fieldExpr = &ast->nodes[fieldExprNode];
    if (fieldExpr->kind != SLAst_FIELD_EXPR) {
        return NULL;
    }
    baseNode = fieldExpr->firstChild;
    if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
        return NULL;
    }
    if (ast->nodes[baseNode].kind == SLAst_IDENT) {
        SLCTFEExecBinding* binding = SLEvalFindBinding(
            execCtx, p->currentFile, ast->nodes[baseNode].dataStart, ast->nodes[baseNode].dataEnd);
        if (binding == NULL) {
            return NULL;
        }
        agg = SLEvalValueAsAggregate(&binding->value);
        if (agg == NULL) {
            agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(&binding->value));
        }
    } else if (ast->nodes[baseNode].kind == SLAst_FIELD_EXPR) {
        SLCTFEValue* baseValue = SLEvalResolveFieldExprValuePtr(p, execCtx, baseNode);
        if (baseValue == NULL) {
            return NULL;
        }
        agg = SLEvalValueAsAggregate(baseValue);
        if (agg == NULL) {
            agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(baseValue));
        }
    }
    if (agg == NULL) {
        return NULL;
    }
    return SLEvalAggregateLookupFieldValuePtr(
        agg, p->currentFile->source, fieldExpr->dataStart, fieldExpr->dataEnd);
}

static int SLEvalAssignValueExprCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    int32_t            lhsExprNode,
    const SLCTFEValue* inValue,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    const SLAst*   ast;
    if (p == NULL || p->currentFile == NULL || execCtx == NULL || inValue == NULL
        || outValue == NULL || outIsConst == NULL)
    {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (lhsExprNode < 0 || (uint32_t)lhsExprNode >= ast->len) {
        *outIsConst = 0;
        return 0;
    }
    if (ast->nodes[lhsExprNode].kind == SLAst_IDENT) {
        if (SliceEqCStr(
                p->currentFile->source,
                ast->nodes[lhsExprNode].dataStart,
                ast->nodes[lhsExprNode].dataEnd,
                "_"))
        {
            *outValue = *inValue;
            *outIsConst = 1;
            return 0;
        }
        int32_t topVarIndex = SLEvalFindCurrentTopVarBySlice(
            p, p->currentFile, ast->nodes[lhsExprNode].dataStart, ast->nodes[lhsExprNode].dataEnd);
        if (topVarIndex >= 0) {
            SLEvalTopVar* topVar = &p->topVars[(uint32_t)topVarIndex];
            topVar->value = *inValue;
            topVar->state = SLEvalTopConstState_READY;
            *outValue = *inValue;
            *outIsConst = 1;
            return 0;
        }
    }
    if (ast->nodes[lhsExprNode].kind == SLAst_INDEX && (ast->nodes[lhsExprNode].flags & 0x7u) == 0u)
    {
        int32_t      baseNode = ast->nodes[lhsExprNode].firstChild;
        int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        SLCTFEValue  baseValue;
        SLCTFEValue  indexValue;
        SLEvalArray* array;
        int          baseIsConst = 0;
        int          indexIsConst = 0;
        int64_t      index = 0;
        if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || SLEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || SLCTFEValueToInt64(&indexValue, &index) != 0) {
            *outIsConst = 0;
            return 0;
        }
        array = SLEvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            array->elems[(uint32_t)index] = *inValue;
            *outValue = *inValue;
            *outIsConst = 1;
            return 0;
        }
        {
            SLCTFEValue* targetValue = SLEvalValueReferenceTarget(&baseValue);
            int32_t      baseTypeCode = SLEvalTypeCode_INVALID;
            int64_t      byteValue = 0;
            if (targetValue == NULL) {
                targetValue = (SLCTFEValue*)SLEvalValueTargetOrSelf(&baseValue);
            }
            if (targetValue != NULL && targetValue->kind == SLCTFEValue_STRING
                && SLEvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
                && baseTypeCode == SLEvalTypeCode_STR_PTR && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len
                && SLCTFEValueToInt64(inValue, &byteValue) == 0 && byteValue >= 0
                && byteValue <= 255)
            {
                ((uint8_t*)targetValue->s.bytes)[(uint32_t)index] = (uint8_t)byteValue;
                SLEvalValueSetInt(outValue, byteValue);
                SLEvalValueSetRuntimeTypeCode(outValue, SLEvalTypeCode_U8);
                *outIsConst = 1;
                return 0;
            }
        }
        *outIsConst = 0;
        return 0;
    }
    *outIsConst = 0;
    return 0;
}

static int SLEvalMatchPatternCb(
    void*              ctx,
    SLCTFEExecCtx*     execCtx,
    const SLCTFEValue* subjectValue,
    int32_t            labelExprNode,
    int*               outMatched) {
    SLEvalProgram*      p = (SLEvalProgram*)ctx;
    SLEvalTaggedEnum*   tagged;
    const SLAstNode*    labelNode;
    const SLParsedFile* enumFile = NULL;
    int32_t             enumNode = -1;
    int32_t             variantNode = -1;
    uint32_t            tagIndex = 0;
    (void)execCtx;
    if (outMatched != NULL) {
        *outMatched = 0;
    }
    if (p == NULL || p->currentFile == NULL || subjectValue == NULL || outMatched == NULL
        || labelExprNode < 0 || (uint32_t)labelExprNode >= p->currentFile->ast.len)
    {
        return -1;
    }
    tagged = SLEvalValueAsTaggedEnum(SLEvalValueTargetOrSelf(subjectValue));
    if (tagged == NULL) {
        return 0;
    }
    labelNode = &p->currentFile->ast.nodes[labelExprNode];
    if (labelNode->kind != SLAst_FIELD_EXPR || labelNode->firstChild < 0
        || p->currentFile->ast.nodes[labelNode->firstChild].kind != SLAst_IDENT)
    {
        return 0;
    }
    enumNode = SLEvalFindNamedEnumDecl(
        p,
        p->currentFile,
        p->currentFile->ast.nodes[labelNode->firstChild].dataStart,
        p->currentFile->ast.nodes[labelNode->firstChild].dataEnd,
        &enumFile);
    if (enumNode < 0 || enumFile == NULL
        || !SLEvalFindEnumVariant(
            enumFile,
            enumNode,
            p->currentFile->source,
            labelNode->dataStart,
            labelNode->dataEnd,
            &variantNode,
            &tagIndex))
    {
        return 0;
    }
    *outMatched =
        tagged->file == enumFile && tagged->enumNode == enumNode && tagged->tagIndex == tagIndex;
    return 1;
}

SL_API_END
#include "evaluator_mir.inc"
SL_API_BEGIN

static int SLEvalInvokeFunction(
    SLEvalProgram*       p,
    int32_t              fnIndex,
    const SLCTFEValue* _Nullable args,
    uint32_t             argCount,
    const SLEvalContext* callContext,
    SLCTFEValue*         outValue,
    int*                 outDidReturn) {
    const SLEvalFunction* fn;
    const SLAst*          ast;
    SLCTFEExecBinding*    paramBindings = NULL;
    SLCTFEExecEnv         paramFrame;
    SLCTFEExecCtx         execCtx;
    const SLParsedFile*   savedFile;
    SLCTFEExecCtx*        savedExecCtx;
    const SLEvalContext*  savedContext;
    int                   isConst = 0;
    int                   rc;
    int32_t               child;
    uint32_t              paramIndex = 0;
    uint32_t              fixedCount = 0;

    if (p == NULL || outValue == NULL || outDidReturn == NULL || (argCount > 0 && args == NULL)
        || fnIndex < 0
        || (uint32_t)fnIndex >= p->funcLen)
    {
        return -1;
    }
    fn = &p->funcs[fnIndex];
    ast = &fn->file->ast;
    fixedCount = fn->isVariadic && fn->paramCount > 0 ? fn->paramCount - 1u : fn->paramCount;
    if ((!fn->isVariadic && argCount != fn->paramCount)
        || (fn->isVariadic && argCount < fixedCount))
    {
        return ErrorEvalUnsupported(
            fn->file->path,
            fn->file->source,
            ast->nodes[fn->fnNode].start,
            ast->nodes[fn->fnNode].end,
            "evaluator backend call arity mismatch");
    }

    if (fn->isBuiltinPackageFn
        && SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, "concat"))
    {
        int concatRc;
        if (argCount != 2) {
            return ErrorEvalUnsupported(
                fn->file->path,
                fn->file->source,
                ast->nodes[fn->fnNode].start,
                ast->nodes[fn->fnNode].end,
                "evaluator backend call arity mismatch");
        }
        concatRc = SLEvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
        if (concatRc < 0) {
            return -1;
        }
        if (concatRc == 0) {
            return ErrorEvalUnsupported(
                fn->file->path,
                fn->file->source,
                ast->nodes[fn->fnNode].start,
                ast->nodes[fn->fnNode].end,
                "concat arguments are not available in evaluator backend");
        }
        *outDidReturn = 1;
        return 0;
    }
    if (fn->isBuiltinPackageFn && SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, "copy"))
    {
        int copyRc;
        if (argCount != 2) {
            return ErrorEvalUnsupported(
                fn->file->path,
                fn->file->source,
                ast->nodes[fn->fnNode].start,
                ast->nodes[fn->fnNode].end,
                "evaluator backend call arity mismatch");
        }
        copyRc = SLEvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
        if (copyRc < 0) {
            return -1;
        }
        if (copyRc == 0) {
            return ErrorEvalUnsupported(
                fn->file->path,
                fn->file->source,
                ast->nodes[fn->fnNode].start,
                ast->nodes[fn->fnNode].end,
                "copy arguments are not available in evaluator backend");
        }
        *outDidReturn = 1;
        return 0;
    }

    if (fn->paramCount > 0) {
        paramBindings = (SLCTFEExecBinding*)SLArenaAlloc(
            p->arena,
            sizeof(SLCTFEExecBinding) * fn->paramCount,
            (uint32_t)_Alignof(SLCTFEExecBinding));
        if (paramBindings == NULL) {
            return ErrorSimple("out of memory");
        }
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const SLAstNode* n = &ast->nodes[child];
        if (n->kind == SLAst_PARAM) {
            if (paramBindings == NULL) {
                return ErrorSimple("out of memory");
            }
            int32_t     paramTypeNode = ASTFirstChild(ast, child);
            SLCTFEValue boundValue;
            if (fn->isVariadic && paramIndex + 1u == fn->paramCount) {
                SLEvalArray* packArray;
                uint32_t     packLen = argCount - fixedCount;
                packArray = SLEvalAllocArrayView(
                    p, fn->file, paramTypeNode, paramTypeNode, NULL, packLen);
                if (packArray == NULL) {
                    return ErrorSimple("out of memory");
                }
                if (packLen > 0) {
                    uint32_t i;
                    packArray->elems = (SLCTFEValue*)SLArenaAlloc(
                        p->arena, sizeof(SLCTFEValue) * packLen, (uint32_t)_Alignof(SLCTFEValue));
                    if (packArray->elems == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    memset(packArray->elems, 0, sizeof(SLCTFEValue) * packLen);
                    for (i = 0; i < packLen; i++) {
                        packArray->elems[i] = args[fixedCount + i];
                    }
                }
                SLEvalValueSetArray(&boundValue, fn->file, paramTypeNode, packArray);
            } else {
                boundValue = args[paramIndex];
                if (paramTypeNode >= 0
                    && SLEvalCoerceValueToTypeNode(p, fn->file, paramTypeNode, &boundValue) != 0)
                {
                    return -1;
                }
            }
            paramBindings[paramIndex].nameStart = n->dataStart;
            paramBindings[paramIndex].nameEnd = n->dataEnd;
            paramBindings[paramIndex].typeId = -1;
            paramBindings[paramIndex].typeNode = paramTypeNode;
            paramBindings[paramIndex].mutable = 1;
            paramBindings[paramIndex]._reserved[0] = 0;
            paramBindings[paramIndex]._reserved[1] = 0;
            paramBindings[paramIndex]._reserved[2] = 0;
            paramBindings[paramIndex].value = boundValue;
            paramIndex++;
        }
        child = ASTNextSibling(ast, child);
    }

    paramFrame.parent = NULL;
    paramFrame.bindings = paramBindings;
    paramFrame.bindingLen = fn->paramCount;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = p->arena;
    execCtx.ast = ast;
    execCtx.src.ptr = fn->file->source;
    execCtx.src.len = fn->file->sourceLen;
    execCtx.env = &paramFrame;
    execCtx.evalExpr = SLEvalExecExprCb;
    execCtx.evalExprCtx = p;
    execCtx.evalExprForType = SLEvalExecExprForTypeCb;
    execCtx.evalExprForTypeCtx = p;
    execCtx.zeroInit = SLEvalZeroInitCb;
    execCtx.zeroInitCtx = p;
    execCtx.assignExpr = SLEvalAssignExprCb;
    execCtx.assignExprCtx = p;
    execCtx.assignValueExpr = SLEvalAssignValueExprCb;
    execCtx.assignValueExprCtx = p;
    execCtx.matchPattern = SLEvalMatchPatternCb;
    execCtx.matchPatternCtx = p;
    execCtx.forInIndex = SLEvalForInIndexCb;
    execCtx.forInIndexCtx = p;
    execCtx.forInIter = SLEvalForInIterCb;
    execCtx.forInIterCtx = p;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = SLCTFE_EXEC_DEFAULT_FOR_LIMIT;
    execCtx.skipConstBlocks = 1u;
    SLCTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    savedContext = p->currentContext;
    p->currentFile = fn->file;
    p->currentExecCtx = &execCtx;
    p->currentContext = callContext;
    p->callStack[p->callDepth++] = (uint32_t)fnIndex;

    rc = SLEvalTryMirInvokeFunction(
        p, fn, fnIndex, args, argCount, outValue, outDidReturn, &isConst);

    p->callDepth--;
    p->currentContext = savedContext;
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
        return ErrorEvalUnsupported(fn->file->path, fn->file->source, errStart, errEnd, reason);
    }
    if (!*outDidReturn) {
        SLEvalValueSetNull(outValue);
    } else if (fn->hasReturnType) {
        int32_t returnTypeNode = SLEvalFunctionReturnTypeNode(fn);
        if (returnTypeNode >= 0
            && SLEvalCoerceValueToTypeNode(p, fn->file, returnTypeNode, outValue) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int SLEvalInvokeFunctionRef(
    SLEvalProgram*     p,
    const SLCTFEValue* calleeValue,
    const SLCTFEValue* _Nullable args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    uint32_t              fnIndex = 0;
    uint32_t              mirFnIndex = 0;
    int                   didReturn = 0;
    const SLEvalFunction* fn;
    if (p == NULL || calleeValue == NULL || outValue == NULL || outIsConst == NULL
        || (argCount > 0 && args == NULL))
    {
        return 0;
    }
    if (SLMirValueAsFunctionRef(calleeValue, &mirFnIndex)) {
        SLMirExecEnv env = { 0 };
        int          mirIsConst = 0;
        if (p->currentMirExecCtx == NULL || p->currentMirExecCtx->mirProgram == NULL
            || mirFnIndex >= p->currentMirExecCtx->mirProgram->funcLen)
        {
            return 0;
        }
        SLEvalMirInitExecEnv(p, p->currentFile, &env, p->currentMirExecCtx);
        if (!SLMirProgramNeedsDynamicResolution(p->currentMirExecCtx->mirProgram)) {
            SLMirExecEnvDisableDynamicResolution(&env);
        }
        if (SLMirEvalFunction(
                p->arena,
                p->currentMirExecCtx->mirProgram,
                mirFnIndex,
                args,
                argCount,
                &env,
                outValue,
                &mirIsConst)
            != 0)
        {
            return -1;
        }
        SLEvalMirAdaptOutValue(p->currentMirExecCtx, outValue, &mirIsConst);
        if (mirIsConst && outValue->kind == SLCTFEValue_INVALID) {
            SLEvalValueSetNull(outValue);
        }
        *outIsConst = mirIsConst;
        return 1;
    }
    if (!SLEvalValueIsFunctionRef(calleeValue, &fnIndex) || fnIndex >= p->funcLen) {
        return 0;
    }
    fn = &p->funcs[fnIndex];
    if (fn->isBuiltinPackageFn) {
        return 0;
    }
    if (SLEvalInvokeFunction(
            p, (int32_t)fnIndex, args, argCount, p->currentContext, outValue, &didReturn)
        != 0)
    {
        return -1;
    }
    if (!didReturn) {
        if (fn->hasReturnType) {
            *outIsConst = 0;
            return 1;
        }
        SLEvalValueSetNull(outValue);
    }
    *outIsConst = 1;
    return 1;
}

static int SLEvalEvalTopVar(
    SLEvalProgram* p, uint32_t topVarIndex, SLCTFEValue* outValue, int* outIsConst) {
    SLEvalTopVar*       topVar;
    const SLParsedFile* savedFile;
    SLCTFEExecCtx*      savedExecCtx;
    SLCTFEExecCtx       execCtx;
    SLCTFEValue         value;
    int                 isConst = 0;
    int                 rc;
    if (p == NULL || outValue == NULL || outIsConst == NULL || topVarIndex >= p->topVarLen) {
        return -1;
    }
    topVar = &p->topVars[topVarIndex];
    if (topVar->state == SLEvalTopConstState_READY) {
        *outValue = topVar->value;
        *outIsConst = 1;
        return 0;
    }
    if (topVar->state == SLEvalTopConstState_VISITING
        || topVar->state == SLEvalTopConstState_FAILED)
    {
        *outIsConst = 0;
        return 0;
    }

    topVar->state = SLEvalTopConstState_VISITING;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = p->arena;
    execCtx.ast = &topVar->file->ast;
    execCtx.src.ptr = topVar->file->source;
    execCtx.src.len = topVar->file->sourceLen;
    execCtx.evalExpr = SLEvalExecExprCb;
    execCtx.evalExprCtx = p;
    execCtx.evalExprForType = SLEvalExecExprForTypeCb;
    execCtx.evalExprForTypeCtx = p;
    execCtx.zeroInit = SLEvalZeroInitCb;
    execCtx.zeroInitCtx = p;
    execCtx.assignExpr = SLEvalAssignExprCb;
    execCtx.assignExprCtx = p;
    execCtx.assignValueExpr = SLEvalAssignValueExprCb;
    execCtx.assignValueExprCtx = p;
    execCtx.matchPattern = SLEvalMatchPatternCb;
    execCtx.matchPatternCtx = p;
    execCtx.forInIndex = SLEvalForInIndexCb;
    execCtx.forInIndexCtx = p;
    execCtx.forInIter = SLEvalForInIterCb;
    execCtx.forInIterCtx = p;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = SLCTFE_EXEC_DEFAULT_FOR_LIMIT;
    execCtx.skipConstBlocks = 1u;
    SLCTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topVar->file;
    p->currentExecCtx = &execCtx;
    {
        int mirSupported = 0;
        rc = SLEvalTryMirEvalTopInit(
            p,
            topVar->file,
            topVar->initExprNode,
            topVar->declTypeNode,
            topVar->nameStart,
            topVar->nameEnd,
            topVar->file,
            topVar->declTypeNode,
            &value,
            &isConst,
            &mirSupported);
        if (rc == 0 && !mirSupported && topVar->initExprNode < 0 && topVar->declTypeNode >= 0) {
            rc = SLEvalTryMirZeroInitType(
                p,
                topVar->file,
                topVar->declTypeNode,
                topVar->nameStart,
                topVar->nameEnd,
                &value,
                &isConst);
        }
    }
    p->currentExecCtx = savedExecCtx;
    p->currentFile = savedFile;

    if (rc != 0) {
        topVar->state = SLEvalTopConstState_FAILED;
        return -1;
    }
    if (!isConst) {
        topVar->state = SLEvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }
    topVar->value = value;
    topVar->state = SLEvalTopConstState_READY;
    *outValue = value;
    *outIsConst = 1;
    return 0;
}

static int SLEvalEvalTopConst(
    SLEvalProgram* p, uint32_t topConstIndex, SLCTFEValue* outValue, int* outIsConst) {
    SLEvalTopConst*     topConst;
    const SLParsedFile* savedFile;
    SLCTFEExecCtx*      savedExecCtx;
    SLCTFEExecCtx       constExecCtx;
    SLCTFEValue         value;
    int                 isConst = 0;
    int                 rc;
    if (p == NULL || outValue == NULL || outIsConst == NULL || topConstIndex >= p->topConstLen) {
        return -1;
    }
    topConst = &p->topConsts[topConstIndex];
    if (topConst->state == SLEvalTopConstState_READY) {
        *outValue = topConst->value;
        *outIsConst = 1;
        return 0;
    }
    if (topConst->state == SLEvalTopConstState_VISITING
        || topConst->state == SLEvalTopConstState_FAILED)
    {
        *outIsConst = 0;
        return 0;
    }
    if (topConst->initExprNode < 0) {
        topConst->state = SLEvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }

    topConst->state = SLEvalTopConstState_VISITING;
    memset(&constExecCtx, 0, sizeof(constExecCtx));
    constExecCtx.arena = p->arena;
    constExecCtx.ast = &topConst->file->ast;
    constExecCtx.src.ptr = topConst->file->source;
    constExecCtx.src.len = topConst->file->sourceLen;
    constExecCtx.evalExpr = SLEvalExecExprCb;
    constExecCtx.evalExprCtx = p;
    constExecCtx.evalExprForType = SLEvalExecExprForTypeCb;
    constExecCtx.evalExprForTypeCtx = p;
    constExecCtx.zeroInit = SLEvalZeroInitCb;
    constExecCtx.zeroInitCtx = p;
    constExecCtx.assignExpr = SLEvalAssignExprCb;
    constExecCtx.assignExprCtx = p;
    constExecCtx.assignValueExpr = SLEvalAssignValueExprCb;
    constExecCtx.assignValueExprCtx = p;
    constExecCtx.matchPattern = SLEvalMatchPatternCb;
    constExecCtx.matchPatternCtx = p;
    constExecCtx.forInIndex = SLEvalForInIndexCb;
    constExecCtx.forInIndexCtx = p;
    constExecCtx.forInIter = SLEvalForInIterCb;
    constExecCtx.forInIterCtx = p;
    constExecCtx.pendingReturnExprNode = -1;
    constExecCtx.forIterLimit = SLCTFE_EXEC_DEFAULT_FOR_LIMIT;
    constExecCtx.skipConstBlocks = 1u;
    SLCTFEExecResetReason(&constExecCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topConst->file;
    p->currentExecCtx = &constExecCtx;
    {
        rc = SLEvalTypeValueFromExprNode(
            p, topConst->file, &topConst->file->ast, topConst->initExprNode, &value);
        if (rc < 0) {
            p->currentExecCtx = savedExecCtx;
            p->currentFile = savedFile;
            topConst->state = SLEvalTopConstState_FAILED;
            return -1;
        }
        if (rc > 0) {
            rc = 0;
            isConst = 1;
        } else {
            int mirSupported = 0;
            rc = SLEvalTryMirEvalTopInit(
                p,
                topConst->file,
                topConst->initExprNode,
                -1,
                topConst->nameStart,
                topConst->nameEnd,
                NULL,
                -1,
                &value,
                &isConst,
                &mirSupported);
        }
    }
    p->currentExecCtx = savedExecCtx;
    p->currentFile = savedFile;

    if (rc != 0) {
        topConst->state = SLEvalTopConstState_FAILED;
        return -1;
    }
    if (!isConst) {
        topConst->state = SLEvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }

    topConst->value = value;
    topConst->state = SLEvalTopConstState_READY;
    *outValue = value;
    *outIsConst = 1;
    return 0;
}

static int SLEvalResolveIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*   p = (SLEvalProgram*)ctx;
    const SLPackage* currentPkg;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL || p->currentExecCtx == NULL
        || p->currentFile == NULL)
    {
        return -1;
    }
    currentPkg = SLEvalFindPackageByFile(p, p->currentFile);
    {
        SLCTFEExecBinding* binding = SLEvalFindBinding(
            p->currentExecCtx, p->currentFile, nameStart, nameEnd);
        if (binding != NULL) {
            *outValue = binding->value;
            if (binding->typeNode >= 0) {
                int32_t typeCode = SLEvalTypeCode_INVALID;
                if (!SLEvalValueGetRuntimeTypeCode(outValue, &typeCode)
                    && SLEvalTypeCodeFromTypeNode(p->currentFile, binding->typeNode, &typeCode))
                {
                    SLEvalValueSetRuntimeTypeCode(outValue, typeCode);
                }
            }
            *outIsConst = 1;
            return 0;
        }
    }
    if (SLCTFEExecEnvLookup(p->currentExecCtx, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    if (SLEvalMirLookupLocalValue(p, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t typeCode = SLEvalTypeCode_INVALID;
        if (SLEvalBuiltinTypeCode(p->currentFile->source, nameStart, nameEnd, &typeCode)) {
            SLEvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
        if (SLEvalResolveTypeValueName(p, p->currentFile, nameStart, nameEnd, outValue) > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    {
        if (currentPkg != NULL) {
            uint32_t i;
            for (i = 0; i < currentPkg->importLen; i++) {
                const SLImportRef* imp = &currentPkg->imports[i];
                if (imp->bindName == NULL || imp->target == NULL) {
                    continue;
                }
                if (strlen(imp->bindName) == (size_t)(nameEnd - nameStart)
                    && memcmp(
                           imp->bindName,
                           p->currentFile->source + nameStart,
                           (size_t)(nameEnd - nameStart))
                           == 0)
                {
                    int pkgIndex = FindPackageIndex(p->loader, imp->target);
                    if (pkgIndex >= 0) {
                        SLEvalValueSetPackageRef(outValue, (uint32_t)pkgIndex);
                        *outIsConst = 1;
                        return 0;
                    }
                }
            }
        }
    }
    {
        int32_t topVarIndex =
            currentPkg != NULL
                ? SLEvalFindTopVarBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : SLEvalFindTopVarBySlice(p, p->currentFile, nameStart, nameEnd);
        if (topVarIndex >= 0) {
            int isConst = 0;
            if (SLEvalEvalTopVar(p, (uint32_t)topVarIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst) {
                *outIsConst = 1;
                return 0;
            }
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "top-level var is not available in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
    }
    {
        int32_t topConstIndex =
            currentPkg != NULL
                ? SLEvalFindTopConstBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : SLEvalFindTopConstBySlice(p, p->currentFile, nameStart, nameEnd);
        if (topConstIndex >= 0) {
            int isConst = 0;
            if (SLEvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst) {
                *outIsConst = 1;
                return 0;
            }
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "top-level const is not available in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
    }
    {
        int32_t fnIndex =
            currentPkg != NULL
                ? SLEvalFindAnyFunctionBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : SLEvalFindAnyFunctionBySlice(p, p->currentFile, nameStart, nameEnd);
        if (fnIndex >= 0) {
            if (p->currentMirExecCtx != NULL && p->currentMirExecCtx->evalToMir != NULL
                && (uint32_t)fnIndex < p->currentMirExecCtx->evalToMirLen
                && p->currentMirExecCtx->evalToMir[(uint32_t)fnIndex] != UINT32_MAX)
            {
                SLMirValueSetFunctionRef(
                    outValue, p->currentMirExecCtx->evalToMir[(uint32_t)fnIndex]);
            } else {
                SLEvalValueSetFunctionRef(outValue, (uint32_t)fnIndex);
            }
            *outIsConst = 1;
            return 0;
        }
        if (fnIndex == -2) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "function value is ambiguous in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
    }
    SLCTFEExecSetReason(
        p->currentExecCtx, nameStart, nameEnd, "identifier is not available in evaluator backend");
    *outIsConst = 0;
    return 0;
}

static int SLEvalResolveCallMirPre(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    SLCTFEValue* outValue,
    int*         outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    int32_t        callNode = -1;
    (void)function;
    (void)nameStart;
    (void)nameEnd;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!SLEvalMirResolveCallNode(program, inst, &callNode)
        || !SLEvalMirCallNodeIsLazyBuiltin(p, callNode))
    {
        return 0;
    }
    if (SLEvalExecExprCb(p, callNode, outValue, outIsConst) != 0) {
        return -1;
    }
    return 1;
}

static int SLEvalExpandMirSpreadLastArgs(
    SLEvalProgram*        p,
    const SLMirInst* _Nullable inst,
    const SLEvalFunction* fn,
    const SLCTFEValue* _Nullable args,
    uint32_t              argCount,
    const SLCTFEValue**   outArgs,
    uint32_t*             outArgCount) {
    const SLEvalArray* spreadArray;
    SLCTFEValue*       expandedArgs;
    uint32_t           prefixCount;
    uint32_t           i;
    if (outArgs != NULL) {
        *outArgs = args;
    }
    if (outArgCount != NULL) {
        *outArgCount = argCount;
    }
    if (p == NULL || fn == NULL || outArgs == NULL || outArgCount == NULL
        || (argCount > 0 && args == NULL))
    {
        return -1;
    }
    if (inst == NULL || !fn->isVariadic || !SLMirCallTokHasSpreadLast(inst->tok)) {
        return 1;
    }
    if (argCount == 0u) {
        return 0;
    }
    spreadArray = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&args[argCount - 1u]));
    if (spreadArray == NULL || spreadArray->len > UINT32_MAX - (argCount - 1u)) {
        return 0;
    }
    if (spreadArray->len > 0 && spreadArray->elems == NULL) {
        return -1;
    }
    prefixCount = argCount - 1u;
    expandedArgs = (SLCTFEValue*)SLArenaAlloc(
        p->arena,
        sizeof(SLCTFEValue) * (prefixCount + spreadArray->len),
        (uint32_t)_Alignof(SLCTFEValue));
    if (expandedArgs == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < prefixCount; i++) {
        expandedArgs[i] = args[i];
        SLEvalAnnotateUntypedLiteralValue(&expandedArgs[i]);
    }
    for (i = 0; i < spreadArray->len; i++) {
        expandedArgs[prefixCount + i] = spreadArray->elems[i];
        SLEvalAnnotateUntypedLiteralValue(&expandedArgs[prefixCount + i]);
    }
    *outArgs = expandedArgs;
    *outArgCount = prefixCount + spreadArray->len;
    return 1;
}

static int SLEvalResolveCallMir(
    void* ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* _Nullable args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    SLEvalProgram*        p = (SLEvalProgram*)ctx;
    int32_t               fnIndex = -1;
    const SLEvalFunction* fn;
    int                   didReturn = 0;
    int                   isReflectKind = 0;
    int                   isReflectIsAlias = 0;
    int                   isReflectTypeName = 0;
    int                   isReflectBase = 0;
    int                   isTypeOf = 0;
    (void)program;
    (void)function;
    (void)inst;
    (void)diag;

    if (p == NULL || p->currentFile == NULL || p->currentExecCtx == NULL || outValue == NULL
        || outIsConst == NULL || (argCount > 0 && args == NULL))
    {
        return -1;
    }

    isReflectKind = SLEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "kind", "reflect");
    isReflectIsAlias = SLEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "is_alias", "reflect");
    isReflectTypeName = SLEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "type_name", "reflect");
    isReflectBase = SLEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "base", "reflect");
    isTypeOf = SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "typeof");

    if (argCount == 1 && isReflectKind) {
        int32_t kind = 0;
        if (SLEvalTypeKindOfValue(&args[0], &kind)) {
            SLEvalValueSetInt(outValue, kind);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectIsAlias) {
        int32_t kind = 0;
        if (SLEvalTypeKindOfValue(&args[0], &kind)) {
            outValue->kind = SLCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = kind == SLEvalTypeKind_ALIAS ? 1u : 0u;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectTypeName) {
        if (SLEvalTypeNameOfValue((SLCTFEValue*)&args[0], outValue)) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectBase) {
        SLEvalReflectedType* rt = SLEvalValueAsReflectedType(&args[0]);
        if (rt != NULL && rt->kind == SLEvalReflectType_NAMED
            && rt->namedKind == SLEvalTypeKind_ALIAS && rt->file != NULL && rt->nodeId >= 0
            && (uint32_t)rt->nodeId < rt->file->ast.len)
        {
            int32_t baseTypeNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            if (baseTypeNode >= 0
                && SLEvalTypeValueFromTypeNode(p, rt->file, baseTypeNode, outValue) > 0)
            {
                *outIsConst = 1;
                return 0;
            }
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "ptr")) {
        SLEvalReflectedType* rt;
        if (args[0].kind == SLCTFEValue_TYPE) {
            rt = (SLEvalReflectedType*)SLArenaAlloc(
                p->arena, sizeof(SLEvalReflectedType), (uint32_t)_Alignof(SLEvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = SLEvalReflectType_PTR;
            rt->namedKind = SLEvalTypeKind_POINTER;
            rt->elemType = args[0];
            SLEvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "slice")) {
        SLEvalReflectedType* rt;
        if (args[0].kind == SLCTFEValue_TYPE) {
            rt = (SLEvalReflectedType*)SLArenaAlloc(
                p->arena, sizeof(SLEvalReflectedType), (uint32_t)_Alignof(SLEvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = SLEvalReflectType_SLICE;
            rt->namedKind = SLEvalTypeKind_SLICE;
            rt->elemType = args[0];
            SLEvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "array")) {
        int64_t              arrayLen = 0;
        SLEvalReflectedType* rt;
        if (args[0].kind == SLCTFEValue_TYPE && SLCTFEValueToInt64(&args[1], &arrayLen) == 0
            && arrayLen >= 0 && arrayLen <= (int64_t)UINT32_MAX)
        {
            rt = (SLEvalReflectedType*)SLArenaAlloc(
                p->arena, sizeof(SLEvalReflectedType), (uint32_t)_Alignof(SLEvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = SLEvalReflectType_ARRAY;
            rt->namedKind = SLEvalTypeKind_ARRAY;
            rt->arrayLen = (uint32_t)arrayLen;
            rt->elemType = args[0];
            SLEvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "concat")) {
        int concatRc = SLEvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
        if (concatRc < 0) {
            return -1;
        }
        if (concatRc > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "copy")) {
        int copyRc = SLEvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
        if (copyRc < 0) {
            return -1;
        }
        if (copyRc > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isTypeOf) {
        int32_t typeCode = SLEvalTypeCode_INVALID;
        if (args[0].kind == SLCTFEValue_TYPE) {
            SLEvalValueSetSimpleTypeValue(outValue, SLEvalTypeCode_TYPE);
            *outIsConst = 1;
            return 0;
        }
        if (SLEvalTypeCodeFromValue(&args[0], &typeCode)) {
            SLEvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "len")) {
        const SLCTFEValue* value = SLEvalValueTargetOrSelf(&args[0]);
        if (value->kind == SLCTFEValue_STRING) {
            SLEvalValueSetInt(outValue, (int64_t)value->s.len);
            *outIsConst = 1;
            return 0;
        }
        if (value->kind == SLCTFEValue_ARRAY) {
            SLEvalValueSetInt(outValue, (int64_t)value->s.len);
            *outIsConst = 1;
            return 0;
        }
        if (value->kind == SLCTFEValue_NULL) {
            SLEvalValueSetInt(outValue, 0);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "cstr")) {
        const SLCTFEValue* value = SLEvalValueTargetOrSelf(&args[0]);
        if (value->kind == SLCTFEValue_STRING) {
            *outValue = *value;
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount > 0) {
        const SLCTFEValue* baseValue = SLEvalValueTargetOrSelf(&args[0]);
        SLEvalAggregate*   agg = SLEvalValueAsAggregate(baseValue);
        SLCTFEValue        fieldValue;
        if (agg != NULL
            && SLEvalAggregateGetFieldValue(
                agg, p->currentFile->source, nameStart, nameEnd, &fieldValue)
            && SLEvalValueIsInvokableFunctionRef(&fieldValue))
        {
            int invoked = SLEvalInvokeFunctionRef(
                p, &fieldValue, args + 1u, argCount - 1u, outValue, outIsConst);
            if (invoked < 0) {
                return -1;
            }
            if (invoked > 0) {
                return 0;
            }
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "print")) {
        if (args[0].kind == SLCTFEValue_STRING) {
            if (args[0].s.len > 0 && args[0].s.bytes != NULL) {
                if (fwrite(args[0].s.bytes, 1, args[0].s.len, stdout) != args[0].s.len) {
                    return ErrorSimple("failed to write print output");
                }
            }
            fputc('\n', stdout);
            fflush(stdout);
            SLEvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        }
    }
    if ((argCount == 1 || argCount == 2)
        && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "free"))
    {
        SLEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (program != NULL && inst != NULL && argCount > 0) {
        int32_t          callNode = -1;
        const SLPackage* targetPkg = NULL;
        if (SLEvalMirResolveCallNode(program, inst, &callNode)) {
            targetPkg = SLEvalMirResolveQualifiedImportCallTargetPkg(p, callNode);
        }
        if (targetPkg != NULL) {
            fnIndex = SLEvalResolveFunctionBySlice(
                p, targetPkg, p->currentFile, nameStart, nameEnd, args + 1u, argCount - 1u);
            if (fnIndex == -2) {
                SLCTFEExecSetReason(
                    p->currentExecCtx,
                    nameStart,
                    nameEnd,
                    "call target is ambiguous in evaluator backend");
                *outIsConst = 0;
                return 0;
            }
            if (fnIndex >= 0) {
                args++;
                argCount--;
            }
        }
    }
    if (fnIndex < 0 && argCount > 0) {
        uint32_t pkgIndex = 0;
        if (SLEvalValueIsPackageRef(&args[0], &pkgIndex)) {
            const SLPackage* targetPkg = NULL;
            if (p->loader == NULL || pkgIndex >= p->loader->packageLen) {
                return -1;
            }
            targetPkg = &p->loader->packages[pkgIndex];
            fnIndex = SLEvalResolveFunctionBySlice(
                p, targetPkg, p->currentFile, nameStart, nameEnd, args + 1, argCount - 1u);
            if (fnIndex == -2) {
                SLCTFEExecSetReason(
                    p->currentExecCtx,
                    nameStart,
                    nameEnd,
                    "call target is ambiguous in evaluator backend");
                *outIsConst = 0;
                return 0;
            }
            if (fnIndex >= 0) {
                args++;
                argCount--;
            }
        }
    }
    if (fnIndex < 0) {
        SLCTFEValue calleeValue;
        int         calleeIsConst = 0;
        const char* savedReason = p->currentExecCtx->nonConstReason;
        uint32_t    savedStart = p->currentExecCtx->nonConstStart;
        uint32_t    savedEnd = p->currentExecCtx->nonConstEnd;
        if (SLEvalResolveIdent(
                p, nameStart, nameEnd, &calleeValue, &calleeIsConst, p->currentExecCtx->diag)
            != 0)
        {
            return -1;
        }
        if (calleeIsConst && SLEvalValueIsInvokableFunctionRef(&calleeValue)) {
            int invoked = SLEvalInvokeFunctionRef(
                p, &calleeValue, args, argCount, outValue, outIsConst);
            if (invoked < 0) {
                return -1;
            }
            if (invoked > 0) {
                return 0;
            }
        }
        p->currentExecCtx->nonConstReason = savedReason;
        p->currentExecCtx->nonConstStart = savedStart;
        p->currentExecCtx->nonConstEnd = savedEnd;
    }
    if (fnIndex < 0) {
        fnIndex = SLEvalResolveFunctionBySlice(
            p, NULL, p->currentFile, nameStart, nameEnd, args, argCount);
    }
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
    {
        const SLCTFEValue* invokeArgs = args;
        uint32_t           invokeArgCount = argCount;
        int                expandRc = SLEvalExpandMirSpreadLastArgs(
            p, inst, fn, args, argCount, &invokeArgs, &invokeArgCount);
        if (expandRc < 0) {
            return -1;
        }
        if (expandRc == 0) {
            SLCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "spread argument is not supported by evaluator backend");
            *outIsConst = 0;
            return 0;
        }
        args = invokeArgs;
        argCount = invokeArgCount;
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

    if (SLEvalInvokeFunction(p, fnIndex, args, argCount, p->currentContext, outValue, &didReturn)
        != 0)
    {
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

static int SLEvalResolveCall(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const SLCTFEValue* _Nullable args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag) {
    return SLEvalResolveCallMir(
        ctx, NULL, NULL, NULL, nameStart, nameEnd, args, argCount, outValue, outIsConst, diag);
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

    if (n->kind == SLAst_COMPOUND_LIT) {
        return SLEvalEvalCompoundLiteral(
            p, exprNode, p->currentFile, p->currentFile, -1, outValue, outIsConst);
    }

    if (n->kind == SLAst_NEW) {
        return SLEvalEvalNewExpr(p, exprNode, outValue, outIsConst);
    }

    if (n->kind == SLAst_CALL_WITH_CONTEXT) {
        int32_t              callNode = n->firstChild;
        int32_t              overlayNode = callNode >= 0 ? ast->nodes[callNode].nextSibling : -1;
        const SLEvalContext* savedContext;
        SLEvalContext        overlayContext;
        int                  overlayRc;
        if (callNode < 0 || (uint32_t)callNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        overlayRc = SLEvalBuildContextOverlay(p, overlayNode, &overlayContext, p->currentFile);
        if (overlayRc != 1) {
            *outIsConst = 0;
            return overlayRc < 0 ? -1 : 0;
        }
        savedContext = p->currentContext;
        p->currentContext = &overlayContext;
        rc = SLEvalExecExprCb(p, callNode, outValue, outIsConst);
        p->currentContext = savedContext;
        return rc;
    }

    if (n->kind == SLAst_SIZEOF) {
        int32_t     childNode = n->firstChild;
        uint64_t    sizeBytes = 0;
        SLCTFEValue childValue;
        int         childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (n->flags == 1u) {
            if (!SLEvalTypeNodeSize(p->currentFile, childNode, &sizeBytes, 0)) {
                *outIsConst = 0;
                return 0;
            }
        } else {
            if (SLEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
                return -1;
            }
            if (!childIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (childValue.kind == SLCTFEValue_BOOL) {
                sizeBytes = 1u;
            } else if (childValue.kind == SLCTFEValue_INT || childValue.kind == SLCTFEValue_FLOAT) {
                sizeBytes = 8u;
            } else if (childValue.kind == SLCTFEValue_STRING) {
                sizeBytes = (uint64_t)(sizeof(void*) * 2u);
            } else if (childValue.kind == SLCTFEValue_ARRAY) {
                sizeBytes = (uint64_t)childValue.s.len * 8u;
            } else if (
                childValue.kind == SLCTFEValue_REFERENCE || childValue.kind == SLCTFEValue_NULL)
            {
                sizeBytes = (uint64_t)sizeof(void*);
            } else {
                *outIsConst = 0;
                return 0;
            }
        }
        SLEvalValueSetInt(outValue, (int64_t)sizeBytes);
        *outIsConst = 1;
        return 0;
    }

    if (n->kind == SLAst_INDEX && (n->flags & SLAstFlag_INDEX_SLICE) != 0u) {
        int32_t            baseNode = n->firstChild;
        int32_t            extraNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        SLCTFEValue        baseValue;
        const SLCTFEValue* targetValue;
        SLEvalArray*       array;
        SLEvalArray*       view;
        SLCTFEValue        startValue;
        SLCTFEValue        endValue;
        int                baseIsConst = 0;
        int                startIsConst = 0;
        int                endIsConst = 0;
        int64_t            start = 0;
        int64_t            end = -1;
        uint32_t           startIndex;
        uint32_t           endIndex;
        if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
            return -1;
        }
        if (!baseIsConst) {
            *outIsConst = 0;
            return 0;
        }
        targetValue = SLEvalValueTargetOrSelf(&baseValue);
        array = SLEvalValueAsArray(targetValue);
        if ((n->flags & SLAstFlag_INDEX_HAS_START) != 0u) {
            if (extraNode < 0 || (uint32_t)extraNode >= ast->len
                || SLEvalExecExprCb(p, extraNode, &startValue, &startIsConst) != 0)
            {
                return extraNode < 0 ? 0 : -1;
            }
            if (!startIsConst || SLCTFEValueToInt64(&startValue, &start) != 0 || start < 0) {
                *outIsConst = 0;
                return 0;
            }
            extraNode = ast->nodes[extraNode].nextSibling;
        }
        if ((n->flags & SLAstFlag_INDEX_HAS_END) != 0u) {
            if (extraNode < 0 || (uint32_t)extraNode >= ast->len
                || SLEvalExecExprCb(p, extraNode, &endValue, &endIsConst) != 0)
            {
                return extraNode < 0 ? 0 : -1;
            }
            if (!endIsConst || SLCTFEValueToInt64(&endValue, &end) != 0 || end < 0) {
                *outIsConst = 0;
                return 0;
            }
            extraNode = ast->nodes[extraNode].nextSibling;
        }
        if (extraNode >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (array == NULL && targetValue->kind == SLCTFEValue_STRING) {
            int32_t currentTypeCode = SLEvalTypeCode_INVALID;
            startIndex = (uint32_t)start;
            endIndex = end >= 0 ? (uint32_t)end : targetValue->s.len;
            if (startIndex > endIndex || endIndex > targetValue->s.len) {
                *outIsConst = 0;
                return 0;
            }
            *outValue = *targetValue;
            outValue->s.bytes =
                targetValue->s.bytes != NULL ? targetValue->s.bytes + startIndex : NULL;
            outValue->s.len = endIndex - startIndex;
            if (!SLEvalValueGetRuntimeTypeCode(targetValue, &currentTypeCode)) {
                currentTypeCode = SLEvalTypeCode_STR_REF;
            }
            SLEvalValueSetRuntimeTypeCode(outValue, currentTypeCode);
            *outIsConst = 1;
            return 0;
        }
        if (array == NULL) {
            *outIsConst = 0;
            return 0;
        }
        startIndex = (uint32_t)start;
        endIndex = end >= 0 ? (uint32_t)end : array->len;
        if (startIndex > endIndex || endIndex > array->len) {
            *outIsConst = 0;
            return 0;
        }
        view = SLEvalAllocArrayView(
            p,
            targetValue->kind == SLCTFEValue_ARRAY ? array->file : p->currentFile,
            exprNode,
            array->elemTypeNode,
            array->elems + startIndex,
            endIndex - startIndex);
        if (view == NULL) {
            return ErrorSimple("out of memory");
        }
        {
            SLCTFEValue viewValue;
            SLEvalValueSetArray(&viewValue, p->currentFile, exprNode, view);
            return SLEvalAllocReferencedValue(p, &viewValue, outValue, outIsConst);
        }
    }

    if (n->kind == SLAst_INDEX && (n->flags & 0x7u) == 0u) {
        int32_t      baseNode = n->firstChild;
        int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        SLCTFEValue  baseValue;
        SLCTFEValue  indexValue;
        SLEvalArray* array;
        int          baseIsConst = 0;
        int          indexIsConst = 0;
        int64_t      index = 0;
        if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
            goto index_fallback;
        }
        if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || SLEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || SLCTFEValueToInt64(&indexValue, &index) != 0) {
            goto index_fallback;
        }
        array = SLEvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            *outValue = array->elems[(uint32_t)index];
            *outIsConst = 1;
            return 0;
        }
        {
            const SLCTFEValue* targetValue = SLEvalValueTargetOrSelf(&baseValue);
            if (targetValue->kind == SLCTFEValue_STRING && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len)
            {
                SLEvalValueSetInt(outValue, (int64_t)targetValue->s.bytes[(uint32_t)index]);
                SLEvalValueSetRuntimeTypeCode(outValue, SLEvalTypeCode_U8);
                *outIsConst = 1;
                return 0;
            }
        }
        if (index < 0) {
            goto index_fallback;
        }
    index_fallback:;
    }

    if (n->kind == SLAst_FIELD_EXPR) {
        int32_t          baseNode = n->firstChild;
        SLCTFEValue      baseValue;
        int              baseIsConst = 0;
        SLEvalAggregate* agg = NULL;
        if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[baseNode].kind == SLAst_IDENT) {
            if (SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "context")
                && SLEvalCurrentContextField(
                    p, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
            {
                *outIsConst = 1;
                return 0;
            }
            const SLParsedFile* enumFile = NULL;
            int32_t             enumNode = -1;
            enumNode = SLEvalFindNamedEnumDecl(
                p,
                p->currentFile,
                ast->nodes[baseNode].dataStart,
                ast->nodes[baseNode].dataEnd,
                &enumFile);
            if (enumNode >= 0 && enumFile != NULL) {
                int32_t  variantNode = -1;
                uint32_t tagIndex = 0;
                if (SLEvalFindEnumVariant(
                        enumFile,
                        enumNode,
                        p->currentFile->source,
                        n->dataStart,
                        n->dataEnd,
                        &variantNode,
                        &tagIndex))
                {
                    const SLAstNode* variantField = &enumFile->ast.nodes[variantNode];
                    int32_t          valueNode = ASTFirstChild(&enumFile->ast, variantNode);
                    if (valueNode >= 0 && enumFile->ast.nodes[valueNode].kind != SLAst_FIELD
                        && !SLEvalEnumHasPayloadVariants(enumFile, enumNode))
                    {
                        SLCTFEValue enumValue;
                        int         enumIsConst = 0;
                        if (SLEvalExecExprInFileWithType(
                                p,
                                enumFile,
                                p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL,
                                valueNode,
                                enumFile,
                                -1,
                                &enumValue,
                                &enumIsConst)
                            != 0)
                        {
                            return -1;
                        }
                        if (!enumIsConst) {
                            *outIsConst = 0;
                            return 0;
                        }
                        *outValue = enumValue;
                        *outIsConst = 1;
                        return 0;
                    }
                    SLEvalValueSetTaggedEnum(
                        p,
                        outValue,
                        enumFile,
                        enumNode,
                        variantField->dataStart,
                        variantField->dataEnd,
                        tagIndex,
                        NULL);
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
            return -1;
        }
        if (!baseIsConst) {
            *outIsConst = 0;
            return 0;
        }
        {
            const SLCTFEValue* targetValue = SLEvalValueTargetOrSelf(&baseValue);
            const SLCTFEValue* payload = NULL;
            SLEvalTaggedEnum*  tagged = SLEvalValueAsTaggedEnum(targetValue);
            if (targetValue->kind == SLCTFEValue_OPTIONAL
                && SLEvalOptionalPayload(targetValue, &payload) && targetValue->b != 0u
                && payload != NULL)
            {
                targetValue = SLEvalValueTargetOrSelf(payload);
                tagged = SLEvalValueAsTaggedEnum(targetValue);
            }
            if (SliceEqCStr(p->currentFile->source, n->dataStart, n->dataEnd, "len")
                && (targetValue->kind == SLCTFEValue_STRING
                    || targetValue->kind == SLCTFEValue_ARRAY
                    || targetValue->kind == SLCTFEValue_NULL))
            {
                SLEvalValueSetInt(outValue, (int64_t)targetValue->s.len);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(p->currentFile->source, n->dataStart, n->dataEnd, "cstr")
                && targetValue->kind == SLCTFEValue_STRING)
            {
                *outValue = *targetValue;
                *outIsConst = 1;
                return 0;
            }
            if (tagged != NULL && tagged->payload != NULL
                && SLEvalAggregateGetFieldValue(
                    tagged->payload, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
            {
                *outIsConst = 1;
                return 0;
            }
        }
        agg = SLEvalValueAsAggregate(&baseValue);
        if (agg == NULL) {
            agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(&baseValue));
        }
        if (agg == NULL) {
            const SLCTFEValue* targetValue = SLEvalValueTargetOrSelf(&baseValue);
            const SLCTFEValue* payload = NULL;
            if (targetValue->kind == SLCTFEValue_OPTIONAL
                && SLEvalOptionalPayload(targetValue, &payload) && targetValue->b != 0u
                && payload != NULL)
            {
                agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(payload));
            }
        }
        if (agg != NULL
            && SLEvalAggregateGetFieldValue(
                agg, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
        {
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_AND) {
        int32_t      childNode = n->firstChild;
        SLCTFEValue  childValue;
        SLCTFEValue* fieldValuePtr;
        int          childIsConst = 0;
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == SLAst_IDENT)
        {
            SLCTFEExecBinding* binding = SLEvalFindBinding(
                p->currentExecCtx,
                p->currentFile,
                ast->nodes[childNode].dataStart,
                ast->nodes[childNode].dataEnd);
            if (binding != NULL) {
                SLEvalValueSetReference(outValue, &binding->value);
                *outIsConst = 1;
                return 0;
            }
            {
                int32_t topVarIndex = SLEvalFindCurrentTopVarBySlice(
                    p,
                    p->currentFile,
                    ast->nodes[childNode].dataStart,
                    ast->nodes[childNode].dataEnd);
                SLCTFEValue topVarValue;
                int         topVarIsConst = 0;
                if (topVarIndex >= 0
                    && SLEvalEvalTopVar(p, (uint32_t)topVarIndex, &topVarValue, &topVarIsConst) == 0
                    && topVarIsConst)
                {
                    SLEvalValueSetReference(outValue, &p->topVars[(uint32_t)topVarIndex].value);
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == SLAst_FIELD_EXPR)
        {
            fieldValuePtr = SLEvalResolveFieldExprValuePtr(p, p->currentExecCtx, childNode);
            if (fieldValuePtr != NULL) {
                SLEvalValueSetReference(outValue, fieldValuePtr);
                *outIsConst = 1;
                return 0;
            }
        }
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == SLAst_INDEX
            && (ast->nodes[childNode].flags & 0x7u) == 0u)
        {
            int32_t      baseNode = ast->nodes[childNode].firstChild;
            int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
            SLCTFEValue  baseValue;
            SLCTFEValue  indexValue;
            SLEvalArray* array;
            int          baseIsConst = 0;
            int          indexIsConst = 0;
            int64_t      index = 0;
            if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
                goto unary_addr_fallback;
            }
            if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
                || SLEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
            {
                return -1;
            }
            if (!baseIsConst || !indexIsConst || SLCTFEValueToInt64(&indexValue, &index) != 0) {
                goto unary_addr_fallback;
            }
            array = SLEvalValueAsArray(&baseValue);
            if (array == NULL) {
                array = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&baseValue));
            }
            if (array == NULL || index < 0 || (uint64_t)index >= (uint64_t)array->len) {
                goto unary_addr_fallback;
            }
            SLEvalValueSetReference(outValue, &array->elems[(uint32_t)index]);
            *outIsConst = 1;
            return 0;
        unary_addr_fallback:;
        }
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (childValue.kind == SLCTFEValue_AGGREGATE || childValue.kind == SLCTFEValue_NULL
            || childValue.kind == SLCTFEValue_ARRAY)
        {
            *outValue = childValue;
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_MUL) {
        int32_t      childNode = n->firstChild;
        SLCTFEValue  childValue;
        SLCTFEValue* target;
        int          childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        target = SLEvalValueReferenceTarget(&childValue);
        if (target != NULL) {
            *outValue = *target;
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == SLAst_UNWRAP) {
        int32_t            childNode = n->firstChild;
        SLCTFEValue        childValue;
        const SLCTFEValue* payload = NULL;
        int                childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (!SLEvalOptionalPayload(&childValue, &payload)) {
            if (childValue.kind == SLCTFEValue_NULL) {
                return ErrorSimple("unwrap of empty optional in evaluator backend");
            }
            *outValue = childValue;
            *outIsConst = 1;
            return 0;
        }
        if (childValue.b == 0u || payload == NULL) {
            return ErrorSimple("unwrap of empty optional in evaluator backend");
        }
        *outValue = *payload;
        *outIsConst = 1;
        return 0;
    }

    if (n->kind == SLAst_TUPLE_EXPR || n->kind == SLAst_EXPR_LIST) {
        SLCTFEValue elems[256];
        uint32_t    elemCount = AstListCount(ast, exprNode);
        uint32_t    i;
        if (elemCount == 0 || elemCount > 256u) {
            *outIsConst = 0;
            return 0;
        }
        for (i = 0; i < elemCount; i++) {
            int32_t itemNode = AstListItemAt(ast, exprNode, i);
            int     elemIsConst = 0;
            if (itemNode < 0 || SLEvalExecExprCb(p, itemNode, &elems[i], &elemIsConst) != 0) {
                return itemNode < 0 ? 0 : -1;
            }
            if (!elemIsConst) {
                *outIsConst = 0;
                return 0;
            }
        }
        return SLEvalAllocTupleValue(
            p, p->currentFile, exprNode, elems, elemCount, outValue, outIsConst);
    }

    if (n->kind == SLAst_UNARY) {
        int32_t     childNode = n->firstChild;
        SLCTFEValue childValue;
        int         childIsConst = 0;
        int         handled = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        handled = SLEvalEvalUnary((SLTokenKind)n->op, &childValue, outValue, outIsConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            return 0;
        }
    }

    if (n->kind == SLAst_BINARY && (SLTokenKind)n->op != SLTok_ASSIGN) {
        int32_t     lhsNode = n->firstChild;
        int32_t     rhsNode = lhsNode >= 0 ? ast->nodes[lhsNode].nextSibling : -1;
        SLCTFEValue lhsValue;
        SLCTFEValue rhsValue;
        int         lhsIsConst = 0;
        int         rhsIsConst = 0;
        int         handled = 0;
        if (lhsNode < 0 || rhsNode < 0 || ast->nodes[rhsNode].nextSibling >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (SLEvalExecExprCb(p, lhsNode, &lhsValue, &lhsIsConst) != 0
            || SLEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0)
        {
            return -1;
        }
        if (!lhsIsConst || !rhsIsConst) {
            *outIsConst = 0;
            return 0;
        }
        handled = SLEvalEvalBinary(
            p, (SLTokenKind)n->op, &lhsValue, &rhsValue, outValue, outIsConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            return 0;
        }
    }

    if (n->kind == SLAst_CALL) {
        int32_t calleeNode = n->firstChild;
        if (calleeNode < 0 || (uint32_t)calleeNode >= ast->len) {
            if (p->currentExecCtx != NULL) {
                SLCTFEExecSetReasonNode(
                    p->currentExecCtx, exprNode, "call expression is malformed");
            }
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == SLAst_IDENT
            && SliceEqCStr(
                p->currentFile->source,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                "typeof"))
        {
            SLCTFEExecBinding*  binding = NULL;
            const SLParsedFile* localTypeFile = NULL;
            int32_t             argNode = ast->nodes[calleeNode].nextSibling;
            int32_t             argExprNode = argNode;
            int32_t             localTypeNode = -1;
            int32_t             visibleLocalTypeNode = -1;
            SLCTFEValue         argValue;
            int                 argIsConst = 0;
            int32_t             typeCode = SLEvalTypeCode_INVALID;
            if (argNode < 0 || ast->nodes[argNode].nextSibling >= 0) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[argNode].kind == SLAst_CALL_ARG) {
                argExprNode = ast->nodes[argNode].firstChild;
            }
            if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[argExprNode].kind == SLAst_IDENT) {
                binding = SLEvalFindBinding(
                    p->currentExecCtx,
                    p->currentFile,
                    ast->nodes[argExprNode].dataStart,
                    ast->nodes[argExprNode].dataEnd);
                if (binding != NULL && binding->typeNode >= 0
                    && !SLEvalTypeNodeIsAnytype(p->currentFile, binding->typeNode))
                {
                    rc = SLEvalTypeValueFromTypeNode(
                        p, p->currentFile, binding->typeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
                if (SLEvalFindVisibleLocalTypeNodeByName(
                        p->currentFile,
                        ast->nodes[argExprNode].start,
                        ast->nodes[argExprNode].dataStart,
                        ast->nodes[argExprNode].dataEnd,
                        &visibleLocalTypeNode)
                    && visibleLocalTypeNode >= 0
                    && !SLEvalTypeNodeIsAnytype(p->currentFile, visibleLocalTypeNode))
                {
                    rc = SLEvalTypeValueFromTypeNode(
                        p, p->currentFile, visibleLocalTypeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
                if (SLEvalMirLookupLocalTypeNode(
                        p,
                        ast->nodes[argExprNode].dataStart,
                        ast->nodes[argExprNode].dataEnd,
                        &localTypeFile,
                        &localTypeNode)
                    && localTypeFile != NULL && localTypeNode >= 0
                    && !SLEvalTypeNodeIsAnytype(localTypeFile, localTypeNode))
                {
                    rc = SLEvalTypeValueFromTypeNode(p, localTypeFile, localTypeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
            }
            rc = SLEvalTypeValueFromExprNode(p, p->currentFile, ast, argExprNode, outValue);
            if (rc < 0) {
                return -1;
            }
            if (rc > 0) {
                if (outValue->kind == SLCTFEValue_TYPE) {
                    SLEvalValueSetSimpleTypeValue(outValue, SLEvalTypeCode_TYPE);
                }
                *outIsConst = 1;
                return 0;
            }
            if (ast->nodes[argExprNode].kind == SLAst_CAST) {
                int32_t typeNode = ast->nodes[argExprNode].firstChild;
                typeNode = typeNode >= 0 ? ast->nodes[typeNode].nextSibling : -1;
                if (typeNode >= 0
                    && SLEvalTypeCodeFromTypeNode(p->currentFile, typeNode, &typeCode))
                {
                    SLEvalValueSetSimpleTypeValue(outValue, typeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (SLEvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
                return -1;
            }
            if (!argIsConst) {
                *outIsConst = 0;
                return 0;
            }
            SLEvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
            if (!SLEvalTypeCodeFromValue(&argValue, &typeCode)) {
                *outIsConst = 0;
                return 0;
            }
            SLEvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
        if (calleeNode >= 0 && ast->nodes[calleeNode].kind == SLAst_FIELD_EXPR) {
            const SLAstNode* callee = &ast->nodes[calleeNode];
            int32_t          baseNode = callee->firstChild;
            int32_t          argNode = ast->nodes[calleeNode].nextSibling;
            SLCTFEValue      calleeValue;
            int              calleeIsConst = 0;
            if (SLEvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                return -1;
            }
            if (calleeIsConst && SLEvalValueIsInvokableFunctionRef(&calleeValue)) {
                uint32_t     argCount = 0;
                SLCTFEValue* args = NULL;
                int collectRc = SLEvalCollectCallArgs(p, ast, argNode, &args, &argCount, NULL);
                if (collectRc <= 0) {
                    *outIsConst = 0;
                    return collectRc < 0 ? -1 : 0;
                }
                {
                    int invoked = SLEvalInvokeFunctionRef(
                        p, &calleeValue, args, argCount, outValue, outIsConst);
                    if (invoked < 0) {
                        return -1;
                    }
                    if (invoked > 0) {
                        return 0;
                    }
                }
            }
            if (baseNode >= 0 && argNode >= 0 && ast->nodes[argNode].nextSibling < 0
                && ast->nodes[baseNode].kind == SLAst_IDENT
                && SliceEqCStr(
                    p->currentFile->source, callee->dataStart, callee->dataEnd, "span_of")
                && SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "reflect"))
            {
                int32_t operandNode = argNode;
                if (ast->nodes[operandNode].kind == SLAst_CALL_ARG) {
                    operandNode = ast->nodes[operandNode].firstChild;
                }
                if (operandNode < 0 || (uint32_t)operandNode >= ast->len) {
                    if (p->currentExecCtx != NULL) {
                        SLCTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[argNode].start,
                            ast->nodes[argNode].end,
                            "reflect.span_of argument is malformed");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                SLEvalValueSetSpan(
                    p->currentFile,
                    ast->nodes[operandNode].start,
                    ast->nodes[operandNode].end,
                    outValue);
                *outIsConst = 1;
                return 0;
            }
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
            if (baseNode >= 0) {
                uint32_t     extraArgCount = 0;
                SLCTFEValue* args = NULL;
                SLCTFEValue* extraArgs = NULL;
                SLCTFEValue  baseValue;
                int          baseIsConst = 0;
                int          collectRc;
                if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
                    return -1;
                }
                if (baseIsConst) {
                    collectRc = SLEvalCollectCallArgs(
                        p, ast, argNode, &extraArgs, &extraArgCount, NULL);
                    if (collectRc <= 0) {
                        *outIsConst = 0;
                        return collectRc < 0 ? -1 : 0;
                    }
                    args = (SLCTFEValue*)SLArenaAlloc(
                        p->arena,
                        sizeof(SLCTFEValue) * (extraArgCount + 1u),
                        (uint32_t)_Alignof(SLCTFEValue));
                    if (args == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    args[0] = baseValue;
                    if (extraArgCount > 0 && extraArgs != NULL) {
                        memcpy(args + 1u, extraArgs, sizeof(SLCTFEValue) * extraArgCount);
                    }

                    if (extraArgCount == 0
                        && SliceEqCStr(
                            p->currentFile->source, callee->dataStart, callee->dataEnd, "len")
                        && (SLEvalValueTargetOrSelf(&baseValue)->kind == SLCTFEValue_STRING
                            || SLEvalValueTargetOrSelf(&baseValue)->kind == SLCTFEValue_ARRAY
                            || SLEvalValueTargetOrSelf(&baseValue)->kind == SLCTFEValue_NULL))
                    {
                        SLEvalValueSetInt(
                            outValue, (int64_t)SLEvalValueTargetOrSelf(&baseValue)->s.len);
                        *outIsConst = 1;
                        return 0;
                    }
                    if (extraArgCount == 0
                        && SliceEqCStr(
                            p->currentFile->source, callee->dataStart, callee->dataEnd, "cstr")
                        && SLEvalValueTargetOrSelf(&baseValue)->kind == SLCTFEValue_STRING)
                    {
                        *outValue = *SLEvalValueTargetOrSelf(&baseValue);
                        *outIsConst = 1;
                        return 0;
                    }

                    if (SLEvalResolveCall(
                            p,
                            callee->dataStart,
                            callee->dataEnd,
                            args,
                            extraArgCount + 1u,
                            outValue,
                            outIsConst,
                            NULL)
                        != 0)
                    {
                        return -1;
                    }
                    if (!*outIsConst && p->currentExecCtx != NULL
                        && p->currentExecCtx->nonConstReason == NULL)
                    {
                        SLCTFEExecSetReasonNode(
                            p->currentExecCtx,
                            exprNode,
                            "qualified call target is not supported by evaluator backend");
                    }
                    return 0;
                }
            }
            if (p->currentExecCtx != NULL) {
                SLCTFEExecSetReasonNode(
                    p->currentExecCtx,
                    exprNode,
                    "qualified call target is not supported by evaluator backend");
            }
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == SLAst_IDENT) {
            int32_t      argNode = ast->nodes[calleeNode].nextSibling;
            uint32_t     argCount = 0;
            SLCTFEValue* args = NULL;
            SLCTFEValue  tempArgs[256];
            int32_t      directFnIndex = -1;
            SLCTFEValue  calleeValue;
            int          calleeIsConst = 0;
            int          calleeMayResolveByNameWithoutValue = 0;
            int32_t      scanNode;
            uint32_t     rawArgCount = 0;
            for (scanNode = argNode; scanNode >= 0; scanNode = ast->nodes[scanNode].nextSibling) {
                rawArgCount++;
            }
            directFnIndex = SLEvalResolveFunctionBySlice(
                p,
                NULL,
                p->currentFile,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                NULL,
                rawArgCount);
            scanNode = argNode;
            while (scanNode >= 0) {
                SLCTFEValue         argValue;
                int                 argIsConst = 0;
                int32_t             argExprNode = scanNode;
                int32_t             paramTypeNode = -1;
                const SLParsedFile* paramTypeFile = p->currentFile;
                if (ast->nodes[scanNode].kind == SLAst_CALL_ARG) {
                    argExprNode = ast->nodes[scanNode].firstChild;
                }
                if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
                    if (p->currentExecCtx != NULL) {
                        SLCTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[scanNode].start,
                            ast->nodes[scanNode].end,
                            "call argument is malformed");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                if (directFnIndex >= 0) {
                    const SLEvalFunction* directFn = &p->funcs[(uint32_t)directFnIndex];
                    uint32_t              fixedCount =
                        directFn->isVariadic && directFn->paramCount > 0
                            ? directFn->paramCount - 1u
                            : directFn->paramCount;
                    if (!(ast->nodes[scanNode].kind == SLAst_CALL_ARG
                          && (ast->nodes[scanNode].flags & SLAstFlag_CALL_ARG_SPREAD) != 0)
                        && (!directFn->isVariadic || argCount < fixedCount))
                    {
                        paramTypeNode = SLEvalFunctionParamTypeNodeAt(directFn, argCount);
                        paramTypeFile = directFn->file;
                        if (paramTypeNode >= 0
                            && directFn->file->ast.nodes[paramTypeNode].kind == SLAst_TYPE_NAME
                            && SliceEqCStr(
                                directFn->file->source,
                                directFn->file->ast.nodes[paramTypeNode].dataStart,
                                directFn->file->ast.nodes[paramTypeNode].dataEnd,
                                "anytype"))
                        {
                            paramTypeNode = -1;
                        }
                    }
                }
                if (paramTypeNode >= 0) {
                    if (SLEvalExecExprWithTypeNode(
                            p, argExprNode, paramTypeFile, paramTypeNode, &argValue, &argIsConst)
                        != 0)
                    {
                        return -1;
                    }
                } else if (SLEvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
                    return -1;
                }
                if (!argIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                if (paramTypeNode >= 0 && SLEvalExprIsAnytypePackIndex(p, ast, argExprNode)
                    && !SLEvalValueMatchesExpectedTypeNode(
                        p, paramTypeFile, paramTypeNode, &argValue))
                {
                    if (p->currentExecCtx != NULL) {
                        SLCTFEExecSetReasonNode(
                            p->currentExecCtx, argExprNode, "anytype pack element type mismatch");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                SLEvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
                if (ast->nodes[scanNode].kind == SLAst_CALL_ARG
                    && (ast->nodes[scanNode].flags & SLAstFlag_CALL_ARG_SPREAD) != 0)
                {
                    SLEvalArray* array = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&argValue));
                    uint32_t     i;
                    if (array == NULL) {
                        *outIsConst = 0;
                        return 0;
                    }
                    if (array->len > 256u - argCount) {
                        return ErrorSimple("too many call arguments for evaluator backend");
                    }
                    for (i = 0; i < array->len; i++) {
                        tempArgs[argCount++] = array->elems[i];
                    }
                } else {
                    if (argCount >= 256u) {
                        return ErrorSimple("too many call arguments for evaluator backend");
                    }
                    tempArgs[argCount++] = argValue;
                }
                scanNode = ast->nodes[scanNode].nextSibling;
            }
            if (argCount > 0) {
                args = (SLCTFEValue*)SLArenaAlloc(
                    p->arena, sizeof(SLCTFEValue) * argCount, (uint32_t)_Alignof(SLCTFEValue));
                if (args == NULL) {
                    return ErrorSimple("out of memory");
                }
                memcpy(args, tempArgs, sizeof(SLCTFEValue) * argCount);
            }
            {
                int32_t resolvedFnIndex = SLEvalResolveFunctionBySlice(
                    p,
                    NULL,
                    p->currentFile,
                    ast->nodes[calleeNode].dataStart,
                    ast->nodes[calleeNode].dataEnd,
                    args,
                    argCount);
                if (resolvedFnIndex >= 0 || directFnIndex < 0) {
                    directFnIndex = resolvedFnIndex;
                }
            }
            if (directFnIndex >= 0 && argCount > 0) {
                (void)SLEvalReorderFixedCallArgsByName(
                    p, &p->funcs[(uint32_t)directFnIndex], ast, argNode, args, argCount, 0u);
            }
            calleeMayResolveByNameWithoutValue =
                directFnIndex >= 0
                || SLEvalNameIsLazyTypeBuiltin(
                    p->currentFile->source,
                    ast->nodes[calleeNode].dataStart,
                    ast->nodes[calleeNode].dataEnd)
                || SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[calleeNode].dataStart,
                    ast->nodes[calleeNode].dataEnd,
                    "len")
                || SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[calleeNode].dataStart,
                    ast->nodes[calleeNode].dataEnd,
                    "concat")
                || SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[calleeNode].dataStart,
                    ast->nodes[calleeNode].dataEnd,
                    "copy")
                || SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[calleeNode].dataStart,
                    ast->nodes[calleeNode].dataEnd,
                    "free");
            {
                const char* savedReason =
                    p->currentExecCtx != NULL ? p->currentExecCtx->nonConstReason : NULL;
                uint32_t savedStart =
                    p->currentExecCtx != NULL ? p->currentExecCtx->nonConstStart : 0;
                uint32_t savedEnd = p->currentExecCtx != NULL ? p->currentExecCtx->nonConstEnd : 0;
                if (SLEvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                    return -1;
                }
                if (calleeIsConst && SLEvalValueIsInvokableFunctionRef(&calleeValue)) {
                    int invoked = SLEvalInvokeFunctionRef(
                        p, &calleeValue, args, argCount, outValue, outIsConst);
                    if (invoked < 0) {
                        return -1;
                    }
                    if (invoked > 0) {
                        return 0;
                    }
                }
                if (calleeMayResolveByNameWithoutValue && p->currentExecCtx != NULL) {
                    p->currentExecCtx->nonConstReason = savedReason;
                    p->currentExecCtx->nonConstStart = savedStart;
                    p->currentExecCtx->nonConstEnd = savedEnd;
                }
            }
            if (SLEvalResolveCall(
                    p,
                    ast->nodes[calleeNode].dataStart,
                    ast->nodes[calleeNode].dataEnd,
                    args,
                    argCount,
                    outValue,
                    outIsConst,
                    NULL)
                != 0)
            {
                return -1;
            }
            if (!*outIsConst && p->currentExecCtx != NULL
                && p->currentExecCtx->nonConstReason == NULL)
            {
                SLCTFEExecSetReasonNode(
                    p->currentExecCtx, exprNode, "call is not supported by evaluator backend");
            }
            return 0;
        }
    }
    if (n->kind == SLAst_CAST) {
        int32_t  valueNode = n->firstChild;
        int32_t  typeNode = valueNode >= 0 ? ast->nodes[valueNode].nextSibling : -1;
        int32_t  extraNode = typeNode >= 0 ? ast->nodes[typeNode].nextSibling : -1;
        char     aliasTargetKind = '\0';
        uint64_t aliasTag = 0;
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0
            && SLEvalResolveSimpleAliasCastTarget(
                p, p->currentFile, typeNode, &aliasTargetKind, &aliasTag))
        {
            SLCTFEValue inValue;
            int         inIsConst = 0;
            if (SLEvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (aliasTargetKind == 'i' && inValue.kind == SLCTFEValue_INT) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 'f' && inValue.kind == SLCTFEValue_FLOAT) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 'b' && inValue.kind == SLCTFEValue_BOOL) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 's' && inValue.kind == SLCTFEValue_STRING) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
        }
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0) {
            SLCTFEValue         inValue;
            int                 inIsConst = 0;
            const SLParsedFile* aliasFile = NULL;
            int32_t             aliasNode = -1;
            int32_t             aliasTargetNode = -1;
            SLEvalArray*        tuple;
            if (SLEvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (SLEvalResolveAliasCastTargetNode(
                    p, p->currentFile, typeNode, &aliasFile, &aliasNode, &aliasTargetNode)
                && aliasFile != NULL && aliasNode >= 0 && aliasTargetNode >= 0)
            {
                tuple = SLEvalValueAsArray(SLEvalValueTargetOrSelf(&inValue));
                if (tuple != NULL && aliasFile->ast.nodes[aliasTargetNode].kind == SLAst_TYPE_TUPLE
                    && AstListCount(&aliasFile->ast, aliasTargetNode) == tuple->len)
                {
                    *outValue = inValue;
                    outValue->typeTag = SLEvalMakeAliasTag(aliasFile, aliasNode);
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0) {
            SLCTFEValue inValue;
            int         inIsConst = 0;
            int32_t     targetTypeCode = SLEvalTypeCode_INVALID;
            uint64_t    nullTypeTag = 0;
            if (SLEvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            (void)SLEvalTypeCodeFromTypeNode(p->currentFile, typeNode, &targetTypeCode);
            if (inValue.kind == SLCTFEValue_NULL
                && SLEvalResolveNullCastTypeTag(p->currentFile, typeNode, &nullTypeTag))
            {
                *outValue = inValue;
                outValue->typeTag = nullTypeTag;
                *outIsConst = 1;
                return 0;
            }
            if (targetTypeCode == SLEvalTypeCode_BOOL) {
                outValue->kind = SLCTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (inValue.kind == SLCTFEValue_BOOL) {
                    outValue->b = inValue.b ? 1u : 0u;
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == SLCTFEValue_INT) {
                    outValue->b = inValue.i64 != 0 ? 1u : 0u;
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == SLCTFEValue_FLOAT) {
                    outValue->b = inValue.f64 != 0.0 ? 1u : 0u;
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == SLCTFEValue_STRING) {
                    outValue->b = 1u;
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == SLCTFEValue_NULL) {
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (targetTypeCode == SLEvalTypeCode_F32 || targetTypeCode == SLEvalTypeCode_F64) {
                outValue->kind = SLCTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (inValue.kind == SLCTFEValue_FLOAT) {
                    outValue->f64 = inValue.f64;
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == SLCTFEValue_INT) {
                    outValue->f64 = (double)inValue.i64;
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == SLCTFEValue_BOOL) {
                    outValue->f64 = inValue.b ? 1.0 : 0.0;
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == SLCTFEValue_NULL) {
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (targetTypeCode == SLEvalTypeCode_U8 || targetTypeCode == SLEvalTypeCode_U16
                || targetTypeCode == SLEvalTypeCode_U32 || targetTypeCode == SLEvalTypeCode_U64
                || targetTypeCode == SLEvalTypeCode_UINT || targetTypeCode == SLEvalTypeCode_I8
                || targetTypeCode == SLEvalTypeCode_I16 || targetTypeCode == SLEvalTypeCode_I32
                || targetTypeCode == SLEvalTypeCode_I64 || targetTypeCode == SLEvalTypeCode_INT)
            {
                int64_t asInt = 0;
                int     canCast = 1;
                if (inValue.kind == SLCTFEValue_INT) {
                    asInt = inValue.i64;
                } else if (inValue.kind == SLCTFEValue_BOOL) {
                    asInt = inValue.b ? 1 : 0;
                } else if (inValue.kind == SLCTFEValue_FLOAT) {
                    if (inValue.f64 != inValue.f64 || inValue.f64 > (double)INT64_MAX
                        || inValue.f64 < (double)INT64_MIN)
                    {
                        *outIsConst = 0;
                        return 0;
                    }
                    asInt = (int64_t)inValue.f64;
                } else if (inValue.kind == SLCTFEValue_NULL) {
                    asInt = 0;
                } else {
                    canCast = 0;
                }
                if (canCast) {
                    SLEvalValueSetInt(outValue, asInt);
                    SLEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == SLAst_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == SLAst_TYPE_PTR)
                && p->currentFile->ast.nodes[typeNode].firstChild >= 0
                && (uint32_t)p->currentFile->ast.nodes[typeNode].firstChild < ast->len
                && p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild].kind
                       == SLAst_TYPE_NAME
                && SliceEqCStr(
                    p->currentFile->source,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataStart,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataEnd,
                    "str")
                && inValue.kind == SLCTFEValue_STRING)
            {
                *outValue = inValue;
                SLEvalValueSetRuntimeTypeCode(
                    outValue,
                    p->currentFile->ast.nodes[typeNode].kind == SLAst_TYPE_REF
                        ? SLEvalTypeCode_STR_REF
                        : SLEvalTypeCode_STR_PTR);
                *outIsConst = 1;
                return 0;
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == SLAst_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == SLAst_TYPE_PTR)
                && p->currentFile->ast.nodes[typeNode].firstChild >= 0
                && (uint32_t)p->currentFile->ast.nodes[typeNode].firstChild < ast->len
                && p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild].kind
                       == SLAst_TYPE_NAME
                && SliceEqCStr(
                    p->currentFile->source,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataStart,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataEnd,
                    "str")
                && SLEvalValueAsArray(SLEvalValueTargetOrSelf(&inValue)) != NULL)
            {
                rc = SLEvalStringValueFromArrayBytes(
                    p->arena,
                    &inValue,
                    p->currentFile->ast.nodes[typeNode].kind == SLAst_TYPE_REF
                        ? SLEvalTypeCode_STR_REF
                        : SLEvalTypeCode_STR_PTR,
                    outValue);
                if (rc < 0) {
                    return -1;
                }
                if (rc > 0) {
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
    }

    rc = SLCTFEEvalExprEx(
        p->arena,
        ast,
        (SLStrView){ p->currentFile->source, p->currentFile->sourceLen },
        exprNode,
        SLEvalResolveIdent,
        SLEvalResolveCall,
        p,
        SLEvalMirMakeTuple,
        p,
        SLEvalMirIndexValue,
        p,
        SLEvalMirAggGetField,
        p,
        SLEvalMirAggAddrField,
        p,
        outValue,
        outIsConst,
        &diag);
    if (rc != 0) {
        if (diag.code != SLDiag_NONE) {
            PrintSLDiag(p->currentFile->path, p->currentFile->source, &diag, 1);
        } else {
            ErrorEvalUnsupported(
                p->currentFile->path,
                p->currentFile->source,
                ast->nodes[exprNode].start,
                ast->nodes[exprNode].end,
                "failed to evaluate expression");
        }
        return -1;
    }
    if (!*outIsConst && p->currentExecCtx != NULL && p->currentExecCtx->nonConstReason == NULL) {
        SLCTFEExecSetReasonNode(
            p->currentExecCtx, exprNode, "expression is not supported by evaluator backend");
    }
    return 0;
}

int RunProgramEval(const char* entryPath, const char* _Nullable platformTarget) {
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
    program.loader = &loader;
    program.entryPkg = entryPkg;
    SLEvalValueSetInt(&program.rootContext.mem, 1);
    SLEvalValueSetInt(&program.rootContext.tempMem, 2);
    SLEvalValueSetInt(&program.rootContext.log, 3);
    program.currentContext = NULL;
    if (SLEvalCollectFunctions(&program) != 0) {
        goto end;
    }
    if (SLEvalCollectTopConsts(&program) != 0) {
        goto end;
    }
    if (SLEvalCollectTopVars(&program) != 0) {
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
        rc = ErrorEvalUnsupported(
            mainFn->file->path,
            mainFn->file->source,
            mainFn->file->ast.nodes[mainFn->fnNode].start,
            mainFn->file->ast.nodes[mainFn->fnNode].end,
            "entrypoint cannot have a context clause in evaluator backend");
        goto end;
    }

    SLEvalValueSetNull(&noArgsValue);
    if (SLEvalInvokeFunction(
            &program, mainIndex, &noArgsValue, 0, &program.rootContext, &retValue, &didReturn)
        != 0)
    {
        goto end;
    }
    rc = program.exitCalled ? program.exitCode : 0;

end:
    free(program.funcs);
    free(program.topConsts);
    free(program.topVars);
    SLArenaDispose(&arena);
    FreeLoader(&loader);
    return rc;
}

SL_API_END
