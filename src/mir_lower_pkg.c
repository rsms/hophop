#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"

SL_API_BEGIN

static void SLMirLowerPkgSetDiag(
    SLDiag* _Nullable diag, SLDiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = SLDiagTypeOfCode(code);
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
} SLMirLowerPkgVarLikeParts;

static int SLMirLowerPkgIsTypeNodeKind(SLAstKind kind) {
    return kind == SLAst_TYPE_NAME || kind == SLAst_TYPE_PTR || kind == SLAst_TYPE_REF
        || kind == SLAst_TYPE_MUTREF || kind == SLAst_TYPE_ARRAY || kind == SLAst_TYPE_VARRAY
        || kind == SLAst_TYPE_SLICE || kind == SLAst_TYPE_MUTSLICE || kind == SLAst_TYPE_OPTIONAL
        || kind == SLAst_TYPE_FN || kind == SLAst_TYPE_TUPLE || kind == SLAst_TYPE_ANON_STRUCT
        || kind == SLAst_TYPE_ANON_UNION;
}

static uint32_t SLMirLowerPkgAstListCount(const SLAst* ast, int32_t listNode) {
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

static int32_t SLMirLowerPkgAstListItemAt(const SLAst* ast, int32_t listNode, uint32_t index) {
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

static int SLMirLowerPkgNameEqSlice(
    SLStrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

static int SLMirLowerPkgVarLikeGetParts(
    const SLAst* _Nonnull ast, int32_t nodeId, SLMirLowerPkgVarLikeParts* _Nonnull out) {
    int32_t          firstChild;
    const SLAstNode* firstNode;
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
    if (firstNode->kind == SLAst_NAME_LIST) {
        int32_t afterNames = ast->nodes[firstChild].nextSibling;
        out->grouped = 1;
        out->nameListNode = firstChild;
        out->nameCount = SLMirLowerPkgAstListCount(ast, firstChild);
        if (afterNames >= 0 && SLMirLowerPkgIsTypeNodeKind(ast->nodes[afterNames].kind)) {
            out->typeNode = afterNames;
            out->initNode = ast->nodes[afterNames].nextSibling;
        } else {
            out->initNode = afterNames;
        }
        return 0;
    }
    out->grouped = 0;
    out->nameCount = 1;
    if (SLMirLowerPkgIsTypeNodeKind(firstNode->kind)) {
        out->typeNode = firstChild;
        out->initNode = ast->nodes[firstChild].nextSibling;
    } else {
        out->initNode = firstChild;
    }
    return 0;
}

static int32_t SLMirLowerPkgVarLikeNameIndexBySlice(
    const SLAst* _Nonnull ast, SLStrView src, int32_t nodeId, uint32_t start, uint32_t end) {
    SLMirLowerPkgVarLikeParts parts;
    uint32_t                  i;
    if (SLMirLowerPkgVarLikeGetParts(ast, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (!parts.grouped) {
        return SLMirLowerPkgNameEqSlice(
                   src, ast->nodes[nodeId].dataStart, ast->nodes[nodeId].dataEnd, start, end)
                 ? 0
                 : -1;
    }
    for (i = 0; i < parts.nameCount; i++) {
        int32_t item = SLMirLowerPkgAstListItemAt(ast, parts.nameListNode, i);
        if (item >= 0
            && SLMirLowerPkgNameEqSlice(
                src, ast->nodes[item].dataStart, ast->nodes[item].dataEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t SLMirLowerPkgVarLikeInitExprNodeAt(
    const SLAst* _Nonnull ast, int32_t nodeId, int32_t nameIndex) {
    SLMirLowerPkgVarLikeParts parts;
    uint32_t                  initCount;
    int32_t                   onlyInit;
    if (SLMirLowerPkgVarLikeGetParts(ast, nodeId, &parts) != 0) {
        return -1;
    }
    if (!parts.grouped) {
        return nameIndex == 0 ? parts.initNode : -1;
    }
    if (parts.initNode < 0 || ast->nodes[parts.initNode].kind != SLAst_EXPR_LIST || nameIndex < 0) {
        return -1;
    }
    initCount = SLMirLowerPkgAstListCount(ast, parts.initNode);
    if (initCount == parts.nameCount) {
        return SLMirLowerPkgAstListItemAt(ast, parts.initNode, (uint32_t)nameIndex);
    }
    if (initCount != 1u) {
        return -1;
    }
    onlyInit = SLMirLowerPkgAstListItemAt(ast, parts.initNode, 0);
    if (onlyInit < 0 || (uint32_t)onlyInit >= ast->len
        || ast->nodes[onlyInit].kind != SLAst_TUPLE_EXPR)
    {
        return -1;
    }
    return SLMirLowerPkgAstListItemAt(ast, onlyInit, (uint32_t)nameIndex);
}

int SLMirLowerAppendZeroInitTypeFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   typeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirFunction    function = { 0 };
    SLMirSourceRef   sourceRef = { 0 };
    SLMirTypeRef     typeRef = { 0 };
    SLMirLocal       local = { 0 };
    uint32_t         functionIndex = 0;
    uint32_t         sourceIndex = 0;
    uint32_t         typeIndex = 0;
    uint32_t         slot = 0;
    const SLAstNode* typeAst;

    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (arena == NULL || builder == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL || typeNode < 0 || (uint32_t)typeNode >= ast->len)
    {
        SLMirLowerPkgSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    typeAst = &ast->nodes[typeNode];

    sourceRef.src = src;
    if (SLMirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        SLMirLowerPkgSetDiag(diag, SLDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    typeRef.astNode = (uint32_t)typeNode;
    typeRef.sourceRef = sourceIndex;
    typeRef.flags = 0;
    typeRef.aux = 0;
    if (SLMirProgramBuilderAddType(builder, &typeRef, &typeIndex) != 0) {
        SLMirLowerPkgSetDiag(diag, SLDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    function.sourceRef = sourceIndex;
    function.nameStart = typeAst->start;
    function.nameEnd = typeAst->end;
    function.typeRef = typeIndex;
    if (SLMirProgramBuilderBeginFunction(builder, &function, &functionIndex) != 0) {
        SLMirLowerPkgSetDiag(diag, SLDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    local.typeRef = typeIndex;
    local.flags = SLMirLocalFlag_ZERO_INIT;
    if (SLMirProgramBuilderAddLocal(builder, &local, &slot) != 0) {
        SLMirLowerPkgSetDiag(diag, SLDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    if (SLMirProgramBuilderAppendInst(
            builder,
            &(SLMirInst){
                .op = SLMirOp_LOCAL_ZERO,
                .aux = slot,
                .start = typeAst->start,
                .end = typeAst->end,
            })
            != 0
        || SLMirProgramBuilderAppendInst(
               builder,
               &(SLMirInst){
                   .op = SLMirOp_LOCAL_LOAD,
                   .aux = slot,
                   .start = typeAst->start,
                   .end = typeAst->end,
               })
               != 0
        || SLMirProgramBuilderAppendInst(
               builder,
               &(SLMirInst){
                   .op = SLMirOp_RETURN,
                   .start = typeAst->start,
                   .end = typeAst->end,
               })
               != 0)
    {
        SLMirLowerPkgSetDiag(diag, SLDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    if (SLMirProgramBuilderEndFunction(builder) != 0) {
        SLMirLowerPkgSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, typeAst->start, typeAst->end);
        return -1;
    }
    *outFunctionIndex = functionIndex;
    *outSupported = 1;
    return 0;
}

int SLMirLowerZeroInitTypeAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   typeNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirProgramBuilder builder;
    uint32_t            functionIndex = UINT32_MAX;
    if (outProgram != NULL) {
        *outProgram = (SLMirProgram){ 0 };
    }
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        SLMirLowerPkgSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    SLMirProgramBuilderInit(&builder, arena);
    if (SLMirLowerAppendZeroInitTypeFunction(
            &builder, arena, ast, src, typeNode, &functionIndex, outSupported, diag)
        != 0)
    {
        return -1;
    }
    if (!*outSupported || functionIndex == UINT32_MAX) {
        return 0;
    }
    SLMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

int SLMirLowerAppendTopInitFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (arena == NULL || builder == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL)
    {
        SLMirLowerPkgSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    if (initExprNode >= 0) {
        return SLMirLowerAppendExprAsFunction(
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
        return SLMirLowerAppendZeroInitTypeFunction(
            builder, arena, ast, src, declTypeNode, outFunctionIndex, outSupported, diag);
    }
    return 0;
}

int SLMirLowerAppendNamedTopInitFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t  nameStart,
    uint32_t  nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    if (SLMirLowerAppendTopInitFunction(
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

int SLMirLowerAppendNamedVarLikeTopInitFunctionBySlice(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   varLikeNode,
    uint32_t  nameStart,
    uint32_t  nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirLowerPkgVarLikeParts parts;
    int32_t                   nameIndex;
    int32_t                   initExprNode;
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (builder == NULL || arena == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL || varLikeNode < 0 || (uint32_t)varLikeNode >= ast->len)
    {
        SLMirLowerPkgSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    if (ast->nodes[varLikeNode].kind != SLAst_CONST && ast->nodes[varLikeNode].kind != SLAst_VAR) {
        return 0;
    }
    if (SLMirLowerPkgVarLikeGetParts(ast, varLikeNode, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    nameIndex = SLMirLowerPkgVarLikeNameIndexBySlice(ast, src, varLikeNode, nameStart, nameEnd);
    if (nameIndex < 0) {
        return 0;
    }
    initExprNode = SLMirLowerPkgVarLikeInitExprNodeAt(ast, varLikeNode, nameIndex);
    return SLMirLowerAppendNamedTopInitFunction(
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

int SLMirLowerBeginNamedTopInitProgram(
    SLMirProgramBuilder* _Nonnull outBuilder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t  nameStart,
    uint32_t  nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (outBuilder == NULL || arena == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL)
    {
        SLMirLowerPkgSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    SLMirProgramBuilderInit(outBuilder, arena);
    return SLMirLowerAppendNamedTopInitFunction(
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

int SLMirLowerTopInitAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag) {
    SLMirProgramBuilder builder;
    uint32_t            functionIndex = UINT32_MAX;
    if (outProgram != NULL) {
        *outProgram = (SLMirProgram){ 0 };
    }
    if (diag != NULL) {
        *diag = (SLDiag){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        SLMirLowerPkgSetDiag(diag, SLDiag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    if (SLMirLowerBeginNamedTopInitProgram(
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
    SLMirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

SL_API_END
