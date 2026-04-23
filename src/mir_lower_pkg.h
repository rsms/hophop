#pragma once
#include "mir.h"

H2_API_BEGIN

int H2MirLowerAppendZeroInitTypeFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   typeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
int H2MirLowerZeroInitTypeAsFunction(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   typeNode,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
int H2MirLowerAppendTopInitFunction(
    H2MirProgramBuilder* _Nonnull builder,
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    uint32_t* _Nonnull outFunctionIndex,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);
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
    H2Diag* _Nullable diag);
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
    H2Diag* _Nullable diag);
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
    H2Diag* _Nullable diag);
int H2MirLowerTopInitAsFunction(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   initExprNode,
    int32_t   declTypeNode,
    H2MirProgram* _Nonnull outProgram,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);

H2_API_END
