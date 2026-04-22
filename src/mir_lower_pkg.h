#pragma once
#include "mir.h"

HOP_API_BEGIN

int HOPMirLowerAppendZeroInitTypeFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    typeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
int HOPMirLowerZeroInitTypeAsFunction(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    typeNode,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
int HOPMirLowerAppendTopInitFunction(
    HOPMirProgramBuilder* _Nonnull builder,
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    initExprNode,
    int32_t    declTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);
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
    HOPDiag* _Nullable diag);
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
    HOPDiag* _Nullable diag);
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
    HOPDiag* _Nullable diag);
int HOPMirLowerTopInitAsFunction(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    initExprNode,
    int32_t    declTypeNode,
    HOPMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);

HOP_API_END
