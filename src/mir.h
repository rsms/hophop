#pragma once
#include "libhop.h"

// Documentation for this IR lives in docs/mir.md

H2_API_BEGIN

typedef enum {
    H2MirOp_INVALID = 0,
    H2MirOp_PUSH_CONST,
    H2MirOp_PUSH_INT,
    H2MirOp_PUSH_FLOAT,
    H2MirOp_PUSH_BOOL,
    H2MirOp_PUSH_STRING,
    H2MirOp_PUSH_NULL,
    H2MirOp_LOAD_IDENT,
    H2MirOp_STORE_IDENT,
    H2MirOp_CALL,
    H2MirOp_UNARY,
    H2MirOp_BINARY,
    H2MirOp_INDEX,
    H2MirOp_SEQ_LEN,
    H2MirOp_STR_CSTR,
    H2MirOp_ITER_INIT,
    H2MirOp_ITER_NEXT,
    H2MirOp_CAST,
    H2MirOp_COERCE,
    H2MirOp_LOCAL_ZERO,
    H2MirOp_LOCAL_LOAD,
    H2MirOp_LOCAL_STORE,
    H2MirOp_LOCAL_ADDR,
    H2MirOp_DROP,
    H2MirOp_JUMP,
    H2MirOp_JUMP_IF_FALSE,
    H2MirOp_ASSERT,
    H2MirOp_CALL_FN,
    H2MirOp_CALL_HOST,
    H2MirOp_CALL_INDIRECT,
    H2MirOp_DEREF_LOAD,
    H2MirOp_DEREF_STORE,
    H2MirOp_ADDR_OF,
    H2MirOp_AGG_MAKE,
    H2MirOp_AGG_ZERO,
    H2MirOp_AGG_GET,
    H2MirOp_AGG_SET,
    H2MirOp_AGG_ADDR,
    H2MirOp_ARRAY_ZERO,
    H2MirOp_ARRAY_GET,
    H2MirOp_ARRAY_SET,
    H2MirOp_ARRAY_ADDR,
    H2MirOp_TUPLE_MAKE,
    H2MirOp_SLICE_MAKE,
    H2MirOp_OPTIONAL_WRAP,
    H2MirOp_OPTIONAL_UNWRAP,
    H2MirOp_TAGGED_MAKE,
    H2MirOp_TAGGED_TAG,
    H2MirOp_TAGGED_PAYLOAD,
    H2MirOp_ALLOC_NEW,
    H2MirOp_CTX_GET,
    H2MirOp_CTX_ADDR,
    H2MirOp_CTX_SET,
    H2MirOp_RETURN,
    H2MirOp_RETURN_VOID,
} H2MirOp;

typedef enum {
    H2MirCastTarget_INVALID = 0,
    H2MirCastTarget_INT = 1,
    H2MirCastTarget_FLOAT = 2,
    H2MirCastTarget_BOOL = 3,
    H2MirCastTarget_STR_VIEW = 4,
    H2MirCastTarget_PTR_LIKE = 5,
} H2MirCastTarget;

enum {
    H2MirCallArgFlag_RECEIVER_ARG0 = 0x8000u,
    H2MirCallArgFlag_SPREAD_LAST = 0x4000u,
    H2MirCallArgFlag_MASK = H2MirCallArgFlag_RECEIVER_ARG0 | H2MirCallArgFlag_SPREAD_LAST,
};

static inline uint32_t H2MirCallArgCountFromTok(uint16_t tok) {
    return (uint32_t)(tok & ~H2MirCallArgFlag_MASK);
}

static inline int H2MirCallTokDropsReceiverArg0(uint16_t tok) {
    return (tok & H2MirCallArgFlag_RECEIVER_ARG0) != 0;
}

static inline int H2MirCallTokHasSpreadLast(uint16_t tok) {
    return (tok & H2MirCallArgFlag_SPREAD_LAST) != 0;
}

enum {
    H2MirIterFlag_HAS_KEY = 1u << 0,
    H2MirIterFlag_KEY_REF = 1u << 1,
    H2MirIterFlag_VALUE_REF = 1u << 2,
    H2MirIterFlag_VALUE_DISCARD = 1u << 3,
};

