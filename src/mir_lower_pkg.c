#include "libhop-impl.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"

HOP_API_BEGIN

static void HOPMirLowerPkgSetDiag(
    HOPDiag* _Nullable diag, HOPDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = HOPDiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
}

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} HOPMirLowerPkgVarLikeParts;

static int HOPMirLowerPkgIsTypeNodeKind(HOPAstKind kind) {
    return kind == HOPAst_TYPE_NAME || kind == HOPAst_TYPE_PTR || kind == HOPAst_TYPE_REF
        || kind == HOPAst_TYPE_MUTREF || kind == HOPAst_TYPE_ARRAY || kind == HOPAst_TYPE_VARRAY
        || kind == HOPAst_TYPE_SLICE || kind == HOPAst_TYPE_MUTSLICE || kind == HOPAst_TYPE_OPTIONAL
        || kind == HOPAst_TYPE_FN || kind == HOPAst_TYPE_TUPLE || kind == HOPAst_TYPE_ANON_STRUCT
        || kind == HOPAst_TYPE_ANON_UNION;
}

static uint32_t HOPMirLowerPkgAstListCount(const HOPAst* ast, int32_t listNode) {
    uint32_t count = 0;
    int32_t  child;
    if (ast == NULL || listNode < 0 || (uint32_t)listNode >= ast->len) {
        return 0;
    }
    child = ast->nodes[listNode].firstChild;
    while (child >= 0) {
        count++;
        child = ast->nodes[child].nextSibling;
    }
    return count;
}

