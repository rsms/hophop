#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctfe.h"
#include "ctfe_exec.h"
#include "evaluator.h"
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
    char* alias;
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
int ValidateEntryMainSignature(const SLPackage* entryPkg);
int FindPackageIndex(const SLPackageLoader* loader, const SLPackage* pkg);

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

static int IsFnReturnTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE;
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
    uint8_t             _reserved[1];
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
    SLCTFEValue*        elems;
    uint32_t            len;
} SLEvalArray;

typedef struct {
    SLCTFEValue mem;
    SLCTFEValue tempMem;
    SLCTFEValue log;
} SLEvalContext;

typedef struct {
    const SLParsedFile* file;
    int32_t             enumNode;
    uint32_t            variantNameStart;
    uint32_t            variantNameEnd;
    uint32_t            tagIndex;
    SLEvalAggregate* _Nullable payload;
} SLEvalTaggedEnum;

#define SL_EVAL_PACKAGE_REF_TAG_FLAG  (UINT64_C(1) << 63)
#define SL_EVAL_NULL_FIXED_LEN_TAG    (UINT64_C(1) << 62)
#define SL_EVAL_FUNCTION_REF_TAG_FLAG (UINT64_C(1) << 61)
#define SL_EVAL_TAGGED_ENUM_TAG_FLAG  (UINT64_C(1) << 60)