typedef struct {
    H2MirOp  op;
    uint16_t tok;
    uint16_t _reserved;
    uint32_t aux;
    uint32_t start;
    uint32_t end;
} H2MirInst;

typedef struct {
    const H2MirInst* _Nullable v;
    uint32_t len;
} H2MirChunk;

typedef enum {
    H2MirConst_INVALID = 0,
    H2MirConst_INT,
    H2MirConst_FLOAT,
    H2MirConst_BOOL,
    H2MirConst_STRING,
    H2MirConst_NULL,
    H2MirConst_TYPE,
    H2MirConst_FUNCTION,
    H2MirConst_HOST,
} H2MirConstKind;

typedef struct {
    H2MirConstKind kind;
    uint32_t       aux;
    uint64_t       bits;
    H2StrView      bytes;
} H2MirConst;

typedef struct {
    H2StrView src;
} H2MirSourceRef;

typedef enum {
    H2MirLocalFlag_NONE = 0,
    H2MirLocalFlag_PARAM = 1u << 0,
    H2MirLocalFlag_MUTABLE = 1u << 1,
    H2MirLocalFlag_ZERO_INIT = 1u << 2,
} H2MirLocalFlag;

typedef struct {
    uint32_t typeRef;
    uint32_t flags;
    uint32_t nameStart;
    uint32_t nameEnd;
} H2MirLocal;

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
} H2MirFunction;

enum {
    H2MirFunctionFlag_NONE = 0,
    H2MirFunctionFlag_VARIADIC = 1u << 0,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t sourceRef;
    uint32_t ownerTypeRef;
    uint32_t typeRef;
} H2MirField;

typedef struct {
    uint32_t astNode;
    uint32_t sourceRef;
    uint32_t flags;
    uint32_t aux;
} H2MirTypeRef;

typedef enum {
    H2MirTypeScalar_NONE = 0,
    H2MirTypeScalar_I32 = 1,
    H2MirTypeScalar_I64 = 2,
    H2MirTypeScalar_F32 = 3,
    H2MirTypeScalar_F64 = 4,
} H2MirTypeScalar;

enum {
    H2MirTypeFlag_SCALAR_MASK = 0x000000ffu,
    H2MirTypeFlag_STR_REF = 0x00000100u,
    H2MirTypeFlag_STR_PTR = 0x00000200u,
    H2MirTypeFlag_U8_PTR = 0x00000400u,
    H2MirTypeFlag_I32_PTR = 0x00000800u,
    H2MirTypeFlag_I8_PTR = 0x00001000u,
    H2MirTypeFlag_U16_PTR = 0x00002000u,
    H2MirTypeFlag_I16_PTR = 0x00004000u,
    H2MirTypeFlag_U32_PTR = 0x00008000u,
    H2MirTypeFlag_FIXED_ARRAY = 0x00010000u,
    H2MirTypeFlag_FIXED_ARRAY_VIEW = 0x00020000u,
    H2MirTypeFlag_SLICE_VIEW = 0x00040000u,
    H2MirTypeFlag_AGGREGATE = 0x00080000u,
    H2MirTypeFlag_OPAQUE_PTR = 0x00100000u,
    H2MirTypeFlag_OPTIONAL = 0x00200000u,
    H2MirTypeFlag_FUNC_REF = 0x00400000u,
    H2MirTypeFlag_STR_OBJ = 0x00800000u,
    H2MirTypeFlag_VARRAY_VIEW = 0x01000000u,
    H2MirTypeFlag_AGG_SLICE_VIEW = 0x02000000u,
};

typedef enum {
    H2MirIntKind_NONE = 0,
    H2MirIntKind_BOOL = 1,
    H2MirIntKind_U8 = 2,
    H2MirIntKind_I8 = 3,
    H2MirIntKind_U16 = 4,
    H2MirIntKind_I16 = 5,
    H2MirIntKind_U32 = 6,
    H2MirIntKind_I32 = 7,
} H2MirIntKind;

