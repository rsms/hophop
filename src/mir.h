#pragma once
#include "libsl.h"

// Documentation for this IR lives in docs/mir.md

SL_API_BEGIN

typedef enum {
    SLMirOp_INVALID = 0,
    SLMirOp_PUSH_CONST,
    SLMirOp_PUSH_INT,
    SLMirOp_PUSH_FLOAT,
    SLMirOp_PUSH_BOOL,
    SLMirOp_PUSH_STRING,
    SLMirOp_PUSH_NULL,
    SLMirOp_LOAD_IDENT,
    SLMirOp_STORE_IDENT,
    SLMirOp_CALL,
    SLMirOp_UNARY,
    SLMirOp_BINARY,
    SLMirOp_INDEX,
    SLMirOp_SEQ_LEN,
    SLMirOp_STR_CSTR,
    SLMirOp_ITER_INIT,
    SLMirOp_ITER_NEXT,
    SLMirOp_CAST,
    SLMirOp_COERCE,
    SLMirOp_LOCAL_ZERO,
    SLMirOp_LOCAL_LOAD,
    SLMirOp_LOCAL_STORE,
    SLMirOp_LOCAL_ADDR,
    SLMirOp_DROP,
    SLMirOp_JUMP,
    SLMirOp_JUMP_IF_FALSE,
    SLMirOp_ASSERT,
    SLMirOp_CALL_FN,
    SLMirOp_CALL_HOST,
    SLMirOp_CALL_INDIRECT,
    SLMirOp_DEREF_LOAD,
    SLMirOp_DEREF_STORE,
    SLMirOp_ADDR_OF,
    SLMirOp_AGG_MAKE,
    SLMirOp_AGG_ZERO,
    SLMirOp_AGG_GET,
    SLMirOp_AGG_SET,
    SLMirOp_AGG_ADDR,
    SLMirOp_ARRAY_ZERO,
    SLMirOp_ARRAY_GET,
    SLMirOp_ARRAY_SET,
    SLMirOp_ARRAY_ADDR,
    SLMirOp_TUPLE_MAKE,
    SLMirOp_SLICE_MAKE,
    SLMirOp_OPTIONAL_WRAP,
    SLMirOp_OPTIONAL_UNWRAP,
    SLMirOp_TAGGED_MAKE,
    SLMirOp_TAGGED_TAG,
    SLMirOp_TAGGED_PAYLOAD,
    SLMirOp_ALLOC_NEW,
    SLMirOp_CTX_GET,
    SLMirOp_CTX_ADDR,
    SLMirOp_CTX_SET,
    SLMirOp_RETURN,
    SLMirOp_RETURN_VOID,
} SLMirOp;

typedef enum {
    SLMirCastTarget_INVALID = 0,
    SLMirCastTarget_INT = 1,
    SLMirCastTarget_FLOAT = 2,
    SLMirCastTarget_BOOL = 3,
    SLMirCastTarget_STR_VIEW = 4,
    SLMirCastTarget_PTR_LIKE = 5,
} SLMirCastTarget;

enum {
    SLMirCallArgFlag_RECEIVER_ARG0 = 0x8000u,
    SLMirCallArgFlag_SPREAD_LAST = 0x4000u,
    SLMirCallArgFlag_MASK = SLMirCallArgFlag_RECEIVER_ARG0 | SLMirCallArgFlag_SPREAD_LAST,
};

static inline uint32_t SLMirCallArgCountFromTok(uint16_t tok) {
    return (uint32_t)(tok & ~SLMirCallArgFlag_MASK);
}

static inline int SLMirCallTokDropsReceiverArg0(uint16_t tok) {
    return (tok & SLMirCallArgFlag_RECEIVER_ARG0) != 0;
}

static inline int SLMirCallTokHasSpreadLast(uint16_t tok) {
    return (tok & SLMirCallArgFlag_SPREAD_LAST) != 0;
}

enum {
    SLMirIterFlag_HAS_KEY = 1u << 0,
    SLMirIterFlag_KEY_REF = 1u << 1,
    SLMirIterFlag_VALUE_REF = 1u << 2,
    SLMirIterFlag_VALUE_DISCARD = 1u << 3,
};