static int32_t HOPMirLowerPkgAstListItemAt(const HOPAst* ast, int32_t listNode, uint32_t index) {
    uint32_t i = 0;
    int32_t  child;
    if (ast == NULL || listNode < 0 || (uint32_t)listNode >= ast->len) {
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

static int HOPMirLowerPkgNameEqSlice(
    HOPStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
    uint32_t len;
    if (aEnd < aStart || bEnd < bStart || aEnd > src.len || bEnd > src.len) {
        return 0;
    }
    len = aEnd - aStart;
    if (len != bEnd - bStart) {
        return 0;
    }
    return len == 0 || memcmp(src.ptr + aStart, src.ptr + bStart, len) == 0;
}

static int HOPMirLowerPkgVarLikeGetParts(
    const HOPAst* _Nonnull ast, int32_t nodeId, HOPMirLowerPkgVarLikeParts* _Nonnull out) {
    int32_t           firstChild;
    const HOPAstNode* firstNode;
    out->nameListNode = -1;
    out->typeNode = -1;
    out->initNode = -1;
    out->nameCount = 0;
    out->grouped = 0;
    if (ast == NULL || nodeId < 0 || (uint32_t)nodeId >= ast->len) {
        return -1;
    }
    firstChild = ast->nodes[nodeId].firstChild;
    if (firstChild < 0) {
        return 0;
    }
    firstNode = &ast->nodes[firstChild];
    if (firstNode->kind == HOPAst_NAME_LIST) {
        int32_t afterNames = ast->nodes[firstChild].nextSibling;
        out->grouped = 1;
        out->nameListNode = firstChild;
        out->nameCount = HOPMirLowerPkgAstListCount(ast, firstChild);
        if (afterNames >= 0 && HOPMirLowerPkgIsTypeNodeKind(ast->nodes[afterNames].kind)) {
            out->typeNode = afterNames;
            out->initNode = ast->nodes[afterNames].nextSibling;
        } else {
            out->initNode = afterNames;
        }
        return 0;
    }
    out->grouped = 0;
    out->nameCount = 1;
    if (HOPMirLowerPkgIsTypeNodeKind(firstNode->kind)) {
        out->typeNode = firstChild;
        out->initNode = ast->nodes[firstChild].nextSibling;
    } else {
        out->initNode = firstChild;
    }
    return 0;
}

static int32_t HOPMirLowerPkgVarLikeNameIndexBySlice(
    const HOPAst* _Nonnull ast, HOPStrView src, int32_t nodeId, uint32_t start, uint32_t end) {
    HOPMirLowerPkgVarLikeParts parts;
    uint32_t                   i;
    if (HOPMirLowerPkgVarLikeGetParts(ast, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (!parts.grouped) {
        return HOPMirLowerPkgNameEqSlice(
                   src, ast->nodes[nodeId].dataStart, ast->nodes[nodeId].dataEnd, start, end)
                 ? 0
                 : -1;
    }
    for (i = 0; i < parts.nameCount; i++) {
        int32_t item = HOPMirLowerPkgAstListItemAt(ast, parts.nameListNode, i);
        if (item >= 0
            && HOPMirLowerPkgNameEqSlice(
                src, ast->nodes[item].dataStart, ast->nodes[item].dataEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t HOPMirLowerPkgVarLikeInitExprNodeAt(
    const HOPAst* _Nonnull ast, int32_t nodeId, int32_t nameIndex) {
    HOPMirLowerPkgVarLikeParts parts;
    uint32_t                   initCount;
    int32_t                    onlyInit;
    if (HOPMirLowerPkgVarLikeGetParts(ast, nodeId, &parts) != 0) {
        return -1;
    }
    if (!parts.grouped) {
        return nameIndex == 0 ? parts.initNode : -1;
    }
    if (parts.initNode < 0 || ast->nodes[parts.initNode].kind != HOPAst_EXPR_LIST || nameIndex < 0)
    {
        return -1;
    }
    initCount = HOPMirLowerPkgAstListCount(ast, parts.initNode);
    if (initCount == parts.nameCount) {
        return HOPMirLowerPkgAstListItemAt(ast, parts.initNode, (uint32_t)nameIndex);
    }
    if (initCount != 1u) {
        return -1;
    }
    onlyInit = HOPMirLowerPkgAstListItemAt(ast, parts.initNode, 0);
    if (onlyInit < 0 || (uint32_t)onlyInit >= ast->len
        || ast->nodes[onlyInit].kind != HOPAst_TUPLE_EXPR)
    {
        return -1;
    }
    return HOPMirLowerPkgAstListItemAt(ast, onlyInit, (uint32_t)nameIndex);
}

int HOPMirLowerAppendZeroInitTypeFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    typeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    HOPMirFunction    function = { 0 };
    HOPMirSourceRef   sourceRef = { 0 };
    HOPMirTypeRef     typeRef = { 0 };
    HOPMirLocal       local = { 0 };
    uint32_t          functionIndex = 0;
    uint32_t          sourceIndex = 0;
    uint32_t          typeIndex = 0;
    uint32_t          slot = 0;
    const HOPAstNode* typeAst;

    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (arena == NULL || builder == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL || typeNode < 0 || (uint32_t)typeNode >= ast->len)
    {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    typeAst = &ast->nodes[typeNode];

    sourceRef.src = src;
    if (HOPMirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    typeRef.astNode = (uint32_t)typeNode;
    typeRef.sourceRef = sourceIndex;
    typeRef.flags = 0;
    typeRef.aux = 0;
    if (HOPMirProgramBuilderAddType(builder, &typeRef, &typeIndex) != 0) {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    function.sourceRef = sourceIndex;
    function.nameStart = typeAst->start;
    function.nameEnd = typeAst->end;
    function.typeRef = typeIndex;
    if (HOPMirProgramBuilderBeginFunction(builder, &function, &functionIndex) != 0) {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    local.typeRef = typeIndex;
    local.flags = HOPMirLocalFlag_ZERO_INIT;
    if (HOPMirProgramBuilderAddLocal(builder, &local, &slot) != 0) {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    if (HOPMirProgramBuilderAppendInst(
            builder,
            &(HOPMirInst){
                .op = HOPMirOp_LOCAL_ZERO,
                .aux = slot,
                .start = typeAst->start,
                .end = typeAst->end,
            })
            != 0
        || HOPMirProgramBuilderAppendInst(
               builder,
               &(HOPMirInst){
                   .op = HOPMirOp_LOCAL_LOAD,
                   .aux = slot,
                   .start = typeAst->start,
                   .end = typeAst->end,
               })
               != 0
        || HOPMirProgramBuilderAppendInst(
               builder,
               &(HOPMirInst){
                   .op = HOPMirOp_RETURN,
                   .start = typeAst->start,
                   .end = typeAst->end,
               })
               != 0)
    {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    if (HOPMirProgramBuilderEndFunction(builder) != 0) {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, typeAst->start, typeAst->end);
        return -1;
    }
    *outFunctionIndex = functionIndex;
    *outSupported = 1;
    return 0;
}

int HOPMirLowerZeroInitTypeAsFunction(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    typeNode,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    HOPMirProgramBuilder builder;
    uint32_t             functionIndex = UINT32_MAX;
    if (outProgram != NULL) {
        *outProgram = (HOPMirProgram){ 0 };
    }
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    HOPMirProgramBuilderInit(&builder, arena);
    if (HOPMirLowerAppendZeroInitTypeFunction(
            &builder, arena, ast, src, typeNode, &functionIndex, outSupported, diag)
        != 0)
    {
        return -1;
    }
    if (!*outSupported || functionIndex == UINT32_MAX) {
        return 0;
    }
    HOPMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

int HOPMirLowerAppendTopInitFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    initExprNode,
    int32_t    declTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (arena == NULL || builder == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL)
    {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    if (initExprNode >= 0) {
        return HOPMirLowerAppendExprAsFunction(
            builder,
            arena,
            ast,
            src,
            initExprNode,
            declTypeNode,
            outFunctionIndex,
            outSupported,
            diag);
    }
    if (declTypeNode >= 0) {
        return HOPMirLowerAppendZeroInitTypeFunction(
            builder, arena, ast, src, declTypeNode, outFunctionIndex, outSupported, diag);
    }
    return 0;
}

int HOPMirLowerAppendNamedTopInitFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    initExprNode,
    int32_t    declTypeNode,
    uint32_t   nameStart,
    uint32_t   nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    if (HOPMirLowerAppendTopInitFunction(
            builder,
            arena,
            ast,
            src,
            initExprNode,
            declTypeNode,
            outFunctionIndex,
            outSupported,
            diag)
        != 0)
    {
        return -1;
    }
    if (*outSupported && *outFunctionIndex != UINT32_MAX && nameEnd > nameStart
        && *outFunctionIndex < builder->funcLen)
    {
        builder->funcs[*outFunctionIndex].nameStart = nameStart;
        builder->funcs[*outFunctionIndex].nameEnd = nameEnd;
    }
    return 0;
}

int HOPMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    varLikeNode,
    uint32_t   nameStart,
    uint32_t   nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    HOPMirLowerPkgVarLikeParts parts;
    int32_t                    nameIndex;
    int32_t                    initExprNode;
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (builder == NULL || arena == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL || varLikeNode < 0 || (uint32_t)varLikeNode >= ast->len)
    {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    if (ast->nodes[varLikeNode].kind != HOPAst_CONST && ast->nodes[varLikeNode].kind != HOPAst_VAR)
    {
        return 0;
    }
    if (HOPMirLowerPkgVarLikeGetParts(ast, varLikeNode, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    nameIndex = HOPMirLowerPkgVarLikeNameIndexBySlice(ast, src, varLikeNode, nameStart, nameEnd);
    if (nameIndex < 0) {
        return 0;
    }
    initExprNode = HOPMirLowerPkgVarLikeInitExprNodeAt(ast, varLikeNode, nameIndex);
    return HOPMirLowerAppendNamedTopInitFunction(
        builder,
        arena,
        ast,
        src,
        initExprNode,
        parts.typeNode,
        nameStart,
        nameEnd,
        outFunctionIndex,
        outSupported,
        diag);
}

int HOPMirLowerBeginNamedTopInitProgram(
    HOPMirProgramBuilder* _Nonnull outBuilder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    initExprNode,
    int32_t    declTypeNode,
    uint32_t   nameStart,
    uint32_t   nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (outBuilder == NULL || arena == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL)
    {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    HOPMirProgramBuilderInit(outBuilder, arena);
    return HOPMirLowerAppendNamedTopInitFunction(
        outBuilder,
        arena,
        ast,
        src,
        initExprNode,
        declTypeNode,
        nameStart,
        nameEnd,
        outFunctionIndex,
        outSupported,
        diag);
}

int HOPMirLowerTopInitAsFunction(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    initExprNode,
    int32_t    declTypeNode,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag) {
    HOPMirProgramBuilder builder;
    uint32_t             functionIndex = UINT32_MAX;
    if (outProgram != NULL) {
        *outProgram = (HOPMirProgram){ 0 };
    }
    if (diag != NULL) {
        *diag = (HOPDiag){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        HOPMirLowerPkgSetDiag(diag, HOPDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    if (HOPMirLowerBeginNamedTopInitProgram(
            &builder,
            arena,
            ast,
            src,
            initExprNode,
            declTypeNode,
            0,
            0,
            &functionIndex,
            outSupported,
            diag)
        != 0)
    {
        return -1;
    }
    if (!*outSupported || functionIndex == UINT32_MAX) {
        return 0;
    }
    HOPMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

HOP_API_END
