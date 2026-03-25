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
typedef int (*SLMirAssignIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLMirExecValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

typedef int (*SLMirResolveCallFn)(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLMirExecValue* _Nonnull args,
    uint32_t argCount,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirResolveCallPreFn)(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirAdjustCallArgsFn)(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirInst* _Nullable inst,
    uint32_t calleeFunctionIndex,
    SLMirExecValue* _Nonnull args,
    uint32_t argCount,
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
typedef int (*SLMirSliceValueFn)(
    void* _Nullable ctx,
    const SLMirExecValue* _Nonnull base,
    const SLMirExecValue* _Nullable start,
    const SLMirExecValue* _Nullable end,
    uint16_t flags,
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
typedef int (*SLMirAggSetFieldFn)(
    void* _Nullable ctx,
    SLMirExecValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const SLMirExecValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirMakeAggregateFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    uint32_t fieldCount,
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
typedef int (*SLMirMakeVariadicPackFn)(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirTypeRef* _Nullable paramTypeRef,
    uint16_t callFlags,
    const SLMirExecValue* _Nonnull args,
    uint32_t argCount,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirEvalBinaryFn)(
    void* _Nullable ctx,
    SLTokenKind op,
    const SLMirExecValue* _Nonnull lhs,
    const SLMirExecValue* _Nonnull rhs,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirAllocNewFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirContextGetFn)(
    void* _Nullable ctx,
    uint32_t fieldId,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);
typedef int (*SLMirEvalWithContextFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    SLMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    SLDiag* _Nullable diag);

typedef int (*SLMirEnterFunctionFn)(
    void* _Nullable ctx, uint32_t functionIndex, uint32_t sourceRef, SLDiag* _Nullable diag);

typedef void (*SLMirLeaveFunctionFn)(void* _Nullable ctx);
typedef int (*SLMirBindFrameFn)(
    void* _Nullable ctx,
    const SLMirProgram* _Nullable program,
    const SLMirFunction* _Nullable function,
    const SLMirExecValue* _Nullable locals,
    uint32_t localCount,
    SLDiag* _Nullable diag);
typedef void (*SLMirUnbindFrameFn)(void* _Nullable ctx);
typedef void (*SLMirSetReasonFn)(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* _Nonnull reason);

typedef struct {
    SLStrView src;
    SLMirResolveIdentFn _Nullable resolveIdent;
    SLMirAssignIdentFn _Nullable assignIdent;
    void* _Nullable assignIdentCtx;
    SLMirResolveCallPreFn _Nullable resolveCallPre;
    SLMirResolveCallFn _Nullable resolveCall;
    SLMirAdjustCallArgsFn _Nullable adjustCallArgs;
    void* _Nullable resolveCtx;
    void* _Nullable adjustCallArgsCtx;
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
    SLMirSliceValueFn _Nullable sliceValue;
    void* _Nullable sliceValueCtx;
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
    SLMirAggSetFieldFn _Nullable aggSetField;
    void* _Nullable aggSetFieldCtx;
    SLMirMakeAggregateFn _Nullable makeAggregate;
    void* _Nullable makeAggregateCtx;
    SLMirMakeTupleFn _Nullable makeTuple;
    void* _Nullable makeTupleCtx;
    SLMirMakeVariadicPackFn _Nullable makeVariadicPack;
    void* _Nullable makeVariadicPackCtx;
    SLMirEvalBinaryFn _Nullable evalBinary;
    void* _Nullable evalBinaryCtx;
    SLMirAllocNewFn _Nullable allocNew;
    void* _Nullable allocNewCtx;
    SLMirContextGetFn _Nullable contextGet;
    void* _Nullable contextGetCtx;
    SLMirEvalWithContextFn _Nullable evalWithContext;
    void* _Nullable evalWithContextCtx;
    SLMirEnterFunctionFn _Nullable enterFunction;
    SLMirLeaveFunctionFn _Nullable leaveFunction;
    void* _Nullable functionCtx;
    SLMirBindFrameFn _Nullable bindFrame;
    SLMirUnbindFrameFn _Nullable unbindFrame;
    void* _Nullable frameCtx;
    SLMirSetReasonFn _Nullable setReason;
    void* _Nullable setReasonCtx;
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
void SLMirExecEnvDisableDynamicResolution(SLMirExecEnv* _Nonnull env);
void SLMirValueSetFunctionRef(SLMirExecValue* _Nonnull value, uint32_t functionIndex);
int  SLMirValueAsFunctionRef(
    const SLMirExecValue* _Nonnull value, uint32_t* _Nullable outFunctionIndex);
void SLMirValueSetByteRefProxy(SLMirExecValue* _Nonnull value, uint8_t* _Nullable targetByte);
int  SLMirValueAsByteRefProxy(
    const SLMirExecValue* _Nonnull value, uint8_t* _Nullable* _Nullable outTargetByte);

SL_API_END
