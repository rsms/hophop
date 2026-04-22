#pragma once
#include "libhop.h"

// Documentation for this IR lives in docs/mir.md

HOP_API_BEGIN

typedef enum {
    HOPMirOp_INVALID = 0,
    HOPMirOp_PUSH_CONST,
    HOPMirOp_PUSH_INT,
    HOPMirOp_PUSH_FLOAT,
    HOPMirOp_PUSH_BOOL,
    HOPMirOp_PUSH_STRING,
    HOPMirOp_PUSH_NULL,
    HOPMirOp_LOAD_IDENT,
    HOPMirOp_STORE_IDENT,
    HOPMirOp_CALL,
    HOPMirOp_UNARY,
    HOPMirOp_BINARY,
    HOPMirOp_INDEX,
    HOPMirOp_SEQ_LEN,
    HOPMirOp_STR_CSTR,
    HOPMirOp_ITER_INIT,
    HOPMirOp_ITER_NEXT,
    HOPMirOp_CAST,
    HOPMirOp_COERCE,
    HOPMirOp_LOCAL_ZERO,
    HOPMirOp_LOCAL_LOAD,
    HOPMirOp_LOCAL_STORE,
    HOPMirOp_LOCAL_ADDR,
    HOPMirOp_DROP,
    HOPMirOp_JUMP,
    HOPMirOp_JUMP_IF_FALSE,
    HOPMirOp_ASSERT,
    HOPMirOp_CALL_FN,
    HOPMirOp_CALL_HOST,
    HOPMirOp_CALL_INDIRECT,
    HOPMirOp_DEREF_LOAD,
    HOPMirOp_DEREF_STORE,
    HOPMirOp_ADDR_OF,
    HOPMirOp_AGG_MAKE,
    HOPMirOp_AGG_ZERO,
    HOPMirOp_AGG_GET,
    HOPMirOp_AGG_SET,
    HOPMirOp_AGG_ADDR,
    HOPMirOp_ARRAY_ZERO,
    HOPMirOp_ARRAY_GET,
    HOPMirOp_ARRAY_SET,
    HOPMirOp_ARRAY_ADDR,
    HOPMirOp_TUPLE_MAKE,
    HOPMirOp_SLICE_MAKE,
    HOPMirOp_OPTIONAL_WRAP,
    HOPMirOp_OPTIONAL_UNWRAP,
    HOPMirOp_TAGGED_MAKE,
    HOPMirOp_TAGGED_TAG,
    HOPMirOp_TAGGED_PAYLOAD,
    HOPMirOp_ALLOC_NEW,
    HOPMirOp_CTX_GET,
    HOPMirOp_CTX_ADDR,
    HOPMirOp_CTX_SET,
    HOPMirOp_RETURN,
    HOPMirOp_RETURN_VOID,
} HOPMirOp;

typedef enum {
    HOPMirCastTarget_INVALID = 0,
    HOPMirCastTarget_INT = 1,
    HOPMirCastTarget_FLOAT = 2,
    HOPMirCastTarget_BOOL = 3,
    HOPMirCastTarget_STR_VIEW = 4,
    HOPMirCastTarget_PTR_LIKE = 5,
} HOPMirCastTarget;

enum {
    HOPMirCallArgFlag_RECEIVER_ARG0 = 0x8000u,
    HOPMirCallArgFlag_SPREAD_LAST = 0x4000u,
    HOPMirCallArgFlag_MASK = HOPMirCallArgFlag_RECEIVER_ARG0 | HOPMirCallArgFlag_SPREAD_LAST,
};

static inline uint32_t HOPMirCallArgCountFromTok(uint16_t tok) {
    return (uint32_t)(tok & ~HOPMirCallArgFlag_MASK);
}

static inline int HOPMirCallTokDropsReceiverArg0(uint16_t tok) {
    return (tok & HOPMirCallArgFlag_RECEIVER_ARG0) != 0;
}

static inline int HOPMirCallTokHasSpreadLast(uint16_t tok) {
    return (tok & HOPMirCallArgFlag_SPREAD_LAST) != 0;
}

