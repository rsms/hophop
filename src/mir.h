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
    SLMirOp_CALL,
    SLMirOp_UNARY,
    SLMirOp_BINARY,
    SLMirOp_INDEX,
    SLMirOp_SEQ_LEN,
    SLMirOp_ITER_INIT,
    SLMirOp_ITER_NEXT,
    SLMirOp_CAST,
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
    SLMirOp_CTX_SET,
    SLMirOp_RETURN,
    SLMirOp_RETURN_VOID,
} SLMirOp;

typedef enum {
    SLMirCastTarget_INVALID = 0,
    SLMirCastTarget_INT = 1,
    SLMirCastTarget_FLOAT = 2,
    SLMirCastTarget_BOOL = 3,
} SLMirCastTarget;

enum {
    SLMirCallArgFlag_RECEIVER_ARG0 = 0x8000u,
};

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
    const SLMirInst* v;
    uint32_t         len;
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
} SLMirFunction;

typedef struct {
    uint32_t nameStart;
    uint32_t nameEnd;
    uint32_t ownerTypeRef;
    uint32_t typeRef;
} SLMirField;

typedef struct {
    uint32_t astNode;
    uint32_t flags;
} SLMirTypeRef;

typedef enum {
    SLMirHost_INVALID = 0,
    SLMirHost_GENERIC,
} SLMirHostKind;

typedef enum {
    SLMirHostTarget_INVALID = 0,
    SLMirHostTarget_PRINT = 1,
    SLMirHostTarget_PLATFORM_EXIT = 2,
} SLMirHostTarget;

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
int SLMirProgramBuilderEndFunction(SLMirProgramBuilder* _Nonnull b);
void SLMirProgramBuilderFinish(
    const SLMirProgramBuilder* _Nonnull b, SLMirProgram* _Nonnull outProgram);
int SLMirValidateProgram(const SLMirProgram* _Nonnull program, SLDiag* _Nullable diag);

SL_API_END
