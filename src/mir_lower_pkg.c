#include "libhop-impl.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"

H2_API_BEGIN

static void H2MirLowerPkgSetDiag(
    H2Diag* _Nullable diag, H2DiagCode code, uint32_t start, uint32_t end) {
    if (diag == NULL) {
        return;
    }
    diag->code = code;
    diag->type = H2DiagTypeOfCode(code);
    diag->start = start;
    diag->end = end;
    diag->argStart = 0;
    diag->argEnd = 0;
    diag->argText = NULL;
    diag->argTextLen = 0;
}

typedef struct {
    int32_t  nameListNode;
    int32_t  typeNode;
    int32_t  initNode;
    uint32_t nameCount;
    uint8_t  grouped;
} H2MirLowerPkgVarLikeParts;

static int H2MirLowerPkgIsTypeNodeKind(H2AstKind kind) {
    return kind == H2Ast_TYPE_NAME || kind == H2Ast_TYPE_PTR || kind == H2Ast_TYPE_REF
        || kind == H2Ast_TYPE_MUTREF || kind == H2Ast_TYPE_ARRAY || kind == H2Ast_TYPE_VARRAY
        || kind == H2Ast_TYPE_SLICE || kind == H2Ast_TYPE_MUTSLICE || kind == H2Ast_TYPE_OPTIONAL
        || kind == H2Ast_TYPE_FN || kind == H2Ast_TYPE_TUPLE || kind == H2Ast_TYPE_ANON_STRUCT
        || kind == H2Ast_TYPE_ANON_UNION;
}