enum {
    H2MirTypeAux_INT_KIND_MASK = 0x000000ffu,
    H2MirTypeAux_ARRAY_COUNT_SHIFT = 8u,
    H2MirTypeAux_ARRAY_COUNT_MASK = 0x00ffff00u,
};

static inline uint32_t H2MirTypeAuxMakeScalarInt(H2MirIntKind intKind) {
    return (uint32_t)intKind;
}

static inline uint32_t H2MirTypeAuxMakeFixedArray(H2MirIntKind elemKind, uint32_t count) {
    return (uint32_t)elemKind
         | ((count << H2MirTypeAux_ARRAY_COUNT_SHIFT) & H2MirTypeAux_ARRAY_COUNT_MASK);
}

static inline uint32_t H2MirTypeAuxMakeVArrayView(H2MirIntKind elemKind, uint32_t countFieldRef) {
    return (uint32_t)elemKind
         | (((countFieldRef + 1u) << H2MirTypeAux_ARRAY_COUNT_SHIFT)
            & H2MirTypeAux_ARRAY_COUNT_MASK);
}

static inline H2MirIntKind H2MirTypeRefIntKind(const H2MirTypeRef* typeRef) {
    return typeRef != NULL
             ? (H2MirIntKind)(typeRef->aux & H2MirTypeAux_INT_KIND_MASK)
             : H2MirIntKind_NONE;
}

static inline uint32_t H2MirTypeRefFixedArrayCount(const H2MirTypeRef* typeRef) {
    return typeRef != NULL
             ? (typeRef->aux & H2MirTypeAux_ARRAY_COUNT_MASK) >> H2MirTypeAux_ARRAY_COUNT_SHIFT
             : 0u;
}

static inline uint32_t H2MirTypeRefVArrayCountField(const H2MirTypeRef* typeRef) {
    uint32_t encoded;
    if (typeRef == NULL) {
        return UINT32_MAX;
    }
    encoded = (typeRef->aux & H2MirTypeAux_ARRAY_COUNT_MASK) >> H2MirTypeAux_ARRAY_COUNT_SHIFT;
    return encoded == 0u ? UINT32_MAX : encoded - 1u;
}

static inline uint32_t H2MirTypeAuxMakeAggSliceView(uint32_t elemTypeRef) {
    return elemTypeRef == UINT32_MAX ? 0u : (elemTypeRef + 1u);
}

static inline uint32_t H2MirTypeRefAggSliceElemTypeRef(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_AGG_SLICE_VIEW) != 0u
                && typeRef->aux != 0u
             ? typeRef->aux - 1u
             : UINT32_MAX;
}

static inline H2MirTypeScalar H2MirTypeRefScalarKind(const H2MirTypeRef* typeRef) {
    return typeRef != NULL
             ? (H2MirTypeScalar)(typeRef->flags & H2MirTypeFlag_SCALAR_MASK)
             : H2MirTypeScalar_NONE;
}

static inline int H2MirTypeRefIsStrRef(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_STR_REF) != 0;
}

static inline int H2MirTypeRefIsStrPtr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_STR_PTR) != 0;
}

static inline int H2MirTypeRefIsStrObj(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_STR_OBJ) != 0;
}

static inline int H2MirTypeRefIsU8Ptr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_U8_PTR) != 0;
}

static inline int H2MirTypeRefIsI32Ptr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_I32_PTR) != 0;
}

static inline int H2MirTypeRefIsI8Ptr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_I8_PTR) != 0;
}

static inline int H2MirTypeRefIsU16Ptr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_U16_PTR) != 0;
}

static inline int H2MirTypeRefIsI16Ptr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_I16_PTR) != 0;
}

static inline int H2MirTypeRefIsU32Ptr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_U32_PTR) != 0;
}

static inline int H2MirTypeRefIsFixedArray(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_FIXED_ARRAY) != 0;
}

static inline int H2MirTypeRefIsFixedArrayView(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_FIXED_ARRAY_VIEW) != 0;
}

static inline int H2MirTypeRefIsSliceView(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_SLICE_VIEW) != 0;
}

static inline int H2MirTypeRefIsVArrayView(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_VARRAY_VIEW) != 0;
}

