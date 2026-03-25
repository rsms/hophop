#pragma once
#include "mir.h"

SL_API_BEGIN

int SLMirLowerAppendZeroInitTypeFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   typeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerZeroInitTypeAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   typeNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
int SLMirLowerAppendTopInitFunction(
    SLMirProgramBuilder* _Nonnull builder,
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);
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
    SLDiag* _Nullable diag);
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
    SLDiag* _Nullable diag);
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
    SLDiag* _Nullable diag);
int SLMirLowerTopInitAsFunction(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    SLMirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);

SL_API_END