enum {
    HOPMirIterFlag_HAS_KEY = 1u << 0,
    HOPMirIterFlag_KEY_REF = 1u << 1,
    HOPMirIterFlag_VALUE_REF = 1u << 2,
    HOPMirIterFlag_VALUE_DISCARD = 1u << 3,
};

typedef struct {
    HOPMirOp op;
    uint16_t tok;
    uint16_t _reserved;
    uint32_t aux;
    uint32_t start;
    uint32_t end;
} HOPMirInst;

typedef struct {
    const HOPMirInst* _Nullable v;
    uint32_t len;
} HOPMirChunk;

typedef enum {
    HOPMirConst_INVALID = 0,
    HOPMirConst_INT,
    HOPMirConst_FLOAT,
    HOPMirConst_BOOL,
    HOPMirConst_STRING,
    HOPMirConst_NULL,
    HOPMirConst_TYPE,
    HOPMirConst_FUNCTION,
    HOPMirConst_HOST,
} HOPMirConstKind;

typedef struct {
    HOPMirConstKind kind;
    uint32_t        aux;
    uint64_t        bits;
    HOPStrView      bytes;
} HOPMirConst;

typedef struct {
    HOPStrView src;
} HOPMirSourceRef;

typedef enum {
    HOPMirLocalFlag_NONE = 0,
    HOPMirLocalFlag_PARAM = 1u << 0,
    HOPMirLocalFlag_MUTABLE = 1u << 1,
    HOPMirLocalFlag_ZERO_INIT = 1u << 2,
} HOPMirLocalFlag;

typedef struct {
    uint32_t typeRef;
    uint32_t flags;
    uint32_t nameStart;
    uint32_t nameEnd;
} HOPMirLocal;

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
} HOPMirFunction;

enum {
    HOPMirFunctionFlag_NONE = 0,
    HOPMirFunctionFlag_VARIADIC = 1u << 0,
};

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t sourceRef;
    uint32_t ownerTypeRef;
    uint32_t typeRef;
} HOPMirField;

typedef struct {
    uint32_t astNode;
    uint32_t sourceRef;
    uint32_t flags;
    uint32_t aux;
} HOPMirTypeRef;

typedef enum {
    HOPMirTypeScalar_NONE = 0,
    HOPMirTypeScalar_I32 = 1,
    HOPMirTypeScalar_I64 = 2,
    HOPMirTypeScalar_F32 = 3,
    HOPMirTypeScalar_F64 = 4,
} HOPMirTypeScalar;

enum {
    HOPMirTypeFlag_SCALAR_MASK = 0x000000ffu,
    HOPMirTypeFlag_STR_REF = 0x00000100u,
    HOPMirTypeFlag_STR_PTR = 0x00000200u,
    HOPMirTypeFlag_U8_PTR = 0x00000400u,
    HOPMirTypeFlag_I32_PTR = 0x00000800u,
    HOPMirTypeFlag_I8_PTR = 0x00001000u,
    HOPMirTypeFlag_U16_PTR = 0x00002000u,
    HOPMirTypeFlag_I16_PTR = 0x00004000u,
    HOPMirTypeFlag_U32_PTR = 0x00008000u,
    HOPMirTypeFlag_FIXED_ARRAY = 0x00010000u,
    HOPMirTypeFlag_FIXED_ARRAY_VIEW = 0x00020000u,
    HOPMirTypeFlag_SLICE_VIEW = 0x00040000u,
    HOPMirTypeFlag_AGGREGATE = 0x00080000u,
    HOPMirTypeFlag_OPAQUE_PTR = 0x00100000u,
    HOPMirTypeFlag_OPTIONAL = 0x00200000u,
    HOPMirTypeFlag_FUNC_REF = 0x00400000u,
    HOPMirTypeFlag_STR_OBJ = 0x00800000u,
    HOPMirTypeFlag_VARRAY_VIEW = 0x01000000u,
    HOPMirTypeFlag_AGG_SLICE_VIEW = 0x02000000u,
};

typedef enum {
    HOPMirIntKind_NONE = 0,
    HOPMirIntKind_BOOL = 1,
    HOPMirIntKind_U8 = 2,
    HOPMirIntKind_I8 = 3,
    HOPMirIntKind_U16 = 4,
    HOPMirIntKind_I16 = 5,
    HOPMirIntKind_U32 = 6,
    HOPMirIntKind_I32 = 7,
} HOPMirIntKind;

