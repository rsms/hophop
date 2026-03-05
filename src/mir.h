#pragma once
#include "libsl.h"

// Documentation for this IR lives in docs/mir.md

SL_API_BEGIN

typedef enum {
    SLMirOp_INVALID = 0,
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
    SLMirOp_CAST,
    SLMirOp_RETURN,
} SLMirOp;

typedef enum {
    SLMirCastTarget_INVALID = 0,
    SLMirCastTarget_INT = 1,
    SLMirCastTarget_FLOAT = 2,
    SLMirCastTarget_BOOL = 3,
} SLMirCastTarget;

typedef struct {
    SLMirOp  op;
    uint16_t tok;
    uint16_t _reserved;
    uint32_t start;
    uint32_t end;
} SLMirInst;

typedef struct {
    const SLMirInst* v;
    uint32_t         len;
} SLMirChunk;

int SLMirBuildExpr(
    SLArena* _Nonnull arena,
    const SLAst* _Nonnull ast,
    SLStrView src,
    int32_t   nodeId,
    SLMirChunk* _Nonnull outChunk,
    int* _Nonnull outSupported,
    SLDiag* _Nullable diag);

SL_API_END