typedef struct {
    SLMirOp  op;
    uint16_t tok;
    uint16_t _reserved;
    uint32_t aux;
    uint32_t start;
    uint32_t end;
} SLMirInst;

typedef struct {
    const SLMirInst* _Nullable v;
    uint32_t len;
} SLMirChunk;

typedef enum {
    SLMirConst_INVALID = 0,
    SLMirConst_INT,
    SLMirConst_FLOAT,
    SLMirConst_BOOL,
    SLMirConst_STRING,
    SLMirConst_NULL,
    SLMirConst_TYPE,
    SLMirConst_FUNCTION,
    SLMirConst_HOST,
} SLMirConstKind;

typedef struct {
    SLMirConstKind kind;
    uint32_t       aux;
    uint64_t       bits;
    SLStrView      bytes;
} SLMirConst;

typedef struct {
    SLStrView src;
} SLMirSourceRef;

typedef enum {
    SLMirLocalFlag_NONE = 0,
    SLMirLocalFlag_PARAM = 1u << 0,
    SLMirLocalFlag_MUTABLE = 1u << 1,
    SLMirLocalFlag_ZERO_INIT = 1u << 2,
} SLMirLocalFlag;

typedef struct {
    uint32_t typeRef;
    uint32_t flags;
    uint32_t nameStart;
    uint32_t nameEnd;
} SLMirLocal;

typedef struct {
    uint32_t instStart;
    uint32_t instLen;
    uint32_t sourceRef;
    uint32_t localStart;
    uint32_t localCount;
    uint32_t paramCount;
    uint32_t tempCount;
    uint32_t typeRef;
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t flags;
} SLMirFunction;

enum {
    SLMirFunctionFlag_NONE = 0,
    SLMirFunctionFlag_VARIADIC = 1u << 0,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t sourceRef;
    uint32_t ownerTypeRef;
    uint32_t typeRef;
} SLMirField;

typedef struct {
    uint32_t astNode;
    uint32_t sourceRef;
    uint32_t flags;
    uint32_t aux;
} SLMirTypeRef;

typedef enum {
    SLMirTypeScalar_NONE = 0,
    SLMirTypeScalar_I32 = 1,
    SLMirTypeScalar_I64 = 2,
    SLMirTypeScalar_F32 = 3,
    SLMirTypeScalar_F64 = 4,
} SLMirTypeScalar;

enum {
    SLMirTypeFlag_SCALAR_MASK = 0x000000ffu,
    SLMirTypeFlag_STR_REF = 0x00000100u,
    SLMirTypeFlag_STR_PTR = 0x00000200u,
    SLMirTypeFlag_U8_PTR = 0x00000400u,
    SLMirTypeFlag_I32_PTR = 0x00000800u,
    SLMirTypeFlag_I8_PTR = 0x00001000u,
    SLMirTypeFlag_U16_PTR = 0x00002000u,
    SLMirTypeFlag_I16_PTR = 0x00004000u,
    SLMirTypeFlag_U32_PTR = 0x00008000u,
    SLMirTypeFlag_FIXED_ARRAY = 0x00010000u,
    SLMirTypeFlag_FIXED_ARRAY_VIEW = 0x00020000u,
    SLMirTypeFlag_SLICE_VIEW = 0x00040000u,
    SLMirTypeFlag_AGGREGATE = 0x00080000u,
    SLMirTypeFlag_OPAQUE_PTR = 0x00100000u,
    SLMirTypeFlag_OPTIONAL = 0x00200000u,
    SLMirTypeFlag_FUNC_REF = 0x00400000u,
    SLMirTypeFlag_STR_OBJ = 0x00800000u,
    SLMirTypeFlag_VARRAY_VIEW = 0x01000000u,
    SLMirTypeFlag_AGG_SLICE_VIEW = 0x02000000u,
};

typedef enum {
    SLMirIntKind_NONE = 0,
    SLMirIntKind_BOOL = 1,
    SLMirIntKind_U8 = 2,
    SLMirIntKind_I8 = 3,
    SLMirIntKind_U16 = 4,
    SLMirIntKind_I16 = 5,
    SLMirIntKind_U32 = 6,
    SLMirIntKind_I32 = 7,
} SLMirIntKind;