enum {
    HOPMirTypeAux_INT_KIND_MASK = 0x000000ffu,
    HOPMirTypeAux_ARRAY_COUNT_SHIFT = 8u,
    HOPMirTypeAux_ARRAY_COUNT_MASK = 0x00ffff00u,
};

static inline uint32_t HOPMirTypeAuxMakeScalarInt(HOPMirIntKind intKind) {
    return (uint32_t)intKind;
}

static inline uint32_t HOPMirTypeAuxMakeFixedArray(HOPMirIntKind elemKind, uint32_t count) {
    return (uint32_t)elemKind
         | ((count << HOPMirTypeAux_ARRAY_COUNT_SHIFT) & HOPMirTypeAux_ARRAY_COUNT_MASK);
}

static inline uint32_t HOPMirTypeAuxMakeVArrayView(HOPMirIntKind elemKind, uint32_t countFieldRef) {
    return (uint32_t)elemKind
         | (((countFieldRef + 1u) << HOPMirTypeAux_ARRAY_COUNT_SHIFT)
            & HOPMirTypeAux_ARRAY_COUNT_MASK);
}

static inline HOPMirIntKind HOPMirTypeRefIntKind(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL
             ? (HOPMirIntKind)(typeRef->aux & HOPMirTypeAux_INT_KIND_MASK)
             : HOPMirIntKind_NONE;
}

static inline uint32_t HOPMirTypeRefFixedArrayCount(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL
             ? (typeRef->aux & HOPMirTypeAux_ARRAY_COUNT_MASK) >> HOPMirTypeAux_ARRAY_COUNT_SHIFT
             : 0u;
}

static inline uint32_t HOPMirTypeRefVArrayCountField(const HOPMirTypeRef* typeRef) {
    uint32_t encoded;
    if (typeRef == NULL) {
        return UINT32_MAX;
    }
    encoded = (typeRef->aux & HOPMirTypeAux_ARRAY_COUNT_MASK) >> HOPMirTypeAux_ARRAY_COUNT_SHIFT;
    return encoded == 0u ? UINT32_MAX : encoded - 1u;
}

static inline uint32_t HOPMirTypeAuxMakeAggSliceView(uint32_t elemTypeRef) {
    return elemTypeRef == UINT32_MAX ? 0u : (elemTypeRef + 1u);
}

static inline uint32_t HOPMirTypeRefAggSliceElemTypeRef(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_AGG_SLICE_VIEW) != 0u
                && typeRef->aux != 0u
             ? typeRef->aux - 1u
             : UINT32_MAX;
}

static inline HOPMirTypeScalar HOPMirTypeRefScalarKind(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL
             ? (HOPMirTypeScalar)(typeRef->flags & HOPMirTypeFlag_SCALAR_MASK)
             : HOPMirTypeScalar_NONE;
}

static inline int HOPMirTypeRefIsStrRef(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_STR_REF) != 0;
}

static inline int HOPMirTypeRefIsStrPtr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_STR_PTR) != 0;
}

static inline int HOPMirTypeRefIsStrObj(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_STR_OBJ) != 0;
}

static inline int HOPMirTypeRefIsU8Ptr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_U8_PTR) != 0;
}

static inline int HOPMirTypeRefIsI32Ptr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_I32_PTR) != 0;
}

static inline int HOPMirTypeRefIsI8Ptr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_I8_PTR) != 0;
}

static inline int HOPMirTypeRefIsU16Ptr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_U16_PTR) != 0;
}

static inline int HOPMirTypeRefIsI16Ptr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_I16_PTR) != 0;
}

static inline int HOPMirTypeRefIsU32Ptr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_U32_PTR) != 0;
}

static inline int HOPMirTypeRefIsFixedArray(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_FIXED_ARRAY) != 0;
}

static inline int HOPMirTypeRefIsFixedArrayView(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_FIXED_ARRAY_VIEW) != 0;
}

