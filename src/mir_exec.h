#pragma once
#include "ctfe.h"
#include "mir.h"

HOP_API_BEGIN

typedef HOPCTFEValue HOPMirExecValue;

typedef int (*HOPMirResolveIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirAssignIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPMirExecValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

typedef int (*HOPMirResolveCallFn)(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPMirExecValue* _Nonnull args,
    uint32_t argCount,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirResolveCallPreFn)(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirAdjustCallArgsFn)(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirInst* _Nullable inst,
    uint32_t calleeFunctionIndex,
    HOPMirExecValue* _Nonnull args,
    uint32_t argCount,
    HOPDiag* _Nullable diag);

typedef int (*HOPMirHostCallFn)(
    void* _Nullable ctx,
    uint32_t hostId,
    const HOPMirExecValue* _Nonnull args,
    uint32_t argCount,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

typedef int (*HOPMirZeroInitLocalFn)(
    void* _Nullable ctx,
    const HOPMirTypeRef* _Nonnull typeRef,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirCoerceValueForTypeFn)(
    void* _Nullable ctx,
    const HOPMirTypeRef* _Nonnull typeRef,
    HOPMirExecValue* _Nonnull inOutValue,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirIndexValueFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull base,
    const HOPMirExecValue* _Nonnull index,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirIndexAddrFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull base,
    const HOPMirExecValue* _Nonnull index,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirSliceValueFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull base,
    const HOPMirExecValue* _Nullable start,
    const HOPMirExecValue* _Nullable end,
    uint16_t flags,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirSequenceLenFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull base,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirIterInitFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    const HOPMirExecValue* _Nonnull source,
    uint16_t flags,
    HOPMirExecValue* _Nonnull outIter,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirIterNextFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull iter,
    uint16_t flags,
    int* _Nonnull outHasItem,
    HOPMirExecValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirAggGetFieldFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirAggAddrFieldFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirAggSetFieldFn)(
    void* _Nullable ctx,
    HOPMirExecValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const HOPMirExecValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirMakeAggregateFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    uint32_t fieldCount,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirMakeTupleFn)(
    void* _Nullable ctx,
    const HOPMirExecValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirMakeVariadicPackFn)(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirTypeRef* _Nullable paramTypeRef,
    uint16_t callFlags,
    const HOPMirExecValue* _Nonnull args,
    uint32_t argCount,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirEvalBinaryFn)(
    void* _Nullable ctx,
    HOPTokenKind op,
    const HOPMirExecValue* _Nonnull lhs,
    const HOPMirExecValue* _Nonnull rhs,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirAllocNewFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef int (*HOPMirContextGetFn)(
    void* _Nullable ctx,
    uint32_t fieldId,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);
typedef HOPMirContextGetFn HOPMirContextAddrFn;
typedef int (*HOPMirEvalWithContextFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    HOPDiag* _Nullable diag);

typedef int (*HOPMirEnterFunctionFn)(
    void* _Nullable ctx, uint32_t functionIndex, uint32_t sourceRef, HOPDiag* _Nullable diag);

typedef void (*HOPMirLeaveFunctionFn)(void* _Nullable ctx);
typedef int (*HOPMirBindFrameFn)(
    void* _Nullable ctx,
    const HOPMirProgram* _Nullable program,
    const HOPMirFunction* _Nullable function,
    const HOPMirExecValue* _Nullable locals,
    uint32_t localCount,
    HOPDiag* _Nullable diag);
typedef void (*HOPMirUnbindFrameFn)(void* _Nullable ctx);
typedef void (*HOPMirSetReasonFn)(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* _Nonnull reason);

typedef struct {
    HOPStrView src;
    HOPMirResolveIdentFn _Nullable resolveIdent;
    HOPMirAssignIdentFn _Nullable assignIdent;
    void* _Nullable assignIdentCtx;
    HOPMirResolveCallPreFn _Nullable resolveCallPre;
    HOPMirResolveCallFn _Nullable resolveCall;
    HOPMirAdjustCallArgsFn _Nullable adjustCallArgs;
    void* _Nullable resolveCtx;
    void* _Nullable adjustCallArgsCtx;
    HOPMirHostCallFn _Nullable hostCall;
    void* _Nullable hostCtx;
    HOPMirZeroInitLocalFn _Nullable zeroInitLocal;
    void* _Nullable zeroInitCtx;
    HOPMirCoerceValueForTypeFn _Nullable coerceValueForType;
    void* _Nullable coerceValueCtx;
    HOPMirIndexValueFn _Nullable indexValue;
    void* _Nullable indexValueCtx;
    HOPMirIndexAddrFn _Nullable indexAddr;
    void* _Nullable indexAddrCtx;
    HOPMirSliceValueFn _Nullable sliceValue;
    void* _Nullable sliceValueCtx;
    HOPMirSequenceLenFn _Nullable sequenceLen;
    void* _Nullable sequenceLenCtx;
    HOPMirIterInitFn _Nullable iterInit;
    void* _Nullable iterInitCtx;
    HOPMirIterNextFn _Nullable iterNext;
    void* _Nullable iterNextCtx;
    HOPMirAggGetFieldFn _Nullable aggGetField;
    void* _Nullable aggGetFieldCtx;
    HOPMirAggAddrFieldFn _Nullable aggAddrField;
    void* _Nullable aggAddrFieldCtx;
    HOPMirAggSetFieldFn _Nullable aggSetField;
    void* _Nullable aggSetFieldCtx;
    HOPMirMakeAggregateFn _Nullable makeAggregate;
    void* _Nullable makeAggregateCtx;
    HOPMirMakeTupleFn _Nullable makeTuple;
    void* _Nullable makeTupleCtx;
    HOPMirMakeVariadicPackFn _Nullable makeVariadicPack;
    void* _Nullable makeVariadicPackCtx;
    HOPMirEvalBinaryFn _Nullable evalBinary;
    void* _Nullable evalBinaryCtx;
    HOPMirAllocNewFn _Nullable allocNew;
    void* _Nullable allocNewCtx;
    HOPMirContextGetFn _Nullable contextGet;
    void* _Nullable contextGetCtx;
    HOPMirContextAddrFn _Nullable contextAddr;
    void* _Nullable contextAddrCtx;
    HOPMirEvalWithContextFn _Nullable evalWithContext;
    void* _Nullable evalWithContextCtx;
    HOPMirEnterFunctionFn _Nullable enterFunction;
    HOPMirLeaveFunctionFn _Nullable leaveFunction;
    void* _Nullable functionCtx;
    HOPMirBindFrameFn _Nullable bindFrame;
    HOPMirUnbindFrameFn _Nullable unbindFrame;
    void* _Nullable frameCtx;
    HOPMirSetReasonFn _Nullable setReason;
    void* _Nullable setReasonCtx;
    uint32_t backwardJumpLimit;
    HOPDiag* _Nullable diag;
} HOPMirExecEnv;

int HOPMirEvalChunk(
    HOPArena* _Nonnull arena,
    HOPMirChunk chunk,
    const HOPMirExecEnv* _Nullable env,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int HOPMirEvalFunction(
    HOPArena* _Nonnull arena,
    const HOPMirProgram* _Nonnull program,
    uint32_t functionIndex,
    const HOPMirExecValue* _Nullable args,
    uint32_t argCount,
    const HOPMirExecEnv* _Nullable env,
    HOPMirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
void HOPMirExecEnvDisableDynamicResolution(HOPMirExecEnv* _Nonnull env);
void HOPMirValueSetFunctionRef(HOPMirExecValue* _Nonnull value, uint32_t functionIndex);
int  HOPMirValueAsFunctionRef(
    const HOPMirExecValue* _Nonnull value, uint32_t* _Nullable outFunctionIndex);
void HOPMirValueSetByteRefProxy(HOPMirExecValue* _Nonnull value, uint8_t* _Nullable targetByte);
int  HOPMirValueAsByteRefProxy(
    const HOPMirExecValue* _Nonnull value, uint8_t* _Nullable* _Nullable outTargetByte);

HOP_API_END