enum {
    SLMirTypeAux_INT_KIND_MASK = 0x000000ffu,
    SLMirTypeAux_ARRAY_COUNT_SHIFT = 8u,
    SLMirTypeAux_ARRAY_COUNT_MASK = 0x00ffff00u,
};

static inline uint32_t SLMirTypeAuxMakeScalarInt(SLMirIntKind intKind) {
    return (uint32_t)intKind;
}

static inline uint32_t SLMirTypeAuxMakeFixedArray(SLMirIntKind elemKind, uint32_t count) {
    return (uint32_t)elemKind
         | ((count << SLMirTypeAux_ARRAY_COUNT_SHIFT) & SLMirTypeAux_ARRAY_COUNT_MASK);
}

static inline uint32_t SLMirTypeAuxMakeVArrayView(SLMirIntKind elemKind, uint32_t countFieldRef) {
    return (uint32_t)elemKind
         | (((countFieldRef + 1u) << SLMirTypeAux_ARRAY_COUNT_SHIFT)
            & SLMirTypeAux_ARRAY_COUNT_MASK);
}

static inline SLMirIntKind SLMirTypeRefIntKind(const SLMirTypeRef* typeRef) {
    return typeRef != NULL
             ? (SLMirIntKind)(typeRef->aux & SLMirTypeAux_INT_KIND_MASK)
             : SLMirIntKind_NONE;
}

static inline uint32_t SLMirTypeRefFixedArrayCount(const SLMirTypeRef* typeRef) {
    return typeRef != NULL
             ? (typeRef->aux & SLMirTypeAux_ARRAY_COUNT_MASK) >> SLMirTypeAux_ARRAY_COUNT_SHIFT
             : 0u;
}

static inline uint32_t SLMirTypeRefVArrayCountField(const SLMirTypeRef* typeRef) {
    uint32_t encoded;
    if (typeRef == NULL) {
        return UINT32_MAX;
    }
    encoded = (typeRef->aux & SLMirTypeAux_ARRAY_COUNT_MASK) >> SLMirTypeAux_ARRAY_COUNT_SHIFT;
    return encoded == 0u ? UINT32_MAX : encoded - 1u;
}

static inline uint32_t SLMirTypeAuxMakeAggSliceView(uint32_t elemTypeRef) {
    return elemTypeRef == UINT32_MAX ? 0u : (elemTypeRef + 1u);
}

static inline uint32_t SLMirTypeRefAggSliceElemTypeRef(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_AGG_SLICE_VIEW) != 0u
                && typeRef->aux != 0u
             ? typeRef->aux - 1u
             : UINT32_MAX;
}

static inline SLMirTypeScalar SLMirTypeRefScalarKind(const SLMirTypeRef* typeRef) {
    return typeRef != NULL
             ? (SLMirTypeScalar)(typeRef->flags & SLMirTypeFlag_SCALAR_MASK)
             : SLMirTypeScalar_NONE;
}

static inline int SLMirTypeRefIsStrRef(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_STR_REF) != 0;
}

static inline int SLMirTypeRefIsStrPtr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_STR_PTR) != 0;
}

static inline int SLMirTypeRefIsStrObj(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_STR_OBJ) != 0;
}

static inline int SLMirTypeRefIsU8Ptr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_U8_PTR) != 0;
}

static inline int SLMirTypeRefIsI32Ptr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_I32_PTR) != 0;
}

static inline int SLMirTypeRefIsI8Ptr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_I8_PTR) != 0;
}

static inline int SLMirTypeRefIsU16Ptr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_U16_PTR) != 0;
}

static inline int SLMirTypeRefIsI16Ptr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_I16_PTR) != 0;
}

static inline int SLMirTypeRefIsU32Ptr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_U32_PTR) != 0;
}

static inline int SLMirTypeRefIsFixedArray(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_FIXED_ARRAY) != 0;
}