static inline int H2MirTypeRefIsAggSliceView(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_AGG_SLICE_VIEW) != 0;
}

static inline int H2MirTypeRefIsAggregate(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_AGGREGATE) != 0;
}

static inline int H2MirTypeRefIsOpaquePtr(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_OPAQUE_PTR) != 0;
}

static inline int H2MirTypeRefIsOptional(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_OPTIONAL) != 0;
}

static inline int H2MirTypeRefIsFuncRef(const H2MirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & H2MirTypeFlag_FUNC_REF) != 0;
}

static inline uint32_t H2MirTypeRefFuncRefFunctionIndex(const H2MirTypeRef* typeRef) {
    if (!H2MirTypeRefIsFuncRef(typeRef) || typeRef->aux == 0u) {
        return UINT32_MAX;
    }
    return typeRef->aux - 1u;
}

static inline uint32_t H2MirTypeRefOpaquePointeeTypeRef(const H2MirTypeRef* typeRef) {
    return H2MirTypeRefIsOpaquePtr(typeRef) ? typeRef->aux : UINT32_MAX;
}

typedef enum {
    H2MirHost_INVALID = 0,
    H2MirHost_GENERIC,
} H2MirHostKind;

typedef enum {
    H2MirHostTarget_INVALID = 0,
    H2MirHostTarget_PRINT = 1,
    H2MirHostTarget_PLATFORM_EXIT = 2,
    H2MirHostTarget_FREE = 3,
    H2MirHostTarget_CONCAT = 4,
    H2MirHostTarget_COPY = 5,
    H2MirHostTarget_PLATFORM_CONSOLE_LOG = 6,
} H2MirHostTarget;

typedef enum {
    H2MirContextField_INVALID = 0,
    H2MirContextField_ALLOCATOR = 1,
    H2MirContextField_TEMP_ALLOCATOR = 2,
    H2MirContextField_LOGGER = 3,
} H2MirContextField;

typedef struct {
    uint32_t      nameStart;
    uint32_t      nameEnd;
    H2MirHostKind kind;
    uint32_t      flags;
    uint32_t      target;
} H2MirHostRef;

typedef enum {
    H2MirSymbol_INVALID = 0,
    H2MirSymbol_IDENT,
    H2MirSymbol_CALL,
    H2MirSymbol_HOST,
} H2MirSymbolKind;

typedef enum {
    H2MirSymbolFlag_NONE = 0,
    H2MirSymbolFlag_CALL_RECEIVER_ARG0 = 1u << 0,
} H2MirSymbolFlag;

enum {
    H2MIR_RAW_CALL_AUX_FLAG_MASK = (1u << 1u) - 1u,
};

static inline uint32_t H2MirRawCallAuxPack(uint32_t nodeId, uint32_t flags) {
    return (nodeId << 1u) | (flags & H2MIR_RAW_CALL_AUX_FLAG_MASK);
}

static inline uint32_t H2MirRawCallAuxNode(uint32_t aux) {
    return aux >> 1u;
}

static inline uint32_t H2MirRawCallAuxFlags(uint32_t aux) {
    return aux & H2MIR_RAW_CALL_AUX_FLAG_MASK;
}

typedef struct {
    uint32_t        nameStart;
    uint32_t        nameEnd;
    H2MirSymbolKind kind;
    uint32_t        flags;
    uint32_t        target;
} H2MirSymbolRef;

typedef struct {
    const H2MirInst*      insts;
    uint32_t              instLen;
    const H2MirConst*     consts;
    uint32_t              constLen;
    const H2MirSourceRef* sources;
    uint32_t              sourceLen;
    const H2MirFunction*  funcs;
    uint32_t              funcLen;
    const H2MirLocal*     locals;
    uint32_t              localLen;
    const H2MirField*     fields;
    uint32_t              fieldLen;
    const H2MirTypeRef*   types;
    uint32_t              typeLen;
    const H2MirHostRef*   hosts;
    uint32_t              hostLen;
    const H2MirSymbolRef* symbols;
    uint32_t              symbolLen;
} H2MirProgram;

