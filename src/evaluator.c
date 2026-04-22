#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ctfe.h"
#include "ctfe_exec.h"
#include "evaluator.h"
#include "libhop-impl.h"
#include "mir_exec.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"
#include "mir_lower_stmt.h"
#include "hop_internal.h"

HOP_API_BEGIN

enum {
    HOP_EVAL_MIR_HOST_INVALID = HOPMirHostTarget_INVALID,
    HOP_EVAL_MIR_HOST_PRINT = HOPMirHostTarget_PRINT,
    HOP_EVAL_MIR_HOST_PLATFORM_EXIT = HOPMirHostTarget_PLATFORM_EXIT,
    HOP_EVAL_MIR_HOST_FREE = HOPMirHostTarget_FREE,
    HOP_EVAL_MIR_HOST_CONCAT = HOPMirHostTarget_CONCAT,
    HOP_EVAL_MIR_HOST_COPY = HOPMirHostTarget_COPY,
    HOP_EVAL_MIR_HOST_PLATFORM_CONSOLE_LOG = HOPMirHostTarget_PLATFORM_CONSOLE_LOG,
};

enum {
    HOP_EVAL_MIR_ITER_KIND_INVALID = 0,
    HOP_EVAL_MIR_ITER_KIND_SEQUENCE = 1,
    HOP_EVAL_MIR_ITER_KIND_PROTOCOL = 2,
    HOP_EVAL_MIR_ITER_MAGIC = 0x534c4954u,
};

static int HOPEvalNameEqLiteralOrPkgBuiltin(
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

static int HOPEvalNameIsCompilerDiagBuiltin(
    const char* _Nullable src, uint32_t start, uint32_t end) {
    return HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "error", "compiler")
        || HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "error_at", "compiler")
        || HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "warn", "compiler")
        || HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "warn_at", "compiler");
}

static int HOPEvalNameIsLazyTypeBuiltin(const char* _Nullable src, uint32_t start, uint32_t end) {
    return SliceEqCStr(src, start, end, "typeof")
        || HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "kind", "reflect")
        || HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "base", "reflect")
        || HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "is_alias", "reflect")
        || HOPEvalNameEqLiteralOrPkgBuiltin(src, start, end, "type_name", "reflect")
        || SliceEqCStr(src, start, end, "ptr") || SliceEqCStr(src, start, end, "slice")
        || SliceEqCStr(src, start, end, "array");
}

typedef struct {
    uint32_t     magic;
    uint32_t     sourceNode;
    uint32_t     index;
    int32_t      iteratorFn;
    uint16_t     flags;
    uint8_t      kind;
    uint8_t      _reserved[1];
    HOPCTFEValue sourceValue;
    HOPCTFEValue iteratorValue;
} HOPEvalMirIteratorState;
static int HOPEvalStringValueFromArrayBytes(
    HOPArena* arena, const HOPCTFEValue* inValue, int32_t targetTypeCode, HOPCTFEValue* outValue);

static int HOPEvalTypeNodeIsAnytype(const HOPParsedFile* file, int32_t typeNode);
static int HOPEvalTypeNodeIsTemplateParamName(const HOPParsedFile* file, int32_t typeNode);

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
    HOPEvalTypeCode_INVALID = 0,
    HOPEvalTypeCode_BOOL = 1,
    HOPEvalTypeCode_U8,
    HOPEvalTypeCode_U16,
    HOPEvalTypeCode_U32,
    HOPEvalTypeCode_U64,
    HOPEvalTypeCode_UINT,
    HOPEvalTypeCode_I8,
    HOPEvalTypeCode_I16,
    HOPEvalTypeCode_I32,
    HOPEvalTypeCode_I64,
    HOPEvalTypeCode_INT,
    HOPEvalTypeCode_F32,
    HOPEvalTypeCode_F64,
    HOPEvalTypeCode_TYPE,
    HOPEvalTypeCode_STR_REF,
    HOPEvalTypeCode_STR_PTR,
    HOPEvalTypeCode_RAWPTR,
    HOPEvalTypeCode_ANYTYPE,
};

typedef struct HOPEvalProgram HOPEvalProgram;
typedef struct HOPEvalContext HOPEvalContext;

static void    HOPEvalValueSetRuntimeTypeCode(HOPCTFEValue* value, int32_t typeCode);
static int     HOPEvalValueGetRuntimeTypeCode(const HOPCTFEValue* value, int32_t* outTypeCode);
static int32_t HOPEvalFindTopConstBySlice(
    const HOPEvalProgram* p, const HOPParsedFile* file, uint32_t nameStart, uint32_t nameEnd);
static int32_t HOPEvalFindTopConstBySliceInPackage(
    const HOPEvalProgram* p,
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd);
static int HOPEvalEvalTopConst(
    HOPEvalProgram* p, uint32_t topConstIndex, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalInvokeFunction(
    HOPEvalProgram* p,
    int32_t         fnIndex,
    const HOPCTFEValue* _Nullable args,
    uint32_t              argCount,
    const HOPEvalContext* callContext,
    HOPCTFEValue*         outValue,
    int*                  outDidReturn);
static int HOPEvalInvokeFunctionRef(
    HOPEvalProgram*     p,
    const HOPCTFEValue* calleeValue,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst);
static int HOPEvalValueNeedsDefaultFieldEval(const HOPCTFEValue* value);
static int HOPEvalMirLookupLocalTypeNode(
    HOPEvalProgram*       p,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const HOPParsedFile** outFile,
    int32_t*              outTypeNode);
static int HOPEvalMirLookupLocalValue(
    HOPEvalProgram* p, uint32_t nameStart, uint32_t nameEnd, HOPCTFEValue* outValue);
static int HOPEvalFindVisibleLocalTypeNodeByName(
    const HOPParsedFile* file,
    uint32_t             beforePos,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    int32_t*             outTypeNode);

static int HOPEvalBuiltinTypeSize(
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

static int HOPEvalBuiltinTypeCode(
    const char* source, uint32_t nameStart, uint32_t nameEnd, int32_t* outTypeCode) {
    if (outTypeCode != NULL) {
        *outTypeCode = HOPEvalTypeCode_INVALID;
    }
    if (source == NULL || outTypeCode == NULL) {
        return 0;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "bool")) {
        *outTypeCode = HOPEvalTypeCode_BOOL;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u8")) {
        *outTypeCode = HOPEvalTypeCode_U8;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u16")) {
        *outTypeCode = HOPEvalTypeCode_U16;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u32")) {
        *outTypeCode = HOPEvalTypeCode_U32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u64")) {
        *outTypeCode = HOPEvalTypeCode_U64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "uint")) {
        *outTypeCode = HOPEvalTypeCode_UINT;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i8")) {
        *outTypeCode = HOPEvalTypeCode_I8;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i16")) {
        *outTypeCode = HOPEvalTypeCode_I16;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i32")) {
        *outTypeCode = HOPEvalTypeCode_I32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i64")) {
        *outTypeCode = HOPEvalTypeCode_I64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "int")) {
        *outTypeCode = HOPEvalTypeCode_INT;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "f32")) {
        *outTypeCode = HOPEvalTypeCode_F32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "f64")) {
        *outTypeCode = HOPEvalTypeCode_F64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "rawptr")) {
        *outTypeCode = HOPEvalTypeCode_RAWPTR;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "type")) {
        *outTypeCode = HOPEvalTypeCode_TYPE;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "anytype")) {
        *outTypeCode = HOPEvalTypeCode_ANYTYPE;
        return 1;
    }
    return 0;
}

static int HOPEvalTypeCodeFromTypeNode(
    const HOPParsedFile* file, int32_t typeNode, int32_t* outTypeCode) {
    const HOPAstNode* n;
    if (outTypeCode != NULL) {
        *outTypeCode = HOPEvalTypeCode_INVALID;
    }
    if (file == NULL || outTypeCode == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == HOPAst_TYPE_NAME) {
        return HOPEvalBuiltinTypeCode(file->source, n->dataStart, n->dataEnd, outTypeCode);
    }
    if ((n->kind == HOPAst_TYPE_REF || n->kind == HOPAst_TYPE_PTR) && n->firstChild >= 0
        && (uint32_t)n->firstChild < file->ast.len
        && file->ast.nodes[n->firstChild].kind == HOPAst_TYPE_NAME
        && SliceEqCStr(
            file->source,
            file->ast.nodes[n->firstChild].dataStart,
            file->ast.nodes[n->firstChild].dataEnd,
            "str"))
    {
        *outTypeCode =
            n->kind == HOPAst_TYPE_REF ? HOPEvalTypeCode_STR_REF : HOPEvalTypeCode_STR_PTR;
        return 1;
    }
    return 0;
}

static int HOPEvalIsU8ElementTypeNode(const HOPParsedFile* file, int32_t typeNode) {
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    return file->ast.nodes[typeNode].kind == HOPAst_TYPE_NAME
        && SliceEqCStr(
               file->source,
               file->ast.nodes[typeNode].dataStart,
               file->ast.nodes[typeNode].dataEnd,
               "u8");
}

static int HOPEvalStringViewTypeCodeFromTypeNode(
    const HOPParsedFile* file, int32_t typeNode, int32_t* outTypeCode) {
    const HOPAstNode* n;
    int32_t           childNode;
    if (outTypeCode != NULL) {
        *outTypeCode = HOPEvalTypeCode_INVALID;
    }
    if (file == NULL || outTypeCode == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    if (HOPEvalTypeCodeFromTypeNode(file, typeNode, outTypeCode)) {
        return *outTypeCode == HOPEvalTypeCode_STR_REF || *outTypeCode == HOPEvalTypeCode_STR_PTR;
    }
    n = &file->ast.nodes[typeNode];
    if ((n->kind != HOPAst_TYPE_PTR && n->kind != HOPAst_TYPE_REF) || n->firstChild < 0
        || (uint32_t)n->firstChild >= file->ast.len)
    {
        return 0;
    }
    childNode = n->firstChild;
    if ((file->ast.nodes[childNode].kind == HOPAst_TYPE_VARRAY
         || file->ast.nodes[childNode].kind == HOPAst_TYPE_ARRAY)
        && file->ast.nodes[childNode].firstChild >= 0
        && HOPEvalIsU8ElementTypeNode(file, file->ast.nodes[childNode].firstChild))
    {
        *outTypeCode =
            n->kind == HOPAst_TYPE_REF ? HOPEvalTypeCode_STR_REF : HOPEvalTypeCode_STR_PTR;
        return 1;
    }
    return 0;
}

static int HOPEvalCloneStringValue(
    HOPArena* arena, const HOPCTFEValue* inValue, HOPCTFEValue* outValue, int32_t typeCode) {
    uint8_t* copyBytes = NULL;
    if (arena == NULL || inValue == NULL || outValue == NULL
        || inValue->kind != HOPCTFEValue_STRING)
    {
        return 0;
    }
    *outValue = *inValue;
    if (inValue->s.len > 0) {
        copyBytes = (uint8_t*)HOPArenaAlloc(arena, inValue->s.len, (uint32_t)_Alignof(uint8_t));
        if (copyBytes == NULL) {
            return -1;
        }
        memcpy(copyBytes, inValue->s.bytes, inValue->s.len);
        outValue->s.bytes = copyBytes;
    }
    HOPEvalValueSetRuntimeTypeCode(outValue, typeCode);
    return 1;
}

static int HOPEvalAdaptStringValueForType(
    HOPArena*            arena,
    const HOPParsedFile* typeFile,
    int32_t              typeNode,
    const HOPCTFEValue*  inValue,
    HOPCTFEValue*        outValue) {
    int32_t targetTypeCode = HOPEvalTypeCode_INVALID;
    int32_t currentTypeCode = HOPEvalTypeCode_INVALID;
    if (arena == NULL || typeFile == NULL || inValue == NULL || outValue == NULL
        || inValue->kind != HOPCTFEValue_STRING)
    {
        return 0;
    }
    if (!HOPEvalStringViewTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)) {
        return 0;
    }
    if (targetTypeCode == HOPEvalTypeCode_STR_PTR) {
        if (HOPEvalValueGetRuntimeTypeCode(inValue, &currentTypeCode)
            && currentTypeCode == HOPEvalTypeCode_STR_PTR)
        {
            *outValue = *inValue;
            return 1;
        }
        return HOPEvalCloneStringValue(arena, inValue, outValue, targetTypeCode);
    }
    *outValue = *inValue;
    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
    return 1;
}

static int HOPEvalTypeCodeFromValue(const HOPCTFEValue* value, int32_t* outTypeCode) {
    if (outTypeCode != NULL) {
        *outTypeCode = HOPEvalTypeCode_INVALID;
    }
    if (value == NULL || outTypeCode == NULL) {
        return 0;
    }
    if (HOPEvalValueGetRuntimeTypeCode(value, outTypeCode)) {
        return 1;
    }
    if (value->kind == HOPCTFEValue_BOOL) {
        *outTypeCode = HOPEvalTypeCode_BOOL;
        return 1;
    }
    if (value->kind == HOPCTFEValue_INT) {
        *outTypeCode = HOPEvalTypeCode_INT;
        return 1;
    }
    if (value->kind == HOPCTFEValue_FLOAT) {
        *outTypeCode = HOPEvalTypeCode_F64;
        return 1;
    }
    if (value->kind == HOPCTFEValue_STRING) {
        *outTypeCode = HOPEvalTypeCode_STR_REF;
        return 1;
    }
    if (value->kind == HOPCTFEValue_TYPE) {
        *outTypeCode = HOPEvalTypeCode_TYPE;
        return 1;
    }
    return 0;
}

static void HOPEvalAnnotateValueTypeFromExpr(
    const HOPParsedFile* file, const HOPAst* ast, int32_t exprNode, HOPCTFEValue* value) {
    int32_t           typeCode = HOPEvalTypeCode_INVALID;
    const HOPAstNode* n;
    if (file == NULL || ast == NULL || value == NULL || exprNode < 0
        || (uint32_t)exprNode >= ast->len)
    {
        return;
    }
    if (HOPEvalValueGetRuntimeTypeCode(value, &typeCode)) {
        return;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == HOPAst_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return;
        }
        n = &ast->nodes[exprNode];
    }
    if (n->kind == HOPAst_CAST) {
        int32_t typeNode = ASTNextSibling(ast, n->firstChild);
        if (typeNode >= 0 && HOPEvalTypeCodeFromTypeNode(file, typeNode, &typeCode)) {
            HOPEvalValueSetRuntimeTypeCode(value, typeCode);
            return;
        }
    }
    if (n->kind == HOPAst_STRING) {
        HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_STR_REF);
        return;
    }
    if (n->kind == HOPAst_BOOL) {
        HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_BOOL);
        return;
    }
    if (n->kind == HOPAst_FLOAT) {
        HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_F64);
        return;
    }
    if (n->kind == HOPAst_INT) {
        HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_INT);
        return;
    }
}

static int HOPEvalTypeNodeSize(
    const HOPParsedFile* file, int32_t typeNode, uint64_t* outSize, uint32_t depth) {
    const HOPAstNode* n;
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
        case HOPAst_TYPE_NAME:
            return HOPEvalBuiltinTypeSize(file->source, n->dataStart, n->dataEnd, outSize);
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF:
        case HOPAst_TYPE_FN:       *outSize = (uint64_t)sizeof(void*); return 1;
        case HOPAst_TYPE_SLICE:
        case HOPAst_TYPE_MUTSLICE: *outSize = (uint64_t)(sizeof(void*) * 2u); return 1;
        case HOPAst_TYPE_OPTIONAL: {
            int32_t child = file->ast.nodes[typeNode].firstChild;
            return child >= 0 ? HOPEvalTypeNodeSize(file, child, outSize, depth + 1u) : 0;
        }
        case HOPAst_TYPE_ARRAY: {
            int32_t  elemTypeNode = file->ast.nodes[typeNode].firstChild;
            uint64_t elemSize = 0;
            uint64_t count = 0;
            if (elemTypeNode < 0 || !HOPEvalTypeNodeSize(file, elemTypeNode, &elemSize, depth + 1u)
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

typedef struct {
    const HOPPackage*    pkg;
    const HOPParsedFile* file;
    int32_t              fnNode;
    int32_t              bodyNode;
    uint32_t             nameStart;
    uint32_t             nameEnd;
    uint32_t             paramCount;
    uint8_t              hasReturnType;
    uint8_t              hasContextClause;
    uint8_t              isBuiltinPackageFn;
    uint8_t              isVariadic;
} HOPEvalFunction;

enum {
    HOPEvalTopConstState_UNSEEN = 0,
    HOPEvalTopConstState_VISITING = 1,
    HOPEvalTopConstState_READY = 2,
    HOPEvalTopConstState_FAILED = 3,
};

typedef struct {
    const HOPParsedFile* file;
    int32_t              nodeId;
    int32_t              initExprNode;
    uint32_t             nameStart;
    uint32_t             nameEnd;
    uint8_t              state;
    uint8_t              _reserved[3];
    HOPCTFEValue         value;
} HOPEvalTopConst;

typedef struct {
    const HOPParsedFile* file;
    int32_t              nodeId;
    int32_t              initExprNode;
    int32_t              declTypeNode;
    uint32_t             nameStart;
    uint32_t             nameEnd;
    uint8_t              state;
    uint8_t              _reserved[3];
    HOPCTFEValue         value;
} HOPEvalTopVar;

typedef struct {
    uint32_t     nameStart;
    uint32_t     nameEnd;
    uint16_t     flags;
    uint16_t     _reserved;
    int32_t      typeNode;
    int32_t      defaultExprNode;
    HOPCTFEValue value;
} HOPEvalAggregateField;

typedef struct {
    const HOPParsedFile*   file;
    int32_t                nodeId;
    HOPEvalAggregateField* fields;
    uint32_t               fieldLen;
} HOPEvalAggregate;

typedef struct {
    const HOPParsedFile* file;
    int32_t              typeNode;
    int32_t              elemTypeNode;
    HOPCTFEValue* _Nullable elems;
    uint32_t len;
} HOPEvalArray;

struct HOPEvalContext {
    HOPCTFEValue allocator;
    HOPCTFEValue tempAllocator;
    HOPCTFEValue logger;
};

typedef struct {
    const HOPParsedFile* file;
    int32_t              enumNode;
    uint32_t             variantNameStart;
    uint32_t             variantNameEnd;
    uint32_t             tagIndex;
    HOPEvalAggregate* _Nullable payload;
} HOPEvalTaggedEnum;

typedef struct {
    const HOPParsedFile* activeTemplateParamFile;
    uint32_t             activeTemplateParamNameStart;
    uint32_t             activeTemplateParamNameEnd;
    const HOPParsedFile* activeTemplateTypeFile;
    int32_t              activeTemplateTypeNode;
    HOPCTFEValue         activeTemplateTypeValue;
    uint8_t              hasActiveTemplateTypeValue;
} HOPEvalTemplateBindingState;

typedef struct HOPEvalReflectedType HOPEvalReflectedType;
struct HOPEvalReflectedType {
    uint8_t              kind;
    uint8_t              namedKind;
    uint16_t             _reserved;
    const HOPParsedFile* file;
    int32_t              nodeId;
    uint32_t             arrayLen;
    HOPCTFEValue         elemType;
};

#define HOP_EVAL_PACKAGE_REF_TAG_FLAG  (UINT64_C(1) << 63)
#define HOP_EVAL_NULL_FIXED_LEN_TAG    (UINT64_C(1) << 62)
#define HOP_EVAL_FUNCTION_REF_TAG_FLAG (UINT64_C(1) << 61)
#define HOP_EVAL_TAGGED_ENUM_TAG_FLAG  (UINT64_C(1) << 60)
#define HOP_EVAL_SIMPLE_TYPE_TAG_FLAG  (UINT64_C(1) << 59)
#define HOP_EVAL_REFLECT_TYPE_TAG_FLAG (UINT64_C(1) << 58)
#define HOP_EVAL_RUNTIME_TYPE_MAGIC    0x534c4556u

enum {
    HOPEvalReflectType_NAMED = 1,
    HOPEvalReflectType_PTR = 2,
    HOPEvalReflectType_SLICE = 3,
    HOPEvalReflectType_ARRAY = 4,
};

enum {
    HOPEvalTypeKind_PRIMITIVE = 1,
    HOPEvalTypeKind_ALIAS = 2,
    HOPEvalTypeKind_STRUCT = 3,
    HOPEvalTypeKind_UNION = 4,
    HOPEvalTypeKind_ENUM = 5,
    HOPEvalTypeKind_POINTER = 6,
    HOPEvalTypeKind_REFERENCE = 7,
    HOPEvalTypeKind_SLICE = 8,
    HOPEvalTypeKind_ARRAY = 9,
    HOPEvalTypeKind_OPTIONAL = 10,
    HOPEvalTypeKind_FUNCTION = 11,
};

struct HOPEvalProgram {
    HOPArena* _Nonnull arena;
    const HOPPackageLoader* loader;
    const HOPPackage*       entryPkg;
    const HOPParsedFile*    currentFile;
    HOPCTFEExecCtx*         currentExecCtx;
    struct HOPEvalMirExecCtx* _Nullable currentMirExecCtx;
    HOPEvalFunction* funcs;
    uint32_t         funcLen;
    uint32_t         funcCap;
    HOPEvalTopConst* topConsts;
    uint32_t         topConstLen;
    uint32_t         topConstCap;
    HOPEvalTopVar*   topVars;
    uint32_t         topVarLen;
    uint32_t         topVarCap;
    uint32_t         callDepth;
    uint32_t         callStack[HOP_EVAL_CALL_MAX_DEPTH];
    HOPEvalContext   rootContext;
    const HOPEvalContext* _Nullable currentContext;
    HOPCTFEValue         loggerPrefix;
    const HOPParsedFile* activeTemplateParamFile;
    uint32_t             activeTemplateParamNameStart;
    uint32_t             activeTemplateParamNameEnd;
    const HOPParsedFile* activeTemplateTypeFile;
    int32_t              activeTemplateTypeNode;
    HOPCTFEValue         activeTemplateTypeValue;
    uint8_t              hasActiveTemplateTypeValue;
    const HOPParsedFile* expectedCallExprFile;
    int32_t              expectedCallExprNode;
    const HOPParsedFile* expectedCallTypeFile;
    int32_t              expectedCallTypeNode;
    const HOPParsedFile* activeCallExpectedTypeFile;
    int32_t              activeCallExpectedTypeNode;
    int                  exitCalled;
    int                  exitCode;
};

typedef struct HOPEvalMirExecCtx {
    HOPEvalProgram*             p;
    uint32_t*                   evalToMir;
    uint32_t                    evalToMirLen;
    uint32_t*                   mirToEval;
    uint32_t                    mirToEvalLen;
    const HOPParsedFile**       sourceFiles;
    uint32_t                    sourceFileCap;
    const HOPParsedFile*        savedFiles[HOP_EVAL_CALL_MAX_DEPTH];
    uint8_t                     pushedFrames[HOP_EVAL_CALL_MAX_DEPTH];
    struct HOPEvalMirExecCtx*   savedMirExecCtxs[HOP_EVAL_CALL_MAX_DEPTH];
    uint32_t                    savedFileLen;
    uint32_t                    rootMirFnIndex;
    const HOPMirProgram*        mirProgram;
    const HOPMirFunction*       mirFunction;
    const HOPCTFEValue*         mirLocals;
    uint32_t                    mirLocalCount;
    const HOPMirProgram*        savedMirPrograms[HOP_EVAL_CALL_MAX_DEPTH];
    const HOPMirFunction*       savedMirFunctions[HOP_EVAL_CALL_MAX_DEPTH];
    const HOPCTFEValue*         savedMirLocals[HOP_EVAL_CALL_MAX_DEPTH];
    uint32_t                    savedMirLocalCounts[HOP_EVAL_CALL_MAX_DEPTH];
    uint32_t                    mirFrameDepth;
    HOPEvalTemplateBindingState savedTemplateBindings[HOP_EVAL_CALL_MAX_DEPTH];
    uint8_t                     restoresTemplateBinding[HOP_EVAL_CALL_MAX_DEPTH];
    HOPEvalTemplateBindingState pendingTemplateBinding;
    uint8_t                     hasPendingTemplateBinding;
} HOPEvalMirExecCtx;

static uint32_t HOPEvalAggregateFieldBindingCount(const HOPEvalAggregate* agg);
static int      HOPEvalAppendAggregateFieldBindings(
    HOPCTFEExecBinding*    fieldBindings,
    uint32_t               bindingCap,
    HOPCTFEExecEnv*        fieldFrame,
    HOPEvalAggregateField* field);
static int HOPEvalAggregateHasReservedFields(const HOPEvalAggregate* agg);
static int HOPEvalReplayReservedAggregateFields(
    HOPCTFEValue* target, const HOPEvalAggregate* sourceAgg);

static void HOPEvalValueSetNull(HOPCTFEValue* value) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_NULL;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void HOPEvalValueSetInt(HOPCTFEValue* value, int64_t n) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_INT;
    value->i64 = n;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void HOPEvalValueSetRuntimeTypeCode(HOPCTFEValue* value, int32_t typeCode) {
    if (value == NULL) {
        return;
    }
    value->span.fileLen = 0;
    value->span.startLine = (uint32_t)typeCode;
    value->span.startColumn = HOP_EVAL_RUNTIME_TYPE_MAGIC;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static int HOPEvalValueGetRuntimeTypeCode(const HOPCTFEValue* value, int32_t* outTypeCode) {
    if (value == NULL || outTypeCode == NULL || value->kind == HOPCTFEValue_SPAN
        || value->span.startColumn != HOP_EVAL_RUNTIME_TYPE_MAGIC)
    {
        return 0;
    }
    *outTypeCode = (int32_t)value->span.startLine;
    return 1;
}

static void HOPEvalValueSetSimpleTypeValue(HOPCTFEValue* value, int32_t typeCode) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = HOP_EVAL_SIMPLE_TYPE_TAG_FLAG | (uint64_t)(uint32_t)typeCode;
    value->s.bytes = NULL;
    value->s.len = 0;
    value->span.fileBytes = NULL;
    value->span.fileLen = 0;
    value->span.startLine = 0;
    value->span.startColumn = 0;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static int HOPEvalValueGetSimpleTypeCode(const HOPCTFEValue* value, int32_t* outTypeCode) {
    if (value == NULL || outTypeCode == NULL || value->kind != HOPCTFEValue_TYPE
        || (value->typeTag & HOP_EVAL_SIMPLE_TYPE_TAG_FLAG) == 0)
    {
        return 0;
    }
    *outTypeCode = (int32_t)(uint32_t)(value->typeTag & ~HOP_EVAL_SIMPLE_TYPE_TAG_FLAG);
    return 1;
}

static void HOPEvalAnnotateUntypedLiteralValue(HOPCTFEValue* value) {
    int32_t typeCode = HOPEvalTypeCode_INVALID;
    if (value == NULL || HOPEvalValueGetRuntimeTypeCode(value, &typeCode)) {
        return;
    }
    switch (value->kind) {
        case HOPCTFEValue_BOOL:  HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_BOOL); break;
        case HOPCTFEValue_INT:   HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_INT); break;
        case HOPCTFEValue_FLOAT: HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_F64); break;
        case HOPCTFEValue_STRING:
            HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_STR_REF);
            break;
        default: break;
    }
}

static uint64_t HOPEvalHashMix64(uint64_t x) {
    x ^= x >> 21;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static uint64_t HOPEvalReflectTypeTagBody(const HOPEvalReflectedType* rt) {
    uint64_t tag = 0;
    if (rt == NULL) {
        return 0;
    }
    tag = ((uint64_t)rt->kind << 52) ^ ((uint64_t)rt->namedKind << 44);
    if (rt->kind == HOPEvalReflectType_NAMED) {
        tag ^= HOPEvalHashMix64((uint64_t)(uintptr_t)rt->file);
        tag ^= HOPEvalHashMix64((uint64_t)(uint32_t)(rt->nodeId + 1));
    } else {
        tag ^= HOPEvalHashMix64(rt->elemType.typeTag);
        tag ^= HOPEvalHashMix64((uint64_t)rt->arrayLen);
    }
    return tag
         & ~(HOP_EVAL_PACKAGE_REF_TAG_FLAG | HOP_EVAL_NULL_FIXED_LEN_TAG
             | HOP_EVAL_FUNCTION_REF_TAG_FLAG | HOP_EVAL_TAGGED_ENUM_TAG_FLAG
             | HOP_EVAL_SIMPLE_TYPE_TAG_FLAG | HOP_EVAL_REFLECT_TYPE_TAG_FLAG);
}

static void HOPEvalValueSetReflectedTypeValue(HOPCTFEValue* value, HOPEvalReflectedType* rt) {
    if (value == NULL || rt == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = HOP_EVAL_REFLECT_TYPE_TAG_FLAG | HOPEvalReflectTypeTagBody(rt);
    value->s.bytes = (const uint8_t*)rt;
    value->s.len = 0;
    value->span.fileBytes = NULL;
    value->span.fileLen = 0;
    value->span.startLine = 0;
    value->span.startColumn = 0;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static HOPEvalReflectedType* _Nullable HOPEvalValueAsReflectedType(const HOPCTFEValue* value) {
    if (value == NULL || value->kind != HOPCTFEValue_TYPE
        || (value->typeTag & HOP_EVAL_REFLECT_TYPE_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (HOPEvalReflectedType*)value->s.bytes;
}

static void HOPEvalValueSetPackageRef(HOPCTFEValue* value, uint32_t pkgIndex) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = HOP_EVAL_PACKAGE_REF_TAG_FLAG | (uint64_t)pkgIndex;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void HOPEvalValueSetFunctionRef(HOPCTFEValue* value, uint32_t fnIndex) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = HOP_EVAL_FUNCTION_REF_TAG_FLAG | (uint64_t)fnIndex;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static int HOPEvalValueIsPackageRef(const HOPCTFEValue* value, uint32_t* outPkgIndex) {
    if (value == NULL || value->kind != HOPCTFEValue_TYPE
        || (value->typeTag & HOP_EVAL_PACKAGE_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outPkgIndex != NULL) {
        *outPkgIndex = (uint32_t)(value->typeTag & ~HOP_EVAL_PACKAGE_REF_TAG_FLAG);
    }
    return 1;
}

static int HOPEvalValueIsFunctionRef(const HOPCTFEValue* value, uint32_t* _Nullable outFnIndex) {
    if (value == NULL || value->kind != HOPCTFEValue_TYPE
        || (value->typeTag & HOP_EVAL_FUNCTION_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outFnIndex != NULL) {
        *outFnIndex = (uint32_t)(value->typeTag & ~HOP_EVAL_FUNCTION_REF_TAG_FLAG);
    }
    return 1;
}

static int HOPEvalValueIsInvokableFunctionRef(const HOPCTFEValue* value) {
    return HOPEvalValueIsFunctionRef(value, NULL) || HOPMirValueAsFunctionRef(value, NULL);
}

static void HOPEvalValueSetTaggedEnum(
    HOPEvalProgram*      p,
    HOPCTFEValue*        value,
    const HOPParsedFile* file,
    int32_t              enumNode,
    uint32_t             variantNameStart,
    uint32_t             variantNameEnd,
    uint32_t             tagIndex,
    HOPEvalAggregate* _Nullable payload) {
    HOPEvalTaggedEnum* tagged;
    if (p == NULL || value == NULL || file == NULL) {
        return;
    }
    tagged = (HOPEvalTaggedEnum*)HOPArenaAlloc(
        p->arena, sizeof(HOPEvalTaggedEnum), (uint32_t)_Alignof(HOPEvalTaggedEnum));
    if (tagged == NULL) {
        HOPEvalValueSetNull(value);
        return;
    }
    memset(tagged, 0, sizeof(*tagged));
    tagged->file = file;
    tagged->enumNode = enumNode;
    tagged->variantNameStart = variantNameStart;
    tagged->variantNameEnd = variantNameEnd;
    tagged->tagIndex = tagIndex;
    tagged->payload = payload;
    value->kind = HOPCTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = HOP_EVAL_TAGGED_ENUM_TAG_FLAG
                   | (((uint64_t)(uintptr_t)file) & ~HOP_EVAL_TAGGED_ENUM_TAG_FLAG);
    value->typeTag ^= (uint64_t)(uint32_t)(enumNode + 1) << 3;
    value->typeTag ^= (uint64_t)tagIndex;
    value->s.bytes = (const uint8_t*)tagged;
    value->s.len = 0;
}

static HOPEvalTaggedEnum* _Nullable HOPEvalValueAsTaggedEnum(const HOPCTFEValue* value) {
    if (value == NULL || value->kind != HOPCTFEValue_TYPE
        || (value->typeTag & HOP_EVAL_TAGGED_ENUM_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (HOPEvalTaggedEnum*)value->s.bytes;
}

static uint64_t HOPEvalMakeAliasTag(const HOPParsedFile* file, int32_t nodeId) {
    uint64_t tag = 0;
    if (file != NULL) {
        tag = (uint64_t)(uintptr_t)file;
    }
    tag ^= (uint64_t)(uint32_t)(nodeId + 1) << 1;
    return tag & ~HOP_EVAL_PACKAGE_REF_TAG_FLAG;
}

static uint64_t HOPEvalMakeNullFixedLenTag(uint32_t len) {
    return HOP_EVAL_NULL_FIXED_LEN_TAG | (uint64_t)len;
}

static int HOPEvalValueGetNullFixedLen(const HOPCTFEValue* value, uint32_t* outLen) {
    if (value == NULL || value->kind != HOPCTFEValue_NULL
        || (value->typeTag & HOP_EVAL_NULL_FIXED_LEN_TAG) == 0)
    {
        return 0;
    }
    if (outLen != NULL) {
        *outLen = (uint32_t)(value->typeTag & ~HOP_EVAL_NULL_FIXED_LEN_TAG);
    }
    return 1;
}

static int HOPEvalParseUintSlice(
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

static int HOPEvalResolveNullCastTypeTag(
    const HOPParsedFile* file, int32_t typeNode, uint64_t* _Nullable outTypeTag) {
    const HOPAstNode* n;
    int32_t           childNode;
    uint32_t          fixedLen = 0;
    if (outTypeTag != NULL) {
        *outTypeTag = 0;
    }
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == HOPAst_TYPE_NAME
        && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "rawptr"))
    {
        return 1;
    }
    if (n->kind != HOPAst_TYPE_PTR && n->kind != HOPAst_TYPE_REF && n->kind != HOPAst_TYPE_MUTREF) {
        return 0;
    }
    childNode = n->firstChild;
    if (childNode >= 0 && (uint32_t)childNode < file->ast.len
        && file->ast.nodes[childNode].kind == HOPAst_TYPE_ARRAY
        && HOPEvalParseUintSlice(
            file->source,
            file->ast.nodes[childNode].dataStart,
            file->ast.nodes[childNode].dataEnd,
            &fixedLen))
    {
        if (outTypeTag != NULL) {
            *outTypeTag = HOPEvalMakeNullFixedLenTag(fixedLen);
        }
    }
    return 1;
}

static const HOPPackage* _Nullable HOPEvalFindPackageByFile(
    const HOPEvalProgram* p, const HOPParsedFile* file) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL || file == NULL) {
        return NULL;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &p->loader->packages[pkgIndex];
        uint32_t          fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (&pkg->files[fileIndex] == file) {
                return pkg;
            }
        }
    }
    return NULL;
}

static void HOPEvalValueSetStringSlice(
    HOPCTFEValue* value, const char* source, uint32_t start, uint32_t end) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_STRING;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = source != NULL ? (const uint8_t*)(source + start) : NULL;
    value->s.len = end >= start ? end - start : 0;
}

static int32_t HOPEvalResolveNamedTypeDeclInPackage(
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const HOPParsedFile** outFile,
    uint8_t*              outNamedKind) {
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
        const HOPParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t              nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const HOPAstNode* n = &pkgFile->ast.nodes[nodeId];
            uint8_t           namedKind = 0;
            if (n->kind == HOPAst_TYPE_ALIAS) {
                namedKind = HOPEvalTypeKind_ALIAS;
            } else if (n->kind == HOPAst_STRUCT) {
                namedKind = HOPEvalTypeKind_STRUCT;
            } else if (n->kind == HOPAst_UNION) {
                namedKind = HOPEvalTypeKind_UNION;
            } else if (n->kind == HOPAst_ENUM) {
                namedKind = HOPEvalTypeKind_ENUM;
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

static int HOPEvalResolveTypePackageAndName(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const HOPPackage**    outPkg,
    uint32_t*             outLookupStart,
    uint32_t*             outLookupEnd) {
    const HOPPackage* currentPkg;
    uint32_t          dot = nameStart;
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
    currentPkg = HOPEvalFindPackageByFile(p, callerFile);
    while (dot < nameEnd) {
        if (callerFile->source[dot] == '.') {
            break;
        }
        dot++;
    }
    if (dot < nameEnd && currentPkg != NULL) {
        uint32_t i;
        for (i = 0; i < currentPkg->importLen; i++) {
            const HOPImportRef* imp = &currentPkg->imports[i];
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

static int HOPEvalMakeNamedTypeValue(
    HOPEvalProgram*      p,
    const HOPParsedFile* declFile,
    int32_t              declNode,
    uint8_t              namedKind,
    HOPCTFEValue*        outValue) {
    HOPEvalReflectedType* rt;
    if (p == NULL || declFile == NULL || outValue == NULL || declNode < 0) {
        return 0;
    }
    rt = (HOPEvalReflectedType*)HOPArenaAlloc(
        p->arena, sizeof(HOPEvalReflectedType), (uint32_t)_Alignof(HOPEvalReflectedType));
    if (rt == NULL) {
        return -1;
    }
    memset(rt, 0, sizeof(*rt));
    rt->kind = HOPEvalReflectType_NAMED;
    rt->namedKind = namedKind;
    rt->file = declFile;
    rt->nodeId = declNode;
    HOPEvalValueSetReflectedTypeValue(outValue, rt);
    return 1;
}

static int HOPEvalResolveTypeValueName(
    HOPEvalProgram*      p,
    const HOPParsedFile* callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    HOPCTFEValue*        outValue) {
    const HOPPackage*    targetPkg = NULL;
    const HOPParsedFile* declFile = NULL;
    uint32_t             lookupStart = nameStart;
    uint32_t             lookupEnd = nameEnd;
    uint8_t              namedKind = 0;
    int32_t              typeCode = HOPEvalTypeCode_INVALID;
    int32_t              declNode = -1;
    uint32_t             pkgIndex;
    if (p == NULL || callerFile == NULL || outValue == NULL) {
        return 0;
    }
    if (HOPEvalBuiltinTypeCode(callerFile->source, nameStart, nameEnd, &typeCode)) {
        HOPEvalValueSetSimpleTypeValue(outValue, typeCode);
        return 1;
    }
    if (HOPEvalResolveTypePackageAndName(
            p, callerFile, nameStart, nameEnd, &targetPkg, &lookupStart, &lookupEnd))
    {
        declNode = HOPEvalResolveNamedTypeDeclInPackage(
            targetPkg, callerFile, lookupStart, lookupEnd, &declFile, &namedKind);
        if (declNode >= 0 && declFile != NULL) {
            return HOPEvalMakeNamedTypeValue(p, declFile, declNode, namedKind, outValue);
        }
        {
            int32_t topConstIndex = HOPEvalFindTopConstBySliceInPackage(
                p, targetPkg, callerFile, lookupStart, lookupEnd);
            if (topConstIndex >= 0) {
                int isConst = 0;
                if (HOPEvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                    return -1;
                }
                if (isConst && outValue->kind == HOPCTFEValue_TYPE) {
                    return 1;
                }
            }
        }
    }
    if (p->loader == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &p->loader->packages[pkgIndex];
        int32_t           topConstIndex;
        if (pkg == targetPkg) {
            continue;
        }
        declNode = HOPEvalResolveNamedTypeDeclInPackage(
            pkg, callerFile, lookupStart, lookupEnd, &declFile, &namedKind);
        if (declNode >= 0 && declFile != NULL) {
            return HOPEvalMakeNamedTypeValue(p, declFile, declNode, namedKind, outValue);
        }
        topConstIndex = HOPEvalFindTopConstBySliceInPackage(
            p, pkg, callerFile, lookupStart, lookupEnd);
        if (topConstIndex >= 0) {
            int isConst = 0;
            if (HOPEvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst && outValue->kind == HOPCTFEValue_TYPE) {
                return 1;
            }
        }
    }
    return 0;
}

static uint64_t HOPEvalMakeAggregateTag(const HOPParsedFile* file, int32_t nodeId) {
    uint64_t tag = 0;
    if (file != NULL) {
        tag = (uint64_t)(uintptr_t)file;
    }
    tag ^= (uint64_t)(uint32_t)(nodeId + 1) << 2;
    return tag
         & ~(HOP_EVAL_PACKAGE_REF_TAG_FLAG | HOP_EVAL_NULL_FIXED_LEN_TAG
             | HOPCTFEValueTag_AGG_PARTIAL);
}

static void HOPEvalValueSetAggregate(
    HOPCTFEValue* value, const HOPParsedFile* file, int32_t nodeId, HOPEvalAggregate* aggregate) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_AGGREGATE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = HOPEvalMakeAggregateTag(file, nodeId);
    value->s.bytes = (const uint8_t*)aggregate;
    value->s.len = aggregate != NULL ? aggregate->fieldLen : 0;
}

static HOPEvalAggregate* _Nullable HOPEvalValueAsAggregate(const HOPCTFEValue* value) {
    if (value == NULL || value->kind != HOPCTFEValue_AGGREGATE || value->s.bytes == NULL) {
        return NULL;
    }
    return (HOPEvalAggregate*)value->s.bytes;
}

static HOPCTFEValue* _Nullable HOPEvalValueReferenceTarget(const HOPCTFEValue* value);
static void HOPEvalValueSetReference(HOPCTFEValue* value, HOPCTFEValue* target);
static int  HOPEvalExecExprCb(void* ctx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
static int  HOPEvalZeroInitTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    HOPCTFEValue*         outValue,
    int*                  outIsConst);
static int HOPEvalResolveAliasCastTargetNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    const HOPParsedFile** outAliasFile,
    int32_t*              outAliasNode,
    int32_t*              outTargetNode);
static int HOPEvalResolveAggregateDeclFromValue(
    const HOPEvalProgram* p,
    const HOPCTFEValue*   value,
    const HOPParsedFile** outFile,
    int32_t*              outNode);
static int HOPEvalAggregateSetFieldValue(
    HOPEvalAggregate*   agg,
    const char*         source,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* inValue,
    HOPCTFEValue* _Nullable outValue);
static int HOPEvalExecExprInFileWithType(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    HOPCTFEExecEnv*      env,
    int32_t              exprNode,
    const HOPParsedFile* typeFile,
    int32_t              typeNode,
    HOPCTFEValue*        outValue,
    int*                 outIsConst);
static HOPEvalAggregateField* _Nullable HOPEvalAggregateFindDirectField(
    HOPEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd);
static int HOPEvalValueSetFieldPath(
    HOPCTFEValue*       value,
    const char*         source,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* inValue);
static int HOPEvalFinalizeAggregateVarArrays(HOPEvalProgram* p, HOPEvalAggregate* agg);
static int HOPEvalForInIndexCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    const HOPCTFEValue* sourceValue,
    uint32_t            index,
    int                 byRef,
    HOPCTFEValue*       outValue,
    int*                outIsConst);
static int HOPEvalForInIterCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    int32_t             sourceNode,
    const HOPCTFEValue* sourceValue,
    uint32_t            index,
    int                 hasKey,
    int                 keyRef,
    int                 valueRef,
    int                 valueDiscard,
    int*                outHasItem,
    HOPCTFEValue*       outKey,
    int*                outKeyIsConst,
    HOPCTFEValue*       outValue,
    int*                outValueIsConst);

static void HOPEvalValueSetArray(
    HOPCTFEValue* value, const HOPParsedFile* file, int32_t typeNode, HOPEvalArray* array) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_ARRAY;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = HOPEvalMakeAggregateTag(file, typeNode);
    value->s.bytes = (const uint8_t*)array;
    value->s.len = array != NULL ? array->len : 0;
}

static HOPEvalArray* _Nullable HOPEvalValueAsArray(const HOPCTFEValue* value) {
    if (value == NULL || value->kind != HOPCTFEValue_ARRAY || value->s.bytes == NULL) {
        return NULL;
    }
    return (HOPEvalArray*)value->s.bytes;
}

static HOPEvalArray* _Nullable HOPEvalAllocArrayView(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    int32_t               elemTypeNode,
    HOPCTFEValue* _Nullable elems,
    uint32_t len) {
    HOPEvalArray* array;
    if (p == NULL) {
        return NULL;
    }
    array = (HOPEvalArray*)HOPArenaAlloc(
        p->arena, sizeof(HOPEvalArray), (uint32_t)_Alignof(HOPEvalArray));
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

static int HOPEvalAllocTupleValue(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    int32_t              typeNode,
    const HOPCTFEValue*  elems,
    uint32_t             len,
    HOPCTFEValue*        outValue,
    int*                 outIsConst) {
    HOPEvalArray* array;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || file == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    array = HOPEvalAllocArrayView(p, file, typeNode, -1, NULL, len);
    if (array == NULL) {
        return ErrorSimple("out of memory");
    }
    if (len > 0) {
        array->elems = (HOPCTFEValue*)HOPArenaAlloc(
            p->arena, sizeof(HOPCTFEValue) * len, (uint32_t)_Alignof(HOPCTFEValue));
        if (array->elems == NULL) {
            return ErrorSimple("out of memory");
        }
        memcpy(array->elems, elems, sizeof(HOPCTFEValue) * len);
    }
    HOPEvalValueSetArray(outValue, file, typeNode, array);
    *outIsConst = 1;
    return 0;
}

static const HOPCTFEValue* HOPEvalValueTargetOrSelf(const HOPCTFEValue* value) {
    const HOPCTFEValue* target = HOPEvalValueReferenceTarget(value);
    return target != NULL ? target : value;
}

static int HOPEvalOptionalPayload(const HOPCTFEValue* value, const HOPCTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (value == NULL || value->kind != HOPCTFEValue_OPTIONAL || outPayload == NULL) {
        return 0;
    }
    if (value->b == 0u) {
        return 1;
    }
    if (value->s.bytes == NULL) {
        return 0;
    }
    *outPayload = (const HOPCTFEValue*)value->s.bytes;
    return 1;
}

static int HOPEvalResolveSimpleAliasCastTarget(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    char*                 outTargetKind,
    uint64_t* _Nullable outAliasTag);

static int HOPEvalCoerceValueToTypeNode(
    HOPEvalProgram* p, const HOPParsedFile* typeFile, int32_t typeNode, HOPCTFEValue* inOutValue) {
    const HOPAstNode*   type;
    const HOPCTFEValue* sourceValue;
    HOPEvalAggregate*   sourceAgg;
    const HOPCTFEValue* optionalPayload = NULL;
    int32_t             targetTypeCode = HOPEvalTypeCode_INVALID;
    uint32_t            i;
    if (p == NULL || typeFile == NULL || typeNode < 0 || inOutValue == NULL
        || (uint32_t)typeNode >= typeFile->ast.len)
    {
        return -1;
    }
    if (HOPEvalTypeNodeIsAnytype(typeFile, typeNode)) {
        HOPEvalAnnotateUntypedLiteralValue(inOutValue);
        return 0;
    }
    if (HOPEvalTypeNodeIsTemplateParamName(typeFile, typeNode)) {
        HOPEvalAnnotateUntypedLiteralValue(inOutValue);
        return 0;
    }
    type = &typeFile->ast.nodes[typeNode];
    sourceValue = HOPEvalValueTargetOrSelf(inOutValue);
    if (type->kind == HOPAst_TYPE_OPTIONAL) {
        int32_t payloadTypeNode = type->firstChild;
        if (sourceValue->kind == HOPCTFEValue_OPTIONAL) {
            return 0;
        }
        if (sourceValue->kind == HOPCTFEValue_NULL) {
            inOutValue->kind = HOPCTFEValue_OPTIONAL;
            inOutValue->i64 = 0;
            inOutValue->f64 = 0.0;
            inOutValue->b = 0u;
            inOutValue->typeTag = 0;
            inOutValue->s.bytes = NULL;
            inOutValue->s.len = 0;
            return 0;
        }
        {
            HOPCTFEValue  payloadValue = *inOutValue;
            HOPCTFEValue* payloadCopy;
            if (payloadTypeNode >= 0
                && HOPEvalCoerceValueToTypeNode(p, typeFile, payloadTypeNode, &payloadValue) != 0)
            {
                return -1;
            }
            payloadCopy = (HOPCTFEValue*)HOPArenaAlloc(
                p->arena, sizeof(HOPCTFEValue), (uint32_t)_Alignof(HOPCTFEValue));
            if (payloadCopy == NULL) {
                return ErrorSimple("out of memory");
            }
            *payloadCopy = payloadValue;
            inOutValue->kind = HOPCTFEValue_OPTIONAL;
            inOutValue->i64 = 0;
            inOutValue->f64 = 0.0;
            inOutValue->b = 1u;
            inOutValue->typeTag = 0;
            inOutValue->s.bytes = (const uint8_t*)payloadCopy;
            inOutValue->s.len = 0;
            return 0;
        }
    }
    if (type->kind != HOPAst_TYPE_OPTIONAL && sourceValue->kind == HOPCTFEValue_OPTIONAL
        && HOPEvalOptionalPayload(sourceValue, &optionalPayload))
    {
        if (sourceValue->b == 0u || optionalPayload == NULL) {
            return 0;
        }
        *inOutValue = *optionalPayload;
        sourceValue = inOutValue;
    }
    if (type->kind == HOPAst_TYPE_NAME) {
        char     aliasTargetKind = '\0';
        uint64_t aliasTag = 0;
        if (HOPEvalResolveSimpleAliasCastTarget(p, typeFile, typeNode, &aliasTargetKind, &aliasTag))
        {
            if ((aliasTargetKind == 'i' && sourceValue->kind == HOPCTFEValue_INT)
                || (aliasTargetKind == 'f' && sourceValue->kind == HOPCTFEValue_FLOAT)
                || (aliasTargetKind == 'b' && sourceValue->kind == HOPCTFEValue_BOOL)
                || (aliasTargetKind == 's' && sourceValue->kind == HOPCTFEValue_STRING))
            {
                *inOutValue = *sourceValue;
                inOutValue->typeTag = aliasTag;
                return 0;
            }
        }
    }
    sourceAgg = HOPEvalValueAsAggregate(sourceValue);
    if ((type->kind == HOPAst_TYPE_NAME || type->kind == HOPAst_TYPE_ANON_STRUCT
         || type->kind == HOPAst_TYPE_ANON_UNION)
        && sourceAgg != NULL)
    {
        HOPCTFEValue        targetValue;
        int                 targetIsConst = 0;
        HOPEvalAggregate*   targetAgg;
        uint8_t*            explicitSet = NULL;
        HOPCTFEExecBinding* fieldBindings = NULL;
        HOPCTFEExecEnv      fieldFrame;
        uint32_t            fieldBindingCap = 0;
        int                 finalizeRc = 0;
        int sourcePartial = (sourceValue->typeTag & HOPCTFEValueTag_AGG_PARTIAL) != 0u;
        if (HOPEvalZeroInitTypeNode(p, typeFile, typeNode, &targetValue, &targetIsConst) != 0) {
            return -1;
        }
        if (!targetIsConst) {
            return 0;
        }
        targetAgg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(&targetValue));
        if (targetAgg == NULL) {
            return 0;
        }
        if (targetAgg->fieldLen > 0) {
            fieldBindingCap = HOPEvalAggregateFieldBindingCount(targetAgg);
            explicitSet = (uint8_t*)HOPArenaAlloc(
                p->arena, targetAgg->fieldLen, (uint32_t)_Alignof(uint8_t));
            fieldBindings = (HOPCTFEExecBinding*)HOPArenaAlloc(
                p->arena,
                sizeof(HOPCTFEExecBinding) * fieldBindingCap,
                (uint32_t)_Alignof(HOPCTFEExecBinding));
            if (explicitSet == NULL || fieldBindings == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(explicitSet, 0, targetAgg->fieldLen);
            memset(fieldBindings, 0, sizeof(HOPCTFEExecBinding) * fieldBindingCap);
        }
        for (i = 0; i < sourceAgg->fieldLen; i++) {
            const HOPEvalAggregateField* field = &sourceAgg->fields[i];
            HOPCTFEValue                 fieldValue = field->value;
            HOPEvalAggregate*            embeddedFieldAgg = HOPEvalValueAsAggregate(&field->value);
            int                          nestedReserved =
                (field->flags & HOPAstFlag_FIELD_EMBEDDED) != 0
                && HOPEvalAggregateHasReservedFields(embeddedFieldAgg);
            if (sourcePartial && field->typeNode >= 0 && (field->_reserved & 1u) == 0u
                && !nestedReserved)
            {
                continue;
            }
            if (explicitSet != NULL) {
                uint32_t j;
                for (j = 0; j < targetAgg->fieldLen; j++) {
                    HOPEvalAggregateField* targetField = &targetAgg->fields[j];
                    if (SliceEqSlice(
                            sourceAgg->file->source,
                            field->nameStart,
                            field->nameEnd,
                            targetAgg->file->source,
                            targetField->nameStart,
                            targetField->nameEnd))
                    {
                        if (!nestedReserved
                            && (!sourcePartial || (field->_reserved & 1u) != 0u
                                || field->typeNode < 0))
                        {
                            explicitSet[j] = 1u;
                        }
                        if (targetField->typeNode >= 0
                            && HOPEvalCoerceValueToTypeNode(
                                   p, targetAgg->file, targetField->typeNode, &fieldValue)
                                   != 0)
                        {
                            return -1;
                        }
                        break;
                    }
                }
            }
            if (nestedReserved && sourcePartial && (field->_reserved & 1u) == 0u) {
                continue;
            }
            if (!HOPEvalValueSetFieldPath(
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
            HOPEvalAggregateField* field = &targetAgg->fields[i];
            if ((explicitSet == NULL || explicitSet[i] == 0u) && field->defaultExprNode >= 0) {
                HOPCTFEValue defaultValue;
                int          defaultIsConst = 0;
                if (HOPEvalExecExprInFileWithType(
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
            if (sourcePartial && (field->flags & HOPAstFlag_FIELD_EMBEDDED) != 0) {
                HOPEvalAggregateField* sourceField = HOPEvalAggregateFindDirectField(
                    sourceAgg, targetAgg->file->source, field->nameStart, field->nameEnd);
                if (sourceField != NULL
                    && !HOPEvalReplayReservedAggregateFields(
                        &targetValue, HOPEvalValueAsAggregate(&sourceField->value)))
                {
                    return 0;
                }
            }
            if (fieldBindings != NULL
                && HOPEvalAppendAggregateFieldBindings(
                       fieldBindings, fieldBindingCap, &fieldFrame, field)
                       != 0)
            {
                return ErrorSimple("out of memory");
            }
            field->_reserved = 0;
        }
        finalizeRc = HOPEvalFinalizeAggregateVarArrays(p, targetAgg);
        if (finalizeRc != 1) {
            return finalizeRc < 0 ? -1 : 0;
        }
        *inOutValue = targetValue;
    }
    if (HOPEvalStringViewTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)
        && sourceValue->kind != HOPCTFEValue_STRING)
    {
        HOPCTFEValue stringValue;
        int          stringRc = HOPEvalStringValueFromArrayBytes(
            p->arena, sourceValue, targetTypeCode, &stringValue);
        if (stringRc < 0) {
            return -1;
        }
        if (stringRc > 0) {
            *inOutValue = stringValue;
        }
    }
    if (HOPEvalAdaptStringValueForType(p->arena, typeFile, typeNode, inOutValue, inOutValue) < 0) {
        return -1;
    }
    sourceValue = HOPEvalValueTargetOrSelf(inOutValue);
    if (HOPEvalTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)) {
        if (sourceValue->kind == HOPCTFEValue_BOOL && targetTypeCode == HOPEvalTypeCode_BOOL) {
            HOPEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == HOPCTFEValue_FLOAT
            && (targetTypeCode == HOPEvalTypeCode_F32 || targetTypeCode == HOPEvalTypeCode_F64))
        {
            HOPEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == HOPCTFEValue_INT
            && (targetTypeCode == HOPEvalTypeCode_U8 || targetTypeCode == HOPEvalTypeCode_U16
                || targetTypeCode == HOPEvalTypeCode_U32 || targetTypeCode == HOPEvalTypeCode_U64
                || targetTypeCode == HOPEvalTypeCode_UINT || targetTypeCode == HOPEvalTypeCode_I8
                || targetTypeCode == HOPEvalTypeCode_I16 || targetTypeCode == HOPEvalTypeCode_I32
                || targetTypeCode == HOPEvalTypeCode_I64 || targetTypeCode == HOPEvalTypeCode_INT))
        {
            HOPEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            targetTypeCode == HOPEvalTypeCode_RAWPTR
            && (sourceValue->kind == HOPCTFEValue_REFERENCE
                || sourceValue->kind == HOPCTFEValue_NULL
                || sourceValue->kind == HOPCTFEValue_STRING))
        {
            HOPEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == HOPCTFEValue_STRING
            && (targetTypeCode == HOPEvalTypeCode_STR_REF
                || targetTypeCode == HOPEvalTypeCode_STR_PTR))
        {
            HOPEvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        }
    }
    return 0;
}

static int HOPEvalForInIndexCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    const HOPCTFEValue* sourceValue,
    uint32_t            index,
    int                 byRef,
    HOPCTFEValue*       outValue,
    int*                outIsConst) {
    const HOPCTFEValue* targetValue;
    HOPEvalArray*       array;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (ctx == NULL || execCtx == NULL || sourceValue == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    targetValue = HOPEvalValueTargetOrSelf(sourceValue);
    if (targetValue->kind == HOPCTFEValue_ARRAY) {
        array = HOPEvalValueAsArray(targetValue);
        if (array == NULL || index >= array->len) {
            return 0;
        }
        if (byRef) {
            HOPEvalValueSetReference(outValue, &array->elems[index]);
        } else {
            *outValue = array->elems[index];
        }
        *outIsConst = 1;
        return 0;
    }
    if (targetValue->kind == HOPCTFEValue_STRING) {
        if (index >= targetValue->s.len) {
            return 0;
        }
        if (byRef) {
            HOPCTFEValue* byteValue = (HOPCTFEValue*)HOPArenaAlloc(
                ((HOPEvalProgram*)ctx)->arena,
                sizeof(HOPCTFEValue),
                (uint32_t)_Alignof(HOPCTFEValue));
            if (byteValue == NULL) {
                return ErrorSimple("out of memory");
            }
            HOPEvalValueSetInt(byteValue, (int64_t)targetValue->s.bytes[index]);
            HOPEvalValueSetRuntimeTypeCode(byteValue, HOPEvalTypeCode_U8);
            HOPEvalValueSetReference(outValue, byteValue);
            *outIsConst = 1;
            return 0;
        }
        HOPEvalValueSetInt(outValue, (int64_t)targetValue->s.bytes[index]);
        HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_U8);
        *outIsConst = 1;
        return 0;
    }
    HOPCTFEExecSetReason(execCtx, 0, 0, "for-in loop source is not supported in evaluator backend");
    return 0;
}

static int HOPEvalCollectCallArgs(
    HOPEvalProgram* p,
    const HOPAst*   ast,
    int32_t         firstArgNode,
    HOPCTFEValue**  outArgs,
    uint32_t*       outArgCount,
    int32_t* _Nullable outLastArgNode) {
    HOPCTFEValue tempArgs[256];
    uint32_t     argCount = 0;
    int32_t      argNode = firstArgNode;
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
        int32_t      argExprNode = argNode;
        HOPCTFEValue argValue;
        int          argIsConst = 0;
        if (ast->nodes[argNode].kind == HOPAst_CALL_ARG) {
            argExprNode = ast->nodes[argNode].firstChild;
        }
        if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
            *outArgCount = 0;
            *outArgs = NULL;
            return 0;
        }
        if (HOPEvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
            return -1;
        }
        if (!argIsConst) {
            *outArgCount = 0;
            *outArgs = NULL;
            return 0;
        }
        HOPEvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
        if (ast->nodes[argNode].kind == HOPAst_CALL_ARG
            && (ast->nodes[argNode].flags & HOPAstFlag_CALL_ARG_SPREAD) != 0)
        {
            HOPEvalArray* array = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&argValue));
            uint32_t      i;
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
        *outArgs = (HOPCTFEValue*)HOPArenaAlloc(
            p->arena, sizeof(HOPCTFEValue) * argCount, (uint32_t)_Alignof(HOPCTFEValue));
        if (*outArgs == NULL) {
            return ErrorSimple("out of memory");
        }
        memcpy(*outArgs, tempArgs, sizeof(HOPCTFEValue) * argCount);
    }
    *outArgCount = argCount;
    return 1;
}

static int HOPEvalCurrentContextFieldByLiteral(
    const HOPEvalProgram* p, const char* fieldName, HOPCTFEValue* outValue) {
    const HOPEvalContext* context;
    if (p == NULL || fieldName == NULL || outValue == NULL) {
        return 0;
    }
    context = p->currentContext != NULL ? p->currentContext : &p->rootContext;
    if (strcmp(fieldName, "allocator") == 0) {
        *outValue = context->allocator;
        return 1;
    }
    if (strcmp(fieldName, "temp_allocator") == 0) {
        *outValue = context->tempAllocator;
        return 1;
    }
    if (strcmp(fieldName, "logger") == 0) {
        *outValue = context->logger;
        return 1;
    }
    return 0;
}

static int HOPEvalCurrentContextFieldAddressByLiteral(
    HOPEvalProgram* p, const char* fieldName, HOPCTFEValue* outValue) {
    HOPEvalContext* context;
    if (p == NULL || fieldName == NULL || outValue == NULL) {
        return 0;
    }
    context = p->currentContext != NULL ? (HOPEvalContext*)p->currentContext : &p->rootContext;
    if (strcmp(fieldName, "allocator") == 0) {
        HOPEvalValueSetReference(outValue, &context->allocator);
        return 1;
    }
    if (strcmp(fieldName, "temp_allocator") == 0) {
        HOPEvalValueSetReference(outValue, &context->tempAllocator);
        return 1;
    }
    if (strcmp(fieldName, "logger") == 0) {
        HOPEvalValueSetReference(outValue, &context->logger);
        return 1;
    }
    return 0;
}

static int HOPEvalCurrentContextField(
    const HOPEvalProgram* p,
    const char*           source,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    HOPCTFEValue*         outValue) {
    if (source == NULL) {
        return 0;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "allocator")) {
        return HOPEvalCurrentContextFieldByLiteral(p, "allocator", outValue);
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "temp_allocator")) {
        return HOPEvalCurrentContextFieldByLiteral(p, "temp_allocator", outValue);
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "logger")) {
        return HOPEvalCurrentContextFieldByLiteral(p, "logger", outValue);
    }
    return 0;
}

static int HOPEvalMirContextGet(
    void* _Nullable ctx,
    uint32_t         fieldId,
    HOPMirExecValue* outValue,
    int*             outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    const char*     fieldName = NULL;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    switch ((HOPMirContextField)fieldId) {
        case HOPMirContextField_ALLOCATOR:      fieldName = "allocator"; break;
        case HOPMirContextField_TEMP_ALLOCATOR: fieldName = "temp_allocator"; break;
        case HOPMirContextField_LOGGER:         fieldName = "logger"; break;
        default:                                return 0;
    }
    if (!HOPEvalCurrentContextFieldByLiteral(p, fieldName, outValue)) {
        return 0;
    }
    *outIsConst = 1;
    return 1;
}

static int HOPEvalMirContextAddr(
    void* _Nullable ctx,
    uint32_t         fieldId,
    HOPMirExecValue* outValue,
    int*             outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    const char*     fieldName = NULL;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    switch ((HOPMirContextField)fieldId) {
        case HOPMirContextField_ALLOCATOR:      fieldName = "allocator"; break;
        case HOPMirContextField_TEMP_ALLOCATOR: fieldName = "temp_allocator"; break;
        case HOPMirContextField_LOGGER:         fieldName = "logger"; break;
        default:                                return 0;
    }
    if (!HOPEvalCurrentContextFieldAddressByLiteral(p, fieldName, outValue)) {
        return 0;
    }
    *outIsConst = 1;
    return 1;
}

static int HOPEvalBuildContextOverlay(
    HOPEvalProgram* p, int32_t overlayNode, HOPEvalContext* outContext, const HOPParsedFile* file);

static int HOPEvalMirEvalWithContext(
    void* _Nullable ctx,
    uint32_t         sourceNode,
    HOPMirExecValue* outValue,
    int*             outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*       p = (HOPEvalProgram*)ctx;
    const HOPEvalContext* savedContext;
    HOPEvalContext        overlayContext;
    const HOPAstNode*     n;
    int32_t               exprNode;
    int32_t               overlayNode;
    int                   overlayRc;
    int                   rc;
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
    if (n->kind != HOPAst_CALL_WITH_CONTEXT) {
        return 0;
    }
    exprNode = n->firstChild;
    overlayNode = exprNode >= 0 ? p->currentFile->ast.nodes[exprNode].nextSibling : -1;
    if (exprNode < 0) {
        return 0;
    }
    overlayRc = HOPEvalBuildContextOverlay(p, overlayNode, &overlayContext, p->currentFile);
    if (overlayRc != 1) {
        return overlayRc < 0 ? -1 : 0;
    }
    savedContext = p->currentContext;
    p->currentContext = &overlayContext;
    rc = HOPEvalExecExprCb(p, exprNode, outValue, outIsConst);
    p->currentContext = savedContext;
    if (rc != 0) {
        return -1;
    }
    return *outIsConst ? 1 : 0;
}

static int HOPEvalBuildContextOverlay(
    HOPEvalProgram* p, int32_t overlayNode, HOPEvalContext* outContext, const HOPParsedFile* file) {
    const HOPAst* ast;
    int32_t       bindNode;
    if (p == NULL || outContext == NULL || file == NULL || p->currentFile == NULL) {
        return -1;
    }
    ast = &file->ast;
    *outContext = p->currentContext != NULL ? *p->currentContext : p->rootContext;
    if (overlayNode < 0 || (uint32_t)overlayNode >= ast->len) {
        return 1;
    }
    if (ast->nodes[overlayNode].kind != HOPAst_CONTEXT_OVERLAY) {
        return 0;
    }
    bindNode = ASTFirstChild(ast, overlayNode);
    while (bindNode >= 0) {
        const HOPAstNode* bind = &ast->nodes[bindNode];
        int32_t           exprNode = ASTFirstChild(ast, bindNode);
        HOPCTFEValue      fieldValue;
        int               fieldIsConst = 0;
        if (bind->kind != HOPAst_CONTEXT_BIND) {
            bindNode = ASTNextSibling(ast, bindNode);
            continue;
        }
        if (exprNode >= 0) {
            if (HOPEvalExecExprCb(p, exprNode, &fieldValue, &fieldIsConst) != 0) {
                return -1;
            }
            if (!fieldIsConst) {
                return 0;
            }
        } else if (!HOPEvalCurrentContextField(
                       p, file->source, bind->dataStart, bind->dataEnd, &fieldValue))
        {
            if (p->currentExecCtx != NULL) {
                HOPCTFEExecSetReason(
                    p->currentExecCtx,
                    bind->dataStart,
                    bind->dataEnd,
                    "context field is not available in evaluator backend");
            }
            return 0;
        }
        if (SliceEqCStr(file->source, bind->dataStart, bind->dataEnd, "allocator")) {
            outContext->allocator = fieldValue;
        } else if (SliceEqCStr(file->source, bind->dataStart, bind->dataEnd, "temp_allocator")) {
            outContext->tempAllocator = fieldValue;
        } else if (SliceEqCStr(file->source, bind->dataStart, bind->dataEnd, "logger")) {
            outContext->logger = fieldValue;
        }
        bindNode = ASTNextSibling(ast, bindNode);
    }
    return 1;
}

static void HOPEvalValueSetReference(HOPCTFEValue* value, HOPCTFEValue* target) {
    if (value == NULL) {
        return;
    }
    value->kind = HOPCTFEValue_REFERENCE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = (const uint8_t*)target;
    value->s.len = 0;
}

static HOPCTFEValue* _Nullable HOPEvalValueReferenceTarget(const HOPCTFEValue* value) {
    if (value == NULL || value->kind != HOPCTFEValue_REFERENCE || value->s.bytes == NULL) {
        return NULL;
    }
    return (HOPCTFEValue*)value->s.bytes;
}

static int32_t HOPEvalFindNamedAggregateDeclInPackage(
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    int32_t               typeNode,
    const HOPParsedFile** outFile) {
    const HOPAstNode* typeNameNode;
    uint32_t          lookupStart;
    uint32_t          lookupEnd;
    uint32_t          fileIndex;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (pkg == NULL || callerFile == NULL || typeNode < 0
        || (uint32_t)typeNode >= callerFile->ast.len)
    {
        return -1;
    }
    typeNameNode = &callerFile->ast.nodes[typeNode];
    if (typeNameNode->kind != HOPAst_TYPE_NAME) {
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
        const HOPParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t              nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const HOPAstNode* n = &pkgFile->ast.nodes[nodeId];
            if ((n->kind == HOPAst_STRUCT || n->kind == HOPAst_UNION)
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

static int32_t HOPEvalFindNamedAggregateDecl(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    int32_t               typeNode,
    const HOPParsedFile** outFile) {
    const HOPPackage* currentPkg;
    uint32_t          pkgIndex;
    int32_t           nodeId;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = HOPEvalFindPackageByFile(p, callerFile);
    if (currentPkg != NULL) {
        nodeId = HOPEvalFindNamedAggregateDeclInPackage(currentPkg, callerFile, typeNode, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    if (p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &p->loader->packages[pkgIndex];
        if (pkg == currentPkg) {
            continue;
        }
        nodeId = HOPEvalFindNamedAggregateDeclInPackage(pkg, callerFile, typeNode, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    return -1;
}

static int32_t HOPEvalFindNamedEnumDeclInPackage(
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const HOPParsedFile** outFile) {
    uint32_t fileIndex;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (pkg == NULL || callerFile == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const HOPParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t              nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const HOPAstNode* n = &pkgFile->ast.nodes[nodeId];
            if (n->kind == HOPAst_ENUM
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

static int32_t HOPEvalFindNamedEnumDecl(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const HOPParsedFile** outFile) {
    const HOPPackage* currentPkg;
    uint32_t          pkgIndex;
    int32_t           nodeId;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = HOPEvalFindPackageByFile(p, callerFile);
    if (currentPkg != NULL) {
        nodeId = HOPEvalFindNamedEnumDeclInPackage(
            currentPkg, callerFile, nameStart, nameEnd, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    if (p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &p->loader->packages[pkgIndex];
        if (pkg == currentPkg) {
            continue;
        }
        nodeId = HOPEvalFindNamedEnumDeclInPackage(pkg, callerFile, nameStart, nameEnd, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    return -1;
}

static int HOPEvalFindEnumVariant(
    const HOPParsedFile* enumFile,
    int32_t              enumNode,
    const char*          source,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    int32_t*             outVariantNode,
    uint32_t*            outTagIndex) {
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
    if (child >= 0 && enumFile->ast.nodes[child].kind != HOPAst_FIELD) {
        child = ASTNextSibling(&enumFile->ast, child);
    }
    while (child >= 0) {
        const HOPAstNode* fieldNode = &enumFile->ast.nodes[child];
        if (fieldNode->kind == HOPAst_FIELD) {
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

static int HOPEvalEnumHasPayloadVariants(const HOPParsedFile* enumFile, int32_t enumNode) {
    int32_t child;
    if (enumFile == NULL || enumNode < 0 || (uint32_t)enumNode >= enumFile->ast.len) {
        return 0;
    }
    child = ASTFirstChild(&enumFile->ast, enumNode);
    while (child >= 0) {
        if (enumFile->ast.nodes[child].kind == HOPAst_FIELD) {
            int32_t valueNode = ASTFirstChild(&enumFile->ast, child);
            if (valueNode >= 0 && (uint32_t)valueNode < enumFile->ast.len
                && enumFile->ast.nodes[valueNode].kind == HOPAst_FIELD)
            {
                return 1;
            }
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    return 0;
}

static int HOPEvalZeroInitTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    HOPCTFEValue*         outValue,
    int*                  outIsConst);
static int HOPEvalResolveAggregateTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  typeFile,
    int32_t               typeNode,
    const HOPParsedFile** outDeclFile,
    int32_t*              outDeclNode);
static int HOPEvalExecExprWithTypeNode(
    HOPEvalProgram*      p,
    int32_t              exprNode,
    const HOPParsedFile* typeFile,
    int32_t              typeNode,
    HOPCTFEValue*        outValue,
    int*                 outIsConst);
static int HOPEvalExecExprInFileWithType(
    HOPEvalProgram*      p,
    const HOPParsedFile* exprFile,
    HOPCTFEExecEnv*      env,
    int32_t              exprNode,
    const HOPParsedFile* typeFile,
    int32_t              typeNode,
    HOPCTFEValue*        outValue,
    int*                 outIsConst);
static int HOPEvalExecExprCb(void* ctx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalAssignExprCb(
    void* ctx, HOPCTFEExecCtx* execCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalAssignValueExprCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    int32_t             lhsExprNode,
    const HOPCTFEValue* inValue,
    HOPCTFEValue*       outValue,
    int*                outIsConst);
static int HOPEvalMatchPatternCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    const HOPCTFEValue* subjectValue,
    int32_t             labelExprNode,
    int*                outMatched);
static int HOPEvalZeroInitCb(void* ctx, int32_t typeNode, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalMirZeroInitLocal(
    void*                ctx,
    const HOPMirTypeRef* typeRef,
    HOPCTFEValue*        outValue,
    int*                 outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirCoerceValueForType(
    void* ctx, const HOPMirTypeRef* typeRef, HOPCTFEValue* inOutValue, HOPDiag* _Nullable diag);
static int HOPEvalMirIndexValue(
    void*               ctx,
    const HOPCTFEValue* base,
    const HOPCTFEValue* index,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirIndexAddr(
    void*               ctx,
    const HOPCTFEValue* base,
    const HOPCTFEValue* index,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirSliceValue(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    const HOPCTFEValue* _Nullable start,
    const HOPCTFEValue* _Nullable end,
    uint16_t flags,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirAggGetField(
    void*               ctx,
    const HOPCTFEValue* base,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirAggAddrField(
    void*               ctx,
    const HOPCTFEValue* base,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirAggSetField(
    void* _Nullable ctx,
    HOPCTFEValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirMakeTuple(
    void*               ctx,
    const HOPCTFEValue* elems,
    uint32_t            elemCount,
    uint32_t            typeNodeHint,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirMakeVariadicPack(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirTypeRef* _Nullable paramTypeRef,
    uint16_t            callFlags,
    const HOPCTFEValue* args,
    uint32_t            argCount,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirHostCall(
    void*               ctx,
    uint32_t            hostId,
    const HOPCTFEValue* args,
    uint32_t            argCount,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalTryMirZeroInitType(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    int32_t              typeNode,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    HOPCTFEValue*        outValue,
    int*                 outIsConst);
static int HOPEvalTryMirEvalTopInit(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    int32_t              initExprNode,
    int32_t              declTypeNode,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const HOPParsedFile* _Nullable coerceTypeFile,
    int32_t       coerceTypeNode,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    int* _Nullable outSupported);
static int HOPEvalMirBuildTopInitProgram(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    int32_t              initExprNode,
    int32_t              declTypeNode,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    HOPMirProgram*       outProgram,
    HOPEvalMirExecCtx*   outExecCtx,
    uint32_t*            outRootMirFnIndex,
    int*                 outSupported);
static void HOPEvalMirAdaptOutValue(
    const HOPEvalMirExecCtx* c, HOPCTFEValue* _Nullable value, int* _Nullable inOutIsConst);
static int HOPEvalTryMirEvalExprWithType(
    HOPEvalProgram*      p,
    int32_t              exprNode,
    const HOPParsedFile* exprFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const HOPParsedFile* _Nullable typeFile,
    int32_t       typeNode,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    int*          outSupported);
static int HOPEvalMirEnterFunction(
    void* ctx, uint32_t functionIndex, uint32_t sourceRef, HOPDiag* _Nullable diag);
static void HOPEvalMirLeaveFunction(void* ctx);
static int  HOPEvalMirBindFrame(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPCTFEValue* _Nullable locals,
    uint32_t localCount,
    HOPDiag* _Nullable diag);
static void HOPEvalMirUnbindFrame(void* _Nullable ctx);
static void HOPEvalMirInitExecEnv(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    HOPMirExecEnv*       env,
    HOPEvalMirExecCtx* _Nullable functionCtx);
static int HOPEvalMirEvalBinary(
    void* _Nullable ctx,
    HOPTokenKind op,
    const HOPMirExecValue* _Nonnull lhs,
    const HOPMirExecValue* _Nonnull rhs,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirAllocNew(
    void* _Nullable ctx,
    uint32_t sourceNode,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirContextGet(
    void* _Nullable ctx,
    uint32_t fieldId,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirEvalWithContext(
    void* _Nullable ctx,
    uint32_t sourceNode,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalResolveIdent(
    void*         ctx,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirAssignIdent(
    void*               ctx,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* inValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalResolveCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalResolveCallMir(
    void* ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalResolveCallMirPre(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalEvalTopVar(
    HOPEvalProgram* p, uint32_t topVarIndex, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalInvokeFunction(
    HOPEvalProgram* p,
    int32_t         fnIndex,
    const HOPCTFEValue* _Nullable args,
    uint32_t              argCount,
    const HOPEvalContext* callContext,
    HOPCTFEValue*         outValue,
    int*                  outDidReturn);

static int32_t HOPEvalAggregateLookupFieldIndex(
    const HOPEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const HOPEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return (int32_t)i;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const HOPEvalAggregateField* field = &agg->fields[i];
        HOPEvalAggregate*            embedded = NULL;
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = HOPEvalValueAsAggregate(&field->value);
        if (embedded != NULL) {
            int32_t nested = HOPEvalAggregateLookupFieldIndex(embedded, source, nameStart, nameEnd);
            if (nested >= 0) {
                return (int32_t)i;
            }
        }
    }
    return -1;
}

static int32_t HOPEvalAggregateLookupDirectFieldIndex(
    const HOPEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const HOPEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static HOPEvalAggregateField* _Nullable HOPEvalAggregateLookupField(
    HOPEvalAggregate* agg,
    const char*       source,
    uint32_t          nameStart,
    uint32_t          nameEnd,
    HOPEvalAggregate** _Nullable outOwner) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            if (outOwner != NULL) {
                *outOwner = agg;
            }
            return field;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        HOPEvalAggregate*      embedded = NULL;
        HOPEvalAggregateField* nested = NULL;
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = HOPEvalValueAsAggregate(&field->value);
        if (embedded == NULL) {
            continue;
        }
        nested = HOPEvalAggregateLookupField(embedded, source, nameStart, nameEnd, outOwner);
        if (nested != NULL) {
            return nested;
        }
    }
    return NULL;
}

static uint32_t HOPEvalAggregateFieldBindingCount(const HOPEvalAggregate* agg) {
    uint32_t i;
    uint32_t count = 0;
    if (agg == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const HOPEvalAggregateField* field = &agg->fields[i];
        count++;
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) != 0) {
            HOPEvalAggregate* embedded = HOPEvalValueAsAggregate(&field->value);
            count += HOPEvalAggregateFieldBindingCount(embedded);
        }
    }
    return count;
}

static int HOPEvalAppendAggregateFieldBindings(
    HOPCTFEExecBinding*    fieldBindings,
    uint32_t               bindingCap,
    HOPCTFEExecEnv*        fieldFrame,
    HOPEvalAggregateField* field) {
    HOPEvalAggregate* embedded;
    uint32_t          i;
    if (fieldBindings == NULL || fieldFrame == NULL || field == NULL) {
        return 0;
    }
    if (fieldFrame->bindingLen >= bindingCap) {
        return -1;
    }
    fieldBindings[fieldFrame->bindingLen].nameStart = field->nameStart;
    fieldBindings[fieldFrame->bindingLen].nameEnd = field->nameEnd;
    fieldBindings[fieldFrame->bindingLen].typeId = -1;
    fieldBindings[fieldFrame->bindingLen].typeNode = field->typeNode;
    fieldBindings[fieldFrame->bindingLen].mutable = 1u;
    fieldBindings[fieldFrame->bindingLen].value = field->value;
    fieldFrame->bindingLen++;
    if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) == 0) {
        return 0;
    }
    embedded = HOPEvalValueAsAggregate(&field->value);
    if (embedded == NULL) {
        return 0;
    }
    for (i = 0; i < embedded->fieldLen; i++) {
        if (HOPEvalAppendAggregateFieldBindings(
                fieldBindings, bindingCap, fieldFrame, &embedded->fields[i])
            != 0)
        {
            return -1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t     nameStart;
    uint32_t     nameEnd;
    int32_t      topFieldIndex;
    HOPCTFEValue value;
} HOPEvalExplicitAggregateField;

static int HOPEvalAggregateGetFieldValue(
    const HOPEvalAggregate* agg,
    const char*             source,
    uint32_t                nameStart,
    uint32_t                nameEnd,
    HOPCTFEValue*           outValue) {
    int32_t fieldIndex;
    if (outValue == NULL) {
        return 0;
    }
    fieldIndex = HOPEvalAggregateLookupFieldIndex(agg, source, nameStart, nameEnd);
    if (fieldIndex < 0) {
        return 0;
    }
    {
        const HOPEvalAggregateField* field = &agg->fields[fieldIndex];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            *outValue = field->value;
            return 1;
        }
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) != 0) {
            HOPEvalAggregate* embedded = HOPEvalValueAsAggregate(&field->value);
            if (embedded != NULL) {
                return HOPEvalAggregateGetFieldValue(
                    embedded, source, nameStart, nameEnd, outValue);
            }
        }
    }
    return 0;
}

static HOPCTFEValue* _Nullable HOPEvalAggregateLookupFieldValuePtr(
    HOPEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return &field->value;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        HOPEvalAggregate*      embedded;
        HOPCTFEValue*          nested;
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = HOPEvalValueAsAggregate(&field->value);
        if (embedded == NULL) {
            continue;
        }
        nested = HOPEvalAggregateLookupFieldValuePtr(embedded, source, nameStart, nameEnd);
        if (nested != NULL) {
            return nested;
        }
    }
    return NULL;
}

static int HOPEvalAggregateSetFieldValue(
    HOPEvalAggregate*   agg,
    const char*         source,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* inValue,
    HOPCTFEValue* _Nullable outValue) {
    uint32_t i;
    if (agg == NULL || source == NULL || inValue == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            field->_reserved |= 1u;
            field->value = *inValue;
            if (outValue != NULL) {
                *outValue = field->value;
            }
            return 1;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        HOPEvalAggregate*      embedded = NULL;
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = HOPEvalValueAsAggregate(&field->value);
        if (embedded != NULL
            && HOPEvalAggregateSetFieldValue(
                embedded, source, nameStart, nameEnd, inValue, outValue))
        {
            return 1;
        }
    }
    return 0;
}

static HOPEvalAggregateField* _Nullable HOPEvalAggregateFindDirectField(
    HOPEvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return field;
        }
    }
    return NULL;
}

static int HOPEvalValueSetFieldPath(
    HOPCTFEValue*       value,
    const char*         source,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* inValue) {
    uint32_t i;
    if (value == NULL || source == NULL || inValue == NULL) {
        return 0;
    }
    for (i = nameStart; i < nameEnd; i++) {
        if (source[i] == '.') {
            HOPCTFEValue*          childValue = NULL;
            HOPEvalAggregateField* field;
            HOPEvalAggregate*      agg = HOPEvalValueAsAggregate(value);
            HOPEvalTaggedEnum*     tagged = HOPEvalValueAsTaggedEnum(value);
            if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
                agg = tagged->payload;
            }
            if (agg == NULL) {
                agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(value));
            }
            if (agg == NULL) {
                return 0;
            }
            field = HOPEvalAggregateFindDirectField(agg, source, nameStart, i);
            if (field == NULL) {
                return 0;
            }
            childValue = &field->value;
            return HOPEvalValueSetFieldPath(childValue, source, i + 1u, nameEnd, inValue);
        }
    }
    if (value->kind == HOPCTFEValue_STRING) {
        if (SliceEqCStr(source, nameStart, nameEnd, "len") && inValue->kind == HOPCTFEValue_INT
            && inValue->i64 >= 0)
        {
            value->s.len = (uint32_t)inValue->i64;
            return 1;
        }
        return 0;
    }
    {
        HOPEvalAggregate*  agg = HOPEvalValueAsAggregate(value);
        HOPEvalTaggedEnum* tagged = HOPEvalValueAsTaggedEnum(value);
        if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
            agg = tagged->payload;
        }
        if (agg == NULL) {
            agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(value));
        }
        if (agg != NULL) {
            return HOPEvalAggregateSetFieldValue(agg, source, nameStart, nameEnd, inValue, NULL);
        }
    }
    return 0;
}

static int HOPEvalAggregateHasReservedFields(const HOPEvalAggregate* agg) {
    uint32_t i;
    if (agg == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const HOPEvalAggregateField* field = &agg->fields[i];
        if ((field->_reserved & 1u) != 0u) {
            return 1;
        }
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) != 0
            && HOPEvalAggregateHasReservedFields(HOPEvalValueAsAggregate(&field->value)))
        {
            return 1;
        }
    }
    return 0;
}

static int HOPEvalReplayReservedAggregateFields(
    HOPCTFEValue* target, const HOPEvalAggregate* sourceAgg) {
    uint32_t i;
    if (target == NULL || sourceAgg == NULL) {
        return 1;
    }
    for (i = 0; i < sourceAgg->fieldLen; i++) {
        const HOPEvalAggregateField* field = &sourceAgg->fields[i];
        if ((field->_reserved & 1u) != 0u
            && !HOPEvalValueSetFieldPath(
                target, sourceAgg->file->source, field->nameStart, field->nameEnd, &field->value))
        {
            return 0;
        }
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) != 0
            && !HOPEvalReplayReservedAggregateFields(
                target, HOPEvalValueAsAggregate(&field->value)))
        {
            return 0;
        }
    }
    return 1;
}

static int HOPEvalFinalizeAggregateVarArrays(HOPEvalProgram* p, HOPEvalAggregate* agg) {
    uint32_t i;
    if (p == NULL || agg == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        const HOPAstNode*      typeNode;
        HOPCTFEValue           lenValue;
        int64_t                len = 0;
        HOPEvalArray*          array;
        uint32_t               j;
        if (field->typeNode < 0 || (uint32_t)field->typeNode >= agg->file->ast.len) {
            continue;
        }
        typeNode = &agg->file->ast.nodes[field->typeNode];
        if (typeNode->kind != HOPAst_TYPE_VARRAY) {
            continue;
        }
        if (!HOPEvalAggregateGetFieldValue(
                agg, agg->file->source, typeNode->dataStart, typeNode->dataEnd, &lenValue)
            || HOPCTFEValueToInt64(&lenValue, &len) != 0 || len < 0)
        {
            return 0;
        }
        array = HOPEvalAllocArrayView(
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
            array->elems = (HOPCTFEValue*)HOPArenaAlloc(
                p->arena, sizeof(HOPCTFEValue) * array->len, (uint32_t)_Alignof(HOPCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(HOPCTFEValue) * array->len);
            for (j = 0; j < array->len; j++) {
                int elemIsConst = 0;
                if (HOPEvalZeroInitTypeNode(
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
        HOPEvalValueSetArray(&field->value, agg->file, field->typeNode, array);
    }
    return 1;
}

static int HOPEvalBuildTaggedEnumPayload(
    HOPEvalProgram*      p,
    const HOPParsedFile* enumFile,
    int32_t              variantNode,
    int32_t              compoundLitNode,
    HOPCTFEValue*        outValue,
    int*                 outIsConst) {
    uint32_t          fieldCount = 0;
    uint32_t          fieldIndex = 0;
    int32_t           child;
    HOPEvalAggregate* agg;
    int32_t           fieldNode;
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
        if (enumFile->ast.nodes[child].kind == HOPAst_FIELD) {
            fieldCount++;
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    agg = (HOPEvalAggregate*)HOPArenaAlloc(
        p->arena, sizeof(HOPEvalAggregate), (uint32_t)_Alignof(HOPEvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = enumFile;
    agg->nodeId = variantNode;
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (HOPEvalAggregateField*)HOPArenaAlloc(
            p->arena,
            sizeof(HOPEvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(HOPEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(HOPEvalAggregateField) * fieldCount);
    }
    child = ASTFirstChild(&enumFile->ast, variantNode);
    while (child >= 0) {
        const HOPAstNode* variantField = &enumFile->ast.nodes[child];
        if (variantField->kind == HOPAst_FIELD) {
            int32_t                fieldTypeNode = ASTFirstChild(&enumFile->ast, child);
            int                    fieldIsConst = 0;
            HOPEvalAggregateField* field = &agg->fields[fieldIndex++];
            field->nameStart = variantField->dataStart;
            field->nameEnd = variantField->dataEnd;
            field->typeNode = fieldTypeNode;
            if (HOPEvalZeroInitTypeNode(p, enumFile, fieldTypeNode, &field->value, &fieldIsConst)
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
        const HOPAstNode* compoundField = &p->currentFile->ast.nodes[fieldNode];
        int32_t           valueNode = ASTFirstChild(&p->currentFile->ast, fieldNode);
        HOPCTFEValue      fieldValue;
        int               fieldIsConst = 0;
        if (compoundField->kind != HOPAst_COMPOUND_FIELD || valueNode < 0) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
            return -1;
        }
        if (!fieldIsConst
            || !HOPEvalAggregateSetFieldValue(
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
    HOPEvalValueSetAggregate(outValue, enumFile, variantNode, agg);
    *outIsConst = 1;
    return 0;
}

static int HOPEvalZeroInitAggregateValue(
    const HOPEvalProgram* p,
    const HOPParsedFile*  declFile,
    int32_t               declNode,
    HOPCTFEValue*         outValue,
    int*                  outIsConst) {
    const HOPAstNode* aggregateDecl;
    HOPEvalAggregate* agg;
    uint32_t          fieldCount = 0;
    uint32_t          fieldIndex = 0;
    int32_t           child;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (outValue == NULL || p == NULL || declFile == NULL || declNode < 0
        || (uint32_t)declNode >= declFile->ast.len)
    {
        return -1;
    }
    aggregateDecl = &declFile->ast.nodes[declNode];
    if (aggregateDecl->kind != HOPAst_STRUCT && aggregateDecl->kind != HOPAst_UNION
        && aggregateDecl->kind != HOPAst_TYPE_ANON_STRUCT
        && aggregateDecl->kind != HOPAst_TYPE_ANON_UNION)
    {
        return 0;
    }
    child = ASTFirstChild(&declFile->ast, declNode);
    while (child >= 0) {
        if (declFile->ast.nodes[child].kind == HOPAst_FIELD) {
            fieldCount++;
        }
        child = ASTNextSibling(&declFile->ast, child);
    }
    agg = (HOPEvalAggregate*)HOPArenaAlloc(
        p->arena, sizeof(HOPEvalAggregate), (uint32_t)_Alignof(HOPEvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = declFile;
    agg->nodeId = declNode;
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (HOPEvalAggregateField*)HOPArenaAlloc(
            p->arena,
            sizeof(HOPEvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(HOPEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(HOPEvalAggregateField) * fieldCount);
    }
    child = ASTFirstChild(&declFile->ast, declNode);
    while (child >= 0) {
        const HOPAstNode* fieldNode = &declFile->ast.nodes[child];
        if (fieldNode->kind == HOPAst_FIELD) {
            int32_t fieldTypeNode = ASTFirstChild(&declFile->ast, child);
            int32_t fieldDefaultNode =
                fieldTypeNode >= 0 ? ASTNextSibling(&declFile->ast, fieldTypeNode) : -1;
            int                    fieldIsConst = 0;
            HOPEvalAggregateField* field = &agg->fields[fieldIndex++];
            field->nameStart = fieldNode->dataStart;
            field->nameEnd = fieldNode->dataEnd;
            field->flags = (uint16_t)fieldNode->flags;
            field->typeNode = fieldTypeNode;
            field->defaultExprNode = fieldDefaultNode;
            if (HOPEvalZeroInitTypeNode(p, declFile, fieldTypeNode, &field->value, &fieldIsConst)
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
    HOPEvalValueSetAggregate(outValue, declFile, declNode, agg);
    if (outIsConst != NULL) {
        *outIsConst = 1;
    }
    return 0;
}

static int HOPEvalTypeValueFromTypeNode(
    HOPEvalProgram* p, const HOPParsedFile* file, int32_t typeNode, HOPCTFEValue* outValue) {
    const HOPAstNode*     n;
    int32_t               childNode;
    int32_t               typeCode = HOPEvalTypeCode_INVALID;
    HOPEvalReflectedType* rt;
    uint32_t              arrayLen = 0;
    if (p == NULL || file == NULL || outValue == NULL || typeNode < 0
        || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    if (HOPEvalTypeCodeFromTypeNode(file, typeNode, &typeCode)) {
        HOPEvalValueSetSimpleTypeValue(outValue, typeCode);
        return 1;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == HOPAst_TYPE_NAME) {
        return HOPEvalResolveTypeValueName(p, file, n->dataStart, n->dataEnd, outValue);
    }
    if (n->kind != HOPAst_TYPE_PTR && n->kind != HOPAst_TYPE_REF && n->kind != HOPAst_TYPE_ARRAY) {
        return 0;
    }
    childNode = ASTFirstChild(&file->ast, typeNode);
    if (childNode < 0 || !HOPEvalTypeValueFromTypeNode(p, file, childNode, outValue)) {
        return 0;
    }
    rt = (HOPEvalReflectedType*)HOPArenaAlloc(
        p->arena, sizeof(HOPEvalReflectedType), (uint32_t)_Alignof(HOPEvalReflectedType));
    if (rt == NULL) {
        return -1;
    }
    memset(rt, 0, sizeof(*rt));
    if (n->kind == HOPAst_TYPE_PTR) {
        rt->kind = HOPEvalReflectType_PTR;
        rt->namedKind = HOPEvalTypeKind_POINTER;
    } else if (n->kind == HOPAst_TYPE_REF) {
        rt->kind = HOPEvalReflectType_PTR;
        rt->namedKind = HOPEvalTypeKind_REFERENCE;
    } else {
        if (!HOPEvalParseUintSlice(file->source, n->dataStart, n->dataEnd, &arrayLen)) {
            return 0;
        }
        rt->kind = HOPEvalReflectType_ARRAY;
        rt->namedKind = HOPEvalTypeKind_ARRAY;
        rt->arrayLen = arrayLen;
    }
    rt->elemType = *outValue;
    HOPEvalValueSetReflectedTypeValue(outValue, rt);
    return 1;
}

static HOPCTFEExecBinding* _Nullable HOPEvalFindBinding(
    const HOPCTFEExecCtx* _Nullable execCtx,
    const HOPParsedFile* file,
    uint32_t             nameStart,
    uint32_t             nameEnd);

static int HOPEvalTypeValueFromExprNode(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    const HOPAst*        ast,
    int32_t              exprNode,
    HOPCTFEValue*        outValue) {
    const HOPAstNode* n;
    if (p == NULL || file == NULL || ast == NULL || outValue == NULL || exprNode < 0
        || (uint32_t)exprNode >= ast->len)
    {
        return 0;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == HOPAst_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return 0;
        }
        n = &ast->nodes[exprNode];
    }
    if (HOPEvalTypeValueFromTypeNode(p, file, exprNode, outValue)) {
        return 1;
    }
    if (n->kind == HOPAst_IDENT) {
        HOPCTFEExecBinding* binding = HOPEvalFindBinding(
            p->currentExecCtx, file, n->dataStart, n->dataEnd);
        const HOPParsedFile* localTypeFile = NULL;
        int32_t              localTypeNode = -1;
        int32_t              visibleLocalTypeNode = -1;
        if (binding != NULL && binding->typeNode >= 0
            && !(
                file->ast.nodes[binding->typeNode].kind == HOPAst_TYPE_NAME
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[binding->typeNode].dataStart,
                    file->ast.nodes[binding->typeNode].dataEnd,
                    "anytype"))
            && HOPEvalTypeValueFromTypeNode(p, file, binding->typeNode, outValue))
        {
            return 1;
        }
        if (HOPEvalFindVisibleLocalTypeNodeByName(
                file, n->start, n->dataStart, n->dataEnd, &visibleLocalTypeNode)
            && visibleLocalTypeNode >= 0
            && !(
                file->ast.nodes[visibleLocalTypeNode].kind == HOPAst_TYPE_NAME
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[visibleLocalTypeNode].dataStart,
                    file->ast.nodes[visibleLocalTypeNode].dataEnd,
                    "anytype"))
            && HOPEvalTypeValueFromTypeNode(p, file, visibleLocalTypeNode, outValue))
        {
            return 1;
        }
        if (HOPEvalMirLookupLocalTypeNode(
                p, n->dataStart, n->dataEnd, &localTypeFile, &localTypeNode)
            && localTypeFile != NULL && localTypeNode >= 0
            && !(
                localTypeFile->ast.nodes[localTypeNode].kind == HOPAst_TYPE_NAME
                && SliceEqCStr(
                    localTypeFile->source,
                    localTypeFile->ast.nodes[localTypeNode].dataStart,
                    localTypeFile->ast.nodes[localTypeNode].dataEnd,
                    "anytype"))
            && HOPEvalTypeValueFromTypeNode(p, localTypeFile, localTypeNode, outValue))
        {
            return 1;
        }
        return HOPEvalResolveTypeValueName(p, file, n->dataStart, n->dataEnd, outValue);
    }
    return 0;
}

static int HOPEvalZeroInitTypeValue(
    const HOPEvalProgram* p,
    const HOPCTFEValue*   typeValue,
    HOPCTFEValue*         outValue,
    int*                  outIsConst) {
    int32_t               typeCode = HOPEvalTypeCode_INVALID;
    HOPEvalReflectedType* rt;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || typeValue == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (HOPEvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        switch (typeCode) {
            case HOPEvalTypeCode_BOOL:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 0;
            case HOPEvalTypeCode_F32:
            case HOPEvalTypeCode_F64:
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 0;
            case HOPEvalTypeCode_STR_REF:
            case HOPEvalTypeCode_STR_PTR:
                outValue->kind = HOPCTFEValue_STRING;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                HOPEvalValueSetRuntimeTypeCode(outValue, typeCode);
                *outIsConst = 1;
                return 0;
            case HOPEvalTypeCode_RAWPTR:
                HOPEvalValueSetNull(outValue);
                HOPEvalValueSetRuntimeTypeCode(outValue, typeCode);
                *outIsConst = 1;
                return 0;
            case HOPEvalTypeCode_U8:
            case HOPEvalTypeCode_U16:
            case HOPEvalTypeCode_U32:
            case HOPEvalTypeCode_U64:
            case HOPEvalTypeCode_UINT:
            case HOPEvalTypeCode_I8:
            case HOPEvalTypeCode_I16:
            case HOPEvalTypeCode_I32:
            case HOPEvalTypeCode_I64:
            case HOPEvalTypeCode_INT:
                HOPEvalValueSetInt(outValue, 0);
                *outIsConst = 1;
                return 0;
            default: return 0;
        }
    }
    rt = HOPEvalValueAsReflectedType(typeValue);
    if (rt == NULL) {
        return 0;
    }
    if (rt->kind == HOPEvalReflectType_NAMED) {
        const HOPAstNode* declNode = NULL;
        if (rt->file == NULL || rt->nodeId < 0 || (uint32_t)rt->nodeId >= rt->file->ast.len) {
            return 0;
        }
        declNode = &rt->file->ast.nodes[rt->nodeId];
        if (rt->namedKind == HOPEvalTypeKind_ALIAS) {
            int32_t      baseTypeNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            HOPCTFEValue baseTypeValue;
            if (baseTypeNode < 0
                || !HOPEvalTypeValueFromTypeNode(
                    (HOPEvalProgram*)p, rt->file, baseTypeNode, &baseTypeValue))
            {
                return 0;
            }
            if (HOPEvalZeroInitTypeValue(p, &baseTypeValue, outValue, outIsConst) != 0) {
                return -1;
            }
            if (*outIsConst) {
                outValue->typeTag = HOPEvalMakeAliasTag(rt->file, rt->nodeId);
            }
            return 0;
        }
        if (rt->namedKind == HOPEvalTypeKind_STRUCT || rt->namedKind == HOPEvalTypeKind_UNION) {
            return HOPEvalZeroInitAggregateValue(p, rt->file, rt->nodeId, outValue, outIsConst);
        }
        if (rt->namedKind == HOPEvalTypeKind_ENUM && declNode != NULL) {
            int32_t  variantNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            uint32_t tagIndex = 0;
            while (variantNode >= 0) {
                if (rt->file->ast.nodes[variantNode].kind == HOPAst_FIELD) {
                    HOPEvalValueSetTaggedEnum(
                        (HOPEvalProgram*)p,
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
                if (rt->file->ast.nodes[variantNode].kind == HOPAst_FIELD) {
                    tagIndex++;
                }
                variantNode = ASTNextSibling(&rt->file->ast, variantNode);
            }
        }
        return 0;
    }
    if (rt->kind == HOPEvalReflectType_PTR) {
        HOPEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (rt->kind == HOPEvalReflectType_ARRAY) {
        uint32_t      i;
        HOPEvalArray* array = (HOPEvalArray*)HOPArenaAlloc(
            p->arena, sizeof(HOPEvalArray), (uint32_t)_Alignof(HOPEvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = rt->file;
        array->typeNode = rt->nodeId;
        array->len = rt->arrayLen;
        if (rt->arrayLen > 0) {
            array->elems = (HOPCTFEValue*)HOPArenaAlloc(
                p->arena, sizeof(HOPCTFEValue) * rt->arrayLen, (uint32_t)_Alignof(HOPCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(HOPCTFEValue) * rt->arrayLen);
            for (i = 0; i < rt->arrayLen; i++) {
                int elemIsConst = 0;
                if (HOPEvalZeroInitTypeValue(p, &rt->elemType, &array->elems[i], &elemIsConst) != 0)
                {
                    return -1;
                }
                if (!elemIsConst) {
                    return 0;
                }
            }
        }
        HOPEvalValueSetArray(outValue, rt->file, rt->nodeId, array);
        *outIsConst = 1;
        return 0;
    }
    return 0;
}

static int HOPEvalTypeKindOfValue(const HOPCTFEValue* typeValue, int32_t* outKind) {
    int32_t               typeCode = HOPEvalTypeCode_INVALID;
    HOPEvalReflectedType* rt;
    if (outKind != NULL) {
        *outKind = 0;
    }
    if (typeValue == NULL || outKind == NULL || typeValue->kind != HOPCTFEValue_TYPE) {
        return 0;
    }
    if (HOPEvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        *outKind = HOPEvalTypeKind_PRIMITIVE;
        return 1;
    }
    rt = HOPEvalValueAsReflectedType(typeValue);
    if (rt == NULL) {
        return 0;
    }
    *outKind = (int32_t)rt->namedKind;
    return 1;
}

static int HOPEvalTypeNameOfValue(HOPCTFEValue* typeValue, HOPCTFEValue* outValue) {
    int32_t               typeCode = HOPEvalTypeCode_INVALID;
    HOPEvalReflectedType* rt;
    if (typeValue == NULL || outValue == NULL || typeValue->kind != HOPCTFEValue_TYPE) {
        return 0;
    }
    if (HOPEvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        switch (typeCode) {
            case HOPEvalTypeCode_BOOL: HOPEvalValueSetStringSlice(outValue, "bool", 0, 4); return 1;
            case HOPEvalTypeCode_U8:   HOPEvalValueSetStringSlice(outValue, "u8", 0, 2); return 1;
            case HOPEvalTypeCode_U16:  HOPEvalValueSetStringSlice(outValue, "u16", 0, 3); return 1;
            case HOPEvalTypeCode_U32:  HOPEvalValueSetStringSlice(outValue, "u32", 0, 3); return 1;
            case HOPEvalTypeCode_U64:  HOPEvalValueSetStringSlice(outValue, "u64", 0, 3); return 1;
            case HOPEvalTypeCode_UINT: HOPEvalValueSetStringSlice(outValue, "uint", 0, 4); return 1;
            case HOPEvalTypeCode_I8:   HOPEvalValueSetStringSlice(outValue, "i8", 0, 2); return 1;
            case HOPEvalTypeCode_I16:  HOPEvalValueSetStringSlice(outValue, "i16", 0, 3); return 1;
            case HOPEvalTypeCode_I32:  HOPEvalValueSetStringSlice(outValue, "i32", 0, 3); return 1;
            case HOPEvalTypeCode_I64:  HOPEvalValueSetStringSlice(outValue, "i64", 0, 3); return 1;
            case HOPEvalTypeCode_INT:  HOPEvalValueSetStringSlice(outValue, "int", 0, 3); return 1;
            case HOPEvalTypeCode_F32:  HOPEvalValueSetStringSlice(outValue, "f32", 0, 3); return 1;
            case HOPEvalTypeCode_F64:  HOPEvalValueSetStringSlice(outValue, "f64", 0, 3); return 1;
            case HOPEvalTypeCode_RAWPTR:
                HOPEvalValueSetStringSlice(outValue, "rawptr", 0, 6);
                return 1;
            case HOPEvalTypeCode_TYPE: HOPEvalValueSetStringSlice(outValue, "type", 0, 4); return 1;
            case HOPEvalTypeCode_ANYTYPE:
                HOPEvalValueSetStringSlice(outValue, "anytype", 0, 7);
                return 1;
            default: return 0;
        }
    }
    rt = HOPEvalValueAsReflectedType(typeValue);
    if (rt == NULL || rt->kind != HOPEvalReflectType_NAMED || rt->file == NULL || rt->nodeId < 0
        || (uint32_t)rt->nodeId >= rt->file->ast.len)
    {
        return 0;
    }
    HOPEvalValueSetStringSlice(
        outValue,
        rt->file->source,
        rt->file->ast.nodes[rt->nodeId].dataStart,
        rt->file->ast.nodes[rt->nodeId].dataEnd);
    return 1;
}

static int HOPEvalZeroInitTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    HOPCTFEValue*         outValue,
    int*                  outIsConst) {
    const HOPAstNode*    typeNameNode;
    const HOPPackage*    currentPkg;
    const HOPParsedFile* aggregateFile = NULL;
    const HOPParsedFile* aliasFile = NULL;
    int32_t              aggregateNode = -1;
    int32_t              aliasNode = -1;
    int32_t              aliasTargetNode = -1;
    uint64_t             nullTag = 0;
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
        case HOPAst_TYPE_NAME: {
            uint32_t             dot = typeNameNode->dataStart;
            const HOPParsedFile* enumFile = NULL;
            int32_t              enumNode = -1;
            int32_t              variantNode = -1;
            uint32_t             tagIndex = 0;
            while (dot < typeNameNode->dataEnd && file->source[dot] != '.') {
                dot++;
            }
            if (HOPEvalTypeNodeIsTemplateParamName(file, typeNode)) {
                HOPEvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (dot < typeNameNode->dataEnd) {
                enumNode = HOPEvalFindNamedEnumDecl(
                    p, file, typeNameNode->dataStart, dot, &enumFile);
                if (enumNode >= 0 && enumFile != NULL
                    && HOPEvalFindEnumVariant(
                        enumFile,
                        enumNode,
                        file->source,
                        dot + 1u,
                        typeNameNode->dataEnd,
                        &variantNode,
                        &tagIndex))
                {
                    const HOPAstNode* variantField = &enumFile->ast.nodes[variantNode];
                    HOPCTFEValue      payloadValue;
                    HOPEvalAggregate* payload = NULL;
                    int               payloadIsConst = 0;
                    if (HOPEvalBuildTaggedEnumPayload(
                            (HOPEvalProgram*)p,
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
                    payload = HOPEvalValueAsAggregate(&payloadValue);
                    HOPEvalValueSetTaggedEnum(
                        (HOPEvalProgram*)p,
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
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_BOOL);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f32")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f64"))
            {
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                HOPEvalValueSetRuntimeTypeCode(
                    outValue,
                    SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f32")
                        ? HOPEvalTypeCode_F32
                        : HOPEvalTypeCode_F64);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "string")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "str"))
            {
                outValue->kind = HOPCTFEValue_STRING;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_STR_REF);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "rawptr"))
            {
                HOPEvalValueSetNull(outValue);
                HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_RAWPTR);
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
                int32_t typeCode = HOPEvalTypeCode_INVALID;
                HOPEvalValueSetInt(outValue, 0);
                if (HOPEvalBuiltinTypeCode(
                        file->source, typeNameNode->dataStart, typeNameNode->dataEnd, &typeCode))
                {
                    HOPEvalValueSetRuntimeTypeCode(outValue, typeCode);
                }
                *outIsConst = 1;
                return 0;
            }
            if (HOPEvalResolveAliasCastTargetNode(
                    p, file, typeNode, &aliasFile, &aliasNode, &aliasTargetNode)
                && aliasFile != NULL && aliasNode >= 0 && aliasTargetNode >= 0)
            {
                if (HOPEvalZeroInitTypeNode(p, aliasFile, aliasTargetNode, outValue, outIsConst)
                    != 0)
                {
                    return -1;
                }
                if (*outIsConst) {
                    outValue->typeTag = HOPEvalMakeAliasTag(aliasFile, aliasNode);
                }
                return 0;
            }
            currentPkg = HOPEvalFindPackageByFile(p, file);
            if (currentPkg == NULL) {
                return 0;
            }
            aggregateNode = HOPEvalFindNamedAggregateDecl(p, file, typeNode, &aggregateFile);
            if (aggregateNode >= 0 && aggregateFile != NULL) {
                return HOPEvalZeroInitAggregateValue(
                    p, aggregateFile, aggregateNode, outValue, outIsConst);
            }
            {
                HOPCTFEValue typeValue;
                int32_t      topConstIndex = HOPEvalFindTopConstBySlice(
                    p, file, typeNameNode->dataStart, typeNameNode->dataEnd);
                if (topConstIndex >= 0) {
                    int typeIsConst = 0;
                    if (HOPEvalEvalTopConst(
                            (HOPEvalProgram*)p, (uint32_t)topConstIndex, &typeValue, &typeIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (typeIsConst && typeValue.kind == HOPCTFEValue_TYPE) {
                        return HOPEvalZeroInitTypeValue(p, &typeValue, outValue, outIsConst);
                    }
                }
                if (HOPEvalTypeValueFromTypeNode((HOPEvalProgram*)p, file, typeNode, &typeValue)) {
                    return HOPEvalZeroInitTypeValue(p, &typeValue, outValue, outIsConst);
                }
            }
            return 0;
        }
        case HOPAst_TYPE_REF: {
            int32_t childNode = ASTFirstChild(&file->ast, typeNode);
            if (childNode >= 0 && (uint32_t)childNode < file->ast.len) {
                const HOPAstNode* child = &file->ast.nodes[childNode];
                if (child->kind == HOPAst_TYPE_NAME
                    && SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str"))
                {
                    outValue->kind = HOPCTFEValue_STRING;
                    outValue->i64 = 0;
                    outValue->f64 = 0.0;
                    outValue->b = 0;
                    outValue->typeTag = 0;
                    outValue->s.bytes = NULL;
                    outValue->s.len = 0;
                    HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_STR_REF);
                    *outIsConst = 1;
                    return 0;
                }
                if (child->kind == HOPAst_TYPE_SLICE) {
                    int32_t       elemTypeNode = ASTFirstChild(&file->ast, childNode);
                    HOPEvalArray* array = HOPEvalAllocArrayView(
                        p, file, typeNode, elemTypeNode, NULL, 0);
                    if (array == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    HOPEvalValueSetArray(outValue, file, typeNode, array);
                    *outIsConst = 1;
                    return 0;
                }
            }
            HOPEvalValueSetNull(outValue);
            if (HOPEvalResolveNullCastTypeTag(file, typeNode, &nullTag)) {
                outValue->typeTag = nullTag;
            }
            *outIsConst = 1;
            return 0;
        }
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_MUTREF:
            HOPEvalValueSetNull(outValue);
            if (HOPEvalResolveNullCastTypeTag(file, typeNode, &nullTag)) {
                outValue->typeTag = nullTag;
            }
            *outIsConst = 1;
            return 0;
        case HOPAst_TYPE_OPTIONAL:
            HOPEvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        case HOPAst_TYPE_ARRAY: {
            const HOPAstNode* arrayTypeNode = &file->ast.nodes[typeNode];
            int32_t           elemTypeNode = ASTFirstChild(&file->ast, typeNode);
            uint32_t          len = 0;
            uint32_t          i;
            HOPEvalArray*     array;
            if (elemTypeNode < 0
                || !HOPEvalParseUintSlice(
                    file->source, arrayTypeNode->dataStart, arrayTypeNode->dataEnd, &len))
            {
                return 0;
            }
            array = (HOPEvalArray*)HOPArenaAlloc(
                p->arena, sizeof(HOPEvalArray), (uint32_t)_Alignof(HOPEvalArray));
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array, 0, sizeof(*array));
            array->file = file;
            array->typeNode = typeNode;
            array->elemTypeNode = elemTypeNode;
            array->len = len;
            if (len > 0) {
                array->elems = (HOPCTFEValue*)HOPArenaAlloc(
                    p->arena, sizeof(HOPCTFEValue) * len, (uint32_t)_Alignof(HOPCTFEValue));
                if (array->elems == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(array->elems, 0, sizeof(HOPCTFEValue) * len);
                for (i = 0; i < len; i++) {
                    int elemIsConst = 0;
                    if (HOPEvalZeroInitTypeNode(
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
            HOPEvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case HOPAst_TYPE_VARRAY: {
            int32_t       elemTypeNode = ASTFirstChild(&file->ast, typeNode);
            HOPEvalArray* array = HOPEvalAllocArrayView(p, file, typeNode, elemTypeNode, NULL, 0);
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            HOPEvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case HOPAst_TYPE_TUPLE: {
            uint32_t      len = AstListCount(&file->ast, typeNode);
            uint32_t      i;
            HOPEvalArray* array = (HOPEvalArray*)HOPArenaAlloc(
                p->arena, sizeof(HOPEvalArray), (uint32_t)_Alignof(HOPEvalArray));
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array, 0, sizeof(*array));
            array->file = file;
            array->typeNode = typeNode;
            array->len = len;
            if (len > 0) {
                array->elems = (HOPCTFEValue*)HOPArenaAlloc(
                    p->arena, sizeof(HOPCTFEValue) * len, (uint32_t)_Alignof(HOPCTFEValue));
                if (array->elems == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(array->elems, 0, sizeof(HOPCTFEValue) * len);
                for (i = 0; i < len; i++) {
                    int32_t elemTypeNode = AstListItemAt(&file->ast, typeNode, i);
                    int     elemIsConst = 0;
                    if (elemTypeNode < 0
                        || HOPEvalZeroInitTypeNode(
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
            HOPEvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case HOPAst_TYPE_ANON_STRUCT:
        case HOPAst_TYPE_ANON_UNION:
            return HOPEvalZeroInitAggregateValue(p, file, typeNode, outValue, outIsConst);
        case HOPAst_TYPE_FN:
            HOPEvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        default: return 0;
    }
}

static int HOPEvalResolveSimpleAliasCastTarget(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    char*                 outTargetKind,
    uint64_t* _Nullable outAliasTag) {
    const HOPPackage* currentPkg;
    const HOPAstNode* typeNameNode;
    uint32_t          fileIndex;
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
    if (typeNameNode->kind != HOPAst_TYPE_NAME) {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    for (fileIndex = 0; fileIndex < currentPkg->fileLen; fileIndex++) {
        const HOPParsedFile* pkgFile = &currentPkg->files[fileIndex];
        int32_t              nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const HOPAstNode* aliasNode = &pkgFile->ast.nodes[nodeId];
            if (aliasNode->kind == HOPAst_TYPE_ALIAS
                && SliceEqSlice(
                    file->source,
                    typeNameNode->dataStart,
                    typeNameNode->dataEnd,
                    pkgFile->source,
                    aliasNode->dataStart,
                    aliasNode->dataEnd))
            {
                int32_t           targetNodeId = aliasNode->firstChild;
                const HOPAstNode* targetNode;
                if (targetNodeId < 0 || (uint32_t)targetNodeId >= pkgFile->ast.len) {
                    return 0;
                }
                targetNode = &pkgFile->ast.nodes[targetNodeId];
                if (targetNode->kind != HOPAst_TYPE_NAME) {
                    return 0;
                }
                if (SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "bool"))
                {
                    *outTargetKind = 'b';
                    if (outAliasTag != NULL) {
                        *outAliasTag = HOPEvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                if (SliceEqCStr(pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "f32")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "f64"))
                {
                    *outTargetKind = 'f';
                    if (outAliasTag != NULL) {
                        *outAliasTag = HOPEvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                if (SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "string"))
                {
                    *outTargetKind = 's';
                    if (outAliasTag != NULL) {
                        *outAliasTag = HOPEvalMakeAliasTag(pkgFile, nodeId);
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
                        *outAliasTag = HOPEvalMakeAliasTag(pkgFile, nodeId);
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

static int HOPEvalResolveAliasCastTargetNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    const HOPParsedFile** outAliasFile,
    int32_t*              outAliasNode,
    int32_t*              outTargetNode) {
    const HOPPackage* currentPkg;
    const HOPAstNode* typeNameNode;
    uint32_t          fileIndex;
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
    if (typeNameNode->kind != HOPAst_TYPE_NAME) {
        return 0;
    }
    currentPkg = HOPEvalFindPackageByFile(p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    for (fileIndex = 0; fileIndex < currentPkg->fileLen; fileIndex++) {
        const HOPParsedFile* pkgFile = &currentPkg->files[fileIndex];
        int32_t              nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const HOPAstNode* aliasNode = &pkgFile->ast.nodes[nodeId];
            if (aliasNode->kind == HOPAst_TYPE_ALIAS
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

static int HOPEvalDecodeNewExprNodes(
    const HOPParsedFile* file,
    int32_t              nodeId,
    int32_t*             outTypeNode,
    int32_t*             outCountNode,
    int32_t*             outInitNode,
    int32_t*             outAllocNode) {
    const HOPAstNode* n;
    int32_t           nextNode;
    int               hasCount;
    int               hasInit;
    int               hasAlloc;
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
    if (n->kind != HOPAst_NEW) {
        return 0;
    }
    hasCount = (n->flags & HOPAstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & HOPAstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & HOPAstFlag_NEW_HAS_ALLOC) != 0;
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

static int HOPEvalAllocReferencedValue(
    HOPEvalProgram* p, const HOPCTFEValue* inValue, HOPCTFEValue* outValue, int* outIsConst) {
    HOPCTFEValue* target;
    if (p == NULL || inValue == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    target = (HOPCTFEValue*)HOPArenaAlloc(
        p->arena, sizeof(HOPCTFEValue), (uint32_t)_Alignof(HOPCTFEValue));
    if (target == NULL) {
        return ErrorSimple("out of memory");
    }
    *target = *inValue;
    HOPEvalValueSetReference(outValue, target);
    *outIsConst = 1;
    return 0;
}

static int HOPEvalCheckAllocatorImplResult(
    HOPEvalProgram* p, int32_t exprNode, const HOPCTFEValue* allocValue, int* outReturnedNull) {
    const HOPCTFEValue* allocTarget;
    HOPEvalAggregate*   allocAgg;
    HOPCTFEValue        handlerValue;
    static const char   handlerName[] = "handler";
    if (outReturnedNull != NULL) {
        *outReturnedNull = 0;
    }
    if (p == NULL || allocValue == NULL) {
        return -1;
    }
    allocTarget = HOPEvalValueTargetOrSelf(allocValue);
    allocAgg = HOPEvalValueAsAggregate(allocTarget);
    if (allocAgg == NULL
        || !HOPEvalAggregateGetFieldValue(allocAgg, handlerName, 0u, 7u, &handlerValue)
        || !HOPEvalValueIsInvokableFunctionRef(&handlerValue))
    {
        return 1;
    }
    {
        HOPCTFEValue args[7];
        HOPCTFEValue newSize;
        HOPCTFEValue result;
        int          resultIsConst = 0;
        int          invoked;
        args[0] = *allocValue;
        HOPEvalValueSetNull(&args[1]);
        HOPEvalValueSetInt(&args[2], 0);
        HOPEvalValueSetInt(&args[3], 0);
        HOPEvalValueSetInt(&newSize, 0);
        HOPEvalValueSetReference(&args[4], &newSize);
        HOPEvalValueSetInt(&args[5], 0);
        HOPEvalValueSetNull(&args[6]);
        invoked = HOPEvalInvokeFunctionRef(p, &handlerValue, args, 7u, &result, &resultIsConst);
        if (invoked < 0) {
            return -1;
        }
        if (invoked > 0 && resultIsConst
            && HOPEvalValueTargetOrSelf(&result)->kind == HOPCTFEValue_NULL)
        {
            if (outReturnedNull != NULL) {
                *outReturnedNull = 1;
            }
            (void)exprNode;
            return 0;
        }
    }
    return 1;
}

static int HOPEvalExpectedNewResultIsOptional(
    const HOPParsedFile* _Nullable typeFile, int32_t typeNode) {
    if (typeFile == NULL || typeNode < 0 || (uint32_t)typeNode >= typeFile->ast.len) {
        return 0;
    }
    return typeFile->ast.nodes[typeNode].kind == HOPAst_TYPE_OPTIONAL;
}

static int HOPEvalEvalNewExpr(
    HOPEvalProgram* p,
    int32_t         exprNode,
    const HOPParsedFile* _Nullable expectedTypeFile,
    int32_t       expectedTypeNode,
    HOPCTFEValue* outValue,
    int*          outIsConst) {
    int32_t      typeNode = -1;
    int32_t      countNode = -1;
    int32_t      initNode = -1;
    int32_t      allocNode = -1;
    HOPCTFEValue allocValue;
    int          allocIsConst = 0;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!HOPEvalDecodeNewExprNodes(
            p->currentFile, exprNode, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    if (allocNode >= 0) {
        if (HOPEvalExecExprCb(p, allocNode, &allocValue, &allocIsConst) != 0) {
            return -1;
        }
        if (!allocIsConst) {
            return 0;
        }
    } else if (!HOPEvalCurrentContextFieldByLiteral(p, "allocator", &allocValue)) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReasonNode(
                p->currentExecCtx,
                exprNode,
                "allocator context is not available in evaluator backend");
        }
        return 0;
    } else {
        allocIsConst = 1;
    }
    if (HOPEvalValueTargetOrSelf(&allocValue)->kind == HOPCTFEValue_NULL) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReasonNode(p->currentExecCtx, exprNode, "invalid allocator");
        }
        return 0;
    }
    {
        int allocReturnedNull = 0;
        int allocRc = HOPEvalCheckAllocatorImplResult(p, exprNode, &allocValue, &allocReturnedNull);
        if (allocRc <= 0) {
            if (allocRc == 0 && allocReturnedNull
                && HOPEvalExpectedNewResultIsOptional(expectedTypeFile, expectedTypeNode))
            {
                HOPEvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (allocRc == 0 && allocReturnedNull && p->currentExecCtx != NULL) {
                HOPCTFEExecSetReasonNode(p->currentExecCtx, exprNode, "allocator returned null");
            }
            return allocRc;
        }
    }
    if (countNode >= 0) {
        HOPCTFEValue  countValue;
        int           countIsConst = 0;
        int64_t       count = 0;
        HOPEvalArray* array;
        HOPCTFEValue  arrayValue;
        uint32_t      i;
        if (HOPEvalExecExprCb(p, countNode, &countValue, &countIsConst) != 0) {
            return -1;
        }
        if (!countIsConst || HOPCTFEValueToInt64(&countValue, &count) != 0 || count < 0) {
            return 0;
        }
        array = (HOPEvalArray*)HOPArenaAlloc(
            p->arena, sizeof(HOPEvalArray), (uint32_t)_Alignof(HOPEvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = p->currentFile;
        array->typeNode = exprNode;
        array->elemTypeNode = typeNode;
        array->len = (uint32_t)count;
        if (array->len > 0) {
            array->elems = (HOPCTFEValue*)HOPArenaAlloc(
                p->arena, sizeof(HOPCTFEValue) * array->len, (uint32_t)_Alignof(HOPCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(HOPCTFEValue) * array->len);
            if (initNode >= 0) {
                HOPCTFEValue initValue;
                int          initIsConst = 0;
                if (HOPEvalExecExprWithTypeNode(
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
                    if (HOPEvalZeroInitTypeNode(
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
        HOPEvalValueSetArray(&arrayValue, p->currentFile, exprNode, array);
        return HOPEvalAllocReferencedValue(p, &arrayValue, outValue, outIsConst);
    }
    {
        HOPCTFEValue value;
        int          valueIsConst = 0;
        if (initNode >= 0) {
            if (HOPEvalExecExprWithTypeNode(
                    p, initNode, p->currentFile, typeNode, &value, &valueIsConst)
                != 0)
            {
                return -1;
            }
        } else if (HOPEvalZeroInitTypeNode(p, p->currentFile, typeNode, &value, &valueIsConst) != 0)
        {
            return -1;
        }
        if (!valueIsConst) {
            return 0;
        }
        {
            HOPEvalAggregate*   agg = HOPEvalValueAsAggregate(&value);
            HOPCTFEExecBinding* fieldBindings = NULL;
            HOPCTFEExecEnv      fieldFrame;
            uint32_t            fieldBindingCap = 0;
            uint32_t            i;
            if (agg != NULL && agg->fieldLen > 0) {
                fieldBindingCap = HOPEvalAggregateFieldBindingCount(agg);
                fieldBindings = (HOPCTFEExecBinding*)HOPArenaAlloc(
                    p->arena,
                    sizeof(HOPCTFEExecBinding) * fieldBindingCap,
                    (uint32_t)_Alignof(HOPCTFEExecBinding));
                if (fieldBindings == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(fieldBindings, 0, sizeof(HOPCTFEExecBinding) * fieldBindingCap);
            }
            fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
            fieldFrame.bindings = fieldBindings;
            fieldFrame.bindingLen = 0;
            if (agg != NULL) {
                for (i = 0; i < agg->fieldLen; i++) {
                    HOPEvalAggregateField* field = &agg->fields[i];
                    if (initNode < 0 && field->defaultExprNode >= 0) {
                        HOPCTFEValue defaultValue;
                        int          defaultIsConst = 0;
                        if (HOPEvalExecExprInFileWithType(
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
                    if (fieldBindings != NULL
                        && HOPEvalAppendAggregateFieldBindings(
                               fieldBindings, fieldBindingCap, &fieldFrame, field)
                               != 0)
                    {
                        return ErrorSimple("out of memory");
                    }
                }
            }
        }
        return HOPEvalAllocReferencedValue(p, &value, outValue, outIsConst);
    }
}

static int HOPEvalValueConcatStrings(
    HOPArena* arena, const HOPCTFEValue* a, const HOPCTFEValue* b, HOPCTFEValue* outValue) {
    uint64_t totalLen64;
    uint32_t totalLen;
    uint8_t* bytes;
    if (arena == NULL || a == NULL || b == NULL || outValue == NULL) {
        return -1;
    }
    if (a->kind != HOPCTFEValue_STRING || b->kind != HOPCTFEValue_STRING) {
        return 0;
    }
    totalLen64 = (uint64_t)a->s.len + (uint64_t)b->s.len;
    if (totalLen64 > UINT32_MAX) {
        return 0;
    }
    totalLen = (uint32_t)totalLen64;
    outValue->kind = HOPCTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    if (totalLen == 0) {
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        return 1;
    }
    bytes = (uint8_t*)HOPArenaAlloc(arena, totalLen, (uint32_t)_Alignof(uint8_t));
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

static void HOPEvalValueSetUInt(HOPCTFEValue* value, uint32_t n) {
    HOPEvalValueSetInt(value, (int64_t)n);
    HOPEvalValueSetRuntimeTypeCode(value, HOPEvalTypeCode_UINT);
}

static int HOPEvalValueCopyBuiltin(
    HOPArena*           arena,
    const HOPCTFEValue* dstArg,
    const HOPCTFEValue* srcArg,
    HOPCTFEValue*       outValue) {
    const HOPCTFEValue* srcValue;
    HOPCTFEValue*       dstValue;
    HOPEvalArray*       dstArray;
    HOPEvalArray*       srcArray;
    uint32_t            copyLen;
    if (arena == NULL || dstArg == NULL || srcArg == NULL || outValue == NULL) {
        return -1;
    }
    srcValue = HOPEvalValueTargetOrSelf(srcArg);
    dstValue = HOPEvalValueReferenceTarget(dstArg);
    if (dstValue == NULL) {
        dstValue = (HOPCTFEValue*)HOPEvalValueTargetOrSelf(dstArg);
    }
    if (dstValue == NULL || srcValue == NULL) {
        return 0;
    }
    dstArray = HOPEvalValueAsArray(dstValue);
    srcArray = HOPEvalValueAsArray(srcValue);
    if (dstArray != NULL && srcArray != NULL) {
        copyLen = dstArray->len < srcArray->len ? dstArray->len : srcArray->len;
        if (copyLen > 0) {
            HOPCTFEValue* temp = (HOPCTFEValue*)HOPArenaAlloc(
                arena, sizeof(HOPCTFEValue) * copyLen, (uint32_t)_Alignof(HOPCTFEValue));
            if (temp == NULL) {
                return -1;
            }
            memcpy(temp, srcArray->elems, sizeof(HOPCTFEValue) * copyLen);
            memcpy(dstArray->elems, temp, sizeof(HOPCTFEValue) * copyLen);
        }
        HOPEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstValue->kind == HOPCTFEValue_STRING && srcValue->kind == HOPCTFEValue_STRING) {
        int32_t dstTypeCode = HOPEvalTypeCode_INVALID;
        if (!HOPEvalValueGetRuntimeTypeCode(dstValue, &dstTypeCode)
            || dstTypeCode != HOPEvalTypeCode_STR_PTR)
        {
            return 0;
        }
        copyLen = dstValue->s.len < srcValue->s.len ? dstValue->s.len : srcValue->s.len;
        if (copyLen > 0 && dstValue->s.bytes != NULL && srcValue->s.bytes != NULL) {
            memmove((uint8_t*)dstValue->s.bytes, srcValue->s.bytes, copyLen);
        }
        HOPEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstArray != NULL && srcValue->kind == HOPCTFEValue_STRING) {
        copyLen = dstArray->len < srcValue->s.len ? dstArray->len : srcValue->s.len;
        for (uint32_t i = 0; i < copyLen; i++) {
            HOPEvalValueSetInt(&dstArray->elems[i], (int64_t)srcValue->s.bytes[i]);
            HOPEvalValueSetRuntimeTypeCode(&dstArray->elems[i], HOPEvalTypeCode_U8);
        }
        HOPEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstValue->kind == HOPCTFEValue_STRING && srcArray != NULL) {
        int32_t  dstTypeCode = HOPEvalTypeCode_INVALID;
        uint8_t* tempBytes;
        if (!HOPEvalValueGetRuntimeTypeCode(dstValue, &dstTypeCode)
            || dstTypeCode != HOPEvalTypeCode_STR_PTR)
        {
            return 0;
        }
        copyLen = dstValue->s.len < srcArray->len ? dstValue->s.len : srcArray->len;
        tempBytes =
            copyLen > 0 ? (uint8_t*)HOPArenaAlloc(arena, copyLen, (uint32_t)_Alignof(uint8_t))
                        : NULL;
        if (copyLen > 0 && tempBytes == NULL) {
            return -1;
        }
        for (uint32_t i = 0; i < copyLen; i++) {
            int64_t byteValue = 0;
            if (HOPCTFEValueToInt64(&srcArray->elems[i], &byteValue) != 0 || byteValue < 0
                || byteValue > 255)
            {
                return 0;
            }
            tempBytes[i] = (uint8_t)byteValue;
        }
        if (copyLen > 0 && dstValue->s.bytes != NULL) {
            memmove((uint8_t*)dstValue->s.bytes, tempBytes, copyLen);
        }
        HOPEvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    return 0;
}

static int HOPEvalStringValueFromArrayBytes(
    HOPArena* arena, const HOPCTFEValue* inValue, int32_t targetTypeCode, HOPCTFEValue* outValue) {
    HOPEvalArray* array;
    uint8_t*      bytes = NULL;
    uint32_t      i;
    if (arena == NULL || inValue == NULL || outValue == NULL) {
        return -1;
    }
    array = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(inValue));
    if (array == NULL) {
        return 0;
    }
    if (array->len > 0) {
        bytes = (uint8_t*)HOPArenaAlloc(arena, array->len, (uint32_t)_Alignof(uint8_t));
        if (bytes == NULL) {
            return -1;
        }
        for (i = 0; i < array->len; i++) {
            int64_t byteValue = 0;
            if (HOPCTFEValueToInt64(&array->elems[i], &byteValue) != 0 || byteValue < 0
                || byteValue > 255)
            {
                return 0;
            }
            bytes[i] = (uint8_t)byteValue;
        }
    }
    outValue->kind = HOPCTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = bytes;
    outValue->s.len = array->len;
    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
    return 1;
}

static void HOPEvalValueSetSpan(
    const HOPParsedFile* file, uint32_t start, uint32_t end, HOPCTFEValue* value) {
    uint32_t startLine = 0;
    uint32_t startCol = 0;
    uint32_t endLine = 0;
    uint32_t endCol = 0;
    if (file == NULL || value == NULL) {
        return;
    }
    DiagOffsetToLineCol(file->source, start, &startLine, &startCol);
    DiagOffsetToLineCol(file->source, end, &endLine, &endCol);
    value->kind = HOPCTFEValue_SPAN;
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
    return ErrorDiagf(file, source, start, end, HOPDiag_EVAL_BACKEND_UNSUPPORTED, detail);
}

static int HOPEvalProgramAppendFunction(HOPEvalProgram* p, const HOPEvalFunction* fn) {
    HOPEvalFunction* newFuncs;
    uint32_t         newCap;
    if (p == NULL || fn == NULL) {
        return -1;
    }
    if (p->funcLen >= p->funcCap) {
        newCap = p->funcCap < 8u ? 8u : p->funcCap * 2u;
        newFuncs = (HOPEvalFunction*)realloc(p->funcs, sizeof(HOPEvalFunction) * newCap);
        if (newFuncs == NULL) {
            return ErrorSimple("out of memory");
        }
        p->funcs = newFuncs;
        p->funcCap = newCap;
    }
    p->funcs[p->funcLen++] = *fn;
    return 0;
}

static int HOPEvalProgramAppendTopConst(HOPEvalProgram* p, const HOPEvalTopConst* topConst) {
    HOPEvalTopConst* newConsts;
    uint32_t         newCap;
    if (p == NULL || topConst == NULL) {
        return -1;
    }
    if (p->topConstLen >= p->topConstCap) {
        newCap = p->topConstCap < 8u ? 8u : p->topConstCap * 2u;
        newConsts = (HOPEvalTopConst*)realloc(p->topConsts, sizeof(HOPEvalTopConst) * newCap);
        if (newConsts == NULL) {
            return ErrorSimple("out of memory");
        }
        p->topConsts = newConsts;
        p->topConstCap = newCap;
    }
    p->topConsts[p->topConstLen++] = *topConst;
    return 0;
}

static int HOPEvalProgramAppendTopVar(HOPEvalProgram* p, const HOPEvalTopVar* topVar) {
    HOPEvalTopVar* newVars;
    uint32_t       newCap;
    if (p == NULL || topVar == NULL) {
        return -1;
    }
    if (p->topVarLen >= p->topVarCap) {
        newCap = p->topVarCap < 8u ? 8u : p->topVarCap * 2u;
        newVars = (HOPEvalTopVar*)realloc(p->topVars, sizeof(HOPEvalTopVar) * newCap);
        if (newVars == NULL) {
            return ErrorSimple("out of memory");
        }
        p->topVars = newVars;
        p->topVarCap = newCap;
    }
    p->topVars[p->topVarLen++] = *topVar;
    return 0;
}

static int32_t HOPEvalVarLikeInitExprNodeAt(
    const HOPParsedFile* file, int32_t varLikeNodeId, int32_t nameIndex) {
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
    if (file->ast.nodes[firstChild].kind != HOPAst_NAME_LIST) {
        return nameIndex == 0 ? initNode : -1;
    }
    if (file->ast.nodes[initNode].kind != HOPAst_EXPR_LIST) {
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
                || file->ast.nodes[onlyInit].kind != HOPAst_TUPLE_EXPR)
            {
                return -1;
            }
            return AstListItemAt(&file->ast, onlyInit, (uint32_t)nameIndex);
        }
    }
}

static int32_t HOPEvalVarLikeDeclTypeNode(const HOPParsedFile* file, int32_t varLikeNodeId) {
    int32_t firstChild;
    int32_t afterNames;
    if (file == NULL || varLikeNodeId < 0 || (uint32_t)varLikeNodeId >= file->ast.len) {
        return -1;
    }
    firstChild = ASTFirstChild(&file->ast, varLikeNodeId);
    if (firstChild < 0 || (uint32_t)firstChild >= file->ast.len) {
        return -1;
    }
    if (file->ast.nodes[firstChild].kind == HOPAst_NAME_LIST) {
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

static int HOPEvalCollectTopConsts(HOPEvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &p->loader->packages[pkgIndex];
        uint32_t          fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const HOPParsedFile* file = &pkg->files[fileIndex];
            const HOPAst*        ast = &file->ast;
            int32_t              nodeId = ASTFirstChild(ast, ast->root);
            while (nodeId >= 0) {
                const HOPAstNode* n = &ast->nodes[nodeId];
                if (n->kind == HOPAst_CONST) {
                    int32_t firstChild = ASTFirstChild(ast, nodeId);
                    if (firstChild >= 0 && ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(ast, firstChild);
                        for (i = 0; i < nameCount; i++) {
                            int32_t           nameNode = AstListItemAt(ast, firstChild, i);
                            const HOPAstNode* name = nameNode >= 0 ? &ast->nodes[nameNode] : NULL;
                            HOPEvalTopConst   topConst;
                            if (name == NULL) {
                                continue;
                            }
                            memset(&topConst, 0, sizeof(topConst));
                            topConst.file = file;
                            topConst.nodeId = nodeId;
                            topConst.initExprNode = HOPEvalVarLikeInitExprNodeAt(
                                file, nodeId, (int32_t)i);
                            topConst.nameStart = name->dataStart;
                            topConst.nameEnd = name->dataEnd;
                            topConst.state = HOPEvalTopConstState_UNSEEN;
                            if (HOPEvalProgramAppendTopConst(p, &topConst) != 0) {
                                return -1;
                            }
                        }
                    } else {
                        HOPEvalTopConst topConst;
                        memset(&topConst, 0, sizeof(topConst));
                        topConst.file = file;
                        topConst.nodeId = nodeId;
                        topConst.initExprNode = HOPEvalVarLikeInitExprNodeAt(file, nodeId, 0);
                        topConst.nameStart = n->dataStart;
                        topConst.nameEnd = n->dataEnd;
                        topConst.state = HOPEvalTopConstState_UNSEEN;
                        if (HOPEvalProgramAppendTopConst(p, &topConst) != 0) {
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

static int HOPEvalCollectTopVars(HOPEvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &p->loader->packages[pkgIndex];
        uint32_t          fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const HOPParsedFile* file = &pkg->files[fileIndex];
            const HOPAst*        ast = &file->ast;
            int32_t              nodeId = ASTFirstChild(ast, ast->root);
            while (nodeId >= 0) {
                const HOPAstNode* n = &ast->nodes[nodeId];
                if (n->kind == HOPAst_VAR) {
                    if (ASTFirstChild(ast, nodeId) >= 0
                        && ast->nodes[ASTFirstChild(ast, nodeId)].kind == HOPAst_NAME_LIST)
                    {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(ast, ASTFirstChild(ast, nodeId));
                        for (i = 0; i < nameCount; i++) {
                            int32_t nameNode = AstListItemAt(ast, ASTFirstChild(ast, nodeId), i);
                            const HOPAstNode* name = nameNode >= 0 ? &ast->nodes[nameNode] : NULL;
                            HOPEvalTopVar     topVar;
                            if (name == NULL) {
                                continue;
                            }
                            memset(&topVar, 0, sizeof(topVar));
                            topVar.file = file;
                            topVar.nodeId = nodeId;
                            topVar.initExprNode = HOPEvalVarLikeInitExprNodeAt(
                                file, nodeId, (int32_t)i);
                            topVar.declTypeNode = HOPEvalVarLikeDeclTypeNode(file, nodeId);
                            topVar.nameStart = name->dataStart;
                            topVar.nameEnd = name->dataEnd;
                            topVar.state = HOPEvalTopConstState_UNSEEN;
                            if (HOPEvalProgramAppendTopVar(p, &topVar) != 0) {
                                return -1;
                            }
                        }
                    } else {
                        HOPEvalTopVar topVar;
                        memset(&topVar, 0, sizeof(topVar));
                        topVar.file = file;
                        topVar.nodeId = nodeId;
                        topVar.initExprNode = HOPEvalVarLikeInitExprNodeAt(file, nodeId, 0);
                        topVar.declTypeNode = HOPEvalVarLikeDeclTypeNode(file, nodeId);
                        topVar.nameStart = n->dataStart;
                        topVar.nameEnd = n->dataEnd;
                        topVar.state = HOPEvalTopConstState_UNSEEN;
                        if (HOPEvalProgramAppendTopVar(p, &topVar) != 0) {
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

static int32_t HOPEvalFindTopConstBySlice(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    uint32_t i;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->topConstLen; i++) {
        const HOPEvalTopConst* topConst = &p->topConsts[i];
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

static int32_t HOPEvalFindTopConstBySliceInPackage(
    const HOPEvalProgram* p,
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->topConstLen; i++) {
        const HOPEvalTopConst* topConst = &p->topConsts[i];
        const HOPPackage*      topConstPkg = HOPEvalFindPackageByFile(p, topConst->file);
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

static int32_t HOPEvalFindTopVarBySliceInPackage(
    const HOPEvalProgram* p,
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->topVarLen; i++) {
        const HOPEvalTopVar* topVar = &p->topVars[i];
        const HOPPackage*    topVarPkg = HOPEvalFindPackageByFile(p, topVar->file);
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

static int32_t HOPEvalFindTopVarBySlice(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    uint32_t i;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->topVarLen; i++) {
        const HOPEvalTopVar* topVar = &p->topVars[i];
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

static int32_t HOPEvalFindCurrentTopVarBySlice(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    const HOPPackage* currentPkg;
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = HOPEvalFindPackageByFile(p, callerFile);
    return currentPkg != NULL
             ? HOPEvalFindTopVarBySliceInPackage(p, currentPkg, callerFile, nameStart, nameEnd)
             : HOPEvalFindTopVarBySlice(p, callerFile, nameStart, nameEnd);
}

static int HOPEvalCollectFunctionsFromPackage(
    HOPEvalProgram* p, const HOPPackage* pkg, uint8_t isBuiltinPackageFn) {
    uint32_t fileIndex;
    if (p == NULL || pkg == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const HOPParsedFile* file = &pkg->files[fileIndex];
        const HOPAst*        ast = &file->ast;
        int32_t              nodeId = ASTFirstChild(ast, ast->root);
        while (nodeId >= 0) {
            const HOPAstNode* n = &ast->nodes[nodeId];
            if (n->kind == HOPAst_FN) {
                HOPEvalFunction fn;
                int32_t         child = ASTFirstChild(ast, nodeId);
                int32_t         bodyNode = -1;
                uint32_t        paramCount = 0;
                uint8_t         hasReturnType = 0;
                uint8_t         hasContextClause = 0;
                uint8_t         isVariadic = 0;

                while (child >= 0) {
                    const HOPAstNode* ch = &ast->nodes[child];
                    if (ch->kind == HOPAst_PARAM) {
                        paramCount++;
                        if ((ch->flags & HOPAstFlag_PARAM_VARIADIC) != 0) {
                            isVariadic = 1;
                        }
                    } else if (ch->kind == HOPAst_CONTEXT_CLAUSE) {
                        hasContextClause = 1;
                    } else if (IsFnReturnTypeNodeKind(ch->kind) && ch->flags == 1) {
                        hasReturnType = 1;
                    } else if (ch->kind == HOPAst_BLOCK) {
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
                    if (HOPEvalProgramAppendFunction(p, &fn) != 0) {
                        return -1;
                    }
                }
            }
            nodeId = ASTNextSibling(ast, nodeId);
        }
    }
    return 0;
}

static int HOPEvalCollectFunctions(HOPEvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const HOPPackage* pkg = &p->loader->packages[pkgIndex];
        uint8_t           isBuiltinPackageFn = StrEq(pkg->name, "builtin") ? 1u : 0u;
        if (HOPEvalCollectFunctionsFromPackage(p, pkg, isBuiltinPackageFn) != 0) {
            return -1;
        }
    }
    return 0;
}

static int32_t HOPEvalFindFunctionBySlice(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    uint32_t              argCount) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
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

static int32_t HOPEvalFindFunctionBySliceInPackage(
    const HOPEvalProgram* p,
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    uint32_t              argCount) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
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

static int32_t HOPEvalFindAnyFunctionBySliceInPackage(
    const HOPEvalProgram* p,
    const HOPPackage*     pkg,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || pkg == NULL || callerFile == NULL || nameEnd < nameStart
        || nameEnd > callerFile->sourceLen)
    {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
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

static int HOPEvalValueSimpleKind(
    const HOPCTFEValue* value, char* outKind, uint64_t* _Nullable outAliasTag) {
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
        case HOPCTFEValue_INT:    *outKind = 'i'; break;
        case HOPCTFEValue_FLOAT:  *outKind = 'f'; break;
        case HOPCTFEValue_BOOL:   *outKind = 'b'; break;
        case HOPCTFEValue_STRING: *outKind = 's'; break;
        default:                  return 0;
    }
    if (outAliasTag != NULL) {
        *outAliasTag = value->typeTag;
    }
    return 1;
}

static int HOPEvalClassifySimpleTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    char*                 outKind,
    uint64_t* _Nullable outAliasTag);
static int HOPEvalAggregateDistanceToType(
    const HOPEvalProgram* p,
    const HOPCTFEValue*   value,
    const HOPParsedFile*  callerFile,
    int32_t               typeNode,
    uint32_t*             outDistance);

static int HOPEvalTypeNodeIsAnytype(const HOPParsedFile* file, int32_t typeNode) {
    const HOPAstNode* n;
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    return n->kind == HOPAst_TYPE_NAME
        && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "anytype");
}

static int HOPEvalTypeNodeIsTemplateParamName(const HOPParsedFile* file, int32_t typeNode) {
    const HOPAstNode* n;
    int32_t           declNode;
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind != HOPAst_TYPE_NAME || n->firstChild >= 0) {
        return 0;
    }
    declNode = ASTFirstChild(&file->ast, file->ast.root);
    while (declNode >= 0) {
        const HOPAstNode* decl = &file->ast.nodes[declNode];
        if ((decl->kind == HOPAst_FN || decl->kind == HOPAst_STRUCT) && decl->start <= n->start
            && decl->end >= n->end)
        {
            int32_t child = ASTFirstChild(&file->ast, declNode);
            while (child >= 0 && file->ast.nodes[child].kind == HOPAst_TYPE_PARAM) {
                if (SliceEqSlice(
                        file->source,
                        n->dataStart,
                        n->dataEnd,
                        file->source,
                        file->ast.nodes[child].dataStart,
                        file->ast.nodes[child].dataEnd))
                {
                    return 1;
                }
                child = ASTNextSibling(&file->ast, child);
            }
        }
        declNode = ASTNextSibling(&file->ast, declNode);
    }
    return 0;
}

static int HOPEvalValueMatchesExpectedTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  typeFile,
    int32_t               typeNode,
    const HOPCTFEValue*   value) {
    char                argKind = '\0';
    char                paramKind = '\0';
    uint64_t            argAliasTag = 0;
    uint64_t            paramAliasTag = 0;
    uint32_t            structDistance = 0;
    int32_t             typeCode = HOPEvalTypeCode_INVALID;
    const HOPCTFEValue* sourceValue = HOPEvalValueTargetOrSelf(value);
    if (p == NULL || typeFile == NULL || value == NULL || typeNode < 0
        || (uint32_t)typeNode >= typeFile->ast.len)
    {
        return 0;
    }
    if (HOPEvalTypeNodeIsAnytype(typeFile, typeNode)) {
        return 1;
    }
    if (HOPEvalTypeNodeIsTemplateParamName(typeFile, typeNode)) {
        return 1;
    }
    if (HOPEvalAggregateDistanceToType(p, value, typeFile, typeNode, &structDistance)) {
        return 1;
    }
    if (HOPEvalValueSimpleKind(sourceValue, &argKind, &argAliasTag)
        && HOPEvalClassifySimpleTypeNode(p, typeFile, typeNode, &paramKind, &paramAliasTag)
        && argKind == paramKind)
    {
        return paramAliasTag == 0 || argAliasTag == paramAliasTag;
    }
    if (!HOPEvalTypeCodeFromTypeNode(typeFile, typeNode, &typeCode)) {
        return 0;
    }
    switch (typeCode) {
        case HOPEvalTypeCode_BOOL:    return sourceValue->kind == HOPCTFEValue_BOOL;
        case HOPEvalTypeCode_F32:
        case HOPEvalTypeCode_F64:     return sourceValue->kind == HOPCTFEValue_FLOAT;
        case HOPEvalTypeCode_U8:
        case HOPEvalTypeCode_U16:
        case HOPEvalTypeCode_U32:
        case HOPEvalTypeCode_U64:
        case HOPEvalTypeCode_UINT:
        case HOPEvalTypeCode_I8:
        case HOPEvalTypeCode_I16:
        case HOPEvalTypeCode_I32:
        case HOPEvalTypeCode_I64:
        case HOPEvalTypeCode_INT:     return sourceValue->kind == HOPCTFEValue_INT;
        case HOPEvalTypeCode_STR_REF:
        case HOPEvalTypeCode_STR_PTR: return sourceValue->kind == HOPCTFEValue_STRING;
        case HOPEvalTypeCode_RAWPTR:
            return sourceValue->kind == HOPCTFEValue_REFERENCE
                || sourceValue->kind == HOPCTFEValue_NULL
                || sourceValue->kind == HOPCTFEValue_STRING;
        case HOPEvalTypeCode_TYPE: return sourceValue->kind == HOPCTFEValue_TYPE;
        default:                   return 0;
    }
}

static int32_t HOPEvalFunctionParamTypeNodeAt(const HOPEvalFunction* fn, uint32_t paramIndex) {
    const HOPAst* ast;
    int32_t       child;
    uint32_t      i = 0;
    if (fn == NULL || fn->file == NULL) {
        return -1;
    }
    ast = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= ast->len) {
        return -1;
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
            if (i == paramIndex) {
                return ASTFirstChild(ast, child);
            }
            i++;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static int32_t HOPEvalFunctionParamIndexByName(
    const HOPEvalFunction* fn, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    const HOPAst* ast;
    int32_t       child;
    uint32_t      i = 0;
    if (fn == NULL || fn->file == NULL || source == NULL) {
        return -1;
    }
    ast = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= ast->len) {
        return -1;
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
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

static int HOPEvalExprIsAnytypePackIndex(HOPEvalProgram* p, const HOPAst* ast, int32_t exprNode) {
    int32_t              baseNode;
    int32_t              idxNode;
    int32_t              extraNode;
    HOPCTFEExecBinding*  binding;
    const HOPCTFEValue*  bindingValue;
    const HOPParsedFile* localTypeFile = NULL;
    int32_t              localTypeNode = -1;
    HOPCTFEValue         localValue;
    while (ast != NULL && exprNode >= 0 && (uint32_t)exprNode < ast->len
           && ast->nodes[exprNode].kind == HOPAst_CALL_ARG)
    {
        exprNode = ast->nodes[exprNode].firstChild;
    }
    if (p == NULL || p->currentFile == NULL || p->currentExecCtx == NULL || ast == NULL
        || exprNode < 0 || (uint32_t)exprNode >= ast->len
        || ast->nodes[exprNode].kind != HOPAst_INDEX
        || (ast->nodes[exprNode].flags & HOPAstFlag_INDEX_SLICE) != 0u)
    {
        return 0;
    }
    baseNode = ast->nodes[exprNode].firstChild;
    idxNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
    extraNode = idxNode >= 0 ? ast->nodes[idxNode].nextSibling : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0 || ast->nodes[baseNode].kind != HOPAst_IDENT)
    {
        return 0;
    }
    binding = HOPEvalFindBinding(
        p->currentExecCtx,
        p->currentFile,
        ast->nodes[baseNode].dataStart,
        ast->nodes[baseNode].dataEnd);
    if (binding != NULL && HOPEvalTypeNodeIsAnytype(p->currentFile, binding->typeNode)) {
        bindingValue = HOPEvalValueTargetOrSelf(&binding->value);
        return bindingValue->kind == HOPCTFEValue_ARRAY;
    }
    if (!HOPEvalMirLookupLocalTypeNode(
            p,
            ast->nodes[baseNode].dataStart,
            ast->nodes[baseNode].dataEnd,
            &localTypeFile,
            &localTypeNode)
        || localTypeFile == NULL || localTypeNode < 0
        || !HOPEvalTypeNodeIsAnytype(localTypeFile, localTypeNode)
        || !HOPEvalMirLookupLocalValue(
            p, ast->nodes[baseNode].dataStart, ast->nodes[baseNode].dataEnd, &localValue))
    {
        return 0;
    }
    bindingValue = HOPEvalValueTargetOrSelf(&localValue);
    return bindingValue->kind == HOPCTFEValue_ARRAY;
}

static int HOPEvalParamNameStartsWithUnderscore(
    const char* source, const uint32_t* starts, const uint32_t* ends, uint32_t index) {
    uint32_t start;
    uint32_t end;
    if (source == NULL || starts == NULL || ends == NULL) {
        return 0;
    }
    start = starts[index];
    end = ends[index];
    return end > start && source[start] == '_';
}

static uint32_t HOPEvalPositionalCallPrefixEnd(
    const char*     source,
    const uint32_t* paramNameStarts,
    const uint32_t* paramNameEnds,
    uint32_t        argCount) {
    uint32_t prefixEnd;
    if (argCount == 0) {
        return 0;
    }
    prefixEnd = 1u;
    while (
        prefixEnd < argCount
        && HOPEvalParamNameStartsWithUnderscore(source, paramNameStarts, paramNameEnds, prefixEnd))
    {
        prefixEnd++;
    }
    return prefixEnd;
}

static int HOPEvalReorderFixedCallArgsByName(
    HOPEvalProgram*        p,
    const HOPEvalFunction* fn,
    const HOPAst*          callAst,
    int32_t                firstArgNode,
    HOPCTFEValue*          args,
    uint32_t               argCount,
    uint32_t               paramOffset) {
    uint32_t      argNameStarts[256];
    uint32_t      argNameEnds[256];
    uint32_t      paramNameStarts[256];
    uint32_t      paramNameEnds[256];
    uint8_t       argExplicitName[256];
    uint8_t       paramAssigned[256];
    HOPCTFEValue  reorderedArgs[256];
    const HOPAst* fnAst;
    const char*   callSource;
    int32_t       child;
    int32_t       argNode;
    uint32_t      i = 0;
    uint32_t      positionalPrefixEnd;
    int           reordered = 0;

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
    memset(argExplicitName, 0, sizeof(argExplicitName));
    argNode = firstArgNode;
    while (argNode >= 0) {
        const HOPAstNode* arg = &callAst->nodes[argNode];
        int32_t           exprNode = argNode;
        if (i >= argCount) {
            return 0;
        }
        if (arg->kind == HOPAst_CALL_ARG) {
            if ((arg->flags & HOPAstFlag_CALL_ARG_SPREAD) != 0) {
                return 0;
            }
            exprNode = arg->firstChild;
            if (arg->dataEnd > arg->dataStart) {
                argNameStarts[i] = arg->dataStart;
                argNameEnds[i] = arg->dataEnd;
                argExplicitName[i] = 1u;
            }
        }
        if (exprNode < 0 || (uint32_t)exprNode >= callAst->len) {
            return 0;
        }
        if (!argExplicitName[i] && callAst->nodes[exprNode].kind == HOPAst_IDENT) {
            HOPCTFEValue ignoredTypeValue;
            if (HOPEvalResolveTypeValueName(
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
        const HOPAstNode* n = &fnAst->nodes[child];
        if (n->kind == HOPAst_PARAM) {
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

    positionalPrefixEnd = HOPEvalPositionalCallPrefixEnd(
        fn->file->source, paramNameStarts, paramNameEnds, argCount);
    for (i = 0; i < argCount; i++) {
        uint32_t j;
        uint32_t matchIndex = UINT32_MAX;
        if (!argExplicitName[i] && i < positionalPrefixEnd) {
            matchIndex = i;
            if (paramAssigned[matchIndex]) {
                return 0;
            }
        } else {
            if (argNameEnds[i] <= argNameStarts[i]) {
                return 0;
            }
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
    memcpy(args, reorderedArgs, sizeof(HOPCTFEValue) * argCount);
    return 1;
}

static int32_t HOPEvalFunctionReturnTypeNode(const HOPEvalFunction* fn) {
    const HOPAst* ast;
    int32_t       child;
    if (fn == NULL || fn->file == NULL) {
        return -1;
    }
    ast = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= ast->len) {
        return -1;
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (IsFnReturnTypeNodeKind(n->kind)) {
            return child;
        }
        if (n->kind == HOPAst_BLOCK) {
            break;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static void HOPEvalSaveTemplateBinding(
    const HOPEvalProgram* p, HOPEvalTemplateBindingState* outState) {
    if (p == NULL || outState == NULL) {
        return;
    }
    outState->activeTemplateParamFile = p->activeTemplateParamFile;
    outState->activeTemplateParamNameStart = p->activeTemplateParamNameStart;
    outState->activeTemplateParamNameEnd = p->activeTemplateParamNameEnd;
    outState->activeTemplateTypeFile = p->activeTemplateTypeFile;
    outState->activeTemplateTypeNode = p->activeTemplateTypeNode;
    outState->activeTemplateTypeValue = p->activeTemplateTypeValue;
    outState->hasActiveTemplateTypeValue = p->hasActiveTemplateTypeValue;
}

static void HOPEvalRestoreTemplateBinding(
    HOPEvalProgram* p, const HOPEvalTemplateBindingState* state) {
    if (p == NULL || state == NULL) {
        return;
    }
    p->activeTemplateParamFile = state->activeTemplateParamFile;
    p->activeTemplateParamNameStart = state->activeTemplateParamNameStart;
    p->activeTemplateParamNameEnd = state->activeTemplateParamNameEnd;
    p->activeTemplateTypeFile = state->activeTemplateTypeFile;
    p->activeTemplateTypeNode = state->activeTemplateTypeNode;
    p->activeTemplateTypeValue = state->activeTemplateTypeValue;
    p->hasActiveTemplateTypeValue = state->hasActiveTemplateTypeValue;
}

static int32_t HOPEvalFunctionFirstTypeParamNode(const HOPEvalFunction* fn) {
    const HOPAst* ast;
    int32_t       child;
    if (fn == NULL || fn->file == NULL) {
        return -1;
    }
    ast = &fn->file->ast;
    if (fn->fnNode < 0 || (uint32_t)fn->fnNode >= ast->len) {
        return -1;
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        if (ast->nodes[child].kind == HOPAst_TYPE_PARAM) {
            return child;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static int HOPEvalBindActiveTemplateTypeValue(
    HOPEvalProgram*        p,
    const HOPEvalFunction* fn,
    int32_t                typeParamNode,
    const HOPCTFEValue*    typeValue,
    const HOPParsedFile* _Nullable typeFile,
    int32_t typeNode) {
    const HOPAstNode* param;
    if (p == NULL || fn == NULL || fn->file == NULL || typeValue == NULL
        || typeValue->kind != HOPCTFEValue_TYPE || typeParamNode < 0
        || (uint32_t)typeParamNode >= fn->file->ast.len)
    {
        return 0;
    }
    param = &fn->file->ast.nodes[typeParamNode];
    p->activeTemplateParamFile = fn->file;
    p->activeTemplateParamNameStart = param->dataStart;
    p->activeTemplateParamNameEnd = param->dataEnd;
    p->activeTemplateTypeFile = typeFile;
    p->activeTemplateTypeNode = typeNode;
    p->activeTemplateTypeValue = *typeValue;
    p->hasActiveTemplateTypeValue = 1u;
    return 1;
}

static int HOPEvalBindActiveTemplateForMirCall(
    HOPEvalProgram* p,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    const HOPEvalFunction* fn,
    const HOPCTFEValue* _Nullable args,
    uint32_t argCount) {
    int32_t  returnTypeNode;
    uint32_t typeArgIndex;
    if (p == NULL || fn == NULL) {
        return 0;
    }
    for (typeArgIndex = 0; typeArgIndex < argCount; typeArgIndex++) {
        if (args != NULL && args[typeArgIndex].kind == HOPCTFEValue_TYPE) {
            int32_t typeParamNode = HOPEvalFunctionFirstTypeParamNode(fn);
            return HOPEvalBindActiveTemplateTypeValue(
                p, fn, typeParamNode, &args[typeArgIndex], NULL, -1);
        }
    }
    returnTypeNode = HOPEvalFunctionReturnTypeNode(fn);
    if (program != NULL && function != NULL && inst != NULL && returnTypeNode >= 0
        && HOPEvalTypeNodeIsTemplateParamName(fn->file, returnTypeNode)
        && p->currentMirExecCtx != NULL)
    {
        uint32_t instIndex = UINT32_MAX;
        if (program->insts != NULL && inst >= program->insts
            && inst < program->insts + program->instLen)
        {
            instIndex = (uint32_t)(inst - program->insts);
        }
        if (instIndex != UINT32_MAX && instIndex + 1u < program->instLen
            && program->insts[instIndex + 1u].op == HOPMirOp_LOCAL_STORE)
        {
            uint32_t localSlot = program->insts[instIndex + 1u].aux;
            if (localSlot < function->localCount
                && function->localStart + localSlot < program->localLen)
            {
                const HOPMirLocal* local = &program->locals[function->localStart + localSlot];
                if (local->typeRef < program->typeLen
                    && program->types[local->typeRef].sourceRef
                           < p->currentMirExecCtx->sourceFileCap)
                {
                    const HOPParsedFile* typeFile =
                        p->currentMirExecCtx->sourceFiles[program->types[local->typeRef].sourceRef];
                    HOPCTFEValue typeValue;
                    memset(&typeValue, 0, sizeof(typeValue));
                    if (typeFile != NULL
                        && HOPEvalTypeValueFromTypeNode(
                               p,
                               typeFile,
                               (int32_t)program->types[local->typeRef].astNode,
                               &typeValue)
                               > 0)
                    {
                        return HOPEvalBindActiveTemplateTypeValue(
                            p,
                            fn,
                            returnTypeNode,
                            &typeValue,
                            typeFile,
                            (int32_t)program->types[local->typeRef].astNode);
                    }
                }
            }
        }
    }
    if (returnTypeNode >= 0 && HOPEvalTypeNodeIsTemplateParamName(fn->file, returnTypeNode)
        && p->activeCallExpectedTypeFile != NULL && p->activeCallExpectedTypeNode >= 0)
    {
        HOPCTFEValue typeValue;
        memset(&typeValue, 0, sizeof(typeValue));
        if (HOPEvalTypeValueFromTypeNode(
                p, p->activeCallExpectedTypeFile, p->activeCallExpectedTypeNode, &typeValue)
            > 0)
        {
            return HOPEvalBindActiveTemplateTypeValue(
                p,
                fn,
                returnTypeNode,
                &typeValue,
                p->activeCallExpectedTypeFile,
                p->activeCallExpectedTypeNode);
        }
    }
    return 0;
}

static int HOPEvalFindExpectedTypeForCallExpr(
    const HOPParsedFile* _Nullable file,
    int32_t                         callNode,
    const HOPParsedFile* _Nullable* outTypeFile,
    int32_t*                        outTypeNode) {
    uint32_t i;
    if (outTypeFile != NULL) {
        *outTypeFile = NULL;
    }
    if (outTypeNode != NULL) {
        *outTypeNode = -1;
    }
    if (file == NULL || callNode < 0 || outTypeFile == NULL || outTypeNode == NULL) {
        return 0;
    }
    for (i = 0; i < file->ast.len; i++) {
        const HOPAstNode* n = &file->ast.nodes[i];
        int32_t           typeNode;
        int32_t           initNode;
        if (n->kind != HOPAst_VAR && n->kind != HOPAst_CONST) {
            continue;
        }
        typeNode = ASTFirstChild(&file->ast, (int32_t)i);
        if (typeNode < 0 || !IsFnReturnTypeNodeKind(file->ast.nodes[typeNode].kind)) {
            continue;
        }
        initNode = ASTNextSibling(&file->ast, typeNode);
        if (initNode == callNode) {
            *outTypeFile = file;
            *outTypeNode = typeNode;
            return 1;
        }
    }
    return 0;
}

static int HOPEvalFindExpectedTypeForInitExpr(
    const HOPParsedFile* _Nullable file,
    int32_t                         initExprNode,
    const HOPParsedFile* _Nullable* outTypeFile,
    int32_t*                        outTypeNode) {
    uint32_t i;
    if (outTypeFile != NULL) {
        *outTypeFile = NULL;
    }
    if (outTypeNode != NULL) {
        *outTypeNode = -1;
    }
    if (file == NULL || initExprNode < 0 || outTypeFile == NULL || outTypeNode == NULL) {
        return 0;
    }
    for (i = 0; i < file->ast.len; i++) {
        const HOPAstNode* n = &file->ast.nodes[i];
        int32_t           typeNode;
        int32_t           initNode;
        if (n->kind != HOPAst_VAR && n->kind != HOPAst_CONST) {
            continue;
        }
        typeNode = HOPEvalVarLikeDeclTypeNode(file, (int32_t)i);
        if (typeNode < 0) {
            continue;
        }
        initNode = HOPEvalVarLikeInitExprNodeAt(file, (int32_t)i, 0);
        if (initNode == initExprNode) {
            *outTypeFile = file;
            *outTypeNode = typeNode;
            return 1;
        }
    }
    return 0;
}

static int HOPEvalClassifySimpleTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  file,
    int32_t               typeNode,
    char*                 outKind,
    uint64_t* _Nullable outAliasTag) {
    const HOPAstNode* n;
    char              aliasKind = '\0';
    uint64_t          aliasTag = 0;
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
    if (n->kind != HOPAst_TYPE_NAME) {
        return 0;
    }
    if (HOPEvalResolveSimpleAliasCastTarget(p, file, typeNode, &aliasKind, &aliasTag)) {
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

static int HOPEvalStructEmbeddedBase(
    const HOPEvalProgram* p,
    const HOPParsedFile*  structFile,
    int32_t               structNode,
    const HOPParsedFile** outBaseFile,
    int32_t*              outBaseNode) {
    int32_t           child;
    const HOPPackage* pkg;
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
    pkg = HOPEvalFindPackageByFile(p, structFile);
    if (pkg == NULL) {
        return 0;
    }
    child = ASTFirstChild(&structFile->ast, structNode);
    while (child >= 0) {
        const HOPAstNode* fieldNode = &structFile->ast.nodes[child];
        if (fieldNode->kind == HOPAst_FIELD && (fieldNode->flags & HOPAstFlag_FIELD_EMBEDDED) != 0)
        {
            int32_t              typeNode = ASTFirstChild(&structFile->ast, child);
            const HOPParsedFile* baseFile = NULL;
            int32_t baseNode = HOPEvalFindNamedAggregateDecl(p, structFile, typeNode, &baseFile);
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

static int HOPEvalAggregateDistanceToType(
    const HOPEvalProgram* p,
    const HOPCTFEValue*   value,
    const HOPParsedFile*  callerFile,
    int32_t               typeNode,
    uint32_t*             outDistance) {
    const HOPPackage*    pkg;
    const HOPParsedFile* targetFile = NULL;
    int32_t              targetNode = -1;
    const HOPParsedFile* curFile = NULL;
    int32_t              curNode = -1;
    uint32_t             distance = 0;
    if (outDistance != NULL) {
        *outDistance = 0;
    }
    if (p == NULL || callerFile == NULL) {
        return 0;
    }
    pkg = HOPEvalFindPackageByFile(p, callerFile);
    if (pkg == NULL) {
        return 0;
    }
    (void)pkg;
    if (!HOPEvalResolveAggregateTypeNode(p, callerFile, typeNode, &targetFile, &targetNode)) {
        return 0;
    }
    if (!HOPEvalResolveAggregateDeclFromValue(p, value, &curFile, &curNode)) {
        return 0;
    }
    while (curFile != NULL && curNode >= 0) {
        if (curFile == targetFile && curNode == targetNode) {
            if (outDistance != NULL) {
                *outDistance = distance;
            }
            return 1;
        }
        if (!HOPEvalStructEmbeddedBase(p, curFile, curNode, &curFile, &curNode)) {
            break;
        }
        distance++;
    }
    return 0;
}

static int HOPEvalScoreFunctionCandidate(
    const HOPEvalProgram*  p,
    const HOPEvalFunction* fn,
    const HOPCTFEValue*    args,
    uint32_t               argCount,
    int*                   outScore) {
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
        int32_t  paramTypeNode = HOPEvalFunctionParamTypeNodeAt(
            fn, fn->isVariadic && i >= fixedCount ? fixedCount : i);
        if (paramTypeNode < 0) {
            return 0;
        }
        if (HOPEvalTypeNodeIsTemplateParamName(fn->file, paramTypeNode)) {
            score += 1;
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind == HOPAst_TYPE_NAME
            && SliceEqCStr(
                fn->file->source,
                fn->file->ast.nodes[paramTypeNode].dataStart,
                fn->file->ast.nodes[paramTypeNode].dataEnd,
                "anytype"))
        {
            continue;
        }
        if (HOPEvalAggregateDistanceToType(p, &args[i], fn->file, paramTypeNode, &structDistance)) {
            score += structDistance == 0 ? 16 : (int)(16u - structDistance);
            continue;
        }
        if (!HOPEvalValueSimpleKind(&args[i], &argKind, &argAliasTag)
            || !HOPEvalClassifySimpleTypeNode(
                p, fn->file, paramTypeNode, &paramKind, &paramAliasTag)
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

static int32_t HOPEvalResolveFunctionBySlice(
    const HOPEvalProgram* p,
    const HOPPackage* _Nullable targetPkg,
    const HOPParsedFile* callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const HOPCTFEValue* _Nullable args,
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
        const HOPEvalFunction* fn = &p->funcs[i];
        int                    score = 0;
        uint32_t               fixedCount =
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
        if (args != NULL && HOPEvalScoreFunctionCandidate(p, fn, args, argCount, &score)) {
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

static int32_t HOPEvalFindAnyFunctionBySlice(
    const HOPEvalProgram* p,
    const HOPParsedFile*  callerFile,
    uint32_t              nameStart,
    uint32_t              nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
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

static int HOPEvalExprContainsFieldExpr(const HOPAst* ast, int32_t nodeId) {
    int32_t child;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[nodeId].kind == HOPAst_FIELD_EXPR) {
        return 1;
    }
    child = ast->nodes[nodeId].firstChild;
    while (child >= 0) {
        if (HOPEvalExprContainsFieldExpr(ast, child)) {
            return 1;
        }
        child = ast->nodes[child].nextSibling;
    }
    return 0;
}

static int HOPEvalEvalUnary(
    HOPTokenKind op, const HOPCTFEValue* inValue, HOPCTFEValue* outValue, int* outIsConst) {
    if (outValue == NULL || outIsConst == NULL || inValue == NULL) {
        return -1;
    }
    switch (op) {
        case HOPTok_NOT:
            if (inValue->kind != HOPCTFEValue_BOOL) {
                return 0;
            }
            outValue->kind = HOPCTFEValue_BOOL;
            outValue->b = inValue->b ? 0u : 1u;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            *outIsConst = 1;
            return 1;
        case HOPTok_SUB:
            if (inValue->kind == HOPCTFEValue_INT) {
                HOPEvalValueSetInt(outValue, -inValue->i64);
                *outIsConst = 1;
                return 1;
            }
            if (inValue->kind == HOPCTFEValue_FLOAT) {
                outValue->kind = HOPCTFEValue_FLOAT;
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

static int HOPEvalLexCompareStrings(const HOPCTFEValue* lhs, const HOPCTFEValue* rhs, int* outCmp) {
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

static int32_t HOPEvalResolveFunctionByLiteralArgs(
    const HOPEvalProgram* p, const char* name, const HOPCTFEValue* args, uint32_t argCount) {
    uint32_t i;
    int32_t  best = -1;
    int      bestScore = -1;
    int      ambiguous = 0;
    if (p == NULL || name == NULL) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
        int                    score = 0;
        uint32_t               fixedCount =
            fn->isVariadic && fn->paramCount > 0 ? fn->paramCount - 1u : fn->paramCount;
        if ((!fn->isVariadic && fn->paramCount != argCount)
            || (fn->isVariadic && argCount < fixedCount))
        {
            continue;
        }
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)) {
            continue;
        }
        if (args != NULL && HOPEvalScoreFunctionCandidate(p, fn, args, argCount, &score)) {
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

static int HOPEvalCompareValues(
    HOPEvalProgram*     p,
    const HOPCTFEValue* lhs,
    const HOPCTFEValue* rhs,
    int*                outCmp,
    int*                outHandled);

static int HOPEvalTaggedEnumPayloadEqual(
    HOPEvalProgram* p, const HOPEvalTaggedEnum* lhs, const HOPEvalTaggedEnum* rhs, int* outEqual) {
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
            if (HOPEvalCompareValues(
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

static int HOPEvalCompareValues(
    HOPEvalProgram*     p,
    const HOPCTFEValue* lhs,
    const HOPCTFEValue* rhs,
    int*                outCmp,
    int*                outHandled) {
    const HOPCTFEValue* lhsValue = HOPEvalValueTargetOrSelf(lhs);
    const HOPCTFEValue* rhsValue = HOPEvalValueTargetOrSelf(rhs);
    if (outCmp != NULL) {
        *outCmp = 0;
    }
    if (outHandled != NULL) {
        *outHandled = 0;
    }
    if (lhs == NULL || rhs == NULL || outCmp == NULL || outHandled == NULL) {
        return -1;
    }
    if ((lhsValue->kind == HOPCTFEValue_NULL && rhsValue->kind == HOPCTFEValue_REFERENCE)
        || (lhsValue->kind == HOPCTFEValue_REFERENCE && rhsValue->kind == HOPCTFEValue_NULL))
    {
        uintptr_t la = lhsValue->kind == HOPCTFEValue_REFERENCE ? (uintptr_t)lhsValue->s.bytes : 0u;
        uintptr_t ra = rhsValue->kind == HOPCTFEValue_REFERENCE ? (uintptr_t)rhsValue->s.bytes : 0u;
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if ((lhsValue->kind == HOPCTFEValue_NULL && rhsValue->kind == HOPCTFEValue_STRING)
        || (lhsValue->kind == HOPCTFEValue_STRING && rhsValue->kind == HOPCTFEValue_NULL))
    {
        int32_t   typeCode = HOPEvalTypeCode_INVALID;
        uintptr_t la = lhsValue->kind == HOPCTFEValue_STRING ? (uintptr_t)lhsValue->s.bytes : 0u;
        uintptr_t ra = rhsValue->kind == HOPCTFEValue_STRING ? (uintptr_t)rhsValue->s.bytes : 0u;
        const HOPCTFEValue* stringValue =
            lhsValue->kind == HOPCTFEValue_STRING ? lhsValue : rhsValue;
        if (!HOPEvalValueGetRuntimeTypeCode(stringValue, &typeCode)
            || (typeCode != HOPEvalTypeCode_RAWPTR && typeCode != HOPEvalTypeCode_STR_PTR
                && typeCode != HOPEvalTypeCode_STR_REF))
        {
            return 0;
        }
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhs->kind == HOPCTFEValue_REFERENCE && rhs->kind == HOPCTFEValue_REFERENCE) {
        uintptr_t la = (uintptr_t)lhs->s.bytes;
        uintptr_t ra = (uintptr_t)rhs->s.bytes;
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if ((lhsValue->kind == HOPCTFEValue_REFERENCE && rhsValue->kind == HOPCTFEValue_STRING)
        || (lhsValue->kind == HOPCTFEValue_STRING && rhsValue->kind == HOPCTFEValue_REFERENCE))
    {
        int32_t             typeCode = HOPEvalTypeCode_INVALID;
        const HOPCTFEValue* stringValue =
            lhsValue->kind == HOPCTFEValue_STRING ? lhsValue : rhsValue;
        uintptr_t la =
            lhsValue->kind == HOPCTFEValue_REFERENCE
                ? (uintptr_t)lhsValue->s.bytes
                : (uintptr_t)lhsValue->s.bytes;
        uintptr_t ra =
            rhsValue->kind == HOPCTFEValue_REFERENCE
                ? (uintptr_t)rhsValue->s.bytes
                : (uintptr_t)rhsValue->s.bytes;
        if (!HOPEvalValueGetRuntimeTypeCode(stringValue, &typeCode)
            || (typeCode != HOPEvalTypeCode_RAWPTR && typeCode != HOPEvalTypeCode_STR_PTR
                && typeCode != HOPEvalTypeCode_STR_REF))
        {
            return 0;
        }
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == HOPCTFEValue_INT && rhsValue->kind == HOPCTFEValue_INT) {
        *outCmp = lhsValue->i64 < rhsValue->i64 ? -1 : (lhsValue->i64 > rhsValue->i64 ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == HOPCTFEValue_FLOAT && rhsValue->kind == HOPCTFEValue_FLOAT) {
        *outCmp = lhsValue->f64 < rhsValue->f64 ? -1 : (lhsValue->f64 > rhsValue->f64 ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == HOPCTFEValue_BOOL && rhsValue->kind == HOPCTFEValue_BOOL) {
        *outCmp = lhsValue->b < rhsValue->b ? -1 : (lhsValue->b > rhsValue->b ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == HOPCTFEValue_STRING && rhsValue->kind == HOPCTFEValue_STRING) {
        int32_t lhsTypeCode = HOPEvalTypeCode_INVALID;
        int32_t rhsTypeCode = HOPEvalTypeCode_INVALID;
        if ((HOPEvalValueGetRuntimeTypeCode(lhsValue, &lhsTypeCode)
             && (lhsTypeCode == HOPEvalTypeCode_RAWPTR || lhsTypeCode == HOPEvalTypeCode_STR_PTR))
            || (HOPEvalValueGetRuntimeTypeCode(rhsValue, &rhsTypeCode)
                && (rhsTypeCode == HOPEvalTypeCode_RAWPTR
                    || rhsTypeCode == HOPEvalTypeCode_STR_PTR)))
        {
            uintptr_t la = (uintptr_t)lhsValue->s.bytes;
            uintptr_t ra = (uintptr_t)rhsValue->s.bytes;
            *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
            *outHandled = 1;
            return 0;
        }
        if (HOPEvalLexCompareStrings(lhsValue, rhsValue, outCmp) != 0) {
            return -1;
        }
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == HOPCTFEValue_TYPE && rhsValue->kind == HOPCTFEValue_TYPE) {
        int32_t lhsTypeCode = 0;
        int32_t rhsTypeCode = 0;
        if (HOPEvalValueGetSimpleTypeCode(lhsValue, &lhsTypeCode)
            && HOPEvalValueGetSimpleTypeCode(rhsValue, &rhsTypeCode))
        {
            *outCmp = lhsTypeCode < rhsTypeCode ? -1 : (lhsTypeCode > rhsTypeCode ? 1 : 0);
            *outHandled = 1;
            return 0;
        }
        {
            HOPEvalReflectedType* lhsType = HOPEvalValueAsReflectedType(lhsValue);
            HOPEvalReflectedType* rhsType = HOPEvalValueAsReflectedType(rhsValue);
            if (lhsType != NULL && rhsType != NULL) {
                if (lhsType->kind != rhsType->kind || lhsType->namedKind != rhsType->namedKind
                    || lhsType->file != rhsType->file || lhsType->nodeId != rhsType->nodeId
                    || lhsType->arrayLen != rhsType->arrayLen)
                {
                    *outCmp = 1;
                    *outHandled = 1;
                    return 0;
                }
                if (lhsType->kind == HOPEvalReflectType_PTR
                    || lhsType->kind == HOPEvalReflectType_SLICE
                    || lhsType->kind == HOPEvalReflectType_ARRAY)
                {
                    int elemCmp = 0;
                    int elemHandled = 0;
                    if (HOPEvalCompareValues(
                            p, &lhsType->elemType, &rhsType->elemType, &elemCmp, &elemHandled)
                        != 0)
                    {
                        return -1;
                    }
                    if (!elemHandled) {
                        return 0;
                    }
                    *outCmp = elemCmp;
                    *outHandled = 1;
                    return 0;
                }
                *outCmp = 0;
                *outHandled = 1;
                return 0;
            }
        }
    }
    {
        HOPEvalTaggedEnum* lhsTagged = HOPEvalValueAsTaggedEnum(lhsValue);
        HOPEvalTaggedEnum* rhsTagged = HOPEvalValueAsTaggedEnum(rhsValue);
        if (lhsTagged != NULL && rhsTagged != NULL && lhsTagged->file == rhsTagged->file
            && lhsTagged->enumNode == rhsTagged->enumNode)
        {
            int equal = 0;
            if (HOPEvalTaggedEnumPayloadEqual(p, lhsTagged, rhsTagged, &equal) != 0) {
                return -1;
            }
            *outCmp = equal ? 0 : 1;
            *outHandled = 1;
            return 0;
        }
    }
    {
        HOPEvalArray* lhsArray = HOPEvalValueAsArray(lhsValue);
        HOPEvalArray* rhsArray = HOPEvalValueAsArray(rhsValue);
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
                if (HOPEvalCompareValues(
                        p, &lhsArray->elems[i], &rhsArray->elems[i], &cmp, &handled)
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
        HOPEvalAggregate* lhsAgg = HOPEvalValueAsAggregate(lhsValue);
        HOPEvalAggregate* rhsAgg = HOPEvalValueAsAggregate(rhsValue);
        if (lhsAgg != NULL && rhsAgg != NULL) {
            HOPCTFEValue args[2];
            args[0] = *lhsValue;
            args[1] = *rhsValue;
            {
                int32_t hookIndex = HOPEvalResolveFunctionByLiteralArgs(p, "__order", args, 2);
                if (hookIndex >= 0) {
                    HOPCTFEValue hookValue;
                    int          didReturn = 0;
                    int64_t      order = 0;
                    if (HOPEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && HOPCTFEValueToInt64(&hookValue, &order) == 0) {
                        *outCmp = order < 0 ? -1 : (order > 0 ? 1 : 0);
                        *outHandled = 1;
                        return 0;
                    }
                }
            }
            {
                int32_t hookIndex = HOPEvalResolveFunctionByLiteralArgs(p, "__equal", args, 2);
                if (hookIndex >= 0) {
                    HOPCTFEValue hookValue;
                    int          didReturn = 0;
                    if (HOPEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && hookValue.kind == HOPCTFEValue_BOOL) {
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
                    if (HOPEvalCompareValues(
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

static int HOPEvalCompareOptionalEq(
    HOPEvalProgram*     p,
    const HOPCTFEValue* lhs,
    const HOPCTFEValue* rhs,
    uint8_t*            outEqual,
    int*                outHandled) {
    const HOPCTFEValue* lhsValue = HOPEvalValueTargetOrSelf(lhs);
    const HOPCTFEValue* rhsValue = HOPEvalValueTargetOrSelf(rhs);
    const HOPCTFEValue* lhsPayload = NULL;
    const HOPCTFEValue* rhsPayload = NULL;
    int                 lhsIsOptional = 0;
    int                 rhsIsOptional = 0;
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
        lhsValue->kind == HOPCTFEValue_OPTIONAL && HOPEvalOptionalPayload(lhsValue, &lhsPayload);
    rhsIsOptional =
        rhsValue->kind == HOPCTFEValue_OPTIONAL && HOPEvalOptionalPayload(rhsValue, &rhsPayload);
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
            if (HOPEvalCompareValues(p, lhsPayload, rhsPayload, &cmp, &handled) != 0) {
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
            *outEqual = rhsValue->kind == HOPCTFEValue_NULL ? 1u : 0u;
            *outHandled = 1;
            return 0;
        }
        if (rhsValue->kind == HOPCTFEValue_NULL) {
            *outEqual = 0u;
            *outHandled = 1;
            return 0;
        }
        {
            int cmp = 0;
            int handled = 0;
            if (HOPEvalCompareValues(p, lhsPayload, rhsValue, &cmp, &handled) != 0) {
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
        *outEqual = lhsValue->kind == HOPCTFEValue_NULL ? 1u : 0u;
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == HOPCTFEValue_NULL) {
        *outEqual = 0u;
        *outHandled = 1;
        return 0;
    }
    {
        int cmp = 0;
        int handled = 0;
        if (HOPEvalCompareValues(p, lhsValue, rhsPayload, &cmp, &handled) != 0) {
            return -1;
        }
        if (handled) {
            *outEqual = cmp == 0 ? 1u : 0u;
            *outHandled = 1;
        }
    }
    return 0;
}

static int HOPEvalEvalBinary(
    HOPEvalProgram*     p,
    HOPTokenKind        op,
    const HOPCTFEValue* lhs,
    const HOPCTFEValue* rhs,
    HOPCTFEValue*       outValue,
    int*                outIsConst) {
    int cmp = 0;
    int handled = 0;
    if (lhs == NULL || rhs == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if ((op == HOPTok_EQ || op == HOPTok_NEQ || op == HOPTok_LT || op == HOPTok_LTE
         || op == HOPTok_GT || op == HOPTok_GTE))
    {
        HOPEvalAggregate* lhsAgg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(lhs));
        HOPEvalAggregate* rhsAgg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(rhs));
        if (p != NULL && lhsAgg != NULL && rhsAgg != NULL) {
            HOPCTFEValue args[2];
            int32_t      hookIndex = -1;
            HOPCTFEValue hookValue;
            int          didReturn = 0;
            args[0] = *HOPEvalValueTargetOrSelf(lhs);
            args[1] = *HOPEvalValueTargetOrSelf(rhs);
            if (op == HOPTok_EQ || op == HOPTok_NEQ) {
                hookIndex = HOPEvalResolveFunctionByLiteralArgs(p, "__equal", args, 2);
                if (hookIndex >= 0) {
                    if (HOPEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && hookValue.kind == HOPCTFEValue_BOOL) {
                        outValue->kind = HOPCTFEValue_BOOL;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->typeTag = 0;
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->b = op == HOPTok_EQ ? hookValue.b : (uint8_t)!hookValue.b;
                        *outIsConst = 1;
                        return 1;
                    }
                }
            } else {
                hookIndex = HOPEvalResolveFunctionByLiteralArgs(p, "__order", args, 2);
                if (hookIndex >= 0) {
                    int64_t order = 0;
                    if (HOPEvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && HOPCTFEValueToInt64(&hookValue, &order) == 0) {
                        outValue->kind = HOPCTFEValue_BOOL;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->typeTag = 0;
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->b =
                            op == HOPTok_LT    ? order < 0
                            : op == HOPTok_LTE ? order <= 0
                            : op == HOPTok_GT
                                ? order > 0
                                : order >= 0;
                        *outIsConst = 1;
                        return 1;
                    }
                }
            }
        }
    }
    if (op == HOPTok_EQ || op == HOPTok_NEQ || op == HOPTok_LT || op == HOPTok_LTE
        || op == HOPTok_GT || op == HOPTok_GTE)
    {
        HOPEvalTaggedEnum* lhsTagged = HOPEvalValueAsTaggedEnum(HOPEvalValueTargetOrSelf(lhs));
        HOPEvalTaggedEnum* rhsTagged = HOPEvalValueAsTaggedEnum(HOPEvalValueTargetOrSelf(rhs));
        if ((op == HOPTok_LT || op == HOPTok_LTE || op == HOPTok_GT || op == HOPTok_GTE)
            && lhsTagged != NULL && rhsTagged != NULL && lhsTagged->file == rhsTagged->file
            && lhsTagged->enumNode == rhsTagged->enumNode)
        {
            outValue->kind = HOPCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            switch (op) {
                case HOPTok_LT:  outValue->b = lhsTagged->tagIndex < rhsTagged->tagIndex; break;
                case HOPTok_LTE: outValue->b = lhsTagged->tagIndex <= rhsTagged->tagIndex; break;
                case HOPTok_GT:  outValue->b = lhsTagged->tagIndex > rhsTagged->tagIndex; break;
                case HOPTok_GTE: outValue->b = lhsTagged->tagIndex >= rhsTagged->tagIndex; break;
                default:         outValue->b = 0; break;
            }
            *outIsConst = 1;
            return 1;
        }
        if (op == HOPTok_EQ || op == HOPTok_NEQ) {
            uint8_t equal = 0;
            if (HOPEvalCompareOptionalEq(p, lhs, rhs, &equal, &handled) != 0) {
                return -1;
            }
            if (handled) {
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->b = op == HOPTok_EQ ? equal : (uint8_t)!equal;
                *outIsConst = 1;
                return 1;
            }
        }
        if (HOPEvalCompareValues(p, lhs, rhs, &cmp, &handled) != 0) {
            return -1;
        }
        if (handled) {
            outValue->kind = HOPCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            switch (op) {
                case HOPTok_EQ:  outValue->b = cmp == 0; break;
                case HOPTok_NEQ: outValue->b = cmp != 0; break;
                case HOPTok_LT:  outValue->b = cmp < 0; break;
                case HOPTok_LTE: outValue->b = cmp <= 0; break;
                case HOPTok_GT:  outValue->b = cmp > 0; break;
                case HOPTok_GTE: outValue->b = cmp >= 0; break;
                default:         break;
            }
            *outIsConst = 1;
            return 1;
        }
    }
    if (lhs->kind == HOPCTFEValue_INT && rhs->kind == HOPCTFEValue_INT) {
        switch (op) {
            case HOPTok_ADD: HOPEvalValueSetInt(outValue, lhs->i64 + rhs->i64); break;
            case HOPTok_SUB: HOPEvalValueSetInt(outValue, lhs->i64 - rhs->i64); break;
            case HOPTok_MUL: HOPEvalValueSetInt(outValue, lhs->i64 * rhs->i64); break;
            case HOPTok_DIV:
                if (rhs->i64 == 0) {
                    return 0;
                }
                HOPEvalValueSetInt(outValue, lhs->i64 / rhs->i64);
                break;
            case HOPTok_MOD:
                if (rhs->i64 == 0) {
                    return 0;
                }
                HOPEvalValueSetInt(outValue, lhs->i64 % rhs->i64);
                break;
            default: return 0;
        }
        *outIsConst = 1;
        return 1;
    }
    if (lhs->kind == HOPCTFEValue_BOOL && rhs->kind == HOPCTFEValue_BOOL) {
        switch (op) {
            case HOPTok_AND:
            case HOPTok_OR:
            case HOPTok_LOGICAL_AND:
            case HOPTok_LOGICAL_OR:
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (op == HOPTok_AND || op == HOPTok_LOGICAL_AND) {
                    outValue->b = lhs->b && rhs->b;
                } else {
                    outValue->b = lhs->b || rhs->b;
                }
                *outIsConst = 1;
                return 1;
            default: return 0;
        }
    }
    if (lhs->kind == HOPCTFEValue_FLOAT && rhs->kind == HOPCTFEValue_FLOAT) {
        switch (op) {
            case HOPTok_ADD:
            case HOPTok_SUB:
            case HOPTok_MUL:
            case HOPTok_DIV:
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 =
                    op == HOPTok_ADD   ? lhs->f64 + rhs->f64
                    : op == HOPTok_SUB ? lhs->f64 - rhs->f64
                    : op == HOPTok_MUL
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
    if (lhs->kind == HOPCTFEValue_NULL && rhs->kind == HOPCTFEValue_NULL) {
        if (op == HOPTok_EQ || op == HOPTok_NEQ) {
            outValue->kind = HOPCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = op == HOPTok_EQ ? 1u : 0u;
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

static int HOPEvalResolveAggregateTypeNode(
    const HOPEvalProgram* p,
    const HOPParsedFile*  typeFile,
    int32_t               typeNode,
    const HOPParsedFile** outDeclFile,
    int32_t*              outDeclNode) {
    const HOPAstNode*    n;
    const HOPParsedFile* declFile = NULL;
    int32_t              declNode = -1;
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
    while (n->kind == HOPAst_TYPE_REF || n->kind == HOPAst_TYPE_MUTREF) {
        typeNode = n->firstChild;
        if (typeNode < 0 || (uint32_t)typeNode >= typeFile->ast.len) {
            return 0;
        }
        n = &typeFile->ast.nodes[typeNode];
    }
    if (n->kind == HOPAst_TYPE_ANON_STRUCT || n->kind == HOPAst_TYPE_ANON_UNION) {
        if (outDeclFile != NULL) {
            *outDeclFile = typeFile;
        }
        if (outDeclNode != NULL) {
            *outDeclNode = typeNode;
        }
        return 1;
    }
    if (n->kind != HOPAst_TYPE_NAME) {
        return 0;
    }
    declNode = HOPEvalFindNamedAggregateDecl(p, typeFile, typeNode, &declFile);
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

static int HOPEvalExecExprForTypeCb(
    void* ctx, int32_t exprNode, int32_t typeNode, HOPCTFEValue* outValue, int* outIsConst);

static int HOPEvalExecExprInFileWithType(
    HOPEvalProgram*      p,
    const HOPParsedFile* exprFile,
    HOPCTFEExecEnv*      env,
    int32_t              exprNode,
    const HOPParsedFile* typeFile,
    int32_t              typeNode,
    HOPCTFEValue*        outValue,
    int*                 outIsConst) {
    const HOPParsedFile* savedFile;
    HOPCTFEExecCtx*      savedExecCtx;
    HOPCTFEExecCtx       tempExecCtx;
    int                  rc;
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
    tempExecCtx.evalExpr = HOPEvalExecExprCb;
    tempExecCtx.evalExprCtx = p;
    tempExecCtx.evalExprForType = HOPEvalExecExprForTypeCb;
    tempExecCtx.evalExprForTypeCtx = p;
    tempExecCtx.zeroInit = HOPEvalZeroInitCb;
    tempExecCtx.zeroInitCtx = p;
    tempExecCtx.assignExpr = HOPEvalAssignExprCb;
    tempExecCtx.assignExprCtx = p;
    tempExecCtx.assignValueExpr = HOPEvalAssignValueExprCb;
    tempExecCtx.assignValueExprCtx = p;
    tempExecCtx.matchPattern = HOPEvalMatchPatternCb;
    tempExecCtx.matchPatternCtx = p;
    tempExecCtx.forInIndex = HOPEvalForInIndexCb;
    tempExecCtx.forInIndexCtx = p;
    tempExecCtx.forInIter = HOPEvalForInIterCb;
    tempExecCtx.forInIterCtx = p;
    tempExecCtx.pendingReturnExprNode = -1;
    if (tempExecCtx.forIterLimit == 0) {
        tempExecCtx.forIterLimit = HOPCTFE_EXEC_DEFAULT_FOR_LIMIT;
    }
    HOPCTFEExecResetReason(&tempExecCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = exprFile;
    p->currentExecCtx = &tempExecCtx;
    rc = HOPEvalExecExprWithTypeNode(p, exprNode, typeFile, typeNode, outValue, outIsConst);
    p->currentExecCtx = savedExecCtx;
    p->currentFile = savedFile;

    if (rc == 0 && !*outIsConst && savedExecCtx != NULL && tempExecCtx.nonConstReason != NULL) {
        HOPCTFEExecSetReason(
            savedExecCtx,
            tempExecCtx.nonConstStart,
            tempExecCtx.nonConstEnd,
            tempExecCtx.nonConstReason);
    }
    return rc;
}

static int HOPEvalEvalCompoundLiteral(
    HOPEvalProgram*      p,
    int32_t              exprNode,
    const HOPParsedFile* litFile,
    const HOPParsedFile* expectedTypeFile,
    int32_t              expectedTypeNode,
    HOPCTFEValue*        outValue,
    int*                 outIsConst) {
    const HOPAst*                  ast;
    int32_t                        child;
    int32_t                        fieldNode;
    const HOPParsedFile*           declFile = NULL;
    int32_t                        declNode = -1;
    const HOPParsedFile*           targetTypeFile = expectedTypeFile;
    int32_t                        targetTypeNode = expectedTypeNode;
    HOPCTFEValue                   aggregateValue;
    int                            aggregateIsConst = 0;
    HOPEvalAggregate*              agg;
    uint8_t*                       explicitSet = NULL;
    HOPCTFEValue*                  explicitValues = NULL;
    HOPEvalExplicitAggregateField* promotedExplicit = NULL;
    HOPCTFEExecBinding*            fieldBindings = NULL;
    HOPCTFEExecEnv                 fieldFrame;
    uint32_t                       promotedExplicitCap = 0;
    uint32_t                       promotedExplicitLen = 0;
    uint32_t                       fieldBindingCap = 0;
    uint32_t                       i;
    int                            rc = 0;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || litFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &litFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len
        || ast->nodes[exprNode].kind != HOPAst_COMPOUND_LIT)
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
        && targetTypeFile->ast.nodes[targetTypeNode].kind == HOPAst_TYPE_NAME)
    {
        const HOPAstNode* typeNode = &targetTypeFile->ast.nodes[targetTypeNode];
        uint32_t          dot = typeNode->dataStart;
        while (dot < typeNode->dataEnd && targetTypeFile->source[dot] != '.') {
            dot++;
        }
        if (dot < typeNode->dataEnd) {
            const HOPParsedFile* enumFile = NULL;
            int32_t              enumNode = HOPEvalFindNamedEnumDecl(
                p, targetTypeFile, typeNode->dataStart, dot, &enumFile);
            int32_t  variantNode = -1;
            uint32_t tagIndex = 0;
            if (enumNode >= 0 && enumFile != NULL
                && HOPEvalFindEnumVariant(
                    enumFile,
                    enumNode,
                    targetTypeFile->source,
                    dot + 1u,
                    typeNode->dataEnd,
                    &variantNode,
                    &tagIndex))
            {
                const HOPAstNode* variantField = &enumFile->ast.nodes[variantNode];
                HOPCTFEValue      payloadValue;
                int               payloadIsConst = 0;
                HOPEvalAggregate* payload = NULL;
                if (HOPEvalBuildTaggedEnumPayload(
                        p, enumFile, variantNode, exprNode, &payloadValue, &payloadIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!payloadIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                payload = HOPEvalValueAsAggregate(&payloadValue);
                HOPEvalValueSetTaggedEnum(
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
        && targetTypeFile->ast.nodes[targetTypeNode].kind == HOPAst_TYPE_NAME
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
        HOPCTFEValue stringValue;
        int          stringIsConst = 0;
        if (HOPEvalZeroInitTypeNode(p, targetTypeFile, targetTypeNode, &stringValue, &stringIsConst)
            != 0)
        {
            return -1;
        }
        if (!stringIsConst) {
            *outIsConst = 0;
            return 0;
        }
        while (fieldNode >= 0) {
            const HOPAstNode* fieldAst = &ast->nodes[fieldNode];
            int32_t           valueNode = ASTFirstChild(ast, fieldNode);
            HOPCTFEValue      fieldValue;
            int               fieldIsConst = 0;
            if (fieldAst->kind != HOPAst_COMPOUND_FIELD || valueNode < 0) {
                *outIsConst = 0;
                return 0;
            }
            if (HOPEvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
                return -1;
            }
            if (!fieldIsConst
                || !HOPEvalValueSetFieldPath(
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
    if (!HOPEvalResolveAggregateTypeNode(
            p,
            targetTypeFile != NULL ? targetTypeFile : litFile,
            targetTypeNode,
            &declFile,
            &declNode))
    {
        uint32_t inferredFieldCount = 0;
        int32_t  scanNode = fieldNode;
        while (scanNode >= 0) {
            if (ast->nodes[scanNode].kind != HOPAst_COMPOUND_FIELD) {
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
        agg = (HOPEvalAggregate*)HOPArenaAlloc(
            p->arena, sizeof(HOPEvalAggregate), (uint32_t)_Alignof(HOPEvalAggregate));
        if (agg == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg, 0, sizeof(*agg));
        agg->file = litFile;
        agg->nodeId = exprNode;
        agg->fieldLen = inferredFieldCount;
        agg->fields = (HOPEvalAggregateField*)HOPArenaAlloc(
            p->arena,
            sizeof(HOPEvalAggregateField) * inferredFieldCount,
            (uint32_t)_Alignof(HOPEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(HOPEvalAggregateField) * inferredFieldCount);
        scanNode = fieldNode;
        for (i = 0; i < inferredFieldCount; i++) {
            const HOPAstNode* fieldAst = &ast->nodes[scanNode];
            int32_t           valueNode = ASTFirstChild(ast, scanNode);
            int               fieldIsConst = 0;
            if (valueNode < 0
                || HOPEvalExecExprCb(p, valueNode, &agg->fields[i].value, &fieldIsConst) != 0)
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
        HOPEvalValueSetAggregate(outValue, litFile, exprNode, agg);
        *outIsConst = 1;
        return 0;
    }
    if (HOPEvalZeroInitAggregateValue(p, declFile, declNode, &aggregateValue, &aggregateIsConst)
        != 0)
    {
        return -1;
    }
    if (!aggregateIsConst) {
        *outIsConst = 0;
        return 0;
    }
    agg = HOPEvalValueAsAggregate(&aggregateValue);
    if (agg == NULL) {
        *outIsConst = 0;
        return 0;
    }
    {
        int32_t scanNode = fieldNode;
        while (scanNode >= 0) {
            if (ast->nodes[scanNode].kind == HOPAst_COMPOUND_FIELD) {
                promotedExplicitCap++;
            }
            scanNode = ASTNextSibling(ast, scanNode);
        }
    }
    if (agg->fieldLen > 0) {
        fieldBindingCap = HOPEvalAggregateFieldBindingCount(agg);
        explicitSet = (uint8_t*)HOPArenaAlloc(p->arena, agg->fieldLen, (uint32_t)_Alignof(uint8_t));
        explicitValues = (HOPCTFEValue*)HOPArenaAlloc(
            p->arena, sizeof(HOPCTFEValue) * agg->fieldLen, (uint32_t)_Alignof(HOPCTFEValue));
        if (promotedExplicitCap > 0) {
            promotedExplicit = (HOPEvalExplicitAggregateField*)HOPArenaAlloc(
                p->arena,
                sizeof(HOPEvalExplicitAggregateField) * promotedExplicitCap,
                (uint32_t)_Alignof(HOPEvalExplicitAggregateField));
        }
        fieldBindings = (HOPCTFEExecBinding*)HOPArenaAlloc(
            p->arena,
            sizeof(HOPCTFEExecBinding) * fieldBindingCap,
            (uint32_t)_Alignof(HOPCTFEExecBinding));
        if (explicitSet == NULL || explicitValues == NULL
            || (promotedExplicitCap > 0 && promotedExplicit == NULL) || fieldBindings == NULL)
        {
            return ErrorSimple("out of memory");
        }
        memset(explicitSet, 0, agg->fieldLen);
        memset(explicitValues, 0, sizeof(HOPCTFEValue) * agg->fieldLen);
        if (promotedExplicit != NULL) {
            memset(
                promotedExplicit, 0, sizeof(HOPEvalExplicitAggregateField) * promotedExplicitCap);
        }
        memset(fieldBindings, 0, sizeof(HOPCTFEExecBinding) * fieldBindingCap);
    }

    while (fieldNode >= 0) {
        const HOPAstNode*      fieldAst = &ast->nodes[fieldNode];
        int32_t                valueNode = ASTFirstChild(ast, fieldNode);
        int32_t                directFieldIndex;
        int32_t                topFieldIndex;
        HOPEvalAggregateField* valueField;
        HOPEvalAggregate*      valueFieldOwner = NULL;
        HOPCTFEValue           fieldValue;
        int                    fieldIsConst = 0;
        if (fieldAst->kind != HOPAst_COMPOUND_FIELD) {
            *outIsConst = 0;
            return 0;
        }
        directFieldIndex = HOPEvalAggregateLookupDirectFieldIndex(
            agg, litFile->source, fieldAst->dataStart, fieldAst->dataEnd);
        topFieldIndex = HOPEvalAggregateLookupFieldIndex(
            agg, litFile->source, fieldAst->dataStart, fieldAst->dataEnd);
        valueField = HOPEvalAggregateLookupField(
            agg, litFile->source, fieldAst->dataStart, fieldAst->dataEnd, &valueFieldOwner);
        if (valueNode >= 0 && valueField != NULL) {
            if (HOPEvalExecExprWithTypeNode(
                    p,
                    valueNode,
                    valueFieldOwner != NULL ? valueFieldOwner->file : agg->file,
                    valueField->typeNode,
                    &fieldValue,
                    &fieldIsConst)
                != 0)
            {
                return -1;
            }
        } else if ((fieldAst->flags & HOPAstFlag_COMPOUND_FIELD_SHORTHAND) != 0) {
            if (HOPEvalResolveIdent(
                    p, fieldAst->dataStart, fieldAst->dataEnd, &fieldValue, &fieldIsConst, NULL)
                != 0)
            {
                return -1;
            }
        } else if (valueNode >= 0) {
            if (HOPEvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
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
        if (directFieldIndex >= 0) {
            if (explicitSet == NULL || explicitValues == NULL) {
                return ErrorSimple("out of memory");
            }
            explicitSet[directFieldIndex] = 1u;
            explicitValues[directFieldIndex] = fieldValue;
        } else if (topFieldIndex >= 0) {
            if (promotedExplicit == NULL || promotedExplicitLen >= promotedExplicitCap) {
                return ErrorSimple("out of memory");
            }
            promotedExplicit[promotedExplicitLen].nameStart = fieldAst->dataStart;
            promotedExplicit[promotedExplicitLen].nameEnd = fieldAst->dataEnd;
            promotedExplicit[promotedExplicitLen].topFieldIndex = topFieldIndex;
            promotedExplicit[promotedExplicitLen].value = fieldValue;
            promotedExplicitLen++;
            if (!HOPEvalValueSetFieldPath(
                    &aggregateValue,
                    litFile->source,
                    fieldAst->dataStart,
                    fieldAst->dataEnd,
                    &fieldValue))
            {
                *outIsConst = 0;
                return 0;
            }
        } else if (!HOPEvalValueSetFieldPath(
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
        HOPEvalAggregateField* field = &agg->fields[i];
        if (explicitSet != NULL && explicitSet[i] != 0u) {
            field->value = explicitValues[i];
        } else if (field->defaultExprNode >= 0) {
            HOPCTFEValue defaultValue;
            int          defaultIsConst = 0;
            if (HOPEvalExecExprInFileWithType(
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
        if ((field->flags & HOPAstFlag_FIELD_EMBEDDED) != 0 && promotedExplicit != NULL) {
            uint32_t j;
            for (j = 0; j < promotedExplicitLen; j++) {
                if (promotedExplicit[j].topFieldIndex == (int32_t)i
                    && !HOPEvalValueSetFieldPath(
                        &aggregateValue,
                        litFile->source,
                        promotedExplicit[j].nameStart,
                        promotedExplicit[j].nameEnd,
                        &promotedExplicit[j].value))
                {
                    *outIsConst = 0;
                    return 0;
                }
            }
        }
        if (fieldBindings != NULL
            && HOPEvalAppendAggregateFieldBindings(
                   fieldBindings, fieldBindingCap, &fieldFrame, field)
                   != 0)
        {
            return ErrorSimple("out of memory");
        }
    }
    rc = HOPEvalFinalizeAggregateVarArrays(p, agg);
    if (rc != 1) {
        *outIsConst = 0;
        return rc < 0 ? -1 : 0;
    }
    *outValue = aggregateValue;
    *outIsConst = 1;
    return 0;
}

static int HOPEvalExecExprWithTypeNode(
    HOPEvalProgram*      p,
    int32_t              exprNode,
    const HOPParsedFile* typeFile,
    int32_t              typeNode,
    HOPCTFEValue*        outValue,
    int*                 outIsConst) {
    const HOPAst* ast;
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return -1;
    }
    if (ast->nodes[exprNode].kind == HOPAst_CALL_ARG) {
        exprNode = ast->nodes[exprNode].firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
    }
    if (typeFile != NULL && typeNode >= 0 && (uint32_t)typeNode < typeFile->ast.len
        && typeFile->ast.nodes[typeNode].kind == HOPAst_TYPE_TUPLE
        && ast->nodes[exprNode].kind == HOPAst_TUPLE_EXPR)
    {
        HOPCTFEValue elems[256];
        uint32_t     elemCount = AstListCount(ast, exprNode);
        uint32_t     i;
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
                || HOPEvalExecExprWithTypeNode(
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
        return HOPEvalAllocTupleValue(
            p, typeFile, typeNode, elems, elemCount, outValue, outIsConst);
    }
    if (ast->nodes[exprNode].kind == HOPAst_COMPOUND_LIT) {
        return HOPEvalEvalCompoundLiteral(
            p,
            exprNode,
            p->currentFile,
            typeFile != NULL ? typeFile : p->currentFile,
            typeNode,
            outValue,
            outIsConst);
    }
    if (ast->nodes[exprNode].kind == HOPAst_NEW) {
        return HOPEvalEvalNewExpr(p, exprNode, typeFile, typeNode, outValue, outIsConst);
    }
    if (ast->nodes[exprNode].kind == HOPAst_CALL && typeFile != NULL && typeNode >= 0) {
        const HOPParsedFile* savedExprFile = p->expectedCallExprFile;
        int32_t              savedExprNode = p->expectedCallExprNode;
        const HOPParsedFile* savedTypeFile = p->expectedCallTypeFile;
        int32_t              savedTypeNode = p->expectedCallTypeNode;
        const HOPParsedFile* savedActiveExpectedTypeFile = p->activeCallExpectedTypeFile;
        int32_t              savedActiveExpectedTypeNode = p->activeCallExpectedTypeNode;
        p->expectedCallExprFile = p->currentFile;
        p->expectedCallExprNode = exprNode;
        p->expectedCallTypeFile = typeFile;
        p->expectedCallTypeNode = typeNode;
        p->activeCallExpectedTypeFile = typeFile;
        p->activeCallExpectedTypeNode = typeNode;
        if (HOPEvalExecExprCb(p, exprNode, outValue, outIsConst) != 0) {
            p->expectedCallExprFile = savedExprFile;
            p->expectedCallExprNode = savedExprNode;
            p->expectedCallTypeFile = savedTypeFile;
            p->expectedCallTypeNode = savedTypeNode;
            p->activeCallExpectedTypeFile = savedActiveExpectedTypeFile;
            p->activeCallExpectedTypeNode = savedActiveExpectedTypeNode;
            return -1;
        }
        p->expectedCallExprFile = savedExprFile;
        p->expectedCallExprNode = savedExprNode;
        p->expectedCallTypeFile = savedTypeFile;
        p->expectedCallTypeNode = savedTypeNode;
        p->activeCallExpectedTypeFile = savedActiveExpectedTypeFile;
        p->activeCallExpectedTypeNode = savedActiveExpectedTypeNode;
    } else {
        if (HOPEvalExecExprCb(p, exprNode, outValue, outIsConst) != 0) {
            return -1;
        }
    }
    if (*outIsConst && typeFile != NULL && typeNode >= 0
        && HOPEvalCoerceValueToTypeNode(p, typeFile, typeNode, outValue) != 0)
    {
        return -1;
    }
    return 0;
}

static int HOPEvalExecExprCb(void* ctx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalAssignExprCb(
    void* ctx, HOPCTFEExecCtx* execCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalZeroInitCb(void* ctx, int32_t typeNode, HOPCTFEValue* outValue, int* outIsConst);
static int HOPEvalMirMakeAggregate(
    void*         ctx,
    uint32_t      sourceNode,
    uint32_t      fieldCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalMirZeroInitLocal(
    void*                ctx,
    const HOPMirTypeRef* typeRef,
    HOPCTFEValue*        outValue,
    int*                 outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalExecExprForTypeCb(
    void* ctx, int32_t exprNode, int32_t typeNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    if (p == NULL) {
        return -1;
    }
    return HOPEvalExecExprWithTypeNode(p, exprNode, p->currentFile, typeNode, outValue, outIsConst);
}
static int HOPEvalResolveIdent(
    void*         ctx,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalResolveCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalResolveCallMir(
    void* ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag);
static int HOPEvalEvalTopConst(
    HOPEvalProgram* p, uint32_t topConstIndex, HOPCTFEValue* outValue, int* outIsConst);

static HOPCTFEExecBinding* _Nullable HOPEvalFindBinding(
    const HOPCTFEExecCtx* _Nullable execCtx,
    const HOPParsedFile* file,
    uint32_t             nameStart,
    uint32_t             nameEnd) {
    const HOPCTFEExecEnv* frame;
    if (execCtx == NULL || file == NULL) {
        return NULL;
    }
    frame = execCtx->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            HOPCTFEExecBinding* binding;
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

static int HOPEvalTypeNodesEquivalent(
    const HOPParsedFile* aFile, int32_t aNode, const HOPParsedFile* bFile, int32_t bNode) {
    const HOPAstNode* a;
    const HOPAstNode* b;
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
        case HOPAst_TYPE_NAME:
            return SliceEqSlice(
                aFile->source, a->dataStart, a->dataEnd, bFile->source, b->dataStart, b->dataEnd);
        case HOPAst_TYPE_PTR:
        case HOPAst_TYPE_REF:
        case HOPAst_TYPE_MUTREF:
        case HOPAst_TYPE_OPTIONAL:
        case HOPAst_TYPE_SLICE:
        case HOPAst_TYPE_MUTSLICE:
        case HOPAst_TYPE_VARRAY:
            return HOPEvalTypeNodesEquivalent(aFile, a->firstChild, bFile, b->firstChild);
        case HOPAst_TYPE_ARRAY:
            return SliceEqSlice(
                       aFile->source,
                       a->dataStart,
                       a->dataEnd,
                       bFile->source,
                       b->dataStart,
                       b->dataEnd)
                && HOPEvalTypeNodesEquivalent(aFile, a->firstChild, bFile, b->firstChild);
        default: return 0;
    }
}

static int32_t HOPEvalResolveFunctionByTypeNodeLiteral(
    const HOPEvalProgram* p, const char* name, const HOPParsedFile* typeFile, int32_t typeNode) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || name == NULL || typeFile == NULL || typeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
        int32_t                paramTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = HOPEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0
            || !HOPEvalTypeNodesEquivalent(fn->file, paramTypeNode, typeFile, typeNode))
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

static int32_t HOPEvalResolvePointerFunctionByPointeeTypeLiteral(
    const HOPEvalProgram* p,
    const char*           name,
    const HOPParsedFile*  typeFile,
    int32_t               pointeeTypeNode) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || name == NULL || typeFile == NULL || pointeeTypeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
        int32_t                paramTypeNode;
        int32_t                childTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = HOPEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (childTypeNode < 0
            || !HOPEvalTypeNodesEquivalent(fn->file, childTypeNode, typeFile, pointeeTypeNode))
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

static int HOPEvalIsTypeNodeKind(HOPAstKind kind) {
    return kind == HOPAst_TYPE_NAME || kind == HOPAst_TYPE_PTR || kind == HOPAst_TYPE_REF
        || kind == HOPAst_TYPE_MUTREF || kind == HOPAst_TYPE_ARRAY || kind == HOPAst_TYPE_VARRAY
        || kind == HOPAst_TYPE_SLICE || kind == HOPAst_TYPE_MUTSLICE || kind == HOPAst_TYPE_OPTIONAL
        || kind == HOPAst_TYPE_FN || kind == HOPAst_TYPE_TUPLE || kind == HOPAst_TYPE_ANON_STRUCT
        || kind == HOPAst_TYPE_ANON_UNION;
}

static int HOPEvalFindVisibleLocalTypeNodeByName(
    const HOPParsedFile* file,
    uint32_t             beforePos,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    int32_t*             outTypeNode) {
    const HOPAst* ast;
    uint32_t      i;
    int32_t       found = -1;
    if (outTypeNode != NULL) {
        *outTypeNode = -1;
    }
    if (file == NULL || outTypeNode == NULL) {
        return 0;
    }
    ast = &file->ast;
    for (i = 0; i < ast->len; i++) {
        const HOPAstNode* n = &ast->nodes[i];
        int32_t           firstChild;
        int32_t           maybeTypeNode;
        int32_t           initNode;
        int               nameMatches = 0;
        if ((n->kind != HOPAst_VAR && n->kind != HOPAst_CONST) || n->start >= beforePos) {
            continue;
        }
        firstChild = n->firstChild;
        if (firstChild < 0 || (uint32_t)firstChild >= ast->len) {
            continue;
        }
        maybeTypeNode = -1;
        if (ast->nodes[firstChild].kind == HOPAst_NAME_LIST) {
            int32_t afterNames;
            int32_t nameNode = ast->nodes[firstChild].firstChild;
            while (nameNode >= 0) {
                if ((uint32_t)nameNode < ast->len && ast->nodes[nameNode].kind == HOPAst_IDENT
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
                && HOPEvalIsTypeNodeKind(ast->nodes[afterNames].kind))
            {
                maybeTypeNode = afterNames;
                initNode = ast->nodes[afterNames].nextSibling;
            } else {
                initNode = afterNames;
            }
        } else {
            nameMatches = SliceEqSlice(
                file->source, n->dataStart, n->dataEnd, file->source, nameStart, nameEnd);
            if (HOPEvalIsTypeNodeKind(ast->nodes[firstChild].kind)) {
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
            && ast->nodes[initNode].kind == HOPAst_COMPOUND_LIT)
        {
            int32_t typeNode = ast->nodes[initNode].firstChild;
            if (typeNode >= 0 && (uint32_t)typeNode < ast->len
                && HOPEvalIsTypeNodeKind(ast->nodes[typeNode].kind))
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

static int HOPEvalResolveAggregateDeclFromValue(
    const HOPEvalProgram* p,
    const HOPCTFEValue*   value,
    const HOPParsedFile** outFile,
    int32_t*              outNode) {
    const HOPCTFEValue* valueTarget;
    HOPEvalAggregate*   agg;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outNode != NULL) {
        *outNode = -1;
    }
    if (p == NULL || value == NULL || outFile == NULL || outNode == NULL) {
        return 0;
    }
    valueTarget = HOPEvalValueTargetOrSelf(value);
    agg = HOPEvalValueAsAggregate(valueTarget);
    if (agg == NULL || agg->file == NULL || agg->nodeId < 0) {
        return 0;
    }
    if ((uint32_t)agg->nodeId < agg->file->ast.len) {
        const HOPAstNode* aggNode = &agg->file->ast.nodes[agg->nodeId];
        if (aggNode->kind == HOPAst_COMPOUND_LIT) {
            int32_t              typeNode = aggNode->firstChild;
            const HOPParsedFile* declFile = NULL;
            int32_t              declNode = -1;
            if (typeNode >= 0
                && HOPEvalResolveAggregateTypeNode(p, agg->file, typeNode, &declFile, &declNode))
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

static int32_t HOPEvalResolvePointerAggregateFunctionByLiteral(
    const HOPEvalProgram* p, const char* name, const HOPCTFEValue* argValue) {
    const HOPCTFEValue*  targetValue;
    HOPEvalAggregate*    agg;
    const HOPParsedFile* aggDeclFile = NULL;
    int32_t              aggDeclNode = -1;
    uint32_t             i;
    int32_t              found = -1;
    if (p == NULL || name == NULL || argValue == NULL) {
        return -1;
    }
    targetValue = HOPEvalValueTargetOrSelf(argValue);
    agg = HOPEvalValueAsAggregate(targetValue);
    if (agg == NULL) {
        return HOPEvalResolveFunctionByLiteralArgs(p, name, argValue, 1);
    }
    if (!HOPEvalResolveAggregateDeclFromValue(p, argValue, &aggDeclFile, &aggDeclNode)) {
        aggDeclFile = agg->file;
        aggDeclNode = agg->nodeId;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
        int32_t                paramTypeNode;
        int32_t                childTypeNode;
        const HOPParsedFile*   declFile = NULL;
        int32_t                declNode = -1;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = HOPEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (!HOPEvalResolveAggregateTypeNode(p, fn->file, childTypeNode, &declFile, &declNode)
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

static int32_t HOPEvalResolveFunctionBySourceExprLiteral(
    HOPEvalProgram*      p,
    HOPCTFEExecCtx*      execCtx,
    const HOPParsedFile* file,
    int32_t              exprNode,
    const char*          name) {
    const HOPAstNode* expr;
    int32_t           bindingTypeNode = -1;
    if (p == NULL || execCtx == NULL || file == NULL || exprNode < 0
        || (uint32_t)exprNode >= file->ast.len || name == NULL)
    {
        return -1;
    }
    expr = &file->ast.nodes[exprNode];
    if (expr->kind == HOPAst_IDENT) {
        HOPCTFEExecBinding* binding = HOPEvalFindBinding(
            execCtx, file, expr->dataStart, expr->dataEnd);
        if (binding != NULL && binding->typeNode >= 0) {
            return HOPEvalResolveFunctionByTypeNodeLiteral(p, name, file, binding->typeNode);
        }
        if (HOPEvalFindVisibleLocalTypeNodeByName(
                file, expr->start, expr->dataStart, expr->dataEnd, &bindingTypeNode))
        {
            return HOPEvalResolveFunctionByTypeNodeLiteral(p, name, file, bindingTypeNode);
        }
        return -1;
    }
    if (expr->kind == HOPAst_UNARY && (HOPTokenKind)expr->op == HOPTok_AND) {
        int32_t childNode = expr->firstChild;
        if (childNode >= 0 && (uint32_t)childNode < file->ast.len
            && file->ast.nodes[childNode].kind == HOPAst_IDENT)
        {
            HOPCTFEExecBinding* binding = HOPEvalFindBinding(
                execCtx,
                file,
                file->ast.nodes[childNode].dataStart,
                file->ast.nodes[childNode].dataEnd);
            if (binding != NULL && binding->typeNode >= 0) {
                return HOPEvalResolvePointerFunctionByPointeeTypeLiteral(
                    p, name, file, binding->typeNode);
            }
            if (HOPEvalFindVisibleLocalTypeNodeByName(
                    file,
                    expr->start,
                    file->ast.nodes[childNode].dataStart,
                    file->ast.nodes[childNode].dataEnd,
                    &bindingTypeNode))
            {
                return HOPEvalResolvePointerFunctionByPointeeTypeLiteral(
                    p, name, file, bindingTypeNode);
            }
        }
    }
    return -1;
}

static int32_t HOPEvalResolveIteratorHookByReturnType(
    const HOPEvalProgram* p, const char* name, int32_t iteratorFnIndex) {
    const HOPEvalFunction* iteratorFn;
    int32_t                iteratorTypeNode;
    uint32_t               i;
    int32_t                found = -1;
    if (p == NULL || name == NULL || iteratorFnIndex < 0 || (uint32_t)iteratorFnIndex >= p->funcLen)
    {
        return -1;
    }
    iteratorFn = &p->funcs[iteratorFnIndex];
    iteratorTypeNode = HOPEvalFunctionReturnTypeNode(iteratorFn);
    if (iteratorTypeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const HOPEvalFunction* fn = &p->funcs[i];
        int32_t                paramTypeNode;
        int32_t                childTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = HOPEvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != HOPAst_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (!HOPEvalTypeNodesEquivalent(
                fn->file, childTypeNode, iteratorFn->file, iteratorTypeNode))
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

static void HOPEvalAdaptForInValueBinding(
    const HOPCTFEValue* inValue, int valueRef, HOPCTFEValue* outValue) {
    const HOPCTFEValue* target;
    if (outValue == NULL) {
        return;
    }
    if (inValue == NULL) {
        HOPEvalValueSetNull(outValue);
        return;
    }
    if (valueRef) {
        *outValue = *inValue;
        return;
    }
    target = HOPEvalValueReferenceTarget(inValue);
    *outValue = target != NULL ? *target : *inValue;
}

static int32_t HOPEvalResolveForInIteratorFn(
    HOPEvalProgram*     p,
    HOPCTFEExecCtx*     execCtx,
    int32_t             sourceNode,
    const HOPCTFEValue* sourceValue) {
    HOPCTFEValue         sourceArg;
    const HOPParsedFile* sourceTypeFile = NULL;
    int32_t              sourceTypeNode = -1;
    int32_t              iteratorFn = -1;
    if (p == NULL || execCtx == NULL || sourceValue == NULL) {
        return -1;
    }
    sourceArg = *sourceValue;
    if (p->currentFile != NULL) {
        iteratorFn = HOPEvalResolveFunctionBySourceExprLiteral(
            p, execCtx, p->currentFile, sourceNode, "__iterator");
        if (sourceNode >= 0 && (uint32_t)sourceNode < p->currentFile->ast.len
            && p->currentFile->ast.nodes[sourceNode].kind == HOPAst_IDENT)
        {
            HOPCTFEExecBinding* binding = HOPEvalFindBinding(
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
        iteratorFn = HOPEvalResolveFunctionByTypeNodeLiteral(
            p, "__iterator", sourceTypeFile, sourceTypeNode);
    }
    if (iteratorFn < 0) {
        iteratorFn = HOPEvalResolvePointerAggregateFunctionByLiteral(p, "__iterator", sourceValue);
    }
    if (iteratorFn < 0) {
        iteratorFn = HOPEvalResolveFunctionByLiteralArgs(p, "__iterator", &sourceArg, 1);
    }
    return iteratorFn;
}

static int HOPEvalAdvanceForInIterator(
    HOPEvalProgram* p,
    HOPCTFEExecCtx* execCtx,
    int32_t         iteratorFn,
    HOPCTFEValue*   iteratorValue,
    int             hasKey,
    int             keyRef,
    int             valueRef,
    int             valueDiscard,
    int*            outHasItem,
    HOPCTFEValue*   outKey,
    int*            outKeyIsConst,
    HOPCTFEValue*   outValue,
    int*            outValueIsConst) {
    HOPCTFEValue        iterRef;
    HOPCTFEValue        callResult;
    const HOPCTFEValue* payload = NULL;
    int32_t             nextFn = -1;
    int32_t             nextReturnTypeNode = -1;
    int                 didReturn = 0;
    int                 usePair = 0;
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
        HOPEvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        HOPEvalValueSetNull(outValue);
    }
    if (p == NULL || execCtx == NULL || iteratorValue == NULL || outHasItem == NULL
        || outKey == NULL || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    if (keyRef) {
        HOPCTFEExecSetReason(
            execCtx, 0, 0, "for-in key reference is not supported in evaluator backend");
        return 0;
    }
    HOPEvalValueSetReference(&iterRef, iteratorValue);
    if (hasKey) {
        if (!valueDiscard) {
            nextFn = HOPEvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
            usePair = 1;
        } else {
            nextFn = HOPEvalResolveIteratorHookByReturnType(p, "next_key", iteratorFn);
            usePair = 0;
            if (nextFn < 0) {
                nextFn = HOPEvalResolveIteratorHookByReturnType(
                    p, "next_key_and_value", iteratorFn);
                usePair = nextFn >= 0 ? 1 : 0;
            }
        }
    } else {
        nextFn = HOPEvalResolveIteratorHookByReturnType(p, "next_value", iteratorFn);
        usePair = 0;
        if (nextFn < 0) {
            nextFn = HOPEvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
            usePair = nextFn >= 0 ? 1 : 0;
        }
    }
    if (nextFn < 0) {
        HOPCTFEExecSetReason(
            execCtx, 0, 0, "for-in iterator hooks are not supported in evaluator backend");
        return 0;
    }
    nextReturnTypeNode = HOPEvalFunctionReturnTypeNode(&p->funcs[nextFn]);
    if (nextReturnTypeNode < 0) {
        HOPCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook has unsupported return type");
        return 0;
    }
    if (HOPEvalInvokeFunction(p, nextFn, &iterRef, 1, p->currentContext, &callResult, &didReturn)
        != 0)
    {
        return -1;
    }
    if (!didReturn) {
        HOPCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook returned unsupported value");
        return 0;
    }
    if (p->funcs[nextFn].file->ast.nodes[nextReturnTypeNode].kind == HOPAst_TYPE_OPTIONAL) {
        if (callResult.kind == HOPCTFEValue_OPTIONAL) {
            if (!HOPEvalOptionalPayload(&callResult, &payload)) {
                HOPCTFEExecSetReason(
                    execCtx, 0, 0, "for-in iterator hook returned unsupported value");
                return 0;
            }
            if (callResult.b == 0u || payload == NULL) {
                *outHasItem = 0;
                *outKeyIsConst = 1;
                *outValueIsConst = 1;
                return 0;
            }
        } else if (callResult.kind == HOPCTFEValue_NULL) {
            *outHasItem = 0;
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        } else {
            payload = &callResult;
        }
    } else {
        HOPCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook has unsupported return type");
        return 0;
    }
    if (payload == NULL) {
        *outHasItem = 0;
        *outKeyIsConst = 1;
        *outValueIsConst = 1;
        return 0;
    }
    if (usePair) {
        const HOPCTFEValue* pairValue = HOPEvalValueTargetOrSelf(payload);
        HOPEvalArray*       tuple = HOPEvalValueAsArray(pairValue);
        if (tuple == NULL || tuple->len != 2u) {
            HOPCTFEExecSetReason(execCtx, 0, 0, "for-in pair iterator returned malformed tuple");
            return 0;
        }
        if (hasKey) {
            *outKey = tuple->elems[0];
            *outKeyIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
        if (!valueDiscard) {
            HOPEvalAdaptForInValueBinding(&tuple->elems[1], valueRef, outValue);
            *outValueIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
    } else if (hasKey) {
        *outKey = *payload;
        *outKeyIsConst = 1;
        *outValueIsConst = 1;
    } else {
        HOPEvalAdaptForInValueBinding(payload, valueRef, outValue);
        *outValueIsConst = 1;
        *outKeyIsConst = 1;
    }
    return 0;
}

static int HOPEvalForInIterCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    int32_t             sourceNode,
    const HOPCTFEValue* sourceValue,
    uint32_t            index,
    int                 hasKey,
    int                 keyRef,
    int                 valueRef,
    int                 valueDiscard,
    int*                outHasItem,
    HOPCTFEValue*       outKey,
    int*                outKeyIsConst,
    HOPCTFEValue*       outValue,
    int*                outValueIsConst) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    HOPCTFEValue    iterValue;
    int32_t         iteratorFn = -1;
    uint32_t        step;
    int             didReturn = 0;
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
        HOPEvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        HOPEvalValueSetNull(outValue);
    }
    if (p == NULL || execCtx == NULL || sourceValue == NULL || outHasItem == NULL || outKey == NULL
        || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    if (keyRef) {
        HOPCTFEExecSetReason(
            execCtx, 0, 0, "for-in key reference is not supported in evaluator backend");
        return 0;
    }
    iteratorFn = HOPEvalResolveForInIteratorFn(p, execCtx, sourceNode, sourceValue);
    if (iteratorFn < 0) {
        HOPCTFEExecSetReason(
            execCtx, 0, 0, "for-in loop source is not supported in evaluator backend");
        return 0;
    }
    if (HOPEvalInvokeFunction(
            p, iteratorFn, sourceValue, 1, p->currentContext, &iterValue, &didReturn)
        != 0)
    {
        return -1;
    }
    if (!didReturn) {
        HOPCTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook did not return a value");
        return 0;
    }
    for (step = 0; step <= index; step++) {
        if (HOPEvalAdvanceForInIterator(
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

static int HOPEvalZeroInitCb(void* ctx, int32_t typeNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL) {
        return -1;
    }
    return HOPEvalZeroInitTypeNode(p, p->currentFile, typeNode, outValue, outIsConst);
}

static int HOPEvalMirMakeAggregate(
    void*         ctx,
    uint32_t      sourceNode,
    uint32_t      fieldCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*   p = (HOPEvalProgram*)ctx;
    HOPEvalAggregate* agg;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        if (diag != NULL) {
            diag->code = HOPDiag_UNEXPECTED_TOKEN;
            diag->type = HOPDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    agg = (HOPEvalAggregate*)HOPArenaAlloc(
        p->arena, sizeof(HOPEvalAggregate), (uint32_t)_Alignof(HOPEvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = p->currentFile;
    agg->nodeId = (int32_t)sourceNode;
    if (sourceNode < p->currentFile->ast.len) {
        const HOPAstNode*    sourceAst = &p->currentFile->ast.nodes[sourceNode];
        const HOPParsedFile* declFile = NULL;
        int32_t              declNode = -1;
        if (sourceAst->kind == HOPAst_COMPOUND_LIT && sourceAst->firstChild >= 0
            && HOPEvalResolveAggregateTypeNode(
                p, p->currentFile, sourceAst->firstChild, &declFile, &declNode))
        {
            agg->file = declFile;
            agg->nodeId = declNode;
        }
    }
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (HOPEvalAggregateField*)HOPArenaAlloc(
            p->arena,
            sizeof(HOPEvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(HOPEvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(HOPEvalAggregateField) * fieldCount);
        {
            uint32_t i;
            for (i = 0; i < fieldCount; i++) {
                agg->fields[i].typeNode = -1;
                agg->fields[i].defaultExprNode = -1;
            }
        }
    }
    HOPEvalValueSetAggregate(outValue, agg->file, agg->nodeId, agg);
    outValue->typeTag |= HOPCTFEValueTag_AGG_PARTIAL;
    *outIsConst = 1;
    return 0;
}

static int HOPEvalMirFindCallNodeBySpan(
    const HOPParsedFile* file,
    uint32_t             callStart,
    uint32_t             callEnd,
    uint32_t             argCount,
    int32_t*             outCallNode) {
    uint32_t i;
    int32_t  found = -1;
    if (outCallNode != NULL) {
        *outCallNode = -1;
    }
    if (file == NULL || outCallNode == NULL) {
        return 0;
    }
    for (i = 0; i < file->ast.len; i++) {
        const HOPAstNode* call = &file->ast.nodes[i];
        const HOPAstNode* callee;
        int32_t           calleeNode;
        uint32_t          curArgCount = 0;
        int32_t           argNode;
        uint32_t          curStart;
        uint32_t          curEnd;
        if (call->kind != HOPAst_CALL) {
            continue;
        }
        calleeNode = call->firstChild;
        if (calleeNode < 0 || (uint32_t)calleeNode >= file->ast.len) {
            continue;
        }
        callee = &file->ast.nodes[calleeNode];
        if (callee->kind != HOPAst_IDENT && callee->kind != HOPAst_FIELD_EXPR) {
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

static int HOPEvalMirAdjustCallArgs(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t         calleeFunctionIndex,
    HOPMirExecValue* args,
    uint32_t         argCount,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*        p = (HOPEvalProgram*)ctx;
    HOPEvalMirExecCtx*     execCtx;
    uint32_t               evalFnIndex;
    uint32_t               receiverArgCount;
    int32_t                callNode = -1;
    int32_t                calleeNode;
    int32_t                argNode;
    int32_t                firstArgNode;
    const HOPEvalFunction* calleeFn;
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
    receiverArgCount = HOPMirCallTokDropsReceiverArg0(inst->tok) ? 1u : 0u;
    if (argCount < receiverArgCount) {
        return 0;
    }
    if (!HOPEvalMirFindCallNodeBySpan(
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
            const HOPAstNode* arg = &p->currentFile->ast.nodes[argNode];
            int32_t           exprNode = argNode;
            int32_t           paramTypeNode;
            int32_t           paramIndex = (int32_t)(argIndex + receiverArgCount);
            if (arg->kind == HOPAst_CALL_ARG) {
                if ((arg->flags & HOPAstFlag_CALL_ARG_SPREAD) != 0) {
                    break;
                }
                exprNode = arg->firstChild;
                if (arg->dataEnd > arg->dataStart) {
                    paramIndex = HOPEvalFunctionParamIndexByName(
                        calleeFn, p->currentFile->source, arg->dataStart, arg->dataEnd);
                    if (paramIndex < 0) {
                        break;
                    }
                }
            }
            if (exprNode < 0 || (uint32_t)exprNode >= p->currentFile->ast.len) {
                break;
            }
            paramTypeNode = HOPEvalFunctionParamTypeNodeAt(calleeFn, (uint32_t)paramIndex);
            if (paramTypeNode >= 0
                && HOPEvalExprIsAnytypePackIndex(p, &p->currentFile->ast, exprNode)
                && !HOPEvalValueMatchesExpectedTypeNode(
                    p, calleeFn->file, paramTypeNode, &args[argIndex + receiverArgCount]))
            {
                if (p->currentExecCtx != NULL) {
                    HOPCTFEExecSetReasonNode(
                        p->currentExecCtx, exprNode, "anytype pack element type mismatch");
                }
                return 1;
            }
            argIndex++;
            argNode = p->currentFile->ast.nodes[argNode].nextSibling;
        }
    }
    (void)HOPEvalReorderFixedCallArgsByName(
        p,
        calleeFn,
        &p->currentFile->ast,
        firstArgNode,
        args + receiverArgCount,
        argCount - receiverArgCount,
        receiverArgCount);
    if (program != NULL && calleeFunctionIndex < program->funcLen
        && !p->currentMirExecCtx->hasPendingTemplateBinding)
    {
        const HOPMirFunction*       calleeMirFn = &program->funcs[calleeFunctionIndex];
        HOPEvalTemplateBindingState savedBinding;
        uint32_t                    directArgCount = argCount - receiverArgCount;
        if ((calleeMirFn->flags & HOPMirFunctionFlag_VARIADIC) == 0u
            && directArgCount == calleeMirFn->paramCount)
        {
            HOPEvalSaveTemplateBinding(p, &savedBinding);
            if (HOPEvalBindActiveTemplateForMirCall(
                    p, program, function, inst, calleeFn, args + receiverArgCount, directArgCount))
            {
                p->currentMirExecCtx->pendingTemplateBinding = savedBinding;
                p->currentMirExecCtx->hasPendingTemplateBinding = 1u;
            }
        }
    }
    return 0;
}

static int HOPEvalMirAssignIdent(
    void*               ctx,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const HOPCTFEValue* inValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    int32_t         topVarIndex;
    HOPCTFEValue    value;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || inValue == NULL || outIsConst == NULL) {
        return -1;
    }
    topVarIndex = HOPEvalFindCurrentTopVarBySlice(p, p->currentFile, nameStart, nameEnd);
    if (topVarIndex < 0) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "identifier assignment is not supported by evaluator backend");
        }
        return 0;
    }
    value = *inValue;
    if (p->topVars[(uint32_t)topVarIndex].declTypeNode >= 0
        && HOPEvalCoerceValueToTypeNode(
               p,
               p->topVars[(uint32_t)topVarIndex].file,
               p->topVars[(uint32_t)topVarIndex].declTypeNode,
               &value)
               != 0)
    {
        return -1;
    }
    p->topVars[(uint32_t)topVarIndex].value = value;
    p->topVars[(uint32_t)topVarIndex].state = HOPEvalTopConstState_READY;
    *outIsConst = 1;
    return 0;
}

static int HOPEvalMirZeroInitLocal(
    void*                ctx,
    const HOPMirTypeRef* typeRef,
    HOPCTFEValue*        outValue,
    int*                 outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL || typeRef == NULL || typeRef->astNode == UINT32_MAX
        || typeRef->astNode >= p->currentFile->ast.len)
    {
        if (diag != NULL) {
            diag->code = HOPDiag_UNEXPECTED_TOKEN;
            diag->type = HOPDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    return HOPEvalZeroInitTypeNode(
        p, p->currentFile, (int32_t)typeRef->astNode, outValue, outIsConst);
}

static int HOPEvalMirCoerceValueForType(
    void* ctx, const HOPMirTypeRef* typeRef, HOPCTFEValue* inOutValue, HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL || typeRef == NULL || inOutValue == NULL
        || typeRef->astNode == UINT32_MAX || typeRef->astNode >= p->currentFile->ast.len)
    {
        if (diag != NULL) {
            diag->code = HOPDiag_UNEXPECTED_TOKEN;
            diag->type = HOPDiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    return HOPEvalCoerceValueToTypeNode(p, p->currentFile, (int32_t)typeRef->astNode, inOutValue);
}

static int HOPEvalMirIndexValue(
    void*               ctx,
    const HOPCTFEValue* base,
    const HOPCTFEValue* index,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*     p = (HOPEvalProgram*)ctx;
    const HOPCTFEValue* baseValue;
    HOPEvalArray*       array;
    int64_t             indexInt = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = HOPEvalValueTargetOrSelf(base);
    if (HOPCTFEValueToInt64(index, &indexInt) != 0 || indexInt < 0) {
        return 0;
    }
    array = HOPEvalValueAsArray(baseValue);
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

static int HOPEvalMirIndexAddr(
    void*               ctx,
    const HOPCTFEValue* base,
    const HOPCTFEValue* index,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*     p = (HOPEvalProgram*)ctx;
    const HOPCTFEValue* baseValue;
    HOPEvalArray*       array;
    int64_t             indexInt = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = HOPEvalValueTargetOrSelf(base);
    if (HOPCTFEValueToInt64(index, &indexInt) != 0 || indexInt < 0) {
        return 0;
    }
    array = HOPEvalValueAsArray(baseValue);
    if (array == NULL || (uint64_t)indexInt >= (uint64_t)array->len) {
        HOPCTFEValue* targetValue = HOPEvalValueReferenceTarget(base);
        int32_t       baseTypeCode = HOPEvalTypeCode_INVALID;
        if (targetValue == NULL) {
            targetValue = (HOPCTFEValue*)baseValue;
        }
        if (targetValue != NULL && targetValue->kind == HOPCTFEValue_STRING
            && HOPEvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
            && baseTypeCode == HOPEvalTypeCode_STR_PTR
            && (uint64_t)indexInt < (uint64_t)targetValue->s.len)
        {
            HOPCTFEValue* byteProxy = (HOPCTFEValue*)HOPArenaAlloc(
                p->arena, sizeof(HOPCTFEValue), (uint32_t)_Alignof(HOPCTFEValue));
            if (byteProxy == NULL) {
                return ErrorSimple("out of memory");
            }
            HOPMirValueSetByteRefProxy(byteProxy, (uint8_t*)targetValue->s.bytes + indexInt);
            HOPEvalValueSetRuntimeTypeCode(byteProxy, HOPEvalTypeCode_U8);
            HOPEvalValueSetReference(outValue, byteProxy);
            *outIsConst = 1;
            return 0;
        }
        return 0;
    }
    HOPEvalValueSetReference(outValue, &array->elems[(uint32_t)indexInt]);
    *outIsConst = 1;
    return 0;
}

static int HOPEvalMirSliceValue(
    void* _Nullable ctx,
    const HOPCTFEValue* _Nonnull base,
    const HOPCTFEValue* _Nullable start,
    const HOPCTFEValue* _Nullable end,
    uint16_t flags,
    HOPCTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*     p = (HOPEvalProgram*)ctx;
    const HOPCTFEValue* baseValue;
    HOPEvalArray*       array;
    HOPEvalArray*       view;
    int64_t             startInt = 0;
    int64_t             endInt = -1;
    uint32_t            startIndex = 0;
    uint32_t            endIndex = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = HOPEvalValueTargetOrSelf(base);
    if ((flags & HOPAstFlag_INDEX_HAS_START) != 0u) {
        if (start == NULL || HOPCTFEValueToInt64(start, &startInt) != 0 || startInt < 0) {
            return 0;
        }
    }
    if ((flags & HOPAstFlag_INDEX_HAS_END) != 0u) {
        if (end == NULL || HOPCTFEValueToInt64(end, &endInt) != 0 || endInt < 0) {
            return 0;
        }
    }
    if (baseValue->kind == HOPCTFEValue_STRING) {
        int32_t currentTypeCode = HOPEvalTypeCode_INVALID;
        startIndex = (uint32_t)startInt;
        endIndex = endInt >= 0 ? (uint32_t)endInt : baseValue->s.len;
        if (startIndex > endIndex || endIndex > baseValue->s.len) {
            return 0;
        }
        *outValue = *baseValue;
        outValue->s.bytes = baseValue->s.bytes != NULL ? baseValue->s.bytes + startIndex : NULL;
        outValue->s.len = endIndex - startIndex;
        if (!HOPEvalValueGetRuntimeTypeCode(baseValue, &currentTypeCode)) {
            currentTypeCode = HOPEvalTypeCode_STR_REF;
        }
        HOPEvalValueSetRuntimeTypeCode(outValue, currentTypeCode);
        *outIsConst = 1;
        return 0;
    }
    array = HOPEvalValueAsArray(baseValue);
    if (array == NULL) {
        return 0;
    }
    startIndex = (uint32_t)startInt;
    endIndex = endInt >= 0 ? (uint32_t)endInt : array->len;
    if (startIndex > endIndex || endIndex > array->len) {
        return 0;
    }
    view = HOPEvalAllocArrayView(
        p,
        baseValue->kind == HOPCTFEValue_ARRAY ? array->file : p->currentFile,
        array->typeNode,
        array->elemTypeNode,
        array->elems + startIndex,
        endIndex - startIndex);
    if (view == NULL) {
        return ErrorSimple("out of memory");
    }
    {
        HOPCTFEValue viewValue;
        HOPEvalValueSetArray(&viewValue, p->currentFile, array->typeNode, view);
        return HOPEvalAllocReferencedValue(p, &viewValue, outValue, outIsConst);
    }
}

static int HOPEvalMirSequenceLen(
    void*               ctx,
    const HOPCTFEValue* base,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*     p = (HOPEvalProgram*)ctx;
    const HOPCTFEValue* baseValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = HOPEvalValueTargetOrSelf(base);
    if (baseValue->kind != HOPCTFEValue_STRING && baseValue->kind != HOPCTFEValue_ARRAY
        && baseValue->kind != HOPCTFEValue_NULL)
    {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReason(
                p->currentExecCtx, 0, 0, "len argument is not supported by evaluator backend");
        }
        return 0;
    }
    HOPEvalValueSetInt(outValue, (int64_t)baseValue->s.len);
    *outIsConst = 1;
    return 0;
}

static int HOPEvalMirMakeTuple(
    void*               ctx,
    const HOPCTFEValue* elems,
    uint32_t            elemCount,
    uint32_t            typeNodeHint,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*      p = (HOPEvalProgram*)ctx;
    const HOPParsedFile* file;
    int32_t              typeNode = -1;
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
    return HOPEvalAllocTupleValue(p, file, typeNode, elems, elemCount, outValue, outIsConst);
}

static int HOPEvalMirMakeVariadicPack(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirTypeRef* _Nullable paramTypeRef,
    uint16_t            callFlags,
    const HOPCTFEValue* args,
    uint32_t            argCount,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*      p = (HOPEvalProgram*)ctx;
    const HOPParsedFile* file;
    const HOPEvalArray*  spreadArray = NULL;
    HOPEvalArray*        packArray;
    int32_t              typeNode = -1;
    uint32_t             packLen = argCount;
    uint32_t             prefixCount = argCount;
    uint32_t             i;
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
    if (HOPMirCallTokHasSpreadLast(callFlags)) {
        if (argCount == 0u) {
            return 0;
        }
        spreadArray = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&args[argCount - 1u]));
        if (spreadArray == NULL || spreadArray->len > UINT32_MAX - (argCount - 1u)) {
            return 0;
        }
        prefixCount = argCount - 1u;
        packLen = prefixCount + spreadArray->len;
    }
    packArray = HOPEvalAllocArrayView(p, file, typeNode, typeNode, NULL, packLen);
    if (packArray == NULL) {
        return ErrorSimple("out of memory");
    }
    if (packLen > 0u) {
        packArray->elems = (HOPCTFEValue*)HOPArenaAlloc(
            p->arena, sizeof(HOPCTFEValue) * packLen, (uint32_t)_Alignof(HOPCTFEValue));
        if (packArray->elems == NULL) {
            return ErrorSimple("out of memory");
        }
        for (i = 0; i < prefixCount; i++) {
            packArray->elems[i] = args[i];
            HOPEvalAnnotateUntypedLiteralValue(&packArray->elems[i]);
        }
        if (spreadArray != NULL) {
            uint32_t j;
            for (j = 0; j < spreadArray->len; j++) {
                packArray->elems[prefixCount + j] = spreadArray->elems[j];
                HOPEvalAnnotateUntypedLiteralValue(&packArray->elems[prefixCount + j]);
            }
        } else {
            for (; i < packLen; i++) {
                packArray->elems[i] = args[i];
                HOPEvalAnnotateUntypedLiteralValue(&packArray->elems[i]);
            }
        }
    }
    HOPEvalValueSetArray(outValue, file, typeNode, packArray);
    *outIsConst = 1;
    return 0;
}

static HOPEvalMirIteratorState* _Nullable HOPEvalMirIteratorStateFromValue(
    const HOPCTFEValue* iterValue) {
    HOPCTFEValue* target;
    if (iterValue == NULL) {
        return NULL;
    }
    target = HOPEvalValueReferenceTarget(iterValue);
    if (target == NULL || target->kind != HOPCTFEValue_SPAN
        || target->typeTag != HOP_EVAL_MIR_ITER_MAGIC || target->s.bytes == NULL)
    {
        return NULL;
    }
    return (HOPEvalMirIteratorState*)target->s.bytes;
}

static int HOPEvalMirIterInit(
    void*               ctx,
    uint32_t            sourceNode,
    const HOPCTFEValue* source,
    uint16_t            flags,
    HOPCTFEValue*       outIter,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*          p = (HOPEvalProgram*)ctx;
    HOPEvalMirIteratorState* state;
    HOPCTFEValue*            target;
    const HOPCTFEValue*      sourceValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || source == NULL || outIter == NULL || outIsConst == NULL) {
        return -1;
    }
    if ((flags & HOPMirIterFlag_KEY_REF) != 0u) {
        return 0;
    }
    state = (HOPEvalMirIteratorState*)HOPArenaAlloc(
        p->arena, sizeof(*state), (uint32_t)_Alignof(HOPEvalMirIteratorState));
    target = (HOPCTFEValue*)HOPArenaAlloc(
        p->arena, sizeof(*target), (uint32_t)_Alignof(HOPCTFEValue));
    if (state == NULL || target == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(state, 0, sizeof(*state));
    state->magic = HOP_EVAL_MIR_ITER_MAGIC;
    state->sourceNode = sourceNode;
    state->index = 0;
    state->iteratorFn = -1;
    state->flags = flags;
    state->sourceValue = *source;
    state->iteratorValue = (HOPCTFEValue){ .kind = HOPCTFEValue_INVALID };
    sourceValue = HOPEvalValueTargetOrSelf(source);
    if (sourceValue->kind == HOPCTFEValue_ARRAY || sourceValue->kind == HOPCTFEValue_STRING
        || sourceValue->kind == HOPCTFEValue_NULL)
    {
        state->kind = HOP_EVAL_MIR_ITER_KIND_SEQUENCE;
    } else {
        int didReturn = 0;
        state->kind = HOP_EVAL_MIR_ITER_KIND_PROTOCOL;
        if (p->currentExecCtx == NULL) {
            return 0;
        }
        state->iteratorFn = HOPEvalResolveForInIteratorFn(
            p, p->currentExecCtx, (int32_t)sourceNode, source);
        if (state->iteratorFn < 0) {
            HOPCTFEExecSetReason(
                p->currentExecCtx,
                0,
                0,
                "for-in loop source is not supported in evaluator backend");
            return 0;
        }
        if (HOPEvalInvokeFunction(
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
            HOPCTFEExecSetReason(
                p->currentExecCtx, 0, 0, "for-in iterator hook did not return a value");
            return 0;
        }
    }
    target->kind = HOPCTFEValue_SPAN;
    target->i64 = 0;
    target->f64 = 0.0;
    target->b = 0;
    target->typeTag = HOP_EVAL_MIR_ITER_MAGIC;
    target->s.bytes = (const uint8_t*)state;
    target->s.len = 0;
    target->span = (HOPCTFESpan){ 0 };
    HOPEvalValueSetReference(outIter, target);
    *outIsConst = 1;
    return 0;
}

static int HOPEvalMirIterNext(
    void*               ctx,
    const HOPCTFEValue* iter,
    uint16_t            flags,
    int*                outHasItem,
    HOPCTFEValue*       outKey,
    int*                outKeyIsConst,
    HOPCTFEValue*       outValue,
    int*                outValueIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*          p = (HOPEvalProgram*)ctx;
    HOPEvalMirIteratorState* state;
    const HOPCTFEValue*      sourceValue;
    int                      hasKey;
    int                      keyRef;
    int                      valueRef;
    int                      valueDiscard;
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
        HOPEvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        HOPEvalValueSetNull(outValue);
    }
    if (p == NULL || iter == NULL || outHasItem == NULL || outKey == NULL || outKeyIsConst == NULL
        || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    state = HOPEvalMirIteratorStateFromValue(iter);
    if (state == NULL) {
        return 0;
    }
    hasKey = (flags & HOPMirIterFlag_HAS_KEY) != 0u;
    keyRef = (flags & HOPMirIterFlag_KEY_REF) != 0u;
    valueRef = (flags & HOPMirIterFlag_VALUE_REF) != 0u;
    valueDiscard = (flags & HOPMirIterFlag_VALUE_DISCARD) != 0u;
    if (state->kind == HOP_EVAL_MIR_ITER_KIND_SEQUENCE) {
        sourceValue = HOPEvalValueTargetOrSelf(&state->sourceValue);
        if (sourceValue->kind != HOPCTFEValue_ARRAY && sourceValue->kind != HOPCTFEValue_STRING
            && sourceValue->kind != HOPCTFEValue_NULL)
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
            HOPEvalValueSetInt(outKey, (int64_t)state->index);
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if (!valueDiscard) {
            if (p->currentExecCtx == NULL) {
                return 0;
            }
            if (HOPEvalForInIndexCb(
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
    if (state->kind == HOP_EVAL_MIR_ITER_KIND_PROTOCOL) {
        if (p->currentExecCtx == NULL) {
            return 0;
        }
        if (HOPEvalAdvanceForInIterator(
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

static int HOPEvalMirAggGetField(
    void*               ctx,
    const HOPCTFEValue* base,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*       p = (HOPEvalProgram*)ctx;
    const HOPCTFEValue*   baseValue;
    const HOPCTFEValue*   payload = NULL;
    HOPEvalAggregate*     agg = NULL;
    HOPEvalReflectedType* rt;
    HOPEvalTaggedEnum*    tagged;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || base == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    baseValue = HOPEvalValueTargetOrSelf(base);
    tagged = HOPEvalValueAsTaggedEnum(baseValue);
    if (tagged != NULL && tagged->payload != NULL
        && HOPEvalAggregateGetFieldValue(
            tagged->payload, p->currentFile->source, nameStart, nameEnd, outValue))
    {
        *outIsConst = 1;
        return 0;
    }
    if (baseValue->kind == HOPCTFEValue_OPTIONAL && HOPEvalOptionalPayload(baseValue, &payload)
        && baseValue->b != 0u && payload != NULL)
    {
        baseValue = HOPEvalValueTargetOrSelf(payload);
    }
    if (baseValue->kind == HOPCTFEValue_INT && baseValue->i64 == 3
        && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "prefix"))
    {
        *outValue = p->loggerPrefix;
        *outIsConst = 1;
        return 0;
    }
    if (SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "len")
        && (baseValue->kind == HOPCTFEValue_STRING || baseValue->kind == HOPCTFEValue_ARRAY
            || baseValue->kind == HOPCTFEValue_NULL))
    {
        HOPEvalValueSetInt(outValue, (int64_t)baseValue->s.len);
        *outIsConst = 1;
        return 0;
    }
    if (SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "cstr")
        && baseValue->kind == HOPCTFEValue_STRING)
    {
        *outValue = *baseValue;
        *outIsConst = 1;
        return 0;
    }
    rt = HOPEvalValueAsReflectedType(baseValue);
    if (rt != NULL && rt->kind == HOPEvalReflectType_NAMED && rt->namedKind == HOPEvalTypeKind_ENUM
        && rt->file != NULL && rt->nodeId >= 0 && (uint32_t)rt->nodeId < rt->file->ast.len)
    {
        int32_t  variantNode = -1;
        uint32_t tagIndex = 0;
        if (HOPEvalFindEnumVariant(
                rt->file,
                rt->nodeId,
                p->currentFile->source,
                nameStart,
                nameEnd,
                &variantNode,
                &tagIndex))
        {
            const HOPAstNode* variantField = &rt->file->ast.nodes[variantNode];
            int32_t           valueNode = ASTFirstChild(&rt->file->ast, variantNode);
            if (valueNode >= 0 && rt->file->ast.nodes[valueNode].kind != HOPAst_FIELD
                && !HOPEvalEnumHasPayloadVariants(rt->file, rt->nodeId))
            {
                int valueIsConst = 0;
                if (HOPEvalExecExprInFileWithType(
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
            HOPEvalValueSetTaggedEnum(
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
    agg = HOPEvalValueAsAggregate(baseValue);
    if (agg == NULL) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field access is not supported by evaluator backend");
        }
        return 0;
    }
    if (!HOPEvalAggregateGetFieldValue(agg, p->currentFile->source, nameStart, nameEnd, outValue)) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReason(
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

static int HOPEvalMirAggAddrField(
    void*               ctx,
    const HOPCTFEValue* base,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*     p = (HOPEvalProgram*)ctx;
    const HOPCTFEValue* baseValue;
    const HOPCTFEValue* payload = NULL;
    HOPEvalAggregate*   agg = NULL;
    HOPCTFEValue*       fieldValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || base == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    baseValue = HOPEvalValueTargetOrSelf(base);
    if (baseValue->kind == HOPCTFEValue_OPTIONAL && HOPEvalOptionalPayload(baseValue, &payload)
        && baseValue->b != 0u && payload != NULL)
    {
        baseValue = HOPEvalValueTargetOrSelf(payload);
    }
    if (baseValue->kind == HOPCTFEValue_INT && baseValue->i64 == 3
        && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "prefix"))
    {
        HOPEvalValueSetReference(outValue, &p->loggerPrefix);
        *outIsConst = 1;
        return 0;
    }
    agg = HOPEvalValueAsAggregate(baseValue);
    if (agg == NULL) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field address is not supported by evaluator backend");
        }
        return 0;
    }
    fieldValue = HOPEvalAggregateLookupFieldValuePtr(
        agg, p->currentFile->source, nameStart, nameEnd);
    if (fieldValue == NULL) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field address is not supported by evaluator backend");
        }
        return 0;
    }
    HOPEvalValueSetReference(outValue, fieldValue);
    *outIsConst = 1;
    return 0;
}

static int HOPEvalMirAggSetField(
    void* _Nullable ctx,
    HOPCTFEValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
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
        HOPCTFEValue*      value = inOutBase;
        HOPEvalAggregate*  agg = HOPEvalValueAsAggregate(value);
        HOPEvalTaggedEnum* tagged = HOPEvalValueAsTaggedEnum(value);
        uint32_t           i;
        uint32_t           dot = nameStart;
        while (dot < nameEnd && p->currentFile->source[dot] != '.') {
            dot++;
        }
        if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
            agg = tagged->payload;
        }
        if (agg == NULL) {
            value = (HOPCTFEValue*)HOPEvalValueTargetOrSelf(inOutBase);
            agg = HOPEvalValueAsAggregate(value);
            tagged = HOPEvalValueAsTaggedEnum(value);
            if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
                agg = tagged->payload;
            }
        }
        if (agg != NULL && dot == nameEnd) {
            if (value != NULL) {
                value->typeTag |= HOPCTFEValueTag_AGG_PARTIAL;
            }
            HOPEvalAggregateField* field = HOPEvalAggregateFindDirectField(
                agg, p->currentFile->source, nameStart, nameEnd);
            if (field != NULL) {
                HOPCTFEValue coercedValue = *inValue;
                if (field->typeNode >= 0
                    && HOPEvalCoerceValueToTypeNode(p, agg->file, field->typeNode, &coercedValue)
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
            value->typeTag |= HOPCTFEValueTag_AGG_PARTIAL;
        }
    }
    if (!HOPEvalValueSetFieldPath(inOutBase, p->currentFile->source, nameStart, nameEnd, inValue)) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReason(
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

static int HOPEvalMirHostCall(
    void*               ctx,
    uint32_t            hostId,
    const HOPCTFEValue* args,
    uint32_t            argCount,
    HOPCTFEValue*       outValue,
    int*                outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (hostId == HOP_EVAL_MIR_HOST_PRINT && argCount == 1u && args[0].kind == HOPCTFEValue_STRING)
    {
        if (args[0].s.len > 0 && args[0].s.bytes != NULL) {
            if (fwrite(args[0].s.bytes, 1, args[0].s.len, stdout) != args[0].s.len) {
                return ErrorSimple("failed to write print output");
            }
        }
        fputc('\n', stdout);
        fflush(stdout);
        HOPEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == HOP_EVAL_MIR_HOST_CONCAT && argCount == 2u) {
        int concatRc = HOPEvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
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
    if (hostId == HOP_EVAL_MIR_HOST_COPY && argCount == 2u) {
        int copyRc = HOPEvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
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
    if (hostId == HOP_EVAL_MIR_HOST_FREE && (argCount == 1u || argCount == 2u)) {
        HOPEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == HOP_EVAL_MIR_HOST_PLATFORM_EXIT && argCount == 1u) {
        int64_t exitCode = 0;
        if (HOPCTFEValueToInt64(&args[0], &exitCode) != 0) {
            *outIsConst = 0;
            return 0;
        }
        p->exitCalled = 1;
        p->exitCode = (int)(exitCode & 255);
        HOPEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == HOP_EVAL_MIR_HOST_PLATFORM_CONSOLE_LOG && argCount == 2u) {
        int64_t flags = 0;
        if (args[0].kind != HOPCTFEValue_STRING || HOPCTFEValueToInt64(&args[1], &flags) != 0) {
            *outIsConst = 0;
            return 0;
        }
        (void)flags;
        if (args[0].s.len > 0 && args[0].s.bytes != NULL) {
            if (fwrite(args[0].s.bytes, 1, args[0].s.len, stdout) != args[0].s.len) {
                return ErrorSimple("failed to write console_log output");
            }
        }
        fputc('\n', stdout);
        fflush(stdout);
        HOPEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    *outIsConst = 0;
    return 0;
}

static void HOPEvalMirSetReason(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* _Nonnull reason) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    if (p == NULL || p->currentExecCtx == NULL || reason[0] == '\0') {
        return;
    }
    HOPCTFEExecSetReason(p->currentExecCtx, start, end, reason);
}

static int HOPEvalMirEnterFunction(
    void* ctx, uint32_t functionIndex, uint32_t sourceRef, HOPDiag* _Nullable diag) {
    HOPEvalMirExecCtx* c = (HOPEvalMirExecCtx*)ctx;
    uint8_t            pushed = 0;
    uint32_t           frameIndex;
    if (c == NULL || c->p == NULL || sourceRef >= c->sourceFileCap
        || c->savedFileLen >= HOP_EVAL_CALL_MAX_DEPTH)
    {
        if (diag != NULL) {
            diag->code = HOPDiag_UNEXPECTED_TOKEN;
            diag->type = HOPDiagTypeOfCode(diag->code);
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
                diag->code = HOPDiag_UNEXPECTED_TOKEN;
                diag->type = HOPDiagTypeOfCode(diag->code);
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
        } else if (evalFnIndex >= c->p->funcLen || c->p->callDepth >= HOP_EVAL_CALL_MAX_DEPTH) {
            if (diag != NULL) {
                diag->code = HOPDiag_UNEXPECTED_TOKEN;
                diag->type = HOPDiagTypeOfCode(diag->code);
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
    frameIndex = c->savedFileLen;
    c->savedMirExecCtxs[c->savedFileLen] = c->p->currentMirExecCtx;
    c->p->currentMirExecCtx = c;
    c->savedFiles[c->savedFileLen++] = c->p->currentFile;
    c->pushedFrames[c->savedFileLen - 1u] = pushed;
    c->restoresTemplateBinding[frameIndex] = 0u;
    if (c->hasPendingTemplateBinding) {
        c->savedTemplateBindings[frameIndex] = c->pendingTemplateBinding;
        c->restoresTemplateBinding[frameIndex] = 1u;
        c->hasPendingTemplateBinding = 0u;
    }
    if (c->sourceFiles[sourceRef] != NULL) {
        c->p->currentFile = c->sourceFiles[sourceRef];
    }
    return 0;
}

static void HOPEvalMirLeaveFunction(void* ctx) {
    HOPEvalMirExecCtx* c = (HOPEvalMirExecCtx*)ctx;
    uint32_t           frameIndex;
    if (c == NULL || c->p == NULL || c->savedFileLen == 0) {
        return;
    }
    frameIndex = c->savedFileLen - 1u;
    if (c->pushedFrames[frameIndex] && c->p->callDepth > 0) {
        c->p->callDepth--;
    }
    if (c->restoresTemplateBinding[frameIndex]) {
        HOPEvalRestoreTemplateBinding(c->p, &c->savedTemplateBindings[frameIndex]);
        c->restoresTemplateBinding[frameIndex] = 0u;
    }
    c->p->currentMirExecCtx = c->savedMirExecCtxs[c->savedFileLen - 1u];
    c->p->currentFile = c->savedFiles[--c->savedFileLen];
}

static int HOPEvalMirBindFrame(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPCTFEValue* _Nullable locals,
    uint32_t localCount,
    HOPDiag* _Nullable diag) {
    HOPEvalMirExecCtx* c = (HOPEvalMirExecCtx*)ctx;
    if (c == NULL || c->mirFrameDepth >= HOP_EVAL_CALL_MAX_DEPTH) {
        if (diag != NULL) {
            diag->code = HOPDiag_UNEXPECTED_TOKEN;
            diag->type = HOPDiagTypeOfCode(diag->code);
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

static void HOPEvalMirUnbindFrame(void* _Nullable ctx) {
    HOPEvalMirExecCtx* c = (HOPEvalMirExecCtx*)ctx;
    if (c == NULL || c->mirFrameDepth == 0) {
        return;
    }
    c->mirFrameDepth--;
    c->mirProgram = c->savedMirPrograms[c->mirFrameDepth];
    c->mirFunction = c->savedMirFunctions[c->mirFrameDepth];
    c->mirLocals = c->savedMirLocals[c->mirFrameDepth];
    c->mirLocalCount = c->savedMirLocalCounts[c->mirFrameDepth];
}

static int HOPEvalMirResolveCallNode(
    const HOPMirProgram* _Nullable program,
    const HOPMirInst* _Nullable inst,
    int32_t* _Nonnull outCallNode) {
    const HOPMirSymbolRef* symbol;
    *outCallNode = -1;
    if (program == NULL || inst == NULL || inst->op != HOPMirOp_CALL
        || inst->aux >= program->symbolLen)
    {
        return 0;
    }
    symbol = &program->symbols[inst->aux];
    if (symbol->kind != HOPMirSymbol_CALL || symbol->target == UINT32_MAX) {
        return 0;
    }
    *outCallNode = (int32_t)symbol->target;
    return 1;
}

static int HOPEvalMirCallNodeIsLazyBuiltin(HOPEvalProgram* p, int32_t callNode) {
    const HOPAst*     ast;
    const HOPAstNode* call;
    const HOPAstNode* callee;
    int32_t           calleeNode;
    int32_t           recvNode;
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
    if (call->kind != HOPAst_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == HOPAst_IDENT) {
        return HOPEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source,
                   callee->dataStart,
                   callee->dataEnd,
                   "source_location_of",
                   "builtin")
            || HOPEvalNameIsLazyTypeBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd)
            || HOPEvalNameIsCompilerDiagBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd);
    }
    if (callee->kind != HOPAst_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= ast->len || ast->nodes[recvNode].kind != HOPAst_IDENT)
    {
        return 0;
    }
    if (SliceEqCStr(
            p->currentFile->source,
            ast->nodes[recvNode].dataStart,
            ast->nodes[recvNode].dataEnd,
            "builtin"))
    {
        return HOPEvalNameEqLiteralOrPkgBuiltin(
            p->currentFile->source,
            callee->dataStart,
            callee->dataEnd,
            "source_location_of",
            "builtin");
    }
    if (SliceEqCStr(
            p->currentFile->source,
            ast->nodes[recvNode].dataStart,
            ast->nodes[recvNode].dataEnd,
            "reflect"))
    {
        return HOPEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "kind", "reflect")
            || HOPEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "base", "reflect")
            || HOPEvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source,
                   callee->dataStart,
                   callee->dataEnd,
                   "is_alias",
                   "reflect")
            || HOPEvalNameEqLiteralOrPkgBuiltin(
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
    return HOPEvalNameIsCompilerDiagBuiltin(
        p->currentFile->source, callee->dataStart, callee->dataEnd);
}

static const HOPPackage* _Nullable HOPEvalMirResolveQualifiedImportCallTargetPkg(
    HOPEvalProgram* p, int32_t callNode) {
    const HOPPackage* currentPkg;
    const HOPAst*     ast;
    const HOPAstNode* call;
    const HOPAstNode* callee;
    const HOPAstNode* base;
    uint32_t          i;
    int32_t           calleeNode;
    int32_t           baseNode;
    if (p == NULL || p->currentFile == NULL) {
        return NULL;
    }
    currentPkg = HOPEvalFindPackageByFile(p, p->currentFile);
    ast = &p->currentFile->ast;
    if (currentPkg == NULL || callNode < 0 || (uint32_t)callNode >= ast->len) {
        return NULL;
    }
    call = &ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &ast->nodes[calleeNode] : NULL;
    baseNode = calleeNode >= 0 ? ast->nodes[calleeNode].firstChild : -1;
    base = baseNode >= 0 ? &ast->nodes[baseNode] : NULL;
    if (call->kind != HOPAst_CALL || callee == NULL || callee->kind != HOPAst_FIELD_EXPR
        || base == NULL || base->kind != HOPAst_IDENT)
    {
        return NULL;
    }
    for (i = 0; i < currentPkg->importLen; i++) {
        const HOPImportRef* imp = &currentPkg->imports[i];
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

static int HOPEvalMirLookupLocalValue(
    HOPEvalProgram* p, uint32_t nameStart, uint32_t nameEnd, HOPCTFEValue* outValue) {
    HOPEvalMirExecCtx*   c;
    const HOPParsedFile* file;
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
                const HOPMirLocal* local =
                    &c->mirProgram->locals[c->mirFunction->localStart + i - 1u];
                const HOPCTFEValue* value = &c->mirLocals[i - 1u];
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
                if (value->kind == HOPCTFEValue_INVALID) {
                    continue;
                }
                *outValue = *value;
                if (local->typeRef != UINT32_MAX && local->typeRef < c->mirProgram->typeLen) {
                    int32_t typeNode = (int32_t)c->mirProgram->types[local->typeRef].astNode;
                    int32_t typeCode = HOPEvalTypeCode_INVALID;
                    if (!HOPEvalValueGetRuntimeTypeCode(outValue, &typeCode) && typeNode >= 0
                        && HOPEvalTypeCodeFromTypeNode(file, typeNode, &typeCode))
                    {
                        HOPEvalValueSetRuntimeTypeCode(outValue, typeCode);
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

static int HOPEvalMirLookupLocalTypeNode(
    HOPEvalProgram*       p,
    uint32_t              nameStart,
    uint32_t              nameEnd,
    const HOPParsedFile** outFile,
    int32_t*              outTypeNode) {
    HOPEvalMirExecCtx*   c;
    const HOPParsedFile* file;
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
                const HOPMirLocal* local =
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

static void HOPEvalMirInitExecEnv(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    HOPMirExecEnv*       env,
    HOPEvalMirExecCtx* _Nullable functionCtx) {
    if (p == NULL || file == NULL || env == NULL) {
        return;
    }
    memset(env, 0, sizeof(*env));
    env->src.ptr = file->source;
    env->src.len = file->sourceLen;
    env->resolveIdent = HOPEvalResolveIdent;
    env->assignIdent = HOPEvalMirAssignIdent;
    env->assignIdentCtx = p;
    env->resolveCallPre = HOPEvalResolveCallMirPre;
    env->resolveCall = HOPEvalResolveCallMir;
    env->adjustCallArgs = HOPEvalMirAdjustCallArgs;
    env->resolveCtx = p;
    env->adjustCallArgsCtx = p;
    env->hostCall = HOPEvalMirHostCall;
    env->hostCtx = p;
    env->zeroInitLocal = HOPEvalMirZeroInitLocal;
    env->zeroInitCtx = p;
    env->coerceValueForType = HOPEvalMirCoerceValueForType;
    env->coerceValueCtx = p;
    env->indexValue = HOPEvalMirIndexValue;
    env->indexValueCtx = p;
    env->indexAddr = HOPEvalMirIndexAddr;
    env->indexAddrCtx = p;
    env->sliceValue = HOPEvalMirSliceValue;
    env->sliceValueCtx = p;
    env->sequenceLen = HOPEvalMirSequenceLen;
    env->sequenceLenCtx = p;
    env->iterInit = HOPEvalMirIterInit;
    env->iterInitCtx = p;
    env->iterNext = HOPEvalMirIterNext;
    env->iterNextCtx = p;
    env->aggGetField = HOPEvalMirAggGetField;
    env->aggGetFieldCtx = p;
    env->aggAddrField = HOPEvalMirAggAddrField;
    env->aggAddrFieldCtx = p;
    env->aggSetField = HOPEvalMirAggSetField;
    env->aggSetFieldCtx = p;
    env->makeAggregate = HOPEvalMirMakeAggregate;
    env->makeAggregateCtx = p;
    env->makeTuple = HOPEvalMirMakeTuple;
    env->makeTupleCtx = p;
    env->makeVariadicPack = HOPEvalMirMakeVariadicPack;
    env->makeVariadicPackCtx = p;
    env->evalBinary = HOPEvalMirEvalBinary;
    env->evalBinaryCtx = p;
    env->allocNew = HOPEvalMirAllocNew;
    env->allocNewCtx = p;
    env->contextGet = HOPEvalMirContextGet;
    env->contextGetCtx = p;
    env->contextAddr = HOPEvalMirContextAddr;
    env->contextAddrCtx = p;
    env->evalWithContext = HOPEvalMirEvalWithContext;
    env->evalWithContextCtx = p;
    env->setReason = HOPEvalMirSetReason;
    env->setReasonCtx = p;
    env->backwardJumpLimit = p->currentExecCtx != NULL ? p->currentExecCtx->forIterLimit : 0;
    env->diag = p->currentExecCtx != NULL ? p->currentExecCtx->diag : NULL;
    if (functionCtx != NULL) {
        env->enterFunction = HOPEvalMirEnterFunction;
        env->leaveFunction = HOPEvalMirLeaveFunction;
        env->functionCtx = functionCtx;
        env->bindFrame = HOPEvalMirBindFrame;
        env->unbindFrame = HOPEvalMirUnbindFrame;
        env->frameCtx = functionCtx;
    }
}

static int HOPEvalMirEvalBinary(
    void* _Nullable ctx,
    HOPTokenKind           op,
    const HOPMirExecValue* lhs,
    const HOPMirExecValue* rhs,
    HOPMirExecValue*       outValue,
    int*                   outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    (void)diag;
    return HOPEvalEvalBinary(p, op, lhs, rhs, outValue, outIsConst);
}

static int HOPEvalTryMirZeroInitType(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    int32_t              typeNode,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    HOPCTFEValue*        outValue,
    int*                 outIsConst) {
    return HOPEvalTryMirEvalTopInit(
        p, file, -1, typeNode, nameStart, nameEnd, NULL, -1, outValue, outIsConst, NULL);
}

static int HOPEvalTryMirEvalTopInit(
    HOPEvalProgram*      p,
    const HOPParsedFile* file,
    int32_t              initExprNode,
    int32_t              declTypeNode,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const HOPParsedFile* _Nullable coerceTypeFile,
    int32_t       coerceTypeNode,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    int* _Nullable outSupported) {
    HOPMirProgram     program = { 0 };
    HOPMirExecEnv     env = { 0 };
    HOPEvalMirExecCtx functionCtx = { 0 };
    uint32_t          rootMirFnIndex = UINT32_MAX;
    int               supported = 0;
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (p == NULL || file == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (HOPEvalMirBuildTopInitProgram(
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
    HOPEvalMirInitExecEnv(p, file, &env, &functionCtx);
    if (!HOPMirProgramNeedsDynamicResolution(&program)) {
        HOPMirExecEnvDisableDynamicResolution(&env);
    }
    if (HOPMirEvalFunction(p->arena, &program, rootMirFnIndex, NULL, 0, &env, outValue, outIsConst)
        != 0)
    {
        return -1;
    }
    HOPEvalMirAdaptOutValue(&functionCtx, outValue, outIsConst);
    if (*outIsConst && coerceTypeFile != NULL && coerceTypeNode >= 0
        && HOPEvalAdaptStringValueForType(
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

static int HOPEvalTryMirEvalExprWithType(
    HOPEvalProgram*      p,
    int32_t              exprNode,
    const HOPParsedFile* exprFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const HOPParsedFile* _Nullable typeFile,
    int32_t       typeNode,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    int*          outSupported) {
    return HOPEvalTryMirEvalTopInit(
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

static int HOPEvalValueNeedsDefaultFieldEval(const HOPCTFEValue* value) {
    HOPEvalAggregate* agg;
    uint32_t          i;
    if (value == NULL) {
        return 0;
    }
    agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(value));
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

static int HOPEvalFinalizeNewAggregateDefaults(HOPEvalProgram* p, HOPCTFEValue* inOutValue) {
    HOPEvalAggregate*   agg = HOPEvalValueAsAggregate(inOutValue);
    HOPCTFEExecBinding* fieldBindings = NULL;
    HOPCTFEExecEnv      fieldFrame;
    uint32_t            fieldBindingCap = 0;
    uint32_t            i;
    if (p == NULL || inOutValue == NULL || agg == NULL
        || !HOPEvalValueNeedsDefaultFieldEval(inOutValue))
    {
        return 1;
    }
    if (agg->fieldLen > 0) {
        fieldBindingCap = HOPEvalAggregateFieldBindingCount(agg);
        fieldBindings = (HOPCTFEExecBinding*)HOPArenaAlloc(
            p->arena,
            sizeof(HOPCTFEExecBinding) * fieldBindingCap,
            (uint32_t)_Alignof(HOPCTFEExecBinding));
        if (fieldBindings == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(fieldBindings, 0, sizeof(HOPCTFEExecBinding) * fieldBindingCap);
    }
    fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
    fieldFrame.bindings = fieldBindings;
    fieldFrame.bindingLen = 0;
    for (i = 0; i < agg->fieldLen; i++) {
        HOPEvalAggregateField* field = &agg->fields[i];
        if (field->defaultExprNode >= 0) {
            HOPCTFEValue defaultValue;
            int          defaultIsConst = 0;
            if (HOPEvalExecExprInFileWithType(
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
        if (fieldBindings != NULL
            && HOPEvalAppendAggregateFieldBindings(
                   fieldBindings, fieldBindingCap, &fieldFrame, field)
                   != 0)
        {
            return ErrorSimple("out of memory");
        }
    }
    return 1;
}

static int HOPEvalMirAllocNew(
    void* _Nullable ctx,
    uint32_t         sourceNode,
    HOPMirExecValue* outValue,
    int*             outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*      p = (HOPEvalProgram*)ctx;
    const HOPParsedFile* newFile;
    int32_t              exprNode = (int32_t)sourceNode;
    int32_t              typeNode = -1;
    int32_t              countNode = -1;
    int32_t              initNode = -1;
    int32_t              allocNode = -1;
    HOPCTFEValue         allocValue;
    int                  allocIsConst = 0;
    int                  allocSupported = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    newFile = p->currentFile;
    if (!HOPEvalDecodeNewExprNodes(newFile, exprNode, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    if (allocNode >= 0) {
        if (HOPEvalTryMirEvalExprWithType(
                p, allocNode, newFile, 0, 0, NULL, -1, &allocValue, &allocIsConst, &allocSupported)
            != 0)
        {
            return -1;
        }
        if (!allocSupported || !allocIsConst) {
            return 0;
        }
    } else if (!HOPEvalCurrentContextFieldByLiteral(p, "allocator", &allocValue)) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReasonNode(
                p->currentExecCtx,
                exprNode,
                "allocator context is not available in evaluator backend");
        }
        return 0;
    }
    if (HOPEvalValueTargetOrSelf(&allocValue)->kind == HOPCTFEValue_NULL) {
        if (p->currentExecCtx != NULL) {
            HOPCTFEExecSetReasonNode(p->currentExecCtx, exprNode, "invalid allocator");
        }
        return 0;
    }
    {
        const HOPParsedFile* expectedTypeFile = NULL;
        int32_t              expectedTypeNode = -1;
        int                  allocReturnedNull = 0;
        int allocRc = HOPEvalCheckAllocatorImplResult(p, exprNode, &allocValue, &allocReturnedNull);
        if (allocRc <= 0) {
            if (allocRc == 0 && allocReturnedNull
                && HOPEvalFindExpectedTypeForInitExpr(
                    newFile, exprNode, &expectedTypeFile, &expectedTypeNode)
                && HOPEvalExpectedNewResultIsOptional(expectedTypeFile, expectedTypeNode))
            {
                HOPEvalValueSetNull(outValue);
                *outIsConst = 1;
                return 1;
            }
            if (allocRc == 0 && allocReturnedNull && p->currentExecCtx != NULL) {
                HOPCTFEExecSetReasonNode(p->currentExecCtx, exprNode, "allocator returned null");
            }
            return allocRc;
        }
    }
    if (countNode >= 0) {
        HOPCTFEValue  countValue;
        HOPCTFEValue  arrayValue;
        HOPEvalArray* array;
        int           countIsConst = 0;
        int           countSupported = 0;
        int64_t       count = 0;
        uint32_t      i;
        if (HOPEvalTryMirEvalExprWithType(
                p, countNode, newFile, 0, 0, NULL, -1, &countValue, &countIsConst, &countSupported)
            != 0)
        {
            return -1;
        }
        if (!countSupported || !countIsConst || HOPCTFEValueToInt64(&countValue, &count) != 0
            || count < 0)
        {
            return 0;
        }
        array = (HOPEvalArray*)HOPArenaAlloc(
            p->arena, sizeof(HOPEvalArray), (uint32_t)_Alignof(HOPEvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = newFile;
        array->typeNode = exprNode;
        array->elemTypeNode = typeNode;
        array->len = (uint32_t)count;
        if (array->len > 0) {
            array->elems = (HOPCTFEValue*)HOPArenaAlloc(
                p->arena, sizeof(HOPCTFEValue) * array->len, (uint32_t)_Alignof(HOPCTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(HOPCTFEValue) * array->len);
            if (initNode >= 0) {
                HOPCTFEValue initValue;
                int          initIsConst = 0;
                if (HOPEvalExecExprWithTypeNode(
                        p, initNode, newFile, typeNode, &initValue, &initIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!initIsConst) {
                    return 0;
                }
                if (HOPEvalCoerceValueToTypeNode(p, newFile, typeNode, &initValue) != 0) {
                    return -1;
                }
                for (i = 0; i < array->len; i++) {
                    array->elems[i] = initValue;
                }
            } else {
                for (i = 0; i < array->len; i++) {
                    int elemIsConst = 0;
                    if (HOPEvalZeroInitTypeNode(
                            p, newFile, typeNode, &array->elems[i], &elemIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (!elemIsConst) {
                        return 0;
                    }
                    {
                        int finalizeRc = HOPEvalFinalizeNewAggregateDefaults(p, &array->elems[i]);
                        if (finalizeRc <= 0) {
                            return finalizeRc < 0 ? -1 : 0;
                        }
                    }
                }
            }
        }
        HOPEvalValueSetArray(&arrayValue, newFile, exprNode, array);
        return HOPEvalAllocReferencedValue(p, &arrayValue, outValue, outIsConst) == 0 ? 1 : -1;
    }
    {
        HOPCTFEValue value;
        int          valueIsConst = 0;
        if (initNode >= 0) {
            if (HOPEvalExecExprWithTypeNode(p, initNode, newFile, typeNode, &value, &valueIsConst)
                != 0)
            {
                return -1;
            }
            if (!valueIsConst) {
                return 0;
            }
        } else {
            if (HOPEvalZeroInitTypeNode(p, newFile, typeNode, &value, &valueIsConst) != 0) {
                return -1;
            }
            if (!valueIsConst) {
                return 0;
            }
        }
        if (initNode >= 0) {
            if (HOPEvalCoerceValueToTypeNode(p, newFile, typeNode, &value) != 0) {
                return -1;
            }
        } else {
            int finalizeRc = HOPEvalFinalizeNewAggregateDefaults(p, &value);
            if (finalizeRc <= 0) {
                return finalizeRc < 0 ? -1 : 0;
            }
        }
        return HOPEvalAllocReferencedValue(p, &value, outValue, outIsConst) == 0 ? 1 : -1;
    }
}

static int HOPEvalBinaryOpForAssignToken(HOPTokenKind assignOp, HOPTokenKind* outBinaryOp) {
    if (outBinaryOp == NULL) {
        return 0;
    }
    switch (assignOp) {
        case HOPTok_ADD_ASSIGN: *outBinaryOp = HOPTok_ADD; return 1;
        case HOPTok_SUB_ASSIGN: *outBinaryOp = HOPTok_SUB; return 1;
        case HOPTok_MUL_ASSIGN: *outBinaryOp = HOPTok_MUL; return 1;
        case HOPTok_DIV_ASSIGN: *outBinaryOp = HOPTok_DIV; return 1;
        case HOPTok_MOD_ASSIGN: *outBinaryOp = HOPTok_MOD; return 1;
        default:                *outBinaryOp = HOPTok_INVALID; return 0;
    }
}

static int HOPEvalAssignExprCb(
    void* ctx, HOPCTFEExecCtx* execCtx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPEvalProgram*   p = (HOPEvalProgram*)ctx;
    const HOPAst*     ast;
    const HOPAstNode* expr;
    int32_t           lhsNode;
    int32_t           rhsNode;
    HOPCTFEValue      rhsValue;
    int               rhsIsConst = 0;
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
    if (expr->kind != HOPAst_BINARY || lhsNode < 0 || rhsNode < 0
        || ast->nodes[rhsNode].nextSibling >= 0)
    {
        *outIsConst = 0;
        return 0;
    }
    if (ast->nodes[lhsNode].kind == HOPAst_IDENT) {
        if (SliceEqCStr(
                p->currentFile->source,
                ast->nodes[lhsNode].dataStart,
                ast->nodes[lhsNode].dataEnd,
                "_"))
        {
            if (HOPEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
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
        int32_t topVarIndex = HOPEvalFindCurrentTopVarBySlice(
            p, p->currentFile, ast->nodes[lhsNode].dataStart, ast->nodes[lhsNode].dataEnd);
        if (topVarIndex >= 0) {
            HOPEvalTopVar* topVar = &p->topVars[(uint32_t)topVarIndex];
            if ((HOPTokenKind)expr->op == HOPTok_ASSIGN) {
                if (topVar->declTypeNode >= 0) {
                    if (HOPEvalExecExprWithTypeNode(
                            p, rhsNode, topVar->file, topVar->declTypeNode, &rhsValue, &rhsIsConst)
                        != 0)
                    {
                        return -1;
                    }
                } else if (HOPEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
                    return -1;
                }
                if (!rhsIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                topVar->value = rhsValue;
                topVar->state = HOPEvalTopConstState_READY;
                *outValue = rhsValue;
                *outIsConst = 1;
                return 0;
            }
        }
    }
    if (ast->nodes[lhsNode].kind == HOPAst_INDEX && (ast->nodes[lhsNode].flags & 0x7u) == 0u) {
        int32_t       baseNode = ast->nodes[lhsNode].firstChild;
        int32_t       indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        HOPCTFEValue  baseValue;
        HOPCTFEValue  indexValue;
        HOPEvalArray* array;
        HOPTokenKind  binaryOp = HOPTok_INVALID;
        int           baseIsConst = 0;
        int           indexIsConst = 0;
        int64_t       index = 0;
        if (HOPEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (!rhsIsConst || baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0)
        {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || HOPEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || HOPCTFEValueToInt64(&indexValue, &index) != 0) {
            *outIsConst = 0;
            return 0;
        }
        array = HOPEvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                int handled;
                if (!HOPEvalBinaryOpForAssignToken((HOPTokenKind)expr->op, &binaryOp)) {
                    *outIsConst = 0;
                    return 0;
                }
                handled = HOPEvalEvalBinary(
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
            HOPCTFEValue* targetValue = HOPEvalValueReferenceTarget(&baseValue);
            int32_t       baseTypeCode = HOPEvalTypeCode_INVALID;
            int64_t       byteValue = 0;
            if (targetValue == NULL) {
                targetValue = (HOPCTFEValue*)HOPEvalValueTargetOrSelf(&baseValue);
            }
            if (targetValue != NULL && targetValue->kind == HOPCTFEValue_STRING
                && HOPEvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
                && baseTypeCode == HOPEvalTypeCode_STR_PTR && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len
                && HOPCTFEValueToInt64(&rhsValue, &byteValue) == 0 && byteValue >= 0
                && byteValue <= 255)
            {
                ((uint8_t*)targetValue->s.bytes)[(uint32_t)index] = (uint8_t)byteValue;
                HOPEvalValueSetInt(outValue, byteValue);
                HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_U8);
                *outIsConst = 1;
                return 0;
            }
        }
        *outIsConst = 0;
        return 0;
    }
    if (ast->nodes[lhsNode].kind == HOPAst_UNARY
        && (HOPTokenKind)ast->nodes[lhsNode].op == HOPTok_MUL)
    {
        int32_t       refNode = ast->nodes[lhsNode].firstChild;
        HOPCTFEValue  refValue;
        HOPCTFEValue* target;
        HOPTokenKind  binaryOp = HOPTok_INVALID;
        int           refIsConst = 0;
        if (HOPEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (!rhsIsConst || refNode < 0) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, refNode, &refValue, &refIsConst) != 0) {
            return -1;
        }
        if (!refIsConst) {
            *outIsConst = 0;
            return 0;
        }
        target = HOPEvalValueReferenceTarget(&refValue);
        if (target == NULL) {
            *outIsConst = 0;
            return 0;
        }
        if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
            int handled;
            if (!HOPEvalBinaryOpForAssignToken((HOPTokenKind)expr->op, &binaryOp)) {
                *outIsConst = 0;
                return 0;
            }
            handled = HOPEvalEvalBinary(p, binaryOp, target, &rhsValue, &rhsValue, outIsConst);
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
    if (ast->nodes[lhsNode].kind != HOPAst_FIELD_EXPR) {
        *outIsConst = 0;
        return 0;
    }
    if (HOPEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
        return -1;
    }
    if (!rhsIsConst) {
        *outIsConst = 0;
        return 0;
    }
    {
        int32_t             curNode = lhsNode;
        HOPCTFEExecBinding* binding = NULL;
        HOPEvalAggregate*   agg = NULL;
        while (ast->nodes[curNode].kind == HOPAst_FIELD_EXPR) {
            int32_t baseNode = ast->nodes[curNode].firstChild;
            if (baseNode < 0) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[baseNode].kind == HOPAst_IDENT) {
                int allowIndirectMutation = 0;
                binding = HOPEvalFindBinding(
                    execCtx,
                    p->currentFile,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd);
                if (binding == NULL) {
                    *outIsConst = 0;
                    return 0;
                }
                agg = HOPEvalValueAsAggregate(&binding->value);
                if (agg != NULL) {
                    allowIndirectMutation = 0;
                } else {
                    agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(&binding->value));
                    allowIndirectMutation = agg != NULL;
                }
                if (!binding->mutable && !allowIndirectMutation) {
                    *outIsConst = 0;
                    return 0;
                }
                break;
            }
            if (ast->nodes[baseNode].kind == HOPAst_FIELD_EXPR) {
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
            while (curNode >= 0 && ast->nodes[curNode].kind == HOPAst_FIELD_EXPR) {
                if (pathLen >= 16u) {
                    *outIsConst = 0;
                    return 0;
                }
                pathNodes[pathLen++] = curNode;
                curNode = ast->nodes[curNode].firstChild;
            }
            while (pathLen > 0) {
                const HOPAstNode* fieldNode;
                pathLen--;
                fieldNode = &ast->nodes[pathNodes[pathLen]];
                if (pathLen == 0) {
                    if ((HOPTokenKind)expr->op != HOPTok_ASSIGN) {
                        HOPCTFEValue curValue;
                        HOPTokenKind binaryOp = HOPTok_INVALID;
                        int          handled = 0;
                        if (!HOPEvalAggregateGetFieldValue(
                                agg,
                                p->currentFile->source,
                                fieldNode->dataStart,
                                fieldNode->dataEnd,
                                &curValue))
                        {
                            *outIsConst = 0;
                            return 0;
                        }
                        if (!HOPEvalBinaryOpForAssignToken((HOPTokenKind)expr->op, &binaryOp)) {
                            *outIsConst = 0;
                            return 0;
                        }
                        handled = HOPEvalEvalBinary(
                            p, binaryOp, &curValue, &rhsValue, &rhsValue, outIsConst);
                        if (handled <= 0 || !*outIsConst) {
                            *outIsConst = 0;
                            return handled < 0 ? -1 : 0;
                        }
                    }
                    if (HOPEvalAggregateSetFieldValue(
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
                    HOPCTFEValue nestedValue;
                    if (!HOPEvalAggregateGetFieldValue(
                            agg,
                            p->currentFile->source,
                            fieldNode->dataStart,
                            fieldNode->dataEnd,
                            &nestedValue))
                    {
                        *outIsConst = 0;
                        return 0;
                    }
                    agg = HOPEvalValueAsAggregate(&nestedValue);
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

static HOPCTFEValue* _Nullable HOPEvalResolveFieldExprValuePtr(
    HOPEvalProgram* p, HOPCTFEExecCtx* execCtx, int32_t fieldExprNode) {
    const HOPAst*     ast;
    const HOPAstNode* fieldExpr;
    int32_t           baseNode;
    HOPEvalAggregate* agg = NULL;
    if (p == NULL || p->currentFile == NULL || execCtx == NULL || fieldExprNode < 0) {
        return NULL;
    }
    ast = &p->currentFile->ast;
    if ((uint32_t)fieldExprNode >= ast->len) {
        return NULL;
    }
    fieldExpr = &ast->nodes[fieldExprNode];
    if (fieldExpr->kind != HOPAst_FIELD_EXPR) {
        return NULL;
    }
    baseNode = fieldExpr->firstChild;
    if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
        return NULL;
    }
    if (ast->nodes[baseNode].kind == HOPAst_IDENT) {
        HOPCTFEExecBinding* binding = HOPEvalFindBinding(
            execCtx, p->currentFile, ast->nodes[baseNode].dataStart, ast->nodes[baseNode].dataEnd);
        if (binding == NULL) {
            return NULL;
        }
        agg = HOPEvalValueAsAggregate(&binding->value);
        if (agg == NULL) {
            agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(&binding->value));
        }
    } else if (ast->nodes[baseNode].kind == HOPAst_FIELD_EXPR) {
        HOPCTFEValue* baseValue = HOPEvalResolveFieldExprValuePtr(p, execCtx, baseNode);
        if (baseValue == NULL) {
            return NULL;
        }
        agg = HOPEvalValueAsAggregate(baseValue);
        if (agg == NULL) {
            agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(baseValue));
        }
    }
    if (agg == NULL) {
        return NULL;
    }
    return HOPEvalAggregateLookupFieldValuePtr(
        agg, p->currentFile->source, fieldExpr->dataStart, fieldExpr->dataEnd);
}

static int HOPEvalAssignValueExprCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    int32_t             lhsExprNode,
    const HOPCTFEValue* inValue,
    HOPCTFEValue*       outValue,
    int*                outIsConst) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    const HOPAst*   ast;
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
    if (ast->nodes[lhsExprNode].kind == HOPAst_IDENT) {
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
        int32_t topVarIndex = HOPEvalFindCurrentTopVarBySlice(
            p, p->currentFile, ast->nodes[lhsExprNode].dataStart, ast->nodes[lhsExprNode].dataEnd);
        if (topVarIndex >= 0) {
            HOPEvalTopVar* topVar = &p->topVars[(uint32_t)topVarIndex];
            topVar->value = *inValue;
            topVar->state = HOPEvalTopConstState_READY;
            *outValue = *inValue;
            *outIsConst = 1;
            return 0;
        }
    }
    if (ast->nodes[lhsExprNode].kind == HOPAst_INDEX
        && (ast->nodes[lhsExprNode].flags & 0x7u) == 0u)
    {
        int32_t       baseNode = ast->nodes[lhsExprNode].firstChild;
        int32_t       indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        HOPCTFEValue  baseValue;
        HOPCTFEValue  indexValue;
        HOPEvalArray* array;
        int           baseIsConst = 0;
        int           indexIsConst = 0;
        int64_t       index = 0;
        if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || HOPEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || HOPCTFEValueToInt64(&indexValue, &index) != 0) {
            *outIsConst = 0;
            return 0;
        }
        array = HOPEvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            array->elems[(uint32_t)index] = *inValue;
            *outValue = *inValue;
            *outIsConst = 1;
            return 0;
        }
        {
            HOPCTFEValue* targetValue = HOPEvalValueReferenceTarget(&baseValue);
            int32_t       baseTypeCode = HOPEvalTypeCode_INVALID;
            int64_t       byteValue = 0;
            if (targetValue == NULL) {
                targetValue = (HOPCTFEValue*)HOPEvalValueTargetOrSelf(&baseValue);
            }
            if (targetValue != NULL && targetValue->kind == HOPCTFEValue_STRING
                && HOPEvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
                && baseTypeCode == HOPEvalTypeCode_STR_PTR && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len
                && HOPCTFEValueToInt64(inValue, &byteValue) == 0 && byteValue >= 0
                && byteValue <= 255)
            {
                ((uint8_t*)targetValue->s.bytes)[(uint32_t)index] = (uint8_t)byteValue;
                HOPEvalValueSetInt(outValue, byteValue);
                HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_U8);
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

static int HOPEvalMatchPatternCb(
    void*               ctx,
    HOPCTFEExecCtx*     execCtx,
    const HOPCTFEValue* subjectValue,
    int32_t             labelExprNode,
    int*                outMatched) {
    HOPEvalProgram*      p = (HOPEvalProgram*)ctx;
    HOPEvalTaggedEnum*   tagged;
    const HOPAstNode*    labelNode;
    const HOPParsedFile* enumFile = NULL;
    int32_t              enumNode = -1;
    int32_t              variantNode = -1;
    uint32_t             tagIndex = 0;
    (void)execCtx;
    if (outMatched != NULL) {
        *outMatched = 0;
    }
    if (p == NULL || p->currentFile == NULL || subjectValue == NULL || outMatched == NULL
        || labelExprNode < 0 || (uint32_t)labelExprNode >= p->currentFile->ast.len)
    {
        return -1;
    }
    tagged = HOPEvalValueAsTaggedEnum(HOPEvalValueTargetOrSelf(subjectValue));
    if (tagged == NULL) {
        return 0;
    }
    labelNode = &p->currentFile->ast.nodes[labelExprNode];
    if (labelNode->kind != HOPAst_FIELD_EXPR || labelNode->firstChild < 0
        || p->currentFile->ast.nodes[labelNode->firstChild].kind != HOPAst_IDENT)
    {
        return 0;
    }
    enumNode = HOPEvalFindNamedEnumDecl(
        p,
        p->currentFile,
        p->currentFile->ast.nodes[labelNode->firstChild].dataStart,
        p->currentFile->ast.nodes[labelNode->firstChild].dataEnd,
        &enumFile);
    if (enumNode < 0 || enumFile == NULL
        || !HOPEvalFindEnumVariant(
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

typedef struct {
    HOPEvalProgram*      p;
    const HOPParsedFile* file;
} HOPEvalMirLowerConstCtx;

static int HOPEvalMirLowerConstExpr(
    void* _Nullable ctx,
    int32_t exprNode,
    HOPMirConst* _Nonnull outValue,
    HOPDiag* _Nullable diag) {
    HOPEvalMirLowerConstCtx* lowerCtx = (HOPEvalMirLowerConstCtx*)ctx;
    HOPCTFEValue             value;
    int32_t                  typeNode;
    int                      rc;
    (void)diag;
    if (lowerCtx == NULL || lowerCtx->p == NULL || lowerCtx->file == NULL || outValue == NULL
        || exprNode < 0 || (uint32_t)exprNode >= lowerCtx->file->ast.len)
    {
        return -1;
    }
    if (lowerCtx->file->ast.nodes[exprNode].kind != HOPAst_TYPE_VALUE) {
        return 0;
    }
    typeNode = ASTFirstChild(&lowerCtx->file->ast, exprNode);
    rc = HOPEvalTypeValueFromTypeNode(lowerCtx->p, lowerCtx->file, typeNode, &value);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0 || value.kind != HOPCTFEValue_TYPE) {
        return 0;
    }
    outValue->kind = HOPMirConst_TYPE;
    outValue->bits = value.typeTag;
    outValue->bytes.ptr = (const char*)value.s.bytes;
    outValue->bytes.len = value.s.len;
    return 1;
}

HOP_API_END
#include "evaluator_mir.inc"
HOP_API_BEGIN

static int HOPEvalInvokeFunction(
    HOPEvalProgram* p,
    int32_t         fnIndex,
    const HOPCTFEValue* _Nullable args,
    uint32_t              argCount,
    const HOPEvalContext* callContext,
    HOPCTFEValue*         outValue,
    int*                  outDidReturn) {
    const HOPEvalFunction* fn;
    const HOPAst*          ast;
    HOPCTFEExecBinding*    paramBindings = NULL;
    HOPCTFEExecEnv         paramFrame;
    HOPCTFEExecCtx         execCtx;
    const HOPParsedFile*   savedFile;
    HOPCTFEExecCtx*        savedExecCtx;
    const HOPEvalContext*  savedContext;
    int                    isConst = 0;
    int                    rc;
    int32_t                child;
    uint32_t               paramIndex = 0;
    uint32_t               fixedCount = 0;

    if (p == NULL || outValue == NULL || outDidReturn == NULL || (argCount > 0 && args == NULL)
        || fnIndex < 0 || (uint32_t)fnIndex >= p->funcLen)
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
        concatRc = HOPEvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
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
        copyRc = HOPEvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
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
        paramBindings = (HOPCTFEExecBinding*)HOPArenaAlloc(
            p->arena,
            sizeof(HOPCTFEExecBinding) * fn->paramCount,
            (uint32_t)_Alignof(HOPCTFEExecBinding));
        if (paramBindings == NULL) {
            return ErrorSimple("out of memory");
        }
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const HOPAstNode* n = &ast->nodes[child];
        if (n->kind == HOPAst_PARAM) {
            if (paramBindings == NULL) {
                return ErrorSimple("out of memory");
            }
            int32_t      paramTypeNode = ASTFirstChild(ast, child);
            HOPCTFEValue boundValue;
            if (fn->isVariadic && paramIndex + 1u == fn->paramCount) {
                HOPEvalArray* packArray;
                uint32_t      packLen = argCount - fixedCount;
                packArray = HOPEvalAllocArrayView(
                    p, fn->file, paramTypeNode, paramTypeNode, NULL, packLen);
                if (packArray == NULL) {
                    return ErrorSimple("out of memory");
                }
                if (packLen > 0) {
                    uint32_t i;
                    packArray->elems = (HOPCTFEValue*)HOPArenaAlloc(
                        p->arena, sizeof(HOPCTFEValue) * packLen, (uint32_t)_Alignof(HOPCTFEValue));
                    if (packArray->elems == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    memset(packArray->elems, 0, sizeof(HOPCTFEValue) * packLen);
                    for (i = 0; i < packLen; i++) {
                        packArray->elems[i] = args[fixedCount + i];
                    }
                }
                HOPEvalValueSetArray(&boundValue, fn->file, paramTypeNode, packArray);
            } else {
                boundValue = args[paramIndex];
                if (paramTypeNode >= 0
                    && HOPEvalCoerceValueToTypeNode(p, fn->file, paramTypeNode, &boundValue) != 0)
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
    execCtx.evalExpr = HOPEvalExecExprCb;
    execCtx.evalExprCtx = p;
    execCtx.evalExprForType = HOPEvalExecExprForTypeCb;
    execCtx.evalExprForTypeCtx = p;
    execCtx.zeroInit = HOPEvalZeroInitCb;
    execCtx.zeroInitCtx = p;
    execCtx.assignExpr = HOPEvalAssignExprCb;
    execCtx.assignExprCtx = p;
    execCtx.assignValueExpr = HOPEvalAssignValueExprCb;
    execCtx.assignValueExprCtx = p;
    execCtx.matchPattern = HOPEvalMatchPatternCb;
    execCtx.matchPatternCtx = p;
    execCtx.forInIndex = HOPEvalForInIndexCb;
    execCtx.forInIndexCtx = p;
    execCtx.forInIter = HOPEvalForInIterCb;
    execCtx.forInIterCtx = p;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = HOPCTFE_EXEC_DEFAULT_FOR_LIMIT;
    execCtx.skipConstBlocks = 1u;
    HOPCTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    savedContext = p->currentContext;
    p->currentFile = fn->file;
    p->currentExecCtx = &execCtx;
    p->currentContext = callContext;
    p->callStack[p->callDepth++] = (uint32_t)fnIndex;

    rc = HOPEvalTryMirInvokeFunction(
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
        HOPEvalValueSetNull(outValue);
    } else if (fn->hasReturnType) {
        int32_t returnTypeNode = HOPEvalFunctionReturnTypeNode(fn);
        if (returnTypeNode >= 0
            && HOPEvalCoerceValueToTypeNode(p, fn->file, returnTypeNode, outValue) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int HOPEvalInvokeFunctionRef(
    HOPEvalProgram*     p,
    const HOPCTFEValue* calleeValue,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst) {
    uint32_t               fnIndex = 0;
    uint32_t               mirFnIndex = 0;
    int                    didReturn = 0;
    const HOPEvalFunction* fn;
    if (p == NULL || calleeValue == NULL || outValue == NULL || outIsConst == NULL
        || (argCount > 0 && args == NULL))
    {
        return 0;
    }
    if (HOPMirValueAsFunctionRef(calleeValue, &mirFnIndex)) {
        HOPMirExecEnv env = { 0 };
        int           mirIsConst = 0;
        if (p->currentMirExecCtx == NULL || p->currentMirExecCtx->mirProgram == NULL
            || mirFnIndex >= p->currentMirExecCtx->mirProgram->funcLen)
        {
            return 0;
        }
        HOPEvalMirInitExecEnv(p, p->currentFile, &env, p->currentMirExecCtx);
        if (!HOPMirProgramNeedsDynamicResolution(p->currentMirExecCtx->mirProgram)) {
            HOPMirExecEnvDisableDynamicResolution(&env);
        }
        if (HOPMirEvalFunction(
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
        HOPEvalMirAdaptOutValue(p->currentMirExecCtx, outValue, &mirIsConst);
        if (mirIsConst && outValue->kind == HOPCTFEValue_INVALID) {
            HOPEvalValueSetNull(outValue);
        }
        *outIsConst = mirIsConst;
        return 1;
    }
    if (!HOPEvalValueIsFunctionRef(calleeValue, &fnIndex) || fnIndex >= p->funcLen) {
        return 0;
    }
    fn = &p->funcs[fnIndex];
    if (fn->isBuiltinPackageFn) {
        return 0;
    }
    {
        HOPEvalTemplateBindingState savedBinding;
        HOPEvalSaveTemplateBinding(p, &savedBinding);
        (void)HOPEvalBindActiveTemplateForMirCall(p, NULL, NULL, NULL, fn, args, argCount);
        if (HOPEvalInvokeFunction(
                p, (int32_t)fnIndex, args, argCount, p->currentContext, outValue, &didReturn)
            != 0)
        {
            HOPEvalRestoreTemplateBinding(p, &savedBinding);
            return -1;
        }
        HOPEvalRestoreTemplateBinding(p, &savedBinding);
    }
    if (!didReturn) {
        if (fn->hasReturnType) {
            *outIsConst = 0;
            return 1;
        }
        HOPEvalValueSetNull(outValue);
    }
    *outIsConst = 1;
    return 1;
}

static int HOPEvalEvalTopVar(
    HOPEvalProgram* p, uint32_t topVarIndex, HOPCTFEValue* outValue, int* outIsConst) {
    HOPEvalTopVar*       topVar;
    const HOPParsedFile* savedFile;
    HOPCTFEExecCtx*      savedExecCtx;
    HOPCTFEExecCtx       execCtx;
    HOPCTFEValue         value;
    int                  isConst = 0;
    int                  rc;
    if (p == NULL || outValue == NULL || outIsConst == NULL || topVarIndex >= p->topVarLen) {
        return -1;
    }
    topVar = &p->topVars[topVarIndex];
    if (topVar->state == HOPEvalTopConstState_READY) {
        *outValue = topVar->value;
        *outIsConst = 1;
        return 0;
    }
    if (topVar->state == HOPEvalTopConstState_VISITING
        || topVar->state == HOPEvalTopConstState_FAILED)
    {
        *outIsConst = 0;
        return 0;
    }

    topVar->state = HOPEvalTopConstState_VISITING;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = p->arena;
    execCtx.ast = &topVar->file->ast;
    execCtx.src.ptr = topVar->file->source;
    execCtx.src.len = topVar->file->sourceLen;
    execCtx.evalExpr = HOPEvalExecExprCb;
    execCtx.evalExprCtx = p;
    execCtx.evalExprForType = HOPEvalExecExprForTypeCb;
    execCtx.evalExprForTypeCtx = p;
    execCtx.zeroInit = HOPEvalZeroInitCb;
    execCtx.zeroInitCtx = p;
    execCtx.assignExpr = HOPEvalAssignExprCb;
    execCtx.assignExprCtx = p;
    execCtx.assignValueExpr = HOPEvalAssignValueExprCb;
    execCtx.assignValueExprCtx = p;
    execCtx.matchPattern = HOPEvalMatchPatternCb;
    execCtx.matchPatternCtx = p;
    execCtx.forInIndex = HOPEvalForInIndexCb;
    execCtx.forInIndexCtx = p;
    execCtx.forInIter = HOPEvalForInIterCb;
    execCtx.forInIterCtx = p;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = HOPCTFE_EXEC_DEFAULT_FOR_LIMIT;
    execCtx.skipConstBlocks = 1u;
    HOPCTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topVar->file;
    p->currentExecCtx = &execCtx;
    {
        int mirSupported = 0;
        rc = HOPEvalTryMirEvalTopInit(
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
            rc = HOPEvalTryMirZeroInitType(
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
        topVar->state = HOPEvalTopConstState_FAILED;
        return -1;
    }
    if (!isConst) {
        topVar->state = HOPEvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }
    topVar->value = value;
    topVar->state = HOPEvalTopConstState_READY;
    *outValue = value;
    *outIsConst = 1;
    return 0;
}

static int HOPEvalEvalTopConst(
    HOPEvalProgram* p, uint32_t topConstIndex, HOPCTFEValue* outValue, int* outIsConst) {
    HOPEvalTopConst*     topConst;
    const HOPParsedFile* savedFile;
    HOPCTFEExecCtx*      savedExecCtx;
    HOPCTFEExecCtx       constExecCtx;
    HOPCTFEValue         value;
    int                  isConst = 0;
    int                  rc;
    if (p == NULL || outValue == NULL || outIsConst == NULL || topConstIndex >= p->topConstLen) {
        return -1;
    }
    topConst = &p->topConsts[topConstIndex];
    if (topConst->state == HOPEvalTopConstState_READY) {
        *outValue = topConst->value;
        *outIsConst = 1;
        return 0;
    }
    if (topConst->state == HOPEvalTopConstState_VISITING
        || topConst->state == HOPEvalTopConstState_FAILED)
    {
        *outIsConst = 0;
        return 0;
    }
    if (topConst->initExprNode < 0) {
        topConst->state = HOPEvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }

    topConst->state = HOPEvalTopConstState_VISITING;
    memset(&constExecCtx, 0, sizeof(constExecCtx));
    constExecCtx.arena = p->arena;
    constExecCtx.ast = &topConst->file->ast;
    constExecCtx.src.ptr = topConst->file->source;
    constExecCtx.src.len = topConst->file->sourceLen;
    constExecCtx.evalExpr = HOPEvalExecExprCb;
    constExecCtx.evalExprCtx = p;
    constExecCtx.evalExprForType = HOPEvalExecExprForTypeCb;
    constExecCtx.evalExprForTypeCtx = p;
    constExecCtx.zeroInit = HOPEvalZeroInitCb;
    constExecCtx.zeroInitCtx = p;
    constExecCtx.assignExpr = HOPEvalAssignExprCb;
    constExecCtx.assignExprCtx = p;
    constExecCtx.assignValueExpr = HOPEvalAssignValueExprCb;
    constExecCtx.assignValueExprCtx = p;
    constExecCtx.matchPattern = HOPEvalMatchPatternCb;
    constExecCtx.matchPatternCtx = p;
    constExecCtx.forInIndex = HOPEvalForInIndexCb;
    constExecCtx.forInIndexCtx = p;
    constExecCtx.forInIter = HOPEvalForInIterCb;
    constExecCtx.forInIterCtx = p;
    constExecCtx.pendingReturnExprNode = -1;
    constExecCtx.forIterLimit = HOPCTFE_EXEC_DEFAULT_FOR_LIMIT;
    constExecCtx.skipConstBlocks = 1u;
    HOPCTFEExecResetReason(&constExecCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topConst->file;
    p->currentExecCtx = &constExecCtx;
    {
        rc = HOPEvalTypeValueFromExprNode(
            p, topConst->file, &topConst->file->ast, topConst->initExprNode, &value);
        if (rc < 0) {
            p->currentExecCtx = savedExecCtx;
            p->currentFile = savedFile;
            topConst->state = HOPEvalTopConstState_FAILED;
            return -1;
        }
        if (rc > 0) {
            rc = 0;
            isConst = 1;
        } else {
            int mirSupported = 0;
            rc = HOPEvalTryMirEvalTopInit(
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
        topConst->state = HOPEvalTopConstState_FAILED;
        return -1;
    }
    if (!isConst) {
        topConst->state = HOPEvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }

    topConst->value = value;
    topConst->state = HOPEvalTopConstState_READY;
    *outValue = value;
    *outIsConst = 1;
    return 0;
}

static int HOPEvalResolveIdent(
    void*         ctx,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*   p = (HOPEvalProgram*)ctx;
    const HOPPackage* currentPkg;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL || p->currentExecCtx == NULL
        || p->currentFile == NULL)
    {
        return -1;
    }
    currentPkg = HOPEvalFindPackageByFile(p, p->currentFile);
    {
        HOPCTFEExecBinding* binding = HOPEvalFindBinding(
            p->currentExecCtx, p->currentFile, nameStart, nameEnd);
        if (binding != NULL) {
            *outValue = binding->value;
            if (binding->typeNode >= 0) {
                int32_t typeCode = HOPEvalTypeCode_INVALID;
                if (!HOPEvalValueGetRuntimeTypeCode(outValue, &typeCode)
                    && HOPEvalTypeCodeFromTypeNode(p->currentFile, binding->typeNode, &typeCode))
                {
                    HOPEvalValueSetRuntimeTypeCode(outValue, typeCode);
                }
            }
            *outIsConst = 1;
            return 0;
        }
    }
    if (HOPCTFEExecEnvLookup(p->currentExecCtx, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    if (HOPEvalMirLookupLocalValue(p, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    if (p->activeTemplateParamFile != NULL && p->hasActiveTemplateTypeValue
        && (p->activeTemplateParamFile == p->currentFile
            || p->activeTemplateParamFile->source == p->currentFile->source)
        && SliceEqSlice(
            p->currentFile->source,
            nameStart,
            nameEnd,
            p->activeTemplateParamFile->source,
            p->activeTemplateParamNameStart,
            p->activeTemplateParamNameEnd)
        && p->activeTemplateTypeValue.kind == HOPCTFEValue_TYPE)
    {
        *outValue = p->activeTemplateTypeValue;
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t typeCode = HOPEvalTypeCode_INVALID;
        if (HOPEvalBuiltinTypeCode(p->currentFile->source, nameStart, nameEnd, &typeCode)) {
            HOPEvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
        if (HOPEvalResolveTypeValueName(p, p->currentFile, nameStart, nameEnd, outValue) > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    {
        if (currentPkg != NULL) {
            uint32_t i;
            for (i = 0; i < currentPkg->importLen; i++) {
                const HOPImportRef* imp = &currentPkg->imports[i];
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
                        HOPEvalValueSetPackageRef(outValue, (uint32_t)pkgIndex);
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
                ? HOPEvalFindTopVarBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : HOPEvalFindTopVarBySlice(p, p->currentFile, nameStart, nameEnd);
        if (topVarIndex >= 0) {
            int isConst = 0;
            if (HOPEvalEvalTopVar(p, (uint32_t)topVarIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst) {
                *outIsConst = 1;
                return 0;
            }
            HOPCTFEExecSetReason(
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
                ? HOPEvalFindTopConstBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : HOPEvalFindTopConstBySlice(p, p->currentFile, nameStart, nameEnd);
        if (topConstIndex >= 0) {
            int isConst = 0;
            if (HOPEvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst) {
                *outIsConst = 1;
                return 0;
            }
            HOPCTFEExecSetReason(
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
                ? HOPEvalFindAnyFunctionBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : HOPEvalFindAnyFunctionBySlice(p, p->currentFile, nameStart, nameEnd);
        if (fnIndex >= 0) {
            if (p->currentMirExecCtx != NULL && p->currentMirExecCtx->evalToMir != NULL
                && (uint32_t)fnIndex < p->currentMirExecCtx->evalToMirLen
                && p->currentMirExecCtx->evalToMir[(uint32_t)fnIndex] != UINT32_MAX)
            {
                HOPMirValueSetFunctionRef(
                    outValue, p->currentMirExecCtx->evalToMir[(uint32_t)fnIndex]);
            } else {
                HOPEvalValueSetFunctionRef(outValue, (uint32_t)fnIndex);
            }
            *outIsConst = 1;
            return 0;
        }
        if (fnIndex == -2) {
            HOPCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "function value is ambiguous in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
    }
    HOPCTFEExecSetReason(
        p->currentExecCtx, nameStart, nameEnd, "identifier is not available in evaluator backend");
    *outIsConst = 0;
    return 0;
}

static int HOPEvalResolveCallMirPre(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t      nameStart,
    uint32_t      nameEnd,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram* p = (HOPEvalProgram*)ctx;
    int32_t         callNode = -1;
    (void)function;
    (void)nameStart;
    (void)nameEnd;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!HOPEvalMirResolveCallNode(program, inst, &callNode)
        || !HOPEvalMirCallNodeIsLazyBuiltin(p, callNode))
    {
        return 0;
    }
    if (HOPEvalExecExprCb(p, callNode, outValue, outIsConst) != 0) {
        return -1;
    }
    return 1;
}

static int HOPEvalExpandMirSpreadLastArgs(
    HOPEvalProgram* p,
    const HOPMirInst* _Nullable inst,
    const HOPEvalFunction* fn,
    const HOPCTFEValue* _Nullable args,
    uint32_t             argCount,
    const HOPCTFEValue** outArgs,
    uint32_t*            outArgCount) {
    const HOPEvalArray* spreadArray;
    HOPCTFEValue*       expandedArgs;
    uint32_t            prefixCount;
    uint32_t            i;
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
    if (inst == NULL || !fn->isVariadic || !HOPMirCallTokHasSpreadLast(inst->tok)) {
        return 1;
    }
    if (argCount == 0u) {
        return 0;
    }
    spreadArray = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&args[argCount - 1u]));
    if (spreadArray == NULL || spreadArray->len > UINT32_MAX - (argCount - 1u)) {
        return 0;
    }
    if (spreadArray->len > 0 && spreadArray->elems == NULL) {
        return -1;
    }
    prefixCount = argCount - 1u;
    expandedArgs = (HOPCTFEValue*)HOPArenaAlloc(
        p->arena,
        sizeof(HOPCTFEValue) * (prefixCount + spreadArray->len),
        (uint32_t)_Alignof(HOPCTFEValue));
    if (expandedArgs == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < prefixCount; i++) {
        expandedArgs[i] = args[i];
        HOPEvalAnnotateUntypedLiteralValue(&expandedArgs[i]);
    }
    for (i = 0; i < spreadArray->len; i++) {
        expandedArgs[prefixCount + i] = spreadArray->elems[i];
        HOPEvalAnnotateUntypedLiteralValue(&expandedArgs[prefixCount + i]);
    }
    *outArgs = expandedArgs;
    *outArgCount = prefixCount + spreadArray->len;
    return 1;
}

static int HOPEvalResolveCallMir(
    void* ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    HOPEvalProgram*        p = (HOPEvalProgram*)ctx;
    int32_t                fnIndex = -1;
    const HOPEvalFunction* fn;
    int                    didReturn = 0;
    int                    isReflectKind = 0;
    int                    isReflectIsAlias = 0;
    int                    isReflectTypeName = 0;
    int                    isReflectBase = 0;
    int                    isTypeOf = 0;
    (void)program;
    (void)function;
    (void)inst;
    (void)diag;

    if (p == NULL || p->currentFile == NULL || p->currentExecCtx == NULL || outValue == NULL
        || outIsConst == NULL || (argCount > 0 && args == NULL))
    {
        return -1;
    }

    isReflectKind = HOPEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "kind", "reflect");
    isReflectIsAlias = HOPEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "is_alias", "reflect");
    isReflectTypeName = HOPEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "type_name", "reflect");
    isReflectBase = HOPEvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "base", "reflect");
    isTypeOf = SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "typeof");

    if (argCount == 1 && isReflectKind) {
        int32_t kind = 0;
        if (HOPEvalTypeKindOfValue(&args[0], &kind)) {
            HOPEvalValueSetInt(outValue, kind);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectIsAlias) {
        int32_t kind = 0;
        if (HOPEvalTypeKindOfValue(&args[0], &kind)) {
            outValue->kind = HOPCTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = kind == HOPEvalTypeKind_ALIAS ? 1u : 0u;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectTypeName) {
        if (HOPEvalTypeNameOfValue((HOPCTFEValue*)&args[0], outValue)) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectBase) {
        HOPEvalReflectedType* rt = HOPEvalValueAsReflectedType(&args[0]);
        if (rt != NULL && rt->kind == HOPEvalReflectType_NAMED
            && rt->namedKind == HOPEvalTypeKind_ALIAS && rt->file != NULL && rt->nodeId >= 0
            && (uint32_t)rt->nodeId < rt->file->ast.len)
        {
            int32_t baseTypeNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            if (baseTypeNode >= 0
                && HOPEvalTypeValueFromTypeNode(p, rt->file, baseTypeNode, outValue) > 0)
            {
                *outIsConst = 1;
                return 0;
            }
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "ptr")) {
        HOPEvalReflectedType* rt;
        if (args[0].kind == HOPCTFEValue_TYPE) {
            rt = (HOPEvalReflectedType*)HOPArenaAlloc(
                p->arena, sizeof(HOPEvalReflectedType), (uint32_t)_Alignof(HOPEvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = HOPEvalReflectType_PTR;
            rt->namedKind = HOPEvalTypeKind_POINTER;
            rt->elemType = args[0];
            HOPEvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "slice")) {
        HOPEvalReflectedType* rt;
        if (args[0].kind == HOPCTFEValue_TYPE) {
            rt = (HOPEvalReflectedType*)HOPArenaAlloc(
                p->arena, sizeof(HOPEvalReflectedType), (uint32_t)_Alignof(HOPEvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = HOPEvalReflectType_SLICE;
            rt->namedKind = HOPEvalTypeKind_SLICE;
            rt->elemType = args[0];
            HOPEvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "array")) {
        int64_t               arrayLen = 0;
        HOPEvalReflectedType* rt;
        if (args[0].kind == HOPCTFEValue_TYPE && HOPCTFEValueToInt64(&args[1], &arrayLen) == 0
            && arrayLen >= 0 && arrayLen <= (int64_t)UINT32_MAX)
        {
            rt = (HOPEvalReflectedType*)HOPArenaAlloc(
                p->arena, sizeof(HOPEvalReflectedType), (uint32_t)_Alignof(HOPEvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = HOPEvalReflectType_ARRAY;
            rt->namedKind = HOPEvalTypeKind_ARRAY;
            rt->arrayLen = (uint32_t)arrayLen;
            rt->elemType = args[0];
            HOPEvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "concat")) {
        int concatRc = HOPEvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
        if (concatRc < 0) {
            return -1;
        }
        if (concatRc > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "copy")) {
        int copyRc = HOPEvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
        if (copyRc < 0) {
            return -1;
        }
        if (copyRc > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isTypeOf) {
        int32_t           typeCode = HOPEvalTypeCode_INVALID;
        HOPEvalAggregate* agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(&args[0]));
        if (args[0].kind == HOPCTFEValue_TYPE) {
            HOPEvalValueSetSimpleTypeValue(outValue, HOPEvalTypeCode_TYPE);
            *outIsConst = 1;
            return 0;
        }
        if (agg != NULL && agg->file != NULL && agg->nodeId >= 0
            && (uint32_t)agg->nodeId < agg->file->ast.len)
        {
            uint8_t namedKind = 0;
            switch (agg->file->ast.nodes[agg->nodeId].kind) {
                case HOPAst_STRUCT: namedKind = HOPEvalTypeKind_STRUCT; break;
                case HOPAst_UNION:  namedKind = HOPEvalTypeKind_UNION; break;
                case HOPAst_ENUM:   namedKind = HOPEvalTypeKind_ENUM; break;
                default:            break;
            }
            if (namedKind != 0
                && HOPEvalMakeNamedTypeValue(p, agg->file, agg->nodeId, namedKind, outValue) > 0)
            {
                *outIsConst = 1;
                return 0;
            }
        }
        if (HOPEvalTypeCodeFromValue(&args[0], &typeCode)) {
            HOPEvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "len")) {
        const HOPCTFEValue* value = HOPEvalValueTargetOrSelf(&args[0]);
        if (value->kind == HOPCTFEValue_STRING) {
            HOPEvalValueSetInt(outValue, (int64_t)value->s.len);
            *outIsConst = 1;
            return 0;
        }
        if (value->kind == HOPCTFEValue_ARRAY) {
            HOPEvalValueSetInt(outValue, (int64_t)value->s.len);
            *outIsConst = 1;
            return 0;
        }
        if (value->kind == HOPCTFEValue_NULL) {
            HOPEvalValueSetInt(outValue, 0);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "cstr")) {
        const HOPCTFEValue* value = HOPEvalValueTargetOrSelf(&args[0]);
        if (value->kind == HOPCTFEValue_STRING) {
            *outValue = *value;
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount > 0) {
        const HOPCTFEValue* baseValue = HOPEvalValueTargetOrSelf(&args[0]);
        HOPEvalAggregate*   agg = HOPEvalValueAsAggregate(baseValue);
        HOPCTFEValue        fieldValue;
        if (agg != NULL
            && HOPEvalAggregateGetFieldValue(
                agg, p->currentFile->source, nameStart, nameEnd, &fieldValue)
            && HOPEvalValueIsInvokableFunctionRef(&fieldValue))
        {
            int invoked = HOPEvalInvokeFunctionRef(
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
        if (args[0].kind == HOPCTFEValue_STRING) {
            if (args[0].s.len > 0 && args[0].s.bytes != NULL) {
                if (fwrite(args[0].s.bytes, 1, args[0].s.len, stdout) != args[0].s.len) {
                    return ErrorSimple("failed to write print output");
                }
            }
            fputc('\n', stdout);
            fflush(stdout);
            HOPEvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        }
    }
    if ((argCount == 1 || argCount == 2)
        && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "free"))
    {
        HOPEvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (program != NULL && inst != NULL && argCount > 0) {
        int32_t           callNode = -1;
        const HOPPackage* targetPkg = NULL;
        if (HOPEvalMirResolveCallNode(program, inst, &callNode)) {
            targetPkg = HOPEvalMirResolveQualifiedImportCallTargetPkg(p, callNode);
        }
        if (targetPkg != NULL) {
            fnIndex = HOPEvalResolveFunctionBySlice(
                p, targetPkg, p->currentFile, nameStart, nameEnd, args + 1u, argCount - 1u);
            if (fnIndex == -2) {
                HOPCTFEExecSetReason(
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
        if (HOPEvalValueIsPackageRef(&args[0], &pkgIndex)) {
            const HOPPackage* targetPkg = NULL;
            if (p->loader == NULL || pkgIndex >= p->loader->packageLen) {
                return -1;
            }
            targetPkg = &p->loader->packages[pkgIndex];
            fnIndex = HOPEvalResolveFunctionBySlice(
                p, targetPkg, p->currentFile, nameStart, nameEnd, args + 1, argCount - 1u);
            if (fnIndex == -2) {
                HOPCTFEExecSetReason(
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
        HOPCTFEValue calleeValue;
        int          calleeIsConst = 0;
        const char*  savedReason = p->currentExecCtx->nonConstReason;
        uint32_t     savedStart = p->currentExecCtx->nonConstStart;
        uint32_t     savedEnd = p->currentExecCtx->nonConstEnd;
        if (HOPEvalResolveIdent(
                p, nameStart, nameEnd, &calleeValue, &calleeIsConst, p->currentExecCtx->diag)
            != 0)
        {
            return -1;
        }
        if (calleeIsConst && HOPEvalValueIsInvokableFunctionRef(&calleeValue)) {
            const HOPParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
            int32_t              savedExpectedTypeNode = p->activeCallExpectedTypeNode;
            int                  invoked;
            if (program != NULL && function != NULL && inst != NULL
                && p->activeCallExpectedTypeFile == NULL && p->currentMirExecCtx != NULL)
            {
                uint32_t instIndex = UINT32_MAX;
                if (program->insts != NULL && inst >= program->insts
                    && inst < program->insts + program->instLen)
                {
                    instIndex = (uint32_t)(inst - program->insts);
                }
                if (instIndex != UINT32_MAX && instIndex + 1u < program->instLen
                    && program->insts[instIndex + 1u].op == HOPMirOp_LOCAL_STORE)
                {
                    uint32_t localSlot = program->insts[instIndex + 1u].aux;
                    if (localSlot < function->localCount
                        && function->localStart + localSlot < program->localLen)
                    {
                        const HOPMirLocal* local =
                            &program->locals[function->localStart + localSlot];
                        if (local->typeRef < program->typeLen
                            && program->types[local->typeRef].sourceRef
                                   < p->currentMirExecCtx->sourceFileCap)
                        {
                            p->activeCallExpectedTypeFile =
                                p->currentMirExecCtx
                                    ->sourceFiles[program->types[local->typeRef].sourceRef];
                            p->activeCallExpectedTypeNode =
                                (int32_t)program->types[local->typeRef].astNode;
                        }
                    }
                }
            }
            invoked = HOPEvalInvokeFunctionRef(
                p, &calleeValue, args, argCount, outValue, outIsConst);
            p->activeCallExpectedTypeFile = savedExpectedTypeFile;
            p->activeCallExpectedTypeNode = savedExpectedTypeNode;
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
        fnIndex = HOPEvalResolveFunctionBySlice(
            p, NULL, p->currentFile, nameStart, nameEnd, args, argCount);
    }
    if (fnIndex == -2) {
        HOPCTFEExecSetReason(
            p->currentExecCtx, nameStart, nameEnd, "call target is ambiguous in evaluator backend");
        *outIsConst = 0;
        return 0;
    }
    if (fnIndex < 0) {
        HOPCTFEExecSetReason(
            p->currentExecCtx,
            nameStart,
            nameEnd,
            "call target is not supported by evaluator backend");
        *outIsConst = 0;
        return 0;
    }
    fn = &p->funcs[fnIndex];
    {
        const HOPCTFEValue* invokeArgs = args;
        uint32_t            invokeArgCount = argCount;
        int                 expandRc = HOPEvalExpandMirSpreadLastArgs(
            p, inst, fn, args, argCount, &invokeArgs, &invokeArgCount);
        if (expandRc < 0) {
            return -1;
        }
        if (expandRc == 0) {
            HOPCTFEExecSetReason(
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

    if (p->callDepth >= HOP_EVAL_CALL_MAX_DEPTH) {
        HOPCTFEExecSetReason(
            p->currentExecCtx, nameStart, nameEnd, "evaluator backend call depth limit exceeded");
        *outIsConst = 0;
        return 0;
    }
    {
        uint32_t i;
        for (i = 0; i < p->callDepth; i++) {
            if (p->callStack[i] == (uint32_t)fnIndex) {
                HOPCTFEExecSetReason(
                    p->currentExecCtx,
                    nameStart,
                    nameEnd,
                    "recursive calls are not supported by evaluator backend");
                *outIsConst = 0;
                return 0;
            }
        }
    }

    {
        HOPEvalTemplateBindingState savedBinding;
        HOPEvalSaveTemplateBinding(p, &savedBinding);
        (void)HOPEvalBindActiveTemplateForMirCall(p, program, function, inst, fn, args, argCount);
        if (HOPEvalInvokeFunction(
                p, fnIndex, args, argCount, p->currentContext, outValue, &didReturn)
            != 0)
        {
            HOPEvalRestoreTemplateBinding(p, &savedBinding);
            return -1;
        }
        HOPEvalRestoreTemplateBinding(p, &savedBinding);
    }
    if (!didReturn) {
        if (fn->hasReturnType) {
            HOPCTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "function returned without a value in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
        HOPEvalValueSetNull(outValue);
    }
    *outIsConst = 1;
    return 0;
}

static int HOPEvalResolveCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPCTFEValue* _Nullable args,
    uint32_t      argCount,
    HOPCTFEValue* outValue,
    int*          outIsConst,
    HOPDiag* _Nullable diag) {
    return HOPEvalResolveCallMir(
        ctx, NULL, NULL, NULL, nameStart, nameEnd, args, argCount, outValue, outIsConst, diag);
}

static int HOPEvalExecExprCb(void* ctx, int32_t exprNode, HOPCTFEValue* outValue, int* outIsConst) {
    HOPEvalProgram*   p = (HOPEvalProgram*)ctx;
    const HOPAst*     ast;
    const HOPAstNode* n;
    HOPDiag           diag = { 0 };
    int               rc;

    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return -1;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == HOPAst_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return -1;
        }
        n = &ast->nodes[exprNode];
    }

    if (n->kind == HOPAst_COMPOUND_LIT) {
        return HOPEvalEvalCompoundLiteral(
            p, exprNode, p->currentFile, p->currentFile, -1, outValue, outIsConst);
    }

    if (n->kind == HOPAst_NEW) {
        return HOPEvalEvalNewExpr(p, exprNode, NULL, -1, outValue, outIsConst);
    }

    if (n->kind == HOPAst_CALL_WITH_CONTEXT) {
        int32_t               callNode = n->firstChild;
        int32_t               overlayNode = callNode >= 0 ? ast->nodes[callNode].nextSibling : -1;
        const HOPEvalContext* savedContext;
        HOPEvalContext        overlayContext;
        int                   overlayRc;
        if (callNode < 0 || (uint32_t)callNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        overlayRc = HOPEvalBuildContextOverlay(p, overlayNode, &overlayContext, p->currentFile);
        if (overlayRc != 1) {
            *outIsConst = 0;
            return overlayRc < 0 ? -1 : 0;
        }
        savedContext = p->currentContext;
        p->currentContext = &overlayContext;
        rc = HOPEvalExecExprCb(p, callNode, outValue, outIsConst);
        p->currentContext = savedContext;
        return rc;
    }

    if (n->kind == HOPAst_SIZEOF) {
        int32_t      childNode = n->firstChild;
        uint64_t     sizeBytes = 0;
        HOPCTFEValue childValue;
        int          childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (n->flags == 1u) {
            if (!HOPEvalTypeNodeSize(p->currentFile, childNode, &sizeBytes, 0)) {
                *outIsConst = 0;
                return 0;
            }
        } else {
            if (HOPEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
                return -1;
            }
            if (!childIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (childValue.kind == HOPCTFEValue_BOOL) {
                sizeBytes = 1u;
            } else if (childValue.kind == HOPCTFEValue_INT || childValue.kind == HOPCTFEValue_FLOAT)
            {
                sizeBytes = 8u;
            } else if (childValue.kind == HOPCTFEValue_STRING) {
                sizeBytes = (uint64_t)(sizeof(void*) * 2u);
            } else if (childValue.kind == HOPCTFEValue_ARRAY) {
                sizeBytes = (uint64_t)childValue.s.len * 8u;
            } else if (
                childValue.kind == HOPCTFEValue_REFERENCE || childValue.kind == HOPCTFEValue_NULL)
            {
                sizeBytes = (uint64_t)sizeof(void*);
            } else {
                *outIsConst = 0;
                return 0;
            }
        }
        HOPEvalValueSetInt(outValue, (int64_t)sizeBytes);
        *outIsConst = 1;
        return 0;
    }

    if (n->kind == HOPAst_INDEX && (n->flags & HOPAstFlag_INDEX_SLICE) != 0u) {
        int32_t             baseNode = n->firstChild;
        int32_t             extraNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        HOPCTFEValue        baseValue;
        const HOPCTFEValue* targetValue;
        HOPEvalArray*       array;
        HOPEvalArray*       view;
        HOPCTFEValue        startValue;
        HOPCTFEValue        endValue;
        int                 baseIsConst = 0;
        int                 startIsConst = 0;
        int                 endIsConst = 0;
        int64_t             start = 0;
        int64_t             end = -1;
        uint32_t            startIndex;
        uint32_t            endIndex;
        if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
            return -1;
        }
        if (!baseIsConst) {
            *outIsConst = 0;
            return 0;
        }
        targetValue = HOPEvalValueTargetOrSelf(&baseValue);
        array = HOPEvalValueAsArray(targetValue);
        if ((n->flags & HOPAstFlag_INDEX_HAS_START) != 0u) {
            if (extraNode < 0 || (uint32_t)extraNode >= ast->len
                || HOPEvalExecExprCb(p, extraNode, &startValue, &startIsConst) != 0)
            {
                return extraNode < 0 ? 0 : -1;
            }
            if (!startIsConst || HOPCTFEValueToInt64(&startValue, &start) != 0 || start < 0) {
                *outIsConst = 0;
                return 0;
            }
            extraNode = ast->nodes[extraNode].nextSibling;
        }
        if ((n->flags & HOPAstFlag_INDEX_HAS_END) != 0u) {
            if (extraNode < 0 || (uint32_t)extraNode >= ast->len
                || HOPEvalExecExprCb(p, extraNode, &endValue, &endIsConst) != 0)
            {
                return extraNode < 0 ? 0 : -1;
            }
            if (!endIsConst || HOPCTFEValueToInt64(&endValue, &end) != 0 || end < 0) {
                *outIsConst = 0;
                return 0;
            }
            extraNode = ast->nodes[extraNode].nextSibling;
        }
        if (extraNode >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (array == NULL && targetValue->kind == HOPCTFEValue_STRING) {
            int32_t currentTypeCode = HOPEvalTypeCode_INVALID;
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
            if (!HOPEvalValueGetRuntimeTypeCode(targetValue, &currentTypeCode)) {
                currentTypeCode = HOPEvalTypeCode_STR_REF;
            }
            HOPEvalValueSetRuntimeTypeCode(outValue, currentTypeCode);
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
        view = HOPEvalAllocArrayView(
            p,
            targetValue->kind == HOPCTFEValue_ARRAY ? array->file : p->currentFile,
            exprNode,
            array->elemTypeNode,
            array->elems + startIndex,
            endIndex - startIndex);
        if (view == NULL) {
            return ErrorSimple("out of memory");
        }
        {
            HOPCTFEValue viewValue;
            HOPEvalValueSetArray(&viewValue, p->currentFile, exprNode, view);
            return HOPEvalAllocReferencedValue(p, &viewValue, outValue, outIsConst);
        }
    }

    if (n->kind == HOPAst_INDEX && (n->flags & 0x7u) == 0u) {
        int32_t       baseNode = n->firstChild;
        int32_t       indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        HOPCTFEValue  baseValue;
        HOPCTFEValue  indexValue;
        HOPEvalArray* array;
        int           baseIsConst = 0;
        int           indexIsConst = 0;
        int64_t       index = 0;
        if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
            goto index_fallback;
        }
        if (HOPEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || HOPEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || HOPCTFEValueToInt64(&indexValue, &index) != 0) {
            goto index_fallback;
        }
        array = HOPEvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            *outValue = array->elems[(uint32_t)index];
            *outIsConst = 1;
            return 0;
        }
        {
            const HOPCTFEValue* targetValue = HOPEvalValueTargetOrSelf(&baseValue);
            if (targetValue->kind == HOPCTFEValue_STRING && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len)
            {
                HOPEvalValueSetInt(outValue, (int64_t)targetValue->s.bytes[(uint32_t)index]);
                HOPEvalValueSetRuntimeTypeCode(outValue, HOPEvalTypeCode_U8);
                *outIsConst = 1;
                return 0;
            }
        }
        if (index < 0) {
            goto index_fallback;
        }
    index_fallback:;
    }

    if (n->kind == HOPAst_FIELD_EXPR) {
        int32_t           baseNode = n->firstChild;
        HOPCTFEValue      baseValue;
        int               baseIsConst = 0;
        HOPEvalAggregate* agg = NULL;
        if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[baseNode].kind == HOPAst_IDENT) {
            if (SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "context")
                && HOPEvalCurrentContextField(
                    p, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
            {
                *outIsConst = 1;
                return 0;
            }
            const HOPParsedFile* enumFile = NULL;
            int32_t              enumNode = -1;
            enumNode = HOPEvalFindNamedEnumDecl(
                p,
                p->currentFile,
                ast->nodes[baseNode].dataStart,
                ast->nodes[baseNode].dataEnd,
                &enumFile);
            if (enumNode >= 0 && enumFile != NULL) {
                int32_t  variantNode = -1;
                uint32_t tagIndex = 0;
                if (HOPEvalFindEnumVariant(
                        enumFile,
                        enumNode,
                        p->currentFile->source,
                        n->dataStart,
                        n->dataEnd,
                        &variantNode,
                        &tagIndex))
                {
                    const HOPAstNode* variantField = &enumFile->ast.nodes[variantNode];
                    int32_t           valueNode = ASTFirstChild(&enumFile->ast, variantNode);
                    if (valueNode >= 0 && enumFile->ast.nodes[valueNode].kind != HOPAst_FIELD
                        && !HOPEvalEnumHasPayloadVariants(enumFile, enumNode))
                    {
                        HOPCTFEValue enumValue;
                        int          enumIsConst = 0;
                        if (HOPEvalExecExprInFileWithType(
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
                    HOPEvalValueSetTaggedEnum(
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
        if (HOPEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
            return -1;
        }
        if (!baseIsConst) {
            *outIsConst = 0;
            return 0;
        }
        {
            const HOPCTFEValue* targetValue = HOPEvalValueTargetOrSelf(&baseValue);
            const HOPCTFEValue* payload = NULL;
            HOPEvalTaggedEnum*  tagged = HOPEvalValueAsTaggedEnum(targetValue);
            if (targetValue->kind == HOPCTFEValue_OPTIONAL
                && HOPEvalOptionalPayload(targetValue, &payload) && targetValue->b != 0u
                && payload != NULL)
            {
                targetValue = HOPEvalValueTargetOrSelf(payload);
                tagged = HOPEvalValueAsTaggedEnum(targetValue);
            }
            if (SliceEqCStr(p->currentFile->source, n->dataStart, n->dataEnd, "len")
                && (targetValue->kind == HOPCTFEValue_STRING
                    || targetValue->kind == HOPCTFEValue_ARRAY
                    || targetValue->kind == HOPCTFEValue_NULL))
            {
                HOPEvalValueSetInt(outValue, (int64_t)targetValue->s.len);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(p->currentFile->source, n->dataStart, n->dataEnd, "cstr")
                && targetValue->kind == HOPCTFEValue_STRING)
            {
                *outValue = *targetValue;
                *outIsConst = 1;
                return 0;
            }
            if (tagged != NULL && tagged->payload != NULL
                && HOPEvalAggregateGetFieldValue(
                    tagged->payload, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
            {
                *outIsConst = 1;
                return 0;
            }
        }
        agg = HOPEvalValueAsAggregate(&baseValue);
        if (agg == NULL) {
            agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(&baseValue));
        }
        if (agg == NULL) {
            const HOPCTFEValue* targetValue = HOPEvalValueTargetOrSelf(&baseValue);
            const HOPCTFEValue* payload = NULL;
            if (targetValue->kind == HOPCTFEValue_OPTIONAL
                && HOPEvalOptionalPayload(targetValue, &payload) && targetValue->b != 0u
                && payload != NULL)
            {
                agg = HOPEvalValueAsAggregate(HOPEvalValueTargetOrSelf(payload));
            }
        }
        if (agg != NULL
            && HOPEvalAggregateGetFieldValue(
                agg, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
        {
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == HOPAst_UNARY && (HOPTokenKind)n->op == HOPTok_AND) {
        int32_t       childNode = n->firstChild;
        HOPCTFEValue  childValue;
        HOPCTFEValue* fieldValuePtr;
        int           childIsConst = 0;
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == HOPAst_IDENT)
        {
            HOPCTFEExecBinding* binding = HOPEvalFindBinding(
                p->currentExecCtx,
                p->currentFile,
                ast->nodes[childNode].dataStart,
                ast->nodes[childNode].dataEnd);
            if (binding != NULL) {
                HOPEvalValueSetReference(outValue, &binding->value);
                *outIsConst = 1;
                return 0;
            }
            {
                int32_t topVarIndex = HOPEvalFindCurrentTopVarBySlice(
                    p,
                    p->currentFile,
                    ast->nodes[childNode].dataStart,
                    ast->nodes[childNode].dataEnd);
                HOPCTFEValue topVarValue;
                int          topVarIsConst = 0;
                if (topVarIndex >= 0
                    && HOPEvalEvalTopVar(p, (uint32_t)topVarIndex, &topVarValue, &topVarIsConst)
                           == 0
                    && topVarIsConst)
                {
                    HOPEvalValueSetReference(outValue, &p->topVars[(uint32_t)topVarIndex].value);
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == HOPAst_FIELD_EXPR)
        {
            fieldValuePtr = HOPEvalResolveFieldExprValuePtr(p, p->currentExecCtx, childNode);
            if (fieldValuePtr != NULL) {
                HOPEvalValueSetReference(outValue, fieldValuePtr);
                *outIsConst = 1;
                return 0;
            }
        }
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == HOPAst_INDEX
            && (ast->nodes[childNode].flags & 0x7u) == 0u)
        {
            int32_t       baseNode = ast->nodes[childNode].firstChild;
            int32_t       indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
            HOPCTFEValue  baseValue;
            HOPCTFEValue  indexValue;
            HOPEvalArray* array;
            int           baseIsConst = 0;
            int           indexIsConst = 0;
            int64_t       index = 0;
            if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
                goto unary_addr_fallback;
            }
            if (HOPEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
                || HOPEvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
            {
                return -1;
            }
            if (!baseIsConst || !indexIsConst || HOPCTFEValueToInt64(&indexValue, &index) != 0) {
                goto unary_addr_fallback;
            }
            array = HOPEvalValueAsArray(&baseValue);
            if (array == NULL) {
                array = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&baseValue));
            }
            if (array == NULL || index < 0 || (uint64_t)index >= (uint64_t)array->len) {
                goto unary_addr_fallback;
            }
            HOPEvalValueSetReference(outValue, &array->elems[(uint32_t)index]);
            *outIsConst = 1;
            return 0;
        unary_addr_fallback:;
        }
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (childValue.kind == HOPCTFEValue_AGGREGATE || childValue.kind == HOPCTFEValue_NULL
            || childValue.kind == HOPCTFEValue_ARRAY)
        {
            *outValue = childValue;
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == HOPAst_UNARY && (HOPTokenKind)n->op == HOPTok_MUL) {
        int32_t       childNode = n->firstChild;
        HOPCTFEValue  childValue;
        HOPCTFEValue* target;
        int           childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        target = HOPEvalValueReferenceTarget(&childValue);
        if (target != NULL) {
            *outValue = *target;
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == HOPAst_UNWRAP) {
        int32_t             childNode = n->firstChild;
        HOPCTFEValue        childValue;
        const HOPCTFEValue* payload = NULL;
        int                 childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (!HOPEvalOptionalPayload(&childValue, &payload)) {
            if (childValue.kind == HOPCTFEValue_NULL) {
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

    if (n->kind == HOPAst_TUPLE_EXPR || n->kind == HOPAst_EXPR_LIST) {
        HOPCTFEValue elems[256];
        uint32_t     elemCount = AstListCount(ast, exprNode);
        uint32_t     i;
        if (elemCount == 0 || elemCount > 256u) {
            *outIsConst = 0;
            return 0;
        }
        for (i = 0; i < elemCount; i++) {
            int32_t itemNode = AstListItemAt(ast, exprNode, i);
            int     elemIsConst = 0;
            if (itemNode < 0 || HOPEvalExecExprCb(p, itemNode, &elems[i], &elemIsConst) != 0) {
                return itemNode < 0 ? 0 : -1;
            }
            if (!elemIsConst) {
                *outIsConst = 0;
                return 0;
            }
        }
        return HOPEvalAllocTupleValue(
            p, p->currentFile, exprNode, elems, elemCount, outValue, outIsConst);
    }

    if (n->kind == HOPAst_TYPE_VALUE) {
        int32_t typeNode = ASTFirstChild(ast, exprNode);
        int     rc = HOPEvalTypeValueFromTypeNode(p, p->currentFile, typeNode, outValue);
        if (rc < 0) {
            return -1;
        }
        if (rc > 0) {
            *outIsConst = 1;
        } else {
            *outIsConst = 0;
        }
        return 0;
    }

    if (n->kind == HOPAst_UNARY) {
        int32_t      childNode = n->firstChild;
        HOPCTFEValue childValue;
        int          childIsConst = 0;
        int          handled = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        handled = HOPEvalEvalUnary((HOPTokenKind)n->op, &childValue, outValue, outIsConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            return 0;
        }
    }

    if (n->kind == HOPAst_BINARY && (HOPTokenKind)n->op != HOPTok_ASSIGN) {
        int32_t      lhsNode = n->firstChild;
        int32_t      rhsNode = lhsNode >= 0 ? ast->nodes[lhsNode].nextSibling : -1;
        HOPCTFEValue lhsValue;
        HOPCTFEValue rhsValue;
        int          lhsIsConst = 0;
        int          rhsIsConst = 0;
        int          handled = 0;
        if (lhsNode < 0 || rhsNode < 0 || ast->nodes[rhsNode].nextSibling >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (HOPEvalExecExprCb(p, lhsNode, &lhsValue, &lhsIsConst) != 0
            || HOPEvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0)
        {
            return -1;
        }
        if (!lhsIsConst || !rhsIsConst) {
            *outIsConst = 0;
            return 0;
        }
        handled = HOPEvalEvalBinary(
            p, (HOPTokenKind)n->op, &lhsValue, &rhsValue, outValue, outIsConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            return 0;
        }
    }

    if (n->kind == HOPAst_CALL) {
        int32_t calleeNode = n->firstChild;
        if (calleeNode < 0 || (uint32_t)calleeNode >= ast->len) {
            if (p->currentExecCtx != NULL) {
                HOPCTFEExecSetReasonNode(
                    p->currentExecCtx, exprNode, "call expression is malformed");
            }
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == HOPAst_IDENT
            && SliceEqCStr(
                p->currentFile->source,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                "typeof"))
        {
            HOPCTFEExecBinding*  binding = NULL;
            const HOPParsedFile* localTypeFile = NULL;
            int32_t              argNode = ast->nodes[calleeNode].nextSibling;
            int32_t              argExprNode = argNode;
            int32_t              localTypeNode = -1;
            int32_t              visibleLocalTypeNode = -1;
            HOPCTFEValue         argValue;
            int                  argIsConst = 0;
            int32_t              typeCode = HOPEvalTypeCode_INVALID;
            if (argNode < 0 || ast->nodes[argNode].nextSibling >= 0) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[argNode].kind == HOPAst_CALL_ARG) {
                argExprNode = ast->nodes[argNode].firstChild;
            }
            if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[argExprNode].kind == HOPAst_IDENT) {
                binding = HOPEvalFindBinding(
                    p->currentExecCtx,
                    p->currentFile,
                    ast->nodes[argExprNode].dataStart,
                    ast->nodes[argExprNode].dataEnd);
                if (binding != NULL && binding->typeNode >= 0
                    && !HOPEvalTypeNodeIsAnytype(p->currentFile, binding->typeNode))
                {
                    rc = HOPEvalTypeValueFromTypeNode(
                        p, p->currentFile, binding->typeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
                if (HOPEvalFindVisibleLocalTypeNodeByName(
                        p->currentFile,
                        ast->nodes[argExprNode].start,
                        ast->nodes[argExprNode].dataStart,
                        ast->nodes[argExprNode].dataEnd,
                        &visibleLocalTypeNode)
                    && visibleLocalTypeNode >= 0
                    && !HOPEvalTypeNodeIsAnytype(p->currentFile, visibleLocalTypeNode))
                {
                    rc = HOPEvalTypeValueFromTypeNode(
                        p, p->currentFile, visibleLocalTypeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
                if (HOPEvalMirLookupLocalTypeNode(
                        p,
                        ast->nodes[argExprNode].dataStart,
                        ast->nodes[argExprNode].dataEnd,
                        &localTypeFile,
                        &localTypeNode)
                    && localTypeFile != NULL && localTypeNode >= 0
                    && !HOPEvalTypeNodeIsAnytype(localTypeFile, localTypeNode))
                {
                    rc = HOPEvalTypeValueFromTypeNode(p, localTypeFile, localTypeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
            }
            rc = HOPEvalTypeValueFromExprNode(p, p->currentFile, ast, argExprNode, outValue);
            if (rc < 0) {
                return -1;
            }
            if (rc > 0) {
                if (outValue->kind == HOPCTFEValue_TYPE) {
                    HOPEvalValueSetSimpleTypeValue(outValue, HOPEvalTypeCode_TYPE);
                }
                *outIsConst = 1;
                return 0;
            }
            if (ast->nodes[argExprNode].kind == HOPAst_CAST) {
                int32_t typeNode = ast->nodes[argExprNode].firstChild;
                typeNode = typeNode >= 0 ? ast->nodes[typeNode].nextSibling : -1;
                if (typeNode >= 0
                    && HOPEvalTypeCodeFromTypeNode(p->currentFile, typeNode, &typeCode))
                {
                    HOPEvalValueSetSimpleTypeValue(outValue, typeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (HOPEvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
                return -1;
            }
            if (!argIsConst) {
                *outIsConst = 0;
                return 0;
            }
            HOPEvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
            {
                HOPEvalAggregate* agg = HOPEvalValueAsAggregate(
                    HOPEvalValueTargetOrSelf(&argValue));
                if (agg != NULL && agg->file != NULL && agg->nodeId >= 0
                    && (uint32_t)agg->nodeId < agg->file->ast.len)
                {
                    uint8_t namedKind = 0;
                    switch (agg->file->ast.nodes[agg->nodeId].kind) {
                        case HOPAst_STRUCT: namedKind = HOPEvalTypeKind_STRUCT; break;
                        case HOPAst_UNION:  namedKind = HOPEvalTypeKind_UNION; break;
                        case HOPAst_ENUM:   namedKind = HOPEvalTypeKind_ENUM; break;
                        default:            break;
                    }
                    if (namedKind != 0
                        && HOPEvalMakeNamedTypeValue(p, agg->file, agg->nodeId, namedKind, outValue)
                               > 0)
                    {
                        *outIsConst = 1;
                        return 0;
                    }
                }
            }
            if (!HOPEvalTypeCodeFromValue(&argValue, &typeCode)) {
                *outIsConst = 0;
                return 0;
            }
            HOPEvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == HOPAst_IDENT
            && HOPEvalNameEqLiteralOrPkgBuiltin(
                p->currentFile->source,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                "source_location_of",
                "builtin"))
        {
            int32_t argNode = ast->nodes[calleeNode].nextSibling;
            int32_t operandNode = argNode;
            if (argNode < 0 || ast->nodes[argNode].nextSibling >= 0) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[operandNode].kind == HOPAst_CALL_ARG) {
                operandNode = ast->nodes[operandNode].firstChild;
            }
            if (operandNode < 0 || (uint32_t)operandNode >= ast->len) {
                if (p->currentExecCtx != NULL) {
                    HOPCTFEExecSetReason(
                        p->currentExecCtx,
                        ast->nodes[argNode].start,
                        ast->nodes[argNode].end,
                        "source_location_of argument is malformed");
                }
                *outIsConst = 0;
                return 0;
            }
            HOPEvalValueSetSpan(
                p->currentFile,
                ast->nodes[operandNode].start,
                ast->nodes[operandNode].end,
                outValue);
            *outIsConst = 1;
            return 0;
        }
        if (calleeNode >= 0 && ast->nodes[calleeNode].kind == HOPAst_FIELD_EXPR) {
            const HOPAstNode* callee = &ast->nodes[calleeNode];
            int32_t           baseNode = callee->firstChild;
            int32_t           argNode = ast->nodes[calleeNode].nextSibling;
            HOPCTFEValue      calleeValue;
            int               calleeIsConst = 0;
            if (HOPEvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                return -1;
            }
            if (calleeIsConst && HOPEvalValueIsInvokableFunctionRef(&calleeValue)) {
                uint32_t      argCount = 0;
                HOPCTFEValue* args = NULL;
                int collectRc = HOPEvalCollectCallArgs(p, ast, argNode, &args, &argCount, NULL);
                if (collectRc <= 0) {
                    *outIsConst = 0;
                    return collectRc < 0 ? -1 : 0;
                }
                {
                    const HOPParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                    int32_t              savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                    const HOPParsedFile* inferredExpectedTypeFile = NULL;
                    int32_t              inferredExpectedTypeNode = -1;
                    int                  invoked;
                    if (p->expectedCallExprFile == p->currentFile
                        && p->expectedCallExprNode == exprNode)
                    {
                        p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                        p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                    } else if (
                        HOPEvalFindExpectedTypeForCallExpr(
                            p->currentFile,
                            exprNode,
                            &inferredExpectedTypeFile,
                            &inferredExpectedTypeNode))
                    {
                        p->activeCallExpectedTypeFile = inferredExpectedTypeFile;
                        p->activeCallExpectedTypeNode = inferredExpectedTypeNode;
                    }
                    invoked = HOPEvalInvokeFunctionRef(
                        p, &calleeValue, args, argCount, outValue, outIsConst);
                    p->activeCallExpectedTypeFile = savedExpectedTypeFile;
                    p->activeCallExpectedTypeNode = savedExpectedTypeNode;
                    if (invoked < 0) {
                        return -1;
                    }
                    if (invoked > 0) {
                        return 0;
                    }
                }
            }
            if (baseNode >= 0 && argNode >= 0 && ast->nodes[argNode].nextSibling < 0
                && ast->nodes[baseNode].kind == HOPAst_IDENT
                && SliceEqCStr(
                    p->currentFile->source,
                    callee->dataStart,
                    callee->dataEnd,
                    "source_location_of")
                && SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "builtin"))
            {
                int32_t operandNode = argNode;
                if (ast->nodes[operandNode].kind == HOPAst_CALL_ARG) {
                    operandNode = ast->nodes[operandNode].firstChild;
                }
                if (operandNode < 0 || (uint32_t)operandNode >= ast->len) {
                    if (p->currentExecCtx != NULL) {
                        HOPCTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[argNode].start,
                            ast->nodes[argNode].end,
                            "builtin.source_location_of argument is malformed");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                HOPEvalValueSetSpan(
                    p->currentFile,
                    ast->nodes[operandNode].start,
                    ast->nodes[operandNode].end,
                    outValue);
                *outIsConst = 1;
                return 0;
            }
            if (baseNode >= 0 && argNode >= 0 && ast->nodes[argNode].nextSibling < 0
                && ast->nodes[baseNode].kind == HOPAst_IDENT
                && SliceEqCStr(p->currentFile->source, callee->dataStart, callee->dataEnd, "exit")
                && SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "platform"))
            {
                HOPCTFEValue argValue;
                int          argIsConst = 0;
                int64_t      exitCode = 0;
                if (HOPEvalExecExprCb(p, argNode, &argValue, &argIsConst) != 0) {
                    return -1;
                }
                if (!argIsConst || HOPCTFEValueToInt64(&argValue, &exitCode) != 0) {
                    if (p->currentExecCtx != NULL) {
                        HOPCTFEExecSetReason(
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
                HOPEvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (baseNode >= 0 && argNode >= 0 && ast->nodes[baseNode].kind == HOPAst_IDENT
                && SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "platform")
                && SliceEqCStr(
                    p->currentFile->source, callee->dataStart, callee->dataEnd, "console_log"))
            {
                int32_t      flagsNode = ast->nodes[argNode].nextSibling;
                int32_t      extraNode = flagsNode >= 0 ? ast->nodes[flagsNode].nextSibling : -1;
                int32_t      messageExpr = ASTFirstChild(ast, argNode);
                int32_t      flagsExpr = ASTFirstChild(ast, flagsNode);
                HOPCTFEValue messageValue;
                HOPCTFEValue flagsValue;
                int          messageIsConst = 0;
                int          flagsIsConst = 0;
                if (flagsNode < 0 || extraNode >= 0 || messageExpr < 0 || flagsExpr < 0) {
                    *outIsConst = 0;
                    return 0;
                }
                if (HOPEvalExecExprCb(p, messageExpr, &messageValue, &messageIsConst) != 0
                    || HOPEvalExecExprCb(p, flagsExpr, &flagsValue, &flagsIsConst) != 0)
                {
                    return -1;
                }
                if (!messageIsConst || messageValue.kind != HOPCTFEValue_STRING || !flagsIsConst) {
                    if (p->currentExecCtx != NULL) {
                        HOPCTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[messageExpr].start,
                            ast->nodes[flagsExpr].end,
                            "platform.console_log requires a string expression and integer flags");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                if (messageValue.s.len > 0 && messageValue.s.bytes != NULL) {
                    if (fwrite(messageValue.s.bytes, 1, messageValue.s.len, stdout)
                        != messageValue.s.len)
                    {
                        return ErrorSimple("failed to write console_log output");
                    }
                }
                fputc('\n', stdout);
                fflush(stdout);
                HOPEvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (baseNode >= 0) {
                uint32_t      extraArgCount = 0;
                HOPCTFEValue* args = NULL;
                HOPCTFEValue* extraArgs = NULL;
                HOPCTFEValue  baseValue;
                int           baseIsConst = 0;
                int           collectRc;
                if (HOPEvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
                    return -1;
                }
                if (baseIsConst) {
                    collectRc = HOPEvalCollectCallArgs(
                        p, ast, argNode, &extraArgs, &extraArgCount, NULL);
                    if (collectRc <= 0) {
                        *outIsConst = 0;
                        return collectRc < 0 ? -1 : 0;
                    }
                    args = (HOPCTFEValue*)HOPArenaAlloc(
                        p->arena,
                        sizeof(HOPCTFEValue) * (extraArgCount + 1u),
                        (uint32_t)_Alignof(HOPCTFEValue));
                    if (args == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    args[0] = baseValue;
                    if (extraArgCount > 0 && extraArgs != NULL) {
                        memcpy(args + 1u, extraArgs, sizeof(HOPCTFEValue) * extraArgCount);
                    }

                    if (extraArgCount == 0
                        && SliceEqCStr(
                            p->currentFile->source, callee->dataStart, callee->dataEnd, "len")
                        && (HOPEvalValueTargetOrSelf(&baseValue)->kind == HOPCTFEValue_STRING
                            || HOPEvalValueTargetOrSelf(&baseValue)->kind == HOPCTFEValue_ARRAY
                            || HOPEvalValueTargetOrSelf(&baseValue)->kind == HOPCTFEValue_NULL))
                    {
                        HOPEvalValueSetInt(
                            outValue, (int64_t)HOPEvalValueTargetOrSelf(&baseValue)->s.len);
                        *outIsConst = 1;
                        return 0;
                    }
                    if (extraArgCount == 0
                        && SliceEqCStr(
                            p->currentFile->source, callee->dataStart, callee->dataEnd, "cstr")
                        && HOPEvalValueTargetOrSelf(&baseValue)->kind == HOPCTFEValue_STRING)
                    {
                        *outValue = *HOPEvalValueTargetOrSelf(&baseValue);
                        *outIsConst = 1;
                        return 0;
                    }

                    {
                        const HOPParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                        int32_t              savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                        if (p->expectedCallExprFile == p->currentFile
                            && p->expectedCallExprNode == exprNode)
                        {
                            p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                            p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                        }
                        if (HOPEvalResolveCall(
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
                            p->activeCallExpectedTypeFile = savedExpectedTypeFile;
                            p->activeCallExpectedTypeNode = savedExpectedTypeNode;
                            return -1;
                        }
                        p->activeCallExpectedTypeFile = savedExpectedTypeFile;
                        p->activeCallExpectedTypeNode = savedExpectedTypeNode;
                    }
                    if (!*outIsConst && p->currentExecCtx != NULL
                        && p->currentExecCtx->nonConstReason == NULL)
                    {
                        HOPCTFEExecSetReasonNode(
                            p->currentExecCtx,
                            exprNode,
                            "qualified call target is not supported by evaluator backend");
                    }
                    return 0;
                }
            }
            if (p->currentExecCtx != NULL) {
                HOPCTFEExecSetReasonNode(
                    p->currentExecCtx,
                    exprNode,
                    "qualified call target is not supported by evaluator backend");
            }
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == HOPAst_IDENT) {
            int32_t       argNode = ast->nodes[calleeNode].nextSibling;
            uint32_t      argCount = 0;
            HOPCTFEValue* args = NULL;
            HOPCTFEValue  tempArgs[256];
            int32_t       directFnIndex = -1;
            HOPCTFEValue  calleeValue;
            int           calleeIsConst = 0;
            int           calleeMayResolveByNameWithoutValue = 0;
            int32_t       scanNode;
            uint32_t      rawArgCount = 0;
            for (scanNode = argNode; scanNode >= 0; scanNode = ast->nodes[scanNode].nextSibling) {
                rawArgCount++;
            }
            directFnIndex = HOPEvalResolveFunctionBySlice(
                p,
                NULL,
                p->currentFile,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                NULL,
                rawArgCount);
            scanNode = argNode;
            while (scanNode >= 0) {
                HOPCTFEValue         argValue;
                int                  argIsConst = 0;
                int32_t              argExprNode = scanNode;
                int32_t              paramTypeNode = -1;
                const HOPParsedFile* paramTypeFile = p->currentFile;
                if (ast->nodes[scanNode].kind == HOPAst_CALL_ARG) {
                    argExprNode = ast->nodes[scanNode].firstChild;
                }
                if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
                    if (p->currentExecCtx != NULL) {
                        HOPCTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[scanNode].start,
                            ast->nodes[scanNode].end,
                            "call argument is malformed");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                if (directFnIndex >= 0) {
                    const HOPEvalFunction* directFn = &p->funcs[(uint32_t)directFnIndex];
                    uint32_t               fixedCount =
                        directFn->isVariadic && directFn->paramCount > 0
                            ? directFn->paramCount - 1u
                            : directFn->paramCount;
                    if (!(ast->nodes[scanNode].kind == HOPAst_CALL_ARG
                          && (ast->nodes[scanNode].flags & HOPAstFlag_CALL_ARG_SPREAD) != 0)
                        && (!directFn->isVariadic || argCount < fixedCount))
                    {
                        paramTypeNode = HOPEvalFunctionParamTypeNodeAt(directFn, argCount);
                        paramTypeFile = directFn->file;
                        if (paramTypeNode >= 0
                            && directFn->file->ast.nodes[paramTypeNode].kind == HOPAst_TYPE_NAME
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
                    if (HOPEvalExecExprWithTypeNode(
                            p, argExprNode, paramTypeFile, paramTypeNode, &argValue, &argIsConst)
                        != 0)
                    {
                        return -1;
                    }
                } else if (HOPEvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
                    return -1;
                }
                if (!argIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                if (paramTypeNode >= 0 && HOPEvalExprIsAnytypePackIndex(p, ast, argExprNode)
                    && !HOPEvalValueMatchesExpectedTypeNode(
                        p, paramTypeFile, paramTypeNode, &argValue))
                {
                    if (p->currentExecCtx != NULL) {
                        HOPCTFEExecSetReasonNode(
                            p->currentExecCtx, argExprNode, "anytype pack element type mismatch");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                HOPEvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
                if (ast->nodes[scanNode].kind == HOPAst_CALL_ARG
                    && (ast->nodes[scanNode].flags & HOPAstFlag_CALL_ARG_SPREAD) != 0)
                {
                    HOPEvalArray* array = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&argValue));
                    uint32_t      i;
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
                args = (HOPCTFEValue*)HOPArenaAlloc(
                    p->arena, sizeof(HOPCTFEValue) * argCount, (uint32_t)_Alignof(HOPCTFEValue));
                if (args == NULL) {
                    return ErrorSimple("out of memory");
                }
                memcpy(args, tempArgs, sizeof(HOPCTFEValue) * argCount);
            }
            {
                int32_t resolvedFnIndex = HOPEvalResolveFunctionBySlice(
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
                (void)HOPEvalReorderFixedCallArgsByName(
                    p, &p->funcs[(uint32_t)directFnIndex], ast, argNode, args, argCount, 0u);
            }
            calleeMayResolveByNameWithoutValue =
                directFnIndex >= 0
                || HOPEvalNameIsLazyTypeBuiltin(
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
                if (HOPEvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                    return -1;
                }
                if (calleeIsConst && HOPEvalValueIsInvokableFunctionRef(&calleeValue)) {
                    const HOPParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                    int32_t              savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                    const HOPParsedFile* inferredExpectedTypeFile = NULL;
                    int32_t              inferredExpectedTypeNode = -1;
                    int                  invoked;
                    if (p->expectedCallExprFile == p->currentFile
                        && p->expectedCallExprNode == exprNode)
                    {
                        p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                        p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                    } else if (
                        HOPEvalFindExpectedTypeForCallExpr(
                            p->currentFile,
                            exprNode,
                            &inferredExpectedTypeFile,
                            &inferredExpectedTypeNode))
                    {
                        p->activeCallExpectedTypeFile = inferredExpectedTypeFile;
                        p->activeCallExpectedTypeNode = inferredExpectedTypeNode;
                    }
                    invoked = HOPEvalInvokeFunctionRef(
                        p, &calleeValue, args, argCount, outValue, outIsConst);
                    p->activeCallExpectedTypeFile = savedExpectedTypeFile;
                    p->activeCallExpectedTypeNode = savedExpectedTypeNode;
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
            {
                const HOPParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                int32_t              savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                if (p->expectedCallExprFile == p->currentFile
                    && p->expectedCallExprNode == exprNode)
                {
                    p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                    p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                }
                if (HOPEvalResolveCall(
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
                    p->activeCallExpectedTypeFile = savedExpectedTypeFile;
                    p->activeCallExpectedTypeNode = savedExpectedTypeNode;
                    return -1;
                }
                p->activeCallExpectedTypeFile = savedExpectedTypeFile;
                p->activeCallExpectedTypeNode = savedExpectedTypeNode;
            }
            if (!*outIsConst && p->currentExecCtx != NULL
                && p->currentExecCtx->nonConstReason == NULL)
            {
                HOPCTFEExecSetReasonNode(
                    p->currentExecCtx, exprNode, "call is not supported by evaluator backend");
            }
            return 0;
        }
    }
    if (n->kind == HOPAst_CAST) {
        int32_t  valueNode = n->firstChild;
        int32_t  typeNode = valueNode >= 0 ? ast->nodes[valueNode].nextSibling : -1;
        int32_t  extraNode = typeNode >= 0 ? ast->nodes[typeNode].nextSibling : -1;
        char     aliasTargetKind = '\0';
        uint64_t aliasTag = 0;
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0
            && HOPEvalResolveSimpleAliasCastTarget(
                p, p->currentFile, typeNode, &aliasTargetKind, &aliasTag))
        {
            HOPCTFEValue inValue;
            int          inIsConst = 0;
            if (HOPEvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (aliasTargetKind == 'i' && inValue.kind == HOPCTFEValue_INT) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 'f' && inValue.kind == HOPCTFEValue_FLOAT) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 'b' && inValue.kind == HOPCTFEValue_BOOL) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 's' && inValue.kind == HOPCTFEValue_STRING) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
        }
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0) {
            HOPCTFEValue         inValue;
            int                  inIsConst = 0;
            const HOPParsedFile* aliasFile = NULL;
            int32_t              aliasNode = -1;
            int32_t              aliasTargetNode = -1;
            HOPEvalArray*        tuple;
            if (HOPEvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (HOPEvalResolveAliasCastTargetNode(
                    p, p->currentFile, typeNode, &aliasFile, &aliasNode, &aliasTargetNode)
                && aliasFile != NULL && aliasNode >= 0 && aliasTargetNode >= 0)
            {
                tuple = HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&inValue));
                if (tuple != NULL && aliasFile->ast.nodes[aliasTargetNode].kind == HOPAst_TYPE_TUPLE
                    && AstListCount(&aliasFile->ast, aliasTargetNode) == tuple->len)
                {
                    *outValue = inValue;
                    outValue->typeTag = HOPEvalMakeAliasTag(aliasFile, aliasNode);
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0) {
            HOPCTFEValue inValue;
            int          inIsConst = 0;
            int32_t      targetTypeCode = HOPEvalTypeCode_INVALID;
            uint64_t     nullTypeTag = 0;
            if (HOPEvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            (void)HOPEvalTypeCodeFromTypeNode(p->currentFile, typeNode, &targetTypeCode);
            if (inValue.kind == HOPCTFEValue_NULL
                && HOPEvalResolveNullCastTypeTag(p->currentFile, typeNode, &nullTypeTag))
            {
                *outValue = inValue;
                outValue->typeTag = nullTypeTag;
                if (targetTypeCode == HOPEvalTypeCode_RAWPTR) {
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                }
                *outIsConst = 1;
                return 0;
            }
            if (targetTypeCode == HOPEvalTypeCode_RAWPTR
                && (inValue.kind == HOPCTFEValue_REFERENCE || inValue.kind == HOPCTFEValue_STRING))
            {
                *outValue = inValue;
                HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                *outIsConst = 1;
                return 0;
            }
            if (targetTypeCode == HOPEvalTypeCode_BOOL) {
                outValue->kind = HOPCTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (inValue.kind == HOPCTFEValue_BOOL) {
                    outValue->b = inValue.b ? 1u : 0u;
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == HOPCTFEValue_INT) {
                    outValue->b = inValue.i64 != 0 ? 1u : 0u;
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == HOPCTFEValue_FLOAT) {
                    outValue->b = inValue.f64 != 0.0 ? 1u : 0u;
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == HOPCTFEValue_STRING) {
                    outValue->b = 1u;
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == HOPCTFEValue_NULL) {
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (targetTypeCode == HOPEvalTypeCode_F32 || targetTypeCode == HOPEvalTypeCode_F64) {
                outValue->kind = HOPCTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (inValue.kind == HOPCTFEValue_FLOAT) {
                    outValue->f64 = inValue.f64;
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == HOPCTFEValue_INT) {
                    outValue->f64 = (double)inValue.i64;
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == HOPCTFEValue_BOOL) {
                    outValue->f64 = inValue.b ? 1.0 : 0.0;
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == HOPCTFEValue_NULL) {
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (targetTypeCode == HOPEvalTypeCode_U8 || targetTypeCode == HOPEvalTypeCode_U16
                || targetTypeCode == HOPEvalTypeCode_U32 || targetTypeCode == HOPEvalTypeCode_U64
                || targetTypeCode == HOPEvalTypeCode_UINT || targetTypeCode == HOPEvalTypeCode_I8
                || targetTypeCode == HOPEvalTypeCode_I16 || targetTypeCode == HOPEvalTypeCode_I32
                || targetTypeCode == HOPEvalTypeCode_I64 || targetTypeCode == HOPEvalTypeCode_INT)
            {
                int64_t asInt = 0;
                int     canCast = 1;
                if (inValue.kind == HOPCTFEValue_INT) {
                    asInt = inValue.i64;
                } else if (inValue.kind == HOPCTFEValue_BOOL) {
                    asInt = inValue.b ? 1 : 0;
                } else if (inValue.kind == HOPCTFEValue_FLOAT) {
                    if (inValue.f64 != inValue.f64 || inValue.f64 > (double)INT64_MAX
                        || inValue.f64 < (double)INT64_MIN)
                    {
                        *outIsConst = 0;
                        return 0;
                    }
                    asInt = (int64_t)inValue.f64;
                } else if (inValue.kind == HOPCTFEValue_NULL) {
                    asInt = 0;
                } else {
                    canCast = 0;
                }
                if (canCast) {
                    HOPEvalValueSetInt(outValue, asInt);
                    HOPEvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_MUTREF
                 || p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_PTR)
                && inValue.kind == HOPCTFEValue_REFERENCE)
            {
                *outValue = inValue;
                *outIsConst = 1;
                return 0;
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_MUTREF
                 || p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_PTR)
                && p->currentFile->ast.nodes[typeNode].firstChild >= 0
                && (uint32_t)p->currentFile->ast.nodes[typeNode].firstChild < ast->len
                && p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild].kind
                       == HOPAst_TYPE_NAME
                && SliceEqCStr(
                    p->currentFile->source,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataStart,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataEnd,
                    "str")
                && inValue.kind == HOPCTFEValue_STRING)
            {
                *outValue = inValue;
                HOPEvalValueSetRuntimeTypeCode(
                    outValue,
                    p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_REF
                        ? HOPEvalTypeCode_STR_REF
                        : HOPEvalTypeCode_STR_PTR);
                *outIsConst = 1;
                return 0;
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_PTR)
                && p->currentFile->ast.nodes[typeNode].firstChild >= 0
                && (uint32_t)p->currentFile->ast.nodes[typeNode].firstChild < ast->len
                && p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild].kind
                       == HOPAst_TYPE_NAME
                && SliceEqCStr(
                    p->currentFile->source,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataStart,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataEnd,
                    "str")
                && HOPEvalValueAsArray(HOPEvalValueTargetOrSelf(&inValue)) != NULL)
            {
                rc = HOPEvalStringValueFromArrayBytes(
                    p->arena,
                    &inValue,
                    p->currentFile->ast.nodes[typeNode].kind == HOPAst_TYPE_REF
                        ? HOPEvalTypeCode_STR_REF
                        : HOPEvalTypeCode_STR_PTR,
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

    rc = HOPCTFEEvalExprEx(
        p->arena,
        ast,
        (HOPStrView){ p->currentFile->source, p->currentFile->sourceLen },
        exprNode,
        HOPEvalResolveIdent,
        HOPEvalResolveCall,
        p,
        HOPEvalMirMakeTuple,
        p,
        HOPEvalMirIndexValue,
        p,
        HOPEvalMirAggGetField,
        p,
        HOPEvalMirAggAddrField,
        p,
        outValue,
        outIsConst,
        &diag);
    if (rc != 0) {
        if (diag.code != HOPDiag_NONE) {
            PrintHOPDiag(p->currentFile->path, p->currentFile->source, &diag, 1);
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
        HOPCTFEExecSetReasonNode(
            p->currentExecCtx, exprNode, "expression is not supported by evaluator backend");
    }
    return 0;
}

int RunProgramEval(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild) {
    HOPPackageLoader loader;
    HOPPackage*      entryPkg;
    HOPEvalProgram   program;
    HOPEvalFunction* mainFn = NULL;
    uint32_t         i;
    int32_t          mainIndex = -1;
    uint8_t          arenaMem[32 * 1024];
    HOPArena         arena;
    HOPCTFEValue     retValue;
    HOPCTFEValue     noArgsValue;
    int              didReturn = 0;
    int              rc = -1;

    if (LoadAndCheckPackage(entryPath, platformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        return -1;
    }
    if (ValidateEntryMainSignature(entryPkg) != 0) {
        FreeLoader(&loader);
        return -1;
    }

    HOPArenaInit(&arena, arenaMem, (uint32_t)sizeof(arenaMem));
    HOPArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    memset(&program, 0, sizeof(program));
    program.arena = &arena;
    program.loader = &loader;
    program.entryPkg = entryPkg;
    HOPEvalValueSetInt(&program.rootContext.allocator, 1);
    HOPEvalValueSetInt(&program.rootContext.tempAllocator, 2);
    HOPEvalValueSetInt(&program.rootContext.logger, 3);
    program.currentContext = NULL;
    if (HOPEvalCollectFunctions(&program) != 0) {
        goto end;
    }
    if (HOPEvalCollectTopConsts(&program) != 0) {
        goto end;
    }
    if (HOPEvalCollectTopVars(&program) != 0) {
        goto end;
    }
    for (i = 0; i < program.funcLen; i++) {
        HOPEvalFunction* fn = &program.funcs[i];
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

    HOPEvalValueSetNull(&noArgsValue);
    if (HOPEvalInvokeFunction(
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
    HOPArenaDispose(&arena);
    FreeLoader(&loader);
    return rc;
}

HOP_API_END
