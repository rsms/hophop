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
    SLMirEnterFunctionFn _Nullable enterFunction;
    SLMirLeaveFunctionFn _Nullable leaveFunction;
    void* _Nullable functionCtx;
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

SL_API_END