typedef struct {
    H2Arena*        arena;
    H2MirInst*      insts;
    uint32_t        instLen;
    uint32_t        instCap;
    H2MirConst*     consts;
    uint32_t        constLen;
    uint32_t        constCap;
    H2MirSourceRef* sources;
    uint32_t        sourceLen;
    uint32_t        sourceCap;
    H2MirFunction*  funcs;
    uint32_t        funcLen;
    uint32_t        funcCap;
    H2MirLocal*     locals;
    uint32_t        localLen;
    uint32_t        localCap;
    H2MirField*     fields;
    uint32_t        fieldLen;
    uint32_t        fieldCap;
    H2MirTypeRef*   types;
    uint32_t        typeLen;
    uint32_t        typeCap;
    H2MirHostRef*   hosts;
    uint32_t        hostLen;
    uint32_t        hostCap;
    H2MirSymbolRef* symbols;
    uint32_t        symbolLen;
    uint32_t        symbolCap;
    uint32_t        openFunc;
    uint8_t         hasOpenFunc;
    uint8_t         _reserved[3];
} H2MirProgramBuilder;

int H2MirBuildExpr(
    H2Arena* _Nonnull arena,
    const H2Ast* _Nonnull ast,
    H2StrView src,
    int32_t   nodeId,
    H2MirChunk* _Nonnull outChunk,
    int* _Nonnull outSupported,
    H2Diag* _Nullable diag);

void H2MirProgramBuilderInit(H2MirProgramBuilder* _Nonnull b, H2Arena* _Nonnull arena);
int  H2MirProgramBuilderAddConst(
    H2MirProgramBuilder* _Nonnull b,
    const H2MirConst* _Nonnull value,
    uint32_t* _Nullable outIndex);
int H2MirProgramBuilderAddSource(
    H2MirProgramBuilder* _Nonnull b,
    const H2MirSourceRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int H2MirProgramBuilderAddLocal(
    H2MirProgramBuilder* _Nonnull b, const H2MirLocal* _Nonnull value, uint32_t* _Nullable outSlot);
int H2MirProgramBuilderAddField(
    H2MirProgramBuilder* _Nonnull b,
    const H2MirField* _Nonnull value,
    uint32_t* _Nullable outIndex);
int H2MirProgramBuilderAddType(
    H2MirProgramBuilder* _Nonnull b,
    const H2MirTypeRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int H2MirProgramBuilderAddHost(
    H2MirProgramBuilder* _Nonnull b,
    const H2MirHostRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int H2MirProgramBuilderAddSymbol(
    H2MirProgramBuilder* _Nonnull b,
    const H2MirSymbolRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int H2MirProgramBuilderBeginFunction(
    H2MirProgramBuilder* _Nonnull b,
    const H2MirFunction* _Nonnull value,
    uint32_t* _Nullable outIndex);
int H2MirProgramBuilderAppendInst(H2MirProgramBuilder* _Nonnull b, const H2MirInst* _Nonnull value);
int H2MirProgramBuilderInsertInst(
    H2MirProgramBuilder* _Nonnull b,
    uint32_t functionIndex,
    uint32_t instIndexInFunction,
    const H2MirInst* _Nonnull value);
int  H2MirProgramBuilderEndFunction(H2MirProgramBuilder* _Nonnull b);
void H2MirProgramBuilderFinish(
    const H2MirProgramBuilder* _Nonnull b, H2MirProgram* _Nonnull outProgram);
int H2MirValidateProgram(const H2MirProgram* _Nonnull program, H2Diag* _Nullable diag);
int H2MirProgramNeedsDynamicResolution(const H2MirProgram* _Nonnull program);
int H2MirFindFirstDynamicResolutionInst(
    const H2MirProgram* _Nonnull program,
    uint32_t* _Nullable outFunctionIndex,
    uint32_t* _Nullable outPc,
    const H2MirInst** _Nullable outInst);
int H2MirDumpProgram(
    const H2MirProgram* _Nonnull program,
    H2StrView src,
    H2Writer* _Nonnull w,
    H2Diag* _Nullable diag);

H2_API_END
