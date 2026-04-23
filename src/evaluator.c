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

H2_API_BEGIN

enum {
    H2_EVAL_MIR_HOST_INVALID = H2MirHostTarget_INVALID,
    H2_EVAL_MIR_HOST_PRINT = H2MirHostTarget_PRINT,
    H2_EVAL_MIR_HOST_PLATFORM_EXIT = H2MirHostTarget_PLATFORM_EXIT,
    H2_EVAL_MIR_HOST_FREE = H2MirHostTarget_FREE,
    H2_EVAL_MIR_HOST_CONCAT = H2MirHostTarget_CONCAT,
    H2_EVAL_MIR_HOST_COPY = H2MirHostTarget_COPY,
    H2_EVAL_MIR_HOST_PLATFORM_CONSOLE_LOG = H2MirHostTarget_PLATFORM_CONSOLE_LOG,
};

enum {
    H2_EVAL_MIR_ITER_KIND_INVALID = 0,
    H2_EVAL_MIR_ITER_KIND_SEQUENCE = 1,
    H2_EVAL_MIR_ITER_KIND_PROTOCOL = 2,
    H2_EVAL_MIR_ITER_MAGIC = 0x534c4954u,
};

static int H2EvalNameEqLiteralOrPkgBuiltin(
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

static int H2EvalNameIsCompilerDiagBuiltin(
    const char* _Nullable src, uint32_t start, uint32_t end) {
    return H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "error", "compiler")
        || H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "error_at", "compiler")
        || H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "warn", "compiler")
        || H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "warn_at", "compiler");
}

static int H2EvalNameIsLazyTypeBuiltin(const char* _Nullable src, uint32_t start, uint32_t end) {
    return SliceEqCStr(src, start, end, "typeof")
        || H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "kind", "reflect")
        || H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "base", "reflect")
        || H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "is_alias", "reflect")
        || H2EvalNameEqLiteralOrPkgBuiltin(src, start, end, "type_name", "reflect")
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
    H2CTFEValue sourceValue;
    H2CTFEValue iteratorValue;
} H2EvalMirIteratorState;
static int H2EvalStringValueFromArrayBytes(
    H2Arena* arena, const H2CTFEValue* inValue, int32_t targetTypeCode, H2CTFEValue* outValue);

static int H2EvalTypeNodeIsAnytype(const H2ParsedFile* file, int32_t typeNode);
static int H2EvalTypeNodeIsTemplateParamName(const H2ParsedFile* file, int32_t typeNode);

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
    H2EvalTypeCode_INVALID = 0,
    H2EvalTypeCode_BOOL = 1,
    H2EvalTypeCode_U8,
    H2EvalTypeCode_U16,
    H2EvalTypeCode_U32,
    H2EvalTypeCode_U64,
    H2EvalTypeCode_UINT,
    H2EvalTypeCode_I8,
    H2EvalTypeCode_I16,
    H2EvalTypeCode_I32,
    H2EvalTypeCode_I64,
    H2EvalTypeCode_INT,
    H2EvalTypeCode_F32,
    H2EvalTypeCode_F64,
    H2EvalTypeCode_TYPE,
    H2EvalTypeCode_STR_REF,
    H2EvalTypeCode_STR_PTR,
    H2EvalTypeCode_RAWPTR,
    H2EvalTypeCode_ANYTYPE,
};

typedef struct H2EvalProgram H2EvalProgram;
typedef struct H2EvalContext H2EvalContext;

static void    H2EvalValueSetRuntimeTypeCode(H2CTFEValue* value, int32_t typeCode);
static int     H2EvalValueGetRuntimeTypeCode(const H2CTFEValue* value, int32_t* outTypeCode);
static int32_t H2EvalFindTopConstBySlice(
    const H2EvalProgram* p, const H2ParsedFile* file, uint32_t nameStart, uint32_t nameEnd);
static int32_t H2EvalFindTopConstBySliceInPackage(
    const H2EvalProgram* p,
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd);
static int H2EvalEvalTopConst(
    H2EvalProgram* p, uint32_t topConstIndex, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalInvokeFunction(
    H2EvalProgram* p,
    int32_t        fnIndex,
    const H2CTFEValue* _Nullable args,
    uint32_t             argCount,
    const H2EvalContext* callContext,
    H2CTFEValue*         outValue,
    int*                 outDidReturn);
static int H2EvalInvokeFunctionRef(
    H2EvalProgram*     p,
    const H2CTFEValue* calleeValue,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst);
static int H2EvalValueNeedsDefaultFieldEval(const H2CTFEValue* value);
static int H2EvalMirLookupLocalTypeNode(
    H2EvalProgram*       p,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const H2ParsedFile** outFile,
    int32_t*             outTypeNode);
static int H2EvalMirLookupLocalValue(
    H2EvalProgram* p, uint32_t nameStart, uint32_t nameEnd, H2CTFEValue* outValue);
static int H2EvalFindVisibleLocalTypeNodeByName(
    const H2ParsedFile* file,
    uint32_t            beforePos,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    int32_t*            outTypeNode);

static int H2EvalBuiltinTypeSize(
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

static int H2EvalBuiltinTypeCode(
    const char* source, uint32_t nameStart, uint32_t nameEnd, int32_t* outTypeCode) {
    if (outTypeCode != NULL) {
        *outTypeCode = H2EvalTypeCode_INVALID;
    }
    if (source == NULL || outTypeCode == NULL) {
        return 0;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "bool")) {
        *outTypeCode = H2EvalTypeCode_BOOL;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u8")) {
        *outTypeCode = H2EvalTypeCode_U8;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u16")) {
        *outTypeCode = H2EvalTypeCode_U16;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u32")) {
        *outTypeCode = H2EvalTypeCode_U32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "u64")) {
        *outTypeCode = H2EvalTypeCode_U64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "uint")) {
        *outTypeCode = H2EvalTypeCode_UINT;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i8")) {
        *outTypeCode = H2EvalTypeCode_I8;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i16")) {
        *outTypeCode = H2EvalTypeCode_I16;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i32")) {
        *outTypeCode = H2EvalTypeCode_I32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "i64")) {
        *outTypeCode = H2EvalTypeCode_I64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "int")) {
        *outTypeCode = H2EvalTypeCode_INT;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "f32")) {
        *outTypeCode = H2EvalTypeCode_F32;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "f64")) {
        *outTypeCode = H2EvalTypeCode_F64;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "rawptr")) {
        *outTypeCode = H2EvalTypeCode_RAWPTR;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "type")) {
        *outTypeCode = H2EvalTypeCode_TYPE;
        return 1;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "anytype")) {
        *outTypeCode = H2EvalTypeCode_ANYTYPE;
        return 1;
    }
    return 0;
}

static int H2EvalTypeCodeFromTypeNode(
    const H2ParsedFile* file, int32_t typeNode, int32_t* outTypeCode) {
    const H2AstNode* n;
    if (outTypeCode != NULL) {
        *outTypeCode = H2EvalTypeCode_INVALID;
    }
    if (file == NULL || outTypeCode == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == H2Ast_TYPE_NAME) {
        return H2EvalBuiltinTypeCode(file->source, n->dataStart, n->dataEnd, outTypeCode);
    }
    if ((n->kind == H2Ast_TYPE_REF || n->kind == H2Ast_TYPE_PTR) && n->firstChild >= 0
        && (uint32_t)n->firstChild < file->ast.len
        && file->ast.nodes[n->firstChild].kind == H2Ast_TYPE_NAME
        && SliceEqCStr(
            file->source,
            file->ast.nodes[n->firstChild].dataStart,
            file->ast.nodes[n->firstChild].dataEnd,
            "str"))
    {
        *outTypeCode = n->kind == H2Ast_TYPE_REF ? H2EvalTypeCode_STR_REF : H2EvalTypeCode_STR_PTR;
        return 1;
    }
    return 0;
}

static int H2EvalIsU8ElementTypeNode(const H2ParsedFile* file, int32_t typeNode) {
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    return file->ast.nodes[typeNode].kind == H2Ast_TYPE_NAME
        && SliceEqCStr(
               file->source,
               file->ast.nodes[typeNode].dataStart,
               file->ast.nodes[typeNode].dataEnd,
               "u8");
}

static int H2EvalStringViewTypeCodeFromTypeNode(
    const H2ParsedFile* file, int32_t typeNode, int32_t* outTypeCode) {
    const H2AstNode* n;
    int32_t          childNode;
    if (outTypeCode != NULL) {
        *outTypeCode = H2EvalTypeCode_INVALID;
    }
    if (file == NULL || outTypeCode == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    if (H2EvalTypeCodeFromTypeNode(file, typeNode, outTypeCode)) {
        return *outTypeCode == H2EvalTypeCode_STR_REF || *outTypeCode == H2EvalTypeCode_STR_PTR;
    }
    n = &file->ast.nodes[typeNode];
    if ((n->kind != H2Ast_TYPE_PTR && n->kind != H2Ast_TYPE_REF) || n->firstChild < 0
        || (uint32_t)n->firstChild >= file->ast.len)
    {
        return 0;
    }
    childNode = n->firstChild;
    if ((file->ast.nodes[childNode].kind == H2Ast_TYPE_VARRAY
         || file->ast.nodes[childNode].kind == H2Ast_TYPE_ARRAY)
        && file->ast.nodes[childNode].firstChild >= 0
        && H2EvalIsU8ElementTypeNode(file, file->ast.nodes[childNode].firstChild))
    {
        *outTypeCode = n->kind == H2Ast_TYPE_REF ? H2EvalTypeCode_STR_REF : H2EvalTypeCode_STR_PTR;
        return 1;
    }
    return 0;
}

static int H2EvalCloneStringValue(
    H2Arena* arena, const H2CTFEValue* inValue, H2CTFEValue* outValue, int32_t typeCode) {
    uint8_t* copyBytes = NULL;
    if (arena == NULL || inValue == NULL || outValue == NULL || inValue->kind != H2CTFEValue_STRING)
    {
        return 0;
    }
    *outValue = *inValue;
    if (inValue->s.len > 0) {
        copyBytes = (uint8_t*)H2ArenaAlloc(arena, inValue->s.len, (uint32_t)_Alignof(uint8_t));
        if (copyBytes == NULL) {
            return -1;
        }
        memcpy(copyBytes, inValue->s.bytes, inValue->s.len);
        outValue->s.bytes = copyBytes;
    }
    H2EvalValueSetRuntimeTypeCode(outValue, typeCode);
    return 1;
}

static int H2EvalAdaptStringValueForType(
    H2Arena*            arena,
    const H2ParsedFile* typeFile,
    int32_t             typeNode,
    const H2CTFEValue*  inValue,
    H2CTFEValue*        outValue) {
    int32_t targetTypeCode = H2EvalTypeCode_INVALID;
    int32_t currentTypeCode = H2EvalTypeCode_INVALID;
    if (arena == NULL || typeFile == NULL || inValue == NULL || outValue == NULL
        || inValue->kind != H2CTFEValue_STRING)
    {
        return 0;
    }
    if (!H2EvalStringViewTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)) {
        return 0;
    }
    if (targetTypeCode == H2EvalTypeCode_STR_PTR) {
        if (H2EvalValueGetRuntimeTypeCode(inValue, &currentTypeCode)
            && currentTypeCode == H2EvalTypeCode_STR_PTR)
        {
            *outValue = *inValue;
            return 1;
        }
        return H2EvalCloneStringValue(arena, inValue, outValue, targetTypeCode);
    }
    *outValue = *inValue;
    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
    return 1;
}

static int H2EvalTypeCodeFromValue(const H2CTFEValue* value, int32_t* outTypeCode) {
    if (outTypeCode != NULL) {
        *outTypeCode = H2EvalTypeCode_INVALID;
    }
    if (value == NULL || outTypeCode == NULL) {
        return 0;
    }
    if (H2EvalValueGetRuntimeTypeCode(value, outTypeCode)) {
        return 1;
    }
    if (value->kind == H2CTFEValue_BOOL) {
        *outTypeCode = H2EvalTypeCode_BOOL;
        return 1;
    }
    if (value->kind == H2CTFEValue_INT) {
        *outTypeCode = H2EvalTypeCode_INT;
        return 1;
    }
    if (value->kind == H2CTFEValue_FLOAT) {
        *outTypeCode = H2EvalTypeCode_F64;
        return 1;
    }
    if (value->kind == H2CTFEValue_STRING) {
        *outTypeCode = H2EvalTypeCode_STR_REF;
        return 1;
    }
    if (value->kind == H2CTFEValue_TYPE) {
        *outTypeCode = H2EvalTypeCode_TYPE;
        return 1;
    }
    return 0;
}

static void H2EvalAnnotateValueTypeFromExpr(
    const H2ParsedFile* file, const H2Ast* ast, int32_t exprNode, H2CTFEValue* value) {
    int32_t          typeCode = H2EvalTypeCode_INVALID;
    const H2AstNode* n;
    if (file == NULL || ast == NULL || value == NULL || exprNode < 0
        || (uint32_t)exprNode >= ast->len)
    {
        return;
    }
    if (H2EvalValueGetRuntimeTypeCode(value, &typeCode)) {
        return;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == H2Ast_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return;
        }
        n = &ast->nodes[exprNode];
    }
    if (n->kind == H2Ast_CAST) {
        int32_t typeNode = ASTNextSibling(ast, n->firstChild);
        if (typeNode >= 0 && H2EvalTypeCodeFromTypeNode(file, typeNode, &typeCode)) {
            H2EvalValueSetRuntimeTypeCode(value, typeCode);
            return;
        }
    }
    if (n->kind == H2Ast_STRING) {
        H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_STR_REF);
        return;
    }
    if (n->kind == H2Ast_BOOL) {
        H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_BOOL);
        return;
    }
    if (n->kind == H2Ast_FLOAT) {
        H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_F64);
        return;
    }
    if (n->kind == H2Ast_INT) {
        H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_INT);
        return;
    }
}

static int H2EvalTypeNodeSize(
    const H2ParsedFile* file, int32_t typeNode, uint64_t* outSize, uint32_t depth) {
    const H2AstNode* n;
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
        case H2Ast_TYPE_NAME:
            return H2EvalBuiltinTypeSize(file->source, n->dataStart, n->dataEnd, outSize);
        case H2Ast_TYPE_PTR:
        case H2Ast_TYPE_REF:
        case H2Ast_TYPE_MUTREF:
        case H2Ast_TYPE_FN:       *outSize = (uint64_t)sizeof(void*); return 1;
        case H2Ast_TYPE_SLICE:
        case H2Ast_TYPE_MUTSLICE: *outSize = (uint64_t)(sizeof(void*) * 2u); return 1;
        case H2Ast_TYPE_OPTIONAL: {
            int32_t child = file->ast.nodes[typeNode].firstChild;
            return child >= 0 ? H2EvalTypeNodeSize(file, child, outSize, depth + 1u) : 0;
        }
        case H2Ast_TYPE_ARRAY: {
            int32_t  elemTypeNode = file->ast.nodes[typeNode].firstChild;
            uint64_t elemSize = 0;
            uint64_t count = 0;
            if (elemTypeNode < 0 || !H2EvalTypeNodeSize(file, elemTypeNode, &elemSize, depth + 1u)
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

typedef struct {
    const H2Package*    pkg;
    const H2ParsedFile* file;
    int32_t             fnNode;
    int32_t             bodyNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint32_t            paramCount;
    uint8_t             hasReturnType;
    uint8_t             hasContextClause;
    uint8_t             isBuiltinPackageFn;
    uint8_t             isVariadic;
} H2EvalFunction;

enum {
    H2EvalTopConstState_UNSEEN = 0,
    H2EvalTopConstState_VISITING = 1,
    H2EvalTopConstState_READY = 2,
    H2EvalTopConstState_FAILED = 3,
};

typedef struct {
    const H2ParsedFile* file;
    int32_t             nodeId;
    int32_t             initExprNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint8_t             state;
    uint8_t             _reserved[3];
    H2CTFEValue         value;
} H2EvalTopConst;

typedef struct {
    const H2ParsedFile* file;
    int32_t             nodeId;
    int32_t             initExprNode;
    int32_t             declTypeNode;
    uint32_t            nameStart;
    uint32_t            nameEnd;
    uint8_t             state;
    uint8_t             _reserved[3];
    H2CTFEValue         value;
} H2EvalTopVar;

typedef struct {
    uint32_t    nameStart;
    uint32_t    nameEnd;
    uint16_t    flags;
    uint16_t    _reserved;
    int32_t     typeNode;
    int32_t     defaultExprNode;
    H2CTFEValue value;
} H2EvalAggregateField;

typedef struct {
    const H2ParsedFile*   file;
    int32_t               nodeId;
    H2EvalAggregateField* fields;
    uint32_t              fieldLen;
} H2EvalAggregate;

typedef struct {
    const H2ParsedFile* file;
    int32_t             typeNode;
    int32_t             elemTypeNode;
    H2CTFEValue* _Nullable elems;
    uint32_t len;
} H2EvalArray;

struct H2EvalContext {
    H2CTFEValue allocator;
    H2CTFEValue tempAllocator;
    H2CTFEValue logger;
};

typedef struct {
    const H2ParsedFile* file;
    int32_t             enumNode;
    uint32_t            variantNameStart;
    uint32_t            variantNameEnd;
    uint32_t            tagIndex;
    H2EvalAggregate* _Nullable payload;
} H2EvalTaggedEnum;

typedef struct {
    const H2ParsedFile* activeTemplateParamFile;
    uint32_t            activeTemplateParamNameStart;
    uint32_t            activeTemplateParamNameEnd;
    const H2ParsedFile* activeTemplateTypeFile;
    int32_t             activeTemplateTypeNode;
    H2CTFEValue         activeTemplateTypeValue;
    uint8_t             hasActiveTemplateTypeValue;
} H2EvalTemplateBindingState;

typedef struct H2EvalReflectedType H2EvalReflectedType;
struct H2EvalReflectedType {
    uint8_t             kind;
    uint8_t             namedKind;
    uint16_t            _reserved;
    const H2ParsedFile* file;
    int32_t             nodeId;
    uint32_t            arrayLen;
    H2CTFEValue         elemType;
};

#define H2_EVAL_PACKAGE_REF_TAG_FLAG  (UINT64_C(1) << 63)
#define H2_EVAL_NULL_FIXED_LEN_TAG    (UINT64_C(1) << 62)
#define H2_EVAL_FUNCTION_REF_TAG_FLAG (UINT64_C(1) << 61)
#define H2_EVAL_TAGGED_ENUM_TAG_FLAG  (UINT64_C(1) << 60)
#define H2_EVAL_SIMPLE_TYPE_TAG_FLAG  (UINT64_C(1) << 59)
#define H2_EVAL_REFLECT_TYPE_TAG_FLAG (UINT64_C(1) << 58)
#define H2_EVAL_RUNTIME_TYPE_MAGIC    0x534c4556u

enum {
    H2EvalReflectType_NAMED = 1,
    H2EvalReflectType_PTR = 2,
    H2EvalReflectType_SLICE = 3,
    H2EvalReflectType_ARRAY = 4,
};

enum {
    H2EvalTypeKind_PRIMITIVE = 1,
    H2EvalTypeKind_ALIAS = 2,
    H2EvalTypeKind_STRUCT = 3,
    H2EvalTypeKind_UNION = 4,
    H2EvalTypeKind_ENUM = 5,
    H2EvalTypeKind_POINTER = 6,
    H2EvalTypeKind_REFERENCE = 7,
    H2EvalTypeKind_SLICE = 8,
    H2EvalTypeKind_ARRAY = 9,
    H2EvalTypeKind_OPTIONAL = 10,
    H2EvalTypeKind_FUNCTION = 11,
};

struct H2EvalProgram {
    H2Arena* _Nonnull arena;
    const H2PackageLoader* loader;
    const H2Package*       entryPkg;
    const H2ParsedFile*    currentFile;
    H2CTFEExecCtx*         currentExecCtx;
    struct H2EvalMirExecCtx* _Nullable currentMirExecCtx;
    H2EvalFunction* funcs;
    uint32_t        funcLen;
    uint32_t        funcCap;
    H2EvalTopConst* topConsts;
    uint32_t        topConstLen;
    uint32_t        topConstCap;
    H2EvalTopVar*   topVars;
    uint32_t        topVarLen;
    uint32_t        topVarCap;
    uint32_t        callDepth;
    uint32_t        callStack[H2_EVAL_CALL_MAX_DEPTH];
    H2EvalContext   rootContext;
    const H2EvalContext* _Nullable currentContext;
    H2CTFEValue         loggerPrefix;
    const H2ParsedFile* activeTemplateParamFile;
    uint32_t            activeTemplateParamNameStart;
    uint32_t            activeTemplateParamNameEnd;
    const H2ParsedFile* activeTemplateTypeFile;
    int32_t             activeTemplateTypeNode;
    H2CTFEValue         activeTemplateTypeValue;
    uint8_t             hasActiveTemplateTypeValue;
    const H2ParsedFile* expectedCallExprFile;
    int32_t             expectedCallExprNode;
    const H2ParsedFile* expectedCallTypeFile;
    int32_t             expectedCallTypeNode;
    const H2ParsedFile* activeCallExpectedTypeFile;
    int32_t             activeCallExpectedTypeNode;
    int                 exitCalled;
    int                 exitCode;
};

typedef struct H2EvalMirExecCtx {
    H2EvalProgram*             p;
    uint32_t*                  evalToMir;
    uint32_t                   evalToMirLen;
    uint32_t*                  mirToEval;
    uint32_t                   mirToEvalLen;
    const H2ParsedFile**       sourceFiles;
    uint32_t                   sourceFileCap;
    const H2ParsedFile*        savedFiles[H2_EVAL_CALL_MAX_DEPTH];
    uint8_t                    pushedFrames[H2_EVAL_CALL_MAX_DEPTH];
    struct H2EvalMirExecCtx*   savedMirExecCtxs[H2_EVAL_CALL_MAX_DEPTH];
    uint32_t                   savedFileLen;
    uint32_t                   rootMirFnIndex;
    const H2MirProgram*        mirProgram;
    const H2MirFunction*       mirFunction;
    const H2CTFEValue*         mirLocals;
    uint32_t                   mirLocalCount;
    const H2MirProgram*        savedMirPrograms[H2_EVAL_CALL_MAX_DEPTH];
    const H2MirFunction*       savedMirFunctions[H2_EVAL_CALL_MAX_DEPTH];
    const H2CTFEValue*         savedMirLocals[H2_EVAL_CALL_MAX_DEPTH];
    uint32_t                   savedMirLocalCounts[H2_EVAL_CALL_MAX_DEPTH];
    uint32_t                   mirFrameDepth;
    H2EvalTemplateBindingState savedTemplateBindings[H2_EVAL_CALL_MAX_DEPTH];
    uint8_t                    restoresTemplateBinding[H2_EVAL_CALL_MAX_DEPTH];
    H2EvalTemplateBindingState pendingTemplateBinding;
    uint8_t                    hasPendingTemplateBinding;
} H2EvalMirExecCtx;

static uint32_t H2EvalAggregateFieldBindingCount(const H2EvalAggregate* agg);
static int      H2EvalAppendAggregateFieldBindings(
    H2CTFEExecBinding*    fieldBindings,
    uint32_t              bindingCap,
    H2CTFEExecEnv*        fieldFrame,
    H2EvalAggregateField* field);
static int H2EvalAggregateHasReservedFields(const H2EvalAggregate* agg);
static int H2EvalReplayReservedAggregateFields(
    H2CTFEValue* target, const H2EvalAggregate* sourceAgg);

static void H2EvalValueSetNull(H2CTFEValue* value) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_NULL;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void H2EvalValueSetInt(H2CTFEValue* value, int64_t n) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_INT;
    value->i64 = n;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void H2EvalValueSetRuntimeTypeCode(H2CTFEValue* value, int32_t typeCode) {
    if (value == NULL) {
        return;
    }
    value->span.fileLen = 0;
    value->span.startLine = (uint32_t)typeCode;
    value->span.startColumn = H2_EVAL_RUNTIME_TYPE_MAGIC;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static int H2EvalValueGetRuntimeTypeCode(const H2CTFEValue* value, int32_t* outTypeCode) {
    if (value == NULL || outTypeCode == NULL || value->kind == H2CTFEValue_SPAN
        || value->span.startColumn != H2_EVAL_RUNTIME_TYPE_MAGIC)
    {
        return 0;
    }
    *outTypeCode = (int32_t)value->span.startLine;
    return 1;
}

static void H2EvalValueSetSimpleTypeValue(H2CTFEValue* value, int32_t typeCode) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = H2_EVAL_SIMPLE_TYPE_TAG_FLAG | (uint64_t)(uint32_t)typeCode;
    value->s.bytes = NULL;
    value->s.len = 0;
    value->span.fileBytes = NULL;
    value->span.fileLen = 0;
    value->span.startLine = 0;
    value->span.startColumn = 0;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static int H2EvalValueGetSimpleTypeCode(const H2CTFEValue* value, int32_t* outTypeCode) {
    if (value == NULL || outTypeCode == NULL || value->kind != H2CTFEValue_TYPE
        || (value->typeTag & H2_EVAL_SIMPLE_TYPE_TAG_FLAG) == 0)
    {
        return 0;
    }
    *outTypeCode = (int32_t)(uint32_t)(value->typeTag & ~H2_EVAL_SIMPLE_TYPE_TAG_FLAG);
    return 1;
}

static void H2EvalAnnotateUntypedLiteralValue(H2CTFEValue* value) {
    int32_t typeCode = H2EvalTypeCode_INVALID;
    if (value == NULL || H2EvalValueGetRuntimeTypeCode(value, &typeCode)) {
        return;
    }
    switch (value->kind) {
        case H2CTFEValue_BOOL:  H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_BOOL); break;
        case H2CTFEValue_INT:   H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_INT); break;
        case H2CTFEValue_FLOAT: H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_F64); break;
        case H2CTFEValue_STRING:
            H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_STR_REF);
            break;
        default: break;
    }
}

static uint64_t H2EvalHashMix64(uint64_t x) {
    x ^= x >> 21;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return x;
}

static uint64_t H2EvalReflectTypeTagBody(const H2EvalReflectedType* rt) {
    uint64_t tag = 0;
    if (rt == NULL) {
        return 0;
    }
    tag = ((uint64_t)rt->kind << 52) ^ ((uint64_t)rt->namedKind << 44);
    if (rt->kind == H2EvalReflectType_NAMED) {
        tag ^= H2EvalHashMix64((uint64_t)(uintptr_t)rt->file);
        tag ^= H2EvalHashMix64((uint64_t)(uint32_t)(rt->nodeId + 1));
    } else {
        tag ^= H2EvalHashMix64(rt->elemType.typeTag);
        tag ^= H2EvalHashMix64((uint64_t)rt->arrayLen);
    }
    return tag
         & ~(H2_EVAL_PACKAGE_REF_TAG_FLAG | H2_EVAL_NULL_FIXED_LEN_TAG
             | H2_EVAL_FUNCTION_REF_TAG_FLAG | H2_EVAL_TAGGED_ENUM_TAG_FLAG
             | H2_EVAL_SIMPLE_TYPE_TAG_FLAG | H2_EVAL_REFLECT_TYPE_TAG_FLAG);
}

static void H2EvalValueSetReflectedTypeValue(H2CTFEValue* value, H2EvalReflectedType* rt) {
    if (value == NULL || rt == NULL) {
        return;
    }
    value->kind = H2CTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = H2_EVAL_REFLECT_TYPE_TAG_FLAG | H2EvalReflectTypeTagBody(rt);
    value->s.bytes = (const uint8_t*)rt;
    value->s.len = 0;
    value->span.fileBytes = NULL;
    value->span.fileLen = 0;
    value->span.startLine = 0;
    value->span.startColumn = 0;
    value->span.endLine = 0;
    value->span.endColumn = 0;
}

static H2EvalReflectedType* _Nullable H2EvalValueAsReflectedType(const H2CTFEValue* value) {
    if (value == NULL || value->kind != H2CTFEValue_TYPE
        || (value->typeTag & H2_EVAL_REFLECT_TYPE_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (H2EvalReflectedType*)value->s.bytes;
}

static void H2EvalValueSetPackageRef(H2CTFEValue* value, uint32_t pkgIndex) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = H2_EVAL_PACKAGE_REF_TAG_FLAG | (uint64_t)pkgIndex;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static void H2EvalValueSetFunctionRef(H2CTFEValue* value, uint32_t fnIndex) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = H2_EVAL_FUNCTION_REF_TAG_FLAG | (uint64_t)fnIndex;
    value->s.bytes = NULL;
    value->s.len = 0;
}

static int H2EvalValueIsPackageRef(const H2CTFEValue* value, uint32_t* outPkgIndex) {
    if (value == NULL || value->kind != H2CTFEValue_TYPE
        || (value->typeTag & H2_EVAL_PACKAGE_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outPkgIndex != NULL) {
        *outPkgIndex = (uint32_t)(value->typeTag & ~H2_EVAL_PACKAGE_REF_TAG_FLAG);
    }
    return 1;
}

static int H2EvalValueIsFunctionRef(const H2CTFEValue* value, uint32_t* _Nullable outFnIndex) {
    if (value == NULL || value->kind != H2CTFEValue_TYPE
        || (value->typeTag & H2_EVAL_FUNCTION_REF_TAG_FLAG) == 0)
    {
        return 0;
    }
    if (outFnIndex != NULL) {
        *outFnIndex = (uint32_t)(value->typeTag & ~H2_EVAL_FUNCTION_REF_TAG_FLAG);
    }
    return 1;
}

static int H2EvalValueIsInvokableFunctionRef(const H2CTFEValue* value) {
    return H2EvalValueIsFunctionRef(value, NULL) || H2MirValueAsFunctionRef(value, NULL);
}

static void H2EvalValueSetTaggedEnum(
    H2EvalProgram*      p,
    H2CTFEValue*        value,
    const H2ParsedFile* file,
    int32_t             enumNode,
    uint32_t            variantNameStart,
    uint32_t            variantNameEnd,
    uint32_t            tagIndex,
    H2EvalAggregate* _Nullable payload) {
    H2EvalTaggedEnum* tagged;
    if (p == NULL || value == NULL || file == NULL) {
        return;
    }
    tagged = (H2EvalTaggedEnum*)H2ArenaAlloc(
        p->arena, sizeof(H2EvalTaggedEnum), (uint32_t)_Alignof(H2EvalTaggedEnum));
    if (tagged == NULL) {
        H2EvalValueSetNull(value);
        return;
    }
    memset(tagged, 0, sizeof(*tagged));
    tagged->file = file;
    tagged->enumNode = enumNode;
    tagged->variantNameStart = variantNameStart;
    tagged->variantNameEnd = variantNameEnd;
    tagged->tagIndex = tagIndex;
    tagged->payload = payload;
    value->kind = H2CTFEValue_TYPE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = H2_EVAL_TAGGED_ENUM_TAG_FLAG
                   | (((uint64_t)(uintptr_t)file) & ~H2_EVAL_TAGGED_ENUM_TAG_FLAG);
    value->typeTag ^= (uint64_t)(uint32_t)(enumNode + 1) << 3;
    value->typeTag ^= (uint64_t)tagIndex;
    value->s.bytes = (const uint8_t*)tagged;
    value->s.len = 0;
}

static H2EvalTaggedEnum* _Nullable H2EvalValueAsTaggedEnum(const H2CTFEValue* value) {
    if (value == NULL || value->kind != H2CTFEValue_TYPE
        || (value->typeTag & H2_EVAL_TAGGED_ENUM_TAG_FLAG) == 0 || value->s.bytes == NULL)
    {
        return NULL;
    }
    return (H2EvalTaggedEnum*)value->s.bytes;
}

static uint64_t H2EvalMakeAliasTag(const H2ParsedFile* file, int32_t nodeId) {
    uint64_t tag = 0;
    if (file != NULL) {
        tag = (uint64_t)(uintptr_t)file;
    }
    tag ^= (uint64_t)(uint32_t)(nodeId + 1) << 1;
    return tag & ~H2_EVAL_PACKAGE_REF_TAG_FLAG;
}

static uint64_t H2EvalMakeNullFixedLenTag(uint32_t len) {
    return H2_EVAL_NULL_FIXED_LEN_TAG | (uint64_t)len;
}

static int H2EvalValueGetNullFixedLen(const H2CTFEValue* value, uint32_t* outLen) {
    if (value == NULL || value->kind != H2CTFEValue_NULL
        || (value->typeTag & H2_EVAL_NULL_FIXED_LEN_TAG) == 0)
    {
        return 0;
    }
    if (outLen != NULL) {
        *outLen = (uint32_t)(value->typeTag & ~H2_EVAL_NULL_FIXED_LEN_TAG);
    }
    return 1;
}

static int H2EvalParseUintSlice(
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

static int H2EvalResolveNullCastTypeTag(
    const H2ParsedFile* file, int32_t typeNode, uint64_t* _Nullable outTypeTag) {
    const H2AstNode* n;
    int32_t          childNode;
    uint32_t         fixedLen = 0;
    if (outTypeTag != NULL) {
        *outTypeTag = 0;
    }
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == H2Ast_TYPE_NAME && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "rawptr"))
    {
        return 1;
    }
    if (n->kind != H2Ast_TYPE_PTR && n->kind != H2Ast_TYPE_REF && n->kind != H2Ast_TYPE_MUTREF) {
        return 0;
    }
    childNode = n->firstChild;
    if (childNode >= 0 && (uint32_t)childNode < file->ast.len
        && file->ast.nodes[childNode].kind == H2Ast_TYPE_ARRAY
        && H2EvalParseUintSlice(
            file->source,
            file->ast.nodes[childNode].dataStart,
            file->ast.nodes[childNode].dataEnd,
            &fixedLen))
    {
        if (outTypeTag != NULL) {
            *outTypeTag = H2EvalMakeNullFixedLenTag(fixedLen);
        }
    }
    return 1;
}

static const H2Package* _Nullable H2EvalFindPackageByFile(
    const H2EvalProgram* p, const H2ParsedFile* file) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL || file == NULL) {
        return NULL;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const H2Package* pkg = &p->loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            if (&pkg->files[fileIndex] == file) {
                return pkg;
            }
        }
    }
    return NULL;
}

static void H2EvalValueSetStringSlice(
    H2CTFEValue* value, const char* source, uint32_t start, uint32_t end) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_STRING;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = source != NULL ? (const uint8_t*)(source + start) : NULL;
    value->s.len = end >= start ? end - start : 0;
}

static int32_t H2EvalResolveNamedTypeDeclInPackage(
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const H2ParsedFile** outFile,
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
        const H2ParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const H2AstNode* n = &pkgFile->ast.nodes[nodeId];
            uint8_t          namedKind = 0;
            if (n->kind == H2Ast_TYPE_ALIAS) {
                namedKind = H2EvalTypeKind_ALIAS;
            } else if (n->kind == H2Ast_STRUCT) {
                namedKind = H2EvalTypeKind_STRUCT;
            } else if (n->kind == H2Ast_UNION) {
                namedKind = H2EvalTypeKind_UNION;
            } else if (n->kind == H2Ast_ENUM) {
                namedKind = H2EvalTypeKind_ENUM;
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

static int H2EvalResolveTypePackageAndName(
    const H2EvalProgram* p,
    const H2ParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const H2Package**    outPkg,
    uint32_t*            outLookupStart,
    uint32_t*            outLookupEnd) {
    const H2Package* currentPkg;
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
    currentPkg = H2EvalFindPackageByFile(p, callerFile);
    while (dot < nameEnd) {
        if (callerFile->source[dot] == '.') {
            break;
        }
        dot++;
    }
    if (dot < nameEnd && currentPkg != NULL) {
        uint32_t i;
        for (i = 0; i < currentPkg->importLen; i++) {
            const H2ImportRef* imp = &currentPkg->imports[i];
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

static int H2EvalMakeNamedTypeValue(
    H2EvalProgram*      p,
    const H2ParsedFile* declFile,
    int32_t             declNode,
    uint8_t             namedKind,
    H2CTFEValue*        outValue) {
    H2EvalReflectedType* rt;
    if (p == NULL || declFile == NULL || outValue == NULL || declNode < 0) {
        return 0;
    }
    rt = (H2EvalReflectedType*)H2ArenaAlloc(
        p->arena, sizeof(H2EvalReflectedType), (uint32_t)_Alignof(H2EvalReflectedType));
    if (rt == NULL) {
        return -1;
    }
    memset(rt, 0, sizeof(*rt));
    rt->kind = H2EvalReflectType_NAMED;
    rt->namedKind = namedKind;
    rt->file = declFile;
    rt->nodeId = declNode;
    H2EvalValueSetReflectedTypeValue(outValue, rt);
    return 1;
}

static int H2EvalResolveTypeValueName(
    H2EvalProgram*      p,
    const H2ParsedFile* callerFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    H2CTFEValue*        outValue) {
    const H2Package*    targetPkg = NULL;
    const H2ParsedFile* declFile = NULL;
    uint32_t            lookupStart = nameStart;
    uint32_t            lookupEnd = nameEnd;
    uint8_t             namedKind = 0;
    int32_t             typeCode = H2EvalTypeCode_INVALID;
    int32_t             declNode = -1;
    uint32_t            pkgIndex;
    if (p == NULL || callerFile == NULL || outValue == NULL) {
        return 0;
    }
    if (H2EvalBuiltinTypeCode(callerFile->source, nameStart, nameEnd, &typeCode)) {
        H2EvalValueSetSimpleTypeValue(outValue, typeCode);
        return 1;
    }
    if (H2EvalResolveTypePackageAndName(
            p, callerFile, nameStart, nameEnd, &targetPkg, &lookupStart, &lookupEnd))
    {
        declNode = H2EvalResolveNamedTypeDeclInPackage(
            targetPkg, callerFile, lookupStart, lookupEnd, &declFile, &namedKind);
        if (declNode >= 0 && declFile != NULL) {
            return H2EvalMakeNamedTypeValue(p, declFile, declNode, namedKind, outValue);
        }
        {
            int32_t topConstIndex = H2EvalFindTopConstBySliceInPackage(
                p, targetPkg, callerFile, lookupStart, lookupEnd);
            if (topConstIndex >= 0) {
                int isConst = 0;
                if (H2EvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                    return -1;
                }
                if (isConst && outValue->kind == H2CTFEValue_TYPE) {
                    return 1;
                }
            }
        }
    }
    if (p->loader == NULL) {
        return 0;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const H2Package* pkg = &p->loader->packages[pkgIndex];
        int32_t          topConstIndex;
        if (pkg == targetPkg) {
            continue;
        }
        declNode = H2EvalResolveNamedTypeDeclInPackage(
            pkg, callerFile, lookupStart, lookupEnd, &declFile, &namedKind);
        if (declNode >= 0 && declFile != NULL) {
            return H2EvalMakeNamedTypeValue(p, declFile, declNode, namedKind, outValue);
        }
        topConstIndex = H2EvalFindTopConstBySliceInPackage(
            p, pkg, callerFile, lookupStart, lookupEnd);
        if (topConstIndex >= 0) {
            int isConst = 0;
            if (H2EvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst && outValue->kind == H2CTFEValue_TYPE) {
                return 1;
            }
        }
    }
    return 0;
}

static uint64_t H2EvalMakeAggregateTag(const H2ParsedFile* file, int32_t nodeId) {
    uint64_t tag = 0;
    if (file != NULL) {
        tag = (uint64_t)(uintptr_t)file;
    }
    tag ^= (uint64_t)(uint32_t)(nodeId + 1) << 2;
    return tag
         & ~(H2_EVAL_PACKAGE_REF_TAG_FLAG | H2_EVAL_NULL_FIXED_LEN_TAG
             | H2CTFEValueTag_AGG_PARTIAL);
}

static void H2EvalValueSetAggregate(
    H2CTFEValue* value, const H2ParsedFile* file, int32_t nodeId, H2EvalAggregate* aggregate) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_AGGREGATE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = H2EvalMakeAggregateTag(file, nodeId);
    value->s.bytes = (const uint8_t*)aggregate;
    value->s.len = aggregate != NULL ? aggregate->fieldLen : 0;
}

static H2EvalAggregate* _Nullable H2EvalValueAsAggregate(const H2CTFEValue* value) {
    if (value == NULL || value->kind != H2CTFEValue_AGGREGATE || value->s.bytes == NULL) {
        return NULL;
    }
    return (H2EvalAggregate*)value->s.bytes;
}

static H2CTFEValue* _Nullable H2EvalValueReferenceTarget(const H2CTFEValue* value);
static void H2EvalValueSetReference(H2CTFEValue* value, H2CTFEValue* target);
static int  H2EvalExecExprCb(void* ctx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
static int  H2EvalZeroInitTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    H2CTFEValue*         outValue,
    int*                 outIsConst);
static int H2EvalResolveAliasCastTargetNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    const H2ParsedFile** outAliasFile,
    int32_t*             outAliasNode,
    int32_t*             outTargetNode);
static int H2EvalResolveAggregateDeclFromValue(
    const H2EvalProgram* p,
    const H2CTFEValue*   value,
    const H2ParsedFile** outFile,
    int32_t*             outNode);
static int H2EvalAggregateSetFieldValue(
    H2EvalAggregate*   agg,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* inValue,
    H2CTFEValue* _Nullable outValue);
static int H2EvalExecExprInFileWithType(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    H2CTFEExecEnv*      env,
    int32_t             exprNode,
    const H2ParsedFile* typeFile,
    int32_t             typeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst);
static H2EvalAggregateField* _Nullable H2EvalAggregateFindDirectField(
    H2EvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd);
static int H2EvalValueSetFieldPath(
    H2CTFEValue*       value,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* inValue);
static int H2EvalFinalizeAggregateVarArrays(H2EvalProgram* p, H2EvalAggregate* agg);
static int H2EvalForInIndexCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    const H2CTFEValue* sourceValue,
    uint32_t           index,
    int                byRef,
    H2CTFEValue*       outValue,
    int*               outIsConst);
static int H2EvalForInIterCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    int32_t            sourceNode,
    const H2CTFEValue* sourceValue,
    uint32_t           index,
    int                hasKey,
    int                keyRef,
    int                valueRef,
    int                valueDiscard,
    int*               outHasItem,
    H2CTFEValue*       outKey,
    int*               outKeyIsConst,
    H2CTFEValue*       outValue,
    int*               outValueIsConst);

static void H2EvalValueSetArray(
    H2CTFEValue* value, const H2ParsedFile* file, int32_t typeNode, H2EvalArray* array) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_ARRAY;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = H2EvalMakeAggregateTag(file, typeNode);
    value->s.bytes = (const uint8_t*)array;
    value->s.len = array != NULL ? array->len : 0;
}

static H2EvalArray* _Nullable H2EvalValueAsArray(const H2CTFEValue* value) {
    if (value == NULL || value->kind != H2CTFEValue_ARRAY || value->s.bytes == NULL) {
        return NULL;
    }
    return (H2EvalArray*)value->s.bytes;
}

static H2EvalArray* _Nullable H2EvalAllocArrayView(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    int32_t              elemTypeNode,
    H2CTFEValue* _Nullable elems,
    uint32_t len) {
    H2EvalArray* array;
    if (p == NULL) {
        return NULL;
    }
    array = (H2EvalArray*)H2ArenaAlloc(
        p->arena, sizeof(H2EvalArray), (uint32_t)_Alignof(H2EvalArray));
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

static int H2EvalAllocTupleValue(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    int32_t             typeNode,
    const H2CTFEValue*  elems,
    uint32_t            len,
    H2CTFEValue*        outValue,
    int*                outIsConst) {
    H2EvalArray* array;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || file == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    array = H2EvalAllocArrayView(p, file, typeNode, -1, NULL, len);
    if (array == NULL) {
        return ErrorSimple("out of memory");
    }
    if (len > 0) {
        array->elems = (H2CTFEValue*)H2ArenaAlloc(
            p->arena, sizeof(H2CTFEValue) * len, (uint32_t)_Alignof(H2CTFEValue));
        if (array->elems == NULL) {
            return ErrorSimple("out of memory");
        }
        memcpy(array->elems, elems, sizeof(H2CTFEValue) * len);
    }
    H2EvalValueSetArray(outValue, file, typeNode, array);
    *outIsConst = 1;
    return 0;
}

static const H2CTFEValue* H2EvalValueTargetOrSelf(const H2CTFEValue* value) {
    const H2CTFEValue* target = H2EvalValueReferenceTarget(value);
    return target != NULL ? target : value;
}

static int H2EvalOptionalPayload(const H2CTFEValue* value, const H2CTFEValue** outPayload) {
    if (outPayload != NULL) {
        *outPayload = NULL;
    }
    if (value == NULL || value->kind != H2CTFEValue_OPTIONAL || outPayload == NULL) {
        return 0;
    }
    if (value->b == 0u) {
        return 1;
    }
    if (value->s.bytes == NULL) {
        return 0;
    }
    *outPayload = (const H2CTFEValue*)value->s.bytes;
    return 1;
}

static int H2EvalResolveSimpleAliasCastTarget(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    char*                outTargetKind,
    uint64_t* _Nullable outAliasTag);

static int H2EvalCoerceValueToTypeNode(
    H2EvalProgram* p, const H2ParsedFile* typeFile, int32_t typeNode, H2CTFEValue* inOutValue) {
    const H2AstNode*   type;
    const H2CTFEValue* sourceValue;
    H2EvalAggregate*   sourceAgg;
    const H2CTFEValue* optionalPayload = NULL;
    int32_t            targetTypeCode = H2EvalTypeCode_INVALID;
    uint32_t           i;
    if (p == NULL || typeFile == NULL || typeNode < 0 || inOutValue == NULL
        || (uint32_t)typeNode >= typeFile->ast.len)
    {
        return -1;
    }
    if (H2EvalTypeNodeIsAnytype(typeFile, typeNode)) {
        H2EvalAnnotateUntypedLiteralValue(inOutValue);
        return 0;
    }
    if (H2EvalTypeNodeIsTemplateParamName(typeFile, typeNode)) {
        H2EvalAnnotateUntypedLiteralValue(inOutValue);
        return 0;
    }
    type = &typeFile->ast.nodes[typeNode];
    sourceValue = H2EvalValueTargetOrSelf(inOutValue);
    if (type->kind == H2Ast_TYPE_OPTIONAL) {
        int32_t payloadTypeNode = type->firstChild;
        if (sourceValue->kind == H2CTFEValue_OPTIONAL) {
            return 0;
        }
        if (sourceValue->kind == H2CTFEValue_NULL) {
            inOutValue->kind = H2CTFEValue_OPTIONAL;
            inOutValue->i64 = 0;
            inOutValue->f64 = 0.0;
            inOutValue->b = 0u;
            inOutValue->typeTag = 0;
            inOutValue->s.bytes = NULL;
            inOutValue->s.len = 0;
            return 0;
        }
        {
            H2CTFEValue  payloadValue = *inOutValue;
            H2CTFEValue* payloadCopy;
            if (payloadTypeNode >= 0
                && H2EvalCoerceValueToTypeNode(p, typeFile, payloadTypeNode, &payloadValue) != 0)
            {
                return -1;
            }
            payloadCopy = (H2CTFEValue*)H2ArenaAlloc(
                p->arena, sizeof(H2CTFEValue), (uint32_t)_Alignof(H2CTFEValue));
            if (payloadCopy == NULL) {
                return ErrorSimple("out of memory");
            }
            *payloadCopy = payloadValue;
            inOutValue->kind = H2CTFEValue_OPTIONAL;
            inOutValue->i64 = 0;
            inOutValue->f64 = 0.0;
            inOutValue->b = 1u;
            inOutValue->typeTag = 0;
            inOutValue->s.bytes = (const uint8_t*)payloadCopy;
            inOutValue->s.len = 0;
            return 0;
        }
    }
    if (type->kind != H2Ast_TYPE_OPTIONAL && sourceValue->kind == H2CTFEValue_OPTIONAL
        && H2EvalOptionalPayload(sourceValue, &optionalPayload))
    {
        if (sourceValue->b == 0u || optionalPayload == NULL) {
            return 0;
        }
        *inOutValue = *optionalPayload;
        sourceValue = inOutValue;
    }
    if (type->kind == H2Ast_TYPE_NAME) {
        char     aliasTargetKind = '\0';
        uint64_t aliasTag = 0;
        if (H2EvalResolveSimpleAliasCastTarget(p, typeFile, typeNode, &aliasTargetKind, &aliasTag))
        {
            if ((aliasTargetKind == 'i' && sourceValue->kind == H2CTFEValue_INT)
                || (aliasTargetKind == 'f' && sourceValue->kind == H2CTFEValue_FLOAT)
                || (aliasTargetKind == 'b' && sourceValue->kind == H2CTFEValue_BOOL)
                || (aliasTargetKind == 's' && sourceValue->kind == H2CTFEValue_STRING))
            {
                *inOutValue = *sourceValue;
                inOutValue->typeTag = aliasTag;
                return 0;
            }
        }
    }
    sourceAgg = H2EvalValueAsAggregate(sourceValue);
    if ((type->kind == H2Ast_TYPE_NAME || type->kind == H2Ast_TYPE_ANON_STRUCT
         || type->kind == H2Ast_TYPE_ANON_UNION)
        && sourceAgg != NULL)
    {
        H2CTFEValue        targetValue;
        int                targetIsConst = 0;
        H2EvalAggregate*   targetAgg;
        uint8_t*           explicitSet = NULL;
        H2CTFEExecBinding* fieldBindings = NULL;
        H2CTFEExecEnv      fieldFrame;
        uint32_t           fieldBindingCap = 0;
        int                finalizeRc = 0;
        int sourcePartial = (sourceValue->typeTag & H2CTFEValueTag_AGG_PARTIAL) != 0u;
        if (H2EvalZeroInitTypeNode(p, typeFile, typeNode, &targetValue, &targetIsConst) != 0) {
            return -1;
        }
        if (!targetIsConst) {
            return 0;
        }
        targetAgg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(&targetValue));
        if (targetAgg == NULL) {
            return 0;
        }
        if (targetAgg->fieldLen > 0) {
            fieldBindingCap = H2EvalAggregateFieldBindingCount(targetAgg);
            explicitSet = (uint8_t*)H2ArenaAlloc(
                p->arena, targetAgg->fieldLen, (uint32_t)_Alignof(uint8_t));
            fieldBindings = (H2CTFEExecBinding*)H2ArenaAlloc(
                p->arena,
                sizeof(H2CTFEExecBinding) * fieldBindingCap,
                (uint32_t)_Alignof(H2CTFEExecBinding));
            if (explicitSet == NULL || fieldBindings == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(explicitSet, 0, targetAgg->fieldLen);
            memset(fieldBindings, 0, sizeof(H2CTFEExecBinding) * fieldBindingCap);
        }
        for (i = 0; i < sourceAgg->fieldLen; i++) {
            const H2EvalAggregateField* field = &sourceAgg->fields[i];
            H2CTFEValue                 fieldValue = field->value;
            H2EvalAggregate*            embeddedFieldAgg = H2EvalValueAsAggregate(&field->value);
            int                         nestedReserved =
                (field->flags & H2AstFlag_FIELD_EMBEDDED) != 0
                && H2EvalAggregateHasReservedFields(embeddedFieldAgg);
            if (sourcePartial && field->typeNode >= 0 && (field->_reserved & 1u) == 0u
                && !nestedReserved)
            {
                continue;
            }
            if (explicitSet != NULL) {
                uint32_t j;
                for (j = 0; j < targetAgg->fieldLen; j++) {
                    H2EvalAggregateField* targetField = &targetAgg->fields[j];
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
                            && H2EvalCoerceValueToTypeNode(
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
            if (!H2EvalValueSetFieldPath(
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
            H2EvalAggregateField* field = &targetAgg->fields[i];
            if ((explicitSet == NULL || explicitSet[i] == 0u) && field->defaultExprNode >= 0) {
                H2CTFEValue defaultValue;
                int         defaultIsConst = 0;
                if (H2EvalExecExprInFileWithType(
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
            if (sourcePartial && (field->flags & H2AstFlag_FIELD_EMBEDDED) != 0) {
                H2EvalAggregateField* sourceField = H2EvalAggregateFindDirectField(
                    sourceAgg, targetAgg->file->source, field->nameStart, field->nameEnd);
                if (sourceField != NULL
                    && !H2EvalReplayReservedAggregateFields(
                        &targetValue, H2EvalValueAsAggregate(&sourceField->value)))
                {
                    return 0;
                }
            }
            if (fieldBindings != NULL
                && H2EvalAppendAggregateFieldBindings(
                       fieldBindings, fieldBindingCap, &fieldFrame, field)
                       != 0)
            {
                return ErrorSimple("out of memory");
            }
            field->_reserved = 0;
        }
        finalizeRc = H2EvalFinalizeAggregateVarArrays(p, targetAgg);
        if (finalizeRc != 1) {
            return finalizeRc < 0 ? -1 : 0;
        }
        *inOutValue = targetValue;
    }
    if (H2EvalStringViewTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)
        && sourceValue->kind != H2CTFEValue_STRING)
    {
        H2CTFEValue stringValue;
        int         stringRc = H2EvalStringValueFromArrayBytes(
            p->arena, sourceValue, targetTypeCode, &stringValue);
        if (stringRc < 0) {
            return -1;
        }
        if (stringRc > 0) {
            *inOutValue = stringValue;
        }
    }
    if (H2EvalAdaptStringValueForType(p->arena, typeFile, typeNode, inOutValue, inOutValue) < 0) {
        return -1;
    }
    sourceValue = H2EvalValueTargetOrSelf(inOutValue);
    if (H2EvalTypeCodeFromTypeNode(typeFile, typeNode, &targetTypeCode)) {
        if (sourceValue->kind == H2CTFEValue_BOOL && targetTypeCode == H2EvalTypeCode_BOOL) {
            H2EvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == H2CTFEValue_FLOAT
            && (targetTypeCode == H2EvalTypeCode_F32 || targetTypeCode == H2EvalTypeCode_F64))
        {
            H2EvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == H2CTFEValue_INT
            && (targetTypeCode == H2EvalTypeCode_U8 || targetTypeCode == H2EvalTypeCode_U16
                || targetTypeCode == H2EvalTypeCode_U32 || targetTypeCode == H2EvalTypeCode_U64
                || targetTypeCode == H2EvalTypeCode_UINT || targetTypeCode == H2EvalTypeCode_I8
                || targetTypeCode == H2EvalTypeCode_I16 || targetTypeCode == H2EvalTypeCode_I32
                || targetTypeCode == H2EvalTypeCode_I64 || targetTypeCode == H2EvalTypeCode_INT))
        {
            H2EvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            targetTypeCode == H2EvalTypeCode_RAWPTR
            && (sourceValue->kind == H2CTFEValue_REFERENCE || sourceValue->kind == H2CTFEValue_NULL
                || sourceValue->kind == H2CTFEValue_STRING))
        {
            H2EvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        } else if (
            sourceValue->kind == H2CTFEValue_STRING
            && (targetTypeCode == H2EvalTypeCode_STR_REF
                || targetTypeCode == H2EvalTypeCode_STR_PTR))
        {
            H2EvalValueSetRuntimeTypeCode(inOutValue, targetTypeCode);
        }
    }
    return 0;
}

static int H2EvalForInIndexCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    const H2CTFEValue* sourceValue,
    uint32_t           index,
    int                byRef,
    H2CTFEValue*       outValue,
    int*               outIsConst) {
    const H2CTFEValue* targetValue;
    H2EvalArray*       array;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (ctx == NULL || execCtx == NULL || sourceValue == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    targetValue = H2EvalValueTargetOrSelf(sourceValue);
    if (targetValue->kind == H2CTFEValue_ARRAY) {
        array = H2EvalValueAsArray(targetValue);
        if (array == NULL || index >= array->len) {
            return 0;
        }
        if (byRef) {
            H2EvalValueSetReference(outValue, &array->elems[index]);
        } else {
            *outValue = array->elems[index];
        }
        *outIsConst = 1;
        return 0;
    }
    if (targetValue->kind == H2CTFEValue_STRING) {
        if (index >= targetValue->s.len) {
            return 0;
        }
        if (byRef) {
            H2CTFEValue* byteValue = (H2CTFEValue*)H2ArenaAlloc(
                ((H2EvalProgram*)ctx)->arena, sizeof(H2CTFEValue), (uint32_t)_Alignof(H2CTFEValue));
            if (byteValue == NULL) {
                return ErrorSimple("out of memory");
            }
            H2EvalValueSetInt(byteValue, (int64_t)targetValue->s.bytes[index]);
            H2EvalValueSetRuntimeTypeCode(byteValue, H2EvalTypeCode_U8);
            H2EvalValueSetReference(outValue, byteValue);
            *outIsConst = 1;
            return 0;
        }
        H2EvalValueSetInt(outValue, (int64_t)targetValue->s.bytes[index]);
        H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_U8);
        *outIsConst = 1;
        return 0;
    }
    H2CTFEExecSetReason(execCtx, 0, 0, "for-in loop source is not supported in evaluator backend");
    return 0;
}

static int H2EvalCollectCallArgs(
    H2EvalProgram* p,
    const H2Ast*   ast,
    int32_t        firstArgNode,
    H2CTFEValue**  outArgs,
    uint32_t*      outArgCount,
    int32_t* _Nullable outLastArgNode) {
    H2CTFEValue tempArgs[256];
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
        H2CTFEValue argValue;
        int         argIsConst = 0;
        if (ast->nodes[argNode].kind == H2Ast_CALL_ARG) {
            argExprNode = ast->nodes[argNode].firstChild;
        }
        if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
            *outArgCount = 0;
            *outArgs = NULL;
            return 0;
        }
        if (H2EvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
            return -1;
        }
        if (!argIsConst) {
            *outArgCount = 0;
            *outArgs = NULL;
            return 0;
        }
        H2EvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
        if (ast->nodes[argNode].kind == H2Ast_CALL_ARG
            && (ast->nodes[argNode].flags & H2AstFlag_CALL_ARG_SPREAD) != 0)
        {
            H2EvalArray* array = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&argValue));
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
        *outArgs = (H2CTFEValue*)H2ArenaAlloc(
            p->arena, sizeof(H2CTFEValue) * argCount, (uint32_t)_Alignof(H2CTFEValue));
        if (*outArgs == NULL) {
            return ErrorSimple("out of memory");
        }
        memcpy(*outArgs, tempArgs, sizeof(H2CTFEValue) * argCount);
    }
    *outArgCount = argCount;
    return 1;
}

static int H2EvalCurrentContextFieldByLiteral(
    const H2EvalProgram* p, const char* fieldName, H2CTFEValue* outValue) {
    const H2EvalContext* context;
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

static int H2EvalCurrentContextFieldAddressByLiteral(
    H2EvalProgram* p, const char* fieldName, H2CTFEValue* outValue) {
    H2EvalContext* context;
    if (p == NULL || fieldName == NULL || outValue == NULL) {
        return 0;
    }
    context = p->currentContext != NULL ? (H2EvalContext*)p->currentContext : &p->rootContext;
    if (strcmp(fieldName, "allocator") == 0) {
        H2EvalValueSetReference(outValue, &context->allocator);
        return 1;
    }
    if (strcmp(fieldName, "temp_allocator") == 0) {
        H2EvalValueSetReference(outValue, &context->tempAllocator);
        return 1;
    }
    if (strcmp(fieldName, "logger") == 0) {
        H2EvalValueSetReference(outValue, &context->logger);
        return 1;
    }
    return 0;
}

static int H2EvalCurrentContextField(
    const H2EvalProgram* p,
    const char*          source,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    H2CTFEValue*         outValue) {
    if (source == NULL) {
        return 0;
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "allocator")) {
        return H2EvalCurrentContextFieldByLiteral(p, "allocator", outValue);
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "temp_allocator")) {
        return H2EvalCurrentContextFieldByLiteral(p, "temp_allocator", outValue);
    }
    if (SliceEqCStr(source, nameStart, nameEnd, "logger")) {
        return H2EvalCurrentContextFieldByLiteral(p, "logger", outValue);
    }
    return 0;
}

static int H2EvalMirContextGet(
    void* _Nullable ctx,
    uint32_t        fieldId,
    H2MirExecValue* outValue,
    int*            outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    const char*    fieldName = NULL;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    switch ((H2MirContextField)fieldId) {
        case H2MirContextField_ALLOCATOR:      fieldName = "allocator"; break;
        case H2MirContextField_TEMP_ALLOCATOR: fieldName = "temp_allocator"; break;
        case H2MirContextField_LOGGER:         fieldName = "logger"; break;
        default:                               return 0;
    }
    if (!H2EvalCurrentContextFieldByLiteral(p, fieldName, outValue)) {
        return 0;
    }
    *outIsConst = 1;
    return 1;
}

static int H2EvalMirContextAddr(
    void* _Nullable ctx,
    uint32_t        fieldId,
    H2MirExecValue* outValue,
    int*            outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    const char*    fieldName = NULL;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    switch ((H2MirContextField)fieldId) {
        case H2MirContextField_ALLOCATOR:      fieldName = "allocator"; break;
        case H2MirContextField_TEMP_ALLOCATOR: fieldName = "temp_allocator"; break;
        case H2MirContextField_LOGGER:         fieldName = "logger"; break;
        default:                               return 0;
    }
    if (!H2EvalCurrentContextFieldAddressByLiteral(p, fieldName, outValue)) {
        return 0;
    }
    *outIsConst = 1;
    return 1;
}

static int H2EvalBuildContextOverlay(
    H2EvalProgram* p, int32_t overlayNode, H2EvalContext* outContext, const H2ParsedFile* file);

static int H2EvalMirEvalWithContext(
    void* _Nullable ctx,
    uint32_t        sourceNode,
    H2MirExecValue* outValue,
    int*            outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*       p = (H2EvalProgram*)ctx;
    const H2EvalContext* savedContext;
    H2EvalContext        overlayContext;
    const H2AstNode*     n;
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
    if (n->kind != H2Ast_CALL_WITH_CONTEXT) {
        return 0;
    }
    exprNode = n->firstChild;
    overlayNode = exprNode >= 0 ? p->currentFile->ast.nodes[exprNode].nextSibling : -1;
    if (exprNode < 0) {
        return 0;
    }
    overlayRc = H2EvalBuildContextOverlay(p, overlayNode, &overlayContext, p->currentFile);
    if (overlayRc != 1) {
        return overlayRc < 0 ? -1 : 0;
    }
    savedContext = p->currentContext;
    p->currentContext = &overlayContext;
    rc = H2EvalExecExprCb(p, exprNode, outValue, outIsConst);
    p->currentContext = savedContext;
    if (rc != 0) {
        return -1;
    }
    return *outIsConst ? 1 : 0;
}

static int H2EvalBuildContextOverlay(
    H2EvalProgram* p, int32_t overlayNode, H2EvalContext* outContext, const H2ParsedFile* file) {
    const H2Ast* ast;
    int32_t      bindNode;
    if (p == NULL || outContext == NULL || file == NULL || p->currentFile == NULL) {
        return -1;
    }
    ast = &file->ast;
    *outContext = p->currentContext != NULL ? *p->currentContext : p->rootContext;
    if (overlayNode < 0 || (uint32_t)overlayNode >= ast->len) {
        return 1;
    }
    if (ast->nodes[overlayNode].kind != H2Ast_CONTEXT_OVERLAY) {
        return 0;
    }
    bindNode = ASTFirstChild(ast, overlayNode);
    while (bindNode >= 0) {
        const H2AstNode* bind = &ast->nodes[bindNode];
        int32_t          exprNode = ASTFirstChild(ast, bindNode);
        H2CTFEValue      fieldValue;
        int              fieldIsConst = 0;
        if (bind->kind != H2Ast_CONTEXT_BIND) {
            bindNode = ASTNextSibling(ast, bindNode);
            continue;
        }
        if (exprNode >= 0) {
            if (H2EvalExecExprCb(p, exprNode, &fieldValue, &fieldIsConst) != 0) {
                return -1;
            }
            if (!fieldIsConst) {
                return 0;
            }
        } else if (!H2EvalCurrentContextField(
                       p, file->source, bind->dataStart, bind->dataEnd, &fieldValue))
        {
            if (p->currentExecCtx != NULL) {
                H2CTFEExecSetReason(
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

static void H2EvalValueSetReference(H2CTFEValue* value, H2CTFEValue* target) {
    if (value == NULL) {
        return;
    }
    value->kind = H2CTFEValue_REFERENCE;
    value->i64 = 0;
    value->f64 = 0.0;
    value->b = 0;
    value->typeTag = 0;
    value->s.bytes = (const uint8_t*)target;
    value->s.len = 0;
}

static H2CTFEValue* _Nullable H2EvalValueReferenceTarget(const H2CTFEValue* value) {
    if (value == NULL || value->kind != H2CTFEValue_REFERENCE || value->s.bytes == NULL) {
        return NULL;
    }
    return (H2CTFEValue*)value->s.bytes;
}

static int32_t H2EvalFindNamedAggregateDeclInPackage(
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
    int32_t              typeNode,
    const H2ParsedFile** outFile) {
    const H2AstNode* typeNameNode;
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
    if (typeNameNode->kind != H2Ast_TYPE_NAME) {
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
        const H2ParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const H2AstNode* n = &pkgFile->ast.nodes[nodeId];
            if ((n->kind == H2Ast_STRUCT || n->kind == H2Ast_UNION)
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

static int32_t H2EvalFindNamedAggregateDecl(
    const H2EvalProgram* p,
    const H2ParsedFile*  callerFile,
    int32_t              typeNode,
    const H2ParsedFile** outFile) {
    const H2Package* currentPkg;
    uint32_t         pkgIndex;
    int32_t          nodeId;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = H2EvalFindPackageByFile(p, callerFile);
    if (currentPkg != NULL) {
        nodeId = H2EvalFindNamedAggregateDeclInPackage(currentPkg, callerFile, typeNode, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    if (p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const H2Package* pkg = &p->loader->packages[pkgIndex];
        if (pkg == currentPkg) {
            continue;
        }
        nodeId = H2EvalFindNamedAggregateDeclInPackage(pkg, callerFile, typeNode, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    return -1;
}

static int32_t H2EvalFindNamedEnumDeclInPackage(
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const H2ParsedFile** outFile) {
    uint32_t fileIndex;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (pkg == NULL || callerFile == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const H2ParsedFile* pkgFile = &pkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const H2AstNode* n = &pkgFile->ast.nodes[nodeId];
            if (n->kind == H2Ast_ENUM
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

static int32_t H2EvalFindNamedEnumDecl(
    const H2EvalProgram* p,
    const H2ParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const H2ParsedFile** outFile) {
    const H2Package* currentPkg;
    uint32_t         pkgIndex;
    int32_t          nodeId;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = H2EvalFindPackageByFile(p, callerFile);
    if (currentPkg != NULL) {
        nodeId = H2EvalFindNamedEnumDeclInPackage(
            currentPkg, callerFile, nameStart, nameEnd, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    if (p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const H2Package* pkg = &p->loader->packages[pkgIndex];
        if (pkg == currentPkg) {
            continue;
        }
        nodeId = H2EvalFindNamedEnumDeclInPackage(pkg, callerFile, nameStart, nameEnd, outFile);
        if (nodeId >= 0) {
            return nodeId;
        }
    }
    return -1;
}

static int H2EvalFindEnumVariant(
    const H2ParsedFile* enumFile,
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
    if (child >= 0 && enumFile->ast.nodes[child].kind != H2Ast_FIELD) {
        child = ASTNextSibling(&enumFile->ast, child);
    }
    while (child >= 0) {
        const H2AstNode* fieldNode = &enumFile->ast.nodes[child];
        if (fieldNode->kind == H2Ast_FIELD) {
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

static int H2EvalEnumHasPayloadVariants(const H2ParsedFile* enumFile, int32_t enumNode) {
    int32_t child;
    if (enumFile == NULL || enumNode < 0 || (uint32_t)enumNode >= enumFile->ast.len) {
        return 0;
    }
    child = ASTFirstChild(&enumFile->ast, enumNode);
    while (child >= 0) {
        if (enumFile->ast.nodes[child].kind == H2Ast_FIELD) {
            int32_t valueNode = ASTFirstChild(&enumFile->ast, child);
            if (valueNode >= 0 && (uint32_t)valueNode < enumFile->ast.len
                && enumFile->ast.nodes[valueNode].kind == H2Ast_FIELD)
            {
                return 1;
            }
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    return 0;
}

static int H2EvalZeroInitTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    H2CTFEValue*         outValue,
    int*                 outIsConst);
static int H2EvalResolveAggregateTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  typeFile,
    int32_t              typeNode,
    const H2ParsedFile** outDeclFile,
    int32_t*             outDeclNode);
static int H2EvalExecExprWithTypeNode(
    H2EvalProgram*      p,
    int32_t             exprNode,
    const H2ParsedFile* typeFile,
    int32_t             typeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst);
static int H2EvalExecExprInFileWithType(
    H2EvalProgram*      p,
    const H2ParsedFile* exprFile,
    H2CTFEExecEnv*      env,
    int32_t             exprNode,
    const H2ParsedFile* typeFile,
    int32_t             typeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst);
static int H2EvalExecExprCb(void* ctx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalAssignExprCb(
    void* ctx, H2CTFEExecCtx* execCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalAssignValueExprCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    int32_t            lhsExprNode,
    const H2CTFEValue* inValue,
    H2CTFEValue*       outValue,
    int*               outIsConst);
static int H2EvalMatchPatternCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    const H2CTFEValue* subjectValue,
    int32_t            labelExprNode,
    int*               outMatched);
static int H2EvalZeroInitCb(void* ctx, int32_t typeNode, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalMirZeroInitLocal(
    void*               ctx,
    const H2MirTypeRef* typeRef,
    H2CTFEValue*        outValue,
    int*                outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirCoerceValueForType(
    void* ctx, const H2MirTypeRef* typeRef, H2CTFEValue* inOutValue, H2Diag* _Nullable diag);
static int H2EvalMirIndexValue(
    void*              ctx,
    const H2CTFEValue* base,
    const H2CTFEValue* index,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirIndexAddr(
    void*              ctx,
    const H2CTFEValue* base,
    const H2CTFEValue* index,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirSliceValue(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    const H2CTFEValue* _Nullable start,
    const H2CTFEValue* _Nullable end,
    uint16_t flags,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirAggGetField(
    void*              ctx,
    const H2CTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirAggAddrField(
    void*              ctx,
    const H2CTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirAggSetField(
    void* _Nullable ctx,
    H2CTFEValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirMakeTuple(
    void*              ctx,
    const H2CTFEValue* elems,
    uint32_t           elemCount,
    uint32_t           typeNodeHint,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirMakeVariadicPack(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirTypeRef* _Nullable paramTypeRef,
    uint16_t           callFlags,
    const H2CTFEValue* args,
    uint32_t           argCount,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirHostCall(
    void*              ctx,
    uint32_t           hostId,
    const H2CTFEValue* args,
    uint32_t           argCount,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalTryMirZeroInitType(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    int32_t             typeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    H2CTFEValue*        outValue,
    int*                outIsConst);
static int H2EvalTryMirEvalTopInit(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const H2ParsedFile* _Nullable coerceTypeFile,
    int32_t      coerceTypeNode,
    H2CTFEValue* outValue,
    int*         outIsConst,
    int* _Nullable outSupported);
static int H2EvalMirBuildTopInitProgram(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    H2MirProgram*       outProgram,
    H2EvalMirExecCtx*   outExecCtx,
    uint32_t*           outRootMirFnIndex,
    int*                outSupported);
static void H2EvalMirAdaptOutValue(
    const H2EvalMirExecCtx* c, H2CTFEValue* _Nullable value, int* _Nullable inOutIsConst);
static int H2EvalTryMirEvalExprWithType(
    H2EvalProgram*      p,
    int32_t             exprNode,
    const H2ParsedFile* exprFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const H2ParsedFile* _Nullable typeFile,
    int32_t      typeNode,
    H2CTFEValue* outValue,
    int*         outIsConst,
    int*         outSupported);
static int H2EvalMirEnterFunction(
    void* ctx, uint32_t functionIndex, uint32_t sourceRef, H2Diag* _Nullable diag);
static void H2EvalMirLeaveFunction(void* ctx);
static int  H2EvalMirBindFrame(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2CTFEValue* _Nullable locals,
    uint32_t localCount,
    H2Diag* _Nullable diag);
static void H2EvalMirUnbindFrame(void* _Nullable ctx);
static void H2EvalMirInitExecEnv(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    H2MirExecEnv*       env,
    H2EvalMirExecCtx* _Nullable functionCtx);
static int H2EvalMirEvalBinary(
    void* _Nullable ctx,
    H2TokenKind op,
    const H2MirExecValue* _Nonnull lhs,
    const H2MirExecValue* _Nonnull rhs,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirAllocNew(
    void* _Nullable ctx,
    uint32_t sourceNode,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirContextGet(
    void* _Nullable ctx,
    uint32_t fieldId,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirEvalWithContext(
    void* _Nullable ctx,
    uint32_t sourceNode,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalResolveIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirAssignIdent(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* inValue,
    int*               outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalResolveCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalResolveCallMir(
    void* ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalResolveCallMirPre(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalEvalTopVar(
    H2EvalProgram* p, uint32_t topVarIndex, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalInvokeFunction(
    H2EvalProgram* p,
    int32_t        fnIndex,
    const H2CTFEValue* _Nullable args,
    uint32_t             argCount,
    const H2EvalContext* callContext,
    H2CTFEValue*         outValue,
    int*                 outDidReturn);

static int32_t H2EvalAggregateLookupFieldIndex(
    const H2EvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const H2EvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return (int32_t)i;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const H2EvalAggregateField* field = &agg->fields[i];
        H2EvalAggregate*            embedded = NULL;
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = H2EvalValueAsAggregate(&field->value);
        if (embedded != NULL) {
            int32_t nested = H2EvalAggregateLookupFieldIndex(embedded, source, nameStart, nameEnd);
            if (nested >= 0) {
                return (int32_t)i;
            }
        }
    }
    return -1;
}

static int32_t H2EvalAggregateLookupDirectFieldIndex(
    const H2EvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const H2EvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static H2EvalAggregateField* _Nullable H2EvalAggregateLookupField(
    H2EvalAggregate* agg,
    const char*      source,
    uint32_t         nameStart,
    uint32_t         nameEnd,
    H2EvalAggregate** _Nullable outOwner) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        H2EvalAggregateField* field = &agg->fields[i];
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
        H2EvalAggregateField* field = &agg->fields[i];
        H2EvalAggregate*      embedded = NULL;
        H2EvalAggregateField* nested = NULL;
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = H2EvalValueAsAggregate(&field->value);
        if (embedded == NULL) {
            continue;
        }
        nested = H2EvalAggregateLookupField(embedded, source, nameStart, nameEnd, outOwner);
        if (nested != NULL) {
            return nested;
        }
    }
    return NULL;
}

static uint32_t H2EvalAggregateFieldBindingCount(const H2EvalAggregate* agg) {
    uint32_t i;
    uint32_t count = 0;
    if (agg == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const H2EvalAggregateField* field = &agg->fields[i];
        count++;
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) != 0) {
            H2EvalAggregate* embedded = H2EvalValueAsAggregate(&field->value);
            count += H2EvalAggregateFieldBindingCount(embedded);
        }
    }
    return count;
}

static int H2EvalAppendAggregateFieldBindings(
    H2CTFEExecBinding*    fieldBindings,
    uint32_t              bindingCap,
    H2CTFEExecEnv*        fieldFrame,
    H2EvalAggregateField* field) {
    H2EvalAggregate* embedded;
    uint32_t         i;
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
    if ((field->flags & H2AstFlag_FIELD_EMBEDDED) == 0) {
        return 0;
    }
    embedded = H2EvalValueAsAggregate(&field->value);
    if (embedded == NULL) {
        return 0;
    }
    for (i = 0; i < embedded->fieldLen; i++) {
        if (H2EvalAppendAggregateFieldBindings(
                fieldBindings, bindingCap, fieldFrame, &embedded->fields[i])
            != 0)
        {
            return -1;
        }
    }
    return 0;
}

typedef struct {
    uint32_t    nameStart;
    uint32_t    nameEnd;
    int32_t     topFieldIndex;
    H2CTFEValue value;
} H2EvalExplicitAggregateField;

static int H2EvalAggregateGetFieldValue(
    const H2EvalAggregate* agg,
    const char*            source,
    uint32_t               nameStart,
    uint32_t               nameEnd,
    H2CTFEValue*           outValue) {
    int32_t fieldIndex;
    if (outValue == NULL) {
        return 0;
    }
    fieldIndex = H2EvalAggregateLookupFieldIndex(agg, source, nameStart, nameEnd);
    if (fieldIndex < 0) {
        return 0;
    }
    {
        const H2EvalAggregateField* field = &agg->fields[fieldIndex];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            *outValue = field->value;
            return 1;
        }
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) != 0) {
            H2EvalAggregate* embedded = H2EvalValueAsAggregate(&field->value);
            if (embedded != NULL) {
                return H2EvalAggregateGetFieldValue(embedded, source, nameStart, nameEnd, outValue);
            }
        }
    }
    return 0;
}

static H2CTFEValue* _Nullable H2EvalAggregateLookupFieldValuePtr(
    H2EvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        H2EvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return &field->value;
        }
    }
    for (i = 0; i < agg->fieldLen; i++) {
        H2EvalAggregateField* field = &agg->fields[i];
        H2EvalAggregate*      embedded;
        H2CTFEValue*          nested;
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = H2EvalValueAsAggregate(&field->value);
        if (embedded == NULL) {
            continue;
        }
        nested = H2EvalAggregateLookupFieldValuePtr(embedded, source, nameStart, nameEnd);
        if (nested != NULL) {
            return nested;
        }
    }
    return NULL;
}

static int H2EvalAggregateSetFieldValue(
    H2EvalAggregate*   agg,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* inValue,
    H2CTFEValue* _Nullable outValue) {
    uint32_t i;
    if (agg == NULL || source == NULL || inValue == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        H2EvalAggregateField* field = &agg->fields[i];
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
        H2EvalAggregateField* field = &agg->fields[i];
        H2EvalAggregate*      embedded = NULL;
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) == 0) {
            continue;
        }
        embedded = H2EvalValueAsAggregate(&field->value);
        if (embedded != NULL
            && H2EvalAggregateSetFieldValue(
                embedded, source, nameStart, nameEnd, inValue, outValue))
        {
            return 1;
        }
    }
    return 0;
}

static H2EvalAggregateField* _Nullable H2EvalAggregateFindDirectField(
    H2EvalAggregate* agg, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (agg == NULL || source == NULL) {
        return NULL;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        H2EvalAggregateField* field = &agg->fields[i];
        if (SliceEqSlice(
                source, nameStart, nameEnd, agg->file->source, field->nameStart, field->nameEnd))
        {
            return field;
        }
    }
    return NULL;
}

static int H2EvalValueSetFieldPath(
    H2CTFEValue*       value,
    const char*        source,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* inValue) {
    uint32_t i;
    if (value == NULL || source == NULL || inValue == NULL) {
        return 0;
    }
    for (i = nameStart; i < nameEnd; i++) {
        if (source[i] == '.') {
            H2CTFEValue*          childValue = NULL;
            H2EvalAggregateField* field;
            H2EvalAggregate*      agg = H2EvalValueAsAggregate(value);
            H2EvalTaggedEnum*     tagged = H2EvalValueAsTaggedEnum(value);
            if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
                agg = tagged->payload;
            }
            if (agg == NULL) {
                agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(value));
            }
            if (agg == NULL) {
                return 0;
            }
            field = H2EvalAggregateFindDirectField(agg, source, nameStart, i);
            if (field == NULL) {
                return 0;
            }
            childValue = &field->value;
            return H2EvalValueSetFieldPath(childValue, source, i + 1u, nameEnd, inValue);
        }
    }
    if (value->kind == H2CTFEValue_STRING) {
        if (SliceEqCStr(source, nameStart, nameEnd, "len") && inValue->kind == H2CTFEValue_INT
            && inValue->i64 >= 0)
        {
            value->s.len = (uint32_t)inValue->i64;
            return 1;
        }
        return 0;
    }
    {
        H2EvalAggregate*  agg = H2EvalValueAsAggregate(value);
        H2EvalTaggedEnum* tagged = H2EvalValueAsTaggedEnum(value);
        if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
            agg = tagged->payload;
        }
        if (agg == NULL) {
            agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(value));
        }
        if (agg != NULL) {
            return H2EvalAggregateSetFieldValue(agg, source, nameStart, nameEnd, inValue, NULL);
        }
    }
    return 0;
}

static int H2EvalAggregateHasReservedFields(const H2EvalAggregate* agg) {
    uint32_t i;
    if (agg == NULL) {
        return 0;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        const H2EvalAggregateField* field = &agg->fields[i];
        if ((field->_reserved & 1u) != 0u) {
            return 1;
        }
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) != 0
            && H2EvalAggregateHasReservedFields(H2EvalValueAsAggregate(&field->value)))
        {
            return 1;
        }
    }
    return 0;
}

static int H2EvalReplayReservedAggregateFields(
    H2CTFEValue* target, const H2EvalAggregate* sourceAgg) {
    uint32_t i;
    if (target == NULL || sourceAgg == NULL) {
        return 1;
    }
    for (i = 0; i < sourceAgg->fieldLen; i++) {
        const H2EvalAggregateField* field = &sourceAgg->fields[i];
        if ((field->_reserved & 1u) != 0u
            && !H2EvalValueSetFieldPath(
                target, sourceAgg->file->source, field->nameStart, field->nameEnd, &field->value))
        {
            return 0;
        }
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) != 0
            && !H2EvalReplayReservedAggregateFields(target, H2EvalValueAsAggregate(&field->value)))
        {
            return 0;
        }
    }
    return 1;
}

static int H2EvalFinalizeAggregateVarArrays(H2EvalProgram* p, H2EvalAggregate* agg) {
    uint32_t i;
    if (p == NULL || agg == NULL) {
        return -1;
    }
    for (i = 0; i < agg->fieldLen; i++) {
        H2EvalAggregateField* field = &agg->fields[i];
        const H2AstNode*      typeNode;
        H2CTFEValue           lenValue;
        int64_t               len = 0;
        H2EvalArray*          array;
        uint32_t              j;
        if (field->typeNode < 0 || (uint32_t)field->typeNode >= agg->file->ast.len) {
            continue;
        }
        typeNode = &agg->file->ast.nodes[field->typeNode];
        if (typeNode->kind != H2Ast_TYPE_VARRAY) {
            continue;
        }
        if (!H2EvalAggregateGetFieldValue(
                agg, agg->file->source, typeNode->dataStart, typeNode->dataEnd, &lenValue)
            || H2CTFEValueToInt64(&lenValue, &len) != 0 || len < 0)
        {
            return 0;
        }
        array = H2EvalAllocArrayView(
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
            array->elems = (H2CTFEValue*)H2ArenaAlloc(
                p->arena, sizeof(H2CTFEValue) * array->len, (uint32_t)_Alignof(H2CTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(H2CTFEValue) * array->len);
            for (j = 0; j < array->len; j++) {
                int elemIsConst = 0;
                if (H2EvalZeroInitTypeNode(
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
        H2EvalValueSetArray(&field->value, agg->file, field->typeNode, array);
    }
    return 1;
}

static int H2EvalBuildTaggedEnumPayload(
    H2EvalProgram*      p,
    const H2ParsedFile* enumFile,
    int32_t             variantNode,
    int32_t             compoundLitNode,
    H2CTFEValue*        outValue,
    int*                outIsConst) {
    uint32_t         fieldCount = 0;
    uint32_t         fieldIndex = 0;
    int32_t          child;
    H2EvalAggregate* agg;
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
        if (enumFile->ast.nodes[child].kind == H2Ast_FIELD) {
            fieldCount++;
        }
        child = ASTNextSibling(&enumFile->ast, child);
    }
    agg = (H2EvalAggregate*)H2ArenaAlloc(
        p->arena, sizeof(H2EvalAggregate), (uint32_t)_Alignof(H2EvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = enumFile;
    agg->nodeId = variantNode;
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (H2EvalAggregateField*)H2ArenaAlloc(
            p->arena,
            sizeof(H2EvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(H2EvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(H2EvalAggregateField) * fieldCount);
    }
    child = ASTFirstChild(&enumFile->ast, variantNode);
    while (child >= 0) {
        const H2AstNode* variantField = &enumFile->ast.nodes[child];
        if (variantField->kind == H2Ast_FIELD) {
            int32_t               fieldTypeNode = ASTFirstChild(&enumFile->ast, child);
            int                   fieldIsConst = 0;
            H2EvalAggregateField* field = &agg->fields[fieldIndex++];
            field->nameStart = variantField->dataStart;
            field->nameEnd = variantField->dataEnd;
            field->typeNode = fieldTypeNode;
            if (H2EvalZeroInitTypeNode(p, enumFile, fieldTypeNode, &field->value, &fieldIsConst)
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
        const H2AstNode* compoundField = &p->currentFile->ast.nodes[fieldNode];
        int32_t          valueNode = ASTFirstChild(&p->currentFile->ast, fieldNode);
        H2CTFEValue      fieldValue;
        int              fieldIsConst = 0;
        if (compoundField->kind != H2Ast_COMPOUND_FIELD || valueNode < 0) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
            return -1;
        }
        if (!fieldIsConst
            || !H2EvalAggregateSetFieldValue(
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
    H2EvalValueSetAggregate(outValue, enumFile, variantNode, agg);
    *outIsConst = 1;
    return 0;
}

static int H2EvalZeroInitAggregateValue(
    const H2EvalProgram* p,
    const H2ParsedFile*  declFile,
    int32_t              declNode,
    H2CTFEValue*         outValue,
    int*                 outIsConst) {
    const H2AstNode* aggregateDecl;
    H2EvalAggregate* agg;
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
    if (aggregateDecl->kind != H2Ast_STRUCT && aggregateDecl->kind != H2Ast_UNION
        && aggregateDecl->kind != H2Ast_TYPE_ANON_STRUCT
        && aggregateDecl->kind != H2Ast_TYPE_ANON_UNION)
    {
        return 0;
    }
    child = ASTFirstChild(&declFile->ast, declNode);
    while (child >= 0) {
        if (declFile->ast.nodes[child].kind == H2Ast_FIELD) {
            fieldCount++;
        }
        child = ASTNextSibling(&declFile->ast, child);
    }
    agg = (H2EvalAggregate*)H2ArenaAlloc(
        p->arena, sizeof(H2EvalAggregate), (uint32_t)_Alignof(H2EvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = declFile;
    agg->nodeId = declNode;
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (H2EvalAggregateField*)H2ArenaAlloc(
            p->arena,
            sizeof(H2EvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(H2EvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(H2EvalAggregateField) * fieldCount);
    }
    child = ASTFirstChild(&declFile->ast, declNode);
    while (child >= 0) {
        const H2AstNode* fieldNode = &declFile->ast.nodes[child];
        if (fieldNode->kind == H2Ast_FIELD) {
            int32_t fieldTypeNode = ASTFirstChild(&declFile->ast, child);
            int32_t fieldDefaultNode =
                fieldTypeNode >= 0 ? ASTNextSibling(&declFile->ast, fieldTypeNode) : -1;
            int                   fieldIsConst = 0;
            H2EvalAggregateField* field = &agg->fields[fieldIndex++];
            field->nameStart = fieldNode->dataStart;
            field->nameEnd = fieldNode->dataEnd;
            field->flags = (uint16_t)fieldNode->flags;
            field->typeNode = fieldTypeNode;
            field->defaultExprNode = fieldDefaultNode;
            if (H2EvalZeroInitTypeNode(p, declFile, fieldTypeNode, &field->value, &fieldIsConst)
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
    H2EvalValueSetAggregate(outValue, declFile, declNode, agg);
    if (outIsConst != NULL) {
        *outIsConst = 1;
    }
    return 0;
}

static int H2EvalTypeValueFromTypeNode(
    H2EvalProgram* p, const H2ParsedFile* file, int32_t typeNode, H2CTFEValue* outValue) {
    const H2AstNode*     n;
    int32_t              childNode;
    int32_t              typeCode = H2EvalTypeCode_INVALID;
    H2EvalReflectedType* rt;
    uint32_t             arrayLen = 0;
    if (p == NULL || file == NULL || outValue == NULL || typeNode < 0
        || (uint32_t)typeNode >= file->ast.len)
    {
        return 0;
    }
    if (H2EvalTypeCodeFromTypeNode(file, typeNode, &typeCode)) {
        H2EvalValueSetSimpleTypeValue(outValue, typeCode);
        return 1;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind == H2Ast_TYPE_NAME) {
        return H2EvalResolveTypeValueName(p, file, n->dataStart, n->dataEnd, outValue);
    }
    if (n->kind != H2Ast_TYPE_PTR && n->kind != H2Ast_TYPE_REF && n->kind != H2Ast_TYPE_ARRAY) {
        return 0;
    }
    childNode = ASTFirstChild(&file->ast, typeNode);
    if (childNode < 0 || !H2EvalTypeValueFromTypeNode(p, file, childNode, outValue)) {
        return 0;
    }
    rt = (H2EvalReflectedType*)H2ArenaAlloc(
        p->arena, sizeof(H2EvalReflectedType), (uint32_t)_Alignof(H2EvalReflectedType));
    if (rt == NULL) {
        return -1;
    }
    memset(rt, 0, sizeof(*rt));
    if (n->kind == H2Ast_TYPE_PTR) {
        rt->kind = H2EvalReflectType_PTR;
        rt->namedKind = H2EvalTypeKind_POINTER;
    } else if (n->kind == H2Ast_TYPE_REF) {
        rt->kind = H2EvalReflectType_PTR;
        rt->namedKind = H2EvalTypeKind_REFERENCE;
    } else {
        if (!H2EvalParseUintSlice(file->source, n->dataStart, n->dataEnd, &arrayLen)) {
            return 0;
        }
        rt->kind = H2EvalReflectType_ARRAY;
        rt->namedKind = H2EvalTypeKind_ARRAY;
        rt->arrayLen = arrayLen;
    }
    rt->elemType = *outValue;
    H2EvalValueSetReflectedTypeValue(outValue, rt);
    return 1;
}

static H2CTFEExecBinding* _Nullable H2EvalFindBinding(
    const H2CTFEExecCtx* _Nullable execCtx,
    const H2ParsedFile* file,
    uint32_t            nameStart,
    uint32_t            nameEnd);

static int H2EvalTypeValueFromExprNode(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    const H2Ast*        ast,
    int32_t             exprNode,
    H2CTFEValue*        outValue) {
    const H2AstNode* n;
    if (p == NULL || file == NULL || ast == NULL || outValue == NULL || exprNode < 0
        || (uint32_t)exprNode >= ast->len)
    {
        return 0;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == H2Ast_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return 0;
        }
        n = &ast->nodes[exprNode];
    }
    if (H2EvalTypeValueFromTypeNode(p, file, exprNode, outValue)) {
        return 1;
    }
    if (n->kind == H2Ast_IDENT) {
        H2CTFEExecBinding* binding = H2EvalFindBinding(
            p->currentExecCtx, file, n->dataStart, n->dataEnd);
        const H2ParsedFile* localTypeFile = NULL;
        int32_t             localTypeNode = -1;
        int32_t             visibleLocalTypeNode = -1;
        if (binding != NULL && binding->typeNode >= 0
            && !(
                file->ast.nodes[binding->typeNode].kind == H2Ast_TYPE_NAME
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[binding->typeNode].dataStart,
                    file->ast.nodes[binding->typeNode].dataEnd,
                    "anytype"))
            && H2EvalTypeValueFromTypeNode(p, file, binding->typeNode, outValue))
        {
            return 1;
        }
        if (H2EvalFindVisibleLocalTypeNodeByName(
                file, n->start, n->dataStart, n->dataEnd, &visibleLocalTypeNode)
            && visibleLocalTypeNode >= 0
            && !(
                file->ast.nodes[visibleLocalTypeNode].kind == H2Ast_TYPE_NAME
                && SliceEqCStr(
                    file->source,
                    file->ast.nodes[visibleLocalTypeNode].dataStart,
                    file->ast.nodes[visibleLocalTypeNode].dataEnd,
                    "anytype"))
            && H2EvalTypeValueFromTypeNode(p, file, visibleLocalTypeNode, outValue))
        {
            return 1;
        }
        if (H2EvalMirLookupLocalTypeNode(
                p, n->dataStart, n->dataEnd, &localTypeFile, &localTypeNode)
            && localTypeFile != NULL && localTypeNode >= 0
            && !(
                localTypeFile->ast.nodes[localTypeNode].kind == H2Ast_TYPE_NAME
                && SliceEqCStr(
                    localTypeFile->source,
                    localTypeFile->ast.nodes[localTypeNode].dataStart,
                    localTypeFile->ast.nodes[localTypeNode].dataEnd,
                    "anytype"))
            && H2EvalTypeValueFromTypeNode(p, localTypeFile, localTypeNode, outValue))
        {
            return 1;
        }
        return H2EvalResolveTypeValueName(p, file, n->dataStart, n->dataEnd, outValue);
    }
    return 0;
}

static int H2EvalZeroInitTypeValue(
    const H2EvalProgram* p, const H2CTFEValue* typeValue, H2CTFEValue* outValue, int* outIsConst) {
    int32_t              typeCode = H2EvalTypeCode_INVALID;
    H2EvalReflectedType* rt;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || typeValue == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (H2EvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        switch (typeCode) {
            case H2EvalTypeCode_BOOL:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 0;
            case H2EvalTypeCode_F32:
            case H2EvalTypeCode_F64:
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                *outIsConst = 1;
                return 0;
            case H2EvalTypeCode_STR_REF:
            case H2EvalTypeCode_STR_PTR:
                outValue->kind = H2CTFEValue_STRING;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                H2EvalValueSetRuntimeTypeCode(outValue, typeCode);
                *outIsConst = 1;
                return 0;
            case H2EvalTypeCode_RAWPTR:
                H2EvalValueSetNull(outValue);
                H2EvalValueSetRuntimeTypeCode(outValue, typeCode);
                *outIsConst = 1;
                return 0;
            case H2EvalTypeCode_U8:
            case H2EvalTypeCode_U16:
            case H2EvalTypeCode_U32:
            case H2EvalTypeCode_U64:
            case H2EvalTypeCode_UINT:
            case H2EvalTypeCode_I8:
            case H2EvalTypeCode_I16:
            case H2EvalTypeCode_I32:
            case H2EvalTypeCode_I64:
            case H2EvalTypeCode_INT:
                H2EvalValueSetInt(outValue, 0);
                *outIsConst = 1;
                return 0;
            default: return 0;
        }
    }
    rt = H2EvalValueAsReflectedType(typeValue);
    if (rt == NULL) {
        return 0;
    }
    if (rt->kind == H2EvalReflectType_NAMED) {
        const H2AstNode* declNode = NULL;
        if (rt->file == NULL || rt->nodeId < 0 || (uint32_t)rt->nodeId >= rt->file->ast.len) {
            return 0;
        }
        declNode = &rt->file->ast.nodes[rt->nodeId];
        if (rt->namedKind == H2EvalTypeKind_ALIAS) {
            int32_t     baseTypeNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            H2CTFEValue baseTypeValue;
            if (baseTypeNode < 0
                || !H2EvalTypeValueFromTypeNode(
                    (H2EvalProgram*)p, rt->file, baseTypeNode, &baseTypeValue))
            {
                return 0;
            }
            if (H2EvalZeroInitTypeValue(p, &baseTypeValue, outValue, outIsConst) != 0) {
                return -1;
            }
            if (*outIsConst) {
                outValue->typeTag = H2EvalMakeAliasTag(rt->file, rt->nodeId);
            }
            return 0;
        }
        if (rt->namedKind == H2EvalTypeKind_STRUCT || rt->namedKind == H2EvalTypeKind_UNION) {
            return H2EvalZeroInitAggregateValue(p, rt->file, rt->nodeId, outValue, outIsConst);
        }
        if (rt->namedKind == H2EvalTypeKind_ENUM && declNode != NULL) {
            int32_t  variantNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            uint32_t tagIndex = 0;
            while (variantNode >= 0) {
                if (rt->file->ast.nodes[variantNode].kind == H2Ast_FIELD) {
                    H2EvalValueSetTaggedEnum(
                        (H2EvalProgram*)p,
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
                if (rt->file->ast.nodes[variantNode].kind == H2Ast_FIELD) {
                    tagIndex++;
                }
                variantNode = ASTNextSibling(&rt->file->ast, variantNode);
            }
        }
        return 0;
    }
    if (rt->kind == H2EvalReflectType_PTR) {
        H2EvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (rt->kind == H2EvalReflectType_ARRAY) {
        uint32_t     i;
        H2EvalArray* array = (H2EvalArray*)H2ArenaAlloc(
            p->arena, sizeof(H2EvalArray), (uint32_t)_Alignof(H2EvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = rt->file;
        array->typeNode = rt->nodeId;
        array->len = rt->arrayLen;
        if (rt->arrayLen > 0) {
            array->elems = (H2CTFEValue*)H2ArenaAlloc(
                p->arena, sizeof(H2CTFEValue) * rt->arrayLen, (uint32_t)_Alignof(H2CTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(H2CTFEValue) * rt->arrayLen);
            for (i = 0; i < rt->arrayLen; i++) {
                int elemIsConst = 0;
                if (H2EvalZeroInitTypeValue(p, &rt->elemType, &array->elems[i], &elemIsConst) != 0)
                {
                    return -1;
                }
                if (!elemIsConst) {
                    return 0;
                }
            }
        }
        H2EvalValueSetArray(outValue, rt->file, rt->nodeId, array);
        *outIsConst = 1;
        return 0;
    }
    return 0;
}

static int H2EvalTypeKindOfValue(const H2CTFEValue* typeValue, int32_t* outKind) {
    int32_t              typeCode = H2EvalTypeCode_INVALID;
    H2EvalReflectedType* rt;
    if (outKind != NULL) {
        *outKind = 0;
    }
    if (typeValue == NULL || outKind == NULL || typeValue->kind != H2CTFEValue_TYPE) {
        return 0;
    }
    if (H2EvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        *outKind = H2EvalTypeKind_PRIMITIVE;
        return 1;
    }
    rt = H2EvalValueAsReflectedType(typeValue);
    if (rt == NULL) {
        return 0;
    }
    *outKind = (int32_t)rt->namedKind;
    return 1;
}

static int H2EvalTypeNameOfValue(H2CTFEValue* typeValue, H2CTFEValue* outValue) {
    int32_t              typeCode = H2EvalTypeCode_INVALID;
    H2EvalReflectedType* rt;
    if (typeValue == NULL || outValue == NULL || typeValue->kind != H2CTFEValue_TYPE) {
        return 0;
    }
    if (H2EvalValueGetSimpleTypeCode(typeValue, &typeCode)) {
        switch (typeCode) {
            case H2EvalTypeCode_BOOL: H2EvalValueSetStringSlice(outValue, "bool", 0, 4); return 1;
            case H2EvalTypeCode_U8:   H2EvalValueSetStringSlice(outValue, "u8", 0, 2); return 1;
            case H2EvalTypeCode_U16:  H2EvalValueSetStringSlice(outValue, "u16", 0, 3); return 1;
            case H2EvalTypeCode_U32:  H2EvalValueSetStringSlice(outValue, "u32", 0, 3); return 1;
            case H2EvalTypeCode_U64:  H2EvalValueSetStringSlice(outValue, "u64", 0, 3); return 1;
            case H2EvalTypeCode_UINT: H2EvalValueSetStringSlice(outValue, "uint", 0, 4); return 1;
            case H2EvalTypeCode_I8:   H2EvalValueSetStringSlice(outValue, "i8", 0, 2); return 1;
            case H2EvalTypeCode_I16:  H2EvalValueSetStringSlice(outValue, "i16", 0, 3); return 1;
            case H2EvalTypeCode_I32:  H2EvalValueSetStringSlice(outValue, "i32", 0, 3); return 1;
            case H2EvalTypeCode_I64:  H2EvalValueSetStringSlice(outValue, "i64", 0, 3); return 1;
            case H2EvalTypeCode_INT:  H2EvalValueSetStringSlice(outValue, "int", 0, 3); return 1;
            case H2EvalTypeCode_F32:  H2EvalValueSetStringSlice(outValue, "f32", 0, 3); return 1;
            case H2EvalTypeCode_F64:  H2EvalValueSetStringSlice(outValue, "f64", 0, 3); return 1;
            case H2EvalTypeCode_RAWPTR:
                H2EvalValueSetStringSlice(outValue, "rawptr", 0, 6);
                return 1;
            case H2EvalTypeCode_TYPE: H2EvalValueSetStringSlice(outValue, "type", 0, 4); return 1;
            case H2EvalTypeCode_ANYTYPE:
                H2EvalValueSetStringSlice(outValue, "anytype", 0, 7);
                return 1;
            default: return 0;
        }
    }
    rt = H2EvalValueAsReflectedType(typeValue);
    if (rt == NULL || rt->kind != H2EvalReflectType_NAMED || rt->file == NULL || rt->nodeId < 0
        || (uint32_t)rt->nodeId >= rt->file->ast.len)
    {
        return 0;
    }
    H2EvalValueSetStringSlice(
        outValue,
        rt->file->source,
        rt->file->ast.nodes[rt->nodeId].dataStart,
        rt->file->ast.nodes[rt->nodeId].dataEnd);
    return 1;
}

static int H2EvalZeroInitTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    H2CTFEValue*         outValue,
    int*                 outIsConst) {
    const H2AstNode*    typeNameNode;
    const H2Package*    currentPkg;
    const H2ParsedFile* aggregateFile = NULL;
    const H2ParsedFile* aliasFile = NULL;
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
        case H2Ast_TYPE_NAME: {
            uint32_t            dot = typeNameNode->dataStart;
            const H2ParsedFile* enumFile = NULL;
            int32_t             enumNode = -1;
            int32_t             variantNode = -1;
            uint32_t            tagIndex = 0;
            while (dot < typeNameNode->dataEnd && file->source[dot] != '.') {
                dot++;
            }
            if (H2EvalTypeNodeIsTemplateParamName(file, typeNode)) {
                H2EvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (dot < typeNameNode->dataEnd) {
                enumNode = H2EvalFindNamedEnumDecl(
                    p, file, typeNameNode->dataStart, dot, &enumFile);
                if (enumNode >= 0 && enumFile != NULL
                    && H2EvalFindEnumVariant(
                        enumFile,
                        enumNode,
                        file->source,
                        dot + 1u,
                        typeNameNode->dataEnd,
                        &variantNode,
                        &tagIndex))
                {
                    const H2AstNode* variantField = &enumFile->ast.nodes[variantNode];
                    H2CTFEValue      payloadValue;
                    H2EvalAggregate* payload = NULL;
                    int              payloadIsConst = 0;
                    if (H2EvalBuildTaggedEnumPayload(
                            (H2EvalProgram*)p,
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
                    payload = H2EvalValueAsAggregate(&payloadValue);
                    H2EvalValueSetTaggedEnum(
                        (H2EvalProgram*)p,
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
                outValue->kind = H2CTFEValue_BOOL;
                outValue->b = 0;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_BOOL);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f32")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f64"))
            {
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                H2EvalValueSetRuntimeTypeCode(
                    outValue,
                    SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "f32")
                        ? H2EvalTypeCode_F32
                        : H2EvalTypeCode_F64);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "string")
                || SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "str"))
            {
                outValue->kind = H2CTFEValue_STRING;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_STR_REF);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(file->source, typeNameNode->dataStart, typeNameNode->dataEnd, "rawptr"))
            {
                H2EvalValueSetNull(outValue);
                H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_RAWPTR);
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
                int32_t typeCode = H2EvalTypeCode_INVALID;
                H2EvalValueSetInt(outValue, 0);
                if (H2EvalBuiltinTypeCode(
                        file->source, typeNameNode->dataStart, typeNameNode->dataEnd, &typeCode))
                {
                    H2EvalValueSetRuntimeTypeCode(outValue, typeCode);
                }
                *outIsConst = 1;
                return 0;
            }
            if (H2EvalResolveAliasCastTargetNode(
                    p, file, typeNode, &aliasFile, &aliasNode, &aliasTargetNode)
                && aliasFile != NULL && aliasNode >= 0 && aliasTargetNode >= 0)
            {
                if (H2EvalZeroInitTypeNode(p, aliasFile, aliasTargetNode, outValue, outIsConst)
                    != 0)
                {
                    return -1;
                }
                if (*outIsConst) {
                    outValue->typeTag = H2EvalMakeAliasTag(aliasFile, aliasNode);
                }
                return 0;
            }
            currentPkg = H2EvalFindPackageByFile(p, file);
            if (currentPkg == NULL) {
                return 0;
            }
            aggregateNode = H2EvalFindNamedAggregateDecl(p, file, typeNode, &aggregateFile);
            if (aggregateNode >= 0 && aggregateFile != NULL) {
                return H2EvalZeroInitAggregateValue(
                    p, aggregateFile, aggregateNode, outValue, outIsConst);
            }
            {
                H2CTFEValue typeValue;
                int32_t     topConstIndex = H2EvalFindTopConstBySlice(
                    p, file, typeNameNode->dataStart, typeNameNode->dataEnd);
                if (topConstIndex >= 0) {
                    int typeIsConst = 0;
                    if (H2EvalEvalTopConst(
                            (H2EvalProgram*)p, (uint32_t)topConstIndex, &typeValue, &typeIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (typeIsConst && typeValue.kind == H2CTFEValue_TYPE) {
                        return H2EvalZeroInitTypeValue(p, &typeValue, outValue, outIsConst);
                    }
                }
                if (H2EvalTypeValueFromTypeNode((H2EvalProgram*)p, file, typeNode, &typeValue)) {
                    return H2EvalZeroInitTypeValue(p, &typeValue, outValue, outIsConst);
                }
            }
            return 0;
        }
        case H2Ast_TYPE_REF: {
            int32_t childNode = ASTFirstChild(&file->ast, typeNode);
            if (childNode >= 0 && (uint32_t)childNode < file->ast.len) {
                const H2AstNode* child = &file->ast.nodes[childNode];
                if (child->kind == H2Ast_TYPE_NAME
                    && SliceEqCStr(file->source, child->dataStart, child->dataEnd, "str"))
                {
                    outValue->kind = H2CTFEValue_STRING;
                    outValue->i64 = 0;
                    outValue->f64 = 0.0;
                    outValue->b = 0;
                    outValue->typeTag = 0;
                    outValue->s.bytes = NULL;
                    outValue->s.len = 0;
                    H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_STR_REF);
                    *outIsConst = 1;
                    return 0;
                }
                if (child->kind == H2Ast_TYPE_SLICE) {
                    int32_t      elemTypeNode = ASTFirstChild(&file->ast, childNode);
                    H2EvalArray* array = H2EvalAllocArrayView(
                        p, file, typeNode, elemTypeNode, NULL, 0);
                    if (array == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    H2EvalValueSetArray(outValue, file, typeNode, array);
                    *outIsConst = 1;
                    return 0;
                }
            }
            H2EvalValueSetNull(outValue);
            if (H2EvalResolveNullCastTypeTag(file, typeNode, &nullTag)) {
                outValue->typeTag = nullTag;
            }
            *outIsConst = 1;
            return 0;
        }
        case H2Ast_TYPE_PTR:
        case H2Ast_TYPE_MUTREF:
            H2EvalValueSetNull(outValue);
            if (H2EvalResolveNullCastTypeTag(file, typeNode, &nullTag)) {
                outValue->typeTag = nullTag;
            }
            *outIsConst = 1;
            return 0;
        case H2Ast_TYPE_OPTIONAL:
            H2EvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        case H2Ast_TYPE_ARRAY: {
            const H2AstNode* arrayTypeNode = &file->ast.nodes[typeNode];
            int32_t          elemTypeNode = ASTFirstChild(&file->ast, typeNode);
            uint32_t         len = 0;
            uint32_t         i;
            H2EvalArray*     array;
            if (elemTypeNode < 0
                || !H2EvalParseUintSlice(
                    file->source, arrayTypeNode->dataStart, arrayTypeNode->dataEnd, &len))
            {
                return 0;
            }
            array = (H2EvalArray*)H2ArenaAlloc(
                p->arena, sizeof(H2EvalArray), (uint32_t)_Alignof(H2EvalArray));
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array, 0, sizeof(*array));
            array->file = file;
            array->typeNode = typeNode;
            array->elemTypeNode = elemTypeNode;
            array->len = len;
            if (len > 0) {
                array->elems = (H2CTFEValue*)H2ArenaAlloc(
                    p->arena, sizeof(H2CTFEValue) * len, (uint32_t)_Alignof(H2CTFEValue));
                if (array->elems == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(array->elems, 0, sizeof(H2CTFEValue) * len);
                for (i = 0; i < len; i++) {
                    int elemIsConst = 0;
                    if (H2EvalZeroInitTypeNode(
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
            H2EvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case H2Ast_TYPE_VARRAY: {
            int32_t      elemTypeNode = ASTFirstChild(&file->ast, typeNode);
            H2EvalArray* array = H2EvalAllocArrayView(p, file, typeNode, elemTypeNode, NULL, 0);
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            H2EvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case H2Ast_TYPE_TUPLE: {
            uint32_t     len = AstListCount(&file->ast, typeNode);
            uint32_t     i;
            H2EvalArray* array = (H2EvalArray*)H2ArenaAlloc(
                p->arena, sizeof(H2EvalArray), (uint32_t)_Alignof(H2EvalArray));
            if (array == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array, 0, sizeof(*array));
            array->file = file;
            array->typeNode = typeNode;
            array->len = len;
            if (len > 0) {
                array->elems = (H2CTFEValue*)H2ArenaAlloc(
                    p->arena, sizeof(H2CTFEValue) * len, (uint32_t)_Alignof(H2CTFEValue));
                if (array->elems == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(array->elems, 0, sizeof(H2CTFEValue) * len);
                for (i = 0; i < len; i++) {
                    int32_t elemTypeNode = AstListItemAt(&file->ast, typeNode, i);
                    int     elemIsConst = 0;
                    if (elemTypeNode < 0
                        || H2EvalZeroInitTypeNode(
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
            H2EvalValueSetArray(outValue, file, typeNode, array);
            *outIsConst = 1;
            return 0;
        }
        case H2Ast_TYPE_ANON_STRUCT:
        case H2Ast_TYPE_ANON_UNION:
            return H2EvalZeroInitAggregateValue(p, file, typeNode, outValue, outIsConst);
        case H2Ast_TYPE_FN:
            H2EvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        default: return 0;
    }
}

static int H2EvalResolveSimpleAliasCastTarget(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    char*                outTargetKind,
    uint64_t* _Nullable outAliasTag) {
    const H2Package* currentPkg;
    const H2AstNode* typeNameNode;
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
    if (typeNameNode->kind != H2Ast_TYPE_NAME) {
        return 0;
    }
    currentPkg = H2EvalFindPackageByFile(p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    for (fileIndex = 0; fileIndex < currentPkg->fileLen; fileIndex++) {
        const H2ParsedFile* pkgFile = &currentPkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const H2AstNode* aliasNode = &pkgFile->ast.nodes[nodeId];
            if (aliasNode->kind == H2Ast_TYPE_ALIAS
                && SliceEqSlice(
                    file->source,
                    typeNameNode->dataStart,
                    typeNameNode->dataEnd,
                    pkgFile->source,
                    aliasNode->dataStart,
                    aliasNode->dataEnd))
            {
                int32_t          targetNodeId = aliasNode->firstChild;
                const H2AstNode* targetNode;
                if (targetNodeId < 0 || (uint32_t)targetNodeId >= pkgFile->ast.len) {
                    return 0;
                }
                targetNode = &pkgFile->ast.nodes[targetNodeId];
                if (targetNode->kind != H2Ast_TYPE_NAME) {
                    return 0;
                }
                if (SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "bool"))
                {
                    *outTargetKind = 'b';
                    if (outAliasTag != NULL) {
                        *outAliasTag = H2EvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                if (SliceEqCStr(pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "f32")
                    || SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "f64"))
                {
                    *outTargetKind = 'f';
                    if (outAliasTag != NULL) {
                        *outAliasTag = H2EvalMakeAliasTag(pkgFile, nodeId);
                    }
                    return 1;
                }
                if (SliceEqCStr(
                        pkgFile->source, targetNode->dataStart, targetNode->dataEnd, "string"))
                {
                    *outTargetKind = 's';
                    if (outAliasTag != NULL) {
                        *outAliasTag = H2EvalMakeAliasTag(pkgFile, nodeId);
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
                        *outAliasTag = H2EvalMakeAliasTag(pkgFile, nodeId);
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

static int H2EvalResolveAliasCastTargetNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    const H2ParsedFile** outAliasFile,
    int32_t*             outAliasNode,
    int32_t*             outTargetNode) {
    const H2Package* currentPkg;
    const H2AstNode* typeNameNode;
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
    if (typeNameNode->kind != H2Ast_TYPE_NAME) {
        return 0;
    }
    currentPkg = H2EvalFindPackageByFile(p, file);
    if (currentPkg == NULL) {
        return 0;
    }
    for (fileIndex = 0; fileIndex < currentPkg->fileLen; fileIndex++) {
        const H2ParsedFile* pkgFile = &currentPkg->files[fileIndex];
        int32_t             nodeId = ASTFirstChild(&pkgFile->ast, pkgFile->ast.root);
        while (nodeId >= 0) {
            const H2AstNode* aliasNode = &pkgFile->ast.nodes[nodeId];
            if (aliasNode->kind == H2Ast_TYPE_ALIAS
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

static int H2EvalDecodeNewExprNodes(
    const H2ParsedFile* file,
    int32_t             nodeId,
    int32_t*            outTypeNode,
    int32_t*            outCountNode,
    int32_t*            outInitNode,
    int32_t*            outAllocNode) {
    const H2AstNode* n;
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
    if (n->kind != H2Ast_NEW) {
        return 0;
    }
    hasCount = (n->flags & H2AstFlag_NEW_HAS_COUNT) != 0;
    hasInit = (n->flags & H2AstFlag_NEW_HAS_INIT) != 0;
    hasAlloc = (n->flags & H2AstFlag_NEW_HAS_ALLOC) != 0;
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

static int H2EvalAllocReferencedValue(
    H2EvalProgram* p, const H2CTFEValue* inValue, H2CTFEValue* outValue, int* outIsConst) {
    H2CTFEValue* target;
    if (p == NULL || inValue == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    target = (H2CTFEValue*)H2ArenaAlloc(
        p->arena, sizeof(H2CTFEValue), (uint32_t)_Alignof(H2CTFEValue));
    if (target == NULL) {
        return ErrorSimple("out of memory");
    }
    *target = *inValue;
    H2EvalValueSetReference(outValue, target);
    *outIsConst = 1;
    return 0;
}

static int H2EvalCheckAllocatorImplResult(
    H2EvalProgram* p, int32_t exprNode, const H2CTFEValue* allocValue, int* outReturnedNull) {
    const H2CTFEValue* allocTarget;
    H2EvalAggregate*   allocAgg;
    H2CTFEValue        handlerValue;
    static const char  handlerName[] = "handler";
    if (outReturnedNull != NULL) {
        *outReturnedNull = 0;
    }
    if (p == NULL || allocValue == NULL) {
        return -1;
    }
    allocTarget = H2EvalValueTargetOrSelf(allocValue);
    allocAgg = H2EvalValueAsAggregate(allocTarget);
    if (allocAgg == NULL
        || !H2EvalAggregateGetFieldValue(allocAgg, handlerName, 0u, 7u, &handlerValue)
        || !H2EvalValueIsInvokableFunctionRef(&handlerValue))
    {
        return 1;
    }
    {
        H2CTFEValue args[7];
        H2CTFEValue newSize;
        H2CTFEValue result;
        int         resultIsConst = 0;
        int         invoked;
        args[0] = *allocValue;
        H2EvalValueSetNull(&args[1]);
        H2EvalValueSetInt(&args[2], 0);
        H2EvalValueSetInt(&args[3], 0);
        H2EvalValueSetInt(&newSize, 0);
        H2EvalValueSetReference(&args[4], &newSize);
        H2EvalValueSetInt(&args[5], 0);
        H2EvalValueSetNull(&args[6]);
        invoked = H2EvalInvokeFunctionRef(p, &handlerValue, args, 7u, &result, &resultIsConst);
        if (invoked < 0) {
            return -1;
        }
        if (invoked > 0 && resultIsConst
            && H2EvalValueTargetOrSelf(&result)->kind == H2CTFEValue_NULL)
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

static int H2EvalExpectedNewResultIsOptional(
    const H2ParsedFile* _Nullable typeFile, int32_t typeNode) {
    if (typeFile == NULL || typeNode < 0 || (uint32_t)typeNode >= typeFile->ast.len) {
        return 0;
    }
    return typeFile->ast.nodes[typeNode].kind == H2Ast_TYPE_OPTIONAL;
}

static int H2EvalEvalNewExpr(
    H2EvalProgram* p,
    int32_t        exprNode,
    const H2ParsedFile* _Nullable expectedTypeFile,
    int32_t      expectedTypeNode,
    H2CTFEValue* outValue,
    int*         outIsConst) {
    int32_t     typeNode = -1;
    int32_t     countNode = -1;
    int32_t     initNode = -1;
    int32_t     allocNode = -1;
    H2CTFEValue allocValue;
    int         allocIsConst = 0;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!H2EvalDecodeNewExprNodes(
            p->currentFile, exprNode, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    if (allocNode >= 0) {
        if (H2EvalExecExprCb(p, allocNode, &allocValue, &allocIsConst) != 0) {
            return -1;
        }
        if (!allocIsConst) {
            return 0;
        }
    } else if (!H2EvalCurrentContextFieldByLiteral(p, "allocator", &allocValue)) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReasonNode(
                p->currentExecCtx,
                exprNode,
                "allocator context is not available in evaluator backend");
        }
        return 0;
    } else {
        allocIsConst = 1;
    }
    if (H2EvalValueTargetOrSelf(&allocValue)->kind == H2CTFEValue_NULL) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReasonNode(p->currentExecCtx, exprNode, "invalid allocator");
        }
        return 0;
    }
    {
        int allocReturnedNull = 0;
        int allocRc = H2EvalCheckAllocatorImplResult(p, exprNode, &allocValue, &allocReturnedNull);
        if (allocRc <= 0) {
            if (allocRc == 0 && allocReturnedNull
                && H2EvalExpectedNewResultIsOptional(expectedTypeFile, expectedTypeNode))
            {
                H2EvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (allocRc == 0 && allocReturnedNull && p->currentExecCtx != NULL) {
                H2CTFEExecSetReasonNode(p->currentExecCtx, exprNode, "allocator returned null");
            }
            return allocRc;
        }
    }
    if (countNode >= 0) {
        H2CTFEValue  countValue;
        int          countIsConst = 0;
        int64_t      count = 0;
        H2EvalArray* array;
        H2CTFEValue  arrayValue;
        uint32_t     i;
        if (H2EvalExecExprCb(p, countNode, &countValue, &countIsConst) != 0) {
            return -1;
        }
        if (!countIsConst || H2CTFEValueToInt64(&countValue, &count) != 0 || count < 0) {
            return 0;
        }
        array = (H2EvalArray*)H2ArenaAlloc(
            p->arena, sizeof(H2EvalArray), (uint32_t)_Alignof(H2EvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = p->currentFile;
        array->typeNode = exprNode;
        array->elemTypeNode = typeNode;
        array->len = (uint32_t)count;
        if (array->len > 0) {
            array->elems = (H2CTFEValue*)H2ArenaAlloc(
                p->arena, sizeof(H2CTFEValue) * array->len, (uint32_t)_Alignof(H2CTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(H2CTFEValue) * array->len);
            if (initNode >= 0) {
                H2CTFEValue initValue;
                int         initIsConst = 0;
                if (H2EvalExecExprWithTypeNode(
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
                    if (H2EvalZeroInitTypeNode(
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
        H2EvalValueSetArray(&arrayValue, p->currentFile, exprNode, array);
        return H2EvalAllocReferencedValue(p, &arrayValue, outValue, outIsConst);
    }
    {
        H2CTFEValue value;
        int         valueIsConst = 0;
        if (initNode >= 0) {
            if (H2EvalExecExprWithTypeNode(
                    p, initNode, p->currentFile, typeNode, &value, &valueIsConst)
                != 0)
            {
                return -1;
            }
        } else if (H2EvalZeroInitTypeNode(p, p->currentFile, typeNode, &value, &valueIsConst) != 0)
        {
            return -1;
        }
        if (!valueIsConst) {
            return 0;
        }
        {
            H2EvalAggregate*   agg = H2EvalValueAsAggregate(&value);
            H2CTFEExecBinding* fieldBindings = NULL;
            H2CTFEExecEnv      fieldFrame;
            uint32_t           fieldBindingCap = 0;
            uint32_t           i;
            if (agg != NULL && agg->fieldLen > 0) {
                fieldBindingCap = H2EvalAggregateFieldBindingCount(agg);
                fieldBindings = (H2CTFEExecBinding*)H2ArenaAlloc(
                    p->arena,
                    sizeof(H2CTFEExecBinding) * fieldBindingCap,
                    (uint32_t)_Alignof(H2CTFEExecBinding));
                if (fieldBindings == NULL) {
                    return ErrorSimple("out of memory");
                }
                memset(fieldBindings, 0, sizeof(H2CTFEExecBinding) * fieldBindingCap);
            }
            fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
            fieldFrame.bindings = fieldBindings;
            fieldFrame.bindingLen = 0;
            if (agg != NULL) {
                for (i = 0; i < agg->fieldLen; i++) {
                    H2EvalAggregateField* field = &agg->fields[i];
                    if (initNode < 0 && field->defaultExprNode >= 0) {
                        H2CTFEValue defaultValue;
                        int         defaultIsConst = 0;
                        if (H2EvalExecExprInFileWithType(
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
                        && H2EvalAppendAggregateFieldBindings(
                               fieldBindings, fieldBindingCap, &fieldFrame, field)
                               != 0)
                    {
                        return ErrorSimple("out of memory");
                    }
                }
            }
        }
        return H2EvalAllocReferencedValue(p, &value, outValue, outIsConst);
    }
}

static int H2EvalValueConcatStrings(
    H2Arena* arena, const H2CTFEValue* a, const H2CTFEValue* b, H2CTFEValue* outValue) {
    uint64_t totalLen64;
    uint32_t totalLen;
    uint8_t* bytes;
    if (arena == NULL || a == NULL || b == NULL || outValue == NULL) {
        return -1;
    }
    if (a->kind != H2CTFEValue_STRING || b->kind != H2CTFEValue_STRING) {
        return 0;
    }
    totalLen64 = (uint64_t)a->s.len + (uint64_t)b->s.len;
    if (totalLen64 > UINT32_MAX) {
        return 0;
    }
    totalLen = (uint32_t)totalLen64;
    outValue->kind = H2CTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    if (totalLen == 0) {
        outValue->s.bytes = NULL;
        outValue->s.len = 0;
        return 1;
    }
    bytes = (uint8_t*)H2ArenaAlloc(arena, totalLen, (uint32_t)_Alignof(uint8_t));
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

static void H2EvalValueSetUInt(H2CTFEValue* value, uint32_t n) {
    H2EvalValueSetInt(value, (int64_t)n);
    H2EvalValueSetRuntimeTypeCode(value, H2EvalTypeCode_UINT);
}

static int H2EvalValueCopyBuiltin(
    H2Arena* arena, const H2CTFEValue* dstArg, const H2CTFEValue* srcArg, H2CTFEValue* outValue) {
    const H2CTFEValue* srcValue;
    H2CTFEValue*       dstValue;
    H2EvalArray*       dstArray;
    H2EvalArray*       srcArray;
    uint32_t           copyLen;
    if (arena == NULL || dstArg == NULL || srcArg == NULL || outValue == NULL) {
        return -1;
    }
    srcValue = H2EvalValueTargetOrSelf(srcArg);
    dstValue = H2EvalValueReferenceTarget(dstArg);
    if (dstValue == NULL) {
        dstValue = (H2CTFEValue*)H2EvalValueTargetOrSelf(dstArg);
    }
    if (dstValue == NULL || srcValue == NULL) {
        return 0;
    }
    dstArray = H2EvalValueAsArray(dstValue);
    srcArray = H2EvalValueAsArray(srcValue);
    if (dstArray != NULL && srcArray != NULL) {
        copyLen = dstArray->len < srcArray->len ? dstArray->len : srcArray->len;
        if (copyLen > 0) {
            H2CTFEValue* temp = (H2CTFEValue*)H2ArenaAlloc(
                arena, sizeof(H2CTFEValue) * copyLen, (uint32_t)_Alignof(H2CTFEValue));
            if (temp == NULL) {
                return -1;
            }
            memcpy(temp, srcArray->elems, sizeof(H2CTFEValue) * copyLen);
            memcpy(dstArray->elems, temp, sizeof(H2CTFEValue) * copyLen);
        }
        H2EvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstValue->kind == H2CTFEValue_STRING && srcValue->kind == H2CTFEValue_STRING) {
        int32_t dstTypeCode = H2EvalTypeCode_INVALID;
        if (!H2EvalValueGetRuntimeTypeCode(dstValue, &dstTypeCode)
            || dstTypeCode != H2EvalTypeCode_STR_PTR)
        {
            return 0;
        }
        copyLen = dstValue->s.len < srcValue->s.len ? dstValue->s.len : srcValue->s.len;
        if (copyLen > 0 && dstValue->s.bytes != NULL && srcValue->s.bytes != NULL) {
            memmove((uint8_t*)dstValue->s.bytes, srcValue->s.bytes, copyLen);
        }
        H2EvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstArray != NULL && srcValue->kind == H2CTFEValue_STRING) {
        copyLen = dstArray->len < srcValue->s.len ? dstArray->len : srcValue->s.len;
        for (uint32_t i = 0; i < copyLen; i++) {
            H2EvalValueSetInt(&dstArray->elems[i], (int64_t)srcValue->s.bytes[i]);
            H2EvalValueSetRuntimeTypeCode(&dstArray->elems[i], H2EvalTypeCode_U8);
        }
        H2EvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    if (dstValue->kind == H2CTFEValue_STRING && srcArray != NULL) {
        int32_t  dstTypeCode = H2EvalTypeCode_INVALID;
        uint8_t* tempBytes;
        if (!H2EvalValueGetRuntimeTypeCode(dstValue, &dstTypeCode)
            || dstTypeCode != H2EvalTypeCode_STR_PTR)
        {
            return 0;
        }
        copyLen = dstValue->s.len < srcArray->len ? dstValue->s.len : srcArray->len;
        tempBytes =
            copyLen > 0 ? (uint8_t*)H2ArenaAlloc(arena, copyLen, (uint32_t)_Alignof(uint8_t))
                        : NULL;
        if (copyLen > 0 && tempBytes == NULL) {
            return -1;
        }
        for (uint32_t i = 0; i < copyLen; i++) {
            int64_t byteValue = 0;
            if (H2CTFEValueToInt64(&srcArray->elems[i], &byteValue) != 0 || byteValue < 0
                || byteValue > 255)
            {
                return 0;
            }
            tempBytes[i] = (uint8_t)byteValue;
        }
        if (copyLen > 0 && dstValue->s.bytes != NULL) {
            memmove((uint8_t*)dstValue->s.bytes, tempBytes, copyLen);
        }
        H2EvalValueSetUInt(outValue, copyLen);
        return 1;
    }
    return 0;
}

static int H2EvalStringValueFromArrayBytes(
    H2Arena* arena, const H2CTFEValue* inValue, int32_t targetTypeCode, H2CTFEValue* outValue) {
    H2EvalArray* array;
    uint8_t*     bytes = NULL;
    uint32_t     i;
    if (arena == NULL || inValue == NULL || outValue == NULL) {
        return -1;
    }
    array = H2EvalValueAsArray(H2EvalValueTargetOrSelf(inValue));
    if (array == NULL) {
        return 0;
    }
    if (array->len > 0) {
        bytes = (uint8_t*)H2ArenaAlloc(arena, array->len, (uint32_t)_Alignof(uint8_t));
        if (bytes == NULL) {
            return -1;
        }
        for (i = 0; i < array->len; i++) {
            int64_t byteValue = 0;
            if (H2CTFEValueToInt64(&array->elems[i], &byteValue) != 0 || byteValue < 0
                || byteValue > 255)
            {
                return 0;
            }
            bytes[i] = (uint8_t)byteValue;
        }
    }
    outValue->kind = H2CTFEValue_STRING;
    outValue->i64 = 0;
    outValue->f64 = 0.0;
    outValue->b = 0;
    outValue->typeTag = 0;
    outValue->s.bytes = bytes;
    outValue->s.len = array->len;
    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
    return 1;
}

static void H2EvalValueSetSpan(
    const H2ParsedFile* file, uint32_t start, uint32_t end, H2CTFEValue* value) {
    uint32_t startLine = 0;
    uint32_t startCol = 0;
    uint32_t endLine = 0;
    uint32_t endCol = 0;
    if (file == NULL || value == NULL) {
        return;
    }
    DiagOffsetToLineCol(file->source, start, &startLine, &startCol);
    DiagOffsetToLineCol(file->source, end, &endLine, &endCol);
    value->kind = H2CTFEValue_SPAN;
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
    return ErrorDiagf(file, source, start, end, H2Diag_EVAL_BACKEND_UNSUPPORTED, detail);
}

static int H2EvalProgramAppendFunction(H2EvalProgram* p, const H2EvalFunction* fn) {
    H2EvalFunction* newFuncs;
    uint32_t        newCap;
    if (p == NULL || fn == NULL) {
        return -1;
    }
    if (p->funcLen >= p->funcCap) {
        newCap = p->funcCap < 8u ? 8u : p->funcCap * 2u;
        newFuncs = (H2EvalFunction*)realloc(p->funcs, sizeof(H2EvalFunction) * newCap);
        if (newFuncs == NULL) {
            return ErrorSimple("out of memory");
        }
        p->funcs = newFuncs;
        p->funcCap = newCap;
    }
    p->funcs[p->funcLen++] = *fn;
    return 0;
}

static int H2EvalProgramAppendTopConst(H2EvalProgram* p, const H2EvalTopConst* topConst) {
    H2EvalTopConst* newConsts;
    uint32_t        newCap;
    if (p == NULL || topConst == NULL) {
        return -1;
    }
    if (p->topConstLen >= p->topConstCap) {
        newCap = p->topConstCap < 8u ? 8u : p->topConstCap * 2u;
        newConsts = (H2EvalTopConst*)realloc(p->topConsts, sizeof(H2EvalTopConst) * newCap);
        if (newConsts == NULL) {
            return ErrorSimple("out of memory");
        }
        p->topConsts = newConsts;
        p->topConstCap = newCap;
    }
    p->topConsts[p->topConstLen++] = *topConst;
    return 0;
}

static int H2EvalProgramAppendTopVar(H2EvalProgram* p, const H2EvalTopVar* topVar) {
    H2EvalTopVar* newVars;
    uint32_t      newCap;
    if (p == NULL || topVar == NULL) {
        return -1;
    }
    if (p->topVarLen >= p->topVarCap) {
        newCap = p->topVarCap < 8u ? 8u : p->topVarCap * 2u;
        newVars = (H2EvalTopVar*)realloc(p->topVars, sizeof(H2EvalTopVar) * newCap);
        if (newVars == NULL) {
            return ErrorSimple("out of memory");
        }
        p->topVars = newVars;
        p->topVarCap = newCap;
    }
    p->topVars[p->topVarLen++] = *topVar;
    return 0;
}

static int32_t H2EvalVarLikeInitExprNodeAt(
    const H2ParsedFile* file, int32_t varLikeNodeId, int32_t nameIndex) {
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
    if (file->ast.nodes[firstChild].kind != H2Ast_NAME_LIST) {
        return nameIndex == 0 ? initNode : -1;
    }
    if (file->ast.nodes[initNode].kind != H2Ast_EXPR_LIST) {
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
                || file->ast.nodes[onlyInit].kind != H2Ast_TUPLE_EXPR)
            {
                return -1;
            }
            return AstListItemAt(&file->ast, onlyInit, (uint32_t)nameIndex);
        }
    }
}

static int32_t H2EvalVarLikeDeclTypeNode(const H2ParsedFile* file, int32_t varLikeNodeId) {
    int32_t firstChild;
    int32_t afterNames;
    if (file == NULL || varLikeNodeId < 0 || (uint32_t)varLikeNodeId >= file->ast.len) {
        return -1;
    }
    firstChild = ASTFirstChild(&file->ast, varLikeNodeId);
    if (firstChild < 0 || (uint32_t)firstChild >= file->ast.len) {
        return -1;
    }
    if (file->ast.nodes[firstChild].kind == H2Ast_NAME_LIST) {
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

static int H2EvalCollectTopConsts(H2EvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const H2Package* pkg = &p->loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const H2ParsedFile* file = &pkg->files[fileIndex];
            const H2Ast*        ast = &file->ast;
            int32_t             nodeId = ASTFirstChild(ast, ast->root);
            while (nodeId >= 0) {
                const H2AstNode* n = &ast->nodes[nodeId];
                if (n->kind == H2Ast_CONST) {
                    int32_t firstChild = ASTFirstChild(ast, nodeId);
                    if (firstChild >= 0 && ast->nodes[firstChild].kind == H2Ast_NAME_LIST) {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(ast, firstChild);
                        for (i = 0; i < nameCount; i++) {
                            int32_t          nameNode = AstListItemAt(ast, firstChild, i);
                            const H2AstNode* name = nameNode >= 0 ? &ast->nodes[nameNode] : NULL;
                            H2EvalTopConst   topConst;
                            if (name == NULL) {
                                continue;
                            }
                            memset(&topConst, 0, sizeof(topConst));
                            topConst.file = file;
                            topConst.nodeId = nodeId;
                            topConst.initExprNode = H2EvalVarLikeInitExprNodeAt(
                                file, nodeId, (int32_t)i);
                            topConst.nameStart = name->dataStart;
                            topConst.nameEnd = name->dataEnd;
                            topConst.state = H2EvalTopConstState_UNSEEN;
                            if (H2EvalProgramAppendTopConst(p, &topConst) != 0) {
                                return -1;
                            }
                        }
                    } else {
                        H2EvalTopConst topConst;
                        memset(&topConst, 0, sizeof(topConst));
                        topConst.file = file;
                        topConst.nodeId = nodeId;
                        topConst.initExprNode = H2EvalVarLikeInitExprNodeAt(file, nodeId, 0);
                        topConst.nameStart = n->dataStart;
                        topConst.nameEnd = n->dataEnd;
                        topConst.state = H2EvalTopConstState_UNSEEN;
                        if (H2EvalProgramAppendTopConst(p, &topConst) != 0) {
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

static int H2EvalCollectTopVars(H2EvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const H2Package* pkg = &p->loader->packages[pkgIndex];
        uint32_t         fileIndex;
        for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
            const H2ParsedFile* file = &pkg->files[fileIndex];
            const H2Ast*        ast = &file->ast;
            int32_t             nodeId = ASTFirstChild(ast, ast->root);
            while (nodeId >= 0) {
                const H2AstNode* n = &ast->nodes[nodeId];
                if (n->kind == H2Ast_VAR) {
                    if (ASTFirstChild(ast, nodeId) >= 0
                        && ast->nodes[ASTFirstChild(ast, nodeId)].kind == H2Ast_NAME_LIST)
                    {
                        uint32_t i;
                        uint32_t nameCount = AstListCount(ast, ASTFirstChild(ast, nodeId));
                        for (i = 0; i < nameCount; i++) {
                            int32_t nameNode = AstListItemAt(ast, ASTFirstChild(ast, nodeId), i);
                            const H2AstNode* name = nameNode >= 0 ? &ast->nodes[nameNode] : NULL;
                            H2EvalTopVar     topVar;
                            if (name == NULL) {
                                continue;
                            }
                            memset(&topVar, 0, sizeof(topVar));
                            topVar.file = file;
                            topVar.nodeId = nodeId;
                            topVar.initExprNode = H2EvalVarLikeInitExprNodeAt(
                                file, nodeId, (int32_t)i);
                            topVar.declTypeNode = H2EvalVarLikeDeclTypeNode(file, nodeId);
                            topVar.nameStart = name->dataStart;
                            topVar.nameEnd = name->dataEnd;
                            topVar.state = H2EvalTopConstState_UNSEEN;
                            if (H2EvalProgramAppendTopVar(p, &topVar) != 0) {
                                return -1;
                            }
                        }
                    } else {
                        H2EvalTopVar topVar;
                        memset(&topVar, 0, sizeof(topVar));
                        topVar.file = file;
                        topVar.nodeId = nodeId;
                        topVar.initExprNode = H2EvalVarLikeInitExprNodeAt(file, nodeId, 0);
                        topVar.declTypeNode = H2EvalVarLikeDeclTypeNode(file, nodeId);
                        topVar.nameStart = n->dataStart;
                        topVar.nameEnd = n->dataEnd;
                        topVar.state = H2EvalTopConstState_UNSEEN;
                        if (H2EvalProgramAppendTopVar(p, &topVar) != 0) {
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

static int32_t H2EvalFindTopConstBySlice(
    const H2EvalProgram* p, const H2ParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->topConstLen; i++) {
        const H2EvalTopConst* topConst = &p->topConsts[i];
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

static int32_t H2EvalFindTopConstBySliceInPackage(
    const H2EvalProgram* p,
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
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
        const H2EvalTopConst* topConst = &p->topConsts[i];
        const H2Package*      topConstPkg = H2EvalFindPackageByFile(p, topConst->file);
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

static int32_t H2EvalFindTopVarBySliceInPackage(
    const H2EvalProgram* p,
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
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
        const H2EvalTopVar* topVar = &p->topVars[i];
        const H2Package*    topVarPkg = H2EvalFindPackageByFile(p, topVar->file);
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

static int32_t H2EvalFindTopVarBySlice(
    const H2EvalProgram* p, const H2ParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->topVarLen; i++) {
        const H2EvalTopVar* topVar = &p->topVars[i];
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

static int32_t H2EvalFindCurrentTopVarBySlice(
    const H2EvalProgram* p, const H2ParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    const H2Package* currentPkg;
    if (p == NULL || callerFile == NULL) {
        return -1;
    }
    currentPkg = H2EvalFindPackageByFile(p, callerFile);
    return currentPkg != NULL
             ? H2EvalFindTopVarBySliceInPackage(p, currentPkg, callerFile, nameStart, nameEnd)
             : H2EvalFindTopVarBySlice(p, callerFile, nameStart, nameEnd);
}

static int H2EvalCollectFunctionsFromPackage(
    H2EvalProgram* p, const H2Package* pkg, uint8_t isBuiltinPackageFn) {
    uint32_t fileIndex;
    if (p == NULL || pkg == NULL) {
        return -1;
    }
    for (fileIndex = 0; fileIndex < pkg->fileLen; fileIndex++) {
        const H2ParsedFile* file = &pkg->files[fileIndex];
        const H2Ast*        ast = &file->ast;
        int32_t             nodeId = ASTFirstChild(ast, ast->root);
        while (nodeId >= 0) {
            const H2AstNode* n = &ast->nodes[nodeId];
            if (n->kind == H2Ast_FN) {
                H2EvalFunction fn;
                int32_t        child = ASTFirstChild(ast, nodeId);
                int32_t        bodyNode = -1;
                uint32_t       paramCount = 0;
                uint8_t        hasReturnType = 0;
                uint8_t        hasContextClause = 0;
                uint8_t        isVariadic = 0;

                while (child >= 0) {
                    const H2AstNode* ch = &ast->nodes[child];
                    if (ch->kind == H2Ast_PARAM) {
                        paramCount++;
                        if ((ch->flags & H2AstFlag_PARAM_VARIADIC) != 0) {
                            isVariadic = 1;
                        }
                    } else if (ch->kind == H2Ast_CONTEXT_CLAUSE) {
                        hasContextClause = 1;
                    } else if (IsFnReturnTypeNodeKind(ch->kind) && ch->flags == 1) {
                        hasReturnType = 1;
                    } else if (ch->kind == H2Ast_BLOCK) {
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
                    if (H2EvalProgramAppendFunction(p, &fn) != 0) {
                        return -1;
                    }
                }
            }
            nodeId = ASTNextSibling(ast, nodeId);
        }
    }
    return 0;
}

static int H2EvalCollectFunctions(H2EvalProgram* p) {
    uint32_t pkgIndex;
    if (p == NULL || p->loader == NULL) {
        return -1;
    }
    for (pkgIndex = 0; pkgIndex < p->loader->packageLen; pkgIndex++) {
        const H2Package* pkg = &p->loader->packages[pkgIndex];
        uint8_t          isBuiltinPackageFn = StrEq(pkg->name, "builtin") ? 1u : 0u;
        if (H2EvalCollectFunctionsFromPackage(p, pkg, isBuiltinPackageFn) != 0) {
            return -1;
        }
    }
    return 0;
}

static int32_t H2EvalFindFunctionBySlice(
    const H2EvalProgram* p,
    const H2ParsedFile*  callerFile,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    uint32_t             argCount) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const H2EvalFunction* fn = &p->funcs[i];
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

static int32_t H2EvalFindFunctionBySliceInPackage(
    const H2EvalProgram* p,
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
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
        const H2EvalFunction* fn = &p->funcs[i];
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

static int32_t H2EvalFindAnyFunctionBySliceInPackage(
    const H2EvalProgram* p,
    const H2Package*     pkg,
    const H2ParsedFile*  callerFile,
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
        const H2EvalFunction* fn = &p->funcs[i];
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

static int H2EvalValueSimpleKind(
    const H2CTFEValue* value, char* outKind, uint64_t* _Nullable outAliasTag) {
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
        case H2CTFEValue_INT:    *outKind = 'i'; break;
        case H2CTFEValue_FLOAT:  *outKind = 'f'; break;
        case H2CTFEValue_BOOL:   *outKind = 'b'; break;
        case H2CTFEValue_STRING: *outKind = 's'; break;
        default:                 return 0;
    }
    if (outAliasTag != NULL) {
        *outAliasTag = value->typeTag;
    }
    return 1;
}

static int H2EvalClassifySimpleTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    char*                outKind,
    uint64_t* _Nullable outAliasTag);
static int H2EvalAggregateDistanceToType(
    const H2EvalProgram* p,
    const H2CTFEValue*   value,
    const H2ParsedFile*  callerFile,
    int32_t              typeNode,
    uint32_t*            outDistance);

static int H2EvalTypeNodeIsAnytype(const H2ParsedFile* file, int32_t typeNode) {
    const H2AstNode* n;
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    return n->kind == H2Ast_TYPE_NAME
        && SliceEqCStr(file->source, n->dataStart, n->dataEnd, "anytype");
}

static int H2EvalTypeNodeIsTemplateParamName(const H2ParsedFile* file, int32_t typeNode) {
    const H2AstNode* n;
    int32_t          declNode;
    if (file == NULL || typeNode < 0 || (uint32_t)typeNode >= file->ast.len) {
        return 0;
    }
    n = &file->ast.nodes[typeNode];
    if (n->kind != H2Ast_TYPE_NAME || n->firstChild >= 0) {
        return 0;
    }
    declNode = ASTFirstChild(&file->ast, file->ast.root);
    while (declNode >= 0) {
        const H2AstNode* decl = &file->ast.nodes[declNode];
        if ((decl->kind == H2Ast_FN || decl->kind == H2Ast_STRUCT) && decl->start <= n->start
            && decl->end >= n->end)
        {
            int32_t child = ASTFirstChild(&file->ast, declNode);
            while (child >= 0 && file->ast.nodes[child].kind == H2Ast_TYPE_PARAM) {
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

static int H2EvalValueMatchesExpectedTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  typeFile,
    int32_t              typeNode,
    const H2CTFEValue*   value) {
    char               argKind = '\0';
    char               paramKind = '\0';
    uint64_t           argAliasTag = 0;
    uint64_t           paramAliasTag = 0;
    uint32_t           structDistance = 0;
    int32_t            typeCode = H2EvalTypeCode_INVALID;
    const H2CTFEValue* sourceValue = H2EvalValueTargetOrSelf(value);
    if (p == NULL || typeFile == NULL || value == NULL || typeNode < 0
        || (uint32_t)typeNode >= typeFile->ast.len)
    {
        return 0;
    }
    if (H2EvalTypeNodeIsAnytype(typeFile, typeNode)) {
        return 1;
    }
    if (H2EvalTypeNodeIsTemplateParamName(typeFile, typeNode)) {
        return 1;
    }
    if (H2EvalAggregateDistanceToType(p, value, typeFile, typeNode, &structDistance)) {
        return 1;
    }
    if (H2EvalValueSimpleKind(sourceValue, &argKind, &argAliasTag)
        && H2EvalClassifySimpleTypeNode(p, typeFile, typeNode, &paramKind, &paramAliasTag)
        && argKind == paramKind)
    {
        return paramAliasTag == 0 || argAliasTag == paramAliasTag;
    }
    if (!H2EvalTypeCodeFromTypeNode(typeFile, typeNode, &typeCode)) {
        return 0;
    }
    switch (typeCode) {
        case H2EvalTypeCode_BOOL:    return sourceValue->kind == H2CTFEValue_BOOL;
        case H2EvalTypeCode_F32:
        case H2EvalTypeCode_F64:     return sourceValue->kind == H2CTFEValue_FLOAT;
        case H2EvalTypeCode_U8:
        case H2EvalTypeCode_U16:
        case H2EvalTypeCode_U32:
        case H2EvalTypeCode_U64:
        case H2EvalTypeCode_UINT:
        case H2EvalTypeCode_I8:
        case H2EvalTypeCode_I16:
        case H2EvalTypeCode_I32:
        case H2EvalTypeCode_I64:
        case H2EvalTypeCode_INT:     return sourceValue->kind == H2CTFEValue_INT;
        case H2EvalTypeCode_STR_REF:
        case H2EvalTypeCode_STR_PTR: return sourceValue->kind == H2CTFEValue_STRING;
        case H2EvalTypeCode_RAWPTR:
            return sourceValue->kind == H2CTFEValue_REFERENCE
                || sourceValue->kind == H2CTFEValue_NULL || sourceValue->kind == H2CTFEValue_STRING;
        case H2EvalTypeCode_TYPE: return sourceValue->kind == H2CTFEValue_TYPE;
        default:                  return 0;
    }
}

static int32_t H2EvalFunctionParamTypeNodeAt(const H2EvalFunction* fn, uint32_t paramIndex) {
    const H2Ast* ast;
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
        const H2AstNode* n = &ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
            if (i == paramIndex) {
                return ASTFirstChild(ast, child);
            }
            i++;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static int32_t H2EvalFunctionParamIndexByName(
    const H2EvalFunction* fn, const char* source, uint32_t nameStart, uint32_t nameEnd) {
    const H2Ast* ast;
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
        const H2AstNode* n = &ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
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

static int H2EvalExprIsAnytypePackIndex(H2EvalProgram* p, const H2Ast* ast, int32_t exprNode) {
    int32_t             baseNode;
    int32_t             idxNode;
    int32_t             extraNode;
    H2CTFEExecBinding*  binding;
    const H2CTFEValue*  bindingValue;
    const H2ParsedFile* localTypeFile = NULL;
    int32_t             localTypeNode = -1;
    H2CTFEValue         localValue;
    while (ast != NULL && exprNode >= 0 && (uint32_t)exprNode < ast->len
           && ast->nodes[exprNode].kind == H2Ast_CALL_ARG)
    {
        exprNode = ast->nodes[exprNode].firstChild;
    }
    if (p == NULL || p->currentFile == NULL || p->currentExecCtx == NULL || ast == NULL
        || exprNode < 0 || (uint32_t)exprNode >= ast->len
        || ast->nodes[exprNode].kind != H2Ast_INDEX
        || (ast->nodes[exprNode].flags & H2AstFlag_INDEX_SLICE) != 0u)
    {
        return 0;
    }
    baseNode = ast->nodes[exprNode].firstChild;
    idxNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
    extraNode = idxNode >= 0 ? ast->nodes[idxNode].nextSibling : -1;
    if (baseNode < 0 || idxNode < 0 || extraNode >= 0 || ast->nodes[baseNode].kind != H2Ast_IDENT) {
        return 0;
    }
    binding = H2EvalFindBinding(
        p->currentExecCtx,
        p->currentFile,
        ast->nodes[baseNode].dataStart,
        ast->nodes[baseNode].dataEnd);
    if (binding != NULL && H2EvalTypeNodeIsAnytype(p->currentFile, binding->typeNode)) {
        bindingValue = H2EvalValueTargetOrSelf(&binding->value);
        return bindingValue->kind == H2CTFEValue_ARRAY;
    }
    if (!H2EvalMirLookupLocalTypeNode(
            p,
            ast->nodes[baseNode].dataStart,
            ast->nodes[baseNode].dataEnd,
            &localTypeFile,
            &localTypeNode)
        || localTypeFile == NULL || localTypeNode < 0
        || !H2EvalTypeNodeIsAnytype(localTypeFile, localTypeNode)
        || !H2EvalMirLookupLocalValue(
            p, ast->nodes[baseNode].dataStart, ast->nodes[baseNode].dataEnd, &localValue))
    {
        return 0;
    }
    bindingValue = H2EvalValueTargetOrSelf(&localValue);
    return bindingValue->kind == H2CTFEValue_ARRAY;
}

static int H2EvalParamNameStartsWithUnderscore(
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

static uint32_t H2EvalPositionalCallPrefixEnd(
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
        && H2EvalParamNameStartsWithUnderscore(source, paramNameStarts, paramNameEnds, prefixEnd))
    {
        prefixEnd++;
    }
    return prefixEnd;
}

static int H2EvalReorderFixedCallArgsByName(
    H2EvalProgram*        p,
    const H2EvalFunction* fn,
    const H2Ast*          callAst,
    int32_t               firstArgNode,
    H2CTFEValue*          args,
    uint32_t              argCount,
    uint32_t              paramOffset) {
    uint32_t     argNameStarts[256];
    uint32_t     argNameEnds[256];
    uint32_t     paramNameStarts[256];
    uint32_t     paramNameEnds[256];
    uint8_t      argExplicitName[256];
    uint8_t      paramAssigned[256];
    H2CTFEValue  reorderedArgs[256];
    const H2Ast* fnAst;
    const char*  callSource;
    int32_t      child;
    int32_t      argNode;
    uint32_t     i = 0;
    uint32_t     positionalPrefixEnd;
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
    memset(argExplicitName, 0, sizeof(argExplicitName));
    argNode = firstArgNode;
    while (argNode >= 0) {
        const H2AstNode* arg = &callAst->nodes[argNode];
        int32_t          exprNode = argNode;
        if (i >= argCount) {
            return 0;
        }
        if (arg->kind == H2Ast_CALL_ARG) {
            if ((arg->flags & H2AstFlag_CALL_ARG_SPREAD) != 0) {
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
        if (!argExplicitName[i] && callAst->nodes[exprNode].kind == H2Ast_IDENT) {
            H2CTFEValue ignoredTypeValue;
            if (H2EvalResolveTypeValueName(
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
        const H2AstNode* n = &fnAst->nodes[child];
        if (n->kind == H2Ast_PARAM) {
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

    positionalPrefixEnd = H2EvalPositionalCallPrefixEnd(
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
    memcpy(args, reorderedArgs, sizeof(H2CTFEValue) * argCount);
    return 1;
}

static int32_t H2EvalFunctionReturnTypeNode(const H2EvalFunction* fn) {
    const H2Ast* ast;
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
        const H2AstNode* n = &ast->nodes[child];
        if (IsFnReturnTypeNodeKind(n->kind)) {
            return child;
        }
        if (n->kind == H2Ast_BLOCK) {
            break;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static void H2EvalSaveTemplateBinding(
    const H2EvalProgram* p, H2EvalTemplateBindingState* outState) {
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

static void H2EvalRestoreTemplateBinding(
    H2EvalProgram* p, const H2EvalTemplateBindingState* state) {
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

static int32_t H2EvalFunctionFirstTypeParamNode(const H2EvalFunction* fn) {
    const H2Ast* ast;
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
        if (ast->nodes[child].kind == H2Ast_TYPE_PARAM) {
            return child;
        }
        child = ASTNextSibling(ast, child);
    }
    return -1;
}

static int H2EvalBindActiveTemplateTypeValue(
    H2EvalProgram*        p,
    const H2EvalFunction* fn,
    int32_t               typeParamNode,
    const H2CTFEValue*    typeValue,
    const H2ParsedFile* _Nullable typeFile,
    int32_t typeNode) {
    const H2AstNode* param;
    if (p == NULL || fn == NULL || fn->file == NULL || typeValue == NULL
        || typeValue->kind != H2CTFEValue_TYPE || typeParamNode < 0
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

static int H2EvalBindActiveTemplateForMirCall(
    H2EvalProgram* p,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    const H2EvalFunction* fn,
    const H2CTFEValue* _Nullable args,
    uint32_t argCount) {
    int32_t  returnTypeNode;
    uint32_t typeArgIndex;
    if (p == NULL || fn == NULL) {
        return 0;
    }
    for (typeArgIndex = 0; typeArgIndex < argCount; typeArgIndex++) {
        if (args != NULL && args[typeArgIndex].kind == H2CTFEValue_TYPE) {
            int32_t typeParamNode = H2EvalFunctionFirstTypeParamNode(fn);
            return H2EvalBindActiveTemplateTypeValue(
                p, fn, typeParamNode, &args[typeArgIndex], NULL, -1);
        }
    }
    returnTypeNode = H2EvalFunctionReturnTypeNode(fn);
    if (program != NULL && function != NULL && inst != NULL && returnTypeNode >= 0
        && H2EvalTypeNodeIsTemplateParamName(fn->file, returnTypeNode)
        && p->currentMirExecCtx != NULL)
    {
        uint32_t instIndex = UINT32_MAX;
        if (program->insts != NULL && inst >= program->insts
            && inst < program->insts + program->instLen)
        {
            instIndex = (uint32_t)(inst - program->insts);
        }
        if (instIndex != UINT32_MAX && instIndex + 1u < program->instLen
            && program->insts[instIndex + 1u].op == H2MirOp_LOCAL_STORE)
        {
            uint32_t localSlot = program->insts[instIndex + 1u].aux;
            if (localSlot < function->localCount
                && function->localStart + localSlot < program->localLen)
            {
                const H2MirLocal* local = &program->locals[function->localStart + localSlot];
                if (local->typeRef < program->typeLen
                    && program->types[local->typeRef].sourceRef
                           < p->currentMirExecCtx->sourceFileCap)
                {
                    const H2ParsedFile* typeFile =
                        p->currentMirExecCtx->sourceFiles[program->types[local->typeRef].sourceRef];
                    H2CTFEValue typeValue;
                    memset(&typeValue, 0, sizeof(typeValue));
                    if (typeFile != NULL
                        && H2EvalTypeValueFromTypeNode(
                               p,
                               typeFile,
                               (int32_t)program->types[local->typeRef].astNode,
                               &typeValue)
                               > 0)
                    {
                        return H2EvalBindActiveTemplateTypeValue(
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
    if (returnTypeNode >= 0 && H2EvalTypeNodeIsTemplateParamName(fn->file, returnTypeNode)
        && p->activeCallExpectedTypeFile != NULL && p->activeCallExpectedTypeNode >= 0)
    {
        H2CTFEValue typeValue;
        memset(&typeValue, 0, sizeof(typeValue));
        if (H2EvalTypeValueFromTypeNode(
                p, p->activeCallExpectedTypeFile, p->activeCallExpectedTypeNode, &typeValue)
            > 0)
        {
            return H2EvalBindActiveTemplateTypeValue(
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

static int H2EvalFindExpectedTypeForCallExpr(
    const H2ParsedFile* _Nullable file,
    int32_t                        callNode,
    const H2ParsedFile* _Nullable* outTypeFile,
    int32_t*                       outTypeNode) {
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
        const H2AstNode* n = &file->ast.nodes[i];
        int32_t          typeNode;
        int32_t          initNode;
        if (n->kind != H2Ast_VAR && n->kind != H2Ast_CONST) {
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

static int H2EvalFindExpectedTypeForInitExpr(
    const H2ParsedFile* _Nullable file,
    int32_t                        initExprNode,
    const H2ParsedFile* _Nullable* outTypeFile,
    int32_t*                       outTypeNode) {
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
        const H2AstNode* n = &file->ast.nodes[i];
        int32_t          typeNode;
        int32_t          initNode;
        if (n->kind != H2Ast_VAR && n->kind != H2Ast_CONST) {
            continue;
        }
        typeNode = H2EvalVarLikeDeclTypeNode(file, (int32_t)i);
        if (typeNode < 0) {
            continue;
        }
        initNode = H2EvalVarLikeInitExprNodeAt(file, (int32_t)i, 0);
        if (initNode == initExprNode) {
            *outTypeFile = file;
            *outTypeNode = typeNode;
            return 1;
        }
    }
    return 0;
}

static int H2EvalClassifySimpleTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  file,
    int32_t              typeNode,
    char*                outKind,
    uint64_t* _Nullable outAliasTag) {
    const H2AstNode* n;
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
    if (n->kind != H2Ast_TYPE_NAME) {
        return 0;
    }
    if (H2EvalResolveSimpleAliasCastTarget(p, file, typeNode, &aliasKind, &aliasTag)) {
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

static int H2EvalStructEmbeddedBase(
    const H2EvalProgram* p,
    const H2ParsedFile*  structFile,
    int32_t              structNode,
    const H2ParsedFile** outBaseFile,
    int32_t*             outBaseNode) {
    int32_t          child;
    const H2Package* pkg;
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
    pkg = H2EvalFindPackageByFile(p, structFile);
    if (pkg == NULL) {
        return 0;
    }
    child = ASTFirstChild(&structFile->ast, structNode);
    while (child >= 0) {
        const H2AstNode* fieldNode = &structFile->ast.nodes[child];
        if (fieldNode->kind == H2Ast_FIELD && (fieldNode->flags & H2AstFlag_FIELD_EMBEDDED) != 0) {
            int32_t             typeNode = ASTFirstChild(&structFile->ast, child);
            const H2ParsedFile* baseFile = NULL;
            int32_t baseNode = H2EvalFindNamedAggregateDecl(p, structFile, typeNode, &baseFile);
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

static int H2EvalAggregateDistanceToType(
    const H2EvalProgram* p,
    const H2CTFEValue*   value,
    const H2ParsedFile*  callerFile,
    int32_t              typeNode,
    uint32_t*            outDistance) {
    const H2Package*    pkg;
    const H2ParsedFile* targetFile = NULL;
    int32_t             targetNode = -1;
    const H2ParsedFile* curFile = NULL;
    int32_t             curNode = -1;
    uint32_t            distance = 0;
    if (outDistance != NULL) {
        *outDistance = 0;
    }
    if (p == NULL || callerFile == NULL) {
        return 0;
    }
    pkg = H2EvalFindPackageByFile(p, callerFile);
    if (pkg == NULL) {
        return 0;
    }
    (void)pkg;
    if (!H2EvalResolveAggregateTypeNode(p, callerFile, typeNode, &targetFile, &targetNode)) {
        return 0;
    }
    if (!H2EvalResolveAggregateDeclFromValue(p, value, &curFile, &curNode)) {
        return 0;
    }
    while (curFile != NULL && curNode >= 0) {
        if (curFile == targetFile && curNode == targetNode) {
            if (outDistance != NULL) {
                *outDistance = distance;
            }
            return 1;
        }
        if (!H2EvalStructEmbeddedBase(p, curFile, curNode, &curFile, &curNode)) {
            break;
        }
        distance++;
    }
    return 0;
}

static int H2EvalScoreFunctionCandidate(
    const H2EvalProgram*  p,
    const H2EvalFunction* fn,
    const H2CTFEValue*    args,
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
        int32_t  paramTypeNode = H2EvalFunctionParamTypeNodeAt(
            fn, fn->isVariadic && i >= fixedCount ? fixedCount : i);
        if (paramTypeNode < 0) {
            return 0;
        }
        if (H2EvalTypeNodeIsTemplateParamName(fn->file, paramTypeNode)) {
            score += 1;
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind == H2Ast_TYPE_NAME
            && SliceEqCStr(
                fn->file->source,
                fn->file->ast.nodes[paramTypeNode].dataStart,
                fn->file->ast.nodes[paramTypeNode].dataEnd,
                "anytype"))
        {
            continue;
        }
        if (H2EvalAggregateDistanceToType(p, &args[i], fn->file, paramTypeNode, &structDistance)) {
            score += structDistance == 0 ? 16 : (int)(16u - structDistance);
            continue;
        }
        if (!H2EvalValueSimpleKind(&args[i], &argKind, &argAliasTag)
            || !H2EvalClassifySimpleTypeNode(p, fn->file, paramTypeNode, &paramKind, &paramAliasTag)
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

static int32_t H2EvalResolveFunctionBySlice(
    const H2EvalProgram* p,
    const H2Package* _Nullable targetPkg,
    const H2ParsedFile* callerFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const H2CTFEValue* _Nullable args,
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
        const H2EvalFunction* fn = &p->funcs[i];
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
        if (args != NULL && H2EvalScoreFunctionCandidate(p, fn, args, argCount, &score)) {
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

static int32_t H2EvalFindAnyFunctionBySlice(
    const H2EvalProgram* p, const H2ParsedFile* callerFile, uint32_t nameStart, uint32_t nameEnd) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || callerFile == NULL || nameEnd < nameStart || nameEnd > callerFile->sourceLen) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const H2EvalFunction* fn = &p->funcs[i];
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

static int H2EvalExprContainsFieldExpr(const H2Ast* ast, int32_t nodeId) {
    int32_t child;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return 0;
    }
    if (ast->nodes[nodeId].kind == H2Ast_FIELD_EXPR) {
        return 1;
    }
    child = ast->nodes[nodeId].firstChild;
    while (child >= 0) {
        if (H2EvalExprContainsFieldExpr(ast, child)) {
            return 1;
        }
        child = ast->nodes[child].nextSibling;
    }
    return 0;
}

static int H2EvalEvalUnary(
    H2TokenKind op, const H2CTFEValue* inValue, H2CTFEValue* outValue, int* outIsConst) {
    if (outValue == NULL || outIsConst == NULL || inValue == NULL) {
        return -1;
    }
    switch (op) {
        case H2Tok_NOT:
            if (inValue->kind != H2CTFEValue_BOOL) {
                return 0;
            }
            outValue->kind = H2CTFEValue_BOOL;
            outValue->b = inValue->b ? 0u : 1u;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            *outIsConst = 1;
            return 1;
        case H2Tok_SUB:
            if (inValue->kind == H2CTFEValue_INT) {
                H2EvalValueSetInt(outValue, -inValue->i64);
                *outIsConst = 1;
                return 1;
            }
            if (inValue->kind == H2CTFEValue_FLOAT) {
                outValue->kind = H2CTFEValue_FLOAT;
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

static int H2EvalLexCompareStrings(const H2CTFEValue* lhs, const H2CTFEValue* rhs, int* outCmp) {
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

static int32_t H2EvalResolveFunctionByLiteralArgs(
    const H2EvalProgram* p, const char* name, const H2CTFEValue* args, uint32_t argCount) {
    uint32_t i;
    int32_t  best = -1;
    int      bestScore = -1;
    int      ambiguous = 0;
    if (p == NULL || name == NULL) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const H2EvalFunction* fn = &p->funcs[i];
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
        if (args != NULL && H2EvalScoreFunctionCandidate(p, fn, args, argCount, &score)) {
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

static int H2EvalCompareValues(
    H2EvalProgram* p, const H2CTFEValue* lhs, const H2CTFEValue* rhs, int* outCmp, int* outHandled);

static int H2EvalTaggedEnumPayloadEqual(
    H2EvalProgram* p, const H2EvalTaggedEnum* lhs, const H2EvalTaggedEnum* rhs, int* outEqual) {
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
            if (H2EvalCompareValues(
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

static int H2EvalCompareValues(
    H2EvalProgram*     p,
    const H2CTFEValue* lhs,
    const H2CTFEValue* rhs,
    int*               outCmp,
    int*               outHandled) {
    const H2CTFEValue* lhsValue = H2EvalValueTargetOrSelf(lhs);
    const H2CTFEValue* rhsValue = H2EvalValueTargetOrSelf(rhs);
    if (outCmp != NULL) {
        *outCmp = 0;
    }
    if (outHandled != NULL) {
        *outHandled = 0;
    }
    if (lhs == NULL || rhs == NULL || outCmp == NULL || outHandled == NULL) {
        return -1;
    }
    if ((lhsValue->kind == H2CTFEValue_NULL && rhsValue->kind == H2CTFEValue_REFERENCE)
        || (lhsValue->kind == H2CTFEValue_REFERENCE && rhsValue->kind == H2CTFEValue_NULL))
    {
        uintptr_t la = lhsValue->kind == H2CTFEValue_REFERENCE ? (uintptr_t)lhsValue->s.bytes : 0u;
        uintptr_t ra = rhsValue->kind == H2CTFEValue_REFERENCE ? (uintptr_t)rhsValue->s.bytes : 0u;
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if ((lhsValue->kind == H2CTFEValue_NULL && rhsValue->kind == H2CTFEValue_STRING)
        || (lhsValue->kind == H2CTFEValue_STRING && rhsValue->kind == H2CTFEValue_NULL))
    {
        int32_t   typeCode = H2EvalTypeCode_INVALID;
        uintptr_t la = lhsValue->kind == H2CTFEValue_STRING ? (uintptr_t)lhsValue->s.bytes : 0u;
        uintptr_t ra = rhsValue->kind == H2CTFEValue_STRING ? (uintptr_t)rhsValue->s.bytes : 0u;
        const H2CTFEValue* stringValue = lhsValue->kind == H2CTFEValue_STRING ? lhsValue : rhsValue;
        if (!H2EvalValueGetRuntimeTypeCode(stringValue, &typeCode)
            || (typeCode != H2EvalTypeCode_RAWPTR && typeCode != H2EvalTypeCode_STR_PTR
                && typeCode != H2EvalTypeCode_STR_REF))
        {
            return 0;
        }
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhs->kind == H2CTFEValue_REFERENCE && rhs->kind == H2CTFEValue_REFERENCE) {
        uintptr_t la = (uintptr_t)lhs->s.bytes;
        uintptr_t ra = (uintptr_t)rhs->s.bytes;
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if ((lhsValue->kind == H2CTFEValue_REFERENCE && rhsValue->kind == H2CTFEValue_STRING)
        || (lhsValue->kind == H2CTFEValue_STRING && rhsValue->kind == H2CTFEValue_REFERENCE))
    {
        int32_t            typeCode = H2EvalTypeCode_INVALID;
        const H2CTFEValue* stringValue = lhsValue->kind == H2CTFEValue_STRING ? lhsValue : rhsValue;
        uintptr_t          la =
            lhsValue->kind == H2CTFEValue_REFERENCE
                ? (uintptr_t)lhsValue->s.bytes
                : (uintptr_t)lhsValue->s.bytes;
        uintptr_t ra =
            rhsValue->kind == H2CTFEValue_REFERENCE
                ? (uintptr_t)rhsValue->s.bytes
                : (uintptr_t)rhsValue->s.bytes;
        if (!H2EvalValueGetRuntimeTypeCode(stringValue, &typeCode)
            || (typeCode != H2EvalTypeCode_RAWPTR && typeCode != H2EvalTypeCode_STR_PTR
                && typeCode != H2EvalTypeCode_STR_REF))
        {
            return 0;
        }
        *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == H2CTFEValue_INT && rhsValue->kind == H2CTFEValue_INT) {
        *outCmp = lhsValue->i64 < rhsValue->i64 ? -1 : (lhsValue->i64 > rhsValue->i64 ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == H2CTFEValue_FLOAT && rhsValue->kind == H2CTFEValue_FLOAT) {
        *outCmp = lhsValue->f64 < rhsValue->f64 ? -1 : (lhsValue->f64 > rhsValue->f64 ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == H2CTFEValue_BOOL && rhsValue->kind == H2CTFEValue_BOOL) {
        *outCmp = lhsValue->b < rhsValue->b ? -1 : (lhsValue->b > rhsValue->b ? 1 : 0);
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == H2CTFEValue_STRING && rhsValue->kind == H2CTFEValue_STRING) {
        int32_t lhsTypeCode = H2EvalTypeCode_INVALID;
        int32_t rhsTypeCode = H2EvalTypeCode_INVALID;
        if ((H2EvalValueGetRuntimeTypeCode(lhsValue, &lhsTypeCode)
             && (lhsTypeCode == H2EvalTypeCode_RAWPTR || lhsTypeCode == H2EvalTypeCode_STR_PTR))
            || (H2EvalValueGetRuntimeTypeCode(rhsValue, &rhsTypeCode)
                && (rhsTypeCode == H2EvalTypeCode_RAWPTR || rhsTypeCode == H2EvalTypeCode_STR_PTR)))
        {
            uintptr_t la = (uintptr_t)lhsValue->s.bytes;
            uintptr_t ra = (uintptr_t)rhsValue->s.bytes;
            *outCmp = la < ra ? -1 : (la > ra ? 1 : 0);
            *outHandled = 1;
            return 0;
        }
        if (H2EvalLexCompareStrings(lhsValue, rhsValue, outCmp) != 0) {
            return -1;
        }
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == H2CTFEValue_TYPE && rhsValue->kind == H2CTFEValue_TYPE) {
        int32_t lhsTypeCode = 0;
        int32_t rhsTypeCode = 0;
        if (H2EvalValueGetSimpleTypeCode(lhsValue, &lhsTypeCode)
            && H2EvalValueGetSimpleTypeCode(rhsValue, &rhsTypeCode))
        {
            *outCmp = lhsTypeCode < rhsTypeCode ? -1 : (lhsTypeCode > rhsTypeCode ? 1 : 0);
            *outHandled = 1;
            return 0;
        }
        {
            H2EvalReflectedType* lhsType = H2EvalValueAsReflectedType(lhsValue);
            H2EvalReflectedType* rhsType = H2EvalValueAsReflectedType(rhsValue);
            if (lhsType != NULL && rhsType != NULL) {
                if (lhsType->kind != rhsType->kind || lhsType->namedKind != rhsType->namedKind
                    || lhsType->file != rhsType->file || lhsType->nodeId != rhsType->nodeId
                    || lhsType->arrayLen != rhsType->arrayLen)
                {
                    *outCmp = 1;
                    *outHandled = 1;
                    return 0;
                }
                if (lhsType->kind == H2EvalReflectType_PTR
                    || lhsType->kind == H2EvalReflectType_SLICE
                    || lhsType->kind == H2EvalReflectType_ARRAY)
                {
                    int elemCmp = 0;
                    int elemHandled = 0;
                    if (H2EvalCompareValues(
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
        H2EvalTaggedEnum* lhsTagged = H2EvalValueAsTaggedEnum(lhsValue);
        H2EvalTaggedEnum* rhsTagged = H2EvalValueAsTaggedEnum(rhsValue);
        if (lhsTagged != NULL && rhsTagged != NULL && lhsTagged->file == rhsTagged->file
            && lhsTagged->enumNode == rhsTagged->enumNode)
        {
            int equal = 0;
            if (H2EvalTaggedEnumPayloadEqual(p, lhsTagged, rhsTagged, &equal) != 0) {
                return -1;
            }
            *outCmp = equal ? 0 : 1;
            *outHandled = 1;
            return 0;
        }
    }
    {
        H2EvalArray* lhsArray = H2EvalValueAsArray(lhsValue);
        H2EvalArray* rhsArray = H2EvalValueAsArray(rhsValue);
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
                if (H2EvalCompareValues(p, &lhsArray->elems[i], &rhsArray->elems[i], &cmp, &handled)
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
        H2EvalAggregate* lhsAgg = H2EvalValueAsAggregate(lhsValue);
        H2EvalAggregate* rhsAgg = H2EvalValueAsAggregate(rhsValue);
        if (lhsAgg != NULL && rhsAgg != NULL) {
            H2CTFEValue args[2];
            args[0] = *lhsValue;
            args[1] = *rhsValue;
            {
                int32_t hookIndex = H2EvalResolveFunctionByLiteralArgs(p, "__order", args, 2);
                if (hookIndex >= 0) {
                    H2CTFEValue hookValue;
                    int         didReturn = 0;
                    int64_t     order = 0;
                    if (H2EvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && H2CTFEValueToInt64(&hookValue, &order) == 0) {
                        *outCmp = order < 0 ? -1 : (order > 0 ? 1 : 0);
                        *outHandled = 1;
                        return 0;
                    }
                }
            }
            {
                int32_t hookIndex = H2EvalResolveFunctionByLiteralArgs(p, "__equal", args, 2);
                if (hookIndex >= 0) {
                    H2CTFEValue hookValue;
                    int         didReturn = 0;
                    if (H2EvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && hookValue.kind == H2CTFEValue_BOOL) {
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
                    if (H2EvalCompareValues(
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

static int H2EvalCompareOptionalEq(
    H2EvalProgram*     p,
    const H2CTFEValue* lhs,
    const H2CTFEValue* rhs,
    uint8_t*           outEqual,
    int*               outHandled) {
    const H2CTFEValue* lhsValue = H2EvalValueTargetOrSelf(lhs);
    const H2CTFEValue* rhsValue = H2EvalValueTargetOrSelf(rhs);
    const H2CTFEValue* lhsPayload = NULL;
    const H2CTFEValue* rhsPayload = NULL;
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
        lhsValue->kind == H2CTFEValue_OPTIONAL && H2EvalOptionalPayload(lhsValue, &lhsPayload);
    rhsIsOptional =
        rhsValue->kind == H2CTFEValue_OPTIONAL && H2EvalOptionalPayload(rhsValue, &rhsPayload);
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
            if (H2EvalCompareValues(p, lhsPayload, rhsPayload, &cmp, &handled) != 0) {
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
            *outEqual = rhsValue->kind == H2CTFEValue_NULL ? 1u : 0u;
            *outHandled = 1;
            return 0;
        }
        if (rhsValue->kind == H2CTFEValue_NULL) {
            *outEqual = 0u;
            *outHandled = 1;
            return 0;
        }
        {
            int cmp = 0;
            int handled = 0;
            if (H2EvalCompareValues(p, lhsPayload, rhsValue, &cmp, &handled) != 0) {
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
        *outEqual = lhsValue->kind == H2CTFEValue_NULL ? 1u : 0u;
        *outHandled = 1;
        return 0;
    }
    if (lhsValue->kind == H2CTFEValue_NULL) {
        *outEqual = 0u;
        *outHandled = 1;
        return 0;
    }
    {
        int cmp = 0;
        int handled = 0;
        if (H2EvalCompareValues(p, lhsValue, rhsPayload, &cmp, &handled) != 0) {
            return -1;
        }
        if (handled) {
            *outEqual = cmp == 0 ? 1u : 0u;
            *outHandled = 1;
        }
    }
    return 0;
}

static int H2EvalEvalBinary(
    H2EvalProgram*     p,
    H2TokenKind        op,
    const H2CTFEValue* lhs,
    const H2CTFEValue* rhs,
    H2CTFEValue*       outValue,
    int*               outIsConst) {
    int cmp = 0;
    int handled = 0;
    if (lhs == NULL || rhs == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if ((op == H2Tok_EQ || op == H2Tok_NEQ || op == H2Tok_LT || op == H2Tok_LTE || op == H2Tok_GT
         || op == H2Tok_GTE))
    {
        H2EvalAggregate* lhsAgg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(lhs));
        H2EvalAggregate* rhsAgg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(rhs));
        if (p != NULL && lhsAgg != NULL && rhsAgg != NULL) {
            H2CTFEValue args[2];
            int32_t     hookIndex = -1;
            H2CTFEValue hookValue;
            int         didReturn = 0;
            args[0] = *H2EvalValueTargetOrSelf(lhs);
            args[1] = *H2EvalValueTargetOrSelf(rhs);
            if (op == H2Tok_EQ || op == H2Tok_NEQ) {
                hookIndex = H2EvalResolveFunctionByLiteralArgs(p, "__equal", args, 2);
                if (hookIndex >= 0) {
                    if (H2EvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && hookValue.kind == H2CTFEValue_BOOL) {
                        outValue->kind = H2CTFEValue_BOOL;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->typeTag = 0;
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->b = op == H2Tok_EQ ? hookValue.b : (uint8_t)!hookValue.b;
                        *outIsConst = 1;
                        return 1;
                    }
                }
            } else {
                hookIndex = H2EvalResolveFunctionByLiteralArgs(p, "__order", args, 2);
                if (hookIndex >= 0) {
                    int64_t order = 0;
                    if (H2EvalInvokeFunction(
                            p, hookIndex, args, 2, p->currentContext, &hookValue, &didReturn)
                        != 0)
                    {
                        return -1;
                    }
                    if (didReturn && H2CTFEValueToInt64(&hookValue, &order) == 0) {
                        outValue->kind = H2CTFEValue_BOOL;
                        outValue->i64 = 0;
                        outValue->f64 = 0.0;
                        outValue->typeTag = 0;
                        outValue->s.bytes = NULL;
                        outValue->s.len = 0;
                        outValue->b =
                            op == H2Tok_LT    ? order < 0
                            : op == H2Tok_LTE ? order <= 0
                            : op == H2Tok_GT
                                ? order > 0
                                : order >= 0;
                        *outIsConst = 1;
                        return 1;
                    }
                }
            }
        }
    }
    if (op == H2Tok_EQ || op == H2Tok_NEQ || op == H2Tok_LT || op == H2Tok_LTE || op == H2Tok_GT
        || op == H2Tok_GTE)
    {
        H2EvalTaggedEnum* lhsTagged = H2EvalValueAsTaggedEnum(H2EvalValueTargetOrSelf(lhs));
        H2EvalTaggedEnum* rhsTagged = H2EvalValueAsTaggedEnum(H2EvalValueTargetOrSelf(rhs));
        if ((op == H2Tok_LT || op == H2Tok_LTE || op == H2Tok_GT || op == H2Tok_GTE)
            && lhsTagged != NULL && rhsTagged != NULL && lhsTagged->file == rhsTagged->file
            && lhsTagged->enumNode == rhsTagged->enumNode)
        {
            outValue->kind = H2CTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            switch (op) {
                case H2Tok_LT:  outValue->b = lhsTagged->tagIndex < rhsTagged->tagIndex; break;
                case H2Tok_LTE: outValue->b = lhsTagged->tagIndex <= rhsTagged->tagIndex; break;
                case H2Tok_GT:  outValue->b = lhsTagged->tagIndex > rhsTagged->tagIndex; break;
                case H2Tok_GTE: outValue->b = lhsTagged->tagIndex >= rhsTagged->tagIndex; break;
                default:        outValue->b = 0; break;
            }
            *outIsConst = 1;
            return 1;
        }
        if (op == H2Tok_EQ || op == H2Tok_NEQ) {
            uint8_t equal = 0;
            if (H2EvalCompareOptionalEq(p, lhs, rhs, &equal, &handled) != 0) {
                return -1;
            }
            if (handled) {
                outValue->kind = H2CTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                outValue->b = op == H2Tok_EQ ? equal : (uint8_t)!equal;
                *outIsConst = 1;
                return 1;
            }
        }
        if (H2EvalCompareValues(p, lhs, rhs, &cmp, &handled) != 0) {
            return -1;
        }
        if (handled) {
            outValue->kind = H2CTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            switch (op) {
                case H2Tok_EQ:  outValue->b = cmp == 0; break;
                case H2Tok_NEQ: outValue->b = cmp != 0; break;
                case H2Tok_LT:  outValue->b = cmp < 0; break;
                case H2Tok_LTE: outValue->b = cmp <= 0; break;
                case H2Tok_GT:  outValue->b = cmp > 0; break;
                case H2Tok_GTE: outValue->b = cmp >= 0; break;
                default:        break;
            }
            *outIsConst = 1;
            return 1;
        }
    }
    if (lhs->kind == H2CTFEValue_INT && rhs->kind == H2CTFEValue_INT) {
        switch (op) {
            case H2Tok_ADD: H2EvalValueSetInt(outValue, lhs->i64 + rhs->i64); break;
            case H2Tok_SUB: H2EvalValueSetInt(outValue, lhs->i64 - rhs->i64); break;
            case H2Tok_MUL: H2EvalValueSetInt(outValue, lhs->i64 * rhs->i64); break;
            case H2Tok_DIV:
                if (rhs->i64 == 0) {
                    return 0;
                }
                H2EvalValueSetInt(outValue, lhs->i64 / rhs->i64);
                break;
            case H2Tok_MOD:
                if (rhs->i64 == 0) {
                    return 0;
                }
                H2EvalValueSetInt(outValue, lhs->i64 % rhs->i64);
                break;
            default: return 0;
        }
        *outIsConst = 1;
        return 1;
    }
    if (lhs->kind == H2CTFEValue_BOOL && rhs->kind == H2CTFEValue_BOOL) {
        switch (op) {
            case H2Tok_AND:
            case H2Tok_OR:
            case H2Tok_LOGICAL_AND:
            case H2Tok_LOGICAL_OR:
                outValue->kind = H2CTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->typeTag = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (op == H2Tok_AND || op == H2Tok_LOGICAL_AND) {
                    outValue->b = lhs->b && rhs->b;
                } else {
                    outValue->b = lhs->b || rhs->b;
                }
                *outIsConst = 1;
                return 1;
            default: return 0;
        }
    }
    if (lhs->kind == H2CTFEValue_FLOAT && rhs->kind == H2CTFEValue_FLOAT) {
        switch (op) {
            case H2Tok_ADD:
            case H2Tok_SUB:
            case H2Tok_MUL:
            case H2Tok_DIV:
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 =
                    op == H2Tok_ADD   ? lhs->f64 + rhs->f64
                    : op == H2Tok_SUB ? lhs->f64 - rhs->f64
                    : op == H2Tok_MUL
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
    if (lhs->kind == H2CTFEValue_NULL && rhs->kind == H2CTFEValue_NULL) {
        if (op == H2Tok_EQ || op == H2Tok_NEQ) {
            outValue->kind = H2CTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = op == H2Tok_EQ ? 1u : 0u;
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

static int H2EvalResolveAggregateTypeNode(
    const H2EvalProgram* p,
    const H2ParsedFile*  typeFile,
    int32_t              typeNode,
    const H2ParsedFile** outDeclFile,
    int32_t*             outDeclNode) {
    const H2AstNode*    n;
    const H2ParsedFile* declFile = NULL;
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
    while (n->kind == H2Ast_TYPE_REF || n->kind == H2Ast_TYPE_MUTREF) {
        typeNode = n->firstChild;
        if (typeNode < 0 || (uint32_t)typeNode >= typeFile->ast.len) {
            return 0;
        }
        n = &typeFile->ast.nodes[typeNode];
    }
    if (n->kind == H2Ast_TYPE_ANON_STRUCT || n->kind == H2Ast_TYPE_ANON_UNION) {
        if (outDeclFile != NULL) {
            *outDeclFile = typeFile;
        }
        if (outDeclNode != NULL) {
            *outDeclNode = typeNode;
        }
        return 1;
    }
    if (n->kind != H2Ast_TYPE_NAME) {
        return 0;
    }
    declNode = H2EvalFindNamedAggregateDecl(p, typeFile, typeNode, &declFile);
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

static int H2EvalExecExprForTypeCb(
    void* ctx, int32_t exprNode, int32_t typeNode, H2CTFEValue* outValue, int* outIsConst);

static int H2EvalExecExprInFileWithType(
    H2EvalProgram*      p,
    const H2ParsedFile* exprFile,
    H2CTFEExecEnv*      env,
    int32_t             exprNode,
    const H2ParsedFile* typeFile,
    int32_t             typeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst) {
    const H2ParsedFile* savedFile;
    H2CTFEExecCtx*      savedExecCtx;
    H2CTFEExecCtx       tempExecCtx;
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
    tempExecCtx.evalExpr = H2EvalExecExprCb;
    tempExecCtx.evalExprCtx = p;
    tempExecCtx.evalExprForType = H2EvalExecExprForTypeCb;
    tempExecCtx.evalExprForTypeCtx = p;
    tempExecCtx.zeroInit = H2EvalZeroInitCb;
    tempExecCtx.zeroInitCtx = p;
    tempExecCtx.assignExpr = H2EvalAssignExprCb;
    tempExecCtx.assignExprCtx = p;
    tempExecCtx.assignValueExpr = H2EvalAssignValueExprCb;
    tempExecCtx.assignValueExprCtx = p;
    tempExecCtx.matchPattern = H2EvalMatchPatternCb;
    tempExecCtx.matchPatternCtx = p;
    tempExecCtx.forInIndex = H2EvalForInIndexCb;
    tempExecCtx.forInIndexCtx = p;
    tempExecCtx.forInIter = H2EvalForInIterCb;
    tempExecCtx.forInIterCtx = p;
    tempExecCtx.pendingReturnExprNode = -1;
    if (tempExecCtx.forIterLimit == 0) {
        tempExecCtx.forIterLimit = H2CTFE_EXEC_DEFAULT_FOR_LIMIT;
    }
    H2CTFEExecResetReason(&tempExecCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = exprFile;
    p->currentExecCtx = &tempExecCtx;
    rc = H2EvalExecExprWithTypeNode(p, exprNode, typeFile, typeNode, outValue, outIsConst);
    p->currentExecCtx = savedExecCtx;
    p->currentFile = savedFile;

    if (rc == 0 && !*outIsConst && savedExecCtx != NULL && tempExecCtx.nonConstReason != NULL) {
        H2CTFEExecSetReason(
            savedExecCtx,
            tempExecCtx.nonConstStart,
            tempExecCtx.nonConstEnd,
            tempExecCtx.nonConstReason);
    }
    return rc;
}

static int H2EvalEvalCompoundLiteral(
    H2EvalProgram*      p,
    int32_t             exprNode,
    const H2ParsedFile* litFile,
    const H2ParsedFile* expectedTypeFile,
    int32_t             expectedTypeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst) {
    const H2Ast*                  ast;
    int32_t                       child;
    int32_t                       fieldNode;
    const H2ParsedFile*           declFile = NULL;
    int32_t                       declNode = -1;
    const H2ParsedFile*           targetTypeFile = expectedTypeFile;
    int32_t                       targetTypeNode = expectedTypeNode;
    H2CTFEValue                   aggregateValue;
    int                           aggregateIsConst = 0;
    H2EvalAggregate*              agg;
    uint8_t*                      explicitSet = NULL;
    H2CTFEValue*                  explicitValues = NULL;
    H2EvalExplicitAggregateField* promotedExplicit = NULL;
    H2CTFEExecBinding*            fieldBindings = NULL;
    H2CTFEExecEnv                 fieldFrame;
    uint32_t                      promotedExplicitCap = 0;
    uint32_t                      promotedExplicitLen = 0;
    uint32_t                      fieldBindingCap = 0;
    uint32_t                      i;
    int                           rc = 0;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || litFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &litFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len
        || ast->nodes[exprNode].kind != H2Ast_COMPOUND_LIT)
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
        && targetTypeFile->ast.nodes[targetTypeNode].kind == H2Ast_TYPE_NAME)
    {
        const H2AstNode* typeNode = &targetTypeFile->ast.nodes[targetTypeNode];
        uint32_t         dot = typeNode->dataStart;
        while (dot < typeNode->dataEnd && targetTypeFile->source[dot] != '.') {
            dot++;
        }
        if (dot < typeNode->dataEnd) {
            const H2ParsedFile* enumFile = NULL;
            int32_t             enumNode = H2EvalFindNamedEnumDecl(
                p, targetTypeFile, typeNode->dataStart, dot, &enumFile);
            int32_t  variantNode = -1;
            uint32_t tagIndex = 0;
            if (enumNode >= 0 && enumFile != NULL
                && H2EvalFindEnumVariant(
                    enumFile,
                    enumNode,
                    targetTypeFile->source,
                    dot + 1u,
                    typeNode->dataEnd,
                    &variantNode,
                    &tagIndex))
            {
                const H2AstNode* variantField = &enumFile->ast.nodes[variantNode];
                H2CTFEValue      payloadValue;
                int              payloadIsConst = 0;
                H2EvalAggregate* payload = NULL;
                if (H2EvalBuildTaggedEnumPayload(
                        p, enumFile, variantNode, exprNode, &payloadValue, &payloadIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!payloadIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                payload = H2EvalValueAsAggregate(&payloadValue);
                H2EvalValueSetTaggedEnum(
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
        && targetTypeFile->ast.nodes[targetTypeNode].kind == H2Ast_TYPE_NAME
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
        H2CTFEValue stringValue;
        int         stringIsConst = 0;
        if (H2EvalZeroInitTypeNode(p, targetTypeFile, targetTypeNode, &stringValue, &stringIsConst)
            != 0)
        {
            return -1;
        }
        if (!stringIsConst) {
            *outIsConst = 0;
            return 0;
        }
        while (fieldNode >= 0) {
            const H2AstNode* fieldAst = &ast->nodes[fieldNode];
            int32_t          valueNode = ASTFirstChild(ast, fieldNode);
            H2CTFEValue      fieldValue;
            int              fieldIsConst = 0;
            if (fieldAst->kind != H2Ast_COMPOUND_FIELD || valueNode < 0) {
                *outIsConst = 0;
                return 0;
            }
            if (H2EvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
                return -1;
            }
            if (!fieldIsConst
                || !H2EvalValueSetFieldPath(
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
    if (!H2EvalResolveAggregateTypeNode(
            p,
            targetTypeFile != NULL ? targetTypeFile : litFile,
            targetTypeNode,
            &declFile,
            &declNode))
    {
        uint32_t inferredFieldCount = 0;
        int32_t  scanNode = fieldNode;
        while (scanNode >= 0) {
            if (ast->nodes[scanNode].kind != H2Ast_COMPOUND_FIELD) {
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
        agg = (H2EvalAggregate*)H2ArenaAlloc(
            p->arena, sizeof(H2EvalAggregate), (uint32_t)_Alignof(H2EvalAggregate));
        if (agg == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg, 0, sizeof(*agg));
        agg->file = litFile;
        agg->nodeId = exprNode;
        agg->fieldLen = inferredFieldCount;
        agg->fields = (H2EvalAggregateField*)H2ArenaAlloc(
            p->arena,
            sizeof(H2EvalAggregateField) * inferredFieldCount,
            (uint32_t)_Alignof(H2EvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(H2EvalAggregateField) * inferredFieldCount);
        scanNode = fieldNode;
        for (i = 0; i < inferredFieldCount; i++) {
            const H2AstNode* fieldAst = &ast->nodes[scanNode];
            int32_t          valueNode = ASTFirstChild(ast, scanNode);
            int              fieldIsConst = 0;
            if (valueNode < 0
                || H2EvalExecExprCb(p, valueNode, &agg->fields[i].value, &fieldIsConst) != 0)
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
        H2EvalValueSetAggregate(outValue, litFile, exprNode, agg);
        *outIsConst = 1;
        return 0;
    }
    if (H2EvalZeroInitAggregateValue(p, declFile, declNode, &aggregateValue, &aggregateIsConst)
        != 0)
    {
        return -1;
    }
    if (!aggregateIsConst) {
        *outIsConst = 0;
        return 0;
    }
    agg = H2EvalValueAsAggregate(&aggregateValue);
    if (agg == NULL) {
        *outIsConst = 0;
        return 0;
    }
    {
        int32_t scanNode = fieldNode;
        while (scanNode >= 0) {
            if (ast->nodes[scanNode].kind == H2Ast_COMPOUND_FIELD) {
                promotedExplicitCap++;
            }
            scanNode = ASTNextSibling(ast, scanNode);
        }
    }
    if (agg->fieldLen > 0) {
        fieldBindingCap = H2EvalAggregateFieldBindingCount(agg);
        explicitSet = (uint8_t*)H2ArenaAlloc(p->arena, agg->fieldLen, (uint32_t)_Alignof(uint8_t));
        explicitValues = (H2CTFEValue*)H2ArenaAlloc(
            p->arena, sizeof(H2CTFEValue) * agg->fieldLen, (uint32_t)_Alignof(H2CTFEValue));
        if (promotedExplicitCap > 0) {
            promotedExplicit = (H2EvalExplicitAggregateField*)H2ArenaAlloc(
                p->arena,
                sizeof(H2EvalExplicitAggregateField) * promotedExplicitCap,
                (uint32_t)_Alignof(H2EvalExplicitAggregateField));
        }
        fieldBindings = (H2CTFEExecBinding*)H2ArenaAlloc(
            p->arena,
            sizeof(H2CTFEExecBinding) * fieldBindingCap,
            (uint32_t)_Alignof(H2CTFEExecBinding));
        if (explicitSet == NULL || explicitValues == NULL
            || (promotedExplicitCap > 0 && promotedExplicit == NULL) || fieldBindings == NULL)
        {
            return ErrorSimple("out of memory");
        }
        memset(explicitSet, 0, agg->fieldLen);
        memset(explicitValues, 0, sizeof(H2CTFEValue) * agg->fieldLen);
        if (promotedExplicit != NULL) {
            memset(promotedExplicit, 0, sizeof(H2EvalExplicitAggregateField) * promotedExplicitCap);
        }
        memset(fieldBindings, 0, sizeof(H2CTFEExecBinding) * fieldBindingCap);
    }

    while (fieldNode >= 0) {
        const H2AstNode*      fieldAst = &ast->nodes[fieldNode];
        int32_t               valueNode = ASTFirstChild(ast, fieldNode);
        int32_t               directFieldIndex;
        int32_t               topFieldIndex;
        H2EvalAggregateField* valueField;
        H2EvalAggregate*      valueFieldOwner = NULL;
        H2CTFEValue           fieldValue;
        int                   fieldIsConst = 0;
        if (fieldAst->kind != H2Ast_COMPOUND_FIELD) {
            *outIsConst = 0;
            return 0;
        }
        directFieldIndex = H2EvalAggregateLookupDirectFieldIndex(
            agg, litFile->source, fieldAst->dataStart, fieldAst->dataEnd);
        topFieldIndex = H2EvalAggregateLookupFieldIndex(
            agg, litFile->source, fieldAst->dataStart, fieldAst->dataEnd);
        valueField = H2EvalAggregateLookupField(
            agg, litFile->source, fieldAst->dataStart, fieldAst->dataEnd, &valueFieldOwner);
        if (valueNode >= 0 && valueField != NULL) {
            if (H2EvalExecExprWithTypeNode(
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
        } else if ((fieldAst->flags & H2AstFlag_COMPOUND_FIELD_SHORTHAND) != 0) {
            if (H2EvalResolveIdent(
                    p, fieldAst->dataStart, fieldAst->dataEnd, &fieldValue, &fieldIsConst, NULL)
                != 0)
            {
                return -1;
            }
        } else if (valueNode >= 0) {
            if (H2EvalExecExprCb(p, valueNode, &fieldValue, &fieldIsConst) != 0) {
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
            if (!H2EvalValueSetFieldPath(
                    &aggregateValue,
                    litFile->source,
                    fieldAst->dataStart,
                    fieldAst->dataEnd,
                    &fieldValue))
            {
                *outIsConst = 0;
                return 0;
            }
        } else if (!H2EvalValueSetFieldPath(
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
        H2EvalAggregateField* field = &agg->fields[i];
        if (explicitSet != NULL && explicitSet[i] != 0u) {
            field->value = explicitValues[i];
        } else if (field->defaultExprNode >= 0) {
            H2CTFEValue defaultValue;
            int         defaultIsConst = 0;
            if (H2EvalExecExprInFileWithType(
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
        if ((field->flags & H2AstFlag_FIELD_EMBEDDED) != 0 && promotedExplicit != NULL) {
            uint32_t j;
            for (j = 0; j < promotedExplicitLen; j++) {
                if (promotedExplicit[j].topFieldIndex == (int32_t)i
                    && !H2EvalValueSetFieldPath(
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
            && H2EvalAppendAggregateFieldBindings(
                   fieldBindings, fieldBindingCap, &fieldFrame, field)
                   != 0)
        {
            return ErrorSimple("out of memory");
        }
    }
    rc = H2EvalFinalizeAggregateVarArrays(p, agg);
    if (rc != 1) {
        *outIsConst = 0;
        return rc < 0 ? -1 : 0;
    }
    *outValue = aggregateValue;
    *outIsConst = 1;
    return 0;
}

static int H2EvalExecExprWithTypeNode(
    H2EvalProgram*      p,
    int32_t             exprNode,
    const H2ParsedFile* typeFile,
    int32_t             typeNode,
    H2CTFEValue*        outValue,
    int*                outIsConst) {
    const H2Ast* ast;
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return -1;
    }
    if (ast->nodes[exprNode].kind == H2Ast_CALL_ARG) {
        exprNode = ast->nodes[exprNode].firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
    }
    if (typeFile != NULL && typeNode >= 0 && (uint32_t)typeNode < typeFile->ast.len
        && typeFile->ast.nodes[typeNode].kind == H2Ast_TYPE_TUPLE
        && ast->nodes[exprNode].kind == H2Ast_TUPLE_EXPR)
    {
        H2CTFEValue elems[256];
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
                || H2EvalExecExprWithTypeNode(
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
        return H2EvalAllocTupleValue(p, typeFile, typeNode, elems, elemCount, outValue, outIsConst);
    }
    if (ast->nodes[exprNode].kind == H2Ast_COMPOUND_LIT) {
        return H2EvalEvalCompoundLiteral(
            p,
            exprNode,
            p->currentFile,
            typeFile != NULL ? typeFile : p->currentFile,
            typeNode,
            outValue,
            outIsConst);
    }
    if (ast->nodes[exprNode].kind == H2Ast_NEW) {
        return H2EvalEvalNewExpr(p, exprNode, typeFile, typeNode, outValue, outIsConst);
    }
    if (ast->nodes[exprNode].kind == H2Ast_CALL && typeFile != NULL && typeNode >= 0) {
        const H2ParsedFile* savedExprFile = p->expectedCallExprFile;
        int32_t             savedExprNode = p->expectedCallExprNode;
        const H2ParsedFile* savedTypeFile = p->expectedCallTypeFile;
        int32_t             savedTypeNode = p->expectedCallTypeNode;
        const H2ParsedFile* savedActiveExpectedTypeFile = p->activeCallExpectedTypeFile;
        int32_t             savedActiveExpectedTypeNode = p->activeCallExpectedTypeNode;
        p->expectedCallExprFile = p->currentFile;
        p->expectedCallExprNode = exprNode;
        p->expectedCallTypeFile = typeFile;
        p->expectedCallTypeNode = typeNode;
        p->activeCallExpectedTypeFile = typeFile;
        p->activeCallExpectedTypeNode = typeNode;
        if (H2EvalExecExprCb(p, exprNode, outValue, outIsConst) != 0) {
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
        if (H2EvalExecExprCb(p, exprNode, outValue, outIsConst) != 0) {
            return -1;
        }
    }
    if (*outIsConst && typeFile != NULL && typeNode >= 0
        && H2EvalCoerceValueToTypeNode(p, typeFile, typeNode, outValue) != 0)
    {
        return -1;
    }
    return 0;
}

static int H2EvalExecExprCb(void* ctx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalAssignExprCb(
    void* ctx, H2CTFEExecCtx* execCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalZeroInitCb(void* ctx, int32_t typeNode, H2CTFEValue* outValue, int* outIsConst);
static int H2EvalMirMakeAggregate(
    void*        ctx,
    uint32_t     sourceNode,
    uint32_t     fieldCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalMirZeroInitLocal(
    void*               ctx,
    const H2MirTypeRef* typeRef,
    H2CTFEValue*        outValue,
    int*                outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalExecExprForTypeCb(
    void* ctx, int32_t exprNode, int32_t typeNode, H2CTFEValue* outValue, int* outIsConst) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    if (p == NULL) {
        return -1;
    }
    return H2EvalExecExprWithTypeNode(p, exprNode, p->currentFile, typeNode, outValue, outIsConst);
}
static int H2EvalResolveIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalResolveCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalResolveCallMir(
    void* ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag);
static int H2EvalEvalTopConst(
    H2EvalProgram* p, uint32_t topConstIndex, H2CTFEValue* outValue, int* outIsConst);

static H2CTFEExecBinding* _Nullable H2EvalFindBinding(
    const H2CTFEExecCtx* _Nullable execCtx,
    const H2ParsedFile* file,
    uint32_t            nameStart,
    uint32_t            nameEnd) {
    const H2CTFEExecEnv* frame;
    if (execCtx == NULL || file == NULL) {
        return NULL;
    }
    frame = execCtx->env;
    while (frame != NULL) {
        uint32_t i = frame->bindingLen;
        while (i > 0) {
            H2CTFEExecBinding* binding;
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

static int H2EvalTypeNodesEquivalent(
    const H2ParsedFile* aFile, int32_t aNode, const H2ParsedFile* bFile, int32_t bNode) {
    const H2AstNode* a;
    const H2AstNode* b;
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
        case H2Ast_TYPE_NAME:
            return SliceEqSlice(
                aFile->source, a->dataStart, a->dataEnd, bFile->source, b->dataStart, b->dataEnd);
        case H2Ast_TYPE_PTR:
        case H2Ast_TYPE_REF:
        case H2Ast_TYPE_MUTREF:
        case H2Ast_TYPE_OPTIONAL:
        case H2Ast_TYPE_SLICE:
        case H2Ast_TYPE_MUTSLICE:
        case H2Ast_TYPE_VARRAY:
            return H2EvalTypeNodesEquivalent(aFile, a->firstChild, bFile, b->firstChild);
        case H2Ast_TYPE_ARRAY:
            return SliceEqSlice(
                       aFile->source,
                       a->dataStart,
                       a->dataEnd,
                       bFile->source,
                       b->dataStart,
                       b->dataEnd)
                && H2EvalTypeNodesEquivalent(aFile, a->firstChild, bFile, b->firstChild);
        default: return 0;
    }
}

static int32_t H2EvalResolveFunctionByTypeNodeLiteral(
    const H2EvalProgram* p, const char* name, const H2ParsedFile* typeFile, int32_t typeNode) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || name == NULL || typeFile == NULL || typeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const H2EvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = H2EvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0
            || !H2EvalTypeNodesEquivalent(fn->file, paramTypeNode, typeFile, typeNode))
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

static int32_t H2EvalResolvePointerFunctionByPointeeTypeLiteral(
    const H2EvalProgram* p,
    const char*          name,
    const H2ParsedFile*  typeFile,
    int32_t              pointeeTypeNode) {
    uint32_t i;
    int32_t  found = -1;
    if (p == NULL || name == NULL || typeFile == NULL || pointeeTypeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const H2EvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        int32_t               childTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = H2EvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (childTypeNode < 0
            || !H2EvalTypeNodesEquivalent(fn->file, childTypeNode, typeFile, pointeeTypeNode))
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

static int H2EvalIsTypeNodeKind(H2AstKind kind) {
    return kind == H2Ast_TYPE_NAME || kind == H2Ast_TYPE_PTR || kind == H2Ast_TYPE_REF
        || kind == H2Ast_TYPE_MUTREF || kind == H2Ast_TYPE_ARRAY || kind == H2Ast_TYPE_VARRAY
        || kind == H2Ast_TYPE_SLICE || kind == H2Ast_TYPE_MUTSLICE || kind == H2Ast_TYPE_OPTIONAL
        || kind == H2Ast_TYPE_FN || kind == H2Ast_TYPE_TUPLE || kind == H2Ast_TYPE_ANON_STRUCT
        || kind == H2Ast_TYPE_ANON_UNION;
}

static int H2EvalFindVisibleLocalTypeNodeByName(
    const H2ParsedFile* file,
    uint32_t            beforePos,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    int32_t*            outTypeNode) {
    const H2Ast* ast;
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
        const H2AstNode* n = &ast->nodes[i];
        int32_t          firstChild;
        int32_t          maybeTypeNode;
        int32_t          initNode;
        int              nameMatches = 0;
        if ((n->kind != H2Ast_VAR && n->kind != H2Ast_CONST) || n->start >= beforePos) {
            continue;
        }
        firstChild = n->firstChild;
        if (firstChild < 0 || (uint32_t)firstChild >= ast->len) {
            continue;
        }
        maybeTypeNode = -1;
        if (ast->nodes[firstChild].kind == H2Ast_NAME_LIST) {
            int32_t afterNames;
            int32_t nameNode = ast->nodes[firstChild].firstChild;
            while (nameNode >= 0) {
                if ((uint32_t)nameNode < ast->len && ast->nodes[nameNode].kind == H2Ast_IDENT
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
                && H2EvalIsTypeNodeKind(ast->nodes[afterNames].kind))
            {
                maybeTypeNode = afterNames;
                initNode = ast->nodes[afterNames].nextSibling;
            } else {
                initNode = afterNames;
            }
        } else {
            nameMatches = SliceEqSlice(
                file->source, n->dataStart, n->dataEnd, file->source, nameStart, nameEnd);
            if (H2EvalIsTypeNodeKind(ast->nodes[firstChild].kind)) {
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
            && ast->nodes[initNode].kind == H2Ast_COMPOUND_LIT)
        {
            int32_t typeNode = ast->nodes[initNode].firstChild;
            if (typeNode >= 0 && (uint32_t)typeNode < ast->len
                && H2EvalIsTypeNodeKind(ast->nodes[typeNode].kind))
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

static int H2EvalResolveAggregateDeclFromValue(
    const H2EvalProgram* p,
    const H2CTFEValue*   value,
    const H2ParsedFile** outFile,
    int32_t*             outNode) {
    const H2CTFEValue* valueTarget;
    H2EvalAggregate*   agg;
    if (outFile != NULL) {
        *outFile = NULL;
    }
    if (outNode != NULL) {
        *outNode = -1;
    }
    if (p == NULL || value == NULL || outFile == NULL || outNode == NULL) {
        return 0;
    }
    valueTarget = H2EvalValueTargetOrSelf(value);
    agg = H2EvalValueAsAggregate(valueTarget);
    if (agg == NULL || agg->file == NULL || agg->nodeId < 0) {
        return 0;
    }
    if ((uint32_t)agg->nodeId < agg->file->ast.len) {
        const H2AstNode* aggNode = &agg->file->ast.nodes[agg->nodeId];
        if (aggNode->kind == H2Ast_COMPOUND_LIT) {
            int32_t             typeNode = aggNode->firstChild;
            const H2ParsedFile* declFile = NULL;
            int32_t             declNode = -1;
            if (typeNode >= 0
                && H2EvalResolveAggregateTypeNode(p, agg->file, typeNode, &declFile, &declNode))
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

static int32_t H2EvalResolvePointerAggregateFunctionByLiteral(
    const H2EvalProgram* p, const char* name, const H2CTFEValue* argValue) {
    const H2CTFEValue*  targetValue;
    H2EvalAggregate*    agg;
    const H2ParsedFile* aggDeclFile = NULL;
    int32_t             aggDeclNode = -1;
    uint32_t            i;
    int32_t             found = -1;
    if (p == NULL || name == NULL || argValue == NULL) {
        return -1;
    }
    targetValue = H2EvalValueTargetOrSelf(argValue);
    agg = H2EvalValueAsAggregate(targetValue);
    if (agg == NULL) {
        return H2EvalResolveFunctionByLiteralArgs(p, name, argValue, 1);
    }
    if (!H2EvalResolveAggregateDeclFromValue(p, argValue, &aggDeclFile, &aggDeclNode)) {
        aggDeclFile = agg->file;
        aggDeclNode = agg->nodeId;
    }
    for (i = 0; i < p->funcLen; i++) {
        const H2EvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        int32_t               childTypeNode;
        const H2ParsedFile*   declFile = NULL;
        int32_t               declNode = -1;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = H2EvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (!H2EvalResolveAggregateTypeNode(p, fn->file, childTypeNode, &declFile, &declNode)
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

static int32_t H2EvalResolveFunctionBySourceExprLiteral(
    H2EvalProgram*      p,
    H2CTFEExecCtx*      execCtx,
    const H2ParsedFile* file,
    int32_t             exprNode,
    const char*         name) {
    const H2AstNode* expr;
    int32_t          bindingTypeNode = -1;
    if (p == NULL || execCtx == NULL || file == NULL || exprNode < 0
        || (uint32_t)exprNode >= file->ast.len || name == NULL)
    {
        return -1;
    }
    expr = &file->ast.nodes[exprNode];
    if (expr->kind == H2Ast_IDENT) {
        H2CTFEExecBinding* binding = H2EvalFindBinding(
            execCtx, file, expr->dataStart, expr->dataEnd);
        if (binding != NULL && binding->typeNode >= 0) {
            return H2EvalResolveFunctionByTypeNodeLiteral(p, name, file, binding->typeNode);
        }
        if (H2EvalFindVisibleLocalTypeNodeByName(
                file, expr->start, expr->dataStart, expr->dataEnd, &bindingTypeNode))
        {
            return H2EvalResolveFunctionByTypeNodeLiteral(p, name, file, bindingTypeNode);
        }
        return -1;
    }
    if (expr->kind == H2Ast_UNARY && (H2TokenKind)expr->op == H2Tok_AND) {
        int32_t childNode = expr->firstChild;
        if (childNode >= 0 && (uint32_t)childNode < file->ast.len
            && file->ast.nodes[childNode].kind == H2Ast_IDENT)
        {
            H2CTFEExecBinding* binding = H2EvalFindBinding(
                execCtx,
                file,
                file->ast.nodes[childNode].dataStart,
                file->ast.nodes[childNode].dataEnd);
            if (binding != NULL && binding->typeNode >= 0) {
                return H2EvalResolvePointerFunctionByPointeeTypeLiteral(
                    p, name, file, binding->typeNode);
            }
            if (H2EvalFindVisibleLocalTypeNodeByName(
                    file,
                    expr->start,
                    file->ast.nodes[childNode].dataStart,
                    file->ast.nodes[childNode].dataEnd,
                    &bindingTypeNode))
            {
                return H2EvalResolvePointerFunctionByPointeeTypeLiteral(
                    p, name, file, bindingTypeNode);
            }
        }
    }
    return -1;
}

static int32_t H2EvalResolveIteratorHookByReturnType(
    const H2EvalProgram* p, const char* name, int32_t iteratorFnIndex) {
    const H2EvalFunction* iteratorFn;
    int32_t               iteratorTypeNode;
    uint32_t              i;
    int32_t               found = -1;
    if (p == NULL || name == NULL || iteratorFnIndex < 0 || (uint32_t)iteratorFnIndex >= p->funcLen)
    {
        return -1;
    }
    iteratorFn = &p->funcs[iteratorFnIndex];
    iteratorTypeNode = H2EvalFunctionReturnTypeNode(iteratorFn);
    if (iteratorTypeNode < 0) {
        return -1;
    }
    for (i = 0; i < p->funcLen; i++) {
        const H2EvalFunction* fn = &p->funcs[i];
        int32_t               paramTypeNode;
        int32_t               childTypeNode;
        if (!SliceEqCStr(fn->file->source, fn->nameStart, fn->nameEnd, name)
            || fn->paramCount != 1u)
        {
            continue;
        }
        paramTypeNode = H2EvalFunctionParamTypeNodeAt(fn, 0);
        if (paramTypeNode < 0) {
            continue;
        }
        if (fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_PTR
            && fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_REF
            && fn->file->ast.nodes[paramTypeNode].kind != H2Ast_TYPE_MUTREF)
        {
            continue;
        }
        childTypeNode = fn->file->ast.nodes[paramTypeNode].firstChild;
        if (!H2EvalTypeNodesEquivalent(fn->file, childTypeNode, iteratorFn->file, iteratorTypeNode))
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

static void H2EvalAdaptForInValueBinding(
    const H2CTFEValue* inValue, int valueRef, H2CTFEValue* outValue) {
    const H2CTFEValue* target;
    if (outValue == NULL) {
        return;
    }
    if (inValue == NULL) {
        H2EvalValueSetNull(outValue);
        return;
    }
    if (valueRef) {
        *outValue = *inValue;
        return;
    }
    target = H2EvalValueReferenceTarget(inValue);
    *outValue = target != NULL ? *target : *inValue;
}

static int32_t H2EvalResolveForInIteratorFn(
    H2EvalProgram* p, H2CTFEExecCtx* execCtx, int32_t sourceNode, const H2CTFEValue* sourceValue) {
    H2CTFEValue         sourceArg;
    const H2ParsedFile* sourceTypeFile = NULL;
    int32_t             sourceTypeNode = -1;
    int32_t             iteratorFn = -1;
    if (p == NULL || execCtx == NULL || sourceValue == NULL) {
        return -1;
    }
    sourceArg = *sourceValue;
    if (p->currentFile != NULL) {
        iteratorFn = H2EvalResolveFunctionBySourceExprLiteral(
            p, execCtx, p->currentFile, sourceNode, "__iterator");
        if (sourceNode >= 0 && (uint32_t)sourceNode < p->currentFile->ast.len
            && p->currentFile->ast.nodes[sourceNode].kind == H2Ast_IDENT)
        {
            H2CTFEExecBinding* binding = H2EvalFindBinding(
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
        iteratorFn = H2EvalResolveFunctionByTypeNodeLiteral(
            p, "__iterator", sourceTypeFile, sourceTypeNode);
    }
    if (iteratorFn < 0) {
        iteratorFn = H2EvalResolvePointerAggregateFunctionByLiteral(p, "__iterator", sourceValue);
    }
    if (iteratorFn < 0) {
        iteratorFn = H2EvalResolveFunctionByLiteralArgs(p, "__iterator", &sourceArg, 1);
    }
    return iteratorFn;
}

static int H2EvalAdvanceForInIterator(
    H2EvalProgram* p,
    H2CTFEExecCtx* execCtx,
    int32_t        iteratorFn,
    H2CTFEValue*   iteratorValue,
    int            hasKey,
    int            keyRef,
    int            valueRef,
    int            valueDiscard,
    int*           outHasItem,
    H2CTFEValue*   outKey,
    int*           outKeyIsConst,
    H2CTFEValue*   outValue,
    int*           outValueIsConst) {
    H2CTFEValue        iterRef;
    H2CTFEValue        callResult;
    const H2CTFEValue* payload = NULL;
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
        H2EvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        H2EvalValueSetNull(outValue);
    }
    if (p == NULL || execCtx == NULL || iteratorValue == NULL || outHasItem == NULL
        || outKey == NULL || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    if (keyRef) {
        H2CTFEExecSetReason(
            execCtx, 0, 0, "for-in key reference is not supported in evaluator backend");
        return 0;
    }
    H2EvalValueSetReference(&iterRef, iteratorValue);
    if (hasKey) {
        if (!valueDiscard) {
            nextFn = H2EvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
            usePair = 1;
        } else {
            nextFn = H2EvalResolveIteratorHookByReturnType(p, "next_key", iteratorFn);
            usePair = 0;
            if (nextFn < 0) {
                nextFn = H2EvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
                usePair = nextFn >= 0 ? 1 : 0;
            }
        }
    } else {
        nextFn = H2EvalResolveIteratorHookByReturnType(p, "next_value", iteratorFn);
        usePair = 0;
        if (nextFn < 0) {
            nextFn = H2EvalResolveIteratorHookByReturnType(p, "next_key_and_value", iteratorFn);
            usePair = nextFn >= 0 ? 1 : 0;
        }
    }
    if (nextFn < 0) {
        H2CTFEExecSetReason(
            execCtx, 0, 0, "for-in iterator hooks are not supported in evaluator backend");
        return 0;
    }
    nextReturnTypeNode = H2EvalFunctionReturnTypeNode(&p->funcs[nextFn]);
    if (nextReturnTypeNode < 0) {
        H2CTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook has unsupported return type");
        return 0;
    }
    if (H2EvalInvokeFunction(p, nextFn, &iterRef, 1, p->currentContext, &callResult, &didReturn)
        != 0)
    {
        return -1;
    }
    if (!didReturn) {
        H2CTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook returned unsupported value");
        return 0;
    }
    if (p->funcs[nextFn].file->ast.nodes[nextReturnTypeNode].kind == H2Ast_TYPE_OPTIONAL) {
        if (callResult.kind == H2CTFEValue_OPTIONAL) {
            if (!H2EvalOptionalPayload(&callResult, &payload)) {
                H2CTFEExecSetReason(
                    execCtx, 0, 0, "for-in iterator hook returned unsupported value");
                return 0;
            }
            if (callResult.b == 0u || payload == NULL) {
                *outHasItem = 0;
                *outKeyIsConst = 1;
                *outValueIsConst = 1;
                return 0;
            }
        } else if (callResult.kind == H2CTFEValue_NULL) {
            *outHasItem = 0;
            *outKeyIsConst = 1;
            *outValueIsConst = 1;
            return 0;
        } else {
            payload = &callResult;
        }
    } else {
        H2CTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook has unsupported return type");
        return 0;
    }
    if (payload == NULL) {
        *outHasItem = 0;
        *outKeyIsConst = 1;
        *outValueIsConst = 1;
        return 0;
    }
    if (usePair) {
        const H2CTFEValue* pairValue = H2EvalValueTargetOrSelf(payload);
        H2EvalArray*       tuple = H2EvalValueAsArray(pairValue);
        if (tuple == NULL || tuple->len != 2u) {
            H2CTFEExecSetReason(execCtx, 0, 0, "for-in pair iterator returned malformed tuple");
            return 0;
        }
        if (hasKey) {
            *outKey = tuple->elems[0];
            *outKeyIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
        if (!valueDiscard) {
            H2EvalAdaptForInValueBinding(&tuple->elems[1], valueRef, outValue);
            *outValueIsConst = 1;
        } else {
            *outValueIsConst = 1;
        }
    } else if (hasKey) {
        *outKey = *payload;
        *outKeyIsConst = 1;
        *outValueIsConst = 1;
    } else {
        H2EvalAdaptForInValueBinding(payload, valueRef, outValue);
        *outValueIsConst = 1;
        *outKeyIsConst = 1;
    }
    return 0;
}

static int H2EvalForInIterCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    int32_t            sourceNode,
    const H2CTFEValue* sourceValue,
    uint32_t           index,
    int                hasKey,
    int                keyRef,
    int                valueRef,
    int                valueDiscard,
    int*               outHasItem,
    H2CTFEValue*       outKey,
    int*               outKeyIsConst,
    H2CTFEValue*       outValue,
    int*               outValueIsConst) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    H2CTFEValue    iterValue;
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
        H2EvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        H2EvalValueSetNull(outValue);
    }
    if (p == NULL || execCtx == NULL || sourceValue == NULL || outHasItem == NULL || outKey == NULL
        || outKeyIsConst == NULL || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    if (keyRef) {
        H2CTFEExecSetReason(
            execCtx, 0, 0, "for-in key reference is not supported in evaluator backend");
        return 0;
    }
    iteratorFn = H2EvalResolveForInIteratorFn(p, execCtx, sourceNode, sourceValue);
    if (iteratorFn < 0) {
        H2CTFEExecSetReason(
            execCtx, 0, 0, "for-in loop source is not supported in evaluator backend");
        return 0;
    }
    if (H2EvalInvokeFunction(
            p, iteratorFn, sourceValue, 1, p->currentContext, &iterValue, &didReturn)
        != 0)
    {
        return -1;
    }
    if (!didReturn) {
        H2CTFEExecSetReason(execCtx, 0, 0, "for-in iterator hook did not return a value");
        return 0;
    }
    for (step = 0; step <= index; step++) {
        if (H2EvalAdvanceForInIterator(
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

static int H2EvalZeroInitCb(void* ctx, int32_t typeNode, H2CTFEValue* outValue, int* outIsConst) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL) {
        return -1;
    }
    return H2EvalZeroInitTypeNode(p, p->currentFile, typeNode, outValue, outIsConst);
}

static int H2EvalMirMakeAggregate(
    void*        ctx,
    uint32_t     sourceNode,
    uint32_t     fieldCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*   p = (H2EvalProgram*)ctx;
    H2EvalAggregate* agg;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    agg = (H2EvalAggregate*)H2ArenaAlloc(
        p->arena, sizeof(H2EvalAggregate), (uint32_t)_Alignof(H2EvalAggregate));
    if (agg == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(agg, 0, sizeof(*agg));
    agg->file = p->currentFile;
    agg->nodeId = (int32_t)sourceNode;
    if (sourceNode < p->currentFile->ast.len) {
        const H2AstNode*    sourceAst = &p->currentFile->ast.nodes[sourceNode];
        const H2ParsedFile* declFile = NULL;
        int32_t             declNode = -1;
        if (sourceAst->kind == H2Ast_COMPOUND_LIT && sourceAst->firstChild >= 0
            && H2EvalResolveAggregateTypeNode(
                p, p->currentFile, sourceAst->firstChild, &declFile, &declNode))
        {
            agg->file = declFile;
            agg->nodeId = declNode;
        }
    }
    agg->fieldLen = fieldCount;
    if (fieldCount > 0) {
        agg->fields = (H2EvalAggregateField*)H2ArenaAlloc(
            p->arena,
            sizeof(H2EvalAggregateField) * fieldCount,
            (uint32_t)_Alignof(H2EvalAggregateField));
        if (agg->fields == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(agg->fields, 0, sizeof(H2EvalAggregateField) * fieldCount);
        {
            uint32_t i;
            for (i = 0; i < fieldCount; i++) {
                agg->fields[i].typeNode = -1;
                agg->fields[i].defaultExprNode = -1;
            }
        }
    }
    H2EvalValueSetAggregate(outValue, agg->file, agg->nodeId, agg);
    outValue->typeTag |= H2CTFEValueTag_AGG_PARTIAL;
    *outIsConst = 1;
    return 0;
}

static int H2EvalMirFindCallNodeBySpan(
    const H2ParsedFile* file,
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
        const H2AstNode* call = &file->ast.nodes[i];
        const H2AstNode* callee;
        int32_t          calleeNode;
        uint32_t         curArgCount = 0;
        int32_t          argNode;
        uint32_t         curStart;
        uint32_t         curEnd;
        if (call->kind != H2Ast_CALL) {
            continue;
        }
        calleeNode = call->firstChild;
        if (calleeNode < 0 || (uint32_t)calleeNode >= file->ast.len) {
            continue;
        }
        callee = &file->ast.nodes[calleeNode];
        if (callee->kind != H2Ast_IDENT && callee->kind != H2Ast_FIELD_EXPR) {
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

static int H2EvalMirAdjustCallArgs(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t        calleeFunctionIndex,
    H2MirExecValue* args,
    uint32_t        argCount,
    H2Diag* _Nullable diag) {
    H2EvalProgram*        p = (H2EvalProgram*)ctx;
    H2EvalMirExecCtx*     execCtx;
    uint32_t              evalFnIndex;
    uint32_t              receiverArgCount;
    int32_t               callNode = -1;
    int32_t               calleeNode;
    int32_t               argNode;
    int32_t               firstArgNode;
    const H2EvalFunction* calleeFn;
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
    receiverArgCount = H2MirCallTokDropsReceiverArg0(inst->tok) ? 1u : 0u;
    if (argCount < receiverArgCount) {
        return 0;
    }
    if (!H2EvalMirFindCallNodeBySpan(
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
            const H2AstNode* arg = &p->currentFile->ast.nodes[argNode];
            int32_t          exprNode = argNode;
            int32_t          paramTypeNode;
            int32_t          paramIndex = (int32_t)(argIndex + receiverArgCount);
            if (arg->kind == H2Ast_CALL_ARG) {
                if ((arg->flags & H2AstFlag_CALL_ARG_SPREAD) != 0) {
                    break;
                }
                exprNode = arg->firstChild;
                if (arg->dataEnd > arg->dataStart) {
                    paramIndex = H2EvalFunctionParamIndexByName(
                        calleeFn, p->currentFile->source, arg->dataStart, arg->dataEnd);
                    if (paramIndex < 0) {
                        break;
                    }
                }
            }
            if (exprNode < 0 || (uint32_t)exprNode >= p->currentFile->ast.len) {
                break;
            }
            paramTypeNode = H2EvalFunctionParamTypeNodeAt(calleeFn, (uint32_t)paramIndex);
            if (paramTypeNode >= 0
                && H2EvalExprIsAnytypePackIndex(p, &p->currentFile->ast, exprNode)
                && !H2EvalValueMatchesExpectedTypeNode(
                    p, calleeFn->file, paramTypeNode, &args[argIndex + receiverArgCount]))
            {
                if (p->currentExecCtx != NULL) {
                    H2CTFEExecSetReasonNode(
                        p->currentExecCtx, exprNode, "anytype pack element type mismatch");
                }
                return 1;
            }
            argIndex++;
            argNode = p->currentFile->ast.nodes[argNode].nextSibling;
        }
    }
    (void)H2EvalReorderFixedCallArgsByName(
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
        const H2MirFunction*       calleeMirFn = &program->funcs[calleeFunctionIndex];
        H2EvalTemplateBindingState savedBinding;
        uint32_t                   directArgCount = argCount - receiverArgCount;
        if ((calleeMirFn->flags & H2MirFunctionFlag_VARIADIC) == 0u
            && directArgCount == calleeMirFn->paramCount)
        {
            H2EvalSaveTemplateBinding(p, &savedBinding);
            if (H2EvalBindActiveTemplateForMirCall(
                    p, program, function, inst, calleeFn, args + receiverArgCount, directArgCount))
            {
                p->currentMirExecCtx->pendingTemplateBinding = savedBinding;
                p->currentMirExecCtx->hasPendingTemplateBinding = 1u;
            }
        }
    }
    return 0;
}

static int H2EvalMirAssignIdent(
    void*              ctx,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    const H2CTFEValue* inValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    int32_t        topVarIndex;
    H2CTFEValue    value;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || inValue == NULL || outIsConst == NULL) {
        return -1;
    }
    topVarIndex = H2EvalFindCurrentTopVarBySlice(p, p->currentFile, nameStart, nameEnd);
    if (topVarIndex < 0) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "identifier assignment is not supported by evaluator backend");
        }
        return 0;
    }
    value = *inValue;
    if (p->topVars[(uint32_t)topVarIndex].declTypeNode >= 0
        && H2EvalCoerceValueToTypeNode(
               p,
               p->topVars[(uint32_t)topVarIndex].file,
               p->topVars[(uint32_t)topVarIndex].declTypeNode,
               &value)
               != 0)
    {
        return -1;
    }
    p->topVars[(uint32_t)topVarIndex].value = value;
    p->topVars[(uint32_t)topVarIndex].state = H2EvalTopConstState_READY;
    *outIsConst = 1;
    return 0;
}

static int H2EvalMirZeroInitLocal(
    void*               ctx,
    const H2MirTypeRef* typeRef,
    H2CTFEValue*        outValue,
    int*                outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL || typeRef == NULL || typeRef->astNode == UINT32_MAX
        || typeRef->astNode >= p->currentFile->ast.len)
    {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    return H2EvalZeroInitTypeNode(
        p, p->currentFile, (int32_t)typeRef->astNode, outValue, outIsConst);
}

static int H2EvalMirCoerceValueForType(
    void* ctx, const H2MirTypeRef* typeRef, H2CTFEValue* inOutValue, H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    if (p == NULL || p->currentFile == NULL || typeRef == NULL || inOutValue == NULL
        || typeRef->astNode == UINT32_MAX || typeRef->astNode >= p->currentFile->ast.len)
    {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
            diag->start = 0;
            diag->end = 0;
            diag->argStart = 0;
            diag->argEnd = 0;
        }
        return -1;
    }
    return H2EvalCoerceValueToTypeNode(p, p->currentFile, (int32_t)typeRef->astNode, inOutValue);
}

static int H2EvalMirIndexValue(
    void*              ctx,
    const H2CTFEValue* base,
    const H2CTFEValue* index,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*     p = (H2EvalProgram*)ctx;
    const H2CTFEValue* baseValue;
    H2EvalArray*       array;
    int64_t            indexInt = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = H2EvalValueTargetOrSelf(base);
    if (H2CTFEValueToInt64(index, &indexInt) != 0 || indexInt < 0) {
        return 0;
    }
    array = H2EvalValueAsArray(baseValue);
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

static int H2EvalMirIndexAddr(
    void*              ctx,
    const H2CTFEValue* base,
    const H2CTFEValue* index,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*     p = (H2EvalProgram*)ctx;
    const H2CTFEValue* baseValue;
    H2EvalArray*       array;
    int64_t            indexInt = 0;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || index == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = H2EvalValueTargetOrSelf(base);
    if (H2CTFEValueToInt64(index, &indexInt) != 0 || indexInt < 0) {
        return 0;
    }
    array = H2EvalValueAsArray(baseValue);
    if (array == NULL || (uint64_t)indexInt >= (uint64_t)array->len) {
        H2CTFEValue* targetValue = H2EvalValueReferenceTarget(base);
        int32_t      baseTypeCode = H2EvalTypeCode_INVALID;
        if (targetValue == NULL) {
            targetValue = (H2CTFEValue*)baseValue;
        }
        if (targetValue != NULL && targetValue->kind == H2CTFEValue_STRING
            && H2EvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
            && baseTypeCode == H2EvalTypeCode_STR_PTR
            && (uint64_t)indexInt < (uint64_t)targetValue->s.len)
        {
            H2CTFEValue* byteProxy = (H2CTFEValue*)H2ArenaAlloc(
                p->arena, sizeof(H2CTFEValue), (uint32_t)_Alignof(H2CTFEValue));
            if (byteProxy == NULL) {
                return ErrorSimple("out of memory");
            }
            H2MirValueSetByteRefProxy(byteProxy, (uint8_t*)targetValue->s.bytes + indexInt);
            H2EvalValueSetRuntimeTypeCode(byteProxy, H2EvalTypeCode_U8);
            H2EvalValueSetReference(outValue, byteProxy);
            *outIsConst = 1;
            return 0;
        }
        return 0;
    }
    H2EvalValueSetReference(outValue, &array->elems[(uint32_t)indexInt]);
    *outIsConst = 1;
    return 0;
}

static int H2EvalMirSliceValue(
    void* _Nullable ctx,
    const H2CTFEValue* _Nonnull base,
    const H2CTFEValue* _Nullable start,
    const H2CTFEValue* _Nullable end,
    uint16_t flags,
    H2CTFEValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*     p = (H2EvalProgram*)ctx;
    const H2CTFEValue* baseValue;
    H2EvalArray*       array;
    H2EvalArray*       view;
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
    baseValue = H2EvalValueTargetOrSelf(base);
    if ((flags & H2AstFlag_INDEX_HAS_START) != 0u) {
        if (start == NULL || H2CTFEValueToInt64(start, &startInt) != 0 || startInt < 0) {
            return 0;
        }
    }
    if ((flags & H2AstFlag_INDEX_HAS_END) != 0u) {
        if (end == NULL || H2CTFEValueToInt64(end, &endInt) != 0 || endInt < 0) {
            return 0;
        }
    }
    if (baseValue->kind == H2CTFEValue_STRING) {
        int32_t currentTypeCode = H2EvalTypeCode_INVALID;
        startIndex = (uint32_t)startInt;
        endIndex = endInt >= 0 ? (uint32_t)endInt : baseValue->s.len;
        if (startIndex > endIndex || endIndex > baseValue->s.len) {
            return 0;
        }
        *outValue = *baseValue;
        outValue->s.bytes = baseValue->s.bytes != NULL ? baseValue->s.bytes + startIndex : NULL;
        outValue->s.len = endIndex - startIndex;
        if (!H2EvalValueGetRuntimeTypeCode(baseValue, &currentTypeCode)) {
            currentTypeCode = H2EvalTypeCode_STR_REF;
        }
        H2EvalValueSetRuntimeTypeCode(outValue, currentTypeCode);
        *outIsConst = 1;
        return 0;
    }
    array = H2EvalValueAsArray(baseValue);
    if (array == NULL) {
        return 0;
    }
    startIndex = (uint32_t)startInt;
    endIndex = endInt >= 0 ? (uint32_t)endInt : array->len;
    if (startIndex > endIndex || endIndex > array->len) {
        return 0;
    }
    view = H2EvalAllocArrayView(
        p,
        baseValue->kind == H2CTFEValue_ARRAY ? array->file : p->currentFile,
        array->typeNode,
        array->elemTypeNode,
        array->elems + startIndex,
        endIndex - startIndex);
    if (view == NULL) {
        return ErrorSimple("out of memory");
    }
    {
        H2CTFEValue viewValue;
        H2EvalValueSetArray(&viewValue, p->currentFile, array->typeNode, view);
        return H2EvalAllocReferencedValue(p, &viewValue, outValue, outIsConst);
    }
}

static int H2EvalMirSequenceLen(
    void*              ctx,
    const H2CTFEValue* base,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*     p = (H2EvalProgram*)ctx;
    const H2CTFEValue* baseValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || base == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    baseValue = H2EvalValueTargetOrSelf(base);
    if (baseValue->kind != H2CTFEValue_STRING && baseValue->kind != H2CTFEValue_ARRAY
        && baseValue->kind != H2CTFEValue_NULL)
    {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReason(
                p->currentExecCtx, 0, 0, "len argument is not supported by evaluator backend");
        }
        return 0;
    }
    H2EvalValueSetInt(outValue, (int64_t)baseValue->s.len);
    *outIsConst = 1;
    return 0;
}

static int H2EvalMirMakeTuple(
    void*              ctx,
    const H2CTFEValue* elems,
    uint32_t           elemCount,
    uint32_t           typeNodeHint,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*      p = (H2EvalProgram*)ctx;
    const H2ParsedFile* file;
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
    return H2EvalAllocTupleValue(p, file, typeNode, elems, elemCount, outValue, outIsConst);
}

static int H2EvalMirMakeVariadicPack(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirTypeRef* _Nullable paramTypeRef,
    uint16_t           callFlags,
    const H2CTFEValue* args,
    uint32_t           argCount,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*      p = (H2EvalProgram*)ctx;
    const H2ParsedFile* file;
    const H2EvalArray*  spreadArray = NULL;
    H2EvalArray*        packArray;
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
    if (H2MirCallTokHasSpreadLast(callFlags)) {
        if (argCount == 0u) {
            return 0;
        }
        spreadArray = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&args[argCount - 1u]));
        if (spreadArray == NULL || spreadArray->len > UINT32_MAX - (argCount - 1u)) {
            return 0;
        }
        prefixCount = argCount - 1u;
        packLen = prefixCount + spreadArray->len;
    }
    packArray = H2EvalAllocArrayView(p, file, typeNode, typeNode, NULL, packLen);
    if (packArray == NULL) {
        return ErrorSimple("out of memory");
    }
    if (packLen > 0u) {
        packArray->elems = (H2CTFEValue*)H2ArenaAlloc(
            p->arena, sizeof(H2CTFEValue) * packLen, (uint32_t)_Alignof(H2CTFEValue));
        if (packArray->elems == NULL) {
            return ErrorSimple("out of memory");
        }
        for (i = 0; i < prefixCount; i++) {
            packArray->elems[i] = args[i];
            H2EvalAnnotateUntypedLiteralValue(&packArray->elems[i]);
        }
        if (spreadArray != NULL) {
            uint32_t j;
            for (j = 0; j < spreadArray->len; j++) {
                packArray->elems[prefixCount + j] = spreadArray->elems[j];
                H2EvalAnnotateUntypedLiteralValue(&packArray->elems[prefixCount + j]);
            }
        } else {
            for (; i < packLen; i++) {
                packArray->elems[i] = args[i];
                H2EvalAnnotateUntypedLiteralValue(&packArray->elems[i]);
            }
        }
    }
    H2EvalValueSetArray(outValue, file, typeNode, packArray);
    *outIsConst = 1;
    return 0;
}

static H2EvalMirIteratorState* _Nullable H2EvalMirIteratorStateFromValue(
    const H2CTFEValue* iterValue) {
    H2CTFEValue* target;
    if (iterValue == NULL) {
        return NULL;
    }
    target = H2EvalValueReferenceTarget(iterValue);
    if (target == NULL || target->kind != H2CTFEValue_SPAN
        || target->typeTag != H2_EVAL_MIR_ITER_MAGIC || target->s.bytes == NULL)
    {
        return NULL;
    }
    return (H2EvalMirIteratorState*)target->s.bytes;
}

static int H2EvalMirIterInit(
    void*              ctx,
    uint32_t           sourceNode,
    const H2CTFEValue* source,
    uint16_t           flags,
    H2CTFEValue*       outIter,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*          p = (H2EvalProgram*)ctx;
    H2EvalMirIteratorState* state;
    H2CTFEValue*            target;
    const H2CTFEValue*      sourceValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || source == NULL || outIter == NULL || outIsConst == NULL) {
        return -1;
    }
    if ((flags & H2MirIterFlag_KEY_REF) != 0u) {
        return 0;
    }
    state = (H2EvalMirIteratorState*)H2ArenaAlloc(
        p->arena, sizeof(*state), (uint32_t)_Alignof(H2EvalMirIteratorState));
    target = (H2CTFEValue*)H2ArenaAlloc(p->arena, sizeof(*target), (uint32_t)_Alignof(H2CTFEValue));
    if (state == NULL || target == NULL) {
        return ErrorSimple("out of memory");
    }
    memset(state, 0, sizeof(*state));
    state->magic = H2_EVAL_MIR_ITER_MAGIC;
    state->sourceNode = sourceNode;
    state->index = 0;
    state->iteratorFn = -1;
    state->flags = flags;
    state->sourceValue = *source;
    state->iteratorValue = (H2CTFEValue){ .kind = H2CTFEValue_INVALID };
    sourceValue = H2EvalValueTargetOrSelf(source);
    if (sourceValue->kind == H2CTFEValue_ARRAY || sourceValue->kind == H2CTFEValue_STRING
        || sourceValue->kind == H2CTFEValue_NULL)
    {
        state->kind = H2_EVAL_MIR_ITER_KIND_SEQUENCE;
    } else {
        int didReturn = 0;
        state->kind = H2_EVAL_MIR_ITER_KIND_PROTOCOL;
        if (p->currentExecCtx == NULL) {
            return 0;
        }
        state->iteratorFn = H2EvalResolveForInIteratorFn(
            p, p->currentExecCtx, (int32_t)sourceNode, source);
        if (state->iteratorFn < 0) {
            H2CTFEExecSetReason(
                p->currentExecCtx,
                0,
                0,
                "for-in loop source is not supported in evaluator backend");
            return 0;
        }
        if (H2EvalInvokeFunction(
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
            H2CTFEExecSetReason(
                p->currentExecCtx, 0, 0, "for-in iterator hook did not return a value");
            return 0;
        }
    }
    target->kind = H2CTFEValue_SPAN;
    target->i64 = 0;
    target->f64 = 0.0;
    target->b = 0;
    target->typeTag = H2_EVAL_MIR_ITER_MAGIC;
    target->s.bytes = (const uint8_t*)state;
    target->s.len = 0;
    target->span = (H2CTFESpan){ 0 };
    H2EvalValueSetReference(outIter, target);
    *outIsConst = 1;
    return 0;
}

static int H2EvalMirIterNext(
    void*              ctx,
    const H2CTFEValue* iter,
    uint16_t           flags,
    int*               outHasItem,
    H2CTFEValue*       outKey,
    int*               outKeyIsConst,
    H2CTFEValue*       outValue,
    int*               outValueIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*          p = (H2EvalProgram*)ctx;
    H2EvalMirIteratorState* state;
    const H2CTFEValue*      sourceValue;
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
        H2EvalValueSetNull(outKey);
    }
    if (outValue != NULL) {
        H2EvalValueSetNull(outValue);
    }
    if (p == NULL || iter == NULL || outHasItem == NULL || outKey == NULL || outKeyIsConst == NULL
        || outValue == NULL || outValueIsConst == NULL)
    {
        return -1;
    }
    state = H2EvalMirIteratorStateFromValue(iter);
    if (state == NULL) {
        return 0;
    }
    hasKey = (flags & H2MirIterFlag_HAS_KEY) != 0u;
    keyRef = (flags & H2MirIterFlag_KEY_REF) != 0u;
    valueRef = (flags & H2MirIterFlag_VALUE_REF) != 0u;
    valueDiscard = (flags & H2MirIterFlag_VALUE_DISCARD) != 0u;
    if (state->kind == H2_EVAL_MIR_ITER_KIND_SEQUENCE) {
        sourceValue = H2EvalValueTargetOrSelf(&state->sourceValue);
        if (sourceValue->kind != H2CTFEValue_ARRAY && sourceValue->kind != H2CTFEValue_STRING
            && sourceValue->kind != H2CTFEValue_NULL)
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
            H2EvalValueSetInt(outKey, (int64_t)state->index);
            *outKeyIsConst = 1;
        } else {
            *outKeyIsConst = 1;
        }
        if (!valueDiscard) {
            if (p->currentExecCtx == NULL) {
                return 0;
            }
            if (H2EvalForInIndexCb(
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
    if (state->kind == H2_EVAL_MIR_ITER_KIND_PROTOCOL) {
        if (p->currentExecCtx == NULL) {
            return 0;
        }
        if (H2EvalAdvanceForInIterator(
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

static int H2EvalMirAggGetField(
    void*              ctx,
    const H2CTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*       p = (H2EvalProgram*)ctx;
    const H2CTFEValue*   baseValue;
    const H2CTFEValue*   payload = NULL;
    H2EvalAggregate*     agg = NULL;
    H2EvalReflectedType* rt;
    H2EvalTaggedEnum*    tagged;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || base == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    baseValue = H2EvalValueTargetOrSelf(base);
    tagged = H2EvalValueAsTaggedEnum(baseValue);
    if (tagged != NULL && tagged->payload != NULL
        && H2EvalAggregateGetFieldValue(
            tagged->payload, p->currentFile->source, nameStart, nameEnd, outValue))
    {
        *outIsConst = 1;
        return 0;
    }
    if (baseValue->kind == H2CTFEValue_OPTIONAL && H2EvalOptionalPayload(baseValue, &payload)
        && baseValue->b != 0u && payload != NULL)
    {
        baseValue = H2EvalValueTargetOrSelf(payload);
    }
    if (baseValue->kind == H2CTFEValue_INT && baseValue->i64 == 3
        && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "prefix"))
    {
        *outValue = p->loggerPrefix;
        *outIsConst = 1;
        return 0;
    }
    if (SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "len")
        && (baseValue->kind == H2CTFEValue_STRING || baseValue->kind == H2CTFEValue_ARRAY
            || baseValue->kind == H2CTFEValue_NULL))
    {
        H2EvalValueSetInt(outValue, (int64_t)baseValue->s.len);
        *outIsConst = 1;
        return 0;
    }
    if (SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "cstr")
        && baseValue->kind == H2CTFEValue_STRING)
    {
        *outValue = *baseValue;
        *outIsConst = 1;
        return 0;
    }
    rt = H2EvalValueAsReflectedType(baseValue);
    if (rt != NULL && rt->kind == H2EvalReflectType_NAMED && rt->namedKind == H2EvalTypeKind_ENUM
        && rt->file != NULL && rt->nodeId >= 0 && (uint32_t)rt->nodeId < rt->file->ast.len)
    {
        int32_t  variantNode = -1;
        uint32_t tagIndex = 0;
        if (H2EvalFindEnumVariant(
                rt->file,
                rt->nodeId,
                p->currentFile->source,
                nameStart,
                nameEnd,
                &variantNode,
                &tagIndex))
        {
            const H2AstNode* variantField = &rt->file->ast.nodes[variantNode];
            int32_t          valueNode = ASTFirstChild(&rt->file->ast, variantNode);
            if (valueNode >= 0 && rt->file->ast.nodes[valueNode].kind != H2Ast_FIELD
                && !H2EvalEnumHasPayloadVariants(rt->file, rt->nodeId))
            {
                int valueIsConst = 0;
                if (H2EvalExecExprInFileWithType(
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
            H2EvalValueSetTaggedEnum(
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
    agg = H2EvalValueAsAggregate(baseValue);
    if (agg == NULL) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field access is not supported by evaluator backend");
        }
        return 0;
    }
    if (!H2EvalAggregateGetFieldValue(agg, p->currentFile->source, nameStart, nameEnd, outValue)) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReason(
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

static int H2EvalMirAggAddrField(
    void*              ctx,
    const H2CTFEValue* base,
    uint32_t           nameStart,
    uint32_t           nameEnd,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*     p = (H2EvalProgram*)ctx;
    const H2CTFEValue* baseValue;
    const H2CTFEValue* payload = NULL;
    H2EvalAggregate*   agg = NULL;
    H2CTFEValue*       fieldValue;
    (void)diag;
    if (outIsConst != NULL) {
        *outIsConst = 0;
    }
    if (p == NULL || p->currentFile == NULL || base == NULL || outValue == NULL
        || outIsConst == NULL)
    {
        return -1;
    }
    baseValue = H2EvalValueTargetOrSelf(base);
    if (baseValue->kind == H2CTFEValue_OPTIONAL && H2EvalOptionalPayload(baseValue, &payload)
        && baseValue->b != 0u && payload != NULL)
    {
        baseValue = H2EvalValueTargetOrSelf(payload);
    }
    if (baseValue->kind == H2CTFEValue_INT && baseValue->i64 == 3
        && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "prefix"))
    {
        H2EvalValueSetReference(outValue, &p->loggerPrefix);
        *outIsConst = 1;
        return 0;
    }
    agg = H2EvalValueAsAggregate(baseValue);
    if (agg == NULL) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field address is not supported by evaluator backend");
        }
        return 0;
    }
    fieldValue = H2EvalAggregateLookupFieldValuePtr(
        agg, p->currentFile->source, nameStart, nameEnd);
    if (fieldValue == NULL) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "field address is not supported by evaluator backend");
        }
        return 0;
    }
    H2EvalValueSetReference(outValue, fieldValue);
    *outIsConst = 1;
    return 0;
}

static int H2EvalMirAggSetField(
    void* _Nullable ctx,
    H2CTFEValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
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
        H2CTFEValue*      value = inOutBase;
        H2EvalAggregate*  agg = H2EvalValueAsAggregate(value);
        H2EvalTaggedEnum* tagged = H2EvalValueAsTaggedEnum(value);
        uint32_t          i;
        uint32_t          dot = nameStart;
        while (dot < nameEnd && p->currentFile->source[dot] != '.') {
            dot++;
        }
        if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
            agg = tagged->payload;
        }
        if (agg == NULL) {
            value = (H2CTFEValue*)H2EvalValueTargetOrSelf(inOutBase);
            agg = H2EvalValueAsAggregate(value);
            tagged = H2EvalValueAsTaggedEnum(value);
            if (agg == NULL && tagged != NULL && tagged->payload != NULL) {
                agg = tagged->payload;
            }
        }
        if (agg != NULL && dot == nameEnd) {
            if (value != NULL) {
                value->typeTag |= H2CTFEValueTag_AGG_PARTIAL;
            }
            H2EvalAggregateField* field = H2EvalAggregateFindDirectField(
                agg, p->currentFile->source, nameStart, nameEnd);
            if (field != NULL) {
                H2CTFEValue coercedValue = *inValue;
                if (field->typeNode >= 0
                    && H2EvalCoerceValueToTypeNode(p, agg->file, field->typeNode, &coercedValue)
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
            value->typeTag |= H2CTFEValueTag_AGG_PARTIAL;
        }
    }
    if (!H2EvalValueSetFieldPath(inOutBase, p->currentFile->source, nameStart, nameEnd, inValue)) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReason(
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

static int H2EvalMirHostCall(
    void*              ctx,
    uint32_t           hostId,
    const H2CTFEValue* args,
    uint32_t           argCount,
    H2CTFEValue*       outValue,
    int*               outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (hostId == H2_EVAL_MIR_HOST_PRINT && argCount == 1u && args[0].kind == H2CTFEValue_STRING) {
        if (args[0].s.len > 0 && args[0].s.bytes != NULL) {
            if (fwrite(args[0].s.bytes, 1, args[0].s.len, stdout) != args[0].s.len) {
                return ErrorSimple("failed to write print output");
            }
        }
        fputc('\n', stdout);
        fflush(stdout);
        H2EvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == H2_EVAL_MIR_HOST_CONCAT && argCount == 2u) {
        int concatRc = H2EvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
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
    if (hostId == H2_EVAL_MIR_HOST_COPY && argCount == 2u) {
        int copyRc = H2EvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
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
    if (hostId == H2_EVAL_MIR_HOST_FREE && (argCount == 1u || argCount == 2u)) {
        H2EvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == H2_EVAL_MIR_HOST_PLATFORM_EXIT && argCount == 1u) {
        int64_t exitCode = 0;
        if (H2CTFEValueToInt64(&args[0], &exitCode) != 0) {
            *outIsConst = 0;
            return 0;
        }
        p->exitCalled = 1;
        p->exitCode = (int)(exitCode & 255);
        H2EvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (hostId == H2_EVAL_MIR_HOST_PLATFORM_CONSOLE_LOG && argCount == 2u) {
        int64_t flags = 0;
        if (args[0].kind != H2CTFEValue_STRING || H2CTFEValueToInt64(&args[1], &flags) != 0) {
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
        H2EvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    *outIsConst = 0;
    return 0;
}

static void H2EvalMirSetReason(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* _Nonnull reason) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    if (p == NULL || p->currentExecCtx == NULL || reason[0] == '\0') {
        return;
    }
    H2CTFEExecSetReason(p->currentExecCtx, start, end, reason);
}

static int H2EvalMirEnterFunction(
    void* ctx, uint32_t functionIndex, uint32_t sourceRef, H2Diag* _Nullable diag) {
    H2EvalMirExecCtx* c = (H2EvalMirExecCtx*)ctx;
    uint8_t           pushed = 0;
    uint32_t          frameIndex;
    if (c == NULL || c->p == NULL || sourceRef >= c->sourceFileCap
        || c->savedFileLen >= H2_EVAL_CALL_MAX_DEPTH)
    {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
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
                diag->code = H2Diag_UNEXPECTED_TOKEN;
                diag->type = H2DiagTypeOfCode(diag->code);
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
        } else if (evalFnIndex >= c->p->funcLen || c->p->callDepth >= H2_EVAL_CALL_MAX_DEPTH) {
            if (diag != NULL) {
                diag->code = H2Diag_UNEXPECTED_TOKEN;
                diag->type = H2DiagTypeOfCode(diag->code);
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

static void H2EvalMirLeaveFunction(void* ctx) {
    H2EvalMirExecCtx* c = (H2EvalMirExecCtx*)ctx;
    uint32_t          frameIndex;
    if (c == NULL || c->p == NULL || c->savedFileLen == 0) {
        return;
    }
    frameIndex = c->savedFileLen - 1u;
    if (c->pushedFrames[frameIndex] && c->p->callDepth > 0) {
        c->p->callDepth--;
    }
    if (c->restoresTemplateBinding[frameIndex]) {
        H2EvalRestoreTemplateBinding(c->p, &c->savedTemplateBindings[frameIndex]);
        c->restoresTemplateBinding[frameIndex] = 0u;
    }
    c->p->currentMirExecCtx = c->savedMirExecCtxs[c->savedFileLen - 1u];
    c->p->currentFile = c->savedFiles[--c->savedFileLen];
}

static int H2EvalMirBindFrame(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2CTFEValue* _Nullable locals,
    uint32_t localCount,
    H2Diag* _Nullable diag) {
    H2EvalMirExecCtx* c = (H2EvalMirExecCtx*)ctx;
    if (c == NULL || c->mirFrameDepth >= H2_EVAL_CALL_MAX_DEPTH) {
        if (diag != NULL) {
            diag->code = H2Diag_UNEXPECTED_TOKEN;
            diag->type = H2DiagTypeOfCode(diag->code);
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

static void H2EvalMirUnbindFrame(void* _Nullable ctx) {
    H2EvalMirExecCtx* c = (H2EvalMirExecCtx*)ctx;
    if (c == NULL || c->mirFrameDepth == 0) {
        return;
    }
    c->mirFrameDepth--;
    c->mirProgram = c->savedMirPrograms[c->mirFrameDepth];
    c->mirFunction = c->savedMirFunctions[c->mirFrameDepth];
    c->mirLocals = c->savedMirLocals[c->mirFrameDepth];
    c->mirLocalCount = c->savedMirLocalCounts[c->mirFrameDepth];
}

static int H2EvalMirResolveCallNode(
    const H2MirProgram* _Nullable program,
    const H2MirInst* _Nullable inst,
    int32_t* _Nonnull outCallNode) {
    const H2MirSymbolRef* symbol;
    *outCallNode = -1;
    if (program == NULL || inst == NULL || inst->op != H2MirOp_CALL
        || inst->aux >= program->symbolLen)
    {
        return 0;
    }
    symbol = &program->symbols[inst->aux];
    if (symbol->kind != H2MirSymbol_CALL || symbol->target == UINT32_MAX) {
        return 0;
    }
    *outCallNode = (int32_t)symbol->target;
    return 1;
}

static int H2EvalMirCallNodeIsLazyBuiltin(H2EvalProgram* p, int32_t callNode) {
    const H2Ast*     ast;
    const H2AstNode* call;
    const H2AstNode* callee;
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
    if (call->kind != H2Ast_CALL || callee == NULL) {
        return 0;
    }
    if (callee->kind == H2Ast_IDENT) {
        return H2EvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source,
                   callee->dataStart,
                   callee->dataEnd,
                   "source_location_of",
                   "builtin")
            || H2EvalNameIsLazyTypeBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd)
            || H2EvalNameIsCompilerDiagBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd);
    }
    if (callee->kind != H2Ast_FIELD_EXPR) {
        return 0;
    }
    recvNode = callee->firstChild;
    if (recvNode < 0 || (uint32_t)recvNode >= ast->len || ast->nodes[recvNode].kind != H2Ast_IDENT)
    {
        return 0;
    }
    if (SliceEqCStr(
            p->currentFile->source,
            ast->nodes[recvNode].dataStart,
            ast->nodes[recvNode].dataEnd,
            "builtin"))
    {
        return H2EvalNameEqLiteralOrPkgBuiltin(
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
        return H2EvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "kind", "reflect")
            || H2EvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source, callee->dataStart, callee->dataEnd, "base", "reflect")
            || H2EvalNameEqLiteralOrPkgBuiltin(
                   p->currentFile->source,
                   callee->dataStart,
                   callee->dataEnd,
                   "is_alias",
                   "reflect")
            || H2EvalNameEqLiteralOrPkgBuiltin(
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
    return H2EvalNameIsCompilerDiagBuiltin(
        p->currentFile->source, callee->dataStart, callee->dataEnd);
}

static const H2Package* _Nullable H2EvalMirResolveQualifiedImportCallTargetPkg(
    H2EvalProgram* p, int32_t callNode) {
    const H2Package* currentPkg;
    const H2Ast*     ast;
    const H2AstNode* call;
    const H2AstNode* callee;
    const H2AstNode* base;
    uint32_t         i;
    int32_t          calleeNode;
    int32_t          baseNode;
    if (p == NULL || p->currentFile == NULL) {
        return NULL;
    }
    currentPkg = H2EvalFindPackageByFile(p, p->currentFile);
    ast = &p->currentFile->ast;
    if (currentPkg == NULL || callNode < 0 || (uint32_t)callNode >= ast->len) {
        return NULL;
    }
    call = &ast->nodes[callNode];
    calleeNode = call->firstChild;
    callee = calleeNode >= 0 ? &ast->nodes[calleeNode] : NULL;
    baseNode = calleeNode >= 0 ? ast->nodes[calleeNode].firstChild : -1;
    base = baseNode >= 0 ? &ast->nodes[baseNode] : NULL;
    if (call->kind != H2Ast_CALL || callee == NULL || callee->kind != H2Ast_FIELD_EXPR
        || base == NULL || base->kind != H2Ast_IDENT)
    {
        return NULL;
    }
    for (i = 0; i < currentPkg->importLen; i++) {
        const H2ImportRef* imp = &currentPkg->imports[i];
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

static int H2EvalMirLookupLocalValue(
    H2EvalProgram* p, uint32_t nameStart, uint32_t nameEnd, H2CTFEValue* outValue) {
    H2EvalMirExecCtx*   c;
    const H2ParsedFile* file;
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
                const H2MirLocal* local =
                    &c->mirProgram->locals[c->mirFunction->localStart + i - 1u];
                const H2CTFEValue* value = &c->mirLocals[i - 1u];
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
                if (value->kind == H2CTFEValue_INVALID) {
                    continue;
                }
                *outValue = *value;
                if (local->typeRef != UINT32_MAX && local->typeRef < c->mirProgram->typeLen) {
                    int32_t typeNode = (int32_t)c->mirProgram->types[local->typeRef].astNode;
                    int32_t typeCode = H2EvalTypeCode_INVALID;
                    if (!H2EvalValueGetRuntimeTypeCode(outValue, &typeCode) && typeNode >= 0
                        && H2EvalTypeCodeFromTypeNode(file, typeNode, &typeCode))
                    {
                        H2EvalValueSetRuntimeTypeCode(outValue, typeCode);
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

static int H2EvalMirLookupLocalTypeNode(
    H2EvalProgram*       p,
    uint32_t             nameStart,
    uint32_t             nameEnd,
    const H2ParsedFile** outFile,
    int32_t*             outTypeNode) {
    H2EvalMirExecCtx*   c;
    const H2ParsedFile* file;
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
                const H2MirLocal* local =
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

static void H2EvalMirInitExecEnv(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    H2MirExecEnv*       env,
    H2EvalMirExecCtx* _Nullable functionCtx) {
    if (p == NULL || file == NULL || env == NULL) {
        return;
    }
    memset(env, 0, sizeof(*env));
    env->src.ptr = file->source;
    env->src.len = file->sourceLen;
    env->resolveIdent = H2EvalResolveIdent;
    env->assignIdent = H2EvalMirAssignIdent;
    env->assignIdentCtx = p;
    env->resolveCallPre = H2EvalResolveCallMirPre;
    env->resolveCall = H2EvalResolveCallMir;
    env->adjustCallArgs = H2EvalMirAdjustCallArgs;
    env->resolveCtx = p;
    env->adjustCallArgsCtx = p;
    env->hostCall = H2EvalMirHostCall;
    env->hostCtx = p;
    env->zeroInitLocal = H2EvalMirZeroInitLocal;
    env->zeroInitCtx = p;
    env->coerceValueForType = H2EvalMirCoerceValueForType;
    env->coerceValueCtx = p;
    env->indexValue = H2EvalMirIndexValue;
    env->indexValueCtx = p;
    env->indexAddr = H2EvalMirIndexAddr;
    env->indexAddrCtx = p;
    env->sliceValue = H2EvalMirSliceValue;
    env->sliceValueCtx = p;
    env->sequenceLen = H2EvalMirSequenceLen;
    env->sequenceLenCtx = p;
    env->iterInit = H2EvalMirIterInit;
    env->iterInitCtx = p;
    env->iterNext = H2EvalMirIterNext;
    env->iterNextCtx = p;
    env->aggGetField = H2EvalMirAggGetField;
    env->aggGetFieldCtx = p;
    env->aggAddrField = H2EvalMirAggAddrField;
    env->aggAddrFieldCtx = p;
    env->aggSetField = H2EvalMirAggSetField;
    env->aggSetFieldCtx = p;
    env->makeAggregate = H2EvalMirMakeAggregate;
    env->makeAggregateCtx = p;
    env->makeTuple = H2EvalMirMakeTuple;
    env->makeTupleCtx = p;
    env->makeVariadicPack = H2EvalMirMakeVariadicPack;
    env->makeVariadicPackCtx = p;
    env->evalBinary = H2EvalMirEvalBinary;
    env->evalBinaryCtx = p;
    env->allocNew = H2EvalMirAllocNew;
    env->allocNewCtx = p;
    env->contextGet = H2EvalMirContextGet;
    env->contextGetCtx = p;
    env->contextAddr = H2EvalMirContextAddr;
    env->contextAddrCtx = p;
    env->evalWithContext = H2EvalMirEvalWithContext;
    env->evalWithContextCtx = p;
    env->setReason = H2EvalMirSetReason;
    env->setReasonCtx = p;
    env->backwardJumpLimit = p->currentExecCtx != NULL ? p->currentExecCtx->forIterLimit : 0;
    env->diag = p->currentExecCtx != NULL ? p->currentExecCtx->diag : NULL;
    if (functionCtx != NULL) {
        env->enterFunction = H2EvalMirEnterFunction;
        env->leaveFunction = H2EvalMirLeaveFunction;
        env->functionCtx = functionCtx;
        env->bindFrame = H2EvalMirBindFrame;
        env->unbindFrame = H2EvalMirUnbindFrame;
        env->frameCtx = functionCtx;
    }
}

static int H2EvalMirEvalBinary(
    void* _Nullable ctx,
    H2TokenKind           op,
    const H2MirExecValue* lhs,
    const H2MirExecValue* rhs,
    H2MirExecValue*       outValue,
    int*                  outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    (void)diag;
    return H2EvalEvalBinary(p, op, lhs, rhs, outValue, outIsConst);
}

static int H2EvalTryMirZeroInitType(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    int32_t             typeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    H2CTFEValue*        outValue,
    int*                outIsConst) {
    return H2EvalTryMirEvalTopInit(
        p, file, -1, typeNode, nameStart, nameEnd, NULL, -1, outValue, outIsConst, NULL);
}

static int H2EvalTryMirEvalTopInit(
    H2EvalProgram*      p,
    const H2ParsedFile* file,
    int32_t             initExprNode,
    int32_t             declTypeNode,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const H2ParsedFile* _Nullable coerceTypeFile,
    int32_t      coerceTypeNode,
    H2CTFEValue* outValue,
    int*         outIsConst,
    int* _Nullable outSupported) {
    H2MirProgram     program = { 0 };
    H2MirExecEnv     env = { 0 };
    H2EvalMirExecCtx functionCtx = { 0 };
    uint32_t         rootMirFnIndex = UINT32_MAX;
    int              supported = 0;
    if (outSupported != NULL) {
        *outSupported = 0;
    }
    if (p == NULL || file == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (H2EvalMirBuildTopInitProgram(
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
    H2EvalMirInitExecEnv(p, file, &env, &functionCtx);
    if (!H2MirProgramNeedsDynamicResolution(&program)) {
        H2MirExecEnvDisableDynamicResolution(&env);
    }
    if (H2MirEvalFunction(p->arena, &program, rootMirFnIndex, NULL, 0, &env, outValue, outIsConst)
        != 0)
    {
        return -1;
    }
    H2EvalMirAdaptOutValue(&functionCtx, outValue, outIsConst);
    if (*outIsConst && coerceTypeFile != NULL && coerceTypeNode >= 0
        && H2EvalAdaptStringValueForType(
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

static int H2EvalTryMirEvalExprWithType(
    H2EvalProgram*      p,
    int32_t             exprNode,
    const H2ParsedFile* exprFile,
    uint32_t            nameStart,
    uint32_t            nameEnd,
    const H2ParsedFile* _Nullable typeFile,
    int32_t      typeNode,
    H2CTFEValue* outValue,
    int*         outIsConst,
    int*         outSupported) {
    return H2EvalTryMirEvalTopInit(
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

static int H2EvalValueNeedsDefaultFieldEval(const H2CTFEValue* value) {
    H2EvalAggregate* agg;
    uint32_t         i;
    if (value == NULL) {
        return 0;
    }
    agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(value));
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

static int H2EvalFinalizeNewAggregateDefaults(H2EvalProgram* p, H2CTFEValue* inOutValue) {
    H2EvalAggregate*   agg = H2EvalValueAsAggregate(inOutValue);
    H2CTFEExecBinding* fieldBindings = NULL;
    H2CTFEExecEnv      fieldFrame;
    uint32_t           fieldBindingCap = 0;
    uint32_t           i;
    if (p == NULL || inOutValue == NULL || agg == NULL
        || !H2EvalValueNeedsDefaultFieldEval(inOutValue))
    {
        return 1;
    }
    if (agg->fieldLen > 0) {
        fieldBindingCap = H2EvalAggregateFieldBindingCount(agg);
        fieldBindings = (H2CTFEExecBinding*)H2ArenaAlloc(
            p->arena,
            sizeof(H2CTFEExecBinding) * fieldBindingCap,
            (uint32_t)_Alignof(H2CTFEExecBinding));
        if (fieldBindings == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(fieldBindings, 0, sizeof(H2CTFEExecBinding) * fieldBindingCap);
    }
    fieldFrame.parent = p->currentExecCtx != NULL ? p->currentExecCtx->env : NULL;
    fieldFrame.bindings = fieldBindings;
    fieldFrame.bindingLen = 0;
    for (i = 0; i < agg->fieldLen; i++) {
        H2EvalAggregateField* field = &agg->fields[i];
        if (field->defaultExprNode >= 0) {
            H2CTFEValue defaultValue;
            int         defaultIsConst = 0;
            if (H2EvalExecExprInFileWithType(
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
            && H2EvalAppendAggregateFieldBindings(
                   fieldBindings, fieldBindingCap, &fieldFrame, field)
                   != 0)
        {
            return ErrorSimple("out of memory");
        }
    }
    return 1;
}

static int H2EvalMirAllocNew(
    void* _Nullable ctx,
    uint32_t        sourceNode,
    H2MirExecValue* outValue,
    int*            outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*      p = (H2EvalProgram*)ctx;
    const H2ParsedFile* newFile;
    int32_t             exprNode = (int32_t)sourceNode;
    int32_t             typeNode = -1;
    int32_t             countNode = -1;
    int32_t             initNode = -1;
    int32_t             allocNode = -1;
    H2CTFEValue         allocValue;
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
    if (!H2EvalDecodeNewExprNodes(newFile, exprNode, &typeNode, &countNode, &initNode, &allocNode))
    {
        return 0;
    }
    if (allocNode >= 0) {
        if (H2EvalTryMirEvalExprWithType(
                p, allocNode, newFile, 0, 0, NULL, -1, &allocValue, &allocIsConst, &allocSupported)
            != 0)
        {
            return -1;
        }
        if (!allocSupported || !allocIsConst) {
            return 0;
        }
    } else if (!H2EvalCurrentContextFieldByLiteral(p, "allocator", &allocValue)) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReasonNode(
                p->currentExecCtx,
                exprNode,
                "allocator context is not available in evaluator backend");
        }
        return 0;
    }
    if (H2EvalValueTargetOrSelf(&allocValue)->kind == H2CTFEValue_NULL) {
        if (p->currentExecCtx != NULL) {
            H2CTFEExecSetReasonNode(p->currentExecCtx, exprNode, "invalid allocator");
        }
        return 0;
    }
    {
        const H2ParsedFile* expectedTypeFile = NULL;
        int32_t             expectedTypeNode = -1;
        int                 allocReturnedNull = 0;
        int allocRc = H2EvalCheckAllocatorImplResult(p, exprNode, &allocValue, &allocReturnedNull);
        if (allocRc <= 0) {
            if (allocRc == 0 && allocReturnedNull
                && H2EvalFindExpectedTypeForInitExpr(
                    newFile, exprNode, &expectedTypeFile, &expectedTypeNode)
                && H2EvalExpectedNewResultIsOptional(expectedTypeFile, expectedTypeNode))
            {
                H2EvalValueSetNull(outValue);
                *outIsConst = 1;
                return 1;
            }
            if (allocRc == 0 && allocReturnedNull && p->currentExecCtx != NULL) {
                H2CTFEExecSetReasonNode(p->currentExecCtx, exprNode, "allocator returned null");
            }
            return allocRc;
        }
    }
    if (countNode >= 0) {
        H2CTFEValue  countValue;
        H2CTFEValue  arrayValue;
        H2EvalArray* array;
        int          countIsConst = 0;
        int          countSupported = 0;
        int64_t      count = 0;
        uint32_t     i;
        if (H2EvalTryMirEvalExprWithType(
                p, countNode, newFile, 0, 0, NULL, -1, &countValue, &countIsConst, &countSupported)
            != 0)
        {
            return -1;
        }
        if (!countSupported || !countIsConst || H2CTFEValueToInt64(&countValue, &count) != 0
            || count < 0)
        {
            return 0;
        }
        array = (H2EvalArray*)H2ArenaAlloc(
            p->arena, sizeof(H2EvalArray), (uint32_t)_Alignof(H2EvalArray));
        if (array == NULL) {
            return ErrorSimple("out of memory");
        }
        memset(array, 0, sizeof(*array));
        array->file = newFile;
        array->typeNode = exprNode;
        array->elemTypeNode = typeNode;
        array->len = (uint32_t)count;
        if (array->len > 0) {
            array->elems = (H2CTFEValue*)H2ArenaAlloc(
                p->arena, sizeof(H2CTFEValue) * array->len, (uint32_t)_Alignof(H2CTFEValue));
            if (array->elems == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(array->elems, 0, sizeof(H2CTFEValue) * array->len);
            if (initNode >= 0) {
                H2CTFEValue initValue;
                int         initIsConst = 0;
                if (H2EvalExecExprWithTypeNode(
                        p, initNode, newFile, typeNode, &initValue, &initIsConst)
                    != 0)
                {
                    return -1;
                }
                if (!initIsConst) {
                    return 0;
                }
                if (H2EvalCoerceValueToTypeNode(p, newFile, typeNode, &initValue) != 0) {
                    return -1;
                }
                for (i = 0; i < array->len; i++) {
                    array->elems[i] = initValue;
                }
            } else {
                for (i = 0; i < array->len; i++) {
                    int elemIsConst = 0;
                    if (H2EvalZeroInitTypeNode(p, newFile, typeNode, &array->elems[i], &elemIsConst)
                        != 0)
                    {
                        return -1;
                    }
                    if (!elemIsConst) {
                        return 0;
                    }
                    {
                        int finalizeRc = H2EvalFinalizeNewAggregateDefaults(p, &array->elems[i]);
                        if (finalizeRc <= 0) {
                            return finalizeRc < 0 ? -1 : 0;
                        }
                    }
                }
            }
        }
        H2EvalValueSetArray(&arrayValue, newFile, exprNode, array);
        return H2EvalAllocReferencedValue(p, &arrayValue, outValue, outIsConst) == 0 ? 1 : -1;
    }
    {
        H2CTFEValue value;
        int         valueIsConst = 0;
        if (initNode >= 0) {
            if (H2EvalExecExprWithTypeNode(p, initNode, newFile, typeNode, &value, &valueIsConst)
                != 0)
            {
                return -1;
            }
            if (!valueIsConst) {
                return 0;
            }
        } else {
            if (H2EvalZeroInitTypeNode(p, newFile, typeNode, &value, &valueIsConst) != 0) {
                return -1;
            }
            if (!valueIsConst) {
                return 0;
            }
        }
        if (initNode >= 0) {
            if (H2EvalCoerceValueToTypeNode(p, newFile, typeNode, &value) != 0) {
                return -1;
            }
        } else {
            int finalizeRc = H2EvalFinalizeNewAggregateDefaults(p, &value);
            if (finalizeRc <= 0) {
                return finalizeRc < 0 ? -1 : 0;
            }
        }
        return H2EvalAllocReferencedValue(p, &value, outValue, outIsConst) == 0 ? 1 : -1;
    }
}

static int H2EvalBinaryOpForAssignToken(H2TokenKind assignOp, H2TokenKind* outBinaryOp) {
    if (outBinaryOp == NULL) {
        return 0;
    }
    switch (assignOp) {
        case H2Tok_ADD_ASSIGN: *outBinaryOp = H2Tok_ADD; return 1;
        case H2Tok_SUB_ASSIGN: *outBinaryOp = H2Tok_SUB; return 1;
        case H2Tok_MUL_ASSIGN: *outBinaryOp = H2Tok_MUL; return 1;
        case H2Tok_DIV_ASSIGN: *outBinaryOp = H2Tok_DIV; return 1;
        case H2Tok_MOD_ASSIGN: *outBinaryOp = H2Tok_MOD; return 1;
        default:               *outBinaryOp = H2Tok_INVALID; return 0;
    }
}

static int H2EvalAssignExprCb(
    void* ctx, H2CTFEExecCtx* execCtx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2EvalProgram*   p = (H2EvalProgram*)ctx;
    const H2Ast*     ast;
    const H2AstNode* expr;
    int32_t          lhsNode;
    int32_t          rhsNode;
    H2CTFEValue      rhsValue;
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
    if (expr->kind != H2Ast_BINARY || lhsNode < 0 || rhsNode < 0
        || ast->nodes[rhsNode].nextSibling >= 0)
    {
        *outIsConst = 0;
        return 0;
    }
    if (ast->nodes[lhsNode].kind == H2Ast_IDENT) {
        if (SliceEqCStr(
                p->currentFile->source,
                ast->nodes[lhsNode].dataStart,
                ast->nodes[lhsNode].dataEnd,
                "_"))
        {
            if (H2EvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
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
        int32_t topVarIndex = H2EvalFindCurrentTopVarBySlice(
            p, p->currentFile, ast->nodes[lhsNode].dataStart, ast->nodes[lhsNode].dataEnd);
        if (topVarIndex >= 0) {
            H2EvalTopVar* topVar = &p->topVars[(uint32_t)topVarIndex];
            if ((H2TokenKind)expr->op == H2Tok_ASSIGN) {
                if (topVar->declTypeNode >= 0) {
                    if (H2EvalExecExprWithTypeNode(
                            p, rhsNode, topVar->file, topVar->declTypeNode, &rhsValue, &rhsIsConst)
                        != 0)
                    {
                        return -1;
                    }
                } else if (H2EvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
                    return -1;
                }
                if (!rhsIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                topVar->value = rhsValue;
                topVar->state = H2EvalTopConstState_READY;
                *outValue = rhsValue;
                *outIsConst = 1;
                return 0;
            }
        }
    }
    if (ast->nodes[lhsNode].kind == H2Ast_INDEX && (ast->nodes[lhsNode].flags & 0x7u) == 0u) {
        int32_t      baseNode = ast->nodes[lhsNode].firstChild;
        int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        H2CTFEValue  baseValue;
        H2CTFEValue  indexValue;
        H2EvalArray* array;
        H2TokenKind  binaryOp = H2Tok_INVALID;
        int          baseIsConst = 0;
        int          indexIsConst = 0;
        int64_t      index = 0;
        if (H2EvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (!rhsIsConst || baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0)
        {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || H2EvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || H2CTFEValueToInt64(&indexValue, &index) != 0) {
            *outIsConst = 0;
            return 0;
        }
        array = H2EvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            if ((H2TokenKind)expr->op != H2Tok_ASSIGN) {
                int handled;
                if (!H2EvalBinaryOpForAssignToken((H2TokenKind)expr->op, &binaryOp)) {
                    *outIsConst = 0;
                    return 0;
                }
                handled = H2EvalEvalBinary(
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
            H2CTFEValue* targetValue = H2EvalValueReferenceTarget(&baseValue);
            int32_t      baseTypeCode = H2EvalTypeCode_INVALID;
            int64_t      byteValue = 0;
            if (targetValue == NULL) {
                targetValue = (H2CTFEValue*)H2EvalValueTargetOrSelf(&baseValue);
            }
            if (targetValue != NULL && targetValue->kind == H2CTFEValue_STRING
                && H2EvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
                && baseTypeCode == H2EvalTypeCode_STR_PTR && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len
                && H2CTFEValueToInt64(&rhsValue, &byteValue) == 0 && byteValue >= 0
                && byteValue <= 255)
            {
                ((uint8_t*)targetValue->s.bytes)[(uint32_t)index] = (uint8_t)byteValue;
                H2EvalValueSetInt(outValue, byteValue);
                H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_U8);
                *outIsConst = 1;
                return 0;
            }
        }
        *outIsConst = 0;
        return 0;
    }
    if (ast->nodes[lhsNode].kind == H2Ast_UNARY && (H2TokenKind)ast->nodes[lhsNode].op == H2Tok_MUL)
    {
        int32_t      refNode = ast->nodes[lhsNode].firstChild;
        H2CTFEValue  refValue;
        H2CTFEValue* target;
        H2TokenKind  binaryOp = H2Tok_INVALID;
        int          refIsConst = 0;
        if (H2EvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
            return -1;
        }
        if (!rhsIsConst || refNode < 0) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, refNode, &refValue, &refIsConst) != 0) {
            return -1;
        }
        if (!refIsConst) {
            *outIsConst = 0;
            return 0;
        }
        target = H2EvalValueReferenceTarget(&refValue);
        if (target == NULL) {
            *outIsConst = 0;
            return 0;
        }
        if ((H2TokenKind)expr->op != H2Tok_ASSIGN) {
            int handled;
            if (!H2EvalBinaryOpForAssignToken((H2TokenKind)expr->op, &binaryOp)) {
                *outIsConst = 0;
                return 0;
            }
            handled = H2EvalEvalBinary(p, binaryOp, target, &rhsValue, &rhsValue, outIsConst);
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
    if (ast->nodes[lhsNode].kind != H2Ast_FIELD_EXPR) {
        *outIsConst = 0;
        return 0;
    }
    if (H2EvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0) {
        return -1;
    }
    if (!rhsIsConst) {
        *outIsConst = 0;
        return 0;
    }
    {
        int32_t            curNode = lhsNode;
        H2CTFEExecBinding* binding = NULL;
        H2EvalAggregate*   agg = NULL;
        while (ast->nodes[curNode].kind == H2Ast_FIELD_EXPR) {
            int32_t baseNode = ast->nodes[curNode].firstChild;
            if (baseNode < 0) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[baseNode].kind == H2Ast_IDENT) {
                int allowIndirectMutation = 0;
                binding = H2EvalFindBinding(
                    execCtx,
                    p->currentFile,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd);
                if (binding == NULL) {
                    *outIsConst = 0;
                    return 0;
                }
                agg = H2EvalValueAsAggregate(&binding->value);
                if (agg != NULL) {
                    allowIndirectMutation = 0;
                } else {
                    agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(&binding->value));
                    allowIndirectMutation = agg != NULL;
                }
                if (!binding->mutable && !allowIndirectMutation) {
                    *outIsConst = 0;
                    return 0;
                }
                break;
            }
            if (ast->nodes[baseNode].kind == H2Ast_FIELD_EXPR) {
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
            while (curNode >= 0 && ast->nodes[curNode].kind == H2Ast_FIELD_EXPR) {
                if (pathLen >= 16u) {
                    *outIsConst = 0;
                    return 0;
                }
                pathNodes[pathLen++] = curNode;
                curNode = ast->nodes[curNode].firstChild;
            }
            while (pathLen > 0) {
                const H2AstNode* fieldNode;
                pathLen--;
                fieldNode = &ast->nodes[pathNodes[pathLen]];
                if (pathLen == 0) {
                    if ((H2TokenKind)expr->op != H2Tok_ASSIGN) {
                        H2CTFEValue curValue;
                        H2TokenKind binaryOp = H2Tok_INVALID;
                        int         handled = 0;
                        if (!H2EvalAggregateGetFieldValue(
                                agg,
                                p->currentFile->source,
                                fieldNode->dataStart,
                                fieldNode->dataEnd,
                                &curValue))
                        {
                            *outIsConst = 0;
                            return 0;
                        }
                        if (!H2EvalBinaryOpForAssignToken((H2TokenKind)expr->op, &binaryOp)) {
                            *outIsConst = 0;
                            return 0;
                        }
                        handled = H2EvalEvalBinary(
                            p, binaryOp, &curValue, &rhsValue, &rhsValue, outIsConst);
                        if (handled <= 0 || !*outIsConst) {
                            *outIsConst = 0;
                            return handled < 0 ? -1 : 0;
                        }
                    }
                    if (H2EvalAggregateSetFieldValue(
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
                    H2CTFEValue nestedValue;
                    if (!H2EvalAggregateGetFieldValue(
                            agg,
                            p->currentFile->source,
                            fieldNode->dataStart,
                            fieldNode->dataEnd,
                            &nestedValue))
                    {
                        *outIsConst = 0;
                        return 0;
                    }
                    agg = H2EvalValueAsAggregate(&nestedValue);
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

static H2CTFEValue* _Nullable H2EvalResolveFieldExprValuePtr(
    H2EvalProgram* p, H2CTFEExecCtx* execCtx, int32_t fieldExprNode) {
    const H2Ast*     ast;
    const H2AstNode* fieldExpr;
    int32_t          baseNode;
    H2EvalAggregate* agg = NULL;
    if (p == NULL || p->currentFile == NULL || execCtx == NULL || fieldExprNode < 0) {
        return NULL;
    }
    ast = &p->currentFile->ast;
    if ((uint32_t)fieldExprNode >= ast->len) {
        return NULL;
    }
    fieldExpr = &ast->nodes[fieldExprNode];
    if (fieldExpr->kind != H2Ast_FIELD_EXPR) {
        return NULL;
    }
    baseNode = fieldExpr->firstChild;
    if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
        return NULL;
    }
    if (ast->nodes[baseNode].kind == H2Ast_IDENT) {
        H2CTFEExecBinding* binding = H2EvalFindBinding(
            execCtx, p->currentFile, ast->nodes[baseNode].dataStart, ast->nodes[baseNode].dataEnd);
        if (binding == NULL) {
            return NULL;
        }
        agg = H2EvalValueAsAggregate(&binding->value);
        if (agg == NULL) {
            agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(&binding->value));
        }
    } else if (ast->nodes[baseNode].kind == H2Ast_FIELD_EXPR) {
        H2CTFEValue* baseValue = H2EvalResolveFieldExprValuePtr(p, execCtx, baseNode);
        if (baseValue == NULL) {
            return NULL;
        }
        agg = H2EvalValueAsAggregate(baseValue);
        if (agg == NULL) {
            agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(baseValue));
        }
    }
    if (agg == NULL) {
        return NULL;
    }
    return H2EvalAggregateLookupFieldValuePtr(
        agg, p->currentFile->source, fieldExpr->dataStart, fieldExpr->dataEnd);
}

static int H2EvalAssignValueExprCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    int32_t            lhsExprNode,
    const H2CTFEValue* inValue,
    H2CTFEValue*       outValue,
    int*               outIsConst) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    const H2Ast*   ast;
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
    if (ast->nodes[lhsExprNode].kind == H2Ast_IDENT) {
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
        int32_t topVarIndex = H2EvalFindCurrentTopVarBySlice(
            p, p->currentFile, ast->nodes[lhsExprNode].dataStart, ast->nodes[lhsExprNode].dataEnd);
        if (topVarIndex >= 0) {
            H2EvalTopVar* topVar = &p->topVars[(uint32_t)topVarIndex];
            topVar->value = *inValue;
            topVar->state = H2EvalTopConstState_READY;
            *outValue = *inValue;
            *outIsConst = 1;
            return 0;
        }
    }
    if (ast->nodes[lhsExprNode].kind == H2Ast_INDEX && (ast->nodes[lhsExprNode].flags & 0x7u) == 0u)
    {
        int32_t      baseNode = ast->nodes[lhsExprNode].firstChild;
        int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        H2CTFEValue  baseValue;
        H2CTFEValue  indexValue;
        H2EvalArray* array;
        int          baseIsConst = 0;
        int          indexIsConst = 0;
        int64_t      index = 0;
        if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || H2EvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || H2CTFEValueToInt64(&indexValue, &index) != 0) {
            *outIsConst = 0;
            return 0;
        }
        array = H2EvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            array->elems[(uint32_t)index] = *inValue;
            *outValue = *inValue;
            *outIsConst = 1;
            return 0;
        }
        {
            H2CTFEValue* targetValue = H2EvalValueReferenceTarget(&baseValue);
            int32_t      baseTypeCode = H2EvalTypeCode_INVALID;
            int64_t      byteValue = 0;
            if (targetValue == NULL) {
                targetValue = (H2CTFEValue*)H2EvalValueTargetOrSelf(&baseValue);
            }
            if (targetValue != NULL && targetValue->kind == H2CTFEValue_STRING
                && H2EvalValueGetRuntimeTypeCode(targetValue, &baseTypeCode)
                && baseTypeCode == H2EvalTypeCode_STR_PTR && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len
                && H2CTFEValueToInt64(inValue, &byteValue) == 0 && byteValue >= 0
                && byteValue <= 255)
            {
                ((uint8_t*)targetValue->s.bytes)[(uint32_t)index] = (uint8_t)byteValue;
                H2EvalValueSetInt(outValue, byteValue);
                H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_U8);
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

static int H2EvalMatchPatternCb(
    void*              ctx,
    H2CTFEExecCtx*     execCtx,
    const H2CTFEValue* subjectValue,
    int32_t            labelExprNode,
    int*               outMatched) {
    H2EvalProgram*      p = (H2EvalProgram*)ctx;
    H2EvalTaggedEnum*   tagged;
    const H2AstNode*    labelNode;
    const H2ParsedFile* enumFile = NULL;
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
    tagged = H2EvalValueAsTaggedEnum(H2EvalValueTargetOrSelf(subjectValue));
    if (tagged == NULL) {
        return 0;
    }
    labelNode = &p->currentFile->ast.nodes[labelExprNode];
    if (labelNode->kind != H2Ast_FIELD_EXPR || labelNode->firstChild < 0
        || p->currentFile->ast.nodes[labelNode->firstChild].kind != H2Ast_IDENT)
    {
        return 0;
    }
    enumNode = H2EvalFindNamedEnumDecl(
        p,
        p->currentFile,
        p->currentFile->ast.nodes[labelNode->firstChild].dataStart,
        p->currentFile->ast.nodes[labelNode->firstChild].dataEnd,
        &enumFile);
    if (enumNode < 0 || enumFile == NULL
        || !H2EvalFindEnumVariant(
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
    H2EvalProgram*      p;
    const H2ParsedFile* file;
} H2EvalMirLowerConstCtx;

static int H2EvalMirLowerConstExpr(
    void* _Nullable ctx, int32_t exprNode, H2MirConst* _Nonnull outValue, H2Diag* _Nullable diag) {
    H2EvalMirLowerConstCtx* lowerCtx = (H2EvalMirLowerConstCtx*)ctx;
    H2CTFEValue             value;
    int32_t                 typeNode;
    int                     rc;
    (void)diag;
    if (lowerCtx == NULL || lowerCtx->p == NULL || lowerCtx->file == NULL || outValue == NULL
        || exprNode < 0 || (uint32_t)exprNode >= lowerCtx->file->ast.len)
    {
        return -1;
    }
    if (lowerCtx->file->ast.nodes[exprNode].kind != H2Ast_TYPE_VALUE) {
        return 0;
    }
    typeNode = ASTFirstChild(&lowerCtx->file->ast, exprNode);
    rc = H2EvalTypeValueFromTypeNode(lowerCtx->p, lowerCtx->file, typeNode, &value);
    if (rc < 0) {
        return -1;
    }
    if (rc == 0 || value.kind != H2CTFEValue_TYPE) {
        return 0;
    }
    outValue->kind = H2MirConst_TYPE;
    outValue->bits = value.typeTag;
    outValue->bytes.ptr = (const char*)value.s.bytes;
    outValue->bytes.len = value.s.len;
    return 1;
}

H2_API_END
#include "evaluator_mir.inc"
H2_API_BEGIN

static int H2EvalInvokeFunction(
    H2EvalProgram* p,
    int32_t        fnIndex,
    const H2CTFEValue* _Nullable args,
    uint32_t             argCount,
    const H2EvalContext* callContext,
    H2CTFEValue*         outValue,
    int*                 outDidReturn) {
    const H2EvalFunction* fn;
    const H2Ast*          ast;
    H2CTFEExecBinding*    paramBindings = NULL;
    H2CTFEExecEnv         paramFrame;
    H2CTFEExecCtx         execCtx;
    const H2ParsedFile*   savedFile;
    H2CTFEExecCtx*        savedExecCtx;
    const H2EvalContext*  savedContext;
    int                   isConst = 0;
    int                   rc;
    int32_t               child;
    uint32_t              paramIndex = 0;
    uint32_t              fixedCount = 0;

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
        concatRc = H2EvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
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
        copyRc = H2EvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
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
        paramBindings = (H2CTFEExecBinding*)H2ArenaAlloc(
            p->arena,
            sizeof(H2CTFEExecBinding) * fn->paramCount,
            (uint32_t)_Alignof(H2CTFEExecBinding));
        if (paramBindings == NULL) {
            return ErrorSimple("out of memory");
        }
    }
    child = ASTFirstChild(ast, fn->fnNode);
    while (child >= 0) {
        const H2AstNode* n = &ast->nodes[child];
        if (n->kind == H2Ast_PARAM) {
            if (paramBindings == NULL) {
                return ErrorSimple("out of memory");
            }
            int32_t     paramTypeNode = ASTFirstChild(ast, child);
            H2CTFEValue boundValue;
            if (fn->isVariadic && paramIndex + 1u == fn->paramCount) {
                H2EvalArray* packArray;
                uint32_t     packLen = argCount - fixedCount;
                packArray = H2EvalAllocArrayView(
                    p, fn->file, paramTypeNode, paramTypeNode, NULL, packLen);
                if (packArray == NULL) {
                    return ErrorSimple("out of memory");
                }
                if (packLen > 0) {
                    uint32_t i;
                    packArray->elems = (H2CTFEValue*)H2ArenaAlloc(
                        p->arena, sizeof(H2CTFEValue) * packLen, (uint32_t)_Alignof(H2CTFEValue));
                    if (packArray->elems == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    memset(packArray->elems, 0, sizeof(H2CTFEValue) * packLen);
                    for (i = 0; i < packLen; i++) {
                        packArray->elems[i] = args[fixedCount + i];
                    }
                }
                H2EvalValueSetArray(&boundValue, fn->file, paramTypeNode, packArray);
            } else {
                boundValue = args[paramIndex];
                if (paramTypeNode >= 0
                    && H2EvalCoerceValueToTypeNode(p, fn->file, paramTypeNode, &boundValue) != 0)
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
    execCtx.evalExpr = H2EvalExecExprCb;
    execCtx.evalExprCtx = p;
    execCtx.evalExprForType = H2EvalExecExprForTypeCb;
    execCtx.evalExprForTypeCtx = p;
    execCtx.zeroInit = H2EvalZeroInitCb;
    execCtx.zeroInitCtx = p;
    execCtx.assignExpr = H2EvalAssignExprCb;
    execCtx.assignExprCtx = p;
    execCtx.assignValueExpr = H2EvalAssignValueExprCb;
    execCtx.assignValueExprCtx = p;
    execCtx.matchPattern = H2EvalMatchPatternCb;
    execCtx.matchPatternCtx = p;
    execCtx.forInIndex = H2EvalForInIndexCb;
    execCtx.forInIndexCtx = p;
    execCtx.forInIter = H2EvalForInIterCb;
    execCtx.forInIterCtx = p;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = H2CTFE_EXEC_DEFAULT_FOR_LIMIT;
    execCtx.skipConstBlocks = 1u;
    H2CTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    savedContext = p->currentContext;
    p->currentFile = fn->file;
    p->currentExecCtx = &execCtx;
    p->currentContext = callContext;
    p->callStack[p->callDepth++] = (uint32_t)fnIndex;

    rc = H2EvalTryMirInvokeFunction(
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
        H2EvalValueSetNull(outValue);
    } else if (fn->hasReturnType) {
        int32_t returnTypeNode = H2EvalFunctionReturnTypeNode(fn);
        if (returnTypeNode >= 0
            && H2EvalCoerceValueToTypeNode(p, fn->file, returnTypeNode, outValue) != 0)
        {
            return -1;
        }
    }
    return 0;
}

static int H2EvalInvokeFunctionRef(
    H2EvalProgram*     p,
    const H2CTFEValue* calleeValue,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst) {
    uint32_t              fnIndex = 0;
    uint32_t              mirFnIndex = 0;
    int                   didReturn = 0;
    const H2EvalFunction* fn;
    if (p == NULL || calleeValue == NULL || outValue == NULL || outIsConst == NULL
        || (argCount > 0 && args == NULL))
    {
        return 0;
    }
    if (H2MirValueAsFunctionRef(calleeValue, &mirFnIndex)) {
        H2MirExecEnv env = { 0 };
        int          mirIsConst = 0;
        if (p->currentMirExecCtx == NULL || p->currentMirExecCtx->mirProgram == NULL
            || mirFnIndex >= p->currentMirExecCtx->mirProgram->funcLen)
        {
            return 0;
        }
        H2EvalMirInitExecEnv(p, p->currentFile, &env, p->currentMirExecCtx);
        if (!H2MirProgramNeedsDynamicResolution(p->currentMirExecCtx->mirProgram)) {
            H2MirExecEnvDisableDynamicResolution(&env);
        }
        if (H2MirEvalFunction(
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
        H2EvalMirAdaptOutValue(p->currentMirExecCtx, outValue, &mirIsConst);
        if (mirIsConst && outValue->kind == H2CTFEValue_INVALID) {
            H2EvalValueSetNull(outValue);
        }
        *outIsConst = mirIsConst;
        return 1;
    }
    if (!H2EvalValueIsFunctionRef(calleeValue, &fnIndex) || fnIndex >= p->funcLen) {
        return 0;
    }
    fn = &p->funcs[fnIndex];
    if (fn->isBuiltinPackageFn) {
        return 0;
    }
    {
        H2EvalTemplateBindingState savedBinding;
        H2EvalSaveTemplateBinding(p, &savedBinding);
        (void)H2EvalBindActiveTemplateForMirCall(p, NULL, NULL, NULL, fn, args, argCount);
        if (H2EvalInvokeFunction(
                p, (int32_t)fnIndex, args, argCount, p->currentContext, outValue, &didReturn)
            != 0)
        {
            H2EvalRestoreTemplateBinding(p, &savedBinding);
            return -1;
        }
        H2EvalRestoreTemplateBinding(p, &savedBinding);
    }
    if (!didReturn) {
        if (fn->hasReturnType) {
            *outIsConst = 0;
            return 1;
        }
        H2EvalValueSetNull(outValue);
    }
    *outIsConst = 1;
    return 1;
}

static int H2EvalEvalTopVar(
    H2EvalProgram* p, uint32_t topVarIndex, H2CTFEValue* outValue, int* outIsConst) {
    H2EvalTopVar*       topVar;
    const H2ParsedFile* savedFile;
    H2CTFEExecCtx*      savedExecCtx;
    H2CTFEExecCtx       execCtx;
    H2CTFEValue         value;
    int                 isConst = 0;
    int                 rc;
    if (p == NULL || outValue == NULL || outIsConst == NULL || topVarIndex >= p->topVarLen) {
        return -1;
    }
    topVar = &p->topVars[topVarIndex];
    if (topVar->state == H2EvalTopConstState_READY) {
        *outValue = topVar->value;
        *outIsConst = 1;
        return 0;
    }
    if (topVar->state == H2EvalTopConstState_VISITING
        || topVar->state == H2EvalTopConstState_FAILED)
    {
        *outIsConst = 0;
        return 0;
    }

    topVar->state = H2EvalTopConstState_VISITING;
    memset(&execCtx, 0, sizeof(execCtx));
    execCtx.arena = p->arena;
    execCtx.ast = &topVar->file->ast;
    execCtx.src.ptr = topVar->file->source;
    execCtx.src.len = topVar->file->sourceLen;
    execCtx.evalExpr = H2EvalExecExprCb;
    execCtx.evalExprCtx = p;
    execCtx.evalExprForType = H2EvalExecExprForTypeCb;
    execCtx.evalExprForTypeCtx = p;
    execCtx.zeroInit = H2EvalZeroInitCb;
    execCtx.zeroInitCtx = p;
    execCtx.assignExpr = H2EvalAssignExprCb;
    execCtx.assignExprCtx = p;
    execCtx.assignValueExpr = H2EvalAssignValueExprCb;
    execCtx.assignValueExprCtx = p;
    execCtx.matchPattern = H2EvalMatchPatternCb;
    execCtx.matchPatternCtx = p;
    execCtx.forInIndex = H2EvalForInIndexCb;
    execCtx.forInIndexCtx = p;
    execCtx.forInIter = H2EvalForInIterCb;
    execCtx.forInIterCtx = p;
    execCtx.pendingReturnExprNode = -1;
    execCtx.forIterLimit = H2CTFE_EXEC_DEFAULT_FOR_LIMIT;
    execCtx.skipConstBlocks = 1u;
    H2CTFEExecResetReason(&execCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topVar->file;
    p->currentExecCtx = &execCtx;
    {
        int mirSupported = 0;
        rc = H2EvalTryMirEvalTopInit(
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
            rc = H2EvalTryMirZeroInitType(
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
        topVar->state = H2EvalTopConstState_FAILED;
        return -1;
    }
    if (!isConst) {
        topVar->state = H2EvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }
    topVar->value = value;
    topVar->state = H2EvalTopConstState_READY;
    *outValue = value;
    *outIsConst = 1;
    return 0;
}

static int H2EvalEvalTopConst(
    H2EvalProgram* p, uint32_t topConstIndex, H2CTFEValue* outValue, int* outIsConst) {
    H2EvalTopConst*     topConst;
    const H2ParsedFile* savedFile;
    H2CTFEExecCtx*      savedExecCtx;
    H2CTFEExecCtx       constExecCtx;
    H2CTFEValue         value;
    int                 isConst = 0;
    int                 rc;
    if (p == NULL || outValue == NULL || outIsConst == NULL || topConstIndex >= p->topConstLen) {
        return -1;
    }
    topConst = &p->topConsts[topConstIndex];
    if (topConst->state == H2EvalTopConstState_READY) {
        *outValue = topConst->value;
        *outIsConst = 1;
        return 0;
    }
    if (topConst->state == H2EvalTopConstState_VISITING
        || topConst->state == H2EvalTopConstState_FAILED)
    {
        *outIsConst = 0;
        return 0;
    }
    if (topConst->initExprNode < 0) {
        topConst->state = H2EvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }

    topConst->state = H2EvalTopConstState_VISITING;
    memset(&constExecCtx, 0, sizeof(constExecCtx));
    constExecCtx.arena = p->arena;
    constExecCtx.ast = &topConst->file->ast;
    constExecCtx.src.ptr = topConst->file->source;
    constExecCtx.src.len = topConst->file->sourceLen;
    constExecCtx.evalExpr = H2EvalExecExprCb;
    constExecCtx.evalExprCtx = p;
    constExecCtx.evalExprForType = H2EvalExecExprForTypeCb;
    constExecCtx.evalExprForTypeCtx = p;
    constExecCtx.zeroInit = H2EvalZeroInitCb;
    constExecCtx.zeroInitCtx = p;
    constExecCtx.assignExpr = H2EvalAssignExprCb;
    constExecCtx.assignExprCtx = p;
    constExecCtx.assignValueExpr = H2EvalAssignValueExprCb;
    constExecCtx.assignValueExprCtx = p;
    constExecCtx.matchPattern = H2EvalMatchPatternCb;
    constExecCtx.matchPatternCtx = p;
    constExecCtx.forInIndex = H2EvalForInIndexCb;
    constExecCtx.forInIndexCtx = p;
    constExecCtx.forInIter = H2EvalForInIterCb;
    constExecCtx.forInIterCtx = p;
    constExecCtx.pendingReturnExprNode = -1;
    constExecCtx.forIterLimit = H2CTFE_EXEC_DEFAULT_FOR_LIMIT;
    constExecCtx.skipConstBlocks = 1u;
    H2CTFEExecResetReason(&constExecCtx);

    savedFile = p->currentFile;
    savedExecCtx = p->currentExecCtx;
    p->currentFile = topConst->file;
    p->currentExecCtx = &constExecCtx;
    {
        rc = H2EvalTypeValueFromExprNode(
            p, topConst->file, &topConst->file->ast, topConst->initExprNode, &value);
        if (rc < 0) {
            p->currentExecCtx = savedExecCtx;
            p->currentFile = savedFile;
            topConst->state = H2EvalTopConstState_FAILED;
            return -1;
        }
        if (rc > 0) {
            rc = 0;
            isConst = 1;
        } else {
            int mirSupported = 0;
            rc = H2EvalTryMirEvalTopInit(
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
        topConst->state = H2EvalTopConstState_FAILED;
        return -1;
    }
    if (!isConst) {
        topConst->state = H2EvalTopConstState_FAILED;
        *outIsConst = 0;
        return 0;
    }

    topConst->value = value;
    topConst->state = H2EvalTopConstState_READY;
    *outValue = value;
    *outIsConst = 1;
    return 0;
}

static int H2EvalResolveIdent(
    void*        ctx,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*   p = (H2EvalProgram*)ctx;
    const H2Package* currentPkg;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL || p->currentExecCtx == NULL
        || p->currentFile == NULL)
    {
        return -1;
    }
    currentPkg = H2EvalFindPackageByFile(p, p->currentFile);
    {
        H2CTFEExecBinding* binding = H2EvalFindBinding(
            p->currentExecCtx, p->currentFile, nameStart, nameEnd);
        if (binding != NULL) {
            *outValue = binding->value;
            if (binding->typeNode >= 0) {
                int32_t typeCode = H2EvalTypeCode_INVALID;
                if (!H2EvalValueGetRuntimeTypeCode(outValue, &typeCode)
                    && H2EvalTypeCodeFromTypeNode(p->currentFile, binding->typeNode, &typeCode))
                {
                    H2EvalValueSetRuntimeTypeCode(outValue, typeCode);
                }
            }
            *outIsConst = 1;
            return 0;
        }
    }
    if (H2CTFEExecEnvLookup(p->currentExecCtx, nameStart, nameEnd, outValue)) {
        *outIsConst = 1;
        return 0;
    }
    if (H2EvalMirLookupLocalValue(p, nameStart, nameEnd, outValue)) {
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
        && p->activeTemplateTypeValue.kind == H2CTFEValue_TYPE)
    {
        *outValue = p->activeTemplateTypeValue;
        *outIsConst = 1;
        return 0;
    }
    {
        int32_t typeCode = H2EvalTypeCode_INVALID;
        if (H2EvalBuiltinTypeCode(p->currentFile->source, nameStart, nameEnd, &typeCode)) {
            H2EvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
        if (H2EvalResolveTypeValueName(p, p->currentFile, nameStart, nameEnd, outValue) > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    {
        if (currentPkg != NULL) {
            uint32_t i;
            for (i = 0; i < currentPkg->importLen; i++) {
                const H2ImportRef* imp = &currentPkg->imports[i];
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
                        H2EvalValueSetPackageRef(outValue, (uint32_t)pkgIndex);
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
                ? H2EvalFindTopVarBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : H2EvalFindTopVarBySlice(p, p->currentFile, nameStart, nameEnd);
        if (topVarIndex >= 0) {
            int isConst = 0;
            if (H2EvalEvalTopVar(p, (uint32_t)topVarIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst) {
                *outIsConst = 1;
                return 0;
            }
            H2CTFEExecSetReason(
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
                ? H2EvalFindTopConstBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : H2EvalFindTopConstBySlice(p, p->currentFile, nameStart, nameEnd);
        if (topConstIndex >= 0) {
            int isConst = 0;
            if (H2EvalEvalTopConst(p, (uint32_t)topConstIndex, outValue, &isConst) != 0) {
                return -1;
            }
            if (isConst) {
                *outIsConst = 1;
                return 0;
            }
            H2CTFEExecSetReason(
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
                ? H2EvalFindAnyFunctionBySliceInPackage(
                      p, currentPkg, p->currentFile, nameStart, nameEnd)
                : H2EvalFindAnyFunctionBySlice(p, p->currentFile, nameStart, nameEnd);
        if (fnIndex >= 0) {
            if (p->currentMirExecCtx != NULL && p->currentMirExecCtx->evalToMir != NULL
                && (uint32_t)fnIndex < p->currentMirExecCtx->evalToMirLen
                && p->currentMirExecCtx->evalToMir[(uint32_t)fnIndex] != UINT32_MAX)
            {
                H2MirValueSetFunctionRef(
                    outValue, p->currentMirExecCtx->evalToMir[(uint32_t)fnIndex]);
            } else {
                H2EvalValueSetFunctionRef(outValue, (uint32_t)fnIndex);
            }
            *outIsConst = 1;
            return 0;
        }
        if (fnIndex == -2) {
            H2CTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "function value is ambiguous in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
    }
    H2CTFEExecSetReason(
        p->currentExecCtx, nameStart, nameEnd, "identifier is not available in evaluator backend");
    *outIsConst = 0;
    return 0;
}

static int H2EvalResolveCallMirPre(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t     nameStart,
    uint32_t     nameEnd,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram* p = (H2EvalProgram*)ctx;
    int32_t        callNode = -1;
    (void)function;
    (void)nameStart;
    (void)nameEnd;
    (void)diag;
    if (p == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    if (!H2EvalMirResolveCallNode(program, inst, &callNode)
        || !H2EvalMirCallNodeIsLazyBuiltin(p, callNode))
    {
        return 0;
    }
    if (H2EvalExecExprCb(p, callNode, outValue, outIsConst) != 0) {
        return -1;
    }
    return 1;
}

static int H2EvalExpandMirSpreadLastArgs(
    H2EvalProgram* p,
    const H2MirInst* _Nullable inst,
    const H2EvalFunction* fn,
    const H2CTFEValue* _Nullable args,
    uint32_t            argCount,
    const H2CTFEValue** outArgs,
    uint32_t*           outArgCount) {
    const H2EvalArray* spreadArray;
    H2CTFEValue*       expandedArgs;
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
    if (inst == NULL || !fn->isVariadic || !H2MirCallTokHasSpreadLast(inst->tok)) {
        return 1;
    }
    if (argCount == 0u) {
        return 0;
    }
    spreadArray = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&args[argCount - 1u]));
    if (spreadArray == NULL || spreadArray->len > UINT32_MAX - (argCount - 1u)) {
        return 0;
    }
    if (spreadArray->len > 0 && spreadArray->elems == NULL) {
        return -1;
    }
    prefixCount = argCount - 1u;
    expandedArgs = (H2CTFEValue*)H2ArenaAlloc(
        p->arena,
        sizeof(H2CTFEValue) * (prefixCount + spreadArray->len),
        (uint32_t)_Alignof(H2CTFEValue));
    if (expandedArgs == NULL) {
        return ErrorSimple("out of memory");
    }
    for (i = 0; i < prefixCount; i++) {
        expandedArgs[i] = args[i];
        H2EvalAnnotateUntypedLiteralValue(&expandedArgs[i]);
    }
    for (i = 0; i < spreadArray->len; i++) {
        expandedArgs[prefixCount + i] = spreadArray->elems[i];
        H2EvalAnnotateUntypedLiteralValue(&expandedArgs[prefixCount + i]);
    }
    *outArgs = expandedArgs;
    *outArgCount = prefixCount + spreadArray->len;
    return 1;
}

static int H2EvalResolveCallMir(
    void* ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    H2EvalProgram*        p = (H2EvalProgram*)ctx;
    int32_t               fnIndex = -1;
    const H2EvalFunction* fn;
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

    isReflectKind = H2EvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "kind", "reflect");
    isReflectIsAlias = H2EvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "is_alias", "reflect");
    isReflectTypeName = H2EvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "type_name", "reflect");
    isReflectBase = H2EvalNameEqLiteralOrPkgBuiltin(
        p->currentFile->source, nameStart, nameEnd, "base", "reflect");
    isTypeOf = SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "typeof");

    if (argCount == 1 && isReflectKind) {
        int32_t kind = 0;
        if (H2EvalTypeKindOfValue(&args[0], &kind)) {
            H2EvalValueSetInt(outValue, kind);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectIsAlias) {
        int32_t kind = 0;
        if (H2EvalTypeKindOfValue(&args[0], &kind)) {
            outValue->kind = H2CTFEValue_BOOL;
            outValue->i64 = 0;
            outValue->f64 = 0.0;
            outValue->b = kind == H2EvalTypeKind_ALIAS ? 1u : 0u;
            outValue->typeTag = 0;
            outValue->s.bytes = NULL;
            outValue->s.len = 0;
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectTypeName) {
        if (H2EvalTypeNameOfValue((H2CTFEValue*)&args[0], outValue)) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isReflectBase) {
        H2EvalReflectedType* rt = H2EvalValueAsReflectedType(&args[0]);
        if (rt != NULL && rt->kind == H2EvalReflectType_NAMED
            && rt->namedKind == H2EvalTypeKind_ALIAS && rt->file != NULL && rt->nodeId >= 0
            && (uint32_t)rt->nodeId < rt->file->ast.len)
        {
            int32_t baseTypeNode = ASTFirstChild(&rt->file->ast, rt->nodeId);
            if (baseTypeNode >= 0
                && H2EvalTypeValueFromTypeNode(p, rt->file, baseTypeNode, outValue) > 0)
            {
                *outIsConst = 1;
                return 0;
            }
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "ptr")) {
        H2EvalReflectedType* rt;
        if (args[0].kind == H2CTFEValue_TYPE) {
            rt = (H2EvalReflectedType*)H2ArenaAlloc(
                p->arena, sizeof(H2EvalReflectedType), (uint32_t)_Alignof(H2EvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = H2EvalReflectType_PTR;
            rt->namedKind = H2EvalTypeKind_POINTER;
            rt->elemType = args[0];
            H2EvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "slice")) {
        H2EvalReflectedType* rt;
        if (args[0].kind == H2CTFEValue_TYPE) {
            rt = (H2EvalReflectedType*)H2ArenaAlloc(
                p->arena, sizeof(H2EvalReflectedType), (uint32_t)_Alignof(H2EvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = H2EvalReflectType_SLICE;
            rt->namedKind = H2EvalTypeKind_SLICE;
            rt->elemType = args[0];
            H2EvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "array")) {
        int64_t              arrayLen = 0;
        H2EvalReflectedType* rt;
        if (args[0].kind == H2CTFEValue_TYPE && H2CTFEValueToInt64(&args[1], &arrayLen) == 0
            && arrayLen >= 0 && arrayLen <= (int64_t)UINT32_MAX)
        {
            rt = (H2EvalReflectedType*)H2ArenaAlloc(
                p->arena, sizeof(H2EvalReflectedType), (uint32_t)_Alignof(H2EvalReflectedType));
            if (rt == NULL) {
                return ErrorSimple("out of memory");
            }
            memset(rt, 0, sizeof(*rt));
            rt->kind = H2EvalReflectType_ARRAY;
            rt->namedKind = H2EvalTypeKind_ARRAY;
            rt->arrayLen = (uint32_t)arrayLen;
            rt->elemType = args[0];
            H2EvalValueSetReflectedTypeValue(outValue, rt);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "concat")) {
        int concatRc = H2EvalValueConcatStrings(p->arena, &args[0], &args[1], outValue);
        if (concatRc < 0) {
            return -1;
        }
        if (concatRc > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 2 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "copy")) {
        int copyRc = H2EvalValueCopyBuiltin(p->arena, &args[0], &args[1], outValue);
        if (copyRc < 0) {
            return -1;
        }
        if (copyRc > 0) {
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && isTypeOf) {
        int32_t          typeCode = H2EvalTypeCode_INVALID;
        H2EvalAggregate* agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(&args[0]));
        if (args[0].kind == H2CTFEValue_TYPE) {
            H2EvalValueSetSimpleTypeValue(outValue, H2EvalTypeCode_TYPE);
            *outIsConst = 1;
            return 0;
        }
        if (agg != NULL && agg->file != NULL && agg->nodeId >= 0
            && (uint32_t)agg->nodeId < agg->file->ast.len)
        {
            uint8_t namedKind = 0;
            switch (agg->file->ast.nodes[agg->nodeId].kind) {
                case H2Ast_STRUCT: namedKind = H2EvalTypeKind_STRUCT; break;
                case H2Ast_UNION:  namedKind = H2EvalTypeKind_UNION; break;
                case H2Ast_ENUM:   namedKind = H2EvalTypeKind_ENUM; break;
                default:           break;
            }
            if (namedKind != 0
                && H2EvalMakeNamedTypeValue(p, agg->file, agg->nodeId, namedKind, outValue) > 0)
            {
                *outIsConst = 1;
                return 0;
            }
        }
        if (H2EvalTypeCodeFromValue(&args[0], &typeCode)) {
            H2EvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "len")) {
        const H2CTFEValue* value = H2EvalValueTargetOrSelf(&args[0]);
        if (value->kind == H2CTFEValue_STRING) {
            H2EvalValueSetInt(outValue, (int64_t)value->s.len);
            *outIsConst = 1;
            return 0;
        }
        if (value->kind == H2CTFEValue_ARRAY) {
            H2EvalValueSetInt(outValue, (int64_t)value->s.len);
            *outIsConst = 1;
            return 0;
        }
        if (value->kind == H2CTFEValue_NULL) {
            H2EvalValueSetInt(outValue, 0);
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount == 1 && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "cstr")) {
        const H2CTFEValue* value = H2EvalValueTargetOrSelf(&args[0]);
        if (value->kind == H2CTFEValue_STRING) {
            *outValue = *value;
            *outIsConst = 1;
            return 0;
        }
    }
    if (argCount > 0) {
        const H2CTFEValue* baseValue = H2EvalValueTargetOrSelf(&args[0]);
        H2EvalAggregate*   agg = H2EvalValueAsAggregate(baseValue);
        H2CTFEValue        fieldValue;
        if (agg != NULL
            && H2EvalAggregateGetFieldValue(
                agg, p->currentFile->source, nameStart, nameEnd, &fieldValue)
            && H2EvalValueIsInvokableFunctionRef(&fieldValue))
        {
            int invoked = H2EvalInvokeFunctionRef(
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
        if (args[0].kind == H2CTFEValue_STRING) {
            if (args[0].s.len > 0 && args[0].s.bytes != NULL) {
                if (fwrite(args[0].s.bytes, 1, args[0].s.len, stdout) != args[0].s.len) {
                    return ErrorSimple("failed to write print output");
                }
            }
            fputc('\n', stdout);
            fflush(stdout);
            H2EvalValueSetNull(outValue);
            *outIsConst = 1;
            return 0;
        }
    }
    if ((argCount == 1 || argCount == 2)
        && SliceEqCStr(p->currentFile->source, nameStart, nameEnd, "free"))
    {
        H2EvalValueSetNull(outValue);
        *outIsConst = 1;
        return 0;
    }
    if (program != NULL && inst != NULL && argCount > 0) {
        int32_t          callNode = -1;
        const H2Package* targetPkg = NULL;
        if (H2EvalMirResolveCallNode(program, inst, &callNode)) {
            targetPkg = H2EvalMirResolveQualifiedImportCallTargetPkg(p, callNode);
        }
        if (targetPkg != NULL) {
            fnIndex = H2EvalResolveFunctionBySlice(
                p, targetPkg, p->currentFile, nameStart, nameEnd, args + 1u, argCount - 1u);
            if (fnIndex == -2) {
                H2CTFEExecSetReason(
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
        if (H2EvalValueIsPackageRef(&args[0], &pkgIndex)) {
            const H2Package* targetPkg = NULL;
            if (p->loader == NULL || pkgIndex >= p->loader->packageLen) {
                return -1;
            }
            targetPkg = &p->loader->packages[pkgIndex];
            fnIndex = H2EvalResolveFunctionBySlice(
                p, targetPkg, p->currentFile, nameStart, nameEnd, args + 1, argCount - 1u);
            if (fnIndex == -2) {
                H2CTFEExecSetReason(
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
        H2CTFEValue calleeValue;
        int         calleeIsConst = 0;
        const char* savedReason = p->currentExecCtx->nonConstReason;
        uint32_t    savedStart = p->currentExecCtx->nonConstStart;
        uint32_t    savedEnd = p->currentExecCtx->nonConstEnd;
        if (H2EvalResolveIdent(
                p, nameStart, nameEnd, &calleeValue, &calleeIsConst, p->currentExecCtx->diag)
            != 0)
        {
            return -1;
        }
        if (calleeIsConst && H2EvalValueIsInvokableFunctionRef(&calleeValue)) {
            const H2ParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
            int32_t             savedExpectedTypeNode = p->activeCallExpectedTypeNode;
            int                 invoked;
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
                    && program->insts[instIndex + 1u].op == H2MirOp_LOCAL_STORE)
                {
                    uint32_t localSlot = program->insts[instIndex + 1u].aux;
                    if (localSlot < function->localCount
                        && function->localStart + localSlot < program->localLen)
                    {
                        const H2MirLocal* local =
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
            invoked = H2EvalInvokeFunctionRef(
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
        fnIndex = H2EvalResolveFunctionBySlice(
            p, NULL, p->currentFile, nameStart, nameEnd, args, argCount);
    }
    if (fnIndex == -2) {
        H2CTFEExecSetReason(
            p->currentExecCtx, nameStart, nameEnd, "call target is ambiguous in evaluator backend");
        *outIsConst = 0;
        return 0;
    }
    if (fnIndex < 0) {
        H2CTFEExecSetReason(
            p->currentExecCtx,
            nameStart,
            nameEnd,
            "call target is not supported by evaluator backend");
        *outIsConst = 0;
        return 0;
    }
    fn = &p->funcs[fnIndex];
    {
        const H2CTFEValue* invokeArgs = args;
        uint32_t           invokeArgCount = argCount;
        int                expandRc = H2EvalExpandMirSpreadLastArgs(
            p, inst, fn, args, argCount, &invokeArgs, &invokeArgCount);
        if (expandRc < 0) {
            return -1;
        }
        if (expandRc == 0) {
            H2CTFEExecSetReason(
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

    if (p->callDepth >= H2_EVAL_CALL_MAX_DEPTH) {
        H2CTFEExecSetReason(
            p->currentExecCtx, nameStart, nameEnd, "evaluator backend call depth limit exceeded");
        *outIsConst = 0;
        return 0;
    }
    {
        uint32_t i;
        for (i = 0; i < p->callDepth; i++) {
            if (p->callStack[i] == (uint32_t)fnIndex) {
                H2CTFEExecSetReason(
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
        H2EvalTemplateBindingState savedBinding;
        H2EvalSaveTemplateBinding(p, &savedBinding);
        (void)H2EvalBindActiveTemplateForMirCall(p, program, function, inst, fn, args, argCount);
        if (H2EvalInvokeFunction(
                p, fnIndex, args, argCount, p->currentContext, outValue, &didReturn)
            != 0)
        {
            H2EvalRestoreTemplateBinding(p, &savedBinding);
            return -1;
        }
        H2EvalRestoreTemplateBinding(p, &savedBinding);
    }
    if (!didReturn) {
        if (fn->hasReturnType) {
            H2CTFEExecSetReason(
                p->currentExecCtx,
                nameStart,
                nameEnd,
                "function returned without a value in evaluator backend");
            *outIsConst = 0;
            return 0;
        }
        H2EvalValueSetNull(outValue);
    }
    *outIsConst = 1;
    return 0;
}

static int H2EvalResolveCall(
    void*    ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2CTFEValue* _Nullable args,
    uint32_t     argCount,
    H2CTFEValue* outValue,
    int*         outIsConst,
    H2Diag* _Nullable diag) {
    return H2EvalResolveCallMir(
        ctx, NULL, NULL, NULL, nameStart, nameEnd, args, argCount, outValue, outIsConst, diag);
}

static int H2EvalExecExprCb(void* ctx, int32_t exprNode, H2CTFEValue* outValue, int* outIsConst) {
    H2EvalProgram*   p = (H2EvalProgram*)ctx;
    const H2Ast*     ast;
    const H2AstNode* n;
    H2Diag           diag = { 0 };
    int              rc;

    if (p == NULL || p->currentFile == NULL || outValue == NULL || outIsConst == NULL) {
        return -1;
    }
    ast = &p->currentFile->ast;
    if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
        return -1;
    }
    n = &ast->nodes[exprNode];
    while (n->kind == H2Ast_CALL_ARG) {
        exprNode = n->firstChild;
        if (exprNode < 0 || (uint32_t)exprNode >= ast->len) {
            return -1;
        }
        n = &ast->nodes[exprNode];
    }

    if (n->kind == H2Ast_COMPOUND_LIT) {
        return H2EvalEvalCompoundLiteral(
            p, exprNode, p->currentFile, p->currentFile, -1, outValue, outIsConst);
    }

    if (n->kind == H2Ast_NEW) {
        return H2EvalEvalNewExpr(p, exprNode, NULL, -1, outValue, outIsConst);
    }

    if (n->kind == H2Ast_CALL_WITH_CONTEXT) {
        int32_t              callNode = n->firstChild;
        int32_t              overlayNode = callNode >= 0 ? ast->nodes[callNode].nextSibling : -1;
        const H2EvalContext* savedContext;
        H2EvalContext        overlayContext;
        int                  overlayRc;
        if (callNode < 0 || (uint32_t)callNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        overlayRc = H2EvalBuildContextOverlay(p, overlayNode, &overlayContext, p->currentFile);
        if (overlayRc != 1) {
            *outIsConst = 0;
            return overlayRc < 0 ? -1 : 0;
        }
        savedContext = p->currentContext;
        p->currentContext = &overlayContext;
        rc = H2EvalExecExprCb(p, callNode, outValue, outIsConst);
        p->currentContext = savedContext;
        return rc;
    }

    if (n->kind == H2Ast_SIZEOF) {
        int32_t     childNode = n->firstChild;
        uint64_t    sizeBytes = 0;
        H2CTFEValue childValue;
        int         childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (n->flags == 1u) {
            if (!H2EvalTypeNodeSize(p->currentFile, childNode, &sizeBytes, 0)) {
                *outIsConst = 0;
                return 0;
            }
        } else {
            if (H2EvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
                return -1;
            }
            if (!childIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (childValue.kind == H2CTFEValue_BOOL) {
                sizeBytes = 1u;
            } else if (childValue.kind == H2CTFEValue_INT || childValue.kind == H2CTFEValue_FLOAT) {
                sizeBytes = 8u;
            } else if (childValue.kind == H2CTFEValue_STRING) {
                sizeBytes = (uint64_t)(sizeof(void*) * 2u);
            } else if (childValue.kind == H2CTFEValue_ARRAY) {
                sizeBytes = (uint64_t)childValue.s.len * 8u;
            } else if (
                childValue.kind == H2CTFEValue_REFERENCE || childValue.kind == H2CTFEValue_NULL)
            {
                sizeBytes = (uint64_t)sizeof(void*);
            } else {
                *outIsConst = 0;
                return 0;
            }
        }
        H2EvalValueSetInt(outValue, (int64_t)sizeBytes);
        *outIsConst = 1;
        return 0;
    }

    if (n->kind == H2Ast_INDEX && (n->flags & H2AstFlag_INDEX_SLICE) != 0u) {
        int32_t            baseNode = n->firstChild;
        int32_t            extraNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        H2CTFEValue        baseValue;
        const H2CTFEValue* targetValue;
        H2EvalArray*       array;
        H2EvalArray*       view;
        H2CTFEValue        startValue;
        H2CTFEValue        endValue;
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
        if (H2EvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
            return -1;
        }
        if (!baseIsConst) {
            *outIsConst = 0;
            return 0;
        }
        targetValue = H2EvalValueTargetOrSelf(&baseValue);
        array = H2EvalValueAsArray(targetValue);
        if ((n->flags & H2AstFlag_INDEX_HAS_START) != 0u) {
            if (extraNode < 0 || (uint32_t)extraNode >= ast->len
                || H2EvalExecExprCb(p, extraNode, &startValue, &startIsConst) != 0)
            {
                return extraNode < 0 ? 0 : -1;
            }
            if (!startIsConst || H2CTFEValueToInt64(&startValue, &start) != 0 || start < 0) {
                *outIsConst = 0;
                return 0;
            }
            extraNode = ast->nodes[extraNode].nextSibling;
        }
        if ((n->flags & H2AstFlag_INDEX_HAS_END) != 0u) {
            if (extraNode < 0 || (uint32_t)extraNode >= ast->len
                || H2EvalExecExprCb(p, extraNode, &endValue, &endIsConst) != 0)
            {
                return extraNode < 0 ? 0 : -1;
            }
            if (!endIsConst || H2CTFEValueToInt64(&endValue, &end) != 0 || end < 0) {
                *outIsConst = 0;
                return 0;
            }
            extraNode = ast->nodes[extraNode].nextSibling;
        }
        if (extraNode >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (array == NULL && targetValue->kind == H2CTFEValue_STRING) {
            int32_t currentTypeCode = H2EvalTypeCode_INVALID;
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
            if (!H2EvalValueGetRuntimeTypeCode(targetValue, &currentTypeCode)) {
                currentTypeCode = H2EvalTypeCode_STR_REF;
            }
            H2EvalValueSetRuntimeTypeCode(outValue, currentTypeCode);
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
        view = H2EvalAllocArrayView(
            p,
            targetValue->kind == H2CTFEValue_ARRAY ? array->file : p->currentFile,
            exprNode,
            array->elemTypeNode,
            array->elems + startIndex,
            endIndex - startIndex);
        if (view == NULL) {
            return ErrorSimple("out of memory");
        }
        {
            H2CTFEValue viewValue;
            H2EvalValueSetArray(&viewValue, p->currentFile, exprNode, view);
            return H2EvalAllocReferencedValue(p, &viewValue, outValue, outIsConst);
        }
    }

    if (n->kind == H2Ast_INDEX && (n->flags & 0x7u) == 0u) {
        int32_t      baseNode = n->firstChild;
        int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
        H2CTFEValue  baseValue;
        H2CTFEValue  indexValue;
        H2EvalArray* array;
        int          baseIsConst = 0;
        int          indexIsConst = 0;
        int64_t      index = 0;
        if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
            goto index_fallback;
        }
        if (H2EvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
            || H2EvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
        {
            return -1;
        }
        if (!baseIsConst || !indexIsConst || H2CTFEValueToInt64(&indexValue, &index) != 0) {
            goto index_fallback;
        }
        array = H2EvalValueAsArray(&baseValue);
        if (array == NULL) {
            array = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&baseValue));
        }
        if (array != NULL && index >= 0 && (uint64_t)index < (uint64_t)array->len) {
            *outValue = array->elems[(uint32_t)index];
            *outIsConst = 1;
            return 0;
        }
        {
            const H2CTFEValue* targetValue = H2EvalValueTargetOrSelf(&baseValue);
            if (targetValue->kind == H2CTFEValue_STRING && index >= 0
                && (uint64_t)index < (uint64_t)targetValue->s.len)
            {
                H2EvalValueSetInt(outValue, (int64_t)targetValue->s.bytes[(uint32_t)index]);
                H2EvalValueSetRuntimeTypeCode(outValue, H2EvalTypeCode_U8);
                *outIsConst = 1;
                return 0;
            }
        }
        if (index < 0) {
            goto index_fallback;
        }
    index_fallback:;
    }

    if (n->kind == H2Ast_FIELD_EXPR) {
        int32_t          baseNode = n->firstChild;
        H2CTFEValue      baseValue;
        int              baseIsConst = 0;
        H2EvalAggregate* agg = NULL;
        if (baseNode < 0 || (uint32_t)baseNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[baseNode].kind == H2Ast_IDENT) {
            if (SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "context")
                && H2EvalCurrentContextField(
                    p, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
            {
                *outIsConst = 1;
                return 0;
            }
            const H2ParsedFile* enumFile = NULL;
            int32_t             enumNode = -1;
            enumNode = H2EvalFindNamedEnumDecl(
                p,
                p->currentFile,
                ast->nodes[baseNode].dataStart,
                ast->nodes[baseNode].dataEnd,
                &enumFile);
            if (enumNode >= 0 && enumFile != NULL) {
                int32_t  variantNode = -1;
                uint32_t tagIndex = 0;
                if (H2EvalFindEnumVariant(
                        enumFile,
                        enumNode,
                        p->currentFile->source,
                        n->dataStart,
                        n->dataEnd,
                        &variantNode,
                        &tagIndex))
                {
                    const H2AstNode* variantField = &enumFile->ast.nodes[variantNode];
                    int32_t          valueNode = ASTFirstChild(&enumFile->ast, variantNode);
                    if (valueNode >= 0 && enumFile->ast.nodes[valueNode].kind != H2Ast_FIELD
                        && !H2EvalEnumHasPayloadVariants(enumFile, enumNode))
                    {
                        H2CTFEValue enumValue;
                        int         enumIsConst = 0;
                        if (H2EvalExecExprInFileWithType(
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
                    H2EvalValueSetTaggedEnum(
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
        if (H2EvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
            return -1;
        }
        if (!baseIsConst) {
            *outIsConst = 0;
            return 0;
        }
        {
            const H2CTFEValue* targetValue = H2EvalValueTargetOrSelf(&baseValue);
            const H2CTFEValue* payload = NULL;
            H2EvalTaggedEnum*  tagged = H2EvalValueAsTaggedEnum(targetValue);
            if (targetValue->kind == H2CTFEValue_OPTIONAL
                && H2EvalOptionalPayload(targetValue, &payload) && targetValue->b != 0u
                && payload != NULL)
            {
                targetValue = H2EvalValueTargetOrSelf(payload);
                tagged = H2EvalValueAsTaggedEnum(targetValue);
            }
            if (SliceEqCStr(p->currentFile->source, n->dataStart, n->dataEnd, "len")
                && (targetValue->kind == H2CTFEValue_STRING
                    || targetValue->kind == H2CTFEValue_ARRAY
                    || targetValue->kind == H2CTFEValue_NULL))
            {
                H2EvalValueSetInt(outValue, (int64_t)targetValue->s.len);
                *outIsConst = 1;
                return 0;
            }
            if (SliceEqCStr(p->currentFile->source, n->dataStart, n->dataEnd, "cstr")
                && targetValue->kind == H2CTFEValue_STRING)
            {
                *outValue = *targetValue;
                *outIsConst = 1;
                return 0;
            }
            if (tagged != NULL && tagged->payload != NULL
                && H2EvalAggregateGetFieldValue(
                    tagged->payload, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
            {
                *outIsConst = 1;
                return 0;
            }
        }
        agg = H2EvalValueAsAggregate(&baseValue);
        if (agg == NULL) {
            agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(&baseValue));
        }
        if (agg == NULL) {
            const H2CTFEValue* targetValue = H2EvalValueTargetOrSelf(&baseValue);
            const H2CTFEValue* payload = NULL;
            if (targetValue->kind == H2CTFEValue_OPTIONAL
                && H2EvalOptionalPayload(targetValue, &payload) && targetValue->b != 0u
                && payload != NULL)
            {
                agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(payload));
            }
        }
        if (agg != NULL
            && H2EvalAggregateGetFieldValue(
                agg, p->currentFile->source, n->dataStart, n->dataEnd, outValue))
        {
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_AND) {
        int32_t      childNode = n->firstChild;
        H2CTFEValue  childValue;
        H2CTFEValue* fieldValuePtr;
        int          childIsConst = 0;
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == H2Ast_IDENT)
        {
            H2CTFEExecBinding* binding = H2EvalFindBinding(
                p->currentExecCtx,
                p->currentFile,
                ast->nodes[childNode].dataStart,
                ast->nodes[childNode].dataEnd);
            if (binding != NULL) {
                H2EvalValueSetReference(outValue, &binding->value);
                *outIsConst = 1;
                return 0;
            }
            {
                int32_t topVarIndex = H2EvalFindCurrentTopVarBySlice(
                    p,
                    p->currentFile,
                    ast->nodes[childNode].dataStart,
                    ast->nodes[childNode].dataEnd);
                H2CTFEValue topVarValue;
                int         topVarIsConst = 0;
                if (topVarIndex >= 0
                    && H2EvalEvalTopVar(p, (uint32_t)topVarIndex, &topVarValue, &topVarIsConst) == 0
                    && topVarIsConst)
                {
                    H2EvalValueSetReference(outValue, &p->topVars[(uint32_t)topVarIndex].value);
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == H2Ast_FIELD_EXPR)
        {
            fieldValuePtr = H2EvalResolveFieldExprValuePtr(p, p->currentExecCtx, childNode);
            if (fieldValuePtr != NULL) {
                H2EvalValueSetReference(outValue, fieldValuePtr);
                *outIsConst = 1;
                return 0;
            }
        }
        if (childNode >= 0 && (uint32_t)childNode < ast->len
            && ast->nodes[childNode].kind == H2Ast_INDEX
            && (ast->nodes[childNode].flags & 0x7u) == 0u)
        {
            int32_t      baseNode = ast->nodes[childNode].firstChild;
            int32_t      indexNode = baseNode >= 0 ? ast->nodes[baseNode].nextSibling : -1;
            H2CTFEValue  baseValue;
            H2CTFEValue  indexValue;
            H2EvalArray* array;
            int          baseIsConst = 0;
            int          indexIsConst = 0;
            int64_t      index = 0;
            if (baseNode < 0 || indexNode < 0 || ast->nodes[indexNode].nextSibling >= 0) {
                goto unary_addr_fallback;
            }
            if (H2EvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0
                || H2EvalExecExprCb(p, indexNode, &indexValue, &indexIsConst) != 0)
            {
                return -1;
            }
            if (!baseIsConst || !indexIsConst || H2CTFEValueToInt64(&indexValue, &index) != 0) {
                goto unary_addr_fallback;
            }
            array = H2EvalValueAsArray(&baseValue);
            if (array == NULL) {
                array = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&baseValue));
            }
            if (array == NULL || index < 0 || (uint64_t)index >= (uint64_t)array->len) {
                goto unary_addr_fallback;
            }
            H2EvalValueSetReference(outValue, &array->elems[(uint32_t)index]);
            *outIsConst = 1;
            return 0;
        unary_addr_fallback:;
        }
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (childValue.kind == H2CTFEValue_AGGREGATE || childValue.kind == H2CTFEValue_NULL
            || childValue.kind == H2CTFEValue_ARRAY)
        {
            *outValue = childValue;
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == H2Ast_UNARY && (H2TokenKind)n->op == H2Tok_MUL) {
        int32_t      childNode = n->firstChild;
        H2CTFEValue  childValue;
        H2CTFEValue* target;
        int          childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        target = H2EvalValueReferenceTarget(&childValue);
        if (target != NULL) {
            *outValue = *target;
            *outIsConst = 1;
            return 0;
        }
    }

    if (n->kind == H2Ast_UNWRAP) {
        int32_t            childNode = n->firstChild;
        H2CTFEValue        childValue;
        const H2CTFEValue* payload = NULL;
        int                childIsConst = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        if (!H2EvalOptionalPayload(&childValue, &payload)) {
            if (childValue.kind == H2CTFEValue_NULL) {
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

    if (n->kind == H2Ast_TUPLE_EXPR || n->kind == H2Ast_EXPR_LIST) {
        H2CTFEValue elems[256];
        uint32_t    elemCount = AstListCount(ast, exprNode);
        uint32_t    i;
        if (elemCount == 0 || elemCount > 256u) {
            *outIsConst = 0;
            return 0;
        }
        for (i = 0; i < elemCount; i++) {
            int32_t itemNode = AstListItemAt(ast, exprNode, i);
            int     elemIsConst = 0;
            if (itemNode < 0 || H2EvalExecExprCb(p, itemNode, &elems[i], &elemIsConst) != 0) {
                return itemNode < 0 ? 0 : -1;
            }
            if (!elemIsConst) {
                *outIsConst = 0;
                return 0;
            }
        }
        return H2EvalAllocTupleValue(
            p, p->currentFile, exprNode, elems, elemCount, outValue, outIsConst);
    }

    if (n->kind == H2Ast_TYPE_VALUE) {
        int32_t typeNode = ASTFirstChild(ast, exprNode);
        int     rc = H2EvalTypeValueFromTypeNode(p, p->currentFile, typeNode, outValue);
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

    if (n->kind == H2Ast_UNARY) {
        int32_t     childNode = n->firstChild;
        H2CTFEValue childValue;
        int         childIsConst = 0;
        int         handled = 0;
        if (childNode < 0 || (uint32_t)childNode >= ast->len) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, childNode, &childValue, &childIsConst) != 0) {
            return -1;
        }
        if (!childIsConst) {
            *outIsConst = 0;
            return 0;
        }
        handled = H2EvalEvalUnary((H2TokenKind)n->op, &childValue, outValue, outIsConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            return 0;
        }
    }

    if (n->kind == H2Ast_BINARY && (H2TokenKind)n->op != H2Tok_ASSIGN) {
        int32_t     lhsNode = n->firstChild;
        int32_t     rhsNode = lhsNode >= 0 ? ast->nodes[lhsNode].nextSibling : -1;
        H2CTFEValue lhsValue;
        H2CTFEValue rhsValue;
        int         lhsIsConst = 0;
        int         rhsIsConst = 0;
        int         handled = 0;
        if (lhsNode < 0 || rhsNode < 0 || ast->nodes[rhsNode].nextSibling >= 0) {
            *outIsConst = 0;
            return 0;
        }
        if (H2EvalExecExprCb(p, lhsNode, &lhsValue, &lhsIsConst) != 0
            || H2EvalExecExprCb(p, rhsNode, &rhsValue, &rhsIsConst) != 0)
        {
            return -1;
        }
        if (!lhsIsConst || !rhsIsConst) {
            *outIsConst = 0;
            return 0;
        }
        handled = H2EvalEvalBinary(
            p, (H2TokenKind)n->op, &lhsValue, &rhsValue, outValue, outIsConst);
        if (handled < 0) {
            return -1;
        }
        if (handled > 0) {
            return 0;
        }
    }

    if (n->kind == H2Ast_CALL) {
        int32_t calleeNode = n->firstChild;
        if (calleeNode < 0 || (uint32_t)calleeNode >= ast->len) {
            if (p->currentExecCtx != NULL) {
                H2CTFEExecSetReasonNode(
                    p->currentExecCtx, exprNode, "call expression is malformed");
            }
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == H2Ast_IDENT
            && SliceEqCStr(
                p->currentFile->source,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                "typeof"))
        {
            H2CTFEExecBinding*  binding = NULL;
            const H2ParsedFile* localTypeFile = NULL;
            int32_t             argNode = ast->nodes[calleeNode].nextSibling;
            int32_t             argExprNode = argNode;
            int32_t             localTypeNode = -1;
            int32_t             visibleLocalTypeNode = -1;
            H2CTFEValue         argValue;
            int                 argIsConst = 0;
            int32_t             typeCode = H2EvalTypeCode_INVALID;
            if (argNode < 0 || ast->nodes[argNode].nextSibling >= 0) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[argNode].kind == H2Ast_CALL_ARG) {
                argExprNode = ast->nodes[argNode].firstChild;
            }
            if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
                *outIsConst = 0;
                return 0;
            }
            if (ast->nodes[argExprNode].kind == H2Ast_IDENT) {
                binding = H2EvalFindBinding(
                    p->currentExecCtx,
                    p->currentFile,
                    ast->nodes[argExprNode].dataStart,
                    ast->nodes[argExprNode].dataEnd);
                if (binding != NULL && binding->typeNode >= 0
                    && !H2EvalTypeNodeIsAnytype(p->currentFile, binding->typeNode))
                {
                    rc = H2EvalTypeValueFromTypeNode(
                        p, p->currentFile, binding->typeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
                if (H2EvalFindVisibleLocalTypeNodeByName(
                        p->currentFile,
                        ast->nodes[argExprNode].start,
                        ast->nodes[argExprNode].dataStart,
                        ast->nodes[argExprNode].dataEnd,
                        &visibleLocalTypeNode)
                    && visibleLocalTypeNode >= 0
                    && !H2EvalTypeNodeIsAnytype(p->currentFile, visibleLocalTypeNode))
                {
                    rc = H2EvalTypeValueFromTypeNode(
                        p, p->currentFile, visibleLocalTypeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
                if (H2EvalMirLookupLocalTypeNode(
                        p,
                        ast->nodes[argExprNode].dataStart,
                        ast->nodes[argExprNode].dataEnd,
                        &localTypeFile,
                        &localTypeNode)
                    && localTypeFile != NULL && localTypeNode >= 0
                    && !H2EvalTypeNodeIsAnytype(localTypeFile, localTypeNode))
                {
                    rc = H2EvalTypeValueFromTypeNode(p, localTypeFile, localTypeNode, outValue);
                    if (rc < 0) {
                        return -1;
                    }
                    if (rc > 0) {
                        *outIsConst = 1;
                        return 0;
                    }
                }
            }
            rc = H2EvalTypeValueFromExprNode(p, p->currentFile, ast, argExprNode, outValue);
            if (rc < 0) {
                return -1;
            }
            if (rc > 0) {
                if (outValue->kind == H2CTFEValue_TYPE) {
                    H2EvalValueSetSimpleTypeValue(outValue, H2EvalTypeCode_TYPE);
                }
                *outIsConst = 1;
                return 0;
            }
            if (ast->nodes[argExprNode].kind == H2Ast_CAST) {
                int32_t typeNode = ast->nodes[argExprNode].firstChild;
                typeNode = typeNode >= 0 ? ast->nodes[typeNode].nextSibling : -1;
                if (typeNode >= 0
                    && H2EvalTypeCodeFromTypeNode(p->currentFile, typeNode, &typeCode))
                {
                    H2EvalValueSetSimpleTypeValue(outValue, typeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (H2EvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
                return -1;
            }
            if (!argIsConst) {
                *outIsConst = 0;
                return 0;
            }
            H2EvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
            {
                H2EvalAggregate* agg = H2EvalValueAsAggregate(H2EvalValueTargetOrSelf(&argValue));
                if (agg != NULL && agg->file != NULL && agg->nodeId >= 0
                    && (uint32_t)agg->nodeId < agg->file->ast.len)
                {
                    uint8_t namedKind = 0;
                    switch (agg->file->ast.nodes[agg->nodeId].kind) {
                        case H2Ast_STRUCT: namedKind = H2EvalTypeKind_STRUCT; break;
                        case H2Ast_UNION:  namedKind = H2EvalTypeKind_UNION; break;
                        case H2Ast_ENUM:   namedKind = H2EvalTypeKind_ENUM; break;
                        default:           break;
                    }
                    if (namedKind != 0
                        && H2EvalMakeNamedTypeValue(p, agg->file, agg->nodeId, namedKind, outValue)
                               > 0)
                    {
                        *outIsConst = 1;
                        return 0;
                    }
                }
            }
            if (!H2EvalTypeCodeFromValue(&argValue, &typeCode)) {
                *outIsConst = 0;
                return 0;
            }
            H2EvalValueSetSimpleTypeValue(outValue, typeCode);
            *outIsConst = 1;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == H2Ast_IDENT
            && H2EvalNameEqLiteralOrPkgBuiltin(
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
            if (ast->nodes[operandNode].kind == H2Ast_CALL_ARG) {
                operandNode = ast->nodes[operandNode].firstChild;
            }
            if (operandNode < 0 || (uint32_t)operandNode >= ast->len) {
                if (p->currentExecCtx != NULL) {
                    H2CTFEExecSetReason(
                        p->currentExecCtx,
                        ast->nodes[argNode].start,
                        ast->nodes[argNode].end,
                        "source_location_of argument is malformed");
                }
                *outIsConst = 0;
                return 0;
            }
            H2EvalValueSetSpan(
                p->currentFile,
                ast->nodes[operandNode].start,
                ast->nodes[operandNode].end,
                outValue);
            *outIsConst = 1;
            return 0;
        }
        if (calleeNode >= 0 && ast->nodes[calleeNode].kind == H2Ast_FIELD_EXPR) {
            const H2AstNode* callee = &ast->nodes[calleeNode];
            int32_t          baseNode = callee->firstChild;
            int32_t          argNode = ast->nodes[calleeNode].nextSibling;
            H2CTFEValue      calleeValue;
            int              calleeIsConst = 0;
            if (H2EvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                return -1;
            }
            if (calleeIsConst && H2EvalValueIsInvokableFunctionRef(&calleeValue)) {
                uint32_t     argCount = 0;
                H2CTFEValue* args = NULL;
                int collectRc = H2EvalCollectCallArgs(p, ast, argNode, &args, &argCount, NULL);
                if (collectRc <= 0) {
                    *outIsConst = 0;
                    return collectRc < 0 ? -1 : 0;
                }
                {
                    const H2ParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                    int32_t             savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                    const H2ParsedFile* inferredExpectedTypeFile = NULL;
                    int32_t             inferredExpectedTypeNode = -1;
                    int                 invoked;
                    if (p->expectedCallExprFile == p->currentFile
                        && p->expectedCallExprNode == exprNode)
                    {
                        p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                        p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                    } else if (
                        H2EvalFindExpectedTypeForCallExpr(
                            p->currentFile,
                            exprNode,
                            &inferredExpectedTypeFile,
                            &inferredExpectedTypeNode))
                    {
                        p->activeCallExpectedTypeFile = inferredExpectedTypeFile;
                        p->activeCallExpectedTypeNode = inferredExpectedTypeNode;
                    }
                    invoked = H2EvalInvokeFunctionRef(
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
                && ast->nodes[baseNode].kind == H2Ast_IDENT
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
                if (ast->nodes[operandNode].kind == H2Ast_CALL_ARG) {
                    operandNode = ast->nodes[operandNode].firstChild;
                }
                if (operandNode < 0 || (uint32_t)operandNode >= ast->len) {
                    if (p->currentExecCtx != NULL) {
                        H2CTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[argNode].start,
                            ast->nodes[argNode].end,
                            "builtin.source_location_of argument is malformed");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                H2EvalValueSetSpan(
                    p->currentFile,
                    ast->nodes[operandNode].start,
                    ast->nodes[operandNode].end,
                    outValue);
                *outIsConst = 1;
                return 0;
            }
            if (baseNode >= 0 && argNode >= 0 && ast->nodes[argNode].nextSibling < 0
                && ast->nodes[baseNode].kind == H2Ast_IDENT
                && SliceEqCStr(p->currentFile->source, callee->dataStart, callee->dataEnd, "exit")
                && SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "platform"))
            {
                H2CTFEValue argValue;
                int         argIsConst = 0;
                int64_t     exitCode = 0;
                if (H2EvalExecExprCb(p, argNode, &argValue, &argIsConst) != 0) {
                    return -1;
                }
                if (!argIsConst || H2CTFEValueToInt64(&argValue, &exitCode) != 0) {
                    if (p->currentExecCtx != NULL) {
                        H2CTFEExecSetReason(
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
                H2EvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (baseNode >= 0 && argNode >= 0 && ast->nodes[baseNode].kind == H2Ast_IDENT
                && SliceEqCStr(
                    p->currentFile->source,
                    ast->nodes[baseNode].dataStart,
                    ast->nodes[baseNode].dataEnd,
                    "platform")
                && SliceEqCStr(
                    p->currentFile->source, callee->dataStart, callee->dataEnd, "console_log"))
            {
                int32_t     flagsNode = ast->nodes[argNode].nextSibling;
                int32_t     extraNode = flagsNode >= 0 ? ast->nodes[flagsNode].nextSibling : -1;
                int32_t     messageExpr = ASTFirstChild(ast, argNode);
                int32_t     flagsExpr = ASTFirstChild(ast, flagsNode);
                H2CTFEValue messageValue;
                H2CTFEValue flagsValue;
                int         messageIsConst = 0;
                int         flagsIsConst = 0;
                if (flagsNode < 0 || extraNode >= 0 || messageExpr < 0 || flagsExpr < 0) {
                    *outIsConst = 0;
                    return 0;
                }
                if (H2EvalExecExprCb(p, messageExpr, &messageValue, &messageIsConst) != 0
                    || H2EvalExecExprCb(p, flagsExpr, &flagsValue, &flagsIsConst) != 0)
                {
                    return -1;
                }
                if (!messageIsConst || messageValue.kind != H2CTFEValue_STRING || !flagsIsConst) {
                    if (p->currentExecCtx != NULL) {
                        H2CTFEExecSetReason(
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
                H2EvalValueSetNull(outValue);
                *outIsConst = 1;
                return 0;
            }
            if (baseNode >= 0) {
                uint32_t     extraArgCount = 0;
                H2CTFEValue* args = NULL;
                H2CTFEValue* extraArgs = NULL;
                H2CTFEValue  baseValue;
                int          baseIsConst = 0;
                int          collectRc;
                if (H2EvalExecExprCb(p, baseNode, &baseValue, &baseIsConst) != 0) {
                    return -1;
                }
                if (baseIsConst) {
                    collectRc = H2EvalCollectCallArgs(
                        p, ast, argNode, &extraArgs, &extraArgCount, NULL);
                    if (collectRc <= 0) {
                        *outIsConst = 0;
                        return collectRc < 0 ? -1 : 0;
                    }
                    args = (H2CTFEValue*)H2ArenaAlloc(
                        p->arena,
                        sizeof(H2CTFEValue) * (extraArgCount + 1u),
                        (uint32_t)_Alignof(H2CTFEValue));
                    if (args == NULL) {
                        return ErrorSimple("out of memory");
                    }
                    args[0] = baseValue;
                    if (extraArgCount > 0 && extraArgs != NULL) {
                        memcpy(args + 1u, extraArgs, sizeof(H2CTFEValue) * extraArgCount);
                    }

                    if (extraArgCount == 0
                        && SliceEqCStr(
                            p->currentFile->source, callee->dataStart, callee->dataEnd, "len")
                        && (H2EvalValueTargetOrSelf(&baseValue)->kind == H2CTFEValue_STRING
                            || H2EvalValueTargetOrSelf(&baseValue)->kind == H2CTFEValue_ARRAY
                            || H2EvalValueTargetOrSelf(&baseValue)->kind == H2CTFEValue_NULL))
                    {
                        H2EvalValueSetInt(
                            outValue, (int64_t)H2EvalValueTargetOrSelf(&baseValue)->s.len);
                        *outIsConst = 1;
                        return 0;
                    }
                    if (extraArgCount == 0
                        && SliceEqCStr(
                            p->currentFile->source, callee->dataStart, callee->dataEnd, "cstr")
                        && H2EvalValueTargetOrSelf(&baseValue)->kind == H2CTFEValue_STRING)
                    {
                        *outValue = *H2EvalValueTargetOrSelf(&baseValue);
                        *outIsConst = 1;
                        return 0;
                    }

                    {
                        const H2ParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                        int32_t             savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                        if (p->expectedCallExprFile == p->currentFile
                            && p->expectedCallExprNode == exprNode)
                        {
                            p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                            p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                        }
                        if (H2EvalResolveCall(
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
                        H2CTFEExecSetReasonNode(
                            p->currentExecCtx,
                            exprNode,
                            "qualified call target is not supported by evaluator backend");
                    }
                    return 0;
                }
            }
            if (p->currentExecCtx != NULL) {
                H2CTFEExecSetReasonNode(
                    p->currentExecCtx,
                    exprNode,
                    "qualified call target is not supported by evaluator backend");
            }
            *outIsConst = 0;
            return 0;
        }
        if (ast->nodes[calleeNode].kind == H2Ast_IDENT) {
            int32_t      argNode = ast->nodes[calleeNode].nextSibling;
            uint32_t     argCount = 0;
            H2CTFEValue* args = NULL;
            H2CTFEValue  tempArgs[256];
            int32_t      directFnIndex = -1;
            H2CTFEValue  calleeValue;
            int          calleeIsConst = 0;
            int          calleeMayResolveByNameWithoutValue = 0;
            int32_t      scanNode;
            uint32_t     rawArgCount = 0;
            for (scanNode = argNode; scanNode >= 0; scanNode = ast->nodes[scanNode].nextSibling) {
                rawArgCount++;
            }
            directFnIndex = H2EvalResolveFunctionBySlice(
                p,
                NULL,
                p->currentFile,
                ast->nodes[calleeNode].dataStart,
                ast->nodes[calleeNode].dataEnd,
                NULL,
                rawArgCount);
            scanNode = argNode;
            while (scanNode >= 0) {
                H2CTFEValue         argValue;
                int                 argIsConst = 0;
                int32_t             argExprNode = scanNode;
                int32_t             paramTypeNode = -1;
                const H2ParsedFile* paramTypeFile = p->currentFile;
                if (ast->nodes[scanNode].kind == H2Ast_CALL_ARG) {
                    argExprNode = ast->nodes[scanNode].firstChild;
                }
                if (argExprNode < 0 || (uint32_t)argExprNode >= ast->len) {
                    if (p->currentExecCtx != NULL) {
                        H2CTFEExecSetReason(
                            p->currentExecCtx,
                            ast->nodes[scanNode].start,
                            ast->nodes[scanNode].end,
                            "call argument is malformed");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                if (directFnIndex >= 0) {
                    const H2EvalFunction* directFn = &p->funcs[(uint32_t)directFnIndex];
                    uint32_t              fixedCount =
                        directFn->isVariadic && directFn->paramCount > 0
                            ? directFn->paramCount - 1u
                            : directFn->paramCount;
                    if (!(ast->nodes[scanNode].kind == H2Ast_CALL_ARG
                          && (ast->nodes[scanNode].flags & H2AstFlag_CALL_ARG_SPREAD) != 0)
                        && (!directFn->isVariadic || argCount < fixedCount))
                    {
                        paramTypeNode = H2EvalFunctionParamTypeNodeAt(directFn, argCount);
                        paramTypeFile = directFn->file;
                        if (paramTypeNode >= 0
                            && directFn->file->ast.nodes[paramTypeNode].kind == H2Ast_TYPE_NAME
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
                    if (H2EvalExecExprWithTypeNode(
                            p, argExprNode, paramTypeFile, paramTypeNode, &argValue, &argIsConst)
                        != 0)
                    {
                        return -1;
                    }
                } else if (H2EvalExecExprCb(p, argExprNode, &argValue, &argIsConst) != 0) {
                    return -1;
                }
                if (!argIsConst) {
                    *outIsConst = 0;
                    return 0;
                }
                if (paramTypeNode >= 0 && H2EvalExprIsAnytypePackIndex(p, ast, argExprNode)
                    && !H2EvalValueMatchesExpectedTypeNode(
                        p, paramTypeFile, paramTypeNode, &argValue))
                {
                    if (p->currentExecCtx != NULL) {
                        H2CTFEExecSetReasonNode(
                            p->currentExecCtx, argExprNode, "anytype pack element type mismatch");
                    }
                    *outIsConst = 0;
                    return 0;
                }
                H2EvalAnnotateValueTypeFromExpr(p->currentFile, ast, argExprNode, &argValue);
                if (ast->nodes[scanNode].kind == H2Ast_CALL_ARG
                    && (ast->nodes[scanNode].flags & H2AstFlag_CALL_ARG_SPREAD) != 0)
                {
                    H2EvalArray* array = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&argValue));
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
                args = (H2CTFEValue*)H2ArenaAlloc(
                    p->arena, sizeof(H2CTFEValue) * argCount, (uint32_t)_Alignof(H2CTFEValue));
                if (args == NULL) {
                    return ErrorSimple("out of memory");
                }
                memcpy(args, tempArgs, sizeof(H2CTFEValue) * argCount);
            }
            {
                int32_t resolvedFnIndex = H2EvalResolveFunctionBySlice(
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
                (void)H2EvalReorderFixedCallArgsByName(
                    p, &p->funcs[(uint32_t)directFnIndex], ast, argNode, args, argCount, 0u);
            }
            calleeMayResolveByNameWithoutValue =
                directFnIndex >= 0
                || H2EvalNameIsLazyTypeBuiltin(
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
                if (H2EvalExecExprCb(p, calleeNode, &calleeValue, &calleeIsConst) != 0) {
                    return -1;
                }
                if (calleeIsConst && H2EvalValueIsInvokableFunctionRef(&calleeValue)) {
                    const H2ParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                    int32_t             savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                    const H2ParsedFile* inferredExpectedTypeFile = NULL;
                    int32_t             inferredExpectedTypeNode = -1;
                    int                 invoked;
                    if (p->expectedCallExprFile == p->currentFile
                        && p->expectedCallExprNode == exprNode)
                    {
                        p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                        p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                    } else if (
                        H2EvalFindExpectedTypeForCallExpr(
                            p->currentFile,
                            exprNode,
                            &inferredExpectedTypeFile,
                            &inferredExpectedTypeNode))
                    {
                        p->activeCallExpectedTypeFile = inferredExpectedTypeFile;
                        p->activeCallExpectedTypeNode = inferredExpectedTypeNode;
                    }
                    invoked = H2EvalInvokeFunctionRef(
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
                const H2ParsedFile* savedExpectedTypeFile = p->activeCallExpectedTypeFile;
                int32_t             savedExpectedTypeNode = p->activeCallExpectedTypeNode;
                if (p->expectedCallExprFile == p->currentFile
                    && p->expectedCallExprNode == exprNode)
                {
                    p->activeCallExpectedTypeFile = p->expectedCallTypeFile;
                    p->activeCallExpectedTypeNode = p->expectedCallTypeNode;
                }
                if (H2EvalResolveCall(
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
                H2CTFEExecSetReasonNode(
                    p->currentExecCtx, exprNode, "call is not supported by evaluator backend");
            }
            return 0;
        }
    }
    if (n->kind == H2Ast_CAST) {
        int32_t  valueNode = n->firstChild;
        int32_t  typeNode = valueNode >= 0 ? ast->nodes[valueNode].nextSibling : -1;
        int32_t  extraNode = typeNode >= 0 ? ast->nodes[typeNode].nextSibling : -1;
        char     aliasTargetKind = '\0';
        uint64_t aliasTag = 0;
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0
            && H2EvalResolveSimpleAliasCastTarget(
                p, p->currentFile, typeNode, &aliasTargetKind, &aliasTag))
        {
            H2CTFEValue inValue;
            int         inIsConst = 0;
            if (H2EvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (aliasTargetKind == 'i' && inValue.kind == H2CTFEValue_INT) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 'f' && inValue.kind == H2CTFEValue_FLOAT) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 'b' && inValue.kind == H2CTFEValue_BOOL) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
            if (aliasTargetKind == 's' && inValue.kind == H2CTFEValue_STRING) {
                *outValue = inValue;
                outValue->typeTag = aliasTag;
                *outIsConst = 1;
                return 0;
            }
        }
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0) {
            H2CTFEValue         inValue;
            int                 inIsConst = 0;
            const H2ParsedFile* aliasFile = NULL;
            int32_t             aliasNode = -1;
            int32_t             aliasTargetNode = -1;
            H2EvalArray*        tuple;
            if (H2EvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            if (H2EvalResolveAliasCastTargetNode(
                    p, p->currentFile, typeNode, &aliasFile, &aliasNode, &aliasTargetNode)
                && aliasFile != NULL && aliasNode >= 0 && aliasTargetNode >= 0)
            {
                tuple = H2EvalValueAsArray(H2EvalValueTargetOrSelf(&inValue));
                if (tuple != NULL && aliasFile->ast.nodes[aliasTargetNode].kind == H2Ast_TYPE_TUPLE
                    && AstListCount(&aliasFile->ast, aliasTargetNode) == tuple->len)
                {
                    *outValue = inValue;
                    outValue->typeTag = H2EvalMakeAliasTag(aliasFile, aliasNode);
                    *outIsConst = 1;
                    return 0;
                }
            }
        }
        if (valueNode >= 0 && typeNode >= 0 && extraNode < 0) {
            H2CTFEValue inValue;
            int         inIsConst = 0;
            int32_t     targetTypeCode = H2EvalTypeCode_INVALID;
            uint64_t    nullTypeTag = 0;
            if (H2EvalExecExprCb(p, valueNode, &inValue, &inIsConst) != 0) {
                return -1;
            }
            if (!inIsConst) {
                *outIsConst = 0;
                return 0;
            }
            (void)H2EvalTypeCodeFromTypeNode(p->currentFile, typeNode, &targetTypeCode);
            if (inValue.kind == H2CTFEValue_NULL
                && H2EvalResolveNullCastTypeTag(p->currentFile, typeNode, &nullTypeTag))
            {
                *outValue = inValue;
                outValue->typeTag = nullTypeTag;
                if (targetTypeCode == H2EvalTypeCode_RAWPTR) {
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                }
                *outIsConst = 1;
                return 0;
            }
            if (targetTypeCode == H2EvalTypeCode_RAWPTR
                && (inValue.kind == H2CTFEValue_REFERENCE || inValue.kind == H2CTFEValue_STRING))
            {
                *outValue = inValue;
                H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                *outIsConst = 1;
                return 0;
            }
            if (targetTypeCode == H2EvalTypeCode_BOOL) {
                outValue->kind = H2CTFEValue_BOOL;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (inValue.kind == H2CTFEValue_BOOL) {
                    outValue->b = inValue.b ? 1u : 0u;
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == H2CTFEValue_INT) {
                    outValue->b = inValue.i64 != 0 ? 1u : 0u;
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == H2CTFEValue_FLOAT) {
                    outValue->b = inValue.f64 != 0.0 ? 1u : 0u;
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == H2CTFEValue_STRING) {
                    outValue->b = 1u;
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == H2CTFEValue_NULL) {
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (targetTypeCode == H2EvalTypeCode_F32 || targetTypeCode == H2EvalTypeCode_F64) {
                outValue->kind = H2CTFEValue_FLOAT;
                outValue->i64 = 0;
                outValue->f64 = 0.0;
                outValue->b = 0;
                outValue->s.bytes = NULL;
                outValue->s.len = 0;
                if (inValue.kind == H2CTFEValue_FLOAT) {
                    outValue->f64 = inValue.f64;
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == H2CTFEValue_INT) {
                    outValue->f64 = (double)inValue.i64;
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == H2CTFEValue_BOOL) {
                    outValue->f64 = inValue.b ? 1.0 : 0.0;
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
                if (inValue.kind == H2CTFEValue_NULL) {
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if (targetTypeCode == H2EvalTypeCode_U8 || targetTypeCode == H2EvalTypeCode_U16
                || targetTypeCode == H2EvalTypeCode_U32 || targetTypeCode == H2EvalTypeCode_U64
                || targetTypeCode == H2EvalTypeCode_UINT || targetTypeCode == H2EvalTypeCode_I8
                || targetTypeCode == H2EvalTypeCode_I16 || targetTypeCode == H2EvalTypeCode_I32
                || targetTypeCode == H2EvalTypeCode_I64 || targetTypeCode == H2EvalTypeCode_INT)
            {
                int64_t asInt = 0;
                int     canCast = 1;
                if (inValue.kind == H2CTFEValue_INT) {
                    asInt = inValue.i64;
                } else if (inValue.kind == H2CTFEValue_BOOL) {
                    asInt = inValue.b ? 1 : 0;
                } else if (inValue.kind == H2CTFEValue_FLOAT) {
                    if (inValue.f64 != inValue.f64 || inValue.f64 > (double)INT64_MAX
                        || inValue.f64 < (double)INT64_MIN)
                    {
                        *outIsConst = 0;
                        return 0;
                    }
                    asInt = (int64_t)inValue.f64;
                } else if (inValue.kind == H2CTFEValue_NULL) {
                    asInt = 0;
                } else {
                    canCast = 0;
                }
                if (canCast) {
                    H2EvalValueSetInt(outValue, asInt);
                    H2EvalValueSetRuntimeTypeCode(outValue, targetTypeCode);
                    *outIsConst = 1;
                    return 0;
                }
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_MUTREF
                 || p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_PTR)
                && inValue.kind == H2CTFEValue_REFERENCE)
            {
                *outValue = inValue;
                *outIsConst = 1;
                return 0;
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_MUTREF
                 || p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_PTR)
                && p->currentFile->ast.nodes[typeNode].firstChild >= 0
                && (uint32_t)p->currentFile->ast.nodes[typeNode].firstChild < ast->len
                && p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild].kind
                       == H2Ast_TYPE_NAME
                && SliceEqCStr(
                    p->currentFile->source,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataStart,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataEnd,
                    "str")
                && inValue.kind == H2CTFEValue_STRING)
            {
                *outValue = inValue;
                H2EvalValueSetRuntimeTypeCode(
                    outValue,
                    p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_REF
                        ? H2EvalTypeCode_STR_REF
                        : H2EvalTypeCode_STR_PTR);
                *outIsConst = 1;
                return 0;
            }
            if ((p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_REF
                 || p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_PTR)
                && p->currentFile->ast.nodes[typeNode].firstChild >= 0
                && (uint32_t)p->currentFile->ast.nodes[typeNode].firstChild < ast->len
                && p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild].kind
                       == H2Ast_TYPE_NAME
                && SliceEqCStr(
                    p->currentFile->source,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataStart,
                    p->currentFile->ast.nodes[p->currentFile->ast.nodes[typeNode].firstChild]
                        .dataEnd,
                    "str")
                && H2EvalValueAsArray(H2EvalValueTargetOrSelf(&inValue)) != NULL)
            {
                rc = H2EvalStringValueFromArrayBytes(
                    p->arena,
                    &inValue,
                    p->currentFile->ast.nodes[typeNode].kind == H2Ast_TYPE_REF
                        ? H2EvalTypeCode_STR_REF
                        : H2EvalTypeCode_STR_PTR,
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

    rc = H2CTFEEvalExprEx(
        p->arena,
        ast,
        (H2StrView){ p->currentFile->source, p->currentFile->sourceLen },
        exprNode,
        H2EvalResolveIdent,
        H2EvalResolveCall,
        p,
        H2EvalMirMakeTuple,
        p,
        H2EvalMirIndexValue,
        p,
        H2EvalMirAggGetField,
        p,
        H2EvalMirAggAddrField,
        p,
        outValue,
        outIsConst,
        &diag);
    if (rc != 0) {
        if (diag.code != H2Diag_NONE) {
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
        H2CTFEExecSetReasonNode(
            p->currentExecCtx, exprNode, "expression is not supported by evaluator backend");
    }
    return 0;
}

int RunProgramEval(
    const char* entryPath,
    const char* _Nullable platformTarget,
    const char* _Nullable archTarget,
    int testingBuild) {
    H2PackageLoader loader;
    H2Package*      entryPkg;
    H2EvalProgram   program;
    H2EvalFunction* mainFn = NULL;
    uint32_t        i;
    int32_t         mainIndex = -1;
    uint8_t         arenaMem[32 * 1024];
    H2Arena         arena;
    H2CTFEValue     retValue;
    H2CTFEValue     noArgsValue;
    int             didReturn = 0;
    int             rc = -1;

    if (LoadAndCheckPackage(entryPath, platformTarget, archTarget, testingBuild, &loader, &entryPkg)
        != 0)
    {
        return -1;
    }
    if (ValidateEntryMainSignature(entryPkg) != 0) {
        FreeLoader(&loader);
        return -1;
    }

    H2ArenaInit(&arena, arenaMem, (uint32_t)sizeof(arenaMem));
    H2ArenaSetAllocator(&arena, NULL, CodegenArenaGrow, CodegenArenaFree);
    memset(&program, 0, sizeof(program));
    program.arena = &arena;
    program.loader = &loader;
    program.entryPkg = entryPkg;
    H2EvalValueSetInt(&program.rootContext.allocator, 1);
    H2EvalValueSetInt(&program.rootContext.tempAllocator, 2);
    H2EvalValueSetInt(&program.rootContext.logger, 3);
    program.currentContext = NULL;
    if (H2EvalCollectFunctions(&program) != 0) {
        goto end;
    }
    if (H2EvalCollectTopConsts(&program) != 0) {
        goto end;
    }
    if (H2EvalCollectTopVars(&program) != 0) {
        goto end;
    }
    for (i = 0; i < program.funcLen; i++) {
        H2EvalFunction* fn = &program.funcs[i];
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

    H2EvalValueSetNull(&noArgsValue);
    if (H2EvalInvokeFunction(
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
    H2ArenaDispose(&arena);
    FreeLoader(&loader);
    return rc;
}

H2_API_END