static inline int SLMirTypeRefIsFixedArrayView(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_FIXED_ARRAY_VIEW) != 0;
}

static inline int SLMirTypeRefIsSliceView(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_SLICE_VIEW) != 0;
}

static inline int SLMirTypeRefIsVArrayView(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_VARRAY_VIEW) != 0;
}

static inline int SLMirTypeRefIsAggSliceView(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_AGG_SLICE_VIEW) != 0;
}

static inline int SLMirTypeRefIsAggregate(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_AGGREGATE) != 0;
}

static inline int SLMirTypeRefIsOpaquePtr(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_OPAQUE_PTR) != 0;
}

static inline int SLMirTypeRefIsOptional(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_OPTIONAL) != 0;
}

static inline int SLMirTypeRefIsFuncRef(const SLMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & SLMirTypeFlag_FUNC_REF) != 0;
}

static inline uint32_t SLMirTypeRefFuncRefFunctionIndex(const SLMirTypeRef* typeRef) {
    if (!SLMirTypeRefIsFuncRef(typeRef) || typeRef->aux == 0u) {
        return UINT32_MAX;
    }
    return typeRef->aux - 1u;
}

static inline uint32_t SLMirTypeRefOpaquePointeeTypeRef(const SLMirTypeRef* typeRef) {
    return SLMirTypeRefIsOpaquePtr(typeRef) ? typeRef->aux : UINT32_MAX;
}

typedef enum {
    SLMirHost_INVALID = 0,
    SLMirHost_GENERIC,
} SLMirHostKind;

typedef enum {
    SLMirHostTarget_INVALID = 0,
    SLMirHostTarget_PRINT = 1,
    SLMirHostTarget_PLATFORM_EXIT = 2,
    SLMirHostTarget_FREE = 3,
    SLMirHostTarget_CONCAT = 4,
    SLMirHostTarget_COPY = 5,
    SLMirHostTarget_PLATFORM_CONSOLE_LOG = 6,
} SLMirHostTarget;

typedef enum {
    SLMirContextField_INVALID = 0,
    SLMirContextField_ALLOCATOR = 1,
    SLMirContextField_TEMP_ALLOCATOR = 2,
    SLMirContextField_LOGGER = 3,
} SLMirContextField;

typedef struct {
    uint32_t      nameStart;
    uint32_t      nameEnd;
    SLMirHostKind kind;
    uint32_t      flags;
    uint32_t      target;
} SLMirHostRef;

typedef enum {
    SLMirSymbol_INVALID = 0,
    SLMirSymbol_IDENT,
    SLMirSymbol_CALL,
    SLMirSymbol_HOST,
} SLMirSymbolKind;

typedef enum {
    SLMirSymbolFlag_NONE = 0,
    SLMirSymbolFlag_CALL_RECEIVER_ARG0 = 1u << 0,
} SLMirSymbolFlag;

enum {
    SLMIR_RAW_CALL_AUX_FLAG_MASK = (1u << 1u) - 1u,
};

static inline uint32_t SLMirRawCallAuxPack(uint32_t nodeId, uint32_t flags) {
    return (nodeId << 1u) | (flags & SLMIR_RAW_CALL_AUX_FLAG_MASK);
}

static inline uint32_t SLMirRawCallAuxNode(uint32_t aux) {
    return aux >> 1u;
}

static inline uint32_t SLMirRawCallAuxFlags(uint32_t aux) {
    return aux & SLMIR_RAW_CALL_AUX_FLAG_MASK;
}

typedef struct {
    uint32_t        nameStart;
    uint32_t        nameEnd;
    SLMirSymbolKind kind;
    uint32_t        flags;
    uint32_t        target;
} SLMirSymbolRef;

typedef struct {
    const SLMirInst*      insts;
    uint32_t              instLen;
    const SLMirConst*     consts;
    uint32_t              constLen;
    const SLMirSourceRef* sources;
    uint32_t              sourceLen;
    const SLMirFunction*  funcs;
    uint32_t              funcLen;
    const SLMirLocal*     locals;
    uint32_t              localLen;
    const SLMirField*     fields;
    uint32_t              fieldLen;
    const SLMirTypeRef*   types;
    uint32_t              typeLen;
    const SLMirHostRef*   hosts;
    uint32_t              hostLen;
    const SLMirSymbolRef* symbols;
    uint32_t              symbolLen;
} SLMirProgram;

