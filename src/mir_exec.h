#pragma once
#include "ctfe.h"
#include "mir.h"

H2_API_BEGIN

typedef H2CTFEValue H2MirExecValue;

typedef int (*H2MirResolveIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirAssignIdentFn)(
    void* _Nullable ctx,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2MirExecValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

typedef int (*H2MirResolveCallFn)(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2MirExecValue* _Nonnull args,
    uint32_t argCount,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirResolveCallPreFn)(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirAdjustCallArgsFn)(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirInst* _Nullable inst,
    uint32_t calleeFunctionIndex,
    H2MirExecValue* _Nonnull args,
    uint32_t argCount,
    H2Diag* _Nullable diag);

typedef int (*H2MirHostCallFn)(
    void* _Nullable ctx,
    uint32_t hostId,
    const H2MirExecValue* _Nonnull args,
    uint32_t argCount,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

typedef int (*H2MirZeroInitLocalFn)(
    void* _Nullable ctx,
    const H2MirTypeRef* _Nonnull typeRef,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirCoerceValueForTypeFn)(
    void* _Nullable ctx,
    const H2MirTypeRef* _Nonnull typeRef,
    H2MirExecValue* _Nonnull inOutValue,
    H2Diag* _Nullable diag);
typedef int (*H2MirIndexValueFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull base,
    const H2MirExecValue* _Nonnull index,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirIndexAddrFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull base,
    const H2MirExecValue* _Nonnull index,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirSliceValueFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull base,
    const H2MirExecValue* _Nullable start,
    const H2MirExecValue* _Nullable end,
    uint16_t flags,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirSequenceLenFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull base,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirIterInitFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    const H2MirExecValue* _Nonnull source,
    uint16_t flags,
    H2MirExecValue* _Nonnull outIter,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirIterNextFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull iter,
    uint16_t flags,
    int* _Nonnull outHasItem,
    H2MirExecValue* _Nonnull outKey,
    int* _Nonnull outKeyIsConst,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outValueIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirAggGetFieldFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirAggAddrFieldFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull base,
    uint32_t nameStart,
    uint32_t nameEnd,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirAggSetFieldFn)(
    void* _Nullable ctx,
    H2MirExecValue* _Nonnull inOutBase,
    uint32_t nameStart,
    uint32_t nameEnd,
    const H2MirExecValue* _Nonnull inValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirMakeAggregateFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    uint32_t fieldCount,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirMakeTupleFn)(
    void* _Nullable ctx,
    const H2MirExecValue* _Nonnull elems,
    uint32_t elemCount,
    uint32_t typeNodeHint,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirMakeVariadicPackFn)(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirTypeRef* _Nullable paramTypeRef,
    uint16_t callFlags,
    const H2MirExecValue* _Nonnull args,
    uint32_t argCount,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirEvalBinaryFn)(
    void* _Nullable ctx,
    H2TokenKind op,
    const H2MirExecValue* _Nonnull lhs,
    const H2MirExecValue* _Nonnull rhs,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirAllocNewFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef int (*H2MirContextGetFn)(
    void* _Nullable ctx,
    uint32_t fieldId,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);
typedef H2MirContextGetFn H2MirContextAddrFn;
typedef int (*H2MirEvalWithContextFn)(
    void* _Nullable ctx,
    uint32_t sourceNode,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst,
    H2Diag* _Nullable diag);

typedef int (*H2MirEnterFunctionFn)(
    void* _Nullable ctx, uint32_t functionIndex, uint32_t sourceRef, H2Diag* _Nullable diag);

typedef void (*H2MirLeaveFunctionFn)(void* _Nullable ctx);
typedef int (*H2MirBindFrameFn)(
    void* _Nullable ctx,
    const H2MirProgram* _Nullable program,
    const H2MirFunction* _Nullable function,
    const H2MirExecValue* _Nullable locals,
    uint32_t localCount,
    H2Diag* _Nullable diag);
typedef void (*H2MirUnbindFrameFn)(void* _Nullable ctx);
typedef void (*H2MirSetReasonFn)(
    void* _Nullable ctx, uint32_t start, uint32_t end, const char* _Nonnull reason);

typedef struct {
    H2StrView src;
    H2MirResolveIdentFn _Nullable resolveIdent;
    H2MirAssignIdentFn _Nullable assignIdent;
    void* _Nullable assignIdentCtx;
    H2MirResolveCallPreFn _Nullable resolveCallPre;
    H2MirResolveCallFn _Nullable resolveCall;
    H2MirAdjustCallArgsFn _Nullable adjustCallArgs;
    void* _Nullable resolveCtx;
    void* _Nullable adjustCallArgsCtx;
    H2MirHostCallFn _Nullable hostCall;
    void* _Nullable hostCtx;
    H2MirZeroInitLocalFn _Nullable zeroInitLocal;
    void* _Nullable zeroInitCtx;
    H2MirCoerceValueForTypeFn _Nullable coerceValueForType;
    void* _Nullable coerceValueCtx;
    H2MirIndexValueFn _Nullable indexValue;
    void* _Nullable indexValueCtx;
    H2MirIndexAddrFn _Nullable indexAddr;
    void* _Nullable indexAddrCtx;
    H2MirSliceValueFn _Nullable sliceValue;
    void* _Nullable sliceValueCtx;
    H2MirSequenceLenFn _Nullable sequenceLen;
    void* _Nullable sequenceLenCtx;
    H2MirIterInitFn _Nullable iterInit;
    void* _Nullable iterInitCtx;
    H2MirIterNextFn _Nullable iterNext;
    void* _Nullable iterNextCtx;
    H2MirAggGetFieldFn _Nullable aggGetField;
    void* _Nullable aggGetFieldCtx;
    H2MirAggAddrFieldFn _Nullable aggAddrField;
    void* _Nullable aggAddrFieldCtx;
    H2MirAggSetFieldFn _Nullable aggSetField;
    void* _Nullable aggSetFieldCtx;
    H2MirMakeAggregateFn _Nullable makeAggregate;
    void* _Nullable makeAggregateCtx;
    H2MirMakeTupleFn _Nullable makeTuple;
    void* _Nullable makeTupleCtx;
    H2MirMakeVariadicPackFn _Nullable makeVariadicPack;
    void* _Nullable makeVariadicPackCtx;
    H2MirEvalBinaryFn _Nullable evalBinary;
    void* _Nullable evalBinaryCtx;
    H2MirAllocNewFn _Nullable allocNew;
    void* _Nullable allocNewCtx;
    H2MirContextGetFn _Nullable contextGet;
    void* _Nullable contextGetCtx;
    H2MirContextAddrFn _Nullable contextAddr;
    void* _Nullable contextAddrCtx;
    H2MirEvalWithContextFn _Nullable evalWithContext;
    void* _Nullable evalWithContextCtx;
    H2MirEnterFunctionFn _Nullable enterFunction;
    H2MirLeaveFunctionFn _Nullable leaveFunction;
    void* _Nullable functionCtx;
    H2MirBindFrameFn _Nullable bindFrame;
    H2MirUnbindFrameFn _Nullable unbindFrame;
    void* _Nullable frameCtx;
    H2MirSetReasonFn _Nullable setReason;
    void* _Nullable setReasonCtx;
    uint32_t backwardJumpLimit;
    H2Diag* _Nullable diag;
} H2MirExecEnv;

int H2MirEvalChunk(
    H2Arena* _Nonnull arena,
    H2MirChunk chunk,
    const H2MirExecEnv* _Nullable env,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
int H2MirEvalFunction(
    H2Arena* _Nonnull arena,
    const H2MirProgram* _Nonnull program,
    uint32_t functionIndex,
    const H2MirExecValue* _Nullable args,
    uint32_t argCount,
    const H2MirExecEnv* _Nullable env,
    H2MirExecValue* _Nonnull outValue,
    int* _Nonnull outIsConst);
void H2MirExecEnvDisableDynamicResolution(H2MirExecEnv* _Nonnull env);
void H2MirValueSetFunctionRef(H2MirExecValue* _Nonnull value, uint32_t functionIndex);
int  H2MirValueAsFunctionRef(
    const H2MirExecValue* _Nonnull value, uint32_t* _Nullable outFunctionIndex);
void H2MirValueSetByteRefProxy(H2MirExecValue* _Nonnull value, uint8_t* _Nullable targetByte);
int  H2MirValueAsByteRefProxy(
    const H2MirExecValue* _Nonnull value, uint8_t* _Nullable* _Nullable outTargetByte);

H2_API_END