static inline int HOPMirTypeRefIsSliceView(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_SLICE_VIEW) != 0;
}

static inline int HOPMirTypeRefIsVArrayView(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_VARRAY_VIEW) != 0;
}

static inline int HOPMirTypeRefIsAggSliceView(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_AGG_SLICE_VIEW) != 0;
}

static inline int HOPMirTypeRefIsAggregate(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_AGGREGATE) != 0;
}

static inline int HOPMirTypeRefIsOpaquePtr(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_OPAQUE_PTR) != 0;
}

static inline int HOPMirTypeRefIsOptional(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_OPTIONAL) != 0;
}

static inline int HOPMirTypeRefIsFuncRef(const HOPMirTypeRef* typeRef) {
    return typeRef != NULL && (typeRef->flags & HOPMirTypeFlag_FUNC_REF) != 0;
}

static inline uint32_t HOPMirTypeRefFuncRefFunctionIndex(const HOPMirTypeRef* typeRef) {
    if (!HOPMirTypeRefIsFuncRef(typeRef) || typeRef->aux == 0u) {
        return UINT32_MAX;
    }
    return typeRef->aux - 1u;
}

static inline uint32_t HOPMirTypeRefOpaquePointeeTypeRef(const HOPMirTypeRef* typeRef) {
    return HOPMirTypeRefIsOpaquePtr(typeRef) ? typeRef->aux : UINT32_MAX;
}

typedef enum {
    HOPMirHost_INVALID = 0,
    HOPMirHost_GENERIC,
} HOPMirHostKind;

typedef enum {
    HOPMirHostTarget_INVALID = 0,
    HOPMirHostTarget_PRINT = 1,
    HOPMirHostTarget_PLATFORM_EXIT = 2,
    HOPMirHostTarget_FREE = 3,
    HOPMirHostTarget_CONCAT = 4,
    HOPMirHostTarget_COPY = 5,
    HOPMirHostTarget_PLATFORM_CONSOLE_LOG = 6,
} HOPMirHostTarget;

typedef enum {
    HOPMirContextField_INVALID = 0,
    HOPMirContextField_ALLOCATOR = 1,
    HOPMirContextField_TEMP_ALLOCATOR = 2,
    HOPMirContextField_LOGGER = 3,
} HOPMirContextField;

typedef struct {
    uint32_t       nameStart;
    uint32_t       nameEnd;
    HOPMirHostKind kind;
    uint32_t       flags;
    uint32_t       target;
} HOPMirHostRef;

typedef enum {
    HOPMirSymbol_INVALID = 0,
    HOPMirSymbol_IDENT,
    HOPMirSymbol_CALL,
    HOPMirSymbol_HOST,
} HOPMirSymbolKind;

typedef enum {
    HOPMirSymbolFlag_NONE = 0,
    HOPMirSymbolFlag_CALL_RECEIVER_ARG0 = 1u << 0,
} HOPMirSymbolFlag;

enum {
    HOPMIR_RAW_CALL_AUX_FLAG_MASK = (1u << 1u) - 1u,
};

static inline uint32_t HOPMirRawCallAuxPack(uint32_t nodeId, uint32_t flags) {
    return (nodeId << 1u) | (flags & HOPMIR_RAW_CALL_AUX_FLAG_MASK);
}

static inline uint32_t HOPMirRawCallAuxNode(uint32_t aux) {
    return aux >> 1u;
}

static inline uint32_t HOPMirRawCallAuxFlags(uint32_t aux) {
    return aux & HOPMIR_RAW_CALL_AUX_FLAG_MASK;
}

typedef struct {
    uint32_t         nameStart;
    uint32_t         nameEnd;
    HOPMirSymbolKind kind;
    uint32_t         flags;
    uint32_t         target;
} HOPMirSymbolRef;

typedef struct {
    const HOPMirInst*      insts;
    uint32_t               instLen;
    const HOPMirConst*     consts;
    uint32_t               constLen;
    const HOPMirSourceRef* sources;
    uint32_t               sourceLen;
    const HOPMirFunction*  funcs;
    uint32_t               funcLen;
    const HOPMirLocal*     locals;
    uint32_t               localLen;
    const HOPMirField*     fields;
    uint32_t               fieldLen;
    const HOPMirTypeRef*   types;
    uint32_t               typeLen;
    const HOPMirHostRef*   hosts;
    uint32_t               hostLen;
    const HOPMirSymbolRef* symbols;
    uint32_t               symbolLen;
} HOPMirProgram;

