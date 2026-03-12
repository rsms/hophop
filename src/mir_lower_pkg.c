#include "libsl-impl.h"
#include "mir.h"
#include "mir_lower.h"
#include "mir_lower_pkg.h"

SL_API_BEGIN

static void SLMirLowerPkgSetDiag(SLDiag* diag, SLDiagCode code, uint32_t start, uint32_t end) {
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
    typeRef.flags = 0;
    if (SLMirProgramBuilderAddType(builder, &typeRef, &typeIndex) != 0) {
        SLMirLowerPkgSetDiag(diag, SLDiag_ARENA_OOM, typeAst->start, typeAst->end);
        return -1;
    }
    function.sourceRef = sourceIndex;
    function.nameStart = typeAst->start;
    function.nameEnd = typeAst->end;
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
            builder, arena, ast, src, initExprNode, outFunctionIndex, outSupported, diag);
    }
    if (declTypeNode >= 0) {
        return SLMirLowerAppendZeroInitTypeFunction(
            builder, arena, ast, src, declTypeNode, outFunctionIndex, outSupported, diag);
    }
    return 0;
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
    SLMirProgramBuilderInit(&builder, arena);
    if (SLMirLowerAppendTopInitFunction(
            &builder,
            arena,
            ast,
            src,
            initExprNode,
            declTypeNode,
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
