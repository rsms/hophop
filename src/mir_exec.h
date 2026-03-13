#pragma once
#include "ctfe.h"
#include "mir.h"

SL_API_BEGIN

typedef SLCTFEValue SLMirExecValue;

typedef int (*SLMirResolveIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

typedef int (*SLMirResolveCallFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLMirExecValue* _Nonnull args,
    uint32_t argCount,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

typedef int (*SLMirHostCallFn)(
    void* _Nullable ctx,
    uint32_t hostId,
    const SLMirExecValue* _Nonnull args,
    uint32_t argCount,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

typedef int (*SLMirZeroInitLocalFn)(
    void* _Nullable ctx,
    const SLMirTypeRef* _Nonnull typeRef,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirCoerceValueForTypeFn)(
    void* _Nullable ctx,
    const SLMirTypeRef* _Nonnull typeRef,
    SLMirExecValue* _Nonnull inOutValue,
    SLDiag* _Nullable diag);
typedef int (*SLMirIndexValueFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull base,
    const SLMirExecValue* _Nonnull index,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirIndexAddrFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull base,
    const SLMirExecValue* _Nonnull index,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirSequenceLenFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull base,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirIterInitFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    const SLMirExecValue* _Nonnull source,
    uint16_t flags,
    SLMirExecValue* _Nonnull outIter,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirIterNextFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull iter,
    uint16_t flags,
    int* _Nonnull outHasItem,
    SLMirExecValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirAggGetFieldFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirAggAddrFieldFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirMakeTupleFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

typedef int (*SLMirEnterFunctionFn)(
    void* _Nullable ctx, uint32_t functionIndex, uint32_t sourceRef, SLDiag* _Nullable diag);

typedef void (*SLMirLeaveFunctionFn)(void* _Nullable ctx);

typedef struct {
    SLStrView src;
    SLMirResolveIdentFn _Nullable resolveIdent;
    SLMirResolveCallFn _Nullable resolveCall;
    void* _Nullable resolveCtx;
    SLMirHostCallFn _Nullable hostCall;
    void* _Nullable hostCtx;
    SLMirZeroInitLocalFn _Nullable zeroInitLocal;
    void* _Nullable zeroInitCtx;
    SLMirCoerceValueForTypeFn _Nullable coerceValueForType;
    void* _Nullable coerceValueCtx;
    SLMirIndexValueFn _Nullable indexValue;
    void* _Nullable indexValueCtx;
    SLMirIndexAddrFn _Nullable indexAddr;
    void* _Nullable indexAddrCtx;
    SLMirSequenceLenFn _Nullable sequenceLen;
    void* _Nullable sequenceLenCtx;
    SLMirIterInitFn _Nullable iterInit;
    void* _Nullable iterInitCtx;
    SLMirIterNextFn _Nullable iterNext;
    void* _Nullable iterNextCtx;
    SLMirAggGetFieldFn _Nullable aggGetField;
    void* _Nullable aggGetFieldCtx;
    SLMirAggAddrFieldFn _Nullable aggAddrField;
    void* _Nullable aggAddrFieldCtx;
    SLMirMakeTupleFn _Nullable makeTuple;
    void* _Nullable makeTupleCtx;
    SLMirEnterFunctionFn _Nullable enterFunction;
    SLMirLeaveFunctionFn _Nullable leaveFunction;
    void* _Nullable functionCtx;
    uint32_t backwardJumpLimit;
    SLDiag* _Nullable diag;
} SLMirExecEnv;

int SLMirEvalChunk(
    SLArena* _Nonnull arena,
    SLMirChunk chunk,
    const SLMirExecEnv* _Nullable env,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int SLMirEvalFunction(
    SLArena* _Nonnull arena,
    const SLMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const SLMirExecValue* _Nullable args,
    uint32_t argCount,
    const SLMirExecEnv* _Nullable env,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int SLMirValueAsFunctionRef(
    const SLMirExecValue* _Nonnull value, uint32_t* _Nullable outFunctionIndex);

SL_API_END