typedef struct {
    SLArena*        arena;
    SLMirInst*      insts;
    uint32_t        instLen;
    uint32_t        instCap;
    SLMirConst*     consts;
    uint32_t        constLen;
    uint32_t        constCap;
    SLMirSourceRef* sources;
    uint32_t        sourceLen;
    uint32_t        sourceCap;
    SLMirFunction*  funcs;
    uint32_t        funcLen;
    uint32_t        funcCap;
    SLMirLocal*     locals;
    uint32_t        localLen;
    uint32_t        localCap;
    SLMirField*     fields;
    uint32_t        fieldLen;
    uint32_t        fieldCap;
    SLMirTypeRef*   types;
    uint32_t        typeLen;
    uint32_t        typeCap;
    SLMirHostRef*   hosts;
    uint32_t        hostLen;
    uint32_t        hostCap;
    SLMirSymbolRef* symbols;
    uint32_t        symbolLen;
    uint32_t        symbolCap;
    uint32_t        openFunc;
    uint8_t         hasOpenFunc;
    uint8_t         _reserved[3];
} SLMirProgramBuilder;

int SLMirBuildExpr(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    SLMirChunk* _Nonnull outChunk,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);

void SLMirProgramBuilderInit(SLMirProgramBuilder* _Nonnull b, SLArena* _Nonnull arena);
int  SLMirProgramBuilderAddConst(
    SLMirProgramBuilder* _Nonnull b,
    const SLMirConst* _Nonnull value,
    uint32_t* _Nullable outIndex);
int SLMirProgramBuilderAddSource(
    SLMirProgramBuilder* _Nonnull b,
    const SLMirSourceRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int SLMirProgramBuilderAddLocal(
    SLMirProgramBuilder* _Nonnull b, const SLMirLocal* _Nonnull value, uint32_t* _Nullable outSlot);
int SLMirProgramBuilderAddField(
    SLMirProgramBuilder* _Nonnull b,
    const SLMirField* _Nonnull value,
    uint32_t* _Nullable outIndex);
int SLMirProgramBuilderAddType(
    SLMirProgramBuilder* _Nonnull b,
    const SLMirTypeRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int SLMirProgramBuilderAddHost(
    SLMirProgramBuilder* _Nonnull b,
    const SLMirHostRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int SLMirProgramBuilderAddSymbol(
    SLMirProgramBuilder* _Nonnull b,
    const SLMirSymbolRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int SLMirProgramBuilderBeginFunction(
    SLMirProgramBuilder* _Nonnull b,
    const SLMirFunction* _Nonnull value,
    uint32_t* _Nullable outIndex);
int SLMirProgramBuilderAppendInst(SLMirProgramBuilder* _Nonnull b, const SLMirInst* _Nonnull value);
int SLMirProgramBuilderInsertInst(
    SLMirProgramBuilder* _Nonnull b,
    uint32_t functionIndex,
    uint32_t instIndexInFunction,
    const SLMirInst* _Nonnull value);
int  SLMirProgramBuilderEndFunction(SLMirProgramBuilder* _Nonnull b);
void SLMirProgramBuilderFinish(
    const SLMirProgramBuilder* _Nonnull b, SLMirProgram* _Nonnull outProgram);
int SLMirValidateProgram(const SLMirProgram* _Nonnull program, SLDiag* _Nullable diag);
int SLMirProgramNeedsDynamicResolution(const SLMirProgram* _Nonnull program);
int SLMirFindFirstDynamicResolutionInst(
    const SLMirProgram* _Nonnull program,
    uint32_t* _Nullable outFunctionIndex,
    uint32_t* _Nullable outPc,
    const SLMirInst** _Nullable outInst);
int SLMirDumpProgram(
    const SLMirProgram* _Nonnull program,
    SLStrView src,
    SLWriter* _Nonnull w,
    SLDiag* _Nullable diag);

SL_API_END