static uint32_t H2MirLowerPkgAstListCount(const H2Ast* ast, int32_t listNode) {
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

static int32_t H2MirLowerPkgAstListItemAt(const H2Ast* ast, int32_t listNode, uint32_t index) {
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

static int H2MirLowerPkgNameEqSlice(
    H2StrView src, uint32_t aStart, uint32_t aEnd, uint32_t bStart, uint32_t bEnd) {
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

static int H2MirLowerPkgVarLikeGetParts(
    const H2Ast* _Nonnull ast, int32_t nodeId, H2MirLowerPkgVarLikeParts* _Nonnull out) {
    int32_t          firstChild;
    const H2AstNode* firstNode;
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
    if (firstNode->kind == H2Ast_NAME_LIST) {
        int32_t afterNames = ast->nodes[firstChild].nextSibling;
        out->grouped = 1;
        out->nameListNode = firstChild;
        out->nameCount = H2MirLowerPkgAstListCount(ast, firstChild);
        if (afterNames >= 0 && H2MirLowerPkgIsTypeNodeKind(ast->nodes[afterNames].kind)) {
            out->typeNode = afterNames;
            out->initNode = ast->nodes[afterNames].nextSibling;
        } else {
            out->initNode = afterNames;
        }
        return 0;
    }
    out->grouped = 0;
    out->nameCount = 1;
    if (H2MirLowerPkgIsTypeNodeKind(firstNode->kind)) {
        out->typeNode = firstChild;
        out->initNode = ast->nodes[firstChild].nextSibling;
    } else {
        out->initNode = firstChild;
    }
    return 0;
}

static int32_t H2MirLowerPkgVarLikeNameIndexBySlice(
    const H2Ast* _Nonnull ast, H2StrView src, int32_t nodeId, uint32_t start, uint32_t end) {
    H2MirLowerPkgVarLikeParts parts;
    uint32_t                  i;
    if (H2MirLowerPkgVarLikeGetParts(ast, nodeId, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    if (!parts.grouped) {
        return H2MirLowerPkgNameEqSlice(
                   src, ast->nodes[nodeId].dataStart, ast->nodes[nodeId].dataEnd, start, end)
                 ? 0
                 : -1;
    }
    for (i = 0; i < parts.nameCount; i++) {
        int32_t item = H2MirLowerPkgAstListItemAt(ast, parts.nameListNode, i);
        if (item >= 0
            && H2MirLowerPkgNameEqSlice(
                src, ast->nodes[item].dataStart, ast->nodes[item].dataEnd, start, end))
        {
            return (int32_t)i;
        }
    }
    return -1;
}

static int32_t H2MirLowerPkgVarLikeInitExprNodeAt(
    const H2Ast* _Nonnull ast, int32_t nodeId, int32_t nameIndex) {
    H2MirLowerPkgVarLikeParts parts;
    uint32_t                  initCount;
    int32_t                   onlyInit;
    if (H2MirLowerPkgVarLikeGetParts(ast, nodeId, &parts) != 0) {
        return -1;
    }
    if (!parts.grouped) {
        return nameIndex == 0 ? parts.initNode : -1;
    }
    if (parts.initNode < 0 || ast->nodes[parts.initNode].kind != H2Ast_EXPR_LIST || nameIndex < 0) {
        return -1;
    }
    initCount = H2MirLowerPkgAstListCount(ast, parts.initNode);
    if (initCount == parts.nameCount) {
        return H2MirLowerPkgAstListItemAt(ast, parts.initNode, (uint32_t)nameIndex);
    }
    if (initCount != 1u) {
        return -1;
    }
    onlyInit = H2MirLowerPkgAstListItemAt(ast, parts.initNode, 0);
    if (onlyInit < 0 || (uint32_t)onlyInit >= ast->len
        || ast->nodes[onlyInit].kind != H2Ast_TUPLE_EXPR)
    {
        return -1;
    }
    return H2MirLowerPkgAstListItemAt(ast, onlyInit, (uint32_t)nameIndex);
}

int H2MirLowerAppendZeroInitTypeFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   typeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    H2MirFunction    function = { 0 };
    H2MirSourceRef   sourceRef = { 0 };
    H2MirTypeRef     typeRef = { 0 };
    H2MirLocal       local = { 0 };
    uint32_t         functionIndex = 0;
    uint32_t         sourceIndex = 0;
    uint32_t         typeIndex = 0;
    uint32_t         slot = 0;
    const H2AstNode* typeAst;

    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (arena == NULL || builder == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL || typeNode < 0 || (uint32_t)typeNode >= ast->len)
    {
        H2MirLowerPkgSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    typeAst = &ast->nodes[typeNode];

    sourceRef.src = src;
    if (H2MirProgramBuilderAddSource(builder, &sourceRef, &sourceIndex) != 0) {
        H2MirLowerPkgSetDiag(diag, H2Diag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    typeRef.astNode = (uint32_t)typeNode;
    typeRef.sourceRef = sourceIndex;
    typeRef.flags = 0;
    typeRef.aux = 0;
    if (H2MirProgramBuilderAddType(builder, &typeRef, &typeIndex) != 0) {
        H2MirLowerPkgSetDiag(diag, H2Diag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    function.sourceRef = sourceIndex;
    function.nameStart = typeAst->start;
    function.nameEnd = typeAst->end;
    function.typeRef = typeIndex;
    if (H2MirProgramBuilderBeginFunction(builder, &function, &functionIndex) != 0) {
        H2MirLowerPkgSetDiag(diag, H2Diag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    local.typeRef = typeIndex;
    local.flags = H2MirLocalFlag_ZERO_INIT;
    if (H2MirProgramBuilderAddLocal(builder, &local, &slot) != 0) {
        H2MirLowerPkgSetDiag(diag, H2Diag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    if (H2MirProgramBuilderAppendInst(
            builder,
            &(H2MirInst){
                .op = H2MirOp_LOCAL_ZERO,
                .aux = slot,
                .start = typeAst->start,
                .end = typeAst->end,
            })
            != 0
        || H2MirProgramBuilderAppendInst(
               builder,
               &(H2MirInst){
                   .op = H2MirOp_LOCAL_LOAD,
                   .aux = slot,
                   .start = typeAst->start,
                   .end = typeAst->end,
               })
               != 0
        || H2MirProgramBuilderAppendInst(
               builder,
               &(H2MirInst){
                   .op = H2MirOp_RETURN,
                   .start = typeAst->start,
                   .end = typeAst->end,
               })
               != 0)
    {
        H2MirLowerPkgSetDiag(diag, H2Diag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    if (H2MirProgramBuilderEndFunction(builder) != 0) {
        H2MirLowerPkgSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, typeAst->start, typeAst->end);
        return -1;
    }
    *outFunctionIndex = functionIndex;
    *outSupported = 1;
    return 0;
}

int H2MirLowerZeroInitTypeAsFunction(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   typeNode,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    H2MirProgramBuilder builder;
    uint32_t            functionIndex = UINT32_MAX;
    if (outProgram != NULL) {
        *outProgram = (H2MirProgram){ 0 };
    }
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        H2MirLowerPkgSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    H2MirProgramBuilderInit(&builder, arena);
    if (H2MirLowerAppendZeroInitTypeFunction(
            &builder, arena, ast, src, typeNode, &functionIndex, outSupported, diag)
        != 0)
    {
        return -1;
    }
    if (!*outSupported || functionIndex == UINT32_MAX) {
        return 0;
    }
    H2MirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

int H2MirLowerAppendTopInitFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (arena == NULL || builder == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL)
    {
        H2MirLowerPkgSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    if (initExprNode >= 0) {
        return H2MirLowerAppendExprAsFunction(
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
        return H2MirLowerAppendZeroInitTypeFunction(
            builder, arena, ast, src, declTypeNode, outFunctionIndex, outSupported, diag);
    }
    return 0;
}

int H2MirLowerAppendNamedTopInitFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t  nameStart,
    uint32_t  nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    if (H2MirLowerAppendTopInitFunction(
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

int H2MirLowerAppendNamedVarLikeTopInitFunctionBySlice(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   varLikeNode,
    uint32_t  nameStart,
    uint32_t  nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    H2MirLowerPkgVarLikeParts parts;
    int32_t                   nameIndex;
    int32_t                   initExprNode;
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (builder == NULL || arena == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL || varLikeNode < 0 || (uint32_t)varLikeNode >= ast->len)
    {
        H2MirLowerPkgSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    *outSupported = 0;
    if (ast->nodes[varLikeNode].kind != H2Ast_CONST && ast->nodes[varLikeNode].kind != H2Ast_VAR) {
        return 0;
    }
    if (H2MirLowerPkgVarLikeGetParts(ast, varLikeNode, &parts) != 0 || parts.nameCount == 0) {
        return -1;
    }
    nameIndex = H2MirLowerPkgVarLikeNameIndexBySlice(ast, src, varLikeNode, nameStart, nameEnd);
    if (nameIndex < 0) {
        return 0;
    }
    initExprNode = H2MirLowerPkgVarLikeInitExprNodeAt(ast, varLikeNode, nameIndex);
    return H2MirLowerAppendNamedTopInitFunction(
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

int H2MirLowerBeginNamedTopInitProgram(
    H2MirProgramBuilder* _Nonnull outBuilder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t  nameStart,
    uint32_t  nameEnd,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    if (outFunctionIndex != NULL) {
        *outFunctionIndex = UINT32_MAX;
    }
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (outBuilder == NULL || arena == NULL || ast == NULL || outFunctionIndex == NULL
        || outSupported == NULL)
    {
        H2MirLowerPkgSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    H2MirProgramBuilderInit(outBuilder, arena);
    return H2MirLowerAppendNamedTopInitFunction(
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

int H2MirLowerTopInitAsFunction(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag) {
    H2MirProgramBuilder builder;
    uint32_t            functionIndex = UINT32_MAX;
    if (outProgram != NULL) {
        *outProgram = (H2MirProgram){ 0 };
    }
    if (diag != NULL) {
        *diag = (H2Diag){ 0 };
    }
    if (arena == NULL || ast == NULL || outProgram == NULL || outSupported == NULL) {
        H2MirLowerPkgSetDiag(diag, H2Diag_UNEXPECTED_TOKEN, 0, 0);
        return -1;
    }
    if (H2MirLowerBeginNamedTopInitProgram(
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
    H2MirProgramBuilderFinish(&builder, outProgram);
    return 0;
}

H2_API_END