typedef struct {
    HOPArena*        arena;
    HOPMirInst*      insts;
    uint32_t         instLen;
    uint32_t         instCap;
    HOPMirConst*     consts;
    uint32_t         constLen;
    uint32_t         constCap;
    HOPMirSourceRef* sources;
    uint32_t         sourceLen;
    uint32_t         sourceCap;
    HOPMirFunction*  funcs;
    uint32_t         funcLen;
    uint32_t         funcCap;
    HOPMirLocal*     locals;
    uint32_t         localLen;
    uint32_t         localCap;
    HOPMirField*     fields;
    uint32_t         fieldLen;
    uint32_t         fieldCap;
    HOPMirTypeRef*   types;
    uint32_t         typeLen;
    uint32_t         typeCap;
    HOPMirHostRef*   hosts;
    uint32_t         hostLen;
    uint32_t         hostCap;
    HOPMirSymbolRef* symbols;
    uint32_t         symbolLen;
    uint32_t         symbolCap;
    uint32_t         openFunc;
    uint8_t          hasOpenFunc;
    uint8_t          _reserved[3];
} HOPMirProgramBuilder;

int HOPMirBuildExpr(
    HOPArena* _Nonnull arena,
    const HOPAst* _Nonnull ast,
    HOPStrView src,
    int32_t    nodeId,
    HOPMirChunk* _Nonnull outChunk,
    int* _Nonnull outSupported,
    HOPDiag* _Nullable diag);

void HOPMirProgramBuilderInit(HOPMirProgramBuilder* _Nonnull b, HOPArena* _Nonnull arena);
int  HOPMirProgramBuilderAddConst(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirConst* _Nonnull value,
    uint32_t* _Nullable outIndex);
int HOPMirProgramBuilderAddSource(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirSourceRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int HOPMirProgramBuilderAddLocal(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirLocal* _Nonnull value,
    uint32_t* _Nullable outSlot);
int HOPMirProgramBuilderAddField(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirField* _Nonnull value,
    uint32_t* _Nullable outIndex);
int HOPMirProgramBuilderAddType(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirTypeRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int HOPMirProgramBuilderAddHost(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirHostRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int HOPMirProgramBuilderAddSymbol(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirSymbolRef* _Nonnull value,
    uint32_t* _Nullable outIndex);
int HOPMirProgramBuilderBeginFunction(
    HOPMirProgramBuilder* _Nonnull b,
    const HOPMirFunction* _Nonnull value,
    uint32_t* _Nullable outIndex);
int HOPMirProgramBuilderAppendInst(
    HOPMirProgramBuilder* _Nonnull b, const HOPMirInst* _Nonnull value);
int HOPMirProgramBuilderInsertInst(
    HOPMirProgramBuilder* _Nonnull b,
    uint32_t functionIndex,
    uint32_t instIndexInFunction,
    const HOPMirInst* _Nonnull value);
int  HOPMirProgramBuilderEndFunction(HOPMirProgramBuilder* _Nonnull b);
void HOPMirProgramBuilderFinish(
    const HOPMirProgramBuilder* _Nonnull b, HOPMirProgram* _Nonnull outProgram);
int HOPMirValidateProgram(const HOPMirProgram* _Nonnull program, HOPDiag* _Nullable diag);
int HOPMirProgramNeedsDynamicResolution(const HOPMirProgram* _Nonnull program);
int HOPMirFindFirstDynamicResolutionInst(
    const HOPMirProgram* _Nonnull program,
    uint32_t* _Nullable outFunctionIndex,
    uint32_t* _Nullable outPc,
    const HOPMirInst** _Nullable outInst);
int HOPMirDumpProgram(
    const HOPMirProgram* _Nonnull program,
    HOPStrView src,
    HOPWriter* _Nonnull w,
    HOPDiag* _Nullable diag);

HOP_API_END