typedef struct {
    SLArena* _Nonnull arena;
    const SLPackageLoader* loader;
    const SLPackage*       entryPkg;
    const SLParsedFile*    currentFile;
    SLCTFEExecCtx*         currentExecCtx;
    SLEvalFunction*        funcs;
    uint32_t               funcLen;
    uint32_t               funcCap;
    SLEvalTopConst*        topConsts;
    uint32_t               topConstLen;
    uint32_t               topConstCap;
    SLEvalTopVar*          topVars;
    uint32_t               topVarLen;
    uint32_t               topVarCap;
    uint32_t               callDepth;
    uint32_t               callStack[SL_EVAL_CALL_MAX_DEPTH];
    SLEvalContext          rootContext;
    const SLEvalContext*   currentContext;
    int                    exitCalled;
    int                    exitCode;
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

static uint64_t SLEvalMakeAggregateTag(const SLParsedFile* file, int32_t nodeId) {
    uint64_t tag = 0;
    if (file != NULL) {
        tag = (uint64_t)(uintptr_t)file;
    }
    tag ^= (uint64_t)(uint32_t)(nodeId + 1) << 2;
    return tag & ~(SL_EVAL_PACKAGE_REF_TAG_FLAG | SL_EVAL_NULL_FIXED_LEN_TAG);
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
static int SLEvalExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);

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

static const SLCTFEValue* SLEvalValueTargetOrSelf(const SLCTFEValue* value) {
    const SLCTFEValue* target = SLEvalValueReferenceTarget(value);
    return target != NULL ? target : value;
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
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
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
        SLEvalAggregate* agg = SLEvalValueAsAggregate(value);
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
    fieldNode = ASTFirstChild(&p->currentFile->ast, compoundLitNode);
    if (fieldNode >= 0 && IsFnReturnTypeNodeKind(p->currentFile->ast.nodes[fieldNode].kind)) {
        fieldNode = ASTNextSibling(&p->currentFile->ast, fieldNode);
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
    if (aggregateDecl->kind != SLAst_STRUCT && aggregateDecl->kind != SLAst_UNION) {
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

static int SLEvalZeroInitTypeNode(
    const SLEvalProgram* p,
    const SLParsedFile*  file,
    int32_t              typeNode,
    SLCTFEValue*         outValue,
    int*                 outIsConst) {
    const SLAstNode*    typeNameNode;
    const SLPackage*    currentPkg;
    const SLParsedFile* aggregateFile = NULL;
    int32_t             aggregateNode = -1;
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
        case SLAst_TYPE_NAME:
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "bool")) {
                outValue->kind = SLCTFEValue_BOOL;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
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
                SLEvalValueSetInt(outValue, 0);
                *outIsConst = 1;
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
            return 0;
        case SLAst_TYPE_PTR:
        case SLAst_TYPE_REF:
        case SLAst_TYPE_MUTREF:
            SLEvalValueSetNull(outValue);
            if (SLEvalResolveNullCastTypeTag(file, typeNode, &nullTag)) {
                outValue->typeTag = nullTag;
            }
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
                    fn._reserved[0] = 0;
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
    SLEvalAggregate*    agg = SLEvalValueAsAggregate(value);
    if (outDistance != NULL) {
        *outDistance = 0;
    }
    if (p == NULL || callerFile == NULL || agg == NULL) {
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
    curFile = agg->file;
    curNode = agg->nodeId;
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
    if (outScore == NULL) {
        return 0;
    }
    *outScore = 0;
    if (p == NULL || fn == NULL || args == NULL || fn->paramCount != argCount) {
        return 0;
    }
    for (i = 0; i < argCount; i++) {
        char     argKind = '\0';
        char     paramKind = '\0';
        uint64_t argAliasTag = 0;
        uint64_t paramAliasTag = 0;
        uint32_t structDistance = 0;
        int32_t  paramTypeNode = SLEvalFunctionParamTypeNodeAt(fn, i);
        if (paramTypeNode < 0) {
            return 0;
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
        if (fn->paramCount != argCount) {
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
        if (fn->paramCount != argCount) {
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
        *outEqual = lhs->payload == rhs->payload;
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
        *outIsConst = 0;
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
    return SLEvalExecExprCb(p, exprNode, outValue, outIsConst);
}

static int SLEvalExecExprCb(void* ctx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalAssignExprCb(
    void* ctx, SLCTFEExecCtx* execCtx, int32_t exprNode, SLCTFEValue* outValue, int* outIsConst);
static int SLEvalZeroInitCb(void* ctx, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst);
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
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst,
    SLDiag* _Nullable diag);
static int SLEvalEvalTopConst(
    SLEvalProgram* p, uint32_t topConstIndex, SLCTFEValue* outValue, int* outIsConst);

static SLCTFEExecBinding* _Nullable SLEvalFindBinding(
    const SLCTFEExecCtx* execCtx, const SLParsedFile* file, uint32_t nameStart, uint32_t nameEnd) {
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

static int SLEvalZeroInitCb(void* ctx, int32_t typeNode, SLCTFEValue* outValue, int* outIsConst) {
    SLEvalProgram* p = (SLEvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL) {
        return -1;
    }
    return SLEvalZeroInitTypeNode(p, p->currentFile, typeNode, outValue, outIsConst);
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
        int32_t topVarIndex = SLEvalFindTopVarBySlice(
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
        if (array == NULL || index < 0 || (uint64_t)index >= (uint64_t)array->len) {
            *outIsConst = 0;
            return 0;
        }
        array->elems[(uint32_t)index] = rhsValue;
        *outValue = rhsValue;
        *outIsConst = 1;
        return 0;
    }
    if (ast->nodes[lhsNode].kind == SLAst_UNARY && (SLTokenKind)ast->nodes[lhsNode].op == SLTok_MUL)
    {
        int32_t      refNode = ast->nodes[lhsNode].firstChild;
        SLCTFEValue  refValue;
        SLCTFEValue* target;
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
                binding = SLEvalFindBinding(
                    execCtx,
                    p->currentFile,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd);
                if (binding == NULL || !binding->mutable) {
                    *outIsConst = 0;
                    return 0;
                }
                agg = SLEvalValueAsAggregate(&binding->value);
                if (agg == NULL) {
                    agg = SLEvalValueAsAggregate(SLEvalValueTargetOrSelf(&binding->value));
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
                        switch ((SLTokenKind)expr->op) {
                            case SLTok_ADD_ASSIGN: binaryOp = SLTok_ADD; break;
                            case SLTok_SUB_ASSIGN: binaryOp = SLTok_SUB; break;
                            case SLTok_MUL_ASSIGN: binaryOp = SLTok_MUL; break;
                            case SLTok_DIV_ASSIGN: binaryOp = SLTok_DIV; break;
                            case SLTok_MOD_ASSIGN: binaryOp = SLTok_MOD; break;
                            default:               *outIsConst = 0; return 0;
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
        int32_t topVarIndex = SLEvalFindTopVarBySlice(
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
        if (array == NULL || index < 0 || (uint64_t)index >= (uint64_t)array->len) {
            *outIsConst = 0;
            return 0;
        }
        array->elems[(uint32_t)index] = *inValue;
        *outValue = *inValue;
        *outIsConst = 1;
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

static int SLEvalInvokeFunction(
    SLEvalProgram*       p,
    int32_t              fnIndex,
    const SLCTFEValue*   args,
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

    if (p == NULL || outValue == NULL || outDidReturn == NULL || fnIndex < 0
        || (uint32_t)fnIndex >= p->funcLen)
    {
        return -1;
    }
    fn = &p->funcs[fnIndex];
    ast = &fn->file->ast;
    if (argCount != fn->paramCount) {
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
            int32_t paramTypeNode = ASTFirstChild(ast, child);
            paramBindings[paramIndex].nameStart = n->dataStart;
            paramBindings[paramIndex].nameEnd = n->dataEnd;
            paramBindings[paramIndex].typeId = -1;
            paramBindings[paramIndex].typeNode = paramTypeNode;
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

    rc = SLCTFEExecEvalBlock(&execCtx, fn->bodyNode, outValue, outDidReturn, &isConst);

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
    }
    return 0;
}

static int SLEvalInvokeFunctionRef(
    SLEvalProgram*     p,
    const SLCTFEValue* calleeValue,
    const SLCTFEValue* args,
    uint32_t           argCount,
    SLCTFEValue*       outValue,
    int*               outIsConst) {
    uint32_t              fnIndex = 0;
    int                   didReturn = 0;
    const SLEvalFunction* fn;
    if (p == NULL || calleeValue == NULL || outValue == NULL || outIsConst == NULL
        || !SLEvalValueIsFunctionRef(calleeValue, &fnIndex) || fnIndex >= p->funcLen)
    {
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
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = SLCTFE_EXEC_DEFAULT_FOR_LIMIT;
    execCtx.skipConstBlocks = 1u;
    SLCTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topVar->file;
    p->currentExecCtx = &execCtx;
    if (topVar->initExprNode >= 0) {
        rc = SLEvalExecExprWithTypeNode(
            p, topVar->initExprNode, topVar->file, topVar->declTypeNode, &value, &isConst);
    } else if (topVar->declTypeNode >= 0) {
        rc = SLEvalZeroInitTypeNode(p, topVar->file, topVar->declTypeNode, &value, &isConst);
    } else {
        rc = 0;
        isConst = 0;
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
    constExecCtx.pendingReturnExprNode = -1;
    constExecCtx.forIterLimit = SLCTFE_EXEC_DEFAULT_FOR_LIMIT;
    constExecCtx.skipConstBlocks = 1u;
    SLCTFEExecResetReason(&constExecCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topConst->file;
    p->currentExecCtx = &constExecCtx;
    rc = SLCTFEEvalExpr(
        p->arena,
        &topConst->file->ast,
        (SLStrView){ topConst->file->source, topConst->file->sourceLen },
        topConst->initExprNode,
        SLEvalResolveIdent,
        SLEvalResolveCall,
        p,
        &value,
        &isConst,
        NULL);
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
    {
        const SLPackage* currentPkg = SLEvalFindPackageByFile(p, p->currentFile);
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
        int32_t topVarIndex = SLEvalFindTopVarBySlice(p, p->currentFile, nameStart, nameEnd);
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
        int32_t topConstIndex = SLEvalFindTopConstBySlice(p, p->currentFile, nameStart, nameEnd);
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
        int32_t fnIndex = SLEvalFindAnyFunctionBySlice(p, p->currentFile, nameStart, nameEnd);
        if (fnIndex >= 0) {
            SLEvalValueSetFunctionRef(outValue, (uint32_t)fnIndex);
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
    if (argCount > 0) {
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
        } else {
            fnIndex = -1;
        }
    } else {
        fnIndex = -1;
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
        if (array == NULL) {
            *outIsConst = 0;
            return 0;
        }
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
        if (array == NULL || index < 0 || (uint64_t)index >= (uint64_t)array->len) {
            goto index_fallback;
        }
        *outValue = array->elems[(uint32_t)index];
        *outIsConst = 1;
        return 0;
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
                    if (valueNode >= 0 && enumFile->ast.nodes[valueNode].kind != SLAst_FIELD) {
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
            SLEvalTaggedEnum*  tagged = SLEvalValueAsTaggedEnum(targetValue);
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
        if (agg != NULL
            && SLEvalAggregateGetFieldValue(
                agg, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
        {
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == SLAst_UNARY && (SLTokenKind)n->op == SLTok_AND) {
        int32_t     childNode = n->firstChild;
        SLCTFEValue childValue;
        int         childIsConst = 0;
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
        if (calleeNode >= 0 && ast->nodes[calleeNode].kind == SLAst_FIELD_EXPR) {
            const SLAstNode* callee = &ast->nodes[calleeNode];
            int32_t          baseNode = callee->firstChild;
            int32_t          argNode = ast->nodes[calleeNode].nextSibling;
            SLCTFEValue      calleeValue;
            int              calleeIsConst = 0;
            if (SLEvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                return -1;
            }
            if (calleeIsConst && SLEvalValueIsFunctionRef(&calleeValue, NULL)) {
                uint32_t     argCount = 0;
                uint32_t     i = 0;
                int32_t      scanNode = argNode;
                SLCTFEValue* args = NULL;
                while (scanNode >= 0) {
                    argCount++;
                    scanNode = ast->nodes[scanNode].nextSibling;
                }
                if (argCount > 0) {
                    args = (SLCTFEValue*)SLArenaAlloc(
                        p->arena, sizeof(SLCTFEValue) * argCount, (uint32_t)_Alignof(SLCTFEValue));
                    if (args == NULL) {
                        return ErrorSimple("out of memory");
                    }
                }
                scanNode = argNode;
                while (scanNode >= 0) {
                    SLCTFEValue argValue;
                    int         argIsConst = 0;
                    int32_t     argExprNode = scanNode;
                    if (ast->nodes[scanNode].kind == SLAst_CALL_ARG) {
                        argExprNode = ast->nodes[scanNode].firstChild;
                    }
                    if (SLEvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
                        return -1;
                    }
                    if (!argIsConst) {
                        *outIsConst = 0;
                        return 0;
                    }
                    args[i++] = argValue;
                    scanNode = ast->nodes[scanNode].nextSibling;
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
                uint32_t     i = 0;
                SLCTFEValue* args = NULL;
                SLCTFEValue  baseValue;
                int          baseIsConst = 0;
                int32_t      scanNode = argNode;
                if (SLEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
                    return -1;
                }
                if (baseIsConst) {
                    while (scanNode >= 0) {
                        if (extraArgCount == UINT32_MAX - 1u) {
                            return ErrorSimple("too many call arguments for evaluator backend");
                        }
                        extraArgCount++;
                        scanNode = ast->nodes[scanNode].nextSibling;
                    }
                    args = (SLCTFEValue*)SLArenaAlloc(
                        p->arena,
                        sizeof(SLCTFEValue) * (extraArgCount + 1u),
                        (uint32_t)_Alignof(SLCTFEValue));
                    if (args == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    args[0] = baseValue;
                    scanNode = argNode;
                    while (scanNode >= 0) {
                        SLCTFEValue extraArgValue;
                        int         extraArgIsConst = 0;
                        int32_t     argExprNode = scanNode;
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
                        if (SLEvalExecExprCb(p, argExprNode, &extraArgValue, &extraArgIsConst) != 0)
                        {
                            return -1;
                        }
                        if (!extraArgIsConst) {
                            *outIsConst = 0;
                            return 0;
                        }
                        args[++i] = extraArgValue;
                        scanNode = ast->nodes[scanNode].nextSibling;
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
            uint32_t     i = 0;
            int32_t      directFnIndex = -1;
            SLCTFEValue  calleeValue;
            int          calleeIsConst = 0;
            while (argNode >= 0) {
                if (argCount == UINT32_MAX) {
                    return ErrorSimple("too many call arguments for evaluator backend");
                }
                argCount++;
                argNode = ast->nodes[argNode].nextSibling;
            }
            if (argCount > 0) {
                args = (SLCTFEValue*)SLArenaAlloc(
                    p->arena, sizeof(SLCTFEValue) * argCount, (uint32_t)_Alignof(SLCTFEValue));
                if (args == NULL) {
                    return ErrorSimple("out of memory");
                }
            }
            directFnIndex = SLEvalResolveFunctionBySlice(
                p,
                NULL,
                p->currentFile,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                NULL,
                argCount);
            argNode = ast->nodes[calleeNode].nextSibling;
            while (argNode >= 0) {
                SLCTFEValue         argValue;
                int                 argIsConst = 0;
                int32_t             argExprNode = argNode;
                int32_t             paramTypeNode = -1;
                const SLParsedFile* paramTypeFile = p->currentFile;
                if (ast->nodes[argNode].kind == SLAst_CALL_ARG) {
                    argExprNode = ast->nodes[argNode].firstChild;
                }
                if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
                    if (p->currentExecCtx != NULL) {
                        SLCTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[argNode].start,
                            ast->nodes[argNode].end,
                            "call argument is malformed");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                if (directFnIndex >= 0) {
                    paramTypeNode = SLEvalFunctionParamTypeNodeAt(
                        &p->funcs[(uint32_t)directFnIndex], i);
                    paramTypeFile = p->funcs[(uint32_t)directFnIndex].file;
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
                args[i++] = argValue;
                argNode = ast->nodes[argNode].nextSibling;
            }
            if (SLEvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                return -1;
            }
            if (calleeIsConst && SLEvalValueIsFunctionRef(&calleeValue, NULL)) {
                int invoked = SLEvalInvokeFunctionRef(
                    p, &calleeValue, args, argCount, outValue, outIsConst);
                if (invoked < 0) {
                    return -1;
                }
                if (invoked > 0) {
                    return 0;
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
            SLCTFEValue inValue;
            int         inIsConst = 0;
            uint64_t    nullTypeTag = 0;
            if (SLEvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (inValue.kind == SLCTFEValue_NULL
                && SLEvalResolveNullCastTypeTag(p->currentFile, typeNode, &nullTypeTag))
            {
                *outValue = inValue;
                outValue->typeTag = nullTypeTag;
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
